#!/usr/bin/env python3
"""impai_check -- ImpAI's observation, judged from a game's log lines.

    tools/impai/impai_check.py logs --los true|permanent [--puppet LOG]... [--patches] <log> [<log> ...]

reads the `impai:` lines the DLL writes (tagpu/ddraw/src/tagpu_impai.c) from the logs given --
tagpu.N.log files of one game, in any order: each is read in the order of the part its first line
names (tools/talog.py, `log: run <id> part <n>`) -- and fails unless:

  (a) the self-checks ran and agreed: every `impai: check ...` line says mismatch=0. Under True
      line of sight (`--los true`) every observing seat's `check los seat=N` line evaluated at
      least LOS_MIN units and the `check radar` line counts at least RADAR_PASSES passes; under
      Permanent (`--los permanent`) the `check los-local` line evaluated at least LOS_MIN, and
      the radar line at least RADAR_PASSES passes as well. The radar line's `stale` is 0: every
      bit an ImpAI seat's published pass held had its record written by that pass. A missing line
      fails: the lever tagpu_impai_check.on was not in place, or the level never ended;
  (b) with --puppet, the logs of the same recording played on a puppet: the overlay's digests
      agree -- every `impai: ov` known digest the two games both logged maps to the same overlay
      digest on both, at least OV_SHARED of them, and within each game one known digest never
      maps to two -- so the overlay is the same function of the recorded state on both -- and the
      knowledge agrees by GameTime: on every GameTime both games logged `impai: known` lines, the
      same seats with the same digests, at least OV_SHARED such GameTimes -- so the puppet knew
      what the live game knew, when it knew it -- and between the later of the two games' first
      such GameTimes and the earlier of their last, each GameTime one game logged the other logged
      too, so a GameTime is never left out of the pairing in the middle of a level, only at its
      ends (a puppet stops at its lane's last point, and logs no GameTime its frame visit did not
      put the recording at). The `known` lines come at every GameTime that is a
      multiple of 30, from the draw after the step; the `ov` lines come when the overlay's cache
      is rebuilt, at GameTimes that follow the renderer's pace, so they are paired by digest only;
  (c) every `impai: work` line is within its bounds, which the line states beside each count
      (`slots_max`, `visits_max`, `cells_max` and `flood_max`, each N/BOUND), its `refused` count
      is 0, and its patch scan completed at least one pass (`scans`);
  (d) every seat with a `work` line has its `impai: cost` line: the instrument ran. Its figures
      are printed, never judged;
  (e) every metal patch a seat published (`impai: patch`) is one the patch rule finds on the
      whole map's metal of the same level (`impai: metal`, the grid the rule reads, dumped at the
      level end right after that level's patch lines), recomputed here with every cell explored: a
      closed patch's flood saw every cell it needed explored, so it is the whole map's. A patch
      line with no dump after it fails. With --patches, at least one seat published a patch.

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
STALE = re.compile(r"impai: check radar passes=\d+ .*? stale=(\d+)")
WORK = re.compile(r"impai: work seat=(\d+) ticks=(\d+) refused=(\d+) slots_max=(\d+)/(\d+) "
                  r"visits_max=(\d+)/(\d+) cells_max=(\d+)/(\d+) flood_max=(\d+)/(\d+)")
BOUNDED = ("slots_max", "visits_max", "cells_max", "flood_max")
SCANS = re.compile(r"impai: work seat=(\d+) ticks=(\d+) .*? scans=(\d+) clusters=(\d+)")
COST = re.compile(r"impai: cost seat=(\d+) .*")
OV = re.compile(r"impai: ov seat=(\d+) tick=(\d+) known=([0-9A-F]{16}) ov=([0-9A-F]{16})")
KNOWN = re.compile(r"impai: known seat=(\d+) tick=(\d+) known=([0-9A-F]{16})")
PART = re.compile(r"^(?:# )?log: run (\S+) part (\d+) ")
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
            if what.startswith("radar"):
                stale = STALE.search(ln)
                if not stale:
                    fails.append(f"(a) the radar line has no stale count: {ln.strip()}")
                elif int(stale.group(1)):
                    fails.append(f"(a) {stale.group(1)} radar bits published with a record older than their pass")
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


def known_at(lines):
    """{tick: {seat: {known}}} of one game's `impai: known` lines: a restore can log one GameTime twice."""
    out = {}
    for ln in lines:
        m = KNOWN.search(ln)
        if m:
            out.setdefault(int(m.group(2)), {}).setdefault(int(m.group(1)), set()).add(m.group(3))
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
    ka, kb = known_at(live), known_at(puppet)
    ticks = sorted(set(ka) & set(kb))
    for t in ticks:
        for seat in sorted(set(ka[t]) | set(kb[t])):
            got_a, got_b = sorted(ka[t].get(seat, ())), sorted(kb[t].get(seat, ()))
            if got_a != got_b:
                fails.append(f"(b) GameTime {t} seat {seat}: live known {got_a or 'none'}, "
                             f"puppet known {got_b or 'none'}")
    if len(ticks) < OV_SHARED:
        fails.append(f"(b) {len(ticks)} GameTimes with knowledge logged by both games, under {OV_SHARED} "
                     f"(live {len(ka)}, puppet {len(kb)})")
    if ka and kb:
        lo, hi = max(min(ka), min(kb)), min(max(ka), max(kb))
        for t in sorted(set(ka) ^ set(kb)):
            if lo <= t <= hi:
                fails.append(f"(b) GameTime {t}: knowledge logged by the {'live game' if t in ka else 'puppet'} only")
    return fails, (f"(b) {len(shared)} shared known digests (live {len(a)}, puppet {len(b)}); "
                   f"{len(ticks)} GameTimes with knowledge logged by both")


