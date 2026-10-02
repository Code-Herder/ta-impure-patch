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
for the whole run, and holds an exclusive lock on INSTANCES/tareplay-<k>.lock for as long: a k
another run holds is skipped, so two runs never touch each other's games. Nothing is ever stopped
by name or pattern, only by tacli on an instance this run holds, or by the PID it started.
SIGTERM or Ctrl-C stops the run: no new game starts, each game in progress is stopped at its next
wait, and every worker stops its instance and its Xvfb. Exit 0: every check passed; 1: a named
failure; 2: something could not run (and nothing failed by name).

The output folder (default ~/.local/share/ta-replay/runs/<time>, or $TAREPLAY_OUT) is outside the
repository: its logs carry absolute paths.
"""
import argparse
import fcntl
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
import tafeat  # noqa: E402

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
STOP = threading.Event()        # set by SIGTERM / Ctrl-C: every wait gives up, no job starts


def say(*a):
    with PRINT:
        print(time.strftime("%H:%M:%S"), *a, flush=True)


class CouldNotRun(Exception):
    """The run could not reach the claim it checks: exit 2, not a failure of the change."""


class Failed(Exception):
    """A named failure of the change (exit 1)."""


def stopping():
    if STOP.is_set():
        raise CouldNotRun("the run was stopped")


def nap(s):
    """A sleep that a stopped run cuts short."""
    if STOP.wait(s):
        raise CouldNotRun("the run was stopped")


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

    def __init__(self, out):
        self.lock = None
        for k in range(1, 100):
            INSTANCES.mkdir(parents=True, exist_ok=True)
            f = open(INSTANCES / f"{PREFIX}{k}.lock", "w")
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                f.close()
                continue
            self.lock, self.name = f, f"{PREFIX}{k}"
            break
        if not self.lock:
            raise CouldNotRun("every tareplay-<k> instance is held by another run")
        self.out = out
        try:
            self.xvfb = start_xvfb()
        except BaseException:
            self.lock.close()
            raise
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
            self.lock.close()
            raise
        say(f"{self.name} on Xvfb {self.xvfb.display} (pid {self.xvfb.pid})")

    def close(self):
        tacli("stop", self.name, "--hard", timeout=120)
        stop_xvfb(self.xvfb)
        self.lock.close()

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
            for f in (list(rd.glob("dump-*.bin")) + list(rd.glob("cover-*.bin")) +
                      (list(rd.glob("*.imprec")) if recordings else [])):
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
            nap(0.4)
            self.read()


# ---- the fixtures and the GameTimes ---------------------------------------------------------

def sources_mtime():
    """The newest modification time of what a build of the DLL reads: its sources, headers and
    the generators' lists. A run on an older DLL would grade another build."""
    d = TREE / "tagpu" / "ddraw"
    files = [f for sub in ("src", "inc") for f in (d / sub).rglob("*")
             if f.suffix in (".c", ".h") and f.name != "git.h"]   # make writes git.h at every parse
    files += [d / "replay-globals.tsv", d / "replay-layout.tsv"] + list(d.glob("*.allow"))
    return max(f.stat().st_mtime for f in files)


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


def cover_ask(w, log, name):
    """The coverage build's functions that ran so far, as replays/cover-<name>.bin; the DLL writes
    the file again at the level's end, with what ran after (tagpu_cover_flush)."""
    arm(w, log, f"cover {name}", rf"cover: replays\\cover-{name}\.bin: \d+ functions",
        rf"cover: .*{name}.*(refused|could not|NOT written)", f"cover {name}")


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


