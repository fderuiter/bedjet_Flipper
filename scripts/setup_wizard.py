#!/usr/bin/env python3
"""Guided first-time setup: check the computer, fetch the pinned firmware source,
build the app and explain how to copy it to the Flipper.

This wizard never flashes, installs or publishes anything. It only downloads the
firmware source named in firmware.lock.json, runs build.py and prints results.

  python3 scripts/setup_wizard.py              # step by step, asks before each step
  python3 scripts/setup_wizard.py --yes        # same steps, no questions
  python3 scripts/setup_wizard.py doctor       # diagnose common problems; changes nothing
  python3 scripts/setup_wizard.py verify-download DIR   # check a downloaded CI build
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / "firmware.lock.json"
DIST = ROOT / "dist"
DEFAULT_FIRMWARE = ROOT.parent / "flipper-bedjet-firmware"
MIN_PYTHON = (3, 10)
# Rough guidance for a firmware checkout, its submodules, the ARM toolchain
# fbt downloads and build output. Not a measured requirement.
RECOMMENDED_FREE_GB = 5
FAP_NAME = "fredjet.fap"
SD_CARD_FOLDER = "/ext/apps/Bluetooth/"

OK, WARN, FAIL = "ok", "warn", "fail"


class Check:
    """One diagnostic result with a plain-language next step when it is not ok."""

    def __init__(self, name, status, detail, fix=""):
        self.name, self.status, self.detail, self.fix = name, status, detail, fix

    def __repr__(self):
        return f"Check({self.name!r}, {self.status!r}, {self.detail!r})"


def load_lock(path=LOCK):
    lock = json.loads(Path(path).read_text(encoding="utf-8"))
    for key in ("repository", "commit", "target", "radio_stack"):
        if not isinstance(lock.get(key), str) or not lock[key]:
            raise ValueError(f"{path} is missing '{key}'")
    if len(lock["commit"]) != 40 or any(c not in "0123456789abcdef" for c in lock["commit"]):
        raise ValueError(f"{path} commit must be a full lowercase 40-character hash")
    patched = lock.get("patched_commit")
    if lock.get("patches") and (not isinstance(patched, str) or len(patched) != 40 or
                                any(c not in "0123456789abcdef" for c in patched)):
        raise ValueError(f"{path} patched_commit must be a full lowercase 40-character hash")
    for patch in lock.get("patches", []):
        if not (ROOT / patch).is_file():
            raise ValueError(f"{path} names a missing patch: {patch}")
    return lock


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 16), b""):
            digest.update(block)
    return digest.hexdigest()


def run_git(directory, *args):
    return subprocess.run(["git", *args], cwd=directory, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


# --- Prerequisite checks -------------------------------------------------------

def check_python(version=None):
    version = version or sys.version_info[:3]
    shown = ".".join(map(str, version))
    if tuple(version[:2]) >= MIN_PYTHON:
        return Check("Python", OK, f"Python {shown}")
    return Check("Python", FAIL, f"Python {shown} is too old",
                 f"Install Python {MIN_PYTHON[0]}.{MIN_PYTHON[1]} or newer and run this wizard with it.")


def check_tool(name, required, purpose, which=shutil.which):
    found = which(name)
    if found:
        return Check(name, OK, found)
    return Check(name, FAIL if required else WARN, f"{name} not found ({purpose})",
                 f"Install {name}; on macOS run 'xcode-select --install', on Debian/Ubuntu "
                 f"'sudo apt install {'git' if name == 'git' else 'build-essential'}'.")


def check_platform(system=None):
    system = system or platform.system()
    if system in ("Linux", "Darwin"):
        return Check("Operating system", OK, system)
    return Check("Operating system", FAIL, f"{system} is not a documented build platform",
                 "Use Linux, macOS or WSL on Windows, or download the CI-built app instead "
                 "(see docs/getting-started.md).")


def check_disk(path, free_bytes=None):
    probe = Path(path)
    while not probe.exists() and probe != probe.parent:
        probe = probe.parent
    free = shutil.disk_usage(probe).free if free_bytes is None else free_bytes
    gigabytes = free / 1e9
    if gigabytes >= RECOMMENDED_FREE_GB:
        return Check("Free disk space", OK, f"{gigabytes:.1f} GB free at {probe}")
    return Check("Free disk space", WARN,
                 f"{gigabytes:.1f} GB free at {probe}; about {RECOMMENDED_FREE_GB} GB is recommended",
                 "Free some space or choose another --firmware-dir.")


def expected_commit(lock):
    """The commit a checkout must end at: the base plus FredJet's patches, if any."""
    return lock.get("patched_commit") or lock["commit"]


