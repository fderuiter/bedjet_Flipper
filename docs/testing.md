# Lifecycle, priority and command test harness

The host suite drives `bedjet_controller.c`, the same production lifecycle,
input, queue and status logic called by the native Flipper worker. There is no
second simulated controller. `tests/controller_test.c` substitutes only the
transport calls and monotonic clock, records writes, injects failures and emits
callbacks. Time advances by explicit increments and timeout results; tests do
not sleep or depend on a real Bluetooth adapter.

## Run

```sh
python3 scripts/check.py --static
python3 scripts/check.py --sanitizers
python3 scripts/check.py --sanitizers --short-enums
python3 scripts/check.py --format --clang-format clang-format-18
python3 scripts/verify_artifact.py
python3 scripts/captures.py bringup.log -o tests/captures/NAME.cap
python3 build.py /path/to/clean/pinned/flipper-firmware
```

`--static` also runs `tests/test_wizards.py`: the setup wizard's checks,
firmware fetch (from a local temporary Git repository), refusal to modify
existing folders and CI-download verification; the acceptance wizard's
checklist parsing, report sanitizing, resume, and log/Info-page frame
conversion, including replaying its output through `tests/capture_test.c`. No
network, Flipper or BedJet is used.

[CI](../.github/workflows/ci.yml) runs the same host checks with GCC and Clang,
then the pinned-firmware FAP build, on every push and fork pull request.

The short-enum run checks the representation used by the ARM build, which can
differ from the host ABI. The formatter checks current production and test C
files, excluding the byte-exact archived prototype source.

AddressSanitizer, LeakSanitizer and UndefinedBehaviorSanitizer are enabled by
`--sanitizers`; `-fno-sanitize-recover=all` makes any UBSan report fail the run
instead of printing and continuing. Earlier records used
`ASAN_OPTIONS=detect_leaks=0` in a container that blocked LeakSanitizer's process
inspection. Do not disable LeakSanitizer on a host where it works.

## Covered behavior

The suite has 72 named scenarios, plus a separate view-label suite. Some scenarios iterate over multiple failures
or lifecycle stages. Fifty-three use deterministic fake transport/time behavior;
two also exercise four concurrent producer threads. The stress tests assert
outcomes, not scheduling-dependent drop or Off-coalescing counts.

| Area | Assertions |
| --- | --- |
| Transport contract | Missing required operations are rejected; unsupported radio never initializes the driver |
| Advertising and scan | Advertising stop deadline, scan start/stop failure, eight-second scan boundary, empty result |
| Discovery | Duplicate identity/RSSI update, address-type distinction, service UUID/name filtering before the inbox, unrelated-report floods, scan-response name merge, unknown scan responses filtered, sanitized/truncated names, connectability, six-device cap and overflow note, malformed AD records, queued reports merged at scan end |
| Input and UI policy | Replayed firmware Press/Long/Repeat/Release sequences: 1 s hold for Off (once per hold), 0.3-1 s release only hints, short presses keep row actions, long OK counts on Home, arrow Long/Repeat steps, Back on Press; mode draft follows observed mode; Units/Info rows never write |
| Setup errors | Stage-specific connect, discovery, missing-CCCD and subscribe messages; raw BLE status retained as diagnostics |
| Setup | Connect, command discovery, status discovery/CCCD, subscription and initial read; each mandatory stage failure/timeout |
| Off/idle reads | Short or failed initial read leaves status unknown; no automatic mode/start write |
| Callback reentrancy | Notifications during subscribe, read, write and removal are copied/enqueued, not processed recursively |
| Queue ownership | Posted payload is independent of the caller's buffer; invalid lengths cannot enter the inbox |
| Cancellation | Back survives a saturated or contended inbox; shutdown discards queued controls; cancellation/shutdown at every setup stage and callback registration prevents later operations/writes or a published ready screen |
| Sessions | Peer loss invalidates status and queued work; old-session keys/status are rejected; reconnect emits no commands |
| Freshness | Receive timestamp retained; stale, out-of-order, wrong-handle and malformed packets cannot refresh state; uint32 clock wrap |
| Diagnostics | Rejected frames recorded with reason and raw bytes, never applied; short frames admitted and oversize counted; 11-byte read tail recorded but not applied; refresh resubscribes and reads once, rate-limited, never writes; evidence reset per connection |
| Controls | Mode, temperature, fan, timer, memory and Off byte vectors; observed values remain authoritative; device bounds respected |
| Write failure | Error exposed, observed value unchanged, no automatic retry |
| Teardown | Temporary/persistent deinit deferral; callbacks detached before context free; scan/disconnect failures; bounded scan-stop retries; inherited scan/link at launch; advertising restored only on confirmed cleanup |
| Priority Off | All eight rows, full mixed inbox, held inbox lock, repeated Off coalescing, Back/shutdown precedence, stale/unknown status, Off during read/write, failed Off and lost link without retry/replay |
| Priority cleanup | Back during Off callback drain/write and refresh, scan/init boundaries, repeated Back during failed teardown; every tick inspected after setup cancellation |
| Command intent | Rapid temperature/fan accumulation, clamping, reversal/undo, one pending setting across kinds, no redundant follow-up |
| Observation | Strict post-write receive boundary, matching fields, stale/out-of-order/malformed/wrong-handle status, timer tolerance and explicit reads |
| Command failure | Sticky transport error, exact timeout boundary, delayed/overflow-lost confirmation, blocked reads, dropped deferred/raw input, Off timeout and no revival/retry |
| Mode/RF policy | Old/Wait/new-mode limits, late limits after timeout, memory Unverified, preset settle window, timed-out Mode/Off not blocking presets, changed bounds, timer tolerance boundaries, zero timer limit, RF-mode conflict and old-key invalidation |
| Intent cancellation | Off preempts coalesced settings; peer loss/reconnect and cancellation during follow-up never replay work; command clock wrap |
| Concurrency | Four producers post 5,000 status events each while the consumer runs, and every post is either admitted or counted as dropped; another four contend on priority requests, then reject old-session requests after reconnect |

