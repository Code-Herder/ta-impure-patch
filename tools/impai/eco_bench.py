#!/usr/bin/env python3
"""eco_bench -- ImpAI's economy against OTA AI's, in the engine: A1b's exit test.

    tools/impai/eco_bench.py engine --map "<name>" --starts 1,2,3 [--j 6] [--out DIR] [--dll PATH] [--gpu]
    tools/impai/eco_bench.py judge DIR [DIR ...]

A GAME is scenarios/impai/eco-bench.json with the map and the seat under test filled in: an idle
human at StartPos1 and one CORE computer seat k at StartPos(k + 1) (fixed starts put seat i at
StartPos(i + 1): the loader at 0x49790E, scout I11), mapped, true line of sight, the level's end
test off, the Difficulty Hard (OTA AI unscaled; ImpAI is exempt at every Difficulty), commanders
only. Seat k is `ai` (OTA AI) or `impai`. `engine` plays both for each start k of --starts. The
two seats are in no alliance group: the SKIRMISH Start refuses a game whose enabled seats share one
(0x479760), so the human is seat k's enemy, and the end test being off keeps the level running
whatever becomes of it. tacli refuses such a seating before it clicks Start, naming the reason.

A LANE is one tareplay Worker (its instance tareplay-<j>, a private Xvfb, a private registry) at
turbo k = 1 with the launch held, as the calibration runner's lanes (tools/tasim/engrun.py); the
lanes are threads under the one run lock (tools/runlock.py). A lane loads a game, holds it at
GameTime FROM, reads seat k's TotalMetalProduced +0xB4 and TotalEnergyProduced +0xAC (doubles, the
economy pass adds each pass's income to them: scout H1), holds at UNTIL, reads them again, and
ends the level: ImpAI logs its `impai: eco` line at the level's end (the teardown 0x491B60), which
stopping the process never reaches. The end is an apply of end_doc -- every unit cleared, the end
test armed again, one CORFUS kept for seat k -- after which the human seat's defeat ends the level,
and the line every level end writes (`impai: handicap rows`) says it has. A hold the engine does not
reach within the harness's bounds is "could not run", never an outcome.

THE INCOME over the last 900 ticks of the first ten minutes, a pass every 30 ticks:

    I = ((M2 - M1) + (E2 - E1) / 60) / 30          60 = TASIM_ROLES_R, energy's metal value

`engine` writes one JSON a game into --out (default .data/impai/eco-bench/<stamp>) with the
metal and energy deltas, I, the game's wall time and the level's `impai: eco` line when seat k is
ImpAI's. `judge` reads every game of the directories given and prints, for each (map, start), both
incomes: exit 0 when ImpAI's is above OTA AI's at every pair, 1 when it is not at one or a pair
lacks a game, 2 when nothing could be read.

THE RENDERER. The games render with GDI (tareplay.GDI_LEVER) and take no GPU token; `--gpu` renders them on
the default renderer for a debugging run, a GPU token a lane. Each game's JSON names its renderer, and `judge`
refuses directories that mix two (research/notes/modules/impai.md, *The renderer*).
"""
import argparse
import json
import queue
import re
import struct
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(TREE / "tools"))
sys.path.insert(0, str(TREE / "tools" / "replay"))
import runlock  # noqa: E402
import tareplay as R  # noqa: E402
from tareplay import CouldNotRun, Failed  # noqa: E402

TEMPLATE = TREE / "scenarios" / "impai" / "eco-bench.json"
FROM, UNTIL = 17100, 18000              # the last 900 ticks of the first ten minutes
PASSES = (UNTIL - FROM) // 30
E_PER_M = 60.0                          # TASIM_ROLES_R: the energy a metal is worth
REC, STRIDE = 0x1B63, 0x14B             # record 0 in main, a record (tagpu_engine.h)
TOTAL_E, TOTAL_M = 0xAC, 0xB4           # TotalEnergyProduced, TotalMetalProduced: doubles
AIS = ("ai", "impai")
J_MAX = 16
ECO = re.compile(r"impai: eco seat=\d+ .*")


def game_doc(mapname, k, ai):
    """The template with the map and seat k, a CORE computer seat of `ai`; the seats between them
    off."""
    doc = json.loads(TEMPLATE.read_text())
    s = doc["setup"]
    s["map"] = mapname
    human = s["players"][0]
    seats = [human] + [{"slot": i, "controller": "off"} for i in range(1, k)]
    seats.append({"slot": k, "controller": ai, "side": "core", "color": k})
    s["players"] = seats
    doc["description"] = f"eco_bench: {mapname}, seat {k} ({ai}) at StartPos{k + 1}"
    return doc


def end_doc(k):
    """The level's end after the reads: every unit cleared, the end test armed again (`objective on`,
    the takeover's write +0x88 = 1), and one unit kept for seat k, so the human seat, left with none,
    is defeated and the level ends with the seat under test standing."""
    return {"format": "ta-scenario/1",
            "description": "eco_bench's level end (tools/impai/eco_bench.py, end_doc).",
            "setup": {"clear_existing": True, "objective": "on"},
            "units": [{"id": "keep", "type": "CORFUS", "owner": k, "pos": [1000, 1000]}]}


