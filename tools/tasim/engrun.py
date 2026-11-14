"""engrun.py -- the calibration runner's engine phase: lanes that play scenario after scenario in one level.

    calib.py engine --scenarios 'scenarios/calib/duel-*.json' --runs 50 --j 6

The protocol and why it is unbiased: research/notes/modules/tasim-calibration.md. In short:

A LANE is one tareplay Worker (its instance tareplay-<k>, a private Xvfb, a private registry) at
turbo k = 1, held at launch and recording the exact stream. A LANE-LAUNCH loads the arena
(tools/tasim/calib-arena.json) and then plays runs, each from an EMPTY POINT -- no unit, no
projectile, the 30-tick phase realigned -- that the previous run's clear and a settle make:

  1. settle: the least T >= T_prev + S_MIN with T = 1 (mod 30) whose hold at T - 1 reads the
     projectile count main+0x141F3 as 0, raised by S_STEP while it does not, never past
     T_prev + S_MAX; then one step to the hold at T (THE EMPTY POINT'S POOL);
  2. apply the run's wire (perturb.py) on the held frame, and check the result: GameTime T, ok,
     nothing cleared (the level was empty), as many delayed orders as the wire asks;
  3. arm `pause at <T + L> or units <seat> <= <n> ...`;
  4. resume and wait for the pause: T_e and the reason;
  5. clear on the held frame, keeping cleared_slots: the survivors, each one's death image its state
     at the end of step T_e;
  6. the delayed orders' report line, when the wire had any; a skipped tick could not run;
  7. the run's manifest line.
A SCENARIO THAT REFUSES ITSELF -- the applier refused its wire (step 2), or a delayed order its unit could
not take (step 6) -- costs only itself: the world is cleared on the held frame, a `refused` row (no `index`)
names it, its remaining runs are not played and the lane goes on to the next scenario (Refused). Its runs
are then missing from its reference, which `refs build` refuses for that scenario alone. Anything else
that could not run abandons the lane-launch.
A fresh launch runs the same protocol from the launch's hold. A run ends on the engine's own unit
count or on the tick limit, never on wall time: the waits are bounded by ACK_S and STALL_S, which
decide "could not run", never an outcome. The lanes are threads under one run lock; a lane keeps its
launch while the setup key stays and relaunches past its run budget.

PUPPET-FREE: a lane's levels are skirmishes the scenario loader starts, never a playing, so no
create passes a recorded slot (engine/network.md, *The allocator*) and the delayed orders' watch
holds; each launch asserts it from the log. The one create that passes a slot is the applier's own,
for a unit whose wire line says `slot=`, admitted only on a slot no delayed entry watches
(tagpu_scenario.c, sched_slot_ok); a run whose wire has one requires the applier's report
(SCHED_RX) to count every such create made, none refused or skipped.

ON A COMPAT SETUP (`calib.py engine --setup NAME`) each lane is a SetupWorker in the setup's own folder
(tools/compat/setups.json), the arena calibref.arena()'s with the setup's overlay, and the calibration root the
setup's alone (calibref.calib_dir, THE ROOT INVARIANT). Every lane-launch, retail's too, reads back the cap in
force, the speed ceiling and the gravity (WHAT EVERY LANE-LAUNCH READS BACK) and records them in its head beside
the setup's name.

THE RENDERER. A lane renders with GDI (tareplay.GDI_LEVER) unless `calib.py engine --gpu` asks for
the default renderer, and a lane-launch's manifest head names it (`renderer`; a head without one was
played on the default renderer, the only one before the field). Runs of two renderers are never
merged: a resume refuses them (done_runs), and a reference is built from one engine run's folder.

THE PACING. A GDI lane runs NO DRAW (tareplay.NODRAW_LEVER): while the turbo runs, a frame whose batch was the
turbo's skips the game screen's draw and runs only what the harness reads (tagpu_nodraw.c, THE GATE); held
frames, where every apply, settle and dump is taken, draw. `calib.py engine --draw` renders every frame, and
--gpu implies it. A lane-launch's head names its pacing (`pacing`: draw or nodraw; a head without one was
drawn, the only pacing before the field), and a resume refuses to mix two, as it refuses two renderers: they
are claimed equal (rendereq.py --pacing nodraw), and a resume does not rest on the claim.

THE PACKED LANE (`calib.py engine --pack K`, on a generated arena: tools/tasim/pack.py). A lane-launch plays
BATCHES instead of runs: one settle, one apply, one pause and one clear each, as one run has, holding up to K
cells -- each one job (scenario, index) with perturb.run_wire's perturbation, swap and seed, translated into a
site of its own and created `at` ticks after the apply at block positions P_k of each seat's block (pack.py,
THE STAGGER). Lane.run_batch:
  1. settle, as a run;
  2. apply pack.wire(batch) on the held frame at T and check the result: GameTime T, nothing cleared, cell 0's
     units made at the apply in the slots asked for, its delayed orders, and every later cell's units and orders
     in the scheduled table;
  3. arm pack.pause_clause(batch, T, L) -- `pause at <T + the last cell's at + L> or cells ...` -- and resume;
     the clause is for speed only (tagpu_replay.c, pause_cells_visit), each cell's end is read offline;
  4. wait for the pause, and the cells' `decided` lines;
  5. clear on the held frame;
  6. the scheduled creates' report (every create made, none refused or skipped) and the delayed orders';
  7. one manifest row for the batch, its cells' jobs and their placement in it, which engtrace.py reads back
     into one run a cell (engtrace.py, THE PACKED LANE).
A batch is atomic: a refusal refuses every scenario it holds, and a resume plays every batch of the run's plan
(PLAN in the run's folder, written once) that no finished lane-launch has a row for.
THE PACKED LANE'S READS, at every launch beside WHAT EVERY LANE-LAUNCH READS BACK (pack_reads): each seat's
block, its first and last record (player +0x67/+0x6B) on the array main+0x14357's stride 0x118, must hold
exactly the unit limit, which the positions P_k and s = limit/30 + 1 are computed from; Mapping on
(main+0x14281 bit 0 clear), which the pathfinder's read of the explored map needs (pack.py, THE EXPLORED
MAP); the gravity synthmap.ENGINE_GRAVITY that the projectile bound assumes; the line shot's append cap,
the u32 at 0x49C9D2, pack.PROJ_CAP; and the temporary sight records' cap, the compare's imm8 at 0x48292E,
the lever's N (tareplay.SIGHTCAP_LEVER, written for the launch). Each failure is "could not run". The
per-player count S (main+0x1434F) and each block's first slot go into the head for the checks.
THE PACKED LANE'S PACING draws every frame: no draw is not yet cleared for a suite (a drawn and an undrawn
run part on two kept fields, tasim-calibration.md *The lane and its runs*), so a batch lane runs it only
when asked (`calib.py engine --nodraw`), and its head names the pacing as every head does.

RESUMING (`calib.py engine --resume DIR`). A run is a function of its scenario and its index, not of
the lane-launch it lands in: its wire, perturbation and seed are perturb.run_wire(path, index, ...)
alone, and it starts from an empty point its own apply resets (Lane.run: the settle, then the apply
of its own wire on the held frame, checked to clear nothing; what the apply does not reset -- the
simulation RNG and the wind among it -- follows the runs its lane-launch played before it, from the
launch's fixed seeds -- tasim-calibration.md, *What a reload does not reset*). An uninterrupted run
already hands its chunks to whichever lane is free, so which runs share a launch is not part of a
result's distribution, though it is part of each run's draw; a resume is one more such hand-out. done_runs takes the runs of every FINISHED
lane-launch in DIR (manifest.jsonl) whose DLL, scenario bytes, arena, setup key, renderer and pacing
are this run's, and refuses the resume, naming each, when one is not. A lane-launch that did not finish
(manifest.partial.jsonl only) has no closed recording, which `refs build` streams its runs' traces
from (engtrace.Lane), so its runs are played again; `refs build` reads finished lane-launches only,
the same ones a resume counts.
"""

