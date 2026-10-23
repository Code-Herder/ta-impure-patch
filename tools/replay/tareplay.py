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
drawn (one more for each round trip a broad entry's `trips` asks for beyond three), each T = 1 (mod 30)
-- the radar pass's step, where a seek's contact bits in any view but the recording's own, which
its own pass makes, agree at once unless something the pass read changed later in its step (O14);
a seek or a skirmish takeover in the recording's viewed player takes them from the recording and
agrees at every T -- between 301 and 0.8 x its end, from
a generator seeded by the fixture's name and HEAD, and
printed: T1 the first, T2..T4 the round trips, Tlast = T4.
An entry's `last_min` requires Tlast to reach a named completion checkpoint; it keeps late
construction events inside the live/puppet comparison on every commit's random seed.
Four processes, each a game of its own:

    L  live      the fixture recorded, dumps l<T1> and l<Tlast>
    P  playing   `replay play` in the recording's viewed player's perspective, dumps p<T1> and
                 p<Tlast>
    R  round     the same, dump r<T1>, `roundtrip rt<T> at <T>` for T2..T4 (frame dumps
                 rt<T>-a and rt<T>-b around a rebuild in place) and a dump rt<T>c at T + 300
    Q  seek      `replay seek T1 --recording` in a fresh process, dump q<T1>

and a fifth where fixtures.json gives the fixture `"watch": true`:

    W  watch     the playing a player starts and drives (watch_lane): from the main menu's
                 REPLAYS, Watch refuses two edited copies of the recording (another layout
                 hash; a map no archive has, which the Start refuses), then plays it; the replay
                 HUD's speed, the in-game Options' OK with the store's gamespeed unchanged, its
                 play to pause, the trigger `hud seek T1` (the HUD's own seek entry), a row's
                 Sight and Sight again with clock and camera unchanged, Follow, Follow again and
                 Jump, the Cursors switch off and on, dump w<T1>, play to resume, Leave and Yes
                 back to the browser, and a second Watch -- every HUD control clicked in device
                 space where the game drew it (`tacli hud`)

and every check fails by name: the recording's end block, `tarec check` and `tarec lossy-check`
(L records the exact stream beside the lossy one, EXACT_LEVER); L's dumps `tarec compare
--exact`; P's, R's, Q's and W's capture dumps `tarec compare --puppet`, with --seek for the dumps
a seek precedes (q, rt<T>c, w); each round trip's line has residue 0 and `tarec compare-dumps
--seek rt<T>-a rt<T>-b` is EQUAL; W's steps each hold what they
claim; L's counts against P's `tarec compare-counts`, with --cob-exact where fixtures.json gives
the fixture `"cob": "exact"` (counts_options). Passing dumps are deleted unless --keep. A fixture with `"decline": true` launches every
process with DECLINE_LEVER in its game folder, so its recording holds blocks the writer's codec
declined on a schedule, each read back like any other, and the stream codec's check fails when it
holds none.

A fixture fixtures.json gives a `takeover` runs two processes more, K after L and F after K:

    K  takeover  `replay takeover` of L's recording at T = T2, or the entry's `at`, as player P
                 (the recording's controlled player unless the entry names one) in a fresh
                 process, which records
                 timeline 1 into the same file; T is the refresh at or before the point drawn,
                 which REFRESH_LEVER's grid makes the point itself: the window, its boxes and ErrorLog.txt watched,
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
wait, and every worker stops its instance and its Xvfb. The workers play the games, -j of them at
most; the checks run beside them on an offline pool, a fixture's as soon as its last lane ends
and the forks and rewind lanes' while those lanes play on (*the offline work*), and the summary
lists every check in the order it was asked for. Exit 0: every check passed; 1: a named
failure; 2: something could not run (and nothing failed by name).

The output folder (default ~/.local/share/ta-replay/runs/<time>, or $TAREPLAY_OUT) is outside the
repository: its logs carry absolute paths. While the run lasts its tarec-cache/ folder holds the
recordings' decoded blocks for every tarec call of the run (TAREC_CACHE, reccache.py).

Tick-driven runs default to internal game speed 20 (2x normal); --game-speed 10 selects normal
speed. The owned instance's private settings are written while stopped, before each launch.
Cost fixtures and W's speed/settings UI test use normal speed; network/timing suites have their
own speed policy.

--turbo K (default 1, 0 for none) is the DLL's test-only pacing (tagpu_replay.c, THE TURBO): K
ticks every frame from the trigger's `turbo on`, which a lane sends while its level is held, once
its dumps and round trips are armed and acknowledged. K = 1 is the batch a player at normal speed
gets: the step's once-a-batch tail makes the batching part of the result, and a fixed K makes it
the same on every run. It runs on L, P, R, F, K, X and U; not on Q, which plays one tick, nor W,
whose ticks pass only between its interface steps; never on a cost fixture, which measures the
game's own pacing (turbo_lane).

THE LAUNCH'S HOLD. The same lanes, a cost fixture's L among them, launch with the DLL's test hold
(tagpu_hold.on; tagpu_replay.c,
THE LAUNCH'S HOLD; held_lane): the level is held after its first step -- a skirmish at GameTime 1,
its scenario applied on the held frames; a playing at its first step; a seek or a takeover at
T-1 -- until the lane's `resume`, so nothing a lane arms can pass before its answer. From there a
lane acts at GameTimes, not at what a poll of the clock finds (run_to): the DLL pauses the level
at the frame visit after step T (`pause at T`), and the lane acts on the held level and resumes
it. So an L step lands on its own tick -- keys, a chat line, an order or a scenario given while
held -- as do the twin (Tlast + 30) and the end scenario (at the fixture's end); K's takeover judge
reads the units JUDGE_TICKS after T, its order and chat line are given there, and it ends at its
dump's GameTime; U's spans are ticks. A `run` step, a script that drives the game, is handed the
level held at its tick with the turbo on, advances it by exact ticks itself
(resource_regression.py, IN LOCKSTEP) and hands it back held. A step whose GameTime a script ran
past could not run, named.

No check is decided by the wall clock. A wait on a fact is bounded by ACK_S for a one-frame answer
and, while the level runs, by STALL_S of GameTime standing still (Stall): each catches a hung or
dead game, never an outcome. A peek the running game has not answered is no progress, not a
failure: a seek's rebuild, a fork crossing's among them, holds the game thread for one frame. A
seek's cost is checked on its work, never its time: the iterations it read, the units it made and
the blocks it decoded, each against the file (W1 to W3, work_verdict).

Every game gets the test render profile (TEST_RENDER): a present cap,
supersampling 1x and the Classic++ look keys off; the engine's own draw and its shadow switch
stay as a player has them.
"""
import argparse
import atexit
import fcntl
import datetime
import hashlib
import itertools
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
from concurrent.futures import ThreadPoolExecutor, wait
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
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
import tarun  # noqa: E402  what the run is doing, for the runs viewer (tools/tarun.py)
import runlock  # noqa: E402  one game regression at a time (tools/runlock.py)
sys.path.insert(0, str(HERE))
import tarec  # noqa: E402
import reccache  # noqa: E402
import tafeat  # noqa: E402

# Load once before workers start; reuse tacli's own prefix-based process identity check.
_CLI_SPEC = spec_from_loader("_tareplay_cli", SourceFileLoader("_tareplay_cli", str(TACLI)))
_CLI_API = module_from_spec(_CLI_SPEC)
sys.modules[_CLI_SPEC.name] = _CLI_API
_CLI_SPEC.loader.exec_module(_CLI_API)

PREFIX = "tareplay-"
UNIT_LIMIT = 500
TEST_GAME_SPEED = 20
NORMAL_GAME_SPEED = 10
TURBO = 1                       # the k --turbo defaults to: normal speed's batch, every frame
# THE TEST RENDER PROFILE: no check reads a pixel, so a test game presents at PRESENT_CAP frames a
# second (fps_limiter.c, test_present_cap) and draws without supersampling or the Classic++ look
# keys (tagpu_settings.h); `shadows`, the engine's own switch, is left as a player has it
TEST_RENDER = {"style": "custom", "assets": "0", "light": "0", "shadowres": "512", "ss": "1"}
PRESENT_CAP = 20
# A fixture fixtures.json gives `"decline": true` launches with this lever in its game folder: the
# writer's codec declines stream blocks 2, 7, 12, ... (tagpu_recorder.h, tagpu_rec_lever_init),
# and the recording must count declined blocks (codec_check)
DECLINE_LEVER = "tagpu_rec.decline"
# L and K launch with EXACT_LEVER in their game folder: the writer follows each stream block with
# the exact records it filtered (RB_EXACT), against which `tarec compare --exact` reads their dumps
# and `tarec lossy-check` the lossy stream (tagpu_reclossy.c). A fixture with a `takeover` launches
# every process with REFRESH_LEVER holding TAKEOVER_REFRESH: a takeover starts at the refresh at or
# before the GameTime asked (tagpu_replay.c, tk_bound), and every point the harness draws is 1
# (mod 30), so on that grid the takeover, the forks and the rewind's fork start where they ask
EXACT_LEVER = "tagpu_rec.exact"
REFRESH_LEVER = "tagpu_rec.refresh"
TAKEOVER_REFRESH = 30
GAMETIME = "*0x511DE8+0x38A47:4"
COB_STOP = "A unit script cannot continue safely"
TRIGGER = "tagpu_replay.trigger"
ROUND_TRIPS = 3
TAKEOVER_SPAN = 1800            # K's dump and the takeover's checks, this many ticks after T
TAKEOVER_BACK = 300             # F's seek, this many ticks before T, and its dump after it
TAKEOVER_CHECKS = {
    "judge": "the window alive with no box and no ErrorLog.txt, and at least 90 % of the units "
             "the takeover made seen JUDGE_TICKS after T",
    "line": "the takeover's line and its seek's work: no create refused, the reader started "
            "where the file says (W1), every live unit made and none left (W2), and the blocks "
            "it decoded the file's own from there (W3)",
    "orders": "the entry's `order` unit of P's moves more than 64 wu when ordered",
    "ai": "every AI seat issues fresh planner orders that reach normal dispatch during an "
          "armed TAKEOVER_SPAN window, and at least 25 % of the AI seats' mobile units move more than "
          "8 wu; construction is reported only",
    "fork": "compare-fork EQUAL, the original's bytes untouched, both timelines check, "
            "timeline 1 complete, K's dump EQUAL to timeline 1's exact stream, F's to its "
            "stream, and timeline 1's lossy-check",
    "victory": "timeline 1 ends complete with P's seat won, within TAKEOVER_SPAN of T",
}
CONTINUE = 300                  # the dump after a round trip, this many ticks on
TWIN_AFTER = 30                 # an L lane's twin, held this many ticks after Tlast
ACK_S = 60                      # a one-frame answer: an arm, a dump's line, a peek, a screen
PEEK_UNANSWERED = "no peek lines"   # tacli peek's words when the game did not answer in its wait
STALL_S = 120                   # GameTime standing still this long while the level runs: a hung game
SEEK_LEAD = 123                 # the reader's lead before a seek's target (tagpu_replay.c, EYE_LEAD)
JUDGE_TICKS = 240               # the takeover judge's sample, this many ticks after T: its old 4 s
                                # at speed 20
NULL_VIEWS = tuple(f"lnv{i}" for i in range(12)) + ("lnselection",)
LAUNCH_TIMEOUT = 400


def null_views(fix):
    return NULL_VIEWS + tuple(f"lnselection{i}" for i in range(len(fix.get("viewer_more", []))))


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
J = tarun.NULL                  # the run's journal once its games start (run_threads)


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


# tacli's own wait on an input batch is bounded at its KEYS_STALL_S + KEYS_ABANDON_S (90 s): it
# abandons a stalled batch itself, which lets its keys up. A caller's timeout under that kills
# tacli first and leaves the batch standing, so every input call keeps at least this.
INPUT_TIMEOUT = 150


def tacli(*args, timeout=180):
    r = subprocess.run([sys.executable, str(TACLI), *map(str, args)], capture_output=True,
                       text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def last(text):
    lines = text.strip().splitlines()
    return lines[-1] if lines else ""



def fixture_speed(fix, default):
    """A fixture's game speed: normal for a cost fixture."""
    return NORMAL_GAME_SPEED if fix.get("cost") else default


def held_lane(fix, proc):
    """Whether a lane launches held (THE LAUNCH'S HOLD): every lane that waits on ticks, each of
    its GameTimes held (run_to) or armed in the DLL; not Q, which plays one tick, nor W, whose ticks
    pass only between its interface steps. A cost fixture's L is held too: the hold stops the level
    at its ticks and changes no step's size, and its pacing is the game's own (turbo_lane)."""
    return proc in "LPRFKXU"


def turbo_lane(fix, proc, k):
    """A lane's turbo k, 0 for none: the held lanes', except a cost fixture's, which measures the
    game's own pacing."""
    return k if k and not fix.get("cost") and held_lane(fix, proc) else 0


class Worker:
    """One tacli instance on one Xvfb, for the whole run."""

    def __init__(self, out, game_speed=TEST_GAME_SPEED):
        if game_speed not in (NORMAL_GAME_SPEED, TEST_GAME_SPEED):
            raise CouldNotRun("test game speed must be 10 or 20")
        self.game_speed = game_speed
        self.turbo = 0
        self.held = False               # the next launch holds its level (THE LAUNCH'S HOLD)
        self.exact = False              # the next launch records the exact stream (EXACT_LEVER)
        self.lane = None                # (fixture, lane) while a job plays, for the journal
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
            # keep 2: the prune keeps one recording besides the new one, which prune_step's
            # expectations count on (tagpu_recfile.h, THE PRUNE)
            tacli("arm", self.name, "recorder.on=keep 2")
        except BaseException:
            stop_xvfb(self.xvfb)
            self.lock.close()
            raise
        say(f"{self.name} on Xvfb {self.xvfb.display} (pid {self.xvfb.pid})")

    def close(self):
        """The instance stopped and left unheld: a launch by hand between runs plays as usual."""
        tacli("stop", self.name, "--hard", timeout=120)
        self.hold_lever(False)
        stop_xvfb(self.xvfb)
        self.lock.close()

    def stop(self, hard=False):
        if hard:
            rc, detail = tacli("stop", self.name, "--hard", timeout=120)
            if rc or self.running():
                raise CouldNotRun(f"failed game cleanup: {last(detail)}; instance still held")
            self.last_collection = None
            return
        if self.last_collection:
            self.quit()
        else:
            tacli("stop", self.name, timeout=120)
        # Exit flushes coverage after its workers finish. Collect that final bitmap and log,
        # rather than the one taken while those workers were still running.
        if self.last_collection:
            rd = self.gamedir / "replays"
            for f in rd.glob("cover-*.bin") if rd.is_dir() else []:
                shutil.copy2(f, self.last_collection / f.name)
            (self.last_collection / "tagpu.log").write_text(talog.run_text(self.gamedir))

    def running(self):
        return _CLI_API.Instance(self.name).running()

    def quit(self):
        """The game's exit hook joins its file workers; SIGTERM bypasses it."""
        if not self.running():
            return
        gui = json.loads(ui(self, "--json"))["gui"].upper()
        if gui == "ENDMSN.GUI":
            ui(self, "click", "MainMenu")
            ui_on(self, "MAINMENU.GUI", 30)
            gui = "MAINMENU.GUI"
        elif gui == "REPLAYS.GUI":
            ui(self, "click", "BACK")
            ui_on(self, "MAINMENU.GUI", 30)
            gui = "MAINMENU.GUI"
        if gui == "MAINMENU.GUI":
            rc, text = tacli("ui", self.name, "click", "EXIT", "--timeout", 5, timeout=INPUT_TIMEOUT)
            if rc and self.running():
                raise CouldNotRun(f"the main menu's exit: {last(text)}")
        elif gui in ("ARMMAIN2.GUI", "CORMAIN2.GUI", "ARMOPT.GUI"):
            if gui != "ARMOPT.GUI":
                rc, text = tacli("keys", self.name, "tab", timeout=INPUT_TIMEOUT)
                if rc:
                    raise CouldNotRun(f"the exit's options: {last(text)}")
            ui(self, "click", "EXIT")
            ui(self, "click", "EXITGAME")
            ui_on(self, "YESORNO.GUI", 30)
            # the Yes ends the process, which can take the key's release with it: the batch then
            # fails with the game gone, which is this step's success
            rc, text = tacli("keys", self.name, "y", timeout=INPUT_TIMEOUT)
            if rc and self.running():
                raise CouldNotRun(f"the exit's Yes: {last(text)}")
        else:
            raise CouldNotRun(f"no graceful exit route from {gui}")
        deadline = time.time() + 30
        while self.running():
            if time.time() >= deadline:
                raise CouldNotRun("the game did not finish its exit in 30 s")
            nap(0.2)

    def place(self, fix, dll):
        """The DLL and this fixture's content in the game folder, before a launch: every fixture's
        content files removed first, so one fixture's units never reach another's game."""
        self.last_collection = None
        speed = fixture_speed(fix, self.game_speed)
        rc, text = tacli("registry", self.name, f"gamespeed={speed}", "--json")
        if rc:
            raise CouldNotRun(f"test game speed: {last(text)}")
        rows = json.loads(text[text.index("{"):])["values"]
        if len(rows) != 1 or rows[0]["value"] != speed or rows[0]["type"] != 4:
            raise CouldNotRun(f"test game speed was not stored as a dword: {last(text)}")
        say(f"{self.name}: game speed {speed} ({speed / NORMAL_GAME_SPEED:g}x target)"
            + (f", turbo k={self.turbo}" if self.turbo else "") + (", held" if self.held else ""))
        turbo = self.gamedir / "tagpu_turbo.on"
        if self.turbo:
            turbo.write_text(f"{self.turbo}\n")
        else:
            turbo.unlink(missing_ok=True)
        self.hold_lever(self.held)
        decline = self.gamedir / DECLINE_LEVER
        if fix.get("decline"):
            decline.write_text("")
        else:
            decline.unlink(missing_ok=True)
        exact = self.gamedir / EXACT_LEVER
        if self.exact:
            exact.write_text("")
        else:
            exact.unlink(missing_ok=True)
        refresh = self.gamedir / REFRESH_LEVER
        if fix.get("takeover"):
            refresh.write_text(f"{TAKEOVER_REFRESH}\n")
        else:
            refresh.unlink(missing_ok=True)
        self.test_render()
        shutil.copy2(dll, self.gamedir / "ddraw.dll")
        rd = self.gamedir / "replays"
        for f in rd.glob("*") if rd.is_dir() else []:
            f.unlink()
        for other in fixtures().values():
            for name in (other.get("content") or {}).get("files", []):
                (self.gamedir / name).unlink(missing_ok=True)
        # A cached catalogue belongs to the previous fixture's installed
        # content. The next launch must resolve its units against its own game.
        _CLI_API._cat_path(_CLI_API.Instance(self.name)).unlink(missing_ok=True)
        content = fix.get("content")
        if content:
            argv = [a.replace("{gamedir}", str(self.gamedir)) for a in content["run"]]
            if argv[0].endswith(".py"):
                argv = [sys.executable, str(TREE / argv[0])] + argv[1:]
            r = subprocess.run(argv, capture_output=True, text=True, timeout=300)
            if r.returncode:
                raise CouldNotRun(f"content {' '.join(content['run'])}: {last(r.stdout + r.stderr)}")

    def hold_lever(self, on):
        """tagpu_hold.on, read at the next launch's attach (THE LAUNCH'S HOLD)."""
        hold = self.gamedir / "tagpu_hold.on"
        if on:
            hold.write_text("")
        else:
            hold.unlink(missing_ok=True)

    def test_render(self):
        """The test render profile (TEST_RENDER) in the instance's store, every other line kept,
        and the present cap's lever."""
        store = self.gamedir / "impure.cfg"
        lines = store.read_text().splitlines() if store.exists() else []
        keep = [ln for ln in lines if ln.partition("=")[0].strip().lower() not in TEST_RENDER]
        store.write_text("\n".join(keep + [f"{k}={v}" for k, v in TEST_RENDER.items()]) + "\n")
        (self.gamedir / "tagpu_presentcap.on").write_text(f"{PRESENT_CAP}\n")

    def gametime(self, wait=ACK_S, busy=False):
        """GameTime, read on the game thread at its next frame visit, `wait` seconds at most. With
        `busy`, None when the game is running and has not answered: a seek's or a fork crossing's
        rebuild runs inside one frame (MEASURED 2026-10-12, battle's X lane at a load average of
        63-87: two fork crossings' seeks of 6.5 s wall each, back to back, and a peek unanswered
        through tacli's 10 s), so a running game that has not answered yet is a fact for the
        caller's own bound (Stall), never an outcome."""
        rc, o = tacli("peek", self.name, GAMETIME, "--json", "--timeout", wait,
                      timeout=wait + ACK_S)
        if rc and busy and PEEK_UNANSWERED in o and self.running():
            return None
        if rc:
            raise CouldNotRun(f"tacli peek: {last(o)}")
        g = int(str(json.loads(o[o.index("{"):])["peeks"][0]["value"]).split()[0])
        if self.lane:
            J.progress(*self.lane, g)
        return g

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


def crash_report(path: Path) -> str:
    """TA's ErrorLog.txt in one line: where the exception was and what it read or wrote."""
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return "ErrorLog.txt (unreadable)"
    keep = [ln.strip() for ln in text.splitlines()
            if re.match(r"\s*(module |Access violation|ExceptionCode|Instruction pointer)", ln)]
    return "; ".join(keep[:4]) or text.strip().splitlines()[0][:200]


class Log:
    """The instance's log from a launch on, across rotation; a COB stop ends the process, and a
    crash ends every wait at once (`alive`)."""

    ALIVE_EVERY = 2.0      # s between the process checks of a wait: each asks tacli

    def __init__(self, w: Worker):
        self.w = w
        self.cursor = talog.Cursor(w.gamedir)
        self.part = ""
        self.lines = []
        self.errorlog = w.gamedir / "ErrorLog.txt"
        self.errorlog_was = self.errorlog.stat().st_mtime if self.errorlog.exists() else None
        self.seen_running = False
        self.checked = 0.0
        self.turbo = False      # `turbo on` holds on this level (turbo_engage)
        self.held = False       # a pause of hold_at's holds the level now
        self.pending = None     # the GameTime hold_at armed and the level has not reached yet
        self.hold_from = 0      # where in `lines` to look for its pause
        self.at = None          # the GameTime the level was last held at

    def alive(self):
        """Raise Failed when the game has crashed since this log began: TA wrote ErrorLog.txt
        (its exception handler, which can leave the process up behind a box), or the process
        that was running is gone. A wait would otherwise sit out its whole timeout -- up to
        half an hour -- on a game that cannot answer."""
        if self.errorlog.exists() and self.errorlog.stat().st_mtime != self.errorlog_was:
            self.read()
            raise Failed(f"the game crashed: {crash_report(self.errorlog)}")
        now = time.time()
        if now - self.checked < self.ALIVE_EVERY:
            return
        self.checked = now
        if self.w.running():
            self.seen_running = True
        elif self.seen_running:
            self.read()
            raise Failed("the game's process exited during a wait"
                         + (f": {self.lines[-1].strip()}" if self.lines else ""))

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
            if not any(rx.search(ln) for ln in self.lines[start:]):
                self.alive()

    def wait_running(self, rx, start=0, poll=None):
        """The first line from index `start` on that matches, on a level that runs toward it: no
        deadline but a stall (Stall), so a loaded machine only makes it later. `poll` is called
        at every look, for a lane that watches the window meanwhile."""
        rx = re.compile(rx)
        stall = Stall(self.w)
        while True:
            for ln in self.lines[start:]:
                if rx.search(ln):
                    return ln
            start = len(self.lines)
            if poll:
                poll()
            stall.check()
            nap(0.4)
            self.read()
            if not any(rx.search(ln) for ln in self.lines[start:]):
                self.alive()


class Stall:
    """The bound of a wait on a running level: STALL_S of GameTime standing still. A loaded machine
    runs the level slower, never not at all, so this catches a hung or dead game and decides no
    outcome. Read every EVERY seconds through a peek, which the game answers at its next flip. A
    peek the running game has not answered within EVERY is no progress and nothing more: the game
    thread is inside one long frame, a seek's rebuild, and STALL_S alone decides when that is a
    hung game. No second, shorter bound stands in for it."""

    EVERY = 5.0

    def __init__(self, w):
        self.w = w
        self.g = None
        self.since = self.next = time.monotonic()

    def check(self):
        now = time.monotonic()
        if now < self.next:
            return
        g = self.w.gametime(self.EVERY, busy=True)
        now = time.monotonic()
        self.next = now + self.EVERY
        if g is not None and g != self.g:
            self.g, self.since = g, now
        elif now - self.since > STALL_S:
            raise CouldNotRun(f"the level stood at GameTime {self.g} for {STALL_S} s while it ran"
                              + ("" if g is not None else ", its last peek unanswered"))


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
            (("W",) if fix.get("watch") else ()) + (("X",) if fix.get("forks") else ()) +
            (("U",) if fix.get("rewind") else ()))


def head():
    r = subprocess.run(["git", "-C", str(TREE), "rev-parse", "HEAD"], capture_output=True,
                       text=True)
    return r.stdout.strip() or "no-head"


def gametimes(name, end, h, trips=3, first=301, last_min=None):
    """T1 and `trips` round trips: T = 1 (mod 30) between `first` and 0.8 x end, with room for
    the dump after a round trip, sorted, from a generator seeded by the fixture and HEAD. A
    fixture's `first` puts every point after its human's interface work (the images gate reads
    the recording machine's interface state as unlisted state, the null twin classifying only
    what a viewer does without changing the game); it moves where the checks run, not what they
    check. Its `last_min` makes the last point reach a named completion checkpoint."""
    if first % 30 != 1:
        raise CouldNotRun(f"{name}: first {first} is not 1 (mod 30)")
    hi = min(int(0.8 * end), end - CONTINUE - 30)
    pool = list(range(first, hi + 1, 30))
    if len(pool) < trips + 1:
        raise CouldNotRun(f"{name}: end {end} leaves fewer than {trips + 1} GameTimes")
    rng = random.Random(int(hashlib.sha1((name + h).encode()).hexdigest()[:8], 16))
    times = sorted(rng.sample(pool, trips + 1))
    if last_min is not None:
        eligible = [t for t in pool if t >= last_min]
        if not eligible:
            raise CouldNotRun(f"{name}: end {end} leaves no GameTime at or after {last_min}")
        if times[-1] < last_min:
            times[-1] = rng.choice(eligible)
    return times


# ---- the four processes ---------------------------------------------------------------------

def arm(w, log, line, ok_rx, bad_rx, what):
    """The trigger line, then its answer: `ok_rx`, or a refusal of this very item."""
    n = len(log.lines)
    w.trigger(line)
    bad = bad_rx + rf"|{re.escape(TRIGGER)}: not understood"
    got = log.wait(f"{ok_rx}|{bad}", ACK_S, n)
    if not got or re.search(bad, got):
        raise CouldNotRun(f"{what}: {got.strip() if got else f'no answer in {ACK_S} s'}")


def turbo_engage(w, log):
    """With a turbo lane's k, `turbo on` -- after the launch acknowledged that k (tagpu_replay.c,
    THE TURBO), on the held level, after the lane's own arms: its ticks start with the `resume`."""
    if not w.turbo or log.turbo:
        return
    got = log.wait(r"turbo: (k=\d+ (at 0x49692D|UNAVAILABLE)|tagpu_turbo\.on refused.*)", ACK_S, 0)
    if not got or f"turbo: k={w.turbo} at 0x49692D" not in got:
        raise CouldNotRun(f"turbo k={w.turbo}: "
                          + (got.strip() if got else "the launch did not acknowledge its k"))
    arm(w, log, "turbo on", rf"turbo: on at GameTime \d+ \(k={w.turbo}\)", r"turbo: on refused",
        "turbo on")
    log.turbo = True


HOLD_RX = r"pause: paused at GameTime (\d+) \(the launch's hold\)"


def held_start(w, log):
    """The level as THE LAUNCH'S HOLD left it (tagpu_replay.c): held, its GameTime the hold's or,
    when a seek or a takeover ran on the held frames since, the one it set. Returns it. The hold's
    line is logged at the frame visit after the first step, which every launch verb waits for or
    passes before it returns."""
    got = log.wait(rf"{HOLD_RX}|hold: (tagpu_hold\.on refused|the launch's hold UNAVAILABLE).*",
                   ACK_S, 0)
    if not got or "paused at" not in got:
        raise CouldNotRun(f"the launch's hold: {got.strip() if got else f'no line in {ACK_S} s'}")
    if not peek(w, PAUSE_WORD) & 1:
        raise CouldNotRun(f"the launch's hold: the pause bit is clear ({got.strip()})")
    log.held, log.pending, log.at = True, None, w.gametime()
    if w.lane:
        J.progress(*w.lane, log.at)
    return log.at


def hold_at(w, log, T):
    """`pause at T` armed on the held level, past where it is held (tagpu_replay.c, THE PAUSE AT
    T): the level is paused at the frame visit after step T. Returns T."""
    n = len(log.lines)
    w.trigger(f"pause at {T}")
    got = log.wait(rf"pause: (armed for GameTime {T}\b|at {T} refused: .*)", ACK_S, n)
    if not got or "armed for" not in got:
        raise CouldNotRun(f"pause at {T}: {got.strip() if got else f'no answer in {ACK_S} s'}")
    log.pending, log.hold_from = T, n
    return T


def wait_paused(w, log, T, poll=None):
    """Until the pause at T is taken: a crash, a stalled level or one that ends first stops it, as
    does `poll` (called at every look, for a lane that watches the window meanwhile)."""
    got = log.wait_running(rf"pause: (paused at GameTime {T}\s*$|at {T} not taken.*)",
                           log.hold_from, poll)
    if "paused at" not in got:
        raise CouldNotRun(f"pause at {T}: {got.strip()}")
    log.pending, log.held, log.at = None, True, T
    if w.lane:
        J.progress(*w.lane, T)


def resume(w, log):
    arm(w, log, "resume", r"pause: resumed at GameTime \d+", r"resume: refused: .*", "resume")
    log.held = False


def adopt_held(w, log, what):
    """The level a script handed back, held where it left it: the pause bit set (the hold keeper
    keeps a taken pause until `resume`) and its GameTime read on the held level."""
    if not peek(w, PAUSE_WORD) & 1:
        raise CouldNotRun(f"{what}: the level was handed back running")
    log.held, log.pending, log.at = True, None, w.gametime()
    if w.lane:
        J.progress(*w.lane, log.at)


def run_to(w, log, G, steps, poll=None):
    """The held level driven to GameTime G and held there, each step of `steps` ((T, kind, arg),
    sorted, popped as run) with T <= G run while the level is held at its T, so it lands on that
    tick whatever the pacing. A `run` step, a script, is handed the level held at its T with the
    turbo on, and hands it back held where it advanced it to; a later step whose T that passed
    could not run. The turbo goes on (a turbo lane) before the first resume. Returns the GameTime
    held."""
    while True:
        if not log.held:
            raise CouldNotRun("run_to: the level is not held")
        while steps and steps[0][0] <= log.at:
            T, kind, arg = steps.pop(0)
            if T < log.at:
                raise CouldNotRun(f"the step `{kind}` at GameTime {T} had passed: the level is "
                                  f"held at {log.at}")
            if kind == "run":
                turbo_engage(w, log)
                step(w, kind, arg)
                adopt_held(w, log, f"the script {' '.join(arg)}")
            else:
                step(w, kind, arg)
        if log.at >= G:
            return log.at
        T = min(steps[0][0], G) if steps else G
        hold_at(w, log, T)
        turbo_engage(w, log)
        resume(w, log)
        wait_paused(w, log, T, poll)


def arm_dump(w, log, name, T):
    arm(w, log, f"dump {name} at {T}", rf"recorder: dump {name} armed for GameTime {T}",
        rf"recorder: dump(:| {name}) .*(refused|please|not letters)", f"dump {name} at {T}")


def cover_ask(w, log, name):
    """The coverage build's functions that ran so far, as replays/cover-<name>.bin; the DLL writes
    the file again at the level's end, with what ran after (tagpu_cover_flush)."""
    arm(w, log, f"cover {name}", rf"cover: replays\\cover-{name}\.bin: \d+ functions",
        rf"cover: .*{name}.*(refused|could not|NOT written)", f"cover {name}")


HUD_ROW_RX = re.compile(r"(\d+):(\d+)@(\d+)\.\.(\d+)/([LD])/c(\d+)(\*?)$")
HUD_STATE_RX = {
    "timelines": re.compile(r"replay: (.+?): hud state timelines (.*); axis (\d+)\.\.(\d+); span "
                            r"(\d+)\.\.(\d+); watched (\d+)\s*$"),
    "cache": re.compile(r"replay: (.+?): hud state cache serial (\d+)\s*$"),
    "rewind": re.compile(r"replay: (.+?): hud state rewind (\w+) available ([01]) span "
                         r"(\d+)\.\.(\d+) G (\d+) T (\d+)\s*$"),
    "result": re.compile(r"replay: (.+?): hud state result (\d+) (\w+) op (-?\d+) "
                         r"(ok|refused): (.*?)\s*$"),
}


def hud_state_parse(lines):
    """The four lines of the trigger's `hud state` (tagpu_replay.c, the trigger) as a dict, or
    None when one is missing: `name`; `timelines`, a row dict each (id, parent, fork_at, last,
    live, controlled, on_chain), [] for `none`; `axis` and `span` as (first, last); `watched`;
    `cache`, the timelines' serial; `rewind` (state, available, span, G, T); `result` (serial,
    kind, op, ok, why). The last of each line counts."""
    got = {}
    for ln in lines:
        for key, rx in HUD_STATE_RX.items():
            m = rx.search(ln)
            if m:
                got[key] = m
    if len(got) != len(HUD_STATE_RX):
        return None
    t, r, q = got["timelines"], got["rewind"], got["result"]
    rows = []
    if t[2] != "none":
        for text in t[2].split(" "):
            m = HUD_ROW_RX.match(text)
            if not m:
                return None
            rows.append({"id": int(m[1]), "parent": int(m[2]), "fork_at": int(m[3]),
                         "last": int(m[4]), "live": m[5] == "L", "controlled": int(m[6]),
                         "on_chain": m[7] == "*"})
    return {"name": t[1], "timelines": rows, "axis": (int(t[3]), int(t[4])),
            "span": (int(t[5]), int(t[6])), "watched": int(t[7]), "cache": int(got["cache"][2]),
            "rewind": {"state": r[2], "available": r[3] == "1", "span": (int(r[4]), int(r[5])),
                       "G": int(r[6]), "T": int(r[7])},
            "result": {"serial": int(q[2]), "kind": q[3], "op": int(q[4]), "ok": q[5] == "ok",
                       "why": q[6]}}


def hud_state(w, log):
    """What the HUD's recorder calls answer now (tagpu_replay.h, THE HUD'S RECORDER CALLS),
    through the trigger's `hud state`: hud_state_parse's dict. The result line is the last of the
    four the frame visit logs."""
    n = len(log.lines)
    w.trigger("hud state")
    if not log.wait(r"hud state result ", ACK_S, n):
        raise CouldNotRun(f"hud state: no answer in {ACK_S} s")
    got = hud_state_parse(log.lines[n:])
    if got is None:
        raise Failed(f"hud state: lines not understood: {log.lines[n:]}")
    return got


def wait_dumps(log, names, running=False):
    """Every named dump taken, or the first that will not be: on a level that runs toward them
    (`running`), bounded by a stall alone; else already taken, each line a frame's answer."""
    for name in names:
        rx = (rf"recorder: dump {name} (at GameTime|for GameTime \d+ not taken|refused)"
              rf"|recorder: dump {name}: ")
        got = log.wait_running(rx) if running else log.wait(rx, ACK_S)
        if not got or "at GameTime" not in got or "not taken" in got:
            raise CouldNotRun(f"dump {name}: {got.strip() if got else f'not taken in {ACK_S} s'}")


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
    elif kind == "keys":
        rc, o = tacli("keys", w.name, *arg, timeout=INPUT_TIMEOUT)
        if rc:
            raise CouldNotRun(f"keys {arg}: {last(o)}")
    elif kind == "type":
        # the chat line: one return opens it, the characters, return sends it. Each call returns
        # once the engine has consumed its events, so the opening return is handled before a
        # character is posted. A space goes as the `space` key: `char: ` is no token the input
        # layer types (tacli, _ui_text_tokens). The sending return is the chat GUI's TALK
        # submission (0x493BF0), which calls a `+` line's typed command 0x417B50 and the message's
        # 0x463E50 before it returns (DISASSEMBLED, the engine map); that the consumption the call
        # waits for is that dispatch is [INFERRED]. So the line has acted when it returns.
        for tokens in (["return"], ["space" if c == " " else f"char:{c}" for c in arg],
                       ["return"]):
            rc, o = tacli("keys", w.name, *tokens, timeout=INPUT_TIMEOUT)
            if rc:
                raise CouldNotRun(f"type {arg!r}: tacli keys {' '.join(tokens)}: {last(o)}")
    elif kind == "order":
        # `tacli order` returns once the applier says the order was issued, on the held level
        rc, o = tacli("roster", w.name, "--json", timeout=ACK_S)
        roster = json.loads(o)["units"] if not rc else []
        kind_, rank = arg["unit"]
        unit = resolve([kind_, rank], roster)
        argv = [resolve(a, roster) for a in arg["args"]]
        rc, o = tacli("order", w.name, argv[0], *argv[1:], "--unit", unit, "--expect", kind_,
                      timeout=ACK_S)
        if rc:
            raise CouldNotRun(f"order {kind_} {rank} {' '.join(argv)}: {last(o)}")
    elif kind == "run":
        # A fixture's assertions use the same native input path as a player.
        # They fail the live lane before state equality could bless a no-op. No wall bound: a
        # script advances the level by ticks it asks for, and each of its waits is bounded by
        # its own answer (resource_regression.py, IN LOCKSTEP), so a loaded machine makes it
        # slower and a hung game fails it.
        argv = [a.replace("{instance}", w.name).replace("{gamedir}", str(w.gamedir))
                for a in arg]
        if argv[0].endswith(".py"):
            argv = [sys.executable, str(TREE / argv[0])] + argv[1:]
        r = subprocess.run(argv, cwd=TREE, capture_output=True, text=True)
        if r.returncode:
            raise Failed(f"fixture assertion {' '.join(arg)}: {last(r.stdout + r.stderr)}")
        say(last(r.stdout))
    else:
        raise CouldNotRun(f"a step of kind {kind!r}")


def keys(w, *tokens):
    """`tacli keys`, which returns once the engine has consumed every event; a failure is the
    run's, named."""
    rc, o = tacli("keys", w.name, *tokens, timeout=INPUT_TIMEOUT)
    if rc:
        raise CouldNotRun(f"tacli keys {' '.join(tokens)}: {last(o)}")


def twin(w, log, fix):
    """The null twin (tarec images-gate), on the level the lane holds: frame dump ln1; then what
    the viewer does without changing the game -- the arrow keys each held until the eye moves, one
    wheel notch in and out at the map's centre, the cursor over the map and over the panel, the
    in-game menu (Tab) opened -- and frame dump ln2. GameTime must not move between them. The
    menu's OK then closes it; the hold keeper (tagpu_replay.c) keeps the level held through it,
    so the twin ends held where it began."""
    g1 = w.gametime()
    n = len(log.lines)
    w.trigger("dump ln1 now")
    if not log.wait(r"recorder: frame dump ln1 at GameTime", ACK_S, n):
        raise CouldNotRun(f"the twin: frame dump ln1 not taken in {ACK_S} s")
    # Sample clicks, cursor positions and camera positions while the world is
    # paused: returning to the same eye or coordinate would hide these viewer
    # writes from a two-image observation. A click is two entries of the
    # mouse-button ring, its press and its release, so whole clicks move the
    # read index by even steps only and never sample its low bit. The ring's
    # indices and the GUI's copy of the event do not rest on this sampling:
    # they are `leave` rows of replay-globals.tsv (mousehead, mousetail,
    # guimsg, guidblclick, guipopwparam).
    tacli("arm", w.name, "menu.open")
    try:
        rc, o = tacli("ui", w.name, "wait", "--gui", "RENDER.GUI", "--timeout", ACK_S,
                      timeout=ACK_S + 30)
        if rc:
            raise CouldNotRun(f"the twin: Impure options did not open: {last(o)}")
        rc, o = tacli("ui", w.name, "--json")
        if rc:
            raise CouldNotRun(f"the twin: Impure options could not be read: {last(o)}")
        gui = json.loads(o)
        labels = [g for g in gui["gadgets"] if g["type"] == "label" and g["active"]]
        if not labels:
            raise CouldNotRun("the twin: Impure options has no inert labels to sample")
        width, height = gui["surface"]
        rc, o = tacli("peek", w.name, "*0x511DE8+0x1422b", "*0x511DE8+0x1422f",
                      "--json", "--timeout", ACK_S, timeout=2 * ACK_S)
        if rc:
            raise CouldNotRun(f"the twin: map dimensions: {last(o)}")
        map_width, map_height = [int(p["value"].split()[0]) for p in json.loads(o)["peeks"]]
        points = ((0, 0), ((1 << (width.bit_length() - 1)) - 1,
                           (1 << (height.bit_length() - 1)) - 1), (width - 1, height - 1))
        for i, name in enumerate(NULL_VIEWS[:-1]):
            x, y = labels[i % len(labels)]["click"]
            keys(w, f"pclick:{x},{y}")
            # Click injection returns the pointer to screen centre. A separate
            # move and its native GUI echo establish the coordinate in this view:
            # the GUI pump copies the event when it runs, before the move is
            # acknowledged (tagpu_input.c), so one read after the call is the echo.
            x, y = points[i % len(points)]
            keys(w, f"pmove:{x},{y}")
            rc, o = tacli("peek", w.name, "*0x511DE8+0x555", "*0x511DE8+0x559",
                          "*0x511DE8+0x565", "--json", "--timeout", ACK_S, timeout=2 * ACK_S)
            if rc:
                raise CouldNotRun(f"the twin: native mouse echo: {last(o)}")
            event = [int(p["value"].split()[0]) for p in json.loads(o)["peeks"]]
            if event != [x, y, 0x200]:
                raise CouldNotRun(f"the twin: mouse event {event} after pmove:{x},{y}, "
                                  f"wanted {[x, y, 0x200]}")
            # The engine clamps these eyes to the map. The views are observations,
            # not assertions about the camera's permitted range.
            tacli("eye", w.name, i * map_width // (len(NULL_VIEWS) - 2),
                  i * map_height // (len(NULL_VIEWS) - 2))
            n = len(log.lines)
            w.trigger(f"dump {name} now")
            if not log.wait(rf"recorder: frame dump {name} at GameTime", ACK_S, n):
                raise CouldNotRun(f"the twin: frame dump {name} not taken")
            if w.gametime() != g1:
                raise CouldNotRun("the twin: GameTime moved during the viewer samples")
    finally:
        # disarmed on every way out: an armed panel opens in every later game on this instance,
        # and no exit route leaves RENDER.GUI
        tacli("arm", w.name, "menu.open=off")
    selections = [("lnselection", fix.get("viewer"))] + [
        (f"lnselection{i}", command) for i, command in enumerate(fix.get("viewer_more", []))]
    for name, command in selections:
        if command:
            step(w, "run", command)
        n = len(log.lines)
        w.trigger(f"dump {name} now")
        if not log.wait(rf"recorder: frame dump {name} at GameTime", ACK_S, n):
            raise CouldNotRun("the twin: selection frame dump not taken")
        if w.gametime() != g1:
            raise CouldNotRun("the twin: GameTime moved during the selection view")
    # The arrows, each held across frames: `tacli keys` returns once the engine has consumed a
    # batch, and the next batch is taken at a later trigger tick, so the scroll poll 0x41CE90
    # (called at 0x496976, on held frames too) sees each key down. Whether the camera then moves
    # is not the twin's to ask: the poll's step is ScrollSpeed (main+0x1434D) times main+0x38A3F,
    # the 30-Hz ticks the scheduler 0x495230 last measured between two frames (0x495243), and a
    # held frame skips the scheduler (0x496926), so a level paused after a frame shorter than a
    # 30-Hz tick keeps 0 and no arrow scrolls it (DISASSEMBLED; MEASURED 2026-10-09: c2-weapon-keys
    # held at 301 under the turbo read 0 there and its eye stood still, without the turbo 1 and
    # it scrolled). The camera's positions are the views' above. The wheel's tween (250 ms) is
    # time the view moves in, and a shorter one only adds presentation bits
    for key in ("up", "down", "left", "right"):
        keys(w, f"down:{key}")
        keys(w, f"up:{key}")
    keys(w, "pmove:512,384")
    keys(w, "wheel:+1")
    nap(0.8)
    keys(w, "wheel:-1")
    nap(0.8)
    keys(w, "pmove:700,500")
    keys(w, "pmove:60,300")
    # the in-game menu is Tab (ARMOPT.GUI): ln2 is taken with it open; Esc does nothing
    keys(w, "tab")
    rc, o = tacli("ui", w.name, "wait", "--gui", "ARMOPT.GUI", "--timeout", ACK_S,
                  timeout=ACK_S + 30)
    if rc:
        raise CouldNotRun(f"the twin: the in-game menu did not open: {last(o)}")
    n = len(log.lines)
    w.trigger("dump ln2 now")
    if not log.wait(r"recorder: frame dump ln2 at GameTime", ACK_S, n):
        raise CouldNotRun(f"the twin: frame dump ln2 not taken in {ACK_S} s")
    if w.gametime() != g1:
        raise CouldNotRun("the twin: GameTime moved between its dumps")
    rc, o = tacli("ui", w.name, "click", "OK", timeout=INPUT_TIMEOUT)
    if rc:
        raise CouldNotRun(f"the twin: the in-game menu's OK: {last(o)}")
    # closed: the screen under it is the world's, or a selected unit's build menu over it
    until_fact(w, lambda: json.loads(ui(w, "--json"))["gui"].upper() != "ARMOPT.GUI",
               "the twin: the in-game menu did not close")
    adopt_held(w, log, "the twin's OK")
    if log.at != g1:
        raise CouldNotRun(f"the twin: the menu's OK let the level run from {g1} to {log.at}")


def until_fact(w, fact, message):
    """Until `fact()` holds on a game that answers: a frame's answer, ACK_S at most."""
    end = time.monotonic() + ACK_S
    while not fact():
        if time.monotonic() >= end:
            raise CouldNotRun(f"{message} in {ACK_S} s")
        nap(0.1)


def live(w, fx, Ts, out):
    """L: the fixture recorded, dumps l<T1> and l<Tlast>; returns the recording's path. The level
    starts held at GameTime 1 with its scenario applied (THE LAUNCH'S HOLD)."""
    fix = fx["fix"]
    w.place(fix, fx["dll"])
    if fix.get("preflight"):
        # a native assertion with launches of its own, paced by the game and its own Pause key:
        # they run unheld, and the recording's launch after them is held again
        w.hold_lever(False)
        step(w, "run", fix["preflight"])
        w.hold_lever(w.held)
    los = [] if fix.get("los") == "file" else ["--los", str(fix.get("los", 0))]
    log = Log(w)
    rc, o = tacli("scenario", "load", w.name, SCENARIOS / fix["load"], "--defaults", *los,
                  "--restart", "--keep-dll", "--unit-limit", str(fix.get("unit_limit", UNIT_LIMIT)),
                  timeout=LAUNCH_TIMEOUT)
    if rc:
        raise CouldNotRun(f"scenario load {fix['load']}: {last(o)}")
    got = log.wait(r"recorder: (recording|not recording|FAILED)", ACK_S)
    if not got or "recorder: recording" not in got:
        raise CouldNotRun(f"the recorder: {got.strip() if got else f'no line in {ACK_S} s'}")
    held_start(w, log)
    steps = sorted(fix.get("steps", []), key=lambda st: st[0])
    names = [f"l{Ts[0]}", f"l{Ts[-1]}"] if Ts else []
    for name, T in zip(names, (Ts[0], Ts[-1]) if Ts else ()):
        arm_dump(w, log, name, T)
    if Ts:
        run_to(w, log, Ts[-1] + TWIN_AFTER, steps)
        wait_dumps(log, names)
        twin(w, log, fix)
    run_to(w, log, fix["end"], steps)
    if fx["cover"]:
        cover_ask(w, log, "L")
    n = len(log.lines)
    rc, o = tacli("scenario", "apply", w.name, END_SCENARIO, timeout=300)
    if rc:
        raise CouldNotRun(f"apply {END_SCENARIO.name}: {last(o)}")
    resume(w, log)
    for rx in (r"recorder: ended after", r"recorder: writer: .* closed"):
        log.wait_running(rx, n)
    wait_dumps(log, names)
    w.collect(out, recordings=True)
    w.stop()
    recs = sorted(out.glob("*.imprec"))
    if not recs:
        raise CouldNotRun("no .imprec in the live run's replays folder")
    rec = out / f"{fx['name']}.imprec"
    recs[-1].rename(rec)
    return rec


def play(w, fx, rec, out, proc, arms, dumps, roundtrips=()):
    """P or R: `replay play` from the start, held at its first step, the dumps and round trips
    armed, then resumed and each waited for; with no dump at all (measure), the recording played
    to its end."""
    w.place(fx["fix"], fx["dll"])
    log = Log(w)
    rc, o = tacli("replay", "play", w.name, rec, "--defaults", "--restart", "--keep-dll",
                  "--perspective", fx["viewed"], timeout=LAUNCH_TIMEOUT)
    if rc:
        raise CouldNotRun(f"replay play: {last(o)}")
    if w.held:
        held_start(w, log)
    for name, T in arms:
        arm_dump(w, log, name, T)
    for name, T in roundtrips:
        arm(w, log, f"roundtrip {name} at {T}", rf"round trip {name} armed for GameTime {T}",
            rf"round trip {name}( at {T})? refused", f"roundtrip {name} at {T}")
    turbo_engage(w, log)
    if log.held:
        resume(w, log)
    lines = {}
    for name, T in roundtrips:
        got = log.wait_running(rf"round trip {name} at {T}(: | not run)")
        if " not run" in got or "seek refused" in got:
            raise CouldNotRun(f"round trip {name}: {got.strip()}")
        lines[name] = got[got.index("replay:"):].strip()
    wait_dumps(log, dumps, running=True)
    if not dumps:
        got = log.wait_running(r"replay: .* (played to its end|cannot be read on|not playing)")
        if "played to its end" not in got:
            raise CouldNotRun(f"the playing: {got.strip()}")
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
                  "--timeout", ACK_S, timeout=LAUNCH_TIMEOUT + ACK_S)
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
    """(T, P) of a fixture's takeover: `at`, else the entry's `at`, else T2; the entry's player,
    the recording's controlled player for "controlled", or `player`."""
    fix = fx["fix"]
    tk = fix["takeover"]
    if "at" in tk:
        a = tk["at"]
        if not isinstance(a, int) or isinstance(a, bool) or not fix.get("first", 301) <= a < fix["end"]:
            raise CouldNotRun(f"{fx['name']}: the takeover's `at` {a!r} is not a GameTime in "
                              f"[{fix.get('first', 301)}, {fix['end']})")
    T = at if at is not None else tk.get("at", fx["Ts"][1])
    P = player if player is not None else tk["player"]
    if P == "controlled":
        P = h["options"]["controlled"]
    return T, int(P)


def takeover_k(w, fx, out):
    """K: L's recording taken over at T as player P in a fresh process; the game after T is
    timeline 1 of the same file. The playing is held at its first step and the takeover runs on
    a held frame, so the level is held at T-1 when the line comes (THE LAUNCH'S HOLD), and the
    fork's recording starts with the first resume. Returns what the takeover's checks read."""
    tc = compat()
    fix, tk = fx["fix"], fx["fix"]["takeover"]
    T, P = fx["tk_T"], fx["tk_P"]
    span = tk.get("dump", TAKEOVER_SPAN)
    res = {"T": T, "P": P, "boxes": [], "errorlog": False, "gone": False}
    w.place(fix, fx["dll"])
    (w.gamedir / "ErrorLog.txt").unlink(missing_ok=True)
    log = Log(w)
    # the dump is armed on the playing before the takeover, so an entry whose level ends soon
    # after T (`dump`) takes it before the end
    dump = f"k{T + span}"
    rc, o = tacli("replay", "takeover", w.name, fx["rec"], "--at", T, "--player", P,
                  "--dump", dump, "--dump-at", T + span,
                  "--defaults", "--restart", "--keep-dll", "--timeout", ACK_S, "--json",
                  timeout=LAUNCH_TIMEOUT + ACK_S)
    try:
        got = json.loads(o[o.index("{"):])
    except ValueError:
        raise CouldNotRun(f"replay takeover: {last(o)}")
    if rc or not got.get("taken") or not got.get("held"):
        why = got.get("line") or got.get("not_recorded") or got.get("timeout") or last(o)
        raise CouldNotRun(f"replay takeover: {why}")
    res.update(got)
    display = int(w.xvfb.display.lstrip(":"))
    seen = set()
    t0 = time.time()

    def watch():
        """the window, its boxes and ErrorLog.txt, at every wait from the takeover's line on"""
        tc.new_boxes(display, seen, res["boxes"], t0)
        if (w.gamedir / "ErrorLog.txt").exists():
            res["errorlog"] = True
        if not any(tc.main_window(*x) for x in tc.x_windows(display)):
            res["gone"] = True
            raise Failed("K: the game's window went away")

    try:
        held = held_start(w, log)
        if held != T - 1:
            raise CouldNotRun(f"K: the takeover's level is held at GameTime {held}, not {T - 1}")
        if "ai" in tk.get("checks", []):
            import tarewind
            start, stop = T, T + TAKEOVER_SPAN
            tarewind.arm_ai(sys.modules[__name__], w, log, "takeover", start, stop)
            res["ai_window"] = [start, stop]
        if fx["cover"]:
            cover_ask(w, log, "K")
        # what Impure draws JUDGE_TICKS after T, against what the takeover made (judge): the
        # first roster header the held level publishes
        held = run_to(w, log, T + JUDGE_TICKS, [], watch)
        # the fork's first capture, the first step after the resume, came before this hold
        rx = (r"recorder: recording timeline \d+ of .* from GameTime \d+ \(parent \d+\)"
              r"|recorder: (?:FAILED )?timeline \d+ of .* is not recorded: .*")
        rec = log.wait(rx, ACK_S)
        if not rec or "is not recorded" in rec:
            raise CouldNotRun(f"K: {rec.strip() if rec else 'no recording line for the fork'}")
        res["recording"] = rec[rec.index("recorder:"):].strip()
        n = len(log.lines)
        alive = log.wait(r"^units: alive=(\d+)", ACK_S, n)
        res["alive_seen"] = int(re.search(r"alive=(\d+)", alive).group(1)) if alive else None
        res["judged_at"] = held
        if tk.get("switches"):
            rc, o = tacli("switches", w.name, *[f"{k}={'on' if v else 'off'}"
                                                for k, v in tk["switches"].items()],
                          timeout=ACK_S)
            if rc:
                raise CouldNotRun(f"switches: {last(o)}")
        if tk.get("type"):
            step(w, "type", tk["type"])
        if tk.get("order"):
            res["ordered"] = tk_order(w, P, tk["order"], takeover_roster(log, held, P))
        # a takeover that checks the victory ends by its own chat line (the entry's `type`); any
        # other, by the end scenario at its dump's GameTime and the AI window's end, as L ends
        if "victory" in tk.get("checks", []):
            # the chat line can end the level on the held frames themselves (MEASURED
            # 2026-10-08: `+iwin` did), so the resume is asked for and not waited on, and taken
            # back if no level was left to read it
            n = 0
            log.read()
            if not any(re.search(r"recorder: ended after", ln) for ln in log.lines):
                w.trigger("resume")
            log.held = False
            log.wait_running(r"recorder: writer: .* closed", 0, watch)
            (w.gamedir / TRIGGER).unlink(missing_ok=True)
        else:
            run_to(w, log, max(T + span, res.get("ai_window", [0, 0])[1], held), [], watch)
            n = len(log.lines)
            rc, o = tacli("scenario", "apply", w.name, END_SCENARIO, timeout=300)
            if rc:
                raise CouldNotRun(f"apply {END_SCENARIO.name}: {last(o)}")
            resume(w, log)
        log.wait_running(r"recorder: writer: .* closed", n, watch)
    except Failed:
        if not res["gone"]:
            raise
    w.collect(out, recordings=True)
    w.stop()
    recs = sorted(out.glob("*.imprec"))
    if not recs:
        raise CouldNotRun("no .imprec in K's replays folder")
    res["file"] = str(recs[-1])
    return res


def takeover_roster(log, G, P):
    """The first complete roster the level publishes once it is held at G, after the takeover,
    never the loader's nor one from before the hold."""
    mark = log.wait(rf"pause: paused at GameTime {G}\s*$", ACK_S)
    if not mark:
        raise CouldNotRun(f"order: the level was not held at GameTime {G}")
    start = log.lines.index(mark) + 1
    row = log.wait(r"^\s+u\d+\s+", ACK_S, start)
    if not row:
        raise CouldNotRun("order: no roster after takeover")
    start = log.lines.index(row, start)
    # roster_log writes the dump as one block before its following units header.
    end = log.wait(r"^units: alive=", ACK_S, start + 1)
    if not end:
        raise CouldNotRun("order: the post-takeover roster is incomplete")
    rx = re.compile(r"^\s+u\d+\s+(\S+)\s+own=(\d+)\s+idx=(-?\d+)\s+"
                    r"world=\((-?\d+),(-?\d+),-?\d+\)")
    roster = []
    for line in log.lines[start:log.lines.index(end, start + 1)]:
        m = rx.match(line)
        if m:
            roster.append({"type": m[1], "owner": int(m[2]), "engine_index": int(m[3]),
                           "world": [int(m[4]), int(m[5])]})
    return roster


def tk_order(w, P, order, roster):
    """The `order` unit of P's (type, rank) moved 300 wu east: its slot and the GameTime the held
    level stands at when the order is given, from which the `orders` check reads the timeline
    300 ticks (10 s) on."""
    kind, rank = order
    units = [u for u in roster if u["owner"] == P and u["type"].upper() == kind and
             u["engine_index"]]
    if len(units) <= rank:
        raise CouldNotRun(f"order: no {kind} of rank {rank} among player {P}'s units")
    u = units[rank]
    x, z = u["world"]
    g = w.gametime()
    rc, o = tacli("order", w.name, "move", "pos", x + 300, z, "--unit", u["engine_index"],
                  "--expect", kind, timeout=ACK_S)
    if rc:
        raise CouldNotRun(f"order move {kind}: {last(o)}")
    return {"slot": u["engine_index"], "at": g}


def takeover_f(w, fx, out):
    """F: K's file played from T-300 in timeline 1 in a fresh process, held at T-301 after its
    seek, across the fork at T, to the dump f<T+300>."""
    T, P, tl = fx["tk_T"], fx["tk_P"], fx["tk"]["timeline"]
    at = T + min(TAKEOVER_BACK, fx["fix"]["takeover"].get("dump", TAKEOVER_SPAN))
    w.place(fx["fix"], fx["dll"])
    log = Log(w)
    rc, o = tacli("replay", "seek", w.name, T - TAKEOVER_BACK, "--recording", fx["tk"]["file"],
                  "--timeline", tl, "--defaults", "--restart", "--keep-dll", "--perspective", P,
                  "--timeout", ACK_S, timeout=LAUNCH_TIMEOUT + ACK_S)
    if rc:
        w.collect(out)
        w.stop()
        raise CouldNotRun(f"replay seek {T - TAKEOVER_BACK} --timeline {tl}: {last(o)}")
    held_start(w, log)
    arm_dump(w, log, f"f{at}", at)
    turbo_engage(w, log)
    resume(w, log)
    wait_dumps(log, [f"f{at}"], running=True)
    if fx["cover"]:
        cover_ask(w, log, "F")
    w.collect(out)
    w.stop()
    return at


# ---- W: the browser's Watch and the replay HUD ---------------------------------------------

def header_rewrite(rec, dst, rebuild):
    """A copy of the recording `rec` at `dst` whose first header block's payload is
    rebuild(its payload), the block stored again with both its CRCs. A header block is always
    stored (tagpu_recfmt.h)."""
    data = Path(rec).read_bytes()
    if data[:8] != tarec.REC_MAGIC:
        raise CouldNotRun(f"{rec}: not a recording")
    head = data[8:40]
    magic, kind, flags, first, upto, _raw, packed, _crc_body, _crc_head = \
        struct.unpack("<IHHIIIIII", head)
    if magic != tarec.REC_BLOCK_MAGIC or kind != tarec.RB_HEADER:
        raise CouldNotRun(f"{rec}: no header block")
    if flags != tarec.RBF_STORED:
        raise CouldNotRun(f"{rec}: a header block with flags {flags:#x}, not stored")
    body = rebuild(bytes(data[40:40 + packed]))
    head = struct.pack("<IHHIIIII", magic, kind, flags, first, upto, len(body), len(body),
                       zlib.crc32(body))
    head += struct.pack("<I", zlib.crc32(head))
    Path(dst).write_bytes(data[:8] + head + body + data[40 + packed:])


def header_copy(rec, dst, edits):
    """A copy of the recording `rec` at `dst` whose first header block's entry of each tag of
    `edits` holds edits[tag](its bytes), of the same length: a file the reader opens, which the
    replay's own checks then judge."""
    def rebuild(raw):
        payload = bytearray(raw)
        at, found = 0, set()
        while at + 4 <= len(payload):
            t, n = struct.unpack_from("<HH", payload, at)
            if t in edits:
                new = edits[t](bytes(payload[at + 4:at + 4 + n]))
                if len(new) != n:
                    raise CouldNotRun(f"{rec}: an edit of header entry {t} changed its length")
                payload[at + 4:at + 4 + n] = new
                found.add(t)
            at += (4 + n + 3) & ~3
        if set(edits) - found:
            raise CouldNotRun(f"{rec}: no header entry {min(set(edits) - found)}")
        return bytes(payload)
    header_rewrite(rec, dst, rebuild)


def format24_copy(rec, dst, seconds):
    """A copy of the recording `rec` at `dst` with a header in format 24's shape: RT_FORMAT 24
    and RT_WALLCLOCK the start alone, a FILETIME of `seconds` since 1970 (UTC) in 8 bytes, the
    other entries as they are. This build reads it as a recording of another format with no
    recorded start (tagpu_recfmt.h)."""
    def rebuild(raw):
        out, seen = bytearray(), set()
        for t, b in tarec.entries(raw):
            if t == tarec.RT_FORMAT:
                b = struct.pack("<I", 24)
            elif t == tarec.RT_WALLCLOCK:
                b = started_at(seconds)(b)[:8]
            seen.add(t)
            out += struct.pack("<HH", t, len(b)) + b + b"\0" * (-len(b) % 4)
        if {tarec.RT_FORMAT, tarec.RT_WALLCLOCK} - seen:
            raise CouldNotRun(f"{rec}: a header without RT_FORMAT or RT_WALLCLOCK")
        return bytes(out)
    header_rewrite(rec, dst, rebuild)


def started_at(seconds):
    """An edit of RT_WALLCLOCK: the start at `seconds` since 1970 (UTC), the offset from UTC
    kept (tagpu_recfmt.h)."""
    def edit(b):
        return struct.pack("<Q", int((seconds + 11644473600) * 10 ** 7)) + b[8:]
    return edit


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
    rc, o = tacli("peek", w.name, spec, "--json", "--timeout", ACK_S, timeout=2 * ACK_S)
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


def in_world(w, timeout):
    """The base in-game screen on top again, no menu over the world."""
    deadline = time.time() + timeout
    while True:
        gui = json.loads(ui(w, "--json"))["gui"].upper()
        if gui in ("ARMMAIN2.GUI", "CORMAIN2.GUI"):
            return
        if time.time() >= deadline:
            raise CouldNotRun(f"the world: {gui} still on top after {timeout} s")
        nap(0.2)


def hud(w, *args, timeout=60):
    """`tacli hud`: the replay HUD's controls, clicked in device space where the game drew them."""
    rc, o = tacli("hud", w.name, *args, "--timeout", timeout,
               timeout=timeout + INPUT_TIMEOUT)
    if rc:
        raise CouldNotRun(f"tacli hud {' '.join(map(str, args))}: {last(o)}")
    return o


def browser_watch(w, log, row, name):
    """Row `row` of the Replays browser selected and Watch pressed; the line that judged it:
    `replay: playing <name>` or `replay: not playing <name>: <why>`. The browser names the file
    it took, which must be `name`'s: the rows are newest first, as `watch_lane` dated them."""
    n = len(log.lines)
    ui(w, "select", "RECS", f"#{row}")
    ui(w, "click", "WATCH")
    took = log.wait(r"replays: Watch \S+", ACK_S, n)
    if not took or not took.rstrip().endswith(f" {name}.imprec"):
        raise CouldNotRun(f"the browser's row {row} is not {name}.imprec: "
                          f"{took.strip() if took else f'no Watch line in {ACK_S} s'}")
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


def watch_hud(w, log, rec):
    """The replay HUD's row actions and switches, clicked in device space while paused: Sight on
    the first row and again (the full map), with the clock and the camera unchanged; Follow,
    Follow again (the free camera) and Jump on the row of the seat the recording's human
    commanded (the perspective streams carry that seat's camera); the Cursors switch off and on.
    A row's buttons show while the pointer is over the row, which `tacli hud` hovers first."""
    seats = tarec.header_only(rec)["players"]
    if not seats:
        raise CouldNotRun("the HUD's rows: no recorded seats")
    viewed = "*0x511DE8+0x2A43:1"
    viewer, tick = peek(w, viewed), w.gametime()
    eye_specs = ("*0x511DE8+0x1431F:4", "*0x511DE8+0x14323:4")
    eye = tuple(peek(w, s) for s in eye_specs)

    def device(what, key, want):
        n = len(log.lines)
        hud(w, "click", key)
        got = log.wait(want, ACK_S, n)
        if not got:
            raise Failed(f"the HUD's {what}: no line matching {want!r} in {ACK_S} s")
        return got.strip()

    # the rows are the recording header's seats in order (tagpu_replay_players)
    for seat in (seats[0]["index"], viewer):
        n = len(log.lines)
        hud(w, "click", "sight:0")
        got = log.wait(rf"replay: sight is seat {seat} at GameTime {tick};|"
                       rf"replay: sight change to seat {seat} refused:", ACK_S, n)
        if not got or "refused:" in got or peek(w, viewed) != seat:
            raise Failed(f"the HUD's Sight: {got.strip() if got else 'no Sight reply'}")
        if w.gametime() != tick or tuple(peek(w, s) for s in eye_specs) != eye:
            raise Failed("the HUD's Sight changed the paused clock or camera")

    h = tarec.header_only(rec)
    own = (h.get("options") or {}).get("controlled")
    row = next((k for k, p in enumerate(seats) if p["index"] == own), None)
    if row is None:
        raise CouldNotRun(f"the HUD's rows: the controlled seat {own} has no row")
    lines = [device("Follow", f"follow:{row}", rf"replay: [^:]+: follow seat {own}, epoch \d+"),
             device("Follow again", f"follow:{row}", r"replay: [^:]+: .*free cam"),
             device("Jump", f"jump:{row}", rf"replay: [^:]+: jump to seat {own}, epoch \d+"),
             device("Cursors off", "cursors", r"replay: overlay cursors off"),
             device("Cursors on", "cursors", r"replay: overlay cursors on")]
    if w.gametime() != tick:
        raise Failed(f"the HUD's rows: Follow, Jump or a switch moved the paused clock from {tick} "
                     f"to {w.gametime()}")
    return ("W: the HUD's rows and switches", PASS, f"Sight {seats[0]['index']} and again "
            f"{viewer}; GameTime {tick} and camera {eye} unchanged; " + "; ".join(lines))


def hud_show(w):
    """The replay HUD's dump of the frame on screen, as `tacli hud show --json` gives it."""
    o = hud(w, "show", "--json")
    return json.loads(o[o.index("{"):])


def until_pick(w, state, what):
    """The HUD's dump once its picker (`pick`: 0 closed, 1 asked or seeking, 2 open) is in
    `state`: the picker moves at the fill, a frame or a seek after the click."""
    end = time.monotonic() + ACK_S
    while True:
        d = hud_show(w)
        if d["state"]["pick"]["state"] == state:
            return d
        if time.monotonic() >= end:
            raise Failed(f"{what}: the picker is {d['state']['pick']} after {ACK_S} s, not in "
                         f"state {state}")
        nap(0.2)


def lines_in_order(log, n, rxs, what):
    """Each of `rxs` matched by a line from `n` on, in that order; Failed naming the first missing."""
    log.read()
    k = n
    for rx in rxs:
        while k < len(log.lines) and not re.search(rx, log.lines[k]):
            k += 1
        if k == len(log.lines):
            raise Failed(f"{what}: no line matching {rx!r} in order after the click")
        k += 1


def watch_takeover(w, log, rec, stem, T, out, cover):
    """V-1B on the HUD's route (owner decision 1-B, snap back): the playback paused 15 ticks past a
    refresh R0, Take over seeks it back to R0 and opens the picker there, with a clickable row for
    exactly the players who still have a unit -- the rule the takeover refuses by -- and the row of
    the seat the recording's human commanded takes over at R0 itself, with no rounding. The fork
    plays 300 ticks and the game ends; offline its first iteration is R0 and equals its parent's
    chain there (compare-fork). First, past the fixture's end scenario (replay-end.json: every
    unit gone but one AI's), the picker at a refresh RD must dim exactly the seats with no unit,
    some of them, and the takeover itself must refuse one of those at RD by the same word. The
    coverage is asked for on the fork's level, so it holds the HUD's takeover. Returns its
    checks."""
    res = []
    seg0 = tarec.file_segments(rec)[0][0]
    first = seg0["first"]
    try:
        # the fixtures record a refresh every 30 ticks: R0 is within 30 of T + 300
        R0 = tarec.refresh_at(str(rec), 0, T + 300)
    except tarec.Unreadable as e:
        raise CouldNotRun(f"the HUD's takeover: the refresh at or before {T + 300}: {e}")
    X = R0 + 15
    if X >= seg0["last"]:
        raise CouldNotRun(f"the HUD's takeover: {X} is past the recording's end {seg0['last']}")
    hud(w, "click", "play")
    try:
        until_fact(w, lambda: peek(w, PAUSE_WORD) & 1, "the HUD's pause")
    except CouldNotRun as e:
        raise Failed(str(e))
    res.append(defeated_seats(w, log, rec, stem, seg0))
    n = len(log.lines)
    w.trigger(f"hud seek {X}")
    got = log.wait(rf"replay: {re.escape(stem)}: at GameTime {X}, sought from", ACK_S, n)
    if not got:
        raise CouldNotRun(f"the HUD's seek to {X}: no line in {ACK_S} s")

    # the snap back: the picker asks, its plan rounds to R0, the playback is sought there
    n = len(log.lines)
    hud(w, "click", "takeover")
    d = until_pick(w, 2, "Take over")
    lines_in_order(log, n, [rf"replay: {re.escape(stem)}: seek to {R0} asked by the HUD",
                            rf"replay: {re.escape(stem)}: at GameTime {R0}, sought from"],
                   "Take over's snap back")
    st, keys = d["state"], {h["key"] for h in d["hits"]["at"]}
    if st["pick"]["R"] != R0:
        raise Failed(f"Take over at {X - 1}: the picker starts at {st['pick']['R']}, not the "
                     f"refresh {R0}")
    if abs(st["t"] - (R0 - 1 - first) / 30) > 1e-3:
        raise Failed(f"the picker opened with the playhead at {st['t']:.3f} s, not R0 - 1's "
                     f"{(R0 - 1 - first) / 30:.3f} s")
    rows = [(k, p["units"]) for k, p in enumerate(st["seats"])]
    wrong = [k for k, u in rows if (f"takeas:{k}" in keys) != (u != 0)]
    if wrong:
        raise Failed(f"the picker's rows {wrong}: clickable is not 'has a unit' at R0 "
                     f"(units {[u for _, u in rows]})")
    out_rows = [k for k, u in rows if u == 0]
    res.append(("W: Take over snaps back to the refresh", PASS,
                f"paused at {X - 1}, the picker opened at R0 {R0} after the HUD's seek; "
                f"{len(rows) - len(out_rows)} rows clickable, {len(out_rows)} out of the game "
                f"{out_rows} not"))

    # the controlled seat's row: the takeover at R0 itself
    seats = tarec.header_only(rec)["players"]
    own = (tarec.header_only(rec).get("options") or {}).get("controlled")
    row = next((k for k, p in enumerate(seats) if p["index"] == own), None)
    if row is None:
        raise CouldNotRun(f"the picker: the controlled seat {own} has no row")
    n = len(log.lines)
    hud(w, "click", f"takeas:{row}")
    got = log.wait(rf"replay: {re.escape(stem)}: taken over at GameTime {R0} as player {own}\b.*"
                   rf"timeline (\d+) \(parent 0\) is recorded from GameTime {R0}\b"
                   rf"|replay: {re.escape(stem)}: takeover (at \d+ )?refused:", ACK_S, n)
    if not got or ": taken over at GameTime" not in got:
        raise Failed(f"the picker's takeover: {got.strip() if got else f'no line in {ACK_S} s'}")
    tl = int(re.search(r"timeline (\d+) \(parent 0\)", got).group(1))
    lines_in_order(log, n, [rf"a takeover as player {own} at GameTime {R0} waits"], "the picker's takeover")
    if any("takes over at" in ln for ln in log.lines[n:]):
        raise Failed("the picker's takeover was rounded: it asked for a GameTime that is not R0")
    res.append(("W: the picker takes over at R0", PASS, got[got.index("replay:"):].strip()))

    # 300 ticks of the fork, held there; the HUD's pause is the Pause key's bit, which the key
    # clears, and `pause at` holds the level at the end
    hold_at(w, log, R0 + 300)
    if peek(w, PAUSE_WORD) & 1:
        rc, o = tacli("keys", w.name, "pause", timeout=INPUT_TIMEOUT)
        if rc:
            raise CouldNotRun(f"tacli keys pause: {last(o)}")
    wait_paused(w, log, R0 + 300)
    if cover:
        cover_ask(w, log, "W")
    n = len(log.lines)
    rc, o = tacli("scenario", "apply", w.name, END_SCENARIO, timeout=300)
    if rc:
        raise CouldNotRun(f"apply {END_SCENARIO.name}: {last(o)}")
    resume(w, log)
    log.wait_running(r"recorder: writer: .* closed", n)
    if not log.wait(rf"replay: forks .*\b{re.escape(stem)}\.imprec: (done|refused):", 120, n):
        raise CouldNotRun("the fork's level did not complete its file maintenance")
    # the skirmish ends on its results screen; its MainMenu builds the main menu, over which the
    # browser the recording was watched from opens again (tagpu_replay.c, menu_built) and logs
    # its init's last line: the quit's Back is sent after it
    ui_on(w, "ENDMSN.GUI", 120)
    n = len(log.lines)
    ui(w, "click", "MainMenu")
    if not log.wait(r"replays: opened - ", 120, n):
        raise CouldNotRun("the fork's results screen did not return to the Replays browser")
    ui_on(w, "REPLAYS.GUI", ACK_S)
    out.mkdir(parents=True, exist_ok=True)
    copy = out / "W-takeover.imprec"
    shutil.copy2(fork_file(w, log), copy)
    seg = next((s for s in tarec.file_segments(copy)[0] if s["id"] == tl), None)
    if not seg or seg["fork_at"] != R0:
        raise Failed(f"the picker's fork {tl}: fork_at {seg['fork_at'] if seg else 'none'}, not R0 {R0}")
    rc, o = tarec_run("compare-fork", copy, tl)
    if rc:
        raise Failed(f"the picker's fork {tl}: compare-fork: {last(o)}")
    res.append(("W: the picker's fork", PASS, f"timeline {tl} from R0 {R0}; compare-fork: {last(o)}"))
    return res


def defeated_seats(w, log, rec, stem, seg0):
    """W's picker where seats are out of the game: paused, the playback sought 5 past the last
    refresh RD of the recording, which the end scenario has left with one AI's unit; Take over
    snaps back to RD and its clickable rows are exactly the seats with a unit, some seats having
    none; then the trigger's takeover of the first seat with none, at RD itself, is refused for
    having no unit there, the word tk_bound reads (+0x144) and the column's `out` with it. Leaves
    the playback paused with the picker closed. Returns its check."""
    try:
        RD = tarec.refresh_at(str(rec), 0, seg0["last"] - 30)
    except tarec.Unreadable as e:
        raise CouldNotRun(f"the defeated seats: the refresh at or before {seg0['last'] - 30}: {e}")
    n = len(log.lines)
    w.trigger(f"hud seek {RD + 5}")
    if not log.wait(rf"replay: {re.escape(stem)}: at GameTime {RD + 5}, sought from", ACK_S, n):
        raise CouldNotRun(f"the HUD's seek to {RD + 5}: no line in {ACK_S} s")
    hud(w, "click", "takeover")
    d = until_pick(w, 2, "Take over past the end scenario")
    st, keys = d["state"], {h["key"] for h in d["hits"]["at"]}
    if st["pick"]["R"] != RD:
        raise Failed(f"Take over at {RD + 4}: the picker starts at {st['pick']['R']}, not {RD}")
    units = [p["units"] for p in st["seats"]]
    wrong = [k for k, u in enumerate(units) if (f"takeas:{k}" in keys) != (u != 0)]
    if wrong:
        raise Failed(f"the picker's rows {wrong} at {RD}: clickable is not 'has a unit' (units {units})")
    out = [k for k, u in enumerate(units) if u == 0]
    if not out or len(out) == len(units):
        raise CouldNotRun(f"the defeated seats: at {RD} the units are {units}, not some seats out")
    hud(w, "click", "takeover")
    until_pick(w, 0, "Take over closed past the end scenario")
    P = tarec.header_only(rec)["players"][out[0]]["index"]
    n = len(log.lines)
    w.trigger(f"takeover {P} at {RD}")
    got = log.wait(rf"replay: {re.escape(stem)}: (takeover (at \d+ )?refused:|taken over at)", ACK_S, n)
    want = f"player {P} has no unit at GameTime {RD}"
    if not got or want not in got:
        raise Failed(f"the takeover of row {out[0]}, out at {RD}: "
                     f"{got.strip() if got else f'no line in {ACK_S} s'}; expected '{want}'")
    return ("W: the picker dims the seats the takeover refuses", PASS,
            f"at RD {RD}: rows {out} out (units {units}), not clickable; the takeover of player {P} "
            f"refused: {want}")


def forks_picker(w, log, T5, P):
    """X's picker on "playing 5", held at T5 (its next iteration T5 + 1 is fork 5's second, a
    refresh): Take over opens at once with no seek; its eviction is the dry run's; closed again;
    then from T5 - 1, whose next iteration T5 is the fork's first, which no takeover starts at, the
    plan snaps one tick forward and the picker opens at T5 + 1 after the seek there. Leaves the
    playing held at T5 with the picker closed. Returns its check."""
    n = len(log.lines)
    hud(w, "click", "takeover")
    d = until_pick(w, 2, "X's Take over")
    log.read()
    if any(re.search(r"seek to \d+ asked by the HUD", ln) for ln in log.lines[n:]):
        raise Failed("X's Take over at a refresh sought: R is the next iteration, no seek runs")
    st = d["state"]
    if st["pick"]["R"] != T5 + 1:
        raise Failed(f"X's Take over: the picker starts at {st['pick']['R']}, not {T5 + 1}")
    n = len(log.lines)
    w.trigger(f"takeover {P} at {T5 + 1} dry-run")
    got = log.wait(r"replay: .*: (fork would delete (no timeline|timeline (\d+))|fork prediction refused: .*)",
                   ACK_S, n)
    if not got or "would delete" not in got:
        raise Failed(f"X's dry run: {got.strip() if got else f'no line in {ACK_S} s'}")
    m = re.search(r"would delete timeline (\d+)", got)
    E = int(m.group(1)) if m else None
    ids = [f["id"] for f in st["forks"]]
    want_replace = ids.index(E) if E in ids else -1
    if st["pick"]["evict"] != E or st["replace"] != want_replace:
        raise Failed(f"X's picker: evict {st['pick']['evict']} replace {st['replace']}; the dry run "
                     f"deletes {E}, lane {want_replace} of {ids}")
    hud(w, "click", "takeover")
    until_pick(w, 0, "X's Take over closed")

    # the forward snap: T5 is the fork's first iteration
    n = len(log.lines)
    w.trigger(f"hud seek {T5}")
    if not log.wait(rf"replay: .*: at GameTime {T5}, sought from", ACK_S, n):
        raise CouldNotRun(f"X's seek to {T5}: no line in {ACK_S} s")
    n = len(log.lines)
    hud(w, "click", "takeover")
    d = until_pick(w, 2, "X's Take over at a first iteration")
    lines_in_order(log, n, [rf"replay: .*: seek to {T5 + 1} asked by the HUD",
                            rf"replay: .*: at GameTime {T5 + 1}, sought from"], "X's forward snap")
    if d["state"]["pick"]["R"] != T5 + 1:
        raise Failed(f"X's forward snap: the picker starts at {d['state']['pick']['R']}, not {T5 + 1}")
    hud(w, "click", "takeover")
    until_pick(w, 0, "X's Take over closed")
    return ("forks: the HUD's picker", PASS,
            f"at T5 {T5}: opened at {T5 + 1} with no seek, evicting {E} as the dry run does "
            f"(lane {want_replace}); from {T5 - 1}: snapped forward to {T5 + 1}")


def watch_lane(w, fx, rec, out, T):
    """W: the playback started as a player starts it, through the Replays browser's Watch, and
    driven through the replay HUD: two copies of the recording refused, the data refusal's marked
    and the missing map's not, then the recording watched, its speed changed on the HUD, the
    in-game Options' OK (the settings save), a pause, the HUD's seek to T with dump w<T>, the
    resume, Leave, and a second Watch from the browser it returns to.
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
        header_copy(rec, rd / f"{name}.imprec", {tag: edit})
        os.utime(rd / f"{name}.imprec", (now - 60 * k, now - 60 * k))
    shutil.copyfile(rec, rd / f"{stem}.imprec")
    os.utime(rd / f"{stem}.imprec", (now - 60 * len(copies),) * 2)
    log = Log(w)
    rc, o = tacli("launch", w.name, "--defaults", "--keep-dll", timeout=LAUNCH_TIMEOUT)
    if rc:
        raise CouldNotRun(f"tacli launch: {last(o)}")
    ui_on(w, "MAINMENU.GUI", 120)
    ui(w, "click", "REPLAYS")
    ui_on(w, "REPLAYS.GUI", ACK_S)

    # the refusals: the header's check (watch_refuse), then the Start's own, past the undoable
    # steps (watch_undo); each notice answered, back to the browser
    for row, (name, _, _, why) in enumerate(copies):
        got = browser_watch(w, log, row, name)
        if got != f"replay: not playing {name}: {why}":
            raise Failed(f"Watch {name}: {got} (expected the refusal: {why})")
        ui_on(w, "MSGBOX.GUI", ACK_S)
        ui(w, "click", "OK")
        ui_on(w, "REPLAYS.GUI", ACK_S)
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
    hud(w, "wait", timeout=ACK_S)
    res.append((f"W: Watch plays {stem}", PASS, got))

    # the HUD's speed, then the settings save with it changed: the store keeps the player's. The
    # live speed reaches the ceiling by the scheduler's throttle, frames after the click
    # (0x495465..0x49547B), so it is waited for
    hud(w, "click", "speed")
    speeds = []

    def raised():
        speeds[:] = [peek(w, SPEED_CEIL), peek(w, SPEED_LIVE)]
        return speeds[0] != ceil0 and speeds[1] == speeds[0]
    try:
        until_fact(w, raised, "the HUD's speed")
    except CouldNotRun:
        raise Failed(f"the HUD's speed: {ceil0} became ceiling {speeds[0]}, live {speeds[1]}")
    ceil1, live1 = speeds
    if ceil1 not in (5, 10, 20, 40, 80):
        raise Failed(f"the HUD's speed: {ceil0} became ceiling {ceil1}, live {live1}")
    rc, o = tacli("keys", w.name, "tab", timeout=INPUT_TIMEOUT)
    if rc:
        raise CouldNotRun(f"tacli keys tab: {last(o)}")
    ui_on(w, "ARMOPT.GUI", ACK_S)
    ui(w, "click", "PREFS")
    ui_on(w, "PREFS.GUI", ACK_S)
    ui(w, "click", "PREV")
    ui_on(w, "ARMOPT.GUI", ACK_S)
    after, ceil2 = store_speed(w), peek(w, SPEED_CEIL)
    if after != stored or ceil2 != ceil1:
        raise Failed(f"the settings save during the playing: the store's gamespeed {stored} -> "
                     f"{after}, the ceiling {ceil1} -> {ceil2}")
    ui(w, "click", "OK")
    in_world(w, ACK_S)
    res.append(("W: the HUD's speed and the settings save", PASS,
                f"speed {ceil0} -> {ceil1} on the HUD; the save stored gamespeed {after}, the "
                f"player's, and left the playing at {ceil2}"))

    # pause, the HUD's seek to T with its dump, resume. The pause bit is the HUD's answer; then
    # two peeks, each answered at a later flip than the last, find GameTime standing
    hud(w, "click", "play")
    try:
        until_fact(w, lambda: peek(w, PAUSE_WORD) & 1, "the HUD's pause")
    except CouldNotRun as e:
        raise Failed(str(e))
    g1 = w.gametime()
    if w.gametime() != g1 or not peek(w, PAUSE_WORD) & 1:
        raise Failed(f"the HUD's pause: the pause word or GameTime moved (at {g1})")
    n = len(log.lines)
    w.trigger(f"hud seek {T}")
    got = log.wait(rf"replay: {re.escape(stem)}: (at GameTime {T}, sought from|seek to {T} "
                   rf"refused)|a seek to {T} from the HUD refused", ACK_S, n)
    if not got or "sought from" not in got:
        raise Failed(f"the HUD's seek to {T}: {got.strip() if got else f'no line in {ACK_S} s'}")
    res.append(watch_hud(w, log, rec))
    arm_dump(w, log, f"w{T}", T)
    hud(w, "click", "play")
    wait_dumps(log, [f"w{T}"], running=True)
    if peek(w, PAUSE_WORD) & 1:
        raise Failed("the HUD's resume: the pause word still set")
    res.append(("W: the HUD's pause, seek and resume", PASS, got[got.index("replay:"):].strip()))

    # Leave: the teardown puts the player's speed back, and the browser returns
    n = len(log.lines)
    hud(w, "click", "leave")
    ui_on(w, "YESORNO.GUI", ACK_S)
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
    # All three files pass the mod filter. scan moves their existing index entries into list
    # order and index_save preserves it. A playing-end touch can move the watched file's row.
    listed = list(index_lines(rd))
    if set(listed) != {f"{stem}.imprec", *(f"{c[0]}.imprec" for c in copies)}:
        raise CouldNotRun("the returned browser's index does not contain its three recordings")
    got = browser_watch(w, log, listed.index(f"{stem}.imprec"), stem)
    if not got.startswith(f"replay: playing {stem}:"):
        raise Failed(f"the second Watch: {got}")
    hud(w, "wait", timeout=ACK_S)
    res += watch_takeover(w, log, rec, stem, T, out, fx["cover"])
    w.collect(out)
    w.stop()
    return res


def forks_lane(w, fx, out):
    """L's recording, four root forks, an LRU touch, a fork of a fork, parent deletion,
    chain playback/export, and deletion of the highest ID followed by another fork. The first root
    fork and the fork of a fork are asked for inside their parent's first refresh interval, so
    each starts at its parent's second iteration (tagpu_replay.c, takeover_point). The tarec
    checks of each step run on the offline pool while the lane plays its next steps (Ordered):
    the lane's results and its first failure are the ones it would have met in order."""
    results = Ordered()
    with results:
        forks_steps(w, fx, out, results)
        got = results.results()
    shutil.rmtree(out / "checked", ignore_errors=True)    # kept where a check did not pass
    return got


def check_or_fail(why, *args):
    """A tarec check that stops the forks lane: Failed(`why`: its last line) unless it exits 0."""
    rc, text = tarec_run(*args)
    if rc:
        raise Failed(f"{why}: {last(text)}")
    return []


def prune_step(w, fx, out, forked):
    """THE PRUNE (tagpu_recfile.h) on a recorded launch of the fixture's scenario, under the
    instance's `keep 2` (Worker): replays\\ seeded, the recorded starts (RT_WALLCLOCK) oldest
    last, with two plain copies of L's recording, a favourite one, a recording with live forks
    (`forked`, the lane's own), one in format 24's shape (format24_copy) and a file that is not a
    recording. The last two have no start this build reads, so they sort after the dated four,
    the format-24 one first by name. The files' last-write times run the other way, so a prune
    that ordered by them would keep the non-recording as the newest and delete the first plain
    copy. Besides the new recording the newest is kept; of the rest the second plain copy and the
    format-24 one go, and the favourite, the forked one and the non-recording stay.
    Returns its check."""
    w.place(fx["fix"], fx["dll"])
    rd = w.gamedir / "replays"
    rd.mkdir(exist_ok=True)
    rec, now = Path(fx["rec"]), time.time()
    seeds = ("plain-a", "plain-b", "starred", "forked", "older", "not-a-recording")
    for k, name in enumerate(seeds):
        dst = rd / f"0000 {name}.imprec"
        start = {tarec.RT_WALLCLOCK: started_at(now - 600 * (k + 1))}
        if name == "forked":
            header_copy(forked, dst, start)
        elif name == "older":
            format24_copy(rec, dst, now - 600 * (k + 1))
        elif name == "not-a-recording":
            dst.write_bytes(b"not a recording")
        else:
            header_copy(rec, dst, start)
        os.utime(dst, (now - 600 * (len(seeds) - k),) * 2)
    star = rd / "0000 starred.imprec"
    # an index line with the favourite flag set: name size format start last end fav played map
    # nseats marks (tagpu_replays.c, the index)
    (rd / "index.dat").write_text("IMPURE-REPLAYS-INDEX\t1\n"
                                  f"{star.name}\t{star.stat().st_size}\t0\t0\t0\t0\t1\t0\t-\t0\t\n",
                                  newline="\n")
    log = Log(w)
    rc, text = tacli("scenario", "load", w.name, SCENARIOS / fx["fix"]["load"], "--defaults",
                     "--restart", "--keep-dll", "--json", timeout=300)
    if rc:
        raise CouldNotRun(f"prune: load: {last(text)}")
    got = log.wait(r"recorder: prune at .*", ACK_S)
    if fx["cover"]:
        cover_ask(w, log, "prune")
    left = sorted(p.name for p in rd.glob("0000 *.imprec"))
    w.collect(out / "prune")
    w.stop()
    want = ("6 older recordings, the newest 1 kept; of the rest, 2 deleted, 2 kept (forked or "
            "marked), 0 in use, 1 unreadable")
    kept = sorted(f"0000 {n}.imprec" for n in ("plain-a", "starred", "forked", "not-a-recording"))
    if not got or want not in got or left != kept:
        raise Failed(f"prune: {got.strip() if got else f'no prune line in {ACK_S} s'}; left {left}")
    return ("forks: the prune", PASS, f"{want}; left {', '.join(left)}")


def fork_file(w, log):
    """The file a takeover's fork is recorded into, as the recorder's line names it relative to
    the game's folder: the line comes at the fork's first capture, the first step after a
    resume."""
    got = log.wait(r"recorder: recording timeline \d+ of .* from GameTime \d+ \(parent \d+\)",
                   ACK_S)
    if not got:
        raise CouldNotRun("no recording line for the fork")
    m = re.search(r"recorder: recording timeline \d+ of (.*) from GameTime", got)
    return w.gamedir / m.group(1).replace("\\", "/")


def forks_steps(w, fx, out, results):
    import hashlib
    out.mkdir(parents=True, exist_ok=True)
    source = out / "forks.imprec"
    shutil.copy2(fx["rec"], source)
    original, _ = tarec.file_segments(source)
    lo, hi = original[0]["range"]
    original_hash = hashlib.sha256(source.read_bytes()[lo:hi]).hexdigest()
    P = tarec.header_only(source)["options"]["controlled"]
    base = max(601, fx["Ts"][0] + 120)
    if base + 900 >= fx["fix"]["end"]:
        base = 301

    def copies(label, *paths):
        """What a check handed over reads: copies taken now, in out/checked/<label>/, of files
        the lane goes on to change (`source`) or to delete (the game's replays folder)."""
        d = out / "checked" / re.sub(r"\W+", "-", label)
        d.mkdir(parents=True, exist_ok=True)
        for p in paths:
            shutil.copy2(p, d / Path(p).name)
        return [d / Path(p).name for p in paths]

    def check_file(path, label):
        segs, stop = tarec.file_segments(path)
        if stop:
            raise Failed(f"{label}: {stop}")
        live = [s for s in segs if s["state"] == 1]
        if len([s for s in live if s["id"]]) > 3:
            raise Failed(f"{label}: more than three live forks")
        copy, = copies(label, path)
        for s in live:
            results.later(check_or_fail, f"{label}: timeline {s['id']}", "check", copy,
                          "--timeline", s["id"])
        if path == source:
            lo, hi = segs[0]["range"]
            if hashlib.sha256(path.read_bytes()[lo:hi]).hexdigest() != original_hash:
                raise Failed(f"{label}: original blocks changed")
        results.add((f"forks: {label}", PASS,
                     f"live {[s['id'] for s in live]}, physical {[s['id'] for s in segs]}"))
        return segs

    def finish_play(log, tl):
        n = len(log.lines)
        keys(w, "tab")
        ui(w, "click", "EXIT")
        ui(w, "click", "MAINMENU")
        keys(w, "y")
        if not log.wait(r"replay: .* at its level's end", 120, n):
            raise CouldNotRun("forks: playback did not leave")
        if not log.wait(rf"replay: forks .*: done: touched timeline {tl}", 120, n):
            raise Failed(f"forks: timeline {tl}'s playing did not touch its last-used time")

    def fork(tid, T, parent=0):
        """Timeline `tid` taken over from `parent` at T, which starts at the refresh at or before
        it (tk_bound): that GameTime, which the fork names as its fork_at."""
        try:
            R = tarec.refresh_at(str(source), parent, T)
        except tarec.Unreadable as e:
            raise CouldNotRun(f"forks: K{tid}: the refresh at or before {T}: {e}")
        say(f"forks: K{tid} from {parent} at {T}" + (f", the refresh {R}" if R != T else ""))
        T = R
        w.place(fx["fix"], fx["dll"])
        log = Log(w)
        read = copies(f"K{tid} read", source)[0]
        rc, text = tacli("replay", "takeover", w.name, source, "--timeline", parent,
                         "--at", T, "--player", P, "--dump", f"fork{tid}", "--dump-at", T + 450,
                         "--defaults", "--restart", "--keep-dll", "--json", "--timeout", ACK_S,
                         timeout=LAUNCH_TIMEOUT + ACK_S)
        if rc:
            raise Failed(f"forks: K{tid}: {last(text)}")
        got = json.loads(text[text.index("{"):])
        if got.get("timeline") != tid:
            raise Failed(f"forks: K{tid} allocated {got.get('timeline')}")
        if got.get("at") != T:
            raise Failed(f"forks: K{tid} taken over at {got.get('at')}, not at the refresh {T}")
        # held at T-1 by the takeover, then at the dump's GameTime, so the fork ends there
        held_start(w, log)
        run_to(w, log, T + 450, [])
        if fx["cover"]:
            cover_ask(w, log, f"K{tid}")
        wait_dumps(log, [f"fork{tid}"])
        n = len(log.lines)
        rc, text = tacli("scenario", "apply", w.name, END_SCENARIO, timeout=300)
        if not rc:
            resume(w, log)
        if rc or not log.wait_running(r"recorder: writer: .* closed", n):
            raise CouldNotRun(f"forks: K{tid} did not end: {last(text)}")
        if not log.wait(r"replay: forks .*: (done|refused):", 120, n):
            raise CouldNotRun(f"forks: K{tid} did not complete maintenance")
        w.collect(out / f"K{tid}", recordings=True)
        w.stop()
        results.later(work_check, f"forks: K{tid}'s takeover", out / f"K{tid}" / "tagpu.log",
                      read, parent, True)
        shutil.copy2(fork_file(w, log), source)
        check_file(source, f"K{tid}")
        return T

    def start_play(tl, T, label):
        """The file played from T in timeline tl, held at T-1 after its seek; its seeks' work is
        checked against a copy of the file as it was read, under `label`."""
        say(f"forks: playing {tl} from {T}")
        w.place(fx["fix"], fx["dll"])
        log = Log(w)
        read = copies(f"{label} read", source)[0]
        rc, text = tacli("replay", "seek", w.name, T, "--recording", source,
                         "--timeline", tl, "--defaults", "--restart", "--keep-dll",
                         "--perspective", P, "--timeout", ACK_S, timeout=LAUNCH_TIMEOUT + ACK_S)
        if rc:
            raise Failed(f"forks: play timeline {tl}: {last(text)}")
        held_start(w, log)
        if fx["cover"]:
            cover_ask(w, log, f"X{tl}")
        plays.append((label, read, tl))
        return log

    def plays_checked(sub):
        """The seeks of the playings started since the last collection, each against its file,
        from the log collected into `sub`."""
        for label, read, tl in plays:
            results.later(work_check, f"forks: {label}'s seeks", out / sub / "tagpu.log", read,
                          tl, False)
        plays.clear()

    plays = []

    def hud_route(log, tl):
        """The HUD's recorder calls on the playing of timeline `tl` (tagpu_replay.h, THE HUD'S
        RECORDER CALLS): `hud state` against the file the game reads, then the HUD's fork route --
        the playing timeline's deletion refused as in use, an export of timeline 4 done, and the
        two outcomes in turn."""
        game = w.gamedir / "replays" / "forks.imprec"
        copy, = copies("hud state", game)
        segs, stop = tarec.file_segments(copy)
        if stop:
            raise Failed(f"forks: hud state: the file: {stop}")
        st = hud_state(w, log)
        want = [(s["id"], s["parent"], s["fork_at"], s["last"] or 0, s["state"] == 1)
                for s in segs]
        rows = st["timelines"]
        got = [(r["id"], r["parent"], r["fork_at"], r["last"], r["live"]) for r in rows]
        if got != want:
            raise Failed(f"forks: hud state timelines {got}, the file's {want}")
        # a fork's header names the seat it was played as: the takeover's player
        seats = {r["id"]: r["controlled"] for r in rows if r["id"]}
        if any(c != P for c in seats.values()):
            raise Failed(f"forks: hud state: the forks' controlled seats {seats}, not {P}")
        start = segs[0]["header"]["start"]
        last = max(s["last"] for s in segs if s["state"] == 1 and s["last"] is not None)
        if st["axis"] != (start, last):
            raise Failed(f"forks: hud state axis {st['axis']}, the file's ({start}, {last})")
        chain = [s["id"] for s in tarec.chain(segs, tl)]
        played = next(s for s in segs if s["id"] == tl)
        if st["span"] != (start, played["last"]):
            raise Failed(f"forks: hud state span {st['span']}, timeline {tl}'s chain "
                         f"({start}, {played['last']})")
        if st["watched"] != tl or [r["id"] for r in rows if r["on_chain"]] != chain:
            raise Failed(f"forks: hud state watches {st['watched']} on "
                         f"{[r['id'] for r in rows if r['on_chain']]}, not {tl} on {chain}")
        # the in-game prediction is the plan's (tagpu_replay_takeover_plan), against the file's
        T = played["last"]
        R = tarec.refresh_at(str(copy), tl, T)
        evict = tarec.fork_prediction(segs, tl, R)
        want = "fork would delete " + ("no timeline" if evict is None else f"timeline {evict}")
        n = len(log.lines)
        w.trigger(f"takeover {P} at {T} dry-run")
        said = log.wait(r"replay: .*: (fork would delete (no timeline|timeline \d+)|fork "
                        r"prediction refused: |takeover refused: )", ACK_S, n)
        rounded = R == T or log.wait(rf"a takeover asked for at GameTime {T} takes over at {R}, ",
                                     0, n)
        if not said or want not in said or not rounded:
            raise Failed(f"forks: the in-game prediction at {T}: "
                         f"{said.strip() if said else 'no answer'}; the file's: {want} from {R}")
        # a refused seek's outcome, the HUD's toast: GameTime 0 is outside every recording
        n = len(log.lines)
        w.trigger("seek 0")
        if not log.wait(r"replay: .*: seek to 0 refused: ", ACK_S, n):
            raise Failed("forks: a seek to GameTime 0 was not refused")
        sought = hud_state(w, log)["result"]
        if (sought["kind"], sought["ok"]) != ("seek", False) or \
                sought["serial"] != st["result"]["serial"] + 1:
            raise Failed(f"forks: hud state result {sought} after a refused seek, from serial "
                         f"{st['result']['serial']}")
        n = len(log.lines)
        w.trigger(f"hud fork delete {tl}")
        if not log.wait(rf"the HUD's fork request \d+ refused: timeline {tl} is in use", ACK_S, n) \
                or not log.wait(rf"replay: forks .*: refused: timeline {tl} is in use", ACK_S, n):
            raise Failed(f"forks: the HUD's deletion of the playing timeline {tl} was not refused "
                         f"as in use")
        n = len(log.lines)
        w.trigger('hud fork export 4 "hudexport4"')
        done = log.wait(r"the HUD's fork request (?:\d+ done|\d+ refused|refused|\d+):", 180, n)
        if not done or " done: " not in done:
            raise Failed(f"forks: the HUD's export: {done.strip() if done else 'no outcome'}")
        if not log.wait(r"replay: forks .*: done: .* \[request=-\]", ACK_S, n):
            raise Failed("forks: the HUD's export: no line from the file worker")
        exported = w.gamedir / "replays" / "hudexport4.imprec"
        ids = [s["id"] for s in check_file(exported, "the HUD's export")]
        if ids != [0, 4]:
            raise Failed(f"forks: the HUD's export holds {ids}, not the chain [0, 4]")
        # the later steps count the replays folder: check_file has copied it
        exported.unlink()
        again = hud_state(w, log)["result"]
        if (again["kind"], again["op"], again["ok"]) != ("fork", 2, True) or \
                again["serial"] != sought["serial"] + 2:
            raise Failed(f"forks: hud state result {again} after the HUD's refused deletion and "
                         f"its export, from serial {sought['serial']}")
        results.add(("forks: the HUD's recorder calls", PASS,
                     f"timelines {got} (serial {st['cache']}), axis {st['axis']}, chain {chain}; "
                     f"{want} at {T}; seek 0 "
                     f"refused; delete {tl} refused in use, export 4 done; serial "
                     f"{st['result']['serial']} -> {again['serial']}"))

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

    # the original's first interval: the takeover starts at its second iteration
    first = original[0]["first"]
    if fork(1, first + 15) != first + 1:
        raise Failed(f"forks: K1 asked for at {first + 15} did not start at {first + 1}")
    for tid in (2, 3):
        fork(tid, base + (tid - 1) * 300)
    before = next(s["last_used"] for s in tarec.file_segments(source)[0] if s["id"] == 2)
    log = start_play(2, base + 310, "playing 2")
    finish_play(log, 2)
    w.collect(out / "touch")
    w.stop()
    plays_checked("touch")
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
    # timeline 2's first interval: the fork of a fork starts at its parent's second iteration
    fork2 = next(s["fork_at"] for s in tarec.file_segments(source)[0] if s["id"] == 2)
    T5 = fork(5, fork2 + 10, 2)
    if T5 != fork2 + 1:
        raise Failed(f"forks: K5 asked for at {fork2 + 10} did not start at {fork2 + 1}")
    log = start_play(5, T5 + 1, "playing 5")
    hud_route(log, 5)
    results.add(forks_picker(w, log, T5, P))
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
    plays_checked("delete")
    log = start_play(5, base - 120, "the chain's playing")
    arm_dump(w, log, "chain5", T5 + 300)
    turbo_engage(w, log)
    resume(w, log)
    wait_dumps(log, ["chain5"], running=True)
    rec, dump = copies("chain5", source, w.gamedir / "replays" / "dump-chain5.bin")
    results.later(check_or_fail, "forks: descendant across two boundaries", "compare", "--puppet",
                  "--seek", rec, dump, "--timeline", 5)
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
    plays_checked("chain")
    saved = source
    source = exported
    log = start_play(5, base - 120, "the export's playing")
    arm_dump(w, log, "export5", T5 + 300)
    turbo_engage(w, log)
    resume(w, log)
    wait_dumps(log, ["export5"], running=True)
    dump, = copies("export5", w.gamedir / "replays" / "dump-export5.bin")
    results.later(check_or_fail, "forks: exported chain playback", "compare", "--puppet",
                  "--seek", exported, dump, "--timeline", 5)
    w.collect(out / "export-play")
    w.stop()
    plays_checked("export-play")
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
    results.add(("forks: two-boundary and exported puppet dumps", PASS,
                 "both EQUAL; highest deleted ID 5 followed by ID 6"))
    results.add(prune_step(w, fx, out, source))
    # A read-only directory is a real writer refusal, without a DLL fault-injection lever.
    w.place(fx["fix"], fx["dll"])
    rd = w.gamedir / "replays"
    rd.mkdir(exist_ok=True)
    mode = rd.stat().st_mode & 0o7777
    log = Log(w)
    try:
        rd.chmod(mode & ~0o222)
        rc, text = tacli("scenario", "load", w.name, SCENARIOS / fx["fix"]["load"],
                         "--defaults", "--restart", "--keep-dll", "--json", timeout=300)
        if rc:
            raise CouldNotRun(f"writer-refused load: {last(text)}")
        failed = log.wait(r"recorder: writer: .* is abandoned at its last complete block",
                          ACK_S)
        if not failed or any(rd.glob("*.imprec")):
            raise Failed("writer-refused: the unwritable directory did not refuse recording")
    finally:
        rd.chmod(mode)
    if fx["cover"]:
        cover_ask(w, log, "writefail")
    # the game goes on past the tick the writer was abandoned at: resumed from its hold, its
    # GameTime passes the one read once the abandon was logged
    held_start(w, log)
    abandoned = log.at
    resume(w, log)
    stall = Stall(w)
    while w.gametime() <= abandoned:
        try:
            stall.check()
        except CouldNotRun as e:
            raise Failed(f"writer-refused: the game did not continue: {e}")
        nap(0.5)
    w.collect(out / "writer-refused")
    w.stop()
    results.add(("forks: unwritable recording and graceful exit", PASS,
                 "writer refused; game continued; exit completed before collection"))


# ---- the offline work -----------------------------------------------------------------------
#
# A check reads files the games wrote and plays no game, so it runs on the offline pool and never
# in a worker's slot: a lane hands its checks over and goes on to its next game, and a fixture's
# report starts as soon as its last lane ends, while other fixtures' games still run.
#
# NO DEADLOCK, by what runs where: a task on the pool is a leaf -- a tarec process, or a function
# that reads files -- and never waits for other pool work. Only the workers and the report
# threads wait, and neither runs on the pool.
#
# A CHECK READS WHAT IT WOULD HAVE READ where it was asked for: what a lane hands over reads only
# files nothing writes after the handing over -- a lane's collected dumps and recordings, which
# no later step touches, or a copy of a file the lane goes on to change (forks_lane's `copies`).
#
# THE SAME VERDICTS IN THE SAME ORDER (Ordered): results are read back in the order they were
# asked for, and the first exception in that order is the one raised, before any exception the
# lane met later in its own steps.

_POOL = None
_POOL_LOCK = threading.Lock()


def offline(games=1):
    """The offline pool, sized by its first caller for `games` games at once (run_threads)."""
    global _POOL
    with _POOL_LOCK:
        if _POOL is None:
            _POOL = ThreadPoolExecutor(max_workers=offline_workers(games),
                                       thread_name_prefix="offline", initializer=yield_to_games)
        return _POOL


def yield_to_games():
    """A pool thread, and every tarec process it starts, gives the CPU to the games first: their
    timed checks (rewind's 2000 ms, a takeover's `ms`) measure the game, not the checks beside it.
    Linux keeps a nice value per thread, and a child starts with its thread's (MEASURED
    2026-10-08: a child of a thread set to 10 reads 10, one of the main thread 0)."""
    try:
        os.setpriority(os.PRIO_PROCESS, 0, 10)
    except (AttributeError, OSError):
        pass


def offline_workers(games):
    """Checks at once: the cores less two for each game, and no more than the GiB available -- a
    tarec process holds 0.3 to 0.7 GB (`check` and `compare-fork` on a battle, MEASURED
    2026-10-08)."""
    try:
        with open("/proc/meminfo") as f:
            gib = next(int(ln.split()[1]) for ln in f if ln.startswith("MemAvailable:")) >> 20
    except (OSError, StopIteration, ValueError):
        gib = os.cpu_count()
    return max(2, min(os.cpu_count() - 2 * games, gib))


class Ordered:
    """Results in the order they were asked for, some still running on the offline pool. `add`
    takes results now; `later(fn, *args)` runs fn on the pool, a leaf returning a list of results;
    `start` runs one whose value an item reads; `then(fn)` calls fn when results() reaches it, in
    the caller's thread, after every item before it -- it may wait for pool work it holds and
    write the caller's state. results() waits for all the pool work, then gives every item's
    results in order or raises the first exception in that order, the same on every call. As a
    context it settles before an exception of the caller's own leaves, so a failure asked for
    earlier is the one the caller stops at."""

    def __init__(self):
        self.items, self.futures, self.outcome = [], [], None

    def add(self, *results):
        self.items.append(lambda: list(results))

    def start(self, fn, *args):
        """fn(*args) on the pool, a leaf, waited for by results() but no item of its own."""
        f = offline().submit(fn, *args)
        self.futures.append(f)
        return f

    def later(self, fn, *args):
        f = self.start(fn, *args)
        self.items.append(f.result)
        return f

    def then(self, fn):
        self.items.append(fn)

    def results(self):
        if self.outcome is None:
            wait(self.futures)
            out = []
            try:
                for item in self.items:
                    out += item()
            except BaseException as e:
                self.outcome = (out, e)
                raise
            self.outcome = (out, None)
        out, e = self.outcome
        if e is not None:
            raise e
        return list(out)

    def settled(self):
        """The results before the first exception, which this does not raise."""
        try:
            self.results()
        except BaseException:
            pass
        return list(self.outcome[0])

    def __enter__(self):
        return self

    def __exit__(self, kind, e, tb):
        if kind is not None:
            self.results()
        return False


# ---- the checks -----------------------------------------------------------------------------

def tarec_run(*args):
    r = subprocess.run([sys.executable, str(TAREC), *map(str, args)], capture_output=True,
                       text=True)
    return r.returncode, r.stdout + r.stderr


PASS, FAIL, UNRUN = "pass", "FAIL", "could not run"


def counts_options(fix):
    """What the L-against-P counts comparison adds for a fixture: --cob-exact where it gives
    `"cob": "exact"`, so the VM's START and CALL children by script name are compared exactly. No
    other comparison takes it: R's round trips and Q's fresh seek start every COB from its Create
    (tagpu_replay.c, THE SEEK), and are compared on their images."""
    cob = fix.get("cob")
    if cob is None:
        return []
    if cob != "exact":
        raise CouldNotRun(f'"cob": {cob!r}: the one value is "exact"')
    return ["--cob-exact"]


def image_gate_options(fx):
    if fx["name"] == "stock-secondary-orders" and fx["fix"].get("dll") == "stock":
        return ["--stock-secondary-cosmetics"]
    return []


def codec_check(info, decline=False):
    """The live recording's stream codec, from `tarec info`'s output: a stream block whose check
    decode differed (the end block's RT_CODEC_MISMATCH) is a codec bug, which the writer wrote as
    records and the gate fails by name; so is a recording with no sectioned stream block, whose
    codec never ran (a writer without memory for the codec writes every block as records, and its
    end block then counts nothing). The blocks the encoder declined are reported; with `decline`
    (the fixture's DECLINE_LEVER) a recording that declined none fails, since its fallback never
    ran."""
    m = re.search(r"the writer's codec: (\d+|absent) blocks declined, (\d+|absent) mismatched",
                  info)
    if not m:
        return ("the stream codec", UNRUN, "no end block")
    declined, mismatched = m.groups()
    if mismatched == "absent":
        return ("the stream codec", FAIL, "the end block carries no RT_CODEC_MISMATCH")
    if int(mismatched):
        return ("the stream codec", FAIL, f"the stream codec did not decode back in {mismatched} "
                                          f"blocks ({declined} declined)")
    s = re.search(r"stream blocks: (\d+), (\d+) sectioned", info)
    if not s:
        return ("the stream codec", UNRUN, "no count of the sectioned stream blocks")
    blocks, sectioned = map(int, s.groups())
    if not sectioned:
        return ("the stream codec", FAIL, f"none of the {blocks} stream blocks is sectioned: the "
                                          f"codec never ran ({declined} declined)")
    if decline and declined in ("0", "absent"):
        return ("the stream codec", FAIL, f"{DECLINE_LEVER} declined no block: the fallback "
                                          f"never ran ({sectioned} of {blocks} sectioned)")
    return ("the stream codec", PASS, f"0 mismatched, {declined} declined, {sectioned} of {blocks} "
                                      f"stream blocks sectioned")


def checks(fx):
    """Every check of the processes that ran, as an Ordered of (name, verdict, detail), each
    tarec call already started on the offline pool. A dump tarec cannot read (its exit 2) could
    not run; only a comparison that ran and differed is a FAIL. A compared dump goes to
    fx["passed"] or fx["held"] as its result is read back, in order."""
    rec, Ts, out, ran = fx["rec"], fx["Ts"], fx["out"], fx["procs"]
    res = Ordered()
    res.add(*fx.get("xres", []), *fx.get("ures", []))

    def info():
        rc, o = tarec_run("info", rec)
        m = re.search(r"ended: (.*)", o)
        end = m.group(1) if m else "no end block"
        clean = (m and "complete" in end and " 0 id rows outside" in end and
                 " 0 nano sprays uncredited" in end)
        return [("the end block", PASS if clean else FAIL if m else UNRUN, end),
                codec_check(o, fx["fix"].get("decline", False))]
    res.later(info)
    res.later(tarec_verdict, "tarec check", "check", rec)

    def compare(label, *args, files=()):
        missing = [f for f in files if not Path(f).exists()]
        if missing:
            res.add((label, UNRUN, f"no {', '.join(Path(f).name for f in missing)}"))
            return
        got = res.later(tarec_verdict, label, *args)

        def file_dumps():
            fx["passed" if got.result()[0][1] == PASS else "held"].extend(files)
            return []
        res.then(file_dumps)

    for n in (f"l{Ts[0]}", f"l{Ts[-1]}"):
        d = out / "L" / f"dump-{n}.bin"
        compare(f"L {n}", "compare", "--exact", rec, d, files=[d])
    res.later(tarec_verdict, "tarec lossy-check", "lossy-check", rec)
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
            # a seek's dump: THE OCCUPANCY RULE for the cells a seek stamps (tarec.occupancy)
            seek = ("--seek",) if proc == "Q" or n.startswith("rt") else ()
            compare(f"{proc} {n}{note}", "compare", "--puppet", *seek, rec, d, files=[d])
    l1, lT = out / "L" / f"dump-l{Ts[0]}.bin", out / "L" / f"dump-l{Ts[-1]}.bin"
    p1, pT = out / "P" / f"dump-p{Ts[0]}.bin", out / "P" / f"dump-p{Ts[-1]}.bin"
    n1, n2 = out / "L" / "dump-ln1.bin", out / "L" / "dump-ln2.bin"
    r1, q1 = out / "R" / f"dump-r{Ts[0]}.bin", out / "Q" / f"dump-q{Ts[0]}.bin"
    if ran.get("P") == "ran":
        compare(f"counts, L against P over [{Ts[0]}, {Ts[-1]})", "compare-counts", rec, l1, lT,
                p1, pT, *counts_options(fx["fix"]), files=[l1, lT, p1, pT])
    if all(ran.get(k) == "ran" for k in "PRQ"):
        views = [out / "L" / f"dump-{n}.bin" for n in null_views(fx["fix"])]
        for view in views + [n2]:
            compare(f"the paused twin's simulation: {view.stem}", "compare-dumps", rec,
                    n1, view, files=[n1, view])
        compare(f"the images gate at {Ts[0]}", "images-gate", *image_gate_options(fx),
                rec, l1, lT, n1, n2, p1, pT, r1, q1,
                *views, files=[l1, lT, n1, n2, p1, pT, r1, q1, *views])
    for proc, d in (("L", lT), ("P", pT)):
        if ran.get(proc) == "ran":
            compare(f"{proc} {d.stem[5:]}: the address scan", "scan", rec, d, files=[d])
    for proc, d in (("R", out / "R" / f"dump-rt{Ts[-1]}c.bin"), ("Q", q1)):
        if ran.get(proc) != "ran":
            continue
        if not d.exists():
            res.add((f"{proc}: COB engine writes on a puppet", UNRUN, f"no {d.name}"),
                    (f"{proc}: native owner-only starts on a puppet", UNRUN, f"no {d.name}"),
                    (f"{proc}: native owner's queries on a puppet", UNRUN, f"no {d.name}"))
            continue
        res.later(puppet_counts, proc, rec, d)
    if ran.get("L") == "ran":
        res.later(yard_dropped_check, out / "L" / "tagpu.log")
    for proc in ("P", "R", "Q"):
        if ran.get(proc) == "ran":
            res.later(yard_check, proc, out / proc / "tagpu.log")
    for proc in ("R", "Q"):
        if ran.get(proc) == "ran":
            # Q's one seek is a fresh process's: a seek asked for before a playing's first step
            # waits for it (tagpu_replay.c, seat), which reads the root's first iteration
            res.later(work_check, f"{proc}: the seeks' work", out / proc / "tagpu.log", rec, 0,
                      False, proc == "Q")
    if ran.get("R") == "ran":
        for T in Ts[1:]:
            line = fx["rt"].get(f"rt{T}", "")
            m = re.search(r"(\d+) tables poisoned, residue (\d+)", line)
            ok = bool(m) and int(m.group(1)) > 0 and m.group(2) == "0"
            res.add((f"round trip {T}: the poison check", PASS if ok else FAIL if m else UNRUN,
                     line or "no line"))
            a, b = out / "R" / f"dump-rt{T}-a.bin", out / "R" / f"dump-rt{T}-b.bin"
            compare(f"round trip {T}: A against B", "compare-dumps", "--seek", rec, a, b,
                    files=[a, b])
    if ran.get("W") == "ran":
        res.add(*fx["wres"])
        d = out / "W" / f"dump-w{Ts[0]}.bin"
        compare(f"W w{Ts[0]} (the HUD's seek, in a playing Watch started)", "compare",
                "--puppet", "--seek", rec, d, files=[d])
    return res


def tarec_verdict(label, *args):
    """[(label, verdict, detail)] of one tarec check: PASS and its last line on exit 0, FAIL on
    1, could not run on anything else, with its whole output."""
    rc, o = tarec_run(*args)
    return [(label, PASS if rc == 0 else FAIL if rc == 1 else UNRUN,
             last(o) if rc == 0 else o.strip())]


WORK_RX = re.compile(
    r"replay: (?P<name>[^:]+): the seek to (?P<T>\d+)'s work: (?P<I>\d+) iterations read to "
    r"GameTime (?P<R>\d+) from (?P<kind>the snapshot|the first iteration) at GameTime "
    r"(?P<S>\d+); the blocks: (?P<B>\d+) decoded \((?P<Bb>\d+) for the base\), (?P<Bp>\d+) bytes "
    r"read, (?P<Bu>\d+) unpacked; the image: (?P<N>\d+) units live, (?P<M>\d+) made, (?P<F>\d+) refused, "
    r"(?P<K>\d+) killed, (?P<A>\d+) left alive; cpu (?P<C>\d+) ms \(user (?P<U>\d+), kernel "
    r"(?P<Kt>\d+)\) of the game thread, (?P<W>\d+) ms wall")


def work_lines(text):
    """Every seek's work line of a log (tagpu_replay.c, sk_work_log), its numbers as ints."""
    return [{k: v if k in ("name", "kind") else int(v) for k, v in m.groupdict().items()}
            for m in WORK_RX.finditer(text)]


def work_verdict(w, rec, timeline, takeover=False, fresh=False):
    """What one seek's work line `w` breaks, [] for nothing; tarec.Unreadable when the file says
    nothing of its target.
    W1, exact: the reader built from where the file says it must (tarec.seek_start, the reader's
    own choice from the file's index), and read no more iterations than from there to its target,
    R - S + 1.
    W2, exact: every unit of the image made or refused (M + F == N: one create each), none left
    alive by the kills (A == 0), and none refused by a takeover, a return to now or a fork.
    W3, exact: the reader decoded the blocks the file holds from there to its target, each once --
    their count, their bytes in the file and unpacked (tarec.seek_blocks) -- with its base's
    block only from a snapshot; with `fresh`, the first seek of a playing in a fresh process past
    its first step, the base is the file's to say too, not the line's (tarec.seek_blocks, base
    None). The seek's cost is its work: W1 to W3 bound every quantity of it
    that a change could multiply, and none is a time, so no load moves a verdict. The game
    thread's CPU time is reported only (tagpu_replay.c, THE SEEK'S WORK): it grows with
    contention for the core."""
    S, snap = tarec.seek_start(rec, timeline, w["R"], SEEK_LEAD)
    kind = "the snapshot" if snap else "the first iteration"
    bad = []
    if (w["S"], w["kind"]) != (S, kind):
        bad.append(f"W1: built from {w['kind']} at {w['S']}, where the file's is {kind} at {S}")
    if w["I"] > w["R"] - w["S"] + 1:
        bad.append(f"W1: {w['I']} iterations read from {w['S']} to {w['R']}")
    if w["M"] + w["F"] != w["N"]:
        bad.append(f"W2: {w['M']} made and {w['F']} refused of {w['N']} units")
    if w["A"]:
        bad.append(f"W2: {w['A']} units left alive by the kills")
    if takeover and w["F"]:
        bad.append(f"W2: {w['F']} creates refused")
    if w["Bb"] and not snap:
        bad.append("W3: a base read for a seek from the first iteration")
    n, packed, raw, base = tarec.seek_blocks(rec, timeline, w["R"], SEEK_LEAD,
                                             None if fresh else w["Bb"])
    if fresh and w["Bb"] != base:
        bad.append(f"W3: {w['Bb']} blocks for the base where a fresh process reads {base}")
    if (w["B"], w["Bp"], w["Bu"]) != (n, packed, raw):
        bad.append(f"W3: {w['B']} blocks decoded, {w['Bp']} bytes read and {w['Bu']} unpacked "
                   f"from {w['S']} to {w['R']}, where the file holds {n}, {packed} and {raw}")
    return bad


def work_summary(w):
    return (f"{w['T']} from {'snapshot' if w['kind'] == 'the snapshot' else 'iteration'} "
            f"{w['S']}, {w['I']} read, {w['B']} blocks, {w['N']} units, cpu {w['C']} ms "
            f"({w['W']} wall)")


def work_check(label, log_path, rec, timeline, takeover, fresh=False):
    """[(label, verdict, detail)]: W1 to W3 of every seek's work line in a lane's log, each
    against the file and timeline it read; with `fresh`, the log's first seek is a fresh
    process's (work_verdict); could not run when the log holds none."""
    try:
        lines = work_lines(Path(log_path).read_text(errors="replace"))
        bad = [f"the seek to {w['T']}: {b}" for i, w in enumerate(lines)
               for b in work_verdict(w, rec, timeline, takeover, fresh and not i)]
    except (OSError, tarec.Unreadable) as e:
        return [(label, UNRUN, str(e))]
    if not lines:
        return [(label, UNRUN, f"no seek's work line in {log_path}")]
    return [(label, FAIL if bad else PASS,
             "; ".join(bad) or f"{len(lines)} seeks: " + "; ".join(map(work_summary, lines)))]


YARD_COUNTS = re.compile(r"(\d+) yards checked at their create, (\d+) differed, (\d+) not checked")


def yard_verdict(label, text):
    """[(label, verdict, detail)]: the yards a recorded create opened, against the ones the
    playing's creates left (tagpu_replay.c, yard_check), from every counts line of a lane's log:
    a difference, or a yard a create that was not made left unchecked, fails; could not run when
    the log holds no counts line."""
    rows = [tuple(map(int, m.groups())) for m in YARD_COUNTS.finditer(text)]
    if not rows:
        return [(label, UNRUN, "no counts line with the yards a create opened")]
    checked, differed, unchecked = rows[-1]
    bad = [f"{d} differed, {u} not checked" for _, d, u in rows if d or u]
    return [(label, FAIL if bad else PASS,
             "; ".join(bad) if bad else f"{checked} checked at their create, all as recorded")]


YARD_DROPPED = re.compile(r"recorder: (\d+) yard events kept out of the file by THE YARD RULE")


def yard_dropped_verdict(label, text):
    """[(label, verdict, detail)]: the yard events the recorder kept out of the file because THE
    YARD RULE would refuse them (tagpu_recorder.c, ev_yard), from every recording's end line of a
    lane's log: any fails, since the engine makes none; could not run with no such line."""
    counts = [int(m.group(1)) for m in YARD_DROPPED.finditer(text)]
    if not counts:
        return [(label, UNRUN, "no recording's end line with the yard events kept out")]
    bad = [n for n in counts if n]
    return [(label, FAIL if bad else PASS,
             f"{sum(bad)} yard events kept out of {len(counts)} recordings" if bad
             else f"none kept out of {len(counts)} recordings")]


def yard_check(proc, log_path):
    try:
        text = Path(log_path).read_text(errors="replace")
    except OSError as e:
        return [(f"{proc}: the yards a create opened", UNRUN, str(e))]
    return yard_verdict(f"{proc}: the yards a create opened", text)


def yard_dropped_check(log_path):
    label = "L: the yard events THE YARD RULE kept out of the file"
    try:
        text = Path(log_path).read_text(errors="replace")
    except OSError as e:
        return [(label, UNRUN, str(e))]
    return yard_dropped_verdict(label, text)


def puppet_counts(proc, rec, d):
    """The three engine checks of a puppet's dump, from `tarec counts`: no COB write, no owner-only
    start and no owner's query the puppet's engine made itself."""
    rc, o = tarec_run("counts", rec, d)
    res = []
    for what, label in (("COB engine writes", "COB write"),
                        ("native owner-only starts", "owner-only start"),
                        ("native owner's queries", "owner's query")):
        hits = [ln.strip() for ln in o.splitlines() if label in ln and not ln.endswith(": 0")]
        res.append((f"{proc}: {what} on a puppet", UNRUN if rc else FAIL if hits else PASS,
                    o.strip() if rc else "; ".join(hits) if hits else f"none ({d.name})"))
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
    """The takeover's checks, as the entry's `checks` lists them: an Ordered of (name, verdict,
    detail), each check's reading of the file already started on the offline pool."""
    tk, ran, out = fx["fix"]["takeover"], fx["procs"], fx["out"]
    want = tk.get("checks", [])
    k = fx.get("tk") or {}
    res = Ordered()

    def unrun(name, why):
        res.add((f"takeover {name}", UNRUN, why))

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
            res.add(("takeover judge", UNRUN if not bad else FAIL,
                     "; ".join(bad + ["no `units: alive=` line"])))
        else:
            if seen < 0.9 * made:
                bad.append(f"{seen} units seen of the {made} made")
            res.add(("takeover judge", FAIL if bad else PASS,
                     "; ".join(bad) or f"{seen} units seen at GameTime {k.get('judged_at')} of "
                                       f"the {made} made"))
    if "line" in want:
        def line():
            rc, o = tarec_run("segments", path)
            size = re.search(rf"timeline {tl}: bytes (\d+) to (\d+)", o)
            log = (out / "K" / "tagpu.log").read_text(errors="replace")
            first = re.search(r"recorder: \d+ iterations captured: the first in (\d+) us", log)
            work = next((w for w in work_lines(log) if w["T"] == T), None)
            if work is None:
                return [("takeover line", UNRUN, f"no work line for the seek to {T} in K's log")]
            try:
                bad = work_verdict(work, fx["rec"], 0, True)
            except tarec.Unreadable as e:
                return [("takeover line", UNRUN, str(e))]
            if k["units_refused"]:
                bad.append(f"{k['units_refused']} creates refused")
            return [("takeover line", FAIL if bad else PASS,
                     "; ".join(bad) or
                     f"cpu {work['C']} ms of the game thread, {k['ms']} ms wall (passability {k['passability_ms']}, AI "
                     f"{k['ai_ms']}); {work_summary(work)}; the fork's first iteration "
                     f"{first.group(1) + ' us' if first else 'not in the log'}, its segment "
                     f"{int(size.group(2)) - int(size.group(1)) if size else '?'} bytes; "
                     f"{k['line']}")]
        res.later(line)
    if "orders" in want:
        def orders():
            try:
                d, why = (tk_moved(path, tl, k["ordered"]) if k.get("ordered") else
                          (None, "no order"))
            except tarec.Unreadable as e:
                d, why = None, str(e)
            return [("takeover orders", UNRUN if d is None else PASS if d > 64 else FAIL,
                     why if d is None else
                     f"unit {k['ordered']['slot']} moved {d:.0f} wu from GameTime "
                     f"{k['ordered']['at']} to {k['ordered']['at'] + 300}")]
        res.later(orders)
    if "ai" in want:
        def ai_check():
            import tarewind
            try:
                if "ai_window" not in k:
                    raise tarec.Unreadable("no armed AI activity window")
                start, stop = k["ai_window"]
                ai, made, frac, n = tk_ai(path, tl, start, stop - 1)
                lines = (out / "K" / "tagpu.log").read_text(errors="replace").splitlines()
                active, detail = tarewind.ai_activity(lines, "takeover", start, stop, ai)
                if frac is None:
                    raise tarec.Unreadable(f"the AI seats hold no mobile unit at GameTime {start}")
                return [("takeover ai", PASS if active and frac >= .25 else FAIL,
                         f"{detail}; {frac:.0%} of {n} mobile units moved; "
                         f"construction observed for seats {sorted(made)} (reported only)")]
            except tarec.Unreadable as e:
                return [("takeover ai", UNRUN, str(e))]
        res.later(ai_check)
    if ran.get("F") == "ran":
        res.later(work_check, "F: the seeks' work", out / "F" / "tagpu.log", path, tl, False, True)
    if "fork" in want:
        compare_fork = res.start(tarec_run, "compare-fork", path, tl)
        segments = res.start(tarec_run, "segments", path)
        timelines = [(t, res.start(tarec_run, "check", path, "--timeline", t)) for t in (0, tl)]
        dumps = []
        lossy = res.start(tarec_run, "lossy-check", path, "--timeline", tl)
        for proc, d, how in (("K", out / "K" / f"dump-k{T + tk.get('dump', TAKEOVER_SPAN)}.bin",
                              ("--exact",)),
                             ("F", out / "F" / f"dump-f{fx.get('tk_f', 0)}.bin",
                              ("--puppet", "--seek"))):
            dumps.append((proc, d, d.exists() and
                          res.start(tarec_run, "compare", *how, "--timeline", tl, path, d)))

        def fork():
            parts, bad, unread = [], [], []
            rc, o = compare_fork.result()
            (parts if rc == 0 else bad if rc == 1 else unread).append(f"compare-fork: {o.strip()}")
            rc, o = segments.result()
            m = re.search(r"original sha256 ([0-9a-f]+)", o)
            mine = hashlib.sha256(Path(fx["rec"]).read_bytes()).hexdigest()
            if not m:
                unread.append(f"segments: {o.strip()}")
            elif m.group(1) != mine:
                bad.append("the original's bytes changed")
            else:
                parts.append("the original untouched")
            for t, f in timelines:
                rc, o = f.result()
                (parts if rc == 0 else bad if rc == 1 else unread).append(
                    f"check --timeline {t}: {last(o) if rc == 0 else o.strip()}")
            rc, o = lossy.result()
            (parts if rc == 0 else bad if rc == 1 else unread).append(
                f"lossy-check --timeline {tl}: {last(o) if rc == 0 else o.strip()}")
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
            for proc, d, f in dumps:
                if not f:
                    unread.append(f"no {proc} dump {d.name}")
                    continue
                rc, o = f.result()
                (parts if rc == 0 else bad if rc == 1 else unread).append(
                    f"{proc} {d.stem[5:]}: {last(o) if rc == 0 else o.strip()}")
            return [("takeover fork", FAIL if bad else UNRUN if unread else PASS,
                     "; ".join(bad + unread + parts))]
        res.then(fork)
    if "victory" in want:
        try:
            seg = [s for s in tarec.file_segments(path)[0] if s["id"] == tl][0]
            ends = [b for b in seg["blocks"] if b[0] == tarec.RB_END]
            e = tarec.end(ends[0][3]) if ends else {}
            won = [r for st, r, _, _ in e.get("seats", []) if st == P]
            ok = (e.get(tarec.RT_END_REASON) == 1 and won == [1] and
                  seg["last"] is not None and seg["last"] <= T + TAKEOVER_SPAN)
            res.add(("takeover victory", PASS if ok else FAIL,
                     f"timeline {tl} to GameTime {seg['last']}: "
                     f"{tarec.END_REASONS.get(e.get(tarec.RT_END_REASON), 'no end block')}; "
                     f"player {P} {tarec.RESULTS.get(won[0], won[0]) if won else 'has no result'}"))
        except tarec.Unreadable as ex:
            res.add(("takeover victory", UNRUN, str(ex)))
    return res


# ---- the run --------------------------------------------------------------------------------

def build_dll(stock, cover):
    """The DLL a run uses and the make line that builds it."""
    d = TREE / "tagpu" / "ddraw"
    name = ("ddraw-stocklimits" if stock else "ddraw") + ("-cover" if cover else "") + ".dll"
    make = ("make -C tagpu/ddraw" + (" LIMITS=stock" if stock else "") +
            (" COVER=1" if cover else ""))
    return d / name, make


# The order a free worker takes queued lanes in: the long ones first -- X, U and K start the
# longest chains a fixture has, and none can start before its fixture's L ends -- then L, then
# the short ones, which fill the run's end. It orders the games only; a lane's own steps and
# every check are the same whatever runs beside it.
LANE_ORDER = "XUKLWRPFQ"


def first_lanes(fxs):
    """The order the fixtures' L lanes are queued in: those whose L starts a long chain (X, U or
    K follows it) first, then the longer games."""
    return sorted(fxs, key=lambda n: (not set("XUK") & set(fxs[n]["want"]), -fxs[n]["fix"]["end"]))


class Run:
    """`fixes` the fixtures by name (fixtures.json's, or measure's scenarios); `measure` runs L
    and P only, with no dump and no check, for the coverage files.

    A fixture's report (fixture_report: its checks, then its lines of the summary) starts on the
    report threads when its last lane ends -- `left` counts the lanes queued or running, and a
    lane queues the lanes that follow it before it ends -- so the checks of one fixture overlap
    the games of the others."""

    def __init__(self, names, args, out, fixes=None, measure=False, takeover=None):
        self.q = queue.PriorityQueue()
        self.seq = itertools.count()
        self.lock = threading.Lock()
        self.pending = 0
        self.left = {}
        self.reports = {}
        self.report_pool = ThreadPoolExecutor(max_workers=max(1, len(names)),
                                              thread_name_prefix="report")
        self.fx = {}
        self.out = out
        self.measure = measure
        self.keep = bool(getattr(args, "keep", False))
        self.game_speed = getattr(args, "game_speed", TEST_GAME_SPEED)
        self.turbo = getattr(args, "turbo", 0)
        self.kind = getattr(args, "cmd", "run")
        # what the estimate keys a lane's past times by: the pacing and the build's kind
        self.variant = (f"gs{self.game_speed}-turbo{self.turbo}"
                        f"{'-cover' if getattr(args, 'cover', False) else ''}")
        h = head()
        fixes = fixes or fixtures()
        for name in names:
            fix = fixes[name]
            counts_options(fix)
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
            Ts = gametimes(name, fix["end"], h, fix.get("trips", 3), fix.get("first", 301),
                           fix.get("last_min"))
            # `takeover`, (player, at) of `tareplay.py takeover`: L, K and F alone
            want = (("L", "K", "F") if takeover else ("L", "P") if measure else
                    ("L",) if fix.get("cost") else procs(fix))
            self.fx[name] = {"name": name, "fix": fix, "dll": dll, "Ts": Ts, "out": out / name,
                             "problems": [], "rt": {}, "passed": [], "held": [],
                             "procs": {}, "cover": cover, "measure": measure,
                             "game_speed": fixture_speed(fix, self.game_speed),
                             "turbo": [(p, turbo_lane(fix, p, self.turbo)) for p in want
                                       if turbo_lane(fix, p, self.turbo)],
                             "map": dll.with_suffix(".map"), "want": want,
                             "tk_ask": takeover or (None, None)}
            if measure:
                say(f"{name}: recorded to GameTime {fix['end']} and played")
            elif fix.get("cost"):
                say(f"{name}: recorded to GameTime {fix['end']} for its cost and size")
            else:
                say(f"{name}: T1 {Ts[0]}, round trips {' '.join(map(str, Ts[1:]))}, "
                    f"Tlast {Ts[-1]}")
        for name in first_lanes(self.fx):
            self.put(name, "L")

    def put(self, name, proc):
        with self.lock:
            self.pending += 1
            self.left[name] = self.left.get(name, 0) + 1
        self.q.put((LANE_ORDER.index(proc), next(self.seq), name, proc))

    def done(self, name):
        with self.lock:
            self.pending -= 1
            self.left[name] -= 1
            last = self.left[name] == 0
        if last and not self.measure:
            self.report_of(name)

    def report_of(self, name):
        """The future of the fixture's report, started once: when its last lane ends, or by
        report() for a fixture a stopped run left with lanes never run."""
        with self.lock:
            if name not in self.reports:
                self.reports[name] = self.report_pool.submit(journaled_report, self.fx[name],
                                                             self.keep)
            return self.reports[name]

    def job(self, w, name, proc):
        """One process. Anything that stops it short of its claim -- a tool's timeout, an
        unexpected answer, a stopped run -- is recorded as could not run, never lost."""
        fx = self.fx[name]
        Ts, out = fx["Ts"], fx["out"] / proc
        t0 = time.time()
        say(f"{name} {proc}: start on {w.name}")
        w.lane = (name, proc)
        J.start(name, proc, instance=w.name, display=str(w.xvfb.display),
                gt_target=lane_target(fx, proc))
        try:
            stopping()
            w.game_speed = NORMAL_GAME_SPEED if proc == "W" else self.game_speed
            w.turbo = turbo_lane(fx["fix"], proc, self.turbo)
            w.held = held_lane(fx["fix"], proc)
            w.exact = proc in ("L", "K")
            if proc == "L":
                cost = bool(fx["fix"].get("cost"))
                fx["rec"] = live(w, fx, None if fx["measure"] or cost else Ts, out)
                h = tarec.header_only(str(fx["rec"]))
                fx["viewed"] = h["options"]["viewed"]
                if not fx["measure"]:
                    # the decode cache filled while the next lanes play (reccache.py): every
                    # check of the report, and the K and X lanes' files after L's, read it
                    fx["warm"] = offline().submit(tarec_run, "info", fx["rec"])
                if "K" in fx["want"]:
                    T, fx["tk_P"] = tk_point(fx, h, *fx["tk_ask"])
                    try:
                        fx["tk_T"] = tarec.refresh_at(str(fx["rec"]), 0, T)
                    except tarec.Unreadable as e:
                        raise CouldNotRun(f"the takeover's refresh at or before {T}: {e}")
                    if fx["tk_T"] != T:
                        say(f"{name}: the takeover asked for at {T} starts at {fx['tk_T']}, the "
                            f"refresh at or before it")
                for p in fx["want"][1:]:
                    if p != "F":                # F follows K, which it reads
                        self.put(name, p)
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
                self.put(name, "F")
            elif proc == "F":
                fx["tk_f"] = takeover_f(w, fx, out)
            elif proc == "X":
                fx["xres"] = forks_lane(w, fx, out)
            elif proc == "U":
                fx["ures"] = rewind_lane(w, fx, out)
            elif proc == "W":
                fx["wres"] = watch_lane(w, fx, fx["rec"], out, Ts[0])
            else:
                fresh(w, fx, fx["rec"], out, Ts[0])
            fx["procs"][proc] = "ran"
            w.lane = None
            J.finish(name, proc, "done")
        except Exception as e:
            kind = "FAILED" if isinstance(e, Failed) else "could not run"
            why = str(e) if isinstance(e, (CouldNotRun, Failed)) else f"{type(e).__name__}: {e}"
            fx["procs"][proc] = kind
            fx["problems"].append((proc, kind, why))
            say(f"{name} {proc}: {kind}: {why}")
            w.lane = None
            J.finish(name, proc, "fail" if kind == "FAILED" else "unrun", why.splitlines()[0])
            # the lanes this one queues on success never start: L queues all the others, K its F
            J.never(name, fx["want"][1:] if proc == "L" else ["F"] if proc == "K" else [],
                    f"{proc} did not finish")
            try:
                w.collect(out)
            except Exception:
                pass
            # A guard stop blocks the game thread, so its GUI cannot perform a
            # graceful exit. The next job may reuse this worker only after the
            # owned process has ended, even when collecting artifacts failed.
            try:
                w.stop(hard=True)
            except Exception as cleanup:
                why = f"failed game cleanup: {cleanup}"
                fx["problems"].append((proc, "could not run", why))
                say(f"{name} {proc}: {why}; retiring {w.name}")
                return False
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
                    _, _, name, proc = self.q.get(timeout=2)
                except queue.Empty:
                    with self.lock:
                        if self.pending == 0:
                            return
                    continue
                try:
                    if self.job(w, name, proc) is False:
                        return
                finally:
                    self.done(name)
        finally:
            w.close()


def cost_line(fx):
    """The recorder's own account of a cost game, reported and never failed (OQ5): its capture
    cost per iteration and the file's size, extrapolated to an hour of game time."""
    log = (fx["out"] / "L" / "tagpu.log").read_text(errors="replace")
    cap = re.search(r"recorder: (\d+) iterations captured: the first in \d+ us, the rest in "
                    r"(\d+) us on average and (\d+) us at most", log)
    size = re.search(r"recorder: writer: .* closed: (\d+) iterations .* stream (\d+) KB of records "
                     r"written as (\d+) KB", log)
    if not cap or not size:
        return "COST: the recorder's capture or close line is missing from L's log"
    n, raw, kb = int(size.group(1)), int(size.group(2)), int(size.group(3))
    if not n:
        return "COST: the recording closed with no iteration"
    hour = 30 * 3600 / n
    return (f"COST: {cap.group(1)} iterations, {cap.group(2)} us an iteration on average, "
            f"{cap.group(3)} us at most; {kb / 1024:.1f} MB ({raw / 1024:.1f} MB of records), "
            f"{kb / 1024 * hour:.0f} MB an hour of game time at that rate")


def fixture_report(fx, keep):
    """One fixture's part of the summary: (lines, failed, unrun) -- its lanes' problems and every
    check of the lanes that ran -- with its passing dumps deleted unless `keep`. It runs once the
    fixture's lanes have all ended (Run.done), so nothing else writes fx while it reads it."""
    if fx.get("warm"):
        fx["warm"].result()             # the cache filled once, not by every check at once
    failed = unrun = False
    cost = bool(fx["fix"].get("cost"))
    lines = [f"== {fx['name']}: " + ("the cost game" if cost else
                                      f"T {' '.join(map(str, fx['Ts']))}") +
             f"; game speed {fx['game_speed']} ({fx['game_speed'] / NORMAL_GAME_SPEED:g}x "
             f"target; W uses 10); "
             + (f"turbo={fx['turbo'][0][1]} on {' '.join(p for p, _ in fx['turbo'])}; "
                if fx["turbo"] else "turbo=0; ")
             + f"{fx['out']}"]
    for proc, kind, why in fx["problems"]:
        lines.append(f"   {proc} {kind.upper()}: {why}")
        failed |= kind == "FAILED"
        unrun |= kind != "FAILED"
    if cost:
        if fx["procs"].get("L") == "ran":
            lines.append("   " + cost_line(fx))
            label, verdict, detail = codec_check(tarec_run("info", fx["rec"])[1])
            fx["verdicts"] = [(label, verdict, detail)]
            lines.append(f"   {verdict:4}  {label}: {detail}")
            failed |= verdict == FAIL
            unrun |= verdict == UNRUN
        elif "L" not in fx["procs"]:
            lines.append("   L NEVER RAN")
            unrun = True
        return lines, failed, unrun
    for proc in fx["want"]:
        if proc not in fx["procs"]:
            lines.append(f"   {proc} NEVER RAN")
            unrun = True
    if fx["procs"].get("L") != "ran":
        return lines, failed, unrun
    planned = [checks(fx)] + ([tk_checks(fx)] if "K" in fx["want"] else [])
    fx["verdicts"] = [r for p in planned for r in p.results()]
    for label, verdict, detail in fx["verdicts"]:
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
    return lines, failed, unrun


def journaled_report(fx, keep):
    """fixture_report, its checks told to the journal: how many passed, and every one that did
    not by its label and the first line of its detail."""
    J.start(fx["name"], "checks")
    try:
        got = fixture_report(fx, keep)
    except Exception as e:
        J.finish(fx["name"], "checks", "unrun", f"{type(e).__name__}: {e}")
        raise
    v = fx.get("verdicts", [])

    def named(verdict):
        return [f"{label}: {detail.splitlines()[0] if detail else ''}"[:160]
                for label, vd, detail in v if vd == verdict]
    fails, unrun = named(FAIL), named(UNRUN)
    J.checks(fx["name"], sum(vd == PASS for _, vd, _ in v), fails, unrun)
    J.finish(fx["name"], "checks", "fail" if fails else "unrun" if unrun or not v else "pass")
    return got


def lane_target(fx, proc):
    """The GameTime a lane plays to, where one GameTime says how far it has come: None for the
    lanes whose course is not a run up the clock (K, F, X, U, W)."""
    Ts = fx.get("Ts")
    if proc == "L" or (proc == "P" and fx["measure"]):
        return fx["fix"]["end"]
    if not Ts:
        return None
    return {"P": Ts[-1], "R": Ts[-1] + CONTINUE, "Q": Ts[0]}.get(proc)


def report(run):
    """The summary, fixture by fixture in the run's order, each from its report (Run.report_of)."""
    failed = unrun = False
    lines = []
    for name in run.fx:
        got, f, u = run.report_of(name).result()
        lines += got
        failed |= f
        unrun |= u
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
    status = report(run)
    if STOP.is_set():
        return status or 2
    return status


def run_threads(run):
    """The run's workers to their end; a stop by SIGTERM or Ctrl-C lets each finish its game.
    The workers are the only threads that play games, one each: never more than -j at once."""
    global J
    offline(run.j)
    J = journal(run)
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


def journal(run):
    """The run planned for the journal: each fixture's lanes on -j game workers in the order a
    free worker takes them (LANE_ORDER, then the fixtures' L order), each after the lane it reads,
    and its checks after them all on the offline pool, which never waits for a game."""
    rank = {n: k for k, n in enumerate(first_lanes(run.fx))}
    items = []
    for name, fx in run.fx.items():
        steps = [{"id": p, "pool": "game", "order": LANE_ORDER.index(p) * 1000 + rank[name],
                  "after": [] if p == "L" else ["K"] if p == "F" else ["L"]}
                 for p in fx["want"]]
        if not run.measure:
            steps.append({"id": "checks", "pool": "offline", "order": 10 ** 6,
                          "after": list(fx["want"])})
        items.append((name, steps))
    names = list(run.fx)
    title = (f"tareplay {run.kind} " + (" ".join(names) if len(names) <= 3 else
                                        f"({len(names)} fixtures)") + f" -j {run.j}")
    return tarun.begin("tareplay", items, kind=run.kind, title=title, gate=True,
                       pools={"game": run.j, "offline": 0}, variant=run.variant,
                       summary=str(run.out / "summary.txt"))


def out_dir(args):
    base = Path(os.environ.get("TAREPLAY_OUT") or
                Path.home() / ".local" / "share" / "ta-replay" / "runs")
    out = Path(args.out) if args.out else base / datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    # every tarec this run starts, in this process or a child, shares one decode cache, which no
    # other run reads and which goes when the run does (reccache.py)
    cache = out / "tarec-cache"
    os.environ[reccache.ENV] = str(cache)
    atexit.register(shutil.rmtree, cache, ignore_errors=True)
    # a tarec process runs one thread of numpy and numba work: their default pools spin beside it
    # for nothing (MEASURED 2026-10-08: `check` on a forks file 10.4 s wall either way, 13.8 s of
    # CPU by default and 10.2 s with one thread), CPU the games and the other checks then lack
    for var in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS", "NUMBA_NUM_THREADS"):
        os.environ.setdefault(var, "1")
    return out


# the recorder's own instrumented sources (the Makefile's COVERED beside the sim files)
RECORDER_COVERED = {"src/tagpu_recorder.c", "src/tagpu_recwriter.c", "src/tagpu_recreader.c",
                    "src/tagpu_replay.c", "src/tagpu_recfile.c", "src/tagpu_reccodec.c",
                    "src/tagpu_reclossy.c",
                    "src/tagpu_recframe_cells.c", "src/tagpu_recframe_orders.c",
                    "src/tagpu_recframe_events.c", "src/tagpu_recframe_state.c"}


def covered_sources():
    """The instrumented sources (the Makefile's COVERED): the sim files and the recorder's own."""
    d = TREE / "tagpu" / "ddraw"
    return set(tafeat.sim_files(d)) | RECORDER_COVERED


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
                DD + "src/tagpu_reccodec.c", DD + "inc/tagpu_reccodec.h", DD + "inc/tagpu_reclanes.h",
                DD + "inc/tagpu_recframe.h", DD + "src/tagpu_recframe_cells.c",
                DD + "src/tagpu_recframe_orders.c", DD + "src/tagpu_recframe_events.c",
                DD + "src/tagpu_recframe_state.c",
                DD + "src/tagpu_reclossy.c", DD + "inc/tagpu_reclossy.h", DD + "inc/tagpu_rechold.h",
                DD + "replay-hold.tsv",
                DD + "replay-lanes.tsv", DD + "src/zstd.c",
                "tools/replay-globals-gen.py", "tools/replay-layout-gen.py", "tools/replay-lanes-gen.py",
                "tools/replay-hold-gen.py",
                "tools/replay-layout-check.sh", "tools/tacli"}
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
        steps = fix.get("steps", []) + fix.get("rewind_steps", [])
        named = {fix["load"]} | {a for _, k, a in steps if k == "apply"}
        if fix.get("rewind_paused"):
            named.add(fix["rewind_paused"])
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
        users = {n for n, f in fixtures().items() if p in f.get("sources", [])}
        if users:
            fx |= users
            why.append(f"{p}: the input regression of {' '.join(sorted(users))}")
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
            # only the DLL's instrumented sources: a coverage build compiles no test program, so
            # no fixture could reach one of its functions (the test programs run in their
            # suites), and it instruments no vendored file (zstd.c)
            if p.startswith(DD) and p[len(DD):] in RECORDER_COVERED:
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
            if not b:                   # a test program's line (tafeat.TEST_ONLY): no game runs it
                continue
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
        status = report(run)
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
    J.note(problems=problems)
    k = sum(1 for key, (_, bs) in funcs.items() if reached([key], bs)) + sum(
        1 for fns, bs in regions.values() if reached(fns, bs))
    total = len(funcs) + len(regions)
    if status == 0 and not problems:
        sha = graded[:8]
        seeds = " ".join(f"{n}:{run.fx[n]['Ts'][0]}" for n in sorted(fx)) if run else ""
        feats = sorted({f for f, _ in funcs.values()} | set(regions))
        note = (f"{NOTE} dll={dll} range={base}..{sha} features={','.join(feats) or '-'} "
                f"fixtures={','.join(sorted(fx)) or '-'} seeds={seeds.replace(' ', ',') or '-'} "
                f"reached={k}/{total} game_speed={args.game_speed} turbo={args.turbo} "
                f"result=pass date={datetime.date.today().isoformat()}")
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
    status = report(run)
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
    J.note(problems=lines)
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
    status = report(run)
    if STOP.is_set():
        return status or 2
    return status


def cmd_forks(args):
    fix = fixtures().get(args.fixture)
    if not fix or not fix.get("takeover"):
        raise CouldNotRun("forks needs a takeover fixture")
    out = out_dir(args)
    w = Worker(out, args.game_speed)
    dll, make = build_dll(fix.get("dll") == "stock", args.cover)
    try:
        if not dll.exists() or dll.stat().st_mtime < sources_mtime():
            raise CouldNotRun(f"build {make} first")
        fx = {"name": args.fixture, "fix": fix, "dll": dll.resolve(), "cover": args.cover,
              "Ts": gametimes(args.fixture, fix["end"], head(), 3, fix.get("first", 301),
                            fix.get("last_min"))}
        w.turbo, w.held = turbo_lane(fix, "L", args.turbo), held_lane(fix, "L")
        fx["rec"] = live(w, fx, None, out / "L")
        w.turbo, w.held = turbo_lane(fix, "X", args.turbo), held_lane(fix, "X")
        res = forks_lane(w, fx, out / "X")
        for name, status, detail in res:
            say(name, status, detail)
        return 0
    except (CouldNotRun, Failed) as e:
        say("forks:", str(e))
        return 1 if isinstance(e, Failed) else 2
    finally:
        w.close()


def rewind_lane(w, fx, out):
    import tarewind
    battle = fixtures()["battle"]
    # Fresh builders give each AI units available for decisions. The fixture supplies
    # no construction orders; the observer attributes new orders to the AI itself.
    fix = {**battle, "steps": battle["steps"] + fx["fix"].get("rewind_steps", [])}
    lane = {**fx, "fix": fix}
    return tarewind.run(sys.modules[__name__], w, lane, out)


def cmd_rewind(args):
    if args.fixture != "battle":
        raise CouldNotRun("rewind's L/P/Q fixture is battle")
    out = out_dir(args)
    dll, make = build_dll(args.stock, args.cover)
    if not dll.exists() or dll.stat().st_mtime < sources_mtime():
        raise CouldNotRun(f"build {make} first")
    w = Worker(out, args.game_speed)
    try:
        fx = {"name": "battle", "fix": fixtures()["battle"], "dll": dll.resolve(),
              "cover": args.cover}
        w.turbo, w.held = turbo_lane(fx["fix"], "U", args.turbo), held_lane(fx["fix"], "U")
        res = rewind_lane(w, fx, out)
        text = "\n".join(f"{status} {name}: {detail}" for name, status, detail in res)
        (out / "summary.txt").write_text(text + "\n")
        return 1 if any(s == FAIL for _, s, _ in res) else 2 if any(s == UNRUN for _, s, _ in res) else 0
    except (CouldNotRun, Failed) as e:
        say("rewind:", str(e))
        w.collect(out / "failure", recordings=True)
        w.stop()
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
    r.add_argument("-j", type=int, default=8, help="games at once (default 8)")
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
    m.add_argument("-j", type=int, default=8, help="games at once (default 8)")
    m.add_argument("--end", type=int, default=1800, help="GameTime each is recorded to")
    m.add_argument("--out", help="the run's folder")
    m.set_defaults(func=cmd_measure, dll=None, stock_dll=None, keep=True)
    la = sub.add_parser("landing", help="the landing tier: the changed features' fixtures on the "
                                        "coverage builds, and their reach")
    la.add_argument("--base", default="main", help="the branch the landing goes to")
    la.add_argument("--check", action="store_true",
                    help="exit 0 when a passing replay-roundtrip note covers HEAD")
    la.add_argument("-j", type=int, default=8, help="games at once (default 8)")
    la.add_argument("--keep", action="store_true", help="keep the passing dumps")
    la.add_argument("--out", help="the run's folder")
    la.set_defaults(func=cmd_landing, dll=None, stock_dll=None)
    br = sub.add_parser("broad", help="the broad tier: every fixture, and what none exercised")
    br.add_argument("--release", metavar="DLL", help="this release DLL, without reach")
    br.add_argument("-j", type=int, default=8, help="games at once (default 8)")
    br.add_argument("--out", help="the run's folder")
    br.set_defaults(func=cmd_broad, dll=None, stock_dll=None)
    tk = sub.add_parser("takeover", help="L, K and F alone: a fixture recorded, taken over, and "
                                         "its fork played across the boundary")
    tk.add_argument("fixture", help="a fixture with a `takeover` entry")
    tk.add_argument("--player", type=int, help="the player to take over (default the entry's)")
    tk.add_argument("--at", type=int,
                    help="the GameTime to take over at (default the entry's `at`, else T2)")
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
    rw = sub.add_parser("rewind", help="live battle rewound, restored, forked and played on P/Q")
    rw.add_argument("fixture", choices=["battle"])
    rw.add_argument("--stock", action="store_true", help="the same battle on stock limits")
    rw.add_argument("--cover", action="store_true")
    rw.add_argument("--out")
    rw.set_defaults(func=cmd_rewind)
    fe = sub.add_parser("features", help="the SIM_FEATURE regions of the sim files")
    fe.add_argument("--check", metavar="DIR", help="the link's check of tagpu/ddraw: exit 0 "
                                                   "clean, 1 a named problem, 2 could not run")
    fe.set_defaults(func=cmd_features)
    for parser in (r, m, la, br, tk, fk, rw):
        parser.add_argument("--game-speed", type=int, choices=(10, 20), default=TEST_GAME_SPEED,
                            help="internal TA speed: 20 = 2x (default), 10 = normal; cost "
                                 "fixtures and W's speed UI test use 10")
    for parser in (r, la, br, tk, fk, rw):
        parser.add_argument("--turbo", type=int, choices=range(6), default=TURBO, metavar="K",
                            help=f"ticks a frame on the turbo lanes once armed, 1..5, 0 for none "
                                 f"(default {TURBO}; tagpu_replay.c, THE TURBO)")
    a = ap.parse_args(argv)
    if a.cmd in ("run", "measure", "landing", "broad", "takeover") and not 1 <= a.j <= 16:
        ap.error("-j is 1..16")
    # the subcommands that read recordings need numpy, numba and zstandard (tarec.py through
    # reccodec.py); `features` and `list` run under the build's bare python3, and so does
    # `landing --check`, which reads git notes only
    if a.cmd not in ("features", "list") and not (a.cmd == "landing" and a.check):
        import devpy
        devpy.ensure()
    # every command that plays games waits for the machine's run lock (tools/runlock.py)
    if a.cmd not in ("features", "list") and not (a.cmd == "landing" and a.check):
        runlock.hold(TREE, "tareplay " + " ".join(sys.argv[1:] if argv is None else argv))
    rc = 2                              # a CouldNotRun or anything unforeseen: it could not run
    try:
        rc = a.func(a)
    finally:
        J.end("stopped" if STOP.is_set() else {0: "pass", 1: "fail"}.get(rc, "unrun"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
