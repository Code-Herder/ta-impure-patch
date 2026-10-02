#!/usr/bin/env python3
"""tareplay -- the demo recorder's replay harness: each fixture recorded live, then checked on
puppets (research/notes/tadr-port/demo-recorder-completeness.md, *The harness*).

    tools/replay/tareplay.py run FIXTURE... [-j N] [--dll PATH] [--stock-dll PATH] [--keep]
                                            [--cover] [--out DIR]
    tools/replay/tareplay.py list
    tools/replay/tareplay.py landing [--base main] [--check] [-j N]   the landing tier
    tools/replay/tareplay.py broad [--release DLL] [-j N]             the broad tier
    tools/replay/tareplay.py features [--check DIR]                   the SIM_FEATURE regions
    tools/replay/tareplay.py measure SCENARIO... [--end T] [-j N]     what each one reaches
    tools/replay/tareplay.py takeover FIXTURE [--player P] [--at T]   L, K and F alone

`landing` runs the fixtures the changed SIM_FEATURE regions name (every fixture when the
recorder's own files change) on the coverage build, checks that they reached every changed
function and records a `replay-roundtrip:` note on the commit it graded; `--check` exits 0 when
such a note covers HEAD. `broad` runs every fixture and fixtures.json's `broad` entries and names
what the tier never exercised.

FIXTURE is a name in tools/replay/fixtures.json, or `all`. For each fixture four GameTimes are
drawn (one more for each round trip a broad entry's `trips` asks for beyond three), each T = 1 (mod 30) -- the radar pass's step, so a seek's contact bits agree at once (O14)
-- between 301 and 0.8 x its end, from a generator seeded by the fixture's name and HEAD, and
printed: T1 the first, T2..T4 the round trips, Tlast = T4. Four processes, each a game of its own:

    L  live      the fixture recorded, dumps l<T1> and l<Tlast>
    P  playing   `replay play` in the recording's viewed player's perspective, dumps p<T1> and
                 p<Tlast>
    R  round     the same, dump r<T1>, `roundtrip rt<T> at <T>` for T2..T4 (frame dumps
                 rt<T>-a and rt<T>-b around a rebuild in place) and a dump rt<T>c at T + 300
    Q  seek      `replay seek T1 --recording` in a fresh process, dump q<T1>

and a fifth where fixtures.json gives the fixture `"watch": true`:

    W  watch     the playing a player starts and drives (watch_lane): from the main menu's
                 REPLAYS, Watch refuses two edited copies of the recording (another layout
                 hash; a map no archive has, which the Start refuses), then plays it; the dock's
                 DKSPEED, the in-game Options' OK with the store's gamespeed unchanged, DKPLAY to
                 pause, the trigger `dock seek T1` (the dock's own seek entry), dump w<T1>,
                 DKPLAY to resume, DKLEAVE and Yes back to the browser, and a second Watch

and every check fails by name: the recording's end block and `tarec check`; L's dumps `tarec
compare`; P's, R's, Q's and W's capture dumps `tarec compare --puppet`; each round trip's line has
residue 0 and `tarec compare-dumps rt<T>-a rt<T>-b` is EQUAL; W's steps each hold what they
claim. Passing dumps are deleted unless --keep.

A fixture fixtures.json gives a `takeover` runs two processes more, K after L and F after K:

    K  takeover  `replay takeover` of L's recording at T = T2 as player P (the recording's
                 controlled player unless the entry names one) in a fresh process, which records
                 timeline 1 into the same file: the window, its boxes and ErrorLog.txt watched,
                 the entry's `order` and `type` given, a dump k<T+dump>, the level ended
    F  boundary  `replay seek T-300 --timeline 1` of K's file in a fresh process, which plays
                 across the fork at T, dump f<T+300> (or at T+dump when that comes first)

and the entry's `checks`, each failing by name: judge, line, orders, ai, fork and victory
(TAKEOVER_CHECKS states each one's claim).

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
import struct
import subprocess
import sys
import threading
import time
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
TACLI = TREE / "tools" / "tacli"
TAREC = HERE / "tarec.py"
SCENARIOS = TREE / "scenarios"
END_SCENARIO = SCENARIOS / "replay-end.json"
STOCK_FIXTURE = "stock-limits"          # the fixture that plays the stock-limits build
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
TAKEOVER_SPAN = 1800            # K's dump and the takeover's checks, this many ticks after T
TAKEOVER_BACK = 300             # F's seek, this many ticks before T, and its dump after it
TAKEOVER_CHECKS = {
    "judge": "the window alive with no box and no ErrorLog.txt, and at least 90 % of the units "
             "the takeover made seen",
    "line": "the takeover's line, no create refused, and in less than the entry's `ms`",
    "orders": "the entry's `order` unit of P's moves more than 64 wu when ordered",
    "ai": "every AI seat that made a unit in the original's [T, T + TAKEOVER_SPAN] makes one "
          "in timeline 1's (a seat that made none in the original is skipped and named), and at "
          "least 25 % of the AI seats' mobile units move more than 8 wu in timeline 1",
    "fork": "compare-fork EQUAL, the original's bytes untouched, both timelines check, "
            "timeline 1 complete, and K's and F's dumps EQUAL",
    "victory": "timeline 1 ends complete with P's seat won, within TAKEOVER_SPAN of T",
}
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
        self.last_collection = None
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
        # Exit flushes coverage after its workers finish. Collect that final bitmap and log,
        # rather than the one taken while those workers were still running.
        if self.last_collection:
            rd = self.gamedir / "replays"
            for f in rd.glob("cover-*.bin") if rd.is_dir() else []:
                shutil.copy2(f, self.last_collection / f.name)
            (self.last_collection / "tagpu.log").write_text(talog.run_text(self.gamedir))

    def place(self, fix, dll):
        """The DLL and this fixture's content in the game folder, before a launch: every fixture's
        content files removed first, so one fixture's units never reach another's game."""
        self.last_collection = None
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
        self.last_collection = into
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


def broad_fixtures():
    """The broad tier's own entries, which no landing runs: a fixture with `trips` round trips,
    and `cost` games recorded live for the recorder's cost and the file's size alone."""
    return json.loads((HERE / "fixtures.json").read_text())["broad"]


def procs(fix):
    """The processes a fixture runs: L, P, R and Q; K and F where fixtures.json gives it a
    takeover; W where it asks for the browser's Watch."""
    return (("L", "P", "R", "Q") + (("K", "F") if fix.get("takeover") else ()) +
            (("W",) if fix.get("watch") else ()) + (("X",) if fix.get("forks") else ()))


def head():
    r = subprocess.run(["git", "-C", str(TREE), "rev-parse", "HEAD"], capture_output=True,
                       text=True)
    return r.stdout.strip() or "no-head"


def gametimes(name, end, h, trips=3):
    """T1 and `trips` round trips: T = 1 (mod 30) between 301 and 0.8 x end, with room for the
    dump after a round trip, sorted, from a generator seeded by the fixture and HEAD."""
    hi = min(int(0.8 * end), end - CONTINUE - 30)
    pool = list(range(301, hi + 1, 30))
    if len(pool) < trips + 1:
        raise CouldNotRun(f"{name}: end {end} leaves fewer than {trips + 1} GameTimes")
    rng = random.Random(int(hashlib.sha1((name + h).encode()).hexdigest()[:8], 16))
    return sorted(rng.sample(pool, trips + 1))


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
        # the chat line: one return opens it, the characters, return sends it; a second return
        # straight after the first closes the line again and the characters land as hotkeys. A
        # second between the three, since back to back the opening return can be dropped. A
        # space goes as the `space` key: `char: ` is no token the input layer types (tacli,
        # _ui_text_tokens)
        tacli("keys", w.name, "return")
        time.sleep(1.0)
        tacli("keys", w.name, *["space" if c == " " else f"char:{c}" for c in arg])
        time.sleep(1.0)
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
                  "--restart", "--keep-dll", "--unit-limit", str(fix.get("unit_limit", UNIT_LIMIT)),
                  timeout=LAUNCH_TIMEOUT)
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


def compat():
    """tacompat's window helpers, the ones the compatibility suite judges a game by"""
    sys.path.insert(0, str(TREE / "tools" / "compat"))
    import tacompat  # noqa: E402
    return tacompat


