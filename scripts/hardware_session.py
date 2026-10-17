#!/usr/bin/env python3
"""Run a hardware test night from the computer, over the Flipper's USB cable.

Talks to the Flipper's serial CLI, the same port qFlipper uses (close qFlipper
and lab.flipper.net first). It copies files to the SD card and checks them by
MD5, records the Flipper log with computer timestamps and session markers,
walks you through sessions A-G of docs/bringup.md, and turns the result into
per-session capture candidates and a sanitized summary.

It never sends a BedJet command on its own. A Flipper button is pressed only
when you type it (`key`, `scan`, or a /command during `run`), exactly as if you
pressed it on the Flipper; BedJet buttons stay yours. The only step that changes the Flipper's firmware is
`push-update --flash`, which also asks you to type "yes".

  python3 scripts/hardware_session.py ports                # list Flipper serial ports
  python3 scripts/hardware_session.py doctor               # firmware, radio stack, app on SD
  python3 scripts/hardware_session.py install-app [FAP]    # copy fredjet.fap to the SD card
  python3 scripts/hardware_session.py push-update DIR      # copy an update package to the SD card
  python3 scripts/hardware_session.py run                  # guided sessions with live log
  python3 scripts/hardware_session.py report dist/bench/RUN  # rebuild a run's summary
  python3 scripts/hardware_session.py open | close | app   # open/close FredJet, show the open app
  python3 scripts/hardware_session.py scan                 # open FredJet if needed, press OK
  python3 scripts/hardware_session.py key ok [--long]      # press a Flipper button
  python3 scripts/hardware_session.py pull-log             # copy FredJet's SD-card session log

Runs go to dist/bench/ (ignored by Git). raw.log there is private: it can hold
unit names and addresses. Share summary.md and the captures/ folder instead.
Standard library only (no pyserial); macOS and Linux.
"""

import argparse
import datetime
import glob
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
BRINGUP = ROOT / "docs/bringup.md"
LOCK = ROOT / "firmware.lock.json"
DEFAULT_FAP = ROOT / "dist/fredjet.fap"
DEFAULT_OUT = ROOT / "dist/bench"
APP_DIR = "/ext/apps/Bluetooth"
APP_PATH = APP_DIR + "/fredjet.fap"
APP_DATA_DIR = "/ext/apps_data/fredjet"
OLD_APP_PATH = APP_DIR + "/bedjet_remote.fap"
UPDATE_ROOT = "/ext/update"
# storage write_chunk reads the pipe in 1 KiB pieces; 4 KiB per command keeps
# each round trip short enough to show progress.
CHUNK = 4096
# ST's wireless stack numbering (shci.h): 1 is BLE Full. Inferred, not read
# from the pinned firmware, so a mismatch is a warning.
FULL_STACK_TYPE = "1"
# The device info keys worth keeping: no names, unique IDs or MAC addresses.
INFO_KEEP = re.compile(r"^(firmware\.|hardware\.(target|ver|model|region)|radio\.(alive|mode|fus\.|stack\.))")
END_WORDS = ("end", "done", ".")
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
PROMPT = re.compile(r">:\s*$")
OK, WARN, FAIL = "ok", "warn", "fail"


