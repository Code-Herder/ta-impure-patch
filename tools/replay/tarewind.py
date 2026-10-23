"""Live rewind's L/P/Q checks; uses tareplay's owned instances and comparison policy."""
import json
import re
import time

JUDGE_SPAN = 3600   # a judge watches this many ticks of play: 60 s at the harness's speed 20


def ai_activity(lines, name, start, stop, seats):
    """Require fresh AI orders that reached a normal handler in the armed window."""
    prefix = rf"recorder: AI activity {re.escape(name)} "
    rows, done, malformed, completions = {}, False, False, 0
    for line in lines:
        m = re.search(prefix + r"seat (\d+) orders (\d+) executed (\d+)", line)
        if m:
            seat, orders, executed = map(int, m.groups())
            malformed |= seat in rows or seat >= 10 or executed > orders
            rows[seat] = (orders, executed)
        m = re.search(prefix + r"done \[(\d+),(\d+)\) bad (\d+)", line)
        if m:
            completions += 1
            malformed |= completions > 1
            done = tuple(map(int, m.groups())) == (start, stop, 0)
    missing = sorted(s for s in seats if not rows.get(s, (0, 0))[1])
    complete = done and not malformed and set(rows) == set(range(10)) and bool(seats)
    detail = (f"[{start},{stop}) orders/executed " +
              ", ".join(f"{s}={rows.get(s, (0, 0))[0]}/{rows.get(s, (0, 0))[1]}"
                        for s in sorted(seats)) +
              f"; missing {missing}; complete={complete}")
    return complete and not missing, detail


def arm_ai(t, w, log, name, start, stop):
    t.arm(w, log, f"ai-activity {name} from {start} until {stop}",
          rf"AI activity {name} armed \[{start},{stop}\)",
          rf"AI activity {name} refused", "AI activity " + name)


