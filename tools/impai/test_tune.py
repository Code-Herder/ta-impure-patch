#!/usr/bin/env python3
"""Tests of A3's offline tuning tools, with no game: CMA-ES (cmaes.py) on synthetic objectives whose optimum is
known, successive halving and tier A's whole search loop (tune.py) on a synthetic fight runner, tier B's race on
simulated games, and tier C's pre-registered judge and its statistics (tourney.py): the threshold, its power and
its false-positive rate, exactly and by simulation.

    .venv-undither/bin/python tools/impai/test_tune.py          (numpy: the shared venv)
"""
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import cmaes  # noqa: E402
import tablefile  # noqa: E402
import tourney  # noqa: E402
import tune  # noqa: E402

MAPS = ["m1", "m2", "m3", "m4", "m5"]


def a_table(**kw) -> tablefile.Table:
    """A table of the format's shape: A2's base shares on both surface layers, the anti-air row at 1.5."""
    t = tablefile.Table(
        base=[0.25, 0.45, 0.15, 0.0, 0.05, 0.25, 0.45, 0.15, 0.0, 0.0, 0.0, 0.0, 0.0],
        rows=[(12, 13), (7, 2), (2, 0), (0, 1), (5, 9), (6, 9), (13, 10), (3, 14)],
        gains=[1.5, 0, 0, 0, 0, 0, 0, 0], counter=1.0, air_from_t=10.0, aa_cap=0.35, tech_m=8.0)
    for k, v in kw.items():
        setattr(t, k, v)
    return t


def run_es(f, dim, seed, gens, sigma=0.3, pop=10):
    es = cmaes.CMAES(dim, pop, seed, sigma)
    for _ in range(gens):
        ys = es.ask()
        es.tell(ys, [f(es.project(y)) for y in ys])
    return es


class CMAESOnKnownOptima(unittest.TestCase):
    OPT = np.linspace(-0.6, 0.7, 9)

    def test_sphere(self):
        es = run_es(lambda x: float(np.sum((x - self.OPT) ** 2)), 9, 7, 60)
        self.assertLess(np.abs(es.mean - self.OPT).max(), 1e-2)
        es = run_es(lambda x: float(np.sum((x - self.OPT) ** 2)), 9, 7, 200)
        self.assertLess(np.abs(es.mean - self.OPT).max(), 1e-6)

    def test_ill_conditioned(self):
        """an ellipsoid of condition 10^2 along the axes: the covariance learns it"""
        w = np.logspace(0, 2, 9)
        es = run_es(lambda x: float(np.sum(w * (x - self.OPT) ** 2)), 9, 7, 200)
        self.assertLess(np.abs(es.mean - self.OPT).max(), 1e-4)

    def test_rotated(self):
        """a rotated ellipsoid: no axis-aligned search would follow it"""
        rng = np.random.Generator(np.random.PCG64(5))
        q, _ = np.linalg.qr(rng.standard_normal((9, 9)))
        h = q @ np.diag(np.logspace(0, 2, 9)) @ q.T
        es = run_es(lambda x: float((x - self.OPT) @ h @ (x - self.OPT)), 9, 3, 250)
        self.assertLess(np.abs(es.mean - self.OPT).max(), 1e-4)

    def test_optimum_outside_the_box(self):
        target = np.array([1.5, -2.0, 0.2])
        es = run_es(lambda x: float(np.sum((x - target) ** 2)), 3, 3, 100)
        np.testing.assert_allclose(es.project(es.mean), [1.0, -1.0, 0.2], atol=1e-4)

    def test_tier_a_budget_on_a_sphere(self):
        """tier A's own budget (population 10, 18 generations, sigma 0.3) takes a 9-number sphere's error from
        its start down by a factor of at least 50"""
        f = lambda x: float(np.sum((x - self.OPT) ** 2))  # noqa: E731
        es = run_es(f, 9, 11, 18)
        self.assertLess(f(es.mean), f(np.zeros(9)) / 50)

    def test_deterministic_from_its_seed(self):
        f = lambda x: float(np.sum(x ** 2))  # noqa: E731
        a, b, c = run_es(f, 4, 9, 15), run_es(f, 4, 9, 15), run_es(f, 4, 10, 15)
        self.assertTrue(np.array_equal(a.mean, b.mean) and np.array_equal(a.C, b.C) and a.sigma == b.sigma)
        self.assertFalse(np.array_equal(a.mean, c.mean))

    def test_resume_from_state(self):
        """stopped after any tell and resumed from its JSON, it draws and updates as one that never stopped"""
        f = lambda x: float(np.sum((x - 0.3) ** 2))  # noqa: E731
        whole = run_es(f, 5, 4, 12)
        es = cmaes.CMAES(5, 10, 4, 0.3)
        for _ in range(12):
            es = cmaes.CMAES.from_state(json.loads(json.dumps(es.state())))
            ys = es.ask()
            es.tell(ys, [f(es.project(y)) for y in ys])
        self.assertTrue(np.array_equal(whole.mean, es.mean) and whole.sigma == es.sigma)

    def test_refusals(self):
        es = cmaes.CMAES(3, 6, 1)
        ys = es.ask()
        with self.assertRaises(ValueError):
            es.tell(ys[:5], [0.0] * 5)
        with self.assertRaises(ValueError):
            es.tell(ys, [0.0] * 5 + [math.nan])


