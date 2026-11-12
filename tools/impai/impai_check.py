#!/usr/bin/env python3
"""impai_check -- ImpAI's observation, judged from a game's log lines.

    tools/impai/impai_check.py logs --los true|permanent [--puppet LOG]... [--patches] <log> [<log> ...]

reads the `impai:` lines the DLL writes (tagpu/ddraw/src/tagpu_impai.c) from the logs given --
tagpu.N.log files of one game, in any order -- and fails unless:

  (a) the self-checks ran and agreed: every `impai: check ...` line says mismatch=0. Under True
      line of sight (`--los true`) every observing seat's `check los seat=N` line evaluated at
      least LOS_MIN units and the `check radar` line counts at least RADAR_PASSES passes; under
      Permanent (`--los permanent`) the `check los-local` line evaluated at least LOS_MIN, and
      the radar line at least RADAR_PASSES passes as well. A missing line fails: the lever
      tagpu_impai_check.on was not in place, or the level never ended;
  (b) with --puppet, the logs of the same recording played on a puppet: the overlay's digests
      agree -- every `impai: ov` known digest the two games both logged maps to the same overlay
      digest on both, at least OV_SHARED of them, and within each game one known digest never
      maps to two -- so the overlay is the same function of the recorded state on both;
  (c) every `impai: work` line is within its bounds, which the line states beside each count
      (`slots_max`, `visits_max`, `cells_max` and `flood_max`, each N/BOUND), its `refused` count
      is 0, and its patch scan completed at least one pass (`scans`); every brain `impai: work`
      line (`decisions=`) likewise: `census_max`, `sites_max`, `snaps_max`, `roots_max` and
      `queue_max` within theirs and `refused=0`;
  (d) every seat with a `work` line has its `impai: cost` line: the instrument ran. Its figures
      are printed, never judged;
  (e) every metal patch a seat published (`impai: patch`) is one the patch rule finds on the
      whole map's metal (`impai: metal`, the grid the rule reads, as the level ended), recomputed
      here with every cell explored: a closed patch's flood saw every cell it needed explored, so
      it is the whole map's. With --patches, at least one seat published a patch.

Exit 0 when every check passes, 1 when one fails, 2 when the logs cannot be read.
"""
import argparse
import re
import sys
from pathlib import Path

LOS_MIN = 10000
RADAR_PASSES = 50
OV_SHARED = 20
PATCH_R = 64                    # tasim_snap.h SNAP_PATCH_R
PATCH_SIDE = 2 * PATCH_R + 1
PATCH_CAP = PATCH_SIDE * PATCH_SIDE

CHECK = re.compile(r"impai: check (los seat=(\d+)|los-local|radar passes=(\d+)) .*?evaluated=(\d+) mismatch=(\d+)")
MISMATCH = re.compile(r"impai: check (los|radar) mismatch .*")
WORK = re.compile(r"impai: work seat=(\d+) ticks=(\d+) refused=(\d+) slots_max=(\d+)/(\d+) "
                  r"visits_max=(\d+)/(\d+) cells_max=(\d+)/(\d+) flood_max=(\d+)/(\d+)")
BOUNDED = ("slots_max", "visits_max", "cells_max", "flood_max")
SCANS = re.compile(r"impai: work seat=(\d+) ticks=(\d+) .*? scans=(\d+) clusters=(\d+)")
COST = re.compile(r"impai: cost seat=(\d+) .*")
BRAIN = re.compile(r"impai: work seat=(\d+) decisions=(\d+) refused=(\d+) census_max=(\d+)/(\d+) "
                   r"sites_max=(\d+)/(\d+) snaps_max=(\d+)/(\d+) roots_max=(\d+)/(\d+) "
                   r"queue_max=(\d+)/(\d+)")
