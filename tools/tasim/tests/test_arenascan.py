"""The arena scan's offline pieces on synthetic data: the engine's meteor rule, the flat rectangle,
the layout drawn on it.

    python3 tools/tasim/tests/test_arenascan.py

No install: a map is a height grid built here.
"""

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import arenascan as A  # noqa: E402

ON = {"meteorweapon": "earthquake", "meteorradius": "500", "meteordensity": ".5",
      "meteorduration": "3", "meteorinterval": "360"}


class FakeTnt:
    def __init__(self, rows, sealevel=20):
        self.h16, self.w16 = len(rows), len(rows[0])
        self.sealevel = sealevel
        self._hs = bytes(v for row in rows for v in row)

    def heights(self):
        return self._hs


def grid(w, h, fill=10):
    return [[fill] * w for _ in range(h)]


class MeteorRule(unittest.TestCase):
    def test_every_key_set_switches_it_on(self):
        self.assertTrue(A.meteors_on(ON))

    def test_any_key_empty_or_zero_leaves_it_off(self):
        for k, off in (("meteorweapon", ""), ("meteorradius", "0"), ("meteordensity", "0"),
                       ("meteorduration", "0.0"), ("meteorinterval", "0")):
            keys = dict(ON, **{k: off})
            self.assertFalse(A.meteors_on(keys), k)
            del keys[k]
            self.assertFalse(A.meteors_on(keys), f"{k} absent")

    def test_no_keys_is_off(self):
        self.assertFalse(A.meteors_on({}))


class FlatRect(unittest.TestCase):
    def test_finds_the_longest_one_height_dry_rectangle(self):
        rows = grid(60, 40)
        for j in range(5, 30):              # a 40 x 25 plateau at 50
            for i in range(10, 50):
                rows[j][i] = 50
        for j in range(12, 15):             # a 3-sample notch at another height splits it
            rows[j][30] = 51
        got = A.flat_rect(FakeTnt(rows), border=0, min_short=16 * 9)
        self.assertEqual(got, (10, 15, 40, 15, 50))

    def test_water_is_not_flat_ground(self):
        rows = grid(40, 40, fill=5)         # all below the sea at 20
        self.assertIsNone(A.flat_rect(FakeTnt(rows), border=0, min_short=16))

    def test_the_border_is_kept_clear(self):
        rows = grid(30, 30, fill=40)
        i0, j0, ni, nj, _v = A.flat_rect(FakeTnt(rows), border=4, min_short=16)
        self.assertEqual((i0, j0, ni, nj), (4, 4, 22, 22))

    def test_the_short_side_bound_wins_over_length(self):
        rows = grid(80, 40, fill=5)
        for i in range(0, 80):              # a 80 x 3 causeway
            for j in range(2, 5):
                rows[j][i] = 30
        for j in range(10, 40):             # a 30 x 30 block
            for i in range(0, 30):
                rows[j][i] = 30
        got = A.flat_rect(FakeTnt(rows), border=0, min_short=16 * 10)
        self.assertEqual(got[2:4], (30, 30))


class Layout(unittest.TestCase):
    def test_duel_line_and_courses_inside_the_rectangle(self):
        row = {"name": "m", "rect": [1000, 2000, 5000, 1600], "height": 80, "sealevel": 20}
        f = A.arena_fields(row)
        self.assertEqual(f["axis"], "x")
        self.assertEqual(f["a"], [1000 + A.MARGIN, 2800])
        self.assertEqual(f["b"], [6000 - A.MARGIN, 2800])
        for (s, e) in f["courses"].values():
            for x, y in (s, e):
                self.assertTrue(1000 <= x <= 6000 and 2000 <= y <= 3600)

    def test_a_tall_rectangle_lays_the_duel_along_y(self):
        row = {"name": "m", "rect": [0, 0, 1600, 4000], "height": 80, "sealevel": 20}
        f = A.arena_fields(row)
        self.assertEqual((f["axis"], f["a"][0], f["a"][1]), ("y", 800, A.MARGIN))



ARENA_X = {"map": "m", "flat": {"rect": [1000, 2000, 5000, 1600]}, "axis": "x",
           "a": [1256, 2800], "b": [5744, 2800]}


class Placement(unittest.TestCase):
    DOC = {"units": [{"id": "a", "type": "ARMPW", "owner": 0, "pos": [0, 0], "facing": 0,
                      "orders": [{"cmd": "attack", "target": "b"}]},
                     {"id": "b", "type": "CORAK", "owner": 1, "pos": [0, 0]}],
           "calib": {"place": {"a": [0, 0, "a"], "b": [400, -32, "b"]}}}

    def test_units_go_to_their_place_on_the_arena(self):
        got = A.placed(self.DOC, ARENA_X, "t")
        self.assertEqual([u["pos"] for u in got["units"]], [[1256, 2800], [1656, 2768]])
        self.assertEqual([u.get("facing") for u in got["units"]], [90, 270])
        arena_y = dict(ARENA_X, axis="y", a=[1800, 2256])
        got = A.placed(self.DOC, arena_y, "t")
        self.assertEqual([u["pos"] for u in got["units"]], [[1800, 2256], [1768, 2656]])
        self.assertEqual([u.get("facing") for u in got["units"]], [0, 180])

    def test_a_unit_with_no_place_is_refused(self):
        doc = {"units": self.DOC["units"], "calib": {"place": {"a": [0, 0, "a"]}}}
        with self.assertRaises(A.calibref.CalibError):
            A.placed(doc, ARENA_X, "t")

    def test_positions_off_the_flat_rectangle_are_refused(self):
        A.calibref.check_positions(A.placed(self.DOC, ARENA_X, "t"), ARENA_X, "t")
        slate = {"units": [{"id": "a", "type": "ARMPW", "owner": 0, "pos": [8100, 3912]}]}
        with self.assertRaises(A.calibref.CalibError) as e:
            A.calibref.check_positions(slate, ARENA_X, "slate")
        self.assertIn("outside the arena's flat rectangle", str(e.exception))
        away = {"units": [{"id": "a", "type": "ARMPW", "owner": 0, "pos": [1256, 2800],
                           "orders": [{"cmd": "move", "to": [9000, 2800]}]}]}
        with self.assertRaises(A.calibref.CalibError):
            A.calibref.check_positions(away, ARENA_X, "away")

    def test_the_tracked_scenarios_are_laid_out_on_the_configured_arena(self):
        cfg = A.calibref.arena()
        self.assertGreaterEqual(A.check_placed(cfg), 4)
        for path, doc in A.placed_scenarios():
            A.calibref.check_positions(doc, cfg, path.name)


if __name__ == "__main__":
    unittest.main()
