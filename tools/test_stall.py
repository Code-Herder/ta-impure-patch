"""stall.Stall, the bound of a wait on a running level: it asks nothing for the wait's first EVERY
seconds, never blocks a look, keeps one read of its own in flight, takes only its own answers,
names a level whose GameTime stands still within its stated bound, and retires a read in flight
before the wait hands the game on. A fake game answers the peek trigger on a fake clock that each
look advances, as tagpu_peek_frame does: the request read, deleted, then its line logged."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parent))
import stall  # noqa: E402

HEAD = b"log: run R1 part 1 of tagpu.log, started now\n"
LOOK = 0.02


class Game:
    """A game folder whose game answers each peek request `latency` s after it is written, with
    GameTime ticks(t), or never while ticks(t) is None; `answers` counts the requests read."""

    def __init__(self, d, ticks, latency=0.05):
        self.dir = Path(d)
        (self.dir / "log").mkdir()
        self.log = self.dir / "log" / "tagpu.log"
        self.log.write_bytes(HEAD)
        self.ticks, self.latency = ticks, latency
        self.clock = 0.0
        self.seen = None        # when the current request was first seen
        self.answers = 0

    @property
    def trigger(self):
        return self.dir / stall.PEEK_TRIGGER

    def sleep(self, s):
        self.clock += s
        if not self.trigger.exists():
            self.seen = None
            return
        if self.seen is None:
            self.seen = self.clock
        g = self.ticks(self.clock)
        if g is None or self.clock - self.seen < self.latency:
            return
        spec = self.trigger.read_text().strip()
        self.trigger.unlink()
        self.seen = None
        self.answers += 1
        with open(self.log, "ab") as f:
            f.write(f"peek: {spec} @0x00512345 = {g} (0x{g:08X})\r\n".encode())

    def log_line(self, text):
        with open(self.log, "ab") as f:
            f.write(text.encode() + b"\r\n")


class Failed(Exception):
    pass


class StallTests(unittest.TestCase):

    def wait(self, ticks, line_at, stall_s=120, gone=lambda: False, latency=0.05):
        """A wait looking every LOOK s for a line written at `line_at` (None: never), with a
        Stall over a game answering ticks(t): (the time it returned or failed, the failure or
        None, the requests the game answered, the game)."""
        with tempfile.TemporaryDirectory() as d:
            game = Game(d, ticks, latency)
            with patch.object(stall.time, "monotonic", lambda: game.clock):
                bound = stall.Stall(game.dir, stall_s, Failed, gone, game.sleep, LOOK)
                try:
                    while line_at is None or game.clock < line_at - 1e-9:
                        bound.check()
                        game.sleep(LOOK)
                    bound.retire()
                    err = None
                except Failed as e:
                    err = str(e)
                left = game.trigger.exists()
            return round(game.clock, 6), err, game.answers, left

    def test_a_line_within_every_asks_nothing(self):
        for line_at in (0, 0.5, stall.Stall.EVERY - LOOK):
            at, err, answers, left = self.wait(lambda t: int(t * 30), line_at)
            self.assertIsNone(err)
            self.assertEqual(answers, 0, "no request was ever written")
            self.assertFalse(left)
            self.assertAlmostEqual(at, line_at, delta=LOOK + 1e-6)

    def test_one_read_every_every_from_every_on(self):
        at, err, answers, left = self.wait(lambda t: int(t * 30), 12.0)
        self.assertIsNone(err)
        self.assertEqual(answers, 2)            # asked at 5 and 10 s, each answered at once
        self.assertFalse(left)
        self.assertAlmostEqual(at, 12.0, delta=LOOK + 1e-6)

    def test_a_read_in_flight_at_the_line_is_retired_first(self):
        # asked at 5 s and seen at the next look, answered 0.5 s after: the line at 5.2 s returns
        # once the answer is in, a look after it at most
        at, err, answers, left = self.wait(lambda t: int(t * 30), 5.2, latency=0.5)
        self.assertIsNone(err)
        self.assertEqual(answers, 1)
        self.assertFalse(left, "no request of the stall's is left for the next writer")
        self.assertGreaterEqual(at, 5.5 - 1e-6)
        self.assertLessEqual(at, 5.5 + 3 * LOOK + 1e-6)

    def test_a_retire_on_a_game_that_is_gone_drops_its_request(self):
        dead = [False]

        def ticks(t):
            if t >= 5.1:
                dead[0] = True
                return None
            return int(t * 30)
        at, err, answers, left = self.wait(ticks, 5.2, gone=lambda: dead[0], latency=0.5)
        self.assertIsNone(err)
        self.assertEqual(answers, 0)
        self.assertFalse(left, "the request a gone game can never read is removed")
        self.assertAlmostEqual(at, 5.2, delta=LOOK + 1e-6)

    def test_gametime_standing_still_is_named_within_the_bound(self):
        # it moves until 30 s, then stands: named at most EVERY + an answer + stall_s after that
        at, err, _, _ = self.wait(lambda t: int(min(t, 30.0) * 30), None)
        self.assertIn("stood at GameTime 900 for 120 s", err)
        self.assertGreater(at, 30.0 + 120)
        self.assertLessEqual(at, 30.0 + stall.Stall.EVERY + 0.05 + 120 + 2 * LOOK)

    def test_a_game_that_never_answers_is_named_from_the_first_read(self):
        at, err, answers, left = self.wait(lambda t: None, None)
        self.assertEqual(answers, 0)
        self.assertIn("its last peek unanswered", err)
        self.assertGreater(at, stall.Stall.EVERY + 120)
        self.assertLessEqual(at, stall.Stall.EVERY + 120 + 2 * LOOK)

    def test_an_unanswered_stretch_inside_the_bound_is_no_failure(self):
        # a seek's rebuild: no answer from 6 to 100 s, then the level runs on
        at, err, _, _ = self.wait(lambda t: None if 6 <= t < 100 else int(t * 30), 110.0)
        self.assertIsNone(err)

    def test_another_peeks_line_is_not_its_answer(self):
        with tempfile.TemporaryDirectory() as d:
            game = Game(d, lambda t: None)
            with patch.object(stall.time, "monotonic", lambda: game.clock):
                bound = stall.Stall(game.dir, 120, Failed, nap=game.sleep, look_s=LOOK)
                game.clock = stall.Stall.EVERY
                bound.check()
                self.assertTrue(game.trigger.exists())
                game.log_line(f"peek: {stall.GAMETIME}:4 @0x00512345 = 77 (0x0000004D)")
                game.log_line("peek: " + bound.asked.replace(":4", ":2") + " @0x00512345 = 1 (0x0001)")
                self.assertIsNone(bound.check())
                self.assertIsNotNone(bound.asked)
                game.log_line(f"peek: {bound.asked} @0x00512345 = 78 (0x0000004E)")
                self.assertEqual(bound.check(), 78)
                self.assertIsNone(bound.asked)

    def test_each_read_is_spelled_apart_and_reads_gametime(self):
        with tempfile.TemporaryDirectory() as d:
            game = Game(d, lambda t: 5)
            with patch.object(stall.time, "monotonic", lambda: game.clock):
                bound = stall.Stall(game.dir, 120, Failed, nap=game.sleep, look_s=LOOK)
                specs = []
                for _ in range(3):
                    game.clock += stall.Stall.EVERY
                    bound.check()
                    specs.append(bound.asked)
                    bound.retire()
        self.assertEqual(len(set(specs)), 3)
        for spec in specs:
            # tagpu_peek.c's eval: `*base`, then each `+term` added to an unsigned 32-bit address
            terms = spec[1:].split(":")[0].split("+")
            self.assertEqual(terms[:2], ["0x511DE8", "0x38A47"])
            self.assertEqual(sum(int(t, 16) for t in terms[1:]) & 0xFFFFFFFF, 0x38A47)

    def test_a_saw_loop_asks_nothing_and_is_bounded(self):
        with tempfile.TemporaryDirectory() as d:
            game = Game(d, lambda t: 7)
            with patch.object(stall.time, "monotonic", lambda: game.clock):
                bound = stall.Stall(game.dir, 120, Failed, nap=game.sleep, look_s=LOOK)
                with self.assertRaisesRegex(Failed, "stood at GameTime 7 for 120 s"):
                    while True:
                        bound.saw(7)
                        game.sleep(0.5)
                self.assertFalse(game.trigger.exists())
                self.assertAlmostEqual(game.clock, 120.5, delta=1e-6)


if __name__ == "__main__":
    unittest.main()
