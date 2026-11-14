#!/usr/bin/env python3
"""l1_run -- one ImpAI game played to a GameTime and ended, its log kept: the runs of ImpAI's L1
that are no replay test group (the patches' map, the cost).

    tools/impai/l1_run.py SCENARIO --end T --out DIR [--lever NAME[=TEXT] ...] [--normal-speed]
                          [--unit-limit N] [--dll PATH] [--gpu] [--draw]

The unit limit is the scenario's own (`setup.unit_limit`, as impai-cost.json's 1500 for its 1499-unit
armies), else the replay harness's default; --unit-limit overrides both. The run prints which it used, and
DIR/run.txt beside the log says so with the scenario, the end, the levers, the speed and the renderer.

THE RENDERER. The game renders with GDI (tareplay.GDI_LEVER) and takes no GPU token; --gpu renders it on
the default renderer, for a debugging run, and takes one. A log is compared only with logs of its own
renderer (research/notes/modules/impai.md, *The renderer*): the cost table of research/notes/impai/player.md
was measured on the default renderer, so a GDI run's `impai: cost` lines are not held to it.

THE PACING. A GDI game at test speed runs NO DRAW (tareplay.NODRAW_LEVER): while the turbo runs, a frame whose
batch was the turbo's skips the game screen's draw (tagpu_nodraw.c, THE GATE). --draw renders every frame,
for debugging; --gpu implies it, and --normal-speed, which has no turbo, draws every frame as well. run.txt
names the pacing.

Under the run lock (tools/runlock.py), on a tareplay worker of its own (an instance tareplay-<k>
on a private Xvfb, tools/replay/tareplay.py): the DLL (default the tree's tagpu/ddraw/ddraw.dll)
placed, each lever written into the game folder as tagpu_<NAME> (tacli arm's naming) and removed again
when the run ends, however it ends, so no later game on the worker reads it; the scenario
launched held (THE LAUNCH'S HOLD) and driven by exact ticks to GameTime T -- at the harness's
test speed with the turbo's one tick a frame, or with --normal-speed at normal speed and no turbo,
as a cost fixture is -- then ended (THE END), so the level's teardown writes ImpAI's level-end lines.
The run's log is written to DIR/tagpu.log and its recording, closed, to DIR/<scenario>.imprec; the
instance is stopped. Exit 0 when the game ran to its end, 2 when it could not.

THE END is the engine's own, whatever the scenario set: on the held tick T the level's end test is
armed (tareplay.REARM_SCENARIO, `objective on`) and then the replay harness's end scenario clears
every unit but one of seat 1's, which the armed test ends within LEVEL_END_TICKS (Log.wait_end). A
scenario with `"objective": "off"` disarms that test (tagpu_scenario.c, apply_objective) and its
level would otherwise run on with the human seat empty, so the re-arm is applied every time, not
read from the scenario: it holds for an objective set off by the load, by a step, or not at all.
The recording is copied once the recorder has said it ended and its writer closed the file, so the
copy is the whole file the game wrote.
"""
import argparse
import json
import shutil
import sys
from pathlib import Path

TREE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TREE / "tools" / "replay"))
sys.path.insert(0, str(TREE / "tools"))
import tareplay as tr  # noqa: E402
import runlock  # noqa: E402
import talog  # noqa: E402

LAUNCH_S = 900


def write_levers(gamedir, specs, written):
    """Each NAME[=TEXT] as gamedir/tagpu_NAME, its path appended to `written` as it is made, so the
    caller removes every lever this wrote even when a later one fails. A path that is a link, which
    would write into the install the game folder links, is refused."""
    for spec in specs:
        name, _, text = spec.partition("=")
        lever = Path(gamedir) / f"tagpu_{name}"
        if lever.is_symlink():
            raise tr.CouldNotRun(f"{lever} is a link; refusing to write through it")
        written.append(lever)
        lever.write_text(text + "\n" if text else "")


def remove_levers(written):
    for lever in written:
        lever.unlink(missing_ok=True)


def keep_recording(gamedir, into):
    """The game's recording copied to `into`, its path: the newest .imprec of the game folder's replays,
    which Worker.place empties before the launch, so it is this game's."""
    recs = sorted((Path(gamedir) / "replays").glob("*.imprec"))
    if not recs:
        raise tr.CouldNotRun("no .imprec in the game's replays folder")
    shutil.copy2(recs[-1], into)
    return into


