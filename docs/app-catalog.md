# Flipper app catalog readiness

Tracker: [#17](https://github.com/fderuiter/flipper_fredjet/issues/17).
Recorded 2026-10-16. Publishing in the official Flipper app catalog is a goal,
but submission is **blocked**. Until it is unblocked, validated prereleases go
out through GitHub Releases ([#9](https://github.com/fderuiter/flipper_fredjet/issues/9)).

## Blockers

1. **No official SDK has BLE central.** The catalog builds apps with uFBT
   against the latest official release or release-candidate SDK (1.5.1 and
   1.5.1-rc, both API 88.2). Neither contains `furi_ble/central.h` or the
   `ble_central_*` exports this app imports, so the catalog build fails and
   the device loader would refuse the FAP with missing imports.
2. **The upstream proposal is stalled.** The central API comes from
   [flipperzero-firmware PR #4436](https://github.com/flipperdevices/flipperzero-firmware/pull/4436),
   open with no maintainer review since 2026-08-09. It now conflicts with
   `dev`, which reused API 88.3 and has moved to 88.4.
3. **The Full BLE radio stack is required.** Official update packages ship the
   Light stack, which cannot act as a central, and the catalog has no way to
   install a different radio stack.
4. **No hardware evidence yet.** Hardware validation
   ([#1](https://github.com/fderuiter/flipper_fredjet/issues/1),
   [#2](https://github.com/fderuiter/flipper_fredjet/issues/2)) and real,
   unedited 512x256 qFlipper screenshots are still needed.

## Naming

Catalog rules restrict third-party trademarks in app names. The owner chose a
homage name on 2026-10-16: the app is **FredJet**, matching the
repository's rename to `flipper_fredjet`. "BedJet" now appears
only where it names the compatible device ("compatible with BedJet 3"), and
the catalog README carries a non-affiliation note.
Because the name echoes BedJet, catalog reviewers may still question it;
raise it with them up front in the submission.

The app id was renamed from `bedjet_remote` to `fredjet` on 2026-10-16 (owner
decision), so the FAP is `fredjet.fap` and the catalog id no longer carries the
brand. The retained prototype keeps its original `bedjet_remote.fap` name as
immutable evidence; C source file names are unchanged. A prior-art search (October 2026) found no other
Flipper BedJet app.

## Prepared now

- `application.fam` declares `fap_weburl`, a 10x10 1-bit `fap_icon`
  (`fredjet_10px.png`, also copied by `build.py`) and `targets=["f7"]`.
  The pinned firmware's f18 SDK inherits the `ble_central_*` exports from f7,
  but the radio stack, option bytes and all build evidence are f7-only, so the
  app is restricted to f7 until an f18 is actually tested.
- [`.catalog/README.md`](../.catalog/README.md) and
  [`.catalog/changelog.md`](../.catalog/changelog.md) use only the catalog's
  Markdown subset: H1/H2 headings, lists and links; no inline code, code blocks
  or tables. They state the firmware and radio requirements and the
  unvalidated status. Keep them in that subset when editing.

## Submission checklist (when unblocked)

- [ ] BLE central is in an official release or RC SDK, and this app builds with
      uFBT against it unchanged, or against a documented replacement API.
- [ ] A supported Full-stack install path exists, or the app no longer needs it.
- [ ] Hardware acceptance (#1, #2) recorded in [validation.md](validation.md).
- [x] App display name changed to FredJet (owner decision, 2026-10-16).
- [x] App id changed to `fredjet` (owner decision, 2026-10-16).
- [ ] Screenshots: unedited 512x256 qFlipper captures saved under `screenshots/`.
- [ ] `.catalog/README.md` status and requirements rewritten for the released
      firmware; `fap_version` bumped and `.catalog/changelog.md` updated.
- [ ] Manifest pull request to `flipperdevices/flipper-application-catalog`,
      pinned to a release commit. Draft, to be checked against the catalog's
      current contribution guide before use:

      ```yaml
      sourcecode:
        type: git
        location:
          origin: https://github.com/fderuiter/flipper_fredjet.git
          commit_sha: <release commit>
      short_description: Direct BLE remote for BedJet 3
      description: "@.catalog/README.md"
      changelog: "@.catalog/changelog.md"
      screenshots:
        - screenshots/control.png
      ```

- [ ] Extra requirements stated in the submission: experimental central
      firmware or its released successor, Full BLE radio stack, an owned
      BedJet 3.
- [ ] AI-use disclosure in the submission: much of this app's code and
      documentation was written with an AI coding assistant and reviewed by
      the owner.
- [ ] Optional: ask catalog maintainers whether a submission depending on an
      experimental SDK could ever be accepted.

## Reassess when

BLE central ships in an official RC or release SDK and a supported Full-stack
install path exists.
