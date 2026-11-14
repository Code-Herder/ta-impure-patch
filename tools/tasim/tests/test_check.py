"""The checker's rules at their edges, its exit codes through a fake tasim, and staleness on a
synthetic git history.

    .venv-undither/bin/python tools/tasim/tests/test_check.py      (the rules need numpy)
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

import numpy

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(TREE / "tools" / "replay"))
import calib  # noqa: E402
import calibref  # noqa: E402
import check  # noqa: E402
import metrics  # noqa: E402
import perturb  # noqa: E402

FX = 1 << 16
DUEL = TREE / "scenarios" / "calib" / "duel-armpw-corak.json"
VALUES = {"ARMPW": {"metal": 45.0, "energy": 900.0, "maxdamage": 300, "bmcode": 1,
                    "footprint": [2, 2], "id": 5},
          "CORAK": {"metal": 40.0, "energy": 600.0, "maxdamage": 250, "bmcode": 1,
                    "footprint": [2, 2], "id": 6}}
SHUFFLE = {"seed": 1, "count": calibref.SHUFFLES}


def dist(e, t, seed=1, strata=None):
    """rule_distribution on one metric, `seed` the reference's shuffle seed."""
    return check.rule_distribution(e, t, "ttk", dict(SHUFFLE, seed=seed), strata)


def mass_point(rng, tail):
    """50 survivors: 70 % at 0.28, the rest over 0.28 − 0.04·tail .. 0.28 + 0.12·tail."""
    return numpy.where(rng.random(50) < 0.7, 0.28, 0.28 + tail * rng.uniform(-0.04, 0.12, 50))


