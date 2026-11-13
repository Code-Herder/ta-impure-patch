#!/usr/bin/env python3
"""test_bar -- the bar harness offline: its game list, the seating a game id gives, the start schema it reads
from an .ota, the stall rule and the judge over synthetic results. No game runs.

    python3 tools/impai/test_bar.py"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bar  # noqa: E402

OTA = b"""[GlobalHeader]
{
  missionname=Test;
  [Schema 0]
  {
    Type=Network 1;
    [specials]
    {
      [special0] { specialwhat=StartPos1; XPos=100; ZPos=200; }
      [special1] { specialwhat=StartPos2; XPos=300; ZPos=400; }
    }
  }
  [Schema 1]
  {
    Type=Network 2;
    [specials]
    {
      [special0] { specialwhat=StartPos1; XPos=110; ZPos=210; }
      [special1] { specialwhat=StartPos2; XPos=310; ZPos=410; }
      [special2] { specialwhat=StartPos3; XPos=510; ZPos=610; }
      [special3] { specialwhat=metal; XPos=1; ZPos=1; }
    }
  }
}
"""


class FakeVfs:
    def enumerate(self, top, pattern, recursive):
        return [("maps/Test.ota", "loose")]

    def read_from(self, path, where):
        return OTA


def pulse(seat, t, built, mroots, eroots, army):
    return (f"impai: pulse seat={seat} t={t} built={built} mroots={mroots} eroots={eroots} army={army} "
            f"squads=F metal=0 energy=0 vlive=0 vbuilt=0")


class TestBar(unittest.TestCase):
    def test_ids(self):
        for pair in bar.PAIRS:
            g = bar.ids(pair)
            self.assertEqual(len(g), 10 if pair == "dry" else 100)
            self.assertEqual(len(set(g)), len(g))
            for gid in g:
                self.assertEqual(bar.parse_id(gid)["id"], gid)

    def test_seating(self):
        g = bar.parse_id("bar-hota-pincushion-ac-1")
        self.assertEqual((g["start"], bar.seat_of(g)), (0, {"A": 1, "B": 2}))
        self.assertEqual(g["sides"], {"A": "arm", "B": "core"})
        self.assertEqual(g["seats"]["A"], {"ai": "impai", "level": "hard"})
        self.assertEqual(g["seats"]["B"]["ai"], "ai")
        g2 = bar.parse_id("bar-hota-pincushion-ac-2")
        self.assertEqual((g2["start"], bar.seat_of(g2)), (1, {"A": 2, "B": 1}))
        self.assertRaises(bar.BarError, bar.parse_id, "bar-xx-pincushion-ac-1")
        self.assertRaises(bar.BarError, bar.parse_id, "bar-hota-nowhere-ac-1")

    def test_starts_and_doc(self):
        starts = bar.start_positions("test", 3, FakeVfs())
        self.assertEqual(starts, {1: (110, 210), 2: (310, 410), 3: (510, 610)})
        g = bar.parse_id("bar-ne-lavamania-ca-2")
        doc = bar.game_doc(g, starts)
        self.assertEqual(doc["setup"]["map"], "Lava Mania")
        self.assertEqual([p["slot"] for p in doc["setup"]["players"]], [0, 1, 2])
        seat2 = doc["setup"]["players"][2]
        self.assertEqual((seat2["controller"], seat2["side"]), ("impai", "core"))     # A, at the second start
        self.assertEqual({u["owner"]: (u["type"], u["pos"]) for u in doc["units"]},
                         {2: ("CORCOM", [510, 610]), 1: ("ARMCOM", [310, 410])})

    def test_stalls(self):
        rows = bar.pulses([pulse(1, 1807, 5, 0, 10, 0), pulse(1, 3607, 5, 0, 10, 0), pulse(1, 5407, 5, 0, 10, 0),
                           pulse(1, 7207, 9, 3, 12, 4), pulse(1, 9007, 12, 3, 15, 4), pulse(1, 10807, 14, 3, 16, 5)])
        st = bar.stalls(rows[1], 20000)
        self.assertEqual([(w["from"], w["economy"], w["army"]) for w in st],
                         [(1807, True, False), (7207, False, True)])
        self.assertEqual(bar.stalls(rows[1], 9000), [{"from": 1807, "to": 5407, "economy": True, "army": False}])
        self.assertEqual(bar.stalls([], 9000), [])

    def test_judge(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)

            def put(gid, **kw):
                r = {"id": gid, "outcome": "A", "end_tick": 30000, "wall_s": 100.0, "stalls": {"A": []}}
                r.update(kw)
                (d / f"{gid}.json").write_text(json.dumps(r))
            for gid in bar.ids("hota"):
                put(gid)
            self.assertEqual(bar.judge([d]), 0)
            put(bar.ids("hota")[3], outcome="cap", end_tick=81000)
            self.assertEqual(bar.judge([d]), 1)
            put(bar.ids("hota")[3])
            put(bar.ids("hota")[4], stalls={"A": [{"from": 1807, "to": 5407, "economy": True, "army": False}]})
            self.assertEqual(bar.judge([d]), 1)
            put(bar.ids("hota")[4])
            for gid in bar.ids("hn")[:95]:
                put(gid, outcome="B" if gid.endswith("-3") and "-pincushion-" in gid else "A")
            self.assertEqual(bar.judge([d]), 2)                     # hn lacks five games
            for gid in bar.ids("hn")[95:]:
                put(gid)
            self.assertEqual(bar.judge([d]), 0)                     # 96 of 100: the ladder asks 90
            put(bar.ids("hota")[0], outcome="could_not_run", reason="x")
            self.assertEqual(bar.judge([d]), 2)


if __name__ == "__main__":
    unittest.main()
