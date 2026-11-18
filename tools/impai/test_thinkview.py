#!/usr/bin/env python3
"""test_thinkview -- thinkview.py offline: the page's data built from a small fake of the recording reader.

    python3 tools/impai/test_thinkview.py

THE TREE is test_think's shape with the members the view reads (a squad's goal and target, the judgements, the
anchor, a plant's way out, a job's site, a known row's block). THE GAME is one squad's known task history -- FORM
at 100, ATTACK at 200 with another goal at 260, RETREAT on a lost prediction at 300, FREE at 350 -- read through
test_think's fake of think.stream, and a fake of thinkview.sample stands for the unit images. The tests that
need the DLL's compiler are skipped where it is not installed; the lanes', the runs' and the pressure's
arithmetic need none.

THE GROUND's tests (thinkmap.py) read no file of the game: THE MAP is a TNT made here, 4 x 3 tiles of one colour
each, loose in a game directory of the test's own beside a palette, a feature section and a unit's FBI, and THE
GRID is a feature grid of 8 x 6 cells written cell by cell. The view's arithmetic is the page's own block
(thinkview.html, between its VIEW marks), evaluated by node where node is installed."""
import array
import base64
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request
import zlib
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_think  # noqa: E402
import think  # noqa: E402
import thinklayout  # noqa: E402
import thinklib  # noqa: E402
import thinkmap  # noqa: E402
import thinkserve  # noqa: E402
import thinkview  # noqa: E402

try:
    import numpy
    from PIL import Image
except ImportError:
    numpy = Image = None

HEADER = """#ifndef IMPAI_H
#define IMPAI_H
#include <stdint.h>
#include "worldq.h"
#define IMPAI_OWN 0x01u
#define IMPAI_LOS 0x02u
#define IMPAI_RADAR 0x08u
#define IMPAI_GHOST 0x20u
#define IMPAI_GRID_MAX 8
#define IMPAI_NO_ANCHOR (-32768)
enum { IMPAI_SQ_FREE, IMPAI_SQ_FORM, IMPAI_SQ_ATTACK, IMPAI_SQ_RETREAT, IMPAI_SQ_TASKS };
typedef enum { TASIM_LAYER_LAND, TASIM_LAYER_SEA, TASIM_LAYER_AIR, TASIM_LAYERS } TASIM_LAYER;
enum { EST_START, EST_STRUCT, EST_KEPT };
typedef struct IMPAI_SQUAD { uint8_t task, layer, n, flags; int16_t tx, tz; uint16_t target, pad; uint32_t since;
    float value; } IMPAI_SQUAD;
typedef struct IMPAI_MIL { int16_t ebx, ebz; uint32_t wave_t, lost_t, press_until; uint16_t waves[3]; int8_t naval;
    uint8_t est_seen, dry, lead, island, pad; IMPAI_SQUAD sq[4]; } IMPAI_MIL;
typedef struct IMPAI_WAYREC { uint16_t type; int16_t cx, cz; } IMPAI_WAYREC;
typedef struct IMPAI_BRAIN { int16_t ax, az; uint8_t nway, opened; IMPAI_WAYREC way[2];
    uint8_t held[IMPAI_GRID_MAX * IMPAI_GRID_MAX / 8]; } IMPAI_BRAIN;
enum { IMPAI_JOB_NONE, IMPAI_JOB_BUILD, IMPAI_JOB_SQUAD };
typedef struct IMPAI_JOB { uint16_t type, build, target; int16_t cx, cz; uint8_t kind, squad; } IMPAI_JOB;
typedef struct IMPAI_SEEN { uint16_t type; uint8_t how, owner; uint16_t bx, bz; } IMPAI_SEEN;
typedef struct IMPAI_ROW { IMPAI_SEEN seat[WQ_SEATS]; } IMPAI_ROW;
#endif
"""
SLOTS, EACH = test_think.SLOTS, test_think.EACH
NAMES = ["None", "A", "B", "C", "BUILDER", "D", "TOWER", "PLANT", "BOAT", "E"]


def episode(squad, task, t0, t1, n=(1, 1), legs=None):
    return {"squad": squad, "task": task, "layer": "SEA", "from": t0, "to": t1, "n": n, "value": 10.0,
            "legs": legs or [(t0, (0, 0), None)]}


def fake_sample(ticks):
    """thinkview.sample's answer: a structure of the seat's at (100, 200), its boat sailing east a px a tick, and
    the opponent's tower until tick 350."""
    return (640, 480), {t: {"units": [(11, 100, 200, 1, 0), (12, 50 + t, 60, 0, 0)]
                            + ([(21, 300, 300, 1, 0)] if t < 350 else []),
                            "metal": {0: 0, 1: 5.0, 2: 1.0}} for t in ticks}


class Arithmetic(unittest.TestCase):
    def test_a_lane_a_squad_a_span_a_task(self):
        eps = [episode(1, "FORM", 100, 200), episode(0, "SCOUT", 150, None), episode(1, "ATTACK", 200, 300),
               episode(1, "RETREAT", 300, 350), episode(2, "FORM", 420, None)]
        self.assertEqual(thinkview.lanes(eps, 400), [
            {"k": 0, "spans": [[150, 400, "SCOUT", 1]]},                # a task still held ends at the end
            {"k": 1, "spans": [[100, 200, "FORM", 0], [200, 300, "ATTACK", 2], [300, 350, "RETREAT", 3]]}])
        self.assertEqual(thinkview.lanes(eps, 250)[1]["spans"][-1], [200, 250, "ATTACK", 2])    # cut at the end
        self.assertEqual(thinkview.lanes([], 400), [])

    def test_a_track_reads_and_runs(self):
        tr = [[1, 31, 90], [-1, 1, 1]]
        self.assertEqual([thinkview.at(tr, t) for t in (0, 1, 30, 31, 500)], [0, -1, -1, 1, 1])
        self.assertEqual(thinkview.at(None, 5, 7), 7)
        self.assertEqual(thinkview.runs(tr, 100), [[0, 1, 0], [1, 31, -1], [31, 100, 1]])
        self.assertEqual(thinkview.runs(tr, 20), [[0, 1, 0], [1, 20, -1]])
        self.assertEqual(thinkview.runs(None, 50), [[0, 50, 0]])

    def test_the_pressure_holds_to_its_stamp(self):
        # stamps that meet are one span; one written past the end, or naming a time gone by, is none
        self.assertEqual(thinkview.pressure_spans([[100, 130, 300, 900], [140, 180, 0, 950]], 500), [[100, 181]])
        self.assertEqual(thinkview.pressure_spans([[100, 400], [140, 460]], 420), [[100, 141], [400, 420]])

    def test_a_judgements_lane_says_what_holds(self):
        trk = {"mil.naval": [[1, 31], [-1, 1]], "mil.est_seen": [[200], [1]], "mil.press_until": [[300], [340]]}
        enums = {"mil.est_seen": {"0": "START", "1": "STRUCT", "2": "KEPT"}}
        got = {j["id"]: j["spans"] for j in thinkview.judgement_lanes(trk, enums, 400)}
        self.assertEqual([s[:2] + s[3:] for s in got["naval"]], [[1, 31, "muted"], [31, 400, "info"]])
        self.assertEqual([s[:2] + s[3:] for s in got["estimate"]], [[0, 200, "muted"], [200, 400, "enemy"]])
        self.assertEqual([s[:2] for s in got["pressure"]], [[300, 341]])
        self.assertNotIn("lead", got)                           # a field the recording does not have: no lane

    def test_the_page_carries_its_recordings(self):
        html = thinkview.page([{"id": "a", "name": "</script>", "seats": [{"seat": 1, "id": "a", "file": "a.js"}]}])
        self.assertIn('[{"id":"a","name":"<\\/script>","seats":[{"seat":1,"id":"a","file":"a.js"}]}]', html)
        self.assertTrue(html.startswith("<!doctype html>"))
        self.assertFalse(thinkview.page([], bare=True).startswith("<!doctype"))
        self.assertTrue(thinkview.data_file("a", {"v": 1}).startswith('thinkviewData("a",{"v":1});'))

    def test_the_transport_and_the_seats_stand_between_the_two_halves(self):
        html = thinkview.TEMPLATE.read_text()
        at = lambda mark: html.index(mark)                                  # noqa: E731
        deck, end = at('<div class="deck"'), at('<div class="now"')
        self.assertLess(at('<canvas id="rec"'), deck)                       # under the flight recorder
        self.assertLess(end, at('<canvas id="map"'))                        # above the war room
        for ident in ("prev", "play", "next", "speed", "nowtick", "nowclock", "seats"):
            self.assertTrue(deck < at(f'id="{ident}"') < end, ident)
            self.assertEqual(html.count(f'id="{ident}"'), 1, ident)
        self.assertNotIn("The seat's", html[at("function drawSide"):at("/* ---------- one clock")])   # a heading names its seat

    def test_a_place_is_a_compass_word_over_the_maps_thirds(self):
        w, h = 600, 300                                             # wider than tall: a swapped axis shows
        for (x, z), word in (((0, 0), "north-west"), ((300, 0), "north"), ((599, 99), "north-east"),
                             ((199, 150), "west"), ((200, 100), "centre"), ((400, 199), "east"),
                             ((100, 200), "south-west"), ((399, 299), "south"), ((599, 299), "south-east")):
            self.assertEqual(thinkview.compass(x, z, w, h), word, (x, z))

    def test_a_colour_has_a_word(self):
        words = [thinkview.colour_word(c) for c in thinkview.STOCK_RGB]
        self.assertEqual(words, ["blue", "red", "white", "green", "dark blue", "purple", "yellow", "black",
                                 "light blue", "pale yellow"])
        self.assertEqual(len(thinkview.STOCK_RGB), 10)              # a colour an index the game's seats may hold