class Rules(unittest.TestCase):

    def test_winrate_in_and_out_of_two_standard_errors(self):
        ok, margin, pe, pt = check.rule_winrate(["A"] * 31 + ["B"] * 19, ["A"] * 26 + ["B"] * 24)
        self.assertTrue(ok)
        self.assertAlmostEqual(pe, 0.62)
        ok, _m, _pe, _pt = check.rule_winrate(["A"] * 40 + ["B"] * 10, ["A"] * 20 + ["B"] * 30)
        self.assertFalse(ok)

    def test_winrate_with_no_spread_needs_equality(self):
        self.assertTrue(check.rule_winrate(["A"] * 50, ["A"] * 50)[0])
        ok, margin, _pe, _pt = check.rule_winrate(["A"] * 50, ["B"] * 50)
        # p̄ = 0.5 here, so SE is not 0: a full reversal is far out of band
        self.assertFalse(ok)
        self.assertGreater(margin, 1)

    def test_a_draw_scores_half(self):
        _ok, _m, pe, pt = check.rule_winrate(["draw"] * 10, ["A"] * 5 + ["B"] * 5)
        self.assertEqual((pe, pt), (0.5, 0.5))

    def test_distribution_median_shape_and_spread(self):
        e = list(range(100, 200))
        self.assertTrue(dist(e, list(range(100, 200)))[0])
        ok, margin = dist(e, list(range(300, 400)))[:2]                  # P50 outside
        self.assertFalse(ok)
        self.assertGreater(margin["outside"], 0)
        ok, margin = dist(e, [150] * 100)[:2]                            # median in, no spread
        self.assertFalse(ok)
        self.assertEqual(margin["outside"], 0)
        self.assertGreater(margin["mad"], margin["mad_limit"])
        ok, margin = dist(e, list(range(-100, 400, 5)))[:2]              # median in, 5x wider
        self.assertFalse(ok)
        self.assertGreater(margin["mad"], margin["mad_limit"])

    def test_distribution_with_no_engine_spread(self):
        """Identical engine runs: tasim's 10 runs off the engine's value all land on one side, which
        almost no reshuffle of the pool does."""
        self.assertTrue(dist([600] * 50, [600] * 50)[0])
        ok, margin = dist([600] * 50, [600] * 40 + [599] * 10)[:2]
        self.assertFalse(ok)
        self.assertEqual(margin["outside"], 0)
        self.assertGreater(margin["cvm"], margin["cvm_limit"])

    def test_cvm_is_its_definition_with_ties(self):
        """The statistic against the formula read literally: both distribution functions at every
        one of the N pooled runs, ties included."""
        rng = numpy.random.default_rng(7)
        a = rng.integers(0, 5, 30).astype(float)
        b = rng.integers(1, 6, 20).astype(float)
        pooled = numpy.r_[a, b]
        literal = sum(((a <= z).mean() - (b <= z).mean()) ** 2 for z in pooled) * 30 * 20 / 50 ** 2
        mask = numpy.r_[numpy.ones(30, bool), numpy.zeros(20, bool)]
        self.assertAlmostEqual(float(check.cvm(pooled, mask)[0]), literal, places=12)

    def test_the_spread_statistic_is_its_definition(self):
        rng = numpy.random.default_rng(8)
        a, b = rng.normal(0, 1, 30), rng.normal(0, 2, 20)
        med = numpy.median(numpy.r_[a, b])
        literal = abs(numpy.abs(a - med).mean() - numpy.abs(b - med).mean())
        mask = numpy.r_[numpy.ones(30, bool), numpy.zeros(20, bool)]
        self.assertAlmostEqual(float(check.mad_gap(numpy.r_[a, b], mask)[0]), literal, places=12)

    def test_a_70_percent_mass_point_alarms_about_5_percent(self):
        """Two samples of 50 from one distribution, 70 % of it at 0.28 and the rest over
        0.24..0.40 (the duel's survivors' shape): the rule's OUT rate, median rule included, over
        400 seeded draws. MEASURED: 17 of 400 (0.043)."""
        rng = numpy.random.default_rng(11)
        out = sum(not dist(mass_point(rng, 1), mass_point(rng, 1), seed=i)[0] for i in range(400))
        self.assertLessEqual(out / 400, 0.08)

    def test_a_seat_effect_reshuffled_by_swap_alarms_about_5_percent(self):
        """Engine and tasim alike, the swapped runs two standard deviations above the unswapped:
        reshuffled within each swap, OUT in at most 8 % of 400 seeded draws. MEASURED: 26 of 400
        (0.065)."""
        rng = numpy.random.default_rng(15)
        swaps = [False] * 25 + [True] * 25
        side = lambda: numpy.r_[rng.normal(100, 10, 25), rng.normal(120, 10, 25)]  # noqa: E731
        out = sum(not dist(side(), side(), seed=i, strata=(swaps, swaps))[0] for i in range(400))
        self.assertLessEqual(out / 400, 0.08)

    def test_a_10_percent_shift_of_the_median_is_out_at_50(self):
        """Engine and tasim normal with a 10 % coefficient of variation, tasim's median 10 % higher,
        50 runs a side: OUT in at least 95 of 100 seeded draws. MEASURED: 99 of 100."""
        rng = numpy.random.default_rng(12)
        out = sum(not dist(rng.normal(100, 10, 50), rng.normal(110, 10, 50), seed=i)[0]
                  for i in range(100))
        self.assertGreaterEqual(out, 95)

    def test_a_doubled_spread_is_out_at_50(self):
        """Tasim with the same median and twice the spread, 50 runs a side, 200 seeded draws per
        shape: OUT in at least 95 % for a normal, a uniform and a lognormal shape. MEASURED: 197,
        200 and 197 of 200. Doubling only the tail of the 70 % mass point (`mass_point(rng, 2)`) is
        OUT in 48 of 200 (0.24): seven runs in ten do not move, so no test bounds that case.
        The spread test carries it: Cramér–von Mises alone weighs the middle of the distributions,
        where a change of scale moves them least."""
        shapes = {"normal": lambda r, k: r.normal(100, 10 * k, 50),
                  "uniform": lambda r, k: 100 + k * r.uniform(-17, 17, 50),
                  "lognormal": lambda r, k: 100 * numpy.exp(r.normal(0, 0.1 * k, 50))}
        for n, (name, draw) in enumerate(shapes.items()):
            rng = numpy.random.default_rng(13 + n)
            out = sum(not dist(draw(rng, 1), draw(rng, 2), seed=i)[0] for i in range(200))
            self.assertGreaterEqual(out / 200, 0.95, name)

    def test_the_limits_reproduce_from_the_seed(self):
        rng = numpy.random.default_rng(14)
        values = numpy.r_[rng.normal(0.0, 1.0, 50), rng.normal(0.0, 1.0, 50)]
        first = numpy.r_[numpy.ones(50, bool), numpy.zeros(50, bool)]
        swaps = [False, True] * 50
        shuffle = calibref.shuffle_of(calibref.sha256_file(DUEL))
        self.assertEqual(shuffle, calibref.shuffle_of(calibref.sha256_file(DUEL)))
        limits = check.permutation_limits(values, first, swaps, "ttk", shuffle)
        self.assertEqual(limits, check.permutation_limits(values, first, swaps, "ttk", shuffle))
        other = dict(shuffle, seed=shuffle["seed"] ^ 1)
        self.assertNotEqual(limits, check.permutation_limits(values, first, swaps, "ttk", other))

    def test_a_censored_value_enters_as_its_limit(self):
        e = [{"ttk": {"value": 5400, "censored": True}}] * 10
        rows = check.grade_runs({"metrics": ["ttk"]}, e, e, SHUFFLE)
        self.assertEqual(rows[0]["engine"]["q"], [5400.0, 5400.0, 5400.0])
        self.assertEqual(rows[0]["verdict"], "in")

    def test_exact_has_no_tolerance(self):
        self.assertEqual(check.rule_exact([[1, "a", 300, 150]], [[1, "a", 300, 150]]), (True, None))
        ok, diff = check.rule_exact([[1, "a", 300, 150]], [[1, "a", 300, 150.0001]])
        self.assertFalse(ok)
        self.assertEqual(diff[0], 0)

    def test_storage_compares_the_passes_not_the_applied_row(self):
        """Two traces that agree at t = 0 (the applied state) and differ at the first economy
        pass differ in the storage family."""
        head = {"schema": "ta-calib-trace/1", "limit": 100, "units": {}}
        p = lambda emax: [0.0] * 4 + [emax, 500.0, 0.0, 0.0]     # noqa: E731
        a = [head, {"t": 0, "p": {"A": p(1000.0), "B": p(1000.0)}},
             {"t": 29, "p": {"A": p(1100.0), "B": p(1000.0)}}]
        b = [head, {"t": 0, "p": {"A": p(1000.0), "B": p(1000.0)}},
             {"t": 29, "p": {"A": p(1000.0), "B": p(1000.0)}}]
        self.assertEqual(metrics.exact_series(a, "storage"),
                         [[29, "A", 1100.0, 500.0], [29, "B", 1000.0, 500.0]])
        ok, diff = check.rule_exact(metrics.exact_series(a, "storage"),
                                    metrics.exact_series(b, "storage"))
        self.assertFalse(ok)
        self.assertEqual(diff[0], 0)

    def test_the_predictor_leaves_the_even_fights_out(self):
        ok, agree, n, left = check.rule_predictor([(0.9, "A"), (0.1, "B"), (0.5, "B"), (0.95, "B")])
        self.assertEqual((n, left), (3, 1))
        self.assertFalse(ok)
        self.assertAlmostEqual(agree, 2 / 3)


