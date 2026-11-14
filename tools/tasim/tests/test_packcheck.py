"""The packed lane's equivalence proof (tools/tasim/packcheck.py) on synthetic reference sets: a null pair
(two arms drawn from the same fights) passes, a copy shifted by 0.1 SD of the paired difference fails T2,
a pair of different jobs is refused, and the Binomial limits are the spec's.

    python3 tools/tasim/tests/test_packcheck.py

No game and no reference generation: the references are built here, U's self-test by check.false_alarms
as `calib.py selftest` writes it, with fewer splits (SPLITS) to keep the test short.
"""

import json
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
BLOCK = {"kind": ["winrate", "distribution"], "metrics": ["winner", "ttk", "survivors", "margin"],
         "runs": RUNS, "limit": 5400, "end_units": {"0": 0, "1": 0}, "area": "group/land/t1/x",
         "exact_family": None, "los": 1, "mapping": 1, "commander_death": 0}
SD = {"ttk": 300.0, "survivors": 0.12, "margin": 0.2}
PREDFIT_PASS = {"halves": {"check": {"fights": 100, "decisive": 60, "agree": 58}}, "kinds": {}}


def params(rng, f):
    return {"p": float(rng.uniform(0.05, 0.95)), "ttk": float(rng.uniform(600, 3000)),
            "survivors": float(rng.uniform(0.2, 0.8)), "margin": float(rng.uniform(-0.6, 0.6))}


def run_metrics(rng, par, shift):
    ttk = int(min(5400, max(1, round(rng.normal(par["ttk"] + shift * SD["ttk"] * 2 ** 0.5, SD["ttk"])))))
    return {"winner": "A" if rng.random() < par["p"] else "B",
            "ttk": {"value": ttk, "censored": False},
            "survivors": float(rng.normal(par["survivors"] + shift * SD["survivors"] * 2 ** 0.5, SD["survivors"])),
            "margin": float(rng.normal(par["margin"] + shift * SD["margin"] * 2 ** 0.5, SD["margin"]))}


def make_arm(root: Path, seed: int, shift=0.0, selftest=False, perturb_salt=0):
    """A reference folder of FIGHTS fights at RUNS runs each; the fights' parameters are seed-independent,
    the runs' draws are the arm's own."""
    import numpy
    fights = numpy.random.default_rng(7)
    draws = numpy.random.default_rng(seed)
    for f in range(FIGHTS):
        par = params(fights, f)
        sid = f"group-{f:03d}@{f:012x}"
        runs = [{"index": i, "seed": i, "swap": i % 2 == 1,
                 "perturbation": {"seed": i + perturb_salt, "swap": i % 2 == 1, "jitter": {}, "after": []},
                 "metrics": run_metrics(draws, par, shift)} for i in range(RUNS)]
        ref = {"schema": calibref.SCHEMA_REF,
               "scenario": {"id": sid, "file": f"/gen/group-{f:03d}.json", "calib": dict(BLOCK)},
               "shuffle": {"seed": 1000 + f, "count": 200}, "runs": runs, "selftest": {}}
        if selftest:
            with mock.patch.object(check, "SPLITS", SPLITS):
                ref["selftest"] = {"false_alarms": check.false_alarms(ref)}
        d = root / sid
        d.mkdir(parents=True)
        (d / "build.json").write_text(json.dumps(ref))


class Limits(unittest.TestCase):
    def test_the_specs_binomial_quantiles(self):
        self.assertEqual(K.binom_ppf(0.99, 284, 0.03), 16)
        self.assertEqual(K.binom_ppf(0.99, 284, 0.05), 23)
        self.assertEqual(K.binom_ppf(0.99, 284, 0.06), 27)
        self.assertEqual(K.binom_ppf(0.9975, 284, 0.01), 9)

    def test_s_max_is_the_two_sided_quantile(self):
        self.assertAlmostEqual(K.z_of(0.0025 / 2), K.S_MAX, places=2)

    def test_a_fight_that_never_differs_has_z_zero(self):
        p1, p2 = K.sign_flip([0.0] * 25, "x")
        self.assertAlmostEqual(K.z_of(p1), 0.0, places=6)
        self.assertEqual(p2, 1.0)


class Arms(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        cls.u, cls.p, cls.shifted, cls.other = (root / n for n in ("u", "p", "shifted", "other"))
        make_arm(cls.u, 1, selftest=True)
        make_arm(cls.p, 2)
        make_arm(cls.shifted, 2, shift=0.1)
        make_arm(cls.other, 2, perturb_salt=1)
        cls.pf = root / "predfit.json"
        cls.pf.write_text(json.dumps(PREDFIT_PASS))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_the_null_pair_passes(self):
        res = K.run(self.u, self.p, self.pf, self.pf)
        self.assertEqual(K.verdict(res), 0, json.dumps(res, indent=1, default=str)[:3000])

    def test_the_shifted_copy_fails_t2(self):
        res = K.run(self.u, self.shifted, self.pf, self.pf)
        self.assertEqual(K.verdict(res), 1)
        for m in ("ttk", "survivors", "margin"):
            self.assertFalse(res["T2"][m]["pass"], (m, res["T2"][m]))
            self.assertGreater(res["T2"][m]["S"], 0)
        self.assertTrue(res["T2"]["winner"]["pass"])

    def test_t4_without_predfit_could_not_run(self):
        res = K.run(self.u, self.p)
        self.assertEqual(K.verdict(res), 2)

    def test_a_failed_predfit_fails(self):
        bad = Path(self.tmp.name) / "predfit-bad.json"
        bad.write_text(json.dumps({"halves": {"check": {"fights": 100, "decisive": 60, "agree": 50}},
                                   "kinds": {}}))
        res = K.run(self.u, self.p, self.pf, bad)
        self.assertEqual(K.verdict(res), 1)

    def test_different_jobs_are_refused(self):
        with self.assertRaises(K.CouldNotRun):
            K.run(self.u, self.other, self.pf, self.pf)

    def test_u_without_its_selftest_could_not_run(self):
        with self.assertRaises(K.CouldNotRun):
            K.run(self.p, self.u, self.pf, self.pf)


if __name__ == "__main__":
    unittest.main()
