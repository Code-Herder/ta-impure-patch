#!/usr/bin/env python3
"""bar.py -- ImpAI's bar: one-against-one games against OTA AI and between ImpAI's levels, judged.

    tools/impai/bar.py ids --pair PAIR [--maps a,b]       the game ids of a pair, one a line
    tools/impai/bar.py play --ids FILE|PAIR --lanes L [--out DIR] [--dll PATH] [--cap T] [--gpu]
    tools/impai/bar.py judge DIR...

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
(`looks`: the GameTime, each seat's units alive, and at the last also its commanders lost, +0x106); a game no side
won, and a game that could not run, keeps the instance log of its launch beside its result (`<id>.tagpu.log`).

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

THE STOP. SIGINT or SIGTERM during `play` sets tareplay's STOP: no game starts after it, every game still playing
is stopped through its own instance (tacli stop, which ends the PID found by that instance's prefix, then its
wineserver) and written as could_not_run, reason "stopped"; the lanes are closed, the run lock is released and the
exit is 2. A game not started is not written, so the same `play` resumes it.

RESUMING. `play` into an out dir that holds results skips every game whose result is already there, played to
the same cap on the same renderer with the same tables (game_kept): a game reloads its scenario from scratch at
its own launch (Lane.play: `scenario load --restart`), so a game skipped changes nothing another plays. A result
that could not run is played again.

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
STALL_WINDOW, STALL_FROM = 3600, 1800
LEVER_LEVEL, LEVER_TABLE = "tagpu_impai_level.on", "tagpu_impai_table.on"
# the pause a hold waits for: at its T, or earlier by a seat's units clause; or its T not taken
PAUSED = r"pause: (?:paused at GameTime (\d+)(?: \(units of seat \d+ <= \d+\))?\s*$|at {T} not taken.*)"
VULKAN = "vulkan"                       # the default renderer's name in a result


class BarError(Exception):
    pass


def renderer(gpu: bool) -> str:
    return VULKAN if gpu else "gdi"


def game_kept(out: Path, game: dict, gpu: bool):
    """The result of `game` already in `out`, or None: a game that ran (not could_not_run) to the same cap, on
    the same renderer, each seat with the table it is to play (a seat's table_hash, where the spec names one)."""
    p = Path(out) / f"{game['id']}.json"
    try:
        r = json.loads(p.read_text())
    except (OSError, ValueError):
        return None
    if r.get("id") != game["id"] or r.get("outcome") in (None, "could_not_run"):
        return None
    if r.get("cap", CAP) != game.get("cap", CAP) or r.get("renderer") != renderer(gpu):
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
    seats {A, B: {ai, level}}, cap; and the bar's own pair and Difficulty."""
    try:
        bar, pair, m, sides, n = gid.split("-")
        assert bar == "bar" and pair in PAIRS and m in MAPS and len(sides) == 2 and set(sides) <= set(SIDE)
        n = int(n)
    except (ValueError, AssertionError) as e:
        raise BarError(f"not a bar game id: {gid!r}") from e
    diff, a, b = PAIRS[pair]
    return {"id": gid, "pair": pair, "map": m, "sides": {"A": SIDE[sides[0]], "B": SIDE[sides[1]]},
            "start": 0 if n % 2 else 1, "rep": n, "difficulty": diff,
            "seats": {"A": {"ai": a[0], "level": a[1]}, "B": {"ai": b[0], "level": b[1]}}, "cap": CAP}


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
               "renderer": renderer(not self.w.gdi)}
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
            T = self.hold(min((T // STEP + 1) * STEP, cap), (seat["A"], seat["B"]))
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
        self.log.read()
        self.w.stop(hard=True)
        # the game has ended: a refusal of the GPU memory cap anywhere in it fails it by name
        refused = self.w.cap_failure()
        if refused:
            raise R.Failed(refused)
        if outcome == "cap":
            self.keep_log(game["id"])
        lines = list(self.log.lines)
        p = pulses(lines)
        by_seat = {v: k for k, v in seat.items()}
        res["tables"] = {by_seat[int(m.group(1))]: m.group(2) for ln in lines for m in [TABLE.search(ln)]
                         if m and int(m.group(1)) in by_seat}
        value = {}
        for s in ("A", "B"):
            rows = [m for ln in lines for m in [PULSE.search(ln)] if m and int(m.group(1)) == seat[s]]
            if rows:
                last = rows[-1]
                value[s] = {"built": float(last.group(11)), "live": float(last.group(10))}
        res.update({"outcome": outcome, "end_tick": end, "value": value,
                    "stalls": {s: stalls(p.get(seat[s], []), end) for s in ("A", "B")
                               if game["seats"][s]["ai"] == "impai"},
                    "wall_s": round(time.monotonic() - t0, 1)})
        R.say(f"lane {self.j}: {game['id']}: {outcome} at {end} ({res['wall_s']} s)")
        return res

    def abandon(self):
        try:
            self.w.stop(hard=True)
        except self.R.CouldNotRun as e:
            self.R.say(f"lane {self.j}: {e}")

    def close(self):
        self.w.close()


_LANES, _LANES_LOCK, _DLL = {}, threading.Lock(), {}


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
    each ImpAI seat's value built and alive from its last pulse. Called from one thread a lane; the caller holds
    the run lock, with a GPU token a lane when `gpu`; one process plays on one renderer."""
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
                "renderer": renderer(gpu), "cap": game.get("cap", CAP)}


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
                     "renderer": renderer(gpu), "cap": g.get("cap", CAP)}
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

def judge(dirs) -> int:
    rows = []
    for d in dirs:
        for p in sorted(Path(d).glob("bar-*.json")):
            if not p.name.endswith(".scenario.json"):
                rows.append(json.loads(p.read_text()))
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
    left = [g for g in games if not game_kept(out, g, a.gpu)]
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
                    help="the GameTime a game ends at: a shorter piece's check; the bar's games end at 81 000")
    pl.add_argument("--gpu", action="store_true",
                    help="render on the default renderer, for debugging, taking a GPU token a lane; without it "
                         "the games render with GDI and take none")
    j = sub.add_parser("judge", help="the verdict over the games in DIR...")
    j.add_argument("dirs", nargs="+", type=Path)
    a = p.parse_args(argv)
    try:
        if a.verb == "ids":
            print("\n".join(ids(a.pair, a.maps.split(",") if a.maps else None)))
            return 0
        if a.verb == "judge":
            return judge(a.dirs)
        return cmd_play(a)
    except BarError as e:
        print(f"bar: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