import hashlib
import json
import math
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
import pack  # noqa: E402
import perturb  # noqa: E402
import synthmap  # noqa: E402
import tarec  # noqa: E402
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
# THE EMPTY POINT'S POOL. The empty point is iteration T's images, and the recorder captures them at
# the head of step T, before that step's projectile tick: the step 0x495490 increments GameTime
# (0x4954B7..0x4954C0) and pumps the network (0x4954C8), then calls the unit tick at 0x4954ED, whose
# stub captures first, and only after it the projectile tick (0x495513) and the explosion tick
# (0x495539) (DISASSEMBLED). A hold at H is the frame visit after step H, and no step runs on a held
# frame: 0x496920 tests the pause bit and jumps past the step's call at 0x49693E (DISASSEMBLED). So a
# peek on the hold at T reads the pool one projectile tick after iteration T's images, and a shot
# removed in step T itself reads 0 there and 1 in the images (MEASURED 2026-11-13: the nine R2
# failures of 566 runs, each one projectile in iteration T's images and none in T + 1's). The settle
# therefore peeks on the hold at T - 1, whose pool is iteration T's: on a held frame the pool is
# reached only through a unit's kill (the destructor's purge 0x49C880, and the level holds no unit)
# or the receiver 0x49D270 handling a peer's packet (the static call graph of 0x495E90 and
# 0x453D40's direct calls, DISASSEMBLED), and a lane has no peer [INFERRED]. Step T then appends
# none: the pool's appends are a unit's weapon (no unit), a burst template's copy (none in the pool),
# the meteor spawn 0x49DF10 (the launch refuses METEOR_ON) and the receiver. R2 reads the pool in
# iterations T and T + 1 (engtrace.Run.empty_point, apply_point), so a broken link fails by name.
S_MIN, S_STEP, S_MAX = SIGHT_TTL + 1, 30, 1800
PHASE = 30                      # the economy passes' period: an apply at 1 (mod 30) is at the
                                # phase of the launch's first apply, GameTime 1
PROJECTILES = "*0x511DE8+0x141F3:4"
# WHAT EVERY LANE-LAUNCH READS BACK on its held level (engine_reads), each checked against what the launch asked:
#   the cap in force, main+0x37EE6 u16: WinMain's 0x491644 reads `UnitLimit` (0x509238) from [Preferences] of the
#     ini whose name format the exe holds at 0x5098A0 (0x49F5A0 formats `%s\<ini>`, GetPrivateProfileIntA) into
#     main+0x37EEC, and the game start copies that to +0x37EE6 (DISASSEMBLED; tagpu_scenario.c OFF_ARRAYCAP);
#   the speed ceiling, main+0x38A4B u16: the start-up reads `gamespeed` through the settings accessor at 0x4304F6
#     and stores it there at 0x430509, or on a failed read stores bx = 10, set at 0x4301AC and not changed before
#     (0x430518), then copies the ceiling to the live speed +0x38A4D (0x43052B) (DISASSEMBLED). No tick reads either
#     speed field, and under turbo a step's batch is k (tagpu_turbo.on), so the speed does not reach a lane's
#     simulation (research/notes/engine/sim.md, *The scheduler, the clock and the test pacing*): it is checked as
#     the evidence that the launch's settings key reached the game, and a 10 where the launch stored 20 says it
#     did not -- the map, the seats and the line of sight come through the same key;
#   the gravity, main+0x14263 i32 (16.16 px a tick squared): read by 14 instructions and stored by none by
#     displacement (research/notes/engine/units.md), the map's, which every reference carries from its lanes;
#   the settings root, the string at 0x509EB8 that the accessor 0x4B6880 pushes (0x4B68E8), and the ini's name
#     format at 0x5098A0: recorded, so a head says which a setup's exe or its loader named (DISASSEMBLED).
UNIT_CAP = "*0x511DE8+0x37EE6:2"
SPEED_CEILING = "*0x511DE8+0x38A4B:2"
GRAVITY = "*0x511DE8+0x14263:4"
SETTINGS_ROOT = "0x509EB8:s18"
INI_NAME = "0x5098A0:s20"
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
# the pause's end (tagpu_replay.c, pause_at_visit): group 2 its GameTime, then the `or units` clause's
# seat and count (3, 4) or the `or cells` clause's (5); a refusal matches with group 2 empty
PAUSE_RX = re.compile(r"pause: (paused at GameTime (\d+)(?: \(units of seat (\d) <= (\d+)\)"
                      r"| \((every cell decided)\))?\s*$"
                      r"|at (\d+) not taken.*)")
# one report of the delayed orders' table, each time it empties (tagpu_scenario.c, delay_report):
# a wire whose scheduled creates delay orders at several ticks has several (delay_reports)
DELAY_RX = re.compile(r"scenario: delayed orders: issued (\d+) of (\d+), refused (\d+), "
                      r"cancelled (\d+) \(deaths (\d+), cleared (\d+), skipped (\d+)\)")
DELAY_KEYS = ("issued", "of", "refused", "cancelled", "deaths", "cleared", "skipped")
# the applier's one line for a wire's `at=` and `slot=` creates, named by the GameTime of the apply
# that filled its table (tagpu_scenario.c, sched_report): a later apply reports what it cancels of an
# earlier one's inside its own visit
SCHED_RX = re.compile(r"scenario: scheduled creates of the apply at GameTime (\d+): made (\d+) of (\d+), "
                      r"refused (\d+), skipped (\d+); orders issued (\d+), delayed (\d+), "
                      r"refused (\d+), skipped (\d+)")
# a cell of the `or cells` clause decided (tagpu_replay.c, pause_cells_visit): its index in the clause, the
# GameTime and why
CELL_RX = re.compile(r"pause: cell (\d+) decided at (\d+) \((limit|units)\)")
# THE PACKED LANE'S READS: the unit array's first record and its slot count, the per-player count S that the
# pathfinder's reweight divides by (a word copied from the cap +0x37EE6 at 0x4854D5, engine/units.md), the
# line-of-sight byte whose bit 0 is Mapping off, the line shot's append cap and the sight records' cap
UNIT_BEGIN = "*0x511DE8+0x14357:4"
UNIT_SLOTS = "*0x511DE8+0x14351:2"
PER_PLAYER = "*0x511DE8+0x1434F:2"
LOS_BYTE = "*0x511DE8+0x14281:1"
PROJ_CAP_IMM = "0x49C9D2:4"
SIGHT_CAP_IMM = "0x48292E:1"
PLAYERS, PLAYER_BYTES = 0x1B63, 0x14B          # main+0x1B63, the ten player records
BLOCK_FIRST, BLOCK_LAST = 0x67, 0x6B            # a player's block of the unit array: record pointers, inclusive
UNIT_BYTES = 0x118
PLAN = "plan.json"
PLAN_SCHEMA = "ta-calib-pack-plan/1"
# a level that plays a recording says so at its first step (tagpu_replay.c)
PLAYING_RX = re.compile(r"replay: (\S+: )?playing ")
CLEAR = TREE / "scenarios" / "calib" / "clear.json"
END = TREE / "scenarios" / "calib" / "end.json"
GDI, VULKAN = "gdi", "vulkan"
LANE_DIR = re.compile(r"lane(\d+)-(\d+)$")


class ResumeRefused(Exception):
    pass


def renderer(gpu: bool) -> str:
    return VULKAN if gpu else GDI