def tk_point(fx, h, player=None, at=None):
    """(T, P) of a fixture's takeover: T2, or `at`; the entry's player, the recording's
    controlled player for "controlled", or `player`."""
    tk = fx["fix"]["takeover"]
    T = at if at is not None else fx["Ts"][1]
    P = player if player is not None else tk["player"]
    if P == "controlled":
        P = h["options"]["controlled"]
    return T, int(P)


def takeover_k(w, fx, out):
    """K: L's recording taken over at T as player P in a fresh process; the game after T is
    timeline 1 of the same file. Returns what the takeover's checks read."""
    tc = compat()
    fix, tk = fx["fix"], fx["fix"]["takeover"]
    T, P = fx["tk_T"], fx["tk_P"]
    span = tk.get("dump", TAKEOVER_SPAN)
    res = {"T": T, "P": P, "boxes": [], "errorlog": False, "gone": False}
    w.place(fix, fx["dll"])
    (w.gamedir / "ErrorLog.txt").unlink(missing_ok=True)
    log = Log(w)
    # the dump is armed on the playing before the takeover, so an entry whose level ends soon
    # after T (`dump`) takes it before the end whatever tacli's latency
    dump = f"k{T + span}"
    rc, o = tacli("replay", "takeover", w.name, fx["rec"], "--at", T, "--player", P,
                  "--dump", dump, "--dump-at", T + span,
                  "--defaults", "--restart", "--keep-dll", "--timeout", 300, "--json",
                  timeout=LAUNCH_TIMEOUT + 300)
    try:
        got = json.loads(o[o.index("{"):])
    except ValueError:
        raise CouldNotRun(f"replay takeover: {last(o)}")
    if rc or not got.get("taken") or "recording" not in got:
        why = got.get("line") or got.get("not_recorded") or got.get("timeout") or last(o)
        raise CouldNotRun(f"replay takeover: {why}")
    res.update(got)
    t_line = time.time()
    if fx["cover"]:
        cover_ask(w, log, "K")
    # what Impure draws 4 s after the line, against what the takeover made (judge)
    nap(max(0.0, t_line + 4 - time.time()))
    res["alive_seen"] = tc.alive_seen(w.gamedir)
    if tk.get("switches"):
        rc, o = tacli("switches", w.name, *[f"{k}={'on' if v else 'off'}"
                                            for k, v in tk["switches"].items()], timeout=60)
        if rc:
            raise CouldNotRun(f"switches: {last(o)}")
    if tk.get("type"):
        step(w, "type", tk["type"])
    if tk.get("order"):
        res["ordered"] = tk_order(w, P, tk["order"])
    display = int(w.xvfb.display.lstrip(":"))
    seen = set()
    t0 = time.time()
    deadline = t0 + 120 + 4 * span / 30
    while True:
        tc.new_boxes(display, seen, res["boxes"], t0)
        if (w.gamedir / "ErrorLog.txt").exists():
            res["errorlog"] = True
        if not any(tc.main_window(*x) for x in tc.x_windows(display)):
            res["gone"] = True
            break
        log.read()
        if any(re.search(rf"recorder: dump {dump} (at GameTime|for GameTime \d+ not taken|refused)"
                         rf"|recorder: writer: .* closed", ln) for ln in log.lines):
            break
        if time.time() > deadline:
            raise CouldNotRun(f"K: no dump {dump} and no end in {deadline - t0:.0f} s")
        nap(2.0)
    if not res["gone"]:
        # a takeover that checks the victory ends by its own chat line (the entry's `type`); any
        # other, by the end scenario, as L ends
        victory = "victory" in tk.get("checks", [])
        n = 0 if victory else len(log.lines)
        if not victory:
            rc, o = tacli("scenario", "apply", w.name, END_SCENARIO, timeout=300)
            if rc:
                raise CouldNotRun(f"apply {END_SCENARIO.name}: {last(o)}")
        if not log.wait(r"recorder: writer: .* closed", 120, n):
            raise CouldNotRun("K: the fork's file did not close")
    w.collect(out, recordings=True)
    w.stop()
    recs = sorted(out.glob("*.imprec"))
    if not recs:
        raise CouldNotRun("no .imprec in K's replays folder")
    res["file"] = str(recs[-1])
    return res


def tk_order(w, P, order):
    """The `order` unit of P's (type, rank) moved 300 wu east: its slot and the GameTime read
    just before the order was given, from which the `orders` check reads the timeline, so the
    order's own latency counts against its 10 s."""
    kind, rank = order
    rc, o = tacli("roster", w.name, "--json", timeout=60)
    roster = json.loads(o)["units"] if not rc else []
    units = [u for u in roster if u["owner"] == P and u["type"].upper() == kind and
             u["engine_index"]]
    if len(units) <= rank:
        raise CouldNotRun(f"order: no {kind} of rank {rank} among player {P}'s units")
    u = units[rank]
    x, z = u["world"]
    g = w.gametime()
    rc, o = tacli("order", w.name, "move", "pos", x + 300, z, "--unit", u["engine_index"],
                  "--expect", kind, timeout=60)
    if rc:
        raise CouldNotRun(f"order move {kind}: {last(o)}")
    return {"slot": u["engine_index"], "at": g}


def takeover_f(w, fx, out):
    """F: K's file played from T-300 in timeline 1 in a fresh process, across the fork at T, to
    the dump f<T+300>."""
    T, P, tl = fx["tk_T"], fx["tk_P"], fx["tk"]["timeline"]
    at = T + min(TAKEOVER_BACK, fx["fix"]["takeover"].get("dump", TAKEOVER_SPAN))
    w.place(fx["fix"], fx["dll"])
    log = Log(w)
    rc, o = tacli("replay", "seek", w.name, T - TAKEOVER_BACK, "--recording", fx["tk"]["file"],
                  "--timeline", tl, "--defaults", "--restart", "--keep-dll", "--perspective", P,
                  "--timeout", 300, timeout=LAUNCH_TIMEOUT + 300)
    if rc:
        w.collect(out)
        w.stop()
        raise CouldNotRun(f"replay seek {T - TAKEOVER_BACK} --timeline {tl}: {last(o)}")
    arm_dump(w, log, f"f{at}", at)
    wait_dumps(log, [f"f{at}"], 600)
    if fx["cover"]:
        cover_ask(w, log, "F")
    w.collect(out)
    w.stop()
    return at


# ---- W: the browser's Watch and the dock --------------------------------------------------

def header_copy(rec, dst, tag, edit):
    """A copy of the recording `rec` at `dst` whose header entry `tag` holds edit(its bytes), of
    the same length, the header block deflated again (or stored again) with both its CRCs: a
    file the reader opens, which the replay's own checks then judge."""
    data = Path(rec).read_bytes()
    if data[:8] != tarec.REC_MAGIC:
        raise CouldNotRun(f"{rec}: not a recording")
    head = data[8:40]
    magic, kind, flags, first, upto, _raw, packed, _crc_body, _crc_head = \
        struct.unpack("<IHHIIIIII", head)
    if magic != tarec.REC_BLOCK_MAGIC or kind != tarec.RB_HEADER:
        raise CouldNotRun(f"{rec}: no header block")
    body = data[40:40 + packed]
    payload = bytearray(body if flags & tarec.RBF_STORED else zlib.decompress(body))
    at, found = 0, False
    while at + 4 <= len(payload):
        t, n = struct.unpack_from("<HH", payload, at)
        if t == tag:
            new = edit(bytes(payload[at + 4:at + 4 + n]))
            if len(new) != n:
                raise CouldNotRun(f"{rec}: an edit of header entry {tag} changed its length")
            payload[at + 4:at + 4 + n] = new
            found = True
        at += (4 + n + 3) & ~3
    if not found:
        raise CouldNotRun(f"{rec}: no header entry {tag}")
    body = bytes(payload) if flags & tarec.RBF_STORED else zlib.compress(bytes(payload))
    head = struct.pack("<IHHIIIII", magic, kind, flags, first, upto, len(payload), len(body),
                       zlib.crc32(body))
    head += struct.pack("<I", zlib.crc32(head))
    Path(dst).write_bytes(data[:8] + head + body + data[40 + packed:])


