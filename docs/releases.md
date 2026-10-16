# Prereleases

FredJet binaries are published only as GitHub prereleases, and only after a
hardware run has passed. Tracking issue:
[#9](https://github.com/fderuiter/flipper_fredjet/issues/9).

**Do not run the prerelease workflow yet.** It stays unused until the device
acceptance checklist in [validation](validation.md#device-acceptance-checklist)
passes on real hardware (#1, #2 and the physical gate of #3). Nothing has been
released, and no tag exists.

## What the workflow does and does not do

[`prerelease.yml`](../.github/workflows/prerelease.yml) runs only when someone
starts it by hand from the Actions tab, and only from `main`. It:

1. refuses unless `releases/<version>/acceptance.json` is committed on `main`
   and passes [`scripts/release_gate.py`](../scripts/release_gate.py);
2. runs the host checks and `verify_artifact.py`;
3. builds the FAP against the pinned firmware with a fresh toolchain download
   (no cache);
4. refuses unless the built FAP has exactly the SHA-256 that was
   hardware-tested;
5. creates a **draft** GitHub prerelease named `v<version>` with
   `fredjet.fap`, `SHA256SUMS`, `provenance.json`, `acceptance.json`,
   `firmware.lock.json` and generated release notes.

It never publishes the draft, pushes a tag, flashes a device or attaches the
firmware update package. GitHub creates the `v<version>` tag only when the
owner presses **Publish release** on the draft. Only the final job has
`contents: write`; everything else is read-only.

## The hardware record

The gate reads the sanitized report written by
[`scripts/acceptance_wizard.py`](../scripts/acceptance_wizard.py). It refuses
the record when:

- the version is not `MAJOR.MINOR.PATCH-(alpha|beta|rc).N`, or its
  `MAJOR.MINOR` differs from `fap_version` in `application.fam` (now `0.1`);
- any step of the current checklist is anything but `pass`, or the record's
  steps differ from the checklist in `docs/validation.md`;
- the Flipper firmware, radio stack or BedJet model is not recorded;
- the tested source commit is missing, was dirty, or is not an ancestor of the
  release commit;
- the tested FAP's SHA-256 is missing or differs from the FAP the workflow built;
- the record still contains something the wizard's sanitizer would remove
  (a full Bluetooth address, key material or Wi-Fi detail).

The results remain tester-reported. The gate checks that the record is complete
and matches the binary; it cannot confirm what happened on the device.

## Steps for the owner, after hardware validation passes

1. Put the exact FAP you tested in `dist/`: for a CI build, unzip the
   `fredjet-fap-<commit>-…` artifact there and check it with
   `python3 scripts/setup_wizard.py verify-download dist`. The wizard records
   the hash of `dist/fredjet.fap`, so without it the record names no binary.
2. In a clean checkout of the tested commit, run
   `python3 scripts/acceptance_wizard.py` (or regenerate an earlier run with
   `--answers dist/acceptance/answers.json`). Review `dist/acceptance/report.md`.
3. Copy `dist/acceptance/report.json` to `releases/<version>/acceptance.json`
   (for example `releases/0.1.0-rc.1/acceptance.json`). Check it locally:

   ```sh
   python3 scripts/release_gate.py check 0.1.0-rc.1 --fap dist/fredjet.fap
   ```

4. Open a PR with the record, the issue updates and the `CHANGELOG.md` entry,
   and merge it. Changing any app source after the test changes the FAP bytes,
   so the gate would refuse; test again instead.
5. In Actions, choose **Prerelease (manual, draft only)**, **Run workflow** on
   `main`, and enter the version.
6. Review the draft release: notes, attached hashes against your record, and
   provenance. Publish it yourself, or delete the draft.

## Reproducibility

The gate depends on CI rebuilding the tested bytes, so it relies on the build
being reproducible at one commit. Evidence so far is in
[validation](validation.md#issue-9--reproducibility-and-prerelease-workflow-2026-10-16):
four GitHub Actions builds of `main` at `38930aa` produced identical bytes.
Builds from different absolute paths or machines have not been compared. If a
rebuild ever differs, the workflow stops before creating anything.
