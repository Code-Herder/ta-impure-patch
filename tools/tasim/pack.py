#!/usr/bin/env python3
"""pack.py -- the packed calibration lane's planner: which runs share a batch, where each cell stands,
and the wire and pause clause that play it.

    pack.py plan --scenarios 'GLOB' --arena tools/tasim/calib-arena-packed.json --k 8 --refs DIR \\
                 [--viewed N] [--json]

A BATCH is one apply on one generated flat map (tools/tasim/synthmap.py). It holds K CELLS, each one
calibration job -- one (fight, index), with perturb.run_wire's perturbation, swap and seed for that
index -- translated into a site of its own on the map, far enough from every other that no unit, shot,
sensor or explosion of one reaches another, and walled off from every other by void cells that no
pathfinder search crosses. Cell k is created 30·k ticks after the apply (THE STAGGER) at block positions
P_k of each seat's block, so it sees the weapon scan and every schedule whose period divides 30 at the
phase an unpacked run sees them (autoheal's: below; the pathfinder's 150-call tick and the feature tick's
cursor keep no such phase). The argument, its conditions, the engine facts it rests on and the checks that
hold each batch to it afterwards (P1 to P5 and P4e, engtrace.py): research/notes/modules/tasim-calibration.md,
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
every cell is one height. Every box_k ⊕ (E + R_k) lies inside the arena's `flat` and outside its
`keepout`: the walls are invisible to every channel but pathing, so the far field keeps its rule.

THE SITES. The pathfinder has no bound tied to a reach -- a search reads pathing cells wherever its A*
frontier and bug walk go, and no engine cap ends one (the A* steps 0x40ED90..0x40EEE2 compare only the
pass cost with 100 at 0x40EEAF, engine/units.md) -- so no spacing bounds it. The map does instead: each
cell stands in a site of its own (synthmap.sites), and every cell outside the sites is void. The
invariant: a search started for a unit of cell k reads class-map entries only inside site k and on the
void cells that border it (synthmap.py, THE WALLS, with its engine facts). Its start is in site k by P1,
since the units stay in box_k ⊕ E and box_k ⊕ WALL_CLEAR lies inside the site; the entries it reads hold
only site k's interior, whose units are cell k's, and constant void. WALL_CLEAR = 640 is at least E
(320) + the largest footprint extent with its ring (72) + GRID, rounded up; site_problem asserts it
against each fight's own extent, so if E grows, WALL_CLEAR and the sites grow with it. Each fight
stands at its CANONICAL place in a site: its box's centre on the site's, rounded down to GRID. All sites
are one size and their origins multiples of GRID, so a cell's world, out to and including its walls, is
a translate of one world fixed by its fight in whatever batch and site it lands: a packed cell's search
is its K = 1 run's on the same map. `assign` takes the cells longest first, each to the first free site
that holds its box ⊕ WALL_CLEAR at the canonical place, keeps the far-field rule and keeps the gap rule
against every cell already placed; a site holds one cell. A search's pops are bounded by its site's cells,
which is more than a seat's credit in a tick, so engtrace's P3 still checks that searches finish within
the tick.

THE BOUNDS, each a refusal here and checked again offline: units a seat a cell <= the capacity; units a
batch <= 400; the cells' projectile bounds summed <= PROJ_MAX = 1500, half the pool's 3000 (THE
PROJECTILE BOUND); each seat's energy and metal spent over the batch's window <= half its storage; the
viewed seat's units <= the temporary sight records' cap (20 stock, the test lever's N with it: the one
list of the game, `cmp eax,0x14` at 0x48292C).

THE PROJECTILE BOUND is static, from the engine's own def images: no measurement, which would never be an
upper bound. A record lives at most shot_life ticks from its append to its compaction, and a slot holds
at most slot_records of them at any instant -- mid-tick and after its shooter's death included, since a
dead unit's shots fly on and the window counts them. The engine facts (DISASSEMBLED, engine/weapons.md):
one fire call per armed slot per unit tick and one append per call; a burst makes its parent and `burst`
copies, at most one copy a parent a tick, `burstrate` apart (0x49B776..0x49B9A9); the reload countdown
falls by one a tick while above 0, the slot fires only at 0, and after a shot it is
⌊⌊(100 - 6L)·reload/100⌋·(120 - ⌊20·hp/maxhp⌋)/100⌋ with L <= 5 (0x49E1C8..0x49E1D2,
0x49E48D..0x49E4EE), so a slot's fires are at least max(1, ⌊70·reload/100⌋) ticks apart at hp <= maxhp;
a line (bit 0 without bit 20) or selfprop (bit 20) shot, built by 0x49C9C0, and a vlaunch shot, built by
0x49CC20, die at GameTime + ⌊(range << 16) / weaponvelocity⌋, or + weapontimer with noautorange (bit 27)
or a velocity of 0 (0x49CB1C..0x49CB65, 0x49CCCE..0x49CD17); a line shot leaves at its death; a selfprop
shot past its death detonates with burnblow (bit 23), else falls under gravity (0x49BAC3..0x49BADE), a
twophase (bit 24) one flighttime later (0x49BB01..0x49BB18), its speed capped at weaponvelocity
(0x49BA16..0x49BA31); a ballistic shot has no death time and detonates on the first tick its altitude is
below the cell's height byte (0x49B36D..0x49B37D), launched at most at weaponvelocity (0x49CE8D..0x49CEC3)
with no vertical wind; a burst copy dies weapontimer or (distance + 16 wu) / speed after it is made, plus
up to randomdecay/2 (0x49B8A3..0x49B8D1). groundbounce (bit 15) reverses vy with no bound and dropped
(bit 8) is not mapped: a weapon of either has no bound, and a fight that arms one is refused, as is a
weapon of none of the mapped kinds (bit 0, 1 or 20) and a vlaunch (bit 4) one without selfprop.
engtrace (C6) holds a batch to the bound: P4a, for every iteration t, projcount (main+0x141F3) in its
image plus the most appends step t can make after it -- each live unit's armed slots and armed burst
slots, and the image's burst parents (+0x60 != 0) -- stays within the cap the line shot's append
compares with (the u32 at 0x49C9D2, 3000), which implies no append was refused; P4e, for every cell and
tick, the records whose attacker (+0x52) is a unit of the cell number no more than the cell's bound, so
going over means shot_life is wrong. Both fail the generation.

THE EXPLORED MAP. The pathfinder's map reader reads the shared mapped grid main+0x14273 and answers an
unexplored block passable whatever stands on it (0x40D817..0x40D831), so a cell's paths would depend on
what other cells explored. A packed fight plays with `mapping` on, which explores the whole map for both
seats from the start (MEASURED 2026-11-13 on both generated maps at the launch's hold: all 409 600
words of the grid hold seat 0's and seat 1's bits), and the planner refuses an arena or a fight without it.

THE PLAN is deterministic, a function of the jobs, K, the bounds, the sites and the prior (each fight's
median length in the unpacked references, for every mod). Round r takes index r of every fight; its fights,
longest first, go first-fit into the round's batches, K distinct fights at most; the round's last
batch, if it is short, is carried into the next round, which fills it only with fights it does not
hold. Within a batch cell 0 is the longest, so the batch ends near max_k(at_k + length_k).
"""

