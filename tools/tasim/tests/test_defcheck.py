"""defcheck's pure comparisons on synthetic records: the duplicate pairing, the [DAMAGE] table's bound and rows, the
weapon fingerprint and the catalogue comparison. No game, no install.

    .venv-undither/bin/python tools/tasim/tests/test_defcheck.py
"""

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import defcheck  # noqa: E402


class PairDuplicates(unittest.TestCase):
    def test_unique_names_pair_by_name(self):
        p = defcheck.pair_duplicates(["ARMCOM", "corcom"], {1: "armcom", 2: "CORCOM"}, lambda r, k: False)
        self.assertEqual(p.pairs, [(0, 1), (1, 2)])
        self.assertEqual((p.problems, p.shared, p.rows_alone, p.types_alone), ([], [], [], []))

    def test_duplicate_pairs_by_fields_either_way_round(self):
        names = ["A", "EMP", "EMP", "Z"]
        engine = {1: "A", 2: "EMP", 3: "EMP", 4: "Z"}
        crossed = defcheck.pair_duplicates(names, engine, lambda r, k: (r, k) in {(1, 3), (2, 2)})
        self.assertEqual(crossed.pairs, [(0, 1), (1, 3), (2, 2), (3, 4)])
        straight = defcheck.pair_duplicates(names, engine, lambda r, k: (r, k) in {(1, 2), (2, 3)})
        self.assertEqual(straight.pairs, [(0, 1), (1, 2), (2, 3), (3, 4)])
        self.assertEqual((crossed.shared, crossed.problems), (["emp"], []))

    def test_duplicate_with_no_equal_type_is_paired_in_order(self):
        p = defcheck.pair_duplicates(["EMP", "EMP"], {1: "EMP", 2: "EMP"}, lambda r, k: (r, k) == (1, 1))
        self.assertEqual(p.pairs, [(0, 2), (1, 1)])

    def test_count_per_name_differs(self):
        p = defcheck.pair_duplicates(["EMP", "EMP"], {1: "EMP"}, lambda r, k: True)
        self.assertEqual(p.problems, ["'emp': the core has 2 row(s), the engine 1 type(s)"])
        self.assertEqual((p.pairs, p.rows_alone), ([(0, 1)], [1]))

    def test_types_are_not_the_rows(self):
        p = defcheck.pair_duplicates(["A", "B"], {1: "B", 2: "A"}, lambda r, k: True)
        self.assertEqual(len(p.problems), 2)
        self.assertIn("the core's rows are types [1], the engine's [2]", p.problems[0])

    def test_a_name_one_side_lacks(self):
        p = defcheck.pair_duplicates(["A", "B"], {1: "A", 2: "C"}, lambda r, k: True)
        self.assertEqual((p.pairs, p.rows_alone, p.types_alone), ([(0, 1)], [1], [2]))


class Damage(unittest.TestCase):
    def test_extent(self):
        self.assertEqual(defcheck.damage_extent(0x1000, 0x1018), 3)
        self.assertEqual(defcheck.damage_extent(0x1000, 0x1000), 0)
        self.assertIsNone(defcheck.damage_extent(0x1018, 0x1000))
        self.assertIsNone(defcheck.damage_extent(0x1000, 0x1004))
        self.assertIsNone(defcheck.damage_extent(0x1000, 0x1000 + 8 * (defcheck.DMG_MAX + 1)))
        self.assertEqual(defcheck.damage_extent(0x1000, 0x1000 + 8 * defcheck.DMG_MAX), defcheck.DMG_MAX)

    def bad(self, engine, engine_long, core, core_long):
        return [text for ok, text in defcheck.compare_damage("W", engine, engine_long, core, core_long) if not ok]

    def test_equal_and_case_is_unobservable(self):
        self.assertEqual(self.bad([("armcom", 10), ("CORCOM", 20)], 0, [("CorCom", 20), ("ARMCOM", 10)], 0), [])

    def test_damage_differs(self):
        bad = self.bad([("ARMCOM", 10)], 0, [("ARMCOM", 11)], 0)
        self.assertEqual(bad, ["W: row 0 of 1/1 by name: core ('ARMCOM', 11) engine ('ARMCOM', 10)"])

    def test_name_differs(self):
        self.assertEqual(len(self.bad([("ARMCOM", 10)], 0, [("ARMCOMX", 10)], 0)), 1)

    def test_fold_is_ascii_only(self):
        self.assertEqual(len(self.bad([("\xe9", 1)], 0, [("\xc9", 1)], 0)), 1)

    def test_a_row_missing(self):
        bad = self.bad([("A", 1), ("B", 2)], 0, [("A", 1)], 0)
        self.assertEqual(bad, ["W: row 1 of 1/2 by name: core None engine ('B', 2)"])

    def test_long_names_counted(self):
        self.assertEqual(self.bad([], 1, [], 1), [])
        self.assertEqual(self.bad([], 2, [], 1), ["W: rows of 32 characters or more: core 1 engine 2"])


