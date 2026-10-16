# Installation and controls

## Firmware comes first

The app targets the experimental central-role firmware revision in
[`firmware.lock.json`](../firmware.lock.json). Build against that exact revision.
Stock firmware lacks the new imported functions, and the ordinary Light BLE
radio stack cannot initiate the connection needed by BedJet.

Read the pinned firmware's `documentation/OTA.md` before installing it. A complete
Full BLE update includes the radio binary, matching stack type and option bytes;
copying the FAP alone does not add these capabilities. This repository's build
script never flashes hardware or changes firmware settings.

Build the app and complete update package:

```sh
python3 build.py /path/to/clean/pinned/firmware --firmware-package
```

The equivalent firmware command, run inside the pinned firmware checkout, is:

```sh
./fbt updater_package COMPACT=1 DEBUG=0 \
  COPRO_OB_DATA=scripts/ob_custradio.data \
  COPRO_STACK_BIN=stm32wb5x_BLE_Stack_full_fw.bin \
  COPRO_STACK_TYPE=ble_full
```

The initial build generated `dist/f7-C/f7-update-local/` inside the firmware
checkout. Review its `update.fuf` and the upstream installation instructions.
Its Full BLE 1.20.0 address `0x080CE000` was checked against the bundled ST release
notes. That build has not been installed by this project. Keep your current
Flipper firmware version and a backup of the device's data before choosing an
experimental firmware installation. Flipper's
[firmware update instructions](https://docs.flipper.net/zero/basics/firmware-update)
cover official update and recovery paths.

Once the matching firmware is installed, copy `dist/bedjet_remote.fap` to
`/ext/apps/Bluetooth/` using qFlipper's file manager. Open Apps > Bluetooth >
BedJet Remote (experimental).

## Pairing

1. Close the official BedJet phone app; the BedJet permits one BLE client at a time.
2. Disconnect the Flipper phone companion during initial testing. The app pauses
   Flipper advertising while scanning and restores it after confirmed cleanup.
   Deferred radio teardown is logged and does not claim advertising restoration.
3. Press OK to scan for eight seconds. Up/Down selects a matching unit; OK connects.
4. If the BedJet requests confirmation, press the BedJet 3 Low Power button twice.
5. Verify displayed values against the physical RF remote. Connecting alone must
   not start the unit. If there is no status while off, select Cool and press OK.

Discovery matches the BedJet service UUID or a local name starting with `BEDJET`
(case insensitive). Renamed units may be missing if they omit the UUID. Up to
six units appear, distinguished by an address suffix. Those limits are tracked
in the roadmap rather than claimed as complete discovery support.

## Controls

| Input | Behavior |
| --- | --- |
| Up / Down | Select a control row or a discovered device |
| Left / Right on Mode | Choose a mode; OK sends it |
| Left / Right on Temperature | Add half a degree Celsius to the desired target |
| Left / Right on Fan | Add one 5% airflow step to the desired target |
| Left / Right on Timer | Edit the draft timer in five-minute steps; OK sends it |
| OK on M1, M2, M3 | Start that existing memory preset |
| Hold OK on any control row | Priority Off; discard older queued adjustments |
| Press Back (including a hold) | Disconnect and return home; Back again exits |

The temperature display uses Fahrenheit. Cool is ambient-air circulation, not
refrigerated air. The app never overrides BedJet's own operating limits.

## Known prototype behavior

- Back and hold-OK Off bypass the ordinary inbox. Shutdown/Back take precedence
  over Off; they disconnect without sending a BedJet command. Off drops older
  queued work and is never retried automatically. Short OK on the TURN OFF row
  does nothing; hold OK from any control row instead.
- A synchronous firmware call can still delay cancellation or Off for seconds.
  See the [priority policy and native wait budgets](cancellation.md). This does
  not establish physical response latency.
- Ordinary input/telemetry can be dropped on queue contention or saturation. A
  sticky `!N` beside the title shows the count, capped at 999, until app exit.
- The first temperature/fan input sends immediately; repeats accumulate in a
  separate want value. One command awaits matching new telemetry; only then can
  the latest coalesced target send. Other command types processed while pending
  are rejected; they must be requested again after completion.
- The footer retains pending/applied, rejected, not-confirmed, conflict or
  unverified results. Actual values remain device-reported. Timeouts and errors
  discard deferred input without retry. M1/M2/M3 remain unverified because the
  decoded status cannot identify the memory slot.
- Temperature and timer rows lock after mode/Off/preset writes until fresh
  appropriate limits arrive. RF-remote mode changes discard older input. Read
  [the command policy](commands.md) before physical acceptance testing.
- The timer row shows its draft, not a running countdown. Some footer text may
  be clipped on the 128x64 screen. Off is available by holding OK on any
  control row.
- A stale connection does not currently trigger automatic status resync or
  reconnect. Do not interpret a connection icon as proof of live device state.

See the [roadmap](roadmap.md). Start with the
[device acceptance checklist](validation.md#device-acceptance-checklist).
