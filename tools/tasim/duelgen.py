#!/usr/bin/env python3
"""duelgen.py -- the calibration duels and movement courses, generated from the install's roles.

    tools/tasim/duelgen.py [--gamedir DIR | --setup NAME] [--out DIR] [--dry-run]
    tools/tasim/duelgen.py --groups | --defence | --naval [--arena FILE]  [--gamedir DIR | --setup NAME] [--out DIR]
                           [--dry-run]

The roles come from `tasim_run roles` (tools/tasim/native.py), the one implementation of the role
rules: nothing here classifies. The scenarios go to <calibration dir>/scenarios/gen/ (calibref.py),
untracked, because they are made from the player's install; they regenerate deterministically.
--setup NAME reads a compat setup's folder (native.run's `setup`) and writes into that setup's own
calibration root, which calibref.calib_dir holds $TACALIB_DIR to.

DUELS. Each unordered pair of the land roles in play (raider, assault, artillery, scout, defence),
a role with itself included and defence against defence left out, at each tech level where both
roles have units, at three ranks of value (the cheapest, the median ⌊(n−1)/2⌋, the dearest, each
role's units sorted by value then name). Side A stands at the arena's `a`, side B along its axis
at the longer ground range + 160 world units; a defence is always side B, at its own range + 160
from the attacker. Each attacks the other. A pair where neither has a ground weapon is dropped,
an identical pair kept once, and a pair whose per-shot costs over the limit would spend more than
half the arena's storage is refused by name.

COURSES. For each land move class present (kbot, tank, hover, amphibious): eight of the class's
cheapest mobile non-builder (by value, then name) at the course's start, moved to its end; the
limit is three times the course at its speed.

THE FIGHTS ImpAI's fight predictor is fitted to and checked on (tools/impai/predfit.py), each chosen by a
random stream seeded with its own name (sha256 of the name), so a fight's content does not depend on which
others are made, and graded for the winner and the margin at GROUP_RUNS runs:
  --groups   GROUPS fights `group-NNN`: side A one to three types of the mobile land roles (raider, assault,
             artillery, scout), each at a rank of value, 3 to 12 units; side B the same at the value ratio
             RATIOS[NNN mod 5] to A as near as 3 to 12 units come; the tech levels in turn.
  --defence  DEFENCES fights `defence-NNN`: one to eight mobile land units of one or two types against one to
             three defence structures of one or two types of the same tech, B at its longest range + 160 (a
             structure whose range + 320 -- the room of side A's second row -- leaves the arena's rectangle,
             like a CORINT, is in none).
  --naval    NAVAL fights `naval-NNN`: --groups' rule for the sea roles, on the water arena --arena names
             (default tools/tasim/calib-arena-naval.json); calib.py engine plays them with the same --arena.
Each side stands in rows of four across the arena's axis, a unit's step 16 x (its footprint + 1) + 16 world
units, side A's front row at the arena's a and side B's at the longer ground range + 160 beyond it (a
defence's own range + 160). Each unit with a mover attacks every enemy the resolver lets it attack, nearest
first by the squared distance between the scenario positions (ties to the lower index), as one TA queue: the
second and later orders carry `"queue": true`, so the engine starts the next attack the tick the head's target dies
(Attack_Chase 0x4034A0 returns 5 on a cleared target); no idle unit re-acquires past its first slot's reach
(Standby 0x405FE0, finder mode 0 0x40B7B0, can-engage 0x49ABB0). A unit without a mover -- a defence structure --
gets no order and keeps the engine's standing Guard_NoMove 0x4021F0, as in real play: queued, its attack
Attack_NoMove 0x402160 drops an out-of-range head (AutoAim raises 0x1000 at 0x49E53A, state 2 at 0x402191 returns 9,
0x43B8E5 frees a node with a successor), so a nearest-first queue would drain to its farthest target and retry it
every 30 to 59 ticks, never reaching the guard. A fight neither side can hurt or order is dropped, one whose shots
would spend more than half the storage or that leaves the arena's rectangle refused by name. --groups, --defence and --naval write only their own kind; without them the duels and
courses are made. write_fight is the one placement every kind uses, for a generator of other armies.
"""

