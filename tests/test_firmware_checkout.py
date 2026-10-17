"""scripts/firmware_checkout.py against local repositories: no network, no firmware."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import firmware_checkout as fc  # noqa: E402

ENV = {**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t",
       "GIT_COMMITTER_EMAIL": "t@t", "GIT_AUTHOR_DATE": "2026-10-17T00:00:00Z",
       "GIT_COMMITTER_DATE": "2026-10-17T00:00:00Z"}


def git(directory, *args):
    return subprocess.run(["git", "-C", str(directory), *args], check=True, text=True,
                          capture_output=True, env=ENV).stdout.strip()


class Checkout(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="fw-checkout-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        upstream = self.tmp / "upstream"
        upstream.mkdir()
        git(upstream, "init", "--quiet")
        git(upstream, "config", "uploadpack.allowAnySHA1InWant", "true")
        (upstream / "central.c").write_text("scan general\n")
        git(upstream, "add", ".")
        git(upstream, "commit", "--quiet", "-m", "base")
        self.base = git(upstream, "rev-parse", "HEAD")
        (upstream / "central.c").write_text("scan observation\n")
        git(upstream, "commit", "--quiet", "-am", "observation scan")
        (self.tmp / "patches").mkdir()
        patch = git(upstream, "format-patch", "-1", "HEAD", "--stdout")
        (self.tmp / "patches/0001.patch").write_text(patch + "\n")
        git(upstream, "reset", "--quiet", "--hard", self.base)
        self.lock = {"repository": str(upstream), "commit": self.base, "patches": ["patches/0001.patch"]}

    def test_patched_commit_is_the_same_on_every_checkout(self):
        self.lock["patched_commit"] = "0" * 40
        with self.assertRaisesRegex(SystemExit, "Patched firmware is"):
            fc.checkout(self.tmp / "a", self.lock, root=self.tmp)
        first = git(self.tmp / "a", "rev-parse", "HEAD")
        self.lock["patched_commit"] = first
        self.assertEqual(fc.checkout(self.tmp / "b", self.lock, root=self.tmp), first)
        self.assertEqual((self.tmp / "b/central.c").read_text(), "scan observation\n")
        # Running it again over an existing checkout lands on the same commit.
        self.assertEqual(fc.checkout(self.tmp / "b", self.lock, root=self.tmp), first)

    def test_without_patches_the_base_commit_is_the_pin(self):
        self.lock["patches"] = []
        self.assertEqual(fc.checkout(self.tmp / "c", self.lock, root=self.tmp), self.base)

    def test_real_lock_names_existing_patches(self):
        lock = fc.load_lock()
        for patch in lock.get("patches", []):
            self.assertTrue((ROOT / patch).is_file(), patch)
        self.assertEqual(len(fc.expected_commit(lock)), 40)


if __name__ == "__main__":
    unittest.main()
