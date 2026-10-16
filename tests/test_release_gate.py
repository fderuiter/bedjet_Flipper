"""The prerelease gate and the manual workflow that runs it.

Needs no network, Flipper or BedJet. Run by `python3 scripts/check.py --static`.
"""

import copy
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import acceptance_wizard as acceptance  # noqa: E402
import release_gate as gate  # noqa: E402

STEPS = ["Check firmware.", "Scan and identify the unit.", "Turn it off."]
TESTED = "a" * 64
COMMIT = "b" * 40


def passing_record():
    answers = {
        "versions": {"flipper_firmware": "df553f8", "radio_stack": "Full BLE 1.20.0",
                     "bedjet_model": "BedJet 3", "bedjet_firmware": "1.20"},
        "steps": {str(n): {"result": "pass"} for n in range(1, len(STEPS) + 1)},
    }
    return acceptance.build_report(answers, STEPS, app={"commit": COMMIT, "dirty": False},
                                   binary={"sha256": TESTED, "size_bytes": 30260},
                                   today="2026-10-17")


def check(record, version="0.1.0-rc.1", fap=TESTED, manifest="0.1", ancestor=lambda _: True):
    return gate.problems(version, record, STEPS, manifest, fap, ancestor=ancestor)


class GateTest(unittest.TestCase):
    def test_complete_record_for_the_built_fap_passes(self):
        self.assertEqual(check(passing_record()), [])

    def test_record_alone_passes_before_the_build(self):
        self.assertEqual(check(passing_record(), fap=None), [])

    def test_version_must_be_a_prerelease_of_the_manifest_version(self):
        for version in ("0.1.0", "v0.1.0-rc.1", "0.1.0-rc", "0.1.0-final.1", "0.1.0-rc.1\n", "0.1.0-rc.1\nx", ""):
            self.assertTrue(check(passing_record(), version=version), version)
        self.assertIn("fap_version", " ".join(check(passing_record(), version="0.2.0-rc.1")))

    def test_missing_record_is_refused(self):
        self.assertIn("no hardware record", " ".join(check(None)))

    def test_any_step_short_of_pass_is_refused(self):
        for result in ("fail", "skipped", "not run"):
            record = passing_record()
            record["steps"][1]["result"] = result
            self.assertEqual(check(record), [f"step 2 is {result!r}, not 'pass'"])

    def test_record_from_a_different_checklist_is_refused(self):
        record = passing_record()
        record["steps"].pop()
        self.assertIn("current checklist", " ".join(check(record)))
        record = passing_record()
        record["steps"][0]["check"] = "Something else."
        self.assertIn("current checklist", " ".join(check(record)))

    def test_different_bytes_from_the_tested_fap_are_refused(self):
        found = check(passing_record(), fap="c" * 64)
        self.assertEqual(len(found), 1)
        self.assertIn("not the hardware-tested FAP", found[0])
        record = passing_record()
        record["app_binary"] = None
        self.assertIn("tested FAP's SHA-256", " ".join(check(record)))

    def test_tested_source_must_be_clean_and_in_history(self):
        for app in ({"commit": COMMIT, "dirty": True}, {"commit": COMMIT, "dirty": None},
                    {"commit": None, "dirty": False}):
            record = passing_record()
            record["app"] = app
            self.assertIn("clean tested source commit", " ".join(check(record)), app)
        self.assertIn("not an ancestor", " ".join(check(passing_record(), ancestor=lambda _: False)))

    def test_device_versions_are_required(self):
        record = passing_record()
        record["versions"]["radio_stack"] = " "
        self.assertEqual(check(record), ["record does not name radio_stack"])

    def test_unsanitized_record_is_refused(self):
        record = passing_record()
        record["notes"] = "Unit C4:7C:8D:12:A3:5F paired"
        self.assertIn("address, key or Wi-Fi", " ".join(check(record)))

    def test_other_report_kinds_are_refused(self):
        record = passing_record()
        record["kind"] = "something-else"
        self.assertIn("acceptance_wizard report", " ".join(check(record)))

    def test_current_checklist_and_manifest_are_readable(self):
        self.assertTrue(acceptance.load_checklist())
        self.assertRegex(gate.fap_version() or "", r"^\d+\.\d+$")

    def test_notes_name_both_commits_and_the_hash(self):
        record = passing_record()
        provenance = {"source": {"commit": "d" * 40}, "fap": {"sha256": TESTED, "size_bytes": 30260},
                      "firmware": {"repository": "r", "commit": "e" * 40, "api_version": "88.3",
                                   "central_h_sha256": "f" * 64},
                      "toolchain": {"fbt_toolchain_version": "39", "gcc": "gcc 12.3.1"}}
        lock = json.loads((ROOT / "firmware.lock.json").read_text())
        notes = gate.render_notes("0.1.0-rc.1", record, provenance, lock)
        for text in ("prerelease", "d" * 40, COMMIT, TESTED, "3 of 3", "Full BLE 1.20.0"):
            self.assertIn(text, notes)


class WorkflowTest(unittest.TestCase):
    """Only a person can start a prerelease, and it can only make a draft."""

    def setUp(self):
        try:
            import yaml
        except ImportError:
            self.skipTest("PyYAML unavailable")
        text = (ROOT / ".github/workflows/prerelease.yml").read_text(encoding="utf-8")
        self.text = text
        self.workflow = yaml.safe_load(text)

    def test_manual_dispatch_is_the_only_trigger(self):
        # PyYAML reads the bare key `on` as True.
        self.assertEqual(list(self.workflow[True]), ["workflow_dispatch"])

    def test_write_access_only_in_the_draft_job(self):
        self.assertEqual(self.workflow["permissions"], {"contents": "read"})
        writers = {name for name, job in self.workflow["jobs"].items()
                   if (job.get("permissions") or {}).get("contents") == "write"}
        self.assertEqual(writers, {"draft"})

    def test_release_is_created_as_a_draft_prerelease_and_never_published(self):
        commands = [step.get("run", "") for job in self.workflow["jobs"].values()
                    for step in job["steps"]]
        creates = [c for c in commands if "gh release create" in c]
        self.assertEqual(len(creates), 1)
        self.assertIn("--draft", creates[0])
        self.assertIn("--prerelease", creates[0])
        for forbidden in ("gh release edit", "--draft=false", "git push", "git tag", "flash"):
            self.assertNotIn(forbidden, "\n".join(commands), forbidden)

    def test_actions_are_pinned_by_commit(self):
        for job in self.workflow["jobs"].values():
            for step in job["steps"]:
                if "uses" in step:
                    self.assertRegex(step["uses"], r"@[0-9a-f]{40}$")


if __name__ == "__main__":
    unittest.main()
