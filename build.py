#!/usr/bin/env python3
"""Build against the locked Flipper firmware. Never flash or publish."""

import argparse
import hashlib
import json
import os
import pathlib
import platform
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent
APP_FILES = (
    "application.fam", "bedjet_remote.c", "bedjet_protocol.c", "bedjet_protocol.h",
    "bedjet_controller.c", "bedjet_controller.h", "bedjet_flipper.c", "bedjet_flipper.h",
    "bedjet_view.c", "bedjet_view.h", "bedjet_log.c", "bedjet_log.h", "fredjet_10px.png",
)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def source_revision():
    try:
        return {"commit": git("rev-parse", "HEAD"),
                "dirty": bool(git("status", "--porcelain", "--untracked-files=no"))}
    except (OSError, subprocess.CalledProcessError):
        return {"commit": None, "dirty": None}


def api_version(firmware):
    for line in (firmware / "targets/f7/api_symbols.csv").read_text().splitlines():
        if line.startswith("Version,"):
            return line.split(",")[2]
    return None


def toolchain(firmware):
    root = pathlib.Path(os.environ.get("FBT_TOOLCHAIN_PATH", firmware)) / "toolchain"
    arch = root / f"{platform.machine()}-{platform.system().lower()}"
    gcc = arch / "bin" / "arm-none-eabi-gcc"
    version_file = arch / "VERSION"
    try:
        gcc_version = subprocess.check_output([str(gcc), "--version"], text=True).splitlines()[0]
    except (OSError, subprocess.CalledProcessError):
        gcc_version = None
    return {"fbt_toolchain_version": version_file.read_text().strip() if version_file.exists() else None,
            "gcc": gcc_version}


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
    destination = firmware / "applications_user/fredjet"
    destination.mkdir(parents=True, exist_ok=True)
    for name in APP_FILES:
        shutil.copy2(ROOT / name, destination / name)
    fbt = str(firmware / "fbt")
    # SCons otherwise iterates hash-randomized sets, so the link order and FAP
    # bytes vary between otherwise identical builds.
    environment = {**os.environ, "PYTHONHASHSEED": "0"}
    subprocess.run([fbt, "fap_fredjet", "COMPACT=1", "DEBUG=0"],
                   cwd=firmware, env=environment, check=True)
    binary = firmware / "build/f7-firmware-C/.extapps/fredjet.fap"
    output = ROOT / "dist"
    output.mkdir(exist_ok=True)
    shutil.copy2(binary, output / "fredjet.fap")
    digest = hashlib.sha256((output / "fredjet.fap").read_bytes()).hexdigest()
    (output / "SHA256SUMS").write_text(f"{digest}  fredjet.fap\n")
    provenance = {
        "status": "compiled-not-hardware-validated",
        "source": source_revision(),
        "firmware": {
            "repository": lock["repository"],
            "commit": actual,
            # Upstream reused API 88.3 for unrelated symbols; identify the
            # experimental central API by commit and header hash instead.
            "api_version": api_version(firmware),
            "central_h_sha256": sha256(firmware / "targets/f7/ble_glue/furi_ble/central.h"),
        },
        "toolchain": toolchain(firmware),
        "command": "PYTHONHASHSEED=0 ./fbt fap_fredjet COMPACT=1 DEBUG=0",
        "fap": {"sha256": digest, "size_bytes": (output / "fredjet.fap").stat().st_size},
        "hardware_tests_performed": False,
    }
    (output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    print(output / "fredjet.fap", flush=True)
    if args.firmware_package:
        subprocess.run([
            fbt, "updater_package", "COMPACT=1", "DEBUG=0",
            f"COPRO_OB_DATA={lock['option_bytes']}",
            f"COPRO_STACK_BIN={lock['radio_binary']}",
            f"COPRO_STACK_TYPE={lock['radio_stack']}",
        ], cwd=firmware, env=environment, check=True)
        print("Firmware package built. No device was flashed.", flush=True)


if __name__ == "__main__":
    main()
