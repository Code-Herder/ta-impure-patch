#!/usr/bin/env python3
"""cobverify_check.py -- whether the COB oracle armed and found the core equal to the engine on every
entry call it compared so far, read from the game's log.

    tools/replay/cobverify_check.py GAMEDIR

THE ORACLE (`tagpu_cobverify.on`, tagpu_patches.c THE ORACLE): the engine's interpreter runs the game
and the core replays each entry call on a copy of the object, answered from what the engine's
interpreter asked; a difference is logged at once as `cobverify: DIFFERENT ...` (the first forty),
and a summary line `cobverify: tick T: N calls, E equal, D different, F not compared ...` every 300
ticks. A fixture step runs this before the level ends.

THE CHECK. The log says `cobverify: ARMED`, carries no DIFFERENT line, and its last summary has
compared calls, none different and none left uncompared because its log overflowed. Exit 0 pass,
1 fail, 2 the log could not be read.
"""

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import talog  # noqa: E402

SUMMARY = re.compile(r"cobverify: tick (\d+): (\d+) calls, (\d+) equal, (\d+) different, (\d+) not compared")


def check(text):
    """(ok, report) for one game's log text"""
    lines = [ln for ln in text.splitlines() if "cobverify:" in ln]
    if not any("cobverify: ARMED" in ln for ln in lines):
        why = next((ln for ln in lines if "NOT armed" in ln), "no cobverify line")
        return False, f"the oracle did not arm: {why.strip()}"
    bad = [ln.strip() for ln in lines if "cobverify: DIFFERENT" in ln]
    if bad:
        return False, f"{len(bad)} calls differ; the first: {bad[0]}"
    sums = [SUMMARY.search(ln) for ln in lines]
    sums = [m for m in sums if m]
    if not sums:
        return False, "no summary line yet: the level has not passed tick 300"
    tick, calls, equal, different, full = (int(g) for g in sums[-1].groups())
    if different or full or not equal:
        return False, f"at tick {tick}: {calls} calls, {equal} equal, {different} different, {full} not compared"
    return True, f"at tick {tick}: {calls} entry calls, every one equal"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("gamedir", type=Path, help="the instance's game folder")
    args = ap.parse_args()
    try:
        text = talog.run_text(args.gamedir, "tagpu")
    except OSError as e:
        print(f"cobverify_check: the log could not be read: {e}", file=sys.stderr)
        return 2
    ok, report = check(text)
    print(f"cobverify_check: {'PASS' if ok else 'FAIL'} {report}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