def level_ends(lines):
    """Each level end's dump with that level's patches, in log order: [((w, h, threshold, rows),
    [(seat, patch)])], rows a list of bytearrays. The DLL writes a level end's `impai: patch` lines and
    then its `impai: metal` dump (tagpu_impai.c, impai_patches_dump), so the patch lines since the last
    dump are the next dump's. Also the patch lines no dump follows."""
    ends, pending, grid = [], [], None
    for ln in lines:
        m = PATCH.search(ln)
        if m:
            pending.append((int(m.group(1)), tuple(int(g) for g in m.groups()[1:])))
            continue
        m = METAL_HEAD.search(ln)
        if m:
            w, h, th = (int(g) for g in m.groups())
            grid = (w, h, th, [bytearray(w) for _ in range(h)])
            ends.append((grid, pending))
            pending = []
            continue
        m = METAL_ROW.search(ln)
        if m and grid is not None:
            z, x = int(m.group(1)), int(m.group(2))
            for run in m.group(3).split():
                v, n = run.split("*")
                grid[3][z][x:x + int(n)] = bytes([int(v, 16)]) * int(n)
                x += int(n)
    return ends, pending


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
    fails, notes = [], []
    ends, orphans = level_ends(lines)
    if orphans:
        fails.append(f"(e) {len(orphans)} patch line(s) with no `impai: metal` dump after them")
    if not ends:
        fails.append("(e) no `impai: metal` dump: the check lever was not in place, or the level never ended")
        return fails, "(e) not judged"
    published = 0
    for k, (grid, patches) in enumerate(ends, 1):
        offline = patches_offline(*grid)
        per = {}
        for seat, p in patches:
            if p not in offline:
                fails.append(f"(e) level end {k}: seat {seat}: patch {p} is none the whole map's metal holds")
            per[seat] = per.get(seat, 0) + 1
        published += len(patches)
        notes.append(f"level end {k}: {len(offline)} patches on the whole map ({grid[0]}x{grid[1]}, threshold "
                     f"{grid[2]}); published " + (", ".join(f"seat {s} {n}" for s, n in sorted(per.items())) or "none"))
    if need and not published:
        fails.append("(e) no seat published a patch")
    return fails, "(e) " + "; ".join(notes)


def log_order(paths):
    """The logs of one game in the order they were written: by the part their first line names, when
    every one names one; else as given."""
    parts = []
    for p in paths:
        with open(p, errors="replace") as f:
            m = PART.match(f.readline())
        parts.append(int(m.group(2)) if m else None)
    if None in parts:
        return list(paths)
    return [p for _, p in sorted(zip(parts, paths), key=lambda t: t[0])]


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
        for p in log_order(paths):
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
