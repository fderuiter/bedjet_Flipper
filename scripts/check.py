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
            "view": ["bedjet_protocol.c", "bedjet_controller.c", "bedjet_view.c",
                     "tests/view_test.c"],
        }
        for name, sources in suites.items():
            binary = str(Path(temporary) / f"{name}-test")
            subprocess.run([compiler, *flags, "-pthread", *sources, "-o", binary],
                           cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True)
        check_capture_tool()
        # Real device captures only; synthetic frames belong in protocol_test.c.
        captures = [str(p.relative_to(ROOT)) for p in tracked("tests/captures/*.cap")]
        binary = str(Path(temporary) / "capture-test")
        subprocess.run([compiler, *flags, "bedjet_protocol.c", "tests/capture_test.c",
                        "-o", binary], cwd=ROOT, check=True)
        check_capture_replay(binary, Path(temporary))
        if captures:
            subprocess.run([binary, *captures], cwd=ROOT, check=True)
        else:
            print("SKIP: no hardware captures in tests/captures yet")


def check_capture_replay(binary, temporary):
    """The replayer must reject a capture the parser would now classify differently."""
    # Synthetic Heat frame from protocol_test.c; a harness check, not a capture.
    heat = "00 56 20 01 02 1E 0F 32 3C 01 09 04 00 26 56 00 00 2A" + " 00" * 14
    bad_mode = heat.replace("3C 01 09", "3C 09 09", 1)
    cases = {
        f"status | Heat | 32 | {heat}": 0,
        f"rejected | mode | 32 | {bad_mode}": 0,
        f"read | length | 3 | 01 02 03": 0,
        f"rejected | length | 40 | {heat}": 0,
        f"status | Cool | 32 | {heat}": 1,
        f"rejected | header | 32 | {heat}": 1,
        f"status | Off | 32 | {bad_mode}": 1,
        f"read | ok | 4 | 01 02 03": 1,
        f"rejected | length | 40 | 01 02": 1,
        f"status | Heat | 32 | {heat} ZZ": 1,
        "status | Heat | 32": 1,
    }
    for line, expected in cases.items():
        fixture = temporary / "case.cap"
        fixture.write_text("# harness case\n" + line + "\n", encoding="utf-8")
        result = subprocess.run([binary, str(fixture)], capture_output=True)
        if (result.returncode != 0) != bool(expected):
            raise SystemExit(f"capture replay misjudged: {line}")
    print(f"PASS: capture replay harness, {len(cases)} cases")


def check_capture_tool():
    """The extractor keeps only frame lines, so names and addresses never reach a fixture."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("captures", ROOT / "scripts/captures.py")
    captures = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(captures)
    log = ("812 \x1b[32m[I][BedJet] \x1b[0mConnected BEDJET_KITCHEN AA:BB:CC:DD:EE:FF\r\n"
           "900 [I][BedJet] read ok len=3: 01 02 03 \r\n"
           "901 [I][BedJet] read ok len=3: 01 02 03 \r\n"
           "950 [I][BtSrv] peer AA:BB:CC:DD:EE:FF\n"
           "990 [I][BedJet] status Ext heat len=2: 0a ff\n"
           "991 [I][BedJet] rejected header len=40: " + "00 " * 32 + "\n")
    text, count = captures.render(log)
    body = [line for line in text.splitlines() if not line.startswith("#")]
    assert count == 3 and body == [
        "read | ok | 3 | 01 02 03", "status | Ext heat | 2 | 0A FF",
        "rejected | header | 40 | " + " ".join(["00"] * 32)], body
    assert "AA:BB" not in text and "KITCHEN" not in text


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
