# Project guidance

- This is a native Flipper Zero C app for an owned BedJet 3. Keep changes within
  that scope unless the issue explicitly expands it.
- Preserve the distinction between compiled, simulated, and hardware-tested
  results. Never claim a real BedJet test without its evidence.
- The BLE central API and Full BLE radio stack are experimental dependencies.
  Read `firmware.lock.json`, `docs/setup.md` and the pinned `central.h` before
  changing transport code. Do not invent upstream API functions.
- Keep the protocol encoder/parser independent of Flipper headers. Add exact
  vectors and malformed-input checks for protocol changes.
- BLE event callbacks must stay short and nonblocking. Perform controller
  operations on the worker, never in GUI or event callbacks.
- Treat observed status as authoritative. Do not label an accepted write as an
  applied setting. Never replay a heating/mode command after reconnect.
- Retain firmware/device limits, input bounds and stale-data checks. Do not
  loosen parsing to accept unexplained frames; capture and document them first.
- Run `python3 scripts/check.py --sanitizers`. Run the format check with
  clang-format 18 and rebuild the FAP for app/API changes. Record unavailable
  hardware checks explicitly. Do not add production tests that merely restate
  implementation; exercise behavior, bounds, and failure paths.
- Build tools must not flash devices or publish releases implicitly. No
  credentials, BedJet Wi-Fi details, Bluetooth bonds or personal logs in Git.
- Source builds go in ignored `dist/`. The retained initial artifact is
  immutable evidence: do not overwrite it with an untracked build. If its
  source changes, remove/archive it or deliberately rebuild it and update its
  provenance. Run `python3 scripts/verify_artifact.py` before publishing it.
- Update the linked issue, validation record and relevant docs with each
  behavior change. Use focused commits; avoid unrelated firmware refactoring.