def twin(w, log):
    """The null twin (tarec images-gate): the live game paused, frame dump ln1; then what the
    viewer does without changing the game -- the arrow keys a second each way, one wheel notch in
    and out at the map's centre, the cursor over the map and over the panel, the in-game menu
    (Tab) opened -- and frame dump ln2. GameTime must not move between them; the menu's OK then
    closes it and the game runs on."""
    tacli("keys", w.name, "pause")
    nap(1.5)
    g1 = w.gametime()
    nap(1.0)
    if w.gametime() != g1:
        raise CouldNotRun("the twin: the game did not pause")
    n = len(log.lines)
    w.trigger("dump ln1 now")
    if not log.wait(r"recorder: frame dump ln1 at GameTime", 30, n):
        raise CouldNotRun("the twin: frame dump ln1 not taken in 30 s")
    for key in ("up", "down", "left", "right"):
        tacli("keys", w.name, f"down:{key}")
        nap(1.0)
        tacli("keys", w.name, f"up:{key}")
        nap(0.3)
    tacli("keys", w.name, "pmove:512,384")
    nap(0.3)
    tacli("keys", w.name, "wheel:+1")
    nap(0.8)
    tacli("keys", w.name, "wheel:-1")
    nap(0.8)
    tacli("keys", w.name, "pmove:700,500")
    nap(0.5)
    tacli("keys", w.name, "pmove:60,300")
    nap(0.5)
    # the in-game menu is Tab (ARMOPT.GUI): ln2 is taken with it open, since its OK resumes the
    # game (MEASURED: GameTime moved between the dumps when ln2 followed the OK); Esc does nothing
    tacli("keys", w.name, "tab")
    rc, o = tacli("ui", w.name, "wait", "--gui", "ARMOPT.GUI", "--timeout", "10", timeout=60)
    if rc:
        raise CouldNotRun(f"the twin: the in-game menu did not open: {last(o)}")
    n = len(log.lines)
    w.trigger("dump ln2 now")
    if not log.wait(r"recorder: frame dump ln2 at GameTime", 30, n):
        raise CouldNotRun("the twin: frame dump ln2 not taken in 30 s")
    if w.gametime() != g1:
        raise CouldNotRun("the twin: GameTime moved between its dumps")
    rc, o = tacli("ui", w.name, "click", "OK", timeout=60)
    if rc:
        raise CouldNotRun(f"the twin: the in-game menu's OK: {last(o)}")
    nap(1.5)
    if w.gametime() == g1:
        tacli("keys", w.name, "pause")
        nap(2.0)
        if w.gametime() == g1:
            raise CouldNotRun("the twin: the game did not resume")


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
    names = [f"l{Ts[0]}", f"l{Ts[-1]}"] if Ts else []
    for name, T in zip(names, (Ts[0], Ts[-1]) if Ts else ()):
        arm_dump(w, log, name, T)
    steps = sorted(fix.get("steps", []), key=lambda s: s[0])
    twinned = False
    # a frozen game or a level that ends early stops here: four times the end's real time
    deadline = time.time() + 120 + 4 * fix["end"] / 30
    while True:
        if time.time() > deadline:
            raise CouldNotRun(f"the live game did not reach GameTime {fix['end']} in "
                              f"{120 + 4 * fix['end'] / 30:.0f} s (at {w.gametime()})")
        g = w.gametime()
        while steps and steps[0][0] <= g:
            _, kind, arg = steps.pop(0)
            step(w, kind, arg)
        if Ts and not twinned and g > Ts[-1] + 5:
            wait_dumps(log, names[1:], 30)
            twin(w, log)
            twinned = True
        if g >= fix["end"] and not steps:
            break
        log.read()
        nap(0.5)
    if fx["cover"]:
        cover_ask(w, log, "L")
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


def play(w, fx, rec, out, proc, arms, dumps, roundtrips=()):
    """P or R: `replay play` from the start, the dumps and round trips armed, each waited for;
    with no dump at all (measure), the recording played to its end."""
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
    if not dumps:
        got = log.wait(r"replay: .* (played to its end|cannot be read on|not playing)", 900)
        if not got or "played to its end" not in got:
            raise CouldNotRun(f"the playing: {got.strip() if got else 'no end in 900 s'}")
    if fx["cover"]:
        cover_ask(w, log, proc)
    w.collect(out)
    w.stop()
    return lines


def fresh(w, fx, rec, out, T):
    """Q: a fresh process that plays the recording from T, dump q<T>."""
    w.place(fx["fix"], fx["dll"])
    log = Log(w)
    rc, o = tacli("replay", "seek", w.name, T, "--recording", rec, "--dump", f"q{T}",
                  "--defaults", "--restart", "--keep-dll", "--perspective", fx["viewed"],
                  "--timeout", 300, timeout=LAUNCH_TIMEOUT + 300)
    if not rc and fx["cover"]:
        log.read()
        cover_ask(w, log, "Q")
    w.collect(out)
    w.stop()
    if rc:
        raise CouldNotRun(f"replay seek {T} --recording: {last(o)}")


# ---- the checks -----------------------------------------------------------------------------

def tarec_run(*args):
    r = subprocess.run([sys.executable, str(TAREC), *map(str, args)], capture_output=True,
                       text=True)
    return r.returncode, r.stdout + r.stderr


PASS, FAIL, UNRUN = "pass", "FAIL", "could not run"


