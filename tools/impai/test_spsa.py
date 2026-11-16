#!/usr/bin/env python3
"""Tests of the automatic tuning's SPSA, offline: the arithmetic (spsa.py) on objectives whose optimum is known,
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
    def test_the_levels_value(self):
        mirror = {f"m{i}": result(f"m{i}", "A" if i < 8 else "B") for i in range(10)}      # w 0.8
        ota = {f"o{i}": result(f"o{i}", "A" if i < 9 else "cap") for i in range(10)}        # o 0.95
        v, _ = tune.level_value("levels-normal", mirror, ota)
        self.assertAlmostEqual(v["w"], 0.8)
        self.assertAlmostEqual(v["o"], 0.95)
        self.assertAlmostEqual(v["J"], 0.95 - tune.LEVEL_K * 0.1)
        mirror["m9"] = None
        self.assertIsNone(tune.level_value("levels-normal", mirror, ota)[0])

    def test_the_margin_is_the_arms_paired_on_each_id(self):
        plus = {"pairs": {"a": result("a", "A", 30000), "b": result("b", "A", 50000), "c": result("c", "B")}}
        minus = {"pairs": {"a": result("a", "A", 40000), "b": result("b", "A", 50100), "c": result("c", "A", 1)}}
        d, detail = tune.spsa_diff("margin", plus, minus)
        self.assertEqual(detail, {"better": 1, "ties": 1, "worse": 1})
        self.assertAlmostEqual(d, 0.0)

    def test_the_roles_share_their_ids_between_the_arms_and_move_on_each_iteration(self):
        r0, r1 = tune.spsa_roles("levels-normal", 0), tune.spsa_roles("levels-normal", 1)
        self.assertEqual(set(r0), {"mirror", "ota"})
        self.assertTrue(all(g.startswith("bar-m:L3-") for g in r0["mirror"]))
        self.assertTrue(all(g.startswith("bar-p:L3-") for g in r0["ota"]))
        self.assertEqual(len(r0["mirror"]), 10)
        self.assertFalse(set(r0["mirror"]) & set(r1["mirror"]))
        self.assertTrue(all(g.startswith("bar-n:L3-") for g in tune.spsa_roles("levels-easy", 0)["mirror"]))
        self.assertEqual(len(tune.spsa_roles("margin", 0)["pairs"]), 16)
        self.assertTrue(all(g.startswith("bar-h:2.000-") for g in tune.spsa_roles("handicap", 0, "h:2.000")["points"]))

    def test_the_settings_seat_each_arm(self):
        self.assertEqual(tune.spsa_settings("levels-normal", "mirror", "R"), {"persona": ["B=R"], "tune": []})
        self.assertEqual(tune.spsa_settings("levels-easy", "mirror", "R", "N"), {"persona": ["A=N", "B=R"], "tune": []})
        self.assertEqual(tune.spsa_settings("levels-easy", "ota", "R", "N"), {"persona": ["A=R"], "tune": []})
        self.assertEqual(tune.spsa_settings("margin", "pairs", {"lead_in": 3000}),
                         {"persona": [], "tune": ["A=lead_in=3000"]})


def strength(row: str) -> float:
    """The fake world's strength of a persona row: its mean coordinate in the persona space, 0 harshest, 1 Hard's."""
    return float(np.mean(tune.PersonaSpace().encode(row)))


def unit(*parts) -> float:
    return int.from_bytes(hashlib.sha256(" ".join(map(str, parts)).encode()).digest()[:4], "big") / 2 ** 32


class FakePlayer:
    """Writes each piece's results as bar.py would, from the row its arm plays: Hard beats a row of strength s in
    the mirror when a draw of the id falls below 1 - 0.3 s, and a row beats OTA AI when a draw falls below 0.5 +
    0.5 s; so the stronger a row, the more it scores against OTA AI and the fewer mirror games Hard wins."""

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
            row = [x.split("=", 1)[1] for x in p["persona"] if x.startswith("B=" if p["role"] == "mirror" else "A=")][0]
            s = strength(row)
            for gid in p["ids"]:
                if gid in self.skip:
                    continue
                u = unit(gid, row)
                win = (u < 1 - 0.3 * s) if p["role"] == "mirror" else (u < 0.5 + 0.5 * s)
                (out / f"{gid}.json").write_text(json.dumps(result(gid, "A" if win else "B")))
        return []


class Run(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(__import__("shutil").rmtree, self.tmp, True)
        self.dll = self.tmp / "x.dll"
        self.dll.write_bytes(b"dll")

    def run_(self, d, player):
        with mock.patch.dict("os.environ", {"TARUN_DIR": str(self.tmp / "runs")}), mock.patch("builtins.print"):
            return tune.spsa_run(d, self.dll, 10, launch=lambda p: None, play=player)

    def init(self, name, iters=4, seed=3):
        with mock.patch("builtins.print"):
            return tune.SpsaRun.init(self.tmp / name, "levels-normal", seed, iters)

    def state(self, name):
        return json.loads((self.tmp / name / tune.SPSA_STATE).read_text())

    def test_a_run_plays_each_iteration_on_common_games_and_records_it(self):
        self.init("a")
        p = FakePlayer()
        self.assertEqual(self.run_(self.tmp / "a", p), 0)
        st = self.state("a")
        self.assertEqual([it["k"] for it in st["iterations"]], [0, 1, 2, 3])
        self.assertEqual(st["search"]["k"], 4)
        self.assertEqual(len(p.played), 4 * 4)                    # two arms x two roles, a block each
        k0 = sorted((self.tmp / "a" / "games" / "k000-plus").glob("*.json"))
        self.assertEqual([f.name for f in k0], sorted(f.name for f in (self.tmp / "a" / "games" / "k000-minus").glob("*.json")))
        for it in st["iterations"]:
            self.assertIsNone(level_lever.persona_error(f"1={it['plus']}"))
            self.assertNotEqual(it["plus"], it["minus"])

    def test_the_same_seed_is_the_same_run(self):
        self.init("a")
        self.init("b")
        self.run_(self.tmp / "a", FakePlayer())
        self.run_(self.tmp / "b", FakePlayer())
        strip = lambda st: [{k: v for k, v in it.items()} for it in st["iterations"]]   # noqa: E731
        self.assertEqual(strip(self.state("a")), strip(self.state("b")))
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
        gid = tune.spsa_roles("levels-normal", 0)["ota"][3]
        p = FakePlayer(skip={gid})
        self.assertEqual(self.run_(self.tmp / "a", p), 2)
        self.assertEqual(len(p.played), 8)                         # the iteration's four pieces, twice
        self.assertEqual(self.state("a")["iterations"], [])

    def test_another_dll_is_refused(self):
        self.init("a", iters=1)
        self.run_(self.tmp / "a", FakePlayer())
        other = self.tmp / "y.dll"
        other.write_bytes(b"other")
        with mock.patch("builtins.print"):
            self.assertEqual(tune.spsa_run(self.tmp / "a", other, 10, launch=lambda p: None, play=FakePlayer()), 2)

    def test_the_search_moves_the_row_towards_the_mirrors_bound(self):
        # the fake world: o = 0.5 + 0.5 s rises with strength, w = 1 - 0.3 s falls; the bound w >= 0.9 holds up to
        # s = 1/3, so a search from the harshest row climbs towards s 1/3 and is held near it
        with mock.patch("builtins.print"):
            tune.SpsaRun.init(self.tmp / "a", "levels-normal", 7, 30, start="L0b")
        self.run_(self.tmp / "a", FakePlayer())
        st = self.state("a")
        s_end = strength(tune.PersonaSpace().decode(st["search"]["theta"]))
        self.assertGreater(s_end, strength(bar.PERSONA_ROWS["L0b"]) + 0.1)
        self.assertLess(s_end, 0.6)

    def test_init_refusals(self):
        with mock.patch("builtins.print"):
            tune.SpsaRun.init(self.tmp / "a", "levels-normal", 1, 2)
            with self.assertRaises(tune.TuneError):
                tune.SpsaRun.init(self.tmp / "a", "levels-normal", 1, 2)
            with self.assertRaises(tune.TuneError):
                tune.SpsaRun.init(self.tmp / "b", "handicap", 1, 2, params=["lead_in"])
            with self.assertRaises(tune.TuneError):
                tune.SpsaRun.init(self.tmp / "c", "levels-normal", 1, 2, start="150,150,150,0,0,0,0,0,0")


if __name__ == "__main__":
    unittest.main()
