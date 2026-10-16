# Command intent and observed results (issue #4)

The device's decoded status stays authoritative. A write accepted by the Flipper
controller does not update the displayed actual mode, temperature, fan or timer.
The control row can show a separate **want** value, and the footer reports the
last command result. This behavior is host-tested and compiled, not yet tested
against a BedJet.

## One pending command

There is one command awaiting observation across all settings, plus one latest
unsent target for the same temperature or fan setting. There is no deferred
queue of other command types.

For example, with an actual temperature step of 60, three Right inputs produce:

| Event | Actual step | Sent target | Desired target | Writes so far |
| --- | --- | --- | --- | --- |
| First Right | 60 | 61 | 61 | `03 3d` |
| Second and third Right, before new telemetry | 60 | 61 | 63 | Still one write |
| Newer status reports 61 | 61 | 63 | 63 | One follow-up, `03 3f` |
| Newer status reports 63 | 63 | 63 | 63 | Applied; no further write |

The first input sends immediately. Repeats accumulate from **desired**, not the
last actual value. Values clamp to the current device temperature range or fan
steps 0–19. Reversing input changes only the latest target; returning desired to
the already-sent value prevents a redundant follow-up. A follow-up is a distinct
user-requested target, never a retry of an unconfirmed command.

When processed while a command is pending, other actuator commands and timer
edits are rejected with a wait message. Navigation and drafting a mode choice remain available.
Rejected intents are not saved to run later. Admission occurs on the worker;
ordinary raw keys retain FIFO interpretation, so a key processed after a command
finishes can become a new intent. Hold-OK Off always uses
its priority path and cancels the previous intent; Back/shutdown also cancel it.

Ordinary key/telemetry delivery retains the bounded, lossy inbox from #3. Only
accepted, processed repeats accumulate; saturation cannot promise every tap was
received. The sticky `!N` counter still reports overflow/contention.

## Results and matching rules

`BedjetModel.command` separates kind, state, desired target, sent target, the
post-write time boundary and a persistent result string. Incoming valid status
updates actual values and restores this result instead of claiming generic
"Live device status" cleared a command error. A new user action can temporarily
show a validation/wait message; the stored command result remains separate.

| State | Meaning |
| --- | --- |
| Idle | No command in this session |
| Pending | Write accepted; waiting for a matching observation |
| Queued | Previous target observed; latest coalesced target ready for one worker dispatch |
| Applied | Matching newer actual value observed; this is not a protocol acknowledgment |
| Rejected | Transport write failed, or newly reported limits make the intent invalid |
| TimedOut | No matching observation before the five-second observation window expired |
| Conflict | Device mode changed while a temperature, fan or timer intent was pending |
| Cancelled | Session/priority cancellation discarded the pending intent |
| Unverified | A memory start was accepted, but decoded telemetry cannot identify that preset |

Confirmation requires a valid, correct-handle status in the same session, less
than 15 seconds old and not older than the last accepted observation. Its
**receive timestamp must be strictly later than the write's return timestamp**.
Packets queued before or during the write, including matching packets in the
same clock tick, cannot confirm it. Explicit status reads use their completion
time. A later periodic update may be needed if the first update arrived too soon.

| Command | Matching observation |
| --- | --- |
| Temperature | Exact target step, with the same mode as at dispatch |
| Fan | Exact fan step, with the same mode as at dispatch |
| Mode | Exact requested observed mode: Cool 4, Heat 1, Dry 5, Extended Heat 3 |
| Off | Observed mode 0 |
| Timer | Same mode, remaining time no higher than requested and no lower than requested minus elapsed whole seconds and a two-second tolerance |
| M1/M2/M3 | No preset identity in the decoded core; remains Unverified |

Mismatching telemetry does not prove device rejection; transitions and RF-remote
changes are possible. It updates actual state while the intent stays Pending.
No parsed device negative acknowledgment exists here. Rejected therefore denotes
transport failure or a client-side limit conflict; otherwise a persistent
mismatch becomes **Not confirmed; no retry**, not an invented device error.
Timer tolerance and the five-second window are explicit initial policies that
need physical validation, including whether off-state telemetry is available.