def checks(fx):
    """Every check of the processes that ran: (name, verdict, detail) each. A dump tarec cannot
    read (its exit 2) could not run; only a comparison that ran and differed is a FAIL."""
    rec, Ts, out, ran = fx["rec"], fx["Ts"], fx["out"], fx["procs"]
    res = []
    rc, o = tarec_run("info", rec)
    m = re.search(r"ended: (.*)", o)
    end = m.group(1) if m else "no end block"
    clean = (m and "complete" in end and " 0 id rows outside" in end and
             " 0 nano sprays uncredited" in end)
    res.append(("the end block", PASS if clean else FAIL if m else UNRUN, end))
    rc, o = tarec_run("check", rec)
    res.append(("tarec check", PASS if rc == 0 else FAIL if rc == 1 else UNRUN,
                last(o) if rc == 0 else o.strip()))

    def compare(label, *args, files=()):
        missing = [f for f in files if not Path(f).exists()]
        if missing:
            res.append((label, UNRUN, f"no {', '.join(Path(f).name for f in missing)}"))
            return
        rc, o = tarec_run(*args)
        res.append((label, PASS if rc == 0 else FAIL if rc == 1 else UNRUN,
                    last(o) if rc == 0 else o.strip()))
        fx["passed" if rc == 0 else "held"] += list(files)

    for n in (f"l{Ts[0]}", f"l{Ts[-1]}"):
        d = out / "L" / f"dump-{n}.bin"
        compare(f"L {n}", "compare", rec, d, files=[d])
    later = {T: [U for U in Ts[1:] if U > T and U <= T + CONTINUE] for T in Ts[1:]}
    for proc, names in (("P", [(f"p{Ts[0]}", ""), (f"p{Ts[-1]}", "")]),
                        ("R", [(f"r{Ts[0]}", "")] +
                              [(f"rt{T}c", f" (after the round trips at {' and '.join(map(str, [T] + later[T]))})")
                               for T in Ts[1:]]),
                        ("Q", [(f"q{Ts[0]}", "")])):
        if ran.get(proc) != "ran":
            continue
        for n, note in names:
            d = out / proc / f"dump-{n}.bin"
            compare(f"{proc} {n}{note}", "compare", "--puppet", rec, d, files=[d])
    l1, lT = out / "L" / f"dump-l{Ts[0]}.bin", out / "L" / f"dump-l{Ts[-1]}.bin"
    p1, pT = out / "P" / f"dump-p{Ts[0]}.bin", out / "P" / f"dump-p{Ts[-1]}.bin"
    n1, n2 = out / "L" / "dump-ln1.bin", out / "L" / "dump-ln2.bin"
    r1, q1 = out / "R" / f"dump-r{Ts[0]}.bin", out / "Q" / f"dump-q{Ts[0]}.bin"
    if ran.get("P") == "ran":
        compare(f"counts, L against P over [{Ts[0]}, {Ts[-1]})", "compare-counts", rec, l1, lT,
                p1, pT, files=[l1, lT, p1, pT])
    if all(ran.get(k) == "ran" for k in "PRQ"):
        compare(f"the images gate at {Ts[0]}", "images-gate", rec, l1, lT, n1, n2, p1, pT, r1, q1,
                files=[l1, lT, n1, n2, p1, pT, r1, q1])
    for proc, d in (("L", lT), ("P", pT)):
        if ran.get(proc) == "ran":
            compare(f"{proc} {d.stem[5:]}: the address scan", "scan", rec, d, files=[d])
    for proc, d in (("R", out / "R" / f"dump-rt{Ts[-1]}c.bin"), ("Q", q1)):
        if ran.get(proc) != "ran":
            continue
        if not d.exists():
            res.append((f"{proc}: COB engine writes on a puppet", UNRUN, f"no {d.name}"))
            continue
        rc, o = tarec_run("counts", rec, d)
        writes = [ln.strip() for ln in o.splitlines() if "COB write" in ln and not ln.endswith(": 0")]
        res.append((f"{proc}: COB engine writes on a puppet",
                    UNRUN if rc else FAIL if writes else PASS,
                    o.strip() if rc else "; ".join(writes) if writes else f"none ({d.name})"))
    if ran.get("R") == "ran":
        for T in Ts[1:]:
            line = fx["rt"].get(f"rt{T}", "")
            m = re.search(r"(\d+) tables poisoned, residue (\d+)", line)
            ok = bool(m) and int(m.group(1)) > 0 and m.group(2) == "0"
            res.append((f"round trip {T}: the poison check", PASS if ok else FAIL if m else UNRUN,
                        line or "no line"))
            a, b = out / "R" / f"dump-rt{T}-a.bin", out / "R" / f"dump-rt{T}-b.bin"
            compare(f"round trip {T}: A against B", "compare-dumps", rec, a, b, files=[a, b])
    return res


# ---- the run --------------------------------------------------------------------------------

def build_dll(stock, cover):
    """The DLL a run uses and the make line that builds it."""
    d = TREE / "tagpu" / "ddraw"
    name = ("ddraw-stocklimits" if stock else "ddraw") + ("-cover" if cover else "") + ".dll"
    make = ("make -C tagpu/ddraw" + (" LIMITS=stock" if stock else "") +
            (" COVER=1" if cover else ""))
    return d / name, make


