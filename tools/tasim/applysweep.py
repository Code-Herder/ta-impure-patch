#!/usr/bin/env python3
"""applysweep.py -- every scenario's orders applied once on a DLL, and each order the engine refuses
named with its unit, its command and its target.

    tools/tasim/applysweep.py plan [--also GLOB ...] [--no-group]           the levels it would play,
                                                                            and what it would not
    tools/tasim/applysweep.py run --dll PATH [--j N] [--out DIR] [--also GLOB ...] [--no-group]
                                  [--only TEXT] [--gpu]

WHY. The applier issues an order only when the engine's order mapper 0x43F0E0 gives it an action,
and the mapper says no by writing action 0 (tagpu_scenario.c, issue_one). A refused order fails its
apply, so a scenario that asks a unit for an order it cannot take no longer applies. This sweep
finds every such order before a test group, a probe or a wiki gate meets it. A flagged scenario is
fixed in the scenario, never by loosening the rule.

THE SET is every `ta-scenario/1` file under scenarios/ with at least one order, plus any `--also`
glob (a generated directory). That is every scenario the replay fixtures, the probes and the tools
load, by construction, with no list to keep: the wiki's scenes (art/wiki/entries) are the DLL's own
stage and issue no order through the applier.

THE LEVELS. A scenario plays on the level it is written for, set up as whatever plays it sets it up:
  - a fixture's (tools/replay/fixtures.json, its `fixtures` and `broad`) mid-level `apply` steps on
    the fixture's own `load`, at the steps' GameTimes, with its content, LOS and unit limit, as
    tareplay launches it (a fixture with no `unit_limit` has tareplay's, FIXTURE_LIMIT);
  - a scenario that names a map on a level it loads itself, with the content its units and its map
    need (CONTENT, below) and its own setup's unit limit (DEFAULT_LIMIT when it names none), and
    every other scenario with the same launch (content, map, players, unit limit, LOS, mapping)
    applied after it on that level when it fits there (THE ROOM, below), each at least HOLD_TICKS
    after the apply before it (`--no-group`: one load each). A scenario that turns a seat's AI off
    changes the seat for the rest of the level, and one that keeps what it finds
    (`clear_existing: false`) depends on it, so each of those loads a level of its own;
  - a calibration scenario (scenarios/calib/, no map) on the calibration arena it is applied to;
  - any other scenario without a map is reported as having no level.
The unit limit is part of the level's key and always passed: an instance keeps the last UnitLimit
any launch wrote (ta-drive, measuring.md), so a launch that names none inherits a stranger's.

CONTENT. A unit or feature type the core's definitions of the stock install (`tasim_run defs`,
tools/tasim/native.py) do not have needs a mod. Such a scenario plays with the content of the first
fixture (by name) whose content installs every one of those types, as tareplay installs it; what a
content installs is read the same way, from the core's definitions of a folder the content command
was run in (ContentTypes). Otherwise it is not played: COULD NOT RUN, naming the types and, when the
compatibility suite plays it (tools/compat/setups.json), that setup. A missing mod is a missing
setup, not an applier failure. A map only content installs takes that content too (map_content).

THE ROOM. A create never takes a unit slot freed in its own tick (the stale-hits hold, tagpu_patches.c
hit_take and hit_held), so the slots an apply's clear frees are not its creates': an owner's creates
fit when they number no more than the level's limit less the slots occupied before the apply -- one
start unit an owner on the load, the previous apply's units after it (and, when that apply kept
what it found, what was there before it). The applier refuses an owner that does not fit by name
(tagpu_scenario.c); the plan never places one there. A scenario that does not fit even on a fresh
load of its own launch is not played: DOES NOT FIT, a failure of the scenario like a refused order.
Units made in play between two applies (a builder's) are not counted; the applier names any owner
they leave without room.
A scenario with delayed orders is followed far enough for its last one to fall due, and the
delayed refusals are read from the log (`delayed order at GameTime G: slot S cannot take cmd C`).

FAST BY CONSTRUCTION. The levels are tareplay's Workers (one instance and one Xvfb each) played by
`--j` threads under the run lock, holding the slots of as many game lanes as it plays at once
(tools/runlock.py, lanes_of) and playing no more at once than the lock's games(); a level serves
every scenario of its launch, and the longest levels go first. A level that fails part-way hands
each remaining scenario that names a map to a level of its own.

THE RENDERER. A level renders with GDI (tareplay.GDI_LEVER) and takes no GPU token; `--gpu` renders
it on the default renderer, for a debugging run, and takes a token a lane; applysweep.json names the
renderer.

WHAT COUNTS AS APPLIED. An apply passes only when its result says so: `ok` 1, no error, no create
refused and at least one entity created. Every swept scenario has a unit with an order, so an apply
that created nothing -- an `on_error: abort`, a seat the level refused -- did not apply it, and is a
failure whatever else the result says.

Exit 0: every scenario applied and no order refused; 1: an order refused, an apply that failed or a
scenario that does not fit its launch (each named); 2: a scenario could not be played (each named),
and nothing refused or failed.
"""

