#!/usr/bin/env python3
"""Tests of the automatic tuning, offline -- the levels' line (tune.py levels-line): the line, its bracket, its
fit, its order check and its runs on a fake player (a level found and confirmed, a resume, a line not monotone,
no row on the line); and SPSA: the arithmetic (spsa.py) on objectives whose optimum is known,
its determinism and its resume; the persona and plan spaces and the tune lever's rules (tune.py, level_lever.py);
the objectives' values; and `tune.py spsa run` on a fake player -- its common random numbers, its resume after a
stop, its determinism from the seed and an iteration that could not run.

    .venv-undither/bin/python tools/impai/test_spsa.py          (numpy: the shared venv)
"""
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bar  # noqa: E402
import level_lever  # noqa: E402
import spsa  # noqa: E402
import tune  # noqa: E402


class Arithmetic(unittest.TestCase):
    def test_the_gains_are_spalls(self):
        s = spsa.SPSA(3, 1, 50, [0.5] * 3, a=0.4, c=0.2)
        ak, ck = s.gains(4)
        self.assertAlmostEqual(ak, 0.4 / (5 + 5.0) ** 0.602)
        self.assertAlmostEqual(ck, 0.2 / 5 ** 0.101)

    def test_the_default_gain_takes_the_first_step_asked(self):
        s = spsa.SPSA(1, 1, 40, [0.5], c=0.15)
        ak, ck = s.gains(0)
        self.assertAlmostEqual(ak * 0.2 / (2 * ck), 0.1 * 0.15 / ck, places=9)

    def test_on_a_linear_objective_the_estimate_is_the_gradient_on_average(self):
        g = np.array([0.3, -0.2, 0.1, 0.0])
        est = []
        for seed in range(2000):
            s = spsa.SPSA(4, seed, 10, [0.5] * 4, c=0.1)
            xp, xm, d = s.ask()
            est.append((g @ xp - g @ xm) / (2 * s.gains(0)[1]) * d)
        self.assertTrue(np.allclose(np.mean(est, axis=0), g, atol=0.02))

    def test_it_climbs_a_noisy_quadratic_to_its_top(self):
        top = np.array([0.7, 0.2, 0.5])
        rng = np.random.default_rng(5)
        s = spsa.SPSA(3, 9, 400, [0.1, 0.9, 0.1], c=0.1)
        while not s.done():
            xp, xm, _ = s.ask()
            noise = rng.normal(0, 0.01)                   # the arms' games are common: much of it cancels
            s.tell(-np.sum((xp - top) ** 2) + noise + np.sum((xm - top) ** 2) - noise
                   + rng.normal(0, 0.01))
        self.assertLess(np.max(np.abs(s.theta - top)), 0.06)

    def test_the_box_and_the_step_hold(self):
        s = spsa.SPSA(2, 3, 20, [0.99, 0.01], a=50.0, c=0.3, max_step=0.2)
        before = s.theta.copy()
        xp, xm, _ = s.ask()
        self.assertTrue(((xp >= 0) & (xp <= 1) & (xm >= 0) & (xm <= 1)).all())
        step = s.tell(10.0)
        self.assertTrue((np.abs(step) <= 0.2 + 1e-12).all())
        self.assertTrue(((s.theta >= 0) & (s.theta <= 1)).all())
        self.assertFalse(np.allclose(before, s.theta))

    def test_an_iterations_perturbation_is_its_seed_and_k_alone(self):
        a, b = spsa.SPSA(5, 11, 30, [0.5] * 5), spsa.SPSA(5, 11, 30, [0.2] * 5)
        self.assertTrue((a.delta(7) == b.delta(7)).all())
        self.assertFalse(all((a.delta(k) == spsa.SPSA(5, 12, 30, [0.5] * 5).delta(k)).all() for k in range(5)))

    def test_a_search_resumed_from_its_state_goes_on_as_one_never_stopped(self):
        def f(x):
            return -float(np.sum((x - 0.3) ** 2))
        whole = spsa.SPSA(4, 2, 12, [0.8] * 4)
        cut = spsa.SPSA(4, 2, 12, [0.8] * 4)
        for _ in range(5):
            xp, xm, _ = cut.ask()
            cut.tell(f(xp) - f(xm))
        cut = spsa.SPSA.from_state(json.loads(json.dumps(cut.state())))
        for s in (whole, cut):
            while not s.done():
                xp, xm, _ = s.ask()
                s.tell(f(xp) - f(xm))
        self.assertEqual(whole.theta.tolist(), cut.theta.tolist())

    def test_refusals(self):
        with self.assertRaises(ValueError):
            spsa.SPSA(0, 1, 5, [])
        with self.assertRaises(ValueError):
            spsa.SPSA(2, 1, 5, [0.5, 0.5], c=0.9)
        with self.assertRaises(ValueError):
            spsa.SPSA(2, 1, 5, [0.5])


