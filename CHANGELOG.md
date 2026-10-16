# Changelog

## Unreleased

- Document firmware waits that have no timeout (advertising pause, ACI
  responses, callback drains) and draft, without submitting, upstream central
  accessors for the failure reason, disconnect reason and link usability (#18).
- CI runs automatically on every push and fork pull request: GCC and Clang
  host checks with fatal sanitizers, clang-format 18, doc-link checks, and a
  pinned-firmware FAP build uploaded with provenance. FAP builds pin
  `PYTHONHASHSEED=0` after unpinned builds were found to vary in link order.
- Off needs a deliberate 1 s hold (the firmware's 300 ms Long sent Off on a
  slightly long OK). Long presses now count for arrows and for OK off the
  control screen. The mode draft follows the actual mode; OK on the current
  mode sends nothing. New Units (F/C display) and Info rows; remaining runtime
  and LIVE/STALE/NO DATA/BAD DATA badges are shown.
- Telemetry evidence: rejected frames are kept with the decoder's reason, the
  status read's raw bytes are recorded (references read an 11-byte tail, not a
  status packet), and Info refresh resubscribes and reads once. Frames are
  also logged as hex for captures.
- Discovery filters before the inbox, merges scan responses by address, shows
  unit names, and reports stage-specific setup errors.
- Fixes: session rotation admitted racing input into cleanup; scan stop
  retried forever; scan residue dropped keys; inherited radio state after a
  deferred teardown; presets unlocked limits on a pre-press status.
- Tests: decoder rules checked against a documented-layout oracle, plus input
  sequence, view, discovery and diagnostics scenarios (72 controller scenarios).

- Separate desired settings from actual status, with one pending command and a
  latest coalesced temperature/fan target. Add matching observation results,
  persistent transport errors, conflicts and five-second observation timeouts.
- Require new-mode limits before temperature/timer writes; discard older keys
  on mode changes or failures. Preserve Off priority and no retry/replay.
- Show want values and locked controls; label memory starts Unverified because
  the decoded telemetry cannot identify a preset. Document RF-remote policy.

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