def trace(winner):
    """A duel trace whose winner is `winner`: the loser dies at t = 100."""
    loser = "b" if winner == "A" else "a"
    head = {"schema": "ta-calib-trace/1", "limit": 5400, "source": "tasim",
            "units": {"a": {"type": "ARMPW", "side": "A"}, "b": {"type": "CORAK", "side": "B"}}}
    return [head, {"t": 0, "ev": [["create", "a", "ARMPW", "A", 1, 0, 0, 0],
                                  ["create", "b", "CORAK", "B", 1, 0, 0, 0]],
                   "u": {"a": [0, 0, 0, 300, 0.0], "b": [0, 0, 0, 250, 0.0]}},
            {"t": 100, "ev": [["death", loser, None]]}]


class ExitCodes(unittest.TestCase):
    """check() end to end on a reference written here, against the fake tasim."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        os.environ["TACALIB_DIR"] = self.tmp.name
        self.arena = calibref.arena()
        self.sid = calibref.scenario_id(DUEL)
        head = calibref.git_commit()
        runs, by_seed = [], {}
        for i in range(8):
            _e, rec, wire, _c = perturb.run_wire(DUEL, i, VALUES, self.arena)
            import hashlib
            runs.append({"index": i, "seed": rec["seed"], "swap": rec["swap"],
                         "wire_sha256": hashlib.sha256(wire.encode()).hexdigest(),
                         "perturbation": rec, "exogenous": {},
                         "metrics": {"winner": "A", "ttk": {"value": 100, "censored": False},
                                     "survivors": 1.0}})
            by_seed[str(rec["seed"])] = {"trace": trace("A"), "t_end": 100, "end": "units 1 <= 0"}
        self.ref = {"schema": calibref.SCHEMA_REF,
                    "scenario": {"id": self.sid, "file": str(DUEL.relative_to(TREE)),
                                 "sha256": calibref.sha256_file(DUEL),
                                 "calib": calibref.calib_block(json.loads(DUEL.read_text()),
                                                               DUEL.name, self.arena)},
                    "engine": {"build": head[:8], "commit": head, "install": ""},
                    "setup": {}, "values": {"R": 60.0, "types": VALUES},
                    "shuffle": calibref.shuffle_of(calibref.sha256_file(DUEL)), "runs": runs}
        self.by_seed = by_seed
        self.write_ref()

    def tearDown(self):
        os.environ.pop("TACALIB_DIR", None)
        os.environ.pop("TASIM_FAKE_TABLE", None)
        self.tmp.cleanup()

    def write_ref(self):
        p = calibref.ref_path(self.sid, self.ref["engine"]["commit"])
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(self.ref))

    def table(self, doc):
        t = Path(self.tmp.name) / "table.json"
        t.write_text(json.dumps(doc))
        os.environ["TASIM_FAKE_TABLE"] = str(t)

    def run_check(self, granted=2):
        """check() at -j 2, its run lock stood in for by a grant of `granted` slots: the machine's
        lock and load never decide a test. The pools it ran on, by size, in self.pools."""
        self.pools, asked = [], []
        real = check.ThreadPoolExecutor

        def offline(tree, what, most, least=1):
            asked.append(most)
            return check.runlock.Workers(granted)

        def pool(n):
            self.pools.append(n)
            return real(n)
        with mock.patch.object(check.runlock, "offline", offline), \
                mock.patch.object(check, "ThreadPoolExecutor", pool):
            rows, errors = check.check(HERE / "fake_tasim.py", j=2)
        self.assertEqual(asked, [2])
        return check.exit_code(rows, errors), rows, errors

    def test_the_plays_run_on_the_slots_granted(self):
        self.table({"by_seed": self.by_seed})
        rc, rows, _e = self.run_check(granted=1)
        self.assertEqual(rc, 0, rows)
        self.assertEqual(set(self.pools), {1})

    def test_in_band_is_0(self):
        self.table({"by_seed": self.by_seed})
        rc, rows, _e = self.run_check()
        self.assertEqual(rc, 0, rows)
        self.assertEqual({r["metric"] for r in rows}, {"winner", "ttk", "survivors"})

    def test_out_of_band_is_1(self):
        self.table({"by_seed": {k: dict(v, trace=trace("B")) for k, v in self.by_seed.items()}})
        rc, rows, _e = self.run_check()
        self.assertEqual(rc, 1)
        self.assertEqual(next(r for r in rows if r["metric"] == "winner")["verdict"], "out")

    def test_a_tasim_failure_is_2(self):
        self.table({"exit": 3})
        rc, _rows, errors = self.run_check()
        self.assertEqual(rc, 2)
        self.assertIn("tasim exited 3", errors[0])

    def test_a_perturbation_mismatch_is_2(self):
        self.ref["runs"][3]["wire_sha256"] = "0" * 64
        self.write_ref()
        self.table({"by_seed": self.by_seed})
        rc, _rows, errors = self.run_check()
        self.assertEqual(rc, 2)
        self.assertIn("perturbation differs", errors[0])

    def test_a_trace_the_metrics_cannot_read_is_2(self):
        broken = [{"schema": "ta-calib-trace/1", "limit": 5400, "source": "tasim"}]
        self.table({"by_seed": {k: dict(v, trace=broken) for k, v in self.by_seed.items()}})
        rc, _rows, errors = self.run_check()
        self.assertEqual(rc, 2)
        self.assertIn(self.sid, errors[0])

    def test_a_stale_out_stays_out(self):
        row = {"verdict": "out"}
        check.mark_stale(row, ["perturb.py changed"])
        self.assertEqual((row["verdict"], row["stale"]), ("out", True))
        self.assertEqual(check.exit_code([row], []), 1)
        row = {"verdict": "in"}
        check.mark_stale(row, ["perturb.py changed"])
        self.assertEqual(row["verdict"], "stale")

    def test_a_dirty_reference_could_not_run(self):
        self.ref["engine"]["build"] += "-dirty"
        self.write_ref()
        with self.assertRaises(check.CouldNotCheck):
            check.load_refs()

    def test_a_reference_without_its_shuffle_seed_could_not_run(self):
        del self.ref["shuffle"]
        self.write_ref()
        with self.assertRaisesRegex(check.CouldNotCheck, "no shuffle seed"):
            check.load_refs()

    def test_the_selftest_counts_false_alarms_per_metric(self):
        self.ref["runs"] = [dict(r, swap=i % 2 == 1) for i, r in enumerate(self.ref["runs"])]
        fa = check.false_alarms(self.ref)
        self.assertEqual(set(fa), {"winner", "ttk", "survivors"})
        self.assertEqual({(r["splits"], r["out"]) for r in fa.values()}, {(check.SPLITS, 0)})

    def test_a_selftest_that_matches_no_reference_could_not_run(self):
        rc, said = selftest_exit(only="scenarios/nothing-*.json")
        self.assertEqual(rc, 2)
        self.assertIn("could not run: no reference matches", said)



class RunIndices(unittest.TestCase):
    """The refs build takes a scenario only with exactly its run indices."""

    def test_a_full_set_is_taken(self):
        self.assertIsNone(calib.indices_problem({"kind": ["winrate"], "runs": 50}, list(range(50))))

    def test_40_of_50_is_refused(self):
        got = [i for i in range(50) if i % 5]
        why = calib.indices_problem({"kind": ["winrate"], "runs": 50}, got)
        self.assertIn("10 missing", why)
        self.assertIn("0..49", why)

    def test_a_run_played_twice_is_refused(self):
        why = calib.indices_problem({"kind": ["winrate"], "runs": 3}, [0, 1, 1, 2])
        self.assertIn("appears twice", why)

    def test_an_exact_scenario_needs_its_fresh_run(self):
        self.assertIsNone(calib.indices_problem({"kind": ["exact"], "runs": 2}, [0, 1, 2]))
        self.assertIn("1 missing ([2])",
                      calib.indices_problem({"kind": ["exact"], "runs": 2}, [0, 1]))

    def test_self_consistency_without_the_fresh_run_could_not_run(self):
        line = [{"schema": "ta-calib-trace/1"}]
        ref = {"scenario": {"calib": {"metrics": ["damage"]}}}
        saved = check.engine_traces
        try:
            check.engine_traces = lambda _ref, _p: {0: line, 1: line}
            self.assertIn("could not run", check.self_consistency(ref, None))
            check.engine_traces = lambda _ref, _p: {0: line, 1: line, 2: line}
            self.assertIsNone(check.self_consistency(ref, None))
        finally:
            check.engine_traces = saved


class Facing(unittest.TestCase):
    """THE FACING (perturb.py): every unit of a wire names its facing."""

    def test_a_unit_without_a_facing_gets_0(self):
        exact = TREE / "scenarios" / "calib" / "exact-damage-armllt.json"
        exp, _rec, wire, _c = perturb.run_wire(exact, 2, VALUES, calibref.arena())
        self.assertEqual([u["facing"] for u in exp["units"]], [0, 0])
        units = [ln.split() for ln in wire.splitlines() if ln.startswith("unit ")]
        self.assertEqual([u[7] for u in units], ["0", "0"])    # unit i type owner x y height facing

    def test_a_named_facing_is_kept(self):
        exp, _rec, _wire, _c = perturb.run_wire(DUEL, 0, VALUES, calibref.arena())
        self.assertEqual([u["facing"] for u in exp["units"]], [90, 270])


class ScheduledCreates(unittest.TestCase):
    """The tasim_run interface's refusal of the packed lane's keys, as the fake stands it in."""

    UNIT = "unit 0 ARMPW 0 100 200 - - - - - - - -"

    def play(self, wire):
        import fake_tasim
        with tempfile.TemporaryDirectory() as tmp:
            w = Path(tmp) / "wire.txt"
            w.write_text("v1 seed=1\n" + wire + "\n")
            t = Path(tmp) / "table.json"
            t.write_text(json.dumps({"by_seed": {"1": {"trace": [], "t_end": 1, "end": "limit"}}}))
            out = subprocess.run([sys.executable, str(HERE / "fake_tasim.py"), "play", "--wire", str(w),
                                  "--run-seed", "1", "--trace", str(Path(tmp) / "trace.jsonl"), "--json"],
                                 capture_output=True, text=True, env=dict(os.environ, TASIM_FAKE_TABLE=str(t)))
        self.assertTrue(fake_tasim.scheduled_or_placed(wire) == (out.returncode == 2))
        return out.returncode, json.loads(out.stdout)

    def test_at_0_plays(self):
        self.assertEqual(self.play(self.UNIT)[0], 0)
        self.assertEqual(self.play(self.UNIT + " at=0")[0], 0)

    def test_a_later_create_or_a_slot_is_refused(self):
        for keys in (" at=30", " slot=4", " at=0 slot=0", " at=30 slot=30"):
            rc, doc = self.play(self.UNIT + keys)
            self.assertEqual(rc, 2, keys)
            self.assertEqual(doc["error"], "scheduled and placed creates are the engine lane's")


class Stagger(unittest.TestCase):
    """THE DRAWS (perturb.py): one stagger a queue."""

    def test_a_file_without_queue_keeps_its_wires(self):
        """The DUEL fixture's wires and records, pinned from before `queue` existed."""
        want = {0: ("53b92143db6c0f0ba98bfd2feccdbf351dc01af65065b43b52ebe205468a2533", [9, 10]),
                1: ("6935f79c89d3c2143e5a1eeca440ec708c3d4d7c2dc211fe2ed4be6838d66525", [8, 1]),
                2: ("62ee5d06ce6ade17bbbedd31609e9e723ef1dda262ec53d1aba9927d9adaa1a0", [5, 0]),
                3: ("604146f2699421d12b936113e66afc6eb527cbaf85913b85e5cee1b2acf77620", [15, 14])}
        import hashlib
        for i, (sha, after) in want.items():
            _e, rec, wire, _c = perturb.run_wire(DUEL, i, VALUES, calibref.arena())
            self.assertEqual(hashlib.sha256(wire.encode()).hexdigest(), sha)
            self.assertEqual(rec["after"], after)

    def test_a_queue_shares_its_heads_draw(self):
        doc = json.loads(DUEL.read_text())
        c = dict(doc["units"][1], id="c", pos=[5200, 1250])
        c.pop("orders")
        plain = dict(doc, units=doc["units"] + [c])
        queued = json.loads(json.dumps(plain))
        queued["units"][0]["orders"].append({"cmd": "attack", "target": "c", "queue": True})
        exps = []
        with tempfile.TemporaryDirectory() as tmp:
            for name, d in (("plain", plain), ("queued", queued)):
                p = Path(tmp) / f"{name}.json"
                p.write_text(json.dumps(d))
                exps.append(perturb.CLI._scn_compile(str(p)))
        _p, rp = perturb.perturb(exps[0], 0, VALUES, 12345)
        out, rq = perturb.perturb(exps[1], 0, VALUES, 12345)
        self.assertEqual(rq["jitter"], rp["jitter"])
        self.assertEqual(rq["after"], [rp["after"][0], rp["after"][0], rp["after"][1]])
        self.assertEqual([o["after"] for o in out["units"][0]["orders"]], [rp["after"][0]] * 2)
        after = f" after={rp['after'][0]}" if rp["after"][0] else ""
        self.assertIn(f"order 0 attack unit 2{after} queue", perturb.CLI._scn_wire(out).splitlines())


