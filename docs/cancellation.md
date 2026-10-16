# Back, Off and shutdown (issue #3)

## Input and priority policy

| Input | Delivery | Effect |
| --- | --- | --- |
| Press Back, including a hold | Session-bound priority bit | Disconnect; from Home or during disconnect, exit |
| Hold OK on any control row | Session-bound priority bit | Send Off once, discard older queued work |
| Shutdown | Global atomic latch | Exit and discard all queued work |
| Other short keys / Left-Right repeats | 24-entry nonblocking inbox | Ordinary control/navigation; can be dropped and counted |
| Advertisements / notifications / explicit refresh | Same ordinary inbox | Copied bounded data; can be dropped and counted |

Shutdown and Back take precedence over Off. Back disconnects without sending a
BedJet command. If disconnection and Off are requested together, do not assume
Back switched the BedJet off. Off is available without fresh telemetry but only
once setup is complete. It is rejected during setup, cleanup and its own active
write. Repeated Off requests pending before a worker tick coalesce into one
write; a later hold after completion is a new deliberate request.

The old short-OK action on the TURN OFF row is replaced by **hold OK from any
control row**. The row now displays that instruction. Flipper's pinned input
service emits Long for a hold and does not subsequently emit Short on release;
OK repeats are ignored. Back uses Press, avoiding both a release delay and the
old loss of a held Back. These actions neither lock the GUI model nor enter a
second application input queue. Physical input routing still needs device QA.

The title's sticky `!N` counter makes ordinary inbox contention/overflow visible
until app exit (display capped at 999; full count retained in the model). Rejected
stale-session, invalid, or busy input is separate from inbox overflow. Ordinary
input is rejected while priority action is pending. Overflow does not drop
Back, Off or shutdown.

## Session and dispatch contract

The session generation and two priority bits occupy one lock-free atomic word.
A strong compare-and-exchange validates the originating session and sets the bit
in the same operation. Within a generation each bit can only be added once;
three attempts bound posting without waiting on the ordinary inbox lock. Session
tokens are opaque; callers must not increment or compare their arithmetic value.

Starting scan/connect and consuming Off rotate the generation with a conditional
exchange, so they cannot erase a Back racing that transition. Disconnect and
shutdown consume the cancelled generation. Old work cannot act on a replacement
session even if queue clearing encounters a producer holding its try-lock.

Before Off dispatch, the worker rotates the generation, closes ordinary input,
and drains the old notification callback. It retains observed status, attempts
one `01 01` write, then rebinds notifications with the new token. Subscription
stays enabled; Off does not reconnect, resubscribe or resend another setting.
Old mode, temperature, fan, timer, preset, navigation and refresh work is stale.
A failed Off remains a failed write with no automatic retry, including after
reconnect. Off now also has a pending observation result and requires a newer
mode-0 observation to report Applied. A timeout does not retry it. The previous
command and its coalesced target are discarded before Off. See the
[command policy](commands.md). Controller acceptance does not confirm the unit
stopped; observed status and the physical remote remain authoritative.

Cancellation is checked before stages, after calls, before write admission, and
after event processing. Setup cancellation publishes Closing rather than a ready
control screen and does not run later discovery/subscription/read stages. A
command already admitted to a synchronous transport call may finish after the
input arrives. Later queued commands are discarded. The app cannot preempt an
in-flight firmware call; it makes no zero-latency or real-time guarantee.

Notification removal drains the old callback before its context is changed.
Successful scan stop drains scan callbacks. A failed scan stop can retain them;
the pinned deinitializer drains/detaches both app callback types even on false.
Cleanup retries are capped at three per phase, including stopping the scan at
the end of discovery; three failed scan stops start the bounded disconnect path.
Persistent failure exits with `cleanup_deferred` and without claiming
advertising was restored. Because the firmware central object outlives the app,
the next launch checks for an inherited scan or link, makes one stop/disconnect
attempt and reports `BT busy: reboot Flipper` if it remains. Session rotation
closes ordinary input and Off admission before publishing the new token, so a
racing key cannot land in the cleanup generation. Repeated Back
coalesces; a second Back during disconnect requests shutdown, and input is then
closed so it cannot restart the teardown retry budget.

## Native wait budgets

These are **configured semaphore-wait budgets**, derived from the unchanged
pinned firmware `targets/f7/ble_glue/furi_ble/central.c` at commit
`df553f8322f41efed97d6b4dbbcb7b75918600d7`. They are not measured physical
latency or strict wall-clock upper bounds. Native mutex acquisition uses
`FuriWaitForever`, ACI/controller delivery adds overhead, and thread scheduling
and callback draining add delay. The host fake tests ordering and cancellation
boundaries; its synthetic timeouts do not measure these firmware internals.

