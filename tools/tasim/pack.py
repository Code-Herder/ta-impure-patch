#!/usr/bin/env python3
"""pack.py -- the packed calibration lane's planner: which runs share a batch, where each cell stands,
and the wire and pause clause that play it.

    pack.py plan --scenarios 'GLOB' --arena tools/tasim/calib-arena-packed.json --k 8 --refs DIR \\
                 --peaks FILE [--viewed N] [--json]

A BATCH is one apply on one generated flat map (tools/tasim/synthmap.py). It holds K CELLS, each one
calibration job -- one (fight, index), with perturb.run_wire's perturbation, swap and seed for that
index -- translated into its own region of the map, far enough from every other that no unit, shot,
sensor or explosion of one reaches another. Cell k is created 30·k ticks after the apply (THE STAGGER)
at block positions P_k of each seat's block, so it sees the weapon scan and every schedule whose period
divides 30 at the phase an unpacked run sees them (autoheal's: below; the pathfinder's 150-call tick and the
feature tick's cursor keep no such phase). The argument, its conditions, the engine facts it rests on and the
checks that hold each batch to it afterwards (P1 to P5, engtrace.py): research/notes/modules/tasim-calibration.md,
*The packed lane*.

THE STAGGER: P_k = (30·s·k) mod B. The weapon scan 0x4089A0 examines s = floor(limit/30) + 1 slots of a
seat's block a step (`imul 0x88888889` at 0x4089C0), its cursor obj+0x39 stepping over every slot,
empty or not, and wrapping from the block's last +0x6B to its first +0x67 (0x4089DC..0x4089FB); the
applier zeroes the cursor at an apply that clears (tagpu_scenario.c reset_weapon_scans) (DISASSEMBLED).
So the window scanned at step j is [s·j, s·j + s) mod B, and a unit at block position P + p created d
steps after the reset is scanned on the same relative steps as position p created at the reset iff
P = s·d (mod B): with d = 30·k, P_k = (30·s·k) mod B. At limit 1500 (s 51, B 1500) P_k = 30·k; at
1000 (s 34) 20·k. Creating cell k at GameTime T + 30·k with T = 1 (mod 30) also keeps every 30-tick
phase (the economy pass, the viewed seat's radar pass, the acquisition scan 0x40AA40) where an unpacked
run has it. The cell's capacity, units a seat, is the least distance between two cells' P_k.
One schedule's period does not divide 30: autoheal, every 8 ticks on GameTime = 0 (mod 8) (`test byte
[GameTime],7` at 0x48AF5E), for a type with a nonzero `healtime` (def +0x200). A batch holding such a
type is staggered by PHASE_HEAL = 120 = lcm(8, 30) ticks a cell, and its P_k follows with d = 120·k;
no type of the rt corpus has one (scout batch PC1).

THE LAYOUT. A unit of cell j reaches a unit of cell k only through a channel with a radius: a weapon's
fire (can-engage holds dx² + dz² <= range², 0x49AC47..0x49ACA2), area damage (the weapon's +0xD6, death
weapons included), acquisition (range, or min(sight, range)), the threat sum 0x40B1C0 (radius 0xA0 at
its one caller, 0x407FC9..0x407FD0), sight (32-wu stamps), and radar, sonar and jamming (exact centre
distances, 0x467892..0x46793D) (DISASSEMBLED; the module note's table). So for cell k, with F = 1.5 for a
projectile's overshoot and E = 320 for a unit's excursion:

    R_k    = max(sense_k, F·range_k + aoe_k, 160, aoe_k)
    zone_k = box_k ⊕ (E + F·range_k)              where the cell's shots may fly (P2)
    gap(box_j ⊕ E, box_k ⊕ E) > max(R_j, R_k) + max(X_j, X_k)   along x or along z, for every pair

box_k is the box over the cell's start positions and every order point, widened by the perturbation's
jitter (perturb.JITTER), so one box holds every index of the fight. X_k is the farthest a footprint of
the cell's types reaches past its unit's centre, pathing ring included: a footprint of fw cells covers
[16·X, 16·(X + fw)) with X = (x - 8·fw + 8) >> 4 (0x43D877..0x43D895), at most 8·fw + 8 world units
from the centre, and the pathing fill marks one 16-wu cell more around it -- 72 for the corpus's 6 x 6
ARMBATS and CORMSHIP. Every offset is a multiple of GRID = 128 world units on x and z, which keeps every
position modulo every grid the skirmish step reads: 16 (heights, footprints, pathing), 32 (sight, LOS,
the explored map), 128 (the sort buckets main+0x142A3) (scout batch PC3: none coarser). The 32-wu
grids are sheared by altitude (row = (z - y/2) >> 5), which a translation keeps because the ground under
every cell is one height. NOT BOUNDED HERE: a pathfinder search reads pathing cells wherever its A*
frontier and bug walk go, and neither has a bound tied to a reach (engine/units.md), so a search toward a
blocked goal could read another cell's obstacles; engtrace's P3 is where a search that leaves its cell
fails the generation. Shelves: the cells sorted by box height, laid in
rows along x; a fight whose R exceeds a quarter of the map gets a row of its own; every offset is
rounded UP to the grid and the next cell is placed from where the last one landed, so the gap rule
holds after the rounding by construction. Every box_k ⊕ (E + R_k) lies inside the arena's `flat` and
outside its `keepout`.

THE BOUNDS, each a refusal here and checked again offline: units a seat a cell <= the capacity; units a
batch <= 400; the cells' measured projectile peaks summed <= 1500 (the pool holds 3000 and refuses an
append past it; a peak is the pool's own count, main+0x141F3, so a burst weapon's shot counts its parent
and its copies, 1 + `burst` records: the copies are appended by the projectile tick, 0x49B809); each
seat's energy and metal spent over the batch's window <= half its storage; the viewed seat's units <= the
temporary sight records' cap (20 stock, the test lever's N with it: the one list of the game, `cmp
eax,0x14` at 0x48292C).

THE EXPLORED MAP. The pathfinder's map reader reads the shared mapped grid main+0x14273 and answers an
unexplored block passable whatever stands on it (0x40D817..0x40D831), so a cell's paths would depend on
what other cells explored. A packed fight plays with `mapping` on, which explores the whole map for both
seats from the start (MEASURED 2026-11-13 on both generated maps at the launch's hold: all 409 600
words of the grid hold seat 0's and seat 1's bits), and the planner refuses an arena or a fight without it.

THE PLAN is deterministic, a function of the jobs, K, the bounds and the prior (each fight's median
length in the unpacked references, for every mod). Round r takes index r of every fight; its fights,
longest first, go first-fit into the round's batches, K distinct fights at most; the round's last
batch, if it is short, is carried into the next round, which fills it only with fights it does not
hold. Within a batch cell 0 is the longest, so the batch ends near max_k(at_k + length_k).
"""

