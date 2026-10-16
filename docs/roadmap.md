# Roadmap

Live tracker: [#11](https://github.com/fderuiter/flipper_fredjet/issues/11).
This file records the initial plan; GitHub issues hold current status and evidence.

## Priorities

- **P0**: release blocker for a dependable basic remote.
- **P1**: core reliability or compatibility work for everyday use.
- **P2**: usability and release engineering after the critical behavior is sound.
- **P3**: future scope requiring separate protocol investigation.

| Priority | Issue | Outcome | Prerequisites |
| --- | --- | --- | --- |
| P0 | [#1 Firmware baseline](https://github.com/fderuiter/flipper_fredjet/issues/1) | Supported physical Flipper + Full BLE setup and compatibility evidence | Pinned build already available |
| P0 | [#2 BedJet acceptance](https://github.com/fderuiter/flipper_fredjet/issues/2) | Real pairing, command, status and limit verification; sanitized captures | #1 |
| P0 | [#3 Cancellation and queue priority](https://github.com/fderuiter/flipper_fredjet/issues/3) | Back/Off delivery; discard stale control on cancellation/session change | Use regression harness from #7 |
| P1 | [#4 Command intents and confirmation](https://github.com/fderuiter/flipper_fredjet/issues/4) | Rapid controls work correctly; mode limits and applied settings confirmed | #3 and #7 |
| P1 | [#5 Status recovery and diagnostics](https://github.com/fderuiter/flipper_fredjet/issues/5) | Bounded stale/off-state recovery and verified protocol parsing | Captures from #2, harness #7 |
| P1 | [#6 Discovery and pairing](https://github.com/fderuiter/flipper_fredjet/issues/6) | Renamed/multiple units and actionable errors | Evidence from #2, harness #7 |
| P1 | [#7 BLE lifecycle harness](https://github.com/fderuiter/flipper_fredjet/issues/7) | Deterministic failure, queue, cancellation and teardown tests | Can start before hardware arrives |
| P2 | [#8 Small-screen UX](https://github.com/fderuiter/flipper_fredjet/issues/8) | Accessible Off, readable errors, current/pending/timer state | Coordinate with #4 and #5 |
| P2 | [#9 Release and CI evidence](https://github.com/fderuiter/flipper_fredjet/issues/9) | Validated prereleases, provenance and explicit required-check policy | P0/P1 gates; Actions when available |
| P3 | [#10 Biorhythm protocol discovery](https://github.com/fderuiter/flipper_fredjet/issues/10) | Verified read/write/clock design before an editor | Basic hardware and reliability gates |

## Recommended order

Start the firmware baseline (#1), then the physical BedJet acceptance work (#2).
The host harness (#7) can progress while hardware testing is being arranged. Use
it to fix cancellation (#3) and command sequencing (#4), then stale telemetry
and pairing/discovery (#5, #6). Complete UX (#8) and release evidence (#9) before
describing the app as ready for everyday use. Biorhythm editing (#10) is later
scope, not a prerequisite for a basic remote.

## Release evidence

Close issues with the relevant test/build/device record. A merged PR alone does
not close a hardware acceptance gate. At minimum, retain the app source revision,
firmware/radio pin, toolchain, binary hash, actual device versions, controls tested,
failure-path results and remaining limitations.

No issue in this list is marked complete by the initial import. The current
status remains a compiled, hardware-unverified prototype.