import argparse
import copy
import dataclasses
import glob as globmod
import json
import math
import statistics
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
import calibref  # noqa: E402
import perturb  # noqa: E402
import synthmap  # noqa: E402

PHASE = 30                      # ticks between two cells' creates: the 30-tick phases' period
PHASE_HEAL = 120                # lcm(30, 8): with a `healtime` type, autoheal's period 8 too (THE STAGGER)
GRID = synthmap.GRID            # world units: every offset a multiple (THE LAYOUT), every site origin too
FOOT_RING = 16                  # the pathing fill's ring of one 16-wu cell around a footprint (THE LAYOUT)
E = 320                        # world units: a unit's excursion beyond its box (P1)
WALL_CLEAR = 640                # world units from a cell's box to its site's walls (THE SITES)
F = 1.5                         # a shot's flight beyond the weapon's range (P2)
THREAT_R = 160                  # 0x40B1C0's radius at its one caller (0x407FC9..0x407FD0)
L = 5400                        # a fight's tick limit (duelgen.LIMIT)
UNITS_MAX = 400                 # units a batch
# the projectile pool's append cap: the u32 the line shot's append compares with (`cmp eax,imm32` at 0x49C9D1,
# its immediate at 0x49C9D2), which the raised limits write through lim_dword at every one of the ten appends
# (tagpu_patches.c); each lane-launch reads it back (engrun.pack_reads)
PROJ_CAP = 3000
PROJ_MAX = PROJ_CAP // 2        # summed projectile bounds a batch
STORAGE_FRAC = 0.5              # of each seat's storage, spent over the batch's window
# THE PROJECTILE BOUND: the highest a shot's fire point stands, world units [INFERRED: no fire point stands
# more than 257 above the highest height byte 255; a wrong value costs a P4e failure, never a silent pass]
ALT_MAX = 512
RELOAD_MIN_PCT = 70             # the reload's least share: (100 - 6·L) at L = 5 kills, at full health
SIGHT_CAP = 20                  # temporary sight records (0x48292C) without the lever
# the cap a packed lane's test lever tagpu_sightcap.on sets: the most the lever takes (21..127, the compare's
# sign-extended imm8, tagpu_patches.c fix_sight_cap), so the viewed seat's bound is the loosest the engine allows
LEVER_SIGHT_CAP = 127
# the metrics a cell's trace gives: the combat ones, which read its units alone. A seat's `p` rows (its
# storage, rates and economy passes) are the seat's, which every cell of a batch shares, so a cell's trace
# carries none and a fight graded on them is not packed
CELL_METRICS = ("winner", "ttk", "survivors", "margin")
ORDERS = ("attack", "move")
SEATS = (0, 1)
VIEWED = 0                      # the viewed seat: seat 0, human (scenarios/calib/arena.json)
# def offsets (tagpu/ddraw/src/tasim_keys.c): sight, radar, sonar, radar jam, sonar jam
SENSE_OFFS = (0x202, 0x204, 0x206, 0x20A, 0x20C)
WEAPON_SLOTS = ("0x1EE", "0x1F2", "0x1F6")
DEATH_SLOTS = ("0x220", "0x224")            # explodeas, selfdestructas
# weapon def offsets (tagpu/ddraw/src/tasim_keys.c): velocities 16.16 a tick, range and aoe world units,
# reload, timer, burstrate, randomdecay and flighttime ticks, and the flag word +0x111
W_RANGE, W_AOE, W_BURST = 0xDC, 0xD6, 0xEA
W_VELOCITY, W_STARTVELOCITY, W_RELOAD, W_TIMER = 0x68, 0x6C, 0xE4, 0xE6
W_BURSTRATE, W_DECAY, W_FLIGHTTIME, W_FLAGS = 0xEC, 0xF2, 0xFC, 0x111
WF_LINE, WF_BALLISTIC, WF_VLAUNCH, WF_DROPPED, WF_BOUNCE = 1 << 0, 1 << 1, 1 << 4, 1 << 8, 1 << 15
WF_SELFPROP, WF_BURNBLOW, WF_TWOPHASE = 1 << 20, 1 << 23, 1 << 24
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
    offset: tuple               # (dx, dz), multiples of GRID: the fight's canonical place in its site
    site: int                   # its index in synthmap.sites(the arena's kind)
    pos: dict                  # seat -> block position P_k
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