@unittest.skipUnless(test_think.HAVE_CC, "the DLL's compiler is not installed")
class Fixture(unittest.TestCase):
    """THE TREE and THE GAME, for the classes that read them."""

    @classmethod
    def setUpClass(cls):
        cls.root = Path(tempfile.mkdtemp())
        with mock.patch.object(test_think, "HEADER", HEADER):
            cls.tree = test_think.make_tree(cls.root / "tree")
        (cls.tree / "tagpu" / "ddraw" / "src" / "impai_brain.c").write_text("static PLAN p = { .base_r = 96, };\n")
        cls.cache = cls.root / "cache"
        cls.lay = think.Layout(thinklayout.load(cls.tree, cls.cache), cls.tree)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, True)

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        (self.tmp / "content.json").write_text(json.dumps({"names": NAMES}))
        for p in (mock.patch.object(think, "stream", test_think.fake_stream),
                  mock.patch.object(think, "TREE", self.tree), mock.patch.object(think, "CACHE", self.cache),
                  mock.patch.object(think, "CONTENT", self.tmp / "content.json"),
                  mock.patch.object(thinkview, "sample", lambda _rec, ticks, *_a, **_kw: fake_sample(ticks))):
            p.start()
            self.addCleanup(p.stop)

    def put(self, part, row, name, value):
        f = next(f for f in self.lay.fields(part) if f.name == name)
        return ["span", part, row * self.lay.size[part] + f.off, f.st.pack(value).hex()]

    def game(self, name="a.imprec", defs=len(NAMES)):
        """THE GAME, seat 1's: unit images' type IDs are a type index + 1."""
        m, b, j, k = (lambda n, v: self.put("mil", 1, n, v)), (lambda n, v: self.put("brain", 1, n, v)), \
            (lambda n, v: self.put("job", 12, n, v)), (lambda n, v: self.put("known", 21, f"seat[1].{n}", v))
        ev = [["tick", 0]] + [["size", p, self.lay.size[p] * (10 if p in think.BY_SEAT else SLOTS)] for p in think.PARTS]
        ev += [["unit", 11, 8], ["unit", 12, 9], ["unit", 21, 7],
               ["tick", 1], m("naval", -1), b("ax", 10), b("az", 20),
               ["tick", 31], m("naval", 1), m("island", 1),
               ["tick", 100], m("sq[1].task", 1), m("sq[1].layer", 1), m("sq[1].n", 2), m("sq[1].target", 0xFFFF),
               j("type", 8), j("kind", 2), j("squad", 1), j("build", 0xFFFF), j("target", 0xFFFF),
               b("nway", 1), b("way[0].type", 7), b("way[0].cx", 4), b("way[0].cz", 8),
               ["tick", 200], m("sq[1].task", 2), m("sq[1].tx", 14), m("sq[1].tz", 36), m("sq[1].target", 21),
               m("sq[1].value", 250.5), m("wave_t", 199), m("waves[1]", 1), m("est_seen", 1), m("ebx", 14),
               m("ebz", 36), k("type", 6), k("how", 0x0A), k("owner", 2), k("bx", 14), k("bz", 36), b("held[1]", 5),
               ["tick", 260], m("sq[1].tx", 15),
               ["tick", 300], m("sq[1].task", 3), m("sq[1].tx", 3), m("sq[1].tz", 4), m("sq[1].target", 0xFFFF),
               m("lost_t", 299), m("press_until", 340), k("how", 0x20),
               ["tick", 350], m("sq[1].task", 0), m("sq[1].n", 0), ["gone", 21],
               ["tick", 400]]
        head = {"build": "abc1234", "map": "Test Map", "slots": SLOTS, "units_each": EACH, "unitdefs": [defs, 0],
                "players": [{"index": 0, "name": "Player", "ai": 0, "side": 0},
                            {"index": 1, "name": "Core", "ai": 1, "side": 1},
                            {"index": 2, "name": "Arm", "ai": 0, "side": 0}],
                "stride": {p: self.lay.size[p] for p in think.PARTS}}
        f = self.tmp / name
        f.write_text(json.dumps({"head": head, "events": ev}))
        return f

    def build(self, **kw):
        ex = think.extract(self.game())
        return thinkview.build(ex, self.lay, 1, fake_sample, think.type_names(ex), **kw)


