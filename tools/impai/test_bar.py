#!/usr/bin/env python3
"""test_bar -- the bar harness offline: its game list, the seating a game id gives, the start schema it reads
from an .ota, the stall rule, the judge over synthetic results, and `play` on fake lanes: a game that could not
run, and a run stopped by SIGINT. No game runs.

    python3 tools/impai/test_bar.py"""
import json
import os
import signal
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

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
                r = {"id": gid, "outcome": "A", "end_tick": 30000, "wall_s": 100.0, "stalls": {"A": []},
                     "renderer": "gdi"}
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
            put(bar.ids("hota")[0])
            self.assertEqual(bar.judge([d]), 0)
            put(bar.ids("hota")[0], renderer="vulkan")             # one default-renderer game among GDI ones
            self.assertEqual(bar.judge([d]), 2)
            put(bar.ids("hota")[0])
            r = json.loads((d / f"{bar.ids('hota')[1]}.json").read_text())
            del r["renderer"]                                       # a result that names no renderer
            (d / f"{bar.ids('hota')[1]}.json").write_text(json.dumps(r))
            with mock.patch("builtins.print") as said:
                self.assertEqual(bar.judge([d]), 2)
            self.assertIn(bar.ids("hota")[1], said.call_args.args[0])

    def test_a_kept_game_is_one_played_as_asked(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            g = dict(bar.parse_id("bar-hota-pincushion-ac-1"))
            self.assertIsNone(bar.game_kept(d, g, False))
            r = {"id": g["id"], "outcome": "A", "cap": bar.CAP, "renderer": "gdi"}
            (d / f"{g['id']}.json").write_text(json.dumps(r))
            self.assertEqual(bar.game_kept(d, g, False)["outcome"], "A")
            self.assertIsNone(bar.game_kept(d, g, True))           # played with GDI, asked on the GPU
            self.assertIsNone(bar.game_kept(d, dict(g, cap=9000), False))
            (d / f"{g['id']}.json").write_text(json.dumps(dict(r, renderer=None, outcome="cap")))
            self.assertIsNone(bar.game_kept(d, g, False))
            (d / f"{g['id']}.json").write_text(json.dumps(dict(r, outcome="could_not_run")))
            self.assertIsNone(bar.game_kept(d, g, False))
            (d / f"{g['id']}.json").write_text("{")
            self.assertIsNone(bar.game_kept(d, g, False))
            t = dict(g, seats={"A": dict(g["seats"]["A"], table_hash="aa"), "B": g["seats"]["B"]})
            (d / f"{g['id']}.json").write_text(json.dumps(dict(r, tables={"A": "bb"})))
            self.assertIsNone(bar.game_kept(d, t, False))           # another table
            (d / f"{g['id']}.json").write_text(json.dumps(dict(r, tables={"A": "aa"})))
            self.assertIsNotNone(bar.game_kept(d, t, False))

    def test_play_skips_the_kept_games_and_takes_no_token_without_gpu(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            ids = bar.ids("hota")[:3]
            (d / f"{ids[0]}.json").write_text(json.dumps({"id": ids[0], "outcome": "A", "cap": bar.CAP,
                                                          "renderer": "gdi"}))
            played = []

            def play(game, lane, out, gpu):
                played.append((game["id"], gpu))
                return {"id": game["id"], "outcome": "B", "cap": game["cap"], "renderer": bar.renderer(gpu)}
            import runlock
            for gpu in (False, True):
                played.clear()
                with mock.patch.object(bar, "play_game", play), mock.patch.object(runlock, "hold") as hold, \
                        mock.patch.object(bar, "close_lanes"), mock.patch("sys.stdout"):
                    argv = ["play", "--ids", ",".join(ids), "--lanes", "2", "--out", str(d)] + (["--gpu"] if gpu else [])
                    self.assertEqual(bar.main(argv), 0)
                self.assertEqual(hold.call_args.kwargs["gpu"], gpu)
                if not gpu:
                    self.assertEqual(sorted(played), [(ids[1], False), (ids[2], False)])
                else:                                       # every GDI result is another renderer's
                    self.assertEqual(sorted(played), [(i, True) for i in ids])


class TestPlay(unittest.TestCase):
    """`play` on fake lanes: what a game that could not run writes, and what SIGINT does to a run."""

    def setUp(self):
        import tareplay
        self.R = tareplay
        self.R.STOP.clear()
        self.addCleanup(self.R.STOP.clear)
        self.addCleanup(bar._LANES.clear)
        self.events = {"played": [], "abandoned": [], "kept": [], "closed": []}
        R, ev = self.R, self.events

        class FakeLane:
            game = None                 # the fake game every lane plays: a function of the game's id

            def __init__(self, j, out, dll, gpu=False):
                self.j, self.out, self.log = j, out, None

            def play(self, game):
                R.stopping()
                ev["played"].append(game["id"])
                return FakeLane.game(game["id"])

            def abandon(self):
                ev["abandoned"].append(self.j)

            def keep_log(self, gid):
                ev["kept"].append(gid)

            def close(self):
                ev["closed"].append(self.j)
        self.Lane = FakeLane

    def play(self, ids, lanes):
        import runlock
        with tempfile.TemporaryDirectory() as d, mock.patch.object(bar, "Lane", self.Lane), \
                mock.patch.object(bar, "_dll", lambda: Path("ddraw.dll")), mock.patch.object(runlock, "hold"), \
                mock.patch.object(runlock, "release") as release, mock.patch("sys.stdout"):
            rc = bar.main(["play", "--ids", ",".join(ids), "--lanes", str(lanes), "--out", d])
            written = {p.stem: json.loads(p.read_text()) for p in Path(d).glob("bar-*.json")}
        return rc, written, release.called

    def test_a_game_that_could_not_run_names_its_renderer_and_keeps_its_log(self):
        def game(gid):
            raise self.R.CouldNotRun("no answer")
        self.Lane.game = staticmethod(game)
        gid = bar.ids("hota")[0]
        rc, written, released = self.play([gid], 1)
        self.assertEqual(rc, 2)
        self.assertEqual({k: written[gid][k] for k in ("outcome", "reason", "renderer")},
                         {"outcome": "could_not_run", "reason": "no answer", "renderer": "gdi"})
        self.assertEqual((self.events["abandoned"], self.events["kept"], self.events["closed"]), ([0], [gid], [0]))
        self.assertTrue(released)

    def test_sigint_stops_the_games_playing_and_starts_none(self):
        R, ev = self.R, self.events
        both = threading.Event()

        def game(gid):
            if len(ev["played"]) == 2:
                both.set()
            end = time.monotonic() + 30
            while time.monotonic() < end:
                R.nap(0.05)             # a game's wait, which gives up once STOP is set
            raise AssertionError(f"{gid} was never stopped")
        self.Lane.game = staticmethod(game)

        def interrupt():
            if both.wait(30):
                os.kill(os.getpid(), signal.SIGINT)
        threading.Thread(target=interrupt, daemon=True).start()
        ids = bar.ids("hota")[:5]
        rc, written, released = self.play(ids, 2)
        self.assertEqual(rc, 2)
        self.assertEqual(sorted(written), sorted(ev["played"]))     # the two playing; the three queued not written
        self.assertEqual(len(written), 2)
        for r in written.values():
            self.assertEqual((r["outcome"], r["reason"], r["renderer"]), ("could_not_run", "stopped", "gdi"))
        self.assertEqual(set(ev["abandoned"]), {0, 1})               # each game stopped through its own lane
        self.assertEqual(ev["kept"], [])                             # a stopped game is not a game to diagnose
        self.assertEqual(sorted(ev["closed"]), [0, 1])
        self.assertTrue(released)
        self.assertIs(signal.getsignal(signal.SIGINT), signal.default_int_handler)


if __name__ == "__main__":
    unittest.main()
