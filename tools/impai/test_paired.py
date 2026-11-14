#!/usr/bin/env python3
"""test_paired -- the mend's value bar offline over synthetic measurements: which deaths a seat-game's lost and
killed count, which seats are seat-games, the pairing's refusals (a partner missing, seeds differing, too few pairs,
an id measured twice, a recording not read to its end, a result naming no seed), the bootstrap's determinism, and
the verdict on (a) and (b) each way. No game runs.

    python3 tools/impai/test_paired.py"""
import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import paired  # noqa: E402

OTA_SEATS = {"1": {"letter": "A", "ai": "impai", "level": "hard"}, "2": {"letter": "B", "ai": "ai", "level": None}}


def death(owner, kp, metal, cls="army", t=100, fin=True):
    return {"t": t, "owner": owner, "kp": kp, "metal": metal, "cls": cls, "fin": fin}


def lost_killed(lost, killed):
    """deaths that give seat 1 exactly `lost` and `killed` against seat 2"""
    return [death(1, 2, lost), death(2, 1, killed)]


class Arms:
    """two arms of measurements, each beside its games' results, in a temporary folder"""

    def __init__(self, root: Path):
        self.root = root

    def add(self, arm, gid, deaths, seats=None, seed=None, stop="None", sub="p1", name=None):
        games = self.root / "games" / arm / sub
        games.mkdir(parents=True, exist_ok=True)
        rec = games / f"{gid}.imprec"
        (games / f"{gid}.json").write_text(json.dumps({"id": gid, "seed": seed if seed is not None else len(gid)}))
        m = self.root / "measure" / arm / sub
        m.mkdir(parents=True, exist_ok=True)
        (m / f"{name or gid}.json").write_text(json.dumps(
            {"rec": str(rec), "id": gid, "stop": stop, "seats": seats or OTA_SEATS, "deaths": deaths}))

    def judge(self, want=paired.PAIRS):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = paired.judge(self.root / "measure" / "base", self.root / "measure" / "test", want)
        return rc, out.getvalue()


class Measures(unittest.TestCase):
    def test_lost_and_killed(self):
        deaths = [
            death(1, 2, 100),                    # lost
            death(1, 2, 50, cls="cons"),         # a constructor lost: not army
            death(1, None, 70),                  # no killing player: no loss to an enemy
            death(1, 1, 30),                     # its own kill
            death(1, 2, 40, t=1),                # the applier's replacement
            death(1, 2, 20, fin=False),          # a frame
            death(2, 1, 200),                    # killed
            death(2, 1, 60, cls="cons"),         # killed
            death(2, 1, 900, cls="struct"),      # a structure: not counted
            death(2, 1, 900, cls="com"),         # the commander: not counted
            death(2, 2, 80),                     # the enemy's own
        ]
        self.assertEqual(paired.seat_measures(deaths, 1), {"lost": 100.0, "killed": 260.0})

    def test_seat_games_against_ota_only(self):
        self.assertEqual(paired.ota_seats(OTA_SEATS), [1])
        both = {"1": {"ai": "impai"}, "2": {"ai": "impai"}}
        self.assertEqual(paired.ota_seats(both), [])
        self.assertEqual(paired.ota_seats({"1": {"ai": "ai"}, "2": {"ai": "ai"}}), [])

    def test_boot_is_deterministic(self):
        xs = [float(k % 7 - 3) for k in range(40)]
        self.assertEqual(paired.boot(xs, 2000), paired.boot(xs, 2000))
        lo, hi = paired.boot(xs, 2000)
        self.assertLessEqual(lo, paired.mean(xs))
        self.assertLessEqual(paired.mean(xs), hi)


