"""calibref.py -- where the calibration runner's files live, and what a scenario's `calib` block says.

    import calibref
    calibref.calib_dir()                     # <main checkout>/.data/calib, or $TACALIB_DIR
    calibref.calib_block(doc, "x.json")      # the block, validated, every default filled in
    calibref.arena()                         # tools/tasim/calib-arena.json, validated

The references are kept untracked (research/notes/modules/tasim-calibration.md, *References*): they
carry the engine's costs and HP of the types a scenario uses and a fingerprint of the install, which
is derived from the player's install. So they live under the main checkout's `.data/`, which
`.gitignore` keeps out, resolved through the common git directory the way the shared venv is, so
every worktree reads and writes the same set.

Layout under calib_dir():
    refs/<scenario-id>/<commit12>.json             a reference, ta-calib-ref/2
    refs/<scenario-id>/<commit12>.traces.jsonl.zst the engine traces of an exact scenario
    runs/<stamp>/lane<k>-<launch>/                 one lane-launch: manifest.jsonl, tagpu.log,
                                                   the dumps and the recording
    scenarios/gen/                                 duelgen.py's output
"""

import hashlib
import json
import os
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
ARENA = HERE / "calib-arena.json"
SCHEMA_REF = "ta-calib-ref/2"
SCHEMA_TRACE = "ta-calib-trace/1"
SCHEMA_SCORE = "ta-calib-score/1"

KINDS = ("winrate", "distribution", "exact")
# the metric families of calibration.md's three kinds; income takes a checkpoint, `income.metal@1800`
METRICS_STAT = ("winner", "ttk", "survivors", "travel.first", "travel.median", "travel.last")
METRICS_INCOME = ("income.metal", "income.energy")
METRICS_EXACT = ("build_time", "cost_per_tick", "extractor", "storage", "range", "reload",
                 "damage", "sight")
INCOME_EVERY = 1800             # the income checkpoints: every minute of game time at 30 ticks
# the distribution rule's shuffles of the engine's runs, per metric (check.py, cvm_limit): at 2,000
# the 95th percentile's Monte-Carlo error is about ±0.5 % of false alarms, and a metric costs a
# few milliseconds
SHUFFLES = 2000


class CalibError(Exception):
    """A calibration file that cannot be used: a scenario's block, the arena, a reference."""


def main_checkout() -> Path:
    r = subprocess.run(["git", "-C", str(TREE), "rev-parse", "--git-common-dir"],
                       capture_output=True, text=True, check=True)
    common = Path(r.stdout.strip())
    if not common.is_absolute():
        common = (TREE / common).resolve()
    return common.parent


def calib_dir() -> Path:
    env = os.environ.get("TACALIB_DIR")
    return Path(env).expanduser() if env else main_checkout() / ".data" / "calib"


def refs_dir() -> Path:
    return calib_dir() / "refs"


def runs_dir() -> Path:
    return calib_dir() / "runs"


def gen_dir() -> Path:
    return calib_dir() / "scenarios" / "gen"


