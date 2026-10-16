"""Non-interactive paths of the setup and acceptance wizards.

Needs no network, Flipper or BedJet: firmware "downloads" come from a local Git
repository and prompts are scripted. Run by `python3 scripts/check.py --static`.
"""

import contextlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import acceptance_wizard as acceptance  # noqa: E402
import setup_wizard as setup  # noqa: E402

FULL_ADDRESS = "C4:7C:8D:12:A3:5F"


def git(directory, *args):
    return subprocess.run(["git", *args], cwd=directory, check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout.strip()


def make_repo(directory):
    """A one-commit repository that can serve a fetch by commit hash."""
    directory.mkdir(parents=True)
    git(directory, "init", "--quiet")
    git(directory, "config", "user.email", "test@example.invalid")
    git(directory, "config", "user.name", "Test")
    git(directory, "config", "uploadpack.allowAnySHA1InWant", "true")
    (directory / "fbt").write_text("#!/bin/sh\n")
    git(directory, "add", "fbt")
    git(directory, "commit", "--quiet", "-m", "pinned")
    return git(directory, "rev-parse", "HEAD")


class Temp(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="wizard-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def lock(self, commit, repository="https://example.invalid/fw.git"):
        return {"repository": repository, "commit": commit, "target": "f7", "radio_stack": "ble_full"}


def quiet(function, *args, **kwargs):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        result = function(*args, **kwargs)
    return result, out.getvalue()


class SetupChecks(Temp):
    def test_python_and_platform_gates(self):
        self.assertEqual(setup.check_python((3, 9, 18)).status, setup.FAIL)
        self.assertEqual(setup.check_python((3, 10, 0)).status, setup.OK)
        self.assertEqual(setup.check_platform("Windows").status, setup.FAIL)
        self.assertEqual(setup.check_platform("Darwin").status, setup.OK)

    def test_missing_tools_fail_only_when_required(self):
        absent = lambda name: None  # noqa: E731
        self.assertEqual(setup.check_tool("git", True, "x", absent).status, setup.FAIL)
        self.assertEqual(setup.check_tool("cc", False, "x", absent).status, setup.WARN)

    def test_low_disk_space_warns_for_a_not_yet_created_folder(self):
        check = setup.check_disk(self.tmp / "a/b/c", free_bytes=1_000_000_000)
        self.assertEqual(check.status, setup.WARN)
        self.assertIn(str(self.tmp), check.detail)
        self.assertEqual(setup.check_disk(self.tmp, free_bytes=10**12).status, setup.OK)

    def test_lock_rejects_short_or_missing_commit(self):
        path = self.tmp / "lock.json"
        path.write_text(json.dumps(self.lock("df553f83")))
        self.assertEqual(setup.check_lock(path).status, setup.FAIL)
        path.write_text("{}")
        self.assertEqual(setup.check_lock(path).status, setup.FAIL)
        self.assertEqual(setup.check_lock().status, setup.OK)  # The real lock file.

    def test_firmware_checkout_states(self):
        commit = make_repo(self.tmp / "fw")
        lock = self.lock(commit)
        self.assertEqual(setup.inspect_firmware(self.tmp / "missing", lock).status, setup.WARN)
        (self.tmp / "plain").mkdir()
        self.assertEqual(setup.inspect_firmware(self.tmp / "plain", lock).status, setup.FAIL)
        self.assertEqual(setup.inspect_firmware(self.tmp / "fw", lock).status, setup.OK)
        wrong = setup.inspect_firmware(self.tmp / "fw", self.lock("0" * 40))
        self.assertEqual(wrong.status, setup.FAIL)
        self.assertIn("0" * 40, wrong.fix)  # Tells the user the exact commit to fetch.
        (self.tmp / "fw/fbt").write_text("modified\n")
        dirty = setup.inspect_firmware(self.tmp / "fw", lock)
        self.assertEqual(dirty.status, setup.FAIL)
        self.assertIn("local changes", dirty.detail)

    def test_fetch_checks_out_exactly_the_pinned_commit(self):
        commit = make_repo(self.tmp / "upstream")
        lock = self.lock(commit, repository=str(self.tmp / "upstream"))
        quiet(setup.fetch_firmware, self.tmp / "checkout", lock,
              runner=lambda command, check: subprocess.run(command, check=check, capture_output=True))
        self.assertEqual(git(self.tmp / "checkout", "rev-parse", "HEAD"), commit)
        self.assertEqual(setup.inspect_firmware(self.tmp / "checkout", lock).status, setup.OK)

    def test_fetch_never_touches_a_non_empty_folder(self):
        target = self.tmp / "busy"
        target.mkdir()
        (target / "mine.txt").write_text("keep")
        calls = []
        with self.assertRaises(SystemExit):
            setup.fetch_firmware(target, self.lock("a" * 40), runner=lambda *a, **k: calls.append(a))
        self.assertEqual(calls, [])
        self.assertEqual([p.name for p in target.iterdir()], ["mine.txt"])

    def test_declining_download_changes_nothing(self):
        target = self.tmp / "new-fw"
        with self.assertRaises(SystemExit) as stop:
            quiet(setup.main, ["--firmware-dir", str(target)], ask=lambda prompt: "n")
        self.assertIn("Nothing was changed", str(stop.exception))
        self.assertFalse(target.exists())

    def test_existing_unpinned_folder_stops_before_building(self):
        target = self.tmp / "other"
        target.mkdir()
        (target / "file").write_text("x")
        prompts = []
        with self.assertRaises(SystemExit):
            quiet(setup.main, ["--yes", "--firmware-dir", str(target)], ask=prompts.append)
        self.assertEqual(prompts, [])
        self.assertEqual((target / "file").read_text(), "x")

    def test_build_command_adds_package_only_when_asked(self):
        self.assertNotIn("--firmware-package", setup.build_command(self.tmp, False))
        self.assertEqual(setup.build_command(self.tmp, True)[-1], "--firmware-package")

    def test_doctor_reports_a_bad_firmware_folder(self):
        (self.tmp / "plain").mkdir()
        code, output = quiet(setup.main, ["doctor", "--firmware-dir", str(self.tmp / "plain")])
        self.assertEqual(code, 1)
        self.assertIn("not a Git checkout", output)


class VerifyDownload(Temp):
    def artifact(self, commit, fap=b"\x7fELF fake fap"):
        folder = self.tmp / "download"
        folder.mkdir(exist_ok=True)
        (folder / setup.FAP_NAME).write_bytes(fap)
        digest = setup.sha256_file(folder / setup.FAP_NAME)
        (folder / "SHA256SUMS").write_text(f"{digest}  {setup.FAP_NAME}\n")
        (folder / "provenance.json").write_text(json.dumps({
            "status": "compiled-not-hardware-validated",
            "firmware": {"commit": commit}, "fap": {"sha256": digest}}))
        return folder

    def test_matching_download_verifies(self):
        problems, result = setup.verify_download(self.artifact("a" * 40), self.lock("a" * 40))
        self.assertEqual(problems, [])
        self.assertEqual(len(result["sha256"]), 64)

    def test_replaced_app_is_rejected(self):
        folder = self.artifact("a" * 40)
        (folder / setup.FAP_NAME).write_bytes(b"something else")
        problems, _ = setup.verify_download(folder, self.lock("a" * 40))
        self.assertTrue(any("not listed in SHA256SUMS" in p for p in problems))
        self.assertTrue(any("different app hash" in p for p in problems))

    def test_other_firmware_pin_is_rejected(self):
        problems, _ = setup.verify_download(self.artifact("b" * 40), self.lock("a" * 40))
        self.assertTrue(any("different firmware commit" in p for p in problems))

    def test_missing_files_are_listed(self):
        (self.tmp / "empty").mkdir()
        problems, result = setup.verify_download(self.tmp / "empty", self.lock("a" * 40))
        self.assertEqual(len(problems), 3)
        self.assertIsNone(result)

    def test_command_exit_codes(self):
        code, output = quiet(setup.main, ["verify-download", str(self.tmp / "nowhere")])
        self.assertEqual(code, 1)
        self.assertIn("did NOT verify", output)


class Sanitizing(unittest.TestCase):
    def test_full_addresses_keep_only_the_suffix(self):
        for text in (FULL_ADDRESS, FULL_ADDRESS.replace(":", "-"), FULL_ADDRESS.replace(":", "").lower()):
            cleaned = acceptance.sanitize(f"unit {text} found")
            self.assertNotIn("C4", cleaned.upper().replace("XX", ""))
            self.assertIn("A35F", cleaned.replace(":", "").upper())

    def test_keys_and_wifi_details_are_removed(self):
        cases = ["ssid: HomeNet", "Wi-Fi password=\"hunter two\"", "psk=abc123",
                 "LTK 00112233445566778899aabbccddeeff", "bond key: deadbeef",
                 "irk=00112233445566778899AABBCCDDEEFF"]
        for case in cases:
            cleaned = acceptance.sanitize(case)
            for secret in ("HomeNet", "hunter", "abc123", "00112233", "deadbeef"):
                self.assertNotIn(secret, cleaned, case)

    def test_hashes_suffixes_and_frames_survive(self):
        commit, digest = "5392ffe6667866d5b34eaab1e90a8fa575a71be5", "ab" * 32
        frame = "00 56 20 01 02 1E 0F 32 3C 01 09 04"
        text = f"{commit} {digest} unit ..A35F {frame} pass"
        self.assertEqual(acceptance.sanitize(text), text)


class Checklist(Temp):
    def test_real_checklist_is_parsed_in_order(self):
        steps = acceptance.load_checklist()
        self.assertGreaterEqual(len(steps), 10)
        self.assertTrue(steps[0].startswith("Check the exact custom firmware"))
        self.assertNotIn("Until those checks pass", " ".join(steps))

    def doc(self, body):
        path = self.tmp / "validation.md"
        path.write_text(body)
        return path

    def test_wrapped_steps_join_and_the_list_ends_at_text(self):
        path = self.doc("# V\n\n## Device acceptance checklist\n\n1. First\n   continued.\n"
                        "2. Second.\n\nAfter the list.\n3. Not a step.\n")
        self.assertEqual(acceptance.load_checklist(path), ["First continued.", "Second."])

    def test_missing_section_or_gap_is_an_error(self):
        with self.assertRaises(SystemExit):
            acceptance.load_checklist(self.doc("# Nothing\n"))
        with self.assertRaises(SystemExit):
            acceptance.load_checklist(self.doc("## Device acceptance checklist\n\n1. A\n3. C\n"))


class Report(Temp):
    STEPS = ["Scan for the unit.", "Connect.", "Turn it off."]
    APP = {"commit": "c" * 40, "dirty": False}

    def report(self, answers):
        return acceptance.build_report(answers, self.STEPS, app=self.APP, binary=None, today="2026-10-16")

    def test_unanswered_steps_are_not_run_never_passed(self):
        report = self.report({"steps": {"1": {"result": "p"}}})
        self.assertEqual([s["result"] for s in report["steps"]], ["pass", "not run", "not run"])
        self.assertEqual(report["summary"]["pass"], 1)
        self.assertEqual(report["summary"]["not run"], 2)

    def test_unknown_step_or_result_is_rejected(self):
        with self.assertRaises(ValueError):
            self.report({"steps": {"4": {"result": "pass"}}})
        with self.assertRaises(ValueError):
            self.report({"steps": {"1": {"result": "maybe"}}})

    def test_written_report_contains_no_private_details(self):
        answers = {
            "versions": {"flipper_firmware": "df553f83", "radio_stack": "Full BLE 1.20.0"},
            "steps": {"1": {"result": "pass", "notes": f"found {FULL_ADDRESS}, ssid: HomeNet"},
                      "2": {"result": "fail", "notes": "LTK 00112233445566778899aabbccddeeff"}},
            "notes": "Wi-Fi password: hunter2",
        }
        (self.tmp / "answers.json").write_text(json.dumps(answers))
        out = self.tmp / "out"
        code, output = quiet(acceptance.main, ["--answers", str(self.tmp / "answers.json"), "--out", str(out)])
        self.assertEqual(code, 0)
        for name in ("report.md", "report.json"):
            text = (out / name).read_text()
            for secret in ("C4:7C:8D", "HomeNet", "00112233", "hunter2"):
                self.assertNotIn(secret, text, name)
        markdown = (out / "report.md").read_text()
        self.assertIn("| 1 | **pass** |", markdown)
        self.assertIn("| 2 | **fail** |", markdown)
        self.assertIn("not verified by this script", markdown)
        self.assertIn("Full BLE 1.20.0", markdown)

    def test_interactive_run_saves_progress_and_resumes(self):
        out = self.tmp / "out"
        steps = acceptance.load_checklist()
        replies = iter(["df553f83", "Full BLE 1.20.0", "BedJet 3", "",
                        "x", "p", f"seen as {FULL_ADDRESS}"])

        def ask(prompt):
            try:
                return next(replies)
            except StopIteration:
                raise EOFError from None

        with self.assertRaises(SystemExit):
            quiet(acceptance.main, ["--out", str(out)], ask=ask)
        saved = json.loads((out / "answers.json").read_text())
        self.assertEqual(saved["steps"]["1"]["result"], "pass")
        self.assertNotIn("C4:7C", saved["steps"]["1"]["notes"])
        # Resuming offers the saved values as defaults; pressing Enter keeps them.
        code, _ = quiet(acceptance.main, ["--resume", "--out", str(out)], ask=lambda prompt: "")
        self.assertEqual(code, 0)
        report = json.loads((out / "report.json").read_text())
        self.assertEqual(report["steps"][0]["result"], "pass")
        self.assertEqual(report["versions"]["bedjet_model"], "BedJet 3")
        self.assertEqual(len(report["steps"]), len(steps))
        self.assertTrue(all(s["result"] == "not run" for s in report["steps"][1:]))


LOG = """\
   4711 [I][BedJet] Connected
   4712 [I][BedJet] rejected header len=20: 00 57 20 01 02 1E 0F 32 3C 01 09 04 00 26 56 00 00 2A 00 00
   4713 [I][BedJet] status Ext heat len=18: 01 56 14 01 02 1E 0F 32 3C 03 09 04 00 26 56 00 00 2A
   4714 [I][BedJet] rejected header len=20: 00 57 20 01 02 1E 0F 32 3C 01 09 04 00 26 56 00 00 2A 00 00
   4715 [I][BedJet] read length len=11: 00 01 02 03 04 05 06 07 08 09 0A
"""


class Frames(Temp):
    def test_log_frames_keep_verdict_and_drop_duplicates(self):
        frames, problems = acceptance.parse_frames(LOG)
        self.assertEqual(problems, [])
        self.assertEqual([(f["kind"], f["app_result"], f["length"]) for f in frames],
                         [("rejected", "header", 20), ("status", "Ext heat", 18), ("read", "length", 11)])
        self.assertTrue(all(f["complete"] for f in frames))

    def test_short_or_oversized_log_lines_are_flagged(self):
        frames, problems = acceptance.parse_frames("[I][BedJet] rejected length len=20: 00 56 20\n")
        self.assertFalse(frames[0]["complete"])
        self.assertIn("len=20 but has 3 bytes", problems[0])
        self.assertIn("INCOMPLETE (3 of 20 bytes)", acceptance.fixture_c(frames))
        frames, problems = acceptance.parse_frames("[I][BedJet] rejected length len=40: 00\n")
        self.assertEqual(frames, [])
        self.assertTrue(problems)

    def test_info_page_lines_form_one_frame_per_block(self):
        text = ("00 56 20 01 02 1E 0F 32 3C 01\n09 04 00 26 56 00 00 2A 00 00\n"
                "00 00 00 00 00 00 00 00 00 00\n\n00 56 12 01 02\n")
        frames, _ = acceptance.parse_frames(text)
        self.assertEqual([f["length"] for f in frames], [30, 5])
        self.assertIsNone(frames[0]["complete"])  # Could be a cut-off 32-byte frame.
        self.assertIn("may be cut off", acceptance.fixture_c(frames))

    def test_no_frames_exits_nonzero(self):
        path = self.tmp / "none.txt"
        path.write_text("nothing here\n")
        code, _ = quiet(acceptance.main, ["frames", str(path)])
        self.assertEqual(code, 1)

    def test_candidates_compile_and_match_the_apps_verdict(self):
        compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            self.skipTest("no C compiler")
        frames, _ = acceptance.parse_frames(LOG)
        status = [f for f in frames if f["kind"] in ("rejected", "status")]
        source = self.tmp / "candidates.c"
        checks = "".join(
            f'    puts(bedjet_status_result_name(bedjet_status_parse(capture_{i}, sizeof capture_{i}, &s)));\n'
            for i in range(1, len(status) + 1))
        source.write_text('#include <stdio.h>\n#include "bedjet_protocol.h"\n'
                          + acceptance.fixture_c(status)
                          + f"int main(void) {{\n    BedjetStatus s;\n{checks}    return 0;\n}}\n")
        binary = self.tmp / "candidates"
        subprocess.run([compiler, "-std=c11", "-Wall", "-Werror", f"-I{ROOT}", str(source),
                        str(ROOT / "bedjet_protocol.c"), "-o", str(binary)], check=True)
        verdicts = subprocess.check_output([str(binary)], text=True).split()
        # The parser agrees with what the app logged: rejected -> its reason, status -> ok.
        self.assertEqual(verdicts, ["header", "ok"])


if __name__ == "__main__":
    unittest.main()