class Spaces(unittest.TestCase):
    def test_every_named_row_round_trips(self):
        sp = tune.PersonaSpace()
        for name, row in bar.PERSONA_ROWS.items():
            self.assertEqual(sp.decode(sp.encode(row)), row, name)

    def test_every_point_of_the_box_is_a_row_the_lever_accepts(self):
        sp = tune.PersonaSpace()
        rng = np.random.default_rng(1)
        for x in list(rng.random((2000, 9))) + [np.zeros(9), np.ones(9)]:
            row = sp.decode(x)
            self.assertIsNone(level_lever.persona_error(f"1={row}"), row)
        self.assertEqual(sp.decode(np.ones(9)), bar.PERSONA_ROWS["L5"])
        self.assertEqual(sp.decode(np.zeros(9)), "150,150,135,2,1,3,1,1,1")      # L0b: the lever's harshest

    def test_every_point_of_a_plan_box_is_a_plan_the_lever_accepts(self):
        sp = tune.PlanSpace(["lead_in", "lead_out", "attack_margin", "retreat_margin", "stall_pm"])
        rng = np.random.default_rng(2)
        for x in rng.random((500, sp.dim)):
            items = sp.decode(x)
            self.assertIsNone(level_lever.tune_error(1, items), items)
        self.assertEqual(sp.decode(sp.encode({})), {n: level_lever.TUNE_PARAMS[n][0] for n in sp.names})

    def test_a_broken_order_is_mended_on_the_untuned_partner_too(self):
        items = tune.PlanSpace(["lead_in"]).decode([0.0])          # lead_in 1500, below lead_out's 2000
        self.assertEqual(items, {"lead_in": 1500, "lead_out": 1499})

    def test_an_unknown_constant_is_refused(self):
        with self.assertRaises(tune.TuneError):
            tune.PlanSpace(["no_such"])


