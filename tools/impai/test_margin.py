#!/usr/bin/env python3
"""test_margin -- the paired margin test offline: the order and its blocks, the pairing's refusals (a seed
mismatch, a result with no partner, an id twice in one arm, a game that could not run), the tie band and each
pair's sign, the loss rule, the GSPRT's ratio against hand-computed references, its floor (no pair, and runs of
ties), identical games accepting NOT-WORSE and rejecting IMPROVE at the minimum sample, its stopping rule (the first
block at a bound decides, the blocks after it unread, no decision at the end of the order), the simulated cost's
error rates at the hypotheses, and `run` on a fake player: it stops at the verdict, plays a block with a game
that could not run once more, and refuses a directory of other DLLs. No game runs and no scipy is needed.

    python3 tools/impai/test_margin.py"""
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
import margin  # noqa: E402

LN19 = math.log(19)


def result(gid, outcome="A", end=30000, **kw):
    r = {"id": gid, "outcome": outcome, "end_tick": end, "cap": bar.CAP, "seed": bar.game_seed(gid),
         "renderer": "gdi"}
    if outcome == "cap":
        r["end_tick"] = bar.CAP
    r.update(kw)
    return r


def arms(times, ids=None):
    """(BASE, TEST) results by id from [(base end, test end)] over the order's first ids; an end of None is a
    loss, "cap" the cap."""
    ids = ids or margin.order()
    base, test = {}, {}
    for gid, (b, t) in zip(ids, times):
        for arm, x in ((base, b), (test, t)):
            arm[gid] = (result(gid, "B", 20000) if x is None else result(gid, "cap") if x == "cap"
                        else result(gid, "A", x))
    return base, test


class TestOrder(unittest.TestCase):
    def test_the_order_is_fixed_hota_then_noh_cut_at_the_cap(self):
        o = margin.order()
        self.assertEqual(o, margin.order())
        self.assertEqual(len(o), margin.CAP_PAIRS)
        self.assertEqual(len(set(o)), len(o))
        self.assertEqual(sorted(o[:100]), sorted(bar.ids("hota")))       # every hota id first
        self.assertTrue(all("-noh-" in g for g in o[100:]))
        self.assertNotEqual(o[:100], bar.ids("hota"))                    # shuffled
        self.assertEqual(o[0], "bar-hota-troutfarm-cc-4")               # pinned: a change reorders every run
        self.assertEqual([len(b) for b in margin.blocks()], [16] * 12 + [8])


class TestPairing(unittest.TestCase):
    def test_refusals(self):
        base, test = arms([(30000, 30000)] * 3)
        g = margin.order()
        self.assertEqual(len(margin.pairs(base, test)), 3)
        with self.assertRaisesRegex(margin.CouldNotJudge, "no partner in TEST"):
            margin.pairs(base, {k: v for k, v in test.items() if k != g[1]})
        with self.assertRaisesRegex(margin.CouldNotJudge, "seeds"):
            margin.pairs(base, dict(test, **{g[0]: dict(test[g[0]], seed=1)}))
        with self.assertRaisesRegex(margin.CouldNotJudge, "could not run"):
            margin.pairs(base, dict(test, **{g[0]: dict(test[g[0]], outcome="could_not_run", reason="x")}))
        self.assertEqual(len(margin.pairs(dict(base, **{"bar-hota-x": {}}), test)), 3)    # not in the order
        with tempfile.TemporaryDirectory() as d:
            for sub in ("a", "b"):
                (Path(d) / sub).mkdir()
                (Path(d) / sub / f"{g[0]}.json").write_text(json.dumps(base[g[0]]))
            (Path(d) / "a" / f"{g[0]}.scenario.json").write_text("{}")
            with self.assertRaisesRegex(margin.CouldNotJudge, "twice"):
                margin.read_arm(Path(d))
            self.assertEqual(list(margin.read_arm(Path(d) / "a")), [g[0]])


