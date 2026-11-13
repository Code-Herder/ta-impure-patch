#!/usr/bin/env python3
"""test_bar -- the bar harness offline: its game list, the seating a game id gives, the start schema it reads
from an .ota, the stall rule, the holds' end at the cap, what a result reads from the level's end, the judge and
the b4 and b5 checks over synthetic results; a game's end on a fake instance (the engine's exit, the recording
kept or deleted, the log kept for every non-win); and `play` on fake lanes: a game that could not run, and a run
stopped by SIGINT. No game runs.

    python3 tools/impai/test_bar.py"""
import json
import os
import shutil
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


def pulse(seat, t, built, mroots, eroots, army, metal=0):
    return (f"impai: pulse seat={seat} t={t} built={built} mroots={mroots} eroots={eroots} army={army} "
            f"squads=F metal={metal} energy=0 vlive=0 vbuilt=0")


def eco_end(seat, classes="lraider:4,lassault:2", ftech="1:2,2:1", yards=0, sea_plants=0):
    return (f"impai: eco seat={seat} decisions=360 orders=40 queued=8 failed=0 army=6 mex=3 energy=2 makers=0 "
            f"factories=3 cons=2 metal_in=9.5 energy_in=120.0 gametime=27015 sea_plants={sea_plants} built=31 "
            f"classes={classes} ftech={ftech} yards={yards}")


def mil_end(seat, naval=0, tech_m=8.0):
    return (f"impai: mil seat={seat} level=2 decisions=900 refused=0 predictions=60 refused_predictions=0 roots=300 "
            f"refused_orders=0 left_out=0 attacks=2 raids=3 retreats=4 scouts=5 defends=1 safe=0 dguns=0 "
            f"army_max=20/512 enemies_max=40/2048 contacts_max=3/32 predict_max=8/12 roots_max=30/40 naval={naval} "
            f"unreached=0 cross=0 tech_m={tech_m:.1f}")


def task(seat, t, attack=0, raid=0, scout=0):
    return (f"impai: mil seat={seat} t={t} attack={attack} raid={raid} scout={scout} retreat=0 safe=0 dgun=0 "
            f"army=6")


REC = "replays\\2026-11-13 153139 Pincushion.imprec"
REC_LINES = [f"recorder: recording {REC} from GameTime 1: 15001 slots, 38 tables",
             f"recorder: writer: {REC} closed: 900 iterations to GameTime 27015, 0 corrections"]


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


class TestLevelEnd(unittest.TestCase):
    """What a result reads from its launch's log once the level has ended, offline."""

    def test_the_level_end_lines_give_each_impai_seat_its_end(self):
        g = bar.parse_id("bar-hota-redtriangle-ac-2")                   # A in seat 2, B (OTA AI) in seat 1
        seat = bar.seat_of(g)
        lines = [task(2, 9000, scout=1), task(2, 12000, raid=1), task(2, 15000, attack=1), task(2, 16000, attack=1),
                 task(1, 100, attack=1),                                 # seat 1 is no ImpAI seat here
                 eco_end(2, yards=1, sea_plants=2), mil_end(2, naval=1),
                 pulse(2, 1807, 1, 0, 1, 0, metal=100), pulse(2, 3607, 2, 0, 2, 0, metal=700)]
        r = bar.read_result(lines, seat, g, "B", 20000)
        self.assertEqual(r["end"]["A"], {"built": 31, "army": {"lraider": 4, "lassault": 2}, "army_built": 6,
                                         "factories": {"1": 2, "2": 1}, "yards": 1, "sea_plants": 2,
                                         "metal_in": 9.5, "attacks": 2, "raids": 3, "scouts": 5, "naval": 1,
                                         "tech_m": 8.0})
        self.assertNotIn("B", r["end"])
        self.assertEqual(r["first"], {"A": {"attack": 15000, "raid": 12000, "scout": 9000}})
        self.assertEqual(r["income_max"], {"A": 10.0})                   # 600 metal over 60 passes
        self.assertEqual((r["outcome"], r["end_tick"]), ("B", 20000))

    def test_a_line_without_the_counts_reads_none_and_no_task_reads_none(self):
        g = bar.parse_id("bar-ne-pincushion-aa-1")                       # both seats ImpAI
        old = ("impai: eco seat=1 decisions=360 orders=40 queued=8 failed=0 army=6 mex=3 energy=2 makers=0 "
               "factories=3 cons=2 metal_in=9.5 energy_in=120.0 gametime=27015 sea_plants=0")
        r = bar.read_result([old, eco_end(2, classes="-", ftech="-")], bar.seat_of(g), g, "cap", 81000)
        self.assertEqual((r["end"]["A"]["built"], r["end"]["A"]["army"], r["end"]["A"]["factories"]),
                         (None, {}, {}))
        self.assertEqual((r["end"]["B"]["army_built"], r["end"]["B"]["factories"]), (0, {}))
        self.assertEqual(r["first"], {s: {"attack": None, "raid": None, "scout": None} for s in ("A", "B")})
        self.assertEqual(r["income_max"], {"A": None, "B": None})

    def test_every_non_win_and_every_stall_is_kept_for_a_diagnosis(self):
        st = [{"from": 1807, "to": 5407, "economy": True, "army": False}]
        self.assertFalse(bar.for_diagnosis({"outcome": "A", "stalls": {"A": []}}))
        for outcome in ("B", "cap"):
            self.assertTrue(bar.for_diagnosis({"outcome": outcome, "stalls": {"A": []}}))
        self.assertTrue(bar.for_diagnosis({"outcome": "A", "stalls": {"A": st}}))
        self.assertTrue(bar.for_diagnosis({"outcome": "A", "stalls": {"A": [], "B": st}}))

    def test_the_recording_is_whole_once_its_writer_closed_it(self):
        self.assertEqual(bar.recording_of(REC_LINES), (REC, True))
        self.assertEqual(bar.recording_of(REC_LINES[:1]), (REC, False))
        self.assertIsNone(bar.recording_of(["recorder: recording timeline 2 of x.imprec from GameTime 5 (parent 0)"]))
        self.assertIsNone(bar.recording_of([]))

    def test_the_holds_end_at_the_cap_whatever_it_is(self):
        for cap in (bar.CAP, 27000, 36000, 4500):
            T, holds = 1, []
            while T < cap:
                T = bar.next_hold(T, cap)
                holds.append(T)
            self.assertEqual(holds[-1], cap)
            self.assertTrue(all(h <= cap for h in holds))
            self.assertTrue(all(b - a <= bar.STEP for a, b in zip(holds, holds[1:])))


