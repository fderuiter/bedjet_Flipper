# Changelog

## Unreleased

- Add session-bound Back/Off priority delivery. Hold OK sends Off from any control
  row, discards older queued work and never replays on reconnect; short OK on
  the Off row is replaced by that hold binding.
- Apply cancellation before publishing setup results, show closing state and a
  sticky queue-drop count, and add priority/race-boundary regression scenarios.
- Document native firmware wait budgets and the remaining physical latency gate.

- Extract the production BLE lifecycle/input controller behind injectable
  transport and monotonic-time operations, with a thin Flipper adapter.
- Add host lifecycle/failure tests, concurrent inbox stress, sanitizer runs and
  a short-enum ABI check. Back/shutdown latches and session tokens make cleanup
  and stale-event rejection testable without hardware.
- Archive the original prototype source alongside its unchanged binary and hashes.

## Initial prototype — 2026-10-15

- Initial native Flipper Zero BedJet 3 BLE prototype.
- Scan/select/connect, mode, target temperature, fan, timer, stored memory and Off.
- Structural status validation and stale-data indication.
- Firmware pin, reproducible build entry point, portable sanitizer tests and
  manual GitHub Actions workflow.
- Initial compiled prototype retained with source/binary hashes.

No hardware-tested release has been published. See `docs/validation.md`.