class View(Fixture):
    def test_who_plays_is_the_headers_rows_and_the_first_places(self):
        ex = think.extract(self.game())
        row = lambda ctl, logo, allies: {"controller": ctl, "logo": logo, "alliances": bytes(allies)}     # noqa: E731
        rows = {0: row(1, 4, [1, 1, 0]), 1: row(2, 7, [1, 1, 1]), 2: row(2, 12, [0, 0, 1])}
        dims, world = fake_sample([0, 100, 200])
        colours = {"rgb": [(i, 0, 255 - i) for i in range(10)]}
        # seat 1's first unit, slot 11, is read at its first tick; seat 2's, slot 21, only at a sample
        who = thinkview.seat_facts(ex, rows, colours, world, {11: [(0, 620, 20), (1, 630, 20)]}, dims)
        self.assertEqual([(w["seat"], w["kind"], w["by"], w["side"]) for w in who],
                         [(0, "human", "human", "Arm"), (1, "impai", "ImpAI", "Core"), (2, "ai", "another AI", "Arm")])
        self.assertEqual([(w["logo"], w["rgb"]) for w in who], [(4, [4, 0, 251]), (7, [7, 0, 248]), (12, None)])
        self.assertEqual(who[2]["colour"], None)                    # a colour index past the game's colours: not guessed
        # an alliance is the byte each holds of the other: seat 1 holds seat 2's, seat 2 does not hold seat 1's
        self.assertEqual([w["allies"] for w in who], [[1], [0], []])
        self.assertEqual([(w["start"], w["start_t"], w["start_exact"], w["where"]) for w in who],
                         [(None, None, None, None), ([620, 20], 0, True, "north-east"), ([300, 300], 0, False, "centre")])
        # the stream not read: every seat is still a row, and what was not read is None
        bare = thinkview.seat_facts(ex, {}, None, {}, {}, dims)
        self.assertEqual([(w["seat"], w["name"], w["kind"], w["logo"], w["rgb"], w["allies"], w["start"]) for w in bare],
                         [(0, "Player", None, None, None, None, None), (1, "Core", "impai", None, None, None, None),
                          (2, "Arm", None, None, None, None, None)])
        self.assertEqual((thinkview.allies_of(rows, 2), thinkview.allies_of({}, 2)), ([], None))
        data = self.build()
        self.assertEqual([w["start"] for w in data["who"]], [None, [100, 200], [300, 300]])
        self.assertEqual(data["colours"]["from"], None)             # no ground: the unmodded game's table, and said so

    def test_an_allys_row_is_no_enemys(self):
        """THE GAME's one known row is seat 2's unit, slot 21: in sight from tick 200, a ghost from tick 300."""
        row = lambda allies: {"controller": 2, "logo": 0, "alliances": bytes(allies)}     # noqa: E731
        contact = lambda d: [e[0] for e in d["events"] if e[1] == "contact"]              # noqa: E731
        foes = self.build(rows={1: row([0, 1, 0]), 2: row([0, 0, 1])})
        self.assertEqual((foes["known"]["now"], foes["known"]["ghost"]), ([[200, 300], [1, 0]], [[300], [1]]))
        self.assertEqual((contact(foes), 200 in foes["decisions"]), ([200], True))
        # allied, each holding the other's byte: the row is in no count, and no contact falls
        friends = self.build(rows={1: row([0, 1, 1]), 2: row([0, 1, 1])})
        self.assertEqual(friends["known"], {"now": [[], []], "pass": [[], []], "ghost": [[], []]})
        self.assertEqual(contact(friends), [])
        self.assertEqual(len(friends["krows"]), len(foes["krows"]))     # the row itself is still the page's to draw
        self.assertEqual(friends["who"][1]["allies"], [2])
        # an alliance one seat alone holds is none, and alliances unread count every other seat's row
        self.assertEqual(self.build(rows={1: row([0, 1, 1]), 2: row([0, 0, 1])})["known"], foes["known"])
        # WHOSE A ROW IS: an ally's unit the table holds under an owner it does not name is the ally's by its slot
        with mock.patch.object(think.Extract, "seat_of", lambda _ex, slot: 0 if slot == 21 else 1):
            self.assertEqual(self.build(rows={0: row([1, 1, 0]), 1: row([1, 1, 0])})["known"], friends["known"])
        unread = self.build()
        self.assertEqual((unread["known"], contact(unread), unread["who"][1]["allies"]), (foes["known"], [200], None))

    def test_the_lanes_are_the_task_history(self):
        d = self.build()
        self.assertEqual(d["lanes"], [{"k": 1, "spans": [[100, 200, "FORM", 0], [200, 300, "ATTACK", 1],
                                                           [300, 350, "RETREAT", 2]]}])
        self.assertEqual([(e["task"], e["from"], e["to"], e["n"]) for e in d["episodes"]],
                         [("FORM", 100, 200, [2, 2]), ("ATTACK", 200, 300, [2, 2]), ("RETREAT", 300, 350, [2, 2])])
        self.assertEqual(d["episodes"][1]["legs"], [[200, 14, 36, 21], [260, 15, 36, 21]])
        j = {x["id"]: [s[:2] for s in x["spans"]] for x in d["judgements"]}
        self.assertEqual((j["naval"], j["island"], j["pressure"], j["dry"]),
                         ([[1, 31], [31, 400]], [[31, 400]], [[300, 341]], []))

    def test_a_moment_is_exact_between_two_samples(self):
        d = self.build()
        trk = d["tracks"]
        tasks = d["enums"]["mil.sq.task"]
        self.assertEqual([tasks[str(thinkview.at(trk["mil.sq[1].task"], t))] for t in (99, 100, 259, 299, 300, 399)],
                         ["FREE", "FORM", "ATTACK", "ATTACK", "RETREAT", "FREE"])
        self.assertEqual([thinkview.at(trk["mil.sq[1].tx"], t) for t in (259, 260, 300)], [14, 15, 3])
        self.assertEqual((thinkview.at(trk["mil.sq[1].value"], 250), thinkview.at(trk["mil.ebx"], 250),
                          thinkview.at(trk["brain.ax"], 250), thinkview.at(trk["mil.naval"], 250)), (250.5, 14, 10, 1))
        self.assertEqual(d["macros"]["WQ_NONE"], 0xFFFF)
        self.assertEqual((d["base_r"], d["px"], d["units"]["held_row"]), (96, [640, 480], 8))

    def test_a_sample_holds_the_seats_side_and_the_truth(self):
        d = self.build()
        fr = {f["t"]: f for f in d["frames"]}
        for t in (100, 200, 260, 300, 350):                     # every decision tick is a sample
            self.assertIn(t, fr)
        f = fr[200]
        jobs = lambda fr_: [d["jrows"][i] for i in fr_["j"]]                    # a row is written once, a sample names it
        known = lambda fr_: [d["krows"][i] for i in fr_["k"]]
        self.assertEqual(jobs(f), [[12, 2, 0xFFFF, 0, 0, 1, 0xFFFF]])           # the boat, in squad 1
        self.assertEqual(known(f), [[21, 6, 0x0A, 2, 14, 36]])                  # the tower, in sight and on radar
        self.assertEqual(fr[260]["k"], f["k"])                                  # the same row at the next sample
        held = set()                                                            # held ground: steps from the one before
        for add, rem in d["helds"][:f["h"] + 1]:
            held = (held - set(rem)) | set(add)
        self.assertEqual(sorted(held), [8, 10])                                 # held[1] = 0b101: cells 8 and 10
        self.assertEqual(known(fr[300])[0][2], 0x20)                            # a ghost by then
        self.assertEqual(fr[90]["k"], [])                                       # not known yet: the truth alone
        lives = {life[0]: life for life in d["lives"]}
        self.assertEqual(lives[11][1:7], [7, 0, None, 1, 100, 200])             # a structure: one place for its life
        self.assertEqual(lives[21][1:4], [6, 0, 350])                           # the tower's life ends at 350
        boat = d["lives"].index(lives[12])
        self.assertEqual(f["m"], [boat, 250, 60])                               # a mobile unit: a place a sample
        self.assertEqual((f["a"], f["i"]), ([0, 2, 1], [0, 5.0, 1.0]))
        self.assertEqual(fr[390]["a"], [0, 2, 0])
        self.assertEqual({k: d["names"][k] for k in ("6", "7", "8")}, {"6": "TOWER", "7": "PLANT", "8": "BOAT"})
        self.assertEqual(d["known"]["now"], [[200, 300], [1, 0]])
        self.assertEqual(d["known"]["ghost"], [[300], [1]])

    def test_the_events_are_the_decisions(self):
        d = self.build()
        ev = {(t, kind): text for t, kind, text, _k in d["events"]}
        self.assertIn("sq[1], 2 units", ev[(200, "wave")])
        self.assertEqual(ev[(300, "lost")], "a prediction is lost: sq[1] retreats")
        self.assertTrue(ev[(300, "task")].endswith("on a lost prediction"))
        self.assertIn("slot 21 of seat 2", ev[(200, "task")])
        self.assertEqual(ev[(260, "leg")], "sq[1] ATTACK goes for (15,36) slot 21")
        self.assertTrue(ev[(100, "plant")].startswith("PLANT sited at block (2,4)"))
        self.assertEqual(ev[(200, "contact")], "the first enemy row: the seat knows of an enemy")
        self.assertNotIn("sight", {kind for _t, kind in ev})    # the first contact is no second event
        self.assertEqual(d["decisions"], sorted({e[0] for e in d["events"]}))
        self.assertEqual([e[0] for e in d["events"]], sorted(e[0] for e in d["events"]))

    def test_a_data_file_is_held_to_its_bound(self):
        full = self.build(step=10)
        self.assertEqual(len(full["frames"]), len(set(range(0, 401, 10)) | set(full["decisions"])))
        self.assertEqual(full["bytes"], len(json.dumps({k: v for k, v in full.items() if k != "bytes"},
                                                       separators=(",", ":"))))
        small = self.build(step=10, budget=full["bytes"] - 600)
        self.assertLessEqual(small["bytes"], full["bytes"] - 600)
        self.assertLess(len(small["frames"]), len(full["frames"]))
        self.assertGreater(small["step"], 10)
        kept = {f["t"] for f in small["frames"]}
        self.assertTrue(set(full["decisions"]) | {0, 400} <= kept)              # the decision ticks stay
        self.assertEqual((small["lanes"], small["tracks"], small["events"]),    # and what is exact is untouched
                         (full["lanes"], full["tracks"], full["events"]))
        with self.assertRaises(thinkview.ViewError) as e:
            self.build(step=10, budget=2000)
        self.assertIn("past --max-bytes 2000", str(e.exception))

    def test_the_result_beside_a_recording_ends_the_game(self):
        rec = self.game()
        rec.with_suffix(".json").write_text(json.dumps({"end_tick": 320, "outcome": "A", "seats": {"A": {"seat": 1}}}))
        ex = think.extract(rec)
        d = thinkview.build(ex, self.lay, 1, fake_sample, [], thinkview.result_of(rec))
        self.assertEqual((d["end"], d["last"], d["result"]["winner"], d["named"]), (320, 400, 1, False))
        self.assertEqual(d["lanes"][0]["spans"][-1], [300, 320, "RETREAT", 2])
        self.assertTrue(all(f["t"] <= 320 for f in d["frames"]))
        self.assertEqual(thinkview.result_of(self.tmp / "none.imprec"), {})

    def test_names_fit_by_the_count_of_definitions(self):
        self.assertEqual(think.type_names(think.extract(self.game())), NAMES)
        self.assertEqual(think.type_names(think.extract(self.game("b.imprec", defs=11))), [])
        self.lay.name_types(NAMES)
        self.addCleanup(self.lay.name_types, [])
        f = {x.name: x for x in self.lay.fields("job")}
        self.assertEqual((f["type"].show(b"\x08\0"), f["type"].show(b"\0\0"), f["type"].show(b"\xff\xff")),
                         ("BOAT(8)", "0", "65535"))

    def test_the_tree_at_the_recordings_build_reads_it(self):
        import copy
        ex = think.extract(self.game())                                     # build abc1234
        here, one, two = (copy.copy(self.lay) for _ in range(3))            # this tree and two --tree, all of sizes that fit
        here.tree, one.tree, two.tree = "here", "one", "two"
        ids = {"here": "0000aaaa", "one": "1111bbbb", "two": "abc1234"}
        self.assertEqual(thinkview.layout_for(ex, [here, one, two], dict(ids)), (two, {"build": "abc1234", "exact": True}))
        self.assertEqual(thinkview.layout_for(ex, [here, one, two], dict(ids, two="")),
                         (one, {"build": "1111bbbb", "exact": False}))      # none at its build: the first --tree
        self.assertEqual(thinkview.layout_for(ex, [here], dict(ids))[0], here)      # no --tree: this tree
        ex.build = "abc1234-dirty"                                          # edits no commit holds: never exact
        self.assertEqual(thinkview.layout_for(ex, [here, one, two], dict(ids)), (two, {"build": "abc1234", "exact": False}))

    def test_the_command_writes_the_page_and_a_file_a_recording(self):
        import contextlib
        import io
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = thinkview.main([str(self.game()), str(self.game("b c.imprec")), "--out", str(self.tmp / "page")])
        self.assertEqual(rc, 0, out.getvalue())
        html = (self.tmp / "page" / "index.html").read_text()
        mark = "const MANIFEST = "
        games = json.JSONDecoder().raw_decode(html[html.index(mark) + len(mark):])[0]
        self.assertEqual([(g["id"], g["name"], g["map"], g["end"], g["seats"]) for g in games],
                         [("a", "a.imprec", "Test Map", 400, [{"seat": 1, "id": "a", "file": "a.js"}]),
                          ("b-c", "b c.imprec", "Test Map", 400, [{"seat": 1, "id": "b-c", "file": "b-c.js"}])])
        self.assertEqual([(w["seat"], w["name"], w["kind"]) for w in games[0]["who"]],     # every seat, played by ImpAI or not
                         [(0, "Player", None), (1, "Core", "impai"), (2, "Arm", None)])
        js = (self.tmp / "page" / "a.js").read_text()
        self.assertTrue(js.startswith('thinkviewData("a",{'))
        self.assertEqual(json.loads(js[len('thinkviewData("a",'):-3])["lanes"][0]["k"], 1)
        self.assertIsNone(games[0]["dense"])                               # no --dense: the page says it has none
        self.assertEqual(sorted(f.name for f in (self.tmp / "page").iterdir()), ["a.js", "b-c.js", "index.html"])
        with mock.patch.object(thinkview, "sample", lambda _rec, ticks, *_a, **_kw: dense_sample(ticks)), \
                contextlib.redirect_stdout(out):
            self.assertEqual(thinkview.main([str(self.game()), "--out", str(self.tmp / "dense"), "--dense", "10"]), 0)
        html = (self.tmp / "dense" / "index.html").read_text()
        game = json.JSONDecoder().raw_decode(html[html.index(mark) + len(mark):])[0][0]
        self.assertEqual(game["dense"], {"k": 10, "per": 409, "n": 1, "files": ["a.dense-0.js"]})
        chunk = (self.tmp / "dense" / "a.dense-0.js").read_text()
        self.assertTrue(chunk.startswith('thinkviewDense("a",0,"'))
        k, s0, ns, runs = unchunk(base64.b64decode(chunk.split('"')[3]))
        self.assertEqual((k, s0, ns, [r[0] for r in runs]), (10, 0, 41, [1, 2, 2]))
        with contextlib.redirect_stdout(out):
            self.assertEqual(thinkview.main([str(self.tmp / "nothing.imprec"), "--out", str(self.tmp / "p2")]), 2)
        self.assertIn("COULD NOT RUN", out.getvalue())
        self.assertFalse((self.tmp / "p2").exists())


