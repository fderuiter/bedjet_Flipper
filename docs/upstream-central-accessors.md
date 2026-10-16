# Proposed upstream central accessors (draft)

**Status: draft, not submitted.** Nothing here has been sent to the firmware
branch or to [flipperzero-firmware#4436](https://github.com/flipperdevices/flipperzero-firmware/pull/4436).
The app does not use these functions and the firmware pin in
[`firmware.lock.json`](../firmware.lock.json) is unchanged. Tracked in
[#18](https://github.com/fderuiter/flipper_fredjet/issues/18).

The proposed change is
[`upstream/ble-central-accessors.patch`](upstream/ble-central-accessors.patch),
a diff against the pinned `targets/f7/ble_glue/furi_ble/central.{c,h}` at
`df553f8`. It adds three read-only accessors and changes no existing behavior
or signature. See the end of this page for what has and has not been checked.

## Gaps in the pinned API

Each finding comes from reading the pinned source, not from a device.

- **Failure reason.** `ble_central_last_status()` holds only the synchronous
  result of the last ACI command. The asynchronous outcome is kept in a private
  `proc_error` field and cleared before the next procedure. A wait that times out
  leaves both at 0. A failed connection's HCI status, and the reason a link
  dropped mid-procedure, go only to the log. ATT Error Responses
  (`ACI_GATT_ERROR_RESP_VSEVT_CODE`, such as 0x05 Insufficient Authentication or
  0x0F Insufficient Encryption) are not handled at all, so a peer that refuses a
  read for security reasons cannot be told apart from a lost link.
- **Disconnect reason.** The Disconnection Complete reason is written into
  `proc_error` only to wake a waiting procedure, and is otherwise only logged.
  A link that drops between calls leaves no reason the app can read.
- **Unusable link.** A procedure timeout sets the private `connection_usable`
  to false and tries to terminate. If the controller refuses termination or does
  not confirm it in time, `connected` stays true. `ble_central_is_connected()`
  then reports a link on which every GATT call fails before reaching the radio.

## Proposed API

```c
typedef enum {
    BleCentralErrorNone,
    BleCentralErrorState,        /* not connected, link unusable or busy */
    BleCentralErrorCommand,      /* controller rejected the command; tBleStatus */
    BleCentralErrorTimeout,      /* no completion within timeout_ms */
    BleCentralErrorProcedure,    /* completion reported failure; its status */
    BleCentralErrorAtt,          /* peer sent an ATT Error Response; ATT error */
    BleCentralErrorDisconnected, /* link dropped during the call; HCI reason */
} BleCentralErrorSource;

typedef struct {
    BleCentralErrorSource source;
    uint8_t code;
    uint8_t att_request_opcode;
    uint16_t att_handle;
} BleCentralError;

void ble_central_last_error(BleCentralError* out);
uint8_t ble_central_disconnect_reason(void);
bool ble_central_is_usable(void);
```

- `ble_central_last_error()` explains why the most recent connection or GATT
  call returned false, and is meaningful only after a false return. It is reset when a call takes the operation lock and set at each failure
  point. It takes the same lock, so it reflects a finished call. Scan calls do
  not change it, and `ble_central_last_status()` keeps its current meaning.
- An ATT Error Response is recorded but reported only when the procedure also
  completes with an error, because discovery normally ends with Attribute Not
  Found. The handler still returns `BleEventNotAck`, as the event does today.
- A procedure timeout is reported as `Timeout` even though it also starts a
  disconnection; that disconnection's reason is available separately.
- `ble_central_disconnect_reason()` keeps the latest HCI reason for the central
  link until the next successful connection. 0x16 means this side terminated,
  which includes teardown after a procedure timeout.
- `ble_central_is_usable()` returns `connected && connection_usable`, the same
  check every GATT call already makes internally.

## How the app would use them

Without these accessors the app now infers a coarse cause from timing, link
state and the raw status, and disconnects after a timed-out read or
resubscribe (see [setup](setup.md)). It cannot see ATT error codes or the
disconnect reason. The accessors below would replace that inference.

These are intended uses, not implemented. The setup stages that now say
`Setup failed: no notify` could name a timeout, a security refusal or a
dropped link instead of showing an unrelated raw status. Before a write, a
connected but unusable link would go straight to disconnect and report
it, instead of each control failing in turn. The Info page would show the
disconnect reason. None of this changes the rule that only observed status
confirms a setting.

## Bounded waits to ask for

These are listed in [waits with no budget](cancellation.md#waits-with-no-budget).
No patch is drafted for them; they are notes for the same upstream discussion.

- `furi_hal_bt_stop_advertising()`: a variant that takes a timeout and returns
  whether the GAP state reached idle. Separately, `gap_advertise_start()`
  could ignore a queued low-power restart once `enable_adv` is false, which
  closes the stop/timer race described there.
- Scan and notification callback drains: a timed acquire would only move the
  problem, since app context cannot be freed while a callback might run.
  Documenting the requirement that callbacks stay short is the realistic ask.
- The 33 s ACI response limit belongs to ST's transport and Flipper's
  `hci_cmd_resp_wait()`, which asserts instead of returning. Changing that is
  outside the central API and is not proposed here.

## Related observation

`ble_central_read()` tolerates a failed long-read continuation when
`proc_error` is 0x07 (Invalid Offset) or 0x0B (Attribute Not Long). Those are
ATT error codes, but `proc_error` holds the `ACI_GATT_PROC_COMPLETE` status,
and ATT Error Responses are not recorded. Inferred from the source: the
tolerance may never apply, so a peer that rejects Read Blob would fail the
whole read. The patch does not change this; with the ATT error recorded, a
follow-up could compare against it. A continuation starts only when the
first response is exactly 22 bytes at the default MTU. Reference implementations
get 11 bytes from the status read, so the app is probably unaffected; no BedJet
read has been captured yet to confirm it.

## Checks

| Check | Result |
| --- | --- |
| Patch applies to the pinned firmware (`git apply --check` at `df553f8`) | Passed |
| clang-format 18 with the firmware's `.clang-format` | Passed |
| Patched `central.c` type-checks: clang 18, `--target=thumbv7em-none-eabihf -fsyntax-only -Wall -Wextra -Werror`, firmware include paths, host libc headers (`-Wformat` off for the libc mismatch) | Passed; unpatched file passes the same command |
| Pinned firmware built with the patch (`./fbt`) | Not run: this container's proxy refused the Flipper toolchain download |
| Firmware API symbol table (`api_symbols.csv`) updated for the new exports | Not done; required before an app could import them |
| Run on a Flipper or against a BedJet | Not done |
