#!/usr/bin/env python3
"""test_bar -- the bar harness offline: its game list and each game's seed, the seating a game id gives, the start
schema it reads from an .ota, the stall rule, the holds' end at the cap, what a result reads from the level's end,
the judge (a ladder game at the cap scored by value at the limit) and the b4 and b5 checks over synthetic results
(b5's yard from the second-factory line, each verdict); `--setup` (a setup's commanders from its sidedata, its
games marked with it and kept to a directory of their own, a setup that plays no ImpAI game refused); the cheat
flag (a setup flagged by its games' evidence alone, and judged apart); a game's end on a fake instance (THE END's
scenario, the level run to its teardown's lines, the game stopped, the play's lines cut at the apply, the cheat
flag's evidence from the recording, the recording kept or deleted, the log kept for every non-win, a level not
seeded from its game's seed); `row` on fake games; and `play` on fake lanes: a game that could not run, and a run
stopped by SIGINT. No game runs.

    python3 tools/impai/test_bar.py"""
import json
import os
import re
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


def pulse(seat, t, built, mroots, eroots, army, metal=0, vlive=0):
    return (f"impai: pulse seat={seat} t={t} built={built} mroots={mroots} eroots={eroots} army={army} "
            f"squads=F metal={metal} energy=0 vlive={vlive} vbuilt=0")


def second(seat, t=9000, naval=0, chose="land", sea_site="-"):
    return f"impai: second seat={seat} gametime={t} naval={naval} chose={chose} sea_site={sea_site}"