class TestMeasure(unittest.TestCase):
    def test_the_tie_band_and_each_pairs_sign(self):
        self.assertEqual(margin.sign(30000, 30000), 0)
        self.assertEqual(margin.sign(30000, 29700), 0)                  # inside the band
        self.assertEqual(margin.sign(30000, 29699), 1)
        self.assertEqual(margin.sign(30000, 30301), -1)
        self.assertEqual(margin.sign(bar.CAP, bar.CAP), 0)              # both at the cap
        self.assertEqual(margin.sign(None, bar.CAP), 1)                 # BASE lost, TEST did not
        self.assertEqual(margin.time_to_win(result("bar-hota-pincushion-aa-1", "cap", draw=True)), bar.CAP)
        self.assertIsNone(margin.time_to_win(result("bar-hota-pincushion-aa-1", "B")))


def block(better, ties, worse):
    """one block's (BASE end, TEST end) pairs: `better` TEST better, `ties` ties, `worse` TEST worse"""
    return [(40000, 30000)] * better + [(30000, 30100)] * ties + [(30000, 40000)] * worse


class TestGsprt(unittest.TestCase):
    def test_the_arithmetic(self):
        self.assertEqual(margin.bounds(), (pytest_approx(-LN19), pytest_approx(LN19)))
        self.assertEqual(margin.bounds(0.1, 0.1), (pytest_approx(-math.log(9)), pytest_approx(math.log(9))))
        # 10 better, 2 ties, 4 worse, improve, by hand: with the pseudo-pair n = 17, m = 11.5 / 17 = 0.67647,
        # v = (10.5 * 0.32353^2 + 2 * 0.17647^2 + 4.5 * 0.67647^2) / 17 = 0.18945,
        # LLR = 17 * 0.05 * (1.35294 - 1.05) / (2 * 0.18945) = 0.67961
        self.assertAlmostEqual(margin.llr(10, 2, 4, 0.50, 0.55), 0.67961, places=5)
        # 8 better, 4 ties, 4 worse, notworse: m = 10.5 / 17 = 0.61765, v = 0.17734, LLR = 0.68373
        self.assertAlmostEqual(margin.llr(8, 4, 4, 0.45, 0.50), 0.68373, places=5)
        # 26 better, 2 ties, 4 worse, improve: m = 27.5 / 33 = 0.83333, v = 0.12374, LLR = 4.11153
        self.assertAlmostEqual(margin.llr(26, 2, 4, 0.50, 0.55), 4.11153, places=5)
        # the defaults, width 0.10: the same 10, 2, 4 at improve 0.50 / 0.60, 17 * 0.1 * (1.35294 - 1.1) / 0.37789
        self.assertEqual(margin.MODES, {"improve": (0.50, 0.60), "notworse": (0.40, 0.50)})
        self.assertAlmostEqual(margin.llr(10, 2, 4, 0.50, 0.60), 1.13489, places=5)
        base, test = arms(block(10, 2, 4))
        v = margin.verdict(base, test, "improve")
        self.assertEqual((v["better"], v["worse"], v["ties"], v["blocks"], v["exit"]), (10, 4, 2, 1, margin.UNDECIDED))
        self.assertAlmostEqual(v["llr"], 1.13489, places=5)
        self.assertAlmostEqual(v["mean"], 11 / 16)
        self.assertIn("no decision yet after 1 blocks", v["verdict"])

    def test_the_floor(self):
        # no pair at all: the pseudo-pair alone, m = 0.5, v = 0.25 -- no division by zero
        self.assertAlmostEqual(margin.llr(0, 0, 0, 0.45, 0.50), 0.005)
        # n ties: m = 0.5, v = 0.25 / (n + 1), LLR = +-(s1 - s0)^2 (n + 1)^2 / 0.5 -- at width 0.05, 0.005 (n + 1)^2:
        # 16 read 1.445, 24 3.125, 32 5.445; at width 0.10, 0.02 (n + 1)^2: 11 read 2.88, 12 3.38, 16 5.78
        for n, x in ((16, 1.445), (23, 2.88), (24, 3.125), (32, 5.445)):
            self.assertAlmostEqual(margin.llr(0, n, 0, 0.45, 0.50), x)
            self.assertAlmostEqual(margin.llr(0, n, 0, 0.50, 0.55), -x)
        for n, x in ((11, 2.88), (12, 3.38), (16, 5.78)):
            self.assertAlmostEqual(margin.llr(0, n, 0, 0.40, 0.50), x)
            self.assertAlmostEqual(margin.llr(0, n, 0, 0.50, 0.60), -x)

    def test_identical_games_decide_at_the_minimum_sample_not_before(self):
        base, test = arms([(30000, 30000)] * 48)                         # every pair the same end tick
        self.assertEqual(margin.MIN_BLOCKS, 2)
        for mode, ex, verdict in (("notworse", margin.ACCEPT, "TEST is not worse"),
                                  ("improve", margin.REJECT, "TEST is not measurably better")):
            v = margin.verdict(base, test, mode)
            self.assertEqual((v["exit"], v["blocks"], v["pairs"], v["ties"], v["verdict"]),
                             (ex, 2, 32, 32, verdict))
            self.assertEqual(len(v["unread"]), 16)
            self.assertAlmostEqual(abs(v["trace"][0][1]), 5.78)         # past the bound at block 1, not read
            self.assertAlmostEqual(abs(v["llr"]), 21.78)
            b1, t1 = arms([(30000, 30000)] * 16)
            self.assertEqual(margin.verdict(b1, t1, mode)["exit"], margin.UNDECIDED)

    def test_a_test_loss_fails_outright_whatever_the_scores(self):
        base, test = arms(block(16, 0, 0)[:15] + [(30000, None)] + block(16, 0, 0))
        for mode in margin.MODES:
            v = margin.verdict(base, test, mode)
            self.assertEqual((v["exit"], v["losses"], v["blocks"]), (margin.REJECT, [margin.order()[15]], 0))
            self.assertIn("a loss fails the test outright", v["verdict"])

    def test_the_first_block_at_a_bound_decides_and_the_rest_is_not_read(self):
        # improve: block 1 reads 1.13, block 2's 16 better take it to 7.56 (33 * 0.1 * (1.66667 - 1.1) / 0.24747);
        # block 3 (all worse) is not read
        base, test = arms(block(10, 2, 4) + block(16, 0, 0) + block(0, 0, 16))
        v = margin.verdict(base, test, "improve")
        self.assertEqual((v["exit"], v["blocks"], v["pairs"], v["verdict"]), (margin.ACCEPT, 2, 32, "TEST is better"))
        self.assertAlmostEqual(v["llr"], 7.55633, places=5)
        self.assertEqual(len(v["unread"]), 16)
        # THE MINIMUM SAMPLE: a block of 16 worse reads -25.05, past ln(1/19), and is not read alone; the second
        # block decides
        base, test = arms(block(0, 0, 16))
        v = margin.verdict(base, test, "notworse")
        self.assertEqual((v["exit"], v["blocks"], v["verdict"]),
                         (margin.UNDECIDED, 1, "no decision yet after 1 blocks"))
        self.assertAlmostEqual(v["llr"], -25.0467, places=3)
        base, test = arms(block(0, 0, 16) + block(0, 0, 16))
        v = margin.verdict(base, test, "notworse")
        self.assertEqual((v["exit"], v["blocks"], v["verdict"]), (margin.REJECT, 2, "TEST is worse"))
        self.assertAlmostEqual(v["trace"][0][1], -25.0467, places=3)
        # an incomplete block is not read
        base, test = arms(block(10, 2, 4) + block(4, 0, 0))
        v = margin.verdict(base, test, "improve")
        self.assertEqual((v["blocks"], v["pairs"], len(v["unread"]), v["exit"]), (1, 16, 4, margin.UNDECIDED))

    def test_no_decision_at_the_end_of_the_order(self):
        # a mean near the midpoint of the hypotheses (0.45, 0.55) keeps the ratio near 0 through every block, the
        # last (8 pairs, 4 better and 4 worse) included: 7 of 16 better (notworse), 9 of 16 (improve)
        b, w = block(1, 0, 0), block(0, 0, 1)
        for mode, pattern in (("notworse", (b + w) * 7 + w + w), ("improve", (w + b) * 7 + b + b)):
            base, test = arms(pattern * 13)
            v = margin.verdict(base, test, mode)
            self.assertEqual((v["exit"], v["blocks"], v["pairs"]), (margin.UNDECIDED, 13, margin.CAP_PAIRS))
            self.assertLess(max(abs(x) for _k, x in v["trace"]), 1.0)             # the bounds are +-2.944
            self.assertIn("NO DECISION", v["verdict"])

    def test_the_simulated_cost(self):
        rng = random.Random(3)
        # all ties: every run decides at the minimum sample, the second block, at either width
        r = margin.simulate(0.5, 1.0, 0.45, 0.50, margin.ALPHA, 50, rng)
        self.assertEqual((r["accept"], r["pairs"], r["open"]), (1.0, 32, 32))
        r = margin.simulate(0.5, 1.0, *margin.MODES["notworse"], margin.ALPHA, 50, rng)
        self.assertEqual((r["accept"], r["pairs"], r["open"]), (1.0, 32, 32))
        # at H0 and H1 the open-ended test errs about alpha (the normal approximation and the block steps: 0.035)
        for mode, (s0, s1) in margin.MODES.items():
            r0 = margin.simulate(s0, 0.3, s0, s1, margin.ALPHA, 600, rng)
            r1 = margin.simulate(s1, 0.3, s0, s1, margin.ALPHA, 600, rng)
            self.assertLess(r0["accept"], margin.ALPHA + 0.035, mode)
            self.assertGreater(r1["accept"] + r1["none"], 1 - margin.ALPHA - 0.035, mode)
            self.assertLessEqual(r1["pairs"], margin.CAP_PAIRS)
            self.assertGreater(r1["open"], r1["pairs"])                  # the cap cuts the open-ended test
        rows = margin.asn_rows("notworse", runs=5)
        self.assertEqual(len(rows), 9 * 4 - 4)                           # tie 0.9 needs 0.45 <= mean <= 0.55
        self.assertEqual({t for _m, t, _r in rows}, {0.0, 0.3, 0.6, 0.9})


