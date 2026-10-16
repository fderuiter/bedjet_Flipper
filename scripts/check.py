#!/usr/bin/env python3
"""Run portable protocol tests and optional C formatting checks."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitizers", action="store_true")
    parser.add_argument("--format", action="store_true")
    parser.add_argument("--clang-format", default="clang-format")
    args = parser.parse_args()
    # CC is the compiler executable, not an arbitrary shell command.
    compiler = os.environ.get("CC", "cc")
    if not shutil.which(compiler):
        raise SystemExit(f"C compiler not found: {compiler}")
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-g"]
    if args.sanitizers:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    with tempfile.TemporaryDirectory(prefix="bedjet-tests-") as temporary:
        binary = str(Path(temporary) / "protocol-test")
        subprocess.run([compiler, *flags, "bedjet_protocol.c", "tests/protocol_test.c",
                        "-o", binary], cwd=ROOT, check=True)
        subprocess.run([binary], cwd=ROOT, check=True)
    if args.format:
        if not shutil.which(args.clang_format):
            raise SystemExit(f"Formatter not found: {args.clang_format}")
        subprocess.run([args.clang_format, "--dry-run", "--Werror",
                        "bedjet_remote.c", "bedjet_protocol.c", "bedjet_protocol.h",
                        "tests/protocol_test.c"], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
