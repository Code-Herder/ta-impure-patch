#!/usr/bin/env python3
"""test_ladder -- the handicap ladder's bisection, fit, run and resume, with a fake player, offline.

    python3 tools/impai/test_ladder.py

The fake player writes each piece's results as bar.py would, from a rule on the game's multiplier (bar.income_of
of its pair), so the run's looks, its state, its resume and its refusals run as they do on games."""
import argparse
import json
import math
import random
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bar  # noqa: E402
import ladder  # noqa: E402


class FakePlayer:
    """play(pieces, launch): writes a result for every id of every piece, A's outcome from `rule(mult, gid)`:
    "A", "B" or "draw"; records the pieces played; leaves out the ids in `lose`."""

    def __init__(self, rule):
        self.rule, self.played, self.lose = rule, [], set()

    def __call__(self, ps, launch):
        for p in ps:
            self.played.append(p["name"])
            Path(p["out"]).mkdir(parents=True, exist_ok=True)
            for gid in p["ids"]:
                if gid in self.lose:
                    continue
                pair = gid.split("-")[1]
                who = self.rule(bar.income_of(pair), gid)
                r = {"id": gid, "outcome": "cap" if who == "draw" else who, "draw": who == "draw"}
                (Path(p["out"]) / f"{gid}.json").write_text(json.dumps(r))
        return []


class Method(unittest.TestCase):
    def test_bisection_moves_the_bracket(self):
        self.assertEqual(ladder.next_x([]), 1.5)
        self.assertEqual(ladder.next_x([{"x": 1.5, "points": 0.5}]), 2.25)
        self.assertEqual(ladder.next_x([{"x": 1.5, "points": 0.45}]), 0.75)
        self.assertEqual(ladder.next_x([{"x": 1.5, "points": 0.6}, {"x": 2.25, "points": 0.2}]), 1.875)

    def test_the_multiplier_is_the_levers(self):
        self.assertEqual(ladder.mult_of(1.5), 2.828)
        self.assertEqual(ladder.mult_of(0.0), 1.0)
        self.assertEqual(ladder.mult_of(3.2), 8.0)
        self.assertEqual(bar.handicap_pair(ladder.mult_of(1.5)), "h:2.828")
        bar.pair_spec(bar.handicap_pair(ladder.mult_of(2.999)))

    def test_the_fit_finds_the_half_point(self):
        rng = random.Random(3)
        xs, ys = [], []
        for x in (0.5, 1.0, 1.5, 2.0, 2.5):
            for _ in range(200):
                xs.append(x)
                ys.append(1.0 if rng.random() < ladder.p_win(x, 1.3, 0.3) else 0.0)
        fr = ladder.fit(xs, ys)
        self.assertAlmostEqual(fr["c"], 1.3, delta=0.05)
        self.assertAlmostEqual(fr["s"], 0.3, delta=0.06)
        self.assertLess(fr["se"], 0.05)

    def test_a_separated_look_set_stays_finite(self):
        fr = ladder.fit([1.0] * 20 + [2.0] * 20, [1.0] * 20 + [0.0] * 20)
        self.assertTrue(1.0 < fr["c"] < 2.0)
        self.assertTrue(math.isfinite(fr["s"]))

    def test_simulation_beats_the_midpoint(self):
        got = ladder.simulate(6, 20, 0.3, 0.3, runs=40, seed=2)
        self.assertLess(got["rmse"], got["mid_rmse"])
        self.assertLess(got["rmse"], 0.12)

    def test_report_says_where_it_lies(self):
        self.assertIn("above the lever's x8", ladder.report({"c": 3.4, "s": 0.3, "se": 0.2}, []))
        self.assertIn("below x1", ladder.report({"c": -0.2, "s": 0.3, "se": None}, []))
        self.assertIn("m* = x2.000, 90 %", ladder.report({"c": 1.0, "s": 0.3, "se": 0.1}, []))