class Run:
    """`fixes` the fixtures by name (fixtures.json's, or measure's scenarios); `measure` runs L
    and P only, with no dump and no check, for the coverage files."""

    def __init__(self, names, args, out, fixes=None, measure=False):
        self.q = queue.Queue()
        self.lock = threading.Lock()
        self.pending = 0
        self.fx = {}
        self.out = out
        self.measure = measure
        h = head()
        fixes = fixes or fixtures()
        cover = bool(getattr(args, "cover", False))
        for name in names:
            fix = fixes[name]
            stock = fix.get("dll") == "stock"
            dll, make = build_dll(stock, cover)
            given = args.stock_dll if stock else args.dll
            if given:
                dll = Path(given)
            dll = dll.resolve()
            if not dll.exists():
                raise CouldNotRun(f"{name}: no DLL at {dll} ({make})")
            if dll.stat().st_mtime < sources_mtime():
                raise CouldNotRun(f"{name}: {dll} is older than its sources ({make})")
            Ts = gametimes(name, fix["end"], h)
            self.fx[name] = {"name": name, "fix": fix, "dll": dll, "Ts": Ts, "out": out / name,
                             "problems": [], "rt": {}, "passed": [], "held": [],
                             "procs": {}, "cover": cover, "measure": measure,
                             "map": dll.with_suffix(".map")}
            if measure:
                say(f"{name}: recorded to GameTime {fix['end']} and played")
            else:
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
        """One process. Anything that stops it short of its claim -- a tool's timeout, an
        unexpected answer, a stopped run -- is recorded as could not run, never lost."""
        fx = self.fx[name]
        Ts, out = fx["Ts"], fx["out"] / proc
        t0 = time.time()
        say(f"{name} {proc}: start on {w.name}")
        try:
            stopping()
            if proc == "L":
                fx["rec"] = live(w, fx, None if fx["measure"] else Ts, out)
                h = tarec.header_only(str(fx["rec"]))
                fx["viewed"] = h["options"]["viewed"]
                for p in ("P",) if fx["measure"] else ("P", "R", "Q"):
                    self.put((name, p))
            elif proc == "P" and fx["measure"]:
                play(w, fx, fx["rec"], out, "P", [], [])
            elif proc == "P":
                play(w, fx, fx["rec"], out, "P", [(f"p{Ts[0]}", Ts[0]), (f"p{Ts[-1]}", Ts[-1])],
                     [f"p{Ts[0]}", f"p{Ts[-1]}"])
            elif proc == "R":
                arms = [(f"r{Ts[0]}", Ts[0])] + [(f"rt{T}c", T + CONTINUE) for T in Ts[1:]]
                fx["rt"] = play(w, fx, fx["rec"], out, "R", arms, [n for n, _ in arms],
                                [(f"rt{T}", T) for T in Ts[1:]])
            else:
                fresh(w, fx, fx["rec"], out, Ts[0])
            fx["procs"][proc] = "ran"
        except Exception as e:
            kind = "FAILED" if isinstance(e, Failed) else "could not run"
            why = str(e) if isinstance(e, (CouldNotRun, Failed)) else f"{type(e).__name__}: {e}"
            fx["procs"][proc] = kind
            fx["problems"].append((proc, kind, why))
            say(f"{name} {proc}: {kind}: {why}")
            try:
                w.collect(out)
                w.stop()
            except Exception:
                pass
        say(f"{name} {proc}: {time.time() - t0:.0f} s")

    def worker(self):
        try:
            w = Worker(self.out)
        except Exception as e:
            say(f"a worker did not start: {e}")
            return
        try:
            while not STOP.is_set():
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
        for proc in ("L", "P", "R", "Q"):
            if proc not in fx["procs"]:
                lines.append(f"   {proc} NEVER RAN")
                unrun = True
        if fx["procs"].get("L") != "ran":
            continue
        for label, verdict, detail in checks(fx):
            lines.append(f"   {verdict:4}  {label}: "
                         + (detail.splitlines()[0] if verdict == PASS
                            else detail.replace("\n", "\n         ")))
            failed |= verdict == FAIL
            unrun |= verdict == UNRUN
        for T in fx["Ts"][1:]:
            if f"rt{T}" in fx["rt"]:
                lines.append(f"         {fx['rt'][f'rt{T}']}")
        if not keep:
            # a dump any check did not pass on stays, whatever else passed on it
            for f in set(map(str, fx["passed"])) - set(map(str, fx["held"])):
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
    out = out_dir(args)
    say(f"tareplay: {len(names)} fixtures, {args.j} at once, into {out}")
    try:
        run = Run(names, args, out)
    except CouldNotRun as e:
        print(f"tareplay: {e}", file=sys.stderr)
        return 2
    run.j = args.j
    run_threads(run)
    status = report(run, args.keep)
    if STOP.is_set():
        return status or 2
    return status


