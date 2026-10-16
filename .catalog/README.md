# Bed Breeze (experimental)

Control a BedJet 3 directly over Bluetooth from a Flipper Zero. No phone, cloud service, ESP32 or BedJet firmware change is involved.

## Status

- Compiled proof of concept. It has not been tested with a real BedJet or on a physical Flipper.
- Settings shown on screen come from the status the BedJet reports. A command that was sent is not shown as applied until that status confirms it.

## Requirements

- Flipper Zero (F7 hardware).
- Experimental firmware with Bluetooth central support, from the open firmware proposal [flipperzero-firmware pull request 4436](https://github.com/flipperdevices/flipperzero-firmware/pull/4436). Stock and current official release firmware do not contain this support, and the app will not load on them.
- The Full BLE radio stack (STM32WB Full BLE 1.20.0). The Light stack shipped in official updates cannot act as a Bluetooth central.
- A BedJet 3 that you own.

## Features

- Scan for nearby BedJet units and pick one by name.
- Select Cool, Heat, Dry or Extended Heat, or turn the unit off with a deliberate hold.
- Adjust target temperature, fan speed and the auto-off timer within the limits the BedJet reports.
- Start the M1, M2 or M3 memory presets already stored on the BedJet.
- See the reported mode, temperature, fan speed and remaining time, with a warning when status is stale.

## Not included

- Sleep program editing, saving presets, Turbo, Wi-Fi setup and BedJet firmware updates.
- Automatic reconnect or automatic command retries.

## Trademarks

- BedJet is a trademark of its owner. Bed Breeze is an independent, unofficial app that is compatible with BedJet 3. It is not made, endorsed or supported by BedJet.

## More information

- [Source, setup guide and validation record](https://github.com/fderuiter/bedjet_Flipper)
