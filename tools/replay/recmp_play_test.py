#!/usr/bin/env python3
"""Play a saved multiplayer (mode 3) recording on a puppet host and dump it.

    tools/replay/recmp_play_test.py RECORDING --out DIR [--dll PATH] [--at T ...] [--to-end] [--perspective N]
                                    [--content FILE ...] [--roundtrip T ...] [--seek A:T ...]
    tools/replay/recmp_play_test.py RECORDING --out DIR --fresh T [--dll PATH] [--content FILE ...]

One tareplay worker (its own tacli instance on its own Xvfb) plays RECORDING through
`tacli replay play` at the standard internal speed 20, takes `dump p<T>` at each --at T, plays to
the end with --to-end or when no dump is asked, exits the game so that its teardown logs the
playing's counts, and keeps the dumps and the run's log in DIR. Exit 0 when
the playing started, every dump was taken, the log names no stop and, when it plays to the end,
it reached it; 1 otherwise. The dumps are
compared separately: this only drives the game. --roundtrip T arms the playing's round trip at T
(dump A, the tables poisoned, a seek to T, dump B: rt<T>-a and rt<T>-b, frame dumps compared with
`tarec.py compare-dumps`). --seek A:T, when the playing reaches GameTime A (its dump pA), seeks
to T in the same process, dumping s<T> at T and s<T+120> 120 ticks on (when the recording
reaches it; at speed 20 a nearer dump cannot be armed in time). --fresh T instead
starts a fresh process at T (`tacli replay seek`), dump q<T>. --content copies a game archive the recording
was made with (tools/reproduce_fixture.py's, say) into the worker's game folder for the playing
and removes it afterwards.
"""
import argparse
import re
import shutil
import sys
from pathlib import Path

import tarec
import tareplay
from tareplay import Log, Worker, arm, arm_dump, tacli, wait_dumps, last, LAUNCH_TIMEOUT

HERE = Path(__file__).resolve().parent


AFTER = 120             # the dump after a seek, this many ticks on


class Done(Exception):
    """the fresh-process seek is complete"""
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
    ap.add_argument("--roundtrip", type=int, action="append", default=[])
    ap.add_argument("--seek", action="append", default=[], help="A:T")
    ap.add_argument("--fresh", type=int, default=None)
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
        if a.fresh is not None:
            rc, o = tacli("replay", "seek", w.name, a.fresh, "--recording", rec, "--dump",
                          f"q{a.fresh}", "--defaults", "--restart", "--keep-dll", "--timeout", 300,
                          timeout=LAUNCH_TIMEOUT + 300)
            print(o.strip())
            if rc:
                raise tareplay.CouldNotRun(f"replay seek {a.fresh}: {last(o)}")
            w.quit()
            w.collect(a.out)
            ok = True
            raise Done
        argv = ["replay", "play", w.name, rec, "--defaults", "--restart", "--keep-dll"]
        if a.perspective is not None:
            argv += ["--perspective", a.perspective]
        rc, o = tacli(*argv, timeout=LAUNCH_TIMEOUT)
        print(o.strip())
        if rc:
            raise tareplay.CouldNotRun(f"replay play: {last(o)}")
        names = []
        seeks = [tuple(map(int, x.split(":"))) for x in a.seek]
        last_tick = tarec.segments(tarec.blocks(rec)[0])[0]["last"]
        for T in sorted(set(a.at) | {A for A, _ in seeks}):
            arm_dump(w, log, f"p{T}", T)
            names.append(f"p{T}")
        for T in a.roundtrip:
            arm(w, log, f"roundtrip rt{T} at {T}", rf"round trip rt{T} armed for GameTime {T}",
                rf"round trip rt{T}( at {T})? refused", f"roundtrip rt{T} at {T}")
        for A, T in sorted(seeks):
            wait_dumps(log, [f"p{A}"], 900)
            n = len(log.lines)
            w.trigger(f"seek {T} dump s{T}")
            got = log.wait(rf"replay: .*(at GameTime {T}, sought from|seek to {T} refused)", 300, n)
            print(got.strip() if got else f"seek to {T}: no answer in 300 s")
            if not got or f"seek to {T} refused" in got:
                raise tareplay.CouldNotRun(f"seek to {T}")
            wait_dumps(log, [f"s{T}"], 300)
            if T + AFTER < last_tick:
                arm_dump(w, log, f"s{T + AFTER}", T + AFTER)
                wait_dumps(log, [f"s{T + AFTER}"], 300)
        for T in a.roundtrip:
            got = log.wait(rf"round trip rt{T} at {T}(: | not run)", 900)
            print(got.strip() if got else f"round trip rt{T}: no line in 900 s")
            if not got or " not run" in got or "seek refused" in got:
                raise tareplay.CouldNotRun(f"round trip rt{T}")
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
    except Done:
        pass
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
                     r"events played|sought from|composed the owners|round trip|refused)", line):
            print(line)
    return 0 if ok and " stops: " not in text else 1


if __name__ == "__main__":
    sys.exit(main())
