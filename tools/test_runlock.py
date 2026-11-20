#!/usr/bin/env python3
"""Tests of the weighted run lock (tools/runlock.py) on a throwaway repository's lock, with real
processes and real files: the slots and the GPU memory held never sum past their budgets, runs
of different caps and GDI and Vulkan lanes of their own weights included, lane weights that cannot
be read refuse every run, each queue keeps arrival order, a run that wants no GPU memory is
admitted while a GPU run waits for its own, all at once or none, a killed holder or waiter frees
its slots and memory, no run deadlocks beside a stand-in of the runlock code that counted GPU
tokens and gathered them in the slot queue, a run's games take seats and wait for one past
games(), every launch writes the run's cap, a nested run takes none, flock(1) on
tareplay.lock still excludes every run, resources exclude by name, an offline phase and a build are
cut to the slots they were granted and never below their least, the load guard holds a run back
past its limit, cuts an offline phase to the room left and admits whatever the load when no other
process holds a slot, the sim pool sizes itself from the engine slots held within its floor and
cap, shares its places between batches in arrival order, frees a dead batch's places, touches no
slot, runs its processes idle, first victims and under its memory bound, is taken off the load
the guard reads as the games it plays, starts no game on a place at or above its size now and
widens again when an engine run ends, is nested like an offline phase and refuses a configuration
it cannot read, and two runs of each tool that names instances or plays a network game at once take distinct names and DirectPlay ports
(tools/dpport.py's leases, which are the machine's own, in /tmp). No game runs.

    python3 tools/test_runlock.py

A run here is `runlock.py run` around a stand-in command, with POLL_S at 0.05 s so the run being
admitted retries at once, and the device's memory given rather than read: none (no GPU budget) for
the tests of the slots, the reference setup's 12 282 MiB -- 287 units of 32 MiB, 9 games at the
play defaults' cap of tools/runlock-vram.cfg -- for the tests of the GPU memory. The machine's load
is given too, from a file each test writes (none: no load guard), so the machine the tests run on
never decides an admission, and so are the lane weights, a test's own file (GDI and VULKAN unless
a test writes others), so the tree's figures never decide a test's arithmetic, and so are the sim
pool's figures and the machine's threads (the owner's 8, 24 and 3072 MiB on THREADS). A step is waited for, never timed: WAIT_S bounds a step that takes
milliseconds, and past it the lock is wrong, not slow.
"""
import json
import os
import re
import resource
import select
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import runlock  # noqa: E402

WAIT_S = 20
REFERENCE_MIB = 12282           # the reference setup's GPU
UNITS = 287                     # its GPU budget: (12 282 - 3072) // 32 (tools/runlock-vram.cfg)
GAMES = 9                       # the games at the play defaults' cap: 287 * 32 // (920 + 64 + 32)
LIMIT = 32.0                    # the load guard's limit: the reference setup's threads at load_per_thread 1.0
THREADS = 32                    # the reference setup's threads, which the sim pool's size is taken from
SIM_FLOOR, SIM_CAP, SIM_MIB = 8, 24, 3072   # the sim pool's figures as the owner set them (THE SIM POOL)
PLACE_MIB = SIM_MIB // SIM_CAP  # 128
# the lane weights every run here reads, unless a test writes others: the tests' own, not the tree's
# (tools/runlock-lanes.cfg), and different, so that a run charged the other renderer's weight shows
GDI, VULKAN = 1, 2
# the host memory of THE HOST MEMORY's tests: the reference setup's 63 437 MiB less a reserve of 20 GiB,
# shared by the 28 slots
HOST_MIB, RESERVE_MIB = 63437, 20480
SLOT_MIB = (HOST_MIB - RESERVE_MIB) // runlock.BUDGET   # 1534
# forked()'s tests: a small machine, so that their jobs pass a bound with a few hundred MiB
SMALL_MIB, SMALL_RESERVE = 8192, 1024
SMALL_SLOT = (SMALL_MIB - SMALL_RESERVE) // runlock.BUDGET  # 256


