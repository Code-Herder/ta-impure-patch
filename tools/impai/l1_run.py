#!/usr/bin/env python3
"""l1_run -- one ImpAI game played to a GameTime and ended, its log kept: the runs of ImpAI's L1
that are no replay test group (the patches' map, the cost).

    tools/impai/l1_run.py SCENARIO --end T --out DIR [--lever NAME[=TEXT] ...] [--normal-speed]
                          [--unit-limit N] [--dll PATH]

Under the run lock (tools/runlock.py), on a tareplay worker of its own (an instance tareplay-<k>
on a private Xvfb, tools/replay/tareplay.py): the DLL (default the tree's tagpu/ddraw/ddraw.dll)
placed, each lever written into the game folder as tagpu_<NAME> (tacli arm's naming), the scenario
launched held (THE LAUNCH'S HOLD) and driven by exact ticks to GameTime T -- at the harness's
test speed with the turbo's one tick a frame, or with --normal-speed at normal speed and no turbo,
as a cost fixture is -- then ended by the replay harness's end scenario, so the level's teardown
writes ImpAI's level-end lines. The run's log is written to DIR/tagpu.log; the instance is
stopped. Exit 0 when the game ran to its end, 2 when it could not.
"""
import argparse
import sys
from pathlib import Path

TREE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TREE / "tools" / "replay"))
sys.path.insert(0, str(TREE / "tools"))
import tareplay as tr  # noqa: E402
import runlock  # noqa: E402
import talog  # noqa: E402

LAUNCH_S = 900


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("scenario", type=Path)
    ap.add_argument("--end", type=int, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--lever", action="append", default=[], help="NAME[=TEXT], written as tagpu_NAME")
    ap.add_argument("--normal-speed", action="store_true")
    ap.add_argument("--unit-limit", type=int, default=tr.UNIT_LIMIT)
    ap.add_argument("--dll", type=Path, default=TREE / "tagpu" / "ddraw" / "ddraw.dll")
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    runlock.hold(TREE, f"l1_run {a.scenario.name} to {a.end}")
    w = tr.Worker(a.out, tr.NORMAL_GAME_SPEED if a.normal_speed else tr.TEST_GAME_SPEED)
    try:
        w.held = True
        w.turbo = 0 if a.normal_speed else 1
        w.place({}, a.dll)
        for spec in a.lever:
            name, _, text = spec.partition("=")
            (w.gamedir / f"tagpu_{name}").write_text(text + "\n" if text else "")
        log = tr.Log(w)
        rc, o = tr.tacli("scenario", "load", w.name, a.scenario.resolve(), "--defaults", "--restart",
                         "--keep-dll", "--unit-limit", str(a.unit_limit), timeout=LAUNCH_S)
        if rc:
            raise tr.CouldNotRun(f"scenario load: {tr.last(o)}")
        tr.held_start(w, log)
        tr.run_to(w, log, a.end, [])
        n = len(log.lines)
        rc, o = tr.tacli("scenario", "apply", w.name, tr.END_SCENARIO, timeout=300)
        if rc:
            raise tr.CouldNotRun(f"apply {tr.END_SCENARIO.name}: {tr.last(o)}")
        tr.resume(w, log)
        log.wait_running(r"impai: handicap rows", n)
        log.read()
        (a.out / "tagpu.log").write_text(talog.run_text(w.gamedir))
        w.stop(hard=True)
    except (tr.CouldNotRun, tr.Failed) as e:
        print(f"l1_run: COULD NOT RUN: {e}")
        try:
            (a.out / "tagpu.log").write_text(talog.run_text(w.gamedir))
        except OSError:
            pass
        return 2
    finally:
        w.close()
    print(f"l1_run: {a.scenario.name} ended at {a.end}; log {a.out / 'tagpu.log'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
