#!/usr/bin/env python3
"""killall_check.py -- whether a commander's death killed the rest of its side by the tick after,
read tick-exactly from the level's recording.

    tools/replay/killall_check.py FILE --owner P [--count N] [--json]

THE RULE. A unit dies at its own turn of the unit tick 0x48AD30, which walks each player record's
block in slot order: a hit that leaves it pending (+0x110 bit 14) is destroyed by the tick's reaper
at that turn (0x48AFB9..0x48AFD1, engine/recorder.md, the 0x48AD30 row). A commander's destroy
calls the kill-all 0x486F10 for its player (0x4866B3); for a LOCAL player (type 1 or 2 with a
DirectPlay id) that hits each of the player's live units with 30 000 of kind 3, the unit as its
own attacker, leaving it pending (engine/units.md, *Kill-all for a player*). So with the commander
dead in the step from GameTime T, a unit of the player in a later slot has its turn, and dies,
in that same walk, the step from T; one in an earlier slot dies in the next walk, the step from
T + 1. The recording's RK_EV_DEATH carries the step (tarec.py, deaths). [INFERRED from the two
disassembled facts; this check is what measures it.]

THE CHECK. The commander is player P's first death by another player's unit: the apply's clear
kills with no attacker, so the level's start units it removes are not it, and commander-death.json's
tower kills the commander before anything else of P's dies. Every later death of P's units must
fall in the step its slot gives; any other step is a FAIL, named with the unit and both steps. `--count N` also requires exactly N such deaths, so a
kill-all that did not fire is a failure rather than nothing to check. Exit 0 pass, 1 fail, 2 the
recording could not be read or no other player killed one of P's units.
"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tarec  # noqa: E402


def check(rows, owner, count=None):
    """(ok, report): the deaths of `owner` after the commander's against the step their slot gives."""
    own = [r for r in rows if r["owner"] == owner]
    first = next((k for k, r in enumerate(own)
                  if r["killer_player"] is not None and r["killer_player"] != owner), None)
    if first is None:
        return None, {"owner": owner,
                      "error": f"no unit of player {owner} was killed by another player's"}
    cmd, rest = own[first], own[first + 1:]
    lines, ok = [], True
    for r in rest:
        want = cmd["gametime"] + (0 if r["slot"] > cmd["slot"] else 1)
        good = r["gametime"] == want
        ok &= good
        lines.append(dict(r, expected=want, ok=good))
    if count is not None and len(rest) != count:
        ok = False
    return ok, {"owner": owner, "commander": cmd, "deaths": lines, "count": len(rest),
                "count_expected": count, "ok": ok}


def main(argv=None):
    ap = argparse.ArgumentParser(prog="killall_check.py", description=__doc__.split("\n\n")[0])
    ap.add_argument("file", help="the level's recording (.imprec)")
    ap.add_argument("--owner", type=int, required=True, help="the player whose commander dies")
    ap.add_argument("--count", type=int, help="the deaths the kill-all must make")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    try:
        rows = tarec.deaths(a.file)
    except (tarec.Unreadable, OSError) as e:
        print(f"killall_check: {e}", file=sys.stderr)
        return 2
    ok, rep = check(rows, a.owner, a.count)
    if a.json:
        print(json.dumps(rep))
    elif ok is None:
        print(f"killall_check: {rep['error']}")
    else:
        c = rep["commander"]
        print(f"commander: slot {c['slot']} (type {c['type']}) died in the step from GameTime "
              f"{c['gametime']}")
        for r in rep["deaths"]:
            print(f"{'ok  ' if r['ok'] else 'FAIL'} slot {r['slot']} (type {r['type']}): the step "
                  f"from GameTime {r['gametime']}, expected {r['expected']}"
                  + (" (its own attacker)" if r["killer"] == r["slot"] else ""))
        if a.count is not None and rep["count"] != a.count:
            print(f"FAIL {rep['count']} deaths after the commander's, expected {a.count}")
        print(f"killall_check: {'PASS' if ok else 'FAIL'}")
    return 2 if ok is None else 0 if ok else 1


if __name__ == "__main__":
    sys.modules.setdefault("tarec", tarec)
    sys.exit(main())