def other_layout(b):
    """replay-layout.tsv's SHA-256 in hex, one digit changed: another exclusion file's."""
    s = bytearray(b)
    s[0] = ord("1") if s[0] != ord("1") else ord("2")
    return bytes(s)


def no_such_map(b):
    """The skirmish settings' map, every character of its name replaced: a map no archive has."""
    return bytes(ord("Q") if c else 0 for c in b)


SPEED_CEIL = "*0x511DE8+0x38A4B:2"     # the game speed's ceiling, which the settings save stores
SPEED_LIVE = "*0x511DE8+0x38A4D:2"
PAUSE_WORD = "*0x511DE8+0x38A51:2"     # bit 0: paused
TERRAIN = "The terrain for the selected map does not exist."    # the Start's own refusal


def peek(w, spec):
    rc, o = tacli("peek", w.name, spec, "--json", timeout=60)
    if rc:
        raise CouldNotRun(f"tacli peek {spec}: {last(o)}")
    return int(str(json.loads(o[o.index("{"):])["peeks"][0]["value"]).split()[0])


def store_speed(w):
    """The game speed the player's settings hold in the instance's registry store, which the
    store rewrites on every change (tagpu_regstore.h)."""
    f = w.gamedir / "tacli-state" / "registry.txt"
    for ln in f.read_text(errors="replace").splitlines() if f.exists() else []:
        parts = ln.split("\t")
        if len(parts) == 4 and parts[1] == "gamespeed":
            return int(parts[3])
    raise CouldNotRun(f"no gamespeed in {f}")


def ui(w, *args, timeout=60):
    rc, o = tacli("ui", w.name, *args, timeout=timeout + 30)
    if rc:
        raise CouldNotRun(f"tacli ui {' '.join(map(str, args))}: {last(o)}")
    return o


def ui_on(w, gui, timeout):
    ui(w, "wait", "--gui", gui, "--timeout", timeout, timeout=timeout)


def browser_watch(w, log, row, name):
    """Row `row` of the Replays browser selected and Watch pressed; the line that judged it:
    `replay: playing <name>` or `replay: not playing <name>: <why>`. The browser names the file
    it took, which must be `name`'s: the rows are newest first, as `watch_lane` dated them."""
    n = len(log.lines)
    ui(w, "select", "RECS", f"#{row}")
    ui(w, "click", "WATCH")
    took = log.wait(r"replays: Watch \S+", 30, n)
    if not took or not took.rstrip().endswith(f" {name}.imprec"):
        raise CouldNotRun(f"the browser's row {row} is not {name}.imprec: "
                          f"{took.strip() if took else 'no Watch line in 30 s'}")
    got = log.wait(rf"replay: (playing|not playing) {re.escape(name)}:", 240, n)
    if not got:
        raise CouldNotRun(f"Watch {name}: no verdict in 240 s")
    return got[got.index("replay:"):].strip()


def index_lines(rd):
    """replays\\index.dat's lines by file name (tagpu_replays.c, the index's format)."""
    f = rd / "index.dat"
    lines = f.read_text(errors="replace").splitlines()[1:] if f.exists() else []
    return {ln.split("\t", 1)[0]: ln for ln in lines if "\t" in ln}


def watch_button(w):
    """The Replays browser's Watch button's state as `tacli ui` shows it: ok or grayed."""
    m = re.search(r"\bbutton\s+WATCH\s+\S+\s+(\S+)", ui(w))
    if not m:
        raise CouldNotRun("no WATCH button on the screen")
    return m.group(1)


def watch_lane(w, fx, rec, out, T):
    """W: the playback started as a player starts it, through the Replays browser's Watch, and
    driven through the dock: two copies of the recording refused, the data refusal's marked and the
    missing map's not, then the recording watched, its
    speed changed on the dock, the in-game Options' OK (the settings save), a pause, the dock's
    seek to T with dump w<T>, the resume, Leave, and a second Watch from the browser it returns to.
    Returns its own checks, (name, verdict, detail) each; a claim that does not hold is Failed."""
    fix, stem, res = fx["fix"], fx["name"], []
    w.stop()
    w.place(fix, fx["dll"])
    rd = w.gamedir / "replays"
    rd.mkdir(exist_ok=True)
    (rd / "index.dat").unlink(missing_ok=True)     # no mark of an earlier run's copies
    copies = [(f"{stem}-layout", tarec.RT_LAYOUT, other_layout,
               "it was recorded with another exclusion file (replay-layout.tsv)"),
              (f"{stem}-map", tarec.RT_SETTINGS_MAP, no_such_map, TERRAIN)]
    now = time.time()
    for k, (name, tag, edit, _) in enumerate(copies):
        header_copy(rec, rd / f"{name}.imprec", tag, edit)
        os.utime(rd / f"{name}.imprec", (now - 60 * k, now - 60 * k))
    shutil.copyfile(rec, rd / f"{stem}.imprec")
    os.utime(rd / f"{stem}.imprec", (now - 60 * len(copies),) * 2)
    log = Log(w)
    rc, o = tacli("launch", w.name, "--defaults", "--keep-dll", timeout=LAUNCH_TIMEOUT)
    if rc:
        raise CouldNotRun(f"tacli launch: {last(o)}")
    ui_on(w, "MAINMENU.GUI", 120)
    ui(w, "click", "REPLAYS")
    ui_on(w, "REPLAYS.GUI", 30)

    # the refusals: the header's check (watch_refuse), then the Start's own, past the undoable
    # steps (watch_undo); each notice answered, back to the browser
    for row, (name, _, _, why) in enumerate(copies):
        got = browser_watch(w, log, row, name)
        if got != f"replay: not playing {name}: {why}":
            raise Failed(f"Watch {name}: {got} (expected the refusal: {why})")
        ui_on(w, "MSGBOX.GUI", 30)
        ui(w, "click", "OK")
        ui_on(w, "REPLAYS.GUI", 30)
        res.append((f"W: Watch refuses {name}", PASS, got))

    # the marks the refusals left (tagpu_replays.c, THE LOCK): the other exclusion file is a data
    # refusal, marked in the index and Watch grey on its row; a missing map never marks
    idx = index_lines(rd)
    for row, (name, tag, _, _) in enumerate(copies):
        line = idx.get(f"{name}.imprec", "")
        mark = re.search(r"\tL\t(\d+)\t", line)
        ui(w, "select", "RECS", f"#{row}")
        state = watch_button(w)
        want = tag == tarec.RT_LAYOUT
        if want and (not mark or mark.group(1) != "4" or state != "grayed"):
            raise Failed(f"{name}: the refusal left mark {mark.group(1) if mark else 'none'} "
                         f"and Watch {state}; expected mark 4 and Watch grayed")
        if not want and (mark or state != "ok"):
            raise Failed(f"{name}: a missing map left mark {mark.group(1) if mark else 'none'} "
                         f"and Watch {state}; expected none and Watch ok")
        res.append((f"W: {name} {'marked' if want else 'not marked'}", PASS,
                    f"index mark {mark.group(1) if mark else 'none'}, Watch {state}"))

    stored, ceil0 = store_speed(w), peek(w, SPEED_CEIL)
    got = browser_watch(w, log, len(copies), stem)
    if not got.startswith(f"replay: playing {stem}:"):
        raise Failed(f"Watch {stem}: {got}")
    ui_on(w, "DOCK.GUI", 60)
    res.append((f"W: Watch plays {stem}", PASS, got))

    # the dock's speed, then the settings save with it changed: the store keeps the player's
    ui(w, "click", "DKSPEED")
    nap(1.0)
    ceil1, live1 = peek(w, SPEED_CEIL), peek(w, SPEED_LIVE)
    if ceil1 == ceil0 or ceil1 not in (5, 10, 20, 40, 80) or live1 != ceil1:
        raise Failed(f"the dock's speed: {ceil0} became ceiling {ceil1}, live {live1}")
    rc, o = tacli("keys", w.name, "tab", timeout=60)
    if rc:
        raise CouldNotRun(f"tacli keys tab: {last(o)}")
    ui_on(w, "ARMOPT.GUI", 20)
    ui(w, "click", "PREFS")
    ui_on(w, "PREFS.GUI", 20)
    ui(w, "click", "PREV")
    ui_on(w, "ARMOPT.GUI", 20)
    after, ceil2 = store_speed(w), peek(w, SPEED_CEIL)
    if after != stored or ceil2 != ceil1:
        raise Failed(f"the settings save during the playing: the store's gamespeed {stored} -> "
                     f"{after}, the ceiling {ceil1} -> {ceil2}")
    ui(w, "click", "OK")
    ui_on(w, "DOCK.GUI", 20)
    res.append(("W: the dock's speed and the settings save", PASS,
                f"speed {ceil0} -> {ceil1} on the dock; the save stored gamespeed {after}, the "
                f"player's, and left the playing at {ceil2}"))

    # pause, the dock's seek to T with its dump, resume
    ui(w, "click", "DKPLAY")
    nap(1.0)
    g1 = w.gametime()
    nap(1.5)
    if not peek(w, PAUSE_WORD) & 1 or w.gametime() != g1:
        raise Failed(f"the dock's pause: the pause word or GameTime moved (at {g1})")
    n = len(log.lines)
    w.trigger(f"dock seek {T}")
    got = log.wait(rf"replay: {re.escape(stem)}: (at GameTime {T}, sought from|seek to {T} "
                   rf"refused)|a seek to {T} from the dock refused", 60, n)
    if not got or "sought from" not in got:
        raise Failed(f"the dock's seek to {T}: {got.strip() if got else 'no line in 60 s'}")
    arm_dump(w, log, f"w{T}", T)
    ui(w, "click", "DKPLAY")
    wait_dumps(log, [f"w{T}"], 120)
    if peek(w, PAUSE_WORD) & 1:
        raise Failed("the dock's resume: the pause word still set")
    res.append(("W: the dock's pause, seek and resume", PASS, got[got.index("replay:"):].strip()))

    # Leave: the teardown puts the player's speed back, and the browser returns
    n = len(log.lines)
    ui(w, "click", "DKLEAVE")
    ui_on(w, "YESORNO.GUI", 20)
    ui(w, "click", "CHOICE1")
    if not log.wait(rf"replay: {re.escape(stem)} at its level's end", 120, n):
        raise CouldNotRun("Leave: no level's end line in 120 s")
    ui_on(w, "REPLAYS.GUI", 120)
    back, ceil3 = store_speed(w), peek(w, SPEED_CEIL)
    if back != stored or ceil3 != ceil0:
        raise Failed(f"after Leave: the store's gamespeed {back}, the ceiling {ceil3}; the "
                     f"player's were {stored} and {ceil0}")
    res.append(("W: Leave", PASS, f"the browser back; gamespeed {back} stored, the ceiling "
                                  f"{ceil3}, the player's"))

    # a second Watch from the browser Leave returned to; its level writes the coverage, which
    # holds the first level's teardown too (the bitmap is the process's)
    got = browser_watch(w, log, len(copies), stem)
    if not got.startswith(f"replay: playing {stem}:"):
        raise Failed(f"the second Watch: {got}")
    if fx["cover"]:
        cover_ask(w, log, "W")
    w.collect(out)
    w.stop()
    return res


