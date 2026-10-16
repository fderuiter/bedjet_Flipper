# Getting started

This guide takes you from nothing installed to a first connection between a
Flipper Zero and your BedJet 3. No firmware development experience is needed.

**Read this first.** FredJet is an independent, unofficial BedJet 3 remote. It is an experimental, compiled proof of concept.
It has not yet been tested on a real BedJet. It needs experimental Flipper
firmware and a different Bluetooth radio stack, so setting it up replaces your
Flipper's firmware. You can return to official firmware afterwards (step 7).
Nothing in this repository flashes or installs anything for you: every change
to the Flipper is a step you take yourself.

## What you need

- A Flipper Zero with an SD card, and a USB cable.
- [qFlipper](https://flipperzero.one/update) on your computer.
- A BedJet 3 in Bluetooth range, and its physical RF remote for comparison.
- To build it yourself: macOS or Linux (or WSL on Windows), Git, Python 3.10 or
  newer and about 5 GB of free disk space. The first build downloads its own
  ARM compiler.

## 1. Check your computer

```sh
git clone https://github.com/fderuiter/flipper_fredjet.git
cd flipper_fredjet
python3 scripts/setup_wizard.py doctor
```

`doctor` changes nothing. Each line is `OK`, `WARN` (worth knowing) or `FAIL`
(must fix first), with an arrow pointing to the fix.

## 2. Build the app and the firmware package

```sh
python3 scripts/setup_wizard.py
```

The wizard asks before each step. It downloads the exact firmware source pinned
in [firmware.lock.json](../firmware.lock.json) next to this folder (in
`../flipper-bedjet-firmware`), checks it, and builds:

- `dist/fredjet.fap`, the app, with `SHA256SUMS` and `provenance.json`.
- If you answer yes to the firmware package question: the matching firmware
  update with the Full BLE radio stack, inside the firmware folder under
  `dist/f7-C/f7-update-local/`. You need this once per Flipper.

For repeat runs without questions: `python3 scripts/setup_wizard.py --yes`
(add `--firmware-package` to also rebuild the firmware update). Use
`--firmware-dir PATH` to put the firmware somewhere else. The wizard never
changes an existing folder; if one is in the way it tells you what to do.

**No build tools?** Every push builds the app on GitHub. Open the
[Actions tab](https://github.com/fderuiter/flipper_fredjet/actions/workflows/ci.yml),
pick a green run, download the `fredjet-fap-...` artifact, unzip it and run
`python3 scripts/setup_wizard.py verify-download PATH/TO/UNZIPPED/FOLDER`. That
download contains only the app. You still need the firmware package from
step 2 (or from someone who built it from the same pin) for step 3.

## 3. Install the experimental firmware with the Full BLE stack

The app uses Bluetooth "central" functions that are only in the
[firmware proposal](https://github.com/flipperdevices/flipperzero-firmware/pull/4436)
pinned by this project, and only work with the Full BLE radio stack. Copying the
app alone is not enough.

1. In qFlipper, make a backup of the Flipper (it keeps your settings and keys).
   Note the firmware version you are on now.
2. Open qFlipper's File manager. Copy the whole `f7-update-local` folder from
   step 2 into `SD Card/update/`.
3. Disconnect qFlipper (close the app). On the Flipper, open Archive, go to the
   Browser tab, then `update` > `f7-update-local` > `update.fuf`, and choose
   **Run in App**.
4. The Flipper restarts several times while it installs the radio stack and
   firmware. Do not unplug it or remove the SD card. If it shows an error code,
   look it up in the pinned firmware's `documentation/OTA.md`.

The project has built this package but not yet installed it on a Flipper, so
please report how it went (step 6).

## 4. Copy the app

1. Connect the Flipper and open qFlipper's File manager.
2. Go to `SD Card/apps/Bluetooth/` (create `Bluetooth` if missing).
3. Drag `fredjet.fap` into it.
4. On the Flipper: Apps > Bluetooth > **FredJet (experimental)**.

Builds before the rename were called `bedjet_remote.fap`. Delete that file if
it is still on the SD card, or the menu shows two copies of the app.

If the app says "Requires Full BLE radio stack", step 3 did not install the Full
stack. If the Flipper refuses to start the app at all, it is probably still on
official firmware.

## 5. First scan and connection

1. **Close the BedJet phone app** (fully, not just in the background). The BedJet
   accepts one Bluetooth connection at a time.
2. Disconnect the Flipper from its phone companion app for now.
3. Press OK to scan. Scanning takes 8 seconds.
4. Pick your unit with Up/Down. Each row shows its name, the last four
   characters of its Bluetooth address and signal strength. Press OK to connect.
5. Connecting alone must never start the BedJet. Compare the mode, temperature
   and fan shown on the Flipper with the RF remote before changing anything.

Controls are in [installation and controls](setup.md#controls). The most
important ones: **hold OK for one second** on any row turns the BedJet off;
**Back** disconnects.

### What the title badge means

| Badge | Meaning | What to do |
| --- | --- | --- |
| LIVE | A valid status from the BedJet arrived in the last 15 seconds. | Trust the values shown. |
| STALE | Connected, but no valid status for 15 seconds or more. | Do not rely on the values. Temperature, fan and timer changes are blocked. Use the Info row's OK to refresh, or press Back and reconnect. |
| NO DATA | Connected, but no valid status yet. A unit that is off may stay quiet. | Select Cool and press OK if you want it running; otherwise wait. |
| BAD DATA | Three or more messages in a row could not be understood. | Open the Info row and save the frames (step 6). Please report it. |

## 6. Test and report results

The acceptance wizard walks you through the
[device checklist](validation.md#device-acceptance-checklist) one step at a time
and writes a report you can paste into
[issue #2](https://github.com/fderuiter/flipper_fredjet/issues/2):

```sh
python3 scripts/acceptance_wizard.py           # start
python3 scripts/acceptance_wizard.py --resume  # continue later
```

For each step type `p` (pass), `f` (fail), `s` (skip) or `n` (not run), then
notes. The report goes to `dist/acceptance/report.md`. Full Bluetooth addresses
(only the last two bytes are kept), pairing keys and Wi-Fi details are removed,
but read the report before posting it anyway.

To turn raw frames into material for the developers, save Flipper CLI `log`
output (or type the hex lines from the app's Info pages into a text file, with a
blank line between frames) and run:

```sh
python3 scripts/acceptance_wizard.py frames capture.txt
```

It prints a capture in the format `scripts/captures.py` writes and the test
suite replays; only the app's frame lines are kept. Prefer `log` output: Info
page hex has no verdict, so it is kept only as comments, and the Info page
shows just 30 bytes of a frame that can be 32 bytes long. The
[bring-up runbook](bringup.md) has the full owner procedure: which captures to
take, what each one decides, and how to commit them under `tests/captures/`.

## 7. Return to official firmware

Open qFlipper and install the latest **Release** firmware from its update
button. Official update packages carry their own radio stack; the updater sees
that it differs from the installed Full BLE stack and replaces it (see the
pinned `documentation/OTA.md`). Your `/int` storage is backed up and restored
during the update. Then delete `SD Card/apps/Bluetooth/fredjet.fap` (and `bedjet_remote.fap`
from older builds). If
the Flipper does not start, follow Flipper's
[firmware update and recovery instructions](https://docs.flipper.net/zero/basics/firmware-update).

## Troubleshooting

| You see | Likely cause and fix |
| --- | --- |
| `None found: phone app open?` | The BedJet phone app (or another device) is connected. Close it, wait a few seconds and scan again. Also check that the BedJet has power and is within a few metres. |
| No units, phone app closed | A renamed BedJet that does not advertise its service is not listed. Power-cycle the BedJet and scan again. |
| `Connect failed: app open?` | Same as above: something else holds the one connection. |
| `Discovery failed: ...` or `Setup failed: no notify` | The unit did not offer the expected Bluetooth services. Press Back, power-cycle the BedJet, try again, then report it with the Info page. |
| `Subscribe failed (pair?)` | The BedJet may want to pair. This firmware can only pair without a code. Report what the BedJet shows in [issue #6](https://github.com/fderuiter/flipper_fredjet/issues/6). |
| `Requires Full BLE radio stack` | Repeat step 3; the Light stack is still installed. |
| `BT busy: reboot Flipper` | A previous session did not close. Restart the Flipper. |
| `Scan failed; OK to retry` or `Advertising did not stop` | Disconnect the phone companion app, then press OK again. |
| Wizard: `is at ..., but this app is pinned to ...` | Your firmware folder is on another version. Run the command it prints, or use a new `--firmware-dir`. |
| Wizard: `has local changes to tracked files` | The build needs an unmodified firmware folder. Use a fresh folder. |
| Wizard: `The build failed` after `Failed to download ...toolchain...` | The computer cannot reach update.flipperzero.one (network, proxy or firewall). Fix access and run the wizard again; the downloaded firmware is reused. |
| Wizard: Windows `FAIL` | Use WSL, or the "No build tools?" path in step 2. |

Run `python3 scripts/setup_wizard.py doctor` any time for a fresh diagnosis.
More detail: [installation and controls](setup.md),
[validation record](validation.md), [command policy](commands.md).
