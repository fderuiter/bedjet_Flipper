"""The hardware session runner against a scripted Flipper CLI on a pseudo-terminal.

Needs no Flipper or BedJet. The fake speaks the pinned firmware's CLI as read
from its sources: echo, `>: ` prompt, `storage write_chunk` with `Ready`,
append-only writes, `storage md5`, and `log` until Ctrl+C. Run by
`python3 scripts/check.py --static`.
"""

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import threading
import time
import tty
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import hardware_session as hs  # noqa: E402

LOCK = hs.load_lock()
FULL_ADDRESS = "C4:7C:8D:12:A3:5F"
HEAT = "00 56 20 01 02 1E 0F 32 3C 01 09 04 00 26 56 00 00 2A" + " 00" * 14
INFO = {
    "firmware.commit": LOCK["commit"][:8], "firmware.version": "ble-central",
    "firmware.api.major": "88", "firmware.api.minor": "3", "hardware.target": "7",
    "hardware.name": "Kitchen", "hardware.uid": "0123456789ABCDEF",
    "radio.alive": "true", "radio.mode": "Stack", "radio.stack.type": "1",
    "radio.stack.major": "1", "radio.stack.minor": "20", "radio.stack.sub": "0",
    "radio.ble.mac": "80E127AABBCC",
}


class FakeFlipper(threading.Thread):
    def __init__(self, info=None, corrupt_uploads=False):
        super().__init__(daemon=True)
        self.master, self.slave = os.openpty()
        tty.setraw(self.slave)
        self.path = os.ttyname(self.slave)
        self.info = dict(INFO if info is None else info)
        self.files, self.dirs = {}, {"/ext", "/ext/apps"}
        self.corrupt_uploads = corrupt_uploads
        self.commands, self.log_lines = [], []
        self.mode, self.need, self.target = "line", 0, None
        self.line, self.running = b"", True
        self.logging = threading.Event()

    def send(self, text):
        os.write(self.master, text.encode() if isinstance(text, str) else text)

    def emit(self, *lines):
        """Log lines as the firmware prints them while `log` runs."""
        for line in lines:
            self.send(f"{line}\r\n")

    def run(self):
        while self.running:
            try:
                data = os.read(self.master, 4096)
            except OSError:
                return
            for byte in data:
                self.byte(bytes([byte]))

    def byte(self, b):
        if self.mode == "chunk":
            self.files[self.target] = self.files.get(self.target, b"") + b
            self.need -= 1
            if not self.need:
                if self.corrupt_uploads:
                    self.files[self.target] = b"X" + self.files[self.target][1:]
                self.mode = "line"
                self.send("\r\n>: ")
            return
        if self.mode == "log":
            if b == b"\x03":
                self.mode = "line"
                self.logging.clear()
                self.send("\r\n>: ")
            return
        if b == b"\x03":
            self.line = b""
            self.send("\r\n>: ")
        elif b == b"\r":
            self.send("\r\n")
            line, self.line = self.line.decode(), b""
            if line:
                self.commands.append(line)
            self.execute(line)
        else:
            self.line += b
            self.send(b)

    def execute(self, line):
        words = line.split()
        out = ""
        if words[:2] == ["info", "device"]:
            out = "".join(f"{k:<30}: {v}\r\n" for k, v in self.info.items())
        elif words[:1] == ["free"]:
            out = "Free heap size: 120000\r\n"
        elif words[:2] == ["storage", "stat"]:
            path = words[2]
            if path == "/ext":
                out = "Storage, 30000000KiB total, 29000000KiB free\r\n"
            elif path in self.dirs:
                out = "Directory\r\n"
            elif path in self.files:
                out = f"File, size: {len(self.files[path])}b\r\n"
            else:
                out = "Storage error: file/dir not exist\r\n"
        elif words[:2] == ["storage", "md5"]:
            data = self.files.get(words[2])
            out = (hashlib.md5(data).hexdigest() if data is not None
                   else "Storage error: file/dir not exist") + "\r\n"
        elif words[:2] == ["storage", "mkdir"]:
            self.dirs.add(words[2])
        elif words[:2] == ["storage", "remove"]:
            self.files.pop(words[2], None)
        elif words[:2] == ["storage", "rename"]:
            if words[3] in self.files:
                out = "Storage error: file/dir already exist\r\n"
            else:
                self.files[words[3]] = self.files.pop(words[2])
        elif words[:2] == ["storage", "write_chunk"]:
            self.target, self.need = words[2], int(words[3])
            if self.target.rsplit("/", 1)[0] not in self.dirs:
                out = "Storage error: file/dir not exist\r\n"
            else:
                self.send("Ready\r\n")
                if self.need:
                    self.mode = "chunk"
                    return
        elif words[:1] == ["log"]:
            self.mode = "log"
            self.send("Current log level: info\r\nUse <log ?> to list available log levels\r\n"
                      "Press CTRL+C to stop...\r\n")
            self.logging.set()
            return
        elif words[:2] == ["update", "install"]:
            out = f"Verifying update package at '{words[2]}'\r\nOK.\r\nRestarting to apply update. BRB\r\n"
        elif words:
            out = f"`{words[0]}` command not found\r\n"
        self.send(out + "\r\n>: ")

    def stop(self):
        self.running = False
        os.close(self.slave)
        os.close(self.master)