def seeds_line(seed):
    return (f"enginefix: run seeds before the creates: the wire's seed {seed} at GameTime 1 seeds the simulation RNG, the game "
            f"thread's CRT stream, the wind and the meteors; the wind draws on the next step and the next shower "
            f"is at 9001")


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

    def test_every_game_has_its_own_seed_and_keeps_it(self):
        every = [gid for pair in bar.PAIRS for gid in bar.ids(pair)]
        seeds = [bar.game_seed(gid) for gid in every]
        self.assertEqual(len(set(seeds)), len(every))                   # no two games of the bar share one
        self.assertTrue(all(0 <= x <= 0xFFFFFFFF for x in seeds))       # the scenario's 32-bit seed
        self.assertEqual([bar.game_seed(gid) for gid in every], seeds)  # a function of the id alone
        self.assertEqual(bar.game_seed("bar-hota-pincushion-ac-1"), 0x2B86D352)   # pinned: a change replays no game
        g1, g3 = bar.parse_id("bar-hota-pincushion-ac-1"), bar.parse_id("bar-hota-pincushion-ac-3")
        self.assertEqual((g1["start"], g1["sides"]), (g3["start"], g3["sides"]))    # one start, one side pairing,
        self.assertNotEqual(g1["seed"], g3["seed"])                                # two games
        doc = bar.game_doc(g1, {2: (1, 2), 3: (3, 4)})
        self.assertEqual(doc["seed"], g1["seed"])
        self.assertNotIn("seed", bar.game_doc(dict(g1, seed=None), {2: (1, 2), 3: (3, 4)}))
        self.assertTrue(bar.seeded(["x", seeds_line(g1["seed"])], g1["seed"]))
        self.assertFalse(bar.seeded([seeds_line(g3["seed"])], g1["seed"]))
        self.assertFalse(bar.seeded(["enginefix: test seeds: the level's simulation RNG seeded with 0x5EED0001"],
                                    g1["seed"]))

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
                     "renderer": "gdi", "seed": bar.game_seed(gid)}
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
            put(bar.ids("hota")[1])
            put(bar.ids("hota")[2], seed=None)                      # played before each game had its own seed
            with mock.patch("builtins.print") as said:
                self.assertEqual(bar.judge([d]), 2)
            self.assertIn("name no seed", said.call_args.args[0])

    def test_a_ladder_game_at_the_cap_is_scored_by_value_at_the_limit(self):
        def cap(gid, a, b, ta=81014, tb=81007):
            return {"id": gid, "outcome": "cap", "end_tick": 81000, "cap": 81000, "renderer": "gdi",
                    "seed": bar.game_seed(gid), "value": {"A": {"built": 9e4, "live": a, "t": ta},
                                                          "B": {"built": 9e4, "live": b, "t": tb}}}
        gid = bar.ids("hn")[1]
        self.assertEqual(bar.cap_by_value(cap(gid, 13000, 10000)), ("A", "value A 13000, B 10000"))
        self.assertEqual(bar.cap_by_value(cap(gid, 12999, 10000))[0], "draw")      # under the 1.3x lead
        self.assertEqual(bar.cap_by_value(cap(gid, 10000, 13000))[0], "B")
        self.assertEqual(bar.cap_by_value(cap(gid, 0, 0))[0], "draw")              # no seat leads
        self.assertEqual(bar.cap_by_value(cap(gid, 500, 0))[0], "A")
        who, why = bar.cap_by_value(cap(gid, 13000, 10000, ta=79214))               # A's last pulse a minute early
        self.assertIsNone(who)
        self.assertIn("no value of A's at the limit", why)
        self.assertIsNone(bar.cap_by_value(dict(cap(gid, 1, 1), value={"A": {"built": 1, "live": 1}}))[0])
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)

            def put(r):
                (d / f"{r['id']}.json").write_text(json.dumps(r))
            hn = bar.ids("hn")
            for g in hn[:86]:
                put({"id": g, "outcome": "A", "end_tick": 30000, "renderer": "gdi", "seed": bar.game_seed(g)})
            for g in hn[86:90]:
                put(cap(g, 20000, 10000))                                   # four won by value: 90 of 100
            for g in hn[90:]:
                put(cap(g, 10000, 10000))                                   # drawn by value
            with mock.patch("builtins.print") as said:
                self.assertEqual(bar.judge([d]), 0)
            text = "\n".join(str(c.args[0]) for c in said.call_args_list)
            self.assertIn("at the cap by value (a 1.3x lead at the limit): A 4, draw 10; A's wins with them: 90 of "
                          "100", text)
            put(cap(hn[86], 10000, 20000))                                  # one of them B's: 89
            self.assertEqual(bar.judge([d]), 1)
            put(dict(cap(hn[86], 20000, 10000), draw=True, end_tick=60000))  # both gone in one tick: a draw
            self.assertEqual(bar.judge([d]), 1)
            put(cap(hn[86], 20000, 10000, tb=79207))                        # no value at the limit: no verdict
            with mock.patch("builtins.print") as said:
                self.assertEqual(bar.judge([d]), 2)
            text = "\n".join(str(c.args[0]) for c in said.call_args_list)
            self.assertIn(f"cap not scored: {hn[86]} (no value of B's at the limit", text)

    def test_a_kept_game_is_one_played_as_asked(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            g = dict(bar.parse_id("bar-hota-pincushion-ac-1"))
            self.assertIsNone(bar.game_kept(d, g, False))
            r = {"id": g["id"], "outcome": "A", "cap": bar.CAP, "renderer": "gdi", "pacing": "nodraw",
                 "seed": g["seed"]}
            (d / f"{g['id']}.json").write_text(json.dumps(r))
            self.assertEqual(bar.game_kept(d, g, False)["outcome"], "A")
            (d / f"{g['id']}.json").write_text(json.dumps(dict(r, seed=None)))   # played before its seed
            self.assertIsNone(bar.game_kept(d, g, False))
            (d / f"{g['id']}.json").write_text(json.dumps(dict(r, seed=g["seed"] + 1)))
            self.assertIsNone(bar.game_kept(d, g, False))
            (d / f"{g['id']}.json").write_text(json.dumps(r))
            self.assertIsNone(bar.game_kept(d, g, True))           # played with GDI, asked on the GPU
            self.assertIsNone(bar.game_kept(d, g, False, True))    # played under no draw, asked drawn
            drawn = {k: v for k, v in r.items() if k != "pacing"}  # a result from before the field: drawn
            (d / f"{g['id']}.json").write_text(json.dumps(drawn))
            self.assertIsNone(bar.game_kept(d, g, False))
            self.assertEqual(bar.game_kept(d, g, False, True)["outcome"], "A")
            (d / f"{g['id']}.json").write_text(json.dumps(r))
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
                                                          "renderer": "gdi", "pacing": "nodraw",
                                                          "seed": bar.game_seed(ids[0])}))
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
                 eco_end(2, yards=1, sea_plants=2), mil_end(2, naval=1), second(2, 8000, 1, "sea", "found"),
                 second(1, 8100, 0, "land", "-"),                       # seat 1 is no ImpAI seat here
                 pulse(2, 1807, 1, 0, 1, 0, metal=100), pulse(2, 3607, 2, 0, 2, 0, metal=700, vlive=4321)]
        r = bar.read_result(lines, seat, g, "B", 20000)
        self.assertEqual(r["end"]["A"], {"built": 31, "army": {"lraider": 4, "lassault": 2}, "army_built": 6,
                                         "factories": {"1": 2, "2": 1}, "yards": 1, "sea_plants": 2,
                                         "metal_in": 9.5, "attacks": 2, "raids": 3, "scouts": 5, "naval": 1,
                                         "tech_m": 8.0, "dguns": 0, "defends": 1, "retreats": 4})
        self.assertNotIn("B", r["end"])
        self.assertEqual(r["first"], {"A": {"attack": 15000, "raid": 12000, "scout": 9000}})
        self.assertEqual(r["income_max"], {"A": 10.0})                   # 600 metal over 60 passes
        self.assertEqual((r["outcome"], r["end_tick"]), ("B", 20000))
        self.assertEqual(r["value"], {"A": {"built": 0.0, "live": 4321.0, "t": 3607}})
        self.assertEqual(r["second"], {"A": {"gametime": 8000, "naval": 1, "chose": "sea", "sea_site": "found"}})

    def test_a_seats_first_second_factory_line_is_its_decision(self):
        g = bar.parse_id("bar-ne-pincushion-aa-1")                       # A in seat 1, B in seat 2
        lines = [second(1, 9000, 1, "land", "none"), second(1, 81015, 1, "none", "-"),
                 second(2, 81015, -1, "none", "-")]
        self.assertEqual(bar.second_factory(lines, bar.seat_of(g), ["A", "B"]),
                         {"A": {"gametime": 9000, "naval": 1, "chose": "land", "sea_site": "none"},
                          "B": {"gametime": 81015, "naval": -1, "chose": "none", "sea_site": "-"}})

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
        self.assertEqual(r["second"], {})

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
         "first": {"A": {"attack": 15000, "raid": 12000, "scout": 6000}}, "income_max": {"A": 9.0},
         "second": {"A": {"gametime": 9000, "naval": 0, "chose": "land", "sea_site": "-"}}}
    for k, v in kw.items():
        if k in ("end", "first", "second"):
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
        out[1] = row_result(ids[1], 36000, end={"yards": 1, "sea_plants": 1, "naval": 1},
                            second={"naval": 1, "chose": "sea", "sea_site": "found"})
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
        rc, said = self.run_check("b5", self.b5(**{g: {"end": {"factories": {"1": 3}}}}))
        self.assertEqual(rc, 1)
        self.assertIn("tech-up NOT MET (best income 9.0 a pass against tech_m 8.0; factories tech 1:3)", said)
        rc, said = self.run_check("b5", self.b5(**{g: {"end": {"factories": {"1": 3}}, "income_max": {"A": 7.5}}}))
        self.assertEqual(rc, 0)
        self.assertIn("tech-up met (not asked: best income 7.5", said)

    def test_b5_judges_the_yard_at_the_second_factory_decision(self):
        g, sea = "bar-hota-pincushion-aa-1", "bar-hota-redtriangle-ac-1"
        rc, said = self.run_check("b5", self.b5())
        self.assertEqual(rc, 0)
        self.assertIn(f"{sea}: cap at 36000: roles met (lassault:2 lraider:4); yard met (yard ordered: the second "
                      f"factory at GameTime 9000, naval=1, sea search found; 1 yards completed)", said)
        self.assertIn("yard met (not asked: the second factory at GameTime 9000, naval=0", said)
        # yard ordered on a map judged naval at the decision, whatever the level end says
        rc, said = self.run_check("b5", self.b5(**{g: {"second": {"naval": 1, "chose": "sea", "sea_site": "found"},
                                                       "end": {"naval": 0}}}))
        self.assertEqual(rc, 0)
        self.assertIn(f"{g}: cap at 36000: roles met (lassault:2 lraider:4); yard met (yard ordered", said)
        # naval, no site: met, and named
        rc, said = self.run_check("b5", self.b5(**{g: {"second": {"naval": 1, "chose": "land",
                                                                  "sea_site": "none"}}}))
        self.assertEqual(rc, 0)
        self.assertIn("yard met (naval, no site: the second factory at GameTime 9000, naval=1, sea search none",
                      said)
        self.assertIn(f"naval, no site (the yard met without one): {g}", said)
        # yard missing: naval at the decision and a land plant with candidates refused, or none searched
        for site in ("refused", "-"):
            rc, said = self.run_check("b5", self.b5(**{g: {"second": {"naval": 1, "chose": "land",
                                                                      "sea_site": site}}}))
            self.assertEqual(rc, 1)
            self.assertIn(f"yard NOT MET (yard missing: the second factory at GameTime 9000, naval=1, sea search "
                          f"{site}", said)
        # the sea map asks a yard whatever ImpAI judged; no second factory by the end is missing there too
        rc, said = self.run_check("b5", self.b5(**{sea: {"second": {"naval": 0, "chose": "land"}}}))
        self.assertEqual(rc, 1)
        self.assertIn("yard NOT MET (yard missing: the second factory at GameTime 9000, naval=0", said)
        rc, said = self.run_check("b5", self.b5(**{sea: {"second": {"gametime": 36015, "naval": 1,
                                                                    "chose": "none"}}}))
        self.assertEqual(rc, 1)
        self.assertIn("yard NOT MET (yard missing: the level end at GameTime 36015, naval=1", said)
        # a level judged naval only at its end asks nothing of the yard: the decision judged land
        rc, said = self.run_check("b5", self.b5(**{g: {"end": {"naval": 1}}}))
        self.assertEqual(rc, 0)
        # no line: a DLL before it
        rc, said = self.run_check("b5", [dict(r, second={}) if r["id"] == g else r for r in self.b5()])
        self.assertEqual(rc, 2)
        self.assertIn(f"{g}: no `impai: second` line of A's", said)

    def test_row_plays_its_games_to_its_bound_then_checks_them(self):
        played = []

        def play(games, a, out):
            played.extend((x["id"], x["cap"], x["seed"]) for x in games)
            for x in self.b4():
                (out / f"{x['id']}.json").write_text(json.dumps(x))
            return 0
        with tempfile.TemporaryDirectory() as d, mock.patch.object(bar, "play_games", play), \
                mock.patch("builtins.print"):
            out = Path(d) / "b4"
            out.mkdir()
            self.assertEqual(bar.main(["row", "b4", "--lanes", "4", "--out", str(out)]), 0)
        self.assertEqual(played, [(g, 27000, bar.game_seed(g)) for g in bar.ROWS["b4"]["ids"]])
        for row, spec in bar.ROWS.items():
            self.assertEqual(len(spec["ids"]), spec["games"])
            self.assertEqual(len({bar.parse_id(g)["map"] for g in spec["ids"]}), spec["maps"])
            self.assertTrue(all(bar.parse_id(g)["pair"] == spec["pair"] for g in spec["ids"]))
        with mock.patch.object(bar, "play_games", lambda games, a, out: 2), mock.patch.object(bar, "check") as chk:
            self.assertEqual(bar.main(["row", "b5", "--lanes", "5", "--out", "/nonexistent"]), 2)
        chk.assert_not_called()                                         # a play that could not run checks nothing

    def test_judge_reports_the_first_tasks(self):
        with tempfile.TemporaryDirectory() as d:
            for i, gid in enumerate(bar.ids("hota")[:3]):
                r = row_result(gid, bar.CAP, outcome="A", wall_s=1.0, seed=bar.game_seed(gid),
                               first={"attack": None if i == 2 else 9000 + i * 1000})
                (Path(d) / f"{gid}.json").write_text(json.dumps(r))
            with mock.patch("builtins.print") as said:
                bar.judge([Path(d)])
        text = "\n".join(str(c.args[0]) for c in said.call_args_list)
        self.assertIn(f"A's first attack: in 2 of 3 games, GameTime 9000..10000, median 9000; none in "
                      f"{bar.ids('hota')[2]}", text)
        self.assertIn("A's first scout: in 3 of 3 games, GameTime 6000..6000, median 6000", text)


