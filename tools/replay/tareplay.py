#!/usr/bin/env python3
"""tareplay -- the demo recorder's replay harness: each fixture recorded live, then checked on
puppets (research/notes/tadr-port/demo-recorder-completeness.md, *The harness*).

    tools/replay/tareplay.py run FIXTURE... [-j N] [--dll PATH] [--stock-dll PATH] [--keep]
                                            [--out DIR]
    tools/replay/tareplay.py list

FIXTURE is a name in tools/replay/fixtures.json, or `all`. For each fixture four GameTimes are
drawn, each T = 1 (mod 30) -- the radar pass's step, so a seek's contact bits agree at once (O14)
-- between 301 and 0.8 x its end, from a generator seeded by the fixture's name and HEAD, and
printed: T1 the first, T2..T4 the round trips, Tlast = T4. Four processes, each a game of its own:

    L  live      the fixture recorded, dumps l<T1> and l<Tlast>
    P  playing   `replay play` in the recording's viewed player's perspective, dumps p<T1> and
                 p<Tlast>
    R  round     the same, dump r<T1>, `roundtrip rt<T> at <T>` for T2..T4 (frame dumps
                 rt<T>-a and rt<T>-b around a rebuild in place) and a dump rt<T>c at T + 300
    Q  seek      `replay seek T1 --recording` in a fresh process, dump q<T1>

and every check fails by name: the recording's end block and `tarec check`; L's dumps `tarec
compare`; P's, R's and Q's capture dumps `tarec compare --puppet`; each round trip's line has
residue 0 and `tarec compare-dumps rt<T>-a rt<T>-b` is EQUAL. Passing dumps are deleted unless
--keep.

Each worker owns one tacli instance, tareplay-<k>, pinned to an Xvfb it reserves by -displayfd,
for the whole run; nothing is ever stopped by name or pattern, only by tacli or by the PID this
tool started. Exit 0: every check passed; 1: a named failure; 2: something could not run.

The output folder (default ~/.local/share/ta-replay/runs/<time>, or $TAREPLAY_OUT) is outside the
repository: its logs carry absolute paths.
"""
import argparse
import datetime
import hashlib
import json
import os
import queue
import random
import re
import select
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
TACLI = TREE / "tools" / "tacli"
TAREC = HERE / "tarec.py"
SCENARIOS = TREE / "scenarios"
END_SCENARIO = SCENARIOS / "replay-end.json"
sys.path.insert(0, str(TREE / "tools"))
import talog  # noqa: E402
sys.path.insert(0, str(HERE))
import tarec  # noqa: E402

PREFIX = "tareplay-"
UNIT_LIMIT = 500
GAMETIME = "*0x511DE8+0x38A47:4"
COB_STOP = "A unit script cannot continue safely"
TRIGGER = "tagpu_replay.trigger"
ROUND_TRIPS = 3
CONTINUE = 300                  # the dump after a round trip, this many ticks on
LAUNCH_TIMEOUT = 400


def main_checkout() -> Path:
    """tacli's instances live in the main checkout, whichever tree this tool runs from."""
    if os.environ.get("TACLI_ROOT"):
        return Path(os.environ["TACLI_ROOT"]).expanduser().resolve()
    try:
        common = subprocess.run(["git", "-C", str(TREE), "rev-parse", "--path-format=absolute",
                                 "--git-common-dir"], capture_output=True, text=True,
                                check=True).stdout.strip()
        return Path(common).parent
    except (subprocess.CalledProcessError, OSError):
        return TREE


INSTANCES = main_checkout() / "tagpu" / "instances"
PRINT = threading.Lock()


def say(*a):
    with PRINT:
        print(time.strftime("%H:%M:%S"), *a, flush=True)


class CouldNotRun(Exception):
    """The run could not reach the claim it checks: exit 2, not a failure of the change."""


class Failed(Exception):
    """A named failure of the change (exit 1)."""


# ---- Xvfb and the instance ------------------------------------------------------------------

def xvfb_reply(fd):
    answer = b""
    deadline = time.monotonic() + 10
    while len(answer) < 32 and b"\n" not in answer:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
            break
        chunk = os.read(fd, 32 - len(answer))
        if not chunk:
            break
        answer += chunk
    return answer


DISPLAYS = range(300, 400)       # the test servers' range, clear of a desktop's own displays
TAKEN = threading.Lock()