def pacing(gpu: bool, draw: bool) -> str:
    """THE PACING: "draw" on the GPU or with --draw, else "nodraw"."""
    return "draw" if gpu or draw else "nodraw"


def arena_sha256(cfg: dict) -> str:
    """The arena a lane-launch played: its configuration, the prose `about` aside, the bytes of
    the scenario its launch loads and, for an arena on a generated map (`synthmap`), the bytes of
    that map's archive (synthmap.sha256), so a change to the map re-keys every reference on it."""
    body = json.dumps({k: v for k, v in cfg.items() if k != "about"}, sort_keys=True)
    text = body + calibref.sha256_file(TREE / cfg["scenario"])
    if cfg.get("synthmap"):
        text += synthmap.sha256(cfg["synthmap"])
    return hashlib.sha256(text.encode()).hexdigest()


def scenario_name(path: Path) -> str:
    """A run's `scenario` in its manifest row: the path under the tree, else as given."""
    return str(path.relative_to(TREE) if path.is_relative_to(TREE) else path)


def finished_lanes(out: Path) -> list:
    """The lane-launches under `out` that finished: the ones `refs build` reads -- `out`'s own, and those of
    every engine run's stamp folder in it (`calib.py engine --out X` plays into X/<stamp>)."""
    out = Path(out)
    return sorted({p.parent for g in ("lane*/manifest.jsonl", "*/lane*/manifest.jsonl") for p in out.glob(g)})


def end_scenario(arena_cfg: dict, where: Path) -> Path:
    """The lane's end for this arena: END, or END with seat 1's unit of another type or at another
    point, written into `where`. An arena's `end` entry gives both (a water arena keeps the unit on
    its land); a setup's `end_type` (calibref.arena) gives the type, for a setup without END's, and
    wins. The applier refuses a type the level lacks, so a wrong one fails the lane's end by name."""
    if "end" not in arena_cfg and not arena_cfg.get("end_type"):
        return END
    doc = json.loads(END.read_text())
    unit = doc["units"][0]
    unit["type"] = arena_cfg.get("end_type") or (arena_cfg.get("end") or {}).get("type") or unit["type"]
    if "end" in arena_cfg:
        unit["pos"] = list(arena_cfg["end"]["pos"])
    doc.pop("calib", None)
    out = Path(where) / "end.json"
    out.write_text(json.dumps(doc, indent=1) + "\n")
    return out


def sched_report(log, n, placed: int, label: str, t_apply: int) -> dict:
    """The applier's report of a wire's `at=` and `slot=` creates, from log line n on, the one of the apply at
    GameTime `t_apply`: every one of the `placed` made, none refused or skipped, and no order of theirs refused
    or skipped; anything else could not run."""
    rx = rf"scenario: scheduled creates of the apply at GameTime {t_apply}: made "
    ln = log.wait(rx, R.ACK_S, n)
    if not ln:
        raise CouldNotRun(f"{label}: no scheduled-create line of the apply at {t_apply} in {R.ACK_S} s")
    got = dict(zip(("made", "of", "refused", "skipped", "issued", "delayed", "ord_refused", "ord_skipped"),
                   (int(g) for g in SCHED_RX.search(ln).groups()[1:])))
    if got["of"] != placed or got["made"] != placed or got["refused"] or got["skipped"] or \
            got["ord_refused"] or got["ord_skipped"]:
        raise CouldNotRun(f"{label}: {ln.strip()} (the wire places {placed})")
    return got


def delay_reports(log, n, delayed: int, label: str) -> tuple:
    """The delayed-orders reports from log line n on that count a wire's `delayed` orders, summed, and their
    lines. The table reports each time it empties (tagpu_scenario.c, delay_report), every order in exactly one
    report's `of`, so the reports are read, ACK_S at most, until their `of` sums to `delayed`; a sum past it
    could not run."""
    total, lines, at = dict.fromkeys(DELAY_KEYS, 0), [], n
    end = time.monotonic() + R.ACK_S
    while total["of"] < delayed:
        ln = log.wait(DELAY_RX, max(0.0, end - time.monotonic()), at)
        if not ln:
            raise CouldNotRun(f"{label}: delayed-order lines for {total['of']} of {delayed} in {R.ACK_S} s")
        at = next(i for i in range(at, len(log.lines)) if DELAY_RX.search(log.lines[i])) + 1
        lines.append(ln.strip())
        for k, v in zip(DELAY_KEYS, DELAY_RX.search(ln).groups()):
            total[k] += int(v)
    if total["of"] != delayed:
        raise CouldNotRun(f"{label}: delayed-order lines for {total['of']} orders, the wire delays {delayed}: "
                          + " | ".join(lines))
    return total, lines


def peek_values(w, log, specs) -> list:
    """The frame visit's answers to `specs`, one trigger, as the peek lines give them (`12 (0x0000000C)`,
    `"text"`), read from the lane's own log, ACK_S at most."""
    log.read()
    n = len(log.lines)
    R._CLI_API.put_trigger(w.gamedir / "tagpu_peek.trigger", "\n".join(specs) + "\n")
    end = time.monotonic() + R.ACK_S
    while True:
        log.read()
        rows = R._CLI_API._peek_rows("\n".join(log.lines[n:]), list(specs))
        if len(rows) == len(specs):
            return [r["value"] for r in rows]
        if time.monotonic() >= end:
            raise CouldNotRun(f"peek {' '.join(specs)}: {len(rows)} of {len(specs)} answered in {R.ACK_S} s")
        R.nap(log.NAP_S)
        log.alive()


def engine_reads(w, log, arena_cfg: dict) -> dict:
    """WHAT EVERY LANE-LAUNCH READS BACK, on the held level: the cap in force, the speed ceiling, the
    gravity, the settings root and the ini's name. CouldNotRun unless the cap is the arena's unit limit
    (on a setup, the one its own ini sets), the ceiling the worker's game speed and the gravity a cap the
    fight predictor takes (1..GRAVITY_MAX)."""
    got = peek_values(w, log, (UNIT_CAP, SPEED_CEILING, GRAVITY, SETTINGS_ROOT, INI_NAME))
    nums = [R._CLI_API._peek_uint({"value": v}) for v in got[:3]]
    texts = [R._CLI_API._peek_str({"value": v}) for v in got[3:]]
    if None in nums or None in texts:
        raise CouldNotRun(f"the launch's reads: {' '.join(map(str, got))}")
    gravity = nums[2] - (1 << 32) if nums[2] >= 1 << 31 else nums[2]
    reads = {"unit_cap": nums[0], "speed_ceiling": nums[1], "gravity": gravity,
             "registry_root": texts[0], "ini": texts[1]}
    if reads["unit_cap"] != arena_cfg["unit_limit"]:
        raise CouldNotRun(f"the unit cap in force is {reads['unit_cap']}, the arena's {arena_cfg['unit_limit']}: "
                          f"UnitLimit in the ini the exe names ({texts[1]!r} at 0x5098A0, or its loader's) is "
                          f"another (a setup's is its calib.unit_limit)")
    if reads["speed_ceiling"] != w.game_speed:
        raise CouldNotRun(f"the speed ceiling is {reads['speed_ceiling']}, the launch stored {w.game_speed}: the "
                          f"settings key HKCU\\Software\\{texts[0]} did not reach the game (10 is its default "
                          f"when the read fails, 0x430518)")
    if not 1 <= gravity <= calibref.GRAVITY_MAX:
        raise CouldNotRun(f"the level's gravity {gravity} is not 1..{calibref.GRAVITY_MAX}")
    return reads


