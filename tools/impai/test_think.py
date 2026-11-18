#!/usr/bin/env python3
"""test_think -- think.py and thinklayout.py offline: a small source tree compiled by the DLL's compiler for the
layout, and a fake of the recording reader for the games. No recording is read and no game runs.

    python3 tools/impai/test_think.py

THE TREE is the shape thinklayout reads -- a Makefile naming the compiler and its flags, impai.h, and
tagpu_impai.c defining the four regions -- with structs a dozen members long; THE WIDE TREE is the same with one
member more in IMPAI_BRAIN, another build's layout. THE GAME is a script of the events a recording's stream gives
(think.stream's sink calls), written as JSON where a recording would be and read by a fake of think.stream, its
bytes packed through the layout the tree gives, as the DLL's writes are. The tests that need the compiler are
skipped where it is not installed; the one over this tree's own headers holds the layout to the header's own
_Static_assert."""
import io
import json
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import think  # noqa: E402
import thinklayout  # noqa: E402

MAKEFILE = "CFLAGS   ?= -Iinc -O2 -Wall -std=c99\nCC        = i686-w64-mingw32-gcc\n"
HEADER = """#ifndef IMPAI_H
#define IMPAI_H
#include <stdint.h>
#include "worldq.h"
#define IMPAI_OWN 0x01u
#define IMPAI_LOS 0x02u
#define IMPAI_RADAR 0x08u
enum { IMPAI_SQ_FREE, IMPAI_SQ_FORM, IMPAI_SQ_ATTACK, IMPAI_SQ_RETREAT, IMPAI_SQ_TASKS };
typedef enum { TASIM_LAYER_LAND, TASIM_LAYER_SEA, TASIM_LAYER_AIR, TASIM_LAYERS } TASIM_LAYER;
typedef struct IMPAI_SQUAD { uint8_t task, layer, n, flags; int16_t tx, tz; uint16_t target, pad; float value; }
    IMPAI_SQUAD;
typedef struct IMPAI_MIL { int8_t naval; uint8_t dry; uint16_t waves[3]; IMPAI_SQUAD sq[4]; uint32_t wave_t;
    TASIM_LAYER last; } IMPAI_MIL;
typedef struct IMPAI_FMEM { int16_t ax, az; uint32_t seen; } IMPAI_FMEM;
typedef struct IMPAI_BRAIN { uint8_t sea_refused, nfmem; IMPAI_FMEM fmem[4]; WIDE uint8_t held[2][4]; } IMPAI_BRAIN;
enum { IMPAI_JOB_NONE, IMPAI_JOB_BUILD, IMPAI_JOB_SQUAD };
typedef struct IMPAI_JOB { uint16_t type; uint8_t kind, squad; } IMPAI_JOB;
typedef struct IMPAI_SEEN { uint16_t type; uint8_t how, owner; } IMPAI_SEEN;
typedef struct IMPAI_ROW { IMPAI_SEEN seat[WQ_SEATS]; } IMPAI_ROW;
#endif
"""
WORLDQ = "#define WQ_NONE 0xFFFFu\n#define WQ_SEATS 10\n"
SOURCE = ('#include "impai.h"\nstatic IMPAI_ROW s_known[64];\nstatic IMPAI_BRAIN s_brain[10];\n'
          "static IMPAI_JOB s_job[64];\nstatic IMPAI_MIL s_mil[10];\n")
SLOTS, EACH = 31, 10            # seat k's slots are 1 + 10 k .. 10 + 10 k
REAL_STREAM = think.stream
HAVE_CC = shutil.which("i686-w64-mingw32-gcc") and shutil.which("i686-w64-mingw32-objdump")


def make_tree(root: Path, wide=False) -> Path:
    src = root / "tagpu" / "ddraw" / "src"
    src.mkdir(parents=True)
    (root / "tagpu" / "ddraw" / "Makefile").write_text(MAKEFILE)
    (src / "impai.h").write_text(HEADER.replace("WIDE", "uint32_t room;" if wide else ""))
    (src / "worldq.h").write_text(WORLDQ)
    (src / "tagpu_impai.c").write_text(SOURCE)
    return root


def fake_stream(path, sink):
    """think.stream over THE GAME at `path`."""
    doc = json.loads(Path(path).read_text())
    if sink is None:
        return doc["head"]
    for ev in doc["events"]:
        if ev[0] == "span":
            sink.span(ev[1], ev[2], bytes.fromhex(ev[3]))
        else:
            getattr(sink, ev[0])(*ev[1:])
    return {"stop": None}


