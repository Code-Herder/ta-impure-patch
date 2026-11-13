#!/usr/bin/env python3
"""Tests of the weighted run lock (tools/runlock.py) on a throwaway repository's lock, with real
processes and real files: the slots and the GPU tokens held never sum past their budgets, each
queue keeps arrival order, a run that wants no token is admitted while a GPU run waits for its
tokens, a killed holder or waiter frees its slots and tokens, no run deadlocks beside a stand-in of
runlock code that gathers its tokens in the slot queue, a nested run takes none, flock(1) on
tareplay.lock still excludes every run, resources exclude by name, and two runs of each tool that
names instances or plays a network game at once take distinct names and DirectPlay ports
(tools/dpport.py's leases, which are the machine's own, in /tmp). No game runs.

    python3 tools/test_runlock.py

A run here is `runlock.py run` around a stand-in command, with POLL_S at 0.05 s so the run being
admitted retries at once, and the device's memory given rather than read: none (no GPU budget) for
the tests of the slots, the reference setup's 12 282 MiB -- 9 tokens with tools/runlock-vram.cfg --
for the tests of the tokens. A step is waited for, never timed: WAIT_S bounds a step that takes
milliseconds, and past it the lock is wrong, not slow.
"""
import json
import os
import select
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import runlock  # noqa: E402

WAIT_S = 20
REFERENCE_MIB = 12282           # the reference setup's GPU
TOKENS = 9                      # its tokens: (12 282 - 3072) // (920 + 64 + 32) (tools/runlock-vram.cfg)
HOLD = "import sys; print('in', flush=True); sys.stdin.read()"


