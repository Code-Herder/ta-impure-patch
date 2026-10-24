#!/usr/bin/env python3
"""Play a saved multiplayer (mode 3) recording on a puppet host and dump it.

    tools/replay/recmp_play_test.py RECORDING --out DIR [--dll PATH] [--at T ...] [--to-end] [--perspective N]
                                    [--content FILE ...] [--roundtrip T ...] [--seek A:T ...]
    tools/replay/recmp_play_test.py RECORDING --out DIR --fresh T [--dll PATH] [--content FILE ...]
    tools/replay/recmp_play_test.py RECORDING --out DIR --takeover T:P [--from A] [--dll PATH] [--content FILE ...]

One tareplay worker (its own tacli instance on its own Xvfb) plays RECORDING through
`tacli replay play` at the standard internal speed 20, launched held at its first step until its
dumps and round trips are armed (tareplay.py, THE LAUNCH'S HOLD), so no GameTime it arms can pass
before the arm's answer; takes `dump p<T>` at each --at T, plays to
the end with --to-end or when no dump is asked, exits the game so that its teardown logs the
playing's counts, and keeps the dumps and the run's log in DIR. Exit 0 when
the playing started, every dump was taken, the log names no stop and, when it plays to the end,
it reached it; 1 otherwise. The dumps are
compared separately: this only drives the game. --roundtrip T arms the playing's round trip at T
(dump A, the tables poisoned, a seek to T, dump B: rt<T>-a and rt<T>-b, frame dumps compared with
`tarec.py compare-dumps FILE A B --seek`, which checks the sort buckets' chains of a multiplayer
file instead of comparing their order; with `--timeline N` for a round trip in timeline N's
segments). --seek A:T, when the playing reaches GameTime A (its dump pA), seeks
to T in the same process, dumping s<T> at T and s<T+120> 120 ticks on (when the recording
reaches it; at speed 20 a nearer dump cannot be armed in time). --fresh T instead
starts a fresh process at T (`tacli replay seek`), dump q<T>. --takeover T:P takes the recording
over as player P in a fresh process (`tacli replay takeover`) at R: T itself in a multiplayer
segment, which holds nothing, else the refresh at or before T (tarec.takeover_at; the takeover's
line names it, its `at`), dumps the live game at
k<R+120>, then requires the AI seats to play: tareplay's takeover check, fresh AI orders reaching a handler
for every AI seat and a quarter of their mobile units moving in the TAKEOVER_SPAN ticks after a
pause; quits after it and keeps the file with its new timeline as DIR/fork-<RECORDING>. With
--from A the recording is played first, and the running playback is taken over after its dump pA. --timeline N plays
or seeks timeline N's chain. --hide-chat hides the chat once the playing starts. --content copies a game archive the recording
was made with (tools/reproduce_fixture.py's, say) into the worker's game folder for the playing
and removes it afterwards.
"""
import argparse
import json
import re
import shutil
import sys
import time
from pathlib import Path

import devpy
import tarec
import tareplay
import tarewind
from tareplay import (Log, Worker, arm, arm_dump, held_start, resume, tacli, wait_dumps, last,
                      LAUNCH_TIMEOUT)

HERE = Path(__file__).resolve().parent


AFTER = 120             # the dump after a seek, this many ticks on


class Done(Exception):
    """the fresh-process seek is complete"""
TREE = HERE.parents[1]