def row_result(gid, bound, **kw):
    """A result of row b4's or b5's shape: A's level end, first tasks and best income, as Lane.play writes them"""
    r = {"id": gid, "map": bar.parse_id(gid)["map"], "outcome": "cap", "end_tick": bound, "cap": bound,
         "renderer": "gdi", "stalls": {"A": []},
         "end": {"A": {"built": 40, "army": {"lraider": 4, "lassault": 2}, "army_built": 6,
                       "factories": {"1": 2, "2": 1}, "yards": 0, "sea_plants": 0, "metal_in": 9.0, "attacks": 1,
                       "raids": 2, "scouts": 3, "naval": 0, "tech_m": 8.0}},
         "first": {"A": {"attack": 15000, "raid": 12000, "scout": 6000}}, "income_max": {"A": 9.0}}
    for k, v in kw.items():
        if k in ("end", "first"):
            r[k] = {"A": dict(r[k]["A"], **v)}
        else:
            r[k] = v
    return r


class TestCheck(unittest.TestCase):
    """`check b4|b5` and judge's first tasks over synthetic results."""

    def run_check(self, row, results):
        with tempfile.TemporaryDirectory() as d:
            for r in results:
                (Path(d) / f"{r['id']}.json").write_text(json.dumps(r))
            with mock.patch("builtins.print") as said:
                rc = bar.check(row, [Path(d)])
        return rc, "\n".join(str(c.args[0]) for c in said.call_args_list)

    def b4(self, **kw):
        ids = ["bar-hota-pincushion-ac-1", "bar-hota-pincushion-ca-2", "bar-hota-troutfarm-aa-1",
               "bar-hota-troutfarm-cc-2"]
        return [row_result(g, 27000, **(kw if g == ids[0] else {})) for g in ids]

    def b5(self, **kw):
        ids = ["bar-hota-pincushion-aa-1", "bar-hota-redtriangle-ac-1", "bar-hota-troutfarm-ca-1",
               "bar-hota-lavamania-cc-1", "bar-hota-plainspasses-ac-2"]
        out = [row_result(g, 36000) for g in ids]
        out[1] = row_result(ids[1], 36000, end={"yards": 1, "sea_plants": 1, "naval": 1})
        for g, k in kw.items():
            out[ids.index(g)] = row_result(g, 36000, **k)
        return out

    def test_b4_asks_an_attack_wave_a_scout_and_no_stall(self):
        self.assertEqual(self.run_check("b4", self.b4())[0], 0)
        rc, said = self.run_check("b4", self.b4(first={"attack": None}))
        self.assertEqual(rc, 1)
        self.assertIn("attack wave NOT MET (none)", said)
        self.assertEqual(self.run_check("b4", self.b4(first={"scout": None}))[0], 1)
        st = [{"from": 9007, "to": 12607, "economy": False, "army": True}]
        rc, said = self.run_check("b4", self.b4(stalls={"A": st}))
        self.assertEqual(rc, 1)
        self.assertIn("no stall NOT MET (1 stall windows, the first from 9007)", said)

    def test_a_row_is_played_to_its_bound_by_its_pair_on_its_maps(self):
        rc, said = self.run_check("b4", self.b4(cap=81000))
        self.assertEqual(rc, 2)
        self.assertIn("played to GameTime 81000; b4 plays to 27000", said)
        self.assertEqual(self.run_check("b4", self.b4()[:3])[0], 2)                    # three games
        self.assertEqual(self.run_check("b4", [r for r in self.b4() if "troutfarm" not in r["id"]])[0], 2)
        self.assertEqual(self.run_check("b4", self.b4(outcome="could_not_run", reason="x"))[0], 2)
        rc, said = self.run_check("b4", [dict(self.b4()[0], end={})] + self.b4()[1:])  # a DLL before the lines
        self.assertEqual(rc, 2)
        self.assertIn("no level-end line of A's", said)
        other = [dict(r, id=r["id"].replace("-hota-", "-nota-")) for r in self.b4()]
        self.assertEqual(self.run_check("b4", other)[0], 2)
        self.assertEqual(self.run_check("b5", self.b5())[0], 0)
        self.assertEqual(self.run_check("b5", self.b5()[:4])[0], 2)                    # a map short

    def test_b5_asks_a_composition_a_yard_on_the_sea_and_a_tech_up_the_income_allows(self):
        g = "bar-hota-pincushion-aa-1"
        rc, said = self.run_check("b5", self.b5(**{g: {"end": {"army": {"lraider": 9}}}}))
        self.assertEqual(rc, 1)
        self.assertIn("roles NOT MET (lraider:9)", said)
        sea = "bar-hota-redtriangle-ac-1"
        rc, said = self.run_check("b5", self.b5(**{sea: {"end": {"naval": 0}}}))        # the sea map, no yard
        self.assertEqual(rc, 1)
        self.assertIn("yard NOT MET (0 completed, 0 ordered)", said)
        self.assertEqual(self.run_check("b5", self.b5(**{g: {"end": {"naval": 1}}}))[0], 1)   # judged naval
        rc, said = self.run_check("b5", self.b5(**{g: {"end": {"factories": {"1": 3}}}}))
        self.assertEqual(rc, 1)
        self.assertIn("tech-up NOT MET (best income 9.0 a pass against tech_m 8.0; factories tech 1:3)", said)
        rc, said = self.run_check("b5", self.b5(**{g: {"end": {"factories": {"1": 3}}, "income_max": {"A": 7.5}}}))
        self.assertEqual(rc, 0)
        self.assertIn("tech-up met (not asked: best income 7.5", said)

    def test_judge_reports_the_first_tasks(self):
        with tempfile.TemporaryDirectory() as d:
            for i, gid in enumerate(bar.ids("hota")[:3]):
                r = row_result(gid, bar.CAP, outcome="A", wall_s=1.0,
                               first={"attack": None if i == 2 else 9000 + i * 1000})
                (Path(d) / f"{gid}.json").write_text(json.dumps(r))
            with mock.patch("builtins.print") as said:
                bar.judge([Path(d)])
        text = "\n".join(str(c.args[0]) for c in said.call_args_list)
        self.assertIn(f"A's first attack: in 2 of 3 games, GameTime 9000..10000, median 9000; none in "
                      f"{bar.ids('hota')[2]}", text)
        self.assertIn("A's first scout: in 3 of 3 games, GameTime 6000..6000, median 6000", text)