The separate codec suite has exact command vectors, one rejecting vector per
validation rule with its reported reason, and every truncation length for
complete and partial frames. All 8,192 single-byte mutations of the reference
frame are checked against an expected result written from the documented byte
layout, not from the decoder, and every truncation of each mutant runs under the
sanitizers. Deleting any one non-redundant rule fails the suite; two
comparisons are implied by others (declared length below 18, minimum above
maximum) and cannot be distinguished.

## Boundary and ownership contract

- `bedjet_controller_init` configures fixed storage and atomics without hardware
  calls. `step` is single-consumer; each native worker tick advances a lifecycle
  stage and publishes a locked model snapshot for the GUI.
- Transport operations are synchronous calls on that worker with the configured
  [native wait budgets](cancellation.md#native-wait-budgets). Time is
  supplied as monotonic uint32 milliseconds. Cancellation is checked between
  stages and before sending a control; an in-flight call cannot be preempted by
  this controller.
- GUI and BLE producers use bounded, nonblocking post functions. A short atomic
  try-lock protects a 24-slot inbox. Ordinary events are dropped/counted on
  contention or capacity exhaustion. Back and Off use a separate session-bound atomic word; shutdown is global.
  Three strong CAS attempts bound priority posting, independently of the inbox.
- Input and callback work carries a session token. Cancelling, servicing Off, starting a new
  scan/connection, or stopping invalidates old work. Setup rejects normal input.
  Off preserves observed status, drains/rebinds notifications, and reopens control
  only for new intent. Back supersedes Off; neither is queued behind telemetry.
- Command state separates desired/sent targets and actual observations. One
  confirmable command can be pending. A same-kind temperature/fan target can
  coalesce, then dispatch only after the first target is observed. A separate
  input generation discards raw keys older than failures or mode changes.
  See [the full matching and conflict policy](commands.md).
- The transport must synchronously drain a removed notification callback.
  A successful scan stop drains its callback; a failed stop may retain it.
- `deinit` must drain/detach **both callback types even when it returns false**.
  This is the pinned firmware API's documented contract. The adapter clears its
  sink pointers only after that boundary. The fake deliberately completes
  callbacks during removal/deinit and verifies no callback can touch freed app
  context afterward.
- After three deferred deinitialization attempts, the app records
  `cleanup_deferred`. Firmware may still track a radio operation; it no longer
  owns application callbacks. Advertising is not reported as restored in that
  case. This dependency must be re-reviewed if the firmware pin changes.

The tests verify the controller and model the upstream drain contract; they do
not prove the physical firmware honors that contract. Flipper adapter signatures
and imports are checked by the real FAP build. Pairing, RF timing and actual
controller behavior remain hardware acceptance work.

## Memory and scope

The portable controller allocates no heap. Compile-time guards limit it to
4,096 bytes and the native application's complete state allocation to 4,096
bytes. The native worker is allocated a 4,096-byte stack; the existing app-entry
stack remains 3,072 bytes. These are fixed budgets, not measured hardware stack
watermarks. Host sizes and native binary results are in the validation record.

The harness covers the portable priority (#3) and command-intent (#4) policies.
Physical latency, command/status behavior and the firmware drain contract remain
device acceptance work. Automatic stale/off-state recovery (#5) and
renamed-device/pairing UX (#6) remain separate work. The refresh boundary only reads status; it is not
new automatic-recovery behavior.

## Recorded coverage

For the original issue #7 host run on October 16, 2026, gcov reported 100% of executable controller
lines, 100% of branches executed, and 81.84% of branches taken at least once.
Coverage is a guide to gaps, not proof of correctness. It excludes the native
GUI/adapter and the Bluetooth firmware. See `docs/validation.md` for the exact
checks and the remaining physical acceptance gate.


For the subsequent issue #3 run, gcov reported 99.76% of 421 executable lines,
100% of branches executed and 84.79% taken at least once. The defensive
exhausted-CAS return was not reached. Eleven new scenarios extend the suite to
33; these coverage figures still exclude the native UI/adapter and firmware.


For issue #4, 22 new command scenarios extend the suite to 55. The clean run
reported 99.16% of 595 executable controller lines, 100% branches executed and
87.36% taken at least once. Defensive CAS/pending-kind/stale-dispatch fallbacks
were not reached. Physical command confirmation and RF behavior remain outside
this host coverage; see [the command acceptance procedure](commands.md#physical-acceptance-still-required).
