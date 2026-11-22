#!/usr/bin/env python3
"""particlecheck_check.py -- whether the particle oracle armed and found every live particle of the
event classes equal to its event's closed form, read from the game's log.

    tools/replay/particlecheck_check.py GAMEDIR

THE ORACLE (`tagpu_particlecheck.on`, tagpu_patches.c THE PARTICLES AS EVENTS; research/notes/
effects.md §7.8): on a Vulkan launch the engine's nano, fire, flare, wake and smoke objects are
made in full beside their events and held to them after every particle walk. The DLL's install line
says `tagpu_particlecheck.on: the engine's objects are made in full`, and each checked tick logs
`pcheck: T=N objects O (missing M extra E trimmed R) particles P differing D [...] nano a/b fire c/d
flare e/f wake g/h smoke2 i/j smoke1 k/l level: ..., wake dots dead over ground w`, a/b the
particles compared and differing in each class, w the level's wake dots whose death walk is the
ground's, not their end tick's. A fixture step runs this before the level ends.

It passes when the oracle armed, no tick had a differing particle, a missing or an extra object,
and the run compared particles of every class the fixture names (`--classes`, default nano and fire)
-- an oracle that compared nothing proves nothing -- and, when the classes include the wake, some
wake dot died over ground, so the ground's branch of the death walk was held to the engine. Exit 0 pass, 1 fail, 2 the log could not be read.
"""

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import talog  # noqa: E402

TICK = re.compile(r"pcheck: T=(\d+) objects \d+ \(missing (\d+) extra (\d+) trimmed \d+\) particles \d+ "
                  r"differing (\d+) .*?nano (\d+)/(\d+) fire (\d+)/(\d+) flare (\d+)/(\d+) "
                  r"wake (\d+)/(\d+) smoke2 (\d+)/(\d+) smoke1 (\d+)/(\d+)")
GROUND = re.compile(r"wake dots dead over ground (\d+)")
ARMED = "tagpu_particlecheck.on: the engine's objects are made in full"
CLASSES = ("nano", "fire", "flare", "wake", "smoke2", "smoke1")


def check(text, want):
    """(ok, report) for one game's log text"""
    if ARMED not in text:
        return False, "the oracle did not arm (no install line naming tagpu_particlecheck.on)"
    ticks = 0
    compared = dict.fromkeys(CLASSES, 0)
    ground = 0
    for ln in text.splitlines():
        m = TICK.search(ln)
        if not m:
            continue
        g = [int(x) for x in m.groups()]
        t, missing, extra, differing = g[:4]
        if missing or extra or differing or any(g[5 + 2 * i] for i in range(len(CLASSES))):
            return False, ln.strip()
        ticks += 1
        gm = GROUND.search(ln)
        if gm:
            ground = max(ground, int(gm.group(1)))
        for i, c in enumerate(CLASSES):
            compared[c] += g[4 + 2 * i]
    if not ticks:
        return False, "no pcheck line: the oracle checked no tick"
    none = [c for c in want if not compared[c]]
    if none:
        return False, f"{ticks} ticks checked, but no particle of {', '.join(none)} was compared"
    if "wake" in want and not ground:
        return False, f"{ticks} ticks checked, but no wake dot died over ground"
    return True, (f"{ticks} ticks, " + ", ".join(f"{c} {compared[c]}" for c in CLASSES)
                  + " particles compared, 0 differing, 0 missing or extra"
                  + (f", {ground} wake dots dead over ground" if "wake" in want else ""))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("gamedir", type=Path, help="the instance's game folder")
    ap.add_argument("--classes", default="nano,fire",
                    help="the classes the game must have compared, comma-separated")
    args = ap.parse_args()
    want = [c for c in args.classes.split(",") if c]
    bad = [c for c in want if c not in CLASSES]
    if bad:
        print(f"particlecheck_check: unknown class {bad[0]}", file=sys.stderr)
        return 2
    try:
        text = talog.run_text(args.gamedir, "tagpu")
    except OSError as e:
        print(f"particlecheck_check: the log could not be read: {e}", file=sys.stderr)
        return 2
    ok, report = check(text, want)
    print(f"particlecheck_check: {'PASS' if ok else 'FAIL'} {report}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