def dense_sample(ticks):
    """fake_sample with THE DENSE TRACKS a pass at K = 10 would keep of THE GAME's 400 ticks: the boat (life 1)
    a px a tick with one leap of 300 px at sample 20, and a unit (life 2) the stream holds over samples 3..5 and
    again from 9."""
    xs = [50 + 10 * j + (300 if j >= 20 else 0) for j in range(41)]
    tracks = {1: [[0, array.array("h", xs), array.array("h", [60] * 41)]],
              2: [[3, array.array("h", [7, 8, 9]), array.array("h", [1, 1, 1])],
                  [9, array.array("h", [-5] * 32), array.array("h", [2] * 32)]]}
    return fake_sample(ticks) + ({"track": None, "first": {}, "archives": None, "map": "Test Map", "dense": tracks},)


def unchunk(blob: bytes) -> tuple:
    """A chunk of THE DENSE TRACKS read back by its layout: (K, its first sample, its samples, [(life, the run's
    first sample from the chunk's, [x, ...], [z, ...]), ...])."""
    raw = zlib.decompress(blob)
    magic, k, _zero, s0, ns, n = struct.unpack_from("<4sHHIII", raw)
    assert magic == b"TVD1"
    at, runs = 20, []
    for _ in range(n):
        life, off, m, x, z = struct.unpack_from("<IHHhh", raw, at)
        at += 12
        xs, zs = [x], [z]
        for _j in range(m - 1):
            dx, dz = struct.unpack_from("<bb", raw, at)
            at += 2
            if dx == -128:
                x, z = struct.unpack_from("<hh", raw, at)
                at += 4
            else:
                x, z = x + dx, z + dz
            xs.append(x)
            zs.append(z)
        runs.append((life, off, xs, zs))
    assert at == len(raw)
    return k, s0, ns, runs


class Dense(unittest.TestCase):
    def setUp(self):
        self.tracks = dense_sample([])[2]["dense"]

    def test_a_chunk_holds_its_stretch_and_the_next_chunks_first_sample(self):
        info, chunks = thinkview.dense_chunks(self.tracks, 10, 400, chunk_ticks=160)
        self.assertEqual(info, {"k": 10, "per": 16, "n": 3})                # 41 samples, a chunk starts every 16
        got = [unchunk(c) for c in chunks]
        self.assertEqual([(k, s0, ns) for k, s0, ns, _r in got], [(10, 0, 17), (10, 16, 17), (10, 32, 9)])
        boat = self.tracks[1][0][1]
        for (_k, s0, ns, runs) in got:                                      # the boat: every sample of each chunk
            life, off, xs, zs = runs[0]
            self.assertEqual((life, off, xs, zs), (1, 0, list(boat[s0:s0 + ns]), [60] * ns))
        self.assertEqual(got[1][3][0][2][4] - got[1][3][0][2][3], 310)      # the leap past 127 px is kept whole
        # a unit the stream lost and held again is two runs, each where it begins in its chunk
        self.assertEqual([(life, off, len(xs)) for life, off, xs, _z in got[0][3][1:]], [(2, 3, 3), (2, 9, 8)])
        self.assertEqual([(life, off, xs[0]) for life, off, xs, _z in got[1][3][1:]], [(2, 0, -5)])
        self.assertEqual(thinkview.dense_chunks({}, 10, 0)[0]["n"], 1)      # a game of no tick: one empty chunk

    @unittest.skipUnless(shutil.which("node"), "node, which evaluates the page's own block")
    def test_the_page_reads_a_chunk_as_it_was_written(self):
        _info, chunks = thinkview.dense_chunks(self.tracks, 10, 400, chunk_ticks=160)
        raw = base64.b64encode(zlib.decompress(chunks[0])).decode()
        got = view(f"(() => {{ const b = Buffer.from('{raw}', 'base64'), c = denseParse(b.buffer.slice(b.byteOffset, b.byteOffset + b.length));"
                   " return [c.s0, c.runs.map(r => [r.life, r.off, r.n, Array.from(r.xs), Array.from(r.zs)])]; })()", "DENSE")
        _k, s0, _ns, runs = unchunk(chunks[0])
        self.assertEqual(got, [s0, [[life, off, len(xs), xs, zs] for life, off, xs, zs in runs]])

    def test_a_chunks_file_is_one_call_of_the_pages(self):
        text = thinkview.dense_file("a b", 3, b"\x00\x01\xff")
        self.assertEqual(text, 'thinkviewDense("a b",3,"AAH/");\n')