import argparse
import glob as globlib
import json
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
SCENARIOS = TREE / "scenarios"
ARENA = SCENARIOS / "calib" / "arena.json"
FIXTURES = TREE / "tools" / "replay" / "fixtures.json"

REFUSED_RX = re.compile(r"order (\d+): this unit cannot take that order \((.*), cmd (\d+)\)")
DELAYED_RX = re.compile(r"scenario: delayed order at GameTime (\d+): slot (\d+) cannot take cmd (\d+)")
# tasim_wire.c's ORDERTYPES, for the delayed lines, which carry the number only
CMD_NAMES = {10: "stop", 2: "move", 3: "attack", 4: "blast", 5: "unload", 6: "load", 7: "defend",
             8: "repair", 9: "patrol", 11: "teleport", 12: "reclaim", 13: "capture",
             14: "mobilebuild", 256: "buildweapon"}
AFTER_MAX = 255                 # an order's `after` (scenario-format.md)
J_MAX = 16
COMPAT = TREE / "tools" / "compat" / "setups.json"
DEFAULT_LIMIT = 1500            # tacli's SCN_DEFAULT_LIMIT: a scenario's limit when it names none
FIXTURE_LIMIT = 500             # tareplay's UNIT_LIMIT: a fixture's when it names none
HOLD_TICKS = 2                  # a slot freed at T is free of the hold from T + 2 (hit_held)
START_UNITS = 1                 # a seat's start units on a load: its commander


# ---- the plan: pure, tested offline ----------------------------------------------------------

def rel(path: Path) -> str:
    try:
        return str(Path(path).resolve().relative_to(SCENARIOS))
    except ValueError:
        return str(path)


def orders_of(doc: dict) -> list:
    """The orders a scenario file asks for, unexpanded: a group's apply to each of its members."""
    out = []
    for u in doc.get("units") or []:
        out += u.get("orders") or []
    for g in doc.get("groups") or []:
        n = sum(c.get("count", 1) for c in g.get("composition") or []) or 1
        out += (g.get("orders") or []) * n
    return out


def max_after(doc: dict) -> int:
    return max([o.get("after") or 0 for o in orders_of(doc)] + [0])