import argparse
import copy
import dataclasses
import glob as globmod
import json
import statistics
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
import calibref  # noqa: E402
import perturb  # noqa: E402

PHASE = 30                      # ticks between two cells' creates: the 30-tick phases' period
PHASE_HEAL = 120                # lcm(30, 8): with a `healtime` type, autoheal's period 8 too (THE STAGGER)
GRID = 128                      # world units: every offset a multiple (THE LAYOUT)
FOOT_RING = 16                  # the pathing fill's ring of one 16-wu cell around a footprint (THE LAYOUT)
E = 320                        # world units: a unit's excursion beyond its box (P1)
F = 1.5                         # a shot's flight beyond the weapon's range (P2)
THREAT_R = 160                  # 0x40B1C0's radius at its one caller (0x407FC9..0x407FD0)
L = 5400                        # a fight's tick limit (duelgen.LIMIT)
UNITS_MAX = 400                 # units a batch
PROJ_MAX = 1500                 # summed projectile peaks a batch: half the pool's 3000
STORAGE_FRAC = 0.5              # of each seat's storage, spent over the batch's window
SIGHT_CAP = 20                  # temporary sight records (0x48292C) without the lever
ORDERS = ("attack", "move")
SEATS = (0, 1)
VIEWED = 0                      # the viewed seat: seat 0, human (scenarios/calib/arena.json)
# def offsets (tagpu/ddraw/src/tasim_keys.c): sight, radar, sonar, radar jam, sonar jam
SENSE_OFFS = (0x202, 0x204, 0x206, 0x20A, 0x20C)
WEAPON_SLOTS = ("0x1EE", "0x1F2", "0x1F6")
DEATH_SLOTS = ("0x220", "0x224")            # explodeas, selfdestructas
W_RANGE, W_AOE, W_BURST = 0xDC, 0xD6, 0xEA
U_ENERGYUSE, U_CLOAKCOST, U_CLOAKMOVING, U_BMCODE = 0x1C6, 0x1DA, 0x1DE, 0x22F
U_HEALTIME, U_FOOTX, U_FOOTZ = 0x200, 0x14A, 0x14C
U_FLAGS, F241_INIT_CLOAKED = 0x241, 1 << 4


