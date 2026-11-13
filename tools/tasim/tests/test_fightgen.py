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


def unit(layer="land", f241=D.F241_WEAPON, f245=D.F245_CANATTACK, slots=(0, None), waterline=0):
    return {"layer": layer, "attack": {"f241": f241, "f245": f245, "slots": list(slots), "waterline": waterline}}


class AttackRule(unittest.TestCase):
    def test_the_resolvers_refusals(self):
        tank, ship, sub = unit(), unit("sea"), unit("sea", slots=(D.WF_WATER, None), waterline=20)
        self.assertTrue(D.takes_attack(tank, ship))
        self.assertFalse(D.takes_attack(unit(f245=0), tank), "no canattack")
        self.assertFalse(D.takes_attack(unit(f241=0), tank), "no weapon, no kamikaze")
        self.assertTrue(D.takes_attack(unit(f241=D.F241_KAMIKAZE), tank), "a kamikaze without a weapon")
        self.assertFalse(D.takes_attack(unit(slots=(D.WF_TOAIR, None)), tank), "slot 0 anti-air on the ground")
        self.assertFalse(D.takes_attack(ship, sub), "a target that may be submerged, no waterweapon")
        self.assertTrue(D.takes_attack(unit("sea", slots=(0, D.WF_WATER)), sub), "slot 1's waterweapon")
        self.assertTrue(D.takes_attack(sub, ship))
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
        ship = row("SHIP", layer="sea")
        sub = row("SUB", layer="sea", slots=(D.WF_WATER, None), waterline=20)
        spy = row("SPY", f241=0)
        _n, doc = D.write_fight("t", [(ship, 2), (spy, 1)], [(sub, 2)], arena, scn, "a test", "test")
        orders = {u["id"]: u.get("orders") for u in doc["units"]}
        self.assertEqual(orders["a0"], None)
        self.assertEqual(orders["a2"], None)
        self.assertEqual(orders["b0"], [{"cmd": "attack", "target": "a0"}])
        self.assertEqual(orders["b1"], [{"cmd": "attack", "target": "a1"}])
        with self.assertRaises(D.GenError):
            D.write_fight("t", [(spy, 1)], [(row("SPY2", f241=0), 1)], arena, scn, "a test", "test")


class RunsFolder(unittest.TestCase):
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
