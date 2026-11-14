#!/usr/bin/env python3
"""bar.py -- ImpAI's bar: one-against-one games against OTA AI and between ImpAI's levels, judged.

    tools/impai/bar.py ids --pair PAIR [--maps a,b]       the game ids of a pair, one a line
    tools/impai/bar.py play --ids FILE|PAIR --lanes L [--out DIR] [--dll PATH] [--cap T] [--gpu] [--draw]
    tools/impai/bar.py judge DIR...
    tools/impai/bar.py check b4|b5 DIR...                 A2b's b4 or b5 criteria over the games in DIR...

THE PAIRS (research/notes/impai/bar.md). Seat "A" is the player under test, "B" its opponent:

    eota   ImpAI Easy   against OTA AI, Difficulty Easy
    nota   ImpAI Normal against OTA AI, Difficulty Medium
    hota   ImpAI Hard   against OTA AI, Difficulty Hard
    ne     ImpAI Normal against ImpAI Easy   (the level lever, Difficulty Hard)
    hn     ImpAI Hard   against ImpAI Normal (the level lever, Difficulty Hard)
    dry    OTA AI against OTA AI, Difficulty Hard: the dry run, which proves the seating, the end of a side and the
           rate before ImpAI fights

A pair is 100 games: five maps x four side pairings (aa, ac, ca, cc: A's side, B's) x five games, A at the map's
first start in games 1, 3 and 5 and its second in 2 and 4; the dry run is the five maps, sides ac, two games each.
A game's id is `bar-<pair>-<map>-<sides>-<n>`, so a piece is a list of ids.

THE MAPS, one of each kind by the rule the specification states (at least three starts, no uniform metal; the land,
sea and mixed maps from the middle third of the 90 such retail maps by area, the small one from the smallest third
and the large one from the largest; each the median of its class by area): Pincushion (land), Red Triangle (sea),
Trout Farm (mixed, 40 % under sea), Lava Mania (small), Plains and Passes (large).

A GAME. The template scenarios/impai/bar.json, filled: the map, the Difficulty, seat 0 an idle human with no unit,
A and B in seats 1 and 2 (A in seat 1 when it plays the first start, seat k standing at StartPos(k + 1)), each a
commander placed at that start position, read from the map's .ota through tavfs (the schema whose start count is
the three seats, else the one with most, as the skirmish picks, N-b1). ImpAI's levels go to the level lever
tagpu_impai_level.on, and a tournament's tables to tagpu_impai_table.on, both in the game folder before the launch
(the DLL reads them at attach). A lane (a tareplay Worker, held, turbo k = 1) loads it, then holds it every 3000
ticks (STEP), or at the first frame visit at which seat 1's or seat 2's units alive, +0x144, is 0 (`pause at T or
units <s> <= 0`, tagpu_replay.c THE PAUSE AT T), and peeks both: the game ends when one is 0 -- the other side won --
or at the cap, GameTime 81 000 (45 minutes), a failure for ImpAI. Both at 0 at one look is a draw, judged as the cap.
With the commander-death rule on, a commander's death kills its side (0x48667E..0x4866B3); bar.json sets the rule
on (setup.commander_death), and the harness reads the level's ActiveCommanderDeath main+0x37EF6 into each result,
so every result states the rule it was played under. A hold the engine does not reach within the harness's bounds
is "could not run", never an outcome. A veteran CORKROG (29 918 HP) can survive the kill-all's 30 000 hit, so a
side whose commander died can still hold that one unit (scout N-b4). Each result carries its last two looks
(`looks`: the GameTime, each seat's units alive, and at the last also its commanders lost, +0x106).

THE END. Once the outcome is read, the level is ended through the engine's own exit (tareplay.Worker.quit: the
in-game menu, its Exit, then Exit Game and its Yes): the exit menu's handler 0x4605C0 calls the exit 0x491C60,
which runs the teardown 0x491B60 before anything else of the exit (engine/recorder.md). The teardown ends the
recording and writes ImpAI's level-end lines (tagpu_impai_level_end), and the exit waits for the recording's
writer to close its file. No tick runs in it: the level stays held through the in-game menu, as tareplay's twin
checks on every record test (its menu opened and closed on a held level leaves GameTime where it was), and the
handler reaches the teardown within its own call, through the exit's three-call stub (engine/memory.md): a blank
frame 0x4257A0; 0x451B60, which returns at once off a network game (`test main+0x2A44, 1` at 0x451B66, the
network bit, engine/sim.md; DISASSEMBLED); the teardown. A game whose exit does not reach the teardown (no
`impai: handicap rows`, the line every level end writes) could not run. From the level-end lines each ImpAI
seat's `end` (level_end): its units completed, its army by class, its factories completed by the tech level they
offer, its water-only factories, the sea plants it ordered, its attack waves, raids and scouts, its naval
judgement and its tech-up income; from the military's task lines its `first` ATTACK, RAID and scouting tick
(first_ticks); from its pulses its best metal income a pass over a pulse minute (`income_max`).

WHAT IS KEPT FOR A DIAGNOSIS. A game A did not win, or in which an ImpAI seat stalled (for_diagnosis), keeps the
instance log of its launch beside its result (`<id>.tagpu.log`), as a game that could not run does; a bar game
(parse_id's `recordings`) keeps its recording too, as `<id>.imprec` beside its result with its path in the
result's `recording`, once its writer's `closed` line is in the log -- that line, logged before the exit goes on,
is what says the file is whole; the `.imprec.lock` beside a recording stays by design (tagpu_recfile.h, THE PATH
LOCK) and says nothing. Every other game's recording is deleted, and its `recording` is null.

THE TURBO is k = 1, one tick a frame, and no option changes it: the step's once-a-call tail -- the temporary sight
records' expiry 0x482130 among it -- runs once a batch, not once a tick (engine/sim.md, *The batch is part of the
result*; tagpu_replay.c, THE TURBO), so a game at k > 1 is another game than the one a player at normal speed
plays, not the same game drawn less often (MEASURED 2026-10-08: speed 20 against k = 2 parted in 1 272 and 1 383
unit fields). The pause at T stays tick-exact at any k (its bound ends the batch at T); the end's clause does not,
since a batch of k ticks ends where it ends.

THE STALL RULE, from ImpAI's pulse lines (`impai: pulse`, one a game minute a seat): a stall is a window of 3600
ticks, after GameTime 1800 and before the game's end, in which the seat's `built` and its `eroots` both did not
rise (the economy stopped), or in which it held at least one army unit and its `mroots` did not rise (the army
stopped).

THE RENDERER. A game renders with GDI (tareplay.GDI_LEVER) and takes no GPU token; `--gpu` renders it on the
default renderer, for a debugging run, and takes a token a lane. Every result names its renderer, a game that could
not run included, and is compared only with results of the same one (research/notes/modules/impai.md, *The
renderer*); `judge` refuses a result that names none.

THE PACING. A GDI game runs NO DRAW (tareplay.NODRAW_LEVER): while the turbo runs, a frame whose batch was the
turbo's skips the game screen's draw and runs only what the harness reads -- the triggers, ImpAI's digests and the
roster (tagpu_nodraw.c, THE GATE). Held frames draw. `--draw` renders every frame, for debugging; `--gpu`
implies it, the lever being GDI's alone. Every result names its pacing (`pacing`: draw or nodraw) and, under no
draw, the level's account (`nodraw`: frames not drawn, drawn for the scoreboard, drawn otherwise).

THE STOP. SIGINT or SIGTERM during `play` sets tareplay's STOP: no game starts after it, every game still playing
is stopped through its own instance (tacli stop, which ends the PID found by that instance's prefix, then its
wineserver) and written as could_not_run, reason "stopped"; the lanes are closed, the run lock is released and the
exit is 2. A game not started is not written, so the same `play` resumes it.

RESUMING. `play` into an out dir that holds results skips every game whose result is already there, played to
the same cap on the same renderer at the same pacing with the same tables (game_kept; a result that names no
pacing was drawn): a game reloads its scenario from scratch at
its own launch (Lane.play: `scenario load --restart`), so a game skipped changes nothing another plays. A result
that could not run is played again.

THE ROW CHECKS. `check b4` and `check b5` score A2b's rows from the results (a2-spec.md section 9): ImpAI Hard
against OTA AI Hard (the pair hota), each game played to the row's bound, given to `play` as `--cap` (b4: 27 000,
four games on two maps or more; b5: 36 000, a game on each of the five maps). b4: an attack wave sent, a scout
sent and no stall. b5: army units of ROLES_MIN classes or more completed; a water-only factory completed on the
sea map and on any map ImpAI judged naval; and a factory of tech 2 or above completed wherever ImpAI's best metal
income a pass reached its tech-up income `tech_m`.

Exit 0 when the verdict passed (or the command ran), 1 when it failed, 2 when it could not run or was stopped."""
import argparse
import json
import queue
import re
import shutil
import signal
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(TREE / "tools"))
sys.path.insert(0, str(TREE / "tools" / "replay"))
sys.path.insert(0, str(TREE / "tools" / "wiki"))