def slug(mapname, k, ai):
    return f"{re.sub(r'[^a-z0-9]+', '-', mapname.lower()).strip('-')}-start{k}-{ai}"


class Lane:
    """One Worker and the games it plays, one at a time."""

    def __init__(self, j, out, dll, gpu=False):
        self.j, self.out, self.dll = j, out, dll
        self.w = R.Worker(out)
        self.w.turbo = 1
        self.w.held = True
        self.w.gdi = not gpu
        self.log = None

    def peek_double(self, off):
        spec = f"*0x511DE8+0x{off:X}:x8"
        self.log.read()
        n = len(self.log.lines)
        R._CLI_API.put_trigger(self.w.gamedir / "tagpu_peek.trigger", spec + "\n")
        end = time.monotonic() + R.ACK_S
        while True:
            self.log.read()
            rows = R._CLI_API._peek_rows("\n".join(self.log.lines[n:]), [spec])
            if rows:
                try:
                    return struct.unpack("<d", bytes.fromhex(rows[0]["value"]))[0]
                except (ValueError, struct.error):
                    raise CouldNotRun(f"peek {spec}: {rows[0]['value']}")
            if time.monotonic() >= end:
                raise CouldNotRun(f"peek {spec}: no answer in {R.ACK_S} s")
            R.nap(self.log.NAP_S)
            self.log.alive()

    def hold(self, T):
        R.hold_at(self.w, self.log, T)
        R.turbo_engage(self.w, self.log)
        R.resume(self.w, self.log)
        R.wait_paused(self.w, self.log, T)

    def totals(self, k):
        base = REC + k * STRIDE
        return self.peek_double(base + TOTAL_M), self.peek_double(base + TOTAL_E)

    def play(self, mapname, k, ai):
        name = slug(mapname, k, ai)
        t0 = time.monotonic()
        scn = self.out / f"{name}.scenario.json"
        scn.write_text(json.dumps(game_doc(mapname, k, ai), indent=1) + "\n")
        self.w.place({"name": "eco-bench"}, self.dll)
        self.log = R.Log(self.w)
        rc, o = R.tacli("scenario", "load", self.w.name, scn, "--defaults", "--restart", "--keep-dll",
                        timeout=R.LAUNCH_TIMEOUT)
        if rc:
            raise CouldNotRun(f"scenario load {scn.name}: {R.last(o)}")
        start = R.held_start(self.w, self.log)
        if start >= FROM:
            raise CouldNotRun(f"the launch's hold is at GameTime {start}, past {FROM}")
        want = f"impai: seats {k} (impure.cfg)" if ai == "impai" else "impai: seats none (impure.cfg)"
        if not any(want in ln for ln in self.log.lines):
            raise CouldNotRun(f"the level init did not say `{want}`")
        self.hold(FROM)
        m1, e1 = self.totals(k)
        self.hold(UNTIL)
        m2, e2 = self.totals(k)
        end = self.out / f"{name}.end.scenario.json"
        end.write_text(json.dumps(end_doc(k), indent=1) + "\n")
        n = len(self.log.lines)
        rc, o = R.tacli("scenario", "apply", self.w.name, end, timeout=300)
        if rc:
            raise CouldNotRun(f"scenario apply {end.name}: {R.last(o)}")
        R.resume(self.w, self.log)
        self.log.wait_running(r"impai: handicap rows", n)
        self.w.stop(hard=True)
        self.log.read()
        dm, de = m2 - m1, e2 - e1
        row = {"map": mapname, "start": k, "ai": ai, "from": FROM, "until": UNTIL,
               "metal": [m1, m2], "energy": [e1, e2], "d_metal": dm, "d_energy": de,
               "income": (dm + de / E_PER_M) / PASSES,
               "metal_per_pass": dm / PASSES, "energy_per_pass": de / PASSES,
               "eco": [ln.strip() for ln in self.log.lines if ECO.search(ln)],
               "renderer": "gdi" if self.w.gdi else "vulkan",
               "wall_s": round(time.monotonic() - t0, 1)}
        (self.out / f"{name}.json").write_text(json.dumps(row, indent=1) + "\n")
        R.say(f"lane {self.j}: {name}: income {row['income']:.3f} ({row['wall_s']} s)")
        return row

    def abandon(self):
        try:
            self.w.stop(hard=True)
        except CouldNotRun as e:
            R.say(f"lane {self.j}: {e}")

    def close(self):
        self.w.close()


def lane_count(starts, j):
    """The lanes run_engine plays on, one game each at once: what `engine` declares to the run lock."""
    return min(j, J_MAX, 2 * len(starts))


