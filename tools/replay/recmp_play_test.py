#!/usr/bin/env python3
"""Play a saved multiplayer (mode 3) recording on a puppet host and dump it.

    tools/replay/recmp_play_test.py RECORDING --out DIR [--dll PATH] [--at T ...] [--to-end] [--perspective N]
                                    [--content FILE ...]

One tareplay worker (its own tacli instance on its own Xvfb) plays RECORDING through
`tacli replay play` at the standard internal speed 20, takes `dump p<T>` at each --at T, plays to
the end with --to-end or when no dump is asked, exits the game so that its teardown logs the
playing's counts, and keeps the dumps and the run's log in DIR. Exit 0 when
the playing started, every dump was taken, the log names no stop and, when it plays to the end,
it reached it; 1 otherwise. The dumps are
compared separately: this only drives the game. --content copies a game archive the recording
was made with (tools/reproduce_fixture.py's, say) into the worker's game folder for the playing
and removes it afterwards.
"""
import argparse
import re
import shutil
import sys
from pathlib import Path

import tareplay
from tareplay import Log, Worker, arm_dump, tacli, wait_dumps, last, LAUNCH_TIMEOUT

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("recording", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--dll", type=Path, default=TREE / "tagpu" / "ddraw" / "ddraw.dll")
    ap.add_argument("--at", type=int, action="append", default=[])
    ap.add_argument("--perspective", type=int, default=None)
    ap.add_argument("--to-end", action="store_true", help="after the dumps, play to the end")
    ap.add_argument("--content", type=Path, action="append", default=[])
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    rec = a.out / a.recording.name
    if rec.resolve() != a.recording.resolve():
        shutil.copy2(a.recording, rec)
    w = Worker(a.out)
    ok = False
    try:
        w.place({}, a.dll)
        for f in a.content:
            shutil.copy2(f, w.gamedir / f.name)
        log = Log(w)
        argv = ["replay", "play", w.name, rec, "--defaults", "--restart", "--keep-dll"]
        if a.perspective is not None:
            argv += ["--perspective", a.perspective]
        rc, o = tacli(*argv, timeout=LAUNCH_TIMEOUT)
        print(o.strip())
        if rc:
            raise tareplay.CouldNotRun(f"replay play: {last(o)}")
        names = []
        for T in sorted(a.at):
            arm_dump(w, log, f"p{T}", T)
            names.append(f"p{T}")
        if names:
            wait_dumps(log, names, 900)
        ended = True
        if a.to_end or not names:
            got = log.wait(r"replay: .* (played to its end|cannot be read on|not playing|stops)",
                           1800)
            print(got.strip() if got else "no end in 1800 s")
            ended = bool(got) and "played to its end" in got
        # the level's teardown logs the playing's counts (counts_log)
        w.quit()
        w.collect(a.out)
        ok = ended
    except tareplay.CouldNotRun as e:
        print(f"could not run: {e}")
        try:
            w.collect(a.out)
        except Exception:
            pass
    finally:
        w.stop(hard=True)
        for f in a.content:
            (w.gamedir / f.name).unlink(missing_ok=True)
        w.close()
    text = (a.out / "tagpu.log").read_text(errors="replace") if (a.out / "tagpu.log").exists() else ""
    for line in text.splitlines():
        if re.search(r"replay: .*(playing|stops|owners:|not playing|refused on puppets|"
                     r"events played)", line):
            print(line)
    return 0 if ok and " stops: " not in text else 1


if __name__ == "__main__":
    sys.exit(main())