TEMPLATE = TREE / "scenarios" / "impai" / "bar.json"
MAPS = {"pincushion": "Pincushion", "redtriangle": "Red Triangle", "troutfarm": "Trout Farm",
        "lavamania": "Lava Mania", "plainspasses": "Plains and Passes"}
SIDE = {"a": "arm", "c": "core"}
COMMANDER = {"arm": "ARMCOM", "core": "CORCOM"}
PAIRS = {   # Difficulty, A (controller, level), B (controller, level)
    "eota": ("easy", ("impai", "easy"), ("ai", None)),
    "nota": ("medium", ("impai", "normal"), ("ai", None)),
    "hota": ("hard", ("impai", "hard"), ("ai", None)),
    "ne": ("hard", ("impai", "normal"), ("impai", "easy")),
    "hn": ("hard", ("impai", "hard"), ("impai", "normal")),
    "dry": ("hard", ("ai", None), ("ai", None)),
}
LADDER = ("ne", "hn")
CAP = 81000
# Ticks between two looks at the units alive. The look does not find a game's end: the units clause of every hold
# does, pausing at the frame visit after the step in which a seat's count reached 0 (Lane.hold), so a longer step
# cannot pass an ending: a game is read at the tick its count fell, whatever the step. The step decides how often
# the level is held -- each hold is five waits on the game's answers -- and how far apart the two `looks` a result
# keeps lie.
# Nothing else reads it: the stall rule reads the pulse lines, which ImpAI logs once a game minute whether the
# level is held or not, and the cap is held at exactly, since no hold is armed past it (`min(..., cap)`; CAP is 27
# steps).
STEP = 3000
REC, STRIDE = 0x1B63, 0x14B             # record 0 in main, a record (tagpu_engine.h)
ALIVE = 0x144                           # WORD, the units alive (engine/units.md)
LOST = 0x106                            # WORD, the commanders lost: the destructor's 0x486996 (engine/units.md)
CMDR_DEATH = 0x37EF6                    # u32 ActiveCommanderDeath
PULSE = re.compile(r"impai: pulse seat=(\d+) t=(\d+) built=(\d+) mroots=(\d+) eroots=(\d+) army=(\d+) "
                   r"squads=(\S+) metal=(\S+) energy=(\S+) vlive=(\S+) vbuilt=(\S+)")
TABLE = re.compile(r"impai: table seat=(\d+) hash=([0-9a-f]{16}) from=(\w+)")
ECO_END = re.compile(r"impai: eco seat=(\d+) (.*)")                       # the level end's (tagpu_impai.c)
MIL_END = re.compile(r"impai: mil seat=(\d+) level=\S+ (.*)")
MIL_TASK = re.compile(r"impai: mil seat=(\d+) t=(\d+) attack=(\d+) raid=(\d+) scout=(\d+)")   # tasks begun
LEVEL_END = "impai: handicap rows"      # every level end writes it, whoever plays (tagpu_impai_level_end)
REC_START = re.compile(r"recorder: recording (?!timeline )(.+?\.imprec) from GameTime")
REC_CLOSED = re.compile(r"recorder: writer: (.+?\.imprec) closed:")
PASS_TICKS = 30                         # an economy pass: the income a pass is what is produced in 30 ticks
STALL_WINDOW, STALL_FROM = 3600, 1800
LEVER_LEVEL, LEVER_TABLE = "tagpu_impai_level.on", "tagpu_impai_table.on"
# the pause a hold waits for: at its T, or earlier by a seat's units clause; or its T not taken
PAUSED = r"pause: (?:paused at GameTime (\d+)(?: \(units of seat \d+ <= \d+\))?\s*$|at {T} not taken.*)"
VULKAN = "vulkan"                       # the default renderer's name in a result


class BarError(Exception):
    pass


