# Third-party references and provenance

This repository is independent of BedJet LLC and Flipper Devices.

| Reference | Use | License / status |
| --- | --- | --- |
| [roycamp/bedjet-protocol](https://github.com/roycamp/bedjet-protocol) | BLE UUIDs, command formats and status-offset reference | MIT; its V3 consolidation is explicitly untested by its author |
| [ESPHome BedJet](https://github.com/esphome/esphome/tree/dev/esphome/components/bedjet) | Cross-check of core commands, temperature/fan encoding, status layout and Write Without Response | ESPHome repository includes GPL-3.0 licensing; consulted as a protocol reference |
| [Flipper firmware](https://github.com/flipperdevices/flipperzero-firmware) | Public app APIs, headers, build toolchain and formatting conventions | GPL-3.0; fetched separately at build time |
| [BLE central proposal](https://github.com/flipperdevices/flipperzero-firmware/pull/4436) | Experimental BLE central/GATT-client API dependency | Open/unmerged when inspected; exact fork revision in `firmware.lock.json` |
| ST STM32WB radio stack | Full BLE runtime installed with the matching firmware | Vendor-supplied binary and its upstream terms; not redistributed in this app source repository |

The application and portable codec were generated with AI assistance under Fred
de Ruiter's direction. Verification so far consists of host tests, FAP compilation,
import checks and an updater-package build. No physical device test is claimed.

The initial FAP is built from this repository's app source using the pinned
firmware SDK. Its manifest and source hashes are in
`artifacts/prototype/provenance.json`. The complete Flipper firmware source and
radio update binaries are not included in that artifact directory; their exact
sources and build instructions are linked in this repository.

BedJet and Flipper names identify compatible products; no endorsement is implied.
