#!/usr/bin/env python3
"""test_opening -- the unmapped opening offline: its id, its seats and its scenario (the bar's template, not
mapped, each commander at its seat's start), the figures from a result, a log and a recording read through
think.py's and tarec.py's interfaces on fakes, the figures' line, and each verdict word. No game runs and no
install is read.

    python3 tools/impai/test_opening.py"""
import json
import struct
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bar  # noqa: E402
import opening  # noqa: E402
import think  # noqa: E402

STARTS = {n: (100 * n, 200 * n) for n in range(1, 11)}
PLANT = "impai: plant seat={seat} gametime={t} want=first T=1 reach=ffffc86f chose={what} s=0.521 next=ARMLAB s=0.653"
NAMES = ["None", "ARMCOM", "ARMVP", "ARMMEX", "ARMSOLAR", "CORUWMME", "ARMMERL"]


def result(seat=2, alive=7, end=6000):
    return {"id": "unmapped-steeljungle-2", "outcome": "cap", "end_tick": end, "cap": end,
            "seats": {"A": {"seat": seat}, "B": {"seat": 1}},
            "looks": [{"at": 3000, "seats": {"A": {"alive": 2}}}, {"at": end, "seats": {"A": {"alive": alive}}}]}


class TestOpening(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(__import__("shutil").rmtree, self.tmp, True)

    def test_an_id_names_a_map_of_the_bars_and_a_start(self):
        self.assertEqual(opening.parse_id("unmapped-lavaalley-3"), ("lavaalley", 3))
        self.assertTrue(opening.is_id("unmapped-x") and not opening.is_id("bar-hota-troutfarm-ca-1"))
        self.assertEqual(opening.parse_id("mapped-lavaalley-3"), ("lavaalley", 3))
        self.assertTrue(opening.mapping("mapped-lavaalley-3") and not opening.mapping("unmapped-lavaalley-3"))
        self.assertEqual(opening.spec("mapped-lavaalley-3")["seed"], opening.spec("unmapped-lavaalley-3")["seed"])
        for bad in ("unmapped-nowhere-2", "unmapped-steeljungle-0", "unmapped-steeljungle", "unmapped-steeljungle-2-1"):
            with self.assertRaises(bar.BarError):
                opening.parse_id(bad)

    def test_the_spec_is_a_bar_game_that_names_its_own_seats(self):
        g = opening.spec("unmapped-steeljungle-2")
        self.assertEqual((g["map"], g["cap"], g["seed"], g["setup"], g["recordings"], g["verdict"]),
                         ("Steel Jungle", 6000, bar.game_seed("unmapped-steeljungle-2"), None, True, "survives"))
        self.assertEqual(g["seats"], {"A": {"ai": "impai", "level": "hard"}, "B": {"ai": "ai", "level": None}})
        self.assertNotIn("doc", g)                                  # read with no install
        self.assertEqual(bar.seat_of(g), {"A": 2, "B": 1})
        self.assertEqual(bar.seat_of(opening.spec("unmapped-lavaalley-3")), {"A": 3, "B": 1})
        self.assertEqual(bar.seat_of(opening.spec("unmapped-troutfarm-1")), {"A": 1, "B": 2})
        self.assertEqual(opening.spec("unmapped-troutfarm-2", 9000)["cap"], 9000)
        # the bar's own games keep the bar's two seats
        self.assertEqual(bar.seat_of(bar.parse_id("bar-hota-troutfarm-ca-1")), {"A": 1, "B": 2})

    def test_the_scenario_is_the_bars_template_unmapped_with_each_seat_at_its_start(self):
        g = opening.game("unmapped-lavaalley-3", self.tmp / "s", starts=STARTS)
        doc, template = g["doc"], json.loads(bar.TEMPLATE.read_text())
        self.assertEqual(json.loads((self.tmp / "s" / "unmapped-lavaalley-3.json").read_text()), doc)
        self.assertIs(template["setup"]["mapping"], True)
        self.assertIs(doc["setup"]["mapping"], False)
        same = {k: v for k, v in doc["setup"].items() if k not in ("map", "mapping", "players")}
        self.assertEqual(same, {k: v for k, v in template["setup"].items() if k not in ("map", "mapping", "players")})
        self.assertEqual((doc["setup"]["map"], doc["seed"]), ("Lava Alley", g["seed"]))
        self.assertEqual([(p["slot"], p["controller"], p["side"], p.get("ai")) for p in doc["setup"]["players"]],
                         [(0, "human", "arm", None), (1, "ai", "core", "off"), (2, "ai", "core", "off"),
                          (3, "impai", "arm", None)])
        # seat k's commander at start k, StartPos(k + 1)
        self.assertEqual([(u["owner"], u["type"], u["pos"]) for u in doc["units"]],
                         [(1, "CORCOM", [200, 400]), (2, "CORCOM", [300, 600]), (3, "ARMCOM", [400, 800])])
        first = opening.game("unmapped-troutfarm-1", self.tmp / "s", starts=STARTS)["doc"]
        self.assertEqual([(u["owner"], u["type"]) for u in first["units"]], [(1, "ARMCOM"), (2, "CORCOM")])
        with self.assertRaisesRegex(bar.BarError, "no StartPos4"):
            opening.game("unmapped-lavaalley-3", self.tmp / "s", starts={1: (0, 0), 2: (0, 0), 3: (0, 0)})

    def test_the_starts_are_read_for_a_skirmish_of_the_games_seats(self):
        asked = []

        def starts(name, seats=3, vfs=None):
            asked.append((name, seats))
            return STARTS
        with mock.patch.object(bar, "start_positions", starts):
            opening.game("unmapped-lavaalley-3", self.tmp / "s")
            opening.game("unmapped-steeljungle-2", self.tmp / "s")
        self.assertEqual(asked, [("Lava Alley", 4), ("Steel Jungle", 3)])

    def test_the_figures_are_the_results_the_logs_and_the_recordings(self):
        log = "\n".join(["impai: seam in", PLANT.format(seat=1, t=30, what="CORVP"),
                         PLANT.format(seat=2, t=1949, what="ARMVP"), PLANT.format(seat=2, t=4000, what="ARMLAB")])
        asked = []

        def recorded(rec, seat, cap, trees):
            asked.append((rec, seat, cap, trees))
            return {"order": 44, "mex": False, "mexes": 0, "income": 1.25}
        f = opening.figures(result(), log, Path("x.imprec"), ["/src/other"], recorded)
        self.assertEqual(f, {"order": 44, "plant": [1949, "ARMVP"], "mex": False, "mexes": 0, "units": 7,
                             "income": 1.25})
        self.assertEqual(asked, [(Path("x.imprec"), 2, 6000, ["/src/other"])])
        self.assertEqual(opening.figures_text(f), "order 44; plant ARMVP 1949; extractor none; at the cap 0 "
                                                  "extractors, 7 units, metal 1.25")
        # no recording: what it holds is not read; a log of the seat's with no plant line holds no plant
        f = opening.figures(result(), "impai: pulse seat=2 t=14 built=1", None)
        self.assertEqual(f, {"order": None, "plant": False, "mex": None, "mexes": None, "units": 7, "income": None})
        self.assertEqual(opening.figures_text(f), "order ?; plant none; extractor ?; at the cap ? extractors, 7 "
                                                  "units, metal ?")
        # a recording that does not read fails nothing, and no log is no plant figure
        def broken(rec, seat, cap, trees):
            raise ValueError("it ends before the cap")
        f = opening.figures(result(), "", Path("x.imprec"), None, broken)
        self.assertEqual((f["plant"], f["order"], f["units"]), (None, None, 7))
        self.assertEqual(opening.figures({"outcome": "could_not_run"}, log, None)["plant"], None)

    def recording(self, events, layout=True):
        """recorded() of a recording faked at think.py's and tarec.py's interfaces: seat 2's block is slots 21 to
        30; its commander, a plant, an extractor born at 5000 and one past the cap, a solar born and dead, and
        seat 1's extractor."""
        ex = think.Extract("fake.imprec", {"units_each": 10, "slots": 40, "unitdefs": [len(NAMES)],
                                           "players": [{"index": k} for k in range(3)], "stride": {"job": 36}})
        for t, slot, typ in ((1, 21, 2), (1, 11, 4), (500, 22, 3), (900, 23, 5), (1200, 23, 0), (5000, 24, 4),
                             (6001, 25, 4)):
            ex.tick(t)
            (ex.unit(slot, typ) if typ else ex.gone(slot))

        def fit(_ex, _lays):
            if not layout:
                raise think.ThinkError("another layout")
            return "lay"
        players = [bytes(0x100), bytes(0x100), bytes(bar.PLAYER_METAL_IN) + struct.pack("<f", 2.5) + bytes(0x60)]
        tarec = types.SimpleNamespace(load=lambda rec: ("h", "bl", None),
                                      stream_to=lambda h, bl, want: types.SimpleNamespace(tick=want, players=players))
        with mock.patch.object(think, "extract", lambda rec: ex), \
                mock.patch.object(think, "type_names", lambda _ex: NAMES), \
                mock.patch.object(think, "layouts", lambda trees: ["lay"]), mock.patch.object(think, "fit", fit), \
                mock.patch.object(think, "select", lambda _ex, lay, seat, paths: (seat, paths)), \
                mock.patch.object(think, "changes", lambda _ex, sels, t0, t1: ({}, events)), \
                mock.patch.dict(sys.modules, {"tarec": tarec}):
            return opening.recorded(Path("fake.imprec"), 2, 6000)

    def test_a_recordings_figures_are_its_job_rows_its_units_and_its_income_at_the_cap(self):
        def ev(t, name, v, size):
            f = types.SimpleNamespace(name=name, value=lambda raw: int.from_bytes(raw, "little"))
            return (t, "job", 22, f, bytes(size), v.to_bytes(size, "little"))
        # a job cleared, a walk to a feature at GameTime 44, a plant, a rocket truck whose name ends as no extractor's
        # does, an extractor
        events = [ev(30, "kind", 0, 1), ev(45, "kind", 5, 1), ev(375, "build", 2, 2), ev(375, "kind", 1, 1),
                  ev(600, "build", 0xFFFF, 2), ev(700, "build", 6, 2), ev(5280, "build", 5, 2)]
        self.assertEqual(self.recording(events), {"mexes": 1, "order": 44, "mex": 5279, "income": 2.5})
        self.assertEqual(self.recording(events[:1]), {"mexes": 1, "order": False, "mex": False, "income": 2.5})
        # another layout's recording: its job rows are not read, its units and income are
        self.assertEqual(self.recording(events, layout=False), {"mexes": 1, "income": 2.5})

    def test_each_verdict_word(self):
        def f(order=44, plant=(374, "ARMVP"), mex=5279, units=12):
            return {"order": order, "plant": list(plant) if plant else plant, "mex": mex, "units": units}
        self.assertEqual(opening.verdict(f(False, False, False, 1), f(44, (1949, "ARMVP"), False, 11)),
                         (1, "the test alone gives an order, a plant; units at the cap 1 | 11"))
        self.assertEqual(opening.verdict(f(), f(mex=False))[0], -1)
        self.assertEqual(opening.verdict(f(), f(mex=False, units=20)),
                         (0, "the base alone gives an extractor; units at the cap 12 | 20"))
        # when an order was given is reported, not judged
        self.assertEqual(opening.verdict(f(), f(plant=(1874, "ARMLAB"))),
                         (0, "the same milestones, the units within the margin"))
        # the units: more than two apart and more than a quarter of the larger
        self.assertEqual([opening.verdict(f(units=a), f(units=b))[0] for a, b in
                          ((12, 14), (12, 15), (12, 16), (16, 12), (1, 3), (1, 4), (40, 31), (40, 29))],
                         [0, 0, 0, 0, 0, 1, 0, -1])
        # a figure not read in either game is no milestone of either
        self.assertEqual(opening.verdict(f(order=None, mex=None), f(order=44, mex=5279))[0], 0)
        self.assertEqual(opening.verdict(f(units=None), f(units=30))[0], 0)


if __name__ == "__main__":
    unittest.main()
