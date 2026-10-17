# Hardware bring-up runbook

This is the owner-run procedure for issues
[#1](https://github.com/fderuiter/bedjet_Flipper/issues/1) (firmware baseline) and
[#2](https://github.com/fderuiter/bedjet_Flipper/issues/2) (BedJet acceptance).
Every step that touches a device is done by the owner. Nothing in this
repository flashes a device. Record what you actually saw, including failures;
an empty or failed step is evidence too.

## 0. Before touching the Flipper

1. In qFlipper, note the current firmware version (shown on its main screen) and
   make a backup (qFlipper > Backup). Keep the SD card's contents as well.
2. Close the official BedJet phone app and disconnect the Flipper phone
   companion. The BedJet accepts one BLE client.
3. Have the BedJet RF remote at hand. It is the reference for every reading.

## 1. Get the firmware package and app (issue #1)

**From CI (recommended).** On GitHub, Actions > CI > Run workflow on `main`, tick
"Also build the matching Full BLE update package". When the run is green,
download two artifacts from its summary page:

- `fredjet-fap-<commit>-…`: `fredjet.fap`, `SHA256SUMS`, `provenance.json`
- `f7-update-full-ble-<commit>-…`: `update/f7-update-local/` (firmware + Full
  BLE 1.20.0 radio stack + option bytes; `update.fuf` is its manifest) and
  `UPDATE_SHA256SUMS`

CI refuses to upload the package unless its `radio.bin` is byte-identical to
the pinned Full BLE binary and its manifest names the pinned address. Record
the run URL and commit as provenance. Artifacts expire after 14 days.

**Or build locally** on a Linux or macOS machine with git and Python 3:

```sh
git clone --branch fredjet-observation-scan https://github.com/fderuiter/flipperzero-firmware.git fw
cd fw && git checkout 38a9ca993c1896edbe20234e35bf043b142d055a
git submodule update --init --recursive
cd ../bedjet_Flipper
python3 build.py ../fw --firmware-package
```

The first run downloads the Flipper ARM toolchain. Outputs:

- `dist/fredjet.fap`, `dist/SHA256SUMS`, `dist/provenance.json` (app)
- `../fw/dist/f7-C/f7-update-local/` (firmware + Full BLE 1.20.0 radio stack +
  option bytes; `update.fuf` is its manifest)

Either way, check `update.fuf` lists `Radio: radio.bin` (the updater's name for
`stm32wb5x_BLE_Stack_full_fw.bin`) and `Radio address: 00 E0 0C 08`
(0x080CE000, written little-endian) before going further.

## 2. Install (owner, issue #1)

1. Close qFlipper. Copy the whole `f7-update-local` folder to the SD card at
   `/ext/update/` (qFlipper file manager or a card reader).
2. On the Flipper: Archive > Browser > `update` > `f7-update-local` >
   `update.fuf` > Run in App. The updater backs up `/int`, replaces the radio
   stack (several restarts), checks option bytes, then writes the firmware.
3. If it stops with an `[XX-YY]` code, photograph it and stop. The code table is
   in the pinned firmware's `documentation/OTA.md`. Recovery is qFlipper's
   official firmware repair (see setup.md).
4. Copy `dist/fredjet.fap` to `/ext/apps/Bluetooth/`.

Record from the Flipper CLI (section 4 shows how to open it):

```text
info device     # firmware commit, hardware target, radio_stack_* values
free            # heap before launching the app
```

**Redact before sharing:** `info device` prints the Flipper's name, unique IDs and
BLE MAC (`hardware_name`, `hardware_uid`, `radio_ble_mac`, and similar). Keep only
`firmware_*`, `hardware_target`/`hardware_ver`, `radio_stack_*`, `radio_fus_*`
and the API version.

Issue #1 also asks for the Light-stack behavior. Testing it means reinstalling
the Light stack, so it is optional; if skipped, record it as untested. The same
applies to returning to official firmware.

## 3. First launch (issue #1 then #2)

Open Apps > Bluetooth > BedJet Remote (experimental). Note whether it loads, or
the exact error. Then scan with OK. Note the units listed and whether a pairing
prompt appears on the Flipper or the BedJet. Pairing is unverified: the app
expects no pairing; Just Works is the only mode the pinned firmware supports.

## 4. Capturing frames

The app logs every status read, the first and each mode-changing accepted frame,
up to 32 rejected frames per connection, and its footer messages at Info level.

1. Close qFlipper (it holds the serial port). Open the CLI with
   `screen -L -Logfile bringup.log /dev/ttyACM0` (macOS:
   `/dev/cu.usbmodemflip_*`), or with the CLI in lab.flipper.net and copy the
   output. Press Enter for the `>:` prompt.
2. Run `log info`. Leave it running; it stops with Ctrl+C.
3. Run the sessions below, one at a time, writing down the wall-clock time and
   what you did. Log lines start with the Flipper's millisecond tick.
4. On the app's Info row, Left/Right pages show frame counters and the last
   rejected, accepted and read frames. Photograph any page that looks wrong.

| Session | What to do | What it settles |
| --- | --- | --- |
| A. Off | Unit off (RF remote). Connect. Wait 30 s. Info OK once | Off-state reads, the 11-byte read tail, whether an idle unit notifies |
| B. Each mode | From the RF remote, start Cool, Heat, Turbo, Dry, Ext Heat in turn; 20 s each | Full/partial frames, mode bytes, per-mode limits |
| C. Steps | In Heat: one fan step, one temperature step from the Flipper; then the same from the RF remote | Command bytes, time from write to matching status (observation window) |
| D. Timer | Set a short timer from the Flipper; compare with the unit | Timer rounding and tolerance |
| E. Presets | Start an existing M1, M2, M3 (never save over them) | Preset settle time and what status shows |
| F. Off hold | Hold OK on several rows; also try 0.5 s presses | Whether the 1 s hold is comfortable and never accidental |
| G. Loss | Walk out of range, then Back; relaunch and reconnect | Stale handling, no replay after reconnect |

Never leave the unit heating unattended during these sessions.

## 5. Turning a log into fixtures

```sh
python3 scripts/captures.py bringup.log -o tests/captures/a-off.cap
```

The script keeps only the app's frame lines (kind, detail, length, bytes) and
drops everything else, including names and addresses. Fill in the header lines
(condition, BedJet model and firmware, Flipper firmware and radio, app commit)
and review the file. Then `python3 scripts/check.py --sanitizers` replays every
committed capture through the parser: a frame accepted on the device must still
parse with the same mode, and a rejected or read frame must keep its result.
Do not edit a capture to make it pass; a mismatch is a finding.

Share the raw log privately in the project thread, not in Git: it can contain
unit names, addresses and timestamps.

## 6. What the captures decide

These are host-tested policy values awaiting device evidence. None changes
without a capture or a recorded observation that justifies it.

| Value | Current | Evidence that settles it |
| --- | --- | --- |
| Off hold | 1 s, hint for 0.3-1 s | Session F: accidental triggers or awkward holds |
| Observation window | 5 s for a matching status | Session C: tick of write message to matching status line |
| Preset settle | 2 s (`BEDJET_PRESET_SETTLE_MS`), see [commands.md](commands.md) | Session E: time until status stops changing |
| Timer tolerance | 2 s below requested minus elapsed | Session D: reported versus set runtime |
| 11-byte read tail | recorded, not applied | Session A: its bytes across off/running states |
| Pairing | none expected | Section 3 |

Results go into [validation.md](validation.md) and the issue they belong to,
each with device and revision details.