class PackError(Exception):
    pass


@dataclasses.dataclass(frozen=True)
class Cell:
    job: tuple                  # (fight path, index)
    k: int                      # 0..K-1
    at: int                     # phase*k (30, or 120 with a healtime type): ticks after the apply to its create
    offset: tuple               # (dx, dz), multiples of GRID
    pos: dict                   # seat -> block position P_k
    box: tuple                  # (x0, z0, x1, z1) after translation, order points and jitter included
    reach: int                  # R_k
    zone: tuple                 # box ⊕ (E + F·range), after translation
    ranges: list                # [(seat, lo, hi, n)]: the pause clause's slot ranges


# ---- the engine's numbers ------------------------------------------------------------------

def scan_step(limit: int) -> int:
    """s: the slots of a seat's block the weapon scan examines a step (0x4089C0)."""
    return limit // 30 + 1


def positions(limit: int, K: int, phase: int = PHASE) -> list:
    """P_k for k < K, cells `phase` ticks apart (THE STAGGER), B the block length = the unit limit."""
    s, B = scan_step(limit), limit
    return [(phase * s * k) % B for k in range(K)]


def capacity(limit: int, K: int, phase: int = PHASE) -> int:
    """Units a seat a cell: the least distance from one P_k to the next, and to the block's end for the
    last (a cell's range never wraps, so the pause clause's lo..hi is one run); 0 when two cells share a
    position."""
    ps = sorted(positions(limit, K, phase))
    gaps = [b - a for a, b in zip(ps, ps[1:])] + [limit - ps[-1]]
    return min(gaps)


def batch_phase(members: list, geoms: dict) -> int:
    """The ticks between two cells' creates in a batch of `members` [(path, index)] (THE STAGGER)."""
    return PHASE_HEAL if any(geoms[p]["heal"] for p, _i in members) else PHASE


# ---- the defs ------------------------------------------------------------------------------

def _row(entry, off):
    for row in entry["image"]:
        if row[0] == off:
            return bytes.fromhex(row[2])
    return None


def _int(entry, off, signed=False):
    b = _row(entry, off)
    return int.from_bytes(b, "little", signed=signed) if b is not None else 0


def _float(entry, off):
    import struct
    b = _row(entry, off)
    return struct.unpack("<f", b)[0] if b is not None and len(b) == 4 else 0.0


def unit_facts(defs_doc: dict, roles_doc: dict) -> dict:
    """{UNITNAME: {sense, range, aoe, bmcode, shot_cost [metal, energy] a tick, upkeep energy a tick,
    foot (the larger footprint side, cells), heal (a nonzero healtime), shot_records (1 + the largest
    `burst` of its weapons: the pool records one shot holds)}} from `tasim_run defs` and `tasim_run roles`
    (native.py): the images of the engine's own definitions. range is over the three weapon slots, death
    weapons (explodeas, selfdestructas) only in aoe.
    upkeep is (energyuse + the cloak's cost) / 30 a tick: both are rates a second [INFERRED: the economy
    pass 0x401360, which reads energyuse, runs once on each player's 30-tick pass, engine/recorder.md].
    The cloak's cost, the larger of cloakcost and cloakcostmoving, counts only for a type that starts
    cloaked (`init_cloaked`, def +0x241 bit 4): a packed fight orders only attack and move, so no other
    unit cloaks [INFERRED]. P4b holds the level to half the storage every tick either way."""
    weapons = {w["section"].upper(): w for w in defs_doc["weapons"]}
    shots = {u["name"].upper(): u["shot_cost_per_tick"] for u in roles_doc["units"]}
    out = {}
    for u in defs_doc["units"]:
        name = u["unitname"].upper()
        slots = [weapons.get((u["weapons"].get(k) or "").upper()) for k in WEAPON_SLOTS]
        deaths = [weapons.get((u["weapons"].get(k) or "").upper()) for k in DEATH_SLOTS]
        mask = next((r[3] for r in u["image"] if r[0] == U_FLAGS), 0)
        if mask != -1 and not mask & F241_INIT_CLOAKED:
            raise PackError(f"{name}: tasim_run's +0x{U_FLAGS:X} leaves out init_cloaked")
        cloaked = _int(u, U_FLAGS) & F241_INIT_CLOAKED
        cloak = max(0.0, _float(u, U_CLOAKCOST), _float(u, U_CLOAKMOVING)) if cloaked else 0.0
        out[name] = {
            "sense": max(_int(u, o) for o in SENSE_OFFS),
            "range": max([_int(w, W_RANGE) for w in slots if w] or [0]),
            "aoe": max([_int(w, W_AOE) for w in slots + deaths if w] or [0]),
            "bmcode": _int(u, U_BMCODE),
            "shot_cost": list(shots.get(name, [0.0, 0.0])),
            "upkeep": (max(0.0, _float(u, U_ENERGYUSE)) + cloak) / PHASE,
            "foot": max(_int(u, U_FOOTX), _int(u, U_FOOTZ)),
            "heal": _int(u, U_HEALTIME) > 0,
            "shot_records": 1 + max([_int(w, W_BURST) for w in slots if w] or [0]),
        }
    return out