def start_xvfb():
    """A server owned only once its -displayfd pipe confirms the number it bound, in :300..:399,
    skipping any number with a socket or a lock: a socket alone is not an acknowledgement, and a
    lost race leaves the number to its winner."""
    with TAKEN:
        for n in DISPLAYS:
            if Path(f"/tmp/.X11-unix/X{n}").exists() or Path(f"/tmp/.X{n}-lock").exists():
                continue
            rfd, wfd = os.pipe()
            p = subprocess.Popen(["Xvfb", f":{n}", "-displayfd", str(wfd), "-screen", "0",
                                  "1280x1024x24", "-nolisten", "tcp"], pass_fds=(wfd,),
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 start_new_session=True)
            os.close(wfd)
            try:
                answer = xvfb_reply(rfd)
            finally:
                os.close(rfd)
            if re.fullmatch(rb"\d+\n", answer) and int(answer) == n and p.poll() is None:
                p.display = f":{n}"
                return p
            stop_xvfb(p)
    raise CouldNotRun("no free X display between :300 and :399")


def stop_xvfb(p):
    p.terminate()
    try:
        p.wait(10)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()


def tacli(*args, timeout=180):
    r = subprocess.run([sys.executable, str(TACLI), *map(str, args)], capture_output=True,
                       text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def last(text):
    lines = text.strip().splitlines()
    return lines[-1] if lines else ""


class Worker:
    """One tacli instance on one Xvfb, for the whole run."""

    def __init__(self, k, out):
        self.name = f"{PREFIX}{k}"
        self.out = out
        self.xvfb = start_xvfb()
        self.gamedir = INSTANCES / self.name / "gamedir"
        try:
            if (INSTANCES / self.name).is_dir():
                rc, o = tacli("rm", self.name, "--force", timeout=120)
                if rc:
                    raise CouldNotRun(f"tacli rm {self.name}: {last(o)}")
            rc, o = tacli("create", self.name, "--display", self.xvfb.display, "--slot", "0",
                          "--private-registry", timeout=300)
            if rc:
                raise CouldNotRun(f"tacli create {self.name}: {last(o)}")
            tacli("arm", self.name, "recorder.on")
        except BaseException:
            stop_xvfb(self.xvfb)
            raise
        say(f"{self.name} on Xvfb {self.xvfb.display} (pid {self.xvfb.pid})")

    def close(self):
        tacli("stop", self.name, "--hard", timeout=120)
        stop_xvfb(self.xvfb)

    def stop(self):
        tacli("stop", self.name, timeout=120)

    def place(self, fix, dll):
        """The DLL and this fixture's content in the game folder, before a launch: every fixture's
        content files removed first, so one fixture's units never reach another's game."""
        shutil.copy2(dll, self.gamedir / "ddraw.dll")
        rd = self.gamedir / "replays"
        for f in rd.glob("*") if rd.is_dir() else []:
            f.unlink()
        for other in fixtures().values():
            for name in (other.get("content") or {}).get("files", []):
                (self.gamedir / name).unlink(missing_ok=True)
        content = fix.get("content")
        if content:
            argv = [a.replace("{gamedir}", str(self.gamedir)) for a in content["run"]]
            if argv[0].endswith(".py"):
                argv = [sys.executable, str(TREE / argv[0])] + argv[1:]
            r = subprocess.run(argv, capture_output=True, text=True, timeout=300)
            if r.returncode:
                raise CouldNotRun(f"content {' '.join(content['run'])}: {last(r.stdout + r.stderr)}")

    def gametime(self):
        rc, o = tacli("peek", self.name, GAMETIME, "--json", timeout=60)
        if rc:
            raise CouldNotRun(f"tacli peek: {last(o)}")
        return int(str(json.loads(o[o.index("{"):])["peeks"][0]["value"]).split()[0])

    def trigger(self, text):
        """Written whole and renamed into place: the frame visit never reads half a line."""
        trig = self.gamedir / TRIGGER
        tmp = trig.with_name(trig.name + ".tmp")
        tmp.write_text(text + "\n")
        os.replace(tmp, trig)

    def collect(self, into: Path, recordings=False):
        """The dumps (and with `recordings` the recordings) and this run's log."""
        into.mkdir(parents=True, exist_ok=True)
        rd = self.gamedir / "replays"
        if rd.is_dir():
            for f in list(rd.glob("dump-*.bin")) + (list(rd.glob("*.imprec")) if recordings else []):
                shutil.copy2(f, into / f.name)
        (into / "tagpu.log").write_text(talog.run_text(self.gamedir))


class Log:
    """The instance's log from a launch on, across rotation; a COB stop ends the process."""

    def __init__(self, w: Worker):
        self.cursor = talog.Cursor(w.gamedir)
        self.part = ""
        self.lines = []

    def read(self):
        got = (self.part + self.cursor.text(advance=True)).split("\n")
        self.part = got.pop()
        self.lines += got
        for ln in got:
            if COB_STOP in ln:
                raise Failed(f"COB STOP: {ln.strip()}")
        return got

    def wait(self, rx, timeout, start=0):
        """The first line from index `start` on that matches, or None after `timeout` s."""
        rx = re.compile(rx)
        end = time.time() + timeout
        while True:
            for ln in self.lines[start:]:
                if rx.search(ln):
                    return ln
            start = len(self.lines)
            if time.time() >= end:
                return None
            time.sleep(0.4)
            self.read()


# ---- the fixtures and the GameTimes ---------------------------------------------------------

def fixtures():
    return json.loads((HERE / "fixtures.json").read_text())["fixtures"]


def head():
    r = subprocess.run(["git", "-C", str(TREE), "rev-parse", "HEAD"], capture_output=True,
                       text=True)
    return r.stdout.strip() or "no-head"


def gametimes(name, end, h):
    """Four T = 1 (mod 30) between 301 and 0.8 x end, with room for the dump after a round trip,
    sorted, from a generator seeded by the fixture and HEAD."""
    hi = min(int(0.8 * end), end - CONTINUE - 30)
    pool = list(range(301, hi + 1, 30))
    if len(pool) < 4:
        raise CouldNotRun(f"{name}: end {end} leaves fewer than four GameTimes")
    rng = random.Random(int(hashlib.sha1((name + h).encode()).hexdigest()[:8], 16))
    return sorted(rng.sample(pool, 4))


# ---- the four processes ---------------------------------------------------------------------

def arm(w, log, line, ok_rx, bad_rx, what):
    """The trigger line, then its answer: `ok_rx`, or a refusal of this very item."""
    n = len(log.lines)
    w.trigger(line)
    bad = bad_rx + rf"|{re.escape(TRIGGER)}: not understood"
    got = log.wait(f"{ok_rx}|{bad}", 30, n)
    if not got or re.search(bad, got):
        raise CouldNotRun(f"{what}: {got.strip() if got else 'no answer in 30 s'}")


def arm_dump(w, log, name, T):
    arm(w, log, f"dump {name} at {T}", rf"recorder: dump {name} armed for GameTime {T}",
        rf"recorder: dump(:| {name}) .*(refused|please|not letters)", f"dump {name} at {T}")


def wait_dumps(log, names, timeout):
    """Every named dump taken, or the first that will not be."""
    for name in names:
        got = log.wait(rf"recorder: dump {name} (at GameTime|for GameTime \d+ not taken|refused)"
                       rf"|recorder: dump {name}: ", timeout)
        if not got or "at GameTime" not in got or "not taken" in got:
            raise CouldNotRun(f"dump {name}: {got.strip() if got else f'not taken in {timeout} s'}")


def resolve(arg, roster):
    if isinstance(arg, list):
        kind, rank = arg
        units = [u for u in roster if u["type"].upper() == kind and u["engine_index"]]
        if rank >= len(units):
            raise CouldNotRun(f"order: no {kind} of rank {rank} ({len(units)} in the roster)")
        return str(units[rank]["engine_index"])
    return str(arg)


def step(w, kind, arg):
    if kind == "apply":
        rc, o = tacli("scenario", "apply", w.name, SCENARIOS / arg, timeout=300)
        if rc:
            raise CouldNotRun(f"apply {arg}: {last(o)}")
    elif kind == "type":
        # the chat line: return opens it, the characters, return sends it
        tacli("keys", w.name, "return", "return")
        time.sleep(0.7)
        tacli("keys", w.name, *[f"char:{c}" for c in arg])
        time.sleep(0.7)
        tacli("keys", w.name, "return")
        time.sleep(1.5)
    elif kind == "order":
        rc, o = tacli("roster", w.name, "--json", timeout=60)
        roster = json.loads(o)["units"] if not rc else []
        kind_, rank = arg["unit"]
        unit = resolve([kind_, rank], roster)
        argv = [resolve(a, roster) for a in arg["args"]]
        rc, o = tacli("order", w.name, argv[0], *argv[1:], "--unit", unit, "--expect", kind_,
                      timeout=60)
        if rc:
            raise CouldNotRun(f"order {kind_} {rank} {' '.join(argv)}: {last(o)}")
    else:
        raise CouldNotRun(f"a step of kind {kind!r}")


def live(w, fx, Ts, out):
    """L: the fixture recorded, dumps l<T1> and l<Tlast>; returns the recording's path."""
    fix = fx["fix"]
    w.place(fix, fx["dll"])
    los = [] if fix.get("los") == "file" else ["--los", str(fix.get("los", 0))]
    log = Log(w)
    rc, o = tacli("scenario", "load", w.name, SCENARIOS / fix["load"], "--defaults", *los,
                  "--restart", "--keep-dll", "--unit-limit", UNIT_LIMIT, timeout=LAUNCH_TIMEOUT)
    if rc:
        raise CouldNotRun(f"scenario load {fix['load']}: {last(o)}")
    got = log.wait(r"recorder: (recording|not recording|FAILED)", 120)
    if not got or "recorder: recording" not in got:
        raise CouldNotRun(f"the recorder: {got.strip() if got else 'no line in 120 s'}")
    names = [f"l{Ts[0]}", f"l{Ts[-1]}"]
    for name, T in zip(names, (Ts[0], Ts[-1])):
        arm_dump(w, log, name, T)
    steps = sorted(fix.get("steps", []), key=lambda s: s[0])
    while True:
        g = w.gametime()
        while steps and steps[0][0] <= g:
            _, kind, arg = steps.pop(0)
            step(w, kind, arg)
        if g >= fix["end"] and not steps:
            break
        log.read()
        time.sleep(0.5)
    n = len(log.lines)
    rc, o = tacli("scenario", "apply", w.name, END_SCENARIO, timeout=300)
    if rc:
        raise CouldNotRun(f"apply {END_SCENARIO.name}: {last(o)}")
    for rx in (r"recorder: ended after", r"recorder: writer: .* closed"):
        if not log.wait(rx, 180, n):
            raise CouldNotRun(f"the recording did not close ({rx})")
    wait_dumps(log, names, 5)
    w.collect(out, recordings=True)
    w.stop()
    recs = sorted(out.glob("*.imprec"))
    if not recs:
        raise CouldNotRun("no .imprec in the live run's replays folder")
    rec = out / f"{fx['name']}.imprec"
    recs[-1].rename(rec)
    return rec


def play(w, fx, rec, out, arms, dumps, roundtrips=()):
    """P or R: `replay play` from the start, the dumps and round trips armed, each waited for."""
    w.place(fx["fix"], fx["dll"])
    log = Log(w)
    rc, o = tacli("replay", "play", w.name, rec, "--defaults", "--restart", "--keep-dll",
                  "--perspective", fx["viewed"], timeout=LAUNCH_TIMEOUT)
    if rc:
        raise CouldNotRun(f"replay play: {last(o)}")
    for name, T in arms:
        arm_dump(w, log, name, T)
    for name, T in roundtrips:
        arm(w, log, f"roundtrip {name} at {T}", rf"round trip {name} armed for GameTime {T}",
            rf"round trip {name}( at {T})? refused", f"roundtrip {name} at {T}")
    lines = {}
    for name, T in roundtrips:
        got = log.wait(rf"round trip {name} at {T}(: | not run)", 900)
        if not got or " not run" in got or "seek refused" in got:
            raise CouldNotRun(f"round trip {name}: {got.strip() if got else 'no line in 900 s'}")
        lines[name] = got[got.index("replay:"):].strip()
    wait_dumps(log, dumps, 900)
    w.collect(out)
    w.stop()
    return lines


def fresh(w, fx, rec, out, T):
    """Q: a fresh process that plays the recording from T, dump q<T>."""
    w.place(fx["fix"], fx["dll"])
    rc, o = tacli("replay", "seek", w.name, T, "--recording", rec, "--dump", f"q{T}",
                  "--defaults", "--restart", "--keep-dll", "--perspective", fx["viewed"],
                  "--timeout", 300, timeout=LAUNCH_TIMEOUT + 300)
    w.collect(out)
    w.stop()
    if rc:
        raise CouldNotRun(f"replay seek {T} --recording: {last(o)}")


# ---- the checks -----------------------------------------------------------------------------

def tarec_run(*args):
    r = subprocess.run([sys.executable, str(TAREC), *map(str, args)], capture_output=True,
                       text=True)
    return r.returncode, r.stdout + r.stderr


def checks(fx):
    """Every check of a fixture whose processes ran: (name, ok, detail) each."""
    rec, Ts, out = fx["rec"], fx["Ts"], fx["out"]
    res = []
    rc, o = tarec_run("info", rec)
    m = re.search(r"ended: (.*)", o)
    end = m.group(1) if m else "no end block"
    clean = (m and "complete" in end and " 0 id rows outside" in end and
             " 0 nano sprays uncredited" in end)
    res.append(("the end block", bool(clean), end))
    rc, o = tarec_run("check", rec)
    res.append(("tarec check", rc == 0, last(o) if rc == 0 else o.strip()))

    def compare(label, *args, files=()):
        missing = [f for f in files if not Path(f).exists()]
        if missing:
            res.append((label, False, f"no {', '.join(Path(f).name for f in missing)}"))
            return
        rc, o = tarec_run(*args)
        res.append((label, rc == 0, last(o) if rc == 0 else o.strip()))
        if rc == 0:
            fx["passed"] += list(files)

    for proc, names in (("L", [f"l{Ts[0]}", f"l{Ts[-1]}"]),):
        for n in names:
            d = out / "L" / f"dump-{n}.bin"
            compare(f"{proc} {n}", "compare", rec, d, files=[d])
    for proc, names in (("P", [f"p{Ts[0]}", f"p{Ts[-1]}"]),
                        ("R", [f"r{Ts[0]}"] + [f"rt{T}c" for T in Ts[1:]]),
                        ("Q", [f"q{Ts[0]}"])):
        for n in names:
            d = out / proc / f"dump-{n}.bin"
            compare(f"{proc} {n}", "compare", "--puppet", rec, d, files=[d])
    for T in Ts[1:]:
        line = fx["rt"].get(f"rt{T}", "")
        m = re.search(r"residue (\d+)", line)
        res.append((f"round trip {T}: the poison check", bool(m) and m.group(1) == "0",
                    line or "no line"))
        a, b = out / "R" / f"dump-rt{T}-a.bin", out / "R" / f"dump-rt{T}-b.bin"
        compare(f"round trip {T}: A against B", "compare-dumps", rec, a, b, files=[a, b])
    return res


# ---- the run --------------------------------------------------------------------------------

class Run:
    def __init__(self, names, args, out):
        self.q = queue.Queue()
        self.lock = threading.Lock()
        self.pending = 0
        self.fx = {}
        self.out = out
        h = head()
        fixes = fixtures()
        for name in names:
            fix = fixes[name]
            dll = Path(args.stock_dll if fix.get("dll") == "stock" else args.dll).resolve()
            if not dll.exists():
                raise CouldNotRun(f"{name}: no DLL at {dll} (make -C tagpu/ddraw"
                                  f"{' LIMITS=stock' if fix.get('dll') == 'stock' else ''})")
            Ts = gametimes(name, fix["end"], h)
            self.fx[name] = {"name": name, "fix": fix, "dll": dll, "Ts": Ts, "out": out / name,
                             "problems": [], "rt": {}, "passed": [], "procs": {}}
            say(f"{name}: T1 {Ts[0]}, round trips {Ts[1]} {Ts[2]} {Ts[3]}, Tlast {Ts[3]}")
            self.put((name, "L"))

    def put(self, job):
        with self.lock:
            self.pending += 1
        self.q.put(job)

    def done(self):
        with self.lock:
            self.pending -= 1

    def job(self, w, name, proc):
        fx = self.fx[name]
        Ts, out = fx["Ts"], fx["out"] / proc
        t0 = time.time()
        say(f"{name} {proc}: start on {w.name}")
        try:
            if proc == "L":
                fx["rec"] = live(w, fx, Ts, out)
                h = tarec.header_only(str(fx["rec"]))
                fx["viewed"] = h["options"]["viewed"]
                for p in ("P", "R", "Q"):
                    self.put((name, p))
            elif proc == "P":
                play(w, fx, fx["rec"], out, [(f"p{Ts[0]}", Ts[0]), (f"p{Ts[-1]}", Ts[-1])],
                     [f"p{Ts[0]}", f"p{Ts[-1]}"])
            elif proc == "R":
                arms = [(f"r{Ts[0]}", Ts[0])] + [(f"rt{T}c", T + CONTINUE) for T in Ts[1:]]
                fx["rt"] = play(w, fx, fx["rec"], out, arms, [n for n, _ in arms],
                                [(f"rt{T}", T) for T in Ts[1:]])
            else:
                fresh(w, fx, fx["rec"], out, Ts[0])
            fx["procs"][proc] = "ran"
        except (CouldNotRun, Failed) as e:
            kind = "FAILED" if isinstance(e, Failed) else "could not run"
            fx["procs"][proc] = kind
            fx["problems"].append((proc, kind, str(e)))
            say(f"{name} {proc}: {kind}: {e}")
            try:
                w.collect(out)
                w.stop()
            except Exception:
                pass
        say(f"{name} {proc}: {time.time() - t0:.0f} s")

    def worker(self, k):
        try:
            w = Worker(k, self.out)
        except CouldNotRun as e:
            say(f"worker {k}: {e}")
            return
        try:
            while True:
                try:
                    name, proc = self.q.get(timeout=2)
                except queue.Empty:
                    with self.lock:
                        if self.pending == 0:
                            return
                    continue
                try:
                    self.job(w, name, proc)
                finally:
                    self.done()
        finally:
            w.close()


def report(run, keep):
    failed = unrun = False
    lines = []
    for name, fx in run.fx.items():
        lines.append(f"== {name}: T {' '.join(map(str, fx['Ts']))}; {fx['out']}")
        for proc, kind, why in fx["problems"]:
            lines.append(f"   {proc} {kind.upper()}: {why}")
            failed |= kind == "FAILED"
            unrun |= kind != "FAILED"
        if fx["procs"].get("L") != "ran":
            continue
        for label, ok, detail in checks(fx):
            lines.append(f"   {'pass' if ok else 'FAIL'}  {label}: "
                         + (detail.splitlines()[0] if ok else detail.replace("\n", "\n         ")))
            failed |= not ok
        for T in fx["Ts"][1:]:
            if f"rt{T}" in fx["rt"]:
                lines.append(f"         {fx['rt'][f'rt{T}']}")
        if not keep:
            for f in fx["passed"]:
                Path(f).unlink(missing_ok=True)
    text = "\n".join(lines).replace(f"{run.out}/", "") + "\n"
    print(text, end="")
    (run.out / "summary.txt").write_text(text)
    return 1 if failed else 2 if unrun else 0


def cmd_run(args):
    names = list(fixtures()) if args.fixtures == ["all"] else args.fixtures
    unknown = [n for n in names if n not in fixtures()]
    if unknown:
        print(f"tareplay: no fixture {', '.join(unknown)} (tareplay.py list)", file=sys.stderr)
        return 2
    base = Path(os.environ.get("TAREPLAY_OUT") or
                Path.home() / ".local" / "share" / "ta-replay" / "runs")
    out = Path(args.out) if args.out else base / datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    say(f"tareplay: {len(names)} fixtures, {args.j} at once, into {out}")
    try:
        run = Run(names, args, out)
    except CouldNotRun as e:
        print(f"tareplay: {e}", file=sys.stderr)
        return 2
    threads = [threading.Thread(target=run.worker, args=(k,), daemon=True)
               for k in range(1, args.j + 1)]

    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    for t in threads:
        t.start()
    try:
        while any(t.is_alive() for t in threads):
            time.sleep(1)
    except KeyboardInterrupt:
        say("interrupted: the workers stop their games and their Xvfbs")
        for t in threads:
            t.join()
        return 2
    if run.pending:
        say(f"{run.pending} jobs never ran: no worker could start")
    return report(run, args.keep) or (2 if run.pending else 0)


def cmd_list(args):
    for name, fix in fixtures().items():
        print(f"{name:16} {fix['load']}, end at GameTime {fix['end']}"
              + (", the stock-limits build" if fix.get("dll") == "stock" else ""))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("\n", 2)[2])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="record each fixture and check it on puppets")
    r.add_argument("fixtures", nargs="+", help="fixture names, or `all`")
    r.add_argument("-j", type=int, default=4, help="games at once (default 4)")
    r.add_argument("--dll", default=str(TREE / "tagpu" / "ddraw" / "ddraw.dll"),
                   help="the build under test (default the tree's)")
    r.add_argument("--stock-dll", default=str(TREE / "tagpu" / "ddraw" / "ddraw-stocklimits.dll"),
                   help="the stock-limits build, for a fixture that names it")
    r.add_argument("--keep", action="store_true", help="keep the passing dumps")
    r.add_argument("--out", help="the run's folder")
    r.set_defaults(func=cmd_run)
    ls = sub.add_parser("list", help="the fixtures")
    ls.set_defaults(func=cmd_list)
    a = ap.parse_args(argv)
    if a.cmd == "run" and not 1 <= a.j <= 9:
        ap.error("-j is 1..9")
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