class Lever(unittest.TestCase):
    def test_the_rules(self):
        self.assertIsNone(level_lever.tune_error(1, {}))
        self.assertIn("outside", level_lever.tune_error(1, {"raid_min": 9}))
        self.assertIn("not below", level_lever.tune_error(1, {"attack_margin": -50}))
        self.assertIn("no constant", level_lever.tune_error(1, {"nope": 1}))
        self.assertEqual(level_lever.parse_tune(["2:lead_in=3000", "2:raid_max=8"]),
                         {2: {"lead_in": 3000, "raid_max": 8}})
        with self.assertRaises(ValueError):
            level_lever.parse_tune(["2:lead_in=3000", "2:lead_in=3100"])

    def test_the_cli_writes_one_item_a_line(self):
        with tempfile.TemporaryDirectory() as t:
            self.assertEqual(level_lever.main([t, "--tune", "2:raid_max=8", "--tune", "1:lead_in=3000"]), 0)
            self.assertEqual((Path(t) / level_lever.TUNE_LEVER).read_text(), "1:lead_in=3000\n2:raid_max=8\n")
            with mock.patch("sys.stderr"):
                self.assertEqual(level_lever.main([t, "--tune", "1:lead_in=99999"]), 2)

    def test_bar_reads_the_init_line_and_keeps_only_a_result_of_its_items(self):
        lines = ["impai: tune seat=2 from=lever attack_margin=150 lead_in=2500",
                 "impai: tune seat=2 from=lever attack_margin=999"]
        self.assertEqual(bar.tunes(lines, {"A": 2, "B": 1}),
                         {"A": {"from": "lever", "values": {"attack_margin": 150, "lead_in": 2500}}})
        g = bar.with_settings(bar.parse_id(bar.ids("hota")[0]), {}, {"A": {"lead_in": 3000}})
        self.assertEqual(g["seats"]["A"]["tune"], {"lead_in": 3000})
        self.assertEqual(bar.tune_override(["A=lead_in=3000,raid_max=8"]), {"A": {"lead_in": 3000, "raid_max": 8}})
        with self.assertRaises(bar.BarError):
            bar.tune_override(["A=lead_in=1"])
        with self.assertRaises(bar.BarError):
            bar.with_settings(bar.parse_id(bar.ids("hota")[0]), {}, {"B": {"lead_in": 3000}})   # OTA AI's seat

    def test_a_piece_passes_its_arms_settings_to_bar_play(self):
        import batch
        p = {"name": "k000-plus-ota-0", "ids": ["x"], "lanes": 1, "out": "o", "setup": None,
             "persona": ["A=60,90,45,10,3,15,3,4,2"], "tune": ["A=lead_in=3000"]}
        with tempfile.TemporaryDirectory() as t, mock.patch("subprocess.Popen") as popen:
            batch.Piece(p, Path("d.dll"), Path(t))
        cmd = popen.call_args.args[0]
        self.assertEqual(cmd[cmd.index("--persona") + 1], "A=60,90,45,10,3,15,3,4,2")
        self.assertEqual(cmd[cmd.index("--tune") + 1], "A=lead_in=3000")

    def test_a_literal_row_is_a_persona_override(self):
        self.assertEqual(bar.settings_override(["B=60,90,45,10,3,15,3,4,2"]), {"B": "60,90,45,10,3,15,3,4,2"})
        with self.assertRaises(bar.BarError):
            bar.settings_override(["B=60,90,90,10,3,15,3,4,2"])              # reacts later than its look allows


def result(gid, outcome, end=40000, cap=bar.CAP):
    r = {"id": gid, "outcome": outcome, "end_tick": end, "cap": cap}
    if outcome == "cap":
        r["value"] = {"A": {"live": 1.0, "t": cap}, "B": {"live": 1.0, "t": cap}}
    return r


class Objectives(unittest.TestCase):
    def test_the_margin_is_the_arms_paired_on_each_id(self):
        plus = {"pairs": {"a": result("a", "A", 30000), "b": result("b", "A", 50000), "c": result("c", "B")}}
        minus = {"pairs": {"a": result("a", "A", 40000), "b": result("b", "A", 50100), "c": result("c", "A", 1)}}
        d, detail = tune.spsa_diff("margin", plus, minus)
        self.assertEqual(detail, {"better": 1, "ties": 1, "worse": 1})
        self.assertAlmostEqual(d, 0.0)

    def test_the_points_objectives_are_each_arms_points(self):
        plus = {"points": {f"g{i}": result(f"g{i}", "A" if i < 7 else "B") for i in range(10)}}
        minus = {"points": {f"g{i}": result(f"g{i}", "A" if i < 4 else "B") for i in range(10)}}
        d, _ = tune.spsa_diff("mirror", plus, minus)
        self.assertAlmostEqual(d, 0.3)
        minus["points"]["g0"] = None
        self.assertIsNone(tune.spsa_diff("mirror", plus, minus)[0])

    def test_the_roles_share_their_ids_between_the_arms_and_move_on_each_iteration(self):
        r0, r1 = tune.spsa_roles("mirror", 0), tune.spsa_roles("mirror", 1)
        self.assertTrue(all(g.startswith("bar-g:H.H-") for g in r0["points"]))
        self.assertEqual(len(r0["points"]), 10)
        self.assertFalse(set(r0["points"]) & set(r1["points"]))
        self.assertEqual(len(tune.spsa_roles("margin", 0)["pairs"]), 16)
        self.assertTrue(all(g.startswith("bar-h:2.000-") for g in tune.spsa_roles("handicap", 0, "h:2.000")["points"]))
        self.assertEqual(tune.spsa_settings({"lead_in": 3000}), {"persona": [], "tune": ["A=lead_in=3000"]})