import argparse
import hashlib
import json
import math
import os
import random
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
import calibref  # noqa: E402
import native  # noqa: E402

ROLES = ("raider", "assault", "artillery", "scout", "defence")
RANKS = ("cheapest", "median", "dearest")
REACH = 160                     # world units beyond the longer ground range
LIMIT = 5400                    # three minutes at 30 ticks
RUNS = 50
COURSE_CLASSES = ("kbot", "tank", "hover", "amphibious")
COURSE_UNITS = 8
COURSE_LIMIT_FACTOR = 3
FIGHT_ROLES = ("raider", "assault", "artillery", "scout")
RATIOS = (0.5, 0.75, 1.0, 1.33, 2.0)
GROUPS, NAVAL, DEFENCES = 150, 40, 30
GROUP_RUNS = 25
ROW = 4                         # units a row, across the axis
GROUP_MAX = 12                  # units a group side holds at most
DEFENCE_MAX = 3                 # defence structures a defence fight holds at most
NAVAL_ARENA = calibref.NAVAL_ARENA
FIGHT_METRICS = ["winner", "ttk", "survivors", "margin"]


class GenError(Exception):
    pass


def pairs():
    out = []
    for i, r1 in enumerate(ROLES):
        for r2 in ROLES[i:]:
            if r1 == r2 == "defence":
                continue
            out.append((r1, r2))
    return out