SIDEDATA = b"""[SIDE0]
{
  name=GOK;
  nameprefix=GOK;
  commander=GoKCommander;
}
[SIDE1]
{
  name=ARM;
  commander=ArmCommander;
}
[SIDE2]
{
  name=CORE;
  commander=CoreCommander;
}
"""


class TestSetup(unittest.TestCase):
    """`--setup`: a setup's commanders from its sidedata, its games marked with it and played into a directory of
    their own, and a directory of another setup's games refused."""

    def test_a_setups_commanders_are_its_sidedatas_by_index(self):
        self.assertEqual(bar.side_commanders(SIDEDATA), {"arm": "GoKCommander", "core": "ArmCommander"})
        with self.assertRaisesRegex(bar.BarError, r"\[SIDE1\] names no commander"):
            bar.side_commanders(SIDEDATA.replace(b"commander=ArmCommander;", b""))

    def test_the_end_keeps_a_unit_of_the_human_seat_alone(self):
        g = bar.parse_id("bar-hota-pincushion-ac-1")
        doc = bar.game_doc(g, {2: (1, 2), 3: (3, 4)}, {"arm": "GoKCommander", "core": "ArmCommander"})
        self.assertEqual({u["owner"]: u["type"] for u in doc["units"]}, {1: "GoKCommander", 2: "ArmCommander"})
        end = bar.end_doc(doc, {"arm": "GoKCommander", "core": "ArmCommander"})
        self.assertEqual(end["setup"], {"clear_existing": True, "objective": "on"})
        self.assertEqual(end["units"], [{"id": "end_keep", "type": "GoKCommander", "owner": 0, "pos": [1, 2],
                                         "facing": 0}])

    def play(self, argv, out):
        """bar.main(argv + --out out) with the lanes and the run lock faked: (exit code, the games play_all was
        given, the setup record the lanes were to play)."""
        import runlock
        got = {}

        def play_all(games, lanes, out_dir, gpu):
            got["games"], got["setup"] = games, bar._SETUP["setup"]
            return [{"id": g["id"], "outcome": "A"} for g in games]
        self.addCleanup(bar._SETUP.update, {"setup": None})
        with mock.patch.object(bar, "play_all", play_all), mock.patch.object(runlock, "hold"), \
                mock.patch.object(runlock, "release"), mock.patch.object(bar, "close_lanes"), \
                mock.patch("builtins.print"):
            rc = bar.main(argv + ["--out", str(out)])
        return rc, got.get("games"), got.get("setup")

    def test_play_marks_each_game_with_its_setup_and_gives_the_lanes_its_record(self):
        ids = bar.ids("nota")[:2]
        with tempfile.TemporaryDirectory() as d:
            rc, games, setup = self.play(["play", "--ids", ",".join(ids), "--lanes", "1", "--setup", "escalation"],
                                         Path(d))
            self.assertEqual(rc, 0)
            self.assertEqual([(g["id"], g["setup"], g["seed"]) for g in games],
                             [(i, "escalation", bar.game_seed(i)) for i in ids])   # the same ids, the same seeds
            self.assertEqual(setup["name"], "escalation")
            rc, games, setup = self.play(["play", "--ids", ids[0], "--lanes", "1"], Path(d))
            self.assertEqual((rc, games[0]["setup"], setup), (0, None, None))

    def test_a_setup_that_plays_no_impai_game_or_none_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            for name in ("no-such-setup", "retail+tadr1-recorder"):     # unknown; its goal is not Impure running
                rc, games, _ = self.play(["play", "--ids", "nota", "--lanes", "1", "--setup", name], Path(d))
                self.assertEqual((rc, games), (1, None))

    def test_a_directory_holds_one_setups_games(self):
        gid = bar.ids("nota")[0]
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / f"{gid}.json").write_text(json.dumps({"id": gid, "outcome": "A", "setup": "escalation"}))
            rc, games, _ = self.play(["play", "--ids", gid, "--lanes", "1", "--setup", "prota-4.8"], Path(d))
            self.assertEqual((rc, games), (1, None))
            rc, games, _ = self.play(["play", "--ids", gid, "--lanes", "1"], Path(d))
            self.assertEqual((rc, games), (1, None))
            rc, games, _ = self.play(["play", "--ids", gid, "--lanes", "1", "--setup", "escalation"], Path(d))
            self.assertEqual(rc, 0)

    def test_a_setups_games_go_to_their_own_directory_and_resume_only_there(self):
        import argparse
        a = argparse.Namespace(out=None, setup="tazero-alpha5")
        self.assertTrue(bar.default_out(a).name.startswith("tazero-alpha5-"))
        self.assertTrue(bar.default_out(a, "b4").name.startswith("tazero-alpha5-b4-"))
        self.assertFalse(bar.default_out(argparse.Namespace(out=None, setup=None)).name.startswith("None"))
        with tempfile.TemporaryDirectory() as d:
            g = dict(bar.parse_id("bar-nota-pincushion-ac-1"), setup="escalation")
            r = {"id": g["id"], "outcome": "A", "cap": bar.CAP, "renderer": "gdi", "pacing": "nodraw",
                 "seed": g["seed"], "setup": "escalation"}
            (Path(d) / f"{g['id']}.json").write_text(json.dumps(r))
            self.assertIsNotNone(bar.game_kept(Path(d), g, False))
            self.assertIsNone(bar.game_kept(Path(d), dict(g, setup=None), False))     # the base game's is another