def unit(*parts) -> float:
    return int.from_bytes(hashlib.sha256(" ".join(map(str, parts)).encode()).digest()[:4], "big") / 2 ** 32


def plan_of(item: str) -> dict:
    return {n: int(v) for n, _, v in (kv.partition("=") for kv in item.split("=", 1)[1].split(","))}


class FakePlayer:
    """Writes each piece's results as bar.py would, from the plan constants its arm plays: A beats ImpAI Hard when a
    draw of the id falls below 0.5 + 0.4 (1 - ((attack_margin - 250) / 350)^2), so the best attack margin is 250."""

    def __init__(self, stop_at=None, skip=None):
        self.played, self.stop_at, self.skip = [], stop_at, skip or set()

    def __call__(self, ps, launch):
        for p in ps:
            k = int(p["name"][1:4])
            if self.stop_at is not None and k >= self.stop_at:
                raise KeyboardInterrupt
            self.played.append(p["name"])
            out = Path(p["out"])
            out.mkdir(parents=True, exist_ok=True)
            plan = plan_of(p["tune"][0])
            q = 0.5 + 0.4 * (1 - ((plan["attack_margin"] - 250) / 350) ** 2)
            for gid in p["ids"]:
                if gid in self.skip:
                    continue
                win = unit(gid, p["tune"][0]) < q
                (out / f"{gid}.json").write_text(json.dumps(result(gid, "A" if win else "B")))
        return []