def sha256_file(path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def scenario_id(path) -> str:
    """`<stem>@<the first 12 hex of the file's sha256>`: a reference names the exact file."""
    return f"{Path(path).stem}@{sha256_file(path)[:12]}"


def shuffle_of(scn_sha256: str) -> dict:
    """A reference's `shuffle`: the seed of the distribution rule's shuffles, the first four bytes
    little endian of sha256("<scenario sha256>:shuffle"), and their count. Stored in the reference
    so that its thresholds reproduce from the file alone."""
    seed = int.from_bytes(hashlib.sha256(f"{scn_sha256}:shuffle".encode()).digest()[:4], "little")
    return {"seed": seed, "count": SHUFFLES}


def ref_path(sid: str, commit: str) -> Path:
    return refs_dir() / sid / f"{commit[:12]}.json"


def traces_path(sid: str, commit: str) -> Path:
    return refs_dir() / sid / f"{commit[:12]}.traces.jsonl.zst"


def _int(node, key, where, lo, hi, default=None):
    v = node.get(key, default)
    if isinstance(v, bool) or not isinstance(v, int) or not lo <= v <= hi:
        raise CalibError(f"{where}.{key}: expected a whole number in {lo}..{hi}, got {v!r}")
    return v


def metric_known(name: str) -> bool:
    if name in METRICS_STAT or name in METRICS_EXACT:
        return True
    base, _, at = name.partition("@")
    return base in METRICS_INCOME and at.isdigit() and int(at) > 0 and int(at) % INCOME_EVERY == 0


def calib_block(doc: dict, label: str, arena_cfg: dict = None) -> dict:
    """A scenario's `calib` block with every default filled in, or CalibError naming the key.
    The line of sight and the mapping default to the arena's, the skirmish defaults the bar's
    games use; a reference is keyed by both."""
    node = doc.get("calib")
    w = f"{label}: calib"
    if not isinstance(node, dict):
        raise CalibError(f"{w}: the scenario has no calib block")
    known = ("kind", "metrics", "runs", "limit", "end_units", "area", "exact_family", "los",
             "mapping", "place")
    for k in node:
        if k not in known:
            raise CalibError(f"{w}: unknown key {k!r} (allowed: {', '.join(known)})")
    kind = node.get("kind")
    if not isinstance(kind, list) or not kind or any(k not in KINDS for k in kind):
        raise CalibError(f"{w}.kind: expected a list of {', '.join(KINDS)}")
    exact = "exact" in kind
    if exact and len(kind) != 1:
        raise CalibError(f"{w}.kind: exact stands alone: one run is the whole answer")
    metrics = node.get("metrics")
    if not isinstance(metrics, list) or not metrics or not all(
            isinstance(m, str) and metric_known(m) for m in metrics):
        raise CalibError(f"{w}.metrics: expected a list of known metrics, got {metrics!r}")
    if exact != all(m in METRICS_EXACT for m in metrics):
        raise CalibError(f"{w}.metrics: an exact scenario carries exact metrics only, and the "
                         f"others none")
    family = node.get("exact_family")
    if exact and family not in METRICS_EXACT:
        raise CalibError(f"{w}.exact_family: an exact scenario names its family")
    if not exact and family is not None:
        raise CalibError(f"{w}.exact_family: only an exact scenario has one")
    ends = node.get("end_units") or {}
    if not isinstance(ends, dict) or any(k not in ("0", "1") for k in ends):
        raise CalibError(f"{w}.end_units: expected {{\"0\": n, \"1\": n}}")
    area = node.get("area")
    if not isinstance(area, str) or not area:
        raise CalibError(f"{w}.area: expected the scoreboard's grouping, like duel/land/t1/x")
    if (doc.get("setup") or {}).get("clear_features") is False:
        # every run must start from a level without features: the empty point's checks (R3)
        # and the perturbation assume it; keeping them is not supported yet
        raise CalibError(f"{label}: setup.clear_features: false is not supported in a "
                         f"calibration scenario: every run clears the level's features")
    cfg = arena_cfg or arena()
    return {
        "kind": list(kind), "metrics": list(metrics),
        "runs": _int(node, "runs", w, 1, 1000, 2 if exact else None),
        "limit": _int(node, "limit", w, 1, 10 ** 7),
        "end_units": {k: _int(ends, k, f"{w}.end_units", 0, 65535) for k in sorted(ends)},
        "area": area, "exact_family": family,
        "los": _int(node, "los", w, 0, 2, cfg["los"]),
        "mapping": _int(node, "mapping", w, 0, 1, cfg["mapping"]),
    }


def arena(path: Path = ARENA) -> dict:
    """The arena: its map and scenario, the line of sight and mapping that are the default of
    every scenario, and the flat rectangle the duels and courses are placed in. Which map, and
    that its .ota leaves the meteor shower off, is tools/tasim/arenascan.py's to derive."""
    try:
        cfg = json.loads(Path(path).read_text())
    except (OSError, json.JSONDecodeError) as e:
        raise CalibError(f"{path}: {e}")
    for key in ("map", "scenario", "los", "mapping", "unit_limit", "flat", "a", "b", "axis",
                "courses"):
        if key not in cfg:
            raise CalibError(f"{path}: missing {key!r}")
    if cfg["axis"] not in ("x", "y"):
        raise CalibError(f"{path}: axis is x or y")
    x, y, wd, ht = cfg["flat"]["rect"]
    for name in ("a", "b"):
        px, py = cfg[name]
        if not (x <= px <= x + wd and y <= py <= y + ht):
            raise CalibError(f"{path}: {name} {cfg[name]} is outside the flat rectangle")
    return cfg


def arena_point(cfg: dict, along, across=0) -> list:
    """The point `along` world units from the arena's a toward b on its axis and `across` world
    units off it (toward +y for an x axis, +x for a y axis)."""
    ax, ay = cfg["a"]
    return [ax + along, ay + across] if cfg["axis"] == "x" else [ax + across, ay + along]


def arena_facing(cfg: dict) -> tuple:
    """(side A's facing, side B's): A faces B along the axis and B faces A, in the scenario
    format's degrees."""
    return (90, 270) if cfg["axis"] == "x" else (0, 180)


def in_flat(cfg: dict, p) -> bool:
    x, y, w, h = cfg["flat"]["rect"]
    return x <= p[0] <= x + w and y <= p[1] <= y + h


def check_positions(doc: dict, cfg: dict, label: str):
    """Every unit of the scenario, and every point an order sends one to, inside the arena's flat
    rectangle, or CalibError naming the first that is not: a scenario laid out for another map
    is refused before a lane plays it."""
    for u in doc.get("units") or []:
        if not in_flat(cfg, u["pos"]):
            raise CalibError(f"{label}: unit {u.get('id', u['type'])} at {u['pos']} is outside "
                             f"the arena's flat rectangle {cfg['flat']['rect']} ({cfg['map']})")
        for o in u.get("orders") or []:
            to = o.get("to")
            if isinstance(to, list) and len(to) == 2 and not in_flat(cfg, to):
                raise CalibError(f"{label}: unit {u.get('id', u['type'])}'s {o['cmd']} to {to} "
                                 f"is outside the arena's flat rectangle {cfg['flat']['rect']}")


ROLES_H = TREE / "tagpu" / "ddraw" / "src" / "tasim_roles.h"


def exchange_rate() -> float:
    """R, the energy a unit of metal is worth: the one named constant TASIM_ROLES_R of
    tasim_roles.h, which the role classifier uses too, read here rather than copied."""
    import re
    try:
        m = re.search(r"#define\s+TASIM_ROLES_R\s+([0-9.]+)", ROLES_H.read_text())
    except OSError as e:
        raise CalibError(f"{ROLES_H}: {e}")
    if not m:
        raise CalibError(f"{ROLES_H}: no TASIM_ROLES_R")
    return float(m.group(1))


def install_fingerprint(catalogue: dict, types) -> str:
    """sha256 of the catalogue's key and CRC of every type used, its weapons' and feature defs'
    fingerprints: what the engine loaded, which a reference is only valid against."""
    by = {u["name"].upper(): [u["key"], u["crc_all"]] for u in catalogue["units"]}
    doc = {"types": {t: by.get(t) for t in sorted(types)},
           "weapons": catalogue.get("weapons"), "feature_defs": catalogue.get("feature_defs")}
    return hashlib.sha256(json.dumps(doc, sort_keys=True).encode()).hexdigest()


def resolve_commit(build: str) -> str:
    """The full commit of a DLL's build name; a dirty build names none."""
    if build.endswith("-dirty"):
        raise CalibError(f"the DLL was built from a dirty tree ({build})")
    r = subprocess.run(["git", "-C", str(TREE), "rev-parse", "--verify", build + "^{commit}"],
                       capture_output=True, text=True)
    if r.returncode:
        raise CalibError(f"the DLL's build {build} is no commit of this repository")
    return r.stdout.strip()


def git_commit() -> str:
    r = subprocess.run(["git", "-C", str(TREE), "rev-parse", "HEAD"], capture_output=True,
                       text=True, check=True)
    return r.stdout.strip()


def git_build() -> str:
    """The build name a DLL built from this tree carries, the recording's RT_BUILD (the
    Makefile's GIT_COMMIT: `git describe --match=NeVeRmAtCh --always --dirty`)."""
    r = subprocess.run(["git", "-C", str(TREE), "describe", "--match=NeVeRmAtCh", "--always",
                        "--dirty"], capture_output=True, text=True, check=True)
    return r.stdout.strip()