def run(t, w, fx, out):
    """Record a battle across two rewinds, compare its restored frame and a fresh seek,
    then play the original across the return-to-now boundary. Every dump is engine-timed.
    The checks of the recording run on the offline pool while Q and P play (t.Ordered): the
    lane's results and its first failure are the ones it would have met in order. Each reads
    only what no later step writes: L's recording and dumps, collected before Q and P start, and
    Q's and P's own dumps."""
    out.mkdir(parents=True, exist_ok=True)
    results = t.Ordered()
    metadata = {}
    fix = fx["fix"]
    steps = sorted(fix.get("steps", []), key=lambda s: s[0])
    live = out / "L"
    log = t.Log(w)

    def check(label, ok, detail):
        results.add(outcome(t, label, ok, detail))

    def compared(label, *args):
        rc, detail = t.tarec_run(*args)
        got = ("rewind " + label, t.PASS if rc == 0 else t.FAIL if rc == 1 else t.UNRUN,
               detail.strip())
        t.say(*got)
        return [got]

    def compare(label, *args):
        results.later(compared, label, *args)

    def command(action, *args, dump=None):
        argv = ["rewind", w.name, action, *args, "--json"]
        if dump:
            argv += ["--dump", dump]
        rc, text = t.tacli(*argv)
        if rc:
            raise t.Failed(f"rewind {action}: {text.strip()}")
        data = json.loads(text[text.index("{"):])
        t.say(data["detail"])
        return data

    def until(G, judge=None):
        """held at GameTime G, the fixture's steps up to it run at theirs (tareplay's run_to)"""
        return t.run_to(w, log, G, steps, judge)

    def objective_armed():
        objective = t.peek(w, "*0x511DE8+0x391ED:4")
        if not objective:
            raise t.CouldNotRun("rewind: missing objective")
        return t.peek(w, f"0x{objective + 0x88:X}:4")

    def pause_probe(label):
        """The Pause key while rewind owns the level: `tacli keys` returns once the engine has
        consumed it, and of two peeks after it the second answers at a later flip than the first,
        so a frame's pause test has run between the key and that peek."""
        g = w.gametime()
        rc, text = t.tacli("keys", w.name, "pause", timeout=t.INPUT_TIMEOUT)
        if rc:
            raise t.CouldNotRun(text)
        g1, g2 = w.gametime(), w.gametime()
        check(label, g1 == g2 == g, f"Pause key at GameTime {g}: {g1}, then {g2}")

    def judge_span(label, ticks):
        """From the GameTime the level is held at, `ticks` of play watched -- the window, its
        boxes and ErrorLog.txt -- and the units Impure draws JUDGE_TICKS on against what the last
        rebuild made; the level is held at the span's end."""
        tc = t.compat()
        display = int(w.xvfb.display[1:])
        boxes, seen = [], set()
        wall_start = time.time()
        G = log.at
        made = None
        log.read()
        for line in reversed(log.lines):
            m = re.search(r"units (\d+) killed, (\d+) made, (\d+) refused", line)
            if m:
                made = int(m[2])
                break
        error = gone = False

        def poll():
            nonlocal error, gone
            tc.new_boxes(display, seen, boxes, wall_start)
            error |= (w.gamedir / "ErrorLog.txt").exists()
            gone |= not any(tc.main_window(*x) for x in tc.x_windows(display))
            if gone:
                raise t.Failed(f"rewind {label}: game window disappeared")

        until(G + t.JUDGE_TICKS, poll)
        n = len(log.lines)
        got = log.wait(r"^units: alive=(\d+)", t.ACK_S, n)
        alive = int(re.search(r"alive=(\d+)", got).group(1)) if got else None
        until(G + ticks, poll)
        poll()
        check(label, not boxes and not error and not gone and alive is not None and
              made is not None and alive >= .9 * made,
              f"{len(boxes)} boxes, errorlog={error}, gone={gone}, {alive} seen at GameTime "
              f"{G + t.JUDGE_TICKS} / {made} made; GameTime {G} to {G + ticks}")

    try:
        with results:
            w.place(fix, fx["dll"])
            (w.gamedir / "ErrorLog.txt").unlink(missing_ok=True)
            los = [] if fix.get("los") == "file" else ["--los", fix.get("los", 0)]
            rc, text = t.tacli("scenario", "load", w.name, t.SCENARIOS / fix["load"], "--defaults",
                               *los, "--restart", "--keep-dll", "--unit-limit",
                               fix.get("unit_limit", t.UNIT_LIMIT), timeout=t.LAUNCH_TIMEOUT)
            if rc:
                raise t.CouldNotRun(text)
            t.held_start(w, log)
            # the holds G0 and G1 are 1 (mod 30), the radar pass's step, as every dump of the
            # harness is (O14): the dumps below are G0 + 30, G1 + 30 and G1 + 600
            until(601)
            log.read()
            start = len(log.lines)
            # the HUD's rewind calls (tagpu_replay.h, LIVE REWIND) as the trigger reaches them: a
            # held live recorded skirmish can open one, the open's state and outcome, and a
            # refused verb's outcome, the HUD's toast
            idle = t.hud_state(w, log)["rewind"]
            check("hud state idle", idle["state"] == "idle" and idle["available"], f"{idle}")
            opened = command("open", dump="open")
            G0 = opened["dump_at"]
            st = t.hud_state(w, log)
            check("hud state open", st["rewind"]["state"] == "open" and st["rewind"]["G"] == G0 and
                  st["result"]["kind"] == "rewind" and st["result"]["op"] == 0 and
                  st["result"]["ok"] and st["result"]["why"].startswith("opened at GameTime "),
                  f"{st['rewind']}; {st['result']}")
            n = len(log.lines)
            # GameTime 0 is outside every recording: the span's check refuses it whatever the
            # recording's start
            w.trigger("rewind seek 0")
            refused = log.wait(r"rewind: seek refused: iteration 0 is outside ", t.ACK_S, n)
            got = t.hud_state(w, log)["result"]
            check("hud state refused", bool(refused) and got["kind"] == "rewind" and
                  got["op"] == 1 and not got["ok"] and got["serial"] == st["result"]["serial"] + 1,
                  f"{refused.strip() if refused else 'no refusal line'}; {got}")
            t.arm_dump(w, log, "a", G0 + 30)
            pause_probe("open pause guard")
            # Do the objective setup under the existing pause: command latency must not
            # push the later AI comparison past its initial construction work. Kill is
            # level 4; seat 9 is empty but still disarms objectives. Restore the debug switch.
            cheats = t.peek(w, "*0x511DE8+0x37F2F:2") & 2
            rc, text = t.tacli("switches", w.name, "cheats=on")
            if rc:
                raise t.CouldNotRun(text)
            t.step(w, "type", "+kill 9")
            rc, text = t.tacli("switches", w.name, "cheats=on" if cheats else "cheats=off")
            if rc:
                raise t.CouldNotRun(text)
            check("objective disarmed", objective_armed() == 0, "engine +kill 9")
            command("close")
            log.read()
            check("open-close", not any("sought from GameTime" in ln for ln in log.lines[start:]),
                  "no rebuild between open and close")
            G1 = until(1501)
            # The engine creates this unit after the last radar scan. Returning to now
            # must retain that scan's contact bits, rather than reveal the new unit early.
            t.step(w, "apply", fix["rewind_paused"])
            opened = command("open", dump="pre")
            check("pre frame", opened["dump_at"] == G1, f"GameTime {G1}")
            T1 = (G1 - 601) // 30 * 30 + 1
            t.arm_dump(w, log, "b", G1 + 30)
            t.arm_dump(w, log, "c", G1 + 600)
            command("seek", T1, dump="r")
            pause_probe("past pause guard")
            command("seek", T1 + 300)
            returned = command("now", dump="post")
            check("return clock", returned["dump_at"] == G1, f"GameTime {G1}")
            check("return objective", objective_armed() == 0, "saved disabled switch retained")
            arm_ai(t, w, log, "now", G1 + 1, G1 + 1801)
            # the return to now puts back the pause bit the open saved, the hold's (rw_finish),
            # and the hold keeper keeps it
            t.adopt_held(w, log, "the return to now")
            judge_span("now judge", JUDGE_SPAN)
            until(G1 + 1801)
            until(max(3000, G1 + 1800))
            opened = command("open", dump="fork-pre")
            G2 = opened["dump_at"]
            T2 = (G2 - 601) // 30 * 30 + 1
            command("seek", T2)
            command("fork", dump="fork")
            check("fork objective", objective_armed() == 1, "takeover arms objectives")
            # the fork puts back the pause bit the open saved, the hold's (tagpu_replay.c,
            # rw_finish), and the hold keeper keeps it: the AI window and the judge start from
            # the held level
            t.adopt_held(w, log, "the fork")
            AF = log.at + 1
            arm_ai(t, w, log, "fork", AF, AF + 1800)
            judge_span("fork judge", JUDGE_SPAN)
            until(AF + 1800)
            metadata.update(G0=G0, G1=G1, G2=G2, T1=T1, T2=T2, ai_fork=AF)
            works = rewind_works(t, log.lines)
            if fx["cover"]:
                t.cover_ask(w, log, "rewind-L")
            log.read()
            start = len(log.lines)
            t.step(w, "apply", "replay-end.json")
            t.resume(w, log)
            log.wait_running(r"recorder: writer: .* closed", start)
            w.collect(live, recordings=True)
            w.stop()
            records = sorted(live.glob("*.imprec"))
            if len(records) != 1:
                raise t.Failed(f"rewind: {len(records)} recordings, expected original plus fork in one")
            rec = records[0]
            header = t.tarec.header_only(rec)
            compare("open-close continuity", "compare", rec, live / "dump-a.bin")
            compare("return frame", "compare-dumps", rec, live / "dump-pre.bin",
                    live / "dump-post.bin", "--takeover")
            for tl in (0, 1):
                compare(f"check timeline {tl}", "check", rec, "--timeline", tl)
            compare("fork", "compare-fork", rec, 1)
            results.later(work_checks, t, works, rec)
            segs, stop = t.tarec.file_segments(rec)
            # the fork starts at the refresh at or before T2 (tagpu_replay.c, tk_bound)
            R2 = t.tarec.refresh_at(str(rec), 0, T2)
            check("segments", not stop and len(segs) == 2 and segs[0]["end_reason"] == 13 and
                  segs[0]["last"] == G2 and segs[1]["parent"] == 0 and segs[1]["fork_at"] == R2,
                  f"original ends forked at {segs[0]['last']}; fork at {segs[1]['fork_at']}, the "
                  f"refresh at or before {T2}")
            results.later(contiguous, t, rec, G1, G2)
            for name, tl, start in (("now", 0, G1 + 1), ("fork", 1, AF)):
                results.later(ai_checks, t, rec, list(log.lines), name, tl, start)

            q = out / "Q"
            w.place(fix, fx["dll"])
            log = t.Log(w)
            rc, text = t.tacli("replay", "play", w.name, rec, "--defaults", "--restart", "--keep-dll",
                               "--perspective", header["options"]["viewed"], timeout=t.LAUNCH_TIMEOUT)
            if rc:
                raise t.CouldNotRun(text)
            log.read()
            start = len(log.lines)
            w.trigger(f"seek-frame {T1} dump q")
            if not log.wait(r"recorder: frame dump q at GameTime", t.ACK_S, start):
                raise t.Failed("fresh seek: no atomic frame dump")
            if fx["cover"]:
                t.cover_ask(w, log, "rewind-Q")
            w.collect(q)
            w.stop()
            compare("fresh seek", "compare-dumps", rec, live / "dump-r.bin", q / "dump-q.bin")

            # Q1: a fork's playing seeks within its own chain, past the original's last (F1)
            q1 = out / "Q1"
            T = G2 + 300
            if not segs[1]["last"] or not G2 < T <= segs[1]["last"]:
                raise t.CouldNotRun(f"rewind Q1: GameTime {T} is not past {G2} in the fork, which "
                                    f"ends at {segs[1]['last']}")
            w.place(fix, fx["dll"])
            log = t.Log(w)
            rc, text = t.tacli("replay", "play", w.name, rec, "--timeline", 1, "--defaults",
                               "--restart", "--keep-dll", timeout=t.LAUNCH_TIMEOUT)
            if rc:
                raise t.CouldNotRun(text)
            log.read()
            start = len(log.lines)
            w.trigger(f"hud seek {T}")
            got = log.wait(r"replay: .*: at GameTime \d+, sought from|"
                           r"replay: .*seek to \d+ (from the HUD )?refused", t.ACK_S, start)
            check("fork span", bool(got) and f"at GameTime {T}, sought from" in got,
                  f"asked {T}, past the original's last {G2}: "
                  f"{got.strip() if got else 'no answer'}")
            if fx["cover"]:
                t.cover_ask(w, log, "rewind-Q1")
            w.collect(q1)
            w.stop()

            p = out / "P"
            playback = {**fx, "viewed": header["options"]["viewed"]}
            t.play(w, playback, rec, p, "rewind-P", [("a", G0 + 30), ("b", G1 + 30),
                                                    ("c", G1 + 600)], ["a", "b", "c"])
            for name in ("b", "c"):
                compare(f"P {name}", "compare", "--puppet", rec, p / f"dump-{name}.bin")
            compare("counts across now", "compare-counts", rec, live / "dump-a.bin",
                    live / "dump-c.bin", p / "dump-a.bin", p / "dump-c.bin", "--rewind")
            return results.results()
    finally:
        (out / "rewind.json").write_text(json.dumps({**metadata, "results": results.settled()},
                                                    indent=2))


