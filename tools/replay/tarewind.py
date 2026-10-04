"""Live rewind's L/P/Q checks; uses tareplay's owned instances and comparison policy."""
import json
import re
import time


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
    then play the original across the return-to-now boundary. Every dump is engine-timed."""
    out.mkdir(parents=True, exist_ok=True)
    results = []
    metadata = {}
    fix = fx["fix"]
    steps = sorted(fix.get("steps", []), key=lambda s: s[0])
    live = out / "L"
    log = t.Log(w)

    def check(label, ok, detail):
        results.append(("rewind " + label, t.PASS if ok else t.FAIL, detail))
        t.say(*results[-1])

    def compare(label, *args):
        rc, detail = t.tarec_run(*args)
        results.append(("rewind " + label, t.PASS if rc == 0 else t.FAIL if rc == 1 else t.UNRUN,
                        detail.strip()))
        t.say(*results[-1])

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
        deadline = time.monotonic() + 120 + 4 * max(0, G - w.gametime()) / 30
        while True:
            g = w.gametime()
            while steps and steps[0][0] <= g:
                _, kind, arg = steps.pop(0)
                t.step(w, kind, arg)
            if g >= G:
                break
            log.read()
            if judge:
                judge()
            if time.monotonic() > deadline:
                raise t.Failed(f"rewind: did not reach GameTime {G}")
            t.nap(0.5)
        if judge:
            judge()

    def objective_armed():
        objective = t.peek(w, "*0x511DE8+0x391ED:4")
        if not objective:
            raise t.CouldNotRun("rewind: missing objective")
        return t.peek(w, f"0x{objective + 0x88:X}:4")

    def pause_probe(label):
        g = w.gametime()
        rc, text = t.tacli("keys", w.name, "pause")
        if rc:
            raise t.CouldNotRun(text)
        t.nap(1)
        check(label, w.gametime() == g, f"Pause key at GameTime {g}")

    def judge_span(label, ticks, seconds):
        tc = t.compat()
        display = int(w.xvfb.display[1:])
        boxes, seen = [], set()
        begin = time.monotonic()
        wall_start = time.time()
        G = w.gametime()
        made = None
        log.read()
        for line in reversed(log.lines):
            m = re.search(r"units (\d+) killed, (\d+) made, (\d+) refused", line)
            if m:
                made = int(m[2])
                break
        alive = None
        error = gone = False

        def poll():
            nonlocal error, gone, alive
            tc.new_boxes(display, seen, boxes, wall_start)
            error |= (w.gamedir / "ErrorLog.txt").exists()
            gone |= not any(tc.main_window(*x) for x in tc.x_windows(display))
            if alive is None and time.monotonic() - begin >= 4:
                alive = tc.alive_seen(w.gamedir)
            if gone:
                raise t.Failed(f"rewind {label}: game window disappeared")

        until(G + ticks, poll)
        while time.monotonic() - begin < seconds:
            poll()
            t.nap(0.5)
        check(label, not boxes and not error and not gone and alive is not None and
              made is not None and alive >= .9 * made,
              f"{len(boxes)} boxes, errorlog={error}, gone={gone}, {alive} seen / {made} made; "
              f"{time.monotonic() - begin:.1f} s")

    try:
        w.place(fix, fx["dll"])
        (w.gamedir / "ErrorLog.txt").unlink(missing_ok=True)
        los = [] if fix.get("los") == "file" else ["--los", fix.get("los", 0)]
        rc, text = t.tacli("scenario", "load", w.name, t.SCENARIOS / fix["load"], "--defaults",
                           *los, "--restart", "--keep-dll", "--unit-limit",
                           fix.get("unit_limit", t.UNIT_LIMIT), timeout=t.LAUNCH_TIMEOUT)
        if rc:
            raise t.CouldNotRun(text)
        until(600)
        log.read()
        start = len(log.lines)
        opened = command("open", dump="open")
        G0 = opened["dump_at"]
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
        until(1500)
        t.tacli("keys", w.name, "pause")
        t.nap(1)
        G1 = w.gametime()
        # The engine creates this unit after the last radar scan. Returning to now
        # must retain that scan's contact bits, rather than reveal the new unit early.
        t.step(w, "apply", fix["rewind_paused"])
        opened = command("open", dump="pre")
        check("pre frame", opened["dump_at"] == G1, f"GameTime {G1}")
        T1 = (G1 - 601) // 30 * 30 + 1
        t.arm_dump(w, log, "b", G1 + 30)
        t.arm_dump(w, log, "c", G1 + 600)
        converted = command("seek", T1, dump="r")
        pause_probe("past pause guard")
        command("seek", T1 + 300)
        returned = command("now", dump="post")
        check("return clock", returned["dump_at"] == G1, f"GameTime {G1}")
        check("return objective", objective_armed() == 0, "saved disabled switch retained")
        arm_ai(t, w, log, "now", G1 + 1, G1 + 1801)
        t.tacli("keys", w.name, "pause")
        judge_span("now judge", 1800, 60)
        until(G1 + 1801)
        until(max(3000, G1 + 1800))
        opened = command("open", dump="fork-pre")
        G2 = opened["dump_at"]
        T2 = (G2 - 601) // 30 * 30 + 1
        command("seek", T2)
        forked = command("fork", dump="fork")
        check("fork objective", objective_armed() == 1, "takeover arms objectives")
        t.tacli("keys", w.name, "pause")
        t.nap(1)
        AF = w.gametime() + 1
        arm_ai(t, w, log, "fork", AF, AF + 1800)
        t.tacli("keys", w.name, "pause")
        judge_span("fork judge", 1800, 60)
        until(AF + 1800)
        for label, data in (("conversion", converted), ("now", returned), ("fork", forked)):
            match = re.search(r"in (\d+) ms", data["detail"])
            ms = int(match[1]) if match else None
            check(label + " time", ms is not None and (label == "conversion" or ms < 2000),
                  f"{ms} ms")
        metadata.update(G0=G0, G1=G1, G2=G2, T1=T1, T2=T2, ai_fork=AF)
        if fx["cover"]:
            t.cover_ask(w, log, "rewind-L")
        log.read()
        start = len(log.lines)
        t.step(w, "apply", "replay-end.json")
        if not log.wait(r"recorder: writer: .* closed", 120, start):
            raise t.CouldNotRun("rewind: fork writer did not close")
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
        segs, stop = t.tarec.file_segments(rec)
        check("segments", not stop and len(segs) == 2 and segs[0]["end_reason"] == 13 and
              segs[0]["last"] == G2 and segs[1]["parent"] == 0 and segs[1]["fork_at"] == T2,
              f"original ends forked at {segs[0]['last']}; fork at {segs[1]['fork_at']}")
        # Capture times must appear exactly once, including the two sides of the suspension.
        h, blocks, _ = t.tarec.load(rec)
        ticks = []

        def visit(world, kind, body):
            if kind == t.tarec.RK_TICK:
                ticks.append(world.tick)
        t.tarec.stream_to(h, blocks, on_record=visit)
        check("contiguous original", ticks == list(range(ticks[0], G2 + 1)) and
              ticks.count(G1) == ticks.count(G1 + 1) == 1,
              f"{len(ticks)} iterations, {ticks[0]}..{ticks[-1]}; boundary {G1}/{G1 + 1}")
        for name, tl, start in (("now", 0, G1 + 1), ("fork", 1, AF)):
            ai, made, fraction, count = t.tk_ai(rec, tl, start, start + 1799)
            ok, detail = ai_activity(log.lines, name, start, start + 1800, ai)
            check(name + " AI orders", ok, detail)
            detail = (f"{fraction} of {count} mobile units moved; "
                      f"construction observed for seats {sorted(made)} (reported only)")
            if fraction is None:
                results.append(("rewind " + name + " AI movement", t.UNRUN, detail))
                t.say(*results[-1])
            else:
                check(name + " AI movement", fraction >= .25, detail)

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
        if not log.wait(r"recorder: frame dump q at GameTime", 60, start):
            raise t.Failed("fresh seek: no atomic frame dump")
        if fx["cover"]:
            t.cover_ask(w, log, "rewind-Q")
        w.collect(q)
        w.stop()
        compare("fresh seek", "compare-dumps", rec, live / "dump-r.bin", q / "dump-q.bin")

        p = out / "P"
        playback = {**fx, "viewed": header["options"]["viewed"]}
        t.play(w, playback, rec, p, "rewind-P", [("a", G0 + 30), ("b", G1 + 30),
                                                ("c", G1 + 600)], ["a", "b", "c"])
        for name in ("b", "c"):
            compare(f"P {name}", "compare", "--puppet", rec, p / f"dump-{name}.bin")
        compare("counts across now", "compare-counts", rec, live / "dump-a.bin",
                live / "dump-c.bin", p / "dump-a.bin", p / "dump-c.bin", "--rewind")
        return results
    finally:
        (out / "rewind.json").write_text(json.dumps({**metadata, "results": results}, indent=2))
