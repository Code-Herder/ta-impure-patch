#!/usr/bin/env python3
"""test_thinkview -- thinkview.py offline: the page's data built from a small fake of the recording reader.

    python3 tools/impai/test_thinkview.py

THE TREE is test_think's shape with the members the view reads (a squad's goal and target, the judgements, the
anchor, a plant's way out, a job's site, a known row's block). THE GAME is one squad's known task history -- FORM
at 100, ATTACK at 200 with another goal at 260, RETREAT on a lost prediction at 300, FREE at 350 -- read through
test_think's fake of think.stream, and a fake of thinkview.sample stands for the unit images. The tests that
need the DLL's compiler are skipped where it is not installed; the lanes', the runs' and the pressure's
arithmetic need none."""
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_think  # noqa: E402
import think  # noqa: E402
import thinklayout  # noqa: E402
import thinkview  # noqa: E402

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
        html = thinkview.page([{"id": "a", "file": "a.js", "name": "</script>"}])
        self.assertIn('[{"id":"a","file":"a.js","name":"<\\/script>"}]', html)
        self.assertTrue(html.startswith("<!doctype html>"))
        self.assertFalse(thinkview.page([], bare=True).startswith("<!doctype"))
        self.assertTrue(thinkview.data_file("a", {"v": 1}).startswith('thinkviewData("a",{"v":1});'))


@unittest.skipUnless(test_think.HAVE_CC, "the DLL's compiler is not installed")
class View(unittest.TestCase):
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
                  mock.patch.object(thinkview, "sample", lambda _rec, ticks: fake_sample(ticks))):
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
        self.assertEqual(f["j"], [12, 2, 0xFFFF, 0, 0, 1, 0xFFFF])              # the boat, in squad 1
        self.assertEqual(f["k"], [21, 6, 0x0A, 2, 14, 36])                      # the tower, in sight and on radar
        self.assertEqual(d["helds"][f["h"]], [8, 10])                           # held[1] = 0b101: cells 8 and 10
        self.assertEqual(fr[300]["k"][2], 0x20)                                 # a ghost by then
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

    def test_the_command_writes_the_page_and_a_file_a_recording(self):
        import contextlib
        import io
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = thinkview.main([str(self.game()), str(self.game("b c.imprec")), "--out", str(self.tmp / "page")])
        self.assertEqual(rc, 0, out.getvalue())
        html = (self.tmp / "page" / "index.html").read_text()
        self.assertIn('"id":"a","file":"a.js"', html)
        self.assertIn('"id":"b-c","file":"b-c.js"', html)
        js = (self.tmp / "page" / "a.js").read_text()
        self.assertTrue(js.startswith('thinkviewData("a",{'))
        self.assertEqual(json.loads(js[len('thinkviewData("a",'):-3])["lanes"][0]["k"], 1)
        with contextlib.redirect_stdout(out):
            self.assertEqual(thinkview.main([str(self.tmp / "nothing.imprec"), "--out", str(self.tmp / "p2")]), 2)
        self.assertIn("COULD NOT RUN", out.getvalue())
        self.assertFalse((self.tmp / "p2").exists())


if __name__ == "__main__":
    unittest.main()