@unittest.skipUnless(HAVE_CC, "the DLL's compiler is not installed")
class Think(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(tempfile.mkdtemp())
        cls.tree, cls.wide = make_tree(cls.root / "tree"), make_tree(cls.root / "wide", wide=True)
        cls.cache = cls.root / "cache"
        cls.lay = think.Layout(thinklayout.load(cls.tree, cls.cache), cls.tree)
        cls.wlay = think.Layout(thinklayout.load(cls.wide, cls.cache), cls.wide)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, True)

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        for p in (mock.patch.object(think, "stream", fake_stream), mock.patch.object(think, "TREE", self.tree),
                  mock.patch.object(think, "CACHE", self.cache)):
            p.start()
            self.addCleanup(p.stop)

    # ---- THE GAME ----
    def put(self, part, row, name, value, lay=None):
        lay = lay or self.lay
        f = next(f for f in lay.fields(part) if f.name == name)
        return ["span", part, row * lay.size[part] + f.off, f.st.pack(value).hex()]

    def game(self, name="a.imprec", lay=None, change=None, extra=(), end=400):
        """A game of seat 1's: the tables sized at tick 0, a naval judgement at 31, a squad forming at 100 and
        attacking slot 21 at 200, and an opponent's unit dying at 300, to tick `end`. `change` replaces events by
        their index and `extra` come before the last tick."""
        lay = lay or self.lay
        ev = [["tick", 0]] + [["size", p, lay.size[p] * (10 if p in think.BY_SEAT else SLOTS)] for p in think.PARTS]
        ev += [["unit", 11, 5], ["unit", 21, 7], ["unit", 22, 7],
               ["tick", 1], self.put("mil", 1, "naval", -1, lay),
               ["tick", 31], self.put("mil", 1, "naval", 1, lay), self.put("brain", 1, "sea_refused", 2, lay),
               ["tick", 100], self.put("mil", 1, "sq[1].task", 1, lay), self.put("mil", 1, "sq[1].layer", 1, lay),
               self.put("mil", 1, "sq[1].n", 2, lay), self.put("mil", 1, "sq[1].target", 0xFFFF, lay),
               self.put("job", 12, "type", 9, lay), self.put("job", 12, "kind", 2, lay),
               self.put("job", 12, "squad", 1, lay), ["unit", 12, 9],
               ["tick", 150], self.put("mil", 1, "naval", 1, lay), self.put("mil", 1, "sq[1].n", 3, lay),
               ["tick", 200], self.put("mil", 1, "sq[1].task", 2, lay), self.put("mil", 1, "sq[1].tx", 14, lay),
               self.put("mil", 1, "sq[1].tz", 36, lay), self.put("mil", 1, "sq[1].target", 21, lay),
               self.put("mil", 1, "sq[1].value", 250.5, lay), self.put("mil", 1, "wave_t", 199, lay),
               self.put("mil", 1, "waves[1]", 1, lay), self.put("mil", 1, "last", 1, lay),
               self.put("known", 21, "seat[1].type", 7, lay), self.put("known", 21, "seat[1].how", 0x0A, lay),
               self.put("known", 21, "seat[1].owner", 2, lay), self.put("known", 21, "seat[2].type", 7, lay),
               self.put("brain", 1, "held[1][2]", 9, lay),
               ["tick", 300], ["gone", 21], self.put("mil", 1, "sq[1].task", 0, lay),
               ["tick", end]]
        for k, e in (change or {}).items():
            ev[k] = e
        ev[-1:-1] = list(extra)
        head = {"build": "abc1234", "map": "Test Map", "slots": SLOTS, "units_each": EACH,
                "players": [{"index": 0, "name": "Player", "ai": 0, "side": 0},
                            {"index": 1, "name": "Core", "ai": 1, "side": 1},
                            {"index": 2, "name": "Arm", "ai": 0, "side": 0}],
                "stride": {p: lay.size[p] for p in think.PARTS}}
        f = self.tmp / name
        f.write_text(json.dumps({"head": head, "events": ev}))
        return f

    def run_(self, *argv):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = think.main([str(a) for a in argv])
        self.said = out.getvalue()
        return rc

    # ---- the layout ----
    def test_the_layout_is_the_compilers(self):
        lay = self.lay
        self.assertEqual({p: lay.size[p] for p in think.PARTS}, {"mil": 80, "brain": 44, "job": 4, "known": 40})
        self.assertEqual(lay.ctype, {"mil": "IMPAI_MIL", "brain": "IMPAI_BRAIN", "job": "IMPAI_JOB",
                                     "known": "IMPAI_ROW"})
        f = {x.name: x for x in lay.fields("mil")}
        self.assertEqual((f["naval"].off, f["waves[2]"].off, f["sq[3].task"].off, f["sq[3].value"].off,
                          f["wave_t"].off, f["last"].off), (0, 6, 56, 68, 72, 76))
        self.assertEqual(f["naval"].show(b"\xff"), "-1")                          # int8_t, signed
        self.assertEqual(f["sq[0].task"].show(b"\x02"), "ATTACK")                 # ENUM_OF, by the header's enum
        self.assertEqual(f["sq[0].task"].show(b"\x09"), "9")                      # no name: the number
        self.assertEqual(f["sq[0].layer"].show(b"\x01"), "SEA")
        self.assertEqual(f["last"].show(b"\x02\0\0\0"), "AIR")                    # a member typed as the enum
        how = next(x for x in lay.seat_fields("known", 1) if x.name == "how")
        self.assertEqual((how.show(b"\x0a"), how.show(b"\x00"), how.show(b"\x41")), ("LOS|RADAR", "0", "OWN|0x40"))
        self.assertEqual(lay.macros["WQ_NONE"], 0xFFFF)
        self.assertEqual([x.name for x in lay.seat_fields("known", 3)], ["type", "how", "owner"])
        self.assertEqual(self.wlay.size["brain"], 48)
        self.assertIn("tagpu/ddraw/src/worldq.h", lay.lay["headers"])             # what impai.h includes counts

    def test_a_layout_is_kept_by_its_headers_hash(self):
        with mock.patch.object(thinklayout, "generate", side_effect=AssertionError("compiled again")):
            self.assertEqual(thinklayout.load(self.tree, self.cache)["roots"], self.lay.lay["roots"])
        before = thinklayout.key(self.tree)
        h = self.tree / "tagpu" / "ddraw" / "src" / "worldq.h"
        text = h.read_text()
        self.addCleanup(h.write_text, text)
        h.write_text(text + "#define WQ_MORE 1\n")
        self.assertNotEqual(thinklayout.key(self.tree), before)                   # an included header's text counts
        self.assertEqual(thinklayout.load(self.tree, self.cache)["macros"]["WQ_MORE"], 1)

    def test_a_tree_that_is_not_one_or_does_not_compile_is_said(self):
        with self.assertRaisesRegex(thinklayout.LayoutError, "is no source tree of the DLL"):
            thinklayout.load(self.tmp, self.cache)
        bad = make_tree(self.tmp / "bad")
        (bad / "tagpu" / "ddraw" / "src" / "impai.h").write_text("typedef struct { int x } IMPAI_MIL;\n")
        with self.assertRaisesRegex(thinklayout.LayoutError, "does not compile"):
            thinklayout.load(bad, None)
        self.assertEqual(self.run_("state", self.game(), "--at", 5, "--tree", self.tmp), 2)
        self.assertIn("think: COULD NOT RUN:", self.said)

    # ---- the verbs ----
    def test_state(self):
        rec = self.game()
        self.assertEqual(self.run_("state", rec, "--at", 250), 0)
        s = self.said
        self.assertIn("state: a.imprec (build abc1234, Test Map, ticks to 400), seat 1 (Core, ImpAI), at tick 250", s)
        self.assertIn("mil: naval=1 dry=0 waves=[0,1,0] wave_t=199 last=SEA", s)
        self.assertIn("mil.sq[*] x3: all zero", s)
        self.assertIn("mil.sq[1]: task=ATTACK layer=SEA n=3 flags=0 tx=14 tz=36 target=21 value=250.5", s)
        self.assertNotIn("pad", s)                                                # zero padding is not printed
        self.assertIn("brain: sea_refused=2 nfmem=0 held=[0,0,0,0,0,0,9,0]", s)
        self.assertIn("job[*] x9: all zero", s)                                   # the seat's ten slots, 11..20
        self.assertIn("job[12]: type=9 kind=SQUAD squad=1", s)
        self.assertIn("known[21]: type=7 how=LOS|RADAR owner=2", s)               # the seat's own column of the row
        self.assertIn(f"known[*] x{SLOTS - 1}: all zero", s)
        # the state at a tick is after that tick's writes and before the next one's
        self.assertEqual(self.run_("state", rec, "--at", 199, "--fields", "mil.sq[1].task,mil.naval,job[12].kind"), 0)
        self.assertEqual(self.said.splitlines()[1:], ["  mil.sq[1].task = FORM", "  mil.naval = 1",
                                                      "  job[12].kind = SQUAD"])
        self.assertEqual(self.run_("state", rec, "--at", 200, "--fields", "mil.sq[1]", "--seat", 1), 0)
        self.assertIn("  mil.sq[1].task = ATTACK", self.said)
        self.assertEqual(self.run_("state", rec, "--at", 250, "--part", "job", "--seat", 2), 0)
        self.assertIn("job[*] x10: all zero", self.said)                          # another seat's rows are its own
        self.assertNotIn("job[12]", self.said)

    def test_the_print_is_held_to_max_and_says_what_it_cut(self):
        rec = self.game()
        self.assertEqual(self.run_("state", rec, "--at", 250, "--fields", "known", "--max", 3), 0)
        lines = self.said.splitlines()
        self.assertEqual(len(lines), 3)
        self.assertRegex(lines[-1], r"^think: \d+ more lines cut at --max 3 ")
        self.assertEqual(self.run_("state", rec, "--at", 250, "--fields", "known.type"), 0)
        self.assertEqual(self.said.splitlines()[1:], ["  known[21].type = 7", f"  {SLOTS - 1} more fields are 0"])

    def test_timeline_prints_only_the_ticks_where_a_value_changes(self):
        rec = self.game()
        self.assertEqual(self.run_("timeline", rec, "--field", "mil.naval,mil.sq[*].task,brain.sea_refused"), 0)
        self.assertEqual(self.said.splitlines()[1:], [
            "       1  mil.naval 0 -> -1",
            "      31  mil.naval -1 -> 1; brain.sea_refused 0 -> 2",
            "     100  mil.sq[1].task FREE -> FORM",                # 150 writes naval = 1 again: no line
            "     200  mil.sq[1].task FORM -> ATTACK",
            "     300  mil.sq[1].task ATTACK -> FREE"])
        self.assertEqual(self.run_("timeline", rec, "--field", "mil.sq[1].task,mil.naval", "--from", 100,
                                   "--to", 250), 0)
        self.assertEqual(self.said.splitlines()[1:], ["  at 100: mil.naval=1 mil.sq[1].task=FORM",
                                                      "     200  mil.sq[1].task FORM -> ATTACK"])
        self.assertEqual(self.run_("timeline", rec, "--field", "job.kind"), 0)    # every row of the seat's
        self.assertEqual(self.said.splitlines()[1:], ["     100  job[12].kind NONE -> SQUAD"])
        self.assertEqual(self.run_("timeline", rec, "--field", "mil.dry"), 0)
        self.assertIn("no change of these fields", self.said)

    # ---- why: THE REASON TRACE, from the log beside the recording ----
    WHY = ("impai: pulse seat=1 t=7 built=0\n"
           "impai: why seat=1 t=7 gates=UNJUDGED naval=-1 pos=- need=- veto=- take=-\n"
           "impai: why seat=2 t=9 gates=- naval=0 pos=- need=factory veto=- take=factory\n"
           "impai: why seat=1 t=99 gates=HOLD+M_STALL naval=1 pos=island need=factory+sea+aa "
           "veto=factory:HOLD+M_STALL take=aa\n"
           "impai: why seat=1 t=199 gates=HOLD naval=1 pos=island+pressed need=factory+sea veto=factory:HOLD,sea:HOLD "
           "take=-\n"
           "impai: why seat=1 t=299 gates=- naval=1 pos=island need=factory+sensor veto=- take=factory\n")

    def traced(self, text=None, **kw):
        rec = self.game(**kw)
        rec.with_suffix(".tagpu.log").write_text(self.WHY if text is None else text)
        return rec

    def test_why_at_a_tick_names_each_wants_gates_and_since_when(self):
        rec = self.traced()
        self.assertEqual(self.run_("why", rec, "--at", 150), 0)
        self.assertEqual(self.said.splitlines(), [
            "why: a.imprec (build abc1234, Test Map, ticks to 400), seat 1 (Core, ImpAI)",
            "  tick 150: the trace last changed at tick 100 (the log's t=99); 4 `why` lines in a.tagpu.log; each row: "
            "the ticks it stands, from..until",
            "  gates up     100..200  HOLD, M_STALL",
            "  posture      100..200  naval 1 (a naval map); island",
            "  want_factory 100..200  needed, VETOED by HOLD+M_STALL",
            "  want_sea     100..200  needed, held by no gate, not taken: no builder of the seat's offers a sea plant",
            "  want_tech    8..end    not needed",
            "  want_def     8..end    not needed",
            "  want_aa      100..200  needed, taken",
            "  want_sensor  8..300    not needed"])
        # a line stamped t=99 stands from tick 100, not at 99; before the first line nothing is traced
        self.assertEqual(self.run_("why", rec, "--at", 99), 0)
        self.assertIn("the trace last changed at tick 8 (the log's t=7)", self.said)
        self.assertIn("gates up     8..100  UNJUDGED", self.said)
        self.assertIn("naval -1 (not judged); no flag", self.said)
        self.assertEqual(self.run_("why", rec, "--at", 7), 0)
        self.assertIn("no economy decision traced yet (the seat's first `why` line stands from tick 8)", self.said)
        self.assertEqual(self.run_("why", rec, "--at", 300, "--seat", 2), 0)     # another seat's lines alone
        self.assertIn("want_factory 10..end  needed, taken", self.said)

    def test_why_at_a_tick_quotes_the_last_plantwalk_line(self):
        walk = ("impai: plantwalk seat=2 t=9 type=CORVP builder=3/1 exit=grew sea=-\n"
                "impai: plantwalk seat=1 t=120 type=CORVP builder=1/41 grow=2 beyond=30 exit=left sea=-\n"
                "impai: plantwalk seat=1 t=180 type=CORVP builder=1/41 grow=2 beyond=31 exit=left sea=-\n")
        rec = self.traced(self.WHY + walk)
        self.assertEqual(self.run_("why", rec, "--at", 150), 0)
        self.assertEqual(self.said.splitlines()[-1],
                         "  the plant rung last ordered nothing at tick 121: type=CORVP builder=1/41 grow=2 beyond=30 "
                         "exit=left sea=-")
        self.assertEqual(self.run_("why", rec, "--at", 120), 0)             # stamped t=120: it stands from tick 121
        self.assertNotIn("plant rung", self.said)
        self.assertEqual(self.run_("why", rec, "--want", "factory", "--at", 150), 0)
        self.assertNotIn("plant rung", self.said)

    def test_why_a_wants_history_change_by_change(self):
        rec = self.traced()
        self.assertEqual(self.run_("why", rec, "--want", "want_factory"), 0)
        self.assertEqual(self.said.splitlines()[1:], [
            "  want_factory over ticks 8..400, 3 changes: taken 100 ticks, vetoed 200 (HOLD 200, M_STALL 100), held by "
            "its last term 0, not needed 92",
            "       8  not needed",
            "     100  needed, VETOED by HOLD+M_STALL",
            "     200  needed, VETOED by HOLD",
            "     300  needed, taken"])
        self.assertEqual(self.run_("why", rec, "--want", "sea", "--at", 250, "--from", 150), 0)
        self.assertEqual(self.said.splitlines()[1:], [
            "  tick 250: the trace last changed at tick 200 (the log's t=199); 4 `why` lines in a.tagpu.log; each row: "
            "the ticks it stands, from..until",
            "  want_sea     200..300  needed, VETOED by HOLD",
            "  want_sea over ticks 8..400, 3 changes: taken 0 ticks, vetoed 100 (HOLD 100), held by its last term 100, "
            "not needed 192",
            "  at 150: needed, held by no gate, not taken: no builder of the seat's offers a sea plant (from tick 100)",
            "     200  needed, VETOED by HOLD",
            "     300  not needed"])
        self.assertEqual(self.run_("why", rec, "--want", "factory", "--max", 3), 0)
        self.assertEqual(len(self.said.splitlines()), 3)
        self.assertIn("more lines cut at --max 3", self.said)

    def test_why_says_what_it_cannot_read(self):
        rec = self.game()
        self.assertEqual(self.run_("why", rec, "--at", 100), 2)                  # no log beside the recording
        self.assertIn("no log to read the reason trace from", self.said)
        rec = self.traced("impai: pulse seat=1 t=7 built=0\n")
        self.assertEqual(self.run_("why", rec, "--at", 100), 2)
        self.assertIn("no `impai: why` line for seat 1", self.said)
        rec = self.traced(self.WHY + "impai: why seat=1 t=50 gates=- naval=1 pos=- need=- veto=- take=-\n")
        self.assertEqual(self.run_("why", rec, "--at", 100), 2)
        self.assertIn("go back from GameTime 299 to 50", self.said)
        rec = self.traced()
        self.assertEqual(self.run_("why", rec), 2)
        self.assertIn("--at T (the trace at a tick), --want NAME", self.said)
        self.assertEqual(self.run_("why", rec, "--want", "want_navy"), 2)
        self.assertIn("the trace's wants are factory, sea, tech, def, aa, sensor", self.said)
        self.assertEqual(self.run_("why", rec, "--at", 401), 2)
        self.assertIn("ticks are 0 to 400", self.said)
        other = self.tmp / "elsewhere.log"
        other.write_text(self.WHY)
        rec.with_suffix(".tagpu.log").unlink()
        self.assertEqual(self.run_("why", rec, "--at", 150, "--log", other), 0)  # --log names it
        self.assertIn("4 `why` lines in elsewhere.log", self.said)

    def test_squads(self):
        self.assertEqual(self.run_("squads", self.game()), 0)
        lines = self.said.splitlines()
        self.assertTrue(lines[1].startswith("  1 FORM, 1 ATTACK;"))
        self.assertRegex(lines[2], r"^ +100-200 +sq\[1\] FORM +SEA +n=2\.\.3 +v=0 *$")
        self.assertRegex(lines[3], r"^ +200-300 +sq\[1\] ATTACK +SEA +n=3 +v=250 +goal \(14,36\) slot 21 of seat 2$")
        self.assertEqual(len(lines), 4)
        # a goal changed inside one task is a leg of it, not another task; a task held at the end has no end tick
        more = [self.put("mil", 1, "sq[0].task", 2), self.put("mil", 1, "sq[0].tx", 5), ["tick", 350],
                self.put("mil", 1, "sq[0].tx", 6), self.put("mil", 1, "sq[0].target", 60000)]
        self.assertEqual(self.run_("squads", self.game("b.imprec", extra=more), "--from", 250), 0)
        self.assertRegex(self.said.splitlines()[-1], r"^ +300-end +sq\[0\] ATTACK +LAND +n=0 +v=0 +"
                                                     r"goal \(5,0\) slot 0; \(6,0\) slot 60000 from 350$")

    def test_units(self):
        self.assertEqual(self.run_("units", self.game(), "--every", 100), 0)
        rows = [ln.split() for ln in self.said.splitlines()[3:-1]]
        # seat 1 holds slot 11 and from 100 slot 12; seat 2 holds 21 and 22 until 21 dies at 300
        self.assertEqual(rows, [["100", "2", "2"], ["200", "2", "2"], ["300", "2", "1"], ["400", "2", "1"]])
        self.assertIn("most at once: seat 1 2 at 100, seat 2 2 at 0", self.said)
        ex = think.extract(self.tmp / "a.imprec")
        self.assertEqual([ex.seat_of(s) for s in (0, 1, 10, 11, 20, 21, 101)], [None, 0, 0, 1, 1, 2, None])
        self.assertEqual(ex.alive(ticks=[99, 299, 300])[1][2], [2, 2, 1])

    def test_diff_names_the_first_tick_and_the_fields(self):
        a = self.game()
        self.assertEqual(self.run_("diff", a, self.game("same.imprec")), 0)
        self.assertIn("  EQUAL at every tick to 400: mil, brain, job, known", self.said)
        # the second game's squad forms with another member count, and from there the two differ
        b = self.game("b.imprec", change={16: self.put("mil", 1, "sq[1].n", 4)})
        self.assertEqual(self.run_("diff", a, b), 1)
        s = self.said.splitlines()
        self.assertEqual(s[2], "  FIRST DIFFERENCE at tick 100, written by the step from GameTime 99 (mil, brain, job, "
                               "known equal at every tick before it), 1 fields:")
        self.assertEqual(s[3], "    mil.sq[1].n  A 2  |  B 4")
        self.assertEqual(self.run_("diff", a, b, "--part", "brain,job"), 0)       # the parts asked for alone
        # another seat's rows and columns are not this seat's state
        c = self.game("c.imprec", extra=[self.put("known", 5, "seat[2].how", 1), self.put("job", 25, "kind", 1)])
        self.assertEqual(self.run_("diff", a, c), 0)
        d = self.game("d.imprec", extra=[self.put("known", 5, "seat[1].how", 1)])
        self.assertEqual(self.run_("diff", a, d), 1)
        self.assertIn("    known[5].how  A 0  |  B OWN", self.said)
        # a game that ends at another tick is no equal game
        e = self.game("e.imprec", end=450)
        self.assertEqual(self.run_("diff", a, e), 1)
        self.assertIn("equal at every tick to 400, where the shorter ends: A to 400, B to 450", self.said)
        self.assertEqual(think.state_parting(a, self.game("same2.imprec"), cache=self.cache),
                         (None, "state equal at every tick to 400"))
        parted, said = think.state_parting(a, b, cache=self.cache)
        self.assertEqual(parted, (100, "mil.sq[1].n", "2", "4"))
        self.assertEqual(said, "state parts in the step from GameTime 99: mil.sq[1].n 2 | 4")

    def test_another_layouts_recording_is_refused_by_name_and_read_by_its_tree(self):
        a, w = self.game(), self.game("wide.imprec", lay=self.wlay)
        self.assertEqual(self.run_("state", w, "--at", 250), 2)
        self.assertRegex(self.said, r"^think: COULD NOT RUN: wide.imprec \(build abc1234\) was made by another layout "
                                    r"and is refused: its s_brain rows are 48 bytes, IMPAI_BRAIN is 44 in \S+ -- name "
                                    r"the source tree that made it with --tree DIR\n$")
        self.assertEqual(self.run_("squads", w, "--tree", self.wide), 0)
        self.assertEqual(self.run_("state", w, "--at", 250, "--part", "brain", "--tree", self.wide), 0)
        self.assertIn("brain: sea_refused=2 nfmem=0 room=0 held=[0,0,0,0,0,0,9,0]", self.said)
        # across the two layouts the fields both name are compared, each read at its own offset
        self.assertEqual(self.run_("diff", a, w), 2)
        self.assertEqual(self.run_("diff", a, w, "--tree", self.wide), 0)
        self.assertIn("  not compared (brain, B alone): brain.room", self.said)
        self.assertIn("  EQUAL at every tick to 400", self.said)
        w2 = self.game("wide2.imprec", lay=self.wlay, extra=[self.put("brain", 1, "held[0][3]", 1, self.wlay)])
        self.assertEqual(self.run_("diff", a, w2, "--tree", self.wide), 1)
        self.assertIn("    brain.held[0][3]  A 0  |  B 1", self.said)

    # ---- recorded data is data ----
    def test_a_malformed_recording_is_a_message(self):
        rec = self.game()
        for what, events in (("a write past", [["span", "mil", 10 * self.lay.size["mil"] - 1, "0102"]]),
                             ("sized", [["size", "job", think.TABLE_MAX + 1]])):
            bad = self.game(f"{what.split()[-1]}.imprec", extra=events)
            self.assertEqual(self.run_("timeline", bad, "--field", "mil.naval,job.kind"), 2, what)
            self.assertRegex(self.said, rf"^think: COULD NOT RUN: .*{what}")
        junk = self.tmp / "junk.imprec"
        junk.write_text("{ not a game")
        self.assertEqual(self.run_("units", junk), 2)
        self.assertIn("think: COULD NOT RUN:", self.said)
        self.assertIn("not a recording this tool reads", self.said)
        self.assertEqual(self.run_("units", self.tmp / "none.imprec"), 2)
        # the real reader on bytes that are no recording: its refusal is the message
        with mock.patch.object(think, "stream", REAL_STREAM):
            self.assertEqual(self.run_("units", junk), 2)
        self.assertIn("not a recording this tool reads", self.said)
        # a table shorter than the seat's row reads as no row, a slot or seat past the recording is refused
        short = self.game("short.imprec", change={1: ["size", "mil", self.lay.size["mil"]]},
                          extra=[])
        self.assertEqual(self.run_("timeline", short, "--field", "mil.naval"), 2)   # its writes are past the table
        for argv, why in ((("state", rec, "--at", 5, "--seat", 7), "no seat 7"),
                          (("state", rec, "--at", 5, "--fields", "job[99999].kind"), "slots 0.."),
                          (("state", rec, "--at", 999), "its ticks are 0..400"),
                          (("state", rec, "--at", 5, "--fields", "mil[3].naval"), "the seat's one row"),
                          (("state", rec, "--at", 5, "--part", "mind"), "--part mind"),
                          (("timeline", rec, "--field", "mil.navl"), "near it: mil.naval"),
                          (("timeline", rec, "--field", "plan.x"), "a path starts with one of")):
            self.assertEqual(self.run_(*argv), 2, argv)
            self.assertIn(why, self.said)
        empty = self.game("norow.imprec", change={k: ["size", p, 0] for k, p in enumerate(think.PARTS, 1)},
                          extra=[])
        ex = think.extract(empty)
        with self.assertRaises(think.ThinkError):
            think.Image(ex, "mil").to(400)

    def test_a_recording_without_a_region_says_so(self):
        doc = json.loads(self.game().read_text())
        del doc["head"]["stride"]["known"]
        doc["head"]["players"][2]["ai"] = 1                                       # and two ImpAI seats: --seat is asked
        f = self.tmp / "old.imprec"
        f.write_text(json.dumps(doc))
        self.assertEqual(self.run_("state", f, "--at", 5), 2)
        self.assertIn("--seat S, one of its seats", self.said)
        self.assertEqual(self.run_("state", f, "--at", 5, "--seat", 1, "--part", "known"), 2)
        self.assertIn("carries no region src/tagpu_impai.c:s_known", self.said)
        self.assertEqual(self.run_("state", f, "--at", 250, "--seat", 1, "--part", "mil"), 0)

    # ---- the extract ----
    def test_the_extract_is_kept_and_a_bad_entry_is_made_again(self):
        rec = self.game("kept.imprec", extra=[["unit", 13, 4]])
        cache = self.tmp / "cache"
        first = think.extract(rec, cache)
        entries = list(cache.glob("rec-*.bin"))
        self.assertEqual(len(entries), 1)
        with mock.patch.object(think, "stream", side_effect=AssertionError("read again")):
            again = think.extract(rec, cache)
        self.assertEqual((again.end, again.stride, again.players, list(again.uslots), bytes(again.uborn)),
                         (first.end, first.stride, first.players, list(first.uslots), bytes(first.uborn)))
        for p in think.PARTS:
            self.assertEqual((list(again.parts[p].offs), bytes(again.parts[p].blob)),
                             (list(first.parts[p].offs), bytes(first.parts[p].blob)))
        for bad in (b"", b"junk", entries[0].read_bytes()[:-9]):
            entries[0].write_bytes(bad)
            self.assertEqual(think.extract(rec, cache).end, 400)                  # read from the recording again
            self.assertNotEqual(entries[0].read_bytes(), bad)
        # another recording's bytes are another entry
        think.extract(self.game("other.imprec", extra=[["unit", 14, 4]]), cache)
        self.assertEqual(len(list(cache.glob("rec-*.bin"))), 2)


