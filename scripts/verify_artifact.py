#!/usr/bin/env python3
"""Check the retained prototype binary and its source hashes; do not execute it."""

import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    manifest = json.loads((ROOT / "artifacts/prototype/provenance.json").read_text())
    entries = {manifest["binary"]["path"]: manifest["binary"]["sha256"], **manifest["source_sha256"]}
    for name, expected in entries.items():
        path = (ROOT / name).resolve()
        if not path.is_relative_to(ROOT):
            raise SystemExit(f"Artifact path leaves the repository: {name}")
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected:
            raise SystemExit(f"Hash mismatch: {name}. Rebuild and review provenance before publishing.")
    print(f"PASS: prototype binary and {len(entries) - 1} source hashes")


if __name__ == "__main__":
    main()