def next_hold(T: int, cap: int) -> int:
    """The GameTime of the hold after one at T: the next multiple of STEP, never past the cap, so a game is held
    at its cap exactly, whatever the cap -- the bar's 81 000 or a row's shorter bound."""
    return min((T // STEP + 1) * STEP, cap)


def renderer(gpu: bool) -> str:
    return VULKAN if gpu else "gdi"


def pacing(gpu: bool, draw: bool) -> str:
    """THE PACING: "draw" on the GPU or with --draw, else "nodraw"."""
    return "draw" if gpu or draw else "nodraw"


def game_kept(out: Path, game: dict, gpu: bool, draw: bool = False):
    """The result of `game` already in `out`, or None: a game that ran (not could_not_run) to the same cap, on
    the same renderer at the same pacing (a result that names none was drawn), each seat with the table it is to
    play (a seat's table_hash, where the spec names one)."""
    p = Path(out) / f"{game['id']}.json"
    try:
        r = json.loads(p.read_text())
    except (OSError, ValueError):
        return None
    if r.get("id") != game["id"] or r.get("outcome") in (None, "could_not_run"):
        return None
    if r.get("cap", CAP) != game.get("cap", CAP) or r.get("renderer") != renderer(gpu):
        return None
    if r.get("pacing", "draw") != pacing(gpu, draw):
        return None
    for s in ("A", "B"):
        want = game["seats"][s].get("table_hash")
        if want and (r.get("tables") or {}).get(s) != want:
            return None
    return r


# ---- the game list ----

def ids(pair: str, maps=None) -> list:
    if pair not in PAIRS:
        raise BarError(f"no pair {pair!r}: {', '.join(PAIRS)}")
    maps = maps or list(MAPS)
    if pair == "dry":
        return [f"bar-dry-{m}-ac-{n}" for m in maps for n in (1, 2)]
    return [f"bar-{pair}-{m}-{s}-{n}" for m in maps for s in ("aa", "ac", "ca", "cc") for n in range(1, 6)]


def parse_id(gid: str) -> dict:
    """A bar game's spec, in the shape tourney.game_spec gives: map, sides, start (A's: 0 first, 1 second),
    seats {A, B: {ai, level}}, cap; and the bar's own pair, its Difficulty, and `recordings`, which keeps the
    recording of a game kept for a diagnosis (THE END in the module's docstring)."""
    try:
        bar, pair, m, sides, n = gid.split("-")
        assert bar == "bar" and pair in PAIRS and m in MAPS and len(sides) == 2 and set(sides) <= set(SIDE)
        n = int(n)
    except (ValueError, AssertionError) as e:
        raise BarError(f"not a bar game id: {gid!r}") from e
    diff, a, b = PAIRS[pair]
    return {"id": gid, "pair": pair, "map": m, "sides": {"A": SIDE[sides[0]], "B": SIDE[sides[1]]},
            "start": 0 if n % 2 else 1, "rep": n, "difficulty": diff,
            "seats": {"A": {"ai": a[0], "level": a[1]}, "B": {"ai": b[0], "level": b[1]}}, "cap": CAP,
            "recordings": True}


# ---- the scenario ----

def map_name(m: str) -> str:
    return MAPS.get(m, m)


def seat_of(game: dict) -> dict:
    """{"A": seat, "B": seat}: A sits in seat 1 when it plays the first start, else in seat 2."""
    return {"A": 1, "B": 2} if game["start"] == 0 else {"A": 2, "B": 1}


def start_positions(name: str, seats: int = 3, vfs=None) -> dict:
    """{StartPos N: (x, z)} of the schema a skirmish of `seats` seats reads: the schema whose start count equals the
    seats, else the one with most, the first of equals (N-b1)."""
    import tavfs
    vfs = vfs or tavfs.Vfs(tavfs.find_install(None))
    hit = None
    for path, where in vfs.enumerate("maps", "*.ota", False):
        if Path(path).stem.lower() == name.lower():
            hit = vfs.read_from(path, where)
            break
    if hit is None:
        raise BarError(f"no map {name!r} in the install")
    schemas = []
    for top in tavfs.tdf_sections(hit):
        for sub in top["sub"]:
            if not sub["_name"].lower().startswith("schema"):
                continue
            pos = {}
            for s in sub["sub"]:
                if s["_name"].lower() != "specials":
                    continue
                for sp in s["sub"]:
                    what = (sp["keys"].get("specialwhat") or "").lower()
                    if what.startswith("startpos") and what[8:].isdigit():
                        pos.setdefault(int(what[8:]), (int(float(sp["keys"].get("xpos", 0))),
                                                       int(float(sp["keys"].get("zpos", 0)))))
            schemas.append(pos)
    if not schemas:
        raise BarError(f"{name}: no schema")
    exact = [p for p in schemas if len(p) == seats]
    return exact[0] if exact else max(schemas, key=len)


def game_doc(game: dict, starts: dict) -> dict:
    doc = json.loads(TEMPLATE.read_text())
    s = doc["setup"]
    s["map"] = map_name(game["map"])
    s["difficulty"] = game["difficulty"]
    seat = seat_of(game)
    doc["units"] = []
    for side in ("A", "B"):
        k = seat[side]
        if k + 1 not in starts:
            raise BarError(f"{s['map']}: no StartPos{k + 1}")
        x, z = starts[k + 1]
        ctl = "impai" if game["seats"][side]["ai"] == "impai" else "ai"
        s["players"].append({"slot": k, "controller": ctl, "side": game["sides"][side], "color": k,
                             "storage": {"metal": 1000, "energy": 1000}})
        doc["units"].append({"id": f"cmd{k}", "type": COMMANDER[game["sides"][side]], "owner": k, "pos": [x, z],
                             "facing": 0})
    s["players"].sort(key=lambda p: p["slot"])
    doc["description"] = f"bar.py: {game['id']}"
    return doc


# ---- the stall rule ----

def pulses(lines) -> dict:
    """{seat: [(t, built, mroots, eroots, army)]} in GameTime order"""
    out = {}
    for ln in lines:
        m = PULSE.search(ln)
        if m:
            seat, t, built, mr, er, army = (int(m.group(i)) for i in range(1, 7))
            out.setdefault(seat, []).append((t, built, mr, er, army))
    for v in out.values():
        v.sort()
    return out


def stalls(rows, end: int) -> list:
    """The stall windows [t0, t0 + 3600] of one seat's pulses: from a pulse after GameTime 1800 to the pulse 3600
    ticks later, before the game's end."""
    at = {r[0] // 1800: r for r in rows}
    out = []
    for r0 in rows:
        if r0[0] < STALL_FROM:
            continue
        r1 = at.get(r0[0] // 1800 + STALL_WINDOW // 1800)
        if not r1 or r1[0] > end:
            continue
        econ = r1[1] == r0[1] and r1[3] == r0[3]
        army = r0[4] >= 1 and r1[2] == r0[2]
        if econ or army:
            out.append({"from": r0[0], "to": r1[0], "economy": econ, "army": army})
    return out


def income_max(lines, k: int):
    """Seat k's best metal income a pass over a pulse minute -- the rise of its TotalMetalProduced between two
    pulses, over the passes between them -- or None with fewer than two pulses. The economy pass adds each pass's
    income to TotalMetalProduced (eco_bench.py, scout H1), so a minute's mean is at most its best pass's income:
    an income this reaches, some pass of that minute reached."""
    rows = sorted((int(m.group(2)), float(m.group(8))) for ln in lines for m in [PULSE.search(ln)]
                  if m and int(m.group(1)) == k)
    best = None
    for (t0, m0), (t1, m1) in zip(rows, rows[1:]):
        if t1 > t0:
            v = (m1 - m0) * PASS_TICKS / (t1 - t0)
            best = v if best is None or v > best else best
    return None if best is None else round(best, 2)


# ---- the level's end ----

def fields(text: str) -> dict:
    """{key: value} of a line's `key=value` words"""
    return dict(w.split("=", 1) for w in text.split() if "=" in w)


def counts(text) -> dict:
    """{name: n} of a level-end list `name:n,name:n`, {} for `-` or none"""
    out = {}
    for item in (text or "").split(","):
        name, _, n = item.partition(":")
        if name and name != "-" and n.isdigit():
            out[name] = int(n)
    return out


def _int(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def _float(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def level_end(lines, seat: dict) -> dict:
    """{A, B: ...} for each side whose seat wrote level-end lines (tagpu_impai.c, impai_play_end): from its `eco`
    line its units completed (`built`), its army units completed by class (`army`, ImpAI's own classes, and their
    sum `army_built`), its factories completed by the tech level they offer (`factories`, {"1": n, "2": n}; "3" holds 3
    and above), its water-only factories completed (`yards`), the sea plants it ordered and its income at the
    end; from its `mil` line its attack waves, raids and scouting runs begun, its naval judgement (-1 never
    judged) and the income a pass at which it techs up (`tech_m`). A key the line does not carry is None."""
    by_seat = {v: k for k, v in seat.items()}
    out = {}
    for ln in lines:
        m = ECO_END.search(ln)
        if m and int(m.group(1)) in by_seat:
            f = fields(m.group(2))
            army = counts(f.get("classes"))
            out.setdefault(by_seat[int(m.group(1))], {}).update({
                "built": _int(f.get("built")), "army": army, "army_built": sum(army.values()),
                "factories": counts(f.get("ftech")), "yards": _int(f.get("yards")),
                "sea_plants": _int(f.get("sea_plants")), "metal_in": _float(f.get("metal_in"))})
            continue
        m = MIL_END.search(ln)
        if m and int(m.group(1)) in by_seat:
            f = fields(m.group(2))
            out.setdefault(by_seat[int(m.group(1))], {}).update({
                "attacks": _int(f.get("attacks")), "raids": _int(f.get("raids")), "scouts": _int(f.get("scouts")),
                "naval": _int(f.get("naval")), "tech_m": _float(f.get("tech_m")), "dguns": _int(f.get("dguns")),
                "defends": _int(f.get("defends")), "retreats": _int(f.get("retreats"))})
    return out


def first_ticks(lines, seat: dict, impai) -> dict:
    """{A, B: {attack, raid, scout}} for each side of `impai`: the GameTime of the military's first task line
    that began an attack wave, a raid, a scouting run (`impai: mil seat= t= attack= raid= scout=`), None for a
    kind it never began."""
    by_seat = {seat[s]: s for s in impai}
    out = {s: {"attack": None, "raid": None, "scout": None} for s in impai}
    for ln in lines:
        m = MIL_TASK.search(ln)
        if not m or int(m.group(1)) not in by_seat:
            continue
        got, t = out[by_seat[int(m.group(1))]], int(m.group(2))
        for kind, g in (("attack", 3), ("raid", 4), ("scout", 5)):
            if int(m.group(g)) and (got[kind] is None or t < got[kind]):
                got[kind] = t
    return out


def for_diagnosis(res: dict) -> bool:
    """A game kept for a diagnosis: one A did not win, or one in which an ImpAI seat stalled."""
    return res.get("outcome") != "A" or any((res.get("stalls") or {}).values())


def read_result(lines, seat: dict, game: dict, outcome: str, end: int) -> dict:
    """What a game's result takes from its launch's log, the level ended: each seat's table hash from its
    `impai: table` line; each seat's value built and alive from its last pulse; and for each ImpAI seat its
    stalls, its level end (level_end), its first tasks (first_ticks) and its best income (income_max)."""
    impai = [s for s in ("A", "B") if game["seats"][s]["ai"] == "impai"]
    by_seat = {v: k for k, v in seat.items()}
    p = pulses(lines)
    value = {}
    for s in ("A", "B"):
        rows = [m for ln in lines for m in [PULSE.search(ln)] if m and int(m.group(1)) == seat[s]]
        if rows:
            value[s] = {"built": float(rows[-1].group(11)), "live": float(rows[-1].group(10))}
    ends = level_end(lines, seat)
    return {"tables": {by_seat[int(m.group(1))]: m.group(2) for ln in lines for m in [TABLE.search(ln)]
                       if m and int(m.group(1)) in by_seat},
            "outcome": outcome, "end_tick": end, "value": value,
            "stalls": {s: stalls(p.get(seat[s], []), end) for s in impai},
            "end": {s: ends[s] for s in impai if s in ends},
            "first": first_ticks(lines, seat, impai),
            "income_max": {s: income_max(lines, seat[s]) for s in impai}}


def recording_of(lines):
    """(the recording's path as the recorder logged it, its writer closed) of a launch's log; None when it
    recorded nothing"""
    rec = None
    for ln in lines:
        m = REC_START.search(ln)
        if m:
            rec = m.group(1)
    if rec is None:
        return None
    name = rec.replace("\\", "/").rsplit("/", 1)[-1]
    closed = any(m and m.group(1).replace("\\", "/").rsplit("/", 1)[-1] == name
                 for ln in lines for m in [REC_CLOSED.search(ln)])
    return rec, closed


# ---- a lane ----

class Lane:
    """One tareplay Worker and the games it plays, one at a time."""

    def __init__(self, j, out, dll, gpu=False):
        import tareplay as R
        self.R = R
        self.j, self.out, self.dll = j, out, dll
        self.w = R.Worker(out)
        self.w.turbo = 1                # THE TURBO in the module's docstring: k > 1 plays another game
        self.w.held = True
        self.w.gdi = not gpu
        self.w.nodraw = pacing(gpu, _PACING["draw"]) == "nodraw"     # THE PACING
        self.log = None

    def peek(self, off: int, size: int) -> int:
        R = self.R
        spec = f"*0x511DE8+0x{off:X}:x{size}"
        self.log.read()
        n = len(self.log.lines)
        R._CLI_API.put_trigger(self.w.gamedir / "tagpu_peek.trigger", spec + "\n")
        end = time.monotonic() + R.ACK_S
        while True:
            self.log.read()
            rows = R._CLI_API._peek_rows("\n".join(self.log.lines[n:]), [spec])
            if rows:
                try:
                    return int.from_bytes(bytes.fromhex(rows[0]["value"]), "little")
                except ValueError:
                    raise R.CouldNotRun(f"peek {spec}: {rows[0]['value']}")
            if time.monotonic() >= end:
                raise R.CouldNotRun(f"peek {spec}: no answer in {R.ACK_S} s")
            R.nap(self.log.NAP_S)
            self.log.alive()

    def hold(self, T, seats=()):
        """The level run on and held at GameTime T, or at the first frame visit before it at which a seat of
        `seats` holds no unit (`pause at T or units <s> <= 0`, tagpu_replay.c THE PAUSE AT T): at k = 1 that is
        the visit after the step in which its count fell to 0, so a game's end is read at its own tick, never up
        to a STEP later, when the other seat's count may have fallen to 0 too. Returns the GameTime held."""
        R = self.R
        line = f"pause at {T}" + "".join(f" or units {k} <= 0" for k in seats)
        n = len(self.log.lines)
        R.arm(self.w, self.log, line, rf"pause: armed for GameTime {T}\b", rf"pause: at {T} refused: .*",
              f"pause at {T}")
        self.log.pending, self.log.hold_from = T, n
        R.turbo_engage(self.w, self.log)
        R.resume(self.w, self.log)
        rx = PAUSED.replace("{T}", str(T))
        m = re.search(rx, self.log.wait_running(rx, n))
        if not m.group(1):
            raise R.CouldNotRun(f"pause at {T}: {m.group(0).strip()}")
        at = int(m.group(1))
        self.log.pending, self.log.held, self.log.at = None, True, at
        return at

    def look(self, seat: dict) -> dict:
        """{A, B: {alive, lost}}: each seat's units alive and commanders lost, read on the held level."""
        rec = {s: REC + seat[s] * STRIDE for s in ("A", "B")}
        return {s: {"alive": self.peek(rec[s] + ALIVE, 2), "lost": self.peek(rec[s] + LOST, 2)} for s in ("A", "B")}

    def keep_log(self, gid: str):
        """The game's instance log from its launch on, beside its result: the next launch on this lane starts a
        run of its own, so a game is diagnosed later from this copy or not at all."""
        if self.log is None:
            return
        try:
            self.log.read()
        except self.R.Failed:
            pass                        # a COB stop's line is in `lines` all the same: read() keeps it, then raises
        (self.out / f"{gid}.tagpu.log").write_text("\n".join(self.log.lines) + "\n")

    def lever(self, name, text):
        """A lever file in the game folder, written fresh: unlinked first, so a link the instance holds is never
        written through; none when text is empty."""
        p = self.w.gamedir / name
        p.unlink(missing_ok=True)
        if text:
            p.write_text(text + "\n")

    def play(self, game: dict) -> dict:
        R = self.R
        R.stopping()
        self.log = None
        self.w.watch_caps()             # this game's refusals of the GPU memory cap, none before it
        t0 = time.monotonic()
        seat = seat_of(game)
        res = {"id": game["id"], "map": game["map"], "sides": game["sides"], "start": game["start"],
               "seats": {s: dict(game["seats"][s], seat=seat[s]) for s in ("A", "B")},
               "difficulty": game.get("difficulty", "hard"), "cap": game.get("cap", CAP),
               "renderer": renderer(not self.w.gdi), "pacing": "nodraw" if self.w.nodraw else "draw"}
        doc = game_doc(dict(game, difficulty=res["difficulty"]), start_positions(map_name(game["map"])))
        scn = self.out / f"{game['id']}.scenario.json"
        scn.write_text(json.dumps(doc, indent=1) + "\n")
        levels = [f"{seat[s]}={game['seats'][s]['level']}" for s in ("A", "B")
                  if game["seats"][s]["ai"] == "impai" and game["seats"][s].get("level")]
        tables = []
        for s in ("A", "B"):
            path = game["seats"][s].get("table")
            if path:
                name = f"impai_table_{seat[s]}.bin"
                (self.w.gamedir / name).unlink(missing_ok=True)
                shutil.copyfile(path, self.w.gamedir / name)
                tables.append(f"{seat[s]}={name}")
        self.w.place({"name": "impai-bar"}, self.dll)
        self.lever(LEVER_LEVEL, ",".join(levels))
        self.lever(LEVER_TABLE, ",".join(tables))
        self.log = R.Log(self.w)
        rc, o = R.tacli("scenario", "load", self.w.name, scn, "--defaults", "--restart", "--keep-dll",
                        timeout=R.LAUNCH_TIMEOUT)
        if rc:
            raise R.CouldNotRun(f"scenario load {scn.name}: {R.last(o)}")
        T = R.held_start(self.w, self.log)
        res["commander_death"] = self.peek(CMDR_DEATH, 4)
        cap = res["cap"]
        outcome, end = "cap", cap
        looks = []
        while True:
            T = self.hold(next_hold(T, cap), (seat["A"], seat["B"]))
            alive = {s: self.peek(REC + seat[s] * STRIDE + ALIVE, 2) for s in ("A", "B")}
            looks = (looks + [{"at": T, "seats": {s: {"alive": alive[s]} for s in ("A", "B")}}])[-2:]
            if not alive["A"] or not alive["B"]:
                # both gone at one tick: no side won, judged as a game run to the cap (a failure for ImpAI)
                outcome = "cap" if not alive["A"] and not alive["B"] else "B" if not alive["A"] else "A"
                res["draw"] = outcome == "cap"
                end = T
                break
            if T >= cap:
                break
        looks[-1]["seats"] = self.look(seat)
        res["looks"] = looks
        R.say(f"lane {self.j}: {game['id']}: at {T}: " + ", ".join(
            f"{s} (seat {seat[s]}) {v['alive']} alive, {v['lost']} commanders lost"
            for s, v in looks[-1]["seats"].items()))
        if end == cap:
            self.hold(cap + 15)             # both seats' pulses of the cap's minute are written by then
        self.end_level()
        # the game has ended: a refusal of the GPU memory cap anywhere in it fails it by name
        refused = self.w.cap_failure()
        if refused:
            raise R.Failed(refused)
        lines = list(self.log.lines)
        res.update(read_result(lines, seat, game, outcome, end))
        if self.w.nodraw:
            res["nodraw"] = self.log.nodraw
        res["wall_s"] = round(time.monotonic() - t0, 1)
        keep = for_diagnosis(res)
        res["recording"] = self.file_recording(game["id"], keep and bool(game.get("recordings")))
        if keep:
            self.keep_log(game["id"])
        R.say(f"lane {self.j}: {game['id']}: {outcome} at {end} ({res['wall_s']} s)"
              + (", kept for a diagnosis" if keep else ""))
        return res

    def end_level(self):
        """The level ended through the engine's exit, the outcome read (THE END in the module's docstring), and
        the launch's log read to its last line: CouldNotRun when that exit did not pass the teardown."""
        R = self.R
        self.w.quit()
        self.log.read()
        if not any(LEVEL_END in ln for ln in self.log.lines):
            raise R.CouldNotRun(f"the game's exit wrote no level end (`{LEVEL_END}`)")

    def file_recording(self, gid: str, keep: bool):
        """The launch's recording moved beside the game's result as `<gid>.imprec` when `keep`, its path
        returned; otherwise deleted, and None. A recording to keep must have been made and closed (its writer's
        `closed` line): CouldNotRun otherwise, since a diagnosis reads it. Every file left in the instance's
        replays folder goes with it, the locks included, so the next launch on the lane starts from none."""
        R = self.R
        replays = self.w.gamedir / "replays"
        path = None
        if keep:
            rec = recording_of(self.log.lines)
            if rec is None:
                raise R.CouldNotRun("a game kept for a diagnosis made no recording")
            if not rec[1]:
                raise R.CouldNotRun(f"the recording {rec[0]} was not closed: its writer logged no `closed`")
            src = replays / rec[0].replace("\\", "/").rsplit("/", 1)[-1]
            if not src.is_file():
                raise R.CouldNotRun(f"the recording {rec[0]} is not in {replays}")
            path = (self.out / f"{gid}.imprec").resolve()
            shutil.move(str(src), str(path))
        for f in replays.glob("*.imprec*") if replays.is_dir() else []:
            f.unlink(missing_ok=True)
        return str(path) if path else None

    def abandon(self):
        try:
            self.w.stop(hard=True)
        except self.R.CouldNotRun as e:
            self.R.say(f"lane {self.j}: {e}")

    def close(self):
        self.w.close()


_LANES, _LANES_LOCK, _DLL = {}, threading.Lock(), {}
# THE PACING of this process's lanes: `play`'s --draw, set before the first lane is made; one process plays at one
# pacing, as on one renderer
_PACING = {"draw": False}


def _dll():
    import tareplay as R
    with _LANES_LOCK:
        if "path" not in _DLL:
            dll, make = R.build_dll(False, False)
            if not Path(dll).exists():
                raise R.CouldNotRun(f"no DLL at {dll} ({make})")
            _DLL["path"] = Path(dll).resolve()
        return _DLL["path"]


def play_game(game: dict, lane: int, out_dir: Path, gpu: bool = False) -> dict:
    """One game on lane `lane` (one Worker a lane, made at its first game and kept), its result in tourney.py's
    format: outcome A, B, cap or could_not_run, the end tick, each seat's table hash from its `impai: table` line,
    each ImpAI seat's value built and alive from its last pulse; and the bar's own (read_result, file_recording):
    each ImpAI seat's stalls, level end, first tasks and best income, and the kept recording's path. Called from
    one thread a lane; the caller holds the run lock, with a GPU token a lane when `gpu`; one process plays on
    one renderer."""
    import tareplay as R
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    try:
        dll = _dll()
        with _LANES_LOCK:
            if lane not in _LANES:
                _LANES[lane] = Lane(lane, out_dir, dll, gpu)
            ln = _LANES[lane]
        ln.out = out_dir
        try:
            return ln.play(game)
        except (R.CouldNotRun, R.Failed, BarError):
            ln.abandon()
            if not R.STOP.is_set():
                ln.keep_log(game["id"])
            raise
    except (R.CouldNotRun, R.Failed, BarError) as e:
        return {"id": game["id"], "outcome": "could_not_run", "reason": "stopped" if R.STOP.is_set() else str(e),
                "renderer": renderer(gpu), "pacing": pacing(gpu, _PACING["draw"]), "cap": game.get("cap", CAP)}


def close_lanes():
    with _LANES_LOCK:
        for ln in _LANES.values():
            ln.close()
        _LANES.clear()


def stop_lanes(js):
    """The game of each lane of `js` stopped through its own instance (Lane.abandon: tacli stop, by the PID its
    prefix runs), from the thread that took the signal: a lane's thread can sit in one tacli call for minutes (a
    launch, tareplay.LAUNCH_TIMEOUT), and its game ends now rather than when that call returns."""
    with _LANES_LOCK:
        lanes = [_LANES[j] for j in js if j in _LANES]
    for ln in lanes:
        ln.abandon()


def play_all(games: list, lanes: int, out: Path, gpu: bool, play=None, stop=None) -> list:
    """Every game of `games` on `lanes` lanes, each result written into `out` as its game ends; the results. One
    thread a lane, each taking the next game from one queue, so no two threads ever play on one lane's Worker.

    THE STOP. SIGINT and SIGTERM set tareplay's STOP while this runs. From then on no lane takes a game, every wait
    of a lane's game gives up (tareplay.nap raises), the game of every lane still playing is stopped through its
    own instance (`stop`, stop_lanes), and play_game writes each such game as could_not_run, reason "stopped". A
    game not taken is not written. The caller closes the lanes after this returns, which stops every lane's
    instance again once its thread has ended, so no game outlives the run whatever its lane was doing when told.
    A harness defect in one game (any other exception) stops the run the same way and is raised here after."""
    import tareplay as R
    play = play or play_game
    stop = stop or stop_lanes
    out = Path(out)
    todo = queue.Queue()
    for g in games:
        todo.put(g)
    results, busy, errors, lock = [], {}, [], threading.Lock()

    def lane(j):
        while not R.STOP.is_set():
            try:
                g = todo.get_nowait()
            except queue.Empty:
                return
            with lock:
                busy[j] = g["id"]
            try:
                r = play(g, j, out, gpu)
            except Exception as e:
                with lock:
                    errors.append(e)
                R.STOP.set()
                r = {"id": g["id"], "outcome": "could_not_run", "reason": f"{type(e).__name__}: {e}",
                     "renderer": renderer(gpu), "pacing": pacing(gpu, _PACING["draw"]), "cap": g.get("cap", CAP)}
            (out / f"{g['id']}.json").write_text(json.dumps(r, indent=1) + "\n")
            with lock:
                results.append(r)
                busy.pop(j, None)

    # The handler only notes the signal: it runs on this thread between two bytecodes, where a lock this thread
    # holds -- STOP's own condition, the print lock -- would deadlock it. This loop sets STOP and says so.
    signalled = []
    threads = [threading.Thread(target=lane, args=(j,), name=f"bar-lane-{j}")
               for j in range(min(lanes, len(games)))]
    before = {s: signal.signal(s, lambda signum, frame: signalled.append(signum))
              for s in (signal.SIGINT, signal.SIGTERM)}
    try:
        for t in threads:
            t.start()
        told = False
        while any(t.is_alive() for t in threads):
            time.sleep(0.2)
            if told or not (signalled or R.STOP.is_set()):
                continue
            told = True
            R.STOP.set()
            if signalled:
                R.say(f"bar: {signal.Signals(signalled[0]).name}: no game starts; each lane's game is stopped")
            with lock:
                playing = sorted(busy)
            stop(playing)
    finally:
        for s, h in before.items():
            signal.signal(s, h)
    if signalled:
        R.STOP.set()                    # one that came as the last game ended still stops the run
    if errors:
        raise errors[0]
    return results


# ---- the verdict ----

def read_results(dirs) -> list:
    rows = []
    for d in dirs:
        for p in sorted(Path(d).glob("bar-*.json")):
            if not p.name.endswith(".scenario.json"):
                rows.append(json.loads(p.read_text()))
    return rows


def first_summary(rs) -> list:
    """For each side with first tasks in `rs`, a line a kind: in how many games it began one, the range and the
    median of the first tick, and the games it began none in."""
    out = []
    for side in ("A", "B"):
        got = [(r["id"], r["first"][side]) for r in rs if side in (r.get("first") or {})]
        for kind in ("attack", "raid", "scout") if got else ():
            ts = sorted(f[kind] for _, f in got if f.get(kind) is not None)
            none = [gid for gid, f in got if f.get(kind) is None]
            line = f"{side}'s first {kind}: in {len(ts)} of {len(got)} games"
            if ts:
                line += f", GameTime {ts[0]}..{ts[-1]}, median {ts[(len(ts) - 1) // 2]}"
            if none:
                line += f"; none in {' '.join(none[:12])}{' ...' if len(none) > 12 else ''}"
            out.append(line)
    return out


def judge(dirs) -> int:
    rows = read_results(dirs)
    if not rows:
        print("bar judge: no game in " + ", ".join(map(str, dirs)))
        return 2
    unnamed = [r.get("id", "?") for r in rows if not r.get("renderer")]
    if unnamed:
        print(f"bar judge: COULD NOT RUN: {len(unnamed)} results name no renderer: {' '.join(unnamed[:12])}"
              f"{' ...' if len(unnamed) > 12 else ''}; a verdict reads games of one renderer, as each result names it")
        return 2
    rs = sorted({r["renderer"] for r in rows})
    if len(rs) > 1:
        print(f"bar judge: COULD NOT RUN: the games were rendered by {' and '.join(rs)}; a verdict reads games "
              f"of one renderer")
        return 2
    by_pair = {}
    for r in rows:
        by_pair.setdefault(r["id"].split("-")[1], []).append(r)
    fail = lacking = 0
    for pair in PAIRS:
        rs = by_pair.get(pair, [])
        if not rs:
            continue
        wins = [r["id"] for r in rs if r.get("outcome") == "A"]
        losses = [r["id"] for r in rs if r.get("outcome") == "B"]
        capped = [r["id"] for r in rs if r.get("outcome") == "cap"]
        cnr = [r["id"] for r in rs if r.get("outcome") == "could_not_run"]
        stalled = [(r["id"], s, w) for r in rs for s, ws in (r.get("stalls") or {}).items() for w in ws]
        walls = sorted(r["wall_s"] for r in rs if "wall_s" in r)
        need = len(ids(pair))
        print(f"{pair}: {len(wins)} of {len(rs)} won by A, {len(losses)} lost, {len(capped)} at the cap, "
              f"{len(cnr)} could not run, {len(stalled)} stalls"
              + (f"; wall {walls[0]}..{walls[-1]} s, 95th {walls[int(0.95 * (len(walls) - 1))]} s" if walls else ""))
        for name, lst in (("lost", losses), ("cap", capped), ("could not run", cnr)):
            if lst:
                print(f"  {name}: {' '.join(lst[:12])}{' ...' if len(lst) > 12 else ''}")
        for gid, s, w in stalled[:12]:
            print(f"  stall: {gid} seat {s} {w['from']}..{w['to']}{' economy' if w['economy'] else ''}"
                  f"{' army' if w['army'] else ''}")
        for line in first_summary(rs):
            print(f"  {line}")
        kept = [r["id"] for r in rs if r.get("recording")]
        if kept:
            print(f"  recordings kept: {len(kept)}, beside their results")
        dead = [r["id"] for r in rs if r.get("commander_death") == 0]
        if dead:
            print(f"  commander-death rule off in: {' '.join(dead)}")
        if pair == "dry":
            continue
        if len(rs) < need:
            lacking += 1
            print(f"  lacks {need - len(rs)} of its {need} games")
        if cnr:
            lacking += 1
        elif pair in LADDER:
            fail += len(wins) < 90
        else:
            fail += len(wins) < len(rs) or bool(stalled)
    verdict = 2 if lacking else 1 if fail else 0
    print(f"bar judge: {('PASS', 'FAIL', 'COULD NOT RUN')[verdict]}")
    return verdict


# ---- A2b's row checks ----

ROWS = {    # a2-spec.md section 9: the pair, the bound each game is played to, the games and maps it asks
    "b4": {"pair": "hota", "cap": 27000, "games": 4, "maps": 2},
    "b5": {"pair": "hota", "cap": 36000, "games": len(MAPS), "maps": len(MAPS)},
}
# b5's "an army of each wanted role": the classes the seat's table wants depend on its picture and its factories'
# menus at each decision, and no line logs them, so the check asks for a composition of at least this many
# classes completed, and prints the classes for the reader.
ROLES_MIN = 2
SEA_MAPS = ("redtriangle",)             # the bar's sea map: a yard is asked there whatever ImpAI judged


def score(row: str, r: dict) -> list:
    """[(criterion, met, detail)] of row b4 or b5 for side A of a result that ran to the row's bound."""
    end, first = r["end"]["A"], (r.get("first") or {}).get("A") or {}
    if row == "b4":
        st = (r.get("stalls") or {}).get("A") or []
        return [("attack wave", first.get("attack") is not None,
                 f"GameTime {first['attack']}" if first.get("attack") is not None else "none"),
                ("scout", first.get("scout") is not None,
                 f"GameTime {first['scout']}" if first.get("scout") is not None else "none"),
                ("no stall", not st, f"{len(st)} stall windows, the first from {st[0]['from']}" if st else "none")]
    army = {k: n for k, n in (end.get("army") or {}).items() if n > 0}
    roles = ("roles", len(army) >= ROLES_MIN,
             " ".join(f"{k}:{n}" for k, n in sorted(army.items())) or "no army unit completed")
    naval = end.get("naval") == 1
    if r.get("map") in SEA_MAPS or naval:
        yard = ("yard", (end.get("yards") or 0) >= 1,
                f"{end.get('yards') or 0} completed, {end.get('sea_plants') or 0} ordered"
                + (", ImpAI judged the map naval" if naval else ""))
    else:
        yard = ("yard", True, "not asked: no sea map, and ImpAI did not judge it naval")
    income, tech_m = (r.get("income_max") or {}).get("A"), end.get("tech_m")
    high = sorted(int(t) for t, n in (end.get("factories") or {}).items() if n > 0 and t.isdigit())
    factories = " ".join(f"tech {t}:{n}" for t, n in sorted((end.get("factories") or {}).items())) or "none"
    if income is not None and tech_m and income >= tech_m:
        tech = ("tech-up", bool(high) and high[-1] >= 2,
                f"best income {income} a pass against tech_m {tech_m}; factories {factories}")
    else:
        tech = ("tech-up", True, f"not asked: best income {income} a pass against tech_m {tech_m}; "
                                 f"factories {factories}")
    return [roles, yard, tech]


def check(row: str, dirs) -> int:
    """Row b4's or b5's criteria over every game in `dirs`: exit 0 when every game meets each, 1 when one does
    not, 2 when a game could not run, is not the row's (another pair, another bound, no level-end lines of A's),
    or the games are fewer than the row asks."""
    spec = ROWS[row]
    rows = read_results(dirs)
    problems, played, fail = [], [], 0
    for r in rows:
        gid = r.get("id", "?")
        pair = gid.split("-")[1] if gid.count("-") == 4 else None
        if pair != spec["pair"]:
            problems.append(f"{gid}: not of the pair {spec['pair']} (ImpAI Hard against OTA AI Hard)")
        elif r.get("outcome") == "could_not_run":
            problems.append(f"{gid}: could not run: {r.get('reason')}")
        elif r.get("cap") != spec["cap"]:
            problems.append(f"{gid}: played to GameTime {r.get('cap')}; {row} plays to {spec['cap']} (--cap)")
        elif "A" not in (r.get("end") or {}):
            problems.append(f"{gid}: no level-end line of A's")
        else:
            got = score(row, r)
            played.append(r)
            fail += not all(ok for _, ok, _ in got)
            print(f"{gid}: {r['outcome']} at {r.get('end_tick')}: "
                  + "; ".join(f"{c} {'met' if ok else 'NOT MET'} ({d})" for c, ok, d in got))
    maps = {r["map"] for r in played}
    if len(played) < spec["games"] or len(maps) < spec["maps"]:
        problems.append(f"{len(played)} games on {len(maps)} maps; {row} asks {spec['games']} games on "
                        f"{spec['maps']} maps or more")
    for p in problems:
        print(f"  {p}")
    verdict = 2 if problems else 1 if fail else 0
    print(f"bar check {row}: {('PASS', 'FAIL', 'COULD NOT RUN')[verdict]}"
          + (f": {fail} of {len(played)} games miss a criterion" if fail else ""))
    return verdict


# ---- the command line ----

def read_ids(arg: str) -> list:
    if arg in PAIRS:
        return ids(arg)
    p = Path(arg)
    if p.exists():
        return [x for x in p.read_text().split() if x]
    return [x for x in arg.split(",") if x]


def cmd_play(a) -> int:
    import runlock
    games = [dict(parse_id(g), cap=a.cap) for g in read_ids(a.ids)]
    out = a.out or TREE / ".data" / "impai" / "bar" / time.strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    if a.dll:
        _DLL["path"] = Path(a.dll).resolve()
    _PACING["draw"] = a.draw
    left = [g for g in games if not game_kept(out, g, a.gpu, a.draw)]
    if len(left) < len(games):
        print(f"bar play: {len(games) - len(left)} games already in {out}, {len(left)} to play")
    games = left
    runlock.hold(TREE, "bar play " + " ".join(sys.argv[2:]), lanes=a.lanes, gpu=a.gpu)
    import tareplay as R
    try:
        results = play_all(games, a.lanes, out, a.gpu)
    finally:
        close_lanes()
        runlock.release()
    bad = [r["id"] for r in results if r.get("outcome") == "could_not_run"]
    if R.STOP.is_set():
        print(f"bar play: STOPPED: {len(results)} games ended in {out}, {len(bad)} of them stopped or could not "
              f"run; {len(games) - len(results)} not started")
        return 2
    print(f"bar play: {len(results)} games played in {out}, {len(bad)} could not run")
    return 2 if bad else 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="verb", required=True)
    i = sub.add_parser("ids", help="the game ids of a pair")
    i.add_argument("--pair", required=True)
    i.add_argument("--maps", default=None)
    pl = sub.add_parser("play", help="play a list of games on L lanes")
    pl.add_argument("--ids", required=True, help="a file of ids, a comma list, or a pair's name")
    pl.add_argument("--lanes", type=int, required=True)
    pl.add_argument("--out", type=Path, default=None)
    pl.add_argument("--dll", default=None)
    pl.add_argument("--cap", type=int, default=CAP,
                    help="the GameTime a game ends at: a row's bound (b4 27 000, b5 36 000, `check`) or a "
                         "shorter piece's; the bar's games end at 81 000")
    pl.add_argument("--gpu", action="store_true",
                    help="render on the default renderer, for debugging, taking a GPU token a lane; without it "
                         "the games render with GDI and take none")
    pl.add_argument("--draw", action="store_true",
                    help="render every frame, for debugging (THE PACING); --gpu implies it")
    j = sub.add_parser("judge", help="the verdict over the games in DIR...")
    j.add_argument("dirs", nargs="+", type=Path)
    c = sub.add_parser("check", help="A2b's row b4 or b5 over the games in DIR...")
    c.add_argument("row", choices=sorted(ROWS))
    c.add_argument("dirs", nargs="+", type=Path)
    a = p.parse_args(argv)
    try:
        if a.verb == "ids":
            print("\n".join(ids(a.pair, a.maps.split(",") if a.maps else None)))
            return 0
        if a.verb == "judge":
            return judge(a.dirs)
        if a.verb == "check":
            return check(a.row, a.dirs)
        return cmd_play(a)
    except BarError as e:
        print(f"bar: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