class RealTree(unittest.TestCase):
    @unittest.skipUnless(HAVE_CC, "the DLL's compiler is not installed")
    def test_this_trees_layout_is_the_headers(self):
        lay = think.Layout(thinklayout.load(think.TREE), think.TREE)
        self.assertEqual(lay.ctype, {"mil": "IMPAI_MIL", "brain": "IMPAI_BRAIN", "job": "IMPAI_JOB",
                                     "known": "IMPAI_ROW"})
        self.assertEqual(lay.size["job"], 40)                 # impai.h's own _Static_assert
        mil = {f.name: f for f in lay.fields("mil")}
        for name in ("naval", "dry", "sq[3].task", "sq[0].target", "exec.npend"):
            self.assertIn(name, mil)
        for m in think.SQUAD_MEMBERS:                         # what `squads` reads of a squad
            self.assertIn(f"sq[0].{m}", mil)
        self.assertEqual(mil["sq[0].task"].names[0], "FREE")
        self.assertIn("ATTACK", mil["sq[0].task"].names.values())
        self.assertIn("sea_refused", {f.name for f in lay.fields("brain")})
        kind = next(f for f in lay.fields("job") if f.name == "kind")
        self.assertEqual((kind.names[0], kind.show(bytes([4]))), ("NONE", "SQUAD"))
        self.assertEqual(lay.fields("known")[0].name, "seat[0].type")
        # every name ENUM_OF and BITS_OF give is one the header has
        enumerators = {n for g in lay.lay["enums"] for n in g}
        self.assertLessEqual(set(think.ENUM_OF.values()), enumerators)
        self.assertLessEqual({n for ns in think.BITS_OF.values() for n in ns}, set(lay.macros))

    def test_short_names(self):
        self.assertEqual(think.short_names({"IMPAI_SQ_FREE": 0, "IMPAI_SQ_FORM": 1, "IMPAI_SQ_TASKS": 2}),
                         {0: "FREE", 1: "FORM", 2: "TASKS"})
        self.assertEqual(think.short_names({"TASIM_LAYER_LAND": 0, "TASIM_LAYER_SEA": 1, "TASIM_LAYERS": 2}),
                         {0: "LAND", 1: "SEA", 2: "TASIM_LAYERS"})
        self.assertEqual(think.short_names({"EST_START": 0, "EST_STRUCT": 1, "EST_KEPT": 2}),
                         {0: "START", 1: "STRUCT", 2: "KEPT"})

    def test_packed_and_capped(self):
        lines = think.packed("mil: ", ["a=1"] * 60)
        self.assertTrue(all(len(ln) <= think.WIDTH for ln in lines) and len(lines) == 3)
        self.assertTrue(lines[1].startswith("     a=1"))
        self.assertEqual(think.capped(["x"] * 5, 5), ["x"] * 5)
        self.assertEqual(len(think.capped(["x"] * 9, 5)), 5)
        self.assertIn("5 more lines cut at --max 5", think.capped(["x"] * 9, 5)[-1])


if __name__ == "__main__":
    unittest.main()
