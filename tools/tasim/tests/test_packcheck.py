"""The packed lane's equivalence proof (tools/tasim/packcheck.py) on synthetic reference sets: a null pair
(two arms drawn from the same fights) passes, a copy shifted by 0.1 SD of the paired difference fails T2,
a pair of different jobs is refused, and THE CLUSTERS: on fights that share batches the cluster null fails
rarely where the fixed limits fail often, its T2 inflation whoever the partners are, with the power kept; with
every run its own cluster the limits are the independent ones; and each refusal.

    python3 tools/tasim/tests/test_packcheck.py
    PACKCHECK_SIZE_REPS=2000 PACKCHECK_SIZE_J=4 python3 tools/tasim/tests/test_packcheck.py Size

No game and no reference generation: the references are built here, U's self-test by check.false_alarms
as `calib.py selftest` writes it, with fewer splits (SPLITS) to keep the test short.

The few-replication tests cannot show a size of 0.0025: "at most 1 of 40" passes with probability 0.94 at a true
size of 1 %. `Size`, skipped unless PACKCHECK_SIZE_REPS is set, plays that many replications of the shared-batch
arms at K.SIGN_FLIPS flips (about 1.2 s each) and bounds T2's and T3's size: it prints each one's failures and
their one-sided 95 % Clopper-Pearson upper bound, and fails when a count is past Binomial(reps, 0.0025)'s 99.9 %
quantile. It is an offline measurement, run under the run lock, not a gate.
"""

import json
import math
import os
import statistics
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import calibref  # noqa: E402
import check  # noqa: E402
import packcheck as K  # noqa: E402

FIGHTS = 150
RUNS = 25
SPLITS = 10
FLIPS = 2000                    # the cluster tests' flips (the spec's), K.SIGN_FLIPS where named
BLOCK = {"kind": ["winrate", "distribution"], "metrics": ["winner", "ttk", "survivors", "margin"],
         "runs": RUNS, "limit": 5400, "end_units": {"0": 0, "1": 0}, "area": "group/land/t1/x",
         "exact_family": None, "los": 1, "mapping": 1, "commander_death": 0}
SD = {"ttk": 300.0, "survivors": 0.12, "margin": 0.2}
PREDFIT_PARAMS = {"fitted_set": "defcfcdb2359", "fitted_content": "0123456789ABCDEF", "theta": "00112233aabbccdd"}
APPLY = {"streams": "apply"}


def params(rng, f):
    return {"p": float(rng.uniform(0.05, 0.95)), "ttk": float(rng.uniform(600, 3000)),
            "survivors": float(rng.uniform(0.2, 0.8)), "margin": float(rng.uniform(-0.6, 0.6))}


def run_metrics(rng, par, shift):
    ttk = int(min(5400, max(1, round(rng.normal(par["ttk"] + shift * SD["ttk"] * 2 ** 0.5, SD["ttk"])))))
    return {"winner": "A" if rng.random() < par["p"] else "B",
            "ttk": {"value": ttk, "censored": False},
            "survivors": float(rng.normal(par["survivors"] + shift * SD["survivors"] * 2 ** 0.5, SD["survivors"])),
            "margin": float(rng.normal(par["margin"] + shift * SD["margin"] * 2 ** 0.5, SD["margin"]))}


def predfit_report(**over) -> dict:
    """A `predfit.py check --evaluation --json` report of the synthetic fights: the margin is the true win rate's
    lean, p - 1/2, so the predictor errs only where an arm's labels stray from the truth."""
    import numpy
    fights = numpy.random.default_rng(7)
    doc = {"evaluation": True, "params": dict(PREDFIT_PARAMS), "content": "CA6CC97C978BF7DF",
           "margins": {f"group-{f:03d}@{f:012x}": params(fights, f)["p"] - 0.5 for f in range(FIGHTS)}}
    doc.update(over)
    return doc