The five-second window starts when the synchronous write returns. Expiration is
checked on worker execution, including after blocking reads. Once expired, even
a matching frame captured before expiry but processed afterward cannot revive
the command. Late matches still update actual status. Dropped confirmation can
produce a timeout even when the device applied the command; the app does not
resend to resolve that ambiguity. Synchronous firmware calls can delay worker
execution beyond five seconds; see [native wait budgets](cancellation.md#native-wait-budgets).

## Mode limits and external changes

A mode or Off dispatch locks temperature and timer controls until a strictly
newer matching-mode status supplies its limits. Old-mode, same-tick and
transitional Wait packets do not unlock them. A timeout does not unlock old
limits. A late matching-mode status can unlock controls for **new input** without
changing a TimedOut result or replaying old intent. A transport-rejected mode
instead requires a new valid actual-mode status to resynchronize limits.

Memory start also invalidates limits. Its resulting mode cannot be known from
the memory slot, and the first newer frame can still report the pre-press mode.
Limits therefore return only on a newer, non-Wait status that shows a mode
change, or the same mode at least two seconds (`BEDJET_PRESET_SETTLE_MS`) after
the write. The preset result stays Unverified. Another memory start waits for
that observation; a deliberate new mode or priority Off can supersede it. A
timed-out Mode or Off keeps temperature/timer limits locked but does not block a
deliberate preset start. The two-second window is an initial policy that needs
device evidence.

Every observed mode switch, app-requested or external, discards older raw key
inputs. Mode/preset/Off dispatch and terminal failures also advance the input
generation. Thus delayed inputs cannot become unintended retries or settings
under another mode's limits. Back/Off priority and BLE session tokens remain
independent of this ordinary-input generation.

External RF-remote values always update the actual display. A different mode
abandons a pending temperature/fan/timer intent and its unsent target. A different
value in the same mode does not trigger a retry: the app waits for a match or
times out. If a newer observation matches, any remaining explicit coalesced
input may proceed. The protocol cannot distinguish an RF-remote change from an
app-caused match; this policy does not claim causal attribution or unconditional
RF-remote priority. After failure/conflict, new deliberate input rebases on the
latest actual value.

Newly reported temperature/runtime bounds can reject an already-sent target.
A coalesced temperature target is reclamped before its follow-up; an invalid or
stale limit prevents dispatch. A zero runtime limit blocks timer writes.
Disconnect, shutdown and Off discard unsent targets. Reconnect clears command
state and never replays mode/heating, temperature, fan, timer, preset or failed
Off writes. No command is automatically retried.

## Physical acceptance still required

With the pinned firmware and a real BedJet, record app commit, radio/device
versions, notification captures and timestamps in #4:

1. Compare rapid temperature/fan repeats and reversals with the RF remote. Check
   accumulation, clamping, one pending command and each coalesced follow-up.
2. Change modes with different temperature/runtime limits. Confirm old, partial
   or transitional status never releases controls with inappropriate bounds.
3. Verify command receipt versus observed application, late/mismatching status,
   genuine device clamping/rejection, missing off-state status, and the timer
   countdown tolerance. Tune the five-second window from measured behavior.
4. Change values and modes with the RF remote during pending app input. Verify
   actual display, conflict handling and that the app does not retry a mismatch.
5. Interrupt pending/coalesced commands with Off, Back, peer loss and app exit.
   Reconnect and verify no deferred intent or failed command is replayed.
6. Check want/actual labels and persistent result text on the physical 128x64
   display. Preset identity must stay Unverified unless supported by separately
   validated protocol fields in a future change.

Mode/command values were checked against the existing codec and the
[community V3 protocol reference](https://github.com/roycamp/bedjet-protocol/blob/main/BEDJET_V3_BLUETOOTH_PROTOCOL.md).
The decoder and packet assumptions are unchanged. See [testing](testing.md) and
[validation](validation.md) for executed host/build evidence and remaining gates.