def units(n, cap=920):
    """The units `n` games under the cap `cap` take, on tools/runlock-vram.cfg's driver and margin."""
    return -(-n * (cap + 64 + 32) // 32)


def mem_job(job):
    """forked()'s test job (MiB to allocate, MiB it says it needs, a log, the starts to wait for): its start
    logged with its bound, its memory held until the log holds that many starts (at most WAIT_S), its end
    logged, then (MiB allocated, its bound, its oom_score_adj)."""
    mib, _needs, log, starts = job
    bound = resource.getrlimit(resource.RLIMIT_DATA)[0]
    bound = -1 if bound == resource.RLIM_INFINITY else bound >> 20
    with open(log, "a") as f:
        f.write(f"start {bound}\n")
    block = bytearray(mib << 20)
    t0 = time.monotonic()
    while Path(log).read_text().count("start") < starts and time.monotonic() - t0 < WAIT_S:
        time.sleep(0.01)
    with open(log, "a") as f:
        f.write(f"end {bound}\n")
    del block
    with open("/proc/self/oom_score_adj") as f:
        return mib, bound, int(f.read())


def doomed_job(how):
    """forked()'s test job that ends without its result: killed, or raising."""
    if how == "kill":
        os.kill(os.getpid(), signal.SIGKILL)
    raise ValueError("a job's own error")
HOLD = "import sys; print('in', flush=True); sys.stdin.read()"


class RunLock(unittest.TestCase):

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tree = Path(tmp.name)
        subprocess.run(["git", "init", "-q", str(self.tree)], check=True)
        self.lock = runlock.lock_path(self.tree)
        self.env = {k: v for k, v in os.environ.items() if k != runlock.ENV}
        self.suites = self.tree / "runlock-suites.cfg"
        self.suites.write_text("# a test's suites\nlight = 320\nheavy = 2000\n")
        real = runlock.SUITES_CFG
        self.addCleanup(setattr, runlock, "SUITES_CFG", real)
        runlock.SUITES_CFG = self.suites
        self.lanes = self.tree / "runlock-lanes.cfg"
        self.weigh(GDI, VULKAN)
        self.addCleanup(setattr, runlock, "LANES_CFG", runlock.LANES_CFG)
        runlock.LANES_CFG = self.lanes
        # the machine's load as every run here reads it: none, so no load guard, unless a test
        # writes a figure
        self.load = self.tree / "loadavg"
        self.load.write_text("")
        # the sim pool's figures and the machine's threads, the tests' own: the reference setup's
        self.sim = self.tree / "runlock-sim.cfg"
        self.sim.write_text(f"# a test's sim pool\nfloor = {SIM_FLOOR}\ncap = {SIM_CAP}\npool_mib = {SIM_MIB}\n")
        for name, value in (("load_limit", lambda: LIMIT),
                            ("loadavg", lambda: float(t) if (t := self.load.read_text()).strip() else None),
                            ("SIM_CFG", self.sim), ("machine_threads", lambda: THREADS)):
            self.addCleanup(setattr, runlock, name, getattr(runlock, name))
            setattr(runlock, name, value)

    def weigh(self, gdi, vulkan):
        """The lane weights of every run here, from now on."""
        self.lanes.write_text(f"# a test's lane weights\nlane_slots_gdi = {gdi}\nlane_slots_vulkan = {vulkan}\n")

    def boot(self, total=None, tree_cfg=None):
        """The Python that sets a run's runlock up as wrapper() says, before what it runs."""
        return (f"import sys; sys.path.insert(0, {str(TOOLS)!r}); import runlock; "
                f"runlock.POLL_S = 0.05; runlock.device_total_mib = lambda: {total!r}; "
                f"runlock.load_limit = lambda: {LIMIT!r}; "
                f"runlock.loadavg = lambda: (lambda t: float(t) if t.strip() else None)"
                f"(open({str(self.load)!r}).read()); "
                f"from pathlib import Path; runlock.SUITES_CFG = Path({str(self.suites)!r}); "
                f"runlock.LANES_CFG = Path({str(self.lanes)!r}); "
                f"runlock.SIM_CFG = Path({str(self.sim)!r}); runlock.machine_threads = lambda: {THREADS}; "
                + (f"runlock.VRAM_CFG = Path({str(tree_cfg)!r}); " if tree_cfg else ""))

    def phase(self, most, least=1, what="phase"):
        """An offline phase of `most` workers at most and `least` at least in a process of its own:
        it says the workers granted once admitted and holds them until its stdin closes."""
        return self.start([sys.executable, "-c", self.boot()
                           + f"w = runlock.offline(Path({str(self.tree)!r}), {what!r}, {most}, {least}); "
                             "print(w.n, flush=True); sys.stdin.read()"])

    def wrapper(self, slots, what, *cmd, resources=(), vram=None, total=None, tree_cfg=None,
                suite=None, sub="run", lanes=None, sim=None):
        """The argv of `runlock.py run` around `cmd`, retrying the slots every 0.05 s, on a device of
        `total` MiB (None: no GPU budget), from a tree whose runlock-vram.cfg is `tree_cfg` (None:
        this one's), its games of `suite` in this test's suites file (self.suites), on a machine
        whose 1-minute load is the figure in self.load (none: no load guard) and whose load guard's
        limit is LIMIT; `sub` "make" for `runlock.py make -j slots -- cmd`; `lanes` for `run --lanes
        lanes` in place of --slots; `sim` for `run --sim sim`."""
        boot = self.boot(total, tree_cfg) + "sys.exit(runlock.main(sys.argv[1:]))"
        if sub == "make":
            return [sys.executable, "-c", boot, "make", "-j", str(slots), "--what", what, "--", *cmd]
        named = [a for r in resources for a in ("--resource", r)]
        gpu = ["--vram", str(vram)] if vram is not None else []
        gpu += ["--suite", suite] if suite else []
        size = ["--slots", str(slots)] if lanes is None else ["--lanes", str(lanes)]
        size = size if sim is None else ["--sim", str(sim)]
        return [sys.executable, "-c", boot, "run", *size, *gpu, "--what", what, *named, "--", *cmd]

    def start(self, argv, env=None):
        p = subprocess.Popen(argv, cwd=self.tree, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, env=env or self.env)

        def end():
            if p.poll() is None:
                p.kill()
            p.wait()
            for f in (p.stdin, p.stdout, p.stderr):
                f.close()
        self.addCleanup(end)
        return p

    def holder(self, slots, what="holder", resources=(), vram=None, total=None, tree_cfg=None,
               suite=None, lanes=None):
        """A run that says `in` once admitted and holds its slots until its stdin closes."""
        return self.start(self.wrapper(slots, what, sys.executable, "-c", HOLD, resources=resources,
                                       vram=vram, total=total, tree_cfg=tree_cfg, suite=suite,
                                       lanes=lanes))

    def stderr_until(self, p, text):
        """The stderr lines of `p` up to the first that holds `text`; a failure when none comes."""
        seen = []
        while True:
            ln = self.line(p.stderr)
            if ln is None:
                self.fail(f"{text!r} never said; said: {seen}")
            seen.append(ln)
            if text in ln:
                return seen

    def held(self, pid=None):
        """The GPU unit files held, or those whose line names `pid`."""
        d = self.lock.parent / "runslots"
        if pid is None:
            return sum(runlock._held_now(p) for p in d.glob("gpu-*"))
        return sum(p.read_text().startswith(f"pid {pid},") for p in d.glob("gpu-*"))

    def line(self, stream, timeout=WAIT_S):
        """The next line of `stream`, None when none comes within `timeout`."""
        ready, _, _ = select.select([stream], [], [], timeout)
        return stream.readline().strip() if ready else None

    def admitted(self, p):
        self.assertEqual(self.line(p.stdout), "in", "the run was not admitted")

    def quiet(self, p):
        """Nothing on stdout now: the run has not been admitted."""
        self.assertIsNone(self.line(p.stdout, 0), "a run was admitted that must wait")

    def let_go(self, p):
        p.stdin.close()
        p.wait(WAIT_S)

    def until(self, cond, what):
        deadline = time.monotonic() + WAIT_S
        while not cond():
            if time.monotonic() > deadline:
                self.fail(what)
            time.sleep(0.02)

    def free(self):
        return runlock.report(self.lock)[0]

    def admitting(self, what):
        return any(ln.startswith("being admitted:") and f", {what}, " in ln
                   for ln in runlock.report(self.lock)[1])

    def waiting_gpu(self, what):
        return any(ln.startswith("waiting for GPU memory:") and f", {what}, " in ln
                   for ln in runlock.report(self.lock)[1])

    def test_a_run_waits_while_the_budget_is_held(self):
        a, b = self.holder(10, "a"), self.holder(10, "b")
        self.admitted(a)
        self.admitted(b)
        self.assertEqual(self.free(), 8)
        c = self.holder(10, "c")
        # c holds the queue and the last 8 free slots: it cannot be admitted before a release
        self.until(lambda: self.admitting("c") and self.free() == 0, "c did not take the free slots")
        self.quiet(c)
        self.let_go(a)
        self.admitted(c)
        self.assertEqual(self.free(), 8)

    def test_the_held_slots_never_sum_past_the_budget(self):
        log = self.tree / "held.log"
        # each command counts the slot files that name its pid -- no other run can write a slot it
        # holds -- and logs it while it holds them
        probe = ("import os, pathlib, sys, time; d = pathlib.Path(sys.argv[1]); "
                 "me = f'pid {os.getpid()},'; "
                 "n = sum(p.read_text().startswith(me) for p in d.glob('slot-*')); "
                 "log = open(sys.argv[2], 'a'); log.write(f'+ {os.getpid()} {sys.argv[3]} {n}\\n'); "
                 "log.flush(); time.sleep(0.1); log.write(f'- {os.getpid()} {sys.argv[3]}\\n')")
        weights = [7, 13, 2, 28, 5, 11, 1, 19, 3, 9, 6, 17, 40, 4]
        runs = [self.start(self.wrapper(w, f"w{w}", sys.executable, "-c", probe,
                                        str(self.lock.parent / "runslots"), str(log),
                                        str(runlock.clamp(w))))
                for w in weights]
        for p in runs:
            self.assertEqual(p.wait(WAIT_S * 3), 0, p.stderr.read())
        held, peak, seen = 0, 0, 0
        for ln in log.read_text().splitlines():
            sign, _, w, *n = ln.split()
            if sign == "+":
                self.assertEqual(int(n[0]), int(w), f"a run held other than its slots: {ln}")
                held += int(w)
                seen += 1
            else:
                held -= int(w)
            peak = max(peak, held)
            self.assertLessEqual(held, runlock.BUDGET, "the slots held passed the budget")
        self.assertEqual(seen, len(weights))
        self.assertEqual(held, 0)

    def test_a_big_run_is_not_overtaken_by_small_ones_behind_it(self):
        a = self.holder(1, "a")
        self.admitted(a)
        big = self.holder(28, "big")
        self.until(lambda: self.admitting("big"), "the big run did not reach the queue")
        smalls = [self.holder(1, f"small{k}") for k in range(3)]
        for s in smalls:
            self.assertIn("waiting for the run lock", self.line(s.stderr) or "")
        # 26 slots are free, and none of them goes to a run behind the big one
        for s in smalls:
            self.quiet(s)
        self.let_go(a)
        self.admitted(big)
        for s in smalls:
            self.quiet(s)
        self.let_go(big)
        for s in smalls:
            self.admitted(s)

    def test_a_killed_holder_frees_its_slots(self):
        a = self.holder(28, "a")
        self.admitted(a)
        b = self.holder(5, "b")
        self.until(lambda: self.admitting("b"), "b did not reach the queue")
        self.quiet(b)
        a.send_signal(signal.SIGKILL)
        a.wait(WAIT_S)
        self.admitted(b)
        self.assertEqual(self.free(), 23)

    def test_a_nested_run_takes_no_slots(self):
        other = self.holder(24, "other")
        self.admitted(other)
        inner = self.wrapper(8, "inner", sys.executable, "-c", "print('in')")
        outer = self.start(self.wrapper(4, "outer", sys.executable, "-c",
                                        f"import os, subprocess, sys; "
                                        f"print(os.environ[{runlock.ENV!r}], flush=True); "
                                        f"sys.exit(subprocess.run({inner!r}).returncode)"))
        held = json.loads(self.line(outer.stdout))
        self.assertEqual((held["slots"], held["path"]), (4, str(self.lock)))
        # every slot is held now: an inner run that took slots of its own would never be admitted
        self.assertEqual(self.line(outer.stdout), "in")
        self.assertEqual(outer.wait(WAIT_S), 0)
        self.assertIn("inner wants 4 games at once inside a run that may play 2", outer.stderr.read())

    def test_under_flock_1_on_the_lock_a_run_is_nested(self):
        p = self.start(["flock", str(self.lock), *self.wrapper(3, "under flock", sys.executable,
                                                               "-c", "print('in')")])
        self.admitted(p)
        self.assertEqual(p.wait(WAIT_S), 0)

    def test_flock_1_still_excludes_every_run(self):
        a = self.holder(2, "a")
        self.admitted(a)
        busy = subprocess.run(["flock", "-x", "-n", str(self.lock), "true"])
        self.assertEqual(busy.returncode, 1, "flock -x took the lock beside a run")
        self.let_go(a)
        hand = self.start(["flock", "-x", str(self.lock), sys.executable, "-c", HOLD])
        self.admitted(hand)
        b = self.holder(1, "b")
        self.assertIn("waiting for the run lock", self.line(b.stderr) or "")
        self.assertIn("tareplay.lock held exclusively, by a flock(1) chain: no run is admitted",
                      runlock.report(self.lock)[1])
        self.quiet(b)
        self.let_go(hand)
        self.admitted(b)

    def test_status_names_the_holders_and_marks_a_gone_pid_stale(self):
        a = self.holder(3, "three")
        self.admitted(a)
        # bash leaves a child holding the slots it inherited, and exits
        orphan = self.start(self.wrapper(2, "orphaning", "bash", "-c", "sleep 600 & echo $!"))
        child = int(self.line(orphan.stdout))
        self.addCleanup(lambda: os.kill(child, signal.SIGKILL) if runlock._alive(child) else None)
        orphan.wait(WAIT_S)
        free, lines = runlock.report(self.lock)
        self.assertEqual(free, 23)
        self.assertTrue(any(ln.startswith(f" 3 slots held by pid {a.pid}, a run of 3 slots, ") and ", three, " in ln
                            and "STALE" not in ln for ln in lines), lines)
        self.assertTrue(any(ln.startswith(f" 2 slots held by pid {orphan.pid}, a run of 2 slots, ") and
                            f"[STALE: pid {orphan.pid} is gone" in ln for ln in lines), lines)
        os.kill(child, signal.SIGKILL)
        self.until(lambda: self.free() == 25, "the orphan's slots outlived it")
        out = subprocess.run([sys.executable, str(TOOLS / "runlock.py"), "status"], cwd=self.tree,
                             capture_output=True, text=True, env=self.env).stdout
        self.assertIn(f"runlock: 25 of {runlock.BUDGET} slots free", out)
        self.assertIn(f"held by pid {a.pid}", out)

    def test_hold_declares_lanes_and_release_lets_them_go(self):
        env = os.environ.pop(runlock.ENV, None)
        self.addCleanup(lambda: env is not None and os.environ.update({runlock.ENV: env}))
        self.addCleanup(runlock.release)
        device = runlock.device_total_mib
        self.addCleanup(setattr, runlock, "device_total_mib", device)
        runlock.device_total_mib = lambda: REFERENCE_MIB
        self.assertTrue(runlock.hold(self.tree, "lanes", 3))
        held = json.loads(os.environ[runlock.ENV])
        self.assertEqual((held["slots"], held["gpu_mib"], held["games"], held["cap_mib"], held["path"]),
                         (3 * VULKAN, units(3) * 32, 3, 920, str(self.lock)))
        self.assertEqual((runlock.tag(), runlock.games(), self.held()), ("s00", 3, units(3)))
        self.assertEqual(len(runlock.fds()), 3 * VULKAN + units(3) + 1)
        self.assertFalse(runlock.hold(self.tree, "again", 1))
        self.assertEqual(self.free(), runlock.BUDGET - 3 * VULKAN)
        runlock.release()
        self.assertNotIn(runlock.ENV, os.environ)
        self.assertEqual(runlock.fds(), ())
        self.assertEqual((self.free(), self.held()), (runlock.BUDGET, 0))

    def test_the_plans_of_a_run(self):
        device = runlock.device_total_mib
        self.addCleanup(setattr, runlock, "device_total_mib", device)
        runlock.device_total_mib = lambda: REFERENCE_MIB
        lk = self.lock
        self.assertEqual(runlock.gpu_budget(lk), UNITS)
        # -j 12 on the reference setup: 9 games at the play defaults' cap, their slots and memory
        self.assertEqual(runlock.plan(12, path=lk), (2 * GAMES, units(GAMES), GAMES, UNITS, 920))
        self.assertEqual(runlock.plan(None, path=lk), (runlock.BUDGET, UNITS, GAMES, UNITS, 920))
        self.assertEqual(runlock.plan(3, gpu=False, path=lk), (3 * GDI, 0, 3, None, None))
        # a suite of a smaller cap: 287 * 32 // 416 = 22 games, so -j 12 plays all 12
        self.assertEqual(runlock.plan(12, path=lk, suite="light"),
                         (24, units(12, 320), 12, UNITS, 320))
        self.assertEqual(runlock.plan(None, path=lk, suite="light")[2], 22)
        # a suite of a larger cap: 287 * 32 // 2096 = 4
        self.assertEqual(runlock.plan(12, path=lk, suite="heavy"), (8, units(4, 2000), 4, UNITS, 2000))
        with self.assertRaises(runlock.BadFigures):
            runlock.plan(2, path=lk, suite="nobody")
        # a small card still holds MIN_GAMES games at the play defaults' cap
        runlock.device_total_mib = lambda: 2048
        self.assertEqual(runlock.gpu_budget(lk), units(runlock.MIN_GAMES))
        self.assertEqual(runlock.plan(12, path=lk)[2], runlock.MIN_GAMES)
        with self.assertRaisesRegex(RuntimeError, "more than the whole GPU budget"):
            runlock.plan(1, path=lk, suite="heavy")
        runlock.device_total_mib = lambda: None
        self.assertEqual(runlock.plan(12, path=lk), (24, 0, 12, None, None))
        self.assertEqual(runlock.plan(40, path=lk), (runlock.BUDGET, 0, runlock.BUDGET // VULKAN, None, None))

    def test_a_lane_takes_its_renderers_weight(self):
        device = runlock.device_total_mib
        self.addCleanup(setattr, runlock, "device_total_mib", device)
        runlock.device_total_mib = lambda: REFERENCE_MIB
        lk = self.lock
        self.weigh(3, 5)
        # a run without GPU memory draws with GDI unless it says otherwise: llvmpipe is Vulkan
        self.assertEqual(runlock.plan(4, gpu=False, path=lk), (12, 0, 4, None, None))
        self.assertEqual(runlock.plan(4, gpu=False, path=lk, renderer="vulkan"), (20, 0, 4, None, None))
        self.assertEqual(runlock.plan(None, gpu=False, path=lk)[2], runlock.BUDGET // 3)
        # a GPU run's lanes are Vulkan's, cut to the GPU memory as ever
        self.assertEqual(runlock.plan(4, path=lk), (20, units(4), 4, UNITS, 920))
        self.assertEqual(runlock.plan(12, path=lk)[:3], (25, units(5), 5))
        # with no device memory to bound, a GPU run is still a Vulkan run
        runlock.device_total_mib = lambda: None
        self.assertEqual(runlock.plan(4, path=lk), (20, 0, 4, None, None))
        self.assertEqual((runlock.lane_slots("gdi"), runlock.lane_slots("vulkan"), runlock.lanes_in(7, "gdi"),
                          runlock.lanes_in(2, "vulkan")), (3, 5, 2, 1))
        with self.assertRaises(ValueError):
            runlock.lane_slots("llvmpipe")
        with self.assertRaises(ValueError):
            runlock.plan(1, gpu=False, path=lk, renderer="opengl")

    def test_held_gdi_and_vulkan_lanes_take_their_weights(self):
        self.in_process()
        self.assertTrue(runlock.hold(self.tree, "gdi lanes", 5, gpu=False))
        self.assertEqual((runlock.games(), json.loads(os.environ[runlock.ENV])["slots"], self.free()),
                         (5, 5 * GDI, runlock.BUDGET - 5 * GDI))
        runlock.release()
        # with no GPU budget a GPU run's lanes still take Vulkan's weight
        self.assertTrue(runlock.hold(self.tree, "vulkan lanes", 5))
        self.assertEqual((runlock.games(), self.free()), (5, runlock.BUDGET - 5 * VULKAN))
        runlock.release()
        a = self.holder(1, "gdi by lanes", lanes=7, vram=0)
        self.admitted(a)
        b = self.holder(1, "vulkan by lanes", lanes=7)
        self.admitted(b)
        self.assertEqual(self.free(), runlock.BUDGET - 7 * GDI - 7 * VULKAN)
        probe = "import runlock; print(runlock.games(), flush=True)"
        env = dict(self.env, PYTHONPATH=str(TOOLS))
        for argv, games in ((self.wrapper(1, "gdi", sys.executable, "-c", probe, lanes=4, vram=0), 4),
                            (self.wrapper(4, "slots", sys.executable, "-c", probe, vram=0), 4 // GDI),
                            (self.wrapper(4, "slots", sys.executable, "-c", probe), 4 // VULKAN)):
            p = self.start(argv, env=env)
            self.assertEqual(self.line(p.stdout), str(games))
            self.assertEqual(p.wait(WAIT_S), 0)

    def test_mixed_lanes_never_sum_past_the_budget(self):
        self.weigh(1, 3)
        log = self.tree / "held.log"
        probe = ("import os, pathlib, sys, time; d = pathlib.Path(sys.argv[1]); "
                 "me = f'pid {os.getpid()},'; "
                 "n = sum(p.read_text().startswith(me) for p in d.glob('slot-*')); "
                 "log = open(sys.argv[2], 'a'); log.write(f'+ {os.getpid()} {sys.argv[3]} {n}\\n'); "
                 "log.flush(); time.sleep(0.1); log.write(f'- {os.getpid()} {sys.argv[3]}\\n')")
        # (lanes, renderer): GDI lanes a slot each, Vulkan ones three
        wanted = [(5, "gdi"), (4, "vulkan"), (12, "gdi"), (9, "vulkan"), (1, "gdi"), (2, "vulkan"),
                  (28, "gdi"), (7, "vulkan"), (3, "gdi"), (6, "gdi")]
        runs = []
        for k, (n, r) in enumerate(wanted):
            slots = runlock.clamp(n * (1 if r == "gdi" else 3))
            runs.append(self.start(self.wrapper(1, f"{r}{k}", sys.executable, "-c", probe,
                                                str(self.lock.parent / "runslots"), str(log), str(slots),
                                                lanes=n, vram=0 if r == "gdi" else None)))
        for p in runs:
            self.assertEqual(p.wait(WAIT_S * 3), 0, p.stderr.read())
        held, seen = 0, 0
        for ln in log.read_text().splitlines():
            sign, _, w, *n = ln.split()
            if sign == "+":
                self.assertEqual(int(n[0]), int(w), f"a run held other than its lanes' slots: {ln}")
                held += int(w)
                seen += 1
            else:
                held -= int(w)
            self.assertLessEqual(held, runlock.BUDGET, "the slots held passed the budget")
        self.assertEqual((seen, held), (len(wanted), 0))

    def test_lane_weights_that_cannot_be_read_refuse_every_run(self):
        good = "lane_slots_gdi = 1\n# a comment = 9\nlane_slots_vulkan = 2\n"
        self.lanes.write_text(good)
        self.assertEqual(runlock.lane_weights(), {"gdi": 1, "vulkan": 2})
        for bad in (good.replace("lane_slots_gdi = 1\n", ""),            # a renderer missing
                    good + "lane_slots_llvmpipe = 4\n",                  # one it does not know
                    good + "lane_slots_gdi = 2\n",                       # one named twice
                    good.replace("= 1", "= one"),
                    good.replace("= 1", "= 0"),
                    good.replace("= 2", f"= {runlock.BUDGET + 1}")):
            self.lanes.write_text(bad)
            with self.assertRaises(runlock.BadLanes, msg=bad):
                runlock.lane_weights()
        p = self.start(self.wrapper(1, "gdi", sys.executable, "-c", "pass", vram=0))
        self.assertEqual(p.wait(WAIT_S), 2)
        err = p.stderr.read()
        self.assertIn("the lane weights cannot be read, so no run is admitted", err)
        self.assertIn(str(self.lanes), err)
        self.assertNotIn("Traceback", err)
        # the tree's own file is whole
        self.addCleanup(setattr, runlock, "LANES_CFG", runlock.LANES_CFG)
        runlock.LANES_CFG = TOOLS / "runlock-lanes.cfg"
        self.assertEqual(set(runlock.lane_weights()), set(runlock.RENDERERS))

    def test_a_game_takes_its_capped_bound_and_its_margin(self):
        # the shipped figures: B = 920 + 64, a game 1016 of the budget, 9 on the reference setup
        cfg = runlock.vram_config(self.lock)
        self.assertEqual(cfg, runlock.read_vram_cfg(runlock.VRAM_CFG))       # seeded from the tree's
        self.assertEqual((runlock.cap_mib(cfg), runlock.game_bound_mib(cfg), runlock.game_mib(cfg)),
                         (cfg["cap_mib"], cfg["cap_mib"] + cfg["driver_mib"],
                          cfg["cap_mib"] + cfg["driver_mib"] + cfg["margin_mib"]))
        self.assertEqual(runlock.game_mib(cfg, 320), 320 + cfg["driver_mib"] + cfg["margin_mib"])
        self.assertEqual((REFERENCE_MIB - cfg["reserve_mib"]) // runlock.GRAIN_MIB, UNITS)
        self.assertEqual(runlock.gpu_games(UNITS, cfg), GAMES)
        # the units of n games hold them at their bound, and never more than the budget offers
        for cap in (64, 320, 920, 2000):
            for n in range(1, runlock.gpu_games(UNITS, cfg, cap) + 1):
                self.assertGreaterEqual(runlock.gpu_units(n, cfg, cap) * runlock.GRAIN_MIB,
                                        n * runlock.game_mib(cfg, cap))
                self.assertLessEqual(runlock.gpu_units(n, cfg, cap), UNITS)
            self.assertGreater(runlock.gpu_units(runlock.gpu_games(UNITS, cfg, cap) + 1, cfg, cap), UNITS)
        # the caps the run lock accepts are the caps the DLL takes: any other it runs uncapped
        header = (TOOLS.parent / "tagpu/ddraw/inc/tagpu_vkcap.h").read_text()
        self.assertIn(f"#define TAGPU_VKCAP_MIN_MIB  {runlock.CAP_MIN_MIB}\n", header)
        self.assertEqual(runlock.CAP_MAX_MIB, 1 << 20)
        self.assertIn("#define TAGPU_VKCAP_MAX_MIB  (1L << 20)\n", header)
        # every suite of the tree's file is a cap the DLL takes
        real = TOOLS / "runlock-suites.cfg"
        self.addCleanup(setattr, runlock, "SUITES_CFG", runlock.SUITES_CFG)
        runlock.SUITES_CFG = real
        for name in ("replay", "compat", "wiki"):
            self.assertTrue(runlock.CAP_MIN_MIB <= runlock.suite_cap(name) <= runlock.CAP_MAX_MIB)

    def test_a_vram_file_that_is_not_whole_is_refused(self):
        real = runlock.VRAM_CFG
        self.addCleanup(setattr, runlock, "VRAM_CFG", real)
        runlock.VRAM_CFG = self.tree / "vram.cfg"
        good = "reserve_mib = 3072\ncap_mib = 512\n# a comment = 9\ndriver_mib = 32\nmargin_mib = 0\n"
        runlock.VRAM_CFG.write_text(good)
        self.assertEqual(runlock.game_mib(runlock.read_vram_cfg(runlock.VRAM_CFG)), 544)
        # a tree's file that is not whole seeds no lock, and the lock's own is read the same way
        runlock.VRAM_CFG.write_text(good + "cap_mib = 900\n")
        with self.assertRaises(ValueError):
            runlock.vram_config(self.lock)
        self.assertFalse(runlock.shared_vram_cfg(self.lock).exists())
        runlock.shared_vram_cfg(self.lock).write_text(good.replace("cap_mib = 512", "cap_mib = 63"))
        with self.assertRaises(ValueError):
            runlock.vram_config(self.lock)
        runlock.VRAM_CFG.write_text(good)
        for bad in (good.replace("margin_mib = 0\n", ""),            # a figure missing
                    good + "game_mib = 1024\n",                      # one it does not know
                    good + "cap_mib = 900\n",                        # one named twice
                    good.replace("driver_mib = 32", "driver_mib = -1"),
                    good.replace("cap_mib = 512", "cap_mib = 63"),     # a cap the DLL ignores
                    good.replace("cap_mib = 512", f"cap_mib = {(1 << 20) + 1}")):
            runlock.VRAM_CFG.write_text(bad)
            with self.assertRaises(ValueError, msg=bad):
                runlock.read_vram_cfg(runlock.VRAM_CFG)
        # a suite's figure is refused the same way
        for bad in ("light = lots\n", "light = 63\n"):
            self.suites.write_text(bad)
            with self.assertRaises(runlock.BadFigures, msg=bad):
                runlock.suite_cap("light")

    def test_the_held_memory_never_sums_past_its_budget_whatever_the_caps(self):
        log = self.tree / "units.log"
        probe = ("import os, pathlib, sys, time; d = pathlib.Path(sys.argv[1]); "
                 "me = f'pid {os.getpid()},'; "
                 "n = sum(p.read_text().startswith(me) for p in d.glob('gpu-*')); "
                 "log = open(sys.argv[2], 'a'); log.write(f'+ {os.getpid()} {sys.argv[3]} {n}\\n'); "
                 "log.flush(); time.sleep(0.1); log.write(f'- {os.getpid()} {sys.argv[3]}\\n')")
        caps = {None: 920, "light": 320, "heavy": 2000}
        wanted = [(3, None), (8, "light"), (1, "heavy"), (5, None), (2, "light"), (12, None),
                  (4, "heavy"), (7, "light"), (1, None), (6, "heavy"), (20, "light"), (3, None)]
        runs = []
        for k, (w, suite) in enumerate(wanted):
            cap = caps[suite]
            n = min(w, UNITS * 32 // (cap + 96), runlock.BUDGET // VULKAN)
            runs.append(self.start(self.wrapper(1, f"v{k}", sys.executable, "-c", probe,
                                                str(self.lock.parent / "runslots"), str(log),
                                                str(units(n, cap)), vram=w, total=REFERENCE_MIB,
                                                suite=suite)))
        for p in runs:
            self.assertEqual(p.wait(WAIT_S * 3), 0, p.stderr.read())
        held, seen = 0, 0
        for ln in log.read_text().splitlines():
            sign, _, w, *n = ln.split()
            if sign == "+":
                self.assertEqual(int(n[0]), int(w), f"a run held other than its units: {ln}")
                held += int(w)
                seen += 1
            else:
                held -= int(w)
            self.assertLessEqual(held, UNITS, "the GPU memory held passed its budget")
        self.assertEqual((seen, held), (len(wanted), 0))

    def test_a_run_without_gpu_memory_is_admitted_while_a_gpu_run_waits_for_its_own(self):
        a = self.holder(1, "a", vram=6, total=REFERENCE_MIB)
        self.admitted(a)
        big = self.holder(2, "big", vram=GAMES, total=REFERENCE_MIB)
        # big holds the token queue, and neither a unit (all at once or none) nor a slot meanwhile
        self.until(lambda: self.waiting_gpu("big"), "big did not reach the token queue")
        self.assertEqual((self.held(big.pid), self.held(), self.free()), (0, units(6), runlock.BUDGET - 1))
        # a GDI run and a wide one want no GPU memory: neither waits for big
        gdi = self.holder(1, "gdi", vram=0, total=REFERENCE_MIB)
        wide = self.holder(20, "wide", vram=0, total=REFERENCE_MIB)
        self.admitted(gdi)
        self.admitted(wide)
        # a GPU run behind big waits behind it for its memory, however little it wants
        small = self.holder(1, "small", vram=1, total=REFERENCE_MIB, suite="light")
        self.assertIn("waiting for the run lock", self.line(small.stderr) or "")
        self.quiet(small)
        self.let_go(a)
        self.admitted(big)
        self.assertEqual(self.held(big.pid), units(GAMES))
        self.quiet(small)                       # one unit is left, and a light game takes 13
        self.let_go(big)
        self.admitted(small)
        self.assertEqual(self.held(small.pid), units(1, 320))

    def test_a_gpu_run_waiting_for_slots_holds_up_no_other_runs_memory(self):
        wall = self.holder(runlock.BUDGET - 1, "wall", vram=0, total=REFERENCE_MIB)
        self.admitted(wall)
        g1 = self.holder(2, "g1", vram=3, total=REFERENCE_MIB)
        # g1 has its memory and waits in the slot queue for a second slot
        self.until(lambda: self.admitting("g1") and self.held(g1.pid) == units(3),
                   "g1 did not reach the slot queue with its memory")
        self.quiet(g1)
        # g2 takes its memory while g1 waits for slots, then queues for slots behind g1
        g2 = self.holder(2, "g2", vram=4, total=REFERENCE_MIB, suite="light")
        self.until(lambda: self.held(g2.pid) == units(4, 320) and not self.waiting_gpu("g2"),
                   "g2 waited for GPU memory behind a run that waits for slots")
        self.quiet(g2)
        self.let_go(wall)
        self.admitted(g1)
        self.admitted(g2)
        self.assertEqual((self.held(g1.pid), self.held(g2.pid)), (units(3), units(4, 320)))
        self.assertEqual(self.free(), runlock.BUDGET - 4)

    def test_a_killed_holder_frees_its_memory(self):
        a = self.holder(1, "a", vram=GAMES, total=REFERENCE_MIB)
        self.admitted(a)
        b = self.holder(1, "b", vram=3, total=REFERENCE_MIB)
        self.until(lambda: self.waiting_gpu("b"), "b did not reach the token queue")
        self.quiet(b)
        a.send_signal(signal.SIGKILL)
        a.wait(WAIT_S)
        self.admitted(b)
        self.assertEqual(self.held(), units(3))
        self.assertIn(f"GPU memory: {units(3) * 32} MiB held", runlock.report(self.lock)[1])

    def test_a_killed_waiter_frees_the_memory_it_waited_with(self):
        a = self.holder(1, "a", vram=6, total=REFERENCE_MIB)
        self.admitted(a)
        big = self.holder(1, "big", vram=GAMES, total=REFERENCE_MIB)
        self.until(lambda: self.waiting_gpu("big"), "big did not reach the token queue")
        nxt = self.holder(1, "next", vram=3, total=REFERENCE_MIB)          # 191 + 96 = 287: it fits
        self.quiet(nxt)
        big.send_signal(signal.SIGKILL)             # in the token queue, holding no unit
        big.wait(WAIT_S)
        self.admitted(nxt)
        self.assertEqual(self.held(), units(6) + units(3))
        # killed in the slot queue, holding its memory: it goes with it
        wall = self.holder(runlock.BUDGET - 2, "wall", vram=0, total=REFERENCE_MIB)
        self.admitted(wall)
        stuck = self.holder(1, "stuck", vram=0, total=REFERENCE_MIB)
        self.let_go(a)
        self.until(lambda: self.free() == 0 and self.held() == units(3), "a's slot and memory were not freed")
        self.admitted(stuck)
        g = self.holder(2, "g", vram=6, total=REFERENCE_MIB)
        self.until(lambda: self.admitting("g") and self.held(g.pid) == units(6),
                   "g did not reach the slot queue with its memory")
        g.send_signal(signal.SIGKILL)
        g.wait(WAIT_S)
        self.until(lambda: self.held() == units(3) and not self.admitting("g"), "g's memory was not freed")

    def test_every_memory_figure_counts_the_same_files(self):
        a = self.holder(1, "a", vram=6, total=REFERENCE_MIB)
        self.admitted(a)
        c = self.holder(1, "c", vram=2, total=REFERENCE_MIB)
        self.admitted(c)
        b = self.holder(2, "b", vram=5, total=REFERENCE_MIB)     # 32 units free of its 159: it waits with none
        self.until(lambda: self.waiting_gpu("b"), "b did not reach the token queue")
        held = units(6) + units(2)
        _free, lines = runlock.report(self.lock, UNITS)
        self.assertIn(f"GPU memory: {held * 32} MiB held of {UNITS * 32}, {(UNITS - held) * 32} free", lines)
        mine = {p.pid: [ln for ln in lines if f"MiB of the GPU held by pid {p.pid}," in ln] for p in (a, b, c)}
        self.assertEqual((len(mine[a.pid]), len(mine[b.pid]), len(mine[c.pid])), (1, 0, 1), lines)
        self.assertTrue(mine[a.pid][0].startswith(f"{units(6) * 32:5} MiB of the GPU held by pid {a.pid}, "
                                                  f"a run of 1 slot and {units(6) * 32} MiB of the GPU"),
                        lines)
        self.assertTrue(mine[c.pid][0].startswith(f"{units(2) * 32:5} MiB of the GPU held by pid {c.pid}, "
                                                  f"a run of 1 slot and {units(2) * 32} MiB of the GPU"),
                        lines)
        # the summary is the sum of the holders' counts, all from one read
        per = sum(int(ln.split()[0]) for ln in lines if " MiB of the GPU held by pid " in ln)
        self.assertEqual(per, held * 32)
        # `status`, on the same device, prints the same lines and no figure of its own
        boot = (f"import sys; sys.path.insert(0, {str(TOOLS)!r}); import runlock; "
                f"runlock.device_total_mib = lambda: {REFERENCE_MIB}; sys.exit(runlock.main(['status']))")
        out = subprocess.run([sys.executable, "-c", boot], cwd=self.tree, capture_output=True, text=True,
                             env=self.env).stdout
        self.assertIn(f"  GPU memory: {held * 32} MiB held of {UNITS * 32}, {(UNITS - held) * 32} free\n", out)
        self.assertEqual(sum(int(ln.split()[0]) for ln in out.splitlines()
                             if " MiB of the GPU held by pid " in ln), held * 32)
        self.assertNotIn("GPU", out.splitlines()[0])
        self.let_go(a)
        self.admitted(b)
        held = units(2) + units(5)
        self.assertIn(f"GPU memory: {held * 32} MiB held of {UNITS * 32}, {(UNITS - held) * 32} free",
                      runlock.report(self.lock, UNITS)[1])

    def old_cfg(self):
        """An older tree's runlock-vram.cfg: a cap of 768, 11 games on the reference setup."""
        p = self.tree / "old-runlock-vram.cfg"
        p.write_text("reserve_mib = 3072\ncap_mib = 768\ndriver_mib = 32\nmargin_mib = 32\n")
        return p

    def test_the_budget_is_the_locks_seeded_once_and_a_tree_that_differs_says_so(self):
        a = self.holder(1, "a", vram=1, total=REFERENCE_MIB)               # seeds the lock from this tree
        self.admitted(a)
        shared = runlock.shared_vram_cfg(self.lock)
        self.assertEqual(shared.read_text(), runlock.VRAM_CFG.read_text())
        old = self.holder(1, "old tree", vram=11, total=REFERENCE_MIB, tree_cfg=self.old_cfg())
        said = self.stderr_until(old, "is not the lock's")
        self.assertTrue(any("cap_mib': 768" in ln and "cap_mib': 920" in ln for ln in said), said)
        # the lock's figures: 9 games at 920, so the old tree's run, wanting 11 cut to 9, waits with
        # none for the 32 units a holds
        self.until(lambda: self.waiting_gpu("old tree"), "the old tree's run did not wait for GPU memory")
        self.assertEqual(self.held(old.pid), 0)
        self.quiet(old)
        self.let_go(a)
        self.admitted(old)
        self.assertEqual(self.held(old.pid), units(GAMES))
        self.assertEqual(shared.read_text(), runlock.VRAM_CFG.read_text())  # never written again

    def hold_files(self, *names):
        """A process that holds each of runslots/`names` LOCK_EX with a line of the runlock code
        that counted GPU tokens, says `in`, and holds them until its stdin closes."""
        code = ("import fcntl, os, sys; fs = []\n"
                "for name in sys.argv[2:]:\n"
                "    f = open(os.path.join(sys.argv[1], name), 'a+')\n"
                "    fcntl.flock(f, fcntl.LOCK_EX); f.seek(0); f.truncate()\n"
                "    f.write(f'pid {os.getpid()}, 2 slots, 2 GPU tokens, since then, old, from there\\n')\n"
                "    f.flush(); fs.append(f)\n"
                "print('in', flush=True); sys.stdin.read()")
        return self.start([sys.executable, "-c", code, str(runlock.slots_dir(self.lock)), *names])

    def test_a_gpu_file_the_budget_does_not_count_holds_up_every_run_that_wants_memory(self):
        # the runlock code that counted tokens holds vram-03 and vram-04; then a run under another
        # budget holds a unit this one has not
        for names, said_rx, line_rx in (
                (("vram-03", "vram-04"), "vram-03, vram-04 held by runs this budget",
                 " 2 GPU tokens of older runlock code held by pid "),
                ((f"gpu-{UNITS:04d}",), f"gpu-{UNITS:04d} held by runs this budget",
                 f"   32 MiB of the GPU held under another budget by pid ")):
            old = self.hold_files(*names)
            self.admitted(old)
            gdi = self.holder(1, "gdi", vram=0, total=REFERENCE_MIB)
            self.admitted(gdi)                              # wanting no GPU memory, it does not wait
            new = self.holder(1, "new", vram=1, total=REFERENCE_MIB)
            said = self.stderr_until(new, "held by runs this budget")
            self.assertTrue(any(said_rx in ln for ln in said), said)
            self.quiet(new)
            self.assertEqual(self.held(new.pid), 0)         # it takes none beside them
            _free, lines = runlock.report(self.lock, UNITS)
            self.assertTrue(any(ln.startswith("GPU memory: 0 MiB held") and
                                ln.endswith("no run takes GPU memory until they end") for ln in lines), lines)
            self.assertTrue(any(ln.startswith(line_rx + f"{old.pid},") for ln in lines), lines)
            old.send_signal(signal.SIGKILL)
            old.wait(WAIT_S)
            self.admitted(new)
            self.assertEqual(self.held(new.pid), units(1))
            self.let_go(new)
            self.let_go(gdi)

    def test_a_game_is_capped_by_its_runs_figures(self):
        shared = runlock.shared_vram_cfg(self.lock)
        shared.write_text("reserve_mib = 3072\ncap_mib = 512\ndriver_mib = 32\nmargin_mib = 0\n")
        probe = "import runlock; print(runlock.held_cap_mib(), runlock.games(), flush=True)"
        env = dict(self.env, PYTHONPATH=str(TOOLS))
        # a game of 544: 287 * 32 // 544 = 16 games, each run's cap 512, whatever its tree says
        for tree_cfg in (None, self.old_cfg()):
            p = self.start(self.wrapper(1, "capped", sys.executable, "-c", probe, vram=12,
                                        total=REFERENCE_MIB, tree_cfg=tree_cfg), env=env)
            self.assertEqual(self.line(p.stdout), "512 12")
            p.wait(WAIT_S)
        # a suite's games run under the suite's cap, sized with the lock's driver and margin
        p = self.start(self.wrapper(1, "light", sys.executable, "-c", probe, vram=30, total=REFERENCE_MIB,
                                    suite="light"), env=env)
        self.assertEqual(self.line(p.stdout), f"320 {runlock.BUDGET // VULKAN}")
        p.wait(WAIT_S)
        # a run that wants no GPU memory publishes no cap: its games are not capped
        p = self.start(self.wrapper(1, "gdi", sys.executable, "-c", probe, vram=0, total=REFERENCE_MIB),
                       env=env)
        self.assertEqual(self.line(p.stdout), "None 1")
        p.wait(WAIT_S)
        # a hold of runlock code that published no cap: its tokens were sized on its own tree's
        # figures, which the lock's need not be, so no cap is claimed for its games
        self.addCleanup(lambda: os.environ.pop(runlock.ENV, None))
        os.environ[runlock.ENV] = json.dumps({"slots": 1, "tokens": 1, "games": 1, "tag": "s00",
                                              "resources": [], "path": str(self.lock)})
        self.assertIsNone(runlock.held_cap_mib())

    def test_every_launch_writes_the_runs_cap(self):
        gamedir = self.tree / "game"
        gamedir.mkdir()
        store = gamedir / "impure.cfg"
        store.write_text("hudscale=150\ngpumem=512\n")
        self.addCleanup(lambda: os.environ.pop(runlock.ENV, None))
        os.environ[runlock.ENV] = json.dumps({"slots": 2, "gpu_mib": 1024, "games": 1, "tag": "s00",
                                              "cap_mib": 320, "resources": [], "path": str(self.lock)})
        self.assertEqual(runlock.write_gpumem(gamedir), 320)
        self.assertEqual(store.read_text(), "hudscale=150\ngpumem=320\n")
        # a run without GPU memory, and no run at all: the line goes, so the game is not capped
        os.environ[runlock.ENV] = json.dumps({"slots": 2, "gpu_mib": 0, "games": 1, "tag": "s00",
                                              "cap_mib": None, "resources": [], "path": str(self.lock)})
        self.assertIsNone(runlock.write_gpumem(gamedir))
        self.assertEqual(store.read_text(), "hudscale=150\n")
        os.environ[runlock.ENV] = json.dumps({"slots": 2, "gpu_mib": 1024, "games": 1, "tag": "s00",
                                              "cap_mib": 320, "resources": [], "path": str(self.lock)})
        runlock.write_gpumem(gamedir)
        os.environ.pop(runlock.ENV)
        self.assertIsNone(runlock.write_gpumem(gamedir))
        self.assertEqual(store.read_text(), "hudscale=150\n")
        (gamedir / "impure.cfg").unlink()
        self.assertIsNone(runlock.write_gpumem(gamedir))
        self.assertFalse(store.exists())                    # nothing to remove: no file made

    def test_a_runs_games_take_seats_and_wait_past_its_games(self):
        # a run of 2 games: two seats, each held by the process given it; a third launch waits
        # until a game ends, then takes the seat it left
        play = ("import os, subprocess, sys, time; import runlock; runlock.POLL_S = 0.05\n"
                "game = [sys.executable, '-c', 'import sys; sys.stdin.read()']\n"
                "def launch(what):\n"
                "    seat = runlock.game_seat(what)\n"
                "    p = subprocess.Popen(game, stdin=subprocess.PIPE, pass_fds=(seat.fileno(),))\n"
                "    seat.close()\n"
                "    return p\n"
                "g1, g2 = launch('g1'), launch('g2')\n"
                "print('two', runlock.games(), flush=True)\n"
                "sys.stdin.readline()\n"
                "import threading; got = []\n"
                "t = threading.Thread(target=lambda: got.append(launch('g3'))); t.start()\n"
                "time.sleep(0.5); print('third waits', not got, flush=True)\n"
                "g1.stdin.close(); g1.wait(); t.join(); print('third', bool(got), flush=True)\n"
                "for g in (g2, got[0]): g.stdin.close(); g.wait()\n")
        env = dict(self.env, PYTHONPATH=str(TOOLS))
        p = self.start(self.wrapper(4, "seats", sys.executable, "-c", play, vram=2, total=REFERENCE_MIB),
                       env=env)
        self.assertEqual(self.line(p.stdout), "two 2")
        _free, lines = runlock.report(self.lock, UNITS)
        self.assertTrue(any(ln.startswith(" 2 game seats of run s00 held: ") for ln in lines), lines)
        p.stdin.write("\n")
        p.stdin.flush()
        self.assertEqual(self.line(p.stdout), "third waits True")
        self.assertEqual(self.line(p.stdout), "third True")
        p.stdin.close()
        self.assertEqual(p.wait(WAIT_S), 0, p.stderr.read())
        # outside a run, and in a run without GPU memory, a game takes no seat
        self.assertIsNone(runlock.game_seat("loose"))
        probe = "import runlock; print(runlock.game_seat('gdi'), flush=True)"
        q = self.start(self.wrapper(1, "gdi", sys.executable, "-c", probe, vram=0, total=REFERENCE_MIB),
                       env=env)
        self.assertEqual(self.line(q.stdout), "None")
        q.wait(WAIT_S)

    def test_games_that_wait_on_each_other_need_their_seats_together(self):
        # a network game's two peers in a run of one game: refused by name before a launch, since
        # the joiner's launch would wait for the seat the host holds while the host waits for it
        for games, refused in ((1, True), (2, False), (3, False)):
            os.environ[runlock.ENV] = json.dumps({"slots": 2 * games, "gpu_mib": 1024, "games": games,
                                                  "tag": "s00", "cap_mib": 320, "resources": [],
                                                  "path": str(self.lock)})
            try:
                why = runlock.together(2, "a network game")
            finally:
                os.environ.pop(runlock.ENV)
            self.assertEqual(why is not None, refused, why)
            if refused:
                self.assertIn("a network game: its 2 games wait on each other and this run may "
                              "play 1 at once", why)

    def test_the_figures_are_read_once_the_token_queue_is_held(self):
        # a GPU run waiting for the token queue is admitted under the figures of the moment it takes
        # that queue, and publishes that cap, not the one of the moment it arrived
        probe = "import runlock; print(runlock.held_cap_mib(), runlock.games(), flush=True)"
        env = dict(self.env, PYTHONPATH=str(TOOLS))
        a = self.holder(1, "a", vram=GAMES, total=REFERENCE_MIB)
        self.admitted(a)
        b = self.holder(1, "b", vram=4, total=REFERENCE_MIB)
        self.until(lambda: self.waiting_gpu("b"), "b did not reach the token queue")
        p = self.start(self.wrapper(1, "late", sys.executable, "-c", probe, vram=2, total=REFERENCE_MIB),
                       env=env)
        self.quiet(p)
        # a game of 544: b's 4 at the 920 figures and late's 2 at these fit beside each other
        shared = runlock.shared_vram_cfg(self.lock)
        shared.write_text("reserve_mib = 3072\ncap_mib = 512\ndriver_mib = 32\nmargin_mib = 0\n")
        self.let_go(a)
        self.admitted(b)
        self.assertEqual(self.line(p.stdout), "512 2")
        p.wait(WAIT_S)

    def test_a_lock_file_that_cannot_be_read_refuses_only_gpu_runs(self):
        shared = runlock.shared_vram_cfg(self.lock)
        shared.write_text("")                                   # what a power loss could leave
        gdi = self.holder(1, "gdi", vram=0, total=REFERENCE_MIB)
        self.admitted(gdi)                                      # wants no GPU memory: reads no figure
        gpu = self.start(self.wrapper(1, "gpu", sys.executable, "-c", "pass", vram=1, total=REFERENCE_MIB))
        self.assertEqual(gpu.wait(WAIT_S), 2)
        err = gpu.stderr.read()
        self.assertIn(f"the GPU figures cannot be read", err)
        self.assertIn(str(shared), err)
        self.assertNotIn("Traceback", err)
        shared.write_text("reserve_mib = 3072\ncap_mib = lots\ndriver_mib = 32\nmargin_mib = 0\n")
        with self.assertRaisesRegex(runlock.BadFigures, "cap_mib = 'lots'"):
            runlock.read_vram_cfg(shared)

    def old_run(self, slots, tokens, what="old", go=False):
        """A run of the runlock code that counted GPU tokens and gathered them in the slot queue: it
        takes the slot queue, writes its line there in that code's form, and gathers slots and
        tokens (vram-NN, a budget of GAMES) in it, keeping each it takes, until it has them all;
        then it says `in`, lets the queue go and holds them until its stdin closes. With `go` it
        takes nothing until a line on its stdin, which orders a test's steps without a race."""
        code = ("import fcntl, os, sys, time\n"
                "d, slots, tokens, go = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4] == 'go'\n"
                "line = f'pid {os.getpid()}, {slots} slots, {tokens} GPU tokens, since then, ' + sys.argv[5] + ', from there\\n'\n"
                "def put(f):\n"
                "    f.seek(0); f.truncate(); f.write(line); f.flush()\n"
                "q = open(os.path.join(d, 'queue'), 'a+'); fcntl.flock(q, fcntl.LOCK_EX); put(q)\n"
                "got = {}\n"
                "while True:\n"
                "    if go: sys.stdin.readline(); go = False\n"
                "    for kind, n, budget in (('slot', slots, 28), ('vram', tokens, int(sys.argv[6]))):\n"
                "        for k in range(budget):\n"
                "            if sum(1 for g in got if g[0] == kind) == n: break\n"
                "            if (kind, k) in got: continue\n"
                "            f = open(os.path.join(d, f'{kind}-{k:02d}'), 'a+')\n"
                "            try: fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)\n"
                "            except BlockingIOError: f.close(); continue\n"
                "            put(f); got[kind, k] = f\n"
                "    if len(got) == slots + tokens: break\n"
                "    time.sleep(0.05)\n"
                "print('in', flush=True); q.close(); sys.stdin.read()\n")
        return self.start([sys.executable, "-c", code, str(runlock.slots_dir(self.lock)), str(slots),
                           str(tokens), "go" if go else "now", what, str(GAMES)])

    def test_no_deadlock_beside_old_code_gathering_tokens_in_the_slot_queue(self):
        # A, of the old code, holds 5 tokens; n, of this code, waits in the token queue holding
        # nothing while any token is held; o, of the old code, takes the slot queue and gathers 7
        # tokens there, keeping the 4 free ones
        a = self.hold_files(*(f"vram-{k:02d}" for k in range(5)))
        self.admitted(a)
        n = self.holder(1, "n", vram=GAMES, total=REFERENCE_MIB)
        self.until(lambda: self.waiting_gpu("n"), "n did not reach the token queue")
        o = self.old_run(1, 7)
        self.until(lambda: sum(p.read_text().startswith(f"pid {o.pid},")
                               for p in runlock.slots_dir(self.lock).glob("vram-*")) == 4,
                   "the old run did not gather the free tokens")
        self.assertEqual(self.held(n.pid), 0)
        gdi = self.holder(1, "gdi", vram=0, total=REFERENCE_MIB)
        self.quiet(gdi)                                     # the old run holds the slot queue
        self.let_go(a)
        self.admitted(o)                                    # o takes 3 of A's 5
        self.admitted(gdi)
        self.quiet(n)                                       # o's tokens are held: n takes no memory
        self.let_go(o)
        self.admitted(n)
        self.assertEqual(self.held(n.pid), units(GAMES))

    def test_memory_waiting_for_the_slot_queue_is_no_obstacle_to_old_code(self):
        # n has its memory and waits for the slot queue, which an old run holds while it gathers
        # tokens: the old code never wants a unit, so n keeps its memory and the old run is admitted
        o = self.old_run(1, 2, go=True)
        self.until(lambda: self.admitting("old"), "the old run did not take the slot queue")
        n = self.holder(1, "n", vram=GAMES, total=REFERENCE_MIB)
        self.until(lambda: self.held(n.pid) == units(GAMES) and not self.waiting_gpu("n"),
                   "n did not take its memory")
        self.quiet(n)
        o.stdin.write("go\n")
        o.stdin.flush()
        self.admitted(o)
        self.admitted(n)
        self.assertEqual(self.held(n.pid), units(GAMES))

    def test_resources_exclude_by_name_and_admit_different_names(self):
        a = self.holder(2, "a", resources=["instance-x"])
        self.admitted(a)
        other = self.holder(2, "other", resources=["instance-y"])
        self.admitted(other)
        b = self.holder(2, "b", resources=["instance-x"])
        self.until(lambda: self.admitting("b"), "b did not reach the queue")
        self.assertTrue(any(ln.startswith("resource instance-x held by") and ", a, " in ln
                            for ln in runlock.report(self.lock)[1]))
        # b holds the queue and its slots, and the instance a holds is all it waits for
        self.quiet(b)
        self.let_go(a)
        self.admitted(b)

    def test_a_resource_waits_in_the_queue_like_a_slot(self):
        a = self.holder(2, "a", resources=["instance-x"])
        self.admitted(a)
        b = self.holder(2, "b", resources=["instance-x"])
        self.until(lambda: self.admitting("b"), "b did not reach the queue")
        c = self.holder(2, "c", resources=["instance-y"])
        self.assertIn("waiting for the run lock", self.line(c.stderr) or "")
        self.quiet(c)                   # behind b in arrival order, though its instance is free
        self.let_go(a)
        self.admitted(b)
        self.admitted(c)

    def test_a_nested_run_takes_a_resource_without_waiting_or_is_refused(self):
        a = self.holder(2, "a", resources=["instance-x"])
        self.admitted(a)
        take = ("import sys, pathlib; sys.path.insert(0, {tools!r}); import runlock; "
                "tree = pathlib.Path({tree!r})\n"
                "try:\n    print(runlock.hold(tree, 'nested', 1, [sys.argv[1]], gpu=False), runlock.fds() != (), flush=True)\n"
                "except runlock.Busy as e:\n    print('busy', e, flush=True)")
        take = take.format(tools=str(TOOLS), tree=str(self.tree))
        outer = self.start(self.wrapper(2, "outer", "bash", "-c",
                                        f"{sys.executable} -c \"$0\" instance-y && "
                                        f"{sys.executable} -c \"$0\" instance-x", take))
        self.assertEqual(self.line(outer.stdout), "True True")
        got = self.line(outer.stdout)
        self.assertTrue(got.startswith("busy instance-x is held by another run (pid "), got)
        self.assertEqual(outer.wait(WAIT_S), 0)

    def test_live_runs_have_distinct_tags_and_a_free_tag_is_reused(self):
        show = ("import json, os, sys; print(json.loads(os.environ['RUNLOCK_HELD'])['tag'], flush=True); "
                "sys.stdin.read()")
        runs, tags = [], []
        for k, w in enumerate((3, 1, 5)):          # one after the other: slots 0-2, 3, 4-8
            runs.append(self.start(self.wrapper(w, f"t{k}", sys.executable, "-c", show)))
            tags.append(self.line(runs[-1].stdout))
        self.assertEqual(tags, ["s00", "s03", "s04"])
        self.let_go(runs[0])
        again = self.start(self.wrapper(1, "again", sys.executable, "-c", show))
        self.assertEqual(self.line(again.stdout), "s00")

    def test_a_dead_runs_game_holds_up_every_run_that_wants_gpu_memory(self):
        go = self.tree / "go"
        play = ("import subprocess, sys; import runlock\n"
                f"g = runlock.start_game([sys.executable, '-c', {SLEEPS_UNTIL!r}, {str(go)!r}], 'orphan',\n"
                "                        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,\n"
                "                        stderr=subprocess.DEVNULL, start_new_session=True)\n"
                "print('launched', g.pid, flush=True)\n"
                "sys.stdin.read()\n")
        self.addCleanup(go.touch)
        run = self.start(self.wrapper(2, "dies", sys.executable, "-c", play, vram=1, total=REFERENCE_MIB),
                         env=dict(self.env, PYTHONPATH=str(TOOLS)))
        word, keeper = self.line(run.stdout).split()
        self.assertEqual(word, "launched")
        d = self.lock.parent / "runslots"
        seat = runlock.seat_file(d, "s00", 0)
        self.assertEqual(runlock.orphaned_seats(d), [], "a live run's seat counted as a dead run's")
        run.kill()                              # the run dies outright; its game plays on
        run.wait(WAIT_S)
        self.assertEqual(self.held(), 0)
        self.assertTrue(runlock._held_now(seat))
        self.assertEqual(runlock.orphaned_seats(d), [seat.name])
        _free, lines = runlock.report(self.lock, UNITS)
        self.assertTrue(any("OF NO LIVE RUN" in ln and f"pid {keeper}," in ln and "STALE" not in ln
                            for ln in lines), lines)
        # a run that wants GPU memory waits for the game, naming its seat; one that wants none does not
        nxt = self.holder(2, "next", vram=1, total=REFERENCE_MIB)
        self.stderr_until(nxt, seat.name)
        self.quiet(nxt)
        gdi = self.holder(2, "gdi", vram=0, total=REFERENCE_MIB)
        self.admitted(gdi)
        go.touch()
        self.admitted(nxt)

    def test_a_gpu_run_inside_a_run_without_gpu_memory_is_refused(self):
        for vram, refused in ((None, True), (0, False)):
            inner = self.wrapper(1, "inner", sys.executable, "-c", "print('in')", vram=vram,
                                 total=REFERENCE_MIB)
            outer = self.start(self.wrapper(2, "outer", sys.executable, "-c",
                                            f"import subprocess, sys; "
                                            f"sys.exit(subprocess.run({inner!r}).returncode)",
                                            vram=0, total=REFERENCE_MIB))
            code = outer.wait(WAIT_S)
            out, err = outer.stdout.read(), outer.stderr.read()
            if refused:
                self.assertEqual((code, out), (2, ""), err)
                self.assertIn("inner wants its games on the GPU inside a run that took no GPU memory",
                              err)
            else:
                self.assertEqual((code, out.strip()), (0, "in"), err)

    def test_a_measurements_cap_sizes_the_run_and_its_games(self):
        probe = "import runlock; print(runlock.held_cap_mib(), runlock.games(), flush=True)"
        env = dict(self.env, PYTHONPATH=str(TOOLS), RUNLOCK_CAP_MIB="2000")
        # a suite of 320 measured at 2000: each game takes 2000 + 64 + 32 of the budget, whatever
        # its suite says
        p = self.start(self.wrapper(1, "measured", sys.executable, "-c", probe, vram=30,
                                    total=REFERENCE_MIB, suite="light"), env=env)
        self.assertEqual(self.line(p.stdout), f"2000 {UNITS * 32 // 2096}")
        p.wait(WAIT_S)
        for bad in ("lots", "32"):
            p = self.start(self.wrapper(1, "bad", sys.executable, "-c", probe, vram=1,
                                        total=REFERENCE_MIB), env=dict(env, RUNLOCK_CAP_MIB=bad))
            self.assertEqual(p.wait(WAIT_S), 2)
            self.assertIn("RUNLOCK_CAP_MIB", p.stderr.read())

    def test_a_launch_under_another_cap_than_the_measurements_is_refused(self):
        gamedir = self.tree / "game"
        gamedir.mkdir()
        self.addCleanup(lambda: [os.environ.pop(k, None) for k in (runlock.ENV, runlock.CAP_ENV)])
        os.environ[runlock.ENV] = json.dumps({"slots": 2, "gpu_mib": 1024, "games": 1, "tag": "s00",
                                              "cap_mib": 320, "resources": [], "path": str(self.lock)})
        os.environ[runlock.CAP_ENV] = "2000"
        with self.assertRaises(RuntimeError) as e:
            runlock.write_gpumem(gamedir)
        self.assertIn("admitted under a cap of 320", str(e.exception))
        self.assertFalse((gamedir / "impure.cfg").exists())
        os.environ[runlock.CAP_ENV] = "320"
        self.assertEqual(runlock.write_gpumem(gamedir), 320)

    # ---- OFFLINE WORK, BUILDS ------------------------------------------------------------------

    def in_process(self):
        """This process's own holds released, and RUNLOCK_HELD put back, after the test; no GPU."""
        env = os.environ.pop(runlock.ENV, None)
        self.addCleanup(lambda: env is not None and os.environ.update({runlock.ENV: env}))
        self.addCleanup(runlock.release)
        self.addCleanup(setattr, runlock, "device_total_mib", runlock.device_total_mib)
        runlock.device_total_mib = lambda: None

    def test_an_offline_phase_takes_what_is_free_and_waits_for_its_least(self):
        self.in_process()
        a = self.holder(20, "a")
        self.admitted(a)
        w = runlock.offline(self.tree, "phase", 16, least=3)
        self.addCleanup(w.release)
        # 8 free of the 16 asked: its workers are cut to them
        self.assertEqual((w.n, self.free()), (8, 0))
        held = json.loads(w.environ()[runlock.ENV])
        self.assertEqual((held["slots"], held["workers"], held["path"]), (8, 8, str(self.lock)))
        self.assertEqual(len(w.fds()), 8 + 1)          # its slots and tareplay.lock shared
        self.assertNotIn(runlock.ENV, os.environ, "a phase's hold is not this process's hold()")
        w.release()
        self.assertEqual(self.free(), 8)
        b = self.holder(6, "b")
        self.admitted(b)
        # 2 free, under its least of 3: it waits in the queue, holding them, and takes the 3 it needs
        p = self.phase(16, 3)
        self.until(lambda: self.admitting("phase") and self.free() == 0, "the phase did not take the free slots")
        self.quiet(p)
        self.let_go(b)
        self.assertEqual(self.line(p.stdout), "8")
        self.assertEqual(self.free(), 0)

    def test_a_nested_offline_phase_takes_none_and_is_cut_to_its_runs_workers(self):
        probe = ("from pathlib import Path; w = runlock.offline(Path({tree!r}), 'inner', {most}); "
                 "print(w.n, w.fds() == (), flush=True)")
        for most, want in ((16, "4 True"), (3, "3 True")):
            outer = self.start(self.wrapper(4, "outer", sys.executable, "-c",
                                            self.boot() + probe.format(tree=str(self.tree), most=most)))
            self.assertEqual(self.line(outer.stdout), want)
            self.assertEqual(outer.wait(WAIT_S), 0)

    def test_offline_workers_beside_game_lanes_are_cut_to_their_grant(self):
        self.in_process()
        a = self.holder(20, "a")
        self.admitted(a)
        # three Vulkan lanes' 6 slots and at least one worker: 8 free, so 2 workers of the 10 asked
        self.assertTrue(runlock.hold(self.tree, "lanes", 3, gpu=False, workers=10, renderer="vulkan"))
        self.assertEqual((runlock.games(), runlock.workers(), self.free()), (3, 2, 0))
        self.assertEqual(json.loads(os.environ[runlock.ENV])["slots"], 8)
        self.assertEqual(runlock.slots(), 8)
        runlock.release()
        # three GDI lanes' 3 slots: the other 5 go to the workers
        self.assertTrue(runlock.hold(self.tree, "lanes", 3, gpu=False, workers=10))
        self.assertEqual((runlock.games(), runlock.workers(), self.free()), (3, 5, 0))
        runlock.release()
        self.let_go(a)
        self.assertTrue(runlock.hold(self.tree, "lanes", 3, gpu=False, workers=10, renderer="vulkan"))
        self.assertEqual((runlock.games(), runlock.workers(), self.free()), (3, 10, 12))
        runlock.release()
        # lanes that would fill the budget leave one slot to the workers they asked for
        self.assertTrue(runlock.hold(self.tree, "lanes", 14, gpu=False, workers=4, renderer="vulkan"))
        self.assertEqual((runlock.games(), runlock.workers(), self.free()), (13, 2, 0))
        # the games' slots and the workers' are the run's every slot
        self.assertEqual(runlock.slots(), runlock.workers() + runlock.game_slots() * runlock.games())
        runlock.release()
        self.assertTrue(runlock.hold(self.tree, "lanes", 28, gpu=False, workers=4))
        self.assertEqual((runlock.games(), runlock.workers(), self.free()), (27, 1, 0))
        runlock.release()
        # a run that asks for none: its workers are its slots, which its own offline work shares
        self.assertTrue(runlock.hold(self.tree, "lanes", 2, gpu=False))
        self.assertEqual((runlock.workers(), self.free()), (2, 26))
        self.assertEqual(runlock.slots(), 2)

    def test_a_phases_tasks_share_its_grant_by_weight_in_the_order_they_ask(self):
        w = runlock.Workers(3)
        order, gates = [], {k: threading.Event() for k in ("heavy", "light1", "light2")}

        def task(name, weight):
            with w.share(weight) as got:
                order.append((name, got))
                gates[name].wait(WAIT_S)
        first = threading.Thread(target=task, args=("light1", 1))
        first.start()
        self.until(lambda: order == [("light1", 1)], "a task did not start on a free grant")
        heavy = threading.Thread(target=task, args=("heavy", 5))       # cut to the grant's 3
        heavy.start()
        self.until(lambda: len(w._turns) == 1, "the heavy task did not ask")
        second = threading.Thread(target=task, args=("light2", 1))
        second.start()
        self.until(lambda: len(w._turns) == 2, "the light task did not ask")
        # one slot is held, two are free: the light task behind the heavy one waits with it
        self.assertEqual(order, [("light1", 1)])
        gates["light1"].set()
        self.until(lambda: len(order) == 2, "the heavy task did not start once the grant was free")
        self.assertEqual(order[1], ("heavy", 3))
        gates["heavy"].set()
        gates["light2"].set()
        for t in (first, heavy, second):
            t.join(WAIT_S)
        self.assertEqual(order[2], ("light2", 1))
        self.assertEqual(w._free, 3)

    def test_a_widened_grant_lets_the_waiting_tasks_in(self):
        w = runlock.Workers(1)
        started, gate = [], threading.Event()

        def task(name):
            with w.share():
                started.append(name)
                gate.wait(WAIT_S)
        threads = [threading.Thread(target=task, args=(n,)) for n in ("a", "b", "c")]
        for t in threads:
            t.start()
        self.until(lambda: len(started) == 1 and len(w._turns) == 2, "the tasks did not ask")
        w.widen(2)
        self.until(lambda: len(started) == 3, "the widened grant did not let the waiting tasks in")
        self.assertEqual(w.n, 3)
        gate.set()
        for t in threads:
            t.join(WAIT_S)
        self.assertEqual(w._free, 3)

    def test_make_runs_at_the_slots_it_was_granted(self):
        bindir = self.tree / "bin"
        bindir.mkdir()
        fake = bindir / "make"
        fake.write_text("#!/bin/sh\necho \"$*\"\nread x\n")
        fake.chmod(0o755)
        env = dict(self.env, PATH=f"{bindir}:{os.environ['PATH']}")
        a = self.holder(26, "a")
        self.admitted(a)
        build = self.start(self.wrapper(8, "a build", "-C", "tagpu/ddraw", "all", sub="make"), env=env)
        # 2 free of the 8 asked: make runs at -j 2 and holds them
        self.assertEqual(self.line(build.stdout), "-j2 -C tagpu/ddraw all")
        self.assertEqual(self.free(), 0)
        self.let_go(build)
        self.assertEqual(self.free(), 2)
        self.let_go(a)
        # inside a run of 4 slots it takes none and runs at the run's 4
        inner = self.wrapper(8, "inner build", "all", sub="make")
        outer = self.start(self.wrapper(4, "outer", *inner), env=env)
        self.assertEqual(self.line(outer.stdout), "-j4 all")
        self.assertEqual(self.free(), runlock.BUDGET - 4)

    # ---- THE HOST MEMORY -----------------------------------------------------------------------

    def host(self, total=HOST_MIB, reserve=RESERVE_MIB):
        """This process's runs on a machine of `total` MiB, the lock's reserve seeded from a tree file
        of `reserve` (the lock's runslots/mem.cfg, once seeded, is what every run reads)."""
        cfg = self.tree / "runlock-mem.cfg"
        cfg.write_text(f"# a test's host memory figures\nreserve_mib = {reserve}\n")
        for name, value in (("MEM_CFG", cfg), ("host_total_mib", lambda: total)):
            self.addCleanup(setattr, runlock, name, getattr(runlock, name))
            setattr(runlock, name, value)
        return cfg

    def test_a_slot_carries_the_machines_memory_less_the_locks_reserve(self):
        cfg = self.host()
        self.assertEqual(runlock.slot_mib(self.lock), SLOT_MIB)
        self.assertLessEqual(runlock.BUDGET * SLOT_MIB, HOST_MIB - RESERVE_MIB, "the slots carry the reserve")
        shared = runlock.shared_mem_cfg(self.lock)
        self.assertEqual(shared.read_text(), cfg.read_text(), "the lock's figures are seeded from the tree's")
        # a tree whose reserve differs reads the lock's
        cfg.write_text("reserve_mib = 1024\n")
        self.assertEqual(runlock.slot_mib(self.lock), SLOT_MIB)
        # a reserve that leaves no memory is refused, so is a file that is not whole
        shared.write_text(f"reserve_mib = {HOST_MIB}\n")
        self.assertRaises(runlock.BadFigures, runlock.slot_mib, self.lock)
        shared.write_text("reserve_mib = 1024\nreserve_mib = 2048\n")
        self.assertRaises(runlock.BadFigures, runlock.slot_mib, self.lock)
        # no memory to read: no figure
        runlock.host_total_mib = lambda: None
        self.assertIsNone(runlock.slot_mib(self.lock))

    def jobs(self, *spec):
        """forked()'s jobs of (MiB allocated, MiB declared[, the starts each waits for]), logging to one file:
        (the jobs, the log)."""
        log = self.tree / "jobs.log"
        return [(mib, needs, log, *(wait or [0])) for mib, needs, *wait in spec], log

    def test_a_forked_job_is_bound_to_its_slots_and_run_again_on_twice_past_them(self):
        self.in_process()
        self.host(SMALL_MIB, SMALL_RESERVE)
        self.assertEqual([runlock.slots_for(m, SMALL_SLOT) for m in (1, SMALL_SLOT, SMALL_SLOT + 1, 700)],
                         [1, 1, 2, 3])
        # 20 MiB in one slot; 300 MiB said to need 100, so one slot and then two; 100 said to need 600, three
        # slots; 3000 said to need 100: one, two, then four, the grant of the two heaviest -- past it, OverBound
        jobs, _log = self.jobs((20, 100), (300, 100), (100, 600), (3000, 100))
        got = runlock.forked(self.tree, "bound jobs", mem_job, jobs, lambda job: job[1], 2)
        adj = runlock.OOM_SCORE_ADJ
        self.assertEqual(got[:3], [(20, SMALL_SLOT, adj), (300, 2 * SMALL_SLOT, adj), (100, 3 * SMALL_SLOT, adj)])
        self.assertIsInstance(got[3], runlock.OverBound)
        self.assertEqual(got[3].mib, 4 * SMALL_SLOT)
        self.assertEqual(self.free(), runlock.BUDGET, "every slot is let go")
        # no memory to read: a slot a job, unbound
        runlock.host_total_mib = lambda: None
        self.assertEqual(runlock.forked(self.tree, "unbound", mem_job, self.jobs((300, 100))[0], lambda job: job[1], 2),
                         [(300, -1, adj)])

    def test_forked_jobs_never_hold_more_memory_than_their_grant_carries(self):
        self.in_process()
        self.host(SMALL_MIB, SMALL_RESERVE)
        a = self.holder(runlock.BUDGET - 6, "a")
        self.admitted(a)
        # six slots free: the jobs' slots never sum past them, and the memory those carry never past six slots'
        # the first two, of four slots together, wait for each other: run one at a time, they would not end
        # together, and the peak would be the first's three slots
        spec = [(10, 3 * SMALL_SLOT, 2), (10, SMALL_SLOT, 2), (10, 2 * SMALL_SLOT), (10, 3 * SMALL_SLOT), (10, 1),
                (10, 2 * SMALL_SLOT), (10, 1)]
        jobs, log = self.jobs(*spec)
        got = runlock.forked(self.tree, "six slots", mem_job, jobs, lambda job: job[1], 4)
        self.assertEqual([g[1] for g in got], [runlock.slots_for(n, SMALL_SLOT) * SMALL_SLOT for _m, n, *_w in spec])
        held = peak = 0
        for line in log.read_text().splitlines():
            event, bound = line.split()
            held += int(bound) if event == "start" else -int(bound)
            peak = max(peak, held)
        self.assertLessEqual(peak, 6 * SMALL_SLOT)
        self.assertGreaterEqual(peak, 4 * SMALL_SLOT, "the first two ran beside each other")
        self.assertEqual(self.free(), 6, "the phase let its slots go")

    def test_a_forked_job_that_is_killed_or_raises_fails_by_name(self):
        self.in_process()
        self.host(SMALL_MIB, SMALL_RESERVE)
        for how, said in (("kill", "killed by signal 9"), ("raise", "ValueError: a job's own error")):
            with self.assertRaises(runlock.JobFailed) as e:
                runlock.forked(self.tree, f"doomed {how}", doomed_job, [how], lambda job: 1, 2)
            self.assertIn(said, str(e.exception))
            self.assertEqual(self.free(), runlock.BUDGET)

    def test_forked_jobs_inside_a_run_take_its_slots_and_their_memory(self):
        self.host(SMALL_MIB, SMALL_RESERVE)
        runlock.slot_mib(self.lock)                     # the lock's figures seeded before the outer run
        code = (self.boot()
                + f"sys.path.insert(0, {str(TOOLS)!r}); import test_runlock; "
                  f"runlock.MEM_CFG = Path({str(self.tree / 'runlock-mem.cfg')!r}); "
                  f"runlock.host_total_mib = lambda: {SMALL_MIB}; "
                  f"print(runlock.forked(Path({str(self.tree)!r}), 'inner', test_runlock.mem_job, "
                  f"[(100, {3 * SMALL_SLOT}, {str(self.tree / 'jobs.log')!r}, 0)], lambda job: job[1], 2), flush=True)")
        outer = self.start(self.wrapper(2, "outer", sys.executable, "-c", code))
        # the run's two slots: the job said to need three is cut to them, and bound to their memory
        self.assertEqual(self.line(outer.stdout), f"[(100, {2 * SMALL_SLOT}, {runlock.OOM_SCORE_ADJ})]")
        self.assertEqual(outer.wait(WAIT_S), 0)

    def test_a_bound_process_fails_its_allocation_and_so_does_its_child(self):
        code = (f"import sys; sys.path.insert(0, {str(TOOLS)!r}); import runlock; "
                "runlock.bind(256); "
                "small = bytearray(64 << 20); "
                "print(open('/proc/self/oom_score_adj').read().strip(), flush=True)\n"
                "try:\n    big = bytearray(512 << 20)\nexcept MemoryError:\n    print('MemoryError', flush=True)\n"
                "else:\n    print('grew', flush=True)")
        r = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, timeout=WAIT_S)
        self.assertEqual(r.stdout.split(), [str(runlock.OOM_SCORE_ADJ), "MemoryError"], r.stderr)
        code = (f"import subprocess, sys; sys.path.insert(0, {str(TOOLS)!r}); import runlock; "
                "runlock.bind(256); "
                "r = subprocess.run([sys.executable, '-c', 'bytearray(512 << 20)'], capture_output=True, text=True); "
                "print('MemoryError' in r.stderr)")
        r = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, timeout=WAIT_S)
        self.assertEqual(r.stdout.strip(), "True", r.stderr)

    def test_an_admitted_run_is_the_kernels_first_victim(self):
        r = subprocess.run(self.wrapper(1, "victim", sys.executable, "-c",
                                        "print(open('/proc/self/oom_score_adj').read().strip())"),
                           capture_output=True, text=True, timeout=WAIT_S, cwd=self.tree, env=self.env)
        self.assertEqual(r.stdout.strip(), str(runlock.OOM_SCORE_ADJ), r.stderr)

    # ---- THE LOAD GUARD ------------------------------------------------------------------------

    def test_the_load_guard_holds_a_run_back_past_the_limit_and_admits_it_under(self):
        a = self.holder(5, "a")
        self.admitted(a)
        # 30 of load beside a's 5 slots leaves room for 2 under the limit of 32: b's 4 wait
        self.load.write_text("30")
        b = self.holder(4, "b")
        said = self.stderr_until(b, "waiting for the run lock")[-1]
        self.assertIn("the 1-minute load 30.0 beside 5 slots held by other runs leaves room for 2", said)
        self.quiet(b)
        # a run behind it waits with it, though it would fit
        c = self.holder(1, "c")
        self.assertIn("waiting for the run lock", self.line(c.stderr) or "")
        self.quiet(c)
        self.load.write_text("20")          # room for 12
        self.admitted(b)
        self.admitted(c)
        self.assertEqual(self.free(), runlock.BUDGET - 10)

    def test_the_load_guard_counts_held_slots_once(self):
        a = self.holder(20, "a")
        self.admitted(a)
        # the load is the holders' own: 20 slots under a load of 18 leave the 8 free to b
        self.load.write_text("18")
        b = self.holder(8, "b")
        self.admitted(b)

    def test_the_load_guard_cuts_an_offline_phase_to_the_room_left(self):
        a = self.holder(5, "a")
        self.admitted(a)
        self.load.write_text("20")          # room for 12
        p = self.phase(16, 3)
        self.assertEqual(self.line(p.stdout), "12")
        self.assertEqual(self.free(), runlock.BUDGET - 17)

    def test_the_load_guard_admits_when_no_other_process_holds_a_slot(self):
        self.load.write_text("100")
        # nothing held: a load that is none of the lock's never holds the queue up
        a = self.holder(10, "a")
        self.admitted(a)
        b = self.holder(1, "b")
        self.until(lambda: self.admitting("b"), "b did not reach the queue")
        self.quiet(b)
        # a ends: no other process holds a slot, so b goes whatever the load
        self.let_go(a)
        self.admitted(b)
        self.let_go(b)
        # a process's own holds count for the load but never hold its next one up
        self.in_process()
        w = runlock.offline(self.tree, "own phase", 4)
        self.addCleanup(w.release)
        self.assertEqual(w.n, 4)
        self.assertTrue(runlock.hold(self.tree, "own lanes", 2, gpu=False))
        self.assertEqual(self.free(), runlock.BUDGET - 4 - 2 * GDI)
        # nor a child on its parent's: this process holds the only slots held, and a run it starts goes
        c = self.holder(2, "child")
        self.admitted(c)

    def test_status_says_what_the_load_guard_says(self):
        a = self.holder(5, "a")
        self.admitted(a)
        self.load.write_text("30")
        env = dict(self.env, PYTHONPATH=str(TOOLS))
        out = subprocess.run([sys.executable, "-c", self.boot() + "sys.exit(runlock.main(['status']))"],
                             cwd=self.tree, capture_output=True, text=True, env=env).stdout
        self.assertIn("load guard: the 1-minute load 30.0 beside 5 slots held by other runs leaves room for 2",
                      out)

    # ---- THE SIM POOL --------------------------------------------------------------------------

    def batch(self, most, least=1, what="batch"):
        """A sim batch of `most` places at most and `least` at least in a process of its own: once
        admitted it says the places granted and whether it runs in the idle class, and holds them
        until its stdin closes."""
        return self.start([sys.executable, "-c", self.boot()
                           + f"import os; w = runlock.sim_pool(Path({str(self.tree)!r}), {what!r}, {most}, {least}); "
                             "print(w.n, os.sched_getscheduler(0) == os.SCHED_IDLE, flush=True); sys.stdin.read()"])

    def gamer(self, most, least=1, what="batch"):
        """A sim batch as batch() makes one, whose every line on stdin asks for that many more games:
        each a thread that plays one game through Workers.game(), a process that waits for the end
        of a pipe only the batch holds -- killed by end_game(), or ended with its batch. What the
        batch says goes to the file said() reads."""
        return self.start([sys.executable, "-c", self.boot()
                           + f"sys.stderr = open({str(self.tree / (what + '.said'))!r}, 'w', buffering=1); "
                           + f"import os, subprocess, threading; "
                             f"w = runlock.sim_pool(Path({str(self.tree)!r}), {what!r}, {most}, {least}); "
                             "r, _keep = os.pipe(); print(w.n, os.sched_getscheduler(0) == os.SCHED_IDLE, flush=True)\n"
                             "def one():\n"
                             "    with w.game() as argv:\n"
                             "        subprocess.run(argv([sys.executable, '-c', 'import sys; sys.stdin.read()']), stdin=r)\n"
                             "for line in sys.stdin:\n"
                             "    for _ in range(int(line)):\n"
                             "        threading.Thread(target=one, daemon=True).start()\n"])

    def said(self, what, text):
        """Wait until the batch gamer() made as `what` has said `text`."""
        f = self.tree / f"{what}.said"
        self.until(lambda: text in f.read_text(), f"{what} never said {text!r}; said: {f.read_text()!r}")

    def ask(self, p, n):
        p.stdin.write(f"{n}\n")
        p.stdin.flush()

    def games(self):
        """{place: pid} of the sim games playing, each game file asked of its own lock and its line's
        pid alive: a file whose new game has not written its line yet names a game that has ended."""
        out = {}
        for k, f in runlock._numbered(self.lock.parent / "runslots", "simgame"):
            m = re.match(r"pid (\d+),", f.read_text())
            if runlock._held_now(f) and m and Path(f"/proc/{m.group(1)}").exists():
                out[k] = int(m.group(1))
        return out

    def end_game(self, pid):
        os.kill(pid, signal.SIGKILL)

    def places(self):
        return runlock.places_held(self.lock.parent / "runslots")

    def granted(self, p, n):
        self.assertEqual(self.line(p.stdout), f"{n} True", "the batch was not granted its places at idle priority")

    def waiting_pool(self, what):
        return any(ln.startswith("waiting for the sim pool:") and f", {what}, " in ln
                   for ln in runlock.sim_report(self.lock))

    def status(self):
        return subprocess.run([sys.executable, "-c", self.boot() + "sys.exit(runlock.main(['status']))"],
                              cwd=self.tree, capture_output=True, text=True, env=self.env).stdout

    def test_the_pools_size_is_the_threads_the_engine_slots_leave_within_its_floor_and_cap(self):
        self.assertEqual([runlock.sim_size(THREADS, s, SIM_FLOOR, SIM_CAP) for s in (0, 8, 16, 24, 28)],
                         [24, 24, 16, 8, 8])
        for engine, want in ((20, 12), (28, 8)):
            a = self.holder(engine, "engine")
            self.admitted(a)
            b = self.batch(24)
            self.granted(b, want)
            self.let_go(b)
            self.let_go(a)

    def test_two_batches_share_the_pool_and_take_no_slot(self):
        a = self.batch(16, what="a")
        self.granted(a, 16)
        b = self.batch(16, what="b")
        self.granted(b, 8)
        self.assertEqual((self.places(), self.free()), (SIM_CAP, runlock.BUDGET))
        # an engine run that wants every slot goes beside a full pool
        c = self.holder(runlock.BUDGET, "engine")
        self.admitted(c)
        # and a batch admitted afterwards waits: the pool is the 8 of the floor, all held
        self.let_go(b)
        d = self.batch(16, least=4, what="d")
        self.until(lambda: self.waiting_pool("d"), "d did not reach the sim queue")
        self.quiet(d)
        self.let_go(a)
        self.granted(d, 8)

    def test_a_batch_waits_for_its_least_in_arrival_order_and_is_admitted_when_another_ends(self):
        a = self.batch(20, what="a")
        self.granted(a, 20)
        # 4 places left, under b's least of 6: b waits holding none
        b = self.batch(8, least=6, what="b")
        self.until(lambda: self.waiting_pool("b"), "b did not reach the sim queue")
        self.quiet(b)
        self.assertEqual(self.places(), 20)
        # c would fit in the 4 left, and waits behind b
        c = self.batch(1, what="c")
        self.assertIn("another batch ahead in the sim queue", self.line(c.stderr) or "")
        self.quiet(c)
        self.let_go(a)
        self.granted(b, 8)
        self.granted(c, 1)
        self.assertEqual(self.places(), 9)

    def test_a_dead_batch_frees_its_places(self):
        a = self.batch(24, what="a")
        self.granted(a, 24)
        b = self.batch(4, least=4, what="b")
        self.until(lambda: self.waiting_pool("b"), "b did not reach the sim queue")
        self.quiet(b)
        a.send_signal(signal.SIGKILL)
        a.wait(WAIT_S)
        self.granted(b, 4)
        self.assertEqual(self.places(), 4)
        out = self.status()
        self.assertIn(f" 4 places of the sim pool held by pid {b.pid}, a sim batch of 4 places, ", out)
        self.assertNotIn(f"pid {a.pid},", out)

    def test_the_load_guard_takes_the_games_playing_off_the_load_and_never_below_zero(self):
        d = self.lock.parent / "runslots"
        a = self.holder(5, "a")
        self.admitted(a)
        b = self.gamer(10, least=10, what="b")
        self.granted(b, 10)
        # 30 of load beside a's 5 slots and 10 places held that play no game: none comes off
        self.load.write_text("30")
        self.assertEqual(runlock._guard(d, ()),
                         (2, "the 1-minute load 30.0 beside 5 slots held by other runs leaves room for 2 under "
                             f"the load guard's {LIMIT:g}"))
        c = self.holder(4, "c")
        self.until(lambda: self.admitting("c"), "c did not reach the queue")
        self.quiet(c)
        # 4 games playing on the 10 places: 26 beside a's 5 slots leaves room for 6, so c's 4 go
        self.ask(b, 4)
        self.until(lambda: len(self.games()) == 4, "the batch did not start its 4 games")
        self.admitted(c)
        self.let_go(c)
        self.assertIn("load guard: the 1-minute load 30.0 less the 4 sim games playing, 26.0, beside 5 slots "
                      "held by other runs leaves room for 6", self.status())
        # a game that ends is no longer taken off
        self.end_game(self.games()[0])
        self.until(lambda: len(self.games()) == 3, "the game did not end")
        self.assertEqual(runlock._guard(d, ())[0], 32 - 27)
        # a load under the games playing is none, never a negative one that would add room
        self.load.write_text("1")
        room, why = runlock._guard(d, ())
        self.assertEqual(room, int(LIMIT) - 5)
        self.assertIn("the 1-minute load 1.0 less the 3 sim games playing, 0.0, beside 5 slots", why)

    def test_a_batch_starts_no_game_past_the_pools_size_now_and_widens_when_it_grows(self):
        b = self.gamer(SIM_CAP, what="b")
        self.granted(b, SIM_CAP)
        self.ask(b, 100)
        self.until(lambda: sorted(self.games()) == list(range(SIM_CAP)), "the batch did not play on its 24 places")
        # an engine run of every slot is admitted beside the full batch at once
        engine = self.holder(runlock.BUDGET, "engine")
        self.admitted(engine)
        # the size is the floor's 8: a game that ends on place 20 is followed by none there
        first = self.games()
        self.end_game(first[20])
        self.said("b", f"b: the sim pool is {SIM_FLOOR} places now ({runlock.BUDGET} engine slots held), "
                             f"so 16 of this batch's 24 places start no game until it grows")
        self.assertEqual(sorted(self.games()), [k for k in range(SIM_CAP) if k != 20])
        # one that ends on place 3 is followed by another there
        self.end_game(first[3])
        self.until(lambda: self.games().get(3, first[3]) != first[3], "no game followed on place 3")
        # once every game in play has ended, the batch plays the floor's 8, on its places under the size
        for k, pid in self.games().items():
            if k != 3:
                self.end_game(pid)
        self.until(lambda: sorted(self.games()) == list(range(SIM_FLOOR))
                   and not set(self.games().values()) & set(first.values()),
                   "the batch did not fall to the floor's 8 games")
        out = self.status()
        self.assertIn(f"{SIM_CAP} held, 0 free; {SIM_FLOOR} games playing, 0 of them on places at or above the "
                      f"size, finishing; 16 places held at or above the size start no game;", out)
        self.assertIn(f"24 places of the sim pool held by pid {b.pid}, a sim batch of 1 to 24 places, ", out)
        self.assertIn(f"; {SIM_FLOOR} games playing, 16 yielding", out)
        # the engine run ends: the batch widens to its grant again
        self.let_go(engine)
        self.until(lambda: sorted(self.games()) == list(range(SIM_CAP)), "the batch did not widen to its 24 places")
        self.said("b", f"b: the sim pool is {SIM_CAP} places now (0 engine slots held): this batch starts "
                             f"games on 24 of its 24 places")

    def test_two_batches_together_never_play_past_the_pools_size(self):
        a = self.gamer(16, what="a")
        self.granted(a, 16)
        b = self.gamer(16, what="b")
        self.granted(b, 8)
        self.ask(a, 100)
        self.ask(b, 100)
        self.until(lambda: len(self.games()) == SIM_CAP, "the batches did not play on their 24 places")
        first = set(self.games().values())
        # an engine run of 20 slots: the size is 12, all of it on a's places 0..15, none on b's 16..23
        engine = self.holder(20, "engine")
        self.admitted(engine)
        for pid in first:
            self.end_game(pid)
        self.until(lambda: sorted(self.games()) == list(range(12)) and not set(self.games().values()) & first,
                   "the batches did not fall to the 12 games of the size")
        self.said("b", "b: the sim pool is 12 places now (20 engine slots held), so 8 of this batch's 8 "
                             "places start no game until it grows")
        out = self.status()
        self.assertIn(f"16 places of the sim pool held by pid {a.pid}, ", out)
        self.assertIn("; 12 games playing, 4 yielding", out)
        self.assertIn(f" 8 places of the sim pool held by pid {b.pid}, ", out)
        self.assertIn("; 0 games playing, 8 yielding", out)
        # b, whose places are all above the size, resumes once the engine run ends
        self.let_go(engine)
        self.until(lambda: len(self.games()) == SIM_CAP, "the batches did not widen to their 24 places")
        self.said("b", "b: the sim pool is 24 places now (0 engine slots held): this batch starts games on "
                             "8 of its 8 places")

    def test_a_batch_nested_in_an_engine_run_or_a_batch_takes_no_place(self):
        # a game played through game() there waits for nothing and holds no game file: it does not yield
        probe = ("import os, subprocess; w = runlock.sim_pool(Path({tree!r}), 'inner', {most}); "
                 "g = w.game().__enter__()([sys.executable, '-c', 'pass']); "
                 "print(w.n, w.fds() == (), runlock.places_held(Path({d!r})), "
                 "os.sched_getscheduler(0) == os.SCHED_IDLE, 'simgame' not in ' '.join(g), "
                 "subprocess.run(g).returncode, flush=True)")
        d = str(self.lock.parent / "runslots")
        # inside an engine run of 4 slots: its workers, no place, its priority
        for most, want in ((16, "4 True 0 False True 0"), (3, "3 True 0 False True 0")):
            outer = self.start(self.wrapper(4, "outer", sys.executable, "-c",
                                            self.boot() + probe.format(tree=str(self.tree), most=most, d=d)))
            self.assertEqual(self.line(outer.stdout), want)
            self.assertEqual(outer.wait(WAIT_S), 0)
        # inside a batch of 6 places, through its environment: its places, no more
        inner = [sys.executable, "-c", self.boot() + probe.format(tree=str(self.tree), most=30, d=d)]
        outer = self.start([sys.executable, "-c", self.boot()
                            + f"import subprocess; w = runlock.sim_pool(Path({str(self.tree)!r}), 'outer', 6); "
                              f"sys.exit(subprocess.run({inner!r}, env=w.environ()).returncode)"])
        self.assertEqual(self.line(outer.stdout), "6 True 6 True True 0")
        self.assertEqual(outer.wait(WAIT_S), 0)

    def test_a_batchs_processes_run_idle_first_victims_and_bound(self):
        probe = ("import os, resource; "
                 "print(os.sched_getscheduler(0) == os.SCHED_IDLE, open('/proc/self/oom_score_adj').read().strip(), "
                 "resource.getrlimit(resource.RLIMIT_DATA)[0] >> 20, flush=True)")
        # run --sim: the command and what it starts
        r = subprocess.run(self.wrapper(0, "hand batch", "bash", "-c", f"{sys.executable} -c \"{probe}\"", sim=3),
                           cwd=self.tree, capture_output=True, text=True, timeout=WAIT_S, env=self.env)
        self.assertEqual(r.stdout.strip(), f"True {runlock.OOM_SCORE_ADJ} {PLACE_MIB}", r.stderr)
        # sim_pool: its process idle and unbound, a game started through argv() idle and bound
        p = self.start([sys.executable, "-c", self.boot()
                        + f"import os, resource, subprocess; w = runlock.sim_pool(Path({str(self.tree)!r}), 'b', 2); "
                          f"{probe}; "
                          f"subprocess.run(w.argv([sys.executable, '-c', {probe!r}]))"])
        unbound = resource.getrlimit(resource.RLIMIT_DATA)[0]
        unbound = -1 if unbound == resource.RLIM_INFINITY else unbound >> 20
        self.assertEqual(self.line(p.stdout), f"True {runlock.OOM_SCORE_ADJ} {unbound}")
        self.assertEqual(self.line(p.stdout), f"True {runlock.OOM_SCORE_ADJ} {PLACE_MIB}")
        self.assertEqual(p.wait(WAIT_S), 0)
        # a game past the bound fails its allocation in its own process
        big = self.start([sys.executable, "-c", self.boot()
                          + f"import subprocess; w = runlock.sim_pool(Path({str(self.tree)!r}), 'b', 1); "
                            f"r = subprocess.run(w.argv([sys.executable, '-c', 'bytearray({2 * PLACE_MIB} << 20)']), "
                            f"capture_output=True, text=True); print('MemoryError' in r.stderr, flush=True)"])
        self.assertEqual(self.line(big.stdout), "True")

    def test_an_engine_run_inside_a_batch_is_refused(self):
        inner = self.wrapper(1, "inner engine", sys.executable, "-c", "print('in')")
        r = subprocess.run(self.wrapper(0, "batch", *inner, sim=2), cwd=self.tree, capture_output=True, text=True,
                           timeout=WAIT_S, env=self.env)
        self.assertEqual(r.returncode, 2)
        self.assertNotIn("in", r.stdout.split())
        self.assertIn("inner engine: this process runs in the idle scheduling class", r.stderr)
        self.assertEqual(self.free(), runlock.BUDGET)

    def test_status_shows_the_pool_its_holders_and_its_waiter(self):
        a = self.holder(16, "engine")
        self.admitted(a)
        b = self.batch(5, what="five")
        self.granted(b, 5)
        c = self.batch(16, least=12, what="twelve")
        self.until(lambda: self.waiting_pool("twelve"), "the batch did not reach the sim queue")
        out = self.status()
        self.assertIn(f"sim pool: 16 places now ({THREADS} threads less the 16 engine slots held, kept within "
                      f"{SIM_FLOOR}..{SIM_CAP}), 5 held, 11 free; 0 games playing, 0 of them on places at or above "
                      f"the size, finishing; 0 places held at or above the size start no game; {SIM_MIB} MiB out of "
                      f"the reserve, {PLACE_MIB} MiB a process; idle priority; the load guard takes the 0 games "
                      f"playing off the load", out)
        self.assertIn(f" 5 places of the sim pool held by pid {b.pid}, a sim batch of 1 to 5 places, ", out)
        self.assertIn("; 0 games playing, 0 yielding", out)
        self.assertIn(f"waiting for the sim pool: pid {c.pid}, a sim batch of 12 to 16 places, ", out)
        self.assertIn(f"runlock: {runlock.BUDGET - 16} of {runlock.BUDGET} slots free", out)
        self.let_go(a)
        self.granted(c, 16)

    def test_a_sim_config_that_cannot_be_read_is_refused_by_name(self):
        for text, said in (("floor = 8\ncap = 24\n", "no pool_mib"),
                           ("floor = 8\ncap = 24\npool_mib = 2048\nplaces = 3\n", "places is not one of"),
                           ("floor = 8\nfloor = 9\ncap = 24\npool_mib = 2048\n", "floor is not one of"),
                           ("floor = 8\ncap = many\npool_mib = 2048\n", "cap = 'many' is not a whole number"),
                           ("floor = 30\ncap = 24\npool_mib = 2048\n", "floor 30 and cap 24"),
                           ("floor = 0\ncap = 24\npool_mib = 2048\n", "floor 0 and cap 24"),
                           ("floor = 8\ncap = 24\npool_mib = 20\n", "pool_mib 20 is less than a MiB")):
            self.sim.write_text(text)
            with self.assertRaises(runlock.BadSim) as e:
                runlock.sim_config()
            self.assertIn(str(self.sim), str(e.exception))
            self.assertIn(said, str(e.exception))
        r = subprocess.run(self.wrapper(0, "refused", "true", sim=2), cwd=self.tree, capture_output=True, text=True,
                           timeout=WAIT_S, env=self.env)
        self.assertEqual(r.returncode, 2)
        self.assertIn(f"the sim pool's figures cannot be read, so no sim batch is admitted: {self.sim}", r.stderr)
        self.assertIn(f"sim pool: {self.sim}", self.status())
        self.sim.unlink()
        self.assertRaises(runlock.BadSim, runlock.sim_config)


class ToolNames(unittest.TestCase):
    """Two runs at once of each tool that names instances, results or plays a network game: what
    each makes for itself carries its run's tag, and its DirectPlay ports are leased."""

    NAMES = ("import json, sys; sys.path[:0] = [{tools!r}, {tools!r} + '/compat', {tools!r} + '/mods']\n"
             "import dpport, modsscreen, runlock, tacompat, wiki_regression\n"
             "leases = [dpport.lease(), dpport.lease()]\n"
             "print(json.dumps({{'names': [tacompat.inst_name('retail'), tacompat.inst_name('retail', '-j'),\n"
             "    tacompat.inst_name('retail+tadr-recorder-ota', '-wh'), str(tacompat.results_dir('wine', 0)),\n"
             "    wiki_regression.gate_instance_name('sp'), modsscreen.instance(), f'mb-{{runlock.tag()}}-H'],\n"
             "    'ports': [x.port for x in leases]}}), flush=True)\n"
             "sys.stdin.read()")

    def setUp(self):
        RunLock.setUp(self)

    wrapper, boot, start, line, weigh = RunLock.wrapper, RunLock.boot, RunLock.start, RunLock.line, RunLock.weigh

    def test_two_runs_take_distinct_names_and_ports(self):
        code = self.NAMES.format(tools=str(TOOLS))
        runs = [self.start(self.wrapper(4, f"tool{k}", sys.executable, "-c", code)) for k in range(2)]
        got = [json.loads(self.line(p.stdout) or "null") for p in runs]
        self.assertTrue(all(got), [p.stderr.read() for p in runs if p.poll() is not None])
        a, b = got
        self.assertFalse(set(a["names"]) & set(b["names"]), got)
        ports = a["ports"] + b["ports"]
        self.assertEqual(len(set(ports)), 4, ports)
        local = __import__("dpport").ephemeral_range()
        self.assertFalse([p for p in ports if p in local], ports)
        self.assertTrue(all(len(n) <= 32 for n in a["names"] if "/" not in n), a["names"])
        for p in runs:
            p.stdin.close()
            p.wait(WAIT_S)

    def test_a_named_port_is_refused_while_leased(self):
        import dpport
        with dpport.lease() as held:
            code = (f"import sys; sys.path.insert(0, {str(TOOLS)!r}); import dpport; "
                    f"dpport.lease({held.port})")
            r = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn(f"port {held.port} is leased by another run", r.stderr)


# Stand-in games. FORKS_OFF writes the pid of a grandchild it forks off -- a double fork into a
# session of its own, as the wineserver makes itself a daemon -- into argv[1] and exits with 3; the
# grandchild lives until the file argv[2] exists. SLEEPS_UNTIL lives until the file argv[1] exists.
FORKS_OFF = ("import os, sys, time\n"
             "if os.fork() == 0:\n"
             "    os.setsid()\n"
             "    if os.fork() == 0:\n"
             "        open(sys.argv[1], 'w').write(str(os.getpid()))\n"
             "        while not os.path.exists(sys.argv[2]):\n"
             "            time.sleep(0.02)\n"
             "    os._exit(0)\n"
             "os.wait()\n"
             "sys.exit(3)\n")
SLEEPS_UNTIL = ("import os, sys, time\n"
                "while not os.path.exists(sys.argv[1]):\n"
                "    time.sleep(0.02)\n")
SLEEPS = "import time; time.sleep(600)"


class Keeper(unittest.TestCase):
    """A game's seat is let go by its keeper once every process of the game has been reaped and the
    prefix's wineserver has ended (tools/runlock.py, A SEAT IS LET GO AFTER ITS GAME), and the
    handle start_game returns answers for the game itself."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tree = Path(tmp.name)
        subprocess.run(["git", "init", "-q", str(self.tree)], check=True)
        lock = runlock.lock_path(self.tree)
        saved = os.environ.get(runlock.ENV)
        self.addCleanup(lambda: os.environ.pop(runlock.ENV, None) if saved is None
                        else os.environ.update({runlock.ENV: saved}))
        os.environ[runlock.ENV] = json.dumps({"slots": 2, "gpu_mib": 1024, "games": 1, "tag": "s00",
                                              "run": "1-1", "cap_mib": 320, "resources": [],
                                              "path": str(lock)})
        self.seat = runlock.seat_file(runlock.slots_dir(lock), "s00", 0)

    def game(self, code, *args, env=None):
        g = runlock.start_game([sys.executable, "-c", code, *map(str, args)], "a stand-in game",
                               env=env, stdin=subprocess.DEVNULL, start_new_session=True)
        self.addCleanup(lambda: g.keeper.poll() is None and g.keeper.kill())
        return g

    def until(self, cond, what):
        deadline = time.monotonic() + WAIT_S
        while not cond():
            if time.monotonic() > deadline:
                self.fail(what)
            time.sleep(0.02)

    def test_the_seat_outlives_every_process_of_the_game(self):
        pidfile, go = self.tree / "grandchild.pid", self.tree / "go"
        self.addCleanup(go.touch)
        g = self.game(FORKS_OFF, pidfile, go)
        self.assertIsInstance(g, runlock.KeptGame)
        # the game's own end is told at once, while a process it started still lives
        self.assertEqual(g.wait(WAIT_S), 3)
        self.until(lambda: pidfile.exists() and pidfile.read_text(), "the grandchild never started")
        gpid = int(pidfile.read_text())
        # its parent has ended: it is the keeper's child now, not init's
        ppid = int(Path(f"/proc/{gpid}/stat").read_text().rsplit(")", 1)[1].split()[1])
        self.assertEqual(ppid, g.pid)
        self.assertTrue(runlock._held_now(self.seat), "the seat went before the game's last process")
        self.assertIsNone(g.keeper.poll())
        # the seat names the keeper, which lives as long as the seat is held, and the run's id
        line = self.seat.read_text()
        self.assertTrue(line.startswith(f"pid {g.pid}, a game's keeper: a stand-in game"), line)
        self.assertIn("[run 1-1]", line)
        self.assertNotIn("STALE", runlock._named(line))
        go.touch()
        self.assertEqual(g.wait_released(WAIT_S), 3)
        self.assertFalse(runlock._held_now(self.seat))

    def test_the_seat_outlives_the_prefixs_wineserver(self):
        bindir, said, go = self.tree / "bin", self.tree / "said", self.tree / "go"
        self.addCleanup(go.touch)
        bindir.mkdir()
        server = bindir / "wineserver"
        server.write_text(f"#!/bin/sh\necho \"$* $WINEPREFIX\" > {said}.tmp && mv {said}.tmp {said}\n"
                          f"while [ ! -e {go} ]; do sleep 0.02; done\n")
        server.chmod(0o755)
        env = dict(os.environ, PATH=f"{bindir}:{os.environ['PATH']}", WINEPREFIX="/some/prefix")
        g = self.game("import sys; sys.exit(0)", env=env)
        self.assertEqual(g.wait(WAIT_S), 0)
        self.until(said.exists, "the keeper never waited for the prefix's wineserver")
        self.assertEqual(said.read_text().split(), ["-w", "/some/prefix"])
        self.assertTrue(runlock._held_now(self.seat), "the seat went before the wineserver")
        go.touch()
        self.assertEqual(g.wait_released(WAIT_S), 0)
        self.assertFalse(runlock._held_now(self.seat))

    def test_the_keeper_is_no_game_of_any_prefix(self):
        g = self.game(SLEEPS, "TotalA.exe", env=dict(os.environ, WINEPREFIX="/some/prefix"))
        keeper_env = Path(f"/proc/{g.pid}/environ").read_bytes().split(b"\0")
        self.assertFalse(any(e.startswith(b"WINEPREFIX=") for e in keeper_env))
        self.assertNotIn(b"totala.exe", Path(f"/proc/{g.pid}/cmdline").read_bytes().lower())
        game_env = Path(f"/proc/{g.game_pid}/environ").read_bytes().split(b"\0")
        self.assertIn(b"WINEPREFIX=/some/prefix", game_env)
        g.kill()
        g.wait_released(WAIT_S)

    def test_kill_and_terminate_reach_the_game_and_the_seat_goes_after(self):
        for end, code in (("kill", -signal.SIGKILL), ("terminate", -signal.SIGTERM)):
            g = self.game(SLEEPS)
            self.assertIsNone(g.poll())
            with self.assertRaises(subprocess.TimeoutExpired):
                g.wait(0.05)
            getattr(g, end)()
            self.assertEqual(g.wait(WAIT_S), code, end)
            g.wait_released(WAIT_S)
            self.assertFalse(runlock._held_now(self.seat), end)

    def test_a_game_that_cannot_start_is_refused_and_its_seat_goes(self):
        with self.assertRaises(OSError) as e:
            runlock.start_game([str(self.tree / "no-such-wine")], "nothing", stdin=subprocess.DEVNULL)
        self.assertIn("its keeper did not start it", str(e.exception))
        self.until(lambda: not runlock._held_now(self.seat), "the seat stayed held")

    def test_outside_a_gpu_run_a_game_is_started_as_it_is(self):
        os.environ.pop(runlock.ENV)
        p = runlock.start_game([sys.executable, "-c", "pass"], "loose")
        self.assertIsInstance(p, subprocess.Popen)
        self.assertEqual(p.wait(WAIT_S), 0)


if __name__ == "__main__":
    unittest.main()
