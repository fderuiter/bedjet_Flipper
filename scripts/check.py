#!/usr/bin/env python3
"""Run portable protocol/lifecycle tests and optional formatting/static checks."""

import argparse
import ast
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FORMAT_MAJOR = "18"
# Byte-exact historical source and raw working notes are not reformatted/linted.
EXCLUDED = ("artifacts/", "notes/", "dist/", "build/")
LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")


def tracked(pattern):
    return sorted(p for p in ROOT.glob(pattern)
                  if not str(p.relative_to(ROOT)).startswith(EXCLUDED))


def run_tests(args):
    # CC is the compiler executable, not an arbitrary shell command.
    compiler = os.environ.get("CC", "cc")
    if not shutil.which(compiler):
        raise SystemExit(f"C compiler not found: {compiler}")
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-g"]
    if args.short_enums:
        flags += ["-fshort-enums"]
    if args.sanitizers:
        # Make every UBSan report fatal; by default it only prints and continues.
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer"]
    with tempfile.TemporaryDirectory(prefix="bedjet-tests-") as temporary:
        suites = {
            "protocol": ["bedjet_protocol.c", "tests/protocol_test.c"],
            "controller": ["bedjet_protocol.c", "bedjet_controller.c", "tests/controller_test.c"],
        }
        for name, sources in suites.items():
            binary = str(Path(temporary) / f"{name}-test")
            subprocess.run([compiler, *flags, "-pthread", *sources, "-o", binary],
                           cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True)


def check_format(formatter):
    if not shutil.which(formatter):
        raise SystemExit(f"Formatter not found: {formatter}")
    version = subprocess.check_output([formatter, "--version"], text=True)
    if not re.search(rf"version {FORMAT_MAJOR}\.", version):
        raise SystemExit(f"clang-format {FORMAT_MAJOR} is required; got: {version.strip()}")
    files = [*tracked("*.c"), *tracked("*.h"), *tracked("tests/*.c")]
    subprocess.run([formatter, "--dry-run", "--Werror", *map(str, files)], cwd=ROOT, check=True)


def check_static():
    for path in [*tracked("*.py"), *tracked("scripts/*.py")]:
        ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    for path in [*tracked("*.json"), *tracked("artifacts/prototype/*.json")]:
        json.loads(path.read_text(encoding="utf-8"))
    workflows = tracked(".github/**/*.yml")
    try:
        import yaml
    except ImportError:
        print(f"SKIP: PyYAML unavailable; {len(workflows)} YAML files not parsed")
    else:
        for path in workflows:
            yaml.safe_load(path.read_text(encoding="utf-8"))
    broken = []
    documents = [*tracked("*.md"), *tracked("docs/*.md"), *tracked(".catalog/*.md"),
                 *tracked(".github/*.md")]
    for document in documents:
        for target in LINK.findall(document.read_text(encoding="utf-8")):
            if re.match(r"^[a-z]+:", target) or target.startswith("#"):
                continue
            if not (document.parent / target.split("#", 1)[0]).exists():
                broken.append(f"{document.relative_to(ROOT)} -> {target}")
    if broken:
        raise SystemExit("Broken local documentation links:\n  " + "\n  ".join(broken))
    print(f"PASS: static checks over {len(documents)} documents")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitizers", action="store_true")
    parser.add_argument("--short-enums", action="store_true",
                        help="Also exercise the short-enum ABI used by the ARM build")
    parser.add_argument("--format", action="store_true",
                        help=f"Check C formatting; requires clang-format {FORMAT_MAJOR}")
    parser.add_argument("--clang-format", default="clang-format")
    parser.add_argument("--static", action="store_true",
                        help="Parse scripts/JSON/workflows and check local Markdown links")
    args = parser.parse_args()
    if args.static:
        check_static()
    if args.format:
        check_format(args.clang_format)
    if args.static or args.format:
        if not (args.sanitizers or args.short_enums):
            return
    run_tests(args)


if __name__ == "__main__":
    main()