class Bench(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="bench-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def flipper(self, **kwargs):
        fake = FakeFlipper(**kwargs)
        fake.start()
        cli = hs.FlipperCli(hs.SerialPort(fake.path))
        cli.sync()
        self.addCleanup(fake.stop)
        self.addCleanup(cli.port.close)
        return fake, cli

    def fap(self, data=b"\x7fELF fredjet" * 1500, sums=True):
        path = self.tmp / "fredjet.fap"
        path.write_bytes(data)
        if sums:
            (self.tmp / "SHA256SUMS").write_text(f"{hashlib.sha256(data).hexdigest()}  fredjet.fap\n")
        return path


def scripted(*answers):
    """An `ask` that returns each answer in turn; callables run first (to emit log lines)."""
    queue = list(answers)

    def ask(prompt):
        ask.prompts.append(prompt)
        if not queue:
            raise EOFError
        item = queue.pop(0)
        return item() if callable(item) else item
    ask.prompts = []
    return ask


def quiet(function, *args, **kwargs):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        result = function(*args, **kwargs)
    return result, out.getvalue()


class Cli(Bench):
    def test_command_output_drops_echo_and_prompt(self):
        _, cli = self.flipper()
        self.assertEqual(cli.command("free"), "Free heap size: 120000")
        self.assertEqual(cli.stat("/ext/apps"), "dir")
        self.assertEqual(cli.stat("/ext/nothing"), "missing")
        self.assertIsNone(cli.md5("/ext/nothing"))

    def test_device_info_keeps_no_names_ids_or_mac(self):
        _, cli = self.flipper()
        info = cli.info_device()
        self.assertEqual(info["radio.ble.mac"], "80E127AABBCC")
        shared = hs.shareable_info(info)
        self.assertNotIn("hardware.name", shared)
        self.assertNotIn("hardware.uid", shared)
        self.assertNotIn("radio.ble.mac", shared)
        self.assertEqual(shared["radio.stack.minor"], "20")

    def test_device_checks_against_the_lock(self):
        self.assertTrue(all(c[0] == hs.OK for c in hs.device_checks(INFO, LOCK)))
        stock = dict(INFO, **{"firmware.commit": "a1b2c3d4", "radio.stack.type": "3"})
        checks = {name: status for status, name, _ in hs.device_checks(stock, LOCK)}
        self.assertEqual(checks["Firmware"], hs.FAIL)
        self.assertEqual(checks["Radio stack"], hs.WARN)
        dead = hs.device_checks(dict(INFO, **{"radio.alive": "false"}), LOCK)
        self.assertEqual(dead[-1][:2], (hs.FAIL, "Radio"))

    def test_port_selection(self):
        self.assertEqual(hs.resolve_port("/dev/x"), "/dev/x")
        with self.assertRaises(hs.CliError):
            hs.resolve_port(None, lambda: [])
        with self.assertRaises(hs.CliError):
            hs.resolve_port(None, lambda: ["/dev/ttyACM0", "/dev/ttyACM1"])
        self.assertEqual(hs.resolve_port(None, lambda: ["/dev/cu.usbmodemflip_A1", "/dev/ttyACM0"]),
                         "/dev/cu.usbmodemflip_A1")

    def test_missing_port_is_explained(self):
        with self.assertRaisesRegex(hs.CliError, "Cannot open"):
            hs.SerialPort(str(self.tmp / "no-such-port"))


class InstallApp(Bench):
    def test_upload_is_verified_and_lands_at_the_app_path(self):
        fake, cli = self.flipper()
        fap = self.fap()
        _, out = quiet(hs.install_app, cli, fap, scripted())
        self.assertEqual(fake.files[hs.APP_PATH], fap.read_bytes())
        self.assertNotIn(hs.APP_PATH + ".part", fake.files)
        self.assertIn("Installed", out)
        self.assertGreater(sum(c.startswith("storage write_chunk") for c in fake.commands), 1)

    def test_identical_app_is_not_copied_again(self):
        fake, cli = self.flipper()
        fap = self.fap()
        fake.dirs.add(hs.APP_DIR)
        fake.files[hs.APP_PATH] = fap.read_bytes()
        _, out = quiet(hs.install_app, cli, fap, scripted())
        self.assertIn("already matches", out)
        self.assertFalse(any(c.startswith("storage write_chunk") for c in fake.commands))

    def test_different_app_is_kept_unless_the_owner_agrees(self):
        fake, cli = self.flipper()
        fake.dirs.add(hs.APP_DIR)
        fake.files[hs.APP_PATH] = b"older build"
        with self.assertRaisesRegex(hs.CliError, "unchanged"):
            quiet(hs.install_app, cli, self.fap(), scripted("n"))
        self.assertEqual(fake.files[hs.APP_PATH], b"older build")
        quiet(hs.install_app, cli, self.fap(), scripted("y"))
        self.assertEqual(fake.files[hs.APP_PATH], self.fap().read_bytes())

    def test_corrupted_upload_never_replaces_the_installed_app(self):
        fake, cli = self.flipper(corrupt_uploads=True)
        fake.dirs.add(hs.APP_DIR)
        fake.files[hs.APP_PATH] = b"older build"
        with self.assertRaisesRegex(hs.CliError, "MD5"):
            quiet(hs.install_app, cli, self.fap(), scripted("y"))
        self.assertEqual(fake.files[hs.APP_PATH], b"older build")
        self.assertNotIn(hs.APP_PATH + ".part", fake.files)

    def test_fap_that_does_not_match_its_hash_list_is_refused(self):
        fake, cli = self.flipper()
        fap = self.fap()
        fap.write_bytes(b"tampered")
        with self.assertRaisesRegex(hs.CliError, "does not match"):
            hs.install_app(cli, fap, scripted())
        self.assertFalse(any(c.startswith("storage write_chunk") for c in fake.commands))
        self.assertNotIn(hs.APP_PATH, fake.files)

    def test_app_check_states(self):
        fake, cli = self.flipper()
        fap = self.fap()
        self.assertEqual(hs.app_check(cli, fap)[0], hs.FAIL)
        fake.dirs.add(hs.APP_DIR)
        fake.files[hs.APP_PATH] = fap.read_bytes()
        self.assertEqual(hs.app_check(cli, fap)[0], hs.OK)
        self.assertEqual(hs.app_check(cli, self.tmp / "absent.fap")[0], hs.WARN)


class PushUpdate(Bench):
    def package(self, address="00 E0 0C 08", radio="radio.bin"):
        folder = self.tmp / "artifact/update/f7-update-local"
        folder.mkdir(parents=True)
        (folder / "update.fuf").write_text(
            "Filetype: Flipper firmware upgrade configuration\nVersion: 2\nTarget: 7\n"
            f"Loader: firmware.dfu\nRadio: {radio}\nRadio address: {address}\n")
        (folder / "firmware.dfu").write_bytes(b"D" * 9000)
        (folder / "radio.bin").write_bytes(b"R" * 5000)
        sums = "".join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  update/f7-update-local/{p.name}\n"
                       for p in sorted(folder.iterdir()))
        (self.tmp / "artifact/UPDATE_SHA256SUMS").write_text(sums)
        return self.tmp / "artifact"

    def test_copy_only_by_default(self):
        fake, cli = self.flipper()
        _, out = quiet(hs.push_update, cli, self.package(), LOCK, False, scripted())
        self.assertEqual(fake.files["/ext/update/f7-update-local/radio.bin"], b"R" * 5000)
        self.assertFalse(any(c.startswith("update") for c in fake.commands))
        self.assertIn("Nothing was installed", out)

    def test_flash_needs_the_flag_and_a_typed_yes(self):
        fake, cli = self.flipper()
        package = self.package()
        for answer in ("y", "YES ", "", "yes please"):
            result, _ = quiet(hs.push_update, cli, package, LOCK, True, scripted(answer))
            self.assertFalse(result, answer)
        self.assertFalse(any(c.startswith("update") for c in fake.commands))
        result, _ = quiet(hs.push_update, cli, package, LOCK, True, scripted("yes"))
        self.assertTrue(result)
        self.assertEqual([c for c in fake.commands if c.startswith("update")],
                         ["update install /ext/update/f7-update-local/update.fuf"])

    def test_wrong_radio_or_damaged_file_is_rejected_before_copying(self):
        fake, cli = self.flipper()
        with self.assertRaisesRegex(hs.CliError, "Radio address"):
            hs.push_update(cli, self.package(address="00 70 0D 08"), LOCK, True, scripted("yes"))
        shutil.rmtree(self.tmp / "artifact")
        package = self.package()
        (package / "update/f7-update-local/radio.bin").write_bytes(b"light stack")
        with self.assertRaisesRegex(hs.CliError, "radio.bin does not match"):
            hs.push_update(cli, package, LOCK, True, scripted("yes"))
        self.assertFalse(fake.files)
        self.assertFalse(any(c.startswith("update") for c in fake.commands))

    def test_flash_flag_is_refused_on_other_commands(self):
        with self.assertRaises(SystemExit):
            quiet(hs.main, ["run", "--flash"], connect=lambda a: self.fail("connected"))


