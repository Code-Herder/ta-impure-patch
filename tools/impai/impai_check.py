#!/usr/bin/env python3
"""impai_check -- ImpAI's observation, judged from a game's log lines.

    tools/impai/impai_check.py logs --los true|permanent <log> [<log> ...]

reads the `impai:` lines the DLL writes at each level's end (tagpu/ddraw/src/tagpu_impai.c,
impai_observe_end) from the logs given -- tagpu.N.log files of one game, in any order -- and
fails unless:

  (a) the self-checks ran and agreed: every `impai: check ...` line says mismatch=0. Under True
      line of sight (`--los true`) every observing seat's `check los seat=N` line evaluated at
      least LOS_MIN units and the `check radar` line counts at least RADAR_PASSES passes; under
      Permanent (`--los permanent`) the `check los-local` line evaluated at least LOS_MIN, and
      the radar line at least RADAR_PASSES passes as well. A missing line fails: the lever
      tagpu_impai_check.on was not in place, or the level never ended;
  (c) every `impai: work` line is within its bounds, which the line states beside each count
      (`slots_max`, `visits_max`, `cells_max` and `flood_max`, each N/BOUND), and its `refused` count
      is 0.

The overlay's digests (b) and the cost lines (d) are not written yet; this judges what is.
Exit 0 when every check passes, 1 when one fails, 2 when the logs cannot be read.
"""
import argparse
import re
import sys
from pathlib import Path

LOS_MIN = 10000
RADAR_PASSES = 50

CHECK = re.compile(r"impai: check (los seat=(\d+)|los-local|radar passes=(\d+)) .*?evaluated=(\d+) mismatch=(\d+)")
MISMATCH = re.compile(r"impai: check (los|radar) mismatch .*")
WORK = re.compile(r"impai: work seat=(\d+) ticks=(\d+) refused=(\d+) slots_max=(\d+)/(\d+) "
                  r"visits_max=(\d+)/(\d+) cells_max=(\d+)/(\d+) flood_max=(\d+)/(\d+)")
BOUNDED = ("slots_max", "visits_max", "cells_max", "flood_max")


def judge(lines, los):
    fails, notes = [], []
    los_seats, local, radar = {}, None, None
    for ln in lines:
        m = CHECK.search(ln)
        if m:
            what, seat, passes, ev, miss = m.group(1), m.group(2), m.group(3), int(m.group(4)), int(m.group(5))
            if miss:
                fails.append(f"(a) {ln.strip()}")
            if seat is not None:
                los_seats[int(seat)] = los_seats.get(int(seat), 0) + ev
            elif what == "los-local":
                local = (local or 0) + ev
            else:
                radar = (radar or 0) + int(passes)
            notes.append(ln.strip())
            continue
        m = MISMATCH.search(ln)
        if m:
            notes.append(ln.strip())
            continue
        m = WORK.search(ln)
        if m:
            seat, refused = int(m.group(1)), int(m.group(3))
            if refused:
                fails.append(f"(c) seat {seat}: refused={refused}")
            for k, name in enumerate(BOUNDED):
                n, bound = int(m.group(4 + 2 * k)), int(m.group(5 + 2 * k))
                if n > bound:
                    fails.append(f"(c) seat {seat}: {name} {n} over its bound {bound}")
            notes.append(ln.strip())
    if radar is None:
        fails.append("(a) no `impai: check radar` line: the check lever was not in place")
    elif radar < RADAR_PASSES:
        fails.append(f"(a) {radar} radar passes checked, under {RADAR_PASSES}")
    if los == "true":
        if not los_seats:
            fails.append("(a) no `impai: check los seat=` line")
        for seat, ev in sorted(los_seats.items()):
            if ev < LOS_MIN:
                fails.append(f"(a) seat {seat}: {ev} line-of-sight comparisons, under {LOS_MIN}")
    else:
        if local is None:
            fails.append("(a) no `impai: check los-local` line")
        elif local < LOS_MIN:
            fails.append(f"(a) {local} local line-of-sight comparisons, under {LOS_MIN}")
    if not any(WORK.search(ln) for ln in lines):
        fails.append("(c) no `impai: work` line: no ImpAI seat observed")
    return fails, notes


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    lg = sub.add_parser("logs", help="judge one game's logs")
    lg.add_argument("--los", choices=("true", "permanent"), required=True,
                    help="the fixture's line-of-sight mode")
    lg.add_argument("logs", nargs="+", type=Path)
    args = ap.parse_args()
    lines = []
    for p in args.logs:
        try:
            lines += p.read_text(errors="replace").splitlines()
        except OSError as e:
            print(f"impai_check: {p}: {e}", file=sys.stderr)
            return 2
    lines = [ln for ln in lines if "impai: " in ln]
    fails, notes = judge(lines, args.los)
    for n in notes:
        print(n)
    for f in fails:
        print(f"FAIL {f}")
    print("impai_check: " + ("PASS" if not fails else f"{len(fails)} failure(s)"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
