#!/usr/bin/env python3
"""test_margin -- the paired margin test offline: the order and its blocks, the pairing's refusals (a seed
mismatch, a result with no partner, an id twice in one arm, a game that could not run), the tie band and each
pair's sign, the loss rule, the SPRT's arithmetic against hand-computed references, its stopping rule (the first
block at a bound decides, the blocks after it unread, no decision at the end of the order), Wald's operating
characteristic and expected pairs against a seeded simulation, and `run` on a fake player: it stops at the
verdict, plays a block with a game that could not run once more, and refuses a directory of other DLLs. No game
runs and no scipy is needed.

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

    def test_a_test_loss_fails_outright(self):
        base, test = arms([(40000, 20000)] * 15 + [(30000, None)] + [(40000, 20000)] * 16)
        v = margin.verdict(base, test, "improve")
        self.assertEqual((v["exit"], v["losses"]), (margin.REJECT, [margin.order()[15]]))
        self.assertIn("a loss fails the test outright", v["verdict"])


class TestSprt(unittest.TestCase):
    def test_the_arithmetic(self):
        self.assertEqual(margin.bounds(), (pytest_approx(-LN19), pytest_approx(LN19)))
        self.assertEqual(margin.steps("improve"), (pytest_approx(math.log(1.2)), pytest_approx(math.log(0.8))))
        self.assertEqual(margin.steps("notworse"), (pytest_approx(math.log(1.25)),
                                                    pytest_approx(math.log(5 / 6))))
        # one block: 10 TEST better, 4 worse, 2 ties
        base, test = arms([(40000, 30000)] * 10 + [(30000, 40000)] * 4 + [(30000, 30100)] * 2)
        v = margin.verdict(base, test, "improve")
        self.assertEqual((v["better"], v["worse"], v["ties"], v["blocks"]), (10, 4, 2, 1))
        self.assertAlmostEqual(v["llr"], 10 * math.log(1.2) + 4 * math.log(0.8))     # 0.9306
        self.assertEqual(v["exit"], margin.UNDECIDED)
        self.assertIn("no decision yet after 1 blocks", v["verdict"])

    def test_the_first_block_at_a_bound_decides_and_the_rest_is_not_read(self):
        # improve: 16 better in block 1 (2.917 < ln 19 = 2.944), 1 more in block 2 crosses; block 3 not read
        base, test = arms([(40000, 30000)] * 17 + [(30000, 30000)] * 15 + [(30000, 40000)] * 16)
        v = margin.verdict(base, test, "improve")
        self.assertEqual((v["exit"], v["blocks"], v["pairs"]), (margin.ACCEPT, 2, 32))
        self.assertAlmostEqual(v["llr"], 17 * math.log(1.2))
        self.assertEqual(len(v["unread"]), 16)
        self.assertEqual(v["verdict"], "TEST is better")
        # notworse: 16 worse in a block, -16 ln(6/5) = -2.917; the 17th crosses ln(1/19)
        base, test = arms([(30000, 40000)] * 17 + [(30000, 30000)] * 15)
        v = margin.verdict(base, test, "notworse")
        self.assertEqual((v["exit"], v["blocks"], v["verdict"]), (margin.REJECT, 2, "TEST is worse"))
        # an incomplete block is not read
        base, test = arms([(40000, 30000)] * 20)
        v = margin.verdict(base, test, "improve")
        self.assertEqual((v["blocks"], v["pairs"], len(v["unread"]), v["exit"]), (1, 16, 4, margin.UNDECIDED))

    def test_no_decision_at_the_end_of_the_order(self):
        base, test = arms([(30000, 30000)] * margin.CAP_PAIRS)            # identical games: every pair a tie
        for mode in margin.MODES:
            v = margin.verdict(base, test, mode)
            self.assertEqual((v["exit"], v["ties"], v["llr"]), (margin.UNDECIDED, 200, 0.0))
            self.assertIn("NO DECISION", v["verdict"])

    def test_the_operating_characteristic_and_the_expected_pairs(self):
        for mode, (p0, p1) in margin.MODES.items():
            self.assertAlmostEqual(margin.oc_asn(p0, mode)[0], 1 - margin.ALPHA, places=6)   # L(p0) = 1 - alpha
            self.assertAlmostEqual(margin.oc_asn(p1, mode)[0], margin.BETA, places=6)        # L(p1) = beta
        # against a seeded simulation of the open-ended test (Wald ignores the overshoot: within 12 %)
        rng = random.Random(7)
        up, down = margin.steps("improve")
        lo, hi = margin.bounds()
        for p in (0.5, 0.6, 0.7):
            ns, acc = [], 0
            for _ in range(3000):
                x, n = 0.0, 0
                while lo < x < hi:
                    x += up if rng.random() < p else down
                    n += 1
                ns.append(n)
                acc += x >= hi
            L, en = margin.oc_asn(p, "improve")
            self.assertLess(abs(sum(ns) / len(ns) - en) / en, 0.12, p)
            self.assertLess(abs(acc / 3000 - (1 - L)), 0.03, p)


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
            self.assertEqual(played, [1, 2])                            # 17 better pairs decide in block 2

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
