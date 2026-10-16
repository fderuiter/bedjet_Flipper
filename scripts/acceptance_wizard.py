#!/usr/bin/env python3
"""Walk the device acceptance checklist and write a sanitized report for issue #2.

The checklist is read from docs/validation.md, so the wizard and the document
cannot drift apart. Every result is what the tester typed; the wizard verifies
nothing on the device itself and never marks a step as passed on its own.

  python3 scripts/acceptance_wizard.py                     # step by step
  python3 scripts/acceptance_wizard.py --resume            # continue a saved run
  python3 scripts/acceptance_wizard.py --answers run.json  # no questions; write the report
  python3 scripts/acceptance_wizard.py frames capture.txt  # log/Info hex -> capture lines

Reports go to dist/acceptance/ (ignored by Git). Full Bluetooth addresses,
pairing keys/bonds and Wi-Fi details are removed from everything written.
"""

import argparse
import datetime
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
VALIDATION = ROOT / "docs/validation.md"
DEFAULT_OUT = ROOT / "dist/acceptance"
CHECKLIST_HEADING = "## Device acceptance checklist"
RESULTS = {"p": "pass", "f": "fail", "s": "skipped", "n": "not run"}
VERSION_FIELDS = (
    ("flipper_firmware", "Flipper firmware (commit or version shown in Settings > About)"),
    ("radio_stack", "Radio stack (type and version, e.g. Full BLE 1.20.0)"),
    ("bedjet_model", "BedJet model (e.g. BedJet 3)"),
    ("bedjet_firmware", "BedJet firmware version, if known"),
)
# The app shows 3 lines of 10 bytes per Info page; status frames can be 32 bytes.
INFO_PAGE_BYTES = 30
MAX_FRAME = 32

# --- Sanitizing -----------------------------------------------------------------

_SEP = r"[:\-]"
MAC = re.compile(rf"\b([0-9A-Fa-f]{{2}}){_SEP}([0-9A-Fa-f]{{2}}){_SEP}([0-9A-Fa-f]{{2}}){_SEP}"
                 rf"([0-9A-Fa-f]{{2}}){_SEP}([0-9A-Fa-f]{{2}}){_SEP}([0-9A-Fa-f]{{2}})\b")
MAC_PLAIN = re.compile(r"(?<![0-9A-Fa-f])[0-9A-Fa-f]{12}(?![0-9A-Fa-f])")
SECRET_WORDS = (r"ssid|wi-?fi|wlan|psk|password|passphrase|passkey|ltk|irk|csrk|"
                r"bond(?:ing)?(?:\s*key)?|link\s*key|pairing\s*key|secret|token")
# One or more keywords ("Wi-Fi password"), a separator, then the value.
SECRET_VALUE = re.compile(rf"(?i)((?:\b(?:{SECRET_WORDS})\b[\s-]*)+)(\s*(?:[:=]|is)\s*|\s+)"
                          rf"(\"[^\"]*\"|'[^']*'|\S+)")
# 128-bit keys (LTK, IRK, CSRK) as 32 hex digits. Commit (40) and SHA-256 (64)
# hashes are deliberately left alone.
KEY_HEX = re.compile(r"(?<![0-9A-Fa-f])[0-9A-Fa-f]{32}(?![0-9A-Fa-f])")


def sanitize(text):
    """Remove full Bluetooth addresses, key material and Wi-Fi details from free text.

    Address suffixes (the last two bytes the app shows) are kept so a unit can
    still be identified; status frames are short spaced hex and are untouched."""
    if not text:
        return text
    text = MAC.sub(lambda m: f"XX:XX:XX:XX:{m.group(5).upper()}:{m.group(6).upper()}", text)
    text = SECRET_VALUE.sub(lambda m: f"{m.group(1)}{m.group(2)}[redacted]", text)
    text = KEY_HEX.sub("[redacted key-like hex]", text)
    return MAC_PLAIN.sub(lambda m: "XXXXXXXX" + m.group(0)[-4:].upper(), text)


def sanitize_tree(value):
    if isinstance(value, str):
        return sanitize(value)
    if isinstance(value, list):
        return [sanitize_tree(v) for v in value]
    if isinstance(value, dict):
        return {k: sanitize_tree(v) for k, v in value.items()}
    return value


# --- Checklist ----------------------------------------------------------------------

def load_checklist(path=VALIDATION):
    """Numbered steps under the checklist heading, joined across wrapped lines."""
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    try:
        start = lines.index(CHECKLIST_HEADING) + 1
    except ValueError:
        raise SystemExit(f"{path} has no '{CHECKLIST_HEADING}' section") from None
    steps = []
    for line in lines[start:]:
        if line.startswith("#"):
            break
        item = re.match(r"^(\d+)\.\s+(.*)$", line)
        if item:
            if int(item.group(1)) != len(steps) + 1:
                raise SystemExit(f"{path}: checklist numbering jumps at step {item.group(1)}")
            steps.append(item.group(2).strip())
        elif steps and line.startswith("   ") and line.strip():
            steps[-1] += " " + line.strip()
        elif steps and line.strip():
            break  # The paragraph after the list.
    if not steps:
        raise SystemExit(f"{path}: no numbered checklist steps found")
    return steps