class NamesHash(unittest.TestCase):
    def test_known_slots(self):
        # by hand, prime 16777619 and basis 2166136261, mod 2**32: "" -> basis * prime = 0x050C5D1F;
        # "A" -> (0x050C5D1F ^ 0x41) * prime = 0x5076FAFA, then * prime = 0xA54D178E; "" -> * prime = 0xC65C148A
        self.assertEqual(defcheck.names_hash(["", "A", ""]), 0xC65C148A)

    def test_a_name_is_cut_at_32(self):
        self.assertEqual(defcheck.names_hash(["X" * 32 + "Y"]), defcheck.names_hash(["X" * 32]))


def core(units, weapons, slots=defcheck.WEAPON_IDS):
    return {"counts": {"weapon_slots": slots},
            "units": [{"unitname": n, "key": k} for n, k in units],
            "weapons": [{"id": i, "section": s} for i, s in weapons]}


def verdict(c, types=None, keys=None, weapons=None):
    slots = [""] * defcheck.WEAPON_IDS
    for w in c["weapons"]:
        slots[w["id"]] = w["section"]
    return {"setup": "s",
            "level": {"types": types or ["NONE"] + [u["unitname"].upper() for u in c["units"]],
                      "weapons": weapons or [defcheck.WEAPON_IDS, "0x%08X" % defcheck.names_hash(slots)]},
            "content": {"keys": keys or dict([("NONE", "0x00000000")] +
                                             [(u["unitname"].upper(), u["key"]) for u in c["units"]])}}


class Catalogue(unittest.TestCase):
    C = core([("armcom", "0x00000001"), ("EMP", "0x00000002"), ("EMP", "0x00000003")], [(1, "Gun"), (7, "Laser")])

    def test_equal(self):
        self.assertEqual(defcheck.compare_catalogue(self.C, verdict(self.C)), [])

    def test_type_order_differs(self):
        bad = defcheck.compare_catalogue(self.C, verdict(self.C, types=["NONE", "EMP", "ARMCOM", "EMP"]))
        self.assertEqual(len(bad), 1)
        self.assertTrue(bad[0].startswith("types: ") and "first difference at type 1" in bad[0])

    def test_either_key_of_a_shared_name(self):
        keys = {"NONE": "0x00000000", "ARMCOM": "0x00000001", "EMP": "0x00000002"}
        self.assertEqual(defcheck.compare_catalogue(self.C, verdict(self.C, keys=keys)), [])

    def test_key_differs(self):
        keys = {"NONE": "0x00000000", "ARMCOM": "0x00000009", "EMP": "0x00000003"}
        self.assertEqual(defcheck.compare_catalogue(self.C, verdict(self.C, keys=keys)),
                         ["key: ARMCOM engine 0x00000009, core 0x00000001"])

    def test_key_of_a_name_the_core_lacks(self):
        keys = {"NONE": "0x00000000", "ARMCOM": "0x00000001", "EMP": "0x00000003", "ZAP": "0x00000004"}
        self.assertEqual(defcheck.compare_catalogue(self.C, verdict(self.C, keys=keys)),
                         ["key: ZAP is not among the core's units"])

    def test_weapons_differ(self):
        bad = defcheck.compare_catalogue(self.C, verdict(self.C, weapons=[defcheck.WEAPON_IDS, "0x00000000"]))
        self.assertEqual(len(bad), 1)
        self.assertTrue(bad[0].startswith("weapons: "))

    def test_another_slot_count_cannot_run(self):
        c = core([("armcom", "0x00000001")], [(1, "Gun")], slots=256)
        with self.assertRaises(defcheck.CouldNotRun):
            defcheck.compare_catalogue(c, verdict(core([("armcom", "0x00000001")], [(1, "Gun")])))


if __name__ == "__main__":
    unittest.main()
