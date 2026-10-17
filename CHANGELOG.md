# Changelog

## Unreleased

- Flipper control from the terminal (`hardware_session.py open`, `close`,
  `scan`, `key KEY [--long]`, `app`) and `/ok`-style commands during `run`,
  recorded as timestamped `flipper:` notes. Presses use the CLI's `input send`
  (Press, Short or Long, Release); `close` falls back to Back because FredJet
  ignores the loader's exit signal. Nothing is pressed unless typed.
- `hardware_session.py` fixes from the first session (#30): `doctor` reads
  `firmware.commit.hash`; `push-update` accepts the CI download layout; and
  `--flash` reports the updater restart instead of "Stopped: disconnected".
- Detailed diagnostic logging (#31), all at Info level or above so
  `hardware_session.py run` records it. At start: firmware version and commit,
  radio stack, central support and free heap. Every BLE call: its arguments,
  result, duration and the firmware's last status, with scanning, connected and
  advertising states on failure. Read and written bytes in hex. Link up/down
  with duration and notification count. Polled states only when they change.
  App phase and screen changes, each unit found at scan end (name, last two
  address bytes, RSSI, what matched), scan accounting (matched, filtered,
  refused), command state changes, dropped events, late, oversized and failed
  reads, and deferred cleanup. Addresses show only their last two bytes.
- Scans now log `Scan heard N reports (S scan responses, O over 31 bytes)`:
  every advertising report the Flipper adapter received, before the BedJet
  filter, then how many of those contain `BEDJET` or the service UUID in their
  raw bytes, with the first such report's bytes in hex (no address) (#31).
  Diagnostic for the first hardware session, where no scan found a BedJet that
  a Mac saw advertising.
- Hardware session runner (#28): `scripts/hardware_session.py` talks to the
  Flipper's USB serial CLI (standard library only). It copies `fredjet.fap`
  and the Full BLE update package to the SD card with MD5 verification on the
  Flipper, compares `info device` with `firmware.lock.json`, records `log info`
  with host timestamps, session markers and timestamped notes while walking
  sessions A-G from `docs/bringup.md`, and writes per-session capture
  candidates, host replay verdicts and a sanitized summary under `dist/bench/`.
  It never sends BedJet commands; installing firmware needs `--flash` and a
  typed `yes`. Tested against a scripted CLI, not yet against a real Flipper.
  See [docs/hardware-session.md](docs/hardware-session.md).
- Setup failures now name an inferred cause (`no reply`, `link lost`,
  `BLE 0xNN`), and a status read or refresh that gets no reply disconnects with
  `No reply: reconnect` instead of leaving a link on which every control fails
  (#18). The firmware does not report these causes; they are inferred.
- The app id is now `fredjet` (#17): builds produce `dist/fredjet.fap`, the
  CI artifact is `fredjet-fap-...` and the entry point is `fredjet_app`. Delete
  an older `bedjet_remote.fap` from the SD card to avoid a duplicate menu entry.
  The retained prototype keeps its original name.
- Document firmware waits that have no timeout (advertising pause, ACI
  responses, callback drains) and draft, without submitting, upstream central
  accessors for the failure reason, disconnect reason and link usability (#18).
- App catalog preparation (#17), metadata only: `fap_weburl`, a 10x10 1-bit
  icon and an f7-only target in `application.fam`; catalog README and changelog
  under `.catalog/`; blockers and submission checklist in `docs/app-catalog.md`.
  Catalog submission remains blocked on BLE central in an official SDK.
- The app is now named FredJet, a homage that keeps the BedJet trademark
  out of the app name; BedJet is named only as the compatible device.
- Hardware bring-up runbook (`docs/bringup.md`) for issues #1 and #2, and a
  capture pipeline: `scripts/captures.py` keeps only the app's frame lines from
  a Flipper CLI log, and `scripts/check.py --sanitizers` replays committed
  captures through the parser so real frames keep their classification.
- Guided setup (#16): `scripts/setup_wizard.py` checks the computer, fetches
  and verifies the pinned firmware, builds via `build.py`, prints hashes and
  qFlipper copy steps, and offers `--yes`, `doctor` and `verify-download` for
  CI builds. It never flashes, installs or publishes and never modifies an
  existing folder. `scripts/acceptance_wizard.py` walks the device checklist
  from `docs/validation.md`, saves progress, and writes a sanitized report for
  #2 (full Bluetooth addresses, key material and Wi-Fi details removed); its
  `frames` command turns `log` output into `scripts/captures.py` capture lines
  (Info-page hex is kept as comments). New plain-language [getting-started guide](docs/getting-started.md),
  linked first from the README. Wizard tests run with `check.py --static`.
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