def run_threads(run):
    """The run's workers to their end; a stop by SIGTERM or Ctrl-C lets each finish its game."""
    threads = [threading.Thread(target=run.worker) for _ in range(run.j)]

    def interrupted(signum, frame):
        if not STOP.is_set():
            say("stopping: no new game starts; each worker stops its game and its Xvfb")
        STOP.set()
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    for t in threads:
        t.start()
    while any(t.is_alive() for t in threads):
        time.sleep(1)


def out_dir(args):
    base = Path(os.environ.get("TAREPLAY_OUT") or
                Path.home() / ".local" / "share" / "ta-replay" / "runs")
    out = Path(args.out) if args.out else base / datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    return out


def covered_sources():
    """The instrumented sources (the Makefile's COVERED): the sim files and the recorder's own."""
    d = TREE / "tagpu" / "ddraw"
    return set(tafeat.sim_files(d)) | {"src/tagpu_recorder.c", "src/tagpu_recwriter.c",
                                       "src/tagpu_recreader.c", "src/tagpu_replay.c"}


def reached_of(run):
    """{fixture: {(source, function)}} from every process's coverage files, each read against the
    map of the build it ran."""
    srcs = covered_sources()
    out = {}
    maps = {}
    for name, fx in run.fx.items():
        covers = sorted(fx["out"].glob("*/cover-*.bin"))
        if not covers:
            continue
        m = maps.setdefault(str(fx["map"]), tafeat.read_map(fx["map"]))
        out[name] = {k for k in tafeat.reached(m, covers) if k[0] in srcs}
    return out


def cmd_measure(args):
    """Each scenario as a fixture, L and P only, on the coverage builds: which functions of the
    instrumented files it reaches (reached.json), for declaring the features (S3c-3, step 3)."""
    fixes = {}
    known = fixtures()
    for sc in args.scenarios:
        stem = Path(sc).stem
        if stem in known:                   # a fixture runs as itself, to its own end
            fixes[stem] = known[stem]
            continue
        if not (SCENARIOS / f"{stem}.json").exists():
            print(f"tareplay: no scenario {stem}.json", file=sys.stderr)
            return 2
        fixes[stem] = {"load": f"{stem}.json", "end": args.end}
    out = out_dir(args)
    args.cover = True
    say(f"tareplay measure: {len(fixes)} scenarios to GameTime {args.end}, {args.j} at once, "
        f"into {out}")
    try:
        run = Run(list(fixes), args, out, fixes=fixes, measure=True)
    except CouldNotRun as e:
        print(f"tareplay: {e}", file=sys.stderr)
        return 2
    run.j = args.j
    run_threads(run)
    got = reached_of(run)
    (out / "reached.json").write_text(json.dumps(
        {k: sorted(f"{s}:{f}" for s, f in v) for k, v in got.items()}, indent=1) + "\n")
    lines = []
    for name, fx in run.fx.items():
        state = " ".join(f"{p} {fx['procs'].get(p, 'never ran')}" for p in ("L", "P"))
        lines.append(f"{name}: {state}; {len(got.get(name, ()))} functions reached")
        for proc, kind, why in fx["problems"]:
            lines.append(f"   {proc} {kind}: {why}")
    text = "\n".join(lines) + "\n"
    print(text, end="")
    (out / "summary.txt").write_text(text)
    return 0 if all(len(fx["procs"]) == 2 and set(fx["procs"].values()) == {"ran"}
                    for fx in run.fx.values()) else 2


# ---- the landing tier and the broad tier ----------------------------------------------------

DD = "tagpu/ddraw/"
RECORDER_OWN = {DD + "src/tagpu_recorder.c", DD + "src/tagpu_recwriter.c",
                DD + "src/tagpu_recreader.c", DD + "src/tagpu_replay.c",
                DD + "inc/tagpu_recorder.h", DD + "inc/tagpu_recfmt.h", DD + "inc/tagpu_recreader.h",
                DD + "inc/tagpu_replay.h", DD + "inc/tagpu_replay_globals.h",
                DD + "inc/tagpu_replay_layout.h", DD + "replay-globals.tsv",
                DD + "replay-layout.tsv", DD + "replay-absent.tsv",
                "tools/replay-globals-gen.py", "tools/replay-layout-gen.py",
                "tools/replay-layout-check.sh"}
FIXTURES_JSON = "tools/replay/fixtures.json"
NOTE = "replay-roundtrip:"


def git(*a, check=True):
    r = subprocess.run(["git", "-C", str(TREE), *a], capture_output=True, text=True)
    if check and r.returncode:
        raise CouldNotRun(f"git {' '.join(a)}: {r.stderr.strip()}")
    return r.stdout


def sim_paths():
    return {DD + f for f in tafeat.sim_files()}