BRAIN_BOUNDED = ("census_max", "sites_max", "snaps_max", "roots_max", "queue_max")
OV = re.compile(r"impai: ov seat=(\d+) tick=(\d+) known=([0-9A-F]{16}) ov=([0-9A-F]{16})")
PATCH = re.compile(r"impai: patch seat=(\d+) ax=(-?\d+) az=(-?\d+) x0=(-?\d+) z0=(-?\d+) x1=(-?\d+) "
                   r"z1=(-?\d+) cells=(\d+)")
METAL_HEAD = re.compile(r"impai: metal w=(\d+) h=(\d+) threshold=(\d+)")
METAL_ROW = re.compile(r"impai: metal z=(\d+) x=(\d+)((?: [0-9A-F]+\*\d+)+)")


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
            continue
        m = BRAIN.search(ln)
        if m:
            seat, refused = int(m.group(1)), int(m.group(3))
            if refused:
                fails.append(f"(c) seat {seat}: the brain was refused {refused} decisions")
            for k, name in enumerate(BRAIN_BOUNDED):
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
    seats = {int(m.group(1)) for m in map(WORK.search, lines) if m}
    for ln in lines:
        m = SCANS.search(ln)
        if WORK.search(ln) and not m:
            fails.append(f"(c) a work line without its scan count: {ln.strip()}")
        if m:
            if int(m.group(2)) and not int(m.group(3)):
                fails.append(f"(c) seat {m.group(1)}: the patch scan never completed a pass in {m.group(2)} ticks")
    costed = set()
    for ln in lines:
        m = COST.search(ln)
        if m:
            costed.add(int(m.group(1)))
            notes.append(ln.strip())
    for seat in sorted(seats - costed):
        fails.append(f"(d) seat {seat}: no `impai: cost` line")
    return fails, notes


def ov_map(lines, side, fails):
    """{(seat, known): ov} of one game's `impai: ov` lines; a known digest with two overlay digests fails."""
    out = {}
    for ln in lines:
        m = OV.search(ln)
        if not m:
            continue
        key, ov = (int(m.group(1)), m.group(3)), m.group(4)
        if out.setdefault(key, ov) != ov:
            fails.append(f"(b) {side}: seat {key[0]} known {key[1]} has two overlay digests, {out[key]} and {ov}")
    return out


def judge_ov(live, puppet):
    fails = []
    a, b = ov_map(live, "live", fails), ov_map(puppet, "puppet", fails)
    shared = sorted(set(a) & set(b))
    for key in shared:
        if a[key] != b[key]:
            fails.append(f"(b) seat {key[0]} known {key[1]}: live overlay {a[key]}, puppet {b[key]}")
    if len(shared) < OV_SHARED:
        fails.append(f"(b) {len(shared)} known digests in both games, under {OV_SHARED} "
                     f"(live {len(a)}, puppet {len(b)})")
    return fails, f"(b) {len(shared)} shared known digests (live {len(a)}, puppet {len(b)})"


def metal_grid(lines):
    """(w, h, threshold, rows) of the last `impai: metal` dump, rows a list of bytearrays; None when none."""
    w = h = th = None
    rows = None
    for ln in lines:
        m = METAL_HEAD.search(ln)
        if m:
            w, h, th = (int(g) for g in m.groups())
            rows = [bytearray(w) for _ in range(h)]
            continue
        m = METAL_ROW.search(ln)
        if m and rows is not None:
            z, x = int(m.group(1)), int(m.group(2))
            for run in m.group(3).split():
                v, n = run.split("*")
                rows[z][x:x + int(n)] = bytes([int(v, 16)]) * int(n)
                x += int(n)
    return None if rows is None else (w, h, th, rows)