def forks_lane(w, fx, out):
    """L's recording, four root forks, an LRU touch, a fork of a fork, parent deletion,
    chain playback/export, and deletion of the highest ID followed by another fork."""
    import hashlib
    out.mkdir(parents=True, exist_ok=True)
    source = out / "forks.imprec"
    shutil.copy2(fx["rec"], source)
    original, _ = tarec.file_segments(source)
    lo, hi = original[0]["range"]
    original_hash = hashlib.sha256(source.read_bytes()[lo:hi]).hexdigest()
    results = []
    P = tarec.header_only(source)["options"]["controlled"]
    base = max(601, fx["Ts"][0] + 120)
    if base + 900 >= fx["fix"]["end"]:
        base = 301

    def check_file(path, label):
        segs, stop = tarec.file_segments(path)
        if stop:
            raise Failed(f"{label}: {stop}")
        live = [s for s in segs if s["state"] == 1]
        if len([s for s in live if s["id"]]) > 3:
            raise Failed(f"{label}: more than three live forks")
        for s in live:
            rc, detail = tarec_run("check", path, "--timeline", s["id"])
            if rc:
                raise Failed(f"{label}: timeline {s['id']}: {last(detail)}")
        if path == source:
            lo, hi = segs[0]["range"]
            if hashlib.sha256(path.read_bytes()[lo:hi]).hexdigest() != original_hash:
                raise Failed(f"{label}: original blocks changed")
        results.append((f"forks: {label}", PASS,
                        f"live {[s['id'] for s in live]}, physical {[s['id'] for s in segs]}"))
        return segs

    def finish_play(log, tl):
        n = len(log.lines)
        tacli("keys", w.name, "tab", "tab")
        ui(w, "click", "EXIT")
        ui(w, "click", "MAINMENU")
        tacli("keys", w.name, "y", "y")
        if not log.wait(r"replay: .* at its level's end", 120, n):
            raise CouldNotRun("forks: playback did not leave")
        if not log.wait(rf"replay: forks .*: done: touched timeline {tl}", 120, n):
            raise Failed(f"forks: timeline {tl}'s playing did not touch its last-used time")

    def fork(tid, T, parent=0):
        say(f"forks: K{tid} from {parent} at {T}")
        w.place(fx["fix"], fx["dll"])
        log = Log(w)
        rc, text = tacli("replay", "takeover", w.name, source, "--timeline", parent,
                         "--at", T, "--player", P, "--dump", f"fork{tid}", "--dump-at", T + 450,
                         "--defaults", "--restart", "--keep-dll", "--json", "--timeout", 300,
                         timeout=LAUNCH_TIMEOUT + 300)
        if rc:
            raise Failed(f"forks: K{tid}: {last(text)}")
        got = json.loads(text[text.index("{"):])
        if got.get("timeline") != tid:
            raise Failed(f"forks: K{tid} allocated {got.get('timeline')}")
        if fx["cover"]:
            cover_ask(w, log, f"K{tid}")
        wait_dumps(log, [f"fork{tid}"], 180)
        n = len(log.lines)
        rc, text = tacli("scenario", "apply", w.name, END_SCENARIO, timeout=300)
        if rc or not log.wait(r"recorder: writer: .* closed", 120, n):
            raise CouldNotRun(f"forks: K{tid} did not end: {last(text)}")
        if not log.wait(r"replay: forks .*: (done|refused):", 120, n):
            raise CouldNotRun(f"forks: K{tid} did not complete maintenance")
        w.collect(out / f"K{tid}", recordings=True)
        w.stop()
        shutil.copy2(Path(got["fork"]), source)
        check_file(source, f"K{tid}")

    def start_play(tl, T):
        say(f"forks: playing {tl} from {T}")
        w.place(fx["fix"], fx["dll"])
        log = Log(w)
        rc, text = tacli("replay", "seek", w.name, T, "--recording", source,
                         "--timeline", tl, "--defaults", "--restart", "--keep-dll",
                         "--perspective", P, "--timeout", 300, timeout=LAUNCH_TIMEOUT + 300)
        if rc:
            raise Failed(f"forks: play timeline {tl}: {last(text)}")
        if fx["cover"]:
            cover_ask(w, log, f"X{tl}")
        return log

    def command(action, tid, export=None, expect=True):
        say(f"forks: {action} {tid}" + (f" to {export}" if export else ""))
        rec = w.gamedir / "replays" / "forks.imprec"
        argv = ["replay", "forks", w.name, rec, action, tid]
        if export:
            argv.append(export)
        rc, text = tacli(*argv, "--json", timeout=180)
        if (rc == 0) != expect:
            raise Failed(f"forks {action} {tid}: {last(text)}")
        if not expect and "refused" not in text and '"done": false' not in text:
            raise Failed(f"forks {action} {tid}: refusal not named: {last(text)}")
        return text

    for tid in (1, 2, 3):
        fork(tid, base + (tid - 1) * 300)
    before = next(s["last_used"] for s in tarec.file_segments(source)[0] if s["id"] == 2)
    log = start_play(2, base + 310)
    finish_play(log, 2)
    w.collect(out / "touch")
    w.stop()
    shutil.copy2(w.gamedir / "replays" / "forks.imprec", source)
    used = next(s["last_used"] for s in tarec.file_segments(source)[0] if s["id"] == 2)
    if used <= before:
        raise Failed("forks: playing did not advance last-used time")
    rc, text = tacli("replay", "takeover", w.name, source, "--at", base + 900,
                     "--player", P, "--dry-run", "--json")
    if rc or json.loads(text[text.index("{"):])["would_delete"] != 1:
        raise Failed(f"forks: dry-run did not predict ID 1: {text}")
    fork(4, base + 900)
    if any(s["id"] == 1 for s in tarec.file_segments(source)[0]):
        raise Failed("forks: LRU 1 was not removed by compaction")
    T5 = base + 451
    fork(5, T5, 2)
    log = start_play(5, T5 + 1)
    # Timeline IDs are local to their file: a playing protects only that recording.
    other = w.gamedir / "replays" / "other.imprec"
    shutil.copy2(source, other)
    rc, text = tacli("replay", "forks", w.name, other, "delete", 5, "--json", timeout=180)
    if rc:
        raise Failed(f"forks: deletion in a different recording: {last(text)}")
    other_segs = check_file(other, "deleted ID 5 in a different recording")
    if any(s["id"] == 5 and s["state"] == 1 for s in other_segs):
        raise Failed("forks: other recording's timeline 5 stayed live")
    playing = tarec.file_segments(w.gamedir / "replays" / "forks.imprec")[0]
    if not any(s["id"] == 5 and s["state"] == 1 for s in playing):
        raise Failed("forks: another recording's deletion changed the current playing")
    command("delete", 5, expect=False)                # the playing is protected
    finish_play(log, 5)
    oldsize = (w.gamedir / "replays" / "forks.imprec").stat().st_size
    command("delete", 2)
    shutil.copy2(w.gamedir / "replays" / "forks.imprec", source)
    segs = check_file(source, "deleted parent")
    ancestor = next(s for s in segs if s["id"] == 2)
    if ancestor["state"] != 2 or ancestor["end_reason"] != 12 or ancestor["last"] < T5 - 1:
        raise Failed("forks: parent prefix was not preserved and pruned")
    if source.stat().st_size >= oldsize:
        raise Failed("forks: parent deletion reclaimed no bytes")
    w.collect(out / "delete")
    w.stop()
    log = start_play(5, base - 120)
    arm_dump(w, log, "chain5", T5 + 300)
    wait_dumps(log, ["chain5"], 180)
    dump = w.gamedir / "replays" / "dump-chain5.bin"
    rc, text = tarec_run("compare", "--puppet", source, dump, "--timeline", 5)
    if rc:
        raise Failed(f"forks: descendant across two boundaries: {last(text)}")
    finish_play(log, 5)
    command("export", 5, "export5")
    command("export", 5, "export5", expect=False)     # an existing export is never overwritten
    exported = out / "export5.imprec"
    shutil.copy2(w.gamedir / "replays" / "export5.imprec", exported)
    segs = check_file(exported, "exported chain")
    if [s["id"] for s in segs] != [0, 2, 5]:
        raise Failed("forks: export holds something besides the chosen chain")
    w.collect(out / "chain")
    w.stop()
    saved = source
    source = exported
    log = start_play(5, base - 120)
    arm_dump(w, log, "export5", T5 + 300)
    wait_dumps(log, ["export5"], 180)
    rc, text = tarec_run("compare", "--puppet", exported,
                        w.gamedir / "replays" / "dump-export5.bin", "--timeline", 5)
    if rc:
        raise Failed(f"forks: exported chain playback: {last(text)}")
    w.collect(out / "export-play")
    w.stop()
    source = saved
    # Removing the highest allocated segment must not permit that ID to be allocated again.
    w.place(fx["fix"], fx["dll"])
    rc, text = tacli("launch", w.name, "--defaults", "--keep-dll", timeout=120)
    if rc:
        raise CouldNotRun(f"forks: shell: {last(text)}")
    (w.gamedir / "replays").mkdir(exist_ok=True)
    shutil.copy2(source, w.gamedir / "replays" / "forks.imprec")
    command("delete", 5)
    shutil.copy2(w.gamedir / "replays" / "forks.imprec", source)
    w.collect(out / "highest")
    w.stop()
    fork(6, base + 600)
    results.append(("forks: two-boundary and exported puppet dumps", PASS,
                    "both EQUAL; highest deleted ID 5 followed by ID 6"))
    return results


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
    res = list(fx.get("xres", []))
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
    if ran.get("W") == "ran":
        res += fx["wres"]
        d = out / "W" / f"dump-w{Ts[0]}.bin"
        compare(f"W w{Ts[0]} (the dock's seek, in a playing Watch started)", "compare",
                "--puppet", rec, d, files=[d])
    return res