def checkout_hint(directory, lock):
    if lock.get("patches"):
        return f"python3 scripts/firmware_checkout.py {directory}"
    return (f"git -C {directory} fetch --depth 1 {lock['repository']} {lock['commit']} "
            f"&& git -C {directory} checkout --detach FETCH_HEAD")


COMMITTER = {"GIT_COMMITTER_NAME": "FredJet build", "GIT_COMMITTER_EMAIL": "fredjet-build@invalid"}


def check_lock(path=LOCK):
    try:
        lock = load_lock(path)
    except (OSError, ValueError) as error:
        return Check("Firmware pin", FAIL, str(error),
                     "Restore firmware.lock.json from Git; do not edit it by hand.")
    patches = len(lock.get("patches", []))
    detail = f"{lock['repository']} @ {lock['commit'][:12]}"
    if patches:
        detail += f" + {patches} FredJet patches = {expected_commit(lock)[:12]}"
    return Check("Firmware pin", OK, detail)


def inspect_firmware(directory, lock):
    """Describe a firmware checkout without changing it."""
    directory = Path(directory)
    if not directory.exists():
        return Check("Firmware checkout", WARN, f"{directory} does not exist yet",
                     "Run the setup wizard to download the pinned firmware source.")
    if not (directory / ".git").exists():
        return Check("Firmware checkout", FAIL, f"{directory} exists but is not a Git checkout",
                     "Choose an empty or new folder with --firmware-dir.")
    head = run_git(directory, "rev-parse", "HEAD")
    if head.returncode:
        return Check("Firmware checkout", FAIL, f"{directory} has no checked-out commit",
                     f"Remove the folder or run: {checkout_hint(directory, lock)}")
    actual = head.stdout.strip()
    if actual != expected_commit(lock):
        return Check("Firmware checkout", FAIL,
                     f"{directory} is at {actual[:12]}, but this app is pinned to "
                     f"{expected_commit(lock)[:12]}",
                     f"Run: {checkout_hint(directory, lock)}")
    dirty = run_git(directory, "status", "--porcelain", "--untracked-files=no")
    if dirty.returncode or dirty.stdout.strip():
        return Check("Firmware checkout", FAIL, f"{directory} has local changes to tracked files",
                     "build.py needs a clean pinned checkout; use a fresh folder.")
    return Check("Firmware checkout", OK, f"{directory} is clean at the pinned commit")


def inspect_dist(dist=DIST):
    fap = Path(dist) / FAP_NAME
    if not fap.is_file():
        return Check("Built app", WARN, f"no {FAP_NAME} in {dist}",
                     "Build it with this wizard, or download the CI build and use verify-download.")
    sums = Path(dist) / "SHA256SUMS"
    digest = sha256_file(fap)
    if sums.is_file() and digest not in sums.read_text(encoding="utf-8"):
        return Check("Built app", FAIL, f"{fap} does not match {sums}",
                     "Rebuild; the file may be partial or replaced.")
    return Check("Built app", OK, f"{fap} sha256 {digest}")


def doctor_checks(firmware_dir, lock_path=LOCK, dist=DIST):
    checks = [check_python(), check_platform(),
              check_tool("git", True, "downloads the firmware source"),
              check_tool(os.environ.get("CC", "cc"), False, "only needed for host tests"),
              check_disk(firmware_dir), check_lock(lock_path)]
    try:
        checks.append(inspect_firmware(firmware_dir, load_lock(lock_path)))
    except (OSError, ValueError):
        pass
    checks.append(inspect_dist(dist))
    return checks


