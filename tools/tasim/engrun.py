"""engrun.py -- the calibration runner's engine phase: lanes that play scenario after scenario in one level.

    calib.py engine --scenarios 'scenarios/calib/duel-*.json' --runs 50 --j 6

The protocol and why it is unbiased: research/notes/modules/tasim-calibration.md. In short:

A LANE is one tareplay Worker (its instance tareplay-<k>, a private Xvfb, a private registry) at
turbo k = 1, held at launch and recording the exact stream. A LANE-LAUNCH loads the arena
(tools/tasim/calib-arena.json) and then plays runs, each from an EMPTY POINT -- no unit, no
projectile, the 30-tick phase realigned -- that the previous run's clear and a settle make:

  1. settle: hold at the least T >= T_prev + S_MIN with T = 1 (mod 30), raised by S_STEP while the
     projectile count main+0x141F3 is not 0, never past T_prev + S_MAX;
  2. apply the run's wire (perturb.py) on the held frame, and check the result: GameTime T, ok,
     nothing cleared (the level was empty), as many delayed orders as the wire asks;
  3. arm `pause at <T + L> or units <seat> <= <n> ...`;
  4. resume and wait for the pause: T_e and the reason;
  5. clear on the held frame, keeping cleared_slots: the survivors, each one's death image its state
     at the end of step T_e;
  6. the delayed orders' report line, when the wire had any; a skipped tick could not run;
  7. the run's manifest line.
A fresh launch runs the same protocol from the launch's hold. A run ends on the engine's own unit
count or on the tick limit, never on wall time: the waits are bounded by ACK_S and STALL_S, which
decide "could not run", never an outcome. The lanes are threads under one run lock; a lane keeps its
launch while the setup key stays and relaunches past its run budget.

PUPPET-FREE: a lane's levels are skirmishes the scenario loader starts, never a playing, so no
create passes a recorded slot (engine/network.md, *The allocator*) and the delayed orders' watch
holds; each launch asserts it from the log.
"""

import hashlib
import json
import queue
import re
import shutil
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TREE / "tools" / "replay"))
import calibref  # noqa: E402
import perturb  # noqa: E402
import tareplay as R  # noqa: E402
from tareplay import CouldNotRun, Failed  # noqa: E402

# THE EMPTY POINT'S SIGHT. A death of a unit of the viewed seat under true LOS leaves a temporary
# sight record (the destructor's 0x482910 at 0x486748) that stamps that seat's sight counters with
# its expiry at the death's GameTime + 60 (0x4829C4); the expiry 0x482130, once a step after its
# tick loop (0x495688), removes it only once GameTime is past that (an unsigned `jae` keeps it,
# 0x48215C..0x48215E). So a record made at GameTime d is gone from the first held frame at
# d + SIGHT_TTL + 1 on (DISASSEMBLED, engine/map.md *The temporary sight records*). Every death a
# run causes is at or before its clear's GameTime T_prev, and the launch's own apply kills the
# skirmish's commanders at its hold, so an empty point at T_prev + S_MIN or later holds no record
# and its counters are zero.
SIGHT_TTL = 60
S_MIN, S_STEP, S_MAX = SIGHT_TTL + 1, 30, 1800
PHASE = 30                      # the economy passes' period: an apply at 1 (mod 30) is at the
                                # phase of the launch's first apply, GameTime 1
PROJECTILES = "*0x511DE8+0x141F3:4"
# `meteoron`, the shower's switch: the .ota reader sets it through 0x437D40 or clears it through
# 0x437D50, and while it is 0 the shower tick 0x437DE0 clears `meteoractive` 0x512318 at every
# shower it schedules (0x437EF5..0x437EFE), so no stone falls (engine/weapons.md)
METEOR_ON = "0x51232C:4"
# The defaults until the speed measurement sets them (the module note's *Speed*): lanes run as
# threads under one lock, and a lane relaunches after this many runs to bound its recording.
J_DEFAULT = 6
J_MAX = 16
RUNS_PER_LAUNCH = 200
CHUNK = 10                      # runs a job, so lanes share a scenario's runs
PAUSE_RX = re.compile(r"pause: (paused at GameTime (\d+)(?: \(units of seat (\d) <= (\d+)\))?\s*$"
                      r"|at (\d+) not taken.*)")