# --- Report ---------------------------------------------------------------------------

def app_revision():
    try:
        commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True,
                                         stderr=subprocess.DEVNULL).strip()
        dirty = bool(subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"],
                                             cwd=ROOT, text=True, stderr=subprocess.DEVNULL).strip())
    except (OSError, subprocess.CalledProcessError):
        return {"commit": None, "dirty": None}
    return {"commit": commit, "dirty": dirty}


def app_binary(dist=ROOT / "dist"):
    fap = Path(dist) / "fredjet.fap"
    if not fap.is_file():
        return None
    return {"sha256": hashlib.sha256(fap.read_bytes()).hexdigest(), "size_bytes": fap.stat().st_size}


def normalize_result(value):
    value = (value or "not run").strip().lower()
    if value in RESULTS:
        return RESULTS[value]
    if value in RESULTS.values():
        return value
    raise ValueError(f"result must be one of {', '.join(RESULTS.values())}; got {value!r}")


def build_report(answers, steps, app=None, binary=None, today=None):
    """Combine the tester's answers with the checklist; sanitize everything the tester typed."""
    answers = sanitize_tree(answers)
    given = {int(k): v for k, v in (answers.get("steps") or {}).items()}
    unknown = sorted(set(given) - set(range(1, len(steps) + 1)))
    if unknown:
        raise ValueError(f"answers name steps that are not in the checklist: {unknown}")
    report = {
        "kind": "bedjet-acceptance-report",
        "date": today or datetime.date.today().isoformat(),
        "evidence": "Tester-reported hardware results; not verified by this script.",
        "app": app if app is not None else app_revision(),
        "app_binary": binary if binary is not None else app_binary(),
        "versions": {key: (answers.get("versions") or {}).get(key, "") for key, _ in VERSION_FIELDS},
        "steps": [],
        "frames": answers.get("frames") or [],
        "notes": answers.get("notes", ""),
    }
    for number, text in enumerate(steps, 1):
        entry = given.get(number) or {}
        report["steps"].append({"number": number, "check": text,
                                "result": normalize_result(entry.get("result")),
                                "notes": entry.get("notes", "")})
    counts = {name: sum(s["result"] == name for s in report["steps"]) for name in RESULTS.values()}
    report["summary"] = counts
    return report


def render_markdown(report):
    app = report["app"] or {}
    binary = report.get("app_binary")
    out = [f"## BedJet acceptance report ({report['date']})", "",
           f"_{report['evidence']}_", "",
           "| Item | Value |", "| --- | --- |",
           f"| App commit | `{app.get('commit') or 'unknown'}`{' (modified)' if app.get('dirty') else ''} |",
           f"| App binary SHA-256 | `{binary['sha256']}` |" if binary else "| App binary | not found in dist/ |"]
    for key, label in VERSION_FIELDS:
        out.append(f"| {label.split(' (')[0]} | {report['versions'].get(key) or 'not recorded'} |")
    summary = report["summary"]
    out += ["", "Summary: " + ", ".join(f"{summary[k]} {k}" for k in RESULTS.values()), "",
            "| # | Result | Check | Notes |", "| --- | --- | --- | --- |"]
    for step in report["steps"]:
        check = step["check"] if len(step["check"]) <= 90 else step["check"][:87] + "..."
        notes = step["notes"].replace("|", "\\|").replace("\n", " ")
        out.append(f"| {step['number']} | **{step['result']}** | {check} | {notes} |")
    if report["frames"]:
        out += ["", "### Frame captures", "",
                "Unreviewed; see docs/bringup.md before committing them under tests/captures.", ""]
        out += ["```text", render_capture(report["frames"]).rstrip(), "```"]
    if report["notes"]:
        out += ["", "### Notes", "", report["notes"]]
    return "\n".join(out) + "\n"


def write_report(report, out_dir):
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (out_dir / "report.md").write_text(render_markdown(report), encoding="utf-8")
    return out_dir / "report.md"


# --- Frames -> capture candidates ---------------------------------------------------------

