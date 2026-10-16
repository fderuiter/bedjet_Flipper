#!/usr/bin/env python3
"""Refuse a prerelease unless a committed hardware record matches the exact FAP.

Used by the manual prerelease workflow; it creates, tags and publishes nothing.

  python3 scripts/release_gate.py check 0.1.0-rc.1                     # record only
  python3 scripts/release_gate.py check 0.1.0-rc.1 --fap dist/fredjet.fap
  python3 scripts/release_gate.py notes 0.1.0-rc.1 --provenance dist/provenance.json

The record is `releases/<version>/acceptance.json`: the sanitized report from
`scripts/acceptance_wizard.py` for a hardware run, committed before the run.
Every current checklist step must be `pass`, the tested FAP's SHA-256 must
equal the FAP this build produced, and the tested source must be clean and an
ancestor of the release commit.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import acceptance_wizard as acceptance  # noqa: E402

VERSION = re.compile(r"(\d+)\.(\d+)\.(\d+)-(alpha|beta|rc)\.(\d+)")
REQUIRED_VERSIONS = ("flipper_firmware", "radio_stack", "bedjet_model")


def record_path(version, root=ROOT):
    return Path(root) / "releases" / version / "acceptance.json"


def fap_version(root=ROOT):
    match = re.search(r'fap_version="([^"]+)"', (Path(root) / "application.fam").read_text(encoding="utf-8"))
    return match.group(1) if match else None


def is_ancestor(commit, head="HEAD", root=ROOT):
    result = subprocess.run(["git", "merge-base", "--is-ancestor", commit, head], cwd=root,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return result.returncode == 0


def problems(version, record, steps, manifest_version, fap_sha256=None, ancestor=is_ancestor):
    """Every reason this record cannot back a prerelease; empty means it can."""
    found = []
    match = VERSION.fullmatch(version or "")
    if not match:
        return [f"version {version!r} is not MAJOR.MINOR.PATCH-(alpha|beta|rc).N"]
    if manifest_version != f"{match.group(1)}.{match.group(2)}":
        found.append(f"version {version} does not match fap_version {manifest_version!r} in application.fam")
    if record is None:
        return found + [f"no hardware record at releases/{version}/acceptance.json"]
    if record.get("kind") != "bedjet-acceptance-report":
        found.append("record is not an acceptance_wizard report")
    if acceptance.sanitize_tree(record) != record:
        found.append("record contains an address, key or Wi-Fi detail the wizard would remove")
    recorded = record.get("steps") or []
    if [s.get("check") for s in recorded] != list(steps):
        found.append("record steps do not match the current checklist in docs/validation.md")
    for step in recorded:
        if step.get("result") != "pass":
            found.append(f"step {step.get('number')} is {step.get('result')!r}, not 'pass'")
    versions = record.get("versions") or {}
    for key in REQUIRED_VERSIONS:
        if not str(versions.get(key) or "").strip():
            found.append(f"record does not name {key}")
    app = record.get("app") or {}
    commit = app.get("commit")
    if not commit or app.get("dirty") is not False:
        found.append("record does not name a clean tested source commit")
    elif not ancestor(commit):
        found.append(f"tested commit {commit} is not an ancestor of the release commit")
    tested = (record.get("app_binary") or {}).get("sha256")
    if not tested:
        found.append("record does not name the tested FAP's SHA-256")
    elif fap_sha256 is not None and tested != fap_sha256:
        found.append(f"built FAP {fap_sha256} is not the hardware-tested FAP {tested}")
    return found


def load_record(version, root=ROOT):
    path = record_path(version, root)
    return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else None


def render_notes(version, record, provenance, lock):
    versions = record["versions"]
    firmware = provenance["firmware"]
    summary = record["summary"]
    lines = [
        f"FredJet {version} is a **prerelease** for an owned BedJet 3 (V3 protocol) and a "
        "Flipper Zero running experimental BLE central firmware with the Full BLE radio stack.",
        "",
        f"The owner hardware-tested this exact FAP on {record['date']}: "
        f"{summary.get('pass', 0)} of {len(record['steps'])} checklist steps passed. The record is "
        f"`releases/{version}/acceptance.json`; results are tester-reported. Other units, firmware "
        "versions and stacks are untested.",
        "",
        "| | |",
        "| --- | --- |",
        f"| Source commit | `{provenance['source']['commit']}` |",
        f"| Tested source commit | `{record['app']['commit']}` |",
        f"| FAP SHA-256 | `{provenance['fap']['sha256']}` ({provenance['fap']['size_bytes']} bytes) |",
        f"| Firmware | {firmware['repository']} `{firmware['commit']}` (API {firmware['api_version']}) |",
        f"| `central.h` SHA-256 | `{firmware['central_h_sha256']}` |",
        f"| Radio stack | {lock['radio_stack']} {lock['radio_version']} at {lock['radio_address']} |",
        f"| Toolchain | fbt {provenance['toolchain']['fbt_toolchain_version']}, "
        f"{provenance['toolchain']['gcc']} |",
        f"| Tested Flipper firmware | {versions['flipper_firmware']} |",
        f"| Tested radio stack | {versions['radio_stack']} |",
        f"| Tested BedJet | {versions['bedjet_model']}, firmware {versions.get('bedjet_firmware') or 'not recorded'} |",
        "",
        "Install with qFlipper as described in `docs/getting-started.md`, after the firmware "
        "in `docs/setup.md`. Stock firmware and the Light radio stack are unsupported. "
        "Known limitations are in `docs/validation.md`.",
        "",
    ]
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("check", "notes"))
    parser.add_argument("version")
    parser.add_argument("--fap", type=Path, help="built FAP whose bytes must be the tested ones")
    parser.add_argument("--provenance", type=Path, help="build provenance.json (for notes)")
    args = parser.parse_args(argv)
    record = load_record(args.version) if VERSION.fullmatch(args.version) else None
    digest = hashlib.sha256(args.fap.read_bytes()).hexdigest() if args.fap else None
    found = problems(args.version, record, acceptance.load_checklist(), fap_version(), digest)
    if found:
        raise SystemExit("Prerelease refused:\n  " + "\n  ".join(found))
    if args.command == "notes":
        if not args.provenance:
            raise SystemExit("notes needs --provenance")
        provenance = json.loads(args.provenance.read_text(encoding="utf-8"))
        lock = json.loads((ROOT / "firmware.lock.json").read_text(encoding="utf-8"))
        sys.stdout.write(render_notes(args.version, record, provenance, lock))
    else:
        print(f"PASS: releases/{args.version}/acceptance.json backs this prerelease"
              + (" and matches the built FAP" if digest else ""))


if __name__ == "__main__":
    main()
