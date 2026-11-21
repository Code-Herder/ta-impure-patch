#!/usr/bin/env python3
"""Tests of mp_agree.py's comparison of two peers' rosters."""
import sys
import unittest
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parent), str(Path(__file__).resolve().parent / "impai")]
import mp_agree  # noqa: E402


def unit(slot, owner, x, z, kind="ARMPW"):
    return {"id": slot, "type": kind, "owner": owner, "engine_index": slot, "world": [x, z]}


class Compare(unittest.TestCase):
    def setUp(self):
        # the host owns slots 1-2 (owner 0 there, 1 on the joiner); the joiner owns slot 3
        self.host = [unit(1, 0, 100, 100), unit(2, 0, 200, 200), unit(3, 1, 300, 300)]
        self.join = [unit(1, 1, 100, 100), unit(2, 1, 250, 200), unit(3, 0, 300, 300)]

    def test_agree(self):
        r = mp_agree.compare(self.host, self.join, 128)
        self.assertEqual(r["problems"], [])
        self.assertEqual((r["both"], r["identical"], r["off_max"]), (3, 2, 50))
        self.assertEqual((r["host_own"], r["join_own"]), (2, 1))

    def test_unit_on_one_peer(self):
        r = mp_agree.compare(self.host + [unit(4, 0, 1, 1)], self.join, 128)
        self.assertEqual(len(r["problems"]), 1)
        self.assertIn("host alone", r["problems"][0])

    def test_type_owner_and_position(self):
        self.join[0] = unit(1, 1, 100, 100, "ARMHAM")
        self.join[1] = unit(2, 0, 200, 200)
        self.join[2] = unit(3, 0, 900, 300)
        problems = " | ".join(mp_agree.compare(self.host, self.join, 128)["problems"])
        self.assertIn("ARMHAM on the joiner", problems)
        self.assertIn("own peer's on both", problems)
        self.assertIn("600 world units apart", problems)

    def test_owners_must_map_one_to_one(self):
        self.join[2] = unit(3, 1, 300, 300)
        self.host[2] = unit(3, 1, 300, 300)
        problems = " | ".join(mp_agree.compare(self.host, self.join, 128)["problems"])
        self.assertIn("neither", problems)


if __name__ == "__main__":
    unittest.main()