class SuccessiveHalving(unittest.TestCase):
    def test_rule(self):
        screen = {0: [1.0, 1.2, 0.8, 1.0], 1: [0.9, 1.1, 0.7, 1.0], 2: [-3.0, -3.1, -2.9, -3.0]}
        self.assertEqual(tune.halving(screen), [0, 1])

    def kept(self, spread, sd, seed):
        """over 400 generations of ten candidates whose true paired scores fall evenly over `spread`, in noise of
        `sd` a fight: how often the best and the worst continue, and how many continue on average"""
        rng = np.random.Generator(np.random.PCG64(seed))
        true = np.linspace(0.0, -spread, 10)
        best = worst = 0
        n = []
        for _ in range(400):
            go = tune.halving({c: list(true[c] + sd * rng.standard_normal(tune.SCREEN)) for c in range(10)})
            best += 0 in go
            worst += 9 in go
            n.append(len(go))
        return best / 400, worst / 400, float(np.mean(n))

    def test_on_noisy_candidates(self):
        """candidates far apart against the noise: the best goes on nearly always, the worst almost never, and
        few go on; candidates close together: more go on, since the screen cannot tell them apart"""
        best, worst, n = self.kept(0.25, 0.15, 2)
        self.assertGreater(best, 0.9)
        self.assertLess(worst, 0.02)
        self.assertLess(n, 3)
        best, worst, n = self.kept(0.03, 0.15, 3)
        self.assertGreater(n, 4)


