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

## Not executed

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