class Run(unittest.TestCase):
    PARAMS = ["attack_margin", "retreat_margin"]

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(__import__("shutil").rmtree, self.tmp, True)
        self.dll = self.tmp / "x.dll"
        self.dll.write_bytes(b"dll")

    def run_(self, d, player):
        with mock.patch.dict("os.environ", {"TARUN_DIR": str(self.tmp / "runs")}), mock.patch("builtins.print"):
            return tune.spsa_run(d, self.dll, 10, launch=lambda p: None, play=player)

    def init(self, name, iters=4, seed=3, start=None):
        with mock.patch("builtins.print"):
            return tune.SpsaRun.init(self.tmp / name, "mirror", seed, iters, start=start, params=self.PARAMS)

    def state(self, name):
        return json.loads((self.tmp / name / tune.SPSA_STATE).read_text())

    def test_a_run_plays_each_iteration_on_common_games_and_records_it(self):
        self.init("a")
        p = FakePlayer()
        self.assertEqual(self.run_(self.tmp / "a", p), 0)
        st = self.state("a")
        self.assertEqual([it["k"] for it in st["iterations"]], [0, 1, 2, 3])
        self.assertEqual(st["search"]["k"], 4)
        self.assertEqual(len(p.played), 4 * 2)                    # two arms, a block each
        k0 = sorted(f.name for f in (self.tmp / "a" / "games" / "k000-plus").glob("*.json"))
        self.assertEqual(k0, sorted(f.name for f in (self.tmp / "a" / "games" / "k000-minus").glob("*.json")))
        for it in st["iterations"]:
            self.assertIsNone(level_lever.tune_error(1, it["plus"]))
            self.assertNotEqual(it["plus"], it["minus"])

    def test_the_same_seed_is_the_same_run(self):
        self.init("a")
        self.init("b")
        self.run_(self.tmp / "a", FakePlayer())
        self.run_(self.tmp / "b", FakePlayer())
        self.assertEqual(self.state("a")["iterations"], self.state("b")["iterations"])
        self.init("c", seed=4)
        self.run_(self.tmp / "c", FakePlayer())
        self.assertNotEqual(self.state("a")["iterations"][0]["delta"], self.state("c")["iterations"][0]["delta"])

    def test_a_stopped_run_resumes_where_it_stopped_and_ends_as_one_never_stopped(self):
        self.init("whole")
        self.run_(self.tmp / "whole", FakePlayer())
        self.init("cut")
        self.assertEqual(self.run_(self.tmp / "cut", FakePlayer(stop_at=2)), 2)
        self.assertEqual(len(self.state("cut")["iterations"]), 2)
        p = FakePlayer()
        self.assertEqual(self.run_(self.tmp / "cut", p), 0)
        self.assertTrue(all(n.startswith(("k002", "k003")) for n in p.played))
        self.assertEqual(self.state("cut")["iterations"], self.state("whole")["iterations"])
        self.assertEqual(self.state("cut")["search"], self.state("whole")["search"])

    def test_an_iteration_missing_a_game_plays_once_more_then_stops_unrun(self):
        self.init("a")
        gid = tune.spsa_roles("mirror", 0)["points"][3]
        p = FakePlayer(skip={gid})
        self.assertEqual(self.run_(self.tmp / "a", p), 2)
        self.assertEqual(len(p.played), 4)                         # the iteration's two pieces, twice
        self.assertEqual(self.state("a")["iterations"], [])

    def test_another_dll_is_refused(self):
        self.init("a", iters=1)
        self.run_(self.tmp / "a", FakePlayer())
        other = self.tmp / "y.dll"
        other.write_bytes(b"other")
        with mock.patch("builtins.print"):
            self.assertEqual(tune.spsa_run(self.tmp / "a", other, 10, launch=lambda p: None, play=FakePlayer()), 2)

    def test_the_search_climbs_towards_the_best_constant(self):
        # the fake world's best attack margin is 250; from the hand-written 100 the search climbs towards it
        self.init("a", iters=30, seed=7)
        self.run_(self.tmp / "a", FakePlayer())
        end = tune.PlanSpace(self.PARAMS).decode(self.state("a")["search"]["theta"])["attack_margin"]
        self.assertGreater(end, 160)

    def test_init_refusals(self):
        self.init("a", iters=2)
        with mock.patch("builtins.print"):
            with self.assertRaises(tune.TuneError):
                tune.SpsaRun.init(self.tmp / "a", "mirror", 1, 2, params=self.PARAMS)
            with self.assertRaises(tune.TuneError):
                tune.SpsaRun.init(self.tmp / "b", "handicap", 1, 2, params=["lead_in"])
            with self.assertRaises(tune.TuneError):
                tune.SpsaRun.init(self.tmp / "c", "mirror", 1, 2, params=self.PARAMS, start={"attack_margin": 999})
            with self.assertRaises(tune.TuneError):
                tune.SpsaRun.init(self.tmp / "d", "levels-normal", 1, 2, params=self.PARAMS)


def harsh_order(v: int) -> float:
    """A budget's or attention's value in the order of strength: 0 is no limit, past every number."""
    return float("inf") if v == 0 else float(v)


