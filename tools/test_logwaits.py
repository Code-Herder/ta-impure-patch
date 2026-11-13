"""The harness waits outside tareplay and tacli that poll a game's log, looking every 20 ms:
tacompat's, which read only what was appended, and resource_regression's lockstep, whose GameTime
peek only bounds the wait. Each sees a line written after many looks, across a rotation and through
a line the sink is still writing, and concludes at its old look what it concludes at the new one.
The game writes on a fake clock that each look's sleep advances."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parent / 'compat'))
import tacompat
import resource_regression

HEAD = b"log: run R%d part %d of tagpu.log, started now\n"


class LockstepTaken(unittest.TestCase):
    """Check._taken: the pause's line is looked for every LOOK_S, GameTime is peeked every
    PROGRESS_S, and a level whose GameTime stands still for STALL_S fails at a peek."""

    def run_taken(self, look_s, line_at, ticks):
        """_taken(300) on a fake clock: the pause's line logged at `line_at` (None: never), each
        peek answering ticks(t). (the line taken or the failure, the fake time, the peeks)."""
        rr = resource_regression
        c = object.__new__(rr.Check)
        clock, peeks, written = [0.0], [0], [False]

        def reader():
            if line_at is not None and not written[0] and clock[0] >= line_at:
                written[0] = True
                return ["units: alive=3", "pause: paused at GameTime 300"]
            return []

        def gametime():
            peeks[0] += 1
            return ticks(clock[0])

        def sleep(s):
            clock[0] += s

        c.reader, c.log = reader, []
        c._gametime = gametime
        with patch.object(rr, "LOOK_S", look_s), patch.object(rr.time, "sleep", sleep), \
                patch.object(rr.time, "monotonic", lambda: clock[0]):
            try:
                c._taken(300, 0)
                got = c.tick
            except AssertionError as e:
                got = str(e)
        return got, round(clock[0], 6), peeks[0]

    def test_the_line_is_seen_within_a_look_and_peeks_are_few(self):
        slow = self.run_taken(0.05, 12.0, lambda t: int(t * 30))
        fast = self.run_taken(resource_regression.LOOK_S, 12.0, lambda t: int(t * 30))
        self.assertEqual(slow[0], fast[0])
        self.assertEqual(fast[0], 300)
        self.assertAlmostEqual(fast[1], 12.0, delta=resource_regression.LOOK_S + 1e-6)
        self.assertEqual(fast[2], 3)                      # at 0, 5 and 10 s

    def test_a_level_standing_still_fails_at_the_bound(self):
        for look_s in (0.05, resource_regression.LOOK_S):
            got, at, _ = self.run_taken(look_s, None, lambda t: 40)
            self.assertIn("GameTime stood at 40", got)
            self.assertGreaterEqual(at, resource_regression.STALL_S)
            self.assertLess(at, resource_regression.STALL_S + resource_regression.PROGRESS_S + 1)

    def test_an_unanswered_peek_is_no_progress(self):
        got, _, _ = self.run_taken(resource_regression.LOOK_S, None,
                                   lambda t: 40 if t < 1 else None)
        self.assertIn("GameTime stood at 40", got)


class CapRefusals(unittest.TestCase):
    """A refusal of the GPU memory cap fails the game in the harnesses here: in every line a lockstep
    Check reads, and in a compatibility run whatever its setup expects."""

    REFUSAL = "vk: gpumem: the cap refuses an allocation of 4194304 bytes - 766 MiB held of 768 MiB"

    def test_a_check_fails_at_the_refusal_it_reads(self):
        c = object.__new__(resource_regression.Check)
        c.log, c.reader = [], lambda: ["units: alive=3\r", self.REFUSAL + "\r"]
        with self.assertRaisesRegex(AssertionError, "the GPU memory cap refused an allocation: "
                                                    "vk: gpumem: the cap refuses an allocation of 4194304"):
            c.read()
        self.assertEqual(c.log, ["units: alive=3", self.REFUSAL], "the lines are kept all the same")

    def test_a_tool_reads_its_instance_from_its_hold_on(self):
        with tempfile.TemporaryDirectory() as d:
            g = Path(d)
            (g / "log").mkdir()
            (g / "log" / "tagpu.log").write_bytes(HEAD % (1, 1) + self.REFUSAL.encode() + b"\r\n")
            with patch.dict(resource_regression._CAPS,
                            {"i": resource_regression.talog.CapWatch(g)}):
                self.assertIsNone(resource_regression.cap_failure("i"), "before the hold: not its")
                with open(g / "log" / "tagpu.log", "ab") as f:
                    f.write(self.REFUSAL.encode() + b"\r\n")
                self.assertEqual(resource_regression.cap_failure("i"),
                                 "the GPU memory cap refused i an allocation: " + self.REFUSAL)
            self.assertIsNone(resource_regression.cap_failure("unheld"))

    def test_a_compatibility_run_fails_whatever_it_expects(self):
        cap = "the GPU memory cap refused an allocation: " + self.REFUSAL
        o = {"outcome": "impure-active", "cap": cap,
             "gui": {"asks": {}, "asks_stress": {}, "gave_up": False, "stress": 0, "indexed": 0,
                     "refused": [self.REFUSAL], "lane_back": True}}
        lever = {"outcome": "impure-active", "refuse_lever": True}
        self.assertEqual(tacompat.judge(o, lever), [cap], "a lever excuses the device, not the cap")
        self.assertEqual(tacompat.judge(dict(o, cap=None), lever), [])


class LogWaits(unittest.TestCase):

    def run_wait(self, look_s, script, call):
        """call(gamedir) while the game runs `script`, [(t, bytes, or a callable of the log
        folder)] in time order; (its result, the fake time it returned at)."""
        with tempfile.TemporaryDirectory() as d:
            g = Path(d)
            (g / "log").mkdir()
            cur = g / "log" / "tagpu.log"
            cur.write_bytes(HEAD % (1, 1))
            clock, todo = [0.0], list(script)

            def sleep(s):
                clock[0] += s
                while todo and todo[0][0] <= clock[0] + 1e-9:
                    act = todo.pop(0)[1]
                    if callable(act):
                        act(g / "log")
                    else:
                        with open(cur, "ab") as f:
                            f.write(act)

            with patch.object(tacompat, "LOOK_S", look_s), \
                    patch.object(tacompat.time, "sleep", sleep), \
                    patch.object(tacompat.time, "monotonic", lambda: clock[0]):
                got = call(g)
            return got, round(clock[0], 6)

    def both(self, script, call):
        slow, fast = self.run_wait(0.5, script, call), self.run_wait(tacompat.LOOK_S, script, call)
        self.assertEqual(slow[0], fast[0])
        self.assertLessEqual(fast[1], slow[1] + tacompat.LOOK_S + 1e-6)   # a bound ends within a look of it
        return slow[0]

    @staticmethod
    def rotate(logdir):
        (logdir / "tagpu.log").rename(logdir / "tagpu.1.log")
        (logdir / "tagpu.log").write_bytes(HEAD % (1, 2))

    def test_the_look_is_the_fast_one(self):
        self.assertEqual(tacompat.LOOK_S, 0.02)

    def test_a_text_written_across_two_looks_is_said(self):
        says = lambda g: tacompat.log_says(g, tacompat.GUI_LANE_BACK, 90)
        text = tacompat.GUI_LANE_BACK.encode()
        self.assertTrue(self.both([(1.0, b"vk: x\n" + text[:20]), (1.01, text[20:] + b"\n")], says))
        self.assertTrue(self.both([(0.3, self.rotate), (2.0, text + b"\n")], says))
        self.assertFalse(self.both([(1.0, b"vk: other\n")], says))

    def test_said_after_reads_past_its_byte_only(self):
        text = tacompat.IMPAI_LEVEL_END
        at = len(HEAD % (1, 1)) + len(text) + 1
        said = lambda g: tacompat.log_says_after(g, text, at, 120)
        # the line before `at` does not count; the one written after it does
        self.assertTrue(self.both([(0, text.encode() + b"\n"), (3.0, b"x\n" + text.encode() + b"\n")], said))
        self.assertFalse(self.both([(0, text.encode() + b"\n")], said))

    def test_the_units_after_the_newest_applier_line_whole(self):
        alive = tacompat.alive_after_apply
        self.assertEqual(self.both([(0, b"units: alive=1 x\n"),
                                    (0.3, b"scenario: app"), (0.31, b"lied=400 failed=0\n"),
                                    (0.45, b"units: alive=3"), (0.47, b"7 onscreen=0\n")], alive), 37)
        # none: the units line before the applier's is another frame's
        self.assertIsNone(self.both([(0.2, b"units: alive=5\nscenario: applied=4 failed=0\n")], alive))

    def test_heartbeats_after_now_and_a_stall(self):
        beat = b"GUI flips=30 x\n"
        after = tacompat.presents_after
        self.assertTrue(self.both([(0, beat), (0.6, beat), (1.1, beat)], after))
        with patch.object(tacompat, "STALL_S", 3):
            self.assertFalse(self.both([(0.6, beat)], after))

    def test_the_lane_came_up_after_its_retry(self):
        back = tacompat.GUI_LANE_BACK.encode()

        def up(g):
            # the retry's line, then the lane's: refuse_once's loop after log_says found the first
            with patch.object(tacompat, "log_says", return_value=True), \
                    patch.object(tacompat, "refuse_lever_write"), \
                    patch.object(tacompat, "presents_after", return_value=True):
                return tacompat.refuse_once(g)
        self.assertIsNone(self.both([(0, b"vk: up in 3 ms\n" + back + b"\n"),
                                     (0.7, b"vk: up in 5 ms\n")], up))


if __name__ == "__main__":
    unittest.main()
