#!/usr/bin/env python3
"""Check the retained prototype binary and its source hashes; do not execute it."""

import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    manifest = json.loads((ROOT / "artifacts/prototype/provenance.json").read_text())
    source_root = Path(manifest.get("source_root", "."))
    binary = manifest["binary"]
    entries = {
        binary["path"]: binary["sha256"],
        **{str(source_root / name): digest for name, digest in manifest["source_sha256"].items()},
    }
    for name, expected in entries.items():
        path = (ROOT / name).resolve()
        if not path.is_relative_to(ROOT):
            raise SystemExit(f"Artifact path leaves the repository: {name}")
        if not path.is_file():
            raise SystemExit(f"Missing retained artifact file: {name}. Restore it or update provenance deliberately.")
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected:
            raise SystemExit(f"Hash mismatch: {name}. Rebuild and review provenance before publishing.")
    size = (ROOT / binary["path"]).stat().st_size
    if size != binary["size_bytes"]:
        raise SystemExit(f"Size mismatch: {binary['path']} is {size} bytes; provenance says {binary['size_bytes']}.")
    print(f"PASS: prototype binary ({size} bytes) and {len(entries) - 1} source hashes")


if __name__ == "__main__":
    main()