def pack_reads(w, log, arena_cfg: dict, gravity: int, sight_cap: int) -> dict:
    """THE PACKED LANE'S READS on the held level: CouldNotRun unless each seat's block is a run of the array's
    records exactly the unit limit long, Mapping is on, the gravity is synthmap.ENGINE_GRAVITY, the append cap
    pack.PROJ_CAP and the sight records' cap `sight_cap`. Returns what the head keeps: each seat's first slot,
    the block's length, the scan step s, S and both caps."""
    blocks = [f"*0x511DE8+{PLAYERS + PLAYER_BYTES * s + off:#x}:4" for s in pack.SEATS
              for off in (BLOCK_FIRST, BLOCK_LAST)]
    got = peek_values(w, log, (UNIT_BEGIN, UNIT_SLOTS, PER_PLAYER, LOS_BYTE, PROJ_CAP_IMM, SIGHT_CAP_IMM, *blocks))
    nums = [R._CLI_API._peek_uint({"value": v}) for v in got]
    if None in nums:
        raise CouldNotRun(f"the packed lane's reads: {' '.join(map(str, got))}")
    base, nslots, per_player, los, cap, sight = nums[:6]
    limit = arena_cfg["unit_limit"]
    first = []
    for s, f, last in zip(pack.SEATS, nums[6::2], nums[7::2]):
        if f < base or last < f or (f - base) % UNIT_BYTES or (last - base) % UNIT_BYTES:
            raise CouldNotRun(f"seat {s}'s block {f:#x}..{last:#x} is no run of the unit array at {base:#x}")
        fi, li = (f - base) // UNIT_BYTES, (last - base) // UNIT_BYTES
        if li >= nslots or li - fi + 1 != limit:
            raise CouldNotRun(f"seat {s}'s block holds slots {fi}..{li} of {nslots}: the positions P_k need "
                              f"exactly the unit limit {limit}")
        first.append(fi)
    if los & 1:
        raise CouldNotRun("Mapping is off (main+0x14281 bit 0): the pathfinder would read the explored map "
                          "(pack.py, THE EXPLORED MAP)")
    if gravity != synthmap.ENGINE_GRAVITY:
        raise CouldNotRun(f"the gravity is {gravity}; the projectile bound assumes {synthmap.ENGINE_GRAVITY}")
    if cap != pack.PROJ_CAP:
        raise CouldNotRun(f"the line shot's append cap at 0x49C9D2 is {cap}, the planner's {pack.PROJ_CAP}")
    if sight != sight_cap:
        raise CouldNotRun(f"the sight records' cap at 0x48292E is {sight}, the lever asked {sight_cap}")
    return {"first": first, "block": limit, "scan_step": pack.scan_step(limit), "S": per_player,
            "proj_cap": cap, "sight_cap": sight}


def aligned(t: int) -> int:
    """The least GameTime at or after t that is 1 (mod 30)."""
    return t + ((1 - t) % PHASE)


def setup_key(block: dict, arena_cfg: dict, fresh: bool, pack_k: int = None):
    """What a lane's launch is: two runs share a launch only when every part is equal. A packed lane's key ends
    with its K, so packed and unpacked runs, or runs of two K, never share a launch or a reference."""
    return (arena_cfg["map"], block["los"], block["mapping"], arena_cfg["unit_limit"],
            R.TEST_GAME_SPEED, "fresh" if fresh else "reload", block["commander_death"]) + \
        (() if pack_k is None else (pack_k,))


def plain_wire(path) -> str:
    try:
        return perturb.CLI._scn_wire(perturb.CLI._scn_compile(str(path)))
    except SystemExit as e:
        raise CouldNotRun(f"{Path(path).name}: {e}")


class SetupWorker(R.Worker):
    """A lane's worker on a compat setup (tools/compat/setups.json): the setup's own folder, as the
    compatibility suite builds it (tacompat.prepare_wine), so the exe, its Patch Loader or TADR module and
    its files are the ones a player of the setup runs.

    - It is a tacli TEST launch, not a player's: the turbo, the launch's hold and the exact stream are
      honoured only in a test launch (tagpu_replay.h, tagpu_recorder.h: tagpu_regstore_active), so the
      store prepare_wine parks is put back and the instance runs native DirectPlay as the player's launch
      does, on a port leased for it (tacompat.as_test_instance), held until close.
    - Its name is unique among live runs by construction: tacompat.inst_name carries the run's tag
      (runlock.tag(), held by calib.py and predfit before a lane starts) and the suffix the lane's index.
    - The instance is removed at close, as defcheck's and the compat suite's are; nothing of it is reused."""

    def __init__(self, out: Path, setup: dict, dll: Path, k: int, game_speed=R.TEST_GAME_SPEED):
        if game_speed not in (R.NORMAL_GAME_SPEED, R.TEST_GAME_SPEED):
            raise CouldNotRun("test game speed must be 10 or 20")
        if not 0 <= k < 16:
            raise CouldNotRun(f"lane {k}: a setup's lane suffix -c<k> holds one hex digit (tacompat.SUFFIX_ROOM)")
        tc = compat()
        self.game_speed, self.turbo, self.held, self.exact, self.gdi = game_speed, 0, False, False, False
        self.lane, self.lock, self.last_collection, self.out = None, None, None, out
        self.setup_name = setup["name"]
        self.dplay_lease = None
        self.xvfb = R.start_xvfb()
        try:
            try:
                inst = tc.prepare_wine(setup, dll, int(self.xvfb.display.lstrip(":")), suffix=f"-c{k:x}")
                self.dplay_lease = tc.as_test_instance(inst)
            except SystemExit as e:         # tacompat's die(): its message is on stderr
                raise CouldNotRun(f"setup {self.setup_name}: prepare_wine failed ({e.code})")
            self.name, self.gamedir = inst["name"], Path(inst["gamedir"])
            # keep 2: as R.Worker's (tagpu_recfile.h, THE PRUNE)
            R.tacli("arm", self.name, "recorder.on=keep 2")
        except BaseException:
            R.stop_xvfb(self.xvfb)
            if self.dplay_lease:
                self.dplay_lease.close()
            raise
        R.say(f"{self.name} ({self.setup_name}) on Xvfb {self.xvfb.display} (pid {self.xvfb.pid})")
        self.watch_caps()

    def close(self):
        """The game stopped, the instance removed and its DirectPlay port's lease released: it holds no lock file
        of the run lock to release."""
        try:
            R.tacli("stop", self.name, "--hard", timeout=120)
        finally:
            R.stop_xvfb(self.xvfb)
            try:
                R.tacli("rm", self.name, "--force", timeout=120)
            finally:
                self.dplay_lease.close()


def compat():
    """tools/compat/tacompat.py, imported when a setup is played: it does not import tareplay."""
    sys.path.insert(0, str(TREE / "tools" / "compat"))
    import tacompat
    return tacompat


