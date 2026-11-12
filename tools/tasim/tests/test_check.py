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
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(TREE / "tools" / "replay"))
import calibref  # noqa: E402
import check  # noqa: E402
import perturb  # noqa: E402

FX = 1 << 16
DUEL = TREE / "scenarios" / "calib" / "duel-armpw-corak.json"
VALUES = {"ARMPW": {"metal": 45.0, "energy": 900.0, "maxdamage": 300, "bmcode": 1,
                    "footprint": [2, 2], "id": 5},
          "CORAK": {"metal": 40.0, "energy": 600.0, "maxdamage": 250, "bmcode": 1,
                    "footprint": [2, 2], "id": 6}}


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

    def test_distribution_band_and_spread(self):
        e = list(range(100, 200))
        self.assertTrue(check.rule_distribution(e, list(range(110, 210)))[0])
        self.assertFalse(check.rule_distribution(e, list(range(300, 400)))[0])     # P50 outside
        self.assertFalse(check.rule_distribution(e, [150] * 100)[0])                # spread 0
        self.assertFalse(check.rule_distribution(e, list(range(-100, 400, 5)))[0])  # spread 5

    def test_distribution_with_no_engine_spread(self):
        self.assertTrue(check.rule_distribution([600] * 50, [600] * 50)[0])
        self.assertFalse(check.rule_distribution([600] * 50, [600] * 40 + [599] * 10)[0])

    def test_a_censored_value_enters_as_its_limit(self):
        e = [{"ttk": {"value": 5400, "censored": True}}] * 10
        rows = check.grade_runs({"metrics": ["ttk"]}, e, e)
        self.assertEqual(rows[0]["engine"]["q"], [5400.0, 5400.0, 5400.0])
        self.assertEqual(rows[0]["verdict"], "in")

    def test_exact_has_no_tolerance(self):
        self.assertEqual(check.rule_exact([[1, "a", 300, 150]], [[1, "a", 300, 150]]), (True, None))
        ok, diff = check.rule_exact([[1, "a", 300, 150]], [[1, "a", 300, 150.0001]])
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
                    "setup": {}, "values": {"R": 60.0, "types": VALUES}, "runs": runs}
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

    def run_check(self):
        rows, errors = check.check(HERE / "fake_tasim.py", j=2)
        return check.exit_code(rows, errors), rows, errors

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

    def test_a_dirty_reference_could_not_run(self):
        self.ref["engine"]["build"] += "-dirty"
        self.write_ref()
        with self.assertRaises(check.CouldNotCheck):
            check.load_refs()

    def test_a_selftest_that_matches_no_reference_could_not_run(self):
        rc, said = selftest_exit(only="scenarios/nothing-*.json")
        self.assertEqual(rc, 2)
        self.assertIn("could not run: no reference matches", said)



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

    def test_since_counts_only_the_landing(self):
        self.commit(self.sim, "/* the step */\nint step(int x) { return x + 2; }\n")
        since = git(self.repo, "rev-parse", "HEAD")
        self.commit(self.sim, "/* the step, again */\nint step(int x) { return x + 2; }\n")
        self.assertEqual(check.stale_reasons(self.ref, since=since), [])


if __name__ == "__main__":
    unittest.main()
