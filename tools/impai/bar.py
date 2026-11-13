#!/usr/bin/env python3
"""bar.py -- ImpAI's bar: one-against-one games against OTA AI and between ImpAI's levels, judged.

    tools/impai/bar.py ids --pair PAIR [--maps a,b]       the game ids of a pair, one a line
    tools/impai/bar.py play --ids FILE|PAIR --lanes L [--out DIR] [--dll PATH] [--cap T]
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
(the DLL reads them at attach). A lane (a tareplay Worker, held, turbo) loads it, then every 300 ticks holds and
peeks seats 1 and 2's units alive, +0x144: the game ends when one is 0 -- the other side won -- or at the cap,
GameTime 81 000 (45 minutes), a failure for ImpAI. With the commander-death rule on, a commander's death kills its
side (0x48667E..0x4866B3); the harness reads the rule, ActiveCommanderDeath main+0x37EF6, into each result, since
no scenario key sets it. A hold the engine does not reach within the harness's bounds is "could not run", never an
outcome. A veteran CORKROG (29 918 HP) can survive the kill-all's 30 000 hit, so a side whose commander died can
still hold that one unit (scout N-b4).

THE STALL RULE, from ImpAI's pulse lines (`impai: pulse`, one a game minute a seat): a stall is a window of 3600
ticks, after GameTime 1800 and before the game's end, in which the seat's `built` and its `eroots` both did not
rise (the economy stopped), or in which it held at least one army unit and its `mroots` did not rise (the army
stopped).

Exit 0 when the verdict passed (or the command ran), 1 when it failed, 2 when it could not run."""
import argparse
import json
import re
import shutil
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
STEP = 300                              # ticks between two looks at the units alive
REC, STRIDE = 0x1B63, 0x14B             # record 0 in main, a record (tagpu_engine.h)
ALIVE = 0x144                           # WORD, the units alive (engine/units.md)
CMDR_DEATH = 0x37EF6                    # u32 ActiveCommanderDeath
PULSE = re.compile(r"impai: pulse seat=(\d+) t=(\d+) built=(\d+) mroots=(\d+) eroots=(\d+) army=(\d+) "
                   r"squads=(\S+) metal=(\S+) energy=(\S+) vlive=(\S+) vbuilt=(\S+)")
TABLE = re.compile(r"impai: table seat=(\d+) hash=([0-9a-f]{16}) from=(\w+)")
STALL_WINDOW, STALL_FROM = 3600, 1800
LEVER_LEVEL, LEVER_TABLE = "tagpu_impai_level.on", "tagpu_impai_table.on"


class BarError(Exception):
    pass


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

    def __init__(self, j, out, dll):
        import tareplay as R
        self.R = R
        self.j, self.out, self.dll = j, out, dll
        self.w = R.Worker(out)
        self.w.turbo = 1
        self.w.held = True
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

    def hold(self, T):
        R = self.R
        R.hold_at(self.w, self.log, T)
        R.turbo_engage(self.w, self.log)
        R.resume(self.w, self.log)
        R.wait_paused(self.w, self.log, T)

    def lever(self, name, text):
        """A lever file in the game folder, written fresh: unlinked first, so a link the instance holds is never
        written through; none when text is empty."""
        p = self.w.gamedir / name
        p.unlink(missing_ok=True)
        if text:
            p.write_text(text + "\n")

    def play(self, game: dict) -> dict:
        R = self.R
        t0 = time.monotonic()
        seat = seat_of(game)
        res = {"id": game["id"], "map": game["map"], "sides": game["sides"], "start": game["start"],
               "seats": {s: dict(game["seats"][s], seat=seat[s]) for s in ("A", "B")},
               "difficulty": game.get("difficulty", "hard"), "cap": game.get("cap", CAP)}
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
        while True:
            T = min((T // STEP + 1) * STEP, cap)
            self.hold(T)
            alive = {s: self.peek(REC + seat[s] * STRIDE + ALIVE, 2) for s in ("A", "B")}
            if not alive["A"] or not alive["B"]:
                # both gone in one step: no side won, judged as a game run to the cap (a failure for ImpAI)
                outcome = "cap" if not alive["A"] and not alive["B"] else "B" if not alive["A"] else "A"
                res["draw"] = outcome == "cap"
                end = T
                break
            if T >= cap:
                self.hold(cap + 15)         # both seats' pulses of the cap's minute are written by then
                break
        self.log.read()
        self.w.stop(hard=True)
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


def play_game(game: dict, lane: int, out_dir: Path) -> dict:
    """One game on lane `lane` (one Worker a lane, made at its first game and kept), its result in tourney.py's
    format: outcome A, B, cap or could_not_run, the end tick, each seat's table hash from its `impai: table` line,
    each ImpAI seat's value built and alive from its last pulse. Called from one thread a lane; the caller holds
    the run lock."""
    import tareplay as R
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    try:
        dll = _dll()
        with _LANES_LOCK:
            if lane not in _LANES:
                _LANES[lane] = Lane(lane, out_dir, dll)
            ln = _LANES[lane]
        ln.out = out_dir
        try:
            return ln.play(game)
        except (R.CouldNotRun, R.Failed, BarError):
            ln.abandon()
            raise
    except (R.CouldNotRun, R.Failed, BarError) as e:
        return {"id": game["id"], "outcome": "could_not_run", "reason": str(e)}


def close_lanes():
    with _LANES_LOCK:
        for ln in _LANES.values():
            ln.close()
        _LANES.clear()


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
        if pair == "dry":
            dead = [r["id"] for r in rs if r.get("commander_death") == 0]
            if dead:
                print(f"  commander-death rule off in: {' '.join(dead)}")
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
    from concurrent.futures import ThreadPoolExecutor
    games = [dict(parse_id(g), cap=a.cap) for g in read_ids(a.ids)]
    out = a.out or TREE / ".data" / "impai" / "bar" / time.strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    if a.dll:
        _DLL["path"] = Path(a.dll).resolve()
    runlock.hold(TREE, "bar play " + " ".join(sys.argv[2:]), lanes=a.lanes)

    def one(job):
        lane, g = job
        r = play_game(g, lane, out)
        (out / f"{g['id']}.json").write_text(json.dumps(r, indent=1) + "\n")
        return r

    try:
        with ThreadPoolExecutor(max_workers=a.lanes) as pool:
            results = list(pool.map(one, [(i % a.lanes, g) for i, g in enumerate(games)]))
    finally:
        close_lanes()
    bad = [r["id"] for r in results if r.get("outcome") == "could_not_run"]
    print(f"bar play: {len(results)} games in {out}, {len(bad)} could not run")
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