def rewind_works(t, lines):
    """The work lines (tareplay.work_lines) of the rewind's seeks, by what each was: the first
    conversion's (`rewind seek` from the live level), the return to now's and the fork's -- each
    the last work line before the line that ends it."""
    works, last = {}, None
    for ln in lines:
        got = t.work_lines(ln)
        if got:
            last = got[0]
        elif last and re.search(r"rewind: seek \d+ complete", ln) and "conversion" not in works:
            works["conversion"] = last
        elif last and "rewind: returned to now at GameTime" in ln:
            works["now"] = last
        elif last and "rewind: forked at GameTime" in ln:
            works["fork"] = last
    return works


def work_checks(t, works, rec):
    """The return to now's and the fork's work, W1 to W3 (tareplay.work_verdict) against the
    recording's original timeline, which both read: the return to now to G-1 of the live file,
    the fork to its T. The conversion's CPU time is reported only."""
    res = []
    for label in ("now", "fork"):
        w = works.get(label)
        if w is None:
            res.append(("rewind " + label + " work", t.UNRUN, "no work line"))
            continue
        try:
            bad = t.work_verdict(w, rec, 0, True)
        except t.tarec.Unreadable as e:
            res.append(("rewind " + label + " work", t.UNRUN, str(e)))
            continue
        res.append(outcome(t, label + " work", not bad, "; ".join(bad) or t.work_summary(w)))
    w = works.get("conversion")
    res.append(("rewind conversion work", t.PASS if w else t.UNRUN,
                f"cpu {w['C']} ms ({w['W']} ms wall), reported only" if w else "no work line"))
    return res