class Line(unittest.TestCase):
    def test_the_line_runs_from_the_harshest_row_to_hards_and_every_point_is_a_row(self):
        self.assertEqual(tune.line_row(0.0), bar.PERSONA_ROWS["L0b"])
        self.assertEqual(tune.line_row(1.0), bar.PERSONA_ROWS["L5"])
        for i in range(201):
            self.assertIsNone(level_lever.persona_error(f"1={tune.line_row(i / 200)}"))

    def test_every_axis_relaxes_as_t_grows(self):
        prev = None
        for i in range(401):
            v = [int(x) for x in tune.line_row(i / 400).split(",")]
            look_e, look_m, react, apm_e, burst_e, apm_m, burst_m, att_e, att_m = v
            now = (-look_e, -look_m, -react, harsh_order(apm_e), harsh_order(burst_e if apm_e else 0),
                   harsh_order(apm_m), harsh_order(burst_m if apm_m else 0), harsh_order(att_e), harsh_order(att_m))
            if prev:
                self.assertTrue(all(b >= a for a, b in zip(prev, now)), (i, v))
            prev = now

    def test_the_bracket_moves_on_the_bound_and_closes_on_a_row_played(self):
        looks = [{"t": 0.5, "row": tune.line_row(0.5), "wins": 18, "games": 20},
                 {"t": 0.75, "row": tune.line_row(0.75), "wins": 17, "games": 20}]
        self.assertEqual(tune.line_bracket(looks), (0.5, 0.75))
        t, row = tune.line_next(looks)
        self.assertAlmostEqual(t, 0.625)
        self.assertEqual(tune.line_row(0.3), tune.line_row(0.3005))       # a bracket inside one row
        same = [{"t": 0.3, "row": tune.line_row(0.3), "wins": 18, "games": 20},
                {"t": 0.301, "row": tune.line_row(0.301), "wins": 10, "games": 20}]
        self.assertIsNone(tune.line_next(same))

    def test_the_fit_finds_the_bound_and_the_aim_lies_inside_it(self):
        rng = np.random.default_rng(4)
        c, s = 0.6, 0.08
        ts, ys = [], []
        for t in (0.2, 0.35, 0.45, 0.5, 0.55, 0.65):
            p = 1 / (1 + np.exp((t - c) / s))
            ts += [t] * 400
            ys += list((rng.random(400) < p).astype(float))
        fr = tune.line_fit(ts, ys)
        self.assertAlmostEqual(fr["t90"], c - s * np.log(9), delta=0.02)
        self.assertLess(fr["t_aim"], fr["t90"])
        self.assertIsNotNone(fr["se"])

    def test_the_order_check_names_a_reversed_pair_only_past_the_noise(self):
        ok = [{"t": 0.2, "row": "a", "wins": 19, "games": 20}, {"t": 0.6, "row": "b", "wins": 17, "games": 20}]
        self.assertEqual(tune.line_breaks(ok), [])
        noise = [{"t": 0.2, "row": "a", "wins": 17, "games": 20}, {"t": 0.6, "row": "b", "wins": 19, "games": 20}]
        self.assertEqual(tune.line_breaks(noise), [])
        bad = [{"t": 0.2, "row": "a", "wins": 8, "games": 20}, {"t": 0.6, "row": "b", "wins": 20, "games": 20}]
        self.assertEqual(len(tune.line_breaks(bad)), 1)

    def test_the_looks_and_the_confirmation_share_no_seed(self):
        conf = set(bar.ids(bar.MIRROR + bar.NORMAL_ROW))
        seen = set()
        for k in range(1, 7):
            ids = tune.line_game_ids("normal", k)
            self.assertEqual(len(ids), 20)
            self.assertFalse(set(ids) & conf)
            self.assertFalse(set(ids) & seen)
            seen |= set(ids)
        self.assertTrue(all(g.startswith("bar-n:L3-") for g in tune.line_game_ids("easy", 1)))
        self.assertEqual(tune.line_items("easy", "mirror", "R", "N"), ["A=N", "B=R"])
        self.assertEqual(tune.line_items("normal", "mirror", "R"), ["B=R"])
        self.assertEqual(tune.line_items("easy", "ota", "R", "N"), ["A=R"])

    def test_the_simulation_puts_the_bound_within_its_bracket(self):
        r = tune.line_simulate(6, 20, 0.1, runs=40, seed=3)
        self.assertLess(r["rmse"], 0.08)
        self.assertGreater(r["p_at"], 0.9)
        self.assertEqual(r["games"], 6 * 20 + 140)


class LinePlayer:
    """Writes each piece's results from the row its arm plays, at t the row's place on the line (its mean
    coordinate): the stronger side wins a mirror game when a draw of the id falls below p(t), logistic through
    `t90` of slope 0.08; the row scores against OTA AI when a draw falls below `ota(t)`."""

    def __init__(self, t90=0.55, ota=lambda t: 0.97, stop_at=None, reverse=False):
        self.t90, self.ota, self.stop_at, self.reverse, self.played = t90, ota, stop_at, reverse, []

    def __call__(self, ps, launch):
        for p in ps:
            if self.stop_at and p["name"].startswith(self.stop_at):
                raise KeyboardInterrupt
            self.played.append(p["name"])
            out = Path(p["out"])
            out.mkdir(parents=True, exist_ok=True)
            ota = p["name"].startswith("confirm-ota")
            row = [x.split("=", 1)[1] for x in p["persona"] if x.startswith("A=" if ota else "B=")][0]
            t = float(np.mean(tune.PersonaSpace().encode(row)))
            if self.reverse:
                t = 1 - t
            for gid in p["ids"]:
                u = unit(gid, row)
                if ota:
                    win = u < self.ota(t)
                else:
                    win = u < 1 / (1 + np.exp((t - (self.t90 + 0.08 * np.log(9))) / 0.08))
                (out / f"{gid}.json").write_text(json.dumps(result(gid, "A" if win else "B")))
        return []


