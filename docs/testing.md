# Lifecycle test harness (issue #7)

The host suite drives `bedjet_controller.c`, the same production lifecycle,
input, queue and status logic called by the native Flipper worker. There is no
second simulated controller. `tests/controller_test.c` substitutes only the
transport calls and monotonic clock, records writes, injects failures and emits
callbacks. Time advances by explicit increments and timeout results; tests do
not sleep or depend on a real Bluetooth adapter.

## Run

```sh
python3 scripts/check.py --sanitizers
python3 scripts/check.py --sanitizers --short-enums
python3 scripts/check.py --format --clang-format clang-format-18
python3 scripts/verify_artifact.py
python3 build.py /path/to/clean/pinned/flipper-firmware
```

The short-enum run checks the representation used by the ARM build, which can
differ from the host ABI. The formatter checks current production and test C
files, excluding the byte-exact archived prototype source.

AddressSanitizer and UndefinedBehaviorSanitizer are enabled by `--sanitizers`.
In this task's container LeakSanitizer cannot inspect processes, so the recorded
host runs used `ASAN_OPTIONS=detect_leaks=0`; the other two sanitizers remained
enabled. Do not disable LeakSanitizer by default on a host where it works.

## Covered behavior

The suite has 22 named scenarios. Some scenarios iterate over multiple failures
or lifecycle stages. Twenty-one use deterministic fake transport/time behavior;
one additionally stresses the real inbox with four concurrent producer threads.
The stress test asserts outcomes, not a scheduling-dependent drop count.

| Area | Assertions |
| --- | --- |
| Transport contract | Missing required operations are rejected; unsupported radio never initializes the driver |
| Advertising and scan | Advertising stop deadline, scan start/stop failure, eight-second scan boundary, empty result |
| Discovery | Duplicate identity/RSSI update, address-type distinction, service UUID/name filtering, malformed/unrelated AD records, six-device cap |
| Setup | Connect, command discovery, status discovery/CCCD, subscription and initial read; each mandatory stage failure/timeout |
| Off/idle reads | Short or failed initial read leaves status unknown; no automatic mode/start write |
| Callback reentrancy | Notifications during subscribe, read, write and removal are copied/enqueued, not processed recursively |
| Queue ownership | Posted payload is independent of the caller's buffer; invalid lengths cannot enter the inbox |
| Cancellation | Back survives a saturated or contended inbox; shutdown discards queued controls; cancellation/shutdown at every setup stage prevents later operations/writes |
| Sessions | Peer loss invalidates status and queued work; old-session keys/status are rejected; reconnect emits no commands |
| Freshness | Receive timestamp retained; stale, out-of-order, wrong-handle and malformed packets cannot refresh state; uint32 clock wrap |
| Controls | Mode, temperature, fan, timer, memory and Off byte vectors; observed values remain authoritative; device bounds respected |
| Write failure | Error exposed, observed value unchanged, no automatic retry |
| Teardown | Temporary/persistent deinit deferral; callbacks detached before context free; scan/disconnect failures; advertising restored only on confirmed cleanup |
| Concurrency | Four producers post 5,000 status events each while the consumer executes the real controller; bounded queue drains without writes |

The separate codec suite retains its exact vectors and 270,336 systematic
mutation/truncation cases.

## Boundary and ownership contract

- `bedjet_controller_init` configures fixed storage and atomics without hardware
  calls. `step` is single-consumer; each native worker tick advances a lifecycle
  stage and publishes a locked model snapshot for the GUI.
- Transport operations are bounded synchronous calls on that worker. Time is
  supplied as monotonic uint32 milliseconds. Cancellation is checked between
  stages and before sending a control; an in-flight call cannot be preempted by
  this controller.
- GUI and BLE producers use bounded, nonblocking post functions. A short atomic
  try-lock protects a 24-slot inbox. Ordinary events are dropped/counted on
  contention or capacity exhaustion. Back and shutdown use separate latches.
- Input and callback work carries a session token. Cancelling, starting a new
  scan/connection, or stopping invalidates old work. Setup rejects normal input.
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

This refactor supplies the regression foundation for #3–#6. It adds minimal
session/cancellation invariants needed to make the tests meaningful, but does
not finish Off prioritization/physical latency (#3), desired-setting coalescing
and command confirmation (#4), automatic stale/off-state recovery (#5), or
renamed-device/pairing UX (#6). The refresh boundary only reads status; it is not
new automatic-recovery behavior.

## Recorded coverage

For the October 16, 2026 host run, gcov reported 100% of executable controller
lines, 100% of branches executed, and 81.84% of branches taken at least once.
Coverage is a guide to gaps, not proof of correctness. It excludes the native
GUI/adapter and the Bluetooth firmware. See `docs/validation.md` for the exact
checks and the remaining physical acceptance gate.