def mobile(img):
    """the unit has a mover: its image's first dword is 1 then (replay-layout.tsv, `mover`)"""
    return struct.unpack_from("<I", img, 0)[0] == 1


def tk_ai(path, tl, T, stop):
    """In timeline `tl` over [T, stop]: its AI seats, those of them that made a unit, and the
    fraction of the AI seats' mobile units alive at T that moved more than 8 wu (None when there
    are none) with their count."""
    h, bl, _ = tarec.load(path, timeline=tl)
    seg = [s for s in tarec.file_segments(path)[0] if s["id"] == tl][0]
    ai = {p["index"] for p in seg["header"]["players"] if p["controller"] == 2}
    w0 = tarec.stream_to(h, bl, want=T)
    start = {slot: struct.unpack_from("<iii", img, 0x6A)
             for slot, img in w0.units.items() if img[0xFF] in ai and mobile(img)}
    moved, made, fresh = set(), set(), set()

    def on_record(w, kind, b):
        if kind == tarec.RK_EV_CREATE and w.tick is not None and w.tick >= T:
            fresh.add(struct.unpack_from("<I", b)[0] - 1)
        if kind is not None or w.tick is None or w.tick < T:
            return
        for slot in list(fresh):
            if slot in w.units:
                made.add(w.units[slot][0xFF])
                fresh.discard(slot)
        for slot, (x, _, z) in start.items():
            img = w.units.get(slot)
            if slot in moved or img is None:
                continue
            x1, _, z1 = struct.unpack_from("<iii", img, 0x6A)
            if ((x1 - x) ** 2 + (z1 - z) ** 2) ** 0.5 > 8 * 65536:
                moved.add(slot)
    tarec.stream_to(h, bl, want=stop, on_record=on_record)
    return ai, made & ai, (len(moved) / len(start) if start else None), len(start)


def ai_compare(ai, original, fork):
    """The `ai` check's comparison, over the AI seats `ai` of timeline 1: the seats that made a
    unit in the original's window (`original`) are compared, each must have made one in timeline
    1's (`fork`); a seat that made none in the original is skipped. (compared, missing, skipped),
    each sorted: compared empty is no comparison, missing non-empty a failure."""
    compared = sorted(s for s in ai if s in original)
    return compared, [s for s in compared if s not in fork], sorted(s for s in ai
                                                                   if s not in original)


def tk_moved(path, tl, ordered):
    """How far the ordered unit is, in timeline `tl`, at the GameTime read before its order and
    300 ticks (10 s) later, in wu: (distance, None), or (None, why)."""
    slot, g = ordered["slot"], ordered["at"]
    h, bl, _ = tarec.load(path, timeline=tl)
    seen = {}

    def on_record(w, kind, b):
        # kind None comes before a TICK, with the world still the iteration before it
        if kind is None and w.tick == g and slot in w.units:
            seen[g] = struct.unpack_from("<iii", w.units[slot], 0x6A)

    w = tarec.stream_to(h, bl, want=g + 300, on_record=on_record)
    if w.tick == g + 300 and slot in w.units:
        seen[g + 300] = struct.unpack_from("<iii", w.units[slot], 0x6A)
    if len(seen) < 2:
        return None, f"unit {slot} is not in timeline {tl} at GameTime {g} and {g + 300}"
    (x0, _, z0), (x1, _, z1) = seen[g], seen[g + 300]
    return ((x1 - x0) ** 2 + (z1 - z0) ** 2) ** 0.5 / 65536, None


