"""The fight generators' attack rule and the runs folder's readers: what the engine's resolver takes, the lanes
every stamp folder holds, and the cost a run takes by kind.

    .venv-undither/bin/python -m pytest tools/tasim/tests/test_fightgen.py
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parents[2] / "tools" / "replay"))
import calib  # noqa: E402
import duelgen as D  # noqa: E402
import engrun  # noqa: E402


def unit(layer="land", f241=D.F241_WEAPON, f245=D.F245_CANATTACK, slots=(0, None), waterline=0, bmcode=1):
    return {"layer": layer, "attack": {"f241": f241, "f245": f245, "slots": list(slots), "waterline": waterline,
                                       "bmcode": bmcode}}


class AttackRule(unittest.TestCase):
    def test_the_resolvers_refusals(self):
        floater = D.F241_WEAPON | D.F241_FLOATER
        tank, ship = unit(), unit("sea", f241=floater)
        sub = unit("sea", slots=(D.WF_WATER, None), waterline=20)
        self.assertTrue(D.takes_attack(tank, ship))
        self.assertFalse(D.takes_attack(unit(f245=0), tank), "no canattack")
        self.assertFalse(D.takes_attack(unit(f241=0), tank), "no weapon, no kamikaze")
        self.assertTrue(D.takes_attack(unit(f241=D.F241_KAMIKAZE), tank), "a kamikaze without a weapon")
        self.assertFalse(D.takes_attack(unit(slots=(D.WF_TOAIR, None)), tank), "slot 0 anti-air on the ground")
        self.assertFalse(D.takes_attack(ship, sub), "a target that may be submerged, no waterweapon")
        self.assertTrue(D.takes_attack(unit("sea", slots=(0, D.WF_WATER)), sub), "slot 1's waterweapon")
        self.assertTrue(D.takes_attack(sub, ship))
        for f241, why in ((D.F241_WEAPON | D.F241_UPRIGHT, "upright: placed on the sea floor"),
                          (floater | D.F241_UPRIGHT, "upright wins over floater"),
                          (D.F241_WEAPON, "neither: fitted to the terrain")):
            self.assertFalse(D.takes_attack(ship, unit("sea", f241=f241)), f"waterline 0 but {why}")
        self.assertFalse(D.takes_attack(ship, unit("sea", f241=floater, waterline=3)), "a floater below the sea")
        hover = unit(f241=D.F241_WEAPON | D.F241_CANHOVER, slots=(D.WF_WATER, None))
        self.assertFalse(D.takes_attack(hover, tank), "a hover's waterweapon on a surface target")
        self.assertFalse(D.takes_attack(hover, sub), "taken only if the target is submerged, which is unknown")
        self.assertFalse(D.takes_attack(unit(f241=D.F241_WEAPON | D.F241_CANFLY), tank), "no flyer is ordered")
        self.assertFalse(D.takes_attack({"layer": "land", "attack": None}, tank), "a type the defs lack")

    def test_a_side_only_attacks_what_it_can(self):
        arena = {"a": [100, 1000], "axis": "x", "flat": {"rect": [0, 0, 4000, 2000]}}
        scn = {"setup": {"players": [{"storage": {"metal": 1e6, "energy": 1e6}}] * 2}}

        def row(name, **kw):
            r = unit(**kw)
            r.update({"name": name, "dps_ground": 1, "range_ground": 300, "footprint": [2, 2],
                      "shot_cost_per_tick": [0, 0]})
            return r
        ship = row("SHIP", layer="sea", f241=D.F241_WEAPON | D.F241_FLOATER)
        sub = row("SUB", layer="sea", slots=(D.WF_WATER, None), waterline=20)
        spy = row("SPY", f241=0)
        _n, doc = D.write_fight("t", [(ship, 2), (spy, 1)], [(sub, 2)], arena, scn, "a test", "test")
        orders = {u["id"]: u.get("orders") for u in doc["units"]}
        self.assertEqual(orders["a0"], None)
        self.assertEqual(orders["a1"], None)
        self.assertEqual(orders["a2"], None)
        # one row of three at across -96, -32, 32 (step 64); B 460 along at -96 and -32: b1 is as far from a0
        # as from a2, and takes a0 first
        self.assertEqual(orders["b0"], [{"cmd": "attack", "target": "a0"},
                                        {"cmd": "attack", "target": "a1", "queue": True},
                                        {"cmd": "attack", "target": "a2", "queue": True}])
        self.assertEqual(orders["b1"], [{"cmd": "attack", "target": "a1"},
                                        {"cmd": "attack", "target": "a0", "queue": True},
                                        {"cmd": "attack", "target": "a2", "queue": True}])
        with self.assertRaises(D.GenError):
            D.write_fight("t", [(spy, 1)], [(row("SPY2", f241=0), 1)], arena, scn, "a test", "test")

    def test_each_unit_queues_every_enemy_nearest_first(self):
        """THE QUEUE RULE on 3 against 3: every enemy the resolver takes, by the squared distance between the
        scenario positions, ties to the lower index; `queue` on the second and later; no order for a unit that
        can attack none."""
        arena = {"a": [100, 1000], "axis": "x", "flat": {"rect": [0, 0, 4000, 2000]}}
        scn = {"setup": {"players": [{"storage": {"metal": 1e6, "energy": 1e6}}] * 2}}

        def row(name, **kw):
            r = unit(**kw)
            r.update({"name": name, "dps_ground": 1, "range_ground": 300, "footprint": [2, 2],
                      "shot_cost_per_tick": [0, 0]})
            return r
        tank, spy = row("TANK"), row("SPY", f241=0)
        sub = row("SUB", layer="sea", slots=(D.WF_WATER, None), waterline=20)
        _n, doc = D.write_fight("t", [(tank, 2), (spy, 1)], [(tank, 1), (sub, 1), (row("TANK2"), 1)], arena,
                                scn, "a test", "test")

        def chain(*ids):
            return [dict({"cmd": "attack", "target": t}, **({"queue": True} if i else {}))
                    for i, t in enumerate(ids)]
        orders = {u["id"]: u.get("orders") for u in doc["units"]}
        # both rows across -96, -32, 32; a tank cannot attack the sub, which may be submerged
        self.assertEqual(orders["a0"], chain("b0", "b2"))
        self.assertEqual(orders["a1"], chain("b0", "b2"), "b0 and b2 are equally far: the lower index first")
        self.assertIsNone(orders["a2"], "a spy attacks nothing")
        self.assertEqual(orders["b0"], chain("a0", "a1", "a2"))
        self.assertEqual(orders["b1"], chain("a1", "a0", "a2"))
        self.assertEqual(orders["b2"], chain("a2", "a1", "a0"))

    def test_a_structure_gets_no_order_and_is_queued_on(self):
        """A unit without a mover (bmcode 0) keeps the engine's standing order; the attackers queue on it."""
        arena = {"a": [100, 1000], "axis": "x", "flat": {"rect": [0, 0, 4000, 2000]}}
        scn = {"setup": {"players": [{"storage": {"metal": 1e6, "energy": 1e6}}] * 2}}

        def row(name, **kw):
            r = unit(**kw)
            r.update({"name": name, "dps_ground": 1, "range_ground": 300, "footprint": [2, 2],
                      "shot_cost_per_tick": [0, 0]})
            return r
        tower = row("TOWER", bmcode=0)
        _n, doc = D.write_fight("t", [(row("TANK"), 2)], [(tower, 2)], arena, scn, "a test", "test")
        orders = {u["id"]: u.get("orders") for u in doc["units"]}
        self.assertIsNone(orders["b0"])
        self.assertIsNone(orders["b1"])
        self.assertEqual(orders["a0"], [{"cmd": "attack", "target": "b0"},
                                        {"cmd": "attack", "target": "b1", "queue": True}])
        self.assertEqual(orders["a1"], [{"cmd": "attack", "target": "b1"},
                                        {"cmd": "attack", "target": "b0", "queue": True}])
        with self.assertRaises(D.GenError):
            D.write_fight("t", [(row("TOWER2", bmcode=0), 1)], [(tower, 1)], arena, scn, "a test", "test")