class CalibBlock(unittest.TestCase):

    def test_the_commander_toggle_defaults_to_the_arenas_and_keys_the_launch(self):
        import engrun
        doc = json.loads(DUEL.read_text())
        cfg = calibref.arena()
        block = calibref.calib_block(doc, "x.json")
        self.assertEqual(block["commander_death"], cfg["commander_death"])
        doc["calib"]["commander_death"] = 1
        on = calibref.calib_block(doc, "x.json")
        self.assertEqual(on["commander_death"], 1)
        self.assertNotEqual(engrun.setup_key(block, cfg, False), engrun.setup_key(on, cfg, False))
        doc["calib"]["commander_death"] = 2
        with self.assertRaises(calibref.CalibError):
            calibref.calib_block(doc, "x.json")

    def test_clear_features_false_is_refused(self):
        doc = json.loads(DUEL.read_text())
        doc.setdefault("setup", {})["clear_features"] = False
        with self.assertRaises(calibref.CalibError) as e:
            calibref.calib_block(doc, "x.json")
        self.assertIn("clear_features", str(e.exception))


class EmptyPoint(unittest.TestCase):
    def test_every_settle_is_past_the_last_deaths_sight_record(self):
        """A record made at GameTime d is dropped from d + SIGHT_TTL + 1 on (engrun.py, THE EMPTY
        POINT'S SIGHT); the settle's first hold after a clear at T_prev must be there or later."""
        import engrun
        for t_prev in range(0, 400):
            T = engrun.aligned(t_prev + engrun.S_MIN)
            self.assertGreaterEqual(T, t_prev + engrun.SIGHT_TTL + 1, t_prev)
            self.assertEqual(T % 30, 1)
        self.assertEqual(engrun.aligned(1 + engrun.S_MIN), 91)

    @staticmethod
    def settle(t_prev, gone_in):
        """engrun.Lane.settle on a lane with no Worker whose pool holds a projectile on every hold
        before `gone_in`: (T, waits) and the holds it asked for."""
        import engrun

        class Fake(engrun.Lane):
            def __init__(self):
                self.T_prev, self.held = t_prev, []

            def hold(self, T):
                self.held.append(T)

            def peek(self, spec):
                assert spec == engrun.PROJECTILES
                return 1 if self.held[-1] < gone_in else 0

        lane = Fake()
        return lane.settle(), lane.held

    def test_the_settle_peeks_the_empty_points_pool_on_the_hold_before_it(self):
        """The pool of iteration T's images is the one the hold at T - 1 reads (engrun.py, THE EMPTY
        POINT'S POOL): the settle ends held at T, its last peek on the hold at T - 1."""
        import engrun
        t_prev = 1921
        for gone_in in range(t_prev, t_prev + 230):
            (T, _waits), held = self.settle(t_prev, gone_in)
            self.assertEqual(T % 30, 1, gone_in)
            self.assertGreaterEqual(T, t_prev + engrun.S_MIN, gone_in)
            self.assertLessEqual(gone_in, T - 1, gone_in)
            self.assertEqual(held[-2:], [T - 1, T], gone_in)

    def test_naval_000_run_3(self):
        """The shot of naval-000 run 3 was removed in step 2011, the settle's first T: the hold at
        2010 still holds it, so the settle waits to 2041."""
        self.assertEqual(self.settle(1921, 2011), ((2041, 1), [2010, 2040, 2041]))

