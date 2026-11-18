#!/usr/bin/env python3
"""test_bar -- the bar harness offline: its game list and each game's seed, the blocks, the targets', the
calibration rows' and the mirror's pairs and their maps, the new persona rows (below L0, or one axis cut from
Hard's), the seating a game id gives, the start schema it reads from an .ota, the stall rule, the holds' end at the
cap, what a result reads from the level's end, the judge (a game at the cap scored by value at the limit; the
points, the interval over map means, the targets, the ladder's curtailment and the ordering's looks, a mirror pair
judged as the ladder, the bounds checked by integration where scipy is installed; `play` stopping a ladder pair
that has failed) and the b4 and b5 checks over synthetic results
(b5's yard from the second-factory line, each verdict); `--setup` (a setup's commanders from its sidedata, its
games marked with it and kept to a directory of their own, a setup that plays no ImpAI game refused); the cheat
flag (a setup flagged by its games' evidence alone, and judged apart); a game's end on a fake instance (THE END's
scenario, the level run to its teardown's lines, the game stopped, the play's lines cut at the apply, the cheat
flag's evidence from the recording, the recording kept or deleted, the log kept for every non-win, a level not
seeded from its game's seed, each seat's value at the cap from the recording, a persona row's lever, a mirror
game's two seats on theirs, and a game that did not play its row); `row` on fake games; and `play` on fake
lanes: a game that could not run, and a run
stopped by SIGINT. No game runs. Where the shared .data holds it, a recorded game's value at the limit, each seat's
from its recording and the ImpAI seat's equal to its pulse (zstandard: the shared venv).

    python3 tools/impai/test_bar.py
    .venv-undither/bin/python tools/impai/test_bar.py      # with the bounds' integration and the recorded game"""
import json
import math
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


def persona_line(seat, row, came="lever", level="hard"):
    le, lm, r, ae, be, am, bm, te, tm = row.split(",")
    return (f"impai: persona seat={seat} level={level} look={le},{lm} react={r} apm={ae}/{be},{am}/{bm} "
            f"att={te},{tm} from={came}")


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


def won(gid, outcome="A", **kw):
    """A played game's result, as the judge reads it."""
    g = bar.parse_id(gid)
    r = {"id": gid, "map": g["map"], "outcome": outcome, "end_tick": 30000, "cap": bar.CAP, "renderer": "gdi",
         "seed": g["seed"], "seats": g["seats"]}
    r.update(kw)
    return r


def cap_result(gid, a, b, ta=bar.CAP, tb=bar.CAP):
    """A game at the cap whose seats' values at the limit are `a` and `b` (read at ta and tb)."""
    return won(gid, "cap", end_tick=bar.CAP, value={"A": {"live": a, "t": ta}, "B": {"live": b, "t": tb}})


REC = "replays\\2026-11-13 153139 Pincushion.imprec"
REC_LINES = [f"recorder: recording {REC} from GameTime 1: 15001 slots, 38 tables",
             f"recorder: writer: {REC} closed: 900 iterations to GameTime 27015, 0 corrections"]