class Bar(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.arms = Arms(Path(self.tmp.name))
        self.resamples = paired.RESAMPLES
        paired.RESAMPLES = 2000

    def tearDown(self):
        paired.RESAMPLES = self.resamples
        self.tmp.cleanup()

    def games(self, n, base, test):
        """n paired games, base(k) and test(k) each (lost, killed)"""
        for k in range(n):
            gid = f"bar-hota-pincushion-ac-{k}"
            self.arms.add("base", gid, lost_killed(*base(k)))
            self.arms.add("test", gid, lost_killed(*test(k)))

    def test_pass(self):
        self.games(60, lambda k: (600 + 37 * (k % 9), 700 + 41 * (k % 5)),
                   lambda k: (450 + 37 * (k % 9), 700 + 41 * ((k + 2) % 5)))
        rc, out = self.arms.judge()
        self.assertEqual(rc, 0, out)
        self.assertIn("60 seat-games against OTA AI", out)
        self.assertIn("paired: PASS", out)

    def test_losses_not_falling(self):
        self.games(60, lambda k: (600 + 37 * (k % 9), 700), lambda k: (600 + 37 * ((k + 4) % 9), 700))
        rc, out = self.arms.judge()
        self.assertEqual(rc, 1, out)
        self.assertIn("(a) lost falls", out)
        self.assertRegex(out, r"\(a\).*FAIL")

    def test_kills_falling_past_the_margin(self):
        self.games(60, lambda k: (600, 1000 + 10 * (k % 3)), lambda k: (400, 800 + 10 * (k % 3)))
        rc, out = self.arms.judge()
        self.assertEqual(rc, 1, out)
        self.assertRegex(out, r"\(a\).*PASS")
        self.assertRegex(out, r"\(b\).*FAIL")

    def test_kills_within_the_margin(self):
        self.games(60, lambda k: (600, 1000 + 10 * (k % 3)), lambda k: (400, 950 + 10 * (k % 3)))
        rc, out = self.arms.judge()
        self.assertEqual(rc, 0, out)

    def test_impai_against_impai_is_no_seat_game(self):
        both = {"1": {"ai": "impai", "level": "hard"}, "2": {"ai": "impai", "level": "normal"}}
        self.games(60, lambda k: (600, 700), lambda k: (400, 700))
        self.arms.add("base", "bar-hn-pincushion-ac-1", lost_killed(1, 1), seats=both)
        self.arms.add("test", "bar-hn-pincushion-ac-1", lost_killed(1, 1), seats=both)
        rc, out = self.arms.judge()
        self.assertEqual(rc, 0, out)
        self.assertIn("60 seat-games", out)

    def test_could_not_judge(self):
        cases = {
            "no partner": lambda: self.arms.add("base", "bar-hota-lavamania-cc-1", []),
            "seeds differ": lambda: (self.arms.add("base", "bar-hota-lavamania-cc-1", [], seed=5),
                                     self.arms.add("test", "bar-hota-lavamania-cc-1", [], seed=6)),
            "measured twice": lambda: self.arms.add("test", "bar-hota-pincushion-ac-3", [], sub="p2"),
            "not read to its end": lambda: self.arms.add("test", "bar-hota-pincushion-ac-3", [],
                                                         stop="truncated block"),
            "no seed": lambda: (Path(self.tmp.name, "games", "test", "p1", "bar-hota-pincushion-ac-3.json")
                                .write_text(json.dumps({"id": "bar-hota-pincushion-ac-3", "seed": None}))),
            "too few": lambda: None,
        }
        for name, spoil in cases.items():
            with self.subTest(name):
                self.tearDown()
                self.setUp()
                self.games(60 if name != "too few" else 59, lambda k: (600, 700), lambda k: (400, 700))
                spoil()
                rc, out = self.arms.judge()
                self.assertEqual(rc, 2, out)
                self.assertIn("COULD NOT JUDGE", out)

    def test_pairs_option(self):
        self.games(13, lambda k: (600 + 37 * (k % 9), 700), lambda k: (400 + 37 * (k % 9), 700))
        self.assertEqual(self.arms.judge()[0], 2)
        self.assertEqual(self.arms.judge(13)[0], 0)

    def test_main(self):
        self.games(60, lambda k: (600, 700), lambda k: (400, 700))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = paired.main([str(Path(self.tmp.name, "measure", "base")), str(Path(self.tmp.name, "measure", "test"))])
        self.assertEqual(rc, 0, out.getvalue())


if __name__ == "__main__":
    unittest.main()