class ApplierSeats(unittest.TestCase):
    """applier_check holds each seat to the last apply that named it."""

    def test_the_economy_pass_after_an_apply(self):
        import applier_check as A
        self.assertEqual([A.next_pass(T) for T in (1, 30, 31, 60, 61)], [31, 31, 61, 61, 91])

    def test_the_fixture_seats_come_from_the_step_at_60(self):
        import applier_check as A
        got = A.seat_states(A.applies(A.fixture()), 120)
        self.assertEqual(sorted(got), [0, 1])
        for s, (T, p, ai_off, blast) in got.items():
            self.assertEqual((T, p["metal"], p["energy"], blast), (60, 5000, 5000, False), s)
        self.assertEqual((got[0][2], got[1][2]), (False, True))
        early = A.seat_states(A.applies(A.fixture()), 59)
        self.assertEqual((early[1][0], early[1][3]), (1, True))

def selftest_exit(only=None):
    """`calib.py selftest`'s exit code and what it printed."""
    import argparse
    import contextlib
    import io
    import calib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = calib.cmd_selftest(argparse.Namespace(only=only, against=None))
    return rc, buf.getvalue()


class SelfTestWithNoReference(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        os.environ["TACALIB_DIR"] = self.tmp.name

    def tearDown(self):
        os.environ.pop("TACALIB_DIR", None)
        self.tmp.cleanup()

    def test_no_reference_could_not_run(self):
        rc, said = selftest_exit()
        self.assertEqual(rc, 2)
        self.assertIn("calib selftest: could not run: no reference", said)


def git(repo, *a):
    env = dict(os.environ, GIT_AUTHOR_NAME="calib-test", GIT_AUTHOR_EMAIL="calib-test",
               GIT_COMMITTER_NAME="calib-test", GIT_COMMITTER_EMAIL="calib-test")
    r = subprocess.run(["git", "-C", str(repo), "-c", "core.hooksPath=/dev/null", *a],
                       capture_output=True, text=True, env=env, check=True)
    return r.stdout.strip()


class ArenaFiles(unittest.TestCase):
    def test_a_reference_depends_on_its_own_arena(self):
        files = check.ARENA_FILES
        want = {"cavedog links cc": "tools/tasim/calib-arena.json",
                "seven islands": "tools/tasim/calib-arena-naval.json",
                "impure-calib-land": "tools/tasim/calib-arena-packed.json",
                "impure-calib-naval": "tools/tasim/calib-arena-packed-naval.json"}
        for name, cfg in want.items():
            self.assertEqual(calibref.arena(TREE / cfg)["map"], name)
            self.assertEqual(check.arena_files({"setup": {"map": name}}), files[cfg], name)
        for kind in ("land", "naval"):
            got = check.arena_files({"setup": {"map": f"impure-calib-{kind}"}})
            for tool in ("tools/tasim/synthmap.py", "tools/hpipack.py", "tools/tasim/pack.py"):
                self.assertIn(tool, got)
        self.assertNotIn("tools/tasim/synthmap.py", check.arena_files({"setup": {"map": "cavedog links cc"}}))
        every = check.arena_files({"setup": {"map": "another map"}})
        self.assertEqual(set(every), {f for fs in files.values() for f in fs})
        for p in every:
            self.assertTrue((TREE / p).exists(), p)

    def test_an_unreadable_config_leaves_every_file(self):
        with mock.patch.object(calibref, "arena", side_effect=calibref.CalibError("unreadable")):
            got = check.arena_files({"setup": {"map": "cavedog links cc"}})
        self.assertIn("tools/tasim/synthmap.py", got)
        self.assertIn("scenarios/calib/arena-naval.json", got)


class Staleness(unittest.TestCase):
    """stale_reasons on a repository made here: tareplay's reading of a sim file's changed code
    lines, pointed at it."""

    def setUp(self):
        import tareplay
        self.R = tareplay
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name)
        git(self.repo, "init", "-q")
        (self.repo / "tagpu" / "ddraw" / "src").mkdir(parents=True)
        (self.repo / "scenarios").mkdir()
        (self.repo / "tagpu" / "ddraw" / "thread-split.allow").write_text(
            "src/sim.c    tooling    sim\n")
        self.sim = self.repo / "tagpu" / "ddraw" / "src" / "sim.c"
        self.sim.write_text("/* the step */\nint step(int x) { return x + 1; }\n")
        self.scn = self.repo / "scenarios" / "x.json"
        self.scn.write_text('{"format": "ta-scenario/1"}\n')
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "base")
        self.ref = {"engine": {"commit": git(self.repo, "rev-parse", "HEAD")},
                    "scenario": {"file": "scenarios/x.json",
                                 "sha256": calibref.sha256_file(self.scn)},
                    "values": {"types": {}}}
        self.saved = self.R.TREE
        self.R.TREE = self.repo

    def tearDown(self):
        self.R.TREE = self.saved
        self.tmp.cleanup()

    def commit(self, path, text):
        path.write_text(text)
        git(self.repo, "commit", "-q", "-am", "change")

    def test_a_comment_change_is_not_stale(self):
        self.commit(self.sim, "/* the step, once a tick */\nint step(int x) { return x + 1; }\n")
        self.assertEqual(check.stale_reasons(self.ref), [])

    def test_a_code_change_is_stale(self):
        self.commit(self.sim, "/* the step */\nint step(int x) { return x + 2; }\n")
        self.assertEqual(check.stale_reasons(self.ref), ["tagpu/ddraw/src/sim.c: code changed"])

    def test_a_scenario_edit_is_stale(self):
        self.commit(self.scn, '{"format": "ta-scenario/1", "seed": 1}\n')
        why = check.stale_reasons(self.ref)
        self.assertIn("scenarios/x.json: changed", why)
        self.assertIn("scenarios/x.json: its sha256 differs", why)

    def test_a_generated_maps_tools_stale_its_references_alone(self):
        tool = self.repo / "tools" / "tasim" / "synthmap.py"
        tool.parent.mkdir(parents=True)
        tool.write_text("GRID = 128\n")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "the map")
        packed = dict(self.ref, engine={"commit": git(self.repo, "rev-parse", "HEAD")},
                      setup={"map": "impure-calib-land"})
        retail = dict(packed, setup={"map": "cavedog links cc"})
        self.commit(tool, "GRID = 256\n")
        self.assertEqual(check.stale_reasons(packed), ["tools/tasim/synthmap.py: changed"])
        self.assertEqual(check.stale_reasons(retail), [])

    def test_since_counts_only_the_landing(self):
        self.commit(self.sim, "/* the step */\nint step(int x) { return x + 2; }\n")
        since = git(self.repo, "rev-parse", "HEAD")
        self.commit(self.sim, "/* the step, again */\nint step(int x) { return x + 2; }\n")
        self.assertEqual(check.stale_reasons(self.ref, since=since), [])

    def test_one_pass_reads_each_base_once_and_answers_as_each_call_alone(self):
        first = self.ref
        self.commit(self.sim, "/* the step */\nint step(int x) { return x + 2; }\n")
        second = dict(first, engine={"commit": git(self.repo, "rev-parse", "HEAD")})
        self.commit(self.scn, '{"format": "ta-scenario/1", "seed": 1}\n')
        refs = [first, second, dict(first)]
        for since in (None, git(self.repo, "rev-parse", "HEAD~1")):
            alone = [check.stale_reasons(r, since=since) for r in refs]
            changes = {}
            with mock.patch.object(check, "code_changes", wraps=check.code_changes) as read:
                shared = [check.stale_reasons(r, since=since, changes=changes) for r in refs]
            self.assertEqual(shared, alone)
            self.assertEqual(read.call_count, 2 if since is None else 1, "one reading a base")
            if since is None:
                # each reference against its own commit: the code change is the first one's alone
                self.assertIn("tagpu/ddraw/src/sim.c: code changed", alone[0])
                self.assertNotIn("tagpu/ddraw/src/sim.c: code changed", alone[1])


if __name__ == "__main__":
    unittest.main()