def pick(units, rank):
    s = sorted(units, key=lambda u: (u["value"], u["name"]))
    return s[{"cheapest": 0, "median": (len(s) - 1) // 2, "dearest": len(s) - 1}[rank]]


# THE ATTACK ORDER'S RULE. A scenario's `attack` goes through the engine's action resolver 0x43F0E0 in mode 3
# (tagpu_scenario.c issue_one, ScriptAction_Type2Index), which answers 0 -- the applier then refuses the order --
# for a unit and an enemy target by these tests (DISASSEMBLED, 0x43F154..0x43F39A; the table is
# research/notes/engine/orders.md's mode 3): def +0x245 bit 4 `canattack` clear; no weapon (+0x110 bit 31, def
# +0x241 bit 16) and not `kamikaze` (+0x241 bit 28); weapon slot 0 `toairweapon` (+0x111 bit 17) against a target
# that is not airborne; a SUBMERGED target (its y + def +0x170 below the sea) when neither slot 0 nor a present
# slot 1 is a `waterweapon` (+0x111 bit 16); a `canhover` unit (+0x241 bit 12) with a waterweapon in slot 0 or 1
# against a target that is not submerged. A flyer (`canfly`, +0x241 bit 11) takes branches no fight here needs, so
# no flyer gets an order. Every bit is read from `tasim_run defs`' images of the install, the engine's own
# definitions (T1a's defcheck holds them equal field by field).
# Whether a sea target is submerged depends on its model's top, which the images do not carry, and on where it
# stands when the order is issued, a few ticks after its create. The creates place a unit with a mover through
# 0x48A870 (research/notes/engine/units.md, the hover wave): an `upright` def (+0x241 bit 20) at the terrain
# height, which at sea is the sea floor; else a `floater` (bit 19) at the sea level less its waterline (+0x22C);
# else fitted to the terrain under its footprint. So a floater that is not upright, of waterline 0, stands at the
# sea and is never submerged [INFERRED: every model's top is above its origin]; a land target stands on dry
# ground above the sea. Any other target may be submerged -- Total Mayhem's and TA Zero's submarines are upright
# of waterline 0, and the resolver refused every attack a ship without a waterweapon was given on one (MEASURED
# 2026-11-14, their naval fights) -- and an order on it is issued only when the resolver takes it either way: a
# waterweapon in slot 0 or 1 and no `canhover`.
F241_CANFLY, F241_CANHOVER, F241_WEAPON, F241_KAMIKAZE = 1 << 11, 1 << 12, 1 << 16, 1 << 28
F241_FLOATER, F241_UPRIGHT = 1 << 19, 1 << 20
F245_CANATTACK = 1 << 4
WF_TOAIR, WF_WATER = 1 << 17, 1 << 16
SLOT_KEYS = ("0x1EE", "0x1F2")              # weapon1 and weapon2: slots 0 and 1


def attack_facts(defs_doc) -> dict:
    """{unitname: the resolver's inputs} from `tasim_run defs`: each flag word and the two slots' weapon flags,
    every bit the rule reads checked to be in its row's mask (else GenError: the images do not say)."""
    weapons = {w["section"].upper(): w for w in defs_doc["weapons"]}

    def word(entry, off, bits):
        for row in entry["image"]:
            if row[0] == off:
                if row[3] != -1 and (row[3] & bits) != bits:
                    raise GenError(f"{entry.get('unitname') or entry.get('section')}: +0x{off:X}'s image "
                                   f"leaves out bits 0x{bits:X}")
                return int.from_bytes(bytes.fromhex(row[2]), "little")
        raise GenError(f"{entry.get('unitname') or entry.get('section')}: no +0x{off:X} in tasim_run's image")

    out = {}
    for u in defs_doc["units"]:
        slots = []
        for key in SLOT_KEYS:
            sec = (u["weapons"].get(key) or "").upper()
            slots.append(word(weapons[sec], 0x111, WF_TOAIR | WF_WATER) if sec in weapons else None)
        img = {row[0]: row for row in u["image"]}
        out[u["unitname"].upper()] = {
            "f241": word(u, 0x241, F241_CANFLY | F241_CANHOVER | F241_WEAPON | F241_KAMIKAZE | F241_FLOATER
                         | F241_UPRIGHT),
            "f245": word(u, 0x245, F245_CANATTACK),
            "slots": slots,
            "waterline": int.from_bytes(bytes.fromhex(img[0x22C][2]), "little") if 0x22C in img else None,
            "bmcode": word(u, 0x22F, 0xFF)}
    return out


def has_mover(u) -> bool:
    """The creates give a unit a mover only when def +0x22F (bmcode) is 1 (0x4860BB, 0x4862CF); a type with none
    is a structure, whose attack is Attack_NoMove (the resolver's mode 3, 0x43F348..0x43F375)."""
    f = u.get("attack")
    return bool(f) and f["bmcode"] == 1


def never_submerged(t) -> bool:
    """A land target, or a sea target placed at the sea: a floater, not upright, of waterline 0 (the rule above)."""
    if t["layer"] == "land":
        return True
    f = t["attack"]
    return f["waterline"] == 0 and f["f241"] & (F241_FLOATER | F241_UPRIGHT) == F241_FLOATER


def takes_attack(u, t) -> bool:
    """The resolver's mode 3 answers an action for unit u on the enemy t (the rule above)."""
    f = u.get("attack")
    if f is None or t.get("attack") is None:
        return False
    if not f["f245"] & F245_CANATTACK or f["f241"] & F241_CANFLY:
        return False
    if not f["f241"] & F241_WEAPON:
        return bool(f["f241"] & F241_KAMIKAZE)
    s0, s1 = f["slots"]
    if s0 is not None and s0 & WF_TOAIR:
        return False
    water = any(w is not None and w & WF_WATER for w in (s0, s1))
    hover_water = bool(f["f241"] & F241_CANHOVER) and water
    if never_submerged(t):
        return not hover_water
    return water and not hover_water


def storage_ok(arena_scn, u, n=1):
    """n of the unit's per-shot costs over the limit fit in half of each seat's storage."""
    st = min(p["storage"][k] for p in arena_scn["setup"]["players"] for k in ("metal", "energy"))
    m, e = u["shot_cost_per_tick"]
    return max(m, e) * n * LIMIT <= st / 2


def duel(u1, u2, r1, r2, t, rank, arena, arena_scn):
    if r1 == "defence":
        u1, u2, r1, r2 = u2, u1, r2, r1
    dist = (u2["range_ground"] if r2 == "defence" else max(u1["range_ground"], u2["range_ground"])) + REACH
    a, b = list(arena["a"]), calibref.arena_point(arena, dist)
    if not calibref.in_flat(arena, b):
        raise GenError(f"{u1['name']} against {u2['name']}: {dist} world units leave the flat area")
    for u in (u1, u2):
        if not storage_ok(arena_scn, u):
            raise GenError(f"{u['name']}: its shots over {LIMIT} ticks spend more than half the storage")
    fa, fb = calibref.arena_facing(arena)
    name = f"duel-t{t}-{r1}-{r2}-{rank}"
    if not takes_attack(u1, u2) and not takes_attack(u2, u1):
        raise GenError(f"{u1['name']} against {u2['name']}: neither can be ordered to attack the other")
    return name, {
        "format": "ta-scenario/1",
        "description": f"Generated by tools/tasim/duelgen.py: {u1['name']} ({r1}) against "
                       f"{u2['name']} ({r2}), tech {t}, the {rank} of each, {dist} world units apart.",
        "units": [{"id": "a", "type": u1["name"], "owner": 0, "pos": a, "facing": fa,
                   **attack_orders(u1, u2, "b")},
                  {"id": "b", "type": u2["name"], "owner": 1, "pos": b, "facing": fb,
                   **attack_orders(u2, u1, "a")}],
        "calib": {"kind": ["winrate", "distribution"], "metrics": FIGHT_METRICS,
                  "runs": RUNS, "limit": LIMIT, "end_units": {"0": 0, "1": 0},
                  "area": f"duel/land/t{t}/{r1}-{r2}"}}


def attack_orders(u, t, target_id) -> dict:
    """The unit's `orders`: an attack on t when the resolver takes it, else none (the unit holds its ground)."""
    return {"orders": [{"cmd": "attack", "target": target_id}]} if takes_attack(u, t) else {}


def rng_of(name: str) -> random.Random:
    return random.Random(int(hashlib.sha256(name.encode()).hexdigest()[:16], 16))


def step_of(u) -> int:
    return 16 * (max(u["footprint"]) + 1) + 16


def rows(arena, units, front: int, toward_b: bool) -> list:
    """[(along, across)] of each unit of `units` (a list of role rows, one a unit) in rows of ROW across the
    axis, the first row at `front` and the next ones away from the other side."""
    step = max(step_of(u) for u in units)
    out = []
    for k in range(len(units)):
        r, c = divmod(k, ROW)
        out.append((front + (r * step if toward_b else -r * step), round((c - (ROW - 1) / 2) * step)))
    return out


def queue_orders(u, upos, them, tpos, other) -> list:
    """Unit u's orders at upos: an attack on every enemy of `them` (at `tpos`, handles `other` + index) the
    resolver lets it attack, nearest first by the squared distance between the scenario positions, ties to the
    lower index; the second and later carry `queue`. Empty when it can attack none or has no mover."""
    if not has_mover(u):
        return []
    near = sorted((t for t in range(len(them)) if takes_attack(u, them[t])),
                  key=lambda t: ((tpos[t][0] - upos[0]) ** 2 + (tpos[t][1] - upos[1]) ** 2, t))
    return [dict({"cmd": "attack", "target": f"{other}{t}"}, **({"queue": True} if i else {}))
            for i, t in enumerate(near)]


def write_fight(name, side_a, side_b, arena, arena_scn, desc, area, gap=None, runs=GROUP_RUNS):
    """The scenario of side_a (owner 0) against side_b (owner 1), each a list of (role row, count): A's front
    row at the arena's a, B's at the longer ground range + REACH beyond (or `gap`). Each unit with a mover
    attacks every enemy the resolver lets it attack (takes_attack), nearest first, as one TA queue
    (queue_orders); a structure, or a unit with no such enemy, gets no order. GenError when no unit can be ordered to attack, a position
    leaves the arena's rectangle or the shots spend more than half the storage."""
    ua = [u for u, n in side_a for _ in range(n)]
    ub = [u for u, n in side_b for _ in range(n)]
    if not ua or not ub:
        raise GenError(f"{name}: a side has no unit")
    if all(u["dps_ground"] == 0 for u in ua + ub):
        raise GenError(f"{name}: neither side can hurt the other")
    if not any(has_mover(u) and takes_attack(u, t) for us, ts in ((ua, ub), (ub, ua)) for u in us for t in ts):
        raise GenError(f"{name}: no unit can be ordered to attack the other side")
    for u, n in side_a + side_b:
        if not storage_ok(arena_scn, u, n):
            raise GenError(f"{name}: {n} {u['name']}'s shots over {LIMIT} ticks spend more than half the storage")
    if gap is None:
        gap = max(u["range_ground"] for u in ua + ub) + REACH
    depth_a = (math.ceil(len(ua) / ROW) - 1) * max(step_of(u) for u in ua)
    pa = rows(arena, ua, depth_a, False)
    pb = rows(arena, ub, depth_a + gap, True)
    fa, fb = calibref.arena_facing(arena)
    at = {}
    for side, us, ps in (("a", ua, pa), ("b", ub, pb)):
        at[side] = []
        for u, (along, across) in zip(us, ps):
            pos = calibref.arena_point(arena, along, across)
            if not calibref.in_flat(arena, pos):
                raise GenError(f"{name}: {u['name']} at {pos} leaves the arena's rectangle")
            at[side].append(pos)
    units = []
    for side, us, face, other, them in (("a", ua, fa, "b", ub), ("b", ub, fb, "a", ua)):
        for k, u in enumerate(us):
            row = {"id": f"{side}{k}", "type": u["name"], "owner": 0 if side == "a" else 1, "pos": at[side][k],
                   "facing": face}
            orders = queue_orders(u, at[side][k], them, at[other], other)
            if orders:
                row["orders"] = orders
            units.append(row)
    return name, {
        "format": "ta-scenario/1",
        "description": f"Generated by tools/tasim/duelgen.py: {desc}, {gap} world units front to front.",
        "units": units,
        "calib": {"kind": ["winrate", "distribution"], "metrics": FIGHT_METRICS, "runs": runs, "limit": LIMIT,
                  "end_units": {"0": 0, "1": 0}, "area": area}}


def side_desc(side) -> str:
    return " + ".join(f"{n} {u['name']}" for u, n in side)


def roles_of(side) -> str:
    return "+".join(sorted({u["role"] for u, _n in side}))


def pick_side(r: random.Random, pools: dict, ntypes: int) -> list:
    """ntypes distinct types: a role of the pool's and a rank each."""
    out, seen = [], set()
    for _ in range(ntypes * 4):
        if len(out) == ntypes:
            break
        role = r.choice(sorted(pools))
        u = pick(pools[role], r.choice(RANKS))
        if u["name"] not in seen:
            seen.add(u["name"])
            out.append(u)
    return out


def split(r: random.Random, n: int, k: int) -> list:
    """n units over k types, each at least one."""
    cuts = sorted(r.sample(range(1, n), k - 1)) if k > 1 else []
    return [b - a for a, b in zip([0] + cuts, cuts + [n])]


def group_fight(name, pools, ratio, arena, arena_scn, layer, t):
    r = rng_of(name)
    ta = pick_side(r, pools, r.randint(1, 3))
    tb = pick_side(r, pools, r.randint(1, 3))
    side_a = list(zip(ta, split(r, r.randint(max(3, len(ta)), GROUP_MAX), len(ta))))
    va = sum(u["value"] * n for u, n in side_a)
    weights = [r.random() + 0.25 for _ in tb]
    best = None
    for nb in range(max(3, len(tb)), GROUP_MAX + 1):
        counts = [max(1, round(nb * w / sum(weights))) for w in weights]
        vb = sum(u["value"] * n for u, n in zip(tb, counts))
        miss = abs(math.log(vb / va / ratio))
        if best is None or miss < best[0]:
            best = (miss, counts, vb)
    side_b = list(zip(tb, best[1]))
    desc = (f"{side_desc(side_a)} against {side_desc(side_b)}, tech {t}, value ratio {best[2] / va:.2f} "
            f"(wanted {ratio})")
    return write_fight(name, side_a, side_b, arena, arena_scn, desc,
                       f"{name.split('-')[0]}/{layer}/t{t}/{roles_of(side_a)}-{roles_of(side_b)}")


def defence_fight(name, mobile, defences, arena, arena_scn, t):
    r = rng_of(name)
    ta = pick_side(r, mobile, r.randint(1, 2))
    tb = pick_side(r, {"defence": defences}, r.randint(1, 2))
    side_a = list(zip(ta, split(r, r.randint(max(1, len(ta)), 8), len(ta))))
    side_b = list(zip(tb, split(r, r.randint(max(1, len(tb)), DEFENCE_MAX), len(tb))))
    gap = max(u["range_ground"] for u, _n in side_b) + REACH
    return write_fight(name, side_a, side_b, arena, arena_scn,
                       f"{side_desc(side_a)} against {side_desc(side_b)}, tech {t}",
                       f"defence/land/t{t}/{roles_of(side_a)}-defence", gap=gap)


def can_be_ordered(u) -> bool:
    """The type takes an attack order on a land target or a ship at the sea (takes_attack), or on any other
    target: a type that takes none -- a spy, a radar or jammer, a flak gun -- adds value to a side and never
    fights, so it is in no fight's pool."""
    land = {"layer": "land", "attack": {"waterline": 0, "f241": 0}}
    sub = {"layer": "sea", "attack": {"waterline": 20, "f241": 0}}
    return takes_attack(u, land) or takes_attack(u, sub)


def fight_pools(roles_doc, layer, arena_scn):
    """{tech: {role: [units]}} of the mobile fight roles on `layer`, the techs with any: the types a side of
    GROUP_MAX of them can arm for the whole limit on half the storage (a unit paying for its shots, like a
    CORKROG, would otherwise stop firing mid-fight for want of energy, which the model has no term for)."""
    out = {}
    for u in roles_doc["units"]:
        if (u["layer"] == layer and u["tech"] >= 1 and u["role"] in FIGHT_ROLES and "mobile" in u["flags"]
                and storage_ok(arena_scn, u, GROUP_MAX) and can_be_ordered(u)):
            out.setdefault(u["tech"], {}).setdefault(u["role"], []).append(u)
    return out


def generate_fights(roles_doc, kind, arena, arena_scn, count=None):
    """([(name, scenario)], [(name, why) refused]) of one kind: groups, naval or defence."""
    layer = "sea" if kind == "naval" else "land"
    pools = fight_pools(roles_doc, layer, arena_scn)
    techs = sorted(pools)
    made, refused = [], []
    if not techs:
        return made, [(kind, f"no mobile {layer} unit of a fight role")]
    if kind == "defence":
        defences = {}
        for u in roles_doc["units"]:
            if (u["layer"] == "land" and u["role"] == "defence" and u["tech"] >= 1 and u["dps_ground"] > 0
                    and storage_ok(arena_scn, u, DEFENCE_MAX) and can_be_ordered(u)
                    and calibref.in_flat(arena, calibref.arena_point(arena, u["range_ground"] + 2 * REACH, 0))):
                defences.setdefault(u["tech"], []).append(u)
        techs = [t for t in techs if t in defences]
        if not techs:
            return made, [(kind, "no defence structure of a tech with mobile units")]
    n = count if count is not None else {"groups": GROUPS, "naval": NAVAL, "defence": DEFENCES}[kind]
    prefix = {"groups": "group", "naval": "naval", "defence": "defence"}[kind]
    for i in range(n):
        name, t = f"{prefix}-{i:03d}", techs[i % len(techs)]
        try:
            if kind == "defence":
                made.append(defence_fight(name, pools[t], defences[t], arena, arena_scn, t))
            else:
                made.append(group_fight(name, pools[t], RATIOS[i % len(RATIOS)], arena, arena_scn, layer, t))
        except GenError as e:
            refused.append((name, str(e)))
    return made, refused


def course(cls, units, arena):
    start, end = arena["courses"][cls]
    u = min(units, key=lambda u: (u["value"], u["name"]))
    if u["speed"] <= 0:
        raise GenError(f"course {cls}: {u['name']} has no speed")
    step = 16 * (max(u["footprint"]) + 1)
    length = math.hypot(end[0] - start[0], end[1] - start[1])
    out = []
    for k in range(COURSE_UNITS):
        row, col = divmod(k, 4)
        pos = [start[0] - row * step, start[1] + (col - 1.5) * step]
        out.append({"id": f"m{k}", "type": u["name"], "owner": 0, "pos": [round(pos[0]), round(pos[1])],
                    "facing": calibref.arena_facing(arena)[0], "orders": [{"cmd": "move", "to": list(end)}]})
    limit = math.ceil(length / u["speed"] * COURSE_LIMIT_FACTOR)
    return f"course-{cls}", {
        "format": "ta-scenario/1",
        "description": f"Generated by tools/tasim/duelgen.py: {len(out)} {u['name']}, the cheapest "
                       f"{cls}, moved {round(length)} world units across the arena.",
        "units": out,
        "calib": {"kind": ["distribution"], "metrics": ["travel.first", "travel.median", "travel.last"],
                  "runs": RUNS, "limit": limit, "area": f"course/land/{cls}"}}


def generate(roles_doc, arena, arena_scn):
    """([(name, scenario)], [(what, why) refused], the pair list for --dry-run)."""
    land = [u for u in roles_doc["units"] if u["layer"] == "land" and u["tech"] >= 1]
    by = {}
    for u in land:
        by.setdefault((u["role"], u["tech"]), []).append(u)
    techs = sorted({u["tech"] for u in land})
    out, refused, listed, seen = [], [], [], set()
    for r1, r2 in pairs():
        for t in techs:
            l1, l2 = by.get((r1, t)), by.get((r2, t))
            if not l1 or not l2:
                continue
            for rank in RANKS:
                u1, u2 = pick(l1, rank), pick(l2, rank)
                if u1["dps_ground"] == 0 and u2["dps_ground"] == 0:
                    continue
                key = (u1["name"], u2["name"])
                if key in seen:
                    continue
                seen.add(key)
                listed.append((t, r1, r2, rank, u1["name"], u2["name"]))
                try:
                    out.append(duel(u1, u2, r1, r2, t, rank, arena, arena_scn))
                except GenError as e:
                    refused.append((f"duel-t{t}-{r1}-{r2}-{rank}", str(e)))
    for cls in COURSE_CLASSES:
        movers = [u for u in land if u["move"] == cls and u["role"] != "builder"
                  and "mobile" in u["flags"]]
        if movers:
            try:
                c = course(cls, movers, arena)
            except GenError as e:
                refused.append((f"course-{cls}", str(e)))
                continue
            out.append(c)
            listed.append(("course", cls, "", "", ",".join(sorted({u["type"] for u in c[1]["units"]})), ""))
    return out, refused, listed


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    p.add_argument("--gamedir")
    p.add_argument("--out", help="default: <calibration dir>/scenarios/gen")
    p.add_argument("--dry-run", action="store_true", help="the list and its count only")
    p.add_argument("--groups", action="store_true", help="the group fights")
    p.add_argument("--defence", action="store_true", help="the defence fights")
    p.add_argument("--naval", action="store_true", help="the naval fights, on --arena")
    p.add_argument("--arena", help=f"the naval fights' water arena (default {NAVAL_ARENA.name})")
    p.add_argument("--setup", help="a compat setup (tools/compat/setups.json): the roles of its folder, the "
                                   "scenarios into its calibration root (TACALIB_DIR exactly "
                                   "<main checkout>/.data/calib-mods/<setup>)")
    a = p.parse_args(argv)
    if a.setup and a.gamedir:
        p.error("--setup reads the setup's own folder: no --gamedir with it")
    if a.setup:
        os.environ[calibref.SETUP_ENV] = a.setup
    try:
        calibref.calib_dir()            # THE ROOT INVARIANT, before anything is generated
    except calibref.CalibError as e:
        print(f"duelgen: could not run: {e}")
        return 2
    rc, text, err = native.roles(a.gamedir, setup=a.setup)
    if rc:
        print(f"duelgen: tasim_run roles could not run ({rc}): {err.strip()[-300:]}")
        return 2
    roles_doc = json.loads(text)
    rc, text, err = native.run("defs", a.gamedir, setup=a.setup)
    if rc:
        print(f"duelgen: tasim_run defs could not run ({rc}): {err.strip()[-300:]}")
        return 2
    try:
        facts = attack_facts(json.loads(text))
    except GenError as e:
        print(f"duelgen: could not run: {e}")
        return 2
    for u in roles_doc["units"]:
        u["attack"] = facts.get(u["name"].upper())
    try:
        arena = calibref.arena(calibref.ARENA)
    except calibref.CalibError as e:
        print(f"duelgen: could not run: {e}")
        return 2
    arena_scn = json.loads((TREE / arena["scenario"]).read_text())
    kinds = [k for k in ("groups", "defence", "naval") if getattr(a, k)]
    if kinds:
        made, refused = [], []
        for kind in kinds:
            ar, scn = arena, arena_scn
            if kind == "naval":
                try:
                    ar = calibref.arena(Path(a.arena) if a.arena else NAVAL_ARENA)
                except calibref.CalibError as e:
                    print(f"duelgen: could not run: {e}")
                    return 2
                scn = json.loads((TREE / ar["scenario"]).read_text())
            m, rf = generate_fights(roles_doc, kind, ar, scn)
            made += m
            refused += rf
        for name, doc in made:
            print(f"{name}  {doc['calib']['area']}  {len(doc['units'])} units")
        for name, why in refused:
            print(f"refused {name}: {why}")
        print(f"duelgen: {len(made)} fights ({', '.join(kinds)}), {len(refused)} refused")
    else:
        made, refused, listed = generate(roles_doc, arena, arena_scn)
        for row in listed:
            print("  ".join(str(x) for x in row if x != ""))
        for name, why in refused:
            print(f"refused {name}: {why}")
        print(f"duelgen: {sum(1 for n, _ in made if n.startswith('duel-'))} duels, "
              f"{sum(1 for n, _ in made if n.startswith('course-'))} courses, {len(refused)} refused")
    if a.dry_run:
        return 0
    out = Path(a.out) if a.out else calibref.gen_dir()
    out.mkdir(parents=True, exist_ok=True)
    for name, doc in made:
        (out / f"{name}.json").write_text(json.dumps(doc, indent=1) + "\n")
    (out / "catalogue.json").write_text(json.dumps({"units": [{"name": u["name"]} for u in
                                                               roles_doc["units"]]}) + "\n")
    print(f"wrote {len(made)} scenarios to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