class TierASearch(unittest.TestCase):
    """The whole tier A loop on a synthetic fight runner: a candidate's margin on a fight is a picture's own
    effect, minus a quadratic in its nine tuned numbers around a known optimum, plus noise. The search must end
    near the optimum, reloading its state from disk at every step as pieces across sessions do."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        tablefile.write(self.d / "base.tbl", a_table())
        self.keys = [f"p{i:02d}@{v}" for i in range(60) for v in (3000, 8000)]
        rng = np.random.Generator(np.random.PCG64(99))
        self.effect = {k: float(rng.normal(0, 0.3)) for k in self.keys}
        self.opt = np.array([1.0, 0.4, -0.3, 0.5, 0.0, 0.2, -0.5, 0.3, 2.0])
        self.noise = np.random.Generator(np.random.PCG64(7))

    def tearDown(self):
        self.tmp.cleanup()

    def margin(self, table_path, key):
        t = tablefile.read(table_path)
        v = np.array(list(t.gains) + [t.counter])
        return self.effect[key] - 0.5 * float(np.sum((v - self.opt) ** 2)) + float(self.noise.normal(0, 0.05))

    def play(self, job):
        return {f["id"]: self.margin(f["table"], f"{f['picture']}@{int(f['budget'])}") for f in job["fights"]}

    def test_converges(self):
        tune.TierA.init(self.d / "a", self.d / "base.tbl", self.keys, seed=3, pop=10, gens=18, sigma=0.3)
        pieces = fights = 0
        while True:
            t = tune.TierA(self.d / "a")
            if t.s["phase"] == "done":
                break
            job = t.job(14)
            self.assertEqual(job["W"], 28)
            fights += len(job["fights"])
            tune.TierA(self.d / "a").ingest(self.play(job))
            pieces += 1
        t = tune.TierA(self.d / "a")
        self.assertEqual(pieces, 1 + 2 * 18)
        hist = t.s["history"]
        self.assertEqual(len(hist), 18)
        for h in hist:
            self.assertTrue(1 <= h["continued"] <= 10)
            self.assertEqual(h["fights"], 10 * 40 + h["continued"] * 80)
        self.assertLess(fights, 120 + 18 * 1200 * 0.75, "the halving saves at least a quarter of the fights")
        names, centre, scale = tune.tier_a_space(tablefile.read(self.d / "base.tbl"))
        best = tablefile.read(t.top(1)[0]["table"]["path"])
        v = np.array(list(best.gains) + [best.counter])
        start = float(np.sum((centre - self.opt) ** 2))
        self.assertLess(float(np.sum((v - self.opt) ** 2)), start / 10)
        self.assertGreater(hist[-1]["best"], hist[0]["best"])
        self.assertEqual(len(t.top(4)), 4)

    def test_refuses_an_incomplete_piece(self):
        tune.TierA.init(self.d / "a", self.d / "base.tbl", self.keys, seed=3, pop=10, gens=2, sigma=0.3)
        t = tune.TierA(self.d / "a")
        res = self.play(t.job())
        res.pop(next(iter(res)))
        with self.assertRaises(tune.TuneError):
            t.ingest(res)
        p = self.d / "r.json"
        p.write_text(json.dumps({"fights": [{"id": "inc-p00@3000", "could_not_run": "lane died"}]}))
        with self.assertRaises(tune.TuneError):
            tune.read_margins(p)

    def test_the_box(self):
        base = a_table()
        _, centre, scale = tune.tier_a_space(base)
        self.assertEqual(list(scale), [1.5, 1, 1, 1, 1, 1, 1, 1, 1])
        t = tune.tier_a_table(base, [5.0] * 9)
        self.assertEqual(t.gains[0], 3.0)
        self.assertEqual(t.counter, 2.0)
        self.assertEqual(t.base, base.base)


class TierCJudge(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        tablefile.write(self.d / "tuned.tbl", a_table(counter=2.0))
        tablefile.write(self.d / "hand.tbl", a_table())
        self.reg = tourney.register(self.d / "c", self.d / "tuned.tbl", self.d / "hand.tbl", MAPS)

    def tearDown(self):
        self.tmp.cleanup()

    def results(self, wins, cap_losses=0, drop=0, could_not_run=0, wrong=0, gdi=0):
        out = self.d / "games"
        out.mkdir(exist_ok=True)
        for p in out.glob("*.json"):
            p.unlink()
        ta, tb = self.reg["tables"]["A"]["hash"], self.reg["tables"]["B"]["hash"]
        for i, gid in enumerate(self.reg["ids"][: len(self.reg["ids"]) - drop]):
            o = "A" if i < wins else ("cap" if i < wins + cap_losses else "B")
            if could_not_run and i >= len(self.reg["ids"]) - drop - could_not_run:
                o = "could_not_run"
            r = {"id": gid, "outcome": o, "end_tick": 50000, "tables": {"A": ta if i >= wrong else tb, "B": tb}}
            if i < gdi:
                r["renderer"] = "gdi"
            (out / f"{gid}.json").write_text(json.dumps(r))
        return out

    def verdict(self, **kw):
        return tourney.main(["judge", "--dir", str(self.d / "c"), str(self.results(**kw))])

    def test_registration(self):
        self.assertEqual(self.reg["n"], 400)
        self.assertEqual(self.reg["threshold"], 224)
        self.assertEqual(len(set(self.reg["ids"])), 400)
        again = tourney.register(self.d / "c", self.d / "tuned.tbl", self.d / "hand.tbl", MAPS)
        self.assertEqual(again, self.reg)
        tablefile.write(self.d / "other.tbl", a_table(counter=3.0))
        with self.assertRaises(tourney.TourneyError):
            tourney.register(self.d / "c", self.d / "other.tbl", self.d / "hand.tbl", MAPS)
        with self.assertRaises(tourney.TourneyError):
            tourney.register(self.d / "c", self.d / "tuned.tbl", self.d / "hand.tbl", ["a", "b", "c", "d", "x"])
        self.assertEqual(tourney.main(["register", "--dir", str(self.d / "c"), "--tuned", str(self.d / "other.tbl"),
                                       "--hand", str(self.d / "hand.tbl"), "--maps", ",".join(MAPS)]), 1)

    def test_verdicts(self):
        self.assertEqual(self.verdict(wins=224), 0)
        self.assertEqual(self.verdict(wins=223), 1)
        self.assertEqual(self.verdict(wins=223, cap_losses=10), 1, "a game at the cap counts against the tuned")
        self.assertEqual(self.verdict(wins=300, drop=1), 2, "a registered game unplayed: could not run")
        self.assertEqual(self.verdict(wins=300, could_not_run=1), 2)
        self.assertEqual(self.verdict(wins=300, wrong=1), 1, "a game played with another table")
        self.assertEqual(self.verdict(wins=300, gdi=1), 2, "games of two renderers")
        self.assertEqual(self.verdict(wins=224, gdi=400), 0)

    def test_ids_and_pieces(self):
        ids = tourney.game_ids("tierC", MAPS)
        g = tourney.parse_id(ids[123])
        self.assertEqual(f"tierC-{g['map']}-{g['sides']['A']}{g['sides']['B']}-s{g['start']}-{g['rep']:02d}", ids[123])
        ps = tourney.pieces(ids, 14)
        self.assertEqual(sum(p["games"] for p in ps), 400)
        self.assertTrue(all(p["W"] == 28 and p["games"] <= 42 and p["est_s"] <= 600 for p in ps))
        self.assertEqual(len(tourney.game_ids("tierB", MAPS)), 40)


class TierCStatistics(unittest.TestCase):
    def test_threshold_and_power_exactly(self):
        k = tourney.threshold(400, 0.01)
        self.assertEqual(k, 224)
        self.assertLess(tourney.binom_tail(400, 224, 0.5), 0.01)
        self.assertGreaterEqual(tourney.binom_tail(400, 223, 0.5), 0.01)
        self.assertAlmostEqual(tourney.binom_tail(400, k, 0.5), 0.00933, places=4)
        self.assertAlmostEqual(tourney.binom_tail(400, k, 0.60), 0.9534, places=3)
        self.assertAlmostEqual(tourney.binom_tail(400, k, 0.58), 0.8056, places=3)
        self.assertAlmostEqual(tourney.binom_tail(400, k, 0.55), 0.3631, places=3)

    def test_by_simulation(self):
        """40 000 simulated tier Cs at each true rate agree with the exact figures within four standard errors:
        the false-positive rate is below 1 % at 50 %, and the power is 0.95 at 60 %"""
        trials = 40000
        for p in (0.50, 0.55, 0.58, 0.60):
            exact = tourney.binom_tail(400, 224, p)
            sim = tourney.simulate(400, 224, p, trials, seed=17)
            se = math.sqrt(max(exact * (1 - exact), 1e-6) / trials)
            self.assertLess(abs(sim - exact), 4 * se, (p, sim, exact))
        self.assertLess(tourney.simulate(400, 224, 0.5, trials, seed=18), 0.0125)
        self.assertGreater(tourney.simulate(400, 224, 0.6, trials, seed=19), 0.94)

    def test_upper_bound(self):
        self.assertAlmostEqual(tourney.upper_bound(0, 10), 1 - 0.05 ** 0.1, places=6)
        self.assertEqual(tourney.upper_bound(40, 40), 1.0)
        self.assertLess(tourney.upper_bound(14, 40), 0.5, "a candidate with 14 of 40 is dropped")
        self.assertGreater(tourney.upper_bound(15, 40), 0.5, "one with 15 of 40 stays")
        from scipy.stats import beta     # the bound's closed form: the beta quantile
        for w, n in ((0, 40), (7, 40), (14, 40), (25, 40), (60, 120)):
            self.assertAlmostEqual(tourney.upper_bound(w, n), float(beta.ppf(0.95, w + 1, n - w)), places=9)


class TierBRace(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        tablefile.write(self.d / "hand.tbl", a_table())
        self.tops = []
        for i, c in enumerate((1.2, 1.4, 1.6, 1.8)):
            p = self.d / f"top{i}.tbl"
            tablefile.write(p, a_table(counter=c, base=[0.2, 0.4, 0.1, 0, 0.05, 0.2, 0.4, 0.1, 0, 0.1, 0.1, 0, 0]))
            self.tops.append(p)

    def tearDown(self):
        self.tmp.cleanup()

    def play_round(self, b, rate, rng):
        for name, games in b.games().items():
            out = b.round_dir() / name
            out.mkdir(parents=True, exist_ok=True)
            for g in games:
                win = rng.random() < rate[name]
                r = {"id": g["id"], "outcome": "cap", "end_tick": 36000,
                     "tables": {"A": g["seats"]["A"]["table_hash"], "B": g["seats"]["B"]["table_hash"]},
                     "value": {"A": {"built": 100, "live": 60 if win else 40}, "B": {"built": 100, "live": 50}}}
                (out / f"{g['id']}.json").write_text(json.dumps(r))

    def test_variants(self):
        t = tablefile.read(self.tops[0])
        up = tune.variants(t, 1.25)
        self.assertAlmostEqual(up.air_from_t, 12.5)
        self.assertAlmostEqual(up.tech_m, 10.0)
        self.assertAlmostEqual(sum(up.base), sum(t.base), places=5)
        self.assertAlmostEqual(up.base[9], 0.125, places=6)

    def test_race(self):
        b = tune.TierB.init(self.d / "b", self.d / "hand.tbl", MAPS, self.tops)
        self.assertEqual(len(b.s["cands"]), 12)
        rate = {c["name"]: 0.3 for c in b.s["cands"]}
        rate["t2p"] = 0.75
        rate["t1"] = 0.55
        rng = np.random.Generator(np.random.PCG64(4))
        ps = b.pieces(14)
        self.assertEqual(len(ps), 12)
        self.assertTrue(all(p["W"] == 28 and p["games"] == 40 for p in ps))
        while not b.s["done"]:
            self.play_round(b, rate, rng)
            v = tune.TierB(self.d / "b").ingest()
            b = tune.TierB(self.d / "b")
        self.assertEqual(v["tuned"], "t2p")
        self.assertLessEqual(b.s["played"], tune.BUDGET_B + 12 * tune.ROUND_GAMES)
        self.assertEqual(len(b.s["rounds"][0]["dropped"]) >= 8, True, b.s["rounds"][0])

    def test_refuses_other_tables(self):
        b = tune.TierB.init(self.d / "b", self.d / "hand.tbl", MAPS, self.tops[:1])
        rng = np.random.Generator(np.random.PCG64(4))
        self.play_round(b, {c["name"]: 0.5 for c in b.s["cands"]}, rng)
        p = next((b.round_dir() / "t0").glob("*.json"))
        r = json.loads(p.read_text())
        r["tables"]["B"] = "0" * 16
        p.write_text(json.dumps(r))
        with self.assertRaises(tune.TuneError):
            b.ingest()


class CapRules(unittest.TestCase):
    def test_value_rule(self):
        r = {"id": "x", "outcome": "cap", "value": {"A": {"built": 200, "live": 100}, "B": {"built": 100, "live": 49}}}
        self.assertEqual(tourney.winner(r, "value"), "A")
        r["value"]["B"]["live"] = 50
        self.assertEqual(tourney.winner(r, "value"), "B", "a tie goes to seat B, the incumbent")
        self.assertEqual(tourney.winner(r, "against_A"), "B")
        self.assertEqual(tourney.winner({"id": "y", "outcome": "A"}, "against_A"), "A")
        with self.assertRaises(tourney.TourneyError):
            tourney.winner({"id": "z", "outcome": "cap"}, "value")
        with self.assertRaises(tourney.TourneyError):
            tourney.winner({"id": "z", "outcome": "could_not_run"}, "value")

    def test_plan(self):
        ps = tune.plan(14)
        self.assertTrue(all(p["W"] == 28 for p in ps))
        self.assertEqual(sum(1 for p in ps if p["tier"] == "A"), 37)
        self.assertTrue(all(p["est_s"] <= 600 for p in ps if p["tier"] == "A"))
        self.assertFalse(any("--gpu" in p["cmd"] for p in ps))
        import runlock
        gpu = tune.plan(14, gpu=True)
        self.assertTrue(all("--gpu" in p["cmd"] and p["W"] == runlock.plan(14, gpu=True)[0]
                            for p in gpu if p["tier"] == "C"))
        self.assertTrue(all(p["W"] == 28 for p in gpu if p["tier"] == "A"))


if __name__ == "__main__":
    unittest.main()