def includes_of(sim):
    """The headers a sim file's object was built from, by its .d file (any build's)."""
    d = TREE / "tagpu" / "ddraw"
    stem = sim[len(DD):-2]
    for suffix in (".d", ".cover.d", ".stock.d", ".stock.cover.d"):
        f = d / (stem + suffix)
        if f.exists():
            words = f.read_text().replace("\\\n", " ").split()
            return {DD + w for w in words if w.endswith(".h") and not w.endswith(":")}
    return set()


def fixture_users(scenario):
    """The fixtures whose load or applied steps name `scenario`."""
    out = set()
    for name, fix in fixtures().items():
        named = {fix["load"]} | {a for _, k, a in fix.get("steps", []) if k == "apply"}
        if scenario in named:
            out.add(name)
    return out


def gated(paths, base):
    """(fixtures, sim files to read by region, recorder files to read by function, why): what
    the changed paths make the landing run."""
    sims = sim_paths()
    every = set(fixtures())
    fx, by_region, by_function, why = set(), set(), set(), []
    headers = {s: includes_of(s) for s in sims}
    for p in sorted(paths):
        if p in sims:
            by_region.add(p)
        elif p == FIXTURES_JSON:
            old = json.loads(git("show", f"{base}:{p}", check=False) or '{"fixtures": {}}')
            old = old.get("fixtures", {})
            new = fixtures()
            changed = {k for k in set(old) | set(new) if old.get(k) != new.get(k)} & set(new)
            fx |= changed
            why.append(f"{p}: the entries {' '.join(sorted(changed)) or 'none left'}")
        elif p in RECORDER_OWN or p.startswith("tools/replay/"):
            fx |= every
            if p.endswith(".c"):
                by_function.add(p)
            why.append(f"{p}: the recorder's own, every fixture")
        elif p.startswith("scenarios/") and fixture_users(Path(p).name):
            users = fixture_users(Path(p).name)
            fx |= users
            why.append(f"{p}: the scenario of {' '.join(sorted(users))}")
        else:
            for sim, hs in headers.items():
                if p in hs:
                    by_region.add(sim)
                    why.append(f"{p}: included by {sim}, every feature of it")
    return fx, by_region, by_function, why