def _load_captures():
    """scripts/captures.py owns the log-to-fixture format that check.py replays."""
    spec = importlib.util.spec_from_file_location("captures", Path(__file__).with_name("captures.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


captures = _load_captures()
HEX_LINE = re.compile(r"^\s*(?:[0-9A-Fa-f]{2}\s+)*[0-9A-Fa-f]{2}\s*$")


def parse_frames(text):
    """Split a capture into fixture lines and Info-page hex.

    Flipper CLI `log` frame lines go through scripts/captures.py unchanged, so
    the output is the `tests/captures/*.cap` format the parser replay checks;
    every other log line (names, addresses) is dropped there. Consecutive bare
    hex lines typed from an Info page form one block. They carry no verdict,
    so they become comments: only `log` output can become a fixture."""
    lines = [f"{k} | {d} | {n} | {b}" for k, d, n, b in captures.frames(text)]
    problems = [f"'{line}': length {n} but {len(b.split())} bytes logged"
                for line, (_, _, n, b) in zip(lines, captures.frames(text))
                if n <= MAX_FRAME and n != len(b.split())]
    info, pending = [], []

    def flush():
        if pending:
            data = " ".join(" ".join(pending).upper().split())
            note = (f"; may be cut off at the Info page's {INFO_PAGE_BYTES}-byte display"
                    if len(data.split()) == INFO_PAGE_BYTES else "")
            info.append(f"# info-page, {len(data.split())} bytes, no verdict{note}: {data}")
            pending.clear()

    for number, line in enumerate(text.splitlines(), 1):
        if HEX_LINE.match(line):
            pending.append(line.strip())
            if sum(len(p.split()) for p in pending) > MAX_FRAME:
                problems.append(f"line {number}: more than {MAX_FRAME} hex bytes without a break")
                pending.clear()
        else:
            flush()
    flush()
    return lines + info, problems


def render_capture(frames):
    """A capture file: captures.py's header to complete by hand, then the frames."""
    return captures.HEADER + "".join(line + "\n" for line in frames)


# --- Interactive run ------------------------------------------------------------------------

def ask_text(prompt, ask, default=""):
    try:
        value = ask(f"{prompt}{f' [{default}]' if default else ''}: ").strip()
    except EOFError:
        raise SystemExit("\nInput ended; progress so far is saved.") from None
    return value or default


def interactive(steps, answers, out_dir, ask=input):
    print("BedJet acceptance checklist. Type p (pass), f (fail), s (skip) or n (not run),\n"
          "then any notes. Do not type full Bluetooth addresses, pairing keys or Wi-Fi\n"
          "details; anything that looks like one is removed from the report anyway.\n")
    versions = answers.setdefault("versions", {})
    for key, label in VERSION_FIELDS:
        versions[key] = ask_text(label, ask, versions.get(key, ""))
    recorded = answers.setdefault("steps", {})
    for number, text in enumerate(steps, 1):
        previous = recorded.get(str(number), {})
        print(f"\nStep {number} of {len(steps)}:\n  {text}")
        while True:
            choice = ask_text("Result p/f/s/n", ask, previous.get("result", ""))
            try:
                result = normalize_result(choice)
                break
            except ValueError as error:
                print(error)
        notes = ask_text("Notes", ask, previous.get("notes", ""))
        recorded[str(number)] = {"result": result, "notes": notes}
        save_progress(answers, out_dir)
    answers["notes"] = ask_text("\nAnything else to report", ask, answers.get("notes", ""))
    capture = ask_text("Path to a log or Info-page capture to convert (optional)", ask, "")
    if capture:
        frames, problems = parse_frames(Path(capture).expanduser().read_text(encoding="utf-8"))
        for problem in problems:
            print(f"  note: {problem}")
        answers["frames"] = frames
    save_progress(answers, out_dir)
    return answers


def save_progress(answers, out_dir):
    Path(out_dir).mkdir(parents=True, exist_ok=True)
    (Path(out_dir) / "answers.json").write_text(json.dumps(sanitize_tree(answers), indent=2) + "\n",
                                                encoding="utf-8")


def main(argv=None, ask=input):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", nargs="?", default="run", choices=("run", "frames", "steps"))
    parser.add_argument("path", nargs="?", help="capture file for 'frames' ('-' reads stdin)")
    parser.add_argument("--answers", help="JSON answers file; runs without questions")
    parser.add_argument("--resume", action="store_true", help="continue from OUT/answers.json")
    parser.add_argument("--out", default=str(DEFAULT_OUT), help=f"report folder (default {DEFAULT_OUT})")
    args = parser.parse_args(argv)
    steps = load_checklist()

    if args.command == "steps":
        for number, text in enumerate(steps, 1):
            print(f"{number}. {text}")
        return 0
    if args.command == "frames":
        if not args.path:
            parser.error("frames needs a capture file, or '-' for stdin")
        text = sys.stdin.read() if args.path == "-" else Path(args.path).read_text(encoding="utf-8")
        frames, problems = parse_frames(text)
        for problem in problems:
            print(f"note: {problem}", file=sys.stderr)
        if not frames:
            print("No hex frames found.", file=sys.stderr)
            return 1
        sys.stdout.write(render_capture(frames))
        return 0

    out_dir = Path(args.out)
    if args.answers:
        answers = json.loads(Path(args.answers).read_text(encoding="utf-8"))
    else:
        saved = out_dir / "answers.json"
        answers = json.loads(saved.read_text(encoding="utf-8")) if args.resume and saved.is_file() else {}
        answers = interactive(steps, answers, out_dir, ask)
    try:
        report = build_report(answers, steps)
    except ValueError as error:
        raise SystemExit(str(error)) from None
    path = write_report(report, out_dir)
    print(f"\nReport written to {path} and {path.with_suffix('.json')}.")
    print("Review it, then paste report.md into https://github.com/fderuiter/flipper_fredjet/issues/2")
    return 0


if __name__ == "__main__":
    sys.exit(main())