class Run(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(__import__("shutil").rmtree, self.tmp, True)
        self.dll = self.tmp / "x.dll"
        self.dll.write_bytes(b"dll")

    def run_(self, player, looks=6, dll=None):
        a = argparse.Namespace(out=str(self.tmp / "out"), dll=str(dll or self.dll), looks=looks, lanes=10)
        with mock.patch.dict("os.environ", {"TARUN_DIR": str(self.tmp / "runs")}), \
                mock.patch("builtins.print") as said:
            rc = ladder.run(a, launch=lambda p: None, play=player)
        self.said = "\n".join(str(c.args[0]) for c in said.call_args_list)
        return rc

    def state(self):
        return json.loads((self.tmp / "out" / ladder.STATE).read_text())

    def test_a_threshold_is_bracketed_and_fitted(self):
        # A wins every game below x3 and loses every one above: the looks close on log2(3), each midpoint taken
        # between the multipliers played, which are rounded to the lever's three places
        p = FakePlayer(lambda m, gid: "A" if m < 3.0 else "B")
        self.assertEqual(self.run_(p), 0)
        looks = self.state()["looks"]
        self.assertEqual([lk["pair"] for lk in looks][:3], ["h:2.828", "h:4.756", "h:3.667"])
        self.assertEqual(len(p.played), 12)                   # two blocks a look, six looks
        self.assertTrue(all(len(lk["ids"]) == 20 for lk in looks))
        fit = (self.tmp / "out" / "fit.txt").read_text()
        mstar = float(fit.split("m* = x")[1].split(",")[0])
        self.assertAlmostEqual(math.log2(mstar), math.log2(3.0), delta=0.1)

    def test_a_draw_is_half_a_point(self):
        p = FakePlayer(lambda m, gid: "draw")
        self.assertEqual(self.run_(p, looks=1), 0)
        self.assertEqual(self.state()["looks"][0]["points"], 0.5)

    def test_resume_plays_only_the_looks_left(self):
        p = FakePlayer(lambda m, gid: "A" if m < 2.0 else "B")
        self.assertEqual(self.run_(p, looks=2), 0)
        self.assertEqual(len(p.played), 4)
        self.assertEqual(self.run_(p, looks=4), 0)
        self.assertEqual(len(p.played), 8)
        self.assertEqual([lk["k"] for lk in self.state()["looks"]], [1, 2, 3, 4])

    def test_a_missing_game_is_played_once_more_then_could_not_judge(self):
        p = FakePlayer(lambda m, gid: "A")
        p.lose = {bar.ids("h:2.828")[0]}
        self.assertEqual(self.run_(p, looks=1), 2)
        self.assertEqual(len(p.played), 4)                    # the look's two pieces, twice
        self.assertIn("COULD NOT JUDGE", self.said)
        p.lose = set()
        self.assertEqual(self.run_(p, looks=1), 0)            # the next run goes on from the look

    def test_another_dll_is_refused(self):
        self.assertEqual(self.run_(FakePlayer(lambda m, gid: "A"), looks=1), 0)
        other = self.tmp / "y.dll"
        other.write_bytes(b"other")
        self.assertEqual(self.run_(FakePlayer(lambda m, gid: "A"), looks=2, dll=other), 2)
        self.assertIn("another DLL", self.said)


class Bar(unittest.TestCase):
    def test_the_handicap_pair(self):
        g = bar.parse_id("bar-h:2.500-pincushion-ac-1")
        self.assertEqual(g["difficulty"], "hard")
        self.assertEqual(g["seats"]["A"], {"ai": "impai", "level": "hard"})
        self.assertEqual(g["seats"]["B"], {"ai": "ai", "level": None, "income": 2.5})
        self.assertTrue(bar.block_pair("h:2.500"))
        self.assertEqual(bar.run_blocks("h:2.500"), bar.HANDICAP_BLOCKS)
        self.assertEqual(len(bar.ids("h:2.500")), 10 * bar.HANDICAP_BLOCKS)
        self.assertEqual(bar.pool("h:2.500"), bar.BAR_MAPS)
        for bad in ("h:2.5", "h:0.900", "h:8.001", "h:x"):
            with self.assertRaises(bar.BarError):
                bar.pair_spec(bad)

    def test_the_lever_lines(self):
        lines = ["income: installed: the economy pass's call 0x46555A; the lever names seats",
                 "income: seat=2 mult=2.500 from=lever"]
        self.assertEqual(bar.incomes(lines, {"A": 1, "B": 2}), {"B": 2.5})
        self.assertEqual(bar.incomes(lines, {"A": 2, "B": 1}), {"A": 2.5})
        self.assertEqual(bar.incomes(lines[:1], {"A": 1, "B": 2}), {})

    def test_the_cheat_flag_reads_the_multiplier_out(self):
        def res(gid, inc_b):
            g = bar.parse_id(gid)
            return {"id": gid, "sides": g["sides"], "difficulty": "hard",
                    "seats": {s: dict(g["seats"][s]) for s in ("A", "B")},
                    "income_t1": {"A": {"metal": 2.0, "energy": 50.0}, "B": inc_b}}
        fair = res("bar-h:2.500-pincushion-cc-1", {"metal": 5.0, "energy": 125.0})
        self.assertEqual(bar.cheat_evidence([fair]), [])
        cheat = res("bar-h:2.500-pincushion-cc-2", {"metal": 10.0, "energy": 250.0})
        self.assertEqual(len(bar.cheat_evidence([cheat])), 1)

    def test_a_kept_result_must_carry_its_multiplier(self):
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(__import__("shutil").rmtree, tmp, True)
        g = bar.parse_id("bar-h:2.500-pincushion-ac-1")
        r = {"id": g["id"], "outcome": "A", "setup": None, "cap": bar.CAP, "renderer": bar.renderer(False),
             "pacing": bar.pacing(False, False), "seed": g["seed"],
             "seats": {s: dict(g["seats"][s]) for s in ("A", "B")}}
        (tmp / f"{g['id']}.json").write_text(json.dumps(r))
        self.assertIsNotNone(bar.game_kept(tmp, g, False))
        r["seats"]["B"].pop("income")
        (tmp / f"{g['id']}.json").write_text(json.dumps(r))
        self.assertIsNone(bar.game_kept(tmp, g, False))


if __name__ == "__main__":
    unittest.main()