| In-flight call | Configured wait budget before returning |
| --- | --- |
| Connect | Up to 16 s: 8 s connection, 2 s cancellation completion, possible 6 s link termination |
| Find command characteristic | Up to 14 s: two 4 s GATT waits, plus 6 s termination on timeout |
| Find status characteristic and CCCD | Up to 18 s: three 4 s GATT waits, plus 6 s termination on timeout |
| Subscribe | Up to 10 s: 4 s GATT wait plus 6 s termination on timeout |
| Read status | Up to 14 s: ordinary and possible long-read waits of 4 s each, plus 6 s termination on timeout |
| Write Without Response | No ATT procedure wait; controller/ACI call and mutex overhead still apply (see below) |
| Disconnect | Up to 6 s per attempt |
| Deinitialize | Possible 2 s pending-connect cancellation plus 6 s disconnect per attempt |

A timed-out GATT procedure marks the link unusable and tries disconnection before
return, which accounts for the extra 6 s above. Cancellation is considered at the
next controller boundary, without draining ordinary work first. An otherwise
idle worker ticks every 20 ms, subject to scheduling. Cleanup can take additional
calls and retries; three disconnect failures can lead to three deinit attempts.
There is no proven end-to-end cancellation deadline on this firmware. Reducing
these native waits or exposing cancellable procedures requires upstream firmware
work and physical evidence; prioritizing the application inbox alone cannot do
that. Keep issue #3's physical acceptance gate open.

## Waits with no budget

The table above covers only the configured semaphore waits. The calls below,
read from the same pinned firmware and its `lib/stm32wb_copro` submodule
(`133182d`), wait with no timeout or end in a firmware assertion. They are
source findings, not observed behavior; no Flipper has exercised them.
Tracked in [#18](https://github.com/fderuiter/flipper_fredjet/issues/18).

| Wait | Where | Bound in the pinned source |
| --- | --- | --- |
| Pausing advertising before a scan | `furi_hal_bt_stop_advertising()` in `targets/f7/furi_hal/furi_hal_bt.c` | None. It queues a stop for the GAP thread, then polls once per tick until the GAP state is idle |
| Every ACI command (connect, terminate, scan start/stop, GATT calls, Write Without Response) | `hci_send_req()` in `hci_tl.c`, waiting through `hci_cmd_resp_wait()` in `targets/f7/ble_glue/ble_app.c` | Up to 33 s for the radio core's reply, then `furi_check` fails and the Flipper crashes. It never returns an error to the app |
| Admission to the ACI transport | `ble_app_hci_status_not_handler()` in `ble_app.c` | `hci_mtx` is taken with `FuriWaitForever`, so a call waits behind any other thread's ACI command (the GAP thread, the phone profile) |
| Stopping the scan, or deinit detaching it | `ble_central_scan_stop()` and `ble_central_scan_callback_clear()` in `central.c` | Waits with `FuriWaitForever` for an in-flight scan callback to finish. The app's callback copies at most 31 bytes into its try-lock inbox, so this lasts as long as the BLE event thread takes to run it |
| Replacing or clearing the notification callback | `ble_central_set_notify_callback()` and the start of `ble_central_deinit()` | Waits with `FuriWaitForever` on the notification mutex while a callback runs. Same reasoning as the scan callback |
| Every central call | `ble_central_op_lock()` | Waits with `FuriWaitForever` for the previous central call, including its full budget from the table above |

Advertising pause detail: `gap_advertise_stop()` sets the GAP state to idle
even when its ACI calls fail, so the poll usually ends once the GAP thread runs.
Two cases in the source can keep it from ending. If the GAP advertising timer
(60 s) has queued a low-power restart behind the stop, `gap_advertise_start()` runs it
without checking `enable_adv` and the state leaves idle again, possibly before
the poll sees it; the timer then keeps restarting advertising. This is a race
inferred from reading the code, not reproduced. Second, the app checks
`furi_hal_bt_is_advertising()` before calling stop; if a phone connects in that
gap, the stop terminates the phone's link.

What this means for the app: none of these waits can be cancelled by Back or
shutdown, and the 33 s ACI limit is a crash, not a recoverable failure. The
app calls them only from its worker, which stops advancing until the firmware
call returns. The app does not try to wrap
these in its own timeouts, since it cannot abandon a firmware call safely.
Bounded variants need upstream changes; the
[proposed central accessors](upstream-central-accessors.md) record what to ask
for alongside the error and link-state accessors.

## Device acceptance still required

Record app commit, firmware/radio versions, trace timestamps and results in #3:

1. Generate sustained status notifications and rapid setting repeats. Confirm
   the drop counter, then hold OK on each control row. Verify one Off request,
   actual shutoff on the RF remote and no older setting/preset writes afterward.
2. Press and hold Back during scan, connect, both discovery calls, notification
   registration/subscription, initial read, explicit read and an admitted write.
   Record request, in-flight return, Closing, last command and disconnect times.
   No later setup stage or queued user command should start after cancellation.
3. Request Off and Back/exit together; verify the documented disconnect-first
   policy. Verify long OK release never starts a mode or preset.
4. Repeat cancellation/exit with unreachable peer, failed scan termination and
   deferred deinit. Confirm app callbacks are detached before app context is
   freed and that Flipper remains usable after repeated launches.
5. Reconnect manually. Confirm no old mode, heating, timer, memory or failed Off
   intent is replayed. New deliberate controls must work in the new session.

Host results and build evidence are in [testing](testing.md) and
[validation](validation.md). No Flipper or BedJet has been used for these checks.
