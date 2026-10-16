# Validation record

Created October 15, 2026 (America/Chicago).

## Executed

- Compiled and linked the actual Flipper application using `fbt`, the ARM
  toolchain, and the pinned central-role firmware revision in the README.
- FAP metadata creation, relocation processing and firmware import checking
  passed against API 88.3. The initial binary is retained at
  `artifacts/prototype/bedjet_remote.fap`; new builds use ignored `dist/`.
- Portable C protocol tests passed with `-Wall -Wextra -Werror` and AddressSanitizer
  plus UndefinedBehaviorSanitizer. LeakSanitizer was disabled because this
  container prevents its process inspection. The portable core allocates no heap.
- Tested exact command bytes for modes, stored memory, temperature, fan and timer;
  input bounds, valid full and partial synthetic status, rejected fields,
  malformed header, truncated input, unchanged output on failure, and 270,336
  systematically mutated/truncated input cases.
- Python build script parsed successfully.
- The matching complete firmware updater package also built successfully using
  `COMPACT=1 DEBUG=0`, the Full BLE radio binary, `COPRO_STACK_TYPE=ble_full`,
  and `scripts/ob_custradio.data`. The generated manifest selected radio
  version 1.20.0 and address `0x080CE000`; that address was cross-checked against
  the pinned STM32WB coprocessor firmware's `Release_Notes.html` table.
  This checks build/package compatibility, not installation or operation.
  Firmware update binaries are not committed to this repository; build them from
  the pinned source following the root README.

## Repository import checks

- The repository build script successfully rebuilt/import-checked the FAP against
  the same clean pinned firmware checkout.
- The repository's host test runner passed with AddressSanitizer and
  UndefinedBehaviorSanitizer (`ASAN_OPTIONS=detect_leaks=0` in this container).
- C formatting passed using the pinned firmware's clang-format 18 toolchain and
  the checked-in `.clang-format`.
- The retained initial FAP and four app source-input hashes were verified using
  `scripts/verify_artifact.py`.
- Python files parsed and the JSON/YAML configuration files passed parsing.
- The manual GitHub Actions workflow is provided but has not been run on GitHub.
  No branch protection or automatic required checks are claimed.

## Issue #7 — portable lifecycle extraction (2026-10-16)

- Extracted `bedjet_controller.c` and wired the production Flipper worker to it
  through `bedjet_flipper.c`. Host tests execute that same controller.
- Passed the codec suite and 22 lifecycle scenarios with `-Wall -Wextra -Werror`,
  AddressSanitizer and UndefinedBehaviorSanitizer, including the short-enum ABI
  run. `ASAN_OPTIONS=detect_leaks=0` was required by this container.
- Covered all mandatory setup-stage failures/timeouts, cancellation and shutdown
  at every setup stage, callbacks during reads/writes/removal, session rejection,
  queue saturation/contention, stale/wrapped time, command vectors, peer loss and
  deferred callback-safe teardown. One scenario adds four concurrent producers
  posting a total of 20,000 events; scheduling/drop counts are not fixed.
- The default host controller size is 1,824 bytes and model size 344 bytes.
  Short-enum host sizes are 1,816 and 340 bytes. Compile-time native state budgets
  are each 4,096 bytes; the worker stack allocation is 4,096 bytes. Runtime
  physical heap/stack watermarks were not measured.
- Rebuilt and import-checked the current FAP against the unchanged locked
  firmware/API 88.3. The generated FAP is 17,616 bytes and is available through
  a current-source build in ignored `dist/`; it is not substituted for the
  retained original prototype binary.
- Archived the byte-exact initial source under `artifacts/prototype/source/`.
  Its unchanged binary and original four source hashes still verify using the
  provenance manifest's `source_root`.
- The clean host coverage run reports 100% controller executable lines and 100%
  branches executed, with 81.84% taken at least once. Native GUI/adapter and radio
  firmware are not included in that coverage.
