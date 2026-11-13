#!/usr/bin/env python3
"""l1_run -- one ImpAI game played to a GameTime and ended, its log kept: the runs of ImpAI's L1
that are no replay test group (the patches' map, the cost).

    tools/impai/l1_run.py SCENARIO --end T --out DIR [--lever NAME[=TEXT] ...] [--normal-speed]
                          [--unit-limit N] [--dll PATH] [--gpu]

The unit limit is the scenario's own (`setup.unit_limit`, as impai-cost.json's 1500 for its 1499-unit
armies), else the replay harness's default; --unit-limit overrides both. The run prints which it used, and
DIR/run.txt beside the log says so with the scenario, the end, the levers, the speed and the renderer.

THE RENDERER. The game renders with GDI (tareplay.GDI_LEVER) and takes no GPU token; --gpu renders it on
the default renderer, for a debugging run, and takes one. A log is compared only with logs of its own
renderer (research/notes/modules/impai.md, *The renderer*): the cost table of research/notes/impai/player.md
was measured on the default renderer, so a GDI run's `impai: cost` lines are not held to it.

Under the run lock (tools/runlock.py), on a tareplay worker of its own (an instance tareplay-<k>
on a private Xvfb, tools/replay/tareplay.py): the DLL (default the tree's tagpu/ddraw/ddraw.dll)
placed, each lever written into the game folder as tagpu_<NAME> (tacli arm's naming) and removed again
when the run ends, however it ends, so no later game on the worker reads it; the scenario
launched held (THE LAUNCH'S HOLD) and driven by exact ticks to GameTime T -- at the harness's
test speed with the turbo's one tick a frame, or with --normal-speed at normal speed and no turbo,
as a cost fixture is -- then ended by the replay harness's end scenario, so the level's teardown
writes ImpAI's level-end lines. The run's log is written to DIR/tagpu.log; the instance is
stopped. Exit 0 when the game ran to its end, 2 when it could not.
"""
import argparse
import json
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
    ap.add_argument("--gpu", action="store_true",
                    help="render on the default renderer and take a GPU token; by default the game "
                         "renders with GDI and takes none")
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    limit, why = unit_limit(a.scenario, a.unit_limit)
    print(f"l1_run: unit limit {limit} ({why})")
    (a.out / "run.txt").write_text(f"scenario {a.scenario.name}\nend {a.end}\nunit limit {limit} ({why})\n"
                                   f"levers {' '.join(a.lever) or 'none'}\n"
                                   f"speed {'normal' if a.normal_speed else 'test, turbo'}\n"
                                   f"renderer {renderer(a.gpu)}\n")
    runlock.hold(TREE, f"l1_run {a.scenario.name} to {a.end}", 1, gpu=a.gpu)
    w = tr.Worker(a.out, tr.NORMAL_GAME_SPEED if a.normal_speed else tr.TEST_GAME_SPEED)
    written = []
    try:
        w.gdi = not a.gpu
        w.held = True
        w.turbo = 0 if a.normal_speed else 1
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
        rc, o = tr.tacli("scenario", "apply", w.name, tr.END_SCENARIO, timeout=300)
        if rc:
            raise tr.CouldNotRun(f"apply {tr.END_SCENARIO.name}: {tr.last(o)}")
        tr.resume(w, log)
        log.wait_end(r"impai: handicap rows", n)
        log.read()
        (a.out / "tagpu.log").write_text(talog.run_text(w.gamedir))
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
    print(f"l1_run: {a.scenario.name} ended at {a.end}; log {a.out / 'tagpu.log'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