class LineRun(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(__import__("shutil").rmtree, self.tmp, True)
        self.dll = self.tmp / "x.dll"
        self.dll.write_bytes(b"dll")

    def run_(self, player, level="normal", name="a", normal_row=None):
        with mock.patch.dict("os.environ", {"TARUN_DIR": str(self.tmp / "runs")}), \
                mock.patch("builtins.print") as said:
            rc = tune.line_run(self.tmp / name, self.dll, level, normal_row, launch=lambda p: None, play=player)
        self.said = "\n".join(str(c.args[0]) for c in said.call_args_list)
        return rc

    def state(self, name="a"):
        return json.loads((self.tmp / name / tune.LINE_STATE).read_text())

    def test_a_level_is_found_inside_the_bound_and_confirmed(self):
        p = LinePlayer(t90=0.55)
        self.assertEqual(self.run_(p), 0)
        st = self.state()
        self.assertEqual(len(st["looks"]), 6)
        self.assertEqual(st["verdict"], "chosen")
        self.assertLess(st["chosen"]["t"], 0.55 + 0.05)
        self.assertEqual(st["confirm"]["mirror_games"], 100)
        self.assertEqual(st["confirm"]["ota_games"], 40)
        self.assertIn("monotone as measured", self.said)
        self.assertIn("CHOSEN:", self.said)

    def test_a_stopped_run_resumes_where_it_stopped(self):
        whole = LinePlayer()
        self.run_(whole, name="whole")
        self.assertEqual(self.run_(LinePlayer(stop_at="look-3"), name="cut"), 2)
        self.assertEqual(len(self.state("cut")["looks"]), 2)
        p = LinePlayer()
        self.assertEqual(self.run_(p, name="cut"), 0)
        self.assertFalse(any(n.startswith(("look-1", "look-2")) for n in p.played))
        self.assertEqual(self.state("cut")["looks"], self.state("whole")["looks"])
        self.assertEqual(self.state("cut")["chosen"], self.state("whole")["chosen"])

    def test_a_line_not_monotone_stops_and_says_so(self):
        self.assertEqual(self.run_(LinePlayer(t90=0.3, reverse=True)), 1)
        self.assertEqual(self.state()["verdict"], "not monotone")
        self.assertIn("BROKEN", self.said)

    def test_no_row_meets_the_mirrors_bound(self):
        self.assertEqual(self.run_(LinePlayer(t90=-0.4)), 1)
        self.assertEqual(self.state()["verdict"], "below the line")
        self.assertIn("NO ROW", self.said)

    def test_no_row_meets_both_bounds(self):
        self.assertEqual(self.run_(LinePlayer(ota=lambda t: 0.3)), 1)
        self.assertEqual(self.state()["verdict"], "below the floor")
        self.assertIn("no point on the line meets both", self.said)

    def test_easy_plays_against_normals_row_with_its_own_floor(self):
        p = LinePlayer(ota=lambda t: 0.55)
        self.assertEqual(self.run_(p, level="easy", normal_row="L3"), 0)
        st = self.state()
        self.assertEqual(st["normal_row"], bar.PERSONA_ROWS["L3"])
        self.assertEqual(st["confirm"]["floor"], 0.50)
        with self.assertRaises(tune.TuneError):
            self.run_(p, level="normal")                               # the directory is Easy's


if __name__ == "__main__":
    unittest.main()