def outcome(t, label, ok, detail):
    got = ("rewind " + label, t.PASS if ok else t.FAIL, detail)
    t.say(*got)
    return got


def contiguous(t, rec, G1, G2):
    """Capture times appear exactly once, including the two sides of the suspension."""
    h, blocks, _ = t.tarec.load(rec)
    ticks = []

    def visit(world, kind, body):
        if kind == t.tarec.RK_TICK:
            ticks.append(world.tick)
    t.tarec.stream_to(h, blocks, on_record=visit)
    return [outcome(t, "contiguous original", ticks == list(range(ticks[0], G2 + 1)) and
                    ticks.count(G1) == ticks.count(G1 + 1) == 1,
                    f"{len(ticks)} iterations, {ticks[0]}..{ticks[-1]}; boundary {G1}/{G1 + 1}")]


def ai_checks(t, rec, lines, name, tl, start):
    """Timeline `tl`'s AI seats ordered and moved over the 1800 ticks from `start`."""
    ai, made, fraction, count = t.tk_ai(rec, tl, start, start + 1799)
    ok, detail = ai_activity(lines, name, start, start + 1800, ai)
    res = [outcome(t, name + " AI orders", ok, detail)]
    detail = (f"{fraction} of {count} mobile units moved; "
              f"construction observed for seats {sorted(made)} (reported only)")
    if fraction is None:
        res.append(("rewind " + name + " AI movement", t.UNRUN, detail))
        t.say(*res[-1])
    else:
        res.append(outcome(t, name + " AI movement", fraction >= .25, detail))
    return res
