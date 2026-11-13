#!/usr/bin/env python3
"""Tests of the weighted run lock (tools/runlock.py) on a throwaway repository's lock, with real
processes and real files: the slots held never sum past the budget, admission keeps arrival order,
a killed holder frees its slots, a nested run takes none, and flock(1) on tareplay.lock still
excludes every run. No game runs.

    python3 tools/test_runlock.py

A run here is `runlock.py run` around a stand-in command, with POLL_S at 0.05 s so the run being
admitted retries at once. A step is waited for, never timed: WAIT_S bounds a step that takes
milliseconds, and past it the lock is wrong, not slow.
"""
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
HOLD = "import sys; print('in', flush=True); sys.stdin.read()"


class RunLock(unittest.TestCase):

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tree = Path(tmp.name)
        subprocess.run(["git", "init", "-q", str(self.tree)], check=True)
        self.lock = runlock.lock_path(self.tree)
        self.env = {k: v for k, v in os.environ.items() if k != runlock.ENV}

    def wrapper(self, slots, what, *cmd):
        """The argv of `runlock.py run` around `cmd`, retrying the slots every 0.05 s."""
        boot = (f"import sys; sys.path.insert(0, {str(TOOLS)!r}); import runlock; "
                f"runlock.POLL_S = 0.05; sys.exit(runlock.main(sys.argv[1:]))")
        return [sys.executable, "-c", boot, "run", "--slots", str(slots), "--what", what, "--",
                *cmd]

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

    def holder(self, slots, what="holder"):
        """A run that says `in` once admitted and holds its slots until its stdin closes."""
        return self.start(self.wrapper(slots, what, sys.executable, "-c", HOLD))

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
        self.assertEqual(self.line(outer.stdout), f"4:{self.lock}")
        # every slot is held now: an inner run that took slots of its own would never be admitted
        self.assertEqual(self.line(outer.stdout), "in")
        self.assertEqual(outer.wait(WAIT_S), 0)
        self.assertIn("inner wants 8 slots inside a run that holds 4", outer.stderr.read())

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
        self.assertTrue(any(ln.startswith(f" 3 held by pid {a.pid}, 3 slots, ") and ", three, " in ln
                            and "STALE" not in ln for ln in lines), lines)
        self.assertTrue(any(ln.startswith(f" 2 held by pid {orphan.pid}, 2 slots, ") and
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
        self.assertTrue(runlock.hold(self.tree, "lanes", 3))
        self.assertEqual(os.environ[runlock.ENV], f"{3 * runlock.LANE_SLOTS}:{self.lock}")
        self.assertEqual(len(runlock.fds()), 3 * runlock.LANE_SLOTS + 1)
        self.assertFalse(runlock.hold(self.tree, "again", 1))
        self.assertEqual(self.free(), runlock.BUDGET - 3 * runlock.LANE_SLOTS)
        runlock.release()
        self.assertNotIn(runlock.ENV, os.environ)
        self.assertEqual(runlock.fds(), ())
        self.assertEqual(self.free(), runlock.BUDGET)
        self.assertEqual(runlock.slots_for(None), runlock.BUDGET)
        self.assertEqual(runlock.slots_for(40), runlock.BUDGET)


if __name__ == "__main__":
    unittest.main()