- The manual workflow now includes the lifecycle and short-enum checks. It has
  not been executed on GitHub. No physical hardware was attached or flashed.

See [the harness contracts and coverage](testing.md). Issues #1/#2 remain the
firmware/hardware gates; #3–#6 still own the remaining production reliability work.

## Issue #3 — priority delivery and cancellation (2026-10-16)

- Added a session-bound atomic Back/Off channel independent of the ordinary
  inbox. Shutdown/Back supersede Off. Off rotates the session and drains/rebinds
  notifications, so older settings, presets, refreshes and telemetry cannot act
  afterward. Failed Off is never retried or replayed on reconnect.
- Hold OK sends Off from any control row; short OK on the Off row is replaced
  by this binding. Back uses Press so a held Back is handled. Queue drops remain
  observable through the sticky title counter. Closing replaces a cancelled
  setup's control screen before the worker publishes its model.
- Passed all 33 lifecycle scenarios and the codec vectors plus 270,336 malformed
  cases with ASan/UBSan in both normal and short-enum host ABIs, using
  `ASAN_OPTIONS=detect_leaks=0` for this container's process-inspection limit.
  Eleven scenarios were added to #7's 22, and existing setup cancellation tests
  now inspect every tick and include notification callback registration.
- Covered saturated mixed input/telemetry on all eight control rows, a held
  inbox lock, repeated requests and priority precedence, Off during read/write,
  cancellation during Off callback removal/rebinding, stale/unknown status,
  missing link, failed Off without retry/replay, scan/init cancellation and
  repeated Back during failed teardown. Four concurrent priority producers add
  20,000 requests and then reject another 20,000 old-session requests after
  reconnect, alongside the existing 20,000-event telemetry stress scenario.
- clang-format 18, whitespace, local documentation links and the unchanged
  historical prototype's binary/four-source hash verification passed.
- The clean gcov run reports 99.76% of 421 controller executable lines, 100% of
  branches executed and 84.79% taken at least once. The unexecuted line is the
  defensive exhausted-CAS fallback; coverage excludes native UI/adapter and
  firmware. These percentages do not prove absence of concurrency races.
- Rebuilt and import-checked the FAP against unchanged locked firmware/API 88.3.
  Current FAP: 19,212 bytes; SHA-256
  `6bd678f7349c0c9e615936d0440aabd83116644d8cb27a1038635fe9a6db3728`.
  Build output stays in ignored `dist/`; the historical binary is unchanged.
  Host controller/model sizes remain 1,824/344 bytes, or 1,816/340 with short
  enums. The native compile-time memory budgets pass; physical stack/heap
  watermarks have not been measured.
- Reviewed pinned firmware wait paths: a status discovery can include up to
  18 seconds of configured semaphore waits, excluding controller, mutex and
  scheduling overhead. See [the policy and wait budgets](cancellation.md).
  Application priority delivery does not establish a physical cancellation
  deadline or preempt an admitted synchronous firmware call.