def patches_offline(w, h, th, rows):
    """Every patch tasim_snap_patches reports on the whole map, every cell explored: the scan's starts, each
    flooded within its window as tasim_snap.c's flood does, reported when closed and the start is its
    anchor. {(ax, az, x0, z0, x1, z1, cells)}."""
    def metal(x, z):
        return rows[z][x] if 0 <= x < w and 0 <= z < h else -1
    found = set()
    for z in range(h):
        for x in range(w):
            if metal(x, z) <= th or metal(x - 1, z) > th or metal(x - 1, z - 1) > th or \
               metal(x, z - 1) > th or metal(x + 1, z - 1) > th:
                continue
            left, top = x - PATCH_R, z - PATCH_R
            seen = {(PATCH_R, PATCH_R)}
            queue = [(PATCH_R, PATCH_R)]
            ax, az, x0, z0, x1, z1 = x, z, x, z, x, z
            boundary = edges = 0
            head, ok = 0, True
            while head < len(queue):
                rx, rz = queue[head]
                head += 1
                if not rx or not rz or rx == PATCH_SIDE - 1 or rz == PATCH_SIDE - 1:
                    ok = False
                    break
                if left + rx == 0: edges |= 1
                if left + rx == w - 1: edges |= 2
                if top + rz == 0: edges |= 4
                if top + rz == h - 1: edges |= 8
                for dz in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        nx, nz = rx + dx, rz + dz
                        wx, wz = left + nx, top + nz
                        if (nx, nz) in seen or wx < 0 or wz < 0 or wx >= w or wz >= h:
                            continue
                        seen.add((nx, nz))
                        if metal(wx, wz) <= th:
                            boundary = 1
                            continue
                        if len(queue) >= PATCH_CAP:
                            ok = False
                            break
                        queue.append((nx, nz))
                        if wz < az or (wz == az and wx < ax):
                            ax, az = wx, wz
                        x0, x1, z0, z1 = min(x0, wx), max(x1, wx), min(z0, wz), max(z1, wz)
                    if not ok:
                        break
                if not ok:
                    break
            if ok and boundary and (edges & 3) != 3 and (edges & 12) != 12 and (ax, az) == (x, z):
                found.add((ax, az, x0, z0, x1, z1, len(queue)))
    return found


def judge_patches(lines, need):
    fails, published = [], []
    for ln in lines:
        m = PATCH.search(ln)
        if m:
            published.append((int(m.group(1)), tuple(int(g) for g in m.groups()[1:])))
    grid = metal_grid(lines)
    if grid is None:
        fails.append("(e) no `impai: metal` dump: the check lever was not in place, or the level never ended")
        return fails, "(e) not judged"
    offline = patches_offline(*grid)
    for seat, p in published:
        if p not in offline:
            fails.append(f"(e) seat {seat}: patch {p} is none the whole map's metal holds")
    if need and not published:
        fails.append("(e) no seat published a patch")
    per = {}
    for seat, _p in published:
        per[seat] = per.get(seat, 0) + 1
    return fails, (f"(e) {len(offline)} patches on the whole map ({grid[0]}x{grid[1]}, threshold {grid[2]}); "
                   f"published " + (", ".join(f"seat {k} {v}" for k, v in sorted(per.items())) or "none"))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    lg = sub.add_parser("logs", help="judge one game's logs")
    lg.add_argument("--los", choices=("true", "permanent"), required=True,
                    help="the fixture's line-of-sight mode")
    lg.add_argument("--puppet", action="append", type=Path, default=[],
                    help="a log of the same recording played on a puppet, once a log: judge (b)")
    lg.add_argument("--patches", action="store_true", help="(e) requires a published patch")
    lg.add_argument("logs", nargs="+", type=Path)
    args = ap.parse_args()

    def read(paths):
        out = []
        for p in paths:
            out += p.read_text(errors="replace").splitlines()
        return [ln for ln in out if "impai: " in ln]
    try:
        lines, puppet = read(args.logs), read(args.puppet)
    except OSError as e:
        print(f"impai_check: {e}", file=sys.stderr)
        return 2
    fails, notes = judge(lines, args.los)
    if args.puppet:
        f, note = judge_ov(lines, puppet)
        fails += f
        notes.append(note)
    f, note = judge_patches(lines, args.patches)
    fails += f
    notes.append(note)
    for n in notes:
        print(n)
    for f in fails:
        print(f"FAIL {f}")
    print("impai_check: " + ("PASS" if not fails else f"{len(fails)} failure(s)"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
