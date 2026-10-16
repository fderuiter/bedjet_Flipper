# FredJet: a BedJet 3 remote for Flipper Zero

Control a **BedJet 3 directly over Bluetooth** from a Flipper Zero.

FredJet is an independent, unofficial app. BedJet is a trademark of its
owner, which does not make, endorse or support this app.

**New here? Start with [Getting started](docs/getting-started.md):** a
plain-language path from nothing installed to a first connection, with setup
and testing wizards, troubleshooting and the way back to official firmware.

**Status: compiled proof of concept. Not hardware validated.** This app requires
experimental Flipper firmware with BLE central support **and the Full BLE radio
stack**. It will not run on stock firmware. No external ESP32, phone app, cloud
service or modification of the BedJet firmware is part of this design.

## Current functionality

- Scan for nearby BedJet devices and select a unit.
- Connect, discover its service and subscribe to device status.
- Select Cool, Heat, Dry or Extended Heat; turn the device off.
- Adjust temperature and airflow with coalesced desired targets; set the auto-off timer.
- Track pending, observed/applied, rejected and timed-out command results.
- Start existing M1/M2/M3 memory presets, reported as unverified by telemetry.
- Show device-reported mode, target temperature and fan speed, and flag stale data.

Temperature uses half-degree Celsius steps, displayed as decimal Fahrenheit.
The timer is bounded by device-reported limits and an application cap of ten hours.
Temperature, fan and timer commands require status less than 15 seconds old.
Temperature/timer controls also wait for matching new-mode limits after a mode
command. [Command policy](docs/commands.md) defines confirmation and timeouts.

Sleep-program editing, saving memory presets, Turbo start, Wi-Fi configuration,
firmware updates and automatic reconnect are not implemented. Settings are not
persisted. The app does not retry commands automatically.

## Compatibility

| Component | Current requirement | Evidence |
| --- | --- | --- |
| BedJet | BedJet 3 / V3 BLE protocol | Community implementations; this unit has not been tested |
| Flipper | Zero, target F7 | Native application compiled and import-checked |
| Flipper firmware | BLE central proposal [PR #4436](https://github.com/flipperdevices/flipperzero-firmware/pull/4436), pinned in [firmware.lock.json](firmware.lock.json) | Open/unmerged when inspected October 15, 2026 |
| Radio firmware | STM32WB Full BLE 1.20.0 | Matching updater package built; not installed |
| Stock / Light BLE | Unsupported | Stock lacks app imports; Light stack lacks central operations |

Build success verifies compilation, not pairing or device behavior. See the
[validation record and hardware checklist](docs/validation.md) before testing.

## Get started

The [getting-started guide](docs/getting-started.md) and
`python3 scripts/setup_wizard.py` do the steps below for you, ask before each
one, and add `doctor` diagnostics. The manual equivalent:

Requirements: Git, Python 3.10+, a C compiler for host tests, and macOS or Linux
for the documented firmware build. The firmware build tool downloads its own ARM
toolchain and submodules. Nothing below flashes a device.

```sh
git clone https://github.com/fderuiter/flipper_fredjet.git
cd flipper_fredjet
python3 scripts/check.py --sanitizers

git clone https://github.com/palmdalian/flipperzero-firmware.git ../flipper-bedjet-firmware
git -C ../flipper-bedjet-firmware checkout df553f8322f41efed97d6b4dbbcb7b75918600d7
python3 build.py ../flipper-bedjet-firmware
```

The app is written to `dist/bedjet_remote.fap`. To also build the matching Full
BLE firmware update package, use:

```sh
python3 build.py ../flipper-bedjet-firmware --firmware-package
```

Read [installation and controls](docs/setup.md) for the firmware requirements,
pairing, copying the FAP to the SD card, and recovery references.

A small [initial compiled artifact](artifacts/prototype/README.md) is retained
for bring-up. It includes hashes and provenance, is tied to the exact source and
firmware pin, and has not been run on hardware. New build output is ignored by
Git; subsequent published binaries should use GitHub Releases after validation.

## Development

```sh
python3 scripts/check.py --static
python3 scripts/check.py --sanitizers
python3 scripts/check.py --sanitizers --short-enums
python3 scripts/check.py --format --clang-format clang-format-18
python3 scripts/verify_artifact.py
```

`--static` also runs the setup and acceptance wizard tests (`tests/test_wizards.py`),
which need no network or device.

The [CI workflow](.github/workflows/ci.yml) runs these host checks with GCC and
Clang on every push and fork pull request, then compiles and import-checks the
FAP against the pinned firmware and uploads it with `provenance.json`. CI never
flashes hardware or publishes releases; its FAP is an unvalidated build output.
Local checks work without GitHub Actions.

- [Getting started](docs/getting-started.md), with the setup and acceptance wizards
- [Architecture and protocol boundaries](docs/architecture.md)
- [Lifecycle harness and coverage](docs/testing.md)
- [Back/Off policy and native wait budgets](docs/cancellation.md)
- [Command intent, confirmation and RF-remote conflicts](docs/commands.md)
- [Proposed upstream BLE central accessors (draft, not submitted)](docs/upstream-central-accessors.md)
- [App catalog readiness and blockers](docs/app-catalog.md)
- [Prioritized roadmap](docs/roadmap.md) and [open issues](https://github.com/fderuiter/flipper_fredjet/issues)
- [Contributing](CONTRIBUTING.md), [agent guidance](AGENTS.md), [security reporting](SECURITY.md)
- [Changelog](CHANGELOG.md), [third-party references](THIRD_PARTY_NOTICES.md), [license](LICENSE)

This is an independent project, not affiliated with BedJet LLC or Flipper Devices.
Source is licensed GPL-3.0-or-later.
