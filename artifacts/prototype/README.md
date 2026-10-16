# Initial compiled prototype

`bedjet_remote.fap` is the initial app binary, compiled against the experimental
firmware revision in [`../../firmware.lock.json`](../../firmware.lock.json).
It is **not hardware validated** and will not run on stock firmware.

[`provenance.json`](provenance.json) records the firmware commit, API, compiler,
build command, file size, SHA256 of the FAP and hashes of all four app inputs.
From the repository root, verify them with:

```sh
python3 scripts/verify_artifact.py
```

Those hashes associate this artifact with source inputs; they are not a digital
signature, hardware test, or proof of a bit-for-bit reproducible build. The
portable protocol tests use synthetic fixtures.

This small initial artifact is retained for hardware bring-up. It is not an
official release. Do not overwrite it silently. Future validated builds should
be distributed as GitHub Release assets with corresponding source, checksums,
provenance and a compatibility record. Generated local builds belong in ignored
`dist/`. See [`../../docs/setup.md`](../../docs/setup.md) before installation.