def tk_checks(fx):
    """The takeover's checks, as the entry's `checks` lists them: (name, verdict, detail) each."""
    tk, ran, out = fx["fix"]["takeover"], fx["procs"], fx["out"]
    want = tk.get("checks", [])
    k = fx.get("tk") or {}
    res = []

    def unrun(name, why):
        res.append((f"takeover {name}", UNRUN, why))

    if ran.get("K") != "ran":
        for name in want:
            unrun(name, "K did not run")
        return res
    T, P, path, tl = k["T"], k["P"], k["file"], k["timeline"]
    if "judge" in want:
        seen, made = k.get("alive_seen"), k.get("made", 0)
        bad = ([f"{len(k['boxes'])} boxes: {', '.join(b['title'] for b in k['boxes'])}"]
               if k["boxes"] else []) + (["ErrorLog.txt"] if k["errorlog"] else []) + \
              (["the window went away"] if k["gone"] else [])
        if seen is None:
            res.append(("takeover judge", UNRUN if not bad else FAIL,
                        "; ".join(bad + ["no `units: alive=` line"])))
        else:
            if seen < 0.9 * made:
                bad.append(f"{seen} units seen of the {made} made")
            res.append(("takeover judge", FAIL if bad else PASS,
                        "; ".join(bad) or f"{seen} units seen of the {made} made"))
    if "line" in want:
        rc, o = tarec_run("segments", path)
        size = re.search(rf"timeline {tl}: bytes (\d+) to (\d+)", o)
        log = (out / "K" / "tagpu.log").read_text(errors="replace")
        first = re.search(r"recorder: \d+ iterations captured: the first in (\d+) us", log)
        bad = []
        if k["units_refused"]:
            bad.append(f"{k['units_refused']} creates refused")
        if tk.get("ms") and k["ms"] >= tk["ms"]:
            bad.append(f"{k['ms']} ms, the entry's bound {tk['ms']}")
        res.append(("takeover line", FAIL if bad else PASS,
                    "; ".join(bad) or
                    f"{k['ms']} ms (passability {k['passability_ms']}, AI {k['ai_ms']}); the "
                    f"fork's first iteration "
                    f"{first.group(1) + ' us' if first else 'not in the log'}, its segment "
                    f"{int(size.group(2)) - int(size.group(1)) if size else '?'} bytes; {k['line']}"))
    if "orders" in want:
        try:
            d, why = tk_moved(path, tl, k["ordered"]) if k.get("ordered") else (None, "no order")
        except tarec.Unreadable as e:
            d, why = None, str(e)
        res.append(("takeover orders", UNRUN if d is None else PASS if d > 64 else FAIL,
                    why if d is None else
                    f"unit {k['ordered']['slot']} moved {d:.0f} wu from GameTime "
                    f"{k['ordered']['at']} to {k['ordered']['at'] + 300}"))
    if "ai" in want:
        try:
            # one window for both timelines, ending where the shorter one does, so neither's
            # seats are given more ticks to make a unit than the other's
            segs = {s["id"]: s for s in tarec.file_segments(path)[0]}
            parent = segs[tl]["parent"]
            stop = min([T + TAKEOVER_SPAN] + [segs[i]["last"] for i in (parent, tl)
                                              if segs[i]["last"] is not None])
            ai, fork, frac, n = tk_ai(path, tl, T, stop)
            _, original, _, _ = tk_ai(path, parent, T, stop)
            compared, missing, skipped = ai_compare(ai, original, fork)
            if not compared:
                raise tarec.Unreadable(
                    f"no AI seat made a unit in timeline {parent}'s [{T}, {stop}]"
                    + (f" (seats {', '.join(map(str, skipped))}: none in the original)"
                       if skipped else ""))
            if frac is None:
                raise tarec.Unreadable(f"the AI seats hold no mobile unit at GameTime {T}")
            moved = f"{frac:.0%} of {n} mobile units moved"
            bad = [f"AI seat {s} made no unit, where the original made one" for s in missing]
            if frac < 0.25:
                bad.append(moved)
            parts = (bad or [f"every AI seat that made a unit in the original "
                             f"({', '.join(map(str, compared))}) made one"])
            parts += [f"seat {s}: none in the original either" for s in skipped]
            if frac >= 0.25:
                parts.append(moved)
            res.append(("takeover ai", FAIL if bad else PASS,
                        f"GameTime [{T}, {stop}]: " + "; ".join(parts)))
        except tarec.Unreadable as e:
            res.append(("takeover ai", UNRUN, str(e)))
    if "fork" in want:
        parts, bad, unread = [], [], []
        rc, o = tarec_run("compare-fork", path, tl)
        (parts if rc == 0 else bad if rc == 1 else unread).append(f"compare-fork: {o.strip()}")
        rc, o = tarec_run("segments", path)
        m = re.search(r"original sha256 ([0-9a-f]+)", o)
        mine = hashlib.sha256(Path(fx["rec"]).read_bytes()).hexdigest()
        if not m:
            unread.append(f"segments: {o.strip()}")
        elif m.group(1) != mine:
            bad.append("the original's bytes changed")
        else:
            parts.append("the original untouched")
        for t in (0, tl):
            rc, o = tarec_run("check", path, "--timeline", t)
            (parts if rc == 0 else bad if rc == 1 else unread).append(
                f"check --timeline {t}: {last(o) if rc == 0 else o.strip()}")
        try:
            seg = [s for s in tarec.file_segments(path)[0] if s["id"] == tl][0]
            ends = [b for b in seg["blocks"] if b[0] == tarec.RB_END]
            e = tarec.end(ends[0][3]) if ends else None
            if not e or e.get(tarec.RT_END_REASON) != 1 or e["corrections"]:
                bad.append(f"timeline {tl} ends "
                           + ("with no end block" if not e else
                              f"{tarec.END_REASONS.get(e.get(tarec.RT_END_REASON))}, "
                              f"{len(e['corrections'])} corrections"))
        except tarec.Unreadable as ex:
            unread.append(str(ex))
        for proc, d, puppet in (("K", out / "K" / f"dump-k{T + tk.get('dump', TAKEOVER_SPAN)}.bin", ()),
                                ("F", out / "F" / f"dump-f{fx.get('tk_f', 0)}.bin", ("--puppet",))):
            if not d.exists():
                unread.append(f"no {proc} dump {d.name}")
                continue
            rc, o = tarec_run("compare", *puppet, "--timeline", tl, path, d)
            (parts if rc == 0 else bad if rc == 1 else unread).append(
                f"{proc} {d.stem[5:]}: {last(o) if rc == 0 else o.strip()}")
        res.append(("takeover fork", FAIL if bad else UNRUN if unread else PASS,
                    "; ".join(bad + unread + parts)))
    if "victory" in want:
        try:
            seg = [s for s in tarec.file_segments(path)[0] if s["id"] == tl][0]
            ends = [b for b in seg["blocks"] if b[0] == tarec.RB_END]
            e = tarec.end(ends[0][3]) if ends else {}
            won = [r for st, r, _, _ in e.get("seats", []) if st == P]
            ok = (e.get(tarec.RT_END_REASON) == 1 and won == [1] and
                  seg["last"] is not None and seg["last"] <= T + TAKEOVER_SPAN)
            res.append(("takeover victory", PASS if ok else FAIL,
                        f"timeline {tl} to GameTime {seg['last']}: "
                        f"{tarec.END_REASONS.get(e.get(tarec.RT_END_REASON), 'no end block')}; "
                        f"player {P} {tarec.RESULTS.get(won[0], won[0]) if won else 'has no result'}"))
        except tarec.Unreadable as ex:
            res.append(("takeover victory", UNRUN, str(ex)))
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

    def __init__(self, names, args, out, fixes=None, measure=False, takeover=None):
        self.q = queue.Queue()
        self.lock = threading.Lock()
        self.pending = 0
        self.fx = {}
        self.out = out
        self.measure = measure
        h = head()
        fixes = fixes or fixtures()
        for name in names:
            fix = fixes[name]
            stock = fix.get("dll") == "stock"
            # a cost game measures the build a player runs, never the instrumented one
            cover = bool(getattr(args, "cover", False)) and not fix.get("cost")
            dll, make = build_dll(stock, cover)
            given = args.stock_dll if stock else args.dll
            if given:
                dll = Path(given)
            dll = dll.resolve()
            if not dll.exists():
                raise CouldNotRun(f"{name}: no DLL at {dll} ({make})")
            if dll.stat().st_mtime < sources_mtime():
                raise CouldNotRun(f"{name}: {dll} is older than its sources ({make})")
            Ts = gametimes(name, fix["end"], h, fix.get("trips", 3))
            # `takeover`, (player, at) of `tareplay.py takeover`: L, K and F alone
            want = (("L", "K", "F") if takeover else ("L", "P") if measure else
                    ("L",) if fix.get("cost") else procs(fix))
            self.fx[name] = {"name": name, "fix": fix, "dll": dll, "Ts": Ts, "out": out / name,
                             "problems": [], "rt": {}, "passed": [], "held": [],
                             "procs": {}, "cover": cover, "measure": measure,
                             "map": dll.with_suffix(".map"), "want": want,
                             "tk_ask": takeover or (None, None)}
            if measure:
                say(f"{name}: recorded to GameTime {fix['end']} and played")
            elif fix.get("cost"):
                say(f"{name}: recorded to GameTime {fix['end']} for its cost and size")
            else:
                say(f"{name}: T1 {Ts[0]}, round trips {' '.join(map(str, Ts[1:]))}, "
                    f"Tlast {Ts[-1]}")
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
                cost = bool(fx["fix"].get("cost"))
                fx["rec"] = live(w, fx, None if fx["measure"] or cost else Ts, out)
                h = tarec.header_only(str(fx["rec"]))
                fx["viewed"] = h["options"]["viewed"]
                if "K" in fx["want"]:
                    fx["tk_T"], fx["tk_P"] = tk_point(fx, h, *fx["tk_ask"])
                for p in fx["want"][1:]:
                    if p != "F":                # F follows K, which it reads
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
            elif proc == "K":
                fx["tk"] = takeover_k(w, fx, out)
                self.put((name, "F"))
            elif proc == "F":
                fx["tk_f"] = takeover_f(w, fx, out)
            elif proc == "X":
                fx["xres"] = forks_lane(w, fx, out)
            elif proc == "W":
                fx["wres"] = watch_lane(w, fx, fx["rec"], out, Ts[0])
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