def make_arm(root: Path, seed: int, shift=0.0, selftest=False, perturb_salt=0, flip_winner=0.0):
    """A reference folder of FIGHTS fights at RUNS runs each, every run unpacked (a cluster of its own); the
    fights' parameters and the runs' seeds are seed-independent, the runs' draws are the arm's own."""
    import numpy
    fights = numpy.random.default_rng(7)
    draws = numpy.random.default_rng(seed)
    for f in range(FIGHTS):
        par = params(fights, f)
        sid = f"group-{f:03d}@{f:012x}"
        if f < flip_winner * FIGHTS:
            par = dict(par, p=1.0 - par["p"])
        runs = [{"index": i, "seed": f * RUNS + i, "swap": i % 2 == 1,
                 "perturbation": {"seed": f * RUNS + i + perturb_salt, "swap": i % 2 == 1, "jitter": {},
                                  "after": []},
                 "metrics": run_metrics(draws, par, shift)} for i in range(RUNS)]
        ref = {"schema": calibref.SCHEMA_REF,
               "scenario": {"id": sid, "file": f"/gen/group-{f:03d}.json", "calib": dict(BLOCK)},
               "setup": dict(APPLY), "engine": {"commit": "0" * 40},
               "shuffle": {"seed": 1000 + f, "count": 200}, "runs": runs, "selftest": {}}
        if selftest:
            with mock.patch.object(check, "SPLITS", SPLITS):
                ref["selftest"] = {"false_alarms": check.false_alarms(ref)}
        d = root / sid
        d.mkdir(parents=True)
        (d / "build.json").write_text(json.dumps(ref))


# ---- batch-shared arms, in memory ------------------------------------------------------------------

RHO = 0.3
K8 = 8
SD_D = 300.0                    # the paired difference's SD on ttk