- No hardware, flashing or GitHub Actions run was involved. Issue #3 remains
  open for its physical acceptance gate; the [device checks](cancellation.md#device-acceptance-still-required)
  must establish actual latency, write ordering and callback lifetime.

## Issue #4 — desired settings and observed results (2026-10-16)

- Added a command model separate from actual status: one pending command and
  one latest coalesced temperature/fan target. Repeats accumulate from desired
  values; a different setting is rejected while pending. No setting is retried.
- A strictly newer matching observation marks a command Applied. Same-tick,
  pre-write, malformed, out-of-order and unrelated observations cannot confirm
  it. Mode/Off writes lock temperature/runtime limits until matching new status;
  presets remain Unverified because the decoded fields cannot identify them.
- Added persistent results, five-second observation expiry, timer tolerance,
  RF-mode conflicts and input-generation barriers. Timeout/failure, Off, Back
  and peer loss discard deferred intent. Late observations update actual state
  without reviving failed work. Reconnect clears command state with no replay.
- UI rows distinguish want values and locked mode limits; actual mode/target/fan
  remain device-reported. Existing input/priority behavior is preserved.
- All **55 lifecycle scenarios** and codec vectors plus 270,336 malformed-input
  cases passed under ASan/UBSan in normal and short-enum ABIs. This adds 22
  scenarios to #3's 33 and updates the vector tests to require observation between
  serialized commands. Both four-producer concurrency scenarios still pass.
- New tests cover rapid accumulation/reversal/clamping, globally one pending
  setting, strict write-return/receive boundaries, timeout/transport failure,
  late and overflow-lost confirmation, blocking reads, mode-limit transitions,
  timer tolerance, changed bounds, memory Unverified, RF conflicts, old raw key
  invalidation, Off preemption, follow-up failure/cancellation and clock wrap.
- clang-format 18, whitespace, local documentation links and the historical
  prototype binary/four-source hashes passed. ASan/UBSan remained enabled;
  `ASAN_OPTIONS=detect_leaks=0` was used for this container's process-inspection
  restriction. No ThreadSanitizer or physical timing result is claimed.
- Rebuilt/import-checked the FAP against unchanged locked firmware
  `df553f8322f41efed97d6b4dbbcb7b75918600d7`, API 88.3. FAP: **22,668 bytes**;
  SHA-256 `6a6ed4e823dab756c70eb5fab7502979342002eb52e0198e9fac3c63a59d9cef`.
  Build output remains ignored in `dist/`; the historical binary is unchanged.
- Host controller/model sizes: 1,984/396 bytes; short-enum sizes: 1,976/388 bytes.
  Native compile-time 4,096-byte state budgets passed. Worker stack allocation
  remains 4,096 bytes; physical stack/heap watermarks have not been measured.
- Clean gcov: 99.16% of 595 controller executable lines, 100% branches executed,
  87.36% taken at least once. Defensive exhausted-CAS, unreachable pending-kind
  default and stale coalesced-dispatch guards were not reached. Coverage excludes
  native UI/adapter and firmware, and does not prove absence of races.
- No hardware was attached or flashed and GitHub Actions was not run. The
  five-second window, timer tolerance, device rejection/clamping, RF conflicts
  and display labels remain physical acceptance work. See
  [command policy and device procedure](commands.md). Issue #4 stays open until
  real-device behavior is recorded; #1/#2 and #3's physical gate also remain.

## Audit follow-up: CI, diagnostics, discovery and UX (2026-10-16)

- An independent audit of the merged #12-#14 code (five review lenses, each
  finding reproduced and challenged separately) confirmed 24 low-to-medium
  findings; the code fixes, test-quality fixes and doc corrections are in this
  change. Each new regression test was checked to fail when its fix is
  reverted.
- Host: 72 controller scenarios, the view suite and the codec suite pass with
  GCC and Clang, ASan/UBSan (now fatal via `-fno-sanitize-recover=all`) and
  LeakSanitizer enabled, in normal and short-enum ABIs. The codec suite checks
  all 8,192 single-byte mutations against a documented-layout oracle; 23 of 25
  single-rule deletions fail it, and the other two are implied by remaining
  rules.
- FAP: compiled and import-checked locally against unchanged pinned firmware
  `df553f8322f41efed97d6b4dbbcb7b75918600d7` (API 88.3, toolchain 39, GCC
  12.3.1): 30,260 bytes. GitHub Actions also built and import-checked the FAP
  for an earlier commit of this change and uploaded it with provenance.
- Reproducibility: three clean relinks of identical sources at one path
  produced different bytes (function order differed) until `build.py` pinned
  `PYTHONHASHSEED=0`; with it, three relinks were byte-identical. Builds from
  different absolute paths have not been compared.
- Upstream: PR #4436 is still open with no maintainer review; its head is
  still the pin. Upstream dev reused API 88.3 for unrelated symbols, so
  provenance now records the commit and `central.h` hash. No official release
  or RC contains BLE central, which blocks the Flipper app catalog.