def cost_line(fx):
    """The recorder's own account of a cost game, reported and never failed (OQ5): its capture
    cost per iteration and the file's size, extrapolated to an hour of game time."""
    log = (fx["out"] / "L" / "tagpu.log").read_text(errors="replace")
    cap = re.search(r"recorder: (\d+) iterations captured: the first in \d+ us, the rest in "
                    r"(\d+) us on average and (\d+) us at most", log)
    size = re.search(r"recorder: writer: .* closed: (\d+) iterations .* stream (\d+) KB deflated "
                     r"to (\d+) KB", log)
    if not cap or not size:
        return "COST: the recorder's capture or close line is missing from L's log"
    n, raw, kb = int(size.group(1)), int(size.group(2)), int(size.group(3))
    if not n:
        return "COST: the recording closed with no iteration"
    hour = 30 * 3600 / n
    return (f"COST: {cap.group(1)} iterations, {cap.group(2)} us an iteration on average, "
            f"{cap.group(3)} us at most; {kb / 1024:.1f} MB ({raw / 1024:.1f} MB before "
            f"deflate), {kb / 1024 * hour:.0f} MB an hour of game time at that rate")


def report(run, keep):
    failed = unrun = False
    lines = []
    for name, fx in run.fx.items():
        cost = bool(fx["fix"].get("cost"))
        lines.append(f"== {name}: " + ("the cost game" if cost else
                                       f"T {' '.join(map(str, fx['Ts']))}") + f"; {fx['out']}")
        for proc, kind, why in fx["problems"]:
            lines.append(f"   {proc} {kind.upper()}: {why}")
            failed |= kind == "FAILED"
            unrun |= kind != "FAILED"
        if cost:
            if fx["procs"].get("L") == "ran":
                lines.append("   " + cost_line(fx))
            elif "L" not in fx["procs"]:
                lines.append("   L NEVER RAN")
                unrun = True
            continue
        for proc in fx["want"]:
            if proc not in fx["procs"]:
                lines.append(f"   {proc} NEVER RAN")
                unrun = True
        if fx["procs"].get("L") != "ran":
            continue
        for label, verdict, detail in checks(fx) + (tk_checks(fx) if "K" in fx["want"] else []):
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
                                       "src/tagpu_recreader.c", "src/tagpu_replay.c", "src/tagpu_recfile.c"}


def reached_of(run):
    """({fixture: {(source, function)}}, problems) from every process's coverage files, each read
    against the map of the build it ran; a file or map that cannot be read is a problem, by name."""
    srcs = covered_sources()
    out, problems = {}, []
    maps = {}
    for name, fx in run.fx.items():
        covers = sorted(fx["out"].rglob("cover-*.bin"))
        if not covers:
            continue
        try:
            m = maps.setdefault(str(fx["map"]), tafeat.read_map(fx["map"]))
            out[name] = {k for k in tafeat.reached(m, covers) if k[0] in srcs}
        except (OSError, ValueError) as e:
            problems.append(f"UNREAD {name}: {e}")
    return out, problems


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
    got, unread = reached_of(run)
    for u in unread:
        say(u)
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
                DD + "src/tagpu_recreader.c", DD + "src/tagpu_replay.c", DD + "src/tagpu_recfile.c",
                DD + "inc/tagpu_recfile.h",
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


def merge_base(base):
    """The commit `git diff base...HEAD` takes its old side from."""
    return git("merge-base", base, "HEAD").strip() or base


def content_users(path):
    """The fixtures whose content command runs the tool at `path`."""
    return {n for n, f in fixtures().items()
            if (f.get("content") or {}).get("run", [""])[0] == path}


def fixture_users(scenario):
    """The fixtures whose load or applied steps name `scenario`."""
    out = set()
    for name, fix in fixtures().items():
        named = {fix["load"]} | {a for _, k, a in fix.get("steps", []) if k == "apply"}
        if scenario in named:
            out.add(name)
    return out


def gated(paths, base):
    """(fixtures, sim files to read by region, recorder files to read by function, sim files a
    changed header reaches (every feature of each), why): what the changed paths make the landing
    run."""
    sims = sim_paths()
    every = set(fixtures())
    fx, by_region, by_function, whole, why = set(), set(), set(), set(), []
    headers = {s: includes_of(s) for s in sims}
    for p in sorted(paths):
        if p in sims:
            by_region.add(p)
        elif p == FIXTURES_JSON:
            old = json.loads(git("show", f"{merge_base(base)}:{p}", check=False)
                             or '{"fixtures": {}}')
            old = old.get("fixtures", {})
            new = fixtures()
            changed = {k for k in set(old) | set(new) if old.get(k) != new.get(k)} & set(new)
            fx |= changed
            why.append(f"{p}: the entries {' '.join(sorted(changed)) or 'none left'}")
        elif p in RECORDER_OWN or p.startswith("tools/replay/") or p == "scenarios/replay-end.json":
            fx |= every
            if p.endswith(".c"):
                by_function.add(p)
            why.append(f"{p}: the recorder's own, every fixture")
        elif p.startswith("scenarios/") and fixture_users(Path(p).name):
            users = fixture_users(Path(p).name)
            fx |= users
            why.append(f"{p}: the scenario of {' '.join(sorted(users))}")
        elif content_users(p):
            fx |= content_users(p)
            why.append(f"{p}: the content of {' '.join(sorted(content_users(p)))}")
        else:
            for sim, hs in headers.items():
                if p in hs:
                    whole.add(sim)
                    why.append(f"{p}: included by {sim}, every feature of it")
    return fx, by_region, by_function, whole, why