def install_facts(gamedir=None, setup=None) -> dict:
    """unit_facts of an install (or a compat setup's folder), through native.py."""
    import native
    docs = []
    for cmd in ("defs", "roles"):
        rc, out, err = native.run(cmd, gamedir, setup=setup)
        if rc:
            raise PackError(f"tasim_run {cmd}: {err.strip() or rc}")
        docs.append(json.loads(out))
    return unit_facts(*docs)


def values_of(facts: dict) -> dict:
    """The value table perturb reads (only bmcode), from the facts: a structure draws no jitter."""
    return {name: {"bmcode": f["bmcode"]} for name, f in facts.items()}


# ---- one fight -----------------------------------------------------------------------------

def admissible(exp: dict, block: dict = None):
    """None, or why the fight cannot be packed: an order other than attack and move (so no create
    happens in play and the requested slots stay unique), a feature (nothing to reclaim), an exact
    scenario (packed by its own stage, never by the plan), no end_units (a course: it plays alone) or
    mapping off (THE EXPLORED MAP)."""
    for u in exp["units"]:
        for o in u["orders"]:
            if o["cmd"] not in ORDERS:
                return f"unit {u['id']}: order {o['cmd']!r} (a packed fight orders only {' and '.join(ORDERS)})"
            if o.get("count"):
                return f"unit {u['id']}: a counted order"
    if exp["features"]:
        return f"{len(exp['features'])} feature(s)"
    if block is not None:
        if "exact" in block["kind"]:
            return "an exact scenario"
        if not block["end_units"]:
            return "no end_units (a course)"
        if block["mapping"] != 1:
            return "mapping off: an unexplored block is passable to the pathfinder (0x40D831)"
    return None