class TestRun(unittest.TestCase):
    def fake(self, times):
        """a block player whose games end as `times` by order index: (BASE end, TEST end), "cnr" for a game that
        could not run on its first play"""
        played = []

        def play(k, ids, dlls, out, lanes):
            played.append(k)
            rc = 0
            for arm, j in (("base", 0), ("test", 1)):
                (out / arm).mkdir(parents=True, exist_ok=True)
                for gid in ids:
                    x = times(margin.order().index(gid))[j]
                    if x == "cnr" and played.count(k) == 1:
                        r, rc = result(gid, "could_not_run", reason="x"), 2      # bar.py play's exit 2
                    else:
                        r = result(gid, "A", 30000 if x == "cnr" else x)
                    (out / arm / f"{gid}.json").write_text(json.dumps(r))
            return rc
        return play, played

    def run_(self, d, play, base=None, test=None):
        import argparse
        dll = Path(d) / "x.dll"
        dll.write_bytes(b"dll")
        a = argparse.Namespace(out=str(Path(d) / "out"), base=str(base or dll), test=str(test or dll),
                               mode="improve", lanes=1)
        with mock.patch("builtins.print"):
            return margin.run(a, play=play)

    def test_run_stops_at_the_verdict(self):
        with tempfile.TemporaryDirectory() as d, mock.patch.dict("os.environ", {"TARUN_DIR": d}):
            play, played = self.fake(lambda i: (40000, 30000))
            self.assertEqual(self.run_(d, play), margin.ACCEPT)
            self.assertEqual(played, [1, 2])                            # block 1 is past the bound, not read

    def test_a_block_with_a_game_that_could_not_run_is_played_once_more(self):
        with tempfile.TemporaryDirectory() as d, mock.patch.dict("os.environ", {"TARUN_DIR": d}):
            play, played = self.fake(lambda i: (40000, "cnr" if i == 3 else 30000))
            self.assertEqual(self.run_(d, play), margin.ACCEPT)
            self.assertEqual(played, [1, 1, 2])

    def test_a_directory_of_other_dlls_is_refused(self):
        with tempfile.TemporaryDirectory() as d, mock.patch.dict("os.environ", {"TARUN_DIR": d}):
            play, played = self.fake(lambda i: (30000, 30000))
            other = Path(d) / "y.dll"
            other.write_bytes(b"other")
            margin.fix_dlls(Path(d) / "out", other, other)
            self.assertEqual(self.run_(d, play), margin.COULD_NOT)
            self.assertEqual(played, [])


def pytest_approx(x):
    """a float equal to x within 1e-12, for assertEqual on tuples"""
    class Approx(float):
        def __eq__(self, other):
            return abs(other - x) < 1e-12
        __hash__ = float.__hash__
    return Approx(x)


if __name__ == "__main__":
    unittest.main()