def scenario_docs(also=()) -> dict:
    """{path: doc} of every ta-scenario/1 file under scenarios/ and in the `also` globs."""
    paths = set(SCENARIOS.rglob("*.json"))
    for pat in also:
        paths |= {Path(p) for p in globlib.glob(pat, recursive=True)}
    out = {}
    for p in sorted(paths):
        try:
            doc = json.loads(p.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        if isinstance(doc, dict) and doc.get("format") == "ta-scenario/1":
            out[p.resolve()] = doc
    return out


def fixture_list(fixtures_json: dict) -> list:
    """[(name, fixture)] of the landing's fixtures and the broad tier's."""
    return list(fixtures_json.get("fixtures", {}).items()) + \
        list((fixtures_json.get("broad") or {}).items())


def launch_of(doc: dict, fix, plays=True) -> dict:
    """What a load of this scenario needs that an apply cannot change: the fixture's content, and
    its launch flags when the fixture plays this scenario (`plays`), as tareplay launches it; else
    the scenario's own setup. A content-only fixture (`plays` False) lends its content alone."""
    s = doc.get("setup") or {}
    own = fix if fix and plays else None
    return {"content": fix[0] if fix and fix[1].get("content") else None,
            "map": (s.get("map") or "").lower(),
            "players": json.dumps(sorted((p.get("slot"), p.get("controller"), p.get("side"))
                                         for p in s.get("players") or []), default=str),
            "unit_limit": (own[1].get("unit_limit", FIXTURE_LIMIT) if own else
                           s.get("unit_limit") or DEFAULT_LIMIT),
            "los": (own[1].get("los", 0) if own else None),     # tareplay's default
            "mapping": s.get("mapping")}


def owner_counts(doc: dict) -> dict:
    """{owner: units the scenario creates for it}: its units and each group's composition."""
    out = {}
    for u in doc.get("units") or []:
        out[u.get("owner")] = out.get(u.get("owner"), 0) + 1
    for g in doc.get("groups") or []:
        n = sum(c.get("count", 1) for c in g.get("composition") or [])
        out[g.get("owner")] = out.get(g.get("owner"), 0) + n
    return out


def clears(doc: dict) -> bool:
    return (doc.get("setup") or {}).get("clear_existing", True) is not False


def room_after(occupied: dict, doc: dict) -> dict:
    """The slots each owner holds after this scenario's apply (THE ROOM): its units, and what was
    there before when it keeps what it finds."""
    need = owner_counts(doc)
    if clears(doc):
        return dict(need)
    out = dict(occupied)
    for o, n in need.items():
        out[o] = out.get(o, 0) + n
    return out


def misfit(occupied: dict, doc: dict, limit: int):
    """Why this scenario's creates do not fit a level whose owners hold `occupied` (THE ROOM); None
    when they do. An owner absent from `occupied` holds its start units."""
    for o, n in sorted(owner_counts(doc).items(), key=lambda kv: str(kv[0])):
        held = occupied.get(o, START_UNITS)
        if n + held > limit:
            return (f"player {o} needs {n} slots and the level's limit is {limit}, {held} of them "
                    + ("held by the clear" if clears(doc) else "occupied") + " at the apply")
    return None


def type_names(doc: dict) -> tuple:
    """({unit type, upper case}, {feature type, lower case}) the scenario names."""
    units = {u.get("type", "").upper() for u in doc.get("units") or [] if u.get("type")}
    for g in doc.get("groups") or []:
        units |= {c.get("type", "").upper() for c in g.get("composition") or [] if c.get("type")}
    feats = {f.get("type", "").lower() for f in doc.get("features") or [] if f.get("type")}
    return units, feats


def needs_mod(doc: dict, stock) -> list:
    """The types the scenario names that the stock install's core definitions do not have, sorted;
    [] with no definitions (`stock` None)."""
    if stock is None:
        return []
    units, feats = type_names(doc)
    return sorted((units - stock[0]) | (feats - stock[1]))


def content_fixtures(fixtures_json: dict) -> list:
    """[(name, fixture)] of every fixture with content, by name: the order a provider is chosen in."""
    return sorted(((n, f) for n, f in fixture_list(fixtures_json) if f.get("content")),
                  key=lambda r: r[0])


class ContentTypes:
    """`provides(name, fixture)`: the non-stock types a fixture's content installs, each distinct
    content read once. The content command runs as tareplay's Worker.place runs it, in a scratch
    folder that holds the install's top-level files as links, and the core's definitions of that
    folder less the stock ones are its types. The folder is the same kind tareplay runs every content
    command in -- an instance's game folder, whose stock files are links into the install -- so a
    content command creates its own files there and writes through none (tools/replay/fixtures.json,
    each content's `files`). A command that fails or runs past 300 s installs nothing."""

    def __init__(self, stock, gamedir=None):
        sys.path.insert(0, str(TREE / "tools" / "wiki"))
        import tavfs
        self.stock, self.root, self.memo = stock, tavfs.find_install(gamedir), {}

    def __call__(self, name, fix):
        key = json.dumps(fix["content"], sort_keys=True)
        if key not in self.memo:
            self.memo[key] = self.read(fix["content"])
        return self.memo[key]

    def read(self, content) -> set:
        gd = Path(tempfile.mkdtemp(prefix="applysweep-content-"))
        try:
            for f in self.root.iterdir():
                if f.is_file():
                    (gd / f.name).symlink_to(f)
            argv = [a.replace("{gamedir}", str(gd)) for a in content["run"]]
            if argv[0].endswith(".py"):
                argv = [sys.executable, str(TREE / argv[0])] + argv[1:]
            try:
                if subprocess.run(argv, capture_output=True, text=True, timeout=300).returncode:
                    return set()
            except (OSError, subprocess.TimeoutExpired):
                return set()
            units, feats = stock_types(gd)
            return (units - self.stock[0]) | (feats - self.stock[1])
        finally:
            shutil.rmtree(gd, ignore_errors=True)


def stock_types(gamedir=None) -> tuple:
    """({unit names}, {feature names}) of the stock install, from `tasim_run defs`."""
    import native
    rc, out, err = native.run("defs", gamedir)
    if rc:
        raise RuntimeError(f"tasim_run defs: {(err or out).strip()[-300:]}")
    d = json.loads(out)
    return ({u["unitname"].upper() for u in d["units"]},
            {f["name"].lower() for f in d["features"]})


def compat_plays() -> dict:
    """{scenario stem: the compatibility suite's setup that plays it}."""
    try:
        setups = json.loads(COMPAT.read_text()).get("setups") or []
    except (OSError, json.JSONDecodeError):
        return {}
    return {stem: st["name"] for st in setups for stem in (st.get("scenarios") or {}).values()}


def writes_seats(doc: dict) -> bool:
    """Whether an apply of this scenario changes a seat for the rest of the level: `ai: off` turns
    an AI seat into a type-1 seat, and the applier refuses `ai: off` on a seat that is not type 2
    (tagpu_scenario.c, resolve_setup), so a second such apply on the level fails."""
    return any(pl.get("ai") == "off" for pl in (doc.get("setup") or {}).get("players") or [])


def map_content(docs: dict, fixtures_json: dict) -> dict:
    """{map: (name, fixture)} for each map that only content installs: every fixture that loads a
    scenario on it installs the same content. Those maps are generated -- `tools/resource_fixture.py`
    writes the `RS ...` maps into the game folder -- and are not in a stock install, where TA loads
    another map instead of a missing one. A map some fixture loads without content, or that two
    fixtures install different content for, is not one of them."""
    seen = {}
    for name, fix in fixture_list(fixtures_json):
        d = docs.get((SCENARIOS / fix["load"]).resolve()) if fix.get("load") else None
        m = ((d or {}).get("setup") or {}).get("map")
        if m:
            seen.setdefault(m.lower(), []).append(
                (name, fix, json.dumps(fix.get("content"), sort_keys=True)))
    return {m: rows[0][:2] for m, rows in seen.items()
            if rows[0][2] != "null" and all(r[2] == rows[0][2] for r in rows)}


def plan(docs: dict, fixtures_json: dict, no_group=False, stock=None, compat=None,
         provides=None) -> tuple:
    """(levels, skipped). A level is {"fix": (name, fixture) or None, "launch": {...},
    "items": [{"path", "at" (a GameTime, or None for the held frame the level is at), "report"}],
    "room": {owner: slots held after its last item}}; its first item is the one `scenario load`
    plays. `skipped` is {path: {"could_not_run": why} or {"does_not_fit": why}}. Every scenario
    with an order is reported by exactly one level or skipped; a host without orders is played but
    reports nothing. `stock` is stock_types()'s answer (None: no type is taken for a mod's),
    `compat` compat_plays()'s and `provides` a ContentTypes (None: no content installs a type)."""
    ordered = {p for p, d in docs.items() if orders_of(d)}
    reported, levels = set(), []
    by_scenario = {}                   # a scenario a fixture loads or applies -> that fixture
    for name, fix in fixture_list(fixtures_json):
        for p in [fix.get("load")] + [s[2] for s in fix.get("steps", []) if s[1] == "apply"]:
            if p:
                by_scenario.setdefault((SCENARIOS / p).resolve(), (name, fix))

    # a fixture's mid-level applies, on its own load at their GameTimes
    for name, fix in fixture_list(fixtures_json):
        applies = sorted((s[0], (SCENARIOS / s[2]).resolve()) for s in fix.get("steps", [])
                         if s[1] == "apply")
        fresh = [p for _t, p in applies if p in ordered and p not in reported]
        if not fresh:
            continue
        host = (SCENARIOS / fix["load"]).resolve()
        if host not in docs:
            continue
        items = [{"path": host, "at": None, "report": host in ordered and host not in reported}]
        reported |= {host} if items[0]["report"] else set()
        for t, p in applies:
            items.append({"path": p, "at": t, "report": p in ordered and p not in reported})
            reported |= {p} if items[-1]["report"] else set()
        # the steps after the last one that reports change nothing the sweep reads
        last = max(k for k, i in enumerate(items) if i["report"])
        items = items[:last + 1]
        levels.append({"fix": (name, fix), "launch": launch_of(docs[host], (name, fix)),
                       "items": items})

    # every other scenario with an order: on its own map, or the arena, grouped by launch
    groups, skipped = {}, {}
    generated = map_content(docs, fixtures_json)
    providers = content_fixtures(fixtures_json) if provides else []
    compat = compat or {}
    for p in sorted(ordered - reported):
        doc = docs[p]
        plays = p in by_scenario
        fix = by_scenario.get(p) or generated.get(((doc.get("setup") or {}).get("map") or "").lower())
        need = set(needs_mod(doc, stock))
        if need and not plays:
            if not (fix and fix[1].get("content") and provides and need <= provides(*fix)):
                fix = next((r for r in providers if need <= provides(*r)), None)
                if fix is None:
                    skipped[p] = {"could_not_run": (
                        f"needs {', '.join(sorted(need))}, which no fixture's content installs"
                        + (f"; the compatibility suite plays it in {compat[p.stem]}"
                           if p.stem in compat else ""))}
                    continue
        if (doc.get("setup") or {}).get("map"):
            launch = launch_of(doc, fix, plays)
            items = [{"path": p, "at": None, "report": True}]
        elif rel(p).startswith("calib/"):
            launch = launch_of(docs[ARENA.resolve()], None) if ARENA.resolve() in docs else None
            if launch is None:
                skipped[p] = {"could_not_run": "no map, and no fixture applies it"}
                continue
            launch = dict(launch, host=str(ARENA.resolve()))
            items = [{"path": p, "at": None, "report": True}]
        else:
            skipped[p] = {"could_not_run": "no map, and no fixture applies it"}
            continue
        limit = launch["unit_limit"]
        host = docs.get(Path(launch["host"])) if launch.get("host") else None
        fresh = room_after({}, host) if host else {}
        why = (misfit({}, host, limit) if host else None) or misfit(fresh, doc, limit)
        if why:
            skipped[p] = {"does_not_fit": why + " on a fresh load of its own launch"}
            continue
        key = json.dumps(launch, sort_keys=True)
        alone = no_group or writes_seats(doc) or not clears(doc)
        lv = None if alone else groups.get(key)
        if lv is not None and misfit(lv["room"], doc, limit):
            lv = None
        if lv is None:
            lv = {"fix": fix if launch.get("content") else None, "launch": launch, "items": [],
                  "room": fresh}
            if launch.get("host"):
                lv["items"].append({"path": Path(launch["host"]), "at": None, "report": False})
            levels.append(lv)
            if not alone:
                groups[key] = lv
        lv["items"] += items
        lv["room"] = room_after(lv["room"], doc)
        reported.add(p)
    levels.sort(key=lambda lv: -len(lv["items"]))
    return levels, skipped


# ---- what an answer says ---------------------------------------------------------------------

def result_of(text: str):
    """The applier's result object in a tacli `--json` answer, or None."""
    dec = json.JSONDecoder()
    for m in re.finditer(r"\{", text):
        try:
            obj, _end = dec.raw_decode(text, m.start())
        except json.JSONDecodeError:
            continue
        if isinstance(obj, dict) and "orders" in obj and "ok" in obj:
            return obj
    return None


def wire_orders(exp: dict) -> list:
    """The wire's orders in the applier's numbering (tacli _scn_wire): each unit's in turn."""
    return [(u, o) for u in exp["units"] for o in u["orders"]]


def describe(u: dict, o: dict) -> dict:
    target = o.get("target") if o.get("target") else (o.get("to") and list(o["to"]))
    row = {"unit": u["id"], "type": u["type"], "owner": u["owner"], "order": o["cmd"],
           "target": target, "after": o.get("after") or 0}
    if o.get("count"):
        row["count"] = o["count"]
    return row


def refusals(res: dict, exp: dict) -> tuple:
    """([refused order], [other error]) of one apply's result."""
    orders = wire_orders(exp)
    refused, other = [], []
    for e in res.get("errors") or []:
        m = REFUSED_RX.search(e)
        if m and int(m.group(1)) < len(orders):
            refused.append(dict(describe(*orders[int(m.group(1))]), error=e))
        else:
            other.append(e)
    return refused, other


def delayed_refusals(lines, res: dict, exp: dict) -> list:
    """The delayed orders the unit could not take when they fell due, from the log lines after the
    apply: each named by its subject's slot (the result's engine index) and the command."""
    by_slot = {}
    for handle, row in (res.get("units") or {}).items():
        if isinstance(row, dict) and row.get("engine_index") is not None:
            by_slot[int(row["engine_index"])] = handle
    out = []
    for ln in lines:
        m = DELAYED_RX.search(ln)
        if not m:
            continue
        slot, cmd = int(m.group(2)), CMD_NAMES.get(int(m.group(3)), m.group(3))
        handle = by_slot.get(slot)
        hit = [(u, o) for u, o in wire_orders(exp)
               if u["id"] == handle and o["cmd"] == cmd and o.get("after")]
        row = describe(*hit[0]) if hit else {"unit": handle or f"slot {slot}", "order": cmd}
        out.append(dict(row, error=ln.strip()))
    return out


# ---- the levels played -----------------------------------------------------------------------

class Level:
    """One Worker playing level after level; a level's scenarios in its order."""

    def __init__(self, k, out, dll, R, gpu=False):
        self.R, self.k, self.out, self.dll = R, k, out, dll
        self.w = R.Worker(out)
        self.w.gdi = not gpu
        self.w.turbo = 1
        self.w.held = True

    def close(self):
        self.w.close()

    def compile(self, path):
        cli = self.R._CLI_API
        try:
            return cli._scn_compile(str(path), None, self.w.name)
        except SystemExit:
            return cli._scn_compile(str(path))

    def answer(self, rc, text, what):
        res = result_of(text)
        if res is None:
            raise self.R.CouldNotRun(f"{what}: {self.R.last(text)}")
        return res

    def follow(self, log, ticks):
        """The held level run `ticks` steps on and held again."""
        R = self.R
        T = log.at + ticks
        R.hold_at(self.w, log, T)
        R.turbo_engage(self.w, log)
        R.resume(self.w, log)
        R.wait_paused(self.w, log, T)

    def play(self, lv, docs):
        """[(path, row)] for every reported item played, the level's failure raised with the
        index of the item it stopped at (Stopped)."""
        R, w = self.R, self.w
        rows, k = [], 0
        try:
            w.place(lv["fix"][1] if lv["fix"] else {"name": "applysweep"}, self.dll)
            log = R.Log(w)
            launch = lv["launch"]
            host = lv["items"][0]["path"]
            argv = ["scenario", "load", "--json", w.name, host, "--defaults", "--restart",
                    "--keep-dll"]
            argv += ["--unit-limit", launch["unit_limit"]]
            if launch.get("los") is not None and launch["los"] != "file":
                argv += ["--los", launch["los"]]
            rc, text = R.tacli(*argv, timeout=R.LAUNCH_TIMEOUT)
            res = self.answer(rc, text, f"scenario load {rel(host)}")
            R.held_start(w, log)
            applied_at = log.at
            for k, item in enumerate(lv["items"]):
                path = item["path"]
                if k:
                    if item["at"] is not None and item["at"] > log.at:
                        self.follow(log, item["at"] - log.at)
                    elif item["at"] is None and log.at < applied_at + HOLD_TICKS:
                        self.follow(log, applied_at + HOLD_TICKS - log.at)
                    applied_at = log.at
                    rc, text = R.tacli("scenario", "apply", "--json", w.name, path, timeout=300)
                    res = self.answer(rc, text, f"scenario apply {rel(path)}")
                log.read()
                n = len(log.lines)
                exp = self.compile(path)
                after = max_after(docs[path])
                if after:
                    self.follow(log, after + 2)
                if not item["report"]:
                    continue
                refused, other = refusals(res, exp)
                refused += delayed_refusals(log.lines[n:], res, exp)
                rows.append((path, {"refused": refused, "other_errors": other,
                                    "creates_failed": res.get("failed", 0),
                                    "ok": bool(res.get("ok")), "applied": res.get("applied", 0),
                                    "orders": res.get("orders"), "gametime": res.get("gametime")}))
            w.stop(hard=True)
        except (R.CouldNotRun, R.Failed) as e:
            try:
                w.stop(hard=True)
            except R.CouldNotRun:
                pass
            raise Stopped(k, str(e), rows)
        return rows


class Stopped(Exception):
    def __init__(self, k, why, rows):
        super().__init__(why)
        self.k, self.why, self.rows = k, why, rows


def renderer(gpu: bool) -> str:
    return "vulkan" if gpu else "gdi"


def lanes_of(levels, j) -> int:
    """The game lanes run_levels plays at once: one Worker a thread, `j` threads, never more than
    there are levels."""
    return min(j, J_MAX, max(1, len(levels)))


def run_levels(levels, docs, j, out, dll, R, gpu=False):
    """{path: row} for every reported scenario, played on `j` Workers of `R` (tareplay), with GDI
    unless `gpu`."""
    work = queue.Queue()
    for lv in levels:
        work.put(lv)
    done, lock = {}, threading.Lock()

    def lane(k):
        L = None
        try:
            L = Level(k, out, dll, R, gpu)
            while not L.R.STOP.is_set():
                try:
                    lv = work.get_nowait()
                except queue.Empty:
                    break
                try:
                    rows = L.play(lv, docs)
                except Stopped as s:
                    rows = s.rows
                    rest = lv["items"][s.k:]
                    for item in rest:
                        if not item["report"]:
                            continue
                        own = (docs[item["path"]].get("setup") or {}).get("map")
                        if own and not lv.get("single") and item is not lv["items"][0]:
                            work.put({"fix": lv["fix"], "launch": lv["launch"], "single": True,
                                      "items": [dict(item, at=None)]})
                        else:
                            with lock:
                                done[item["path"]] = {"could_not_run": s.why}
                    L.R.say(f"level {k}: {rel(lv['items'][0]['path'])} stopped at item {s.k}: "
                            f"{s.why}")
                with lock:
                    for path, row in rows:
                        done[path] = row
        except Exception as e:      # a Worker that could not start: its levels stay queued
            with lock:
                done[f"<lane {k}>"] = {"could_not_run": f"{type(e).__name__}: {e}"}
        finally:
            if L:
                L.close()

    threads = [threading.Thread(target=lane, args=(k,), daemon=True)
               for k in range(lanes_of(levels, j))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    while not work.empty():
        lv = work.get_nowait()
        for item in lv["items"]:
            if item["report"] and item["path"] not in done:
                done[item["path"]] = {"could_not_run": "no lane was left to play it"}
    return done


def apply_failures(row) -> list:
    """Why a played scenario's apply did not apply it (WHAT COUNTS AS APPLIED); [] when it did."""
    why = list(row.get("other_errors") or [])
    if row.get("creates_failed"):
        why.append(f"{row['creates_failed']} creates failed; their units' orders were not tried")
    if not row.get("applied"):
        why.append("nothing was created")
    if not row.get("ok") and not why and not row.get("refused"):
        why.append("the applier answered ok 0")
    return why


def verdict(done, skipped):
    """(exit code, lines): the rows played (`done`) and the scenarios the plan skipped."""
    lines, refused, failed, unrun, clean = [], 0, 0, 0, 0
    for path, row in sorted({**done, **skipped}.items(), key=lambda kv: str(kv[0])):
        name = rel(path) if isinstance(path, Path) else path
        if "does_not_fit" in row:
            failed += 1
            lines.append(f"DOES NOT FIT {name}: {row['does_not_fit']}")
            continue
        if "could_not_run" in row:
            unrun += 1
            lines.append(f"COULD NOT RUN {name}: {row['could_not_run']}")
            continue
        for r in row["refused"]:
            refused += 1
            lines.append(f"REFUSED {name}: {r['unit']} ({r.get('type', '?')}, seat "
                         f"{r.get('owner', '?')}) {r['order']} -> {r.get('target')}"
                         + (f" after {r['after']}" if r.get("after") else ""))
        why = apply_failures(row)
        failed += bool(why)
        clean += not why and not row["refused"]
        for e in why:
            lines.append(f"FAILED {name}: {e}")
    lines.append(f"applysweep: {clean} scenario(s) applied clean, {refused} order(s) refused, "
                 f"{failed} apply(s) failed, {unrun} could not run")
    return (1 if refused or failed else 2 if unrun else 0), lines


def planned(a):
    """plan() on the tree, with the stock install's types (stock_types) and the compatibility
    suite's setups: (levels, skipped, docs), or None when the definitions cannot be read."""
    docs = scenario_docs(a.also)
    try:
        stock = stock_types(a.gamedir)
    except (RuntimeError, OSError, ValueError, KeyError, SystemExit,
            subprocess.CalledProcessError) as e:
        print(f"applysweep: could not run: the stock install's definitions: {e}")
        return None
    levels, skipped = plan(docs, json.loads(FIXTURES.read_text()), a.no_group, stock,
                           compat_plays(), ContentTypes(stock, a.gamedir))
    return levels, skipped, docs


def cmd_plan(a):
    got = planned(a)
    if got is None:
        return 2
    levels, skipped, _docs = got
    n = 0
    for lv in levels:
        items = lv["items"]
        n += sum(i["report"] for i in items)
        print(f"{rel(items[0]['path'])} [{lv['launch']['map'] or 'no map'}"
              + (f", content of {lv['fix'][0]}" if lv["fix"] and lv["launch"].get("content") else "")
              + f"]: " + ", ".join(rel(i["path"]) + (f"@{i['at']}" if i["at"] else "")
                                   + ("" if i["report"] else " (host)") for i in items))
    for p, why in sorted(skipped.items(), key=lambda kv: str(kv[0])):
        print(f"no level: {rel(p)}: " + (f"does not fit: {why['does_not_fit']}" if "does_not_fit"
                                          in why else why["could_not_run"]))
    print(f"applysweep plan: {n} scenario(s) with orders on {len(levels)} level(s), "
          f"{len(skipped)} without a level")
    return 0


def cmd_run(a):
    sys.path.insert(0, str(TREE / "tools"))
    sys.path.insert(0, str(TREE / "tools" / "replay"))
    import runlock
    import tareplay as R
    dll = Path(a.dll).resolve()
    if not dll.exists():
        print(f"applysweep: could not run: no DLL at {dll}")
        return 2
    got = planned(a)
    if got is None:
        return 2
    levels, skipped, docs = got
    if a.only:
        levels = [lv for lv in levels if any(a.only in rel(i["path"]) for i in lv["items"])]
        skipped = {p: why for p, why in skipped.items() if a.only in rel(p)}
    out = Path(a.out) if a.out else TREE / ".data" / "applysweep" / time.strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    runlock.hold(TREE, "applysweep " + " ".join(sys.argv[2:]), lanes_of(levels, a.j), gpu=a.gpu)
    j = min(a.j, runlock.games())       # no more lanes than the run lock's games
    print(f"applysweep: {len(levels)} level(s) on j={j}, renderer {renderer(a.gpu)}, DLL {dll}, "
          f"out {out}")
    done = run_levels(levels, docs, j, out, dll, R, a.gpu)
    rc, lines = verdict(done, skipped)
    (out / "applysweep.json").write_text(json.dumps(
        {"dll": str(dll), "renderer": renderer(a.gpu), "levels": len(levels),
         "scenarios": {(rel(p) if isinstance(p, Path) else p): row for p, row in done.items()},
         "skipped": {rel(p): why for p, why in skipped.items()}}, indent=1, default=str))
    print("\n".join(lines))
    return rc


def main(argv=None):
    sys.path.insert(0, str(HERE))
    ap = argparse.ArgumentParser(prog="applysweep.py", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("plan", "run"):
        p = sub.add_parser(name)
        p.add_argument("--also", action="append", default=[], metavar="GLOB",
                       help="more scenario files, e.g. a generated directory's")
        p.add_argument("--no-group", action="store_true",
                       help="one level for each scenario that names a map")
        p.add_argument("--gamedir", help="the stock install whose types are stock "
                                         "(default: the one tools/wiki/tavfs.py finds)")
        if name == "run":
            p.add_argument("--dll", required=True, help="the ddraw.dll to sweep")
            p.add_argument("--j", type=int, default=6, help=f"levels in parallel, 1..{J_MAX}")
            p.add_argument("--out", help="where the report and the levels' logs go")
            p.add_argument("--only", help="only the levels with a scenario whose path has this")
            p.add_argument("--gpu", action="store_true",
                           help="render on the default renderer and take a GPU token a lane; by "
                                "default the levels render with GDI and take none")
    a = ap.parse_args(argv)
    if a.cmd == "run" and not 1 <= a.j <= J_MAX:
        ap.error(f"--j is 1..{J_MAX}")
    return cmd_plan(a) if a.cmd == "plan" else cmd_run(a)


if __name__ == "__main__":
    sys.exit(main())