class TestBar(unittest.TestCase):
    def test_ids(self):
        size = {"dry": 10, "eoh": 120, "noh": 120, "p:L0": 40}
        for pair in list(bar.PAIRS) + ["p:L0"]:
            g = bar.ids(pair)
            self.assertEqual(len(g), size.get(pair, 100))
            self.assertEqual(len(set(g)), len(g))
            for gid in g:
                self.assertEqual(bar.parse_id(gid)["id"], gid)
        self.assertEqual(len(bar.MAPS), 13)
        self.assertEqual(list(bar.MAPS)[:10], list(bar.BAR_MAPS) + list(bar.DEV_MAPS))    # THE METAL MAPS stand last
        self.assertFalse(set(bar.METAL_MAPS) & (set(bar.BAR_MAPS) | set(bar.DEV_MAPS)))
        self.assertEqual({bar.parse_id(g)["map"] for g in bar.ids("hota") + bar.ids("eoh") + bar.ids("hn")},
                         set(bar.BAR_MAPS))                             # the held-out pool judges
        self.assertEqual({bar.parse_id(g)["map"] for g in bar.ids("p:L3")}, set(bar.DEV_MAPS))
        self.assertEqual(len(bar.ids("hn", blocks=12)), 120)

    def test_a_block_is_every_map_from_both_starts_with_every_side_pairing(self):
        for pair in ("eoh", "hn", "p:L1"):
            g = bar.ids(pair)
            maps = bar.pool(pair)
            for b in range(1, len(g) // 10 + 1):
                block = [bar.parse_id(x) for x in g[10 * (b - 1):10 * b]]      # the run is its blocks in order
                self.assertEqual({bar.block_of(x["id"]) for x in block}, {b})
                self.assertEqual(sorted((x["map"], x["start"]) for x in block),
                                 sorted((m, s) for m in maps for s in (0, 1)))
                pairings = {x["id"].split("-")[3] for x in block}
                self.assertEqual(pairings, set(bar.BLOCK_SIDES))
                for m in maps:                                            # one side pairing a map in a block
                    self.assertEqual(len({x["id"].split("-")[3] for x in block if x["map"] == m}), 1)
        on = [x.split("-")[3] for x in bar.ids("eoh") if "-pincushion-" in x]
        self.assertEqual({p: on.count(p) for p in on}, {p: 6 for p in bar.BLOCK_SIDES})   # a map plays each alike

    def test_the_new_pairs_seat_their_levels_against_ota_ai_at_hard(self):
        for pair, level in (("eoh", "easy"), ("noh", "normal")):
            g = bar.parse_id(bar.ids(pair)[0])
            self.assertEqual((g["difficulty"], g["seats"]), ("hard", {"A": {"ai": "impai", "level": level},
                                                                       "B": {"ai": "ai", "level": None}}))
        g = bar.parse_id("bar-p:L0-acidfoursome-ac-2")
        self.assertEqual((g["difficulty"], g["seats"]["A"], g["seats"]["B"], g["start"]),
                         ("hard", {"ai": "impai", "level": "hard", "persona": bar.PERSONA_ROWS["L0"]},
                          {"ai": "ai", "level": None}, 1))
        import level_lever
        for row, persona in bar.PERSONA_ROWS.items():                    # every row a lever the DLL takes
            self.assertIsNone(level_lever.persona_error(f"1={persona}"), row)
        for bad in ("bar-p:L9-acidfoursome-ac-1", "bar-p:-acidfoursome-ac-1", "bar-q:L1-acidfoursome-ac-1"):
            with self.assertRaises(bar.BarError):
                bar.parse_id(bad)
        with self.assertRaises(bar.BarError):
            bar.ids("p:L9")

    def test_the_mirror_seats_impai_against_impai_with_b_on_a_row(self):
        g = bar.parse_id("bar-m:L0b-acidfoursome-ac-2")
        self.assertEqual((g["difficulty"], g["seats"], g["start"]),
                         ("hard", {"A": {"ai": "impai", "level": "hard"},
                                   "B": {"ai": "impai", "level": "hard", "persona": bar.PERSONA_ROWS["L0b"]}}, 1))
        g = bar.parse_id("bar-n:Hatt-lavaalley-ca-1")
        self.assertEqual(g["seats"], {"A": {"ai": "impai", "level": "hard", "persona": bar.PERSONA_ROWS["L3"]},
                                      "B": {"ai": "impai", "level": "hard", "persona": bar.PERSONA_ROWS["Hatt"]}})
        self.assertEqual(bar.NORMAL_ROW, "L3")                           # the DLL's Normal, until one is chosen
        for pair in ("m:L1", "n:L0a"):
            gs = bar.ids(pair)
            self.assertEqual(len(gs), 100)                               # ten blocks, as the ladder
            self.assertEqual({bar.parse_id(x)["map"] for x in gs}, set(bar.DEV_MAPS))
            self.assertTrue(bar.block_pair(pair) and bar.ladder_pair(pair))
            self.assertEqual([x.replace(pair, "p:L1") for x in bar.ids(pair, blocks=2)], bar.ids("p:L1", blocks=2))
        self.assertFalse(bar.ladder_pair("p:L1"))
        self.assertTrue(bar.ladder_pair("hn"))
        self.assertNotEqual(bar.game_seed("bar-m:L1-acidfoursome-ac-1"), bar.game_seed("bar-p:L1-acidfoursome-ac-1"))
        for bad in ("bar-m:L9-acidfoursome-ac-1", "bar-n:-acidfoursome-ac-1", "bar-m-acidfoursome-ac-1"):
            with self.assertRaises(bar.BarError):
                bar.parse_id(bad)

    def test_the_new_rows_lie_below_l0_or_cut_one_axis_from_hards(self):
        rows = {k: [int(x) for x in v.split(",")] for k, v in bar.PERSONA_ROWS.items()}
        axes = {"look": (0, 1), "react": (2,), "apm": (3, 4, 5, 6), "att": (7, 8)}
        for row, cut in (("Hlook", {"look"}), ("Hreact", {"look", "react"}), ("Happm", {"apm"}), ("Hatt", {"att"})):
            self.assertEqual({a for a, ix in axes.items() if any(rows[row][i] != rows["L5"][i] for i in ix)}, cut,
                             row)
        self.assertEqual(rows["Hreact"][:2], rows["Hlook"][:2])          # Hreact against Hlook: the reaction alone

        def harsher(a, b):
            """every field of row a as harsh as b's or harsher: a look or reaction no shorter, a rate, burst or
            attention no larger and not 0 (0 is no limit), and at least one field strictly so"""
            ok = all(a[i] >= b[i] for i in (0, 1, 2)) and all(0 < a[i] <= b[i] for i in range(3, 9))
            return ok and a != b
        self.assertTrue(harsher(rows["L0a"], rows["L0"]))
        self.assertTrue(harsher(rows["L0b"], rows["L0a"]))
        self.assertTrue(harsher(rows["L0"], rows["L01"]) and harsher(rows["L01"], rows["L1"]))

    def test_every_game_has_its_own_seed_and_keeps_it(self):
        every = [gid for pair in list(bar.PAIRS) + [f"p:{r}" for r in bar.PERSONA_ROWS] for gid in bar.ids(pair)]
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
                r = {"id": gid, "map": bar.parse_id(gid)["map"], "outcome": "A", "end_tick": 30000, "wall_s": 100.0,
                     "stalls": {"A": []}, "renderer": "gdi", "seed": bar.game_seed(gid)}
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
            lost = [g for g in bar.ids("hn") if "-pincushion-" in g][:4]
            for gid in bar.ids("hn")[:95]:
                put(gid, outcome="B" if gid in lost else "A")
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

    def test_a_game_at_the_cap_is_scored_by_value_at_the_limit(self):
        gid = bar.ids("hn")[1]
        self.assertEqual(bar.by_value(cap_result(gid, 13000, 10000)), ("A", "value A 13000, B 10000"))
        self.assertEqual(bar.by_value(cap_result(gid, 12999, 10000))[0], "draw")    # under the 1.3x lead
        self.assertEqual(bar.by_value(cap_result(gid, 10000, 13000))[0], "B")
        self.assertEqual(bar.by_value(cap_result(gid, 0, 0))[0], "draw")            # no seat leads
        self.assertEqual(bar.by_value(cap_result(gid, 500, 0))[0], "A")
        who, why = bar.by_value(cap_result(gid, 13000, 10000, ta=81014))             # a pulse's tick, not the cap's
        self.assertIsNone(who)
        self.assertIn("no value of A's at the limit", why)
        self.assertIsNone(bar.by_value(dict(cap_result(gid, 1, 1), value={"A": {"live": 1}}))[0])
        self.assertEqual(bar.outcome_of(dict(cap_result(gid, 13000, 1), draw=True))[0], "draw")   # gone at one look
        self.assertIsNone(bar.outcome_of({"id": gid, "outcome": "could_not_run"})[0])
        self.assertEqual([bar.POINTS[bar.outcome_of(cap_result(gid, a, 10000))[0]] for a in (13000, 10000, 1)],
                         [1.0, 0.5, 0.0])
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)

            def put(r):
                (d / f"{r['id']}.json").write_text(json.dumps(r))
            hn = bar.ids("hn")
            for g in hn[:86]:
                put(won(g))
            for g in hn[86:90]:
                put(cap_result(g, 20000, 10000))                            # four won by value: 90 of 100
            for g in hn[90:]:
                put(cap_result(g, 10000, 10000))                            # drawn by value
            with mock.patch("builtins.print") as said:
                self.assertEqual(bar.judge([d]), 0)
            text = "\n".join(str(c.args[0]) for c in said.call_args_list)
            self.assertIn("at the cap (a 1.3x lead in value at the limit, or both gone at one look): A 4, draw 10",
                          text)
            self.assertIn("ladder: 90 of its first 100 won, 90 asked", text)
            put(cap_result(hn[86], 10000, 20000))                           # one of them B's: 89, and 11 not won
            with mock.patch("builtins.print") as said:
                self.assertEqual(bar.judge([d]), 1)
            self.assertIn("ladder: curtailed, failed for certain: 11", "\n".join(str(c.args[0])
                                                                                 for c in said.call_args_list))
            put(dict(cap_result(hn[86], 20000, 10000), draw=True, end_tick=60000))   # both gone in one tick: a draw
            self.assertEqual(bar.judge([d]), 1)
            put(cap_result(hn[86], 20000, 10000, tb=81007))                 # no value at the limit: no verdict
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