def changed_lines(path, base):
    """The new-side lines of `path` that changed in code, not only in comments or whitespace."""
    old = git("show", f"{base}:{path}", check=False)
    new = (TREE / path).read_text()
    ob = tafeat.blank(old).split("\n")
    nb = tafeat.blank(new).split("\n")
    out = set()
    diff = git("diff", "-U0", f"{base}...HEAD", "--", path)
    for m in re.finditer(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", diff, re.M):
        a, b = int(m.group(1)), int(m.group(2) if m.group(2) is not None else 1)
        c, d = int(m.group(3)), int(m.group(4) if m.group(4) is not None else 1)
        old_code = "".join("".join(ob[a - 1:a - 1 + b]).split())
        new_code = "".join("".join(nb[c - 1:c - 1 + d]).split())
        if old_code == new_code:
            continue
        out |= set(range(c, c + d)) if d else {max(c, 1)}
    return out


def landing_plan(base):
    """What the landing must reach: (fixtures, functions {(src, fn): feature}, regions
    {feature: [(src, fn)...]} for file-scope lines, problems, why)."""
    paths = [p for p in git("diff", "--name-only", f"{base}...HEAD").split() if p]
    fx, by_region, by_function, why = gated(paths, base)
    feats, problems = tafeat.all_features()
    funcs, regions = {}, {}
    for path in sorted(by_region | by_function):
        if not (TREE / path).exists():
            continue
        rel = path[len(DD):]
        text = (TREE / path).read_text()
        fns = tafeat.functions(text)
        lines = changed_lines(path, base) if path in paths else None
        if lines is None:                   # a header it includes changed: every feature of it
            for f in feats:
                if f["file"] == rel:
                    fx |= set(f["fixtures"])
            continue
        for ln in sorted(lines):
            fn = next((f for f in fns if f[1] <= ln <= f[2]), None)
            if path in by_region:
                feat = tafeat.feature_at(feats, rel, ln)
                if not feat:
                    problems.append(f"UNFEATURED {path}:{ln}")
                    continue
                fx |= set(feat["fixtures"])
                if fn:
                    funcs[(rel, fn[0])] = feat["name"]
                else:
                    regions.setdefault(feat["name"], [(rel, f[0]) for f in fns
                                                      if feat["line"] <= f[1] <= feat["end"]])
            elif fn:
                funcs[(rel, fn[0])] = "the recorder's own"
    return fx, funcs, regions, problems, why


def cmd_landing(args):
    base = args.base
    if args.check:
        return landing_check(base)
    dirty = [ln[3:] for ln in git("status", "--porcelain", "--", "tagpu/ddraw", "tools",
                                   "scenarios").splitlines() if ln[:2].strip()]
    if dirty:
        print(f"tareplay landing: uncommitted changes ({' '.join(dirty[:6])}"
              f"{' ...' if len(dirty) > 6 else ''}): the landing grades HEAD, so commit first",
              file=sys.stderr)
        return 2
    # the commit graded, taken now: the note goes on it even if HEAD moves while the games run
    graded = git("rev-parse", "HEAD").strip()
    dll = git("describe", "--always", "--dirty").strip()
    fx, funcs, regions, problems, why = landing_plan(base)
    for w_ in why:
        say(w_)
    if not fx and not funcs and not problems:
        print("tareplay landing: nothing gated changed")
        return 0
    say(f"tareplay landing: fixtures {' '.join(sorted(fx)) or 'none'}; {len(funcs)} changed "
        f"functions; {len(regions)} file-scope regions")
    status = 0
    run = None
    if fx:
        args.cover = True
        out = out_dir(args)
        try:
            run = Run(sorted(fx), args, out)
        except CouldNotRun as e:
            print(f"tareplay: {e}", file=sys.stderr)
            return 2
        run.j = args.j
        run_threads(run)
        status = report(run, args.keep)
        if STOP.is_set():
            return status or 2
    got = set().union(*reached_of(run).values()) if run else set()
    for (src, fn), feat in sorted(funcs.items()):
        if (src, fn) not in got:
            problems.append(f"UNCOVERED {src}:{fn} (feature {feat}; fixtures "
                            f"{' '.join(sorted(fx)) or 'none'})")
    for feat, fns in sorted(regions.items()):
        if not any(k in got for k in fns):
            problems.append(f"UNCOVERED {feat}: a changed file-scope line, and no function of its "
                            f"region ran")
    for p in problems:
        print(f"tareplay landing: {p}")
    k = sum(1 for key in funcs if key in got) + sum(
        1 for fns in regions.values() if any(x in got for x in fns))
    total = len(funcs) + len(regions)
    if status == 0 and not problems:
        sha = graded[:8]
        seeds = " ".join(f"{n}:{run.fx[n]['Ts'][0]}" for n in sorted(fx)) if run else ""
        feats = sorted(set(funcs.values()) | set(regions))
        note = (f"{NOTE} dll={dll} range={base}..{sha} features={','.join(feats) or '-'} "
                f"fixtures={','.join(sorted(fx)) or '-'} seeds={seeds.replace(' ', ',') or '-'} "
                f"reached={k}/{total} result=pass date={datetime.date.today().isoformat()}")
        git("notes", "append", "-m", note, graded)
        print(f"tareplay landing: pass, noted on {sha}: {note}")
        return 0
    return status or 1


def landing_check(base):
    """0 when a passing note sits on a commit R of base..HEAD and R..HEAD changes no gated
    file; 1 otherwise, saying why."""
    paths = [p for p in git("diff", "--name-only", f"{base}...HEAD").split() if p]
    fx, by_region, by_function, _ = gated(paths, base)
    if not (fx or by_region or by_function):
        print("tareplay landing --check: nothing gated changed")
        return 0
    for c in git("rev-list", f"{base}..HEAD").split():
        note = git("notes", "show", c, check=False)
        if any(ln.startswith(NOTE) and " result=pass " in ln for ln in note.splitlines()):
            later = [p for p in git("diff", "--name-only", c, "HEAD").split() if p]
            hit = [p for p in later if any(gated([p], c)[:3])]
            if hit:
                print(f"tareplay landing --check: the note on {c[:8]} predates gated changes: "
                      f"{' '.join(sorted(hit))}")
                return 1
            print(f"tareplay landing --check: pass ({c[:8]})")
            return 0
    print("tareplay landing --check: no passing replay-roundtrip note in "
          f"{base}..HEAD; run tools/replay/tareplay.py landing")
    return 1


def cmd_broad(args):
    """Every fixture, then what the tier never exercised: a feature none of its fixtures reached,
    a root or sink site no run counted, a recorded table no recording changed between T1 and
    Tlast."""
    names = list(fixtures())
    args.cover = not args.release
    if args.release:
        args.dll = args.release
    out = out_dir(args)
    args.keep = True
    try:
        run = Run(names, args, out)
    except CouldNotRun as e:
        print(f"tareplay: {e}", file=sys.stderr)
        return 2
    run.j = args.j
    run_threads(run)
    status = report(run, True)
    if STOP.is_set():
        return status or 2
    lines = []
    if not args.release:
        got = reached_of(run)
        feats, _ = tafeat.all_features()
        for f in feats:
            reached_fns = set().union(*(got.get(x, set()) for x in f["fixtures"]))
            fns = [(f["file"], fn[0]) for fn in
                   tafeat.functions((TREE / DD / f["file"]).read_text())
                   if f["line"] <= fn[1] <= f["end"]]
            if fns and not any(k in reached_fns for k in fns):
                lines.append(f"UNCOVERED feature {f['name']} ({f['file']}:{f['line']}): none of "
                             f"{' '.join(f['fixtures'])} reached it")
    sites, changed, tables = set(), set(), None
    for name, fx in run.fx.items():
        Ts = fx["Ts"]
        a, b = fx["out"] / "L" / f"dump-l{Ts[0]}.bin", fx["out"] / "L" / f"dump-l{Ts[-1]}.bin"
        if not (a.exists() and b.exists() and fx.get("rec")):
            continue
        h = tarec.header_only(str(fx["rec"]))
        _, c, _, _ = tarec.counts_of(str(b), h)
        sites |= {k[0] for k, n in c.items() if n}
        _, wa = tarec.dump_world(str(a), h)
        _, wb = tarec.dump_world(str(b), h)
        tables = tables or {ix: t["name"] for ix, t in h["tables"].items()}
        changed |= {ix for ix in wa.tables if wa.tables[ix] != wb.tables.get(ix)}
    for site in sorted(set(tarec.COUNT_ROOTS) | set(tarec.COUNT_SINKS)):
        if site not in sites:
            lines.append(f"UNCOVERED site 0x{site:06X}: no fixture counted it")
    for ix, nm in sorted((tables or {}).items()):
        if ix not in changed:
            lines.append(f"UNCOVERED table {nm}: no recording changed it between T1 and Tlast")
    text = "\n".join(lines) + ("\n" if lines else "")
    print(text or "tareplay broad: every feature, site and table exercised\n", end="")
    (out / "broad.txt").write_text(text)
    return status or (1 if lines else 0)


def cmd_features(args):
    if args.check:
        return tafeat.check(Path(args.check))
    feats, problems = tafeat.all_features()
    for f in feats:
        print(f"{f['file']}:{f['line']}-{f['end']}  {f['name']}  {f['puppet'][11:].lower()}  "
              f"{' '.join(f['fixtures'])}")
    for p in problems:
        print(p)
    return 1 if problems else 0


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
    r.add_argument("--dll", help="the build under test (default the tree's, its coverage twin "
                                 "with --cover)")
    r.add_argument("--stock-dll", help="the stock-limits build, for a fixture that names it")
    r.add_argument("--cover", action="store_true",
                   help="the coverage builds; each process writes its cover-<proc>.bin")
    r.add_argument("--keep", action="store_true", help="keep the passing dumps")
    r.add_argument("--out", help="the run's folder")
    r.set_defaults(func=cmd_run)
    ls = sub.add_parser("list", help="the fixtures")
    ls.set_defaults(func=cmd_list)
    m = sub.add_parser("measure", help="scenarios recorded and played on the coverage builds: "
                                       "the functions each reaches")
    m.add_argument("scenarios", nargs="+", help="fixture names, or scenario names in scenarios/")
    m.add_argument("-j", type=int, default=4, help="games at once (default 4)")
    m.add_argument("--end", type=int, default=1800, help="GameTime each is recorded to")
    m.add_argument("--out", help="the run's folder")
    m.set_defaults(func=cmd_measure, dll=None, stock_dll=None, keep=True)
    la = sub.add_parser("landing", help="the landing tier: the changed features' fixtures on the "
                                        "coverage builds, and their reach")
    la.add_argument("--base", default="main", help="the branch the landing goes to")
    la.add_argument("--check", action="store_true",
                    help="exit 0 when a passing replay-roundtrip note covers HEAD")
    la.add_argument("-j", type=int, default=4, help="games at once (default 4)")
    la.add_argument("--keep", action="store_true", help="keep the passing dumps")
    la.add_argument("--out", help="the run's folder")
    la.set_defaults(func=cmd_landing, dll=None, stock_dll=None)
    br = sub.add_parser("broad", help="the broad tier: every fixture, and what none exercised")
    br.add_argument("--release", metavar="DLL", help="this release DLL, without reach")
    br.add_argument("-j", type=int, default=4, help="games at once (default 4)")
    br.add_argument("--out", help="the run's folder")
    br.set_defaults(func=cmd_broad, dll=None, stock_dll=None)
    fe = sub.add_parser("features", help="the SIM_FEATURE regions of the sim files")
    fe.add_argument("--check", metavar="DIR", help="the link's check of tagpu/ddraw: exit 0 "
                                                   "clean, 1 a named problem, 2 could not run")
    fe.set_defaults(func=cmd_features)
    a = ap.parse_args(argv)
    if a.cmd in ("run", "measure", "landing", "broad") and not 1 <= a.j <= 9:
        ap.error("-j is 1..9")
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