def unit_limit(scenario, given):
    """(limit, where it came from): --unit-limit, else the scenario's setup.unit_limit, a positive
    integer, else the harness's -- also for a scenario that is not a JSON object with a `setup` object."""
    if given is not None:
        return given, "--unit-limit"
    try:
        doc = json.loads(Path(scenario).read_text())
    except (OSError, ValueError):
        doc = None
    setup = doc.get("setup") if isinstance(doc, dict) else None
    want = setup.get("unit_limit") if isinstance(setup, dict) else None
    if isinstance(want, int) and not isinstance(want, bool) and want > 0:
        return want, f"{Path(scenario).name}'s setup.unit_limit"
    return tr.UNIT_LIMIT, "the replay harness's default"


def renderer(gpu):
    return "vulkan" if gpu else "gdi"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("scenario", type=Path)
    ap.add_argument("--end", type=int, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--lever", action="append", default=[], help="NAME[=TEXT], written as tagpu_NAME")
    ap.add_argument("--normal-speed", action="store_true")
    ap.add_argument("--unit-limit", type=int, default=None,
                    help="default: the scenario's setup.unit_limit, else the replay harness's")
    ap.add_argument("--dll", type=Path, default=TREE / "tagpu" / "ddraw" / "ddraw.dll")
    ap.add_argument("--draw", action="store_true",
                    help="render every frame, for debugging (THE PACING); --gpu implies it")
    ap.add_argument("--gpu", action="store_true",
                    help="render on the default renderer and take a GPU token; by default the game "
                         "renders with GDI and takes none")
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    limit, why = unit_limit(a.scenario, a.unit_limit)
    nodraw = not a.gpu and not a.draw and not a.normal_speed     # THE PACING
    print(f"l1_run: unit limit {limit} ({why})")
    (a.out / "run.txt").write_text(f"scenario {a.scenario.name}\nend {a.end}\nunit limit {limit} ({why})\n"
                                   f"levers {' '.join(a.lever) or 'none'}\n"
                                   f"speed {'normal' if a.normal_speed else 'test, turbo'}\n"
                                   f"renderer {renderer(a.gpu)}\n"
                                   f"pacing {'nodraw' if nodraw else 'draw'}\n")
    runlock.hold(TREE, f"l1_run {a.scenario.name} to {a.end}", 1, gpu=a.gpu)
    w = tr.Worker(a.out, tr.NORMAL_GAME_SPEED if a.normal_speed else tr.TEST_GAME_SPEED)
    written = []
    try:
        w.gdi = not a.gpu
        w.held = True
        w.turbo = 0 if a.normal_speed else 1
        w.nodraw = nodraw
        w.place({}, a.dll)
        write_levers(w.gamedir, a.lever, written)
        log = tr.Log(w)
        rc, o = tr.tacli("scenario", "load", w.name, a.scenario.resolve(), "--defaults", "--restart",
                         "--keep-dll", "--unit-limit", str(limit), timeout=LAUNCH_S)
        if rc:
            raise tr.CouldNotRun(f"scenario load: {tr.last(o)}")
        tr.held_start(w, log)
        tr.run_to(w, log, a.end, [])
        n = len(log.lines)
        for sc in (tr.SCENARIOS / tr.REARM_SCENARIO, tr.END_SCENARIO):        # THE END
            rc, o = tr.tacli("scenario", "apply", w.name, sc, timeout=300)
            if rc:
                raise tr.CouldNotRun(f"apply {sc.name}: {tr.last(o)}")
        tr.resume(w, log)
        for rx in (r"impai: handicap rows", r"recorder: ended after", r"recorder: writer: .* closed"):
            log.wait_end(rx, n)
        log.read()
        (a.out / "tagpu.log").write_text(talog.run_text(w.gamedir))
        rec = keep_recording(w.gamedir, a.out / f"{a.scenario.stem}.imprec")
        w.stop(hard=True)
        # the game has ended: a refusal of the GPU memory cap anywhere in it fails the run by name
        refused = w.cap_failure()
        if refused:
            print(f"l1_run: FAILED: {refused}")
            return 1
    except (tr.CouldNotRun, tr.Failed) as e:
        print(f"l1_run: COULD NOT RUN: {e}")
        try:
            (a.out / "tagpu.log").write_text(talog.run_text(w.gamedir))
        except OSError:
            pass
        return 2
    finally:
        try:
            remove_levers(written)
        finally:
            w.close()
    print(f"l1_run: {a.scenario.name} ended at {a.end}; log {a.out / 'tagpu.log'}; recording {rec}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