def _load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(f"{name}.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# captures.py owns the frame format check.py replays; the acceptance wizard owns
# sanitizing and the issue #2 report.
captures = _load("captures")
acceptance = _load("acceptance_wizard")
sanitize = acceptance.sanitize


class CliError(Exception):
    pass


def now_iso():
    return datetime.datetime.now().isoformat(timespec="milliseconds")


# --- Serial port --------------------------------------------------------------------

def find_ports(patterns=("/dev/cu.usbmodemflip*", "/dev/serial/by-id/*Flipper*", "/dev/ttyACM*")):
    """Candidate Flipper ports, most specific first. macOS names them usbmodemflip_<name>."""
    found = []
    for pattern in patterns:
        for path in sorted(glob.glob(pattern)):
            if path not in found:
                found.append(path)
    return found


def resolve_port(requested, finder=find_ports):
    if requested:
        return requested
    ports = finder()
    if not ports:
        raise CliError("No Flipper serial port found. Plug the Flipper in over USB, unlock it, "
                       "close qFlipper, then retry (or pass --port).")
    if len(ports) > 1 and not ports[0].startswith("/dev/cu.usbmodemflip"):
        raise CliError("Several serial ports found; pick one with --port: " + ", ".join(ports))
    return ports[0]


class SerialPort:
    """A raw tty opened with the standard library; USB CDC ignores the baud rate."""

    def __init__(self, path):
        import termios
        import tty
        try:
            self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError as error:
            raise CliError(f"Cannot open {path}: {error.strerror}. If qFlipper or lab.flipper.net "
                           "is open, close it; only one program can use the port.") from None
        tty.setraw(self.fd)
        if hasattr(termios, "TIOCEXCL"):
            try:
                import fcntl
                fcntl.ioctl(self.fd, termios.TIOCEXCL)
            except OSError:
                pass
        self.path = path

    def read(self, timeout):
        try:
            ready, _, _ = select.select([self.fd], [], [], max(timeout, 0))
            return os.read(self.fd, 4096) if ready else b""
        except OSError as error:
            raise CliError(f"The Flipper disconnected ({error.strerror}).") from None

    def write(self, data):
        view = memoryview(data)
        while view:
            try:
                select.select([], [self.fd], [], 5)
                view = view[os.write(self.fd, view):]
            except BlockingIOError:
                continue
            except OSError as error:
                raise CliError(f"The Flipper disconnected ({error.strerror}).") from None

    def close(self):
        os.close(self.fd)


def clean(raw):
    return ANSI.sub("", raw.decode("utf-8", errors="replace")).replace("\r", "")


class FlipperCli:
    """Commands of the pinned firmware's CLI (checked against its storage, log and update
    CLI sources). Each command echoes, prints its output, then the `>: ` prompt."""

    def __init__(self, port):
        self.port = port
        self.buffer = b""

    def _read_until(self, done, timeout, what):
        deadline = time.monotonic() + timeout
        while not done(clean(self.buffer)):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise CliError(f"No answer from the Flipper while waiting for {what}. "
                               "Is it unlocked and not inside an app that holds the CLI?")
            self.buffer += self.port.read(min(remaining, 0.2))
        return clean(self.buffer)

    def _drain(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if not self.port.read(deadline - time.monotonic()):
                break

    def sync(self):
        """Get a fresh prompt; Ctrl+C first stops a `log` left running."""
        self.buffer = b""
        self.port.write(b"\x03\r")
        self._read_until(lambda t: PROMPT.search(t), 5, "the CLI prompt")
        self._drain(0.3)
        self.buffer = b""

    def command(self, line, timeout=10):
        self.buffer = b""
        self.port.write(line.encode() + b"\r")
        text = self._read_until(lambda t: PROMPT.search(t) and "\n" in t, timeout, f"'{line}'")
        return output_of(text, line)

    def info_device(self):
        return parse_info(self.command("info device"))

    def stat(self, path):
        """'missing', 'dir', or the file size in bytes."""
        out = self.command(f"storage stat {path}")
        size = re.search(r"File, size: (\d+)b", out)
        if size:
            return int(size.group(1))
        if out.startswith("Directory") or re.match(r"Storage(,|$)", out):
            return "dir"
        return "missing"

    def md5(self, path):
        out = self.command(f"storage md5 {path}", timeout=60)
        return out if re.fullmatch(r"[0-9a-f]{32}", out) else None

    def free_kib(self, root="/ext"):
        found = re.search(r"(\d+)KiB free", self.command(f"storage stat {root}"))
        return int(found.group(1)) if found else None

    def mkdirs(self, path):
        parts = path.strip("/").split("/")
        for depth in range(2, len(parts) + 1):
            sub = "/" + "/".join(parts[:depth])
            if self.stat(sub) == "missing":
                out = self.command(f"storage mkdir {sub}")
                if "error" in out.lower():
                    raise CliError(f"Could not create {sub}: {out}")

    def remove(self, path):
        out = self.command(f"storage remove {path}")
        if "error" in out.lower():
            raise CliError(f"Could not remove {path}: {out}")

    def rename(self, old, new):
        out = self.command(f"storage rename {old} {new}")
        if "error" in out.lower():
            raise CliError(f"Could not rename {old} to {new}: {out}")

    def write_file(self, path, data, progress=None):
        """Append-only on the device, so the target must not exist yet."""
        sent = 0
        while sent < len(data) or not data:
            piece = data[sent:sent + CHUNK]
            self.buffer = b""
            line = f"storage write_chunk {path} {len(piece)}"
            self.port.write(line.encode() + b"\r")
            text = self._read_until(lambda t: "Ready" in t or (PROMPT.search(t) and "\n" in t),
                                    10, "the upload to start")
            if "Ready" not in text:
                raise CliError(f"Upload to {path} refused: {output_of(text, line)}")
            self.buffer = b""
            self.port.write(piece)
            after = self._read_until(lambda t: PROMPT.search(t), 30, "the upload to finish")
            if "error" in after.lower():
                raise CliError(f"Upload to {path} failed: {after.strip()}")
            sent += len(piece)
            if progress:
                progress(sent, len(data))
            if not data:
                break

    def read_file(self, path, chunk=512):
        """`storage read_chunks`: Size, then one chunk per byte we send after Ready?."""
        self.buffer = b""
        self.port.write(f"storage read_chunks {path} {chunk}\r".encode())
        self._read_until(lambda t: "Size: " in t or "Storage error" in t, 10, f"'{path}'")
        text = clean(self.buffer)
        if "Storage error" in text and "Size: " not in text:
            self._read_until(lambda t: PROMPT.search(t), 5, "the prompt")
            raise CliError(f"Cannot read {path}: {output_of(text, 'storage').strip()}")
        raw = self.buffer
        while b"\r\n" not in raw[raw.index(b"Size: "):]:
            raw += self.port.read(0.2)
        head, _, rest = raw[raw.index(b"Size: "):].partition(b"\r\n")
        size, data, self.buffer = int(head[6:]), b"", rest
        while len(data) < size:
            deadline = time.monotonic() + 10
            while b"Ready?\r\n" not in self.buffer:
                if time.monotonic() > deadline:
                    raise CliError(f"No answer while reading {path}.")
                self.buffer += self.port.read(0.2)
            self.buffer = self.buffer.split(b"Ready?\r\n", 1)[1]
            self.port.write(b"y")
            want = min(chunk, size - len(data))
            deadline = time.monotonic() + 10
            while len(self.buffer) < want:
                if time.monotonic() > deadline:
                    raise CliError(f"Read of {path} stalled.")
                self.buffer += self.port.read(0.2)
            data, self.buffer = data + self.buffer[:want], self.buffer[want:]
        self._read_until(lambda t: PROMPT.search(t), 10, "the prompt after reading")
        self.buffer = b""
        if self.md5(path) != md5_bytes(data):
            raise CliError(f"{path} changed or arrived damaged; try again.")
        return data

    def start_log(self, level="info"):
        self.buffer = b""
        self.port.write(f"log {level}\r".encode())
        text = self._read_until(lambda t: "CTRL+C" in t, 5, "the log to start")
        # Anything after the banner is already log output.
        rest = text.split("CTRL+C to stop...", 1)[-1]
        self.buffer = b""
        return rest.lstrip("\n")

    def stop_log(self):
        self.buffer = b""
        self.port.write(b"\x03")
        self._read_until(lambda t: PROMPT.search(t), 5, "the log to stop")
        self.buffer = b""


def output_of(text, line):
    """A command's output: what follows its echo, without the trailing prompt."""
    lines = text.split("\n")
    echo = next((i for i, value in enumerate(lines) if line in value), 0)
    lines = lines[echo + 1:]
    if lines and PROMPT.search(lines[-1]):
        lines[-1] = PROMPT.sub("", lines[-1])
    return "\n".join(value.rstrip() for value in lines).strip()


def parse_info(text):
    """`info device` lines are `key<padding>: value` with dotted keys."""
    info = {}
    for line in text.splitlines():
        key, sep, value = line.partition(":")
        if sep and re.fullmatch(r"[a-z0-9_.]+", key.strip()):
            info[key.strip()] = value.strip()
    return info


def shareable_info(info):
    """Only firmware, hardware target and radio values; never names, UIDs or MACs."""
    return {k: v for k, v in info.items() if INFO_KEEP.match(k) and "mac" not in k}


# --- Checks -------------------------------------------------------------------------------

def load_lock(path=LOCK):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def md5_bytes(data):
    return hashlib.md5(data).hexdigest()  # Matches `storage md5`; not a security check.


def device_checks(info, lock):
    """Compare the Flipper's `info device` with firmware.lock.json."""
    checks = []
    # `info device` prints the commit as firmware.commit.hash (#30).
    commit = info.get("firmware.commit.hash", info.get("firmware.commit", ""))
    pinned = lock.get("patched_commit") or lock["commit"]
    if commit and len(commit) >= 7 and pinned.startswith(commit.lower()):
        checks.append((OK, "Firmware", f"pinned commit {commit}"))
    else:
        checks.append((FAIL, "Firmware", f"commit {commit or 'unknown'}; the pin is {pinned[:8]}. "
                       "Install the Full BLE update package first (push-update)."))
    api = f"{info.get('firmware.api.major', '?')}.{info.get('firmware.api.minor', '?')}"
    checks.append((OK if api == lock.get("api_version") else FAIL, "Firmware API",
                   f"{api}; the app is built for {lock.get('api_version')}"))
    target = info.get("hardware.target", "")
    checks.append((OK if f"f{target}" == lock.get("target") else WARN, "Hardware target",
                   f"f{target or '?'}"))
    if info.get("radio.alive") != "true":
        checks.append((FAIL, "Radio", "radio core not running"))
        return checks
    version = ".".join(info.get(f"radio.stack.{p}", "?") for p in ("major", "minor", "sub"))
    stack_type = info.get("radio.stack.type", "?")
    status = OK if version == lock.get("radio_version") and stack_type == FULL_STACK_TYPE else WARN
    checks.append((status, "Radio stack", f"type {stack_type} (1 is Full BLE, inferred from ST's "
                   f"numbering), version {version}; the pin is Full BLE {lock.get('radio_version')}"))
    return checks


def app_check(cli, fap):
    """Whether the SD card holds exactly the local fredjet.fap."""
    remote = cli.md5(APP_PATH) if cli.stat(APP_PATH) != "missing" else None
    if not fap or not Path(fap).is_file():
        return (WARN if remote else FAIL, "App on SD",
                "installed; no local fredjet.fap to compare" if remote else "not installed")
    local = md5_bytes(Path(fap).read_bytes())
    if remote == local:
        return (OK, "App on SD", f"matches {Path(fap).name}")
    return (FAIL, "App on SD", "differs from the local build" if remote else "not installed")


def print_checks(checks, out=None):
    out = out or sys.stdout
    for status, name, detail in checks:
        print(f"  [{status:4}] {name}: {detail}", file=out)


# --- Copying files ------------------------------------------------------------------------

def upload(cli, local_bytes, remote, label, out=None):
    """Write to a temporary name, verify MD5, then move into place."""
    out = out or sys.stdout
    expected = md5_bytes(local_bytes)
    folder = remote.rsplit("/", 1)[0]
    cli.mkdirs(folder)
    part = remote + ".part"
    if cli.stat(part) != "missing":
        cli.remove(part)

    def progress(sent, total):
        print(f"\r  {label}: {sent * 100 // max(total, 1)}%", end="", file=out, flush=True)

    cli.write_file(part, local_bytes, progress)
    print(file=out)
    actual = cli.md5(part)
    if actual != expected:
        cli.remove(part)
        raise CliError(f"{label}: MD5 on the Flipper is {actual}, expected {expected}. "
                       "Nothing was replaced; retry the copy.")
    if cli.stat(remote) != "missing":
        cli.remove(remote)
    cli.rename(part, remote)
    return expected


def check_fap(fap):
    """When SHA256SUMS sits next to the FAP (a CI download or build.py output), it must match."""
    fap = Path(fap)
    if not fap.is_file():
        raise CliError(f"{fap} not found. Download the CI artifact or build first (setup_wizard.py).")
    digest = hashlib.sha256(fap.read_bytes()).hexdigest()
    sums = fap.with_name("SHA256SUMS")
    if sums.is_file() and f"{digest}  {fap.name}" not in sums.read_text(encoding="utf-8").splitlines():
        raise CliError(f"{fap.name} does not match {sums}; it may be a different or damaged build.")
    return digest


def install_app(cli, fap, ask, out=None):
    out = out or sys.stdout
    digest = check_fap(fap)
    data = Path(fap).read_bytes()
    if cli.stat(APP_PATH) != "missing":
        if cli.md5(APP_PATH) == md5_bytes(data):
            print(f"  fredjet.fap on the SD card already matches (SHA-256 {digest[:16]}...).", file=out)
            return digest
        if not yes(ask("  A different fredjet.fap is on the SD card. Replace it? [y/N] ")):
            raise CliError("Left the installed app unchanged.")
    upload(cli, data, APP_PATH, "fredjet.fap", out)
    print(f"  Installed {APP_PATH} (SHA-256 {digest}).", file=out)
    if cli.stat(OLD_APP_PATH) != "missing":
        print(f"  Note: an older {OLD_APP_PATH} is still there and shows a second menu entry.\n"
              "  Delete it from the Flipper's file browser if you no longer need it.", file=out)
    return digest


def find_package(directory):
    """The f7-update-local folder: DIR itself, or the one update.fuf beneath it."""
    directory = Path(directory)
    if (directory / "update.fuf").is_file():
        return directory
    found = sorted(directory.glob("**/update.fuf"))
    if len(found) != 1:
        raise CliError(f"Expected one update.fuf under {directory}; found {len(found)}.")
    return found[0].parent


def check_package(package, lock):
    """The manifest must install the pinned Full BLE stack at the pinned address."""
    manifest = {}
    for line in (package / "update.fuf").read_text(encoding="utf-8", errors="replace").splitlines():
        key, sep, value = line.partition(":")
        if sep:
            manifest[key.strip()] = value.strip()
    problems = []
    address = int(lock["radio_address"], 16).to_bytes(4, "little")
    expected = " ".join(f"{b:02X}" for b in address)
    if manifest.get("Radio") != "radio.bin":
        problems.append(f"manifest Radio is {manifest.get('Radio')!r}, expected 'radio.bin'")
    if manifest.get("Radio address", "").upper() != expected:
        problems.append(f"manifest Radio address is {manifest.get('Radio address')!r}, expected {expected}")
    for key in ("Loader", "Radio", "Resources", "Splash"):
        name = manifest.get(key)
        if name and not (package / name).is_file():
            problems.append(f"manifest names {name} ({key}) but it is not in {package}")
    sums = next((p / "UPDATE_SHA256SUMS" for p in [package, *list(package.parents)[:3]]
                 if (p / "UPDATE_SHA256SUMS").is_file()), None)
    if sums:
        # CI writes the list from inside dist/update but stores it in dist/ (#30), so an
        # entry may be relative to the list's folder or to the package's parent.
        for line in sums.read_text(encoding="utf-8").splitlines():
            digest, _, name = line.partition("  ")
            path = next((b / name.strip() for b in (sums.parent, package.parent)
                         if (b / name.strip()).is_file()), sums.parent / name.strip())
            if not name.strip():
                continue
            if not path.is_file():
                problems.append(f"{sums.name} lists {name.strip()} but it is missing")
            elif hashlib.sha256(path.read_bytes()).hexdigest() != digest.strip():
                problems.append(f"{name.strip()} does not match {sums.name}")
    return problems, sums


def push_update(cli, directory, lock, flash, ask, out=None):
    out = out or sys.stdout
    package = find_package(directory)
    problems, sums = check_package(package, lock)
    if problems:
        raise CliError("Update package rejected:\n  " + "\n  ".join(problems))
    print(f"  Package {package} checked" + (f" against {sums.name}." if sums else
          " (no UPDATE_SHA256SUMS found next to it)."), file=out)
    remote_root = f"{UPDATE_ROOT}/{package.name}"
    files = sorted(p for p in package.rglob("*") if p.is_file())
    for path in files:
        remote = f"{remote_root}/{path.relative_to(package).as_posix()}"
        data = path.read_bytes()
        if cli.stat(remote) != "missing" and cli.md5(remote) == md5_bytes(data):
            print(f"  {path.name}: already on the SD card", file=out)
            continue
        upload(cli, data, remote, path.name, out)
    manifest = f"{remote_root}/update.fuf"
    print(f"  Copied {len(files)} files to {remote_root}.", file=out)
    if not flash:
        print("  Nothing was installed. On the Flipper: Archive > Browser > update > "
              f"{package.name} > update.fuf > Run in App.\n"
              "  Or rerun with --flash to start it from here.", file=out)
        return False
    print("\n  This replaces the Flipper's firmware and radio stack. Make a qFlipper backup\n"
          "  first (docs/bringup.md section 0). The Flipper restarts several times; do not\n"
          "  unplug it. If it stops with an [XX-YY] code, photograph it and stop.", file=out)
    if ask('  Type "yes" to install the update now: ').strip() != "yes":
        print("  Not installed.", file=out)
        return False
    try:
        print("  " + cli.command(f"update install {manifest}", timeout=60).replace("\n", "\n  "), file=out)
    except CliError:
        # The Flipper restarts into the updater as soon as it accepts the package, which
        # drops the USB port before the prompt returns (#30).
        print("  The Flipper restarted into the updater (USB dropped, as expected).\n"
              "  Watch its screen until it is back on the desktop, then run doctor.", file=out)
    return True


# --- FredJet's SD-card session log -------------------------------------------------------------

SD_LOGS = (APP_DATA_DIR + "/session.1.log", APP_DATA_DIR + "/session.log")


def pull_logs(cli, out_root, out=None):
    """Copy FredJet's session logs (older first) into a new folder and summarise them."""
    out = out or sys.stdout
    folder = Path(out_root) / datetime.datetime.now().strftime("sdlog-%Y%m%d-%H%M%S")
    texts = []
    for remote in SD_LOGS:
        if cli.stat(remote) == "missing":
            continue
        data = cli.read_file(remote)
        folder.mkdir(parents=True, exist_ok=True)
        (folder / Path(remote).name).write_bytes(data)
        texts.append(data.decode("utf-8", errors="replace"))
    if not texts:
        raise CliError("No FredJet session log on the SD card yet. Open FredJet once first.")
    text = "\n".join(texts)
    counts = {key: text.count(key) for key in
              ("=== FredJet session", "rejected ", "Link down", "Retry ", "BLE connect: ok", "FAILED")}
    print(f"  Saved to {folder} (private: it can hold unit names).", file=out)
    print("  Sessions {}, connects {}, link drops {}, retries {}, rejected frames {}, failed calls {}.".format(
        counts["=== FredJet session"], counts["BLE connect: ok"], counts["Link down"], counts["Retry "],
        counts["rejected "], counts["FAILED"]), file=out)
    return folder


# --- Flipper control -------------------------------------------------------------------------

KEYS = ("up", "down", "left", "right", "ok", "back")
ACTION_HELP = "/open, /close, /ok, /up, /down, /left, /right, /back, /long KEY, /app"


def press(cli, key, long=False):
    """One button press as the Flipper's input service reports it: Press, Short (or Long),
    Release. The app reacts as if you pressed the button; this is you pressing it."""
    if key not in KEYS:
        raise CliError(f"Unknown key {key!r}; use one of {', '.join(KEYS)}.")
    for kind in ("press", "long" if long else "short", "release"):
        out = cli.command(f"input send {key} {kind}")
        if out:
            raise CliError(f"The Flipper refused 'input send {key} {kind}': {out}")
        time.sleep(0.05)


def running_app(cli):
    """The `loader info` line: which application is open, if any."""
    return cli.command("loader info")


def app_open(cli):
    """Whether an application is open (`loader info` prints 'Application "X" is running')."""
    return running_app(cli).startswith("Application")


def open_app(cli):
    if app_open(cli):
        raise CliError(f"An app is already open ({running_app(cli)}). Close it first (close).")
    out = cli.command(f"loader open {APP_PATH}", timeout=20)
    if out:
        raise CliError(f"Could not open FredJet: {out}")


def close_app(cli, tries=3):
    """`loader close`; FredJet does not take the loader's exit signal, so fall back to Back,
    which leaves FredJet from its start screen (from a connection it disconnects first)."""
    out = cli.command("loader close", timeout=20)
    if "closed manually" not in out:
        return out
    for _ in range(tries):
        press(cli, "back")
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if not app_open(cli):
                return "closed with Back"
            time.sleep(0.3)
    raise CliError(f"Still open after {tries} Back presses: {running_app(cli)}")


def flipper_action(cli, text):
    """Run one '/command' typed during a session; returns what was done, for the log."""
    words = text.lstrip("/").lower().split()
    if not words or words[0] in ("help", "?"):
        return None
    verb, rest = words[0], words[1:]
    if verb == "open":
        open_app(cli)
        return "opened FredJet"
    if verb == "close":
        return f"closed app: {close_app(cli)}"
    if verb == "app":
        return running_app(cli)
    if verb in ("long", "hold") and len(rest) == 1:
        press(cli, rest[0], long=True)
        return f"held {rest[0]}"
    if verb in KEYS and not rest:
        press(cli, verb)
        return f"pressed {verb}"
    raise CliError(f"Unknown command {text!r}. Commands: {ACTION_HELP}")


# --- Sessions and recording -------------------------------------------------------------------

def load_sessions(path=BRINGUP):
    """Sessions A-G from the bring-up runbook's table, so the two cannot drift apart."""
    rows = re.findall(r"^\|\s*([A-Z])\.\s*([^|]+?)\s*\|\s*([^|]+?)\s*\|\s*([^|]+?)\s*\|\s*$",
                      Path(path).read_text(encoding="utf-8"), re.MULTILINE)
    if not rows:
        raise SystemExit(f"{path} has no session table (| A. Name | What to do | What it settles |)")
    return [{"id": i, "name": n, "todo": t, "settles": s} for i, n, t, s in rows]


BEDJET_LINE = re.compile(r"\[BedJet\]\s+(.*)$")


class Recorder:
    """Writes `<host time> | dev|mark|note | text` lines and counts frames per session.

    Device lines keep their own millisecond tick, so the log has both clocks."""

    def __init__(self, path, out=None, live=True):
        out = out or sys.stdout
        self.file = open(path, "a", encoding="utf-8")
        self.lock = threading.Lock()
        self.session = None
        self.counts = {}
        self.messages = {}
        self.out, self.live = out, live
        self.partial = ""

    def _write(self, kind, text):
        self.file.write(f"{now_iso()} | {kind} | {text}\n")
        self.file.flush()

    def feed(self, chunk):
        with self.lock:
            self.partial += chunk
            *lines, self.partial = self.partial.split("\n")
        for line in lines:
            if line.strip():
                self.device(line.rstrip())

    def device(self, line):
        with self.lock:
            self._write("dev", line)
            counts = self.counts.setdefault(self.session, {"status": 0, "read": 0, "rejected": 0})
            frame = captures.FRAME.search(line)
            if frame:
                counts[frame.group(1)] += 1
                if frame.group(1) == "status":
                    self._say(f"status frame: {frame.group(2)}")
                elif frame.group(1) == "rejected":
                    self._say(f"REJECTED frame ({frame.group(2)}): a finding, keep this log")
                return
            message = BEDJET_LINE.search(line)
            if message:
                text = sanitize(message.group(1).strip())
                self.messages.setdefault(self.session, []).append(text)
                self._say(f"FredJet: {text}")

    def _say(self, text):
        if self.live:
            print(f"    · {text}", file=self.out, flush=True)

    def mark(self, kind, text):
        with self.lock:
            self._write(kind, text)

    def start(self, session):
        with self.lock:
            self.session = session
            self.counts.setdefault(session, {"status": 0, "read": 0, "rejected": 0})
            self._write("mark", f"session {session} start")

    def end(self, session, how="end"):
        with self.lock:
            self._write("mark", f"session {session} {how}")
            self.session = None

    def seen(self, session):
        with self.lock:
            return dict(self.counts.get(session, {"status": 0, "read": 0, "rejected": 0}))

    def close(self):
        with self.lock:
            if self.partial.strip():
                self._write("dev", self.partial.rstrip())
            self.file.close()


def pump(cli, recorder, stop):
    """Reader thread: copies log output into the recorder until `stop` is set."""
    while not stop.is_set():
        try:
            data = cli.port.read(0.2)
        except CliError as error:
            recorder.mark("mark", f"port error: {error}")
            return
        if data:
            recorder.feed(clean(data))


class LogPump:
    """`log` on the CLI plus the reader thread. Pausing frees the CLI for one command;
    output in the moment between stop and restart is not recorded."""

    def __init__(self, cli, recorder):
        self.cli, self.recorder = cli, recorder
        self.stop, self.thread = None, None

    def start(self):
        self.recorder.feed(self.cli.start_log())
        self.stop = threading.Event()
        self.thread = threading.Thread(target=pump, args=(self.cli, self.recorder, self.stop), daemon=True)
        self.thread.start()

    def halt(self):
        if self.thread:
            self.stop.set()
            self.thread.join(2)
            self.thread = None
            self.cli.stop_log()

    def action(self, text, out):
        """Pause the log, run a '/command', record it, resume the log."""
        self.halt()
        try:
            done = flipper_action(self.cli, text)
            if done is None:
                print(f"  Flipper commands: {ACTION_HELP}", file=out)
            else:
                self.recorder.mark("note", f"flipper: {done}")
                print(f"  Flipper: {done}", file=out)
        except CliError as error:
            self.recorder.mark("note", f"flipper: failed {text}: {error}")
            print(f"  Flipper: {error}", file=out)
        finally:
            self.start()


def split_log(text):
    """Device lines per session from a raw.log, plus notes and marks in order."""
    per_session, notes, current, marks = {}, {}, None, []
    for line in text.splitlines():
        parts = line.split(" | ", 2)
        if len(parts) != 3:
            continue
        stamp, kind, body = parts
        if kind == "mark":
            marks.append((stamp, body))
            found = re.match(r"session (\S+) (start|end|aborted|skipped)$", body)
            if found:
                current = found.group(1) if found.group(2) == "start" else None
        elif kind == "note":
            notes.setdefault(current, []).append(f"{stamp[11:19]} {body}")
        elif kind == "dev":
            per_session.setdefault(current, []).append(body)
    return per_session, notes, marks


def frame_counts(lines):
    counts = {"status": 0, "read": 0, "rejected": 0}
    for kind, _, _, _ in captures.frames("\n".join(lines)):
        counts[kind] += 1
    return counts


# --- Report -----------------------------------------------------------------------------------

def replay(paths, compiler=None):
    """Replay capture candidates through the host parser (tests/capture_test.c).

    Returns {path: (passed, output)}, or None when no C compiler is available."""
    compiler = compiler or os.environ.get("CC", "cc")
    if not paths or not shutil.which(compiler):
        return None
    with tempfile.TemporaryDirectory(prefix="bench-replay-") as temporary:
        binary = str(Path(temporary) / "capture-test")
        built = subprocess.run([compiler, "-std=c11", "bedjet_protocol.c", "tests/capture_test.c",
                                "-o", binary], cwd=ROOT, capture_output=True, text=True)
        if built.returncode:
            return None
        results = {}
        for path in paths:
            run = subprocess.run([binary, str(path)], cwd=ROOT, capture_output=True, text=True)
            results[path] = (run.returncode == 0, (run.stdout + run.stderr).strip())
        return results


def build_report(run_dir, compiler=None):
    """Write captures/, log.sanitized.txt and summary.md from a run folder. Re-runnable."""
    run_dir = Path(run_dir)
    raw = (run_dir / "raw.log").read_text(encoding="utf-8", errors="replace")
    state = json.loads((run_dir / "session.json").read_text(encoding="utf-8"))
    per_session, notes, marks = split_log(raw)
    capture_dir = run_dir / "captures"
    capture_dir.mkdir(exist_ok=True)
    written = []
    for session, lines in per_session.items():
        text, count = captures.render("\n".join(lines))
        if count:
            path = capture_dir / f"session-{session or 'between'}.cap"
            header = text.replace("# date: <YYYY-MM-DD>", f"# date: {state.get('date', '')}")
            path.write_text(header, encoding="utf-8")
            written.append(path)
    results = replay(written, compiler)

    shared = []
    for line in raw.splitlines():
        parts = line.split(" | ", 2)
        if len(parts) == 3 and (parts[1] != "dev" or "[BedJet]" in parts[2]):
            shared.append(sanitize(line))
    (run_dir / "log.sanitized.txt").write_text("\n".join(shared) + "\n", encoding="utf-8")

    out = [f"## FredJet hardware session ({state.get('date', '')})", "",
           "_Owner-run session. Results and notes are what the owner typed; frame counts come "
           "from the Flipper log. Host replay is a parser check on this computer, not a device "
           "result._", "",
           "| Item | Value |", "| --- | --- |",
           f"| App commit | `{(state.get('app') or {}).get('commit') or 'unknown'}` |",
           f"| App SHA-256 | `{state.get('fap_sha256') or 'unknown'}` |"]
    for key, value in sorted((state.get("device") or {}).items()):
        if key.startswith(("firmware.commit", "firmware.api", "firmware.version", "hardware.target",
                           "radio.stack.type", "radio.stack.major", "radio.stack.minor", "radio.stack.sub")):
            out.append(f"| {key} | {sanitize(value)} |")
    out += ["", "| Session | Result | status | read | rejected | Owner notes |",
            "| --- | --- | --- | --- | --- | --- |"]
    sessions = state.get("sessions") or {}
    order = [s for s in sessions] + [s for s in per_session if s and s not in sessions]
    for session in order:
        entry = sessions.get(session, {})
        counts = frame_counts(per_session.get(session, []))
        typed = [entry.get("notes", "")] + notes.get(session, [])
        cell = sanitize("; ".join(t for t in typed if t)).replace("|", "\\|")
        out.append(f"| {session} {entry.get('name', '')} | **{entry.get('result', 'not run')}** | "
                   f"{counts['status']} | {counts['read']} | {counts['rejected']} | {cell} |")
    if None in per_session:
        counts = frame_counts(per_session[None])
        out.append(f"| between sessions | - | {counts['status']} | {counts['read']} | "
                   f"{counts['rejected']} | |")
    out += ["", "### Capture candidates", ""]
    if not written:
        out.append("No BedJet frame lines were logged. Was the app connected while `log info` ran?")
    for path in written:
        verdict = "host replay not run (no C compiler)" if results is None else (
            "host replay: same classification" if results[path][0]
            else "host replay MISMATCH: " + results[path][1].splitlines()[0])
        out.append(f"- `captures/{path.name}`: {verdict}")
    if written:
        out += ["", "Fill in each header before copying a capture to tests/captures "
                "(docs/bringup.md section 5). A mismatch is a finding; never edit frames."]
    if state.get("aborted"):
        out += ["", f"Run stopped early: {state['aborted']}."]
    out += ["", "Private: raw.log stays on this computer. Shareable: this file, log.sanitized.txt "
            "(review it for unit names) and captures/."]
    (run_dir / "summary.md").write_text("\n".join(out) + "\n", encoding="utf-8")
    return run_dir / "summary.md", written, results


# --- Interactive run --------------------------------------------------------------------------

def yes(answer):
    return answer.strip().lower() in ("y", "yes")


def ask_line(ask, prompt):
    try:
        return ask(prompt)
    except EOFError:
        raise KeyboardInterrupt from None


def save_state(run_dir, state):
    (Path(run_dir) / "session.json").write_text(json.dumps(acceptance.sanitize_tree(state), indent=2)
                                                + "\n", encoding="utf-8")


def run_session(recorder, session, ask, out, act=None):
    print(f"\n=== Session {session['id']}. {session['name']} ===\n  Do: {session['todo']}\n"
          f"  Settles: {session['settles']}\n  The unit must never heat unattended.", file=out)
    if ask_line(ask, "  Enter to start, s to skip: ").strip().lower() == "s":
        recorder.mark("mark", f"session {session['id']} skipped")
        return {"name": session["name"], "result": "skipped", "notes": ""}
    while True:
        recorder.start(session["id"])
        print("  Recording. Type a note at any moment (each is timestamped, e.g. 'pressed fan up');\n"
              f"  type 'end' when the session is done. Flipper buttons: {ACTION_HELP}", file=out)
        while True:
            note = ask_line(ask, "  note> ").strip()
            if note.lower() in END_WORDS:
                break
            if note.startswith("/") and act:
                act(note)
            elif note:
                recorder.mark("note", note)
        recorder.end(session["id"])
        seen = recorder.seen(session["id"])
        print(f"  Logged: {seen['status']} status, {seen['read']} read, {seen['rejected']} rejected frames.",
              file=out)
        if sum(seen.values()) or not yes(ask_line(ask, "  No BedJet frames were logged. Is the app "
                                                       "connected? Redo this session? [y/N] ")):
            break
    while True:
        try:
            result = acceptance.normalize_result(ask_line(ask, "  Result p/f/s/n: "))
            break
        except ValueError as error:
            print(f"  {error}", file=out)
    notes = ask_line(ask, "  What did the unit, RF remote and Flipper show? ").strip()
    return {"name": session["name"], "result": result, "notes": notes}


def guided_run(cli, args, ask, out=None, today=None):
    out = out or sys.stdout
    lock = load_lock()
    sessions = load_sessions()
    if args.sessions:
        wanted = [s.strip().upper() for s in args.sessions.split(",")]
        unknown = [w for w in wanted if w not in {s["id"] for s in sessions}]
        if unknown:
            raise CliError(f"Unknown session(s) {unknown}; docs/bringup.md has "
                           + ", ".join(s["id"] for s in sessions))
        sessions = [s for s in sessions if s["id"] in wanted]
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = Path(args.out) / stamp
    run_dir.mkdir(parents=True, exist_ok=False)
    info = cli.info_device()
    state = {"date": today or datetime.date.today().isoformat(), "app": acceptance.app_revision(),
             "device": shareable_info(info), "sessions": {}}
    print(f"Run folder: {run_dir}\nFlipper checks:", file=out)
    checks = device_checks(info, lock)
    fap = Path(args.fap) if args.fap else DEFAULT_FAP
    checks.append(app_check(cli, fap))
    print_checks(checks, out)
    state["checks"] = [list(c) for c in checks]
    if checks[-1][0] == FAIL and fap.is_file() and yes(ask_line(ask, f"  Install {fap} now? [y/N] ")):
        state["fap_sha256"] = install_app(cli, fap, ask, out)
    elif fap.is_file():
        state["fap_sha256"] = hashlib.sha256(fap.read_bytes()).hexdigest()
    if any(c[0] == FAIL for c in checks[:-1]):
        print("  The Flipper is not on the pinned firmware; the app will not work until it is.", file=out)
        if not yes(ask_line(ask, "  Continue anyway? [y/N] ")):
            save_state(run_dir, state)
            return run_dir, False
    state["free_before"] = cli.command("free")
    save_state(run_dir, state)

    recorder = Recorder(run_dir / "raw.log", out, live=not args.quiet)
    log = LogPump(cli, recorder)
    act = lambda text: log.action(text, out)  # noqa: E731
    try:
        log.start()
        recorder.mark("mark", "log started")
        print("\nLog running. On the Flipper: Apps > Bluetooth > FredJet, OK to scan, pick your\n"
              "BedJet and wait for 'Live device status'. Close the BedJet phone app first.\n"
              f"You can also press Flipper buttons from here: {ACTION_HELP}", file=out)
        recorder.start("connect")
        while True:
            line = ask_line(ask, "  Enter once connected (notes so far are in the log): ").strip()
            if not line.startswith("/"):
                if line:
                    recorder.mark("note", line)
                break
            act(line)
        recorder.end("connect")
        for session in sessions:
            state["sessions"][session["id"]] = run_session(recorder, session, ask, out, act)
            save_state(run_dir, state)
    except KeyboardInterrupt:
        state["aborted"] = "stopped by the owner (Ctrl+C or end of input)"
        if recorder.session:
            recorder.end(recorder.session, "aborted")
    except CliError as error:
        state["aborted"] = str(error)
    finally:
        try:
            log.halt()
            state["free_after"] = cli.command("free")
        except CliError as error:
            state.setdefault("aborted", str(error))
        recorder.close()
        save_state(run_dir, state)
    summary, written, _ = build_report(run_dir)
    print(f"\nSummary: {summary}\nCapture candidates: {len(written)} in {run_dir / 'captures'}\n"
          "If the BedJet is still running, turn it off from its RF remote.", file=out)
    return run_dir, True


def prefill_acceptance(run_dir, state):
    """Seed the acceptance wizard with what the log and device already recorded."""
    device = state.get("device") or {}
    version = ".".join(device.get(f"radio.stack.{p}", "?") for p in ("major", "minor", "sub"))
    raw = (Path(run_dir) / "raw.log").read_text(encoding="utf-8", errors="replace")
    device_lines = [line for lines in split_log(raw)[0].values() for line in lines]
    frames, _ = acceptance.parse_frames("\n".join(device_lines))
    answers = {"versions": {
        "flipper_firmware": f"{device.get('firmware.version', '')} {device.get('firmware.commit.hash', device.get('firmware.commit', ''))}".strip(),
        "radio_stack": f"type {device.get('radio.stack.type', '?')} {version}"}, "frames": frames}
    folder = Path(run_dir) / "acceptance"
    folder.mkdir(exist_ok=True)
    (folder / "answers.json").write_text(json.dumps(acceptance.sanitize_tree(answers), indent=2) + "\n",
                                         encoding="utf-8")
    return folder


# --- Command line -----------------------------------------------------------------------------

def open_cli(args):
    port = resolve_port(args.port)
    cli = FlipperCli(SerialPort(port))
    cli.sync()
    return cli


def main(argv=None, ask=input, out=None, connect=open_cli):
    out = out or sys.stdout
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("ports", "doctor", "install-app", "push-update", "run", "report",
                                            "open", "close", "scan", "key", "app", "pull-log"))
    parser.add_argument("path", nargs="?", help="FAP for install-app, package folder for push-update, "
                        "run folder for report, button for key")
    parser.add_argument("--port", help="serial port (default: the one Flipper found)")
    parser.add_argument("--fap", help=f"local app build to compare and install (default {DEFAULT_FAP})")
    parser.add_argument("--out", default=str(DEFAULT_OUT), help=f"run folder root (default {DEFAULT_OUT})")
    parser.add_argument("--sessions", help="only these sessions, e.g. A,C")
    parser.add_argument("--quiet", action="store_true", help="do not echo log events while recording")
    parser.add_argument("--long", action="store_true", help="key only: hold the button (Long press)")
    parser.add_argument("--flash", action="store_true",
                        help="push-update only: also start the firmware update (asks you to type yes)")
    args = parser.parse_args(argv)
    if args.flash and args.command != "push-update":
        parser.error("--flash only applies to push-update")

    try:
        if args.command == "ports":
            ports = find_ports()
            print("\n".join(ports) if ports else "No Flipper serial port found.", file=out)
            return 0 if ports else 1
        if args.command == "report":
            if not args.path:
                parser.error("report needs a run folder")
            summary, written, _ = build_report(args.path)
            print(f"Wrote {summary} and {len(written)} capture candidates.", file=out)
            return 0
        if args.command == "key" and args.path not in KEYS:
            parser.error(f"key needs one of {', '.join(KEYS)}")
        if args.command == "push-update" and not args.path:
            parser.error("push-update needs the downloaded package folder")
        cli = connect(args)
        if args.command == "doctor":
            checks = device_checks(cli.info_device(), load_lock())
            checks.append(app_check(cli, Path(args.fap) if args.fap else DEFAULT_FAP))
            free = cli.free_kib()
            checks.append((OK if free and free > 4096 else WARN, "SD card",
                           f"{free} KiB free" if free is not None else "not found"))
            print_checks(checks, out)
            return 1 if any(c[0] == FAIL for c in checks) else 0
        if args.command == "pull-log":
            pull_logs(cli, args.out)
            return 0
        if args.command in ("open", "close", "scan", "key", "app"):
            if args.command == "open":
                open_app(cli)
                print("Opened FredJet.", file=out)
            elif args.command == "close":
                print(close_app(cli) or "Closed.", file=out)
            elif args.command == "app":
                print(running_app(cli), file=out)
            elif args.command == "scan":
                if not app_open(cli):
                    open_app(cli)
                    time.sleep(2)
                press(cli, "ok")
                print("Pressed OK on FredJet's start screen (scan).", file=out)
            else:
                press(cli, args.path, long=args.long)
                print(f"{'Held' if args.long else 'Pressed'} {args.path}.", file=out)
            return 0
        if args.command == "install-app":
            install_app(cli, Path(args.path or args.fap or DEFAULT_FAP), ask, out)
            return 0
        if args.command == "push-update":
            push_update(cli, args.path, load_lock(), args.flash, ask, out)
            return 0
        run_dir, finished = guided_run(cli, args, ask, out)
        if finished and yes(ask_line(ask, "\nWalk the issue #2 acceptance checklist now? [y/N] ")):
            state = json.loads((run_dir / "session.json").read_text(encoding="utf-8"))
            folder = prefill_acceptance(run_dir, state)
            return acceptance.main(["--resume", "--out", str(folder)], ask)
        return 0
    except CliError as error:
        print(f"Stopped: {error}", file=out)
        return 2
    except KeyboardInterrupt:
        print("\nStopped.", file=out)
        return 130


if __name__ == "__main__":
    sys.exit(main())
