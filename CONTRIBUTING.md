# Contributing

Start with the [roadmap](docs/roadmap.md) and link an existing issue, or describe
the concrete problem before proposing a new feature. The first release goal is
reliable basic control on a real BedJet 3.

## Checks

```sh
python3 scripts/check.py --sanitizers
python3 scripts/check.py --format --clang-format clang-format-18
python3 scripts/verify_artifact.py
python3 build.py /path/to/clean/pinned/flipper-firmware
```

Use the repository's `.clang-format` and Python 3.10+. The ARM toolchain is
managed by the pinned firmware's `fbt`. Do not change dependency pins just to
make a local build pass. Explain pin updates and revalidate the app imports,
Full BLE package, memory layout and hardware behavior.

## Pull requests

- Explain the user-visible behavior and link the issue. Keep changes focused.
- Include the exact checks run and distinguish host tests, FAP compilation and
  device tests. Mark checks unavailable if no hardware is attached.
- Add behavioral tests for protocol, connection, queue or timing changes.
- Record firmware revision, radio stack and BedJet model/firmware for hardware
  results. Sanitize logs and fixtures before committing.
- Update documentation when setup, controls or supported hardware changes.

There is no automatic CI gate or branch protection claimed by this repository.
The checked-in workflow is manual until maintainers choose to enable automatic
checks. Hardware acceptance remains separate from CI.

The source is GPL-3.0-or-later. Contributions must be compatible with that license.
Disclose AI assistance and the verification actually performed; generated code
requires the same review and testing as other code.