def _fall(v: float, h: float, g: float) -> int:
    """Ticks a body h world units up, rising at v a tick, takes to come down under g a tick squared."""
    return math.ceil((v + math.sqrt(v * v + 2 * g * max(h, 0))) / g)


def shot_life(w: dict, g16: int, ground_min: int, alt_max: int = ALT_MAX):
    """Ticks from a record's append to its compaction, at most, for weapon image `w`; None for a kind with
    no bound (THE PROJECTILE BOUND). g16 is the level's gravity main+0x14263, 16.16 a tick squared; ground_min
    the lowest height byte under any fight (synthmap.GROUND)."""
    flags = _int(w, W_FLAGS)
    if flags & (WF_DROPPED | WF_BOUNCE):
        return None
    if flags & WF_VLAUNCH and not flags & WF_SELFPROP:
        return None
    if not flags & (WF_LINE | WF_BALLISTIC | WF_SELFPROP):
        return None
    vel, start = _int(w, W_VELOCITY), _int(w, W_STARTVELOCITY)
    g = g16 / 65536
    if flags & (WF_LINE | WF_SELFPROP):
        # the death: the shot's and every burst copy's, the copy's randomdecay included
        reach = -(-(((_int(w, W_RANGE) + 16) << 16) & 0xFFFFFFFF) // vel) if vel else 0
        t1 = max(_int(w, W_TIMER), reach) + -(-_int(w, W_DECAY) // 2)
        if not flags & WF_SELFPROP:
            return t1 + 1
        tp = t1 + (_int(w, W_FLIGHTTIME) if flags & WF_TWOPHASE else 0)
        vmax = max(vel, start) / 65536                 # a powered shot's speed is capped (0x49BA16..0x49BA31)
        fall = 0 if flags & WF_BURNBLOW else _fall(vmax, alt_max + tp * vmax - ground_min, g)
        return tp + fall + 1
    return _fall(vel / 65536, alt_max - ground_min, g) + 1


def slot_records(w: dict, g16: int, ground_min: int, alt_max: int = ALT_MAX):
    """The most records one armed slot of weapon `w` holds at any instant, or None (shot_life's): every
    fire of the last W ticks, at least p apart, each a parent and `burst` copies."""
    life = shot_life(w, g16, ground_min, alt_max)
    if life is None:
        return None
    burst = _int(w, W_BURST)
    window = life + burst * _int(w, W_BURSTRATE) + 1
    p = max(1, RELOAD_MIN_PCT * _int(w, W_RELOAD) // 100)
    return (1 + burst) * (window // p + 1)


def unit_facts(defs_doc: dict, roles_doc: dict, g16: int = synthmap.ENGINE_GRAVITY) -> dict:
    """{UNITNAME: {sense, range, aoe, bmcode, shot_cost [metal, energy] a tick, upkeep energy a tick,
    foot (the larger footprint side, cells), foot_min (the smaller), heal (a nonzero healtime), slots (its
    armed weapon slots), bursts (those of them whose weapon has a `burst`), proj_records ({map kind: the sum
    of slot_records over its armed slots on that kind's ground}, None when a weapon has no bound), unbounded (why, or None: a weapon with no bound, or
    a weapon past the three slots)}} from `tasim_run defs` and `tasim_run roles` (native.py): the images
    of the engine's own definitions. range is over the three weapon slots, death weapons (explodeas,
    selfdestructas) only in aoe. A type with a weapon past the three slots (Impure's extra-weapons module,
    the defs' `slots` 3..9) is refused until these facts read them.
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
        armed = [w for w in slots if w]
        unbounded = None
        extra = [i for i, s in enumerate(u.get("slots") or []) if i >= len(WEAPON_SLOTS) and s >= 0]
        if extra:
            unbounded = f"a weapon in slot {extra[0] + 1}, past the three these facts read"
        records = {}
        for kind in synthmap.KINDS:
            per = [slot_records(w, g16, synthmap.GROUND[kind]) for w in armed]
            records[kind] = None if None in per else sum(per)
        if records["land"] is None:
            w = next(w for w in armed if shot_life(w, g16, 0) is None)
            unbounded = unbounded or f"weapon {w['section']}: a kind with no projectile bound"
        out[name] = {
            "sense": max(_int(u, o) for o in SENSE_OFFS),
            "range": max([_int(w, W_RANGE) for w in armed] or [0]),
            "aoe": max([_int(w, W_AOE) for w in slots + deaths if w] or [0]),
            "bmcode": _int(u, U_BMCODE),
            "shot_cost": list(shots.get(name, [0.0, 0.0])),
            "upkeep": (max(0.0, _float(u, U_ENERGYUSE)) + cloak) / PHASE,
            "foot": max(_int(u, U_FOOTX), _int(u, U_FOOTZ)),
            "foot_min": min(_int(u, U_FOOTX), _int(u, U_FOOTZ)),
            "heal": _int(u, U_HEALTIME) > 0,
            "slots": len(armed),
            "bursts": sum(1 for w in armed if _int(w, W_BURST)),
            "proj_records": None if unbounded else records,
            "unbounded": unbounded,
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

def admissible(exp: dict, block: dict = None, facts: dict = None, site: tuple = None):
    """None, or why the fight cannot be packed: an order other than attack and move (so no create
    happens in play and the requested slots stay unique), a feature (nothing to reclaim), an exact
    scenario (packed by its own stage, never by the plan), no end_units (a course: it plays alone),
    mapping off (THE EXPLORED MAP), a metric past CELL_METRICS, or a tick limit other than L (the batch's
    pause clause and window give every cell L). With the facts: a unit above 100 % health (the reload bound takes
    hp <= maxhp), a mobile type (bmcode not 0) of footprint below 1 (its entry would not cover its own cell,
    so a void cell's entry could be nonzero: THE SITES), a type whose weapons have no projectile bound
    (THE PROJECTILE BOUND). With a site as well: a fight whose box ⊕ WALL_CLEAR does not fit it at its
    canonical place (site_problem; every site of a map is one size)."""
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
        seat_wide = [m for m in block["metrics"] if m not in CELL_METRICS]
        if seat_wide:
            return f"metric {seat_wide[0]}: a cell's trace gives only {', '.join(CELL_METRICS)}"
        if block["limit"] != L:
            return f"a tick limit of {block['limit']}, the packed lane's is {L}"
    if facts is None:
        return None
    for u in exp["units"]:
        if (u.get("health") or 100) > 100:
            return f"unit {u['id']}: health {u['health']} %, above 100"
        f = facts.get(u["type"].upper())
        if f is None:
            return f"unit {u['id']}: no definition for {u['type']}"
        if f["bmcode"] != 0 and f["foot_min"] < 1:
            return f"{u['type'].upper()}: a mobile type of footprint {f['foot_min']}, below 1"
        if f["unbounded"]:
            return f"{u['type'].upper()}: {f['unbounded']}"
    if site is not None:
        return site_problem(fight_geometry(exp, facts, kind_of(block) if block else "land"), site)
    return None


def fight_geometry(exp: dict, facts: dict, kind: str = "land") -> dict:
    """The fight before translation: box (x0, z0, x1, z1) over every start position and order point,
    widened by the jitter; sense, range, aoe and reach over its types; extent, the farthest a footprint
    reaches past its centre with the pathing ring (THE LAYOUT); its units per owner; proj_records, the
    sum of its units' projectile bounds on the `kind` map's ground (THE PROJECTILE BOUND)."""
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
    unbounded = sorted(t for t in types if facts[t]["proj_records"] is None)
    if unbounded:
        raise PackError(f"no projectile bound for {', '.join(unbounded)} (admissible refuses them)")
    j = perturb.JITTER // 2
    sense = max(facts[t]["sense"] for t in types)
    rng = max(facts[t]["range"] for t in types)
    aoe = max(facts[t]["aoe"] for t in types)
    reach = int(-(-max(sense, F * rng + aoe, THREAT_R, aoe) // 1))
    return {"box": (min(xs) - j, min(zs) - j, max(xs) + j, max(zs) + j), "sense": sense, "range": rng,
            "aoe": aoe, "reach": reach, "units": per_owner, "types": sorted(types),
            "extent": max(8 * facts[t]["foot"] + 8 for t in types) + FOOT_RING,
            "heal": any(facts[t]["heal"] for t in types),
            "proj_records": sum(facts[u["type"].upper()]["proj_records"][kind] for u in exp["units"]),
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


def bounds_problem(members: list, geoms: dict, bounds: dict, K: int):
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
        proj += g["proj_records"]
    if units > bounds["units"]:
        return f"{units} units, past {bounds['units']}"
    if proj > bounds["proj"]:
        return f"projectile bounds {proj}, past {bounds['proj']}"
    if viewed > bounds["viewed"]:
        return f"{viewed} units of the viewed seat, past the sight records' cap {bounds['viewed']}"
    for s in SEATS:
        for r, what in ((0, "metal"), (1, "energy")):
            room = bounds["storage"][s][r] * bounds["storage_frac"]
            if spend[s][r] > room:
                return f"seat {s}'s {what} over {win} ticks {spend[s][r]:.0f}, past {room:.0f}"
    return None


# ---- the sites -----------------------------------------------------------------------------

def _grow(box, by):
    return (box[0] - by, box[1] - by, box[2] + by, box[3] + by)


def _overlaps(a, b) -> bool:
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


def _inside(a, b) -> bool:
    return b[0] <= a[0] and b[1] <= a[1] and a[2] <= b[2] and a[3] <= b[3]


def _shift(box, dx, dz):
    return (box[0] + dx, box[1] + dz, box[2] + dx, box[3] + dz)


def gap(a, b) -> int:
    """The separation of two boxes along the axis that separates them most; negative when they meet."""
    return max(b[0] - a[2], a[0] - b[2], b[1] - a[3], a[1] - b[3])


def separation(r1: int, x1: int, r2: int, x2: int) -> int:
    """The gap two cells' boxes ⊕ E must exceed: the larger reach and the larger footprint extent."""
    return max(r1, r2) + max(x1, x2)


def canonical(g, site) -> tuple:
    """(dx, dz): the box's centre on the site's, rounded down to GRID. dx - sx and dz - sz depend on the
    fight and the site's size alone, so every run of a fight stands at one place relative to its walls in
    every site (THE SITES)."""
    sx, sz, sw, sh = site
    b = g["box"]
    return (sx + GRID * ((sw - b[0] - b[2]) // (2 * GRID)),
            sz + GRID * ((sh - b[1] - b[3]) // (2 * GRID)))


def site_problem(g, site):
    """None, or why the fight cannot stand in `site`: WALL_CLEAR below E + its footprint extent + GRID
    (so a unit inside its box ⊕ E would stand within a footprint and a ring of the wall), or its box ⊕
    WALL_CLEAR at the canonical place not inside the site."""
    need = E + g["extent"] + GRID
    if WALL_CLEAR < need:
        return f"WALL_CLEAR {WALL_CLEAR} is below E + the footprint extent + GRID = {need}"
    sx, sz, sw, sh = site
    outer = _grow(_shift(g["box"], *canonical(g, site)), WALL_CLEAR)
    if not _inside(outer, (sx, sz, sx + sw, sz + sh)):
        b = g["box"]
        return (f"its box {b[2] - b[0]} x {b[3] - b[1]} with {WALL_CLEAR} of wall clearance does not fit "
                f"a site of {sw} x {sh}")
    return None


def assign(cells: list, arena: dict, sites: list):
    """{k: (site index, dx, dz)} for cells [(k, geometry)] in their batch's order (longest first), or None
    when they do not fit (THE SITES): first-fit, each cell into the first site not yet taken that holds it
    at its canonical place (site_problem), whose box ⊕ (E + R) lies inside the arena's flat region and
    outside its keepout, and that keeps the gap rule against every cell already placed."""
    fx, fz, fw, fh = arena["flat"]["rect"]
    flat = (fx, fz, fx + fw, fz + fh)
    kx, kz, kw, kh = arena.get("keepout") or (0, 0, 0, 0)
    keep = (kx, kz, kx + kw, kz + kh)
    out, placed = {}, []                # placed: (k, box ⊕ E, reach, extent)
    for k, g in cells:
        r, x = g["reach"], g["extent"]
        for i, site in enumerate(sites):
            if i in {s for s, _dx, _dz in out.values()} or site_problem(g, site):
                continue
            dx, dz = canonical(g, site)
            box = _shift(g["box"], dx, dz)
            outer = _grow(box, E + r)
            if not _inside(outer, flat) or _overlaps(outer, keep):
                continue
            bx = _grow(box, E)
            if any(gap(bx, pb) <= separation(r, x, pr, px) for _k, pb, pr, px in placed):
                continue
            out[k] = (i, dx, dz)
            placed.append((k, bx, r, x))
            break
        else:
            return None
    for i, (kj, bj, rj, xj) in enumerate(placed):
        for kk, bk, rk, xk in placed[i + 1:]:
            if gap(bj, bk) <= separation(rj, xj, rk, xk):
                raise PackError(f"the sites broke the gap rule between cells {kj} and {kk}")
    return out


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


def _length(prior, path) -> int:
    return min(L, int((prior.get(Path(path).stem) or {}).get("length", L)))


def _cells(members, geoms, arena, K, placed):
    """The batch's Cells, k = 0 the longest, each in its site at its canonical place."""
    phase = batch_phase(members, geoms)
    ps = positions(arena["unit_limit"], K, phase)
    out = []
    for k, (path, index) in enumerate(members):
        g = geoms[path]
        site, dx, dz = placed[k]
        box = _shift(g["box"], dx, dz)
        ranges = []
        n_of = dict(perturb.end_clauses(g["block"], index % 2 == 1))
        for owner in (0, 1):
            seat = seat_of(owner, index)
            ranges.append((seat, ps[k], ps[k] + g["units"][owner] - 1, n_of.get(seat, 0)))
        ranges.sort()
        out.append(Cell(job=(str(path), index), k=k, at=phase * k, offset=(dx, dz), site=site,
                        pos={s: ps[k] for s in SEATS}, box=box, reach=g["reach"],
                        zone=_grow(box, E + int(-(-F * g["range"] // 1))), ranges=ranges))
    return out


def _order(members, prior):
    return sorted(members, key=lambda m: (-_length(prior, m[0]), Path(m[0]).stem, m[1]))


def _fits(members, geoms, prior, arena, bounds, K):
    if bounds_problem(members, geoms, bounds, K):
        return None
    ordered = _order(members, prior)
    placed = assign([(k, geoms[p]) for k, (p, _i) in enumerate(ordered)], arena,
                    synthmap.sites(arena["synthmap"]))
    return None if placed is None else (ordered, placed)


def plan(jobs, arena: dict, K: int, bounds: dict, prior: dict, geoms: dict) -> list:
    """[[Cell]]: every job in exactly one batch (THE PLAN). `geoms` is {fight path: fight_geometry with
    its calib block under "block"}; a fight's prior has its length."""
    if not 1 <= K <= arena["unit_limit"] // PHASE or not capacity(arena["unit_limit"], K):
        raise PackError(f"K = {K}: 1..{arena['unit_limit'] // PHASE}")
    if arena.get("mapping") != 1:
        raise PackError("the arena's mapping is off (THE EXPLORED MAP)")
    if arena.get("synthmap") not in synthmap.KINDS:
        raise PackError("the arena is no generated map: a packed batch stands its cells in its sites")
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
        ordered, placed = _fits(members, geoms, prior, arena, bounds, K)
        out.append(_cells(ordered, geoms, arena, K, placed))
    return out


def cell_json(c: Cell) -> dict:
    """A Cell as a plan or a manifest row keeps it (cell_from_json reads it back): its fight's path under the
    tree, as a manifest names a scenario, so a run's folder reads back the same in every checkout."""
    d = dataclasses.asdict(c)
    p = Path(c.job[0])
    d["job"] = [str(p.relative_to(TREE)) if p.is_relative_to(TREE) else str(p), c.job[1]]
    return d


def cell_from_json(d: dict) -> Cell:
    """The Cell a cell_json gave: its fight's path back under this tree, JSON's lists back to tuples and its
    seat keys to ints."""
    p = Path(d["job"][0])
    return Cell(job=(str(p if p.is_absolute() else TREE / p), d["job"][1]), k=d["k"], at=d["at"],
                offset=tuple(d["offset"]), site=d["site"],
                pos={int(s): v for s, v in d["pos"].items()}, box=tuple(d["box"]), reach=d["reach"],
                zone=tuple(d["zone"]), ranges=[tuple(r) for r in d["ranges"]])


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
    """({path: geometry with its block}, {path: why it is not packable}) for scenario files on a
    generated arena; every site of its map is one size, so the first stands for all in admissible."""
    if arena.get("synthmap") not in synthmap.KINDS:
        raise PackError("the arena is no generated map: a packed batch stands its cells in its sites")
    kind = arena["synthmap"]
    site = synthmap.sites(kind)[0]
    geoms, refused = {}, {}
    for p in paths:
        p = str(Path(p).resolve())
        doc, exp = perturb.compile_scenario(p)
        block = calibref.calib_block(doc, Path(p).name, arena)
        why = None if kind_of(block) == kind else f"a {kind_of(block)} fight on the {kind} arena"
        why = why or admissible(exp, block, facts, site)
        if why:
            refused[p] = why
            continue
        g = fight_geometry(exp, facts, kind)
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
    p.add_argument("--viewed", type=int, default=SIGHT_CAP, help="the sight records' cap (the lever's N)")
    p.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    arena = calibref.arena(Path(a.arena))
    facts = install_facts()
    geoms, refused = load_fights(sorted(globmod.glob(a.scenarios)), arena, facts)
    prior = prior_from_refs(a.refs)
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