class Library(unittest.TestCase):
    """thinklib.py: a folder a label, a hard link a recording, no index."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="test_thinklib."))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.lib, self.runs = self.tmp / "library", self.tmp / "runs"
        self.runs.mkdir()
        self.said = []

    def rec(self, name="g1.imprec", body=b"recording one", result=None, log=False) -> Path:
        f = self.runs / name
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(body)
        if result is not None:
            f.with_suffix(".json").write_text(json.dumps(result))
        if log:
            f.with_suffix(".tagpu.log").write_text("impai: why\n")
        return f

    def add(self, recs, label, **kw):
        return thinklib.add(recs, label, library=self.lib, say=self.said.append, **kw)

    def test_a_label_is_one_path_component(self):
        self.assertEqual(thinklib.label_name("  item19 \n try1 "), "item19 try1")
        self.assertEqual(thinklib.label_name("tier @39562add"), "tier @39562add")
        self.assertEqual(thinklib.label_name("a/../b\\c\x00d"), "a-..-b-c-d")
        for bad in ("", "  ", ".", "..", ".cache", ".hidden/", "../../outside"):
            with self.assertRaises(thinklib.LibraryError, msg=bad):
                thinklib.label_name(bad)
        got = self.add([self.rec()], "exp/../../outside")
        self.assertEqual(got[0].parent.parent, self.lib)                    # in the library, whatever the label
        self.assertEqual([p.name for p in self.lib.iterdir()], ["exp-..-..-outside"])

    def test_a_recording_is_added_as_a_hard_link_with_its_sidecar(self):
        src = self.rec(result={"id": "g1", "end_tick": 320, "outcome": "A", "seats": {"A": {"seat": 1}}, "setup": None}, log=True)
        dest, = self.add([src], "exp one", note="why it was kept", tool="session.py", command="session.py --label x", run="r7")
        self.assertTrue(dest.samefile(src))
        self.assertEqual(os.stat(src).st_nlink, 2)
        self.assertTrue(dest.with_suffix(".tagpu.log").samefile(src.with_suffix(".tagpu.log")))
        side = thinklib.sidecar(dest)
        self.assertEqual({k: side[k] for k in ("label", "how", "tool", "command", "run", "game", "note", "source")},
                         {"label": "exp one", "how": "link", "tool": "session.py", "command": "session.py --label x",
                          "run": "r7", "game": "g1", "note": "why it was kept", "source": str(src.resolve())})
        self.assertEqual(thinkview.result_of(dest), {"end_tick": 320, "outcome": "A", "winner": 1, "cap": None})
        self.assertEqual(thinklib.sidecar(src), {})                         # a harness's own result is no sidecar
        # the same recording again is left as it is; another of its name gets the next free one
        self.assertEqual(self.add([src], "exp one"), [dest])
        other = self.rec("sub/g1.imprec", b"another recording, longer")
        self.assertEqual([p.name for p in self.add([other], "exp one")], ["g1-2.imprec"])
        rows = thinklib.labels(self.lib)
        self.assertEqual([(r["label"], r["n"], r["bytes"]) for r in rows], [("exp one", 2, len(b"recording one") + len(b"another recording, longer"))])
        with self.assertRaises(thinklib.LibraryError):
            self.add([self.runs / "nothing.imprec"], "exp one")

    def test_where_no_link_can_be_made_it_is_a_copy_and_said(self):
        src = self.rec()
        with mock.patch.object(os, "link", side_effect=OSError(18, "Invalid cross-device link")):
            dest, = self.add([src], "far")
        self.assertFalse(dest.samefile(src))
        self.assertEqual((dest.read_bytes(), thinklib.sidecar(dest)["how"]), (src.read_bytes(), "copy"))
        self.assertTrue(any("copied" in line for line in self.said))

    def test_drop_removes_names_and_never_the_last_one(self):
        linked, moved = self.rec("a.imprec", b"aaa"), self.rec("b.imprec", b"bbbb")
        with mock.patch.object(os, "link", side_effect=OSError(18, "x")):
            copied, = self.add([self.rec("c.imprec", b"ccccc")], "exp")
        self.add([linked], "exp")
        self.add([moved], "exp", move=True)
        self.assertFalse(moved.exists())                                    # moved in: the label holds the only copy
        gone, kept = thinklib.drop("exp", self.lib, self.said.append)
        self.assertEqual((sorted(gone), kept), (["a.imprec", "c.imprec"], ["b.imprec"]))
        self.assertEqual(linked.read_bytes(), b"aaa")                       # a source is never touched
        self.assertEqual(sorted(p.name for p in (self.lib / "exp").iterdir()), ["b.imprec", "b.json"])
        self.assertTrue(any("only copy" in line for line in self.said))
        with self.assertRaises(thinklib.LibraryError):
            thinklib.drop("no such label", self.lib)
        # a link whose source is gone since is the only copy too
        self.add([linked], "exp2")
        linked.unlink()
        self.assertEqual(thinklib.drop("exp2", self.lib, self.said.append), ([], ["a.imprec"]))

    def test_the_run_tools_label_adds_what_a_run_kept_and_never_fails_it(self):
        self.rec("base/g1.imprec")
        self.rec("mod/g2.imprec", b"two")
        got = thinklib.add_kept([self.runs], "tier @abc", tool="batch.py", command="batch.py --label", library=self.lib,
                                say=self.said.append)
        self.assertEqual(sorted(p.name for p in got), ["g1.imprec", "g2.imprec"])
        self.assertEqual(thinklib.sidecar(got[0])["tool"], "batch.py")
        self.assertEqual(thinklib.add_kept([self.tmp / "empty"], "x", library=self.lib, say=self.said.append), [])
        (self.tmp / "file").write_text("no folder")
        self.assertEqual(thinklib.add_kept([self.runs], "x", library=self.tmp / "file", say=self.said.append), [])
        self.assertIn("WARNING", self.said[-1])

    def test_a_recording_is_being_recorded_while_a_process_holds_it_or_it_changes(self):
        rec = self.rec()
        Path(str(rec) + ".lock").write_bytes(b"")                           # THE PATH LOCK stays by design: no sign
        seen = {}
        self.assertFalse(thinklib.being_recorded(rec, thinklib.open_files(self.tmp), seen))
        holder = subprocess.Popen([sys.executable, "-c", "import sys; f = open(sys.argv[1], 'rb'); print('held', flush=True); sys.stdin.read()",
                                   str(rec)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(holder.stdout.readline().strip(), "held")
            held = thinklib.open_files(self.tmp)
            if Path("/proc").is_dir():
                self.assertEqual(held, {str(rec.resolve())})
                self.assertTrue(thinklib.being_recorded(rec, held, seen))
        finally:
            holder.communicate("")
        self.assertFalse(thinklib.being_recorded(rec, thinklib.open_files(self.tmp), seen))
        with open(rec, "ab") as f:                                          # it grew since the last look
            f.write(b"more")
        self.assertTrue(thinklib.being_recorded(rec, set(), seen))
        self.assertFalse(thinklib.being_recorded(rec, set(), seen))        # and stands still at the next

    def test_the_game_directory_is_the_one_that_holds_the_headers_archives(self):
        listed = [("mod.hpi", 5, 0), ("base.ccx", 3, 0), ("impure-patch.ufo", 9, 0)]
        for name, files in (("none", {}), ("one", {"BASE.CCX": 3, "mod.hpi": 7}), ("both", {"base.ccx": 3, "mods/x/Mod.hpi": 5})):
            for f, size in files.items():
                (self.tmp / name / f).parent.mkdir(parents=True, exist_ok=True)
                (self.tmp / name / f).write_bytes(b"x" * size)
        (self.tmp / "none").mkdir(exist_ok=True)
        with mock.patch.object(thinkmap, "_tavfs", side_effect=OSError):
            pick = lambda names: thinkview.archive_dirs(listed, [self.tmp / n for n in names])      # noqa: E731
            self.assertEqual(pick(["none", "one", "both"]), [self.tmp / "both"])
            self.assertEqual(pick(["one", "none"]), [self.tmp / "one"])     # an archive of another size is not it
            self.assertEqual(pick(["none", "nowhere"]), [])

    @unittest.skipUnless(shutil.which("git"), "git, which holds the tree at a build")
    def test_the_tree_at_a_build_is_extracted_from_the_repository(self):
        repo = test_think.make_tree(self.tmp / "repo")
        (repo / "tagpu" / "ddraw" / "src" / "impai_brain.c").write_text("static PLAN p = { .base_r = 96, };\n")
        (repo / "tagpu" / "ddraw" / "src" / "unrelated.c").write_text("int x;\n")
        git = lambda *a: subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t", *a],     # noqa: E731
                                        capture_output=True, text=True, check=True).stdout.strip()
        git("init", "-q")
        git("add", "-A")
        git("commit", "-q", "-m", "a build")
        sha = git("rev-parse", "HEAD")
        (repo / "tagpu" / "ddraw" / "src" / "impai.h").write_text("/* edited since */\n")
        tree = thinkview.tree_at(sha[:8] + "-dirty", self.tmp / "cache", repo)
        self.assertEqual(tree, self.tmp / "cache" / f"tree-{sha[:12]}")
        got = sorted(p.relative_to(tree).as_posix() for p in tree.rglob("*") if p.is_file())
        self.assertEqual(got, [".thinkview-build", "tagpu/ddraw/Makefile", "tagpu/ddraw/src/impai.h",
                               "tagpu/ddraw/src/impai_brain.c", "tagpu/ddraw/src/tagpu_impai.c", "tagpu/ddraw/src/worldq.h"])
        self.assertEqual((tree / "tagpu" / "ddraw" / "src" / "impai.h").read_text(), test_think.HEADER.replace("WIDE", ""))
        self.assertEqual((thinkview.tree_build(tree), thinkview.plan_value(tree, "base_r")), (sha[:8], 96))
        self.assertEqual(thinkview.tree_at(sha[:8], self.tmp / "cache", repo), tree)       # made once
        for unknown in ("0123abcd", "not-a-build", ""):
            self.assertIsNone(thinkview.tree_at(unknown, self.tmp / "cache", repo))


def facts_of(rec) -> dict:
    """thinkview.rec_facts' answer for THE GAME, which no tarec.py reads."""
    return {"map": "Test Map", "build": "abc1234", "version": "t", "played": 1000.0, "end": 400, "ended": "complete: the level ended",
            "archives": None, "who": [{"seat": 1, "name": "Core", "kind": "impai", "by": "ImpAI", "side": "Core", "logo": 1, "result": "won"}]}