class Sessions(unittest.TestCase):
    def test_sessions_come_from_the_runbook_table(self):
        sessions = hs.load_sessions()
        self.assertEqual([s["id"] for s in sessions], list("ABCDEFG"))
        self.assertEqual(sessions[0]["name"], "Off")
        self.assertIn("RF remote", sessions[0]["todo"])

    def test_missing_table_is_an_error(self):
        with tempfile.NamedTemporaryFile("w", suffix=".md") as empty:
            empty.write("# nothing\n")
            empty.flush()
            with self.assertRaises(SystemExit):
                hs.load_sessions(empty.name)


class GuidedRun(Bench):
    def args(self, **kwargs):
        values = {"out": str(self.tmp / "runs"), "fap": None, "sessions": "A,C", "quiet": True}
        values.update(kwargs)
        return type("Args", (), values)()

    def wait_for(self, recorder_lines, fake):
        def emit():
            fake.emit(*recorder_lines)
            time.sleep(0.5)
            return "end"
        return emit

    def test_full_run_records_marks_frames_and_a_clean_report(self):
        fake, cli = self.flipper()
        fap = self.fap()
        fake.dirs.add(hs.APP_DIR)
        fake.files[hs.APP_PATH] = fap.read_bytes()
        session_a = [f"1200 [I][BedJet] Connected BEDJET {FULL_ADDRESS}",
                     "1300 [I][BedJet] read length len=3: 01 02 03",
                     f"1400 [I][BedJet] status Heat len=32: {HEAT}",
                     "1500 [I][BtSrv] peer Kitchen"]
        session_c = ["2000 [I][BedJet] rejected mode len=32: " + HEAT.replace("3C 01 09", "3C 09 09", 1)]
        ask = scripted("",                                   # connected
                       "", "unit was off", self.wait_for(session_a, fake), "p", "Wi-Fi password: hunter2",
                       "", self.wait_for(session_c, fake), "f", "fan step not shown")
        (run_dir, finished), out = quiet(hs.guided_run, cli, self.args(fap=str(fap)), ask,
                                         today="2026-10-16")
        self.assertTrue(finished)
        self.assertFalse(fake.logging.is_set(), "log must be stopped with Ctrl+C")
        raw = (run_dir / "raw.log").read_text()
        self.assertIn("| mark | session A start", raw)
        self.assertIn("| note | unit was off", raw)
        self.assertIn(FULL_ADDRESS, raw)  # raw.log is the private copy

        summary = (run_dir / "summary.md").read_text()
        self.assertIn("| A Off | **pass** | 1 | 1 | 0 |", summary)
        self.assertIn("| C Steps | **fail** | 0 | 0 | 1 |", summary)
        self.assertIn("host replay: same classification", summary)
        cap_a = (run_dir / "captures/session-A.cap").read_text()
        self.assertIn(f"status | Heat | 32 | {HEAT}", cap_a)
        self.assertIn("# date: 2026-10-16", cap_a)
        shared = (run_dir / "log.sanitized.txt").read_text() + summary + (run_dir / "session.json").read_text()
        for private in (FULL_ADDRESS, "hunter2", "Kitchen", "0123456789ABCDEF", "80E127AABBCC"):
            self.assertNotIn(private, shared)
        self.assertNotIn("write_chunk", " ".join(fake.commands), "matching app must not be re-sent")

    def test_rejected_capture_mismatch_is_reported(self):
        run_dir = self.tmp / "run"
        run_dir.mkdir()
        (run_dir / "session.json").write_text(json.dumps({"date": "2026-10-16", "sessions": {
            "B": {"name": "Each mode", "result": "pass", "notes": ""}}}))
        bad = HEAT.replace("3C 01 09", "3C 09 09", 1)
        (run_dir / "raw.log").write_text(
            "2026-10-16T22:00:00.000 | mark | session B start\n"
            f"2026-10-16T22:00:01.000 | dev | 10 [I][BedJet] status Heat len=32: {bad}\n"
            "2026-10-16T22:00:02.000 | mark | session B end\n")
        (summary, written, results), _ = quiet(hs.build_report, run_dir)
        self.assertEqual([p.name for p in written], ["session-B.cap"])
        self.assertIn("MISMATCH", summary.read_text())

    def test_session_without_frames_offers_a_redo(self):
        fake, cli = self.flipper()
        ask = scripted("", "", "end", "y", "end", "n", "s", "")
        (run_dir, _), out = quiet(hs.guided_run, cli, self.args(sessions="A"), ask)
        self.assertEqual(sum("No BedJet frames were logged" in p for p in ask.prompts), 2)
        raw = (run_dir / "raw.log").read_text()
        self.assertEqual(raw.count("session A start"), 2)
        self.assertIn("No BedJet frame lines were logged", (run_dir / "summary.md").read_text())

    def test_interrupted_run_still_stops_the_log_and_writes_a_report(self):
        fake, cli = self.flipper()
        ask = scripted("", "", "pressed heat")  # input ends mid-session
        (run_dir, _), _ = quiet(hs.guided_run, cli, self.args(sessions="B"), ask)
        self.assertFalse(fake.logging.is_set())
        state = json.loads((run_dir / "session.json").read_text())
        self.assertIn("stopped by the owner", state["aborted"])
        self.assertIn("session B aborted", (run_dir / "raw.log").read_text())
        self.assertIn("Run stopped early", (run_dir / "summary.md").read_text())

    def test_unpinned_firmware_stops_before_logging_unless_confirmed(self):
        fake, cli = self.flipper(info=dict(INFO, **{"firmware.commit": "a1b2c3d4"}))
        (run_dir, finished), out = quiet(hs.guided_run, cli, self.args(), scripted("n"))
        self.assertFalse(finished)
        self.assertNotIn("log info", fake.commands)

    def test_unknown_session_is_refused(self):
        _, cli = self.flipper()
        with self.assertRaisesRegex(hs.CliError, "Unknown session"):
            hs.guided_run(cli, self.args(sessions="Z"), scripted())

    def test_acceptance_prefill_carries_versions_and_frames(self):
        run_dir = self.tmp / "run"
        run_dir.mkdir()
        (run_dir / "raw.log").write_text(
            f"2026-10-16T22:00:01.000 | dev | 10 [I][BedJet] status Heat len=32: {HEAT}\n")
        folder = hs.prefill_acceptance(run_dir, {"device": hs.shareable_info(INFO)})
        answers = json.loads((folder / "answers.json").read_text())
        self.assertEqual(answers["versions"]["radio_stack"], "type 1 1.20.0")
        self.assertEqual(answers["frames"], [f"status | Heat | 32 | {HEAT}"])


if __name__ == "__main__":
    unittest.main()