class TestEnd(unittest.TestCase):
    """Lane.play over a fake instance, every way a game ends: the level ended through the engine's exit, the
    level-end lines read, the recording kept or deleted, the log kept for every non-win."""

    def setUp(self):
        import tareplay
        self.R = tareplay
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        (self.tmp / "out").mkdir()
        (self.tmp / "gamedir" / "replays").mkdir(parents=True)

    def lane(self, alive, end_lines, recording=REC_LINES):
        """A Lane whose holds land where they are asked, whose seats hold alive(T) units, and whose exit writes
        `end_lines` and `recording`'s lines."""
        R, tmp = self.R, self.tmp
        quits = []

        class Log:
            NAP_S = 0

            def __init__(self, w):
                self.lines = list(recording[:1]) + [pulse(1, 1807, 1, 0, 1, 0)]

            def read(self):
                return []

        class Worker:
            gdi, name, gamedir = True, "tareplay-0", tmp / "gamedir"

            def watch_caps(self):
                pass

            def place(self, fix, dll):
                rd = self.gamedir / "replays"
                (rd / REC.split("\\")[-1]).write_bytes(b"recording")
                (rd / (REC.split("\\")[-1] + ".lock")).write_bytes(b"")

            def quit(self):
                quits.append(1)
                lane.log.lines += end_lines + list(recording[1:])

            def cap_failure(self):
                return None

        def hold(T, seats=()):
            self.T = T
            return T

        def peek(off, size):
            if off == bar.CMDR_DEATH:
                return 1
            k, field = divmod(off - bar.REC, bar.STRIDE)
            return alive(self.T, k) if field == bar.ALIVE else 0

        lane = bar.Lane.__new__(bar.Lane)
        lane.R, lane.j, lane.out, lane.dll, lane.w, lane.log = R, 0, tmp / "out", Path("ddraw.dll"), Worker(), None
        lane.hold, lane.peek = hold, peek
        self.T, self.quits, self.Log = 1, quits, Log
        return lane

    def play(self, lane, gid, cap=27000):
        R = self.R
        with mock.patch.object(R, "Log", self.Log), mock.patch.object(R, "tacli", lambda *a, **k: (0, "")), \
                mock.patch.object(R, "held_start", lambda w, log: 1), mock.patch.object(R, "say"), \
                mock.patch.object(bar, "start_positions", lambda name: {2: (100, 100), 3: (900, 900)}):
            return lane.play(dict(bar.parse_id(gid), cap=cap))

    def files(self):
        return sorted(p.name for p in (self.tmp / "out").iterdir() if not p.name.endswith(".scenario.json")), \
            sorted(p.name for p in (self.tmp / "gamedir" / "replays").iterdir())

    def test_a_win_ends_the_level_reads_its_end_and_deletes_its_recording(self):
        gid = "bar-hota-pincushion-ac-1"                                 # A in seat 1
        lane = self.lane(lambda T, s: 0 if s == 2 and T >= 9000 else 5,
                         [eco_end(1), mil_end(1), "impai: handicap rows scaled/exempt gametime=9000:"])
        r = self.play(lane, gid)
        self.assertEqual((r["outcome"], r["end_tick"], self.quits), ("A", 9000, [1]))
        self.assertEqual(r["end"]["A"]["army"], {"lraider": 4, "lassault": 2})
        self.assertIsNone(r["recording"])
        self.assertEqual(self.files(), ([], []))                         # no log kept, no recording left

    def test_a_loss_keeps_its_closed_recording_and_its_log(self):
        gid = "bar-hota-pincushion-ac-1"
        lane = self.lane(lambda T, s: 0 if s == 1 and T >= 12000 else 5,
                         [eco_end(1), mil_end(1), "impai: handicap rows scaled/exempt gametime=12000:"])
        r = self.play(lane, gid)
        self.assertEqual(r["outcome"], "B")
        self.assertEqual(r["recording"], str((self.tmp / "out" / f"{gid}.imprec").resolve()))
        self.assertEqual((self.tmp / "out" / f"{gid}.imprec").read_bytes(), b"recording")
        self.assertEqual(self.files(), ([f"{gid}.imprec", f"{gid}.tagpu.log"], []))

    def test_a_game_run_to_the_cap_is_held_past_it_for_its_pulses_then_kept(self):
        gid = "bar-hota-pincushion-ac-2"                                 # A in seat 2
        lane = self.lane(lambda T, s: 5,
                         [eco_end(2), mil_end(2), "impai: handicap rows scaled/exempt gametime=27015:"])
        r = self.play(lane, gid)
        self.assertEqual((r["outcome"], r["end_tick"], self.T), ("cap", 27000, 27015))
        self.assertEqual(r["end"]["A"]["tech_m"], 8.0)
        self.assertTrue(r["recording"].endswith(f"{gid}.imprec"))

    def test_an_exit_that_writes_no_level_end_could_not_run(self):
        lane = self.lane(lambda T, s: 5, [])
        with self.assertRaisesRegex(self.R.CouldNotRun, "no level end"):
            self.play(lane, "bar-hota-pincushion-ac-1")

    def test_a_recording_to_keep_that_was_not_closed_could_not_run(self):
        lane = self.lane(lambda T, s: 5, [eco_end(1), "impai: handicap rows scaled/exempt gametime=27015:"],
                         recording=REC_LINES[:1])
        with self.assertRaisesRegex(self.R.CouldNotRun, "was not closed"):
            self.play(lane, "bar-hota-pincushion-ac-1")


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