DELAY_RX = re.compile(r"scenario: delayed orders: issued (\d+) of (\d+), cancelled (\d+) "
                      r"\(deaths (\d+), cleared (\d+), skipped (\d+)\)")
# a level that plays a recording says so at its first step (tagpu_replay.c)
PLAYING_RX = re.compile(r"replay: (\S+: )?playing ")
CLEAR = TREE / "scenarios" / "calib" / "clear.json"
END = TREE / "scenarios" / "calib" / "end.json"


class CalibLog(R.Log):
    NAP_S = 0.02


def aligned(t: int) -> int:
    """The least GameTime at or after t that is 1 (mod 30)."""
    return t + ((1 - t) % PHASE)


def setup_key(block: dict, arena_cfg: dict, fresh: bool):
    """What a lane's launch is: two runs share a launch only when every part is equal."""
    return (arena_cfg["map"], block["los"], block["mapping"], arena_cfg["unit_limit"],
            R.TEST_GAME_SPEED, "fresh" if fresh else "reload")


def plain_wire(path) -> str:
    try:
        return perturb.CLI._scn_wire(perturb.CLI._scn_compile(str(path)))
    except SystemExit as e:
        raise CouldNotRun(f"{Path(path).name}: {e}")


class Lane:
    """One Worker and the lane-launches it plays, one at a time."""

    def __init__(self, k: int, root: Path, dll: Path, arena_cfg: dict):
        self.k, self.root, self.dll, self.arena = k, root, dll, arena_cfg
        self.w = R.Worker(root)
        self.w.turbo = 1
        self.w.held = True
        self.w.exact = True
        self.launches = 0
        self.key = None
        self.runs = 0
        self.log = None
        self.lines = []
        self.values = None

    # ---- verbs -----------------------------------------------------------------------------

    def peek(self, spec: str) -> int:
        """One peek, read from this lane's own log: the frame visit's answer, ACK_S at most."""
        log = self.log
        log.read()
        n = len(log.lines)
        R._CLI_API.put_trigger(self.w.gamedir / "tagpu_peek.trigger", spec + "\n")
        end = time.monotonic() + R.ACK_S
        while True:
            log.read()
            rows = R._CLI_API._peek_rows("\n".join(log.lines[n:]), [spec])
            if rows:
                v = R._CLI_API._peek_uint(rows[0])
                if v is None:
                    raise CouldNotRun(f"peek {spec}: {rows[0]['value']}")
                return v
            if time.monotonic() >= end:
                raise CouldNotRun(f"peek {spec}: no answer in {R.ACK_S} s")
            R.nap(log.NAP_S)
            log.alive()

    def apply(self, wire: str, what: str) -> dict:
        """The wire applied in-process through tacli's own call; its refusal could not run."""
        try:
            res = R._CLI_API._scn_apply(R._CLI_API.Instance(self.w.name), wire, R.ACK_S)
        except SystemExit as e:
            raise CouldNotRun(f"{what}: {e}")
        if res.get("ok") != 1:
            raise CouldNotRun(f"{what}: the applier said {res.get('errors')}")
        return res

    def hold(self, T: int):
        R.hold_at(self.w, self.log, T)
        R.turbo_engage(self.w, self.log)
        R.resume(self.w, self.log)
        R.wait_paused(self.w, self.log, T)

    def dump_now(self, name: str) -> int:
        R.arm(self.w, self.log, f"dump {name} now", R._CLI_API.dump_written_rx(name),
              R._CLI_API.dump_refused_rx(name), f"dump {name}")
        return self.log.at

    # ---- a lane-launch ---------------------------------------------------------------------

    def launch(self, key, block: dict):
        self.launches += 1
        self.key, self.runs = key, 0
        self.dir = self.root / f"lane{self.k}-{self.launches}"
        self.dir.mkdir(parents=True, exist_ok=True)
        self.lines = []
        self.started = time.monotonic()
        self.ticks = 0
        self.e0 = False
        w = self.w
        w.place({"name": "calib"}, self.dll)
        self.log = CalibLog(w)
        rc, o = R.tacli("scenario", "load", w.name, TREE / self.arena["scenario"], "--defaults",
                        "--los", block["los"], "--mapping", block["mapping"], "--restart",
                        "--keep-dll", "--unit-limit", self.arena["unit_limit"],
                        timeout=R.LAUNCH_TIMEOUT)
        if rc:
            raise CouldNotRun(f"scenario load {self.arena['scenario']}: {R.last(o)}")
        got = self.log.wait(r"recorder: (recording|not recording|FAILED)", R.ACK_S)
        if not got or "recorder: recording" not in got:
            raise CouldNotRun(f"the recorder: {got.strip() if got else 'no line'}")
        self.T_prev = R.held_start(w, self.log)
        if any(PLAYING_RX.search(ln) for ln in self.log.lines):
            raise CouldNotRun("the lane's level plays a recording: a lane is puppet-free")
        if self.peek(METEOR_ON):
            raise CouldNotRun(f"{self.arena['map']}: the level's meteor shower is switched on "
                              f"(0x51232C): the arena must leave it off")
        rc, o = R.tacli("units", w.name, "--json", timeout=R.ACK_S + 30)
        if rc:
            raise CouldNotRun(f"tacli units: {R.last(o)}")
        cat = json.loads(o[o.index("{"):])
        self.values = {u["name"].upper(): {"metal": u["cost"][0], "energy": u["cost"][1],
                                           "maxdamage": u["maxdamage"],
                                           "buildtime": u["buildtime"],
                                           "workertime": u["workertime"], "bmcode": u["bmcode"]}
                       for u in cat["units"]}
        (self.dir / "units.json").write_text(json.dumps(cat, indent=1))
        self.head = {"lane": self.k, "launch": self.launches, "recording": None,
                     "setup_key": list(key),
                     "dll": {"build": calibref.git_build(), "sha256": calibref.sha256_file(self.dll)},
                     "catalogue_sha256": hashlib.sha256(
                         json.dumps(self.values, sort_keys=True).encode()).hexdigest()}
        self.partial = (self.dir / "manifest.partial.jsonl").open("w")
        self.write(self.head)

    def write(self, row: dict):
        self.lines.append(row)
        self.partial.write(json.dumps(row) + "\n")
        self.partial.flush()

    def settle(self):
        """The empty point: held at T = 1 (mod 30), S_MIN on from the last run's end -- past every
        temporary sight record its deaths left (THE EMPTY POINT'S SIGHT) -- with no projectile
        left in flight. Returns (T, the settle's extra steps)."""
        T, waits = aligned(self.T_prev + S_MIN), 0
        while True:
            if T > self.T_prev + S_MAX:
                raise CouldNotRun(f"projectiles still in flight {S_MAX} ticks after the clear at "
                                  f"{self.T_prev}")
            self.hold(T)
            if self.peek(PROJECTILES) == 0:
                return T, waits
            T += S_STEP
            waits += 1

    def run(self, path: Path, index: int, block: dict, mode: str) -> dict:
        wall = {}
        t0 = time.monotonic()
        T, waits = self.settle()
        wall["settle"] = time.monotonic() - t0
        if not self.e0:
            self.write({"dump": "e0", "t": self.dump_now("e0")})
            self.e0 = True
        exp, record, wire, clauses = perturb.run_wire(path, index, self.values, self.arena)
        delayed = sum(1 for u in exp["units"] for o in u["orders"] if o["after"])
        t1 = time.monotonic()
        n_apply = len(self.log.lines)
        res = self.apply(wire, f"{path.name} run {index}")
        wall["apply"] = time.monotonic() - t1
        if res.get("gametime") != T or res.get("cleared") != 0 or res.get("delayed") != delayed:
            raise CouldNotRun(f"{path.name} run {index}: applied at GameTime {res.get('gametime')} "
                              f"(held at {T}), cleared {res.get('cleared')}, delayed "
                              f"{res.get('delayed')} of {delayed}")
        units = {}
        for ordinal, row in (res.get("units") or {}).items():
            handle = exp["units"][int(ordinal)]["id"]
            units[handle] = [row["engine_index"], *row["actual"]]
        dump = None
        if "sight" in block["metrics"]:
            dump = f"s{self.runs}"      # unique within the launch; letters and digits only
            R.arm_dump(self.w, self.log, dump, T + 2)
        Tend = T + block["limit"]
        line = f"pause at {Tend}" + "".join(f" or units {s} <= {n}" for s, n in clauses)
        n_arm = len(self.log.lines)
        R.arm(self.w, self.log, line, rf"pause: armed for GameTime {Tend}\b",
              rf"pause: at {Tend} refused: .*", f"{path.name} run {index}'s end")
        t2 = time.monotonic()
        R.resume(self.w, self.log)
        got = self.log.wait_running(PAUSE_RX, n_arm)
        m = PAUSE_RX.search(got)
        if not m.group(2):
            raise CouldNotRun(f"{path.name} run {index}: {got.strip()}")
        Te = int(m.group(2))
        end = f"units {m.group(3)} <= {m.group(4)}" if m.group(3) else "limit"
        self.log.held, self.log.pending, self.log.at = True, None, Te
        wall["run"] = time.monotonic() - t2
        t3 = time.monotonic()
        cleared = self.apply(plain_wire(CLEAR), f"{path.name} run {index}'s clear")
        wall["clear"] = time.monotonic() - t3
        if cleared.get("gametime") != Te:
            raise CouldNotRun(f"{path.name} run {index}: cleared at GameTime "
                              f"{cleared.get('gametime')}, paused at {Te}")
        report = None
        if delayed:
            ln = self.log.wait(DELAY_RX, R.ACK_S, n_apply)
            if not ln:
                raise CouldNotRun(f"{path.name} run {index}: no delayed-order line in {R.ACK_S} s")
            got_d = [int(g) for g in DELAY_RX.search(ln).groups()]
            report = dict(zip(("issued", "of", "cancelled", "deaths", "cleared", "skipped"), got_d))
            if report["skipped"] or report["of"] != delayed or \
                    report["issued"] + report["cancelled"] != report["of"]:
                raise CouldNotRun(f"{path.name} run {index}: {ln.strip()}")
        if dump:
            R.wait_dumps(self.log, [dump])
        wall["total"] = time.monotonic() - t0
        row = {"scenario": str(path.relative_to(TREE) if path.is_relative_to(TREE) else path),
               "scenario_sha256": calibref.sha256_file(path), "index": index,
               "seed": record["seed"], "swap": record["swap"], "mode": mode,
               "perturbation": record,
               "wire_sha256": hashlib.sha256(wire.encode()).hexdigest(),
               "t_apply": T, "apply": {"units": units, "delayed": res.get("delayed"),
                                       "features": [f["def"] for f in
                                                    (res.get("features") or {}).values()]},
               "arm": {"limit": block["limit"], "units": [list(c) for c in clauses]},
               "t_end": Te, "end": end,
               "clear": {"t": Te, "cleared_slots": cleared.get("cleared_slots") or []},
               "delayed": report, "settle": {"t_next": T, "waits": waits}, "dump": dump,
               "wall": {k: round(v, 3) for k, v in wall.items()}}
        self.write(row)
        self.T_prev = Te
        self.runs += 1
        self.ticks += Te - T
        return row

    def finish(self):
        """The last empty point's dump, then the lane's end: the objective armed again with seat
        0 left no unit, so the level ends and its recording closes complete."""
        T = aligned(self.T_prev + S_MAX)
        self.hold(T)
        if self.peek(PROJECTILES):
            raise CouldNotRun(f"projectiles in flight at the lane's last empty point, GameTime {T}")
        self.write({"dump": "eN", "t": self.dump_now("eN")})
        n = len(self.log.lines)
        self.apply(plain_wire(END), "the lane's end")
        R.resume(self.w, self.log)
        for rx in (r"recorder: ended after", r"recorder: writer: .* closed"):
            self.log.wait_running(rx, n)
        self.w.collect(self.dir, recordings=True)
        self.w.stop()
        recs = sorted(self.dir.glob("*.imprec"), key=lambda p: p.stat().st_mtime)
        if not recs:
            raise CouldNotRun("no recording in the lane's replays folder")
        self.head["recording"] = recs[-1].name
        self.head["lane_seconds"] = round(time.monotonic() - self.started, 3)
        self.head["ticks"] = self.ticks
        self.head["recording_bytes"] = recs[-1].stat().st_size
        self.partial.close()
        rows = [self.head] + self.lines[1:]
        (self.dir / "manifest.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows))
        (self.dir / "manifest.partial.jsonl").unlink()
        self.key = None

    def abandon(self):
        """A lane-launch that could not run: the game stopped, its partial manifest kept."""
        try:
            self.partial.close()
        except AttributeError:
            pass
        try:
            self.w.stop(hard=True)
        except CouldNotRun as e:
            R.say(f"lane {self.k}: {e}")
        self.key = None

    def close(self):
        self.w.close()


def jobs_of(scenarios, runs, mode, arena_cfg, only=None):
    """[(setup key, path, block, indices, mode)], chunked by CHUNK, sorted by setup key so a lane
    keeps its launch. An exact scenario's runs are 0 and 1 by reload and a third fresh launch."""
    out = []
    for path in scenarios:
        doc = json.loads(path.read_text())
        block = calibref.calib_block(doc, path.name, arena_cfg)
        n = runs or block["runs"]
        fresh = mode == "fresh"
        indices = list(only) if only is not None else list(range(n))
        if "exact" in block["kind"] and mode != "fresh" and only is None:
            out.append((setup_key(block, arena_cfg, False), path, block, [0, 1], "reload"))
            out.append((setup_key(block, arena_cfg, True), path, block, [2], "fresh"))
            continue
        step = 1 if fresh else CHUNK
        for i in range(0, len(indices), step):
            key = setup_key(block, arena_cfg, fresh)
            out.append((key, path, block, indices[i:i + step], "fresh" if fresh else "reload"))
    return sorted(out, key=lambda j: (j[0], str(j[1]), j[3][0]))


def lane_count(jobs, j):
    """The lanes run_engine plays `jobs` on, one game each at once: what calib.py declares to the
    run lock."""
    return min(j, J_MAX, max(1, len(jobs)))


def run_engine(scenarios, runs, j, mode, out: Path, dll: Path, budget=RUNS_PER_LAUNCH,
               only=None):
    """Every job on j lanes. Returns (manifest dirs, the could-not-run messages)."""
    arena_cfg = calibref.arena()
    jobs = jobs_of(scenarios, runs, mode, arena_cfg, only)
    work = queue.Queue()
    for jb in jobs:
        work.put(jb)
    errors, lock = [], threading.Lock()
    lanes = []

    def lane_main(k):
        lane = None
        try:
            lane = Lane(k, out, dll, arena_cfg)
            with lock:
                lanes.append(lane)
            while not R.STOP.is_set():
                try:
                    key, path, block, indices, how = work.get_nowait()
                except queue.Empty:
                    break
                try:
                    for index in indices:
                        if lane.key != key or lane.runs >= budget:
                            if lane.key is not None:
                                lane.finish()
                            lane.launch(key, block)
                        lane.run(path, index, block, how)
                        if how == "fresh":
                            lane.finish()
                except (CouldNotRun, Failed) as e:
                    with lock:
                        errors.append(f"{path.name} runs {indices}: {e}")
                    R.say(f"lane {k}: could not run {path.name} {indices}: {e}")
                    lane.abandon()
            if lane.key is not None:
                lane.finish()
        except (CouldNotRun, Failed) as e:
            with lock:
                errors.append(f"lane {k}: {e}")
            if lane:
                lane.abandon()
        finally:
            if lane:
                lane.close()

    threads = [threading.Thread(target=lane_main, args=(k,), daemon=True)
               for k in range(lane_count(jobs, j))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    dirs = sorted(p.parent for p in out.glob("lane*/manifest.jsonl"))
    return dirs, errors


def stamp() -> str:
    return time.strftime("%Y%m%d-%H%M%S")


def new_out(root: Path = None) -> Path:
    out = (root or calibref.runs_dir()) / stamp()
    out.mkdir(parents=True, exist_ok=False)
    return out


def remove_recordings(dirs):
    for d in dirs:
        for f in d.glob("*.imprec"):
            f.unlink()
        shutil.rmtree(d / "replays", ignore_errors=True)
