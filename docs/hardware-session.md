# Hardware session runner

`scripts/hardware_session.py` runs a test night from the computer, over the
Flipper's USB cable. It replaces the manual parts of the
[bring-up runbook](bringup.md): copying files with qFlipper, recording the CLI
log with `screen`, writing down times, and running the capture and report
scripts afterwards. You still press every button on the Flipper and the BedJet.

What it does:

- Talks to the Flipper's serial CLI (the port qFlipper uses). Standard library
  Python only; nothing to install.
- Copies `fredjet.fap` or a firmware update package to the SD card, checks each
  file by MD5 on the Flipper, and only then moves it into place.
- Compares `info device` with `firmware.lock.json` (firmware commit, API,
  radio stack version and type).
- Records `log info` with a computer timestamp on every line, session start and
  end markers, and your timestamped notes. Each line keeps the Flipper's own
  millisecond tick too.
- Walks you through sessions A-G from the runbook's table, shows status and
  rejected frames as they arrive, and offers a redo when a session logged no
  frames.
- Splits the log into one capture candidate per session, replays each through
  the host parser, and writes a sanitized summary.

What it never does: send a BedJet command, or install firmware without
`push-update --flash` and a typed `yes`.

## Before you start

1. Close qFlipper and lab.flipper.net. Only one program can hold the port.
2. Plug the Flipper in over USB-C and unlock it. Leave it on the desktop, not
   inside an app.
3. Close the BedJet phone app. Have the RF remote at hand.

```sh
python3 scripts/hardware_session.py ports    # should print /dev/cu.usbmodemflip_<name>
python3 scripts/hardware_session.py doctor
```

`doctor` prints one line per check. `fail` on Firmware means the Flipper is not
on the pinned Full BLE firmware yet; `fail` on App on SD means `fredjet.fap` is
missing or differs from your local build.

## Install the firmware package (once)

Download the CI artifacts as the runbook's section 1 describes, unzip them, then:

```sh
python3 scripts/hardware_session.py push-update ~/Downloads/f7-update-full-ble-<commit>
```

It finds `update.fuf`, refuses the package unless its manifest installs
`radio.bin` at the pinned address and every file matches `UPDATE_SHA256SUMS`,
then copies the folder to `/ext/update/`. Nothing is installed. Start the update
on the Flipper (Archive > Browser > update > f7-update-local > update.fuf > Run
in App), or from the computer:

```sh
python3 scripts/hardware_session.py push-update ~/Downloads/f7-update-full-ble-<commit> --flash
```

`--flash` asks you to type `yes` and then runs the CLI's `update install`. Make
the qFlipper backup first (runbook section 0). The Flipper restarts several
times and the port disappears; run `doctor` again once it is back.

## Install the app

```sh
python3 scripts/hardware_session.py install-app ~/Downloads/fredjet-fap-<commit>/fredjet.fap
```

With no path it uses `dist/fredjet.fap`. When `SHA256SUMS` sits next to the
file, the app must match it. An identical app on the SD card is left alone; a
different one is replaced only after you agree.

## Run the sessions

```sh
python3 scripts/hardware_session.py run --fap ~/Downloads/fredjet-fap-<commit>/fredjet.fap
python3 scripts/hardware_session.py run --sessions A,C   # only some sessions
```

1. The runner checks the Flipper, offers to install the app if it is missing,
   records free heap, and starts `log info`.
2. Open FredJet on the Flipper, scan, pick your BedJet and wait for
   `Live device status`. Press Enter.
3. For each session it shows what to do and what it settles. Press Enter to
   start, or `s` to skip. While recording, type a note whenever you press
   something or see something (`pressed fan up on Flipper`, `unit shows 30 C`);
   each note is timestamped, which is how session C's write-to-status time is
   measured. Type `end` to finish the session.
4. It prints the frame counts, then asks for a result (`p`, `f`, `s`, `n`) and
   what the unit, the RF remote and the Flipper showed.

Ctrl+C at any point stops the log cleanly and still writes the summary. The
unit must never heat unattended; turn it off from the RF remote when you stop.

At the end it offers the issue #2 acceptance checklist, prefilled with the
Flipper firmware, radio stack and logged frames.

## What you get

Each run is a new folder under `dist/bench/<date-time>/` (ignored by Git):

| File | Share it? | Contents |
| --- | --- | --- |
| `summary.md` | yes | Versions, a row per session with result, frame counts and notes, capture replay verdicts |
| `captures/session-X.cap` | yes, after review | That session's frame lines in the `tests/captures` format |
| `log.sanitized.txt` | yes, after a read | Markers, notes and FredJet lines; full addresses and key material removed |
| `session.json` | yes | The same data as the summary, for tools |
| `raw.log` | **no** | Everything the Flipper printed; can contain unit names and addresses |
| `acceptance/` | yes | The acceptance wizard's report, when you ran it |

`python3 scripts/hardware_session.py report dist/bench/<run>` rebuilds the
summary, captures and replay verdicts from `raw.log`, for example after a
parser change.

A host replay `MISMATCH` means a frame the Flipper accepted (or rejected)
classifies differently on this computer. It is a finding: keep the run and share
the summary. Before committing a capture under `tests/captures/`, fill in its
header as runbook section 5 describes; never edit frame lines.

## Evidence

The summary separates the three kinds of result this project keeps apart:
results and notes are what you typed; frame counts come from the device log;
host replay is a parser check on the computer. The runner itself verifies only
file transfers (MD5 on the Flipper) and what `info device` reports.

The serial commands (`storage write_chunk`, `storage md5`, `log`, `update
install`, `info device`) were read from the pinned firmware's CLI sources and
are tested against a scripted CLI on a pseudo-terminal
(`tests/test_hardware_session.py`). The runner has not yet been run against a
real Flipper.
