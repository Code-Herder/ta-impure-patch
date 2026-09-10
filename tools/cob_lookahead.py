#!/usr/bin/env python3
"""How much of a COB can we predict from a sleeping thread's resume point?

The question behind `research/notes/smooth-motion.md` option B: a thread parked
in SLEEP carries its resume pc (record `+0x04`) and its ticks left (`+0x0C`), so
if we can decode forward from that pc to the pose writes it will apply on wake,
we can interpolate toward them instead of snapping. This measures how often that
decode succeeds, over the whole stock corpus.

The scan is an abstract interpreter with a constant-only stack:

  * PUSH_CONSTANT pushes a value; PUSH local/static pushes UNKNOWN
  * MOVE_NOW / TURN_NOW record (piece, axis) -> value, and refuse an UNKNOWN
  * MOVE / TURN (the *speed* variants) are stepped over, not refused: those
    pieces are already interpolated by the engine's stepper 0x4B1C00, so they
    need no prediction
  * JUMP follows; JUMP_NOT_EQUAL resolves when the condition is known and
    otherwise FORKS -- offline we do not know a static's value, so we explore
    both and only accept when every path agrees
  * SLEEP and RETURN end a path; anything else refuses

Resolved means every path completed and agreed on the same write map. Reaching
SLEEP having written nothing counts as resolved: it says no pose change is
coming, which is as actionable as knowing one is.

The forking is what makes this an UNDER-estimate of what the DLL could do. In
the game the statics are at `cob+0x10` (count x 4, read by PUSH_STATIC at
0x4B13AA) and the locals are in the thread record's own stack at `+0x24`, so the
condition resolves and no fork is needed. Every blocker this reports on a walk
script is one of those two reads away from resolving.

    tools/cob_lookahead.py                 # walk scripts, then every script
    tools/cob_lookahead.py --all-scripts   # just the corpus-wide pass
    tools/cob_lookahead.py --json
"""

import argparse
import collections
import importlib.machinery
import importlib.util
import json
import statistics
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def load_tacob():
    """tacob has no .py extension; sibling tools reach it by path, as here."""
    path = REPO / "tools" / "tacob"
    loader = importlib.machinery.SourceFileLoader("tacob", str(path))
    spec = importlib.util.spec_from_loader("tacob", loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


T = load_tacob()
OPNAME = {row[1] & 0xFFFFF000: row[0] for row in T.OPCODES}

PUSH = 0x10021000
SLEEP = 0x10013000
JUMP = 0x10064000
JNE = 0x10066000
RETURN = 0x10065000
MOVE_NOW = 0x1000B000
TURN_NOW = 0x1000C000
MOVE = 0x10001000
TURN = 0x10002000

UNKNOWN = object()


def scan(code, pc, forks=8, cap=3000):
    """-> (resolved, npieces, blocker). See the module docstring."""
    completed = []
    pending = [(pc, [], {}, 0)]
    while pending:
        pc, stack, writes, depth = pending.pop()
        steps = 0
        while True:
            steps += 1
            if steps > cap or pc < 0 or pc >= len(code):
                return False, 0, "cap/off-end"
            word = code[pc]
            op = word & 0xFFFFF000
            if op == PUSH:
                stack.append(code[pc + 1] if (word & 7) == 1 else UNKNOWN)
                pc += 2
            elif op in (MOVE_NOW, TURN_NOW):
                value = stack.pop() if stack else UNKNOWN
                if value is UNKNOWN:
                    return False, 0, "unknown value"
                writes[(code[pc + 1], code[pc + 2], op)] = value
                pc += 3
            elif op in (MOVE, TURN):
                for _ in range(2):
                    if stack:
                        stack.pop()
                pc += 3
            elif op in (SLEEP, RETURN):
                completed.append(dict(writes))
                break
            elif op == JUMP:
                pc = code[pc + 1]
            elif op == JNE:
                value = stack.pop() if stack else UNKNOWN
                if value is UNKNOWN:
                    if depth >= forks:
                        return False, 0, "fork limit"
                    pending.append((code[pc + 1], list(stack), dict(writes), depth + 1))
                    pc += 2
                    depth += 1
                else:
                    pc = code[pc + 1] if value == 0 else pc + 2
            else:
                return False, 0, OPNAME.get(op, hex(op))
    if not completed:
        return False, 0, "no path"
    first = completed[0]
    if all(d == first for d in completed):
        return True, len(first), None
    return False, 0, "paths disagree"


def sweep(cobs, want):
    total = resolved = nochange = 0
    blockers = collections.Counter()
    widths = []
    for blob, unit in cobs:
        try:
            cob = T.read_cob(blob)
        except Exception:
            continue
        code = cob.code
        for name, lo, hi in T.script_ranges(cob):
            if not want(name):
                continue
            for pc in range(lo, hi):
                if (code[pc] & 0xFFFFF000) != SLEEP:
                    continue
                total += 1
                ok, npieces, why = scan(code, pc + 1)
                if not ok:
                    blockers[why] += 1
                    continue
                resolved += 1
                if npieces:
                    widths.append(npieces)
                else:
                    nochange += 1
    return {
        "resume_points": total,
        "resolved": resolved,
        "pct": round(100.0 * resolved / total, 1) if total else 0.0,
        "with_pose_change": resolved - nochange,
        "no_change_coming": nochange,
        "pieces_median": statistics.median(widths) if widths else 0,
        "pieces_max": max(widths) if widths else 0,
        "blocked": dict(blockers.most_common()),
    }


def report(label, r):
    print(f"=== {label}: {r['resolved']}/{r['resume_points']} resolved  ({r['pct']}%)")
    print(f"    {r['with_pose_change']} carry a pose change, "
          f"{r['no_change_coming']} are 'no change coming'")
    if r["pieces_max"]:
        print(f"    pieces written per wake: median {r['pieces_median']:.0f}, "
              f"max {r['pieces_max']}")
    for why, n in r["blocked"].items():
        print(f"    blocked {n:6d}  {why}")
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--gamedir")
    ap.add_argument("--all-scripts", action="store_true",
                    help="only the corpus-wide pass, not the walk one")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    assets = T.load_assets(args.gamedir)
    cobs = [(assets.read(p), Path(p).stem) for p in assets.glob("scripts/*.cob")]
    if not cobs:
        print("no scripts/*.cob found in the game archives", file=sys.stderr)
        return 2

    out = {"corpus": len(cobs)}
    if not args.all_scripts:
        out["walk"] = sweep(cobs, lambda n: "walk" in n.lower())
    out["all"] = sweep(cobs, lambda n: True)

    if args.json:
        print(json.dumps(out, indent=1))
    else:
        print(f"corpus: {len(cobs)} stock COBs\n")
        if "walk" in out:
            report("walk / walklegs", out["walk"])
        report("every script", out["all"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
