#!/usr/bin/env python3
"""Tests of wiki_regression.py's gate schedule (gate_schedule) and the run lock's early release (runlock.release,
runlock.fds): the offline lanes start while another run holds the lock and the game lanes wait for it; the game
lanes' processes hold it past the gate's own release; an exception ends the lanes and then releases it. Every test
locks a throwaway repository's run lock, never the machine's, and its lanes are stand-ins: no game runs.

    python3 tools/test_wiki_regression.py
"""
import concurrent.futures
import json
import io
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import runlock  # noqa: E402
import wiki_regression  # noqa: E402

WAIT_S = 20                     # a bound on a step that takes milliseconds: past it the schedule is wrong, not slow


class GateSchedule(unittest.TestCase):
    """Clean-ups run last registered first: a holder lets go, a schedule's thread is joined, the pools end, this
    process's hold is released and the repository removed -- so a failing test leaves no lock and no thread."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.tree = Path(self.tmp.name)
        subprocess.run(["git", "init", "-q", str(self.tree)], check=True)
        self.lock = runlock.lock_path(self.tree)
        env = os.environ.get(runlock.ENV)
        self.addCleanup(lambda: env is not None and os.environ.update({runlock.ENV: env}))
        self.addCleanup(runlock.release)
        self.checks = concurrent.futures.ThreadPoolExecutor(max_workers=2)
        self.games = concurrent.futures.ThreadPoolExecutor(max_workers=2)
        self.addCleanup(self.checks.shutdown, wait=True)
        self.addCleanup(self.games.shutdown, wait=True)
        self.out, self.err = io.StringIO(), io.StringIO()
        self.holders = []
        for name, value in (("POLL_S", 0.05), ("device_total_mib", lambda: None)):    # no GPU budget
            patcher = mock.patch.object(runlock, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def holder(self):
        """Another run holding the lock until its stdin closes."""
        code = ("import fcntl, sys; f = open(sys.argv[1], 'a+'); fcntl.flock(f, fcntl.LOCK_EX); "
                "print('held', flush=True); sys.stdin.read()")
        p = subprocess.Popen([sys.executable, "-c", code, str(self.lock)], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, text=True)
        self.addCleanup(p.wait, WAIT_S)
        self.addCleanup(p.stdout.close)
        self.addCleanup(p.stdin.close)
        self.holders.append(p)
        self.assertEqual(p.stdout.readline().strip(), "held")
        return p

    def started(self, fn):
        """`fn` on a thread of its own, joined before the pools end, every holder let go first."""
        t = threading.Thread(target=fn)
        t.start()

        def join():
            for p in self.holders:
                p.stdin.close()
            t.join(WAIT_S)
        self.addCleanup(join)
        return t

    def free(self):
        """Whether another process could take the lock now."""
        code = "import fcntl, sys; fcntl.flock(open(sys.argv[1], 'a+'), fcntl.LOCK_EX | fcntl.LOCK_NB)"
        return subprocess.run([sys.executable, "-c", code, str(self.lock)], capture_output=True).returncode == 0

    def schedule(self, offline, games, cleanup=lambda: None):
        with redirect_stdout(self.out), redirect_stderr(self.err):
            return wiki_regression.gate_schedule(offline, games, self.checks, self.games, "test gate", cleanup,
                                                 tree=self.tree)

    def test_the_offline_lanes_run_while_another_run_holds_the_lock(self):
        other = self.holder()
        offline_ran, games_made = threading.Event(), threading.Event()
        seen = {}

        def offline():
            offline_ran.set()
            return ("offline", "a", 0, "")

        def game():
            seen["env"] = os.environ.get(runlock.ENV)
            seen["free"] = self.free()
            return ("game", "g", 0, "")

        def games(fds):
            seen["fds"] = fds
            games_made.set()
            return [game]

        result = {}
        t = self.started(lambda: result.setdefault("lanes", self.schedule([offline], games)))
        self.assertTrue(offline_ran.wait(WAIT_S), "the offline lane did not start while the lock was held")
        # the game lanes cannot be made while the other run holds the lock: waited on, not timed
        self.assertFalse(games_made.is_set())
        self.assertFalse(self.free())
        other.stdin.close()
        other.wait(WAIT_S)
        t.join(WAIT_S)
        self.assertFalse(t.is_alive())
        self.assertEqual(result["lanes"], [("offline", "a", 0, ""), ("game", "g", 0, "")])
        self.assertTrue(seen["fds"])
        held = json.loads(seen["env"])
        self.assertEqual((held["slots"], held["path"]), (runlock.BUDGET, str(self.lock)))
        self.assertFalse(seen["free"], "a game lane ran without the lock")
        self.assertTrue(self.free(), "the lock was kept after the last game lane")
        self.assertEqual(runlock.fds(), ())
        self.assertNotIn(str(self.lock), os.environ.get(runlock.ENV, ""))
        log = self.out.getvalue()
        self.assertIn("the game lanes wait for the run lock", log)
        self.assertIn("the run lock taken after", log)
        self.assertIn("the run lock released", log)
        self.assertIn("waiting for the run lock", self.err.getvalue())

    def test_the_lock_is_released_with_offline_lanes_still_running(self):
        go = threading.Event()

        def offline():
            go.wait(WAIT_S)
            return ("offline", "slow", 0, "")

        result = {}
        t = self.started(lambda: result.setdefault("lanes", self.schedule(
            [offline], lambda fds: [lambda: ("game", "g", 0, "")])))
        deadline = time.monotonic() + WAIT_S
        while "1 offline lanes still running" not in self.out.getvalue() and time.monotonic() < deadline:
            time.sleep(0.02)
        try:
            self.assertTrue(self.free(), "the lock was held for an offline lane")
        finally:
            go.set()
        t.join(WAIT_S)
        self.assertEqual(result["lanes"], [("offline", "slow", 0, ""), ("game", "g", 0, "")])

    def test_a_game_lanes_process_holds_the_lock_past_the_gates_release(self):
        kids = []

        def games(fds):
            def lane():
                kid = subprocess.Popen([sys.executable, "-c", "import sys; sys.stdin.read()"], stdin=subprocess.PIPE,
                                       pass_fds=fds)
                self.addCleanup(kid.wait, WAIT_S)
                self.addCleanup(kid.stdin.close)
                kids.append(kid)
                return ("game", "g", 0, "")
            return [lane]

        self.schedule([], games)
        self.assertEqual(runlock.fds(), ())
        self.assertFalse(self.free(), "the lock went while a game lane's process still ran")
        kids[0].stdin.close()
        kids[0].wait(WAIT_S)
        self.assertTrue(self.free())

    def test_no_game_lane_takes_no_lock(self):
        self.holder()
        self.assertEqual(self.schedule([lambda: ("offline", "a", 0, "")], None), [("offline", "a", 0, "")])
        self.assertNotIn("run lock", self.out.getvalue())

    def test_an_exception_ends_the_lanes_before_the_lock_goes(self):
        seen = {}

        def cleanup():
            seen["free during clean-up"] = self.free()

        def games(fds):
            raise RuntimeError("an instance could not be made")

        with self.assertRaisesRegex(RuntimeError, "could not be made"):
            self.schedule([lambda: ("offline", "a", 0, "")], games, cleanup)
        self.assertEqual(seen, {"free during clean-up": False})
        self.assertTrue(self.free(), "the lock outlived the gate's exception")
        self.assertEqual(runlock.fds(), ())

    def test_an_interrupt_in_a_game_lane_ends_the_lanes_and_releases(self):
        ended = []

        def game():
            raise KeyboardInterrupt

        with self.assertRaises(KeyboardInterrupt):
            self.schedule([], lambda fds: [game], lambda: ended.append(True))
        self.assertEqual(ended, [True])
        self.assertTrue(self.free())


class WaitLine(unittest.TestCase):
    """wait_line answers the first line after its mark that matches, so a look every 0.3 s and a look every LOOK_S
    take the same line; the game writes on a fake clock that each look's sleep advances."""

    class Log:
        def __init__(self, script):
            self.log, self.todo, self.clock = ["wiki: shot 4 asked: an earlier one"], list(script), [0.0]

        def mark(self):
            while self.todo and self.todo[0][0] <= self.clock[0] + 1e-9:
                self.log.append(self.todo.pop(0)[1])
            return len(self.log)

    def wait(self, look_s, script, rx, seconds=30):
        c = self.Log(script)
        start = c.mark()

        def sleep(s):
            c.clock[0] += s

        with mock.patch.object(wiki_regression, "LOOK_S", look_s), \
                mock.patch.object(wiki_regression.time, "sleep", sleep), \
                mock.patch.object(wiki_regression.time, "monotonic", lambda: c.clock[0]):
            try:
                got = wiki_regression.wait_line(c, start, rx, "the shot")
            except AssertionError as e:
                got = str(e)
        return got, round(c.clock[0], 6)

    def test_the_first_line_after_the_mark_at_either_look(self):
        script = [(0.1, "wiki3d: stage quickbuild.base: the ground in 3D - 4 tiles"),
                  (0.2, "units: alive=0"),
                  (0.25, "wiki3d: stage buildshapes.flat: the ground in 3D - 9 tiles")]
        rx = r"wiki3d: stage \S+: the ground"
        slow, fast = self.wait(0.3, script, rx), self.wait(wiki_regression.LOOK_S, script, rx)
        self.assertEqual(slow[0], fast[0])
        self.assertEqual(fast[0], script[0][1])
        self.assertLess(fast[1], slow[1])

    def test_a_line_before_the_mark_is_not_the_answer(self):
        got, _ = self.wait(wiki_regression.LOOK_S, [(1.0, "wiki: shot 5 asked: quickbuild 1 30")],
                           r"wiki: shot \d+ asked")
        self.assertEqual(got, "wiki: shot 5 asked: quickbuild 1 30")

    def test_no_line_fails_at_the_bound(self):
        got, at = self.wait(wiki_regression.LOOK_S, [], r"never")
        self.assertIn("the shot: no line matching `never` in 30 s", got)
        self.assertGreaterEqual(at, 30 - 1e-6)


if __name__ == "__main__":
    unittest.main()