def run_engine(mapname, starts, j, out, dll, gpu=False):
    work = queue.Queue()
    for k in starts:
        for ai in AIS:
            work.put((k, ai))
    errors, lock = [], threading.Lock()

    def lane_main(n):
        lane = None
        try:
            lane = Lane(n, out, dll, gpu)
            while not R.STOP.is_set():
                try:
                    k, ai = work.get_nowait()
                except queue.Empty:
                    break
                try:
                    lane.play(mapname, k, ai)
                except (CouldNotRun, Failed) as e:
                    with lock:
                        errors.append(f"{slug(mapname, k, ai)}: {e}")
                    lane.abandon()
        except (CouldNotRun, Failed) as e:
            with lock:
                errors.append(f"lane {n}: {e}")
        finally:
            if lane:
                lane.close()

    threads = [threading.Thread(target=lane_main, args=(n,), daemon=True)
               for n in range(lane_count(starts, j))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return errors


def judge(dirs):
    rows = []
    for d in dirs:
        for p in sorted(Path(d).glob("*.json")):
            if p.name.endswith(".scenario.json"):
                continue
            rows.append(json.loads(p.read_text()))
    if not rows:
        print("eco_bench judge: no game in " + ", ".join(map(str, dirs)))
        return 2
    # a game written before the renderer was recorded was played on the default renderer
    renderers = sorted({r.get("renderer", "vulkan") for r in rows})
    if len(renderers) > 1:
        print(f"eco_bench judge: the games were rendered by {' and '.join(renderers)}: a judgement compares "
              f"games of one renderer")
        return 2
    pairs = {}
    for r in rows:
        pairs.setdefault((r["map"], r["start"]), {})[r["ai"]] = r
    fails = 0
    print(f"{'map':<28} {'start':>5} {'OTA AI':>10} {'ImpAI':>10}   metal/energy a pass (OTA | ImpAI)")
    for (mapname, k), g in sorted(pairs.items()):
        ota, imp = g.get("ai"), g.get("impai")
        if not ota or not imp:
            print(f"{mapname:<28} {k:>5}  missing {'OTA AI' if not ota else 'ImpAI'}")
            fails += 1
            continue
        ok = imp["income"] > ota["income"]
        fails += not ok
        print(f"{mapname:<28} {k:>5} {ota['income']:>10.3f} {imp['income']:>10.3f}   "
              f"{ota['metal_per_pass']:.2f}/{ota['energy_per_pass']:.1f} | "
              f"{imp['metal_per_pass']:.2f}/{imp['energy_per_pass']:.1f}  {'ok' if ok else 'FAIL'}")
    walls = [r["wall_s"] for r in rows]
    print(f"games {len(rows)}, wall {min(walls)}..{max(walls)} s a game")
    print(f"eco_bench judge: {'PASS' if not fails else 'FAIL'} ({len(pairs) - fails} of {len(pairs)} pairs "
          f"pass, {fails} fail)")
    return 1 if fails else 0


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="verb", required=True)
    e = sub.add_parser("engine", help="play OTA AI and ImpAI at each start of one map")
    e.add_argument("--map", required=True)
    e.add_argument("--starts", default="1,2,3", help="seats k, each at StartPos(k + 1)")
    e.add_argument("--j", type=int, default=6, help="lanes")
    e.add_argument("--out", type=Path, default=None)
    e.add_argument("--dll", default=None, help="the DLL to play (default this tree's build)")
    e.add_argument("--gpu", action="store_true",
                   help="render on the default renderer, for debugging, taking a GPU token a lane; without it "
                        "the games render with GDI and take none")
    jd = sub.add_parser("judge", help="ImpAI's income against OTA AI's, every (map, start)")
    jd.add_argument("dirs", nargs="+", type=Path)
    a = p.parse_args()
    if a.verb == "judge":
        return judge(a.dirs)
    try:
        starts = sorted({int(x) for x in a.starts.split(",") if x.strip()})
    except ValueError:
        starts = []
    if not starts or not all(1 <= k <= 9 for k in starts):
        print(f"eco_bench: --starts {a.starts!r}: seats 1-9, comma-separated")
        return 2
    try:
        dll, make = R.build_dll(False, False)
        dll = Path(a.dll).resolve() if a.dll else dll.resolve()
        if not dll.exists():
            raise CouldNotRun(f"no DLL at {dll} ({make})")
        out = a.out or TREE / ".data" / "impai" / "eco-bench" / time.strftime("%Y%m%d-%H%M%S")
        out.mkdir(parents=True, exist_ok=True)
        runlock.hold(TREE, f"eco_bench engine {a.map} {a.starts}", lane_count(starts, a.j), gpu=a.gpu)
        t0 = time.monotonic()
        errors = run_engine(a.map, starts, min(a.j, runlock.games()), out, dll, a.gpu)
    except CouldNotRun as e:
        print(f"eco_bench: COULD NOT RUN: {e}")
        return 2
    for err in errors:
        print(f"eco_bench: could not run {err}")
    print(f"eco_bench engine: {out} in {time.monotonic() - t0:.0f} s")
    return 2 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
