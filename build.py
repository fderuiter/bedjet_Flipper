#!/usr/bin/env python3
"""Build against the locked Flipper firmware. Never flash or publish."""

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent
APP_FILES = (
    "application.fam", "bedjet_remote.c", "bedjet_protocol.c", "bedjet_protocol.h",
    "bedjet_controller.c", "bedjet_controller.h", "bedjet_flipper.c", "bedjet_flipper.h",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware_checkout", type=pathlib.Path)
    parser.add_argument("--firmware-package", action="store_true",
                        help="Also build the matching Full BLE update package; do not install it")
    args = parser.parse_args()
    lock = json.loads((ROOT / "firmware.lock.json").read_text())
    firmware = args.firmware_checkout.expanduser().resolve()
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=firmware, text=True).strip()
    if actual != lock["commit"]:
        raise SystemExit(f"Expected firmware {lock['commit']}; got {actual}. Review changes before updating the lock.")
    # Local firmware modifications would invalidate the source pin and provenance.
    dirty = subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"],
                                    cwd=firmware, text=True)
    if dirty.strip():
        raise SystemExit("Firmware has tracked modifications. Use a clean pinned checkout.")
    destination = firmware / "applications_user/bedjet_remote"
    destination.mkdir(parents=True, exist_ok=True)
    for name in APP_FILES:
        shutil.copy2(ROOT / name, destination / name)
    fbt = str(firmware / "fbt")
    subprocess.run([fbt, "fap_bedjet_remote", "COMPACT=1", "DEBUG=0"], cwd=firmware, check=True)
    binary = firmware / "build/f7-firmware-C/.extapps/bedjet_remote.fap"
    output = ROOT / "dist"
    output.mkdir(exist_ok=True)
    shutil.copy2(binary, output / "bedjet_remote.fap")
    digest = hashlib.sha256((output / "bedjet_remote.fap").read_bytes()).hexdigest()
    (output / "SHA256SUMS").write_text(f"{digest}  bedjet_remote.fap\n")
    print(output / "bedjet_remote.fap", flush=True)
    if args.firmware_package:
        subprocess.run([
            fbt, "updater_package", "COMPACT=1", "DEBUG=0",
            f"COPRO_OB_DATA={lock['option_bytes']}",
            f"COPRO_STACK_BIN={lock['radio_binary']}",
            f"COPRO_STACK_TYPE={lock['radio_stack']}",
        ], cwd=firmware, check=True)
        print("Firmware package built. No device was flashed.", flush=True)


if __name__ == "__main__":
    main()