def print_checks(checks, out=None):
    out = out or sys.stdout
    marks = {OK: "  OK  ", WARN: " WARN ", FAIL: " FAIL "}
    for check in checks:
        print(f"[{marks[check.status]}] {check.name}: {check.detail}", file=out)
        if check.fix and check.status != OK:
            print(f"         -> {check.fix}", file=out)


# --- Downloaded CI build ------------------------------------------------------

def verify_download(directory, lock):
    """Check a downloaded CI artifact folder: hash list and firmware pin."""
    directory = Path(directory)
    problems = []
    fap, sums, provenance_path = (directory / FAP_NAME, directory / "SHA256SUMS",
                                  directory / "provenance.json")
    for path in (fap, sums, provenance_path):
        if not path.is_file():
            problems.append(f"missing {path.name}")
    if problems:
        return problems, None
    digest = sha256_file(fap)
    if f"{digest}  {FAP_NAME}" not in sums.read_text(encoding="utf-8").splitlines():
        problems.append(f"{FAP_NAME} sha256 {digest} is not listed in SHA256SUMS")
    try:
        provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    except ValueError:
        return problems + ["provenance.json is not valid JSON"], None
    if provenance.get("fap", {}).get("sha256") != digest:
        problems.append("provenance.json names a different app hash")
    if provenance.get("firmware", {}).get("commit") != expected_commit(lock):
        problems.append("the build used a different firmware commit than this checkout's firmware.lock.json")
    return problems, {"sha256": digest, "provenance": provenance}


# --- Steps that change the computer -------------------------------------------

def fetch_firmware(directory, lock, runner=subprocess.run):
    """Create a new checkout at exactly the pinned commit. Never touches an existing one."""
    directory = Path(directory)
    if directory.exists() and any(directory.iterdir()):
        raise SystemExit(f"{directory} is not empty; refusing to change it.")
    commands = [
        ["git", "init", "--quiet", str(directory)],
        ["git", "-C", str(directory), "fetch", "--depth", "1", lock["repository"], lock["commit"]],
        ["git", "-C", str(directory), "-c", "advice.detachedHead=false",
         "checkout", "--quiet", "--detach", "FETCH_HEAD"],
    ]
    for patch in lock.get("patches", []):
        commands.append(["git", "-C", str(directory), "-c", "user.name=FredJet build",
                         "-c", "user.email=fredjet-build@invalid", "am", "--quiet",
                         "--committer-date-is-author-date", str(ROOT / patch)])
    for command in commands:
        print("$ " + " ".join(command), flush=True)
        runner(command, check=True, env={**os.environ, **COMMITTER})


def build_command(firmware_dir, firmware_package):
    command = [sys.executable, str(ROOT / "build.py"), str(firmware_dir)]
    if firmware_package:
        command.append("--firmware-package")
    return command


def update_packages(firmware_dir):
    return sorted(Path(firmware_dir).glob("dist/f7-*/f7-update-*"))


def copy_instructions(fap, digest):
    return f"""
Your app is ready (built, not hardware tested):
  {fap}
  SHA-256 {digest}

Copy it to the Flipper with qFlipper:
  1. Install the matching experimental firmware with the Full BLE radio stack
     first (docs/getting-started.md, step 3). The app does not run on stock firmware.
  2. Connect the Flipper by USB and open qFlipper. Close other apps using the Flipper.
  3. Open the File manager tab and go to SD Card > apps > Bluetooth
     ({SD_CARD_FOLDER}); create the Bluetooth folder if it is missing.
  4. Drag {FAP_NAME} into that folder and wait for the copy to finish.
  5. On the Flipper: Apps > Bluetooth > FredJet.
"""


def confirm(question, assume_yes, ask=input):
    if assume_yes:
        print(f"{question} [yes: --yes]")
        return True
    try:
        answer = ask(f"{question} [Y/n] ").strip().lower()
    except EOFError:
        return False
    return answer in ("", "y", "yes")