class Server(Fixture):
    """thinkserve.py on a port of its own, a library and a main checkout of the test's."""

    def setUp(self):
        super().setUp()
        self.main, self.lib = self.tmp / "main", self.tmp / "main" / ".data" / "impai" / "library"
        replays = self.main / "tagpu" / "instances" / "inst one" / "gamedir" / "replays"
        replays.mkdir(parents=True)
        self.played = replays / "played game.imprec"
        self.played.write_bytes(self.game("p.imprec").read_bytes())
        Path(str(self.played) + ".lock").write_bytes(b"")
        self.built = []
        real = thinkview.build_game
        for p in (mock.patch.object(thinklib, "main_checkout", lambda: self.main),
                  mock.patch.object(thinkview, "rec_facts", facts_of),
                  mock.patch.object(thinkview, "sample", lambda _rec, ticks, *_a, **_kw: dense_sample(ticks)),
                  mock.patch.object(thinkview, "build_game", lambda rec, *a, **kw: self.built.append(rec.name) or real(rec, *a, **kw))):
            p.start()
            self.addCleanup(p.stop)
        thinklib.add([self.game()], "exp one", note="kept for the test", library=self.lib, say=lambda _l: None)
        self.serve()

    def serve(self):
        self.said = []
        self.httpd, self.app = thinkserve.start(0, self.lib, self.main, self.said.append)
        thread = __import__("threading").Thread(target=self.httpd.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(lambda httpd=self.httpd: (httpd.shutdown(), httpd.server_close()))
        self.url = f"http://127.0.0.1:{self.httpd.server_address[1]}"

    def get(self, path, host=None):
        req = urllib.request.Request(self.url + path, headers={"Host": host} if host else {})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.status, r.read(), r.headers.get("Content-Type")
        except urllib.error.HTTPError as e:
            return e.code, e.read(), e.headers.get("Content-Type")

    def api(self, path):
        code, body, _ctype = self.get(path)
        return code, json.loads(body)

    def open(self, query, most=60.0):
        """A recording's build asked for until it is ready or failed: (the states seen, the last answer)."""
        seen, until = [], time.monotonic() + most
        while True:
            _code, st = self.api("/api/open?" + query)
            if st["state"] not in seen:
                seen.append(st["state"])
            if st["state"] in ("ready", "failed") or time.monotonic() > until:
                return seen, st
            time.sleep(.02)

    def test_the_first_screen_is_the_library_as_the_folders_hold_it(self):
        self.assertEqual(self.httpd.server_address[0], "127.0.0.1")
        code, body, ctype = self.get("/")
        self.assertEqual((code, ctype), (200, "text/html; charset=utf-8"))
        self.assertIn(b"const MANIFEST = null;", body)                      # the one page, in its app mode
        _code, doc = self.api("/api/library")
        groups = {(g["kind"], g["name"]): g for g in doc["groups"]}
        self.assertEqual(sorted(groups), [("label", "exp one"), ("played", "inst one")])
        self.assertEqual(groups[("played", "inst one")]["title"], "played · inst one")
        row, = groups[("label", "exp one")]["recs"]
        self.assertEqual({k: row[k] for k in ("name", "state", "map", "build", "end", "outcome", "note")},
                         {"name": "a", "state": "ready", "map": "Test Map", "build": "abc1234", "end": 400,
                          "outcome": "seat 1 won", "note": "kept for the test"})
        self.assertEqual([(w["seat"], w["side"], w["by"], w["rgb"]) for w in row["who"]],
                         [(1, "Core", "ImpAI", list(thinkview.STOCK_RGB[1]))])
        self.assertEqual(groups[("played", "inst one")]["recs"][0]["state"], "ready")      # its .lock says nothing
        # no index: a label added and one dropped while it serves are there and gone at the next asking
        thinklib.add([self.game("late.imprec")], "exp two", library=self.lib, say=lambda _l: None)
        thinklib.drop("exp one", self.lib, lambda _l: None)
        _code, doc = self.api("/api/library")
        self.assertEqual(sorted((g["kind"], g["name"]) for g in doc["groups"]), [("label", "exp two"), ("played", "inst one")])

    def test_a_recording_is_built_when_it_is_opened_and_kept(self):
        seen, st = self.open("k=label&g=exp+one&r=a")
        self.assertEqual((st["state"], self.built), ("ready", ["a.imprec"]), st)
        self.assertTrue(set(seen) <= {"queued", "building", "ready"} and seen[-1] == "ready", seen)
        game = st["game"]
        self.assertRegex(game["base"], rf"^cache/{self.app.builder.tool}/[0-9a-f]{{24}}/$")
        self.assertEqual((game["map"], game["end"], game["seats"]), ("Test Map", 400, [{"seat": 1, "id": "a", "file": "a.js"}]))
        self.assertEqual(game["dense"]["k"], thinkview.DENSE_K)
        code, body, ctype = self.get("/" + game["base"] + "a.js")
        self.assertEqual((code, ctype, body[:19]), (200, "text/javascript; charset=utf-8", b'thinkviewData("a",{'))
        code, body, _c = self.get("/" + game["base"] + game["dense"]["files"][0])
        self.assertEqual((code, body[:22]), (200, b'thinkviewDense("a",0,"'))
        # the second opening reads the cache: nothing is built, and a server started anew finds it too
        self.assertEqual(self.open("k=label&g=exp+one&r=a")[0], ["ready"])
        self.serve()
        self.assertEqual((self.open("k=label&g=exp+one&r=a")[0], self.built), (["ready"], ["a.imprec"]))
        # a played game is built where it lies, and nothing is written beside it
        before = sorted(p.name for p in self.played.parent.iterdir())
        self.assertEqual(self.open("k=played&g=inst+one&r=played+game")[1]["state"], "ready")
        self.assertEqual(sorted(p.name for p in self.played.parent.iterdir()), before)
        # another version of the tools trusts none of it: its own folder, and the old one removed at its start
        with mock.patch.object(thinkserve, "tool_digest", lambda: "0123456789ab"):
            self.serve()
            self.assertTrue(any("another version" in line for line in self.said), self.said)
            self.assertEqual([p.name for p in (self.lib / ".cache").iterdir()], [])
            self.assertEqual((self.open("k=label&g=exp+one&r=a")[1]["state"], self.built.count("a.imprec")), ("ready", 2))

    def test_a_build_that_fails_says_why_and_is_made_again_when_asked(self):
        with mock.patch.object(thinkview, "sample", side_effect=thinkview.ViewError("its stream could not be sampled")):
            _seen, st = self.open("k=label&g=exp+one&r=a")
            self.assertEqual((st["state"], st["error"]), ("failed", "its stream could not be sampled"))
            self.assertEqual(self.open("k=label&g=exp+one&r=a")[1]["state"], "failed")      # not made again unasked
        self.assertEqual(self.open("k=label&g=exp+one&r=a&again=1")[1]["state"], "ready")

    def test_what_a_request_names_is_looked_up_never_joined_into_a_path(self):
        self.open("k=label&g=exp+one&r=a")
        outside = self.tmp / "secret.js"
        outside.write_text("no")
        tool = self.app.builder.tool
        key = next(p.name for p in (self.lib / ".cache" / tool).iterdir())
        for path in ("/cache/../../secret.js", f"/cache/{tool}/../../../../secret.js", f"/cache/{tool}/{key}/..%2f..%2f..%2fsecret.js",
                     f"/cache/{tool}/{key}/game.json", f"/cache/{tool}/{key}/", f"/cache/{tool}/{key}/.hidden.js",
                     "/api/nothing", "/thinkview.py", "//etc/passwd"):
            self.assertEqual(self.get(path)[0], 404, path)
        for query in ("k=label&g=..%2F..&r=secret", "k=label&g=exp+one&r=..%2Fa", "k=played&g=exp+one&r=a", "k=label&g=.cache&r=a", ""):
            code, st = self.api("/api/open?" + query)
            self.assertEqual((code, st["state"]), (404, "failed"), query)
        self.assertEqual(self.get("/api/library", host="elsewhere.example")[0], 403)       # its own address alone
        (self.lib / ".cache" / tool / key / "link.js").symlink_to(outside)                 # a link out of the cache
        self.assertEqual(self.get(f"/cache/{tool}/{key}/link.js")[0], 404)

    def test_a_recording_being_recorded_is_shown_and_not_opened(self):
        with open(self.played, "ab") as f:
            self.api("/api/library")
            f.write(b" ")                                                   # it grows between two listings
            f.flush()
            _code, doc = self.api("/api/library")
            row, = next(g for g in doc["groups"] if g["kind"] == "played")["recs"]
            self.assertEqual(row["state"], "recording")
            self.assertNotIn("map", row)                                    # nothing of it is read
        holder = subprocess.Popen([sys.executable, "-c", "import sys; f = open(sys.argv[1], 'ab'); print('held', flush=True); sys.stdin.read()",
                                   str(self.played)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(holder.stdout.readline().strip(), "held")
            if Path("/proc").is_dir():
                code, st = self.api("/api/open?k=played&g=inst+one&r=played+game")
                self.assertEqual((code, st["state"]), (409, "failed"))
                self.assertIn("being recorded", st["error"])
        finally:
            holder.communicate("")
        self.assertEqual(self.built, [])


def gaf_bytes(name: str, frames: list) -> bytes:
    """A GAF of one sequence `name`: each frame (width, height, the palette index of its middle pixel), plain
    rows, every other pixel index 1."""
    head = 12 + 4
    table = head + 40 + 8 * len(frames)
    out = struct.pack("<IIII", 0x00010100, 1, 0, head) + struct.pack("<HHI32s", len(frames), 1, 0, name.encode())
    bodies, at = b"", table
    for w, h, mid in frames:
        out += struct.pack("<II", at, 0)
        px = bytearray(b"\x01" * (w * h))
        px[(h // 2) * w + w // 2] = mid
        bodies += struct.pack("<HHhhBBHII", w, h, 0, 0, 0, 0, 0, 0, at + 24) + struct.pack("<I", 0) + bytes(px)
        at += 24 + w * h
    return out + bodies


MW, MH = 4, 3                   # THE MAP, in 32-px tiles: wider than tall, so a swapped axis shows
GW, GH = MW * 2, MH * 2         # ... and in 16-px cells
PALETTE = [(i * 20 % 256, 255 - i * 20 % 256, i * 7 % 256) for i in range(256)]


def tnt_bytes(heights=None, features=None, names=("Tree1", "Rock1"), sea=40) -> bytes:
    """THE MAP as a TNT (tascene's layout): tile t is colour t + 1 all over and stands at map cell t, row-major;
    `heights` a byte a 16-px cell, `features` {cell: index into `names`}."""
    heights = heights or bytes(GW * GH)
    attr = b"".join(struct.pack("<BHB", heights[c], (features or {}).get(c, 0xFFFF), 0) for c in range(GW * GH))
    tiles = b"".join(bytes([t + 1]) * 1024 for t in range(MW * MH))
    anims = b"".join(struct.pack("<i128s", i, n.encode()) for i, n in enumerate(names))
    p_attr = 64 + MW * MH * 2
    p_gfx = p_attr + len(attr)
    p_anim = p_gfx + len(tiles)
    head = struct.pack(thinkmap._tavfs().tascene.TNT_HDR, 0x2000, GW, GH, 64, p_attr, p_gfx, MW * MH, len(names), p_anim,
                       sea, p_anim + len(anims), 0, 0, 0, 0, 0)
    return head + struct.pack(f"<{MW * MH}H", *range(MW * MH)) + attr + tiles + anims + struct.pack("<2i", 1, 1) + b"\0"


def grid(heights=None, metal=None, defs=None) -> bytes:
    """THE GRID: tarec's World.grid, 13 bytes a cell (+4 the height, +7 the metal, +8 the def)."""
    out = bytearray()
    for c in range(GW * GH):
        cell = bytearray(13)
        cell[4], cell[7] = (heights or bytes(GW * GH))[c], (metal or bytes(GW * GH))[c]
        struct.pack_into("<H", cell, 8, (defs or {}).get(c, 0xFFFF))
        out += cell
    return bytes(out)


@unittest.skipUnless(numpy and Image, "numpy and Pillow, the picture's and the layers' need")
class Ground(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="test_thinkmap."))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.dir = self.tmp / "gamedir"
        for sub in ("maps", "palettes", "features", "units"):
            (self.dir / sub).mkdir(parents=True)
        self.heights = bytes(min(255, 10 * (c // GW) + c % GW) for c in range(GW * GH))
        (self.dir / "maps" / "Test Map.tnt").write_bytes(tnt_bytes(self.heights, {5: 0, 20: 1}))
        (self.dir / "palettes" / "PALETTE.PAL").write_bytes(b"".join(bytes(c) + b"\0" for c in PALETTE))
        (self.dir / "features" / "all.tdf").write_text(
            "[Tree1] { description=Tree; footprintx=1; footprintz=2; energy=250; reclaimable=1; blocking=1; }\n"
            "[Rock1] { description=Rock; metal=40; energy=3; reclaimable=1; }\n")
        (self.dir / "units" / "MEX.fbi").write_text("[UNITINFO] { UnitName=MEX; ExtractsMetal=0.001; }")
        (self.dir / "units" / "BOAT.fbi").write_text("[UNITINFO] { UnitName=BOAT; Floater=1; }")
        (self.dir / "textures").mkdir()
        (self.dir / "textures" / "logos.gaf").write_bytes(gaf_bytes("32xLogos", [(3, 3, 7), (5, 4, 9), (65, 2, 11)]))
        self.listed = [("impure-patch.ufo", 1, 0)]     # Impure's own archive: left out, so the loose files are all

    def source(self, listed=None):
        return thinkmap.Source(self.listed if listed is None else listed, [self.dir])

    def tnt(self):
        return thinkmap.tnt_of(self.source(), "Test Map")[0]

    def test_the_picture_shows_each_tile_where_the_world_has_it(self):
        for side in (4096, 100, 48):                    # whole; resized; each tile halved and then resized
            img, n = thinkmap.render(self.tnt(), PALETTE, side)
            info = {"w": img.width, "h": img.height, "px": [MW * 32, MH * 32]}
            self.assertEqual((n, max(img.size)), {4096: (1, 128), 100: (1, 100), 48: (2, 48)}[side])
            self.assertEqual(img.width * MH, img.height * MW)               # the map's shape, both axes
            for t in range(MW * MH):
                x, z = thinkmap.image_px(info, (t % MW) * 32 + 16, (t // MW) * 32 + 16)
                for got, want in zip(img.getpixel((int(x), int(z))), PALETTE[t + 1]):
                    self.assertAlmostEqual(got, want, delta=3, msg=f"tile {t} at side {side}")
        self.assertEqual(thinkmap.image_px(info, MW * 32, MH * 32), (img.width, img.height))

    def test_the_picture_is_held_to_its_bytes(self):
        noise = Image.frombytes("RGB", (256, 256), bytes(numpy.random.default_rng(1).integers(0, 256, 256 * 256 * 3, numpy.uint8)))
        raw, q, size = thinkmap.encode(noise, 30000)
        self.assertLessEqual(len(raw), 30000)
        self.assertEqual((raw[:4], raw[8:12]), (b"RIFF", b"WEBP"))
        self.assertTrue(q < thinkmap.QUALITIES[0] or size != (256, 256))    # noise does not fit as it is

    def test_a_file_opens_as_the_recordings_game_opened_it(self):
        src = self.source()
        self.assertEqual(src.where("maps\\TEST MAP.TNT"), ("loose", []))     # without case, as the engine
        self.assertEqual(src.notes(), [])
        gone = self.source([("first.hpi", 10, 0)])                          # a listed archive that is not found
        self.assertEqual(gone.notes(), ["first.hpi: not found"])
        self.assertEqual(gone.open("maps/Test Map.tnt")[1], "loose")        # loose stands before every archive
        with self.assertRaisesRegex(thinkmap.MapError, "first.hpi"):
            gone.open("maps/Other.tnt")
        with self.assertRaisesRegex(thinkmap.MapError, "no game directory"):
            thinkmap.Source(self.listed, [self.tmp / "nowhere"])

    def test_the_grid_gives_the_heights_the_metal_and_the_features(self):
        metal = bytearray([3] * (GW * GH))
        metal[10:13] = b"\x1e\x1e\x28"
        tr = thinkmap.Track(GW, GH)
        tr.take(2, grid(self.heights, bytes(metal), {5: 0, 6: 0xFFFE, 20: 1}))     # 0xFFFE: a footprint's cell, no anchor
        tr.take(30, grid(self.heights, bytes(metal), {5: 0, 6: 0xFFFE, 20: 1}))
        tr.take(60, grid(self.heights, bytes(metal), {5: 0, 6: 0xFFFE, 33: 7}))    # the rock gone, a wreck made
        g = tr.ground(sea=40)
        self.assertEqual(g["lives"], [[5, 0, 0, None], [20, 1, 0, 60], [33, 7, 60, None]])
        self.assertEqual((g["gw"], g["gh"], g["at"], g["sea"], g["hstep"]), (GW, GH, 2, 40, 1))
        self.assertEqual(base64.b64decode(g["h"]), self.heights)
        cells = [v for v, n in zip(g["metal"]["rle"][::2], g["metal"]["rle"][1::2]) for _ in range(n)]
        self.assertEqual((bytes(cells), g["metal"]["base"], g["metal"]["max"]), (bytes(metal), 3, 40))
        self.assertEqual(thinkmap.Track(GW, GH).ground(), {})               # no sample: no ground

    def test_the_heights_are_handed_over_a_value_a_block_on_a_large_map(self):
        with mock.patch.object(thinkmap, "HEIGHT_SIDE", 4):
            tr = thinkmap.Track(GW, GH)
            tr.take(1, grid(self.heights))
            g = tr.ground()
        self.assertEqual((g["hstep"], g["hw"], g["hh"]), (2, 4, 3))
        h = base64.b64decode(g["h"])
        self.assertEqual(h[0], round((0 + 1 + 10 + 11) / 4 + .01))          # a block's mean
        self.assertEqual(len(h), 12)

    def test_the_map_file_is_held_to_the_recordings_ground(self):
        tr = thinkmap.Track(GW, GH)
        tr.take(1, grid(self.heights))
        self.assertEqual(thinkmap.checked(self.tnt(), tr), [])
        other = thinkmap.Track(GW, GH)
        other.take(1, grid(bytes(GW * GH)))
        self.assertRegex(thinkmap.checked(self.tnt(), other)[0], r"^\d+ of its 48 cells' heights")
        with self.assertRaisesRegex(thinkmap.MapError, "not the map this game played"):
            thinkmap.checked(self.tnt(), thinkmap.Track(GW + 2, GH))

    def test_a_feature_and_a_unit_type_are_told_by_their_own_files(self):
        table, lost = thinkmap.feature_table(self.source(), ["Tree1", "Rock1", "Gone9"])
        self.assertEqual(table["0"], ["Tree1", "energy", 1, 2, 0, 250, "Tree"])
        self.assertEqual(table["1"], ["Rock1", "metal", 1, 1, 40, 3, "Rock"])
        self.assertEqual(lost, ["Gone9"])
        for body, kind in (({"geothermal": "1", "reclaimable": "1"}, "vent"), ({"blocking": "1"}, "block"), ({}, "decor")):
            self.assertEqual(thinkmap.feature_kind(body), kind)
        kinds = thinkmap.unit_kinds(self.source(), {1: "MEX", 2: "BOAT", 3: "NOFILE"})
        self.assertEqual(kinds, {1: thinkmap.F_EXTRACTS, 2: thinkmap.F_FLOATS})

    def test_a_players_colour_is_its_logo_frames_middle_pixel(self):
        rgb, where = thinkmap.team_colours(self.source())
        self.assertEqual((rgb, where), ([PALETTE[7], PALETTE[9], None], "loose"))      # a frame past 64 px gives none
        (self.dir / "textures" / "logos.gaf").write_bytes(gaf_bytes("other", [(3, 3, 7)]))
        with self.assertRaisesRegex(thinkmap.MapError, "no sequence 32xlogos"):
            thinkmap.team_colours(self.source())
        # the recording's own colours when its archives hold them, else the unmodded game's and why
        self.assertEqual(thinkview.terrain({"track": None, "archives": self.listed}, {}, [self.dir], {})["colours"],
                         {"rgb": thinkview.STOCK_RGB, "from": None, "why": "textures/logos.gaf in loose holds no sequence 32xlogos"})
        (self.dir / "textures" / "logos.gaf").write_bytes(gaf_bytes("32xlogos", [(3, 3, 7)]))
        self.assertEqual(thinkview.terrain({"track": None, "archives": self.listed}, {}, [self.dir], {})["colours"],
                         {"rgb": [PALETTE[7]], "from": "loose", "why": ""})

    def test_the_ground_of_a_recording_and_what_could_not_be_read(self):
        tr = thinkmap.Track(GW, GH)
        tr.take(1, grid(self.heights, defs={5: 0, 20: 1, 33: 9}))
        found = {"track": tr, "archives": self.listed, "map": "Test Map"}
        pictures = {}
        out = thinkview.terrain(found, {1: "MEX", 4: "BOAT"}, [self.dir], pictures)
        again = thinkview.terrain(found, {}, [self.dir], pictures)
        (raw, entry), = pictures.values()                                   # one picture a map file
        self.assertEqual(Image.open(__import__("io").BytesIO(raw)).size, (entry["w"], entry["h"]))
        self.assertEqual((entry["px"], entry["from"], entry["w"], entry["h"]), ([MW * 32, MH * 32], "loose", 128, 96))
        self.assertRegex(entry["file"], r"^Test-Map\.[0-9a-f]{8}\.map\.webp$")
        self.assertIs(again["map"], entry)
        self.assertEqual(out["kinds"], {"1": thinkmap.F_EXTRACTS, "4": thinkmap.F_FLOATS})
        self.assertEqual((out["ground"]["named"], out["ground"]["sea"], out["ground"]["defs"]["1"][1]), (2, 40, "metal"))
        self.assertEqual(out["said"], [])
        self.assertEqual(list(self.dir.rglob("*.webp")) + list(Path(thinkview.HERE).glob("*.webp")), [])   # nothing is written here
        # a map the archives do not hold: the recording's own layers still stand, and the page is told
        lost = thinkview.terrain(dict(found, map="No Such Map"), {}, [self.dir], pictures)
        self.assertIsNone(lost["map"])
        self.assertEqual(len(lost["ground"]["lives"]), 3)
        self.assertRegex(lost["said"][0], "^the map's picture is not drawn: no maps/No Such Map.tnt")
        self.assertRegex(thinkview.terrain({"track": None}, {}, [self.dir], pictures)["said"][0], "^no sample")


def view(calls: str, name="VIEW"):
    """A marked block of the page (View's by default) evaluated by node: `calls` is a JS expression over it, its
    value as JSON."""
    text = thinkview.TEMPLATE.read_text()
    block = text[text.index(f"/*{name}*/"):text.index(f"/*END {name}*/")]
    got = subprocess.run(["node", "-e", block + f"\nconsole.log(JSON.stringify({calls}));"], capture_output=True, text=True, timeout=60)
    if got.returncode:
        raise AssertionError(got.stderr)
    return json.loads(got.stdout)


@unittest.skipUnless(shutil.which("node"), "node, which evaluates the page's own View block")
class ViewMaths(unittest.TestCase):
    PW, PH, W, H = 4000, 3000, 800, 600             # a map of 4000 x 3000 px on a canvas of its shape

    def call(self, body: str):
        return view(f"(() => {{ const [pw, ph, w, h] = [{self.PW}, {self.PH}, {self.W}, {self.H}]; {body} }})()")

    def test_the_fit_shows_the_whole_map(self):
        got = self.call("const v = View.fit(pw, ph); return [v, View.to(v, pw, w, h, 0, 0), View.to(v, pw, w, h, pw, ph), View.scale(v, pw, w)];")
        self.assertEqual(got, [{"k": 1, "x": 2000, "z": 1500}, [0, 0], [800, 600], 0.2])

    def test_a_zoom_keeps_the_point_under_the_cursor(self):
        got = self.call("""const v = { k: 2, x: 1800, z: 1400 }, at = View.from(v, pw, w, h, 300, 250);
            const z = View.zoomAt(v, 1.5, 300, 250, pw, ph, w, h); return [z.k, at, View.from(z, pw, w, h, 300, 250), View.to(z, pw, w, h, at[0], at[1])];""")
        self.assertEqual(got[0], 3)
        for a, b in zip(got[1], got[2]):
            self.assertAlmostEqual(a, b, places=6)
        for a, b in zip(got[3], (300, 250)):
            self.assertAlmostEqual(a, b, places=6)

    def test_the_map_never_leaves_the_canvas(self):
        got = self.call("""return [View.zoomAt(View.fit(pw, ph), .5, 10, 10, pw, ph, w, h), View.pan({ k: 2, x: 1000, z: 750 }, 5000, 5000, pw, ph, w),
            View.pan({ k: 2, x: 2000, z: 1500 }, -40, 20, pw, ph, w), View.zoomAt(View.fit(pw, ph), 1e6, 400, 300, pw, ph, w, h).k, View.kmax(pw, w)];""")
        self.assertEqual(got[0], {"k": 1, "x": 2000, "z": 1500})            # no zoom out past the fit
        self.assertEqual(got[1], {"k": 2, "x": 1000, "z": 750})             # a drag past the edge stops at it
        self.assertEqual(got[2], {"k": 2, "x": 2100, "z": 1450})            # 40 px at 0.4 px a world px: 100 world px
        self.assertEqual((got[3], got[4]), (15, 15))                        # 3 canvas px a world px at the most

    def test_the_ground_under_a_lifted_point(self):
        got = self.call("""const height = (x, z) => 100 + z / 10;            // a slope: the fixed point of z - height / 2 = zl
            const z = View.ground(0, 900, height); return [z, z - height(0, z) / 2, View.ground(0, 500, () => 0),
            View.gridStep(.2, 32, 58), View.gridStep(3, 32, 58), View.gridStep(.001, 32, 58)];""")
        self.assertAlmostEqual(got[0], 1000, places=2)
        self.assertAlmostEqual(got[1], 900, places=2)
        self.assertEqual(got[2:], [500, 16, 1, 128])


if __name__ == "__main__":
    unittest.main()