class RunsFolder(unittest.TestCase):
    def test_refs_build_refuses_a_runs_folder_of_another_root(self):
        import calibref
        import os
        from unittest import mock
        with tempfile.TemporaryDirectory() as tmp:
            root, other = Path(tmp) / "calib-rt", Path(tmp) / "calib"
            (root / "runs" / "p0").mkdir(parents=True)
            (other / "runs" / "p0").mkdir(parents=True)
            with mock.patch.dict(os.environ, {"TACALIB_DIR": str(root)}):
                self.assertEqual(calibref.runs_in_root(root / "runs" / "p0"), (root / "runs" / "p0").resolve())
                with self.assertRaises(calibref.CalibError):
                    calibref.runs_in_root(other / "runs" / "p0")
                with mock.patch("builtins.print") as out:
                    self.assertEqual(calib.cmd_refs(mock.Mock(runs_dir=str(other / "runs" / "p0"))), 2)
                self.assertIn("is not in the calibration root", out.call_args[0][0])

    def test_stamps_cost_and_refusals(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for stamp, lane, secs, rows in (("s1", "lane0-1", 30.0, [("group-001", 0), ("group-001", 1)]),
                                            ("s2", "lane1-1", 20.0, [("duel-x", 0)])):
                d = root / stamp / lane
                d.mkdir(parents=True)
                lines = [{"lane_seconds": secs}] + [{"scenario": f"gen/{s}.json", "index": i, "wall": {"total": 4.0}}
                                                   for s, i in rows]
                lines.append({"refused": "gen/group-002.json", "refused_index": 0, "t": 1, "why": "x"})
                (d / "manifest.jsonl").write_text("".join(json.dumps(r) + "\n" for r in lines))
            lanes = engrun.finished_lanes(root)
            self.assertEqual([p.name for p in lanes], ["lane0-1", "lane1-1"])
            cost = calib.cost_by_kind(lanes)
            self.assertEqual(cost["group"], (2, 15.0, 4.0))
            self.assertEqual(cost["duel"], (1, 20.0, 4.0))


if __name__ == "__main__":
    unittest.main()