class Lane:
    """One Worker and the lane-launches it plays, one at a time."""

    def __init__(self, k: int, root: Path, dll: Path, arena_cfg: dict, gpu: bool = False,
                 launches: int = 0, setup: dict = None, draw: bool = False, packed: dict = None):
        """`packed`, {"K", "sight_cap"}, makes it a packed lane (THE PACKED LANE): its launches set the sight
        records' lever and read THE PACKED LANE'S READS, and it plays batches."""
        self.k, self.root, self.dll, self.arena, self.setup = k, root, dll, arena_cfg, setup
        self.packed = packed
        self.w = SetupWorker(root, setup, dll, k) if setup else R.Worker(root)
        self.w.turbo = 1
        self.w.held = True
        self.w.exact = True
        self.w.gdi = not gpu
        self.w.nodraw = pacing(gpu, draw) == "nodraw"                 # THE PACING
        # a lane-launch's folder is lane<k>-<launches>: a resume starts past every folder there is
        self.launches = launches
        self.arena_checked = False
        self.key = None
        self.runs = 0
        self.log = None
        self.lines = []
        self.values = None

    # ---- verbs -----------------------------------------------------------------------------

    def peek(self, spec: str) -> int:
        """One peek, read from this lane's own log: the frame visit's answer, ACK_S at most."""
        value = peek_values(self.w, self.log, [spec])[0]
        v = R._CLI_API._peek_uint({"value": value})
        if v is None:
            raise CouldNotRun(f"peek {spec}: {value}")
        return v

    def apply(self, wire: str, what: str, refusal=None) -> dict:
        """The wire applied in-process through tacli's own call, on the worker's one Instance (Worker.instance:
        the game it found is verified at every call, not listed again). Its refusal could not run --
        `refusal`'s exception when the caller names one (the applier refused the wire, the game is intact) --,
        and a call that did not answer is the lane's CouldNotRun."""
        try:
            res = R._CLI_API._scn_apply(self.w.instance, wire, R.ACK_S)
        except SystemExit as e:
            raise CouldNotRun(f"{what}: {e}")
        if res.get("ok") != 1:
            raise (refusal or CouldNotRun)(f"{what}: the applier said {res.get('errors')}")
        return res

    def refused(self, path: Path, index: int, T: int, why: str):
        """A run its scenario refused: the world cleared at the held GameTime T, so the next run settles from
        it as from any run's end, and a row that names the refusal (no `index`: no reader takes it for a
        run). Raises Refused."""
        cleared = self.apply(plain_wire(CLEAR), f"{path.name} run {index}'s clear after its refusal")
        if cleared.get("gametime") != T:
            raise CouldNotRun(f"{path.name} run {index}: cleared at GameTime {cleared.get('gametime')}, "
                              f"held at {T}")
        self.T_prev = T
        self.write({"refused": scenario_name(path), "refused_index": index, "t": T, "why": why})
        raise Refused(why)

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
        w.watch_caps()                  # this lane-launch's refusals of the GPU memory cap alone
        w.place({"name": "calib"}, self.dll)
        # the arena's generated map, and no other kind's, in the game folder before the game starts
        # and mounts its archives; a retail arena removes both
        synthmap.place(w.gamedir, self.arena.get("synthmap"))
        # the sight records' lever, after place (which removes it): a packed lane's alone, read at attach
        if self.packed:
            (w.gamedir / R.SIGHTCAP_LEVER).write_text(f"{self.packed['sight_cap']}\n")
        self.log = R.Log(w)
        # On a setup no --unit-limit: tacli writes it into totala.ini, which a mod's exe does not read
        # (its ini is the one 0x5098A0 names, or the one its Patch Loader renames it to); the setup's own
        # ini sets the cap, and engine_reads checks it against the setup's calib.unit_limit.
        limit = [] if self.setup else ["--unit-limit", self.arena["unit_limit"]]
        rc, o = R.tacli("scenario", "load", w.name, TREE / self.arena["scenario"], "--defaults",
                        "--los", block["los"], "--mapping", block["mapping"],
                        "--commander-death", block["commander_death"], "--restart",
                        "--keep-dll", *limit, timeout=R.LAUNCH_TIMEOUT)
        if rc:
            raise CouldNotRun(f"scenario load {self.arena['scenario']}: {R.last(o)}")
        got = self.log.wait(r"recorder: (recording|not recording|FAILED)", R.ACK_S)
        if not got or "recorder: recording" not in got:
            raise CouldNotRun(f"the recorder: {got.strip() if got else 'no line'}")
        self.T_prev = R.held_start(w, self.log)
        if any(PLAYING_RX.search(ln) for ln in self.log.lines):
            raise CouldNotRun("the lane's level plays a recording: a lane is puppet-free")
        reads = engine_reads(w, self.log, self.arena)
        packed = pack_reads(w, self.log, self.arena, reads["gravity"], self.packed["sight_cap"]) \
            if self.packed else None
        if self.setup and not self.arena_checked and (calibref.default_arena() or self.arena.get("synthmap")):
            self.arena_against_setup()
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
                     "setup": self.setup["name"] if self.setup else None, "engine_reads": reads,
                     "setup_key": list(key), "renderer": renderer(not self.w.gdi),
                     "pacing": "nodraw" if self.w.nodraw else "draw",
                     "arena_sha256": arena_sha256(self.arena),
                     "dll": {"build": None, "sha256": calibref.sha256_file(self.dll)},
                     "catalogue_sha256": hashlib.sha256(
                         json.dumps(self.values, sort_keys=True).encode()).hexdigest()}
        if packed:
            self.head["pack"] = {"K": self.packed["K"], **packed}
        self.partial = (self.dir / "manifest.partial.jsonl").open("w")
        self.write(self.head)

    def arena_against_setup(self):
        """The arena graded against the setup's own files, once a lane: the folder read through its running
        image (tavfs.running_image: the exe with its Patch Loader's list applied), so the mounts are the ones
        this game made. The land arena by arenascan.check (calib.py grades it against the retail install; a
        setup's files could override the map, which this reads where the game reads it); a generated map by
        where the game opens it from: its .tnt and .ota must come from the archive synthmap installed, so no
        file of the setup shadows the map the arena's hash names."""
        import arenascan
        import tavfs
        try:
            vfs = tavfs.Vfs(self.w.gamedir, image=tavfs.running_image(self.w.gamedir)[0])
            kind = self.arena.get("synthmap")
            if kind:
                for ext in (".tnt", ".ota"):
                    path = f"maps/{synthmap.map_name(kind)}{ext}"
                    where = vfs.where(path)
                    if (where or "").lower() != synthmap.ufo_name(kind).lower():
                        raise calibref.CalibError(f"{path} opens from {where!r}, not {synthmap.ufo_name(kind)}")
            else:
                arenascan.check(self.arena, vfs)
        except (calibref.CalibError, tavfs.VfsError) as e:
            raise CouldNotRun(f"the arena in setup {self.setup['name']}'s folder: {e}")
        self.arena_checked = True

    def write(self, row: dict):
        self.lines.append(row)
        self.partial.write(json.dumps(row) + "\n")
        self.partial.flush()

    def settle(self):
        """The empty point: held at T = 1 (mod 30), S_MIN on from the last run's end -- past every
        temporary sight record its deaths left (THE EMPTY POINT'S SIGHT) -- with no projectile in
        the pool the empty point's images hold, read on the hold at T - 1 (THE EMPTY POINT'S
        POOL). Returns (T, the settle's extra steps)."""
        T, waits = aligned(self.T_prev + S_MIN), 0
        while True:
            if T > self.T_prev + S_MAX:
                raise CouldNotRun(f"projectiles still in flight {S_MAX} ticks after the clear at "
                                  f"{self.T_prev}")
            self.hold(T - 1)
            if self.peek(PROJECTILES) == 0:
                self.hold(T)
                return T, waits
            T += S_STEP
            waits += 1

    def batch_refused(self, i: int, cells: list, t: int, why: str):
        """A batch the engine refused with the game intact, cleared at GameTime t: one `refused` row for each
        scenario it holds (refs build refuses each), then Refused, so the lane plays the next batch."""
        self.T_prev = t
        for c in cells:
            self.write({"refused": scenario_name(Path(c.job[0])), "refused_index": c.job[1], "refused_batch": i,
                        "t": t, "why": why})
        raise Refused(f"batch {i}: {why}")

    def run_batch(self, i: int, batch: list, geoms: dict, facts: dict) -> dict:
        """Batch i of the plan (THE PACKED LANE): its cells, pack.Cell; `geoms` the planner's fight_geometry by
        path, `facts` pack.unit_facts. Returns its manifest row."""
        wall = {}
        t0 = time.monotonic()
        T, waits = self.settle()
        wall["settle"] = time.monotonic() - t0
        if not self.e0:
            self.write({"dump": "e0", "t": self.dump_now("e0")})
            self.e0 = True
        cells = sorted(batch, key=lambda c: c.k)
        label = f"batch {i}"
        pk = self.head["pack"]
        phase = pack.batch_phase([tuple(c.job) for c in cells], geoms)
        want_pos = pack.positions(pk["block"], pk["K"], phase)
        if any(c.pos != {s: want_pos[c.k] for s in pack.SEATS} or c.at != phase * c.k for c in cells):
            raise CouldNotRun(f"{label}: its positions are not the P_k of the block this launch read "
                              f"({pk['block']} slots, K {pk['K']})")
        exp = pack.batch_exp(cells, self.arena, self.values)
        wire = pack.wire(cells, self.arena, self.values)
        first = dict(zip(pack.SEATS, pk["first"]))
        now = [u for u in exp["units"] if not u["at"]]
        later = [u for u in exp["units"] if u["at"]]
        delayed_now = sum(1 for u in now for o in u["orders"] if o["after"])
        delayed = sum(1 for u in exp["units"] for o in u["orders"] if o["after"])
        sched_want = {"units": len(later), "orders": sum(len(u["orders"]) for u in later)}
        n_apply = len(self.log.lines)
        t1 = time.monotonic()
        try:
            res = self.apply(wire, label, refusal=Refused)
        except Refused as e:
            cleared = self.apply(plain_wire(CLEAR), f"{label}'s clear after its refusal")
            if cleared.get("gametime") != T:
                raise CouldNotRun(f"{label}: cleared at GameTime {cleared.get('gametime')}, held at {T}")
            self.batch_refused(i, cells, T, str(e))
        wall["apply"] = time.monotonic() - t1
        if (res.get("gametime"), res.get("cleared"), res.get("delayed"), res.get("scheduled")) != \
                (T, 0, delayed_now, sched_want):
            raise CouldNotRun(f"{label}: applied at GameTime {res.get('gametime')} (held at {T}), cleared "
                              f"{res.get('cleared')}, delayed {res.get('delayed')} of {delayed_now}, scheduled "
                              f"{res.get('scheduled')} of {sched_want}")
        units = {}
        for ordinal, row in (res.get("units") or {}).items():
            u = exp["units"][int(ordinal)]
            asked = first[u["owner"]] + u["slot"]
            if row["engine_index"] != asked:
                raise CouldNotRun(f"{label}: {u['id']} made in slot {row['engine_index']}, asked {asked}")
            units[u["id"]] = [row["engine_index"], *row["actual"]]
        if set(units) != {u["id"] for u in now}:
            raise CouldNotRun(f"{label}: the apply made {sorted(units)}, cell 0 is {sorted(u['id'] for u in now)}")
        Tend = T + cells[-1].at + pack.L
        n_arm = len(self.log.lines)
        R.arm(self.w, self.log, pack.pause_clause(cells, T, pack.L), rf"pause: armed for GameTime {Tend}\b",
              rf"pause: at {Tend} refused: .*", f"{label}'s end")
        t2 = time.monotonic()
        R.resume(self.w, self.log)
        got = self.log.wait_running(PAUSE_RX, n_arm)
        m = PAUSE_RX.search(got)
        if not m.group(2) or m.group(3):
            raise CouldNotRun(f"{label}: {got.strip()}")
        Te = int(m.group(2))
        end = "cells" if m.group(5) else "limit"
        self.log.held, self.log.pending, self.log.at = True, None, Te
        wall["run"] = time.monotonic() - t2
        decided = {}
        for ln in self.log.lines[n_arm:]:
            mc = CELL_RX.search(ln)
            if mc:
                decided[int(mc.group(1))] = [int(mc.group(2)), mc.group(3)]
        t3 = time.monotonic()
        cleared = self.apply(plain_wire(CLEAR), f"{label}'s clear")
        wall["clear"] = time.monotonic() - t3
        if cleared.get("gametime") != Te:
            raise CouldNotRun(f"{label}: cleared at GameTime {cleared.get('gametime')}, paused at {Te}")
        sched = sched_report(self.log, n_apply, len(exp["units"]), label, T)
        report = None
        if delayed:
            report, lines = delay_reports(self.log, n_apply, delayed, label)
            ln = " | ".join(lines)
            if report["skipped"] or report["issued"] + report["refused"] + report["cancelled"] != report["of"]:
                raise CouldNotRun(f"{label}: {ln}")
            if report["refused"]:
                # the batch played and is cleared at Te: the lane is at a batch's end
                self.batch_refused(i, cells, Te, ln)
        wall["total"] = time.monotonic() - t0
        rows = []
        for c in cells:
            path = Path(c.job[0])
            _cexp, record, cwire, _cl = perturb.run_wire(path, c.job[1], self.values, self.arena)
            rows.append({"scenario": scenario_name(path), "scenario_sha256": calibref.sha256_file(path),
                         "index": c.job[1], "seed": record["seed"], "swap": record["swap"],
                         "perturbation": record, "wire_sha256": hashlib.sha256(cwire.encode()).hexdigest(),
                         "proj_bound": geoms[c.job[0]]["proj_records"], "decided": decided.get(c.k),
                         "cell": pack.cell_json(c)})
        types = sorted({u["type"].upper() for u in exp["units"]})
        row = {"batch": i, "K": pk["K"], "t_apply": T, "t_end": Te, "end": end,
               "wire_sha256": hashlib.sha256(wire.encode()).hexdigest(), "cells": rows,
               "types": {t: {"slots": facts[t]["slots"], "bursts": facts[t]["bursts"]} for t in types},
               "apply": {"units": units, "delayed": res.get("delayed"), "scheduled": res.get("scheduled"),
                         "features": [f["def"] for f in (res.get("features") or {}).values()]},
               "sched": sched, "clear": {"t": Te, "cleared_slots": cleared.get("cleared_slots") or []},
               "delayed": report, "settle": {"t_next": T, "waits": waits},
               "wall": {k: round(v, 3) for k, v in wall.items()}}
        self.write(row)
        self.T_prev = Te
        self.runs += 1
        self.ticks += Te - T
        return row

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
        try:
            res = self.apply(wire, f"{path.name} run {index}", refusal=Refused)
        except Refused as e:
            self.refused(path, index, T, str(e))
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
        end = f"units {m.group(3)} <= {m.group(4)}" if m.group(3) else "cells" if m.group(5) else "limit"
        self.log.held, self.log.pending, self.log.at = True, None, Te
        wall["run"] = time.monotonic() - t2
        t3 = time.monotonic()
        cleared = self.apply(plain_wire(CLEAR), f"{path.name} run {index}'s clear")
        wall["clear"] = time.monotonic() - t3
        if cleared.get("gametime") != Te:
            raise CouldNotRun(f"{path.name} run {index}: cleared at GameTime "
                              f"{cleared.get('gametime')}, paused at {Te}")
        placed = sum(1 for u in exp["units"] if u.get("at") or u.get("slot") is not None)
        if placed:
            sched_report(self.log, n_apply, placed, f"{path.name} run {index}", T)
        report = None
        if delayed:
            report, lines = delay_reports(self.log, n_apply, delayed, f"{path.name} run {index}")
            ln = " | ".join(lines)
            if report["skipped"] or \
                    report["issued"] + report["refused"] + report["cancelled"] != report["of"]:
                raise CouldNotRun(f"{path.name} run {index}: {ln}")
            if report["refused"]:
                # the run played and is cleared at Te: the lane is at a run's end, as after any run
                self.T_prev = Te
                self.write({"refused": scenario_name(path), "refused_index": index, "t": Te,
                            "why": ln.strip()})
                raise Refused(f"{path.name} run {index}: {ln.strip()}")
        if dump:
            R.wait_dumps(self.log, [dump])
        wall["total"] = time.monotonic() - t0
        row = {"scenario": scenario_name(path),
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
        self.apply(plain_wire(end_scenario(self.arena, self.dir)), "the lane's end")
        R.resume(self.w, self.log)
        for rx in (r"recorder: ended after", r"recorder: writer: .* closed"):
            self.log.wait_end(rx, n)
        self.w.collect(self.dir, recordings=True)
        self.w.stop()
        recs = sorted(self.dir.glob("*.imprec"), key=lambda p: p.stat().st_mtime)
        if not recs:
            raise CouldNotRun("no recording in the lane's replays folder")
        self.head["recording"] = recs[-1].name
        # the build the DLL that played embeds: its own GIT_COMMIT, from its recording, so the
        # label cannot be the tree's when the tree has moved since the DLL was built
        try:
            self.head["dll"]["build"] = tarec.header_only(recs[-1]).get("build")
        except tarec.Unreadable as e:
            raise CouldNotRun(f"the lane's recording: {e}")
        if not self.head["dll"]["build"]:
            raise CouldNotRun(f"{recs[-1].name} carries no build (RT_BUILD)")
        self.head["lane_seconds"] = round(time.monotonic() - self.started, 3)
        self.head["ticks"] = self.ticks
        self.head["recording_bytes"] = recs[-1].stat().st_size
        self.partial.close()
        rows = [self.head] + self.lines[1:]
        (self.dir / "manifest.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows))
        (self.dir / "manifest.partial.jsonl").unlink()
        self.key = None
        # the game has ended: a refusal of the GPU memory cap anywhere in it fails the
        # lane-launch by name, its rows kept for the reading
        refused = self.w.cap_failure()
        if refused:
            raise Failed(refused)

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


def jobs_of(scenarios, runs, mode, arena_cfg, done=frozenset()):
    """[(setup key, path, block, indices, mode)], chunked by CHUNK, sorted by setup key so a lane
    keeps its launch. An exact scenario's runs are 0 and 1 by reload and a third fresh launch. A
    run in `done`, {(scenario_name, index)}, is left out."""
    out = []
    for path in scenarios:
        doc = json.loads(path.read_text())
        block = calibref.calib_block(doc, path.name, arena_cfg)
        n = runs or block["runs"]
        fresh = mode == "fresh"
        name = scenario_name(path)

        def left(indices):
            return [i for i in indices if (name, i) not in done]
        if "exact" in block["kind"] and mode != "fresh":
            for idx, how in (([0, 1], "reload"), ([2], "fresh")):
                if left(idx):
                    out.append((setup_key(block, arena_cfg, how == "fresh"), path, block, left(idx), how))
            continue
        indices = left(range(n))
        step = 1 if fresh else CHUNK
        for i in range(0, len(indices), step):
            key = setup_key(block, arena_cfg, fresh)
            out.append((key, path, block, indices[i:i + step], "fresh" if fresh else "reload"))
    return sorted(out, key=lambda j: (j[0], str(j[1]), j[3][0]))


def lane_launches(out: Path):
    """(folder, head, rows, partial lines) of every lane-launch folder in `out`, in name order: a finished one's
    head and other rows from its manifest.jsonl; for one that did not finish, head None and its partial
    manifest's lines (none for a folder with neither)."""
    for d in sorted(Path(out).glob("lane*")):
        f = d / "manifest.jsonl"
        if not f.exists():
            p = d / "manifest.partial.jsonl"
            yield d, None, [], p.read_text().splitlines() if p.exists() else []
            continue
        rows = [json.loads(ln) for ln in f.read_text().splitlines() if ln.strip()]
        yield d, rows[0], rows[1:], []


def head_problems(head: dict, arena_cfg: dict, dll: Path, gpu: bool, draw: bool) -> list:
    """What a finished lane-launch's head played with that this run does not: the DLL, the renderer, the pacing,
    the compat setup or the arena. Its runs and this run's would not be one sample."""
    dll_sha, rend, pace, mod = calibref.sha256_file(dll), renderer(gpu), pacing(gpu, draw), arena_cfg.get("setup")
    # a head without a renderer was played before the field, on the default renderer
    why = [f"the DLL {head['dll']['sha256'][:12]}, this run's {dll_sha[:12]}"
           if head["dll"]["sha256"] != dll_sha else None,
           f"the renderer {head.get('renderer', VULKAN)}, this run's {rend}"
           if head.get("renderer", VULKAN) != rend else None,
           # a head without a pacing was drawn, the only pacing before the field
           f"the pacing {head.get('pacing', 'draw')}, this run's {pace}"
           if head.get("pacing", "draw") != pace else None,
           f"the setup {head.get('setup') or 'retail'}, this run's {mod or 'retail'}"
           if head.get("setup") != mod else None,
           "another arena" if head.get("arena_sha256") != arena_sha256(arena_cfg) else None]
    return [w for w in why if w]


def done_runs(out: Path, scenarios, runs, mode, arena_cfg, dll: Path, gpu: bool, draw: bool = False):
    """A resume of the engine run in `out`: ({(scenario_name, index)} its finished lane-launches
    hold, the launch number every new lane-launch starts past, the unfinished lane-launches' runs
    that are played again). ResumeRefused names every finished lane-launch or run that another
    DLL, arena, compat setup, renderer or pacing played, or whose scenario file or setup key is not this
    run's: its runs and this run's would not be one sample. Another setup's arena hash differs
    already (calibref.arena); the setup is named as well, so the refusal says which."""
    want = {}
    for key, path, _block, indices, _how in jobs_of(scenarios, runs, mode, arena_cfg):
        for i in indices:
            want[scenario_name(path), i] = (list(key), calibref.sha256_file(path))
    done, bad, base, replayed = {}, [], 0, 0
    for d, head, rows, partial in lane_launches(out):
        m = LANE_DIR.match(d.name)
        if m:
            base = max(base, int(m.group(2)))
        if head is None:
            replayed += sum(1 for ln in partial if '"index"' in ln)
            continue
        why = head_problems(head, arena_cfg, dll, gpu, draw)
        if why:
            bad.append(f"{d.name}: played with {'; '.join(why)}")
            continue
        for r in rows:
            if "index" not in r:
                continue
            k = (r["scenario"], r["index"])
            if k not in want:
                continue
            setup, sha = want[k]
            if r["scenario_sha256"] != sha:
                bad.append(f"{d.name}: {r['scenario']} run {r['index']}: the scenario file changed since")
            elif head["setup_key"] != setup:
                bad.append(f"{d.name}: {r['scenario']} run {r['index']}: setup {head['setup_key']}, "
                           f"this run's {setup}")
            elif k in done:
                bad.append(f"{d.name}: {r['scenario']} run {r['index']}: also in {done[k]}")
            else:
                done[k] = d.name
    if bad:
        raise ResumeRefused("\n".join(bad))
    return set(done), base, replayed



def lane_count(jobs, j):
    """The lanes run_engine plays `jobs` on, one game each at once: what calib.py declares to the
    run lock."""
    return min(j, J_MAX, max(1, len(jobs)))


def run_engine(scenarios, runs, j, mode, out: Path, dll: Path, budget=RUNS_PER_LAUNCH,
               done=frozenset(), gpu=False, launches=0, setup: dict = None, draw=False):
    """Every job on j lanes, but the runs in `done`; each lane's launches numbered past `launches`;
    on `setup` (a compat setup's record) each lane a SetupWorker in that setup's folder. Returns
    (manifest dirs, the could-not-run messages)."""
    arena_cfg = calibref.arena()
    jobs = jobs_of(scenarios, runs, mode, arena_cfg, done)
    work = queue.Queue()
    for jb in jobs:
        work.put(jb)
    errors, lock = [], threading.Lock()
    lanes = []

    def lane_main(k):
        lane = None
        try:
            lane = Lane(k, out, dll, arena_cfg, gpu, launches, setup, draw)
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
                        try:
                            lane.run(path, index, block, how)
                        except Refused as e:
                            # the scenario refuses itself: its other runs are not played, the lane goes on
                            with lock:
                                errors.append(f"{path.name} refused at run {index}, runs {indices} not "
                                              f"played: {e}")
                            R.say(f"lane {k}: {path.name} refused at run {index}: {e}")
                            if how == "fresh":
                                lane.finish()
                            break
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
    return finished_lanes(out), errors


# ---- the packed lane: its plan, its resume, its lanes --------------------------------------------------------

def batch_key(batch: list, geoms: dict, arena_cfg: dict, K: int) -> tuple:
    """The setup key every cell of a batch plays under; CouldNotRun when two cells' would differ, which no one
    launch can hold."""
    keys = {setup_key(geoms[c.job[0]]["block"], arena_cfg, False, K) for c in batch}
    if len(keys) != 1:
        raise CouldNotRun(f"a batch's cells ask {len(keys)} launches: {sorted(keys)}")
    return next(iter(keys))


def plan_doc(batches: list, K: int, arena_cfg: dict, prior: dict) -> dict:
    """The packed run's plan as PLAN keeps it: K, the arena's hash, the lever's cap, every scenario's sha256,
    the prior and every batch's cells. Written once, before any lane, so a resume plays this plan whatever the
    prior's folder holds by then."""
    paths = sorted({c.job[0] for b in batches for c in b})
    return {"schema": PLAN_SCHEMA, "K": K, "arena_sha256": arena_sha256(arena_cfg),
            "sight_cap": pack.LEVER_SIGHT_CAP,
            "scenarios": {scenario_name(Path(p)): calibref.sha256_file(p) for p in paths},
            "prior": prior, "batches": [[pack.cell_json(c) for c in b] for b in batches]}


def write_plan(out: Path, doc: dict):
    (Path(out) / PLAN).write_text(json.dumps(doc, indent=1) + "\n")


def read_plan(out: Path, K: int, arena_cfg: dict, jobs: list) -> list:
    """The batches of the packed run in `out` (PLAN), for a resume: ResumeRefused unless the plan is for this K
    and arena, its scenario files are unchanged and it holds exactly `jobs`, [(path, index)]."""
    f = Path(out) / PLAN
    if not f.exists():
        raise ResumeRefused(f"{out} holds no {PLAN}: it is no packed engine run")
    doc = json.loads(f.read_text())
    bad = []
    if doc.get("schema") != PLAN_SCHEMA:
        bad.append(f"{PLAN} is not {PLAN_SCHEMA}")
    else:
        if doc["K"] != K:
            bad.append(f"its plan is for K = {doc['K']}, this run's {K}")
        if doc["arena_sha256"] != arena_sha256(arena_cfg):
            bad.append("its plan is for another arena")
        for name, sha in sorted(doc["scenarios"].items()):
            path = TREE / name
            if not path.exists() or calibref.sha256_file(path) != sha:
                bad.append(f"{name}: the scenario file changed since the plan")
    if bad:
        raise ResumeRefused("\n".join(bad))
    batches = [[pack.cell_from_json(c) for c in b] for b in doc["batches"]]
    if sorted(c.job for b in batches for c in b) != sorted((str(p), i) for p, i in jobs):
        raise ResumeRefused("its plan holds other jobs than this run's --scenarios and --runs")
    return batches


def done_batches(out: Path, batches: list, keys: list, K: int, arena_cfg: dict, dll: Path, gpu: bool,
                 draw: bool):
    """A resume of the packed engine run in `out`: ({batch index} its finished lane-launches hold, the launch
    number every new lane-launch starts past, the unfinished lane-launches' batches that are played again).
    ResumeRefused, as done_runs, names every finished lane-launch another DLL, arena, setup, renderer, pacing
    or K played, and every batch row that is not the plan's batch of its index (`keys`, each batch's setup key)
    or is held twice."""
    done, bad, base, replayed = {}, [], 0, 0
    for d, head, rows, partial in lane_launches(out):
        m = LANE_DIR.match(d.name)
        if m:
            base = max(base, int(m.group(2)))
        if head is None:
            replayed += sum(1 for ln in partial if '"cells"' in ln)
            continue
        why = head_problems(head, arena_cfg, dll, gpu, draw)
        if (head.get("pack") or {}).get("K") != K:
            why.append(f"K {(head.get('pack') or {}).get('K')}, this run's {K}")
        if why:
            bad.append(f"{d.name}: played with {'; '.join(why)}")
            continue
        for r in rows:
            if "cells" not in r:
                continue
            i = r["batch"]
            got = [pack.cell_from_json(c["cell"]).job for c in r["cells"]]
            if not 0 <= i < len(batches) or got != [c.job for c in sorted(batches[i], key=lambda c: c.k)]:
                bad.append(f"{d.name}: batch {i} is not the plan's")
            elif head["setup_key"] != list(keys[i]):
                bad.append(f"{d.name}: batch {i}: setup {head['setup_key']}, this run's {list(keys[i])}")
            elif i in done:
                bad.append(f"{d.name}: batch {i}: also in {done[i]}")
            else:
                done[i] = d.name
    if bad:
        raise ResumeRefused("\n".join(bad))
    return set(done), base, replayed


def run_packed(batches: list, geoms: dict, facts: dict, K: int, j: int, out: Path, dll: Path, budget: int,
               done=frozenset(), gpu=False, launches=0, setup: dict = None, draw=True):
    """Every batch of the plan but those in `done`, on j packed lanes (THE PACKED LANE), each lane-launch
    playing at most `budget` batches. Returns (manifest dirs, the could-not-run messages)."""
    arena_cfg = calibref.arena()
    work = queue.Queue()
    for i, b in enumerate(batches):
        if i not in done:
            work.put((i, b))
    n = work.qsize()
    errors, lock = [], threading.Lock()
    packed = {"K": K, "sight_cap": pack.LEVER_SIGHT_CAP}

    def lane_main(k):
        lane = None
        try:
            lane = Lane(k, out, dll, arena_cfg, gpu, launches, setup, draw, packed)
            while not R.STOP.is_set():
                try:
                    i, batch = work.get_nowait()
                except queue.Empty:
                    break
                try:
                    key = batch_key(batch, geoms, arena_cfg, K)
                    if lane.key != key or lane.runs >= budget:
                        if lane.key is not None:
                            lane.finish()
                        lane.launch(key, geoms[batch[0].job[0]]["block"])
                    lane.run_batch(i, batch, geoms, facts)
                except Refused as e:
                    # the batch refuses itself: its scenarios' refused rows are written, the lane goes on
                    with lock:
                        errors.append(f"batch {i} refused: {e}")
                    R.say(f"lane {k}: batch {i} refused: {e}")
                except (CouldNotRun, Failed) as e:
                    with lock:
                        errors.append(f"batch {i}: {e}")
                    R.say(f"lane {k}: could not run batch {i}: {e}")
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

    threads = [threading.Thread(target=lane_main, args=(k,), daemon=True) for k in range(min(j, J_MAX, max(1, n)))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return finished_lanes(out), errors


def batches_per_launch(K: int) -> int:
    """A packed lane-launch's budget: RUNS_PER_LAUNCH runs' worth of batches, which bounds its recording as a
    run budget does."""
    return math.ceil(RUNS_PER_LAUNCH / K)


class Refused(CouldNotRun):
    """A run its scenario refused -- the applier refused its wire, or a delayed order its unit could not take --
    with the game intact: the scenario is not played on, the lane carries on."""


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