def stock_result(gid, inc_a, inc_b, setup=None, outcome="A", ai_only=None):
    """A result of game `gid` carrying THE CHEAT FLAG's evidence: each side's income at t=1 (metal, energy)."""
    g = bar.parse_id(gid)
    return {"id": gid, "outcome": outcome, "end_tick": 30000, "renderer": "gdi", "seed": g["seed"], "setup": setup,
            "sides": g["sides"], "difficulty": g["difficulty"], "stalls": {"A": []},
            "seats": {s: dict(g["seats"][s], seat=bar.seat_of(g)[s]) for s in ("A", "B")},
            "income_t1": {"at": 1, "A": {"metal": inc_a[0], "energy": inc_a[1]},
                          "B": {"metal": inc_b[0], "energy": inc_b[1]}},
            "ai_only": ai_only or {"A": {}, "B": {}}}


class TestCheat(unittest.TestCase):
    """THE CHEAT FLAG: a setup flagged by its games' evidence alone, and judged apart."""

    def test_no_more_income_than_a_player_of_the_side_is_no_cheat(self):
        # the loader's: OTA AI at Medium makes 0.7 of its commander's income (the stock handicap), both sides alike
        self.assertEqual(bar.cheat_evidence([stock_result("bar-nota-pincushion-ac-1", (1.0, 25.0), (0.7, 17.5)),
                                             stock_result("bar-nota-pincushion-ca-1", (1.0, 25.0), (0.7, 17.5))]), [])
        # ProTA's and Total Mayhem's: the handicap dropped at Medium, the same income as ImpAI's
        self.assertEqual(bar.cheat_evidence([stock_result("bar-nota-pincushion-ac-1", (1.0, 25.0), (1.0, 25.0))]),
                         [])
        # TA Zero's sides make different incomes (GoKCommander 6 energy, ArmCommander 8): at Hard each side's OTA AI
        # makes its side's, which compared across the sides would read as more than ImpAI's
        rs = [stock_result("bar-hota-pincushion-ac-1", (1.0, 6.0), (1.0, 8.0)),
              stock_result("bar-hota-pincushion-ca-1", (1.0, 8.0), (1.0, 6.0))]
        self.assertEqual(bar.cheat_evidence(rs), [])
        # a float32 rounding above ImpAI's is no cheat
        self.assertEqual(bar.cheat_evidence([stock_result("bar-hota-pincushion-aa-1", (1.0, 25.0),
                                                          (1.00001, 25.0))]), [])
        # a seat with no ImpAI on its side is not compared
        self.assertEqual(bar.cheat_evidence([stock_result("bar-nota-pincushion-ac-1", (1.0, 25.0), (9.0, 99.0))]),
                         [])

    def test_an_income_above_a_players_or_an_ai_only_type_is_a_cheat(self):
        rs = [stock_result("bar-nota-pincushion-ac-2", (2.0, 50.0), (4.0, 100.0)),   # Escalation's, A arm
              stock_result("bar-nota-lavamania-ca-1", (2.0, 50.0), (4.0, 100.0))]    # A core
        got = bar.cheat_evidence(rs)
        self.assertEqual(len(got), 2)
        self.assertIn("OTA AI's core income at t=1 4 metal, 100 energy (bar-nota-pincushion-ac-2, medium) above "
                      "ImpAI's 2, 50 (bar-nota-lavamania-ca-1) in metal and energy", got)
        built = {"A": {"ARMT1MEX_AI": 3}, "B": {"ArmT1Mex_AI": 2, "ArmT1AF_AI": 1}}
        got = bar.cheat_evidence([stock_result("bar-nota-pincushion-ac-1", (1.0, 6.0), (0.7, 5.6), ai_only=built),
                                  stock_result("bar-nota-pincushion-ac-3", (1.0, 6.0), (0.7, 5.6),
                                               ai_only={"A": {}, "B": {"ArmT1Mex_AI": 1}})])
        self.assertEqual(got, ["OTA AI completed AI-only types: ArmT1AF_AI x1 (bar-nota-pincushion-ac-1), "
                               "ArmT1Mex_AI x3 (bar-nota-pincushion-ac-1)"])           # ImpAI's are not counted

    def test_judge_counts_only_the_stock_setups(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            for gid in bar.ids("hota"):
                (d / f"{gid}.json").write_text(json.dumps(stock_result(gid, (1.0, 25.0), (1.0, 25.0))))
            esc = d / "escalation"
            esc.mkdir()
            for gid in bar.ids("hota")[:4]:
                (esc / f"{gid}.json").write_text(json.dumps(
                    stock_result(gid, (2.0, 50.0), (4.0, 100.0), setup="escalation", outcome="B")))
            with mock.patch("builtins.print") as said:
                self.assertEqual(bar.judge([d, esc]), 0)            # escalation's losses and lack count for nothing
            text = "\n".join(str(c.args[0]) for c in said.call_args_list)
            self.assertIn("the base game: stock: no evidence of a cheat in the 100 of its games that carry it", text)
            self.assertIn("setup escalation: CHEATS, its games not counted: OTA AI's", text)
            self.assertIn("hota: 0 of 4 won by A, 4 lost, 0 at the cap", text)
            with mock.patch("builtins.print") as said:
                self.assertEqual(bar.judge([esc]), 2)
            self.assertIn("no stock setup's games", said.call_args.args[0])
            loss = bar.ids("hota")[7]
            (d / f"{loss}.json").write_text(json.dumps(stock_result(loss, (1.0, 25.0), (1.0, 25.0), outcome="B")))
            with mock.patch("builtins.print"):
                self.assertEqual(bar.judge([d, esc]), 1)             # a stock game lost fails the bar


class TestEnd(unittest.TestCase):
    """Lane.play over a fake instance, every way a game ends: the level ended by THE END (the end scenario applied,
    the level run to its teardown's lines, the game stopped), the level-end lines read, the play's lines cut at the
    apply, the cheat flag's evidence taken from the recording, the recording kept or deleted, the log kept for every
    non-win."""

    def setUp(self):
        import tareplay
        self.R = tareplay
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        (self.tmp / "out").mkdir()
        (self.tmp / "gamedir" / "replays").mkdir(parents=True)

    def lane(self, alive, end_lines, recording=REC_LINES, nodraw=False, seeded=True, commanders=bar.COMMANDER):
        """A Lane whose holds land where they are asked, whose seats hold alive(T) units, and whose level, once
        resumed after the end scenario, writes `end_lines` and `recording`'s lines; with `nodraw` its worker runs
        no draw and its log holds the level's account; with `seeded` its launch's apply seeded the level from the
        game's seed. self.events records the end's steps in order."""
        R, tmp = self.R, self.tmp
        events = []
        account = {"from": 2, "to": 9000, "skipped": 8900, "board": 0, "drawn": 98} if nodraw else None

        class Log:
            NAP_S = 0
            nodraw = account
            seed = None                 # the game's, set by play()

            def __init__(self, w):
                self.lines = (list(recording[:1]) + ([seeds_line(Log.seed)] if seeded else [])
                              + [pulse(1, 1807, 1, 0, 1, 0, vlive=500)])
                self.ended = False

            def read(self):
                return []

            def wait_end(self, rx, start=0, poll=None, after="the end scenario"):
                events.append(f"wait {rx}")
                if "resume" in events and not self.ended:
                    self.ended = True
                    self.lines += (end_lines + (["recorder: ended after GameTime 9170: the level ended"] if end_lines
                                                else []) + list(recording[1:]))
                if not any(re.search(rx, ln) for ln in self.lines[start:]):
                    raise R.Failed(f"the level did not end after {after}: no /{rx}/")
                return ""

        class Worker:
            gdi, name, gamedir = True, "tareplay-0", tmp / "gamedir"
            nodraw = False

            def watch_caps(self):
                pass

            def place(self, fix, dll):
                rd = self.gamedir / "replays"
                (rd / REC.split("\\")[-1]).write_bytes(b"recording")
                (rd / (REC.split("\\")[-1] + ".lock")).write_bytes(b"")

            def stop(self, hard=False):
                events.append(f"stop hard={hard}")

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
        lane.setup, lane.types = None, {7: "ARMMEX", 9: "CORT1MEX_AI"}
        lane.install = {"vfs": None, "commanders": commanders, "ai_only": {"cort1mex_ai"}}
        lane.w.nodraw = nodraw
        lane.hold, lane.peek = hold, peek
        self.T, self.events, self.Log = 1, events, Log
        return lane

    def play(self, lane, gid, cap=27000, rec=None):
        """Lane.play of game `gid` on the fake lane, its recording reading as `rec` (recording_evidence's shape;
        by default each record's commander income and no death)."""
        R, events = self.R, self.events
        rec = rec or {"at": 1, "income": {k: (1.0, 25.0) for k in range(10)}, "deaths": []}
        self.applied = []

        def tacli(*a, **k):
            if a[:2] == ("scenario", "apply"):
                events.append("apply")
                self.applied.append(json.loads(Path(a[3]).read_text()))
            return 0, ""
        with mock.patch.object(R, "Log", self.Log), mock.patch.object(R, "tacli", tacli), \
                mock.patch.object(R, "resume", lambda w, log: events.append("resume")), \
                mock.patch.object(R, "held_start", lambda w, log: 1), mock.patch.object(R, "say"), \
                mock.patch.object(bar, "recording_evidence", lambda path, R: rec), \
                mock.patch.object(bar, "start_positions", lambda name, vfs=None: {2: (100, 100), 3: (900, 900)}):
            self.Log.seed = bar.game_seed(gid)
            return lane.play(dict(bar.parse_id(gid), cap=cap))

    def files(self):
        return sorted(p.name for p in (self.tmp / "out").iterdir() if not p.name.endswith(".scenario.json")), \
            sorted(p.name for p in (self.tmp / "gamedir" / "replays").iterdir())

    def test_a_win_ends_the_level_reads_its_end_and_deletes_its_recording(self):
        gid = "bar-hota-pincushion-ac-1"                                 # A in seat 1
        lane = self.lane(lambda T, s: 0 if s == 2 and T >= 9000 else 5,
                         [eco_end(1), mil_end(1), "impai: handicap rows scaled/exempt gametime=9000:"])
        r = self.play(lane, gid)
        self.assertEqual((r["outcome"], r["end_tick"], r["level_end_at"]), ("A", 9000, 9000))
        self.assertEqual(r["end"]["A"]["army"], {"lraider": 4, "lassault": 2})
        self.assertIsNone(r["recording"])
        self.assertEqual(self.files(), ([], []))                         # no log kept, no recording left

    def test_a_result_names_its_pacing_and_a_no_draw_game_its_account(self):
        gid = "bar-hota-pincushion-ac-1"
        ends = [eco_end(1), mil_end(1), "impai: handicap rows scaled/exempt gametime=9000:"]
        r = self.play(self.lane(lambda T, s: 0 if s == 2 and T >= 9000 else 5, ends), gid)
        self.assertEqual(r["pacing"], "draw")
        self.assertNotIn("nodraw", r)
        r = self.play(self.lane(lambda T, s: 0 if s == 2 and T >= 9000 else 5, ends, nodraw=True), gid)
        self.assertEqual((r["pacing"], r["nodraw"]["skipped"], r["nodraw"]["drawn"]), ("nodraw", 8900, 98))

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

    def test_a_level_not_seeded_from_its_games_seed_could_not_run(self):
        gid = "bar-hota-pincushion-ac-1"
        ends = [eco_end(1), mil_end(1), "impai: handicap rows scaled/exempt gametime=9000:"]
        r = self.play(self.lane(lambda T, s: 0 if s == 2 and T >= 9000 else 5, ends), gid)
        self.assertEqual(r["seed"], bar.game_seed(gid))
        with self.assertRaisesRegex(self.R.CouldNotRun, f"not seeded from the game's seed {bar.game_seed(gid)}"):
            self.play(self.lane(lambda T, s: 0 if s == 2 and T >= 9000 else 5, ends, seeded=False), gid)

    def test_a_level_whose_end_writes_no_level_end_could_not_run(self):
        lane = self.lane(lambda T, s: 5, [])
        with self.assertRaisesRegex(self.R.Failed, "did not end .*handicap"):
            self.play(lane, "bar-hota-pincushion-ac-1")
        self.assertNotIn("stop hard=True", self.events)                 # play_game's abandon stops it

    def test_a_level_whose_recording_is_not_closed_could_not_run(self):
        lane = self.lane(lambda T, s: 5, [eco_end(1), "impai: handicap rows scaled/exempt gametime=27015:"],
                         recording=REC_LINES[:1])
        with self.assertRaisesRegex(self.R.Failed, "recorder: writer"):
            self.play(lane, "bar-hota-pincushion-ac-1")

    def test_the_end_clears_keeps_the_humans_commander_arms_the_test_then_stops_the_game(self):
        gid = "bar-hota-pincushion-ca-1"                 # A core in seat 1, B arm in seat 2
        tazero = {"arm": "GoKCommander", "core": "ArmCommander"}
        ends = [pulse(1, 9037, 9, 0, 9, 0, vlive=0),     # logged after the clear: not the play's
                eco_end(1), mil_end(1), "impai: handicap rows scaled/exempt gametime=9170:"]
        r = self.play(self.lane(lambda T, s: 0 if s == 2 and T >= 9000 else 5, ends, commanders=tazero), gid)
        self.assertEqual(self.events, ["apply", "resume", f"wait {re.escape(bar.LEVEL_END)}",
                                       "wait recorder: ended after", f"wait {bar.REC_CLOSED.pattern}",
                                       "stop hard=True"])
        end = self.applied[0]
        self.assertEqual(end["setup"], {"clear_existing": True, "objective": "on"})
        self.assertEqual([(u["type"], u["owner"]) for u in end["units"]], [("GoKCommander", 0)])   # the human's side
        doc = json.loads((self.tmp / "out" / f"{gid}.scenario.json").read_text())
        self.assertEqual({u["owner"]: u["type"] for u in doc["units"]}, {1: "ArmCommander", 2: "GoKCommander"})
        self.assertEqual((r["outcome"], r["end_tick"], r["level_end_at"]), ("A", 9000, 9170))
        self.assertEqual(r["value"]["A"], {"built": 0.0, "live": 500.0, "t": 1807})     # the play's last pulse
        self.assertTrue((self.tmp / "out" / f"{gid}.end.scenario.json").is_file())

    def test_a_result_carries_the_cheat_flags_evidence_from_its_recording(self):
        gid = "bar-nota-pincushion-ca-1"                 # A core in seat 1, B arm in seat 2
        ends = [eco_end(1), mil_end(1), "impai: handicap rows scaled/exempt gametime=9170:"]
        rec = {"at": 1, "income": {k: {1: (1.0, 8.0), 2: (0.7, 4.2)}.get(k, (0.0, 0.0)) for k in range(10)},
               "deaths": [(2, 9, 0.0), (2, 9, 0.0), (2, 9, 0.5), (1, 9, 0.0), (2, 7, 0.0), (3, 9, 0.0)]}
        r = self.play(self.lane(lambda T, s: 0 if s == 2 and T >= 9000 else 5, ends), gid, rec=rec)
        self.assertEqual(r["income_t1"], {"at": 1, "A": {"metal": 1.0, "energy": 8.0},
                                          "B": {"metal": 0.7, "energy": 4.2}})
        # completed (no fraction left) units of an AI-only type, by side; a frame and another type are not
        self.assertEqual(r["ai_only"], {"A": {"CORT1MEX_AI": 1}, "B": {"CORT1MEX_AI": 2}})

    def test_a_lane_plays_only_its_own_setups_games(self):
        lane = self.lane(lambda T, s: 5, [])
        with self.assertRaisesRegex(bar.BarError, "setup escalation; this lane plays the base game"):
            self.R.STOP.clear()
            lane.play(dict(bar.parse_id("bar-hota-pincushion-ac-1"), setup="escalation"))


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

            def __init__(self, j, out, dll, gpu=False, setup=None):
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