def fight_geometry(exp: dict, facts: dict) -> dict:
    """The fight before translation: box (x0, z0, x1, z1) over every start position and order point,
    widened by the jitter; sense, range, aoe and reach over its types; extent, the farthest a footprint
    reaches past its centre with the pathing ring (THE LAYOUT); its units per owner."""
    xs, zs = [], []
    per_owner = {0: 0, 1: 0}
    types = set()
    for u in exp["units"]:
        xs.append(u["pos"][0])
        zs.append(u["pos"][1])
        if u["owner"] not in per_owner:
            raise PackError(f"unit {u['id']}: owner {u['owner']} (a packed fight has seats 0 and 1)")
        per_owner[u["owner"]] += 1
        types.add(u["type"].upper())
        for o in u["orders"]:
            if o.get("to"):
                xs.append(o["to"][0])
                zs.append(o["to"][1])
    missing = sorted(t for t in types if t not in facts)
    if missing:
        raise PackError(f"no definition for {', '.join(missing)}")
    j = perturb.JITTER // 2
    sense = max(facts[t]["sense"] for t in types)
    rng = max(facts[t]["range"] for t in types)
    aoe = max(facts[t]["aoe"] for t in types)
    reach = int(-(-max(sense, F * rng + aoe, THREAT_R, aoe) // 1))
    return {"box": (min(xs) - j, min(zs) - j, max(xs) + j, max(zs) + j), "sense": sense, "range": rng,
            "aoe": aoe, "reach": reach, "units": per_owner, "types": sorted(types),
            "extent": max(8 * facts[t]["foot"] + 8 for t in types) + FOOT_RING,
            "heal": any(facts[t]["heal"] for t in types),
            "shot_records": max(facts[t]["shot_records"] for t in types),
            "shot_cost": {o: [sum(facts[u["type"].upper()]["shot_cost"][r] for u in exp["units"]
                                  if u["owner"] == o) for r in (0, 1)] for o in per_owner},
            "upkeep": {o: sum(facts[u["type"].upper()]["upkeep"] for u in exp["units"] if u["owner"] == o)
                       for o in per_owner}}


def seat_of(owner: int, index: int) -> int:
    """The seat a fight's owner plays at this index: perturb swaps them on an odd index."""
    return 1 - owner if index % 2 == 1 else owner


def kind_of(block: dict) -> str:
    """land or naval: the arena a fight is laid out for (its area's second part, `sea` for naval)."""
    return "naval" if block["area"].split("/")[1:2] == ["sea"] else "land"


# ---- the bounds ----------------------------------------------------------------------------

def default_bounds(arena: dict, viewed: int = SIGHT_CAP) -> dict:
    scn = json.loads((TREE / arena["scenario"]).read_text())
    storage = {p["slot"]: p["storage"] for p in scn["setup"]["players"]}
    return {"units": UNITS_MAX, "proj": PROJ_MAX, "viewed": viewed, "storage_frac": STORAGE_FRAC,
            "storage": {s: [storage[s]["metal"], storage[s]["energy"]] for s in SEATS},
            "limit": arena["unit_limit"], "L": L}


def window(K: int, bounds: dict, phase: int = PHASE) -> int:
    """Ticks a batch of K cells may run: its last cell's create plus the tick limit."""
    return phase * (K - 1) + bounds["L"]


def bounds_problem(members: list, geoms: dict, prior: dict, bounds: dict, K: int):
    """None, or the first bound the cells `members` [(path, index)] break as one batch of K."""
    phase = batch_phase(members, geoms)
    cap = capacity(bounds["limit"], K, phase)
    units = viewed = proj = 0
    spend = {s: [0.0, 0.0] for s in SEATS}
    win = window(K, bounds, phase)
    for path, index in members:
        g = geoms[path]
        for owner, n in g["units"].items():
            seat = seat_of(owner, index)
            if n > cap:
                return f"{Path(path).stem}: {n} units a seat, the cell holds {cap}"
            units += n
            if seat == VIEWED:
                viewed += n
            for r in (0, 1):
                spend[seat][r] += g["shot_cost"][owner][r] * win
            spend[seat][1] += g["upkeep"][owner] * win
        peak = (prior.get(Path(path).stem) or {}).get("proj_peak")
        if peak is None:
            raise PackError(f"{Path(path).stem}: no measured projectile peak in the prior")
        proj += peak
    if units > bounds["units"]:
        return f"{units} units, past {bounds['units']}"
    if proj > bounds["proj"]:
        return f"projectile peaks {proj}, past {bounds['proj']}"
    if viewed > bounds["viewed"]:
        return f"{viewed} units of the viewed seat, past the sight records' cap {bounds['viewed']}"
    for s in SEATS:
        for r, what in ((0, "metal"), (1, "energy")):
            room = bounds["storage"][s][r] * bounds["storage_frac"]
            if spend[s][r] > room:
                return f"seat {s}'s {what} over {win} ticks {spend[s][r]:.0f}, past {room:.0f}"
    return None


# ---- the layout ----------------------------------------------------------------------------

def _up(v: int) -> int:
    return -(-v // GRID) * GRID


def _grow(box, by):
    return (box[0] - by, box[1] - by, box[2] + by, box[3] + by)


def _overlaps(a, b) -> bool:
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


def _inside(a, b) -> bool:
    return b[0] <= a[0] and b[1] <= a[1] and a[2] <= b[2] and a[3] <= b[3]


def gap(a, b) -> int:
    """The separation of two boxes along the axis that separates them most; negative when they meet."""
    return max(b[0] - a[2], a[0] - b[2], b[1] - a[3], a[1] - b[3])


def separation(r1: int, x1: int, r2: int, x2: int) -> int:
    """The gap two cells' boxes ⊕ E must exceed: the larger reach and the larger footprint extent."""
    return max(r1, r2) + max(x1, x2)


def layout(cells: list, arena: dict):
    """{k: (dx, dz)} for cells [(k, geometry)], or None when they do not fit the arena (THE LAYOUT).
    Rows along x, the tallest box first; a row closes when the next cell would pass the flat region's
    right edge, and a fight whose reach passes a quarter of the map stands in a row of its own."""
    fx, fz, fw, fh = arena["flat"]["rect"]
    flat = (fx, fz, fx + fw, fz + fh)
    kx, kz, kw, kh = arena.get("keepout") or (0, 0, 0, 0)
    keep = (kx, kz, kx + kw, kz + kh)
    quarter = max(fw, fh) / 4
    order = sorted(cells, key=lambda c: (-(c[1]["box"][3] - c[1]["box"][1]), c[0]))
    offsets, placed = {}, []            # placed: (k, box ⊕ E, reach, extent)
    done_rows = []                      # every closed row's (lowest edge of its boxes ⊕ E, reach, extent)
    row = None                          # the open row: {"alone", "reach", "extent", "z", "last", "boxes"}

    def close():
        if row:
            done_rows.append((max(b[3] for b in row["boxes"]), row["reach"], row["extent"]))

    def put(g, z, x):
        """(dx, dz, box ⊕ E) placing g's box ⊕ E at or right of x and at or below z, clear of the keepout."""
        b, r = g["box"], g["reach"]
        dz = _up(z + E - b[1])
        while True:
            dx = _up(x + E - b[0])
            bx = (b[0] + dx - E, b[1] + dz - E, b[2] + dx + E, b[3] + dz + E)
            if not _overlaps(_grow(bx, r), keep):
                return dx, dz, bx
            x = keep[2] + r + 1

    for k, g in order:
        r, x = g["reach"], g["extent"]
        alone = r > quarter
        fits = None
        if row and not alone and not row["alone"]:
            reach, ext = max(row["reach"], r), max(row["extent"], x)
            z = row["z"]
            for bottom, rr, xx in done_rows:    # a wider reach in this row moves it past earlier rows
                if bottom + separation(rr, xx, reach, ext) + 1 > z:
                    z = None
                    break
            if z is not None:
                last_edge, last_r, last_x = row["last"]
                dx, dz, bx = put(g, z, last_edge + separation(last_r, last_x, r, x) + 1)
                if _inside(_grow(bx, r), flat):
                    fits = (dx, dz, bx)
        if fits is None:
            close()
            z = flat[1] + r
            for bottom, rr, xx in done_rows:
                z = max(z, bottom + separation(rr, xx, r, x) + 1)
            dx, dz, bx = put(g, z, flat[0] + r)
            if not _inside(_grow(bx, r), flat):
                return None
            row = {"alone": alone, "reach": r, "extent": x, "z": z, "boxes": []}
            fits = (dx, dz, bx)
        dx, dz, bx = fits
        row["reach"], row["extent"] = max(row["reach"], r), max(row["extent"], x)
        row["last"] = (bx[2], r, x)
        row["boxes"].append(bx)
        offsets[k] = (dx, dz)
        placed.append((k, bx, r, x))
    for i, (kj, bj, rj, xj) in enumerate(placed):
        for kk, bk, rk, xk in placed[i + 1:]:
            if gap(bj, bk) <= separation(rj, xj, rk, xk):
                raise PackError(f"the layout broke the gap rule between cells {kj} and {kk}")
    return offsets


# ---- the plan ------------------------------------------------------------------------------

def prior_from_refs(refs_dir) -> dict:
    """{fight stem: {"length": the median ticks from apply to end}} over a reference set's runs
    (calib.py refs build's folder: <stem>@<hash>/<build>.json)."""
    out = {}
    for f in sorted(Path(refs_dir).glob("*@*/*.json")):
        stem = f.parent.name.split("@")[0]
        doc = json.loads(f.read_text())
        lens = [r["t_end"] - r["t_apply"] for r in doc.get("runs", []) if "t_end" in r and "t_apply" in r]
        if lens:
            out.setdefault(stem, {})["length"] = int(statistics.median(lens))
    return out


def with_peaks(prior: dict, peaks: dict) -> dict:
    """The prior with each fight's measured projectile peak beside its length."""
    out = copy.deepcopy(prior)
    for stem, n in peaks.items():
        out.setdefault(stem, {})["proj_peak"] = int(n)
    return out


def _length(prior, path) -> int:
    return min(L, int((prior.get(Path(path).stem) or {}).get("length", L)))


def _cells(members, geoms, arena, K, offsets):
    """The batch's Cells, k = 0 the longest, with their offsets."""
    phase = batch_phase(members, geoms)
    ps = positions(arena["unit_limit"], K, phase)
    out = []
    for k, (path, index) in enumerate(members):
        g = geoms[path]
        dx, dz = offsets[k]
        b = g["box"]
        box = (b[0] + dx, b[1] + dz, b[2] + dx, b[3] + dz)
        ranges = []
        n_of = dict(perturb.end_clauses(g["block"], index % 2 == 1))
        for owner in (0, 1):
            seat = seat_of(owner, index)
            ranges.append((seat, ps[k], ps[k] + g["units"][owner] - 1, n_of.get(seat, 0)))
        ranges.sort()
        out.append(Cell(job=(str(path), index), k=k, at=phase * k, offset=(dx, dz),
                        pos={s: ps[k] for s in SEATS}, box=box, reach=g["reach"],
                        zone=_grow(box, E + int(-(-F * g["range"] // 1))), ranges=ranges))
    return out


def _order(members, prior):
    return sorted(members, key=lambda m: (-_length(prior, m[0]), Path(m[0]).stem, m[1]))


def _fits(members, geoms, prior, arena, bounds, K):
    if bounds_problem(members, geoms, prior, bounds, K):
        return None
    ordered = _order(members, prior)
    offsets = layout([(k, geoms[p]) for k, (p, _i) in enumerate(ordered)], arena)
    return None if offsets is None else (ordered, offsets)


def plan(jobs, arena: dict, K: int, bounds: dict, prior: dict, geoms: dict) -> list:
    """[[Cell]]: every job in exactly one batch (THE PLAN). `geoms` is {fight path: fight_geometry with
    its calib block under "block"}; a fight's prior has its length and its projectile peak."""
    if not 1 <= K <= arena["unit_limit"] // PHASE or not capacity(arena["unit_limit"], K):
        raise PackError(f"K = {K}: 1..{arena['unit_limit'] // PHASE}")
    if arena.get("mapping") != 1:
        raise PackError("the arena's mapping is off (THE EXPLORED MAP)")
    rounds = {}
    for path, index in jobs:
        rounds.setdefault(index, []).append(str(path))
    batches, carried = [], None
    for r in sorted(rounds):
        fights = sorted(set(rounds[r]), key=lambda p: (-_length(prior, p), Path(p).stem))
        if len(fights) != len(rounds[r]):
            raise PackError(f"round {r} names a job twice")
        open_ = [carried] if carried else []
        for p in fights:
            for b in open_:
                if len(b) >= K or any(q == p for q, _i in b):
                    continue
                if _fits(b + [(p, r)], geoms, prior, arena, bounds, K):
                    b.append((p, r))
                    break
            else:
                if not _fits([(p, r)], geoms, prior, arena, bounds, K):
                    raise PackError(f"{Path(p).stem} index {r} fits no batch on its own")
                open_.append([(p, r)])
        if carried is not None:
            open_ = open_[1:]
            batches.append(carried)
        carried = open_.pop() if open_ and len(open_[-1]) < K else None
        batches.extend(open_)
    if carried:
        batches.append(carried)
    out = []
    for members in batches:
        ordered, offsets = _fits(members, geoms, prior, arena, bounds, K)
        out.append(_cells(ordered, geoms, arena, K, offsets))
    return out


def batch_length(batch: list, prior: dict) -> int:
    return max(c.at + _length(prior, c.job[0]) for c in batch)


# ---- the wire and the pause clause -----------------------------------------------------------

def batch_exp(batch: list, arena: dict, values: dict) -> dict:
    """One expanded scenario for the batch: each cell's run (perturb.run_wire: its perturbation, swap
    and seed), translated by its offset, its units created `at` ticks after the apply at block position
    P_k + p of their seat (p their rank among the cell's units of that seat, in wire order: where an
    unpacked run's creates land), ids prefixed by the cell, cells in k order."""
    exps = []
    for c in sorted(batch, key=lambda c: c.k):
        exp, _record, _wire, _ends = perturb.run_wire(c.job[0], c.job[1], values, arena)
        dx, dz = c.offset
        rank = {s: 0 for s in SEATS}
        for u in exp["units"]:
            u["id"] = f"c{c.k}:{u['id']}"
            u["pos"] = [u["pos"][0] + dx, u["pos"][1] + dz]
            u["at"] = c.at
            u["slot"] = c.pos[u["owner"]] + rank[u["owner"]]
            rank[u["owner"]] += 1
            for o in u["orders"]:
                if o.get("target") is not None:
                    o["target"] = f"c{c.k}:{o['target']}"
                if o.get("to"):
                    o["to"] = [o["to"][0] + dx, o["to"][1] + dz]
        exps.append(exp)
    out = copy.deepcopy(exps[0])
    out["units"] = [u for e in exps for u in e["units"]]
    out["features"] = []
    for e in exps[1:]:
        if e["setup"] != out["setup"]:
            raise PackError("two cells' runs carry different setups")
    return out


def wire(batch: list, arena: dict, values: dict) -> str:
    """The batch's wire, through tacli's own writer; refused when the writer does not carry the at= and
    slot= keys (without them every cell would be created at the apply, in the first free slots)."""
    exp = batch_exp(batch, arena, values)
    text = perturb.CLI._scn_wire(exp)
    lines = [ln for ln in text.splitlines() if ln.startswith("unit ")]
    if len(lines) != len(exp["units"]) or any(
            (f" at={u['at']}" not in ln if u["at"] else False) or f" slot={u['slot']}" not in ln
            for u, ln in zip(exp["units"], lines)):
        raise PackError("tacli's wire writer does not emit at= and slot=: the packed lane needs it")
    return text


def pause_clause(batch: list, T: int, limit_ticks: int = L) -> str:
    """`pause at <T + the last cell's at + L> or cells ...`: each cell's start and end GameTimes and its
    two seat ranges with their end_units (tagpu_replay.c, the `or cells` clause; speed only, P5 is the
    authority)."""
    cells = sorted(batch, key=lambda c: c.k)
    parts = []
    for c in cells:
        start = T + c.at
        seats = ",".join(f"{s}:{lo}-{hi}<={n}" for s, lo, hi, n in c.ranges)
        parts.append(f"{start},{start + limit_ticks},{seats}")
    return f"pause at {T + cells[-1].at + limit_ticks} or cells " + ";".join(parts)


# ---- the corpus ----------------------------------------------------------------------------

def load_fights(paths, arena: dict, facts: dict) -> tuple:
    """({path: geometry with its block}, {path: why it is not packable}) for scenario files."""
    geoms, refused = {}, {}
    for p in paths:
        p = str(Path(p).resolve())
        doc, exp = perturb.compile_scenario(p)
        block = calibref.calib_block(doc, Path(p).name, arena)
        why = admissible(exp, block)
        if why is None and kind_of(block) != arena.get("synthmap", "land"):
            why = f"a {kind_of(block)} fight on the {arena.get('synthmap')} arena"
        if why:
            refused[p] = why
            continue
        g = fight_geometry(exp, facts)
        g["block"] = block
        geoms[p] = g
    return geoms, refused


def jobs_of(geoms: dict) -> list:
    """[(path, index)] for every run of every fight: its block's runs."""
    return [(p, i) for p in sorted(geoms) for i in range(geoms[p]["block"]["runs"])]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plan", help="the batches of a corpus on one arena")
    p.add_argument("--scenarios", required=True, help="a glob of fight files")
    p.add_argument("--arena", required=True)
    p.add_argument("--k", type=int, required=True)
    p.add_argument("--refs", required=True, help="the unpacked references the lengths come from")
    p.add_argument("--peaks", required=True, help='JSON {"<fight stem>": projectile peak}')
    p.add_argument("--viewed", type=int, default=SIGHT_CAP, help="the sight records' cap (the lever's N)")
    p.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    arena = calibref.arena(Path(a.arena))
    facts = install_facts()
    geoms, refused = load_fights(sorted(globmod.glob(a.scenarios)), arena, facts)
    prior = with_peaks(prior_from_refs(a.refs), json.loads(Path(a.peaks).read_text()))
    batches = plan(jobs_of(geoms), arena, a.k, default_bounds(arena, a.viewed), prior, geoms)
    if a.json:
        print(json.dumps({"refused": refused, "batches": [[dataclasses.asdict(c) for c in b] for b in batches]}))
    else:
        for why in sorted(set(refused.values())):
            print(f"not packed ({sum(1 for v in refused.values() if v == why)}): {why}")
        for i, b in enumerate(batches):
            print(f"batch {i}: {len(b)} cells, about {batch_length(b, prior)} ticks: "
                  + " ".join(f"{Path(c.job[0]).stem}#{c.job[1]}" for c in b))
    return 0


if __name__ == "__main__":
    sys.exit(main())