class RunLock(unittest.TestCase):

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tree = Path(tmp.name)
        subprocess.run(["git", "init", "-q", str(self.tree)], check=True)
        self.lock = runlock.lock_path(self.tree)
        self.env = {k: v for k, v in os.environ.items() if k != runlock.ENV}

    def wrapper(self, slots, what, *cmd, resources=(), vram=None, total=None, tree_cfg=None):
        """The argv of `runlock.py run` around `cmd`, retrying the slots every 0.05 s, on a device of
        `total` MiB (None: no GPU budget), from a tree whose runlock-vram.cfg is `tree_cfg` (None:
        this one's)."""
        boot = (f"import sys; sys.path.insert(0, {str(TOOLS)!r}); import runlock; "
                f"runlock.POLL_S = 0.05; runlock.device_total_mib = lambda: {total!r}; "
                + (f"from pathlib import Path; runlock.VRAM_CFG = Path({str(tree_cfg)!r}); "
                   if tree_cfg else "")
                + "sys.exit(runlock.main(sys.argv[1:]))")
        named = [a for r in resources for a in ("--resource", r)]
        tokens = ["--vram", str(vram)] if vram is not None else []
        return [sys.executable, "-c", boot, "run", "--slots", str(slots), *tokens, "--what", what,
                *named, "--", *cmd]

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

    def holder(self, slots, what="holder", resources=(), vram=None, total=None, tree_cfg=None):
        """A run that says `in` once admitted and holds its slots until its stdin closes."""
        return self.start(self.wrapper(slots, what, sys.executable, "-c", HOLD, resources=resources,
                                       vram=vram, total=total, tree_cfg=tree_cfg))

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

    def tokens(self, pid=None):
        """The GPU token files held, or those whose line names `pid`."""
        d = self.lock.parent / "runslots"
        if pid is None:
            return sum(runlock._held_now(p) for p in d.glob("vram-*"))
        return sum(p.read_text().startswith(f"pid {pid},") for p in d.glob("vram-*"))

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

    def waiting_tokens(self, what):
        return any(ln.startswith("waiting for GPU tokens:") and f", {what}, " in ln
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
        self.assertEqual((held["slots"], held["tokens"], held["games"], held["path"]),
                         (3 * runlock.LANE_SLOTS, 3, 3, str(self.lock)))
        self.assertEqual((runlock.tag(), runlock.games(), self.tokens()), ("s00", 3, 3))
        self.assertEqual(len(runlock.fds()), 3 * runlock.LANE_SLOTS + 3 + 1)
        self.assertFalse(runlock.hold(self.tree, "again", 1))
        self.assertEqual(self.free(), runlock.BUDGET - 3 * runlock.LANE_SLOTS)
        runlock.release()
        self.assertNotIn(runlock.ENV, os.environ)
        self.assertEqual(runlock.fds(), ())
        self.assertEqual((self.free(), self.tokens()), (runlock.BUDGET, 0))

    def test_the_plans_of_a_run(self):
        device = runlock.device_total_mib
        self.addCleanup(setattr, runlock, "device_total_mib", device)
        runlock.device_total_mib = lambda: REFERENCE_MIB
        lk = self.lock
        self.assertEqual(runlock.vram_tokens(lk), TOKENS)
        # -j 12 on the reference setup: 9 games, their slots and their tokens
        self.assertEqual(runlock.plan(12, path=lk), (2 * TOKENS, TOKENS, TOKENS, TOKENS))
        self.assertEqual(runlock.plan(None, path=lk), (runlock.BUDGET, TOKENS, TOKENS, TOKENS))
        self.assertEqual(runlock.plan(3, gpu=False, path=lk), (6, 0, 3, None))
        runlock.device_total_mib = lambda: 2048
        self.assertEqual(runlock.vram_tokens(lk), runlock.MIN_TOKENS)
        runlock.device_total_mib = lambda: None
        self.assertEqual(runlock.plan(12, path=lk), (24, 0, 12, None))
        self.assertEqual(runlock.plan(40, path=lk), (runlock.BUDGET, 0, runlock.BUDGET // runlock.LANE_SLOTS,
                                                     None))

    def test_a_token_is_the_capped_game_and_its_margins(self):
        # the shipped figures: B = 920 + 64, a token 1016, 9 on the reference setup
        cfg = runlock.vram_config(self.lock)
        self.assertEqual(cfg, runlock.read_vram_cfg(runlock.VRAM_CFG))       # seeded from the tree's
        self.assertEqual((runlock.cap_mib(cfg), runlock.game_bound_mib(cfg), runlock.game_mib(cfg)),
                         (cfg["cap_mib"], cfg["cap_mib"] + cfg["driver_mib"],
                          cfg["cap_mib"] + cfg["driver_mib"] + cfg["margin_mib"]))
        self.assertEqual((REFERENCE_MIB - cfg["reserve_mib"]) // runlock.game_mib(cfg), TOKENS)
        # a token holds a game at its bound: the tokens never promise past the device less the reserve
        self.assertLessEqual(TOKENS * runlock.game_mib(cfg), REFERENCE_MIB - cfg["reserve_mib"])
        self.assertGreaterEqual(runlock.game_mib(cfg), runlock.game_bound_mib(cfg))
        # the caps the run lock accepts are the caps the DLL takes: any other it runs uncapped
        header = (TOOLS.parent / "tagpu/ddraw/inc/tagpu_vkcap.h").read_text()
        self.assertIn(f"#define TAGPU_VKCAP_MIN_MIB  {runlock.CAP_MIN_MIB}\n", header)
        self.assertEqual(runlock.CAP_MAX_MIB, 1 << 20)
        self.assertIn("#define TAGPU_VKCAP_MAX_MIB  (1L << 20)\n", header)

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

    def test_the_held_tokens_never_sum_past_their_budget(self):
        log = self.tree / "tokens.log"
        probe = ("import os, pathlib, sys, time; d = pathlib.Path(sys.argv[1]); "
                 "me = f'pid {os.getpid()},'; "
                 "n = sum(p.read_text().startswith(me) for p in d.glob('vram-*')); "
                 "log = open(sys.argv[2], 'a'); log.write(f'+ {os.getpid()} {sys.argv[3]} {n}\\n'); "
                 "log.flush(); time.sleep(0.1); log.write(f'- {os.getpid()} {sys.argv[3]}\\n')")
        wanted = [3, 8, 1, 5, 2, 12, 4, 7, 1, 6, 2, 3]
        runs = [self.start(self.wrapper(1, f"v{k}", sys.executable, "-c", probe,
                                        str(self.lock.parent / "runslots"), str(log),
                                        str(min(w, TOKENS)), vram=w, total=REFERENCE_MIB))
                for k, w in enumerate(wanted)]
        for p in runs:
            self.assertEqual(p.wait(WAIT_S * 3), 0, p.stderr.read())
        held, seen = 0, 0
        for ln in log.read_text().splitlines():
            sign, _, w, *n = ln.split()
            if sign == "+":
                self.assertEqual(int(n[0]), int(w), f"a run held other than its tokens: {ln}")
                held += int(w)
                seen += 1
            else:
                held -= int(w)
            self.assertLessEqual(held, TOKENS, "the GPU tokens held passed their budget")
        self.assertEqual((seen, held), (len(wanted), 0))

    def test_a_run_without_tokens_is_admitted_while_a_gpu_run_waits_for_them(self):
        a = self.holder(1, "a", vram=6, total=REFERENCE_MIB)
        self.admitted(a)
        big = self.holder(2, "big", vram=TOKENS, total=REFERENCE_MIB)
        # big holds the token queue, and neither a token (all at once or none) nor a slot meanwhile
        self.until(lambda: self.waiting_tokens("big"), "big did not reach the token queue")
        self.assertEqual((self.tokens(big.pid), self.tokens(), self.free()), (0, 6, runlock.BUDGET - 1))
        # a GDI run and a wide one want no token: neither waits for big
        gdi = self.holder(1, "gdi", vram=0, total=REFERENCE_MIB)
        wide = self.holder(20, "wide", vram=0, total=REFERENCE_MIB)
        self.admitted(gdi)
        self.admitted(wide)
        # a GPU run behind big waits behind it for its tokens, however few it wants
        small = self.holder(1, "small", vram=1, total=REFERENCE_MIB)
        self.assertIn("waiting for the run lock", self.line(small.stderr) or "")
        self.quiet(small)
        self.let_go(a)
        self.admitted(big)
        self.assertEqual(self.tokens(big.pid), TOKENS)
        self.quiet(small)
        self.let_go(big)
        self.admitted(small)
        self.assertEqual(self.tokens(small.pid), 1)

    def test_a_gpu_run_waiting_for_slots_holds_up_no_other_runs_tokens(self):
        wall = self.holder(runlock.BUDGET - 1, "wall", vram=0, total=REFERENCE_MIB)
        self.admitted(wall)
        g1 = self.holder(2, "g1", vram=3, total=REFERENCE_MIB)
        # g1 has its tokens and waits in the slot queue for a second slot
        self.until(lambda: self.admitting("g1") and self.tokens(g1.pid) == 3,
                   "g1 did not reach the slot queue with its tokens")
        self.quiet(g1)
        # g2 takes its tokens while g1 waits for slots, then queues for slots behind g1
        g2 = self.holder(2, "g2", vram=4, total=REFERENCE_MIB)
        self.until(lambda: self.tokens(g2.pid) == 4 and not self.waiting_tokens("g2"),
                   "g2 waited for tokens behind a run that waits for slots")
        self.quiet(g2)
        self.assertEqual(self.tokens(), 7)
        self.let_go(wall)
        self.admitted(g1)
        self.admitted(g2)
        self.assertEqual((self.tokens(g1.pid), self.tokens(g2.pid)), (3, 4))
        self.assertEqual(self.free(), runlock.BUDGET - 4)

    def test_a_killed_holder_frees_its_tokens(self):
        a = self.holder(1, "a", vram=TOKENS, total=REFERENCE_MIB)
        self.admitted(a)
        b = self.holder(1, "b", vram=3, total=REFERENCE_MIB)
        self.until(lambda: self.waiting_tokens("b"), "b did not reach the token queue")
        self.quiet(b)
        a.send_signal(signal.SIGKILL)
        a.wait(WAIT_S)
        self.admitted(b)
        self.assertEqual(self.tokens(), 3)
        self.assertIn("GPU tokens: 3 held", runlock.report(self.lock)[1])

    def test_a_killed_waiter_frees_the_tokens_it_waited_with(self):
        a = self.holder(1, "a", vram=TOKENS - 3, total=REFERENCE_MIB)
        self.admitted(a)
        big = self.holder(1, "big", vram=TOKENS, total=REFERENCE_MIB)
        self.until(lambda: self.waiting_tokens("big"), "big did not reach the token queue")
        nxt = self.holder(1, "next", vram=3, total=REFERENCE_MIB)
        self.quiet(nxt)
        big.send_signal(signal.SIGKILL)             # in the token queue, holding no token
        big.wait(WAIT_S)
        self.admitted(nxt)
        self.assertEqual(self.tokens(), TOKENS)
        # killed in the slot queue, holding its tokens: they go with it
        wall = self.holder(runlock.BUDGET - 2, "wall", vram=0, total=REFERENCE_MIB)
        self.admitted(wall)
        stuck = self.holder(1, "stuck", vram=0, total=REFERENCE_MIB)
        self.let_go(a)
        self.until(lambda: self.free() == 0 and self.tokens() == 3, "a's slot and tokens were not freed")
        self.admitted(stuck)
        g = self.holder(2, "g", vram=TOKENS - 3, total=REFERENCE_MIB)
        self.until(lambda: self.admitting("g") and self.tokens(g.pid) == TOKENS - 3,
                   "g did not reach the slot queue with its tokens")
        g.send_signal(signal.SIGKILL)
        g.wait(WAIT_S)
        self.until(lambda: self.tokens() == 3 and not self.admitting("g"), "g's tokens were not freed")

    def test_every_token_figure_counts_the_same_files(self):
        a = self.holder(1, "a", vram=TOKENS - 3, total=REFERENCE_MIB)
        self.admitted(a)
        c = self.holder(1, "c", vram=2, total=REFERENCE_MIB)
        self.admitted(c)
        b = self.holder(2, "b", vram=5, total=REFERENCE_MIB)     # one free of the five: it waits with none
        self.until(lambda: self.waiting_tokens("b"), "b did not reach the token queue")
        _free, lines = runlock.report(self.lock, TOKENS)
        self.assertIn(f"GPU tokens: {TOKENS - 1} held of {TOKENS}, 1 free", lines)
        mine = {p.pid: [ln for ln in lines if f"GPU tokens held by pid {p.pid}," in ln] for p in (a, b, c)}
        self.assertEqual((len(mine[a.pid]), len(mine[b.pid]), len(mine[c.pid])), (1, 0, 1), lines)
        self.assertTrue(mine[a.pid][0].startswith(f"{TOKENS - 3:2} GPU tokens held by pid {a.pid}, "
                                                  f"a run of 1 slot and {TOKENS - 3} GPU tokens"), lines)
        self.assertTrue(mine[c.pid][0].startswith(f" 2 GPU tokens held by pid {c.pid}, "
                                                  f"a run of 1 slot and 2 GPU tokens"), lines)
        # the summary is the sum of the holders' counts, all from one read
        per = sum(int(ln.split()[0]) for ln in lines if " GPU tokens held by pid " in ln)
        self.assertEqual(per, TOKENS - 1)
        # `status`, on the same device, prints the same lines and no figure of its own
        boot = (f"import sys; sys.path.insert(0, {str(TOOLS)!r}); import runlock; "
                f"runlock.device_total_mib = lambda: {REFERENCE_MIB}; sys.exit(runlock.main(['status']))")
        out = subprocess.run([sys.executable, "-c", boot], cwd=self.tree, capture_output=True, text=True,
                             env=self.env).stdout
        self.assertIn(f"  GPU tokens: {TOKENS - 1} held of {TOKENS}, 1 free\n", out)
        self.assertEqual(sum(int(ln.split()[0]) for ln in out.splitlines()
                             if " GPU tokens held by pid " in ln), TOKENS - 1)
        self.assertNotIn("GPU tokens,", out.splitlines()[0])
        self.let_go(a)
        self.admitted(b)
        self.assertIn(f"GPU tokens: 7 held of {TOKENS}, {TOKENS - 7} free", runlock.report(self.lock, TOKENS)[1])

    def old_cfg(self):
        """An older tree's runlock-vram.cfg: a cap of 768, 11 tokens on the reference setup."""
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
        # the lock's budget: 9 tokens, 1 of them a's, so the old tree's run, wanting 11 cut to the
        # lock's 9, waits with none
        self.until(lambda: self.waiting_tokens("old tree"), "the old tree's run did not wait for tokens")
        self.assertEqual(self.tokens(old.pid), 0)
        self.quiet(old)
        self.let_go(a)
        self.admitted(old)
        self.assertEqual(self.tokens(old.pid), TOKENS)
        self.assertEqual(shared.read_text(), runlock.VRAM_CFG.read_text())  # never written again

    def test_a_token_past_the_budget_holds_up_every_run_that_wants_one(self):
        # the old 11-token layout: a run of older runlock code holds vram-09 and vram-10, which this
        # lock's budget of 9 never hands out
        d = runlock.slots_dir(self.lock)
        hold_old = ("import fcntl, os, sys; fs = []\n"
                    "for k in (9, 10):\n"
                    "    f = open(os.path.join(sys.argv[1], f'vram-{k:02d}'), 'a+')\n"
                    "    fcntl.flock(f, fcntl.LOCK_EX); f.seek(0); f.truncate()\n"
                    "    f.write(f'pid {os.getpid()}, 2 slots, 2 GPU tokens, since then, old, from there\\n')\n"
                    "    f.flush(); fs.append(f)\n"
                    "print('in', flush=True); sys.stdin.read()")
        old = self.start([sys.executable, "-c", hold_old, str(d)])
        self.admitted(old)
        gdi = self.holder(1, "gdi", vram=0, total=REFERENCE_MIB)
        self.admitted(gdi)                                  # wanting no token, it does not wait
        new = self.holder(1, "new", vram=1, total=REFERENCE_MIB)
        said = self.stderr_until(new, "held under another budget")
        self.assertTrue(any("vram-09, vram-10" in ln for ln in said), said)
        self.quiet(new)
        self.assertEqual(self.tokens(new.pid), 0)           # it takes none beside them
        _free, lines = runlock.report(self.lock, TOKENS)
        self.assertIn(f"GPU tokens: 0 held of {TOKENS}, {TOKENS} free; 2 more held under another budget, "
                      f"so no run takes a token until they end", lines)
        self.assertTrue(any(ln.startswith(f" 2 GPU tokens held under another budget by pid {old.pid},")
                            for ln in lines), lines)
        old.send_signal(signal.SIGKILL)
        old.wait(WAIT_S)
        self.admitted(new)
        self.assertEqual(self.tokens(new.pid), 1)

    def test_a_game_is_capped_by_the_locks_figures(self):
        shared = runlock.shared_vram_cfg(self.lock)
        shared.write_text("reserve_mib = 3072\ncap_mib = 512\ndriver_mib = 32\nmargin_mib = 0\n")
        probe = "import runlock; print(runlock.held_cap_mib(), runlock.games(), flush=True)"
        env = dict(self.env, PYTHONPATH=str(TOOLS))
        # a token of 544: (12 282 - 3072) // 544 = 16 games, each run's cap 512, whatever its tree says
        for tree_cfg in (None, self.old_cfg()):
            p = self.start(self.wrapper(1, "capped", sys.executable, "-c", probe, vram=12,
                                        total=REFERENCE_MIB, tree_cfg=tree_cfg), env=env)
            self.assertEqual(self.line(p.stdout), "512 12")
            p.wait(WAIT_S)
        # a run that wants no token publishes no cap: its games are not capped
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

    def test_the_figures_are_read_once_the_token_queue_is_held(self):
        # a GPU run waiting for the token queue is admitted under the figures of the moment it takes
        # that queue, and publishes that cap, not the one of the moment it arrived
        probe = "import runlock; print(runlock.held_cap_mib(), runlock.games(), flush=True)"
        env = dict(self.env, PYTHONPATH=str(TOOLS))
        a = self.holder(1, "a", vram=TOKENS, total=REFERENCE_MIB)
        self.admitted(a)
        b = self.holder(1, "b", vram=TOKENS, total=REFERENCE_MIB)
        self.until(lambda: self.waiting_tokens("b"), "b did not reach the token queue")
        p = self.start(self.wrapper(1, "late", sys.executable, "-c", probe, vram=2, total=REFERENCE_MIB),
                       env=env)
        self.quiet(p)
        # a token of 544: 16 of them, b's 9 of the 920 figures among them
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
        self.admitted(gdi)                                      # wants no token: reads no figure
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
        """A run of runlock code of before the token queue, on the same figures (a budget of TOKENS):
        it takes the slot queue, writes its line there in that code's form, and gathers slots and
        tokens in it, keeping each it takes, until it has them all; then it says `in`, lets the queue
        go and holds them until its stdin closes. With `go` it takes no token until a line on its
        stdin, which orders a test's steps without a race."""
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
                           str(tokens), "go" if go else "now", what, str(TOKENS)])

    def test_no_deadlock_beside_old_code_gathering_tokens_in_the_slot_queue(self):
        # a holds 5 tokens; n, of this code, wants all 9 and waits in the token queue with none; o,
        # of old code, takes the slot queue and gathers its 7 tokens there, keeping the 4 free ones
        a = self.holder(1, "a", vram=5, total=REFERENCE_MIB)
        self.admitted(a)
        n = self.holder(1, "n", vram=TOKENS, total=REFERENCE_MIB)
        self.until(lambda: self.waiting_tokens("n"), "n did not reach the token queue")
        o = self.old_run(1, 7)
        self.until(lambda: self.tokens(o.pid) == 4, "the old run did not gather the free tokens")
        self.assertEqual(self.tokens(n.pid), 0)             # with a partial set n and o would wait on each other
        gdi = self.holder(1, "gdi", vram=0, total=REFERENCE_MIB)
        self.quiet(gdi)                                     # the old run holds the slot queue
        self.let_go(a)
        self.admitted(o)                                    # o takes 3 of a's 5; n still wants 9
        self.admitted(gdi)
        self.quiet(n)
        self.let_go(o)
        self.admitted(n)
        self.assertEqual(self.tokens(n.pid), TOKENS)

    def test_tokens_waiting_for_the_slot_queue_are_let_go_to_an_old_run_gathering(self):
        # n has all its tokens and waits for the slot queue, which an old run holds while it wants
        # tokens n has: n lets them go, the old run is admitted, then n takes them again
        a = self.holder(1, "a", vram=TOKENS, total=REFERENCE_MIB)
        self.admitted(a)
        o = self.old_run(1, 2, go=True)
        self.until(lambda: self.admitting("old"), "the old run did not take the slot queue")
        n = self.holder(1, "n", vram=TOKENS, total=REFERENCE_MIB)
        self.until(lambda: self.waiting_tokens("n"), "n did not reach the token queue")
        self.let_go(a)
        self.stderr_until(n, f"lets its {TOKENS} go")      # n took all 9 and waits behind the old run
        self.assertEqual(self.tokens(), 0)
        o.stdin.write("go\n")
        o.stdin.flush()
        self.admitted(o)
        self.assertEqual(self.tokens(o.pid), 2)
        self.quiet(n)                                       # 7 free: n waits for o's 2
        self.let_go(o)
        self.admitted(n)
        self.assertEqual(self.tokens(n.pid), TOKENS)

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
                "try:\n    print(runlock.hold(tree, 'nested', 1, [sys.argv[1]]), runlock.fds() != (), flush=True)\n"
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

    wrapper, start, line = RunLock.wrapper, RunLock.start, RunLock.line

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


if __name__ == "__main__":
    unittest.main()
