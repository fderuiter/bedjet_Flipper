#!/usr/bin/env python3
"""Turn a Flipper CLI `log` capture into a sanitized BedJet capture fixture.

Only the app's frame lines ("[BedJet] status|read|rejected <detail> len=N: hex")
are kept. Everything else in the log, including unit names, Bluetooth
addresses and other apps' messages, is dropped. Consecutive duplicate frames
are collapsed. The output header must be completed by hand before committing.
"""

import argparse
import pathlib
import re
import sys

FRAME = re.compile(
    r"\[BedJet\]\s+(status|read|rejected)\s+(.+?)\s+len=(\d+):\s*((?:[0-9A-Fa-f]{2}\s*)*)")
ANSI = re.compile(r"\x1b\[[0-9;]*m")
HEADER = """\
# BedJet capture (sanitized by scripts/captures.py; review before committing)
# condition: <what the unit was doing, e.g. off after RF Off, Heat 30 C fan 50%>
# bedjet: <model, firmware version from the BedJet app>
# flipper: <hardware target, firmware commit, radio stack and version>
# app: <bedjet_remote source commit and FAP SHA-256>
# date: <YYYY-MM-DD>
# kind | detail | length | bytes
"""


def frames(text):
    previous = None
    for line in text.splitlines():
        match = FRAME.search(ANSI.sub("", line))
        if not match:
            continue
        kind, detail, length, raw = match.groups()
        frame = (kind, detail, int(length), " ".join(raw.upper().split()))
        if frame != previous:
            yield frame
        previous = frame


def render(text):
    lines = [f"{k} | {d} | {n} | {b}" for k, d, n, b in frames(text)]
    return HEADER + "".join(line + "\n" for line in lines), len(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=pathlib.Path, help="Saved Flipper CLI log output")
    parser.add_argument("-o", "--output", type=pathlib.Path,
                        help="Fixture to write (default: standard output)")
    args = parser.parse_args()
    output, count = render(args.log.read_text(encoding="utf-8", errors="replace"))
    if not count:
        raise SystemExit("No BedJet frame lines found. Was `log` running at Info level?")
    if args.output:
        if args.output.exists():
            raise SystemExit(f"{args.output} exists; refusing to overwrite a capture")
        args.output.write_text(output, encoding="utf-8")
    else:
        sys.stdout.write(output)
    print(f"{count} frames", file=sys.stderr)


if __name__ == "__main__":
    main()