def changed_lines(path, base):
    """The new-side lines of `path` that changed, not only in comments or whitespace: a changed
    #define, string or character literal is a changed line. The old side is the merge-base, as
    `git diff base...HEAD`'s is."""
    old = git("show", f"{merge_base(base)}:{path}", check=False)
    new = (TREE / path).read_text()
    ob = tafeat.blank_comments(old).split("\n")
    nb = tafeat.blank_comments(new).split("\n")
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
    """What the landing must reach: (fixtures, functions {(src, fn): (feature, builds)}, regions
    {feature: ([(src, fn)...], builds)} for file-scope lines, problems, why, gated) -- `builds`
    the builds that compile the changed lines (tafeat.builds), each of which must reach them;
    `gated` whether any changed path is gated at all."""
    paths = [p for p in git("diff", "--name-only", f"{base}...HEAD").split() if p]
    fx, by_region, by_function, whole, why = gated(paths, base)
    feats, problems = tafeat.all_features()
    funcs, regions = {}, {}
    for path in sorted(whole):              # a header it includes changed: every feature of it
        for f in feats:
            if f["file"] == path[len(DD):]:
                fx |= set(f["fixtures"])
    for path in sorted(by_region | by_function):
        if not (TREE / path).exists():
            continue
        rel = path[len(DD):]
        text = (TREE / path).read_text()
        fns = tafeat.functions(text)
        bl = tafeat.builds(text)
        for ln in sorted(changed_lines(path, base)):
            fn = next((f for f in fns if f[1] <= ln <= f[2]), None)
            b = bl[ln] if ln < len(bl) else tafeat.BOTH
            if "stock" in b:
                fx.add(STOCK_FIXTURE)
            if path in by_region:
                feat = tafeat.feature_at(feats, rel, ln)
                if not feat:
                    problems.append(f"UNFEATURED {path}:{ln}")
                    continue
                fx |= set(feat["fixtures"])
                if fn:
                    old = funcs.get((rel, fn[0]), (feat["name"], frozenset()))
                    funcs[(rel, fn[0])] = (feat["name"], old[1] | b)
                else:
                    old = regions.get(feat["name"], ([(rel, f[0]) for f in fns
                                                      if feat["line"] <= f[1] <= feat["end"]],
                                                     frozenset()))
                    regions[feat["name"]] = (old[0], old[1] | b)
            elif fn:
                old = funcs.get((rel, fn[0]), ("the recorder's own", frozenset()))
                funcs[(rel, fn[0])] = (old[0], old[1] | b)
    return fx, funcs, regions, problems, why, bool(fx or by_region or by_function or whole)


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
    fx, funcs, regions, problems, why, gated_any = landing_plan(base)
    for w_ in why:
        say(w_)
    if not gated_any:
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
    # reach per build: a line only the stock-limits build compiles is reached only by a fixture
    # that ran that build, and a line only the default build compiles only by one that did not
    per, unread = reached_of(run) if run else ({}, [])
    problems += unread
    got = {b: set().union(set(), *(v for n, v in per.items()
                                   if (run.fx[n]["fix"].get("dll") == "stock") == (b == "stock")))
           for b in ("raised", "stock")}

    def reached(keys, bs):
        return all(any(k in got[b] for k in keys) for b in bs)
    for (src, fn), (feat, bs) in sorted(funcs.items()):
        if not reached([(src, fn)], bs):
            problems.append(f"UNCOVERED {src}:{fn} (feature {feat}; builds {'+'.join(sorted(bs))}; "
                            f"fixtures {' '.join(sorted(fx)) or 'none'})")
    for feat, (fns, bs) in sorted(regions.items()):
        if not reached(fns, bs):
            problems.append(f"UNCOVERED {feat}: a changed file-scope line, and no function of its "
                            f"region ran in the build{'s' if len(bs) > 1 else ''} "
                            f"{'+'.join(sorted(bs))}")
    for p in problems:
        print(f"tareplay landing: {p}")
    k = sum(1 for key, (_, bs) in funcs.items() if reached([key], bs)) + sum(
        1 for fns, bs in regions.values() if reached(fns, bs))
    total = len(funcs) + len(regions)
    if status == 0 and not problems:
        sha = graded[:8]
        seeds = " ".join(f"{n}:{run.fx[n]['Ts'][0]}" for n in sorted(fx)) if run else ""
        feats = sorted({f for f, _ in funcs.values()} | set(regions))
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
    fx, by_region, by_function, whole, _ = gated(paths, base)
    if not (fx or by_region or by_function or whole):
        print("tareplay landing --check: nothing gated changed")
        return 0
    for c in git("rev-list", f"{base}..HEAD").split():
        note = git("notes", "show", c, check=False)
        if any(ln.startswith(NOTE) and " result=pass " in ln for ln in note.splitlines()):
            later = [p for p in git("diff", "--name-only", c, "HEAD").split() if p]
            hit = [p for p in later if any(gated([p], c)[:4])]
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
    """Every fixture and the broad tier's own entries (fixtures.json's `broad`: limits-tier1 with
    twenty round trips, a ten-minute four-AI game for the cost and the size), then what the tier
    never exercised: a feature none of its fixtures reached, a root or sink site no run counted, a
    recorded table no recording changed between T1 and Tlast."""
    fixes = {**fixtures(), **broad_fixtures()}
    names = list(fixes)
    args.cover = not args.release
    if args.release:
        args.dll = args.release
    out = out_dir(args)
    args.keep = True
    try:
        run = Run(names, args, out, fixes=fixes)
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
        got, unread = reached_of(run)
        lines += unread
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
        if fx["fix"].get("cost"):
            continue
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


def cmd_takeover(args):
    """L, K and F alone, for one fixture with a takeover"""
    fix = fixtures().get(args.fixture)
    if not fix or not fix.get("takeover"):
        print(f"tareplay: {args.fixture} is no fixture with a takeover (tareplay.py list)",
              file=sys.stderr)
        return 2
    out = out_dir(args)
    say(f"tareplay takeover: {args.fixture}, into {out}")
    try:
        run = Run([args.fixture], args, out, takeover=(args.player, args.at))
    except CouldNotRun as e:
        print(f"tareplay: {e}", file=sys.stderr)
        return 2
    run.j = args.j
    run_threads(run)
    status = report(run, args.keep)
    if STOP.is_set():
        return status or 2
    return status


def cmd_forks(args):
    fix = fixtures().get(args.fixture)
    if not fix or not fix.get("takeover"):
        raise CouldNotRun("forks needs a takeover fixture")
    out = out_dir(args)
    w = Worker(out)
    dll, make = build_dll(fix.get("dll") == "stock", args.cover)
    try:
        if not dll.exists() or dll.stat().st_mtime < sources_mtime():
            raise CouldNotRun(f"build {make} first")
        fx = {"name": args.fixture, "fix": fix, "dll": dll.resolve(), "cover": args.cover,
              "Ts": gametimes(args.fixture, fix["end"], head(), 3)}
        fx["rec"] = live(w, fx, None, out / "L")
        res = forks_lane(w, fx, out / "X")
        for name, status, detail in res:
            say(name, status, detail)
        return 0
    except (CouldNotRun, Failed) as e:
        say("forks:", str(e))
        return 1 if isinstance(e, Failed) else 2
    finally:
        w.close()


def cmd_list(args):
    for name, fix in fixtures().items():
        print(f"{name:16} {fix['load']}, end at GameTime {fix['end']}"
              + (", the stock-limits build" if fix.get("dll") == "stock" else "")
              + (", a takeover" if fix.get("takeover") else ""))
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
    tk = sub.add_parser("takeover", help="L, K and F alone: a fixture recorded, taken over, and "
                                         "its fork played across the boundary")
    tk.add_argument("fixture", help="a fixture with a `takeover` entry")
    tk.add_argument("--player", type=int, help="the player to take over (default the entry's)")
    tk.add_argument("--at", type=int, help="the GameTime to take over at (default T2)")
    tk.add_argument("-j", type=int, default=2, help="games at once (default 2)")
    tk.add_argument("--cover", action="store_true", help="the coverage builds")
    tk.add_argument("--keep", action="store_true", help="keep the passing dumps")
    tk.add_argument("--out", help="the run's folder")
    tk.set_defaults(func=cmd_takeover, dll=None, stock_dll=None)
    fk = sub.add_parser("forks", help="fork limit, LRU, shared prefixes, compaction and export")
    fk.add_argument("fixture")
    fk.add_argument("--cover", action="store_true")
    fk.add_argument("--out")
    fk.set_defaults(func=cmd_forks, dll=None, stock_dll=None)
    fe = sub.add_parser("features", help="the SIM_FEATURE regions of the sim files")
    fe.add_argument("--check", metavar="DIR", help="the link's check of tagpu/ddraw: exit 0 "
                                                   "clean, 1 a named problem, 2 could not run")
    fe.set_defaults(func=cmd_features)
    a = ap.parse_args(argv)
    if a.cmd in ("run", "measure", "landing", "broad", "takeover") and not 1 <= a.j <= 9:
        ap.error("-j is 1..9")
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