def main():
    devpy.ensure()
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
    ap.add_argument("--takeover", default=None, help="T:P")
    ap.add_argument("--from", dest="from_", type=int, default=None,
                    help="with --takeover: play to this GameTime, then take the playback over")
    ap.add_argument("--timeline", type=int, default=None)
    ap.add_argument("--hide-chat", action="store_true", help="`chat hide` once the playing starts")
    a = ap.parse_args()
    tareplay.runlock.hold(TREE, "recmp_play_test " + " ".join(sys.argv[1:]))
    a.out.mkdir(parents=True, exist_ok=True)
    rec = a.out / a.recording.name
    if rec.resolve() != a.recording.resolve():
        shutil.copy2(a.recording, rec)
    w = Worker(a.out)
    # held, the launches that play from the start; a takeover or a seek in a fresh process arms
    # its one dump in its own request
    w.held = a.fresh is None and (a.takeover is None or a.from_ is not None)
    ok = False
    try:
        w.place({}, a.dll)
        for f in a.content:
            shutil.copy2(f, w.gamedir / f.name)
        log = Log(w)
        tl = ["--timeline", a.timeline] if a.timeline is not None else []
        if a.takeover is not None:
            T, P = map(int, a.takeover.split(":"))
            # the takeover starts at T, or the refresh at or before it (tarec.takeover_at), which
            # its line names; it is asked for at that start, and the dump, armed before it, is
            # named from it
            try:
                R = tarec.takeover_at(str(rec), a.timeline or 0, T)
            except tarec.Unreadable as e:
                raise tareplay.CouldNotRun(f"the takeover's start for {T}: {e}")
            if R != T:
                print(f"the takeover asked for at {T} starts at {R}")
            if a.from_ is None:
                src = [rec, *tl, "--defaults", "--restart", "--keep-dll"]
            else:
                # the running playback's own composition, carried to A, is what the takeover
                # replaces with T's
                rc, o = tacli("replay", "play", w.name, rec, *tl, "--defaults", "--restart",
                              "--keep-dll", timeout=LAUNCH_TIMEOUT)
                print(o.strip())
                if rc:
                    raise tareplay.CouldNotRun(f"replay play: {last(o)}")
                held_start(w, log)
                arm_dump(w, log, f"p{a.from_}", a.from_)
                resume(w, log)
                wait_dumps(log, [f"p{a.from_}"], 900)
                src = []
            rc, o = tacli("replay", "takeover", w.name, *src, "--at", R, "--player", P,
                          "--dump", f"k{R + AFTER}", "--dump-at", R + AFTER, "--timeout", 300,
                          "--json", timeout=LAUNCH_TIMEOUT + 300)
            print(o.strip())
            try:
                got = json.loads(o[o.index("{"):])
            except ValueError:
                raise tareplay.CouldNotRun(f"replay takeover: {last(o)}")
            if rc or not got.get("taken") or "fork" not in got:
                raise tareplay.CouldNotRun(f"replay takeover: {last(o)}")
            if got.get("at") != R:
                raise tareplay.CouldNotRun(f"replay takeover: taken over at {got.get('at')}, "
                                           f"not at the refresh {R}")
            tk, got_fork = got, got["fork"]
            wait_dumps(log, [f"k{R + AFTER}"], 600)
            # the window opens after a pause the engine acknowledged, so it lies wholly ahead
            # (tareplay.takeover_k)
            rc, o = tacli("keys", w.name, "pause")
            if rc:
                raise tareplay.CouldNotRun(f"pause: {last(o)}")
            deadline = time.monotonic() + 30
            while not tareplay.peek(w, "*0x511DE8+0x38A51:2") & 1:
                if time.monotonic() > deadline:
                    raise tareplay.CouldNotRun("the engine did not acknowledge pause")
                tareplay.nap(.1)
            start = w.gametime() + 1
            stop = start + tareplay.TAKEOVER_SPAN
            tarewind.arm_ai(tareplay, w, log, "takeover", start, stop)
            rc, o = tacli("keys", w.name, "pause")
            if rc:
                raise tareplay.CouldNotRun(f"resume: {last(o)}")
            got = log.wait(r"recorder: AI activity takeover done", 1200)
            if not got:
                raise tareplay.CouldNotRun("no AI activity line in 1200 s")
            # the level's teardown ends the timeline (tagpu_rec_level_end)
            w.quit()
            w.collect(a.out)
            fork = a.out / f"fork-{a.recording.name}"
            shutil.copy2(got_fork, fork)
            seats = {i for i in range(10) if f"{i}=ai" in tk.get("seats", "")}
            active, detail = tarewind.ai_activity(
                (a.out / "tagpu.log").read_text(errors="replace").splitlines(), "takeover",
                start, stop, seats)
            ai, made, frac, n = tareplay.tk_ai(fork, tk["timeline"], start, stop - 1)
            print(f"takeover ai: {detail}; {frac or 0:.0%} of {n} mobile units moved; "
                  f"construction observed for seats {sorted(made)}")
            ok = active and frac is not None and frac >= .25
            raise Done
        if a.fresh is not None:
            rc, o = tacli("replay", "seek", w.name, a.fresh, "--recording", rec, *tl, "--dump",
                          f"q{a.fresh}", "--defaults", "--restart", "--keep-dll", "--timeout", 300,
                          timeout=LAUNCH_TIMEOUT + 300)
            print(o.strip())
            if rc:
                raise tareplay.CouldNotRun(f"replay seek {a.fresh}: {last(o)}")
            w.quit()
            w.collect(a.out)
            ok = True
            raise Done
        argv = ["replay", "play", w.name, rec, *tl, "--defaults", "--restart", "--keep-dll"]
        if a.perspective is not None:
            argv += ["--perspective", a.perspective]
        rc, o = tacli(*argv, timeout=LAUNCH_TIMEOUT)
        print(o.strip())
        if rc:
            raise tareplay.CouldNotRun(f"replay play: {last(o)}")
        held_start(w, log)
        if a.hide_chat:
            n = len(log.lines)
            w.trigger("chat hide")
            if not log.wait(r"replay: chat hidden", 60, n):
                raise tareplay.CouldNotRun("chat hide: no answer in 60 s")
        names = []
        seeks = [tuple(map(int, x.split(":"))) for x in a.seek]
        last_tick = tarec.chain(tarec.segments(tarec.blocks(rec)[0]), a.timeline or 0)[-1]["last"]
        for T in sorted(set(a.at) | {A for A, _ in seeks}):
            arm_dump(w, log, f"p{T}", T)
            names.append(f"p{T}")
        for T in a.roundtrip:
            arm(w, log, f"roundtrip rt{T} at {T}", rf"round trip rt{T} armed for GameTime {T}",
                rf"round trip rt{T}( at {T})? refused", f"roundtrip rt{T} at {T}")
        resume(w, log)
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
                     r"events played|sought from|composed the owners|round trip|refused|taken over|"
                     r"timeline)|recorder: .*timeline", line):
            print(line)
    return 0 if ok and " stops: " not in text else 1


if __name__ == "__main__":
    sys.exit(main())