def shared_arms(seed, fights=200, runs=25, rho=RHO, mixed=False, packed=True, shift=0.0, shifted=(), big=0.0,
                winner=False):
    """(U, P) in memory, {sid: reference}. Round i puts the fights in batches of K8 -- the same eight every round,
    or with `mixed` a new draw each round -- and each pair's difference d = P - U, in SD_D, is a batch-common part
    of correlation rho plus noise: d = sqrt(rho)·g_batch + sqrt(1 - rho)·e + shift (+ big on the fights in
    `shifted`). With `winner`, P's and U's winners instead: A when a latent normal is above the fight's
    threshold, P's latent carrying the batch's g, so both arms win at the fight's own rate. `packed` False makes
    every P run a cluster of its own."""
    import numpy
    rng = numpy.random.default_rng(seed)
    block = {"kind": ["distribution"], "metrics": ["winner" if winner else "ttk"]}
    u, p = {}, {}
    rates = rng.uniform(0.1, 0.9, fights)
    orders = [rng.permutation(fights) if mixed else numpy.arange(fights) for _ in range(runs)]
    g = rng.normal(size=(runs, fights // K8 + 1))
    for f in range(fights):
        sid = f"syn-{f:03d}"
        u[sid] = {"scenario": {"id": sid, "calib": block}, "setup": dict(APPLY), "runs": [],
                  "shuffle": {"seed": 1000 + f, "count": 200}}
        p[sid] = {"scenario": {"id": sid, "calib": block}, "setup": dict(APPLY), "runs": []}
    for i in range(runs):
        for pos, f in enumerate(orders[i]):
            sid, b = f"syn-{f:03d}", pos // K8
            common = {"index": i, "seed": f * runs + i, "swap": i % 2 == 1,
                      "perturbation": {"seed": f * runs + i}}
            if winner:
                thr = statistics.NormalDist().inv_cdf(1 - float(rates[f]))
                lu = rng.normal()
                lp = rho ** 0.5 * g[i, b] + (1 - rho) ** 0.5 * rng.normal()
                mu = {"winner": "A" if lu > thr else "B"}
                mp = {"winner": "A" if lp > thr else "B"}
            else:
                d = rho ** 0.5 * g[i, b] + (1 - rho) ** 0.5 * rng.normal() + shift + (big if f in shifted else 0.0)
                ttk = 2000.0 + 300.0 * rng.normal()
                mu = {"ttk": {"value": ttk, "censored": False}}
                mp = {"ttk": {"value": ttk + SD_D * d, "censored": False}}
            u[sid]["runs"].append({**common, "metrics": mu})
            prow = {**common, "metrics": mp}
            if packed:
                prow["pack"] = {"batch_wire": f"w{i}:{b}", "batch_seed": 10 ** 6 + i * 1000 + b}
            p[sid]["runs"].append(prow)
    return u, p


def t23(u, p, flips=FLIPS, metric="ttk"):
    prs = K.pairs(u, p)
    n, cols = K.clusters(u, p, prs)
    F = K.flip_matrix(n, flips)
    fights = K.per_fight(prs, cols, F)
    return K.t2(fights)[metric], K.t3(fights)[metric]


class Limits(unittest.TestCase):
    def test_the_specs_binomial_quantiles(self):
        self.assertEqual(K.binom_ppf(0.99, 284, 0.03), 16)
        self.assertEqual(K.binom_ppf(0.99, 284, 0.05), 23)
        self.assertEqual(K.binom_ppf(0.99, 284, 0.06), 27)
        self.assertEqual(K.binom_ppf(0.9975, 284, 0.01), 9)

    def test_s_max_is_the_two_sided_quantile(self):
        self.assertAlmostEqual(K.z_of(0.0025 / 2), K.S_MAX, places=2)

    def test_a_fight_that_never_differs_has_z_zero(self):
        F = K.flip_matrix(25, FLIPS)
        d = [0.0] * 25
        M = (F * 0.0).mean(axis=1)
        p1, p2 = K.sign_flip(d, M)
        self.assertAlmostEqual(K.z_of(p1), 0.0, places=6)
        self.assertEqual(p2, 1.0)
        num2, p2b = K.null_ranks(d, M)
        self.assertTrue((K.z_table(2 * FLIPS)[num2] == 0.0).all())
        self.assertTrue((p2b == 1.0).all())

    def test_the_null_ranks_are_sign_flips_rule_on_each_row(self):
        import numpy
        F = K.flip_matrix(25, 300)
        d = numpy.random.default_rng(3).normal(size=25)
        M = (F * d).mean(axis=1)
        num2, p2b = K.null_ranks(d, M)
        for b in (0, 7, 151, 299):
            rest = numpy.delete(M, b)
            above = int((rest > M[b] + 1e-12 * max(1.0, abs(d).max())).sum())
            tie = int((abs(rest - M[b]) <= 1e-12 * max(1.0, abs(d).max())).sum())
            self.assertEqual(num2[b], 2 * above + tie + 1)
            self.assertAlmostEqual(p2b[b], (int((abs(rest) >= abs(M[b]) - 1e-12).sum()) + 1) / 300)


class GradeSwaps(unittest.TestCase):
    def test_every_row_is_grade_runs_on_the_exchanged_runs(self):
        import numpy
        rng = numpy.random.default_rng(11)
        par = params(rng, 0)
        e = [run_metrics(rng, par, 0.0) for _ in range(RUNS)]
        t = [run_metrics(rng, par, 0.3) for _ in range(RUNS)]
        swaps = [i % 2 == 1 for i in range(RUNS)]
        shuffle = {"seed": 99, "count": 200}
        ex = rng.random((6, RUNS)) < 0.5
        ex[0] = False
        got = check.grade_swaps(BLOCK, e, t, shuffle, swaps, ex)
        for b in range(len(ex)):
            eb = [t[i] if ex[b, i] else e[i] for i in range(RUNS)]
            tb = [e[i] if ex[b, i] else t[i] for i in range(RUNS)]
            for row in check.grade_runs(BLOCK, eb, tb, shuffle, (swaps, swaps)):
                self.assertEqual(bool(got[row["metric"]][b]), row["verdict"] == "out", (b, row["metric"]))


class Arms(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        cls.u, cls.p, cls.shifted, cls.other, cls.flipped = (root / n for n in ("u", "p", "shifted", "other",
                                                                                 "flipped"))
        make_arm(cls.u, 1, selftest=True)
        make_arm(cls.p, 2)
        make_arm(cls.shifted, 2, shift=0.1)
        make_arm(cls.other, 2, perturb_salt=1)
        make_arm(cls.flipped, 2, flip_winner=0.1)
        cls.pf = root / "predfit.json"
        cls.pf.write_text(json.dumps(predfit_report()))
        cls.null = K.run(cls.u, cls.p, cls.pf, cls.pf)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_the_null_pair_passes(self):
        self.assertEqual(K.verdict(self.null), 0, json.dumps(self.null, indent=1, default=str)[:3000])

    def test_independence_gives_the_independent_limits(self):
        """Every run its own cluster, K.SIGN_FLIPS flips: T2's limit is the normal one and T3's the Binomial's.
        Not on the winner: a 0/0.5/1 difference takes few p values, and a fight whose p cannot fall below
        P_FIGHT never counts, so its null count is lower than the Binomial's, which no data could reach."""
        self.assertEqual((self.null["clusters"], self.null["flips"]), (FIGHTS * RUNS, K.SIGN_FLIPS))
        for m, r in self.null["T2"].items():
            self.assertLess(abs(r["limit"] - K.S_MAX), 0.2, (m, r))
            self.assertLess(abs(r["inflation"] - 1.0), 0.15, (m, r))
        for m in ("ttk", "survivors", "margin"):
            r = self.null["T3"][m]
            self.assertLessEqual(abs(r["limit"] - K.binom_ppf(K.T3_Q, FIGHTS, K.P_FIGHT)), 1, (m, r))
        self.assertLessEqual(self.null["T3"]["winner"]["limit"], self.null["T3"]["winner"]["binomial"])

    def test_t1_names_the_binomial_beside_its_limit_with_a_selftest(self):
        for m, r in self.null["T1"].items():
            self.assertIsNotNone(r["alpha"], m)
            self.assertEqual(r["binomial"], K.binom_ppf(K.T1_Q, FIGHTS, r["alpha"]))
        bare = K.run(self.p, self.u, self.pf, self.pf, flips=FLIPS)
        self.assertTrue(all(r["binomial"] is None for r in bare["T1"].values()))

    def test_the_shifted_copy_fails_t2(self):
        res = K.run(self.u, self.shifted, self.pf, self.pf)
        self.assertEqual(K.verdict(res), 1)
        for m in ("ttk", "survivors", "margin"):
            self.assertFalse(res["T2"][m]["pass"], (m, res["T2"][m]))
            self.assertGreater(res["T2"][m]["S"], 0)
        self.assertTrue(res["T2"]["winner"]["pass"])

    def test_t4_without_predfit_could_not_run(self):
        res = K.run(self.u, self.p, flips=FLIPS)
        self.assertEqual(K.verdict(res), 2)

    def test_predfits_errors_are_the_labels_alone(self):
        """The null pair's errors differ within the exchanges' limit; fights whose packed labels turned over
        raise P's errors past it, with the same margins in both reports."""
        r = self.null["T4"]["predfit"]
        self.assertTrue(r["pass"], r)
        self.assertEqual(r["fights"], FIGHTS)
        res = K.run(self.u, self.flipped, self.pf, self.pf, flips=FLIPS)
        r = res["T4"]["predfit"]
        self.assertGreater(r["errors_p"] - r["errors_u"], r["limit"], r)
        self.assertEqual(K.verdict(res), 1)

    def test_two_reports_of_one_fight_must_agree(self):
        """Other coefficients, a fight's other margin or a fight with none: could not run, as is a report
        without margins (a predfit from before --evaluation)."""
        root = Path(self.tmp.name)
        bad = {"params": predfit_report(params=dict(PREDFIT_PARAMS, theta="ffffffffffffffff")),
               "content": predfit_report(content="5E2B2D5878EA9112"),
               "margin": predfit_report(), "gone": predfit_report()}
        sid = "group-003@000000000003"
        bad["margin"]["margins"][sid] += 0.01
        del bad["gone"]["margins"][sid]
        bad["old"] = {"halves": {"check": {"fights": 100, "decisive": 60, "agree": 58}}, "kinds": {}}
        for name, doc in bad.items():
            f = root / f"predfit-{name}.json"
            f.write_text(json.dumps(doc))
            with self.assertRaises(K.CouldNotRun, msg=name):
                K.run(self.u, self.p, self.pf, f, flips=FLIPS)

    def test_different_jobs_are_refused(self):
        with self.assertRaises(K.CouldNotRun):
            K.run(self.u, self.other, self.pf, self.pf)

    def test_two_runs_give_the_same_results(self):
        a = K.run(self.u, self.p, self.pf, self.pf, flips=FLIPS)
        b = K.run(self.u, self.p, self.pf, self.pf, flips=FLIPS)
        self.assertEqual(json.dumps(a, sort_keys=True), json.dumps(b, sort_keys=True))


class Clusters(unittest.TestCase):
    """THE CLUSTERS on arms whose fights share batches: 200 fights x 25 runs at K = 8, rho 0.3."""

    def test_shared_batches_hold_alpha_where_the_fixed_limit_does_not(self):
        import numpy
        fail2 = fail3 = 0
        s_alone = []
        for rep in range(40):
            t2, t3 = t23(*shared_arms(100 + rep))
            fail2 += not t2["pass"]
            fail3 += not t3["pass"]
            t2_alone, _t3 = t23(*shared_arms(100 + rep, packed=False))
            s_alone.append(t2_alone["S"])
        self.assertLessEqual(fail2, 1)
        self.assertLessEqual(fail3, 1)
        # the S the independent null would grade against 3.02: its variance is 1 + (K - 1)·rho, not 1
        self.assertGreater(float(numpy.var(s_alone)), 2.0)

    def test_the_partners_do_not_matter(self):
        """Fixed partners and partners drawn afresh every round inflate S's null alike: by (K - 1)·rho, the runs
        a run shares its batch with, whichever fights they belong to."""
        fixed = t23(*shared_arms(7))[0]["inflation"]
        mixed = t23(*shared_arms(7, mixed=True))[0]["inflation"]
        want = 1 + (K8 - 1) * RHO
        self.assertLess(abs(fixed - mixed) / max(fixed, mixed), 0.15, (fixed, mixed))
        for v in (fixed, mixed):
            self.assertLess(abs(v - want) / want, 0.20, (v, want))

    def test_the_power_is_kept(self):
        t2, _t3 = t23(*shared_arms(9, shift=0.25))
        self.assertFalse(t2["pass"], t2)
        _t2, t3 = t23(*shared_arms(9, shifted=set(range(0, 200, 17)), big=1.2))
        self.assertEqual(len(range(0, 200, 17)), 12)
        self.assertFalse(t3["pass"], t3)

    def test_t1_and_t4_hold_alpha_on_shared_winners(self):
        fail1 = fail4 = 0
        for rep in range(20):
            u, p = shared_arms(300 + rep, winner=True)
            prs = K.pairs(u, p)
            n, cols = K.clusters(u, p, prs)
            F = K.flip_matrix(n, FLIPS)
            fail1 += not K.t1(u, prs, cols, F)["winner"]["pass"]
            fail4 += not K.t4(prs, cols, F)["labels_pass"]
        self.assertLessEqual(fail1, 1)
        self.assertLessEqual(fail4, 1)

    def test_a_cell_without_its_batch_wire_is_refused(self):
        u, p = shared_arms(1, fights=16, runs=4)
        del p["syn-003"]["runs"][2]["pack"]["batch_wire"]
        with self.assertRaisesRegex(K.CouldNotRun, "batch_wire"):
            K.clusters(u, p, K.pairs(u, p))

    def test_two_runs_of_a_fight_in_one_batch_are_refused(self):
        u, p = shared_arms(1, fights=16, runs=4)
        p["syn-003"]["runs"][2]["pack"]["batch_wire"] = p["syn-003"]["runs"][1]["pack"]["batch_wire"]
        with self.assertRaisesRegex(K.CouldNotRun, "two of its runs in one batch"):
            K.clusters(u, p, K.pairs(u, p))

    def test_an_arm_without_its_streams_is_refused_after_the_fixed_seeds(self):
        u, p = shared_arms(1, fights=16, runs=4)
        del u["syn-005"]["setup"]["streams"]
        with self.assertRaisesRegex(K.CouldNotRun, "no engine commit"):
            K.clusters(u, p, K.pairs(u, p))
        u["syn-005"]["engine"] = {"commit": "abc"}
        with mock.patch.object(K, "descends_from_fixed", return_value=True):
            with self.assertRaisesRegex(K.CouldNotRun, "wind schedule"):
                K.clusters(u, p, K.pairs(u, p))
        with mock.patch.object(K, "descends_from_fixed", return_value=False):
            K.clusters(u, p, K.pairs(u, p))                  # a clock-seeded engine's launches

    def test_the_fixed_seeds_commit_descends_from_itself(self):
        self.assertTrue(K.descends_from_fixed(K.STREAMS_FIXED))
        with self.assertRaises(K.CouldNotRun):
            K.descends_from_fixed("0" * 40)

    def test_clusters_that_share_a_seed_are_one(self):
        u, p = shared_arms(1, fights=16, runs=4)
        n, _cols = K.clusters(u, p, K.pairs(u, p))
        self.assertEqual(n, 4 * 2)                             # 16 fights in batches of 8, four rounds
        p["syn-009"]["runs"][3]["pack"]["batch_seed"] = p["syn-001"]["runs"][0]["pack"]["batch_seed"]
        n2, cols = K.clusters(u, p, K.pairs(u, p))
        self.assertEqual(n2, n - 1)
        self.assertEqual(cols["syn-009"][3], cols["syn-001"][0])


def _size_rep(seed):
    t2, t3 = t23(*shared_arms(10 ** 5 + seed), flips=K.SIGN_FLIPS)
    return (not t2["pass"], not t3["pass"])


def binom_cdf(k: int, n: int, p: float) -> float:
    return sum(math.exp(math.lgamma(n + 1) - math.lgamma(i + 1) - math.lgamma(n - i + 1)
                        + i * math.log(p) + (n - i) * math.log1p(-p)) for i in range(k + 1))


def upper_95(k: int, n: int) -> float:
    """The one-sided 95 % Clopper-Pearson upper bound of a rate seen k times in n."""
    lo, hi = k / n, 1.0
    for _ in range(60):
        mid = (lo + hi) / 2
        lo, hi = (mid, hi) if binom_cdf(k, n, mid) > 0.05 else (lo, mid)
    return hi


@unittest.skipUnless(os.environ.get("PACKCHECK_SIZE_REPS"), "an offline measurement: set PACKCHECK_SIZE_REPS")
class Size(unittest.TestCase):
    def test_the_size_on_shared_batches(self):
        from multiprocessing import Pool
        reps = int(os.environ["PACKCHECK_SIZE_REPS"])
        with Pool(int(os.environ.get("PACKCHECK_SIZE_J", "1"))) as pool:
            got = pool.map(_size_rep, range(reps))
        limit = K.binom_ppf(0.999, reps, K.T2_ALPHA)
        for i, test in enumerate(("T2", "T3")):
            fails = sum(g[i] for g in got)
            print(f"{test}: {fails} of {reps} replications failed, size <= {upper_95(fails, reps):.4f} (95 %), "
                  f"stated {K.T2_ALPHA if test == 'T2' else 1 - K.T3_Q:.4f}")
            self.assertLessEqual(fails, limit, test)


if __name__ == "__main__":
    unittest.main()