class TestReadIds(unittest.TestCase):
    """`play --ids`: a comma list longer than a file name can be is a list, not a failed stat (ENAMETOOLONG)."""

    def test_a_long_comma_list(self):
        got = bar.ids("h:2.828")[:12]
        arg = ",".join(got)
        self.assertGreater(len(arg), 255)
        self.assertEqual(bar.read_ids(arg), got)

    def test_a_file_of_ids(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "ids.txt"
            f.write_text("bar-x-1\nbar-x-2\n")
            self.assertEqual(bar.read_ids(str(f)), ["bar-x-1", "bar-x-2"])

    def test_a_short_comma_list(self):
        self.assertEqual(bar.read_ids("bar-x-1,bar-x-2"), ["bar-x-1", "bar-x-2"])


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
        self.assertNotIn("value", r)                                    # the value is the recording's, at the cap
        self.assertEqual(r["persona"], {})
        self.assertEqual(bar.read_result(lines + [persona_line(2, "60,90,45,10,3,15,3,4,2", "level"),
                                                  persona_line(1, "15,30,0,0,0,0,0,0,0")], seat, g, "B",
                                         20000)["persona"],
                         {"A": {"row": "60,90,45,10,3,15,3,4,2", "from": "level"}})
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

    def test_a_takeovers_recording_is_the_file_its_fork_went_into(self):
        fork = ["recorder: recording timeline 1 of replays\\x.imprec from GameTime 62701 (parent 0): 15001 slots",
                "recorder: writer: replays\\x.imprec closed: 18450 iterations to GameTime 81150"]
        self.assertEqual(bar.recording_of(fork, fork=True), ("replays\\x.imprec", True))
        self.assertIsNone(bar.recording_of(fork))
        self.assertIsNone(bar.recording_of(REC_LINES, fork=True))

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

        def play_all(games, lanes, out_dir, gpu, give_up=None):
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

    def lane(self, alive, end_lines, recording=REC_LINES, nodraw=False, seeded=True, commanders=bar.COMMANDER,
             play_lines=()):
        """A Lane whose holds land where they are asked, whose seats hold alive(T) units, and whose level, once
        resumed after the end scenario, writes `end_lines` and `recording`'s lines; with `nodraw` its worker runs
        no draw and its log holds the level's account; with `seeded` its launch's apply seeded the level from the
        game's seed; its play logs `play_lines`. self.events records the end's steps in order."""
        R, tmp = self.R, self.tmp
        events = []
        account = {"from": 2, "to": 9000, "skipped": 8900, "board": 0, "drawn": 98} if nodraw else None

        class Log:
            NAP_S = 0
            nodraw = account
            seed = None                 # the game's, set by play()

            def __init__(self, w):
                self.lines = (list(recording[:1]) + ([seeds_line(Log.seed)] if seeded else []) + list(play_lines)
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
        lane.costs = {7: (50.0, 600.0), 9: (60.0, 0.0), 12: (2000.0, 30000.0)}
        lane.install = {"vfs": None, "commanders": commanders, "ai_only": {"cort1mex_ai"}}
        lane.w.nodraw = nodraw
        lane.hold, lane.peek = hold, peek
        self.T, self.events, self.Log = 1, events, Log
        return lane

    def play(self, lane, gid, cap=27000, rec=None, units=(), values=()):
        """Lane.play of game `gid` on the fake lane (its `values` asked), its recording reading as `rec`
        (recording_evidence's shape; by default each record's commander income and no death) and holding `units`
        alive at every iteration it is asked for. self.read_at is the cap's, self.read_values the others'."""
        R, events = self.R, self.events
        rec = rec or {"at": 1, "income": {k: (1.0, 25.0) for k in range(10)}, "deaths": []}
        self.applied, self.read_at, self.read_values = [], None, None

        def recording_evidence(path, R, at=None, values=()):
            self.read_at, self.read_values = at, list(values)
            out = dict(rec, values={t: {"at": t, "units": list(units)} for t in values})
            return dict(out, units={"at": at, "units": list(units)}) if at is not None else out

        def tacli(*a, **k):
            if a[:2] == ("scenario", "apply"):
                events.append("apply")
                self.applied.append(json.loads(Path(a[3]).read_text()))
            return 0, ""
        with mock.patch.object(R, "Log", self.Log), mock.patch.object(R, "tacli", tacli), \
                mock.patch.object(R, "resume", lambda w, log: events.append("resume")), \
                mock.patch.object(R, "held_start", lambda w, log: 1), mock.patch.object(R, "say"), \
                mock.patch.object(bar, "recording_evidence", recording_evidence), \
                mock.patch.object(bar, "start_positions", lambda name, vfs=None: {2: (100, 100), 3: (900, 900)}):
            self.Log.seed = bar.game_seed(gid)
            return lane.play(dict(bar.parse_id(gid), cap=cap, values=list(values)))

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

    def test_a_game_run_to_the_cap_takes_each_seats_value_at_the_cap_from_its_recording_then_is_kept(self):
        gid = "bar-hota-pincushion-ac-2"                                 # A in seat 2, B in seat 1
        lane = self.lane(lambda T, s: 5,
                         [eco_end(2), mil_end(2), "impai: handicap rows scaled/exempt gametime=27015:"])
        # (owner, type, fraction left): A's two finished mexes and a frame; B's finished fusion; the human's mex
        units = [(2, 7, 0.0), (2, 7, 0.0), (2, 12, 0.5), (1, 12, 0.0), (0, 7, 0.0)]
        r = self.play(lane, gid, units=units)
        self.assertEqual((r["outcome"], r["end_tick"], self.T), ("cap", 27000, 27015))   # the end applied after
        self.assertEqual(self.read_at, 27000)                            # the value is the cap's own iteration
        self.assertEqual(r["value"], {"A": {"live": 120.0, "t": 27000}, "B": {"live": 2500.0, "t": 27000}})
        self.assertEqual(bar.by_value(r)[0], "B")
        self.assertEqual(r["end"]["A"]["tech_m"], 8.0)
        self.assertTrue(r["recording"].endswith(f"{gid}.imprec"))
        lane = self.lane(lambda T, s: 5, [eco_end(2), "impai: handicap rows scaled/exempt gametime=27015:"])
        with self.assertRaisesRegex(self.R.CouldNotRun, "no value of a seat's: .* type 40"):
            self.play(lane, gid, units=[(2, 40, 0.0)])                   # a type the level's table does not hold

    def test_the_values_asked_at_fixed_gametimes_are_read_before_the_games_end(self):
        gid = "bar-hota-pincushion-ac-1"                                 # A in seat 1, B in seat 2; won at 9000
        lane = self.lane(lambda T, s: 0 if s == 2 and T >= 9000 else 5,
                         [eco_end(1), mil_end(1), "impai: handicap rows scaled/exempt gametime=9000:"])
        r = self.play(lane, gid, units=[(1, 7, 0.0), (2, 9, 0.0)], values=(30000, 6000, 9000))
        self.assertEqual((r["outcome"], self.read_at, self.read_values), ("A", None, [6000]))   # 9000 is its end
        self.assertEqual(r["values"], {"6000": {"A": {"live": 60.0, "t": 6000}, "B": {"live": 60.0, "t": 6000}}})
        self.assertNotIn("value", r)

    def test_a_persona_row_goes_to_its_lever_and_a_game_that_did_not_play_it_could_not_run(self):
        gid = "bar-p:L0-acidfoursome-ac-1"                               # A in seat 1
        ends = [eco_end(1), mil_end(1), "impai: handicap rows scaled/exempt gametime=9000:"]
        row = bar.PERSONA_ROWS["L0"]
        r = self.play(self.lane(lambda T, s: 0 if s == 2 and T >= 9000 else 5, ends,
                                play_lines=[persona_line(1, row)]), gid)
        self.assertEqual((r["outcome"], r["persona"]), ("A", {"A": {"row": row, "from": "lever"}}))
        self.assertEqual(r["seats"]["A"]["persona"], row)
        gamedir = self.tmp / "gamedir"
        self.assertEqual((gamedir / "tagpu_impai_persona.on").read_text(), f"1={row}\n")
        self.assertEqual((gamedir / "tagpu_impai_level.on").read_text(), "1=hard\n")
        for lines in ([], [persona_line(1, row, "level")], [persona_line(1, bar.PERSONA_ROWS["L1"])]):
            with self.assertRaisesRegex(self.R.CouldNotRun, "did not play its persona"):
                self.play(self.lane(lambda T, s: 0 if s == 2 and T >= 9000 else 5, ends, play_lines=lines), gid)
        self.play(self.lane(lambda T, s: 0 if s == 2 and T >= 9000 else 5, ends), "bar-hota-pincushion-ac-1")
        self.assertFalse((gamedir / "tagpu_impai_persona.on").exists())   # a game on no row clears the lever

    def test_a_mirror_game_puts_b_alone_on_its_row_and_both_seats_at_hard(self):
        gid = "bar-m:L0b-acidfoursome-ac-1"                              # A in seat 1, B in seat 2
        row, hard = bar.PERSONA_ROWS["L0b"], bar.PERSONA_ROWS["L5"]
        ends = [eco_end(1), mil_end(1), eco_end(2), mil_end(2), "impai: handicap rows scaled/exempt gametime=9000:"]
        alive = lambda T, s: 0 if s == 2 and T >= 9000 else 5           # noqa: E731
        r = self.play(self.lane(alive, ends, play_lines=[persona_line(1, hard, "level"), persona_line(2, row)]), gid)
        self.assertEqual((r["outcome"], r["persona"]), ("A", {"A": {"row": hard, "from": "level"},
                                                                "B": {"row": row, "from": "lever"}}))
        gamedir = self.tmp / "gamedir"
        self.assertEqual((gamedir / "tagpu_impai_persona.on").read_text(), f"2={row}\n")
        self.assertEqual((gamedir / "tagpu_impai_level.on").read_text(), "1=hard\n2=hard\n")
        with self.assertRaisesRegex(self.R.CouldNotRun, "seat B did not play its persona"):
            self.play(self.lane(alive, ends, play_lines=[persona_line(1, hard, "level"),
                                                         persona_line(2, row, "level")]), gid)
        # n:<row>: A on Normal's row through the lever too, B on its own
        gid = "bar-n:L0b-acidfoursome-ac-2"                              # A in seat 2, B in seat 1
        normal = bar.PERSONA_ROWS[bar.NORMAL_ROW]
        r = self.play(self.lane(lambda T, s: 0 if s == 1 and T >= 9000 else 5, ends,
                                play_lines=[persona_line(1, row), persona_line(2, normal)]), gid)
        self.assertEqual((r["outcome"], r["persona"]["A"]), ("A", {"row": normal, "from": "lever"}))
        self.assertEqual((gamedir / "tagpu_impai_persona.on").read_text(), f"2={normal}\n1={row}\n")

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
        self.assertNotIn("value", r)                    # a game decided before the cap needs none
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


def _scipy():
    try:
        import scipy.stats  # noqa: F401
        return True
    except ImportError:
        return False


class TestProtocol(unittest.TestCase):
    """The levels' verdicts: THE INTERVAL over map means, THE LOOKS and their futility, the targets, THE
    CURTAILMENT in the judge and in `play`."""

    def judge(self, results):
        with tempfile.TemporaryDirectory() as d:
            for r in results:
                (Path(d) / f"{r['id']}.json").write_text(json.dumps(r))
            with mock.patch("builtins.print") as said:
                rc = bar.judge([Path(d)])
        return rc, "\n".join(str(c.args[0]) for c in said.call_args_list)

    def test_the_interval_and_the_t_over_map_means(self):
        mean, half = bar.interval([1.0, 1.0, 1.0, 0.0, 0.0])
        self.assertAlmostEqual(mean, 0.6)
        self.assertAlmostEqual(half, 2.132 * (0.3 ** 0.5) / 5 ** 0.5)      # sd 0.548 over 5 maps, df 4
        self.assertEqual(bar.interval([0.7]), (0.7, None))
        self.assertAlmostEqual(bar.t_stat([0.6, 0.7, 0.8, 0.7, 0.7]), 0.2 / (0.0707107 / 5 ** 0.5), places=3)
        self.assertEqual(bar.t_stat([0.8] * 5), math.inf)
        self.assertEqual(bar.t_stat([0.2] * 5), -math.inf)
        self.assertEqual(bar.t_stat([0.5] * 5), 0.0)

    def test_a_look_decides_at_its_bound_and_gives_up_below_its_conditional_power(self):
        self.assertEqual(bar.look(1, 3, 5, 6.4)[0], "better")
        self.assertEqual(bar.look(1, 3, 5, -6.4)[0], "worse")
        verdict, bound, cp = bar.look(1, 3, 5, 2.0)                       # a strong trend: carry on
        self.assertEqual((verdict, bound), (None, 6.398))
        self.assertAlmostEqual(cp, 0.984, places=3)
        verdict, _, cp = bar.look(1, 3, 5, 0.2)                           # no trend: futile
        self.assertEqual(verdict, "futile")
        self.assertAlmostEqual(cp, 0.047, places=3)
        self.assertEqual(bar.look(3, 3, 5, 2.0), (None, 2.253, None))     # the last look has no futility
        self.assertEqual(bar.look(3, 3, 5, 2.3)[0], "better")
        self.assertEqual(bar.look(2, 2, 10, 1.9)[0], "better")

    @unittest.skipUnless(_scipy(), "scipy is not installed (the shared venv has it)")
    def test_the_bounds_are_the_specifications_by_integration(self):
        import numpy as np
        from scipy import stats
        for df, q in bar.T95.items():
            self.assertAlmostEqual(q, stats.t.isf(0.05, df), places=3)
        for k, zb in bar.OBF_Z.items():
            t = np.arange(1, k + 1) / k
            cov = np.sqrt(np.minimum.outer(t, t) / np.maximum.outer(t, t))
            b = np.array(zb)
            inside = stats.multivariate_normal(mean=np.zeros(k), cov=cov).cdf(b, lower_limit=-b)
            self.assertAlmostEqual(1 - inside, 0.10, places=3)            # two-sided 10 % over the looks
            self.assertTrue(np.allclose(b * np.sqrt(t), b[-1], atol=2e-3))    # O'Brien-Fleming: c / sqrt(t)
            for maps in (5, 10):
                want = stats.t.isf(stats.norm.sf(b), maps - 1)
                self.assertTrue(np.allclose(bar.OBF_T[(k, maps)], want, atol=2e-3), (k, maps))

    def test_the_targets_on_their_blocks(self):
        def by_map(pair, wins_on, blocks=None):
            return [won(g, "A" if bar.parse_id(g)["map"] in wins_on else "B") for g in bar.ids(pair, blocks=blocks)]
        three = ("pincushion", "redtriangle", "troutfarm")
        rc, text = self.judge(by_map("eoh", three))                       # 0.6 of the points: in Easy's band
        self.assertEqual(rc, 0)
        self.assertIn("points: 0.600 of A's over 12 blocks, t90 [0.078, 1.122] over 5 map means", text)
        self.assertIn("Easy's floor 0.50: met", text)
        rc, text = self.judge(by_map("eoh", three + ("lavamania",)))       # 0.8: above the band, accepted
        self.assertEqual((rc, "Easy's floor 0.50: met (above the band's 0.75, accepted)" in text), (0, True))
        rc, text = self.judge(by_map("eoh", ("pincushion", "redtriangle")))  # 0.4: below the floor
        self.assertEqual((rc, "Easy's floor 0.50: missed" in text), (1, True))
        rs = by_map("eoh", three)
        rs[7]["stalls"] = {"A": [{"from": 1807, "to": 5407, "economy": True, "army": False}]}
        rc, text = self.judge(rs)                                          # in the band, but a stall
        self.assertEqual(rc, 1)
        self.assertIn(f"stalls, each a failure (the no-stall rule): {rs[7]['id']}", text)
        rc, text = self.judge(by_map("eoh", three)[:110])                  # 11 blocks of 12
        self.assertEqual(rc, 2)
        self.assertIn("lacks blocks: 11 of its 12 complete", text)
        rs = by_map("noh", list(bar.BAR_MAPS))
        rs[3] = cap_result(rs[3]["id"], 100, 100)                         # a draw at the cap: half a point
        rc, text = self.judge(rs)
        self.assertEqual(rc, 0)
        self.assertIn("Normal's floor 0.90: met", text)
        self.assertIn("points: 0.996", text)
        rs[3] = cap_result(rs[3]["id"], 100, 100, tb=81014)               # a cap with no value at the limit
        rc, text = self.judge(rs)
        self.assertEqual(rc, 2)
        self.assertIn("lacks blocks: 0 of its 12 complete", text)          # block 1 holds it
        rc, text = self.judge(by_map("p:L1", ("acidfoursome", "steeljungle", "luschaven", "gasplantplain")))
        self.assertEqual(rc, 0)                                            # a calibration row reports, judges none
        self.assertIn("points: 0.800 of A's over 4 blocks", text)
        self.assertIn("as Easy (band 0.50..0.75): above it, a harsher row; as Normal (floor 0.90): below it, a "
                      "softer row", text)
        rs = by_map("p:L1", list(bar.DEV_MAPS))
        rs[0]["seats"] = {"A": {"ai": "impai", "level": "hard", "persona": bar.PERSONA_ROWS["L2"]}}
        rc, text = self.judge(rs)                                          # a game of another row under its name
        self.assertEqual(rc, 2)
        self.assertIn(f"played on another persona than p:L1's, not read: {rs[0]['id']}", text)

    def test_the_ladder_is_curtailed_and_the_ordering_reads_its_looks(self):
        hn = bar.ids("hn", blocks=12)
        rc, text = self.judge([won(g) for g in hn[:40]])                  # 4 blocks, all won: the ordering decides
        self.assertEqual(rc, 2)                                            # the ladder lacks its 100
        self.assertIn("ordering: look 1 at 40 games: t inf, bound 6.398: better", text)
        rc, text = self.judge([won(g, "B" if i < 11 else "A") for i, g in enumerate(hn[:30])])
        self.assertEqual(rc, 1)                                            # failed for certain at 30 games
        self.assertIn("ladder: curtailed, failed for certain: 11 of its first 100 games not won", text)
        lost = [g for g in hn if bar.parse_id(g)["map"] in ("pincushion", "redtriangle")]
        rs = [won(g, "B" if g in lost else "A") for g in hn]               # 0.6 a block, two maps lost
        rc, text = self.judge(rs)
        self.assertEqual(rc, 1)
        self.assertIn("ordering: look 1 at 40 games: t 0.41, bound 6.398, conditional power 0.11: futile", text)
        self.assertNotIn("look 2", text)
        rs = [won(g, "B" if g in lost[:4] else "A") for g in hn]           # 4 lost in 120, all in block 1
        rc, text = self.judge(rs)
        self.assertEqual(rc, 0)
        self.assertIn("ladder: 96 of its first 100 won, 90 asked", text)
        self.assertIn("ordering: look 1 at 40 games", text)

    def test_a_mirror_pair_is_judged_as_the_ladder_is(self):
        m = bar.ids("m:L0b", blocks=12)
        rc, text = self.judge([won(g, "B" if i < 11 else "A") for i, g in enumerate(m[:30])])
        self.assertEqual(rc, 1)
        self.assertIn("m:L0b: 19 of 30 won by A", text)
        self.assertIn("ladder: curtailed, failed for certain: 11 of its first 100 games not won", text)
        rc, text = self.judge([won(g) for g in m[:40]])
        self.assertIn("ordering: look 1 at 40 games: t inf, bound 6.398: better", text)
        lost = [g for g in m[:20] if bar.parse_id(g)["map"] in ("acidfoursome", "steeljungle")]
        rc, text = self.judge([won(g, "B" if g in lost else "A") for g in m[:20]])   # two blocks, two maps lost
        self.assertEqual(rc, 2)
        self.assertIn("ladder: 12 won of the 20 scored of its first 100; lacks games", text)
        self.assertIn("ordering: look 1 at 40 games not yet read (2 blocks complete; t 0.41 over the 2 complete, "
                      "no look)", text)
        self.assertNotIn("as Easy", text)                                # a calibration row's lines, not the mirror's
        self.assertEqual(self.play(["--ids", "m:L0b"]), bar.ids("m:L0b")[:11])

    def play(self, argv, kept=()):
        """bar.main(play argv) with play_all faked to lose every game it is given, in order, unless give_up: the
        ids it played."""
        import runlock
        got = []

        def play_all(games, lanes, out_dir, gpu, give_up=None):
            rs = []
            for g in games:
                if not give_up(g, rs):
                    rs.append(won(g["id"], "B"))
                    got.append(g["id"])
            return rs
        with tempfile.TemporaryDirectory() as d:
            for r in kept:
                (Path(d) / f"{r['id']}.json").write_text(json.dumps(r))
            with mock.patch.object(bar, "play_all", play_all), mock.patch.object(runlock, "hold"), \
                    mock.patch.object(runlock, "release"), mock.patch.object(bar, "close_lanes"), \
                    mock.patch("builtins.print"):
                self.assertEqual(bar.main(["play", "--lanes", "1", "--out", d] + argv), 0)
        return got

    def test_play_stops_a_ladder_pair_once_it_has_failed_for_certain(self):
        hn = bar.ids("hn")
        self.assertEqual(self.play(["--ids", "hn"]), hn[:11])
        self.assertEqual(len(self.play(["--ids", "hn", "--no-curtail"])), 100)
        self.assertEqual(len(self.play(["--ids", "eoh"])), 120)           # a target's games are all played
        kept = [won(g, "B", pacing="nodraw") for g in hn[:10]]             # ten lost already: one more ends it
        self.assertEqual(self.play(["--ids", "hn"], kept), hn[10:11])


def _common_data() -> Path:
    """The main checkout's .data, which every worktree shares (its git common dir's parent)."""
    import subprocess
    out = subprocess.run(["git", "-C", str(bar.TREE), "rev-parse", "--git-common-dir"], capture_output=True,
                         text=True).stdout.strip()
    return (bar.TREE / (out or ".git")).resolve().parent / ".data"


class TestRecordedGames(unittest.TestCase):
    """THE VALUE AT THE LIMIT on a recorded game of the bar (the shared .data, when this checkout has it): each
    seat's value from the recording at the cap; the ImpAI seat's equal to its own pulse's `vlive`."""
    DATA = _common_data() / "impai" / "runs"
    GAME = DATA / "broad" / "198899ef" / "base" / "bar-eota-troutfarm-ca-2.json"
    UNITS = DATA / "levels" / "p2a" / "base-units.json"          # the base game's `tacli units --json`

    def setUp(self):
        r = json.loads(self.GAME.read_text()) if self.GAME.is_file() else {}
        if not (self.UNITS.is_file() and Path(r.get("recording") or "-").is_file()):
            self.skipTest("the recorded game or its type table is not in this checkout's .data")
        try:
            import zstandard  # noqa: F401
        except ImportError:
            self.skipTest("a recording's packed blocks need zstandard (the shared venv has it)")
        self.r = r

    def test_each_seats_value_at_the_limit(self):
        import tareplay
        r = self.r
        costs = {u["id"]: (float(u["cost"][0]), float(u["cost"][1]))
                 for u in json.loads(self.UNITS.read_text())["units"]}
        seat = {s: r["seats"][s]["seat"] for s in ("A", "B")}               # A ImpAI Easy in seat 2, B OTA AI
        rec = bar.recording_evidence(Path(r["recording"]), tareplay, at=81014)    # A's pulse of the cap's minute
        v = bar.value_at(rec["units"], seat, costs)
        self.assertAlmostEqual(v["A"]["live"], 54253, delta=0.5)          # its pulse: vlive=54253 at t=81014
        self.assertAlmostEqual(v["B"]["live"], 39769.09, delta=0.01)
        self.assertEqual(bar.by_value(dict(r, value={s: dict(v[s], t=r["cap"]) for s in v})),
                         ("A", "value A 54253, B 39769"))                   # 1.364 times B's: A's game


if __name__ == "__main__":
    unittest.main()