- Protocol cross-check: ESPHome and the Home Assistant BedJet integration agree
  with the UUIDs, commands and core status offsets, check no status checksum,
  and read an 11-byte tail from the status characteristic. The app now records
  that read instead of treating it as status. Prior-art search found no other
  Flipper BedJet app.
- Not performed: no Flipper or BedJet was attached or flashed. The 1 s Off
  hold, preset settle window, discovery merge and diagnostics need device
  validation under #2-#6 and #8.

## Issue #17 — app catalog metadata (2026-10-16)

- Metadata only; no app behavior changed. Added `fap_weburl`, the 10x10 1-bit
  `fap_icon` and `targets=["f7"]`. The pinned firmware's f18 SDK also exports
  `ble_central_*`, but f18 is untested, so the app stays f7-only.
- `scripts/check.py --static` now rejects an icon that is not a 10x10 1-bit
  PNG and `.catalog/*.md` lines outside the catalog Markdown subset. Both
  failure paths were exercised locally (a 10x11 icon; an H3 heading).
- Passed static checks, sanitizer tests (72 lifecycle scenarios), clang-format
  18 and `verify_artifact.py` locally. The retained prototype is unchanged; its
  provenance hashes its own copy of `application.fam`.
- A local FAP build was not possible here: the container's proxy refused the
  Flipper toolchain download. The FAP build with the icon is verified by the
  CI `FAP against pinned firmware` job on this change. Catalog submission
  itself remains blocked; see [app-catalog.md](app-catalog.md).

## Hardware and integration checks not executed

No Flipper or BedJet was attached. The binary has not been installed or run on
hardware. Pairing, scan visibility, unit-specific firmware behavior, temperature
limits and teardown therefore remain unverified. No device was flashed. No
BedJet, home automation, phone or account settings were changed.

## Device acceptance checklist

1. Check the exact custom firmware and Full BLE stack configuration first.
   Confirm the runtime unsupported-stack screen when using that custom firmware
   with the Light stack; stock firmware may refuse to load the app's imports.
2. With the official BedJet phone app closed, scan and identify the unit by its
   Bluetooth address suffix. Test repeated scans and empty scans.
3. Connect and perform physical BedJet pairing if requested. Verify service and
   characteristic discovery against actual GATT data. A renamed unit may not
   appear if its advertisement lacks the service UUID.
4. Confirm that connecting alone never starts heating or airflow. If status is
   absent while off, explicitly select Cool and press OK.
5. Compare displayed mode, target temperature and fan speed with the physical
   remote. Check half-degree Celsius / decimal Fahrenheit conversion.
6. Change airflow one step and confirm it on the physical remote. Change the
   temperature one step within the device-reported range and confirm it.
7. Set a short timer and confirm its value and eventual shutoff on the unit.
8. Start a known existing M1/M2/M3 memory preset and confirm the selected program.
   This test does not overwrite the stored preset.
9. Test Heat, Dry and Extended Heat individually; compare the temperature and
   time limits reported after each mode change. Verify Off in every mode.
10. Change a setting using the physical RF remote while the app is connected;
    confirm that incoming status updates the app.
11. Leave Bluetooth range or power off the unit. Confirm stale-data indication,
    blocked temperature/fan/timer writes, disconnected state, and no automatic
    reconnect or queued-command replay. Rescan manually.
12. Back out while scanning, connecting, connected and disconnected. Confirm
    the phone companion can connect to Flipper again after app exit. Repeat
    launch/connect/exit cycles and inspect heap usage using firmware diagnostics.
13. Inspect actual status payloads if telemetry is rejected. The parser's
    synthetic fixtures are not a replacement for real packet captures; do not
    weaken bounds checks blindly to accept an unknown packet format.

Until those checks pass, this is a compiled proof of concept, not a verified
replacement remote. The actionable release gates and known defects are linked
from [the roadmap](roadmap.md).