def run_setup(args, ask=input):
    lock = load_lock()
    firmware_dir = Path(args.firmware_dir).expanduser().resolve()
    print("FredJet setup. Nothing here flashes or installs anything on your Flipper.\n")
    print("Step 1 of 4: check this computer")
    checks = [check_python(), check_platform(),
              check_tool("git", True, "downloads the firmware source"),
              check_disk(firmware_dir), check_lock()]
    print_checks(checks)
    if any(c.status == FAIL for c in checks):
        raise SystemExit("\nFix the FAIL items above, then run the wizard again.")

    print(f"\nStep 2 of 4: get the pinned firmware source ({lock['commit'][:12]})")
    state = inspect_firmware(firmware_dir, lock)
    if state.status == OK:
        print_checks([state])
    elif not firmware_dir.exists() or not any(firmware_dir.iterdir()):
        if not confirm(f"Download {lock['repository']} into {firmware_dir}?", args.yes, ask):
            raise SystemExit("Stopped before downloading. Nothing was changed.")
        fetch_firmware(firmware_dir, lock)
        state = inspect_firmware(firmware_dir, lock)
        print_checks([state])
        if state.status != OK:
            raise SystemExit("The downloaded firmware did not verify; see above.")
    else:
        print_checks([state])
        raise SystemExit("\nThe wizard does not modify an existing folder. Follow the hint above "
                         "or pass a new --firmware-dir.")

    print("\nStep 3 of 4: build the app")
    package = args.firmware_package
    if package is None:
        package = (not args.yes) and confirm(
            "Also build the matching Full BLE firmware update package (needed once per Flipper)?",
            False, ask)
    command = build_command(firmware_dir, package)
    if not confirm("Start the build? The first build downloads the ARM toolchain and can take a while.",
                   args.yes, ask):
        raise SystemExit("Stopped before building.")
    print("$ " + " ".join(command), flush=True)
    if subprocess.run(command).returncode:
        raise SystemExit(
            "\nThe build failed; the first error above names the cause. A failed toolchain or\n"
            "submodule download usually means no access to update.flipperzero.one or\n"
            "github.com. Run 'python3 scripts/setup_wizard.py doctor', fix the problem and run\n"
            "the wizard again; the verified firmware folder is reused.")

    print("\nStep 4 of 4: results")
    fap = DIST / FAP_NAME
    digest = sha256_file(fap)
    provenance = json.loads((DIST / "provenance.json").read_text(encoding="utf-8"))
    print(f"Provenance: {DIST / 'provenance.json'} (status: {provenance.get('status')})")
    if package:
        for folder in update_packages(firmware_dir):
            print(f"Firmware update package (not installed): {folder}")
    print(copy_instructions(fap, digest))
    print("Next: docs/getting-started.md, then scripts/acceptance_wizard.py for device testing.")


def main(argv=None, ask=input):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", nargs="?", default="setup",
                        choices=("setup", "doctor", "verify-download"))
    parser.add_argument("path", nargs="?", help="folder for verify-download")
    parser.add_argument("--firmware-dir", default=str(DEFAULT_FIRMWARE),
                        help=f"where the pinned firmware lives (default {DEFAULT_FIRMWARE})")
    parser.add_argument("--yes", "-y", action="store_true",
                        help="answer yes to every step (for repeat runs)")
    package = parser.add_mutually_exclusive_group()
    package.add_argument("--firmware-package", dest="firmware_package", action="store_true",
                         default=None, help="also build the Full BLE firmware update package")
    package.add_argument("--no-firmware-package", dest="firmware_package", action="store_false")
    args = parser.parse_args(argv)

    if args.command == "doctor":
        checks = doctor_checks(Path(args.firmware_dir).expanduser().resolve())
        print_checks(checks)
        failed = sum(c.status == FAIL for c in checks)
        print(f"\n{failed} problem(s) found." if failed else "\nNo blocking problems found.")
        return 1 if failed else 0
    if args.command == "verify-download":
        if not args.path:
            parser.error("verify-download needs the folder you unzipped the CI build into")
        problems, result = verify_download(args.path, load_lock())
        if problems:
            print("Download did NOT verify:\n  " + "\n  ".join(problems))
            return 1
        print(f"Download verified: {FAP_NAME} sha256 {result['sha256']}, built against the pinned "
              f"firmware (status: {result['provenance'].get('status')}).")
        print(copy_instructions(Path(args.path) / FAP_NAME, result["sha256"]))
        return 0
    run_setup(args, ask)
    return 0


if __name__ == "__main__":
    sys.exit(main())
