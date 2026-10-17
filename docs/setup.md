# Installation and controls

First time? [Getting started](getting-started.md) walks through these steps in
plain language, with `scripts/setup_wizard.py` to build and
`scripts/acceptance_wizard.py` to record device results.

## Firmware comes first

The app targets the experimental central-role firmware revision in
[`firmware.lock.json`](../firmware.lock.json). Build against that exact revision.
Stock firmware lacks the new imported functions, and the ordinary Light BLE
radio stack cannot initiate the connection needed by BedJet.

The pin is two patches on top of the upstream central-role branch
(`palmdalian/flipperzero-firmware`, `ble-central-role` at `df553f8`, the code
behind [flipperdevices/flipperzero-firmware#4436](https://github.com/flipperdevices/flipperzero-firmware/pull/4436)).
The patches, kept in [`firmware/patches/`](../firmware/patches/) and published as
branch `fredjet-observation-scan` of `fderuiter/flipperzero-firmware`, makes
`ble_central_scan_start` use the GAP observation procedure instead of general
discovery. General discovery reports only advertisers that set the
discoverable flag, and the first hardware session showed the Flipper never
reporting a BedJet 3 that phones and Macs see (#31). The second patch passes
extended advertising reports (HCI subevent 0x0D) to scan callbacks; the pinned
central handled only legacy reports. Each scan logs how many of each arrived.

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

Once the matching firmware is installed, copy `dist/fredjet.fap` to
`/ext/apps/Bluetooth/` using qFlipper's file manager. Open Apps > Bluetooth >
FredJet (experimental).

## Pairing

1. Close the official BedJet phone app; the BedJet permits one BLE client at a time.
2. Disconnect the Flipper phone companion during initial testing. The app pauses
   Flipper advertising while scanning and restores it after confirmed cleanup.
   Deferred radio teardown is logged and does not claim advertising restoration.
3. Hold the Flipper near the BedJet's main unit and press OK to scan for 15
   seconds. Up/Down selects a matching unit; OK connects (wait up to 20 s).
   Down instead of OK starts the advanced scan (below).
4. Verify displayed values against the physical RF remote. Connecting alone must
   not start the unit. If there is no status while off, select Cool and press OK.

Pairing is unverified. The ESPHome and Home Assistant implementations connect
without pairing or bonding. If the BedJet does ask for pairing, the pinned
firmware's central role supports Just Works only: it rejects passkey entry and
numeric comparison, so a unit that requires either cannot pair with this build.
Record what happens in issue #6 rather than retrying blindly.

The advanced scan (Down on the start screen) lists every device heard, not only
BedJets: a safety net in case the filter ever misses a unit. Unidentified
devices may use only half of the event inbox, and a full list keeps every
BedJet and otherwise the strongest devices. Rows without a name read
`unknown`. Connecting to a device that is not a BedJet fails at the
characteristic lookup, before anything is written.

Discovery matches the BedJet V3 service UUID or a local name starting with
`BEDJET` (case insensitive). Other devices are filtered before they reach the
app's event queue, so a busy radio environment cannot crowd a BedJet out. A scan
response's name is merged into its unit, so a renamed unit that still
advertises the service UUID shows its custom name. Each row shows the name (or
"BedJet"), the last two address bytes and signal strength; up to six units are
listed, with a note when more were seen. A renamed unit that advertises neither
the UUID nor a `BEDJET` name is still not found: the app does not fall back to
connecting to arbitrary devices.

Setup failures name the failing stage and, where the app can tell, the cause:
`<stage>: no reply` when the call used its whole wait, `<stage>: link lost`
when the link dropped, and `<stage>: BLE 0xNN` when the controller refused
it. Otherwise the stage's own advice is shown, for example
`Connect failed: app open?` (the BedJet accepts one client, so an open phone
app is the usual cause) or `Subscribe failed (pair?)`. The firmware does not
report these causes; the app infers them from timing, link state and the raw
status, so treat them as hints. The raw status is also on the Info page.

If a status read or refresh gets no reply while connected, the app
disconnects and shows `No reply: reconnect`. The firmware stops accepting
requests on such a link, so every control would otherwise fail. Reconnect
manually; nothing is resent.

## Controls

| Input | Behavior |
| --- | --- |
| Up / Down | Select a control row or a discovered device; hold to keep moving |
| Left / Right on Mode | Choose a mode draft; OK sends it. The draft follows the unit's actual mode, shown as "(now)"; OK on the current mode sends nothing |
| Left / Right on Temperature | Add half a degree Celsius to the desired target |
| Left / Right on Fan | Add one 5% airflow step to the desired target |
| Left / Right on Timer | Edit the draft timer in five-minute steps; OK sends it |
| OK on M1, M2, M3 | Start that existing memory preset |
| OK on Units | Switch the display between Fahrenheit and Celsius (nothing is sent) |
| Info row | Left / Right pages through frame counters, the last rejected and accepted raw frames and the last status read; OK refreshes (resubscribe and one read, never a command) |
| Hold OK for 1 second on any control row | Priority Off; discard older queued adjustments. Releasing between 0.3 and 1 s does nothing and shows a hint |
| Press Back (including a hold) | Disconnect and return home; Back again exits |

A short press is under 0.3 s. Holding an arrow counts as one step at 0.3 s and
repeats every 0.15 s from 0.45 s. On Home and the unit list, a slightly long OK
still counts as OK. The title shows LIVE, STALE, NO DATA (connected, no valid
status yet; a unit in standby may not notify) or BAD DATA (frames arrive but
none validate; open the Info row). The status line shows mode, target, fan and
the remaining runtime. Cool is ambient-air circulation, not refrigerated air.
The app never overrides BedJet's own operating limits.

For bring-up captures, the app also logs messages, status reads, the first and
mode-changing valid frames and up to 32 rejected frames per connection as hex
at the Info log level (Flipper CLI `log`). Sanitize before sharing.

## Known prototype behavior

- Back and hold-OK Off bypass the ordinary inbox. Shutdown/Back take precedence
  over Off; they disconnect without sending a BedJet command. Off drops older
  queued work and is never retried automatically. Off needs a 1 s hold of OK on
  any control row; short OK on the Off row only shows that hint.
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
- The timer row shows its draft; the status line shows the unit's remaining
  runtime. Text is clipped to the 128 px width with the firmware's font metrics.
- A stale connection does not trigger automatic resync or reconnect; use the
  Info row's OK for one manual refresh. Trust the LIVE badge, not the presence
  of a connection, as evidence of current device state.

For the step-by-step owner procedure, captures and what they decide, follow
the [bring-up runbook](bringup.md). See the [roadmap](roadmap.md). Start with the
[device acceptance checklist](validation.md#device-acceptance-checklist).
