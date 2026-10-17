#!/usr/bin/env python3
"""Check out the pinned firmware: the upstream base commit plus FredJet's patches.

  python3 scripts/firmware_checkout.py ../flipper-firmware

Fetches `repository` at `commit` from firmware.lock.json, applies each file in
`patches` with `git am` under a fixed committer and the patch's own date, so
every machine gets the same commit, and checks it equals `patched_commit`.
Never builds, flashes or pushes.
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMMITTER = {"GIT_COMMITTER_NAME": "FredJet build", "GIT_COMMITTER_EMAIL": "fredjet-build@invalid"}


def load_lock(path=ROOT / "firmware.lock.json"):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def expected_commit(lock):
    """The commit a checkout must end at: patched when there are patches."""
    return lock.get("patched_commit") or lock["commit"]


def git(directory, *args, env=None):
    return subprocess.run(["git", "-C", str(directory), *args], check=True, text=True,
                          capture_output=True, env={**os.environ, **(env or {})}).stdout.strip()


def checkout(directory, lock, root=ROOT):
    directory = Path(directory)
    if not (directory / ".git").exists():
        directory.mkdir(parents=True, exist_ok=True)
        git(directory, "init", "--quiet")
    git(directory, "fetch", "--quiet", "--depth", "1", lock["repository"], lock["commit"])
    git(directory, "-c", "advice.detachedHead=false", "checkout", "--quiet", "--force", "--detach", "FETCH_HEAD")
    if git(directory, "rev-parse", "HEAD") != lock["commit"]:
        raise SystemExit(f"Fetched commit is not {lock['commit']}.")
    for patch in lock.get("patches", []):
        git(directory, "-c", "user.name=FredJet build", "-c", "user.email=fredjet-build@invalid",
            "am", "--quiet", "--committer-date-is-author-date", str(root / patch), env=COMMITTER)
    head = git(directory, "rev-parse", "HEAD")
    if head != expected_commit(lock):
        raise SystemExit(f"Patched firmware is {head}, expected {expected_commit(lock)}. "
                         "Review the patches before updating the lock.")
    return head


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args(argv)
    print(checkout(args.directory, load_lock()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
