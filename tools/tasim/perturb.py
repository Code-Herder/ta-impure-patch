"""perturb.py -- a calibration run's wire: the scenario, reset to the arena's empty point and perturbed.

    exp, record, wire = perturb.run_wire(path, index, values, arena_cfg)

One module makes every run's wire, with tacli's own compiler and wire writer, so the engine runner
(engrun.py) applies and tasim reads the same bytes. The checker calls it again and refuses a
reference whose recorded perturbation differs: that means this module changed and the reference
is stale (research/notes/modules/tasim-calibration.md, *Perturbations*).

THE DRAWS, in this order, from tacli's xorshift32 (_ScnRandom) seeded with run_seed:
  1. for each unit of the expanded scenario, in ordinal order, if its type is mobile (the value
     table's bmcode is not 0: the engine places a structure on cells, so a structure draws
     nothing): dx = r % 16 - 8, then dy = r % 16 - 8, world units added to its position;
  2. then for each order without `queue`, in wire order (unit by unit, each unit's orders in turn):
     r % 16 ticks added to its `after` and to that of each queued order after it. A unit's queue
     waits together, so the engine receives it in order and R8 sees one list change; a file with no
     queued order draws exactly as it did before the key existed, so its wires are unchanged.
On an odd index the seats swap: owner 0 <-> 1 in every unit, and the end clauses with them. Side A
is the scenario's owner 0 and side B its owner 1 whatever seat plays them, so every metric is per
side. An exact scenario is never perturbed: every index is the file as written.

THE RUN'S SEED is the wire's (`v1 seed=`): the applier's clearing apply seeds the simulation RNG, the game
thread's CRT stream, the wind and the meteors from it under the harness's switch tagpu_seeds.on, which every
tareplay Worker writes (tagpu_patches.c, A RUN'S SEEDS), so a run's draws are a function of its scenario and
its index whatever its lane played before it. The reference records them as each run's exogenous inputs.

THE FACING. Every unit of a wire names its facing: the scenario's, or 0 (the heading 0x8000) where
it names none. The create draws a structure's heading from the simulation RNG, 0x8000 - BuildAngle/2
+ rand(BuildAngle) with BuildAngle the def's word +0x210 (0x485BD6..0x485C06; a mobile unit's is
then overwritten with that word, 0x4860BB..0x4860ED), so a structure without a facing faces where
the launch's random stream says, and its turret's first aim, which turns from that heading, takes
longer or shorter (MEASURED 2026-11-12, the exact scenario's ARMLLT: headings 0x6E49 and 0x7054
fired first at t = 16, 0x61D0 at t = 18, and every later shot kept the offset). The applier writes
a named facing after the create (tagpu_scenario.c dress_unit), so the heading is the wire's in
every run, fresh or reload.
"""

import copy
import hashlib
import json
import sys
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
import calibref  # noqa: E402

_SPEC = spec_from_loader("_perturb_cli", SourceFileLoader("_perturb_cli", str(TREE / "tools" / "tacli")))
CLI = module_from_spec(_SPEC)
sys.modules.setdefault(_SPEC.name, CLI)
_SPEC.loader.exec_module(CLI)

JITTER = 16                     # dx, dy in [-8, 7]: within one footprint cell of 16 world units
STAGGER = 16                    # an order's added after in [0, 15] ticks


class PerturbError(Exception):
    pass


def run_seed(scn_sha256: str, index: int) -> int:
    """The first four bytes, little endian, of sha256("<scenario sha256>:<index>")."""
    return int.from_bytes(hashlib.sha256(f"{scn_sha256}:{index}".encode()).digest()[:4], "little")


def mobile(values: dict, type_name: str) -> bool:
    row = values.get(type_name.upper())
    if row is None:
        raise PerturbError(f"{type_name}: not in the value table")
    return row["bmcode"] != 0


def perturb(exp: dict, index: int, values: dict, seed: int, exact: bool = False):
    """(a perturbed copy of tacli's expanded form, the record of what was drawn)."""
    out = copy.deepcopy(exp)
    out["seed"] = seed
    afters = [o for u in out["units"] for o in u["orders"]]
    if exact:
        return out, {"seed": seed, "swap": False, "jitter": {}, "after": [0] * len(afters)}
    rnd = CLI._ScnRandom(seed)
    jitter = {}
    for u in out["units"]:
        if not mobile(values, u["type"]):
            continue
        dx = rnd.next() % JITTER - JITTER // 2
        dy = rnd.next() % JITTER - JITTER // 2
        u["pos"] = [u["pos"][0] + dx, u["pos"][1] + dy]
        jitter[u["id"]] = [dx, dy]
    drawn, a = [], None
    for o in afters:
        if not o.get("queue"):
            a = rnd.next() % STAGGER
        elif a is None:
            raise PerturbError("a queued order with no order before it")   # tacli refuses it
        if o["after"] + a > 255:
            raise PerturbError(f"an order's after {o['after']} + {a} is past 255")
        o["after"] += a
        drawn.append(a)
    swap = index % 2 == 1
    if swap:
        for u in out["units"]:
            if u["owner"] in (0, 1):
                u["owner"] = 1 - u["owner"]
    return out, {"seed": seed, "swap": swap, "jitter": jitter, "after": drawn}


def name_facings(exp: dict) -> None:
    """THE FACING: 0 for every unit whose scenario names none."""
    for u in exp["units"]:
        if u.get("facing") is None:
            u["facing"] = 0


def empty_point_setup(exp: dict, doc: dict, arena_scn: dict) -> None:
    """The setup every run's wire carries: the units cleared, the features cleared unless the
    scenario says it needs the map's (then it plays fresh), the arena's engine switches, and the
    arena's two seats with their base storage and a level equal to it, so every run starts with
    the same resources."""
    setup = exp["setup"]
    setup["clear_existing"] = True
    setup["clear_features"] = (doc.get("setup") or {}).get("clear_features", True)
    setup["objective"] = None
    setup["switches"] = dict(arena_scn["setup"]["switches"])
    players = []
    for p in arena_scn["setup"]["players"]:
        st = p["storage"]
        players.append({"slot": p["slot"], "controller": p["controller"], "side": p.get("side"),
                        "color": p.get("color"), "ai": None,
                        "storage": {"metal": st["metal"], "energy": st["energy"]},
                        "metal": int(st["metal"]), "energy": int(st["energy"])})
    setup["players"] = players
    exp["camera"] = None


def end_clauses(block: dict, swap: bool):
    """[(seat, n)] of the pause's units clauses: the block's end_units are per owner, which a swap
    moves to the other seat."""
    return [((1 - int(k)) if swap else int(k), n) for k, n in sorted(block["end_units"].items())]


def compile_scenario(path):
    doc = json.loads(Path(path).read_text())
    return doc, CLI._scn_compile(str(path))


def run_wire(path, index: int, values: dict, arena_cfg: dict):
    """(the run's expanded scenario, its perturbation record, its wire, its end clauses)."""
    doc, exp = compile_scenario(path)
    block = calibref.calib_block(doc, Path(path).name, arena_cfg)
    _adoc, arena_exp = compile_scenario(TREE / arena_cfg["scenario"])
    empty_point_setup(exp, doc, arena_exp)
    name_facings(exp)
    exact = "exact" in block["kind"]
    seed = run_seed(calibref.sha256_file(path), index)
    exp2, record = perturb(exp, index, values, seed, exact)
    return exp2, record, CLI._scn_wire(exp2), end_clauses(block, record["swap"])
