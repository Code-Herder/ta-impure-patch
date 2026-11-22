#!/usr/bin/env python3
"""fpverify_check.py -- whether the exact FPU paths armed their check and found every result equal to
the engine's, read from the game's log.

    tools/replay/fpverify_check.py GAMEDIR

THE CHECK (`tagpu_fpverify.on`, tagpu_patches.c THE PIECE ROTATION FROM A TABLE and HYPOT WITH ONE
MODE SWITCH EACH WAY): each call runs the engine's code and the replacement and compares the results;
the first difference is logged at once as `fprot: DIFFER` / `fphyp: DIFFER`, and a summary line
`fprot: N calls, Z of angle 0, T on the table, D differ` / `fphyp: N calls, F fast, D differ` at every power of two
from 65 536 calls. A fixture step runs this before the level ends.

It passes when both installed under the lever, no DIFFER line was logged, and each path's last summary
has calls taken by the replacement and none different -- a replacement that never ran proves nothing.
Exit 0 pass, 1 fail, 2 the log could not be read.
"""

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import talog  # noqa: E402

SUMMARY = {"fprot": re.compile(r"fprot: (\d+) calls, \d+ of angle 0, (\d+) on the table, (\d+) differ"),
           "fphyp": re.compile(r"fphyp: (\d+) calls, (\d+) fast, (\d+) differ")}


def check(text):
    """(ok, report) for one game's log text"""
    reports = []
    for tag, pat in SUMMARY.items():
        lines = [ln for ln in text.splitlines() if f"{tag}:" in ln]
        if not any("installed" in ln and "tagpu_fpverify.on" in ln for ln in lines):
            why = next((ln.strip() for ln in lines if "NOT installed" in ln), f"no {tag} line under the lever")
            return False, f"{tag} did not install its check: {why}"
        bad = [ln.strip() for ln in lines if f"{tag}: DIFFER" in ln]
        if bad:
            return False, bad[0]
        sums = [m for m in (pat.search(ln) for ln in lines) if m]
        if not sums:
            return False, f"{tag}: no summary yet: fewer than 65 536 calls"
        calls, taken, differ = (int(g) for g in sums[-1].groups())
        if differ or not taken:
            return False, f"{tag}: {calls} calls, {taken} by the replacement, {differ} differ"
        reports.append(f"{tag} {calls} calls, {taken} by the replacement, every one equal")
    return True, "; ".join(reports)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("gamedir", type=Path, help="the instance's game folder")
    args = ap.parse_args()
    try:
        text = talog.run_text(args.gamedir, "tagpu")
    except OSError as e:
        print(f"fpverify_check: the log could not be read: {e}", file=sys.stderr)
        return 2
    ok, report = check(text)
    print(f"fpverify_check: {'PASS' if ok else 'FAIL'} {report}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
