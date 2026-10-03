#!/usr/bin/env python3
"""Tests of the replay harness's own logic (tafeat.py, tareplay.py's landing tier, tarec.py's
segments): the C parser against the coverage build's map, the features' regions, the comment
filter, `landing --check`, a recording's segments and chains, read from files built from the
format alone, the fixtures' takeover entries and the W lane's edited header copies. Standard
library only; no game runs.

    python3 tools/replay/test_tareplay.py
"""
import io
import json
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from unittest.mock import patch
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import tafeat  # noqa: E402
import tarec  # noqa: E402
import tareplay  # noqa: E402

MAPS = [tafeat.DDRAW / "ddraw-cover.map", tafeat.DDRAW / "ddraw-stocklimits-cover.map"]


class RewindPolicy(unittest.TestCase):
    def compare(self, key, rewind, puppet=False):
        meta = (0, (1 << len(tarec.COUNT_SINKS)) - 1, 1)
        samples = [(10, {}, meta, {}), (20, {} if puppet else {key: 100}, meta, {}),
                   (10, {}, meta, {}), (20, {key: 100} if puppet else {}, meta, {})]
        out = io.StringIO()
        with patch.object(tarec, "header_only", return_value={}), \
             patch.object(tarec, "counts_of", side_effect=samples), \
             patch.object(tarec, "absent_rows", return_value=((5, 2), [])), \
             redirect_stdout(out):
            result = tarec.cmd_compare_counts("rec", "a", "b", "c", "d", "rows", rewind)
        return result, out.getvalue()

    def test_only_rewind_vm_children_are_accepted(self):
        for caller in (0x4B18C0, 0x4B192D):
            key = (0x4B08C0, caller, 123)
            self.assertEqual(self.compare(key, False)[0], 1)
            result, report = self.compare(key, True)
            self.assertEqual(result, 0)
            self.assertIn("accepted rewind COB child", report)
        for key in ((0x4B08C0, 0x80001000, 123), (0x4B08C0, 0x123456, 123),
                    (0x420A30, 0x4B18C0, 0), (0x4861B3, 0x4B18C0, 0)):
            self.assertEqual(self.compare(key, True)[0], 1)
        self.assertEqual(self.compare((0x480B57, 0, 0), True, puppet=True)[0], 1)

    def test_seated_sight_remains_exact(self):
        from types import SimpleNamespace
        def world():
            return SimpleNamespace(lists={}, players=[bytearray(tarec.REC_PLAYER_IMAGE)
                for _ in range(tarec.REC_PLAYERS)], sight={0: bytearray(b"a"), 4: bytearray(b"b")},
                ctl=[], view=[0, 0], units={})
        a, b = world(), world()
        b.sight[0] = bytearray(b"c")
        b.sight.pop(4)
        h = {"players": [{"index": 0, "controller": 1}]}
        tarec.fork_view(a, b, h, h)
        self.assertNotIn(4, a.sight)
        self.assertNotIn(4, b.sight)
        self.assertNotEqual(a.sight[0], b.sight[0])


class Parser(unittest.TestCase):
    def test_blank_keeps_lines(self):
        text = 'a /* { */ "}" \'{\' // }\n#define X {\\\n }\nint f(void) { return 0; }\n'
        b = tafeat.blank(text)
        self.assertEqual(b.count("\n"), text.count("\n"))
        self.assertNotIn("{ */", b)
        self.assertEqual(b.count("{"), 1)

    def test_functions(self):
        text = ("static int t[] = { 1, 2 };\n"
                "struct s { int a; };\n"
                "static void a(int x)\n{\n    if (x) { x++; }\n}\n"
                "int __cdecl b(void* E) { return 0; }\n")
        self.assertEqual(tafeat.functions(text), [("a", 3, 6), ("b", 7, 7)])

    def test_blank_comments_keeps_code(self):
        text = '#define A 1 /* x */\n"s/*t" // c\nint x; /* y */\n'
        self.assertEqual(tafeat.blank_comments(text),
                         '#define A 1        \n"s/*t"     \nint x;        \n')

    def test_builds(self):
        text = ("a\n#ifndef TAGPU_LIMITS_STOCK\nb\n#else\nc\n#endif\n"
                "#if defined(TAGPU_LIMITS_STOCK)\nd\n#endif\n#ifdef OTHER\ne\n#endif\n")
        b = tafeat.builds(text)
        self.assertEqual([sorted(b[i]) for i in (1, 3, 5, 8, 11)],
                         [["raised", "stock"], ["raised"], ["stock"], ["stock"],
                          ["raised", "stock"]])

    @unittest.skipUnless(all(m.exists() for m in MAPS), "no coverage build (make COVER=1)")
    def test_every_parsed_function_is_in_a_map(self):
        mapped = set()
        for m in MAPS:
            mapped |= set(tafeat.read_map(m))
        for rel in tareplay.covered_sources():
            for name, _, _ in tafeat.functions((tafeat.DDRAW / rel).read_text()):
                self.assertIn((rel, name), mapped, f"{rel}: {name} is in no map")


class Features(unittest.TestCase):
    def test_regions(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "x.c"
            p.write_text('#include "tagpu_simreg.h"\n'
                         'SIM_FEATURE(one, SIM_PUPPET_RUNS, "battle");\n'
                         'static int a;\n'
                         'SIM_FEATURE(two, SIM_PUPPET_OWNER, "battle naval-push");\n'
                         'void f(void) { }\n')
            feats, problems = tafeat.features_of(p, "src/x.c")
            self.assertEqual(problems, [])
            self.assertEqual([(f["name"], f["line"], f["end"]) for f in feats],
                             [("one", 2, 3), ("two", 4, 5)])
            self.assertEqual(tafeat.feature_at(feats, "src/x.c", 5)["name"], "two")
            self.assertIsNone(tafeat.feature_at(feats, "src/x.c", 1))

    def test_unfeatured_and_malformed(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "x.c"
            p.write_text('static int a;\nSIM_FEATURE(one, SIM_PUPPET_RUNS,\n "battle");\n'
                         'void f(void) { }\n')
            _, problems = tafeat.features_of(p, "src/x.c")
            kinds = sorted(q.split()[0] for q in problems)
            self.assertEqual(kinds, ["MALFORMED", "UNFEATURED"])


def sh(cwd, *a):
    subprocess.run(a, cwd=cwd, check=True, capture_output=True, text=True)


class HeaderCopy(unittest.TestCase):
    """W's refused copies: one header entry edited, every block still read whole."""

    @staticmethod
    def block(kind, payload, stored):
        body = payload if stored else zlib.compress(payload)
        head = struct.pack("<IHHIIIII", tarec.REC_BLOCK_MAGIC, kind, tarec.RBF_STORED if stored
                           else 0, 1, 9, len(payload), len(body), zlib.crc32(body))
        return head + struct.pack("<I", zlib.crc32(head)) + body

    def test_edit(self):
        def entry(tag, data):
            raw = struct.pack("<HH", tag, len(data)) + data
            return raw + b"\0" * (-len(raw) % 4)
        layout, smap = b"ab" * 32 + b"\0", b"Coast To Coast\0"
        head = entry(tarec.RT_LAYOUT, layout) + entry(tarec.RT_SETTINGS_MAP, smap)
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "a.imprec", Path(d) / "b.imprec"
            src.write_bytes(tarec.REC_MAGIC + self.block(tarec.RB_HEADER, head, False)
                            + self.block(tarec.RB_STREAM, b"\1\2\3\4", True))
            tareplay.header_copy(src, dst, tarec.RT_SETTINGS_MAP, tareplay.no_such_map)
            got, why = tarec.blocks(dst)
            self.assertIsNone(why)
            self.assertEqual(got[1], (tarec.RB_STREAM, 1, 9, b"\1\2\3\4"))
            ents = dict(tarec.entries(got[0][3]))
            self.assertEqual(ents[tarec.RT_SETTINGS_MAP], b"Q" * 14 + b"\0")
            self.assertEqual(ents[tarec.RT_LAYOUT], layout)
            tareplay.header_copy(src, dst, tarec.RT_LAYOUT, tareplay.other_layout)
            ents = dict(tarec.entries(tarec.blocks(dst)[0][0][3]))
            self.assertEqual(ents[tarec.RT_LAYOUT], b"1" + layout[1:])


class TakeoverRoster(unittest.TestCase):
    """A cached loader roster and a partial new dump cannot supply the takeover's order."""

    class Log:
        def __init__(self, lines):
            self.lines = lines

        def wait(self, rx, timeout, start=0):
            return next((line for line in self.lines[start:] if re.search(rx, line)), None)

    old = "  u001 armck own=0 idx=99 world=(1,2,3) screen=(4,5) nano=0.00\r"
    mark = "replay: battle: taken over at GameTime 1441 as player 0, difficulty 0\r"
    new = "  u001 armck own=0 idx=344 world=(4260,1170,85) screen=(4388,1159) nano=0.00\r"
    head = "units: alive=1 onscreen=0 eye=(0,0) me=0\r"

    def test_only_the_roster_after_the_acknowledgement_is_used(self):
        log = self.Log([self.old, self.head, self.mark, self.head, self.new, self.head])
        self.assertEqual(tareplay.takeover_roster(log, 1441, 0),
                         [{"type": "armck", "owner": 0, "engine_index": 344,
                           "world": [4260, 1170]}])

    def test_the_old_roster_does_not_complete_the_wait(self):
        with self.assertRaisesRegex(tareplay.CouldNotRun, "no roster after takeover"):
            tareplay.takeover_roster(self.Log([self.old, self.head, self.mark, self.head]),
                                     1441, 0)

    def test_a_partial_dump_does_not_complete_the_wait(self):
        with self.assertRaisesRegex(tareplay.CouldNotRun, "roster is incomplete"):
            tareplay.takeover_roster(self.Log([self.old, self.head, self.mark, self.new]),
                                     1441, 0)


class Landing(unittest.TestCase):
    """A throwaway repository: the comment filter and --check's three answers."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        sh(self.root, "git", "init", "-q", "-b", "main")
        for key in ("user.name", "user.email"):     # the enclosing repository's identity
            value = subprocess.run(["git", "-C", str(HERE), "config", key], check=True,
                                   capture_output=True, text=True).stdout.strip()
            sh(self.root, "git", "config", key, value)
        (self.root / "tools" / "replay").mkdir(parents=True)
        (self.root / "tools" / "replay" / "x.c").write_text("int f(void)\n{\n    return 1;\n}\n")
        (self.root / "README").write_text("r\n")
        sh(self.root, "git", "add", "-A")
        sh(self.root, "git", "commit", "-q", "-m", "base")
        sh(self.root, "git", "checkout", "-q", "-b", "work")
        self.saved = tareplay.TREE
        tareplay.TREE = self.root

    def tearDown(self):
        tareplay.TREE = self.saved
        self.tmp.cleanup()

    def commit(self, path, text):
        (self.root / path).write_text(text)
        sh(self.root, "git", "commit", "-qam", path)

    def test_comment_only_changes_nothing(self):
        self.commit("tools/replay/x.c", "int f(void)\n{\n    return 1; /* one */\n}\n")
        self.assertEqual(tareplay.changed_lines("tools/replay/x.c", "main"), set())
        self.commit("tools/replay/x.c", "#define K 2\nint f(void)\n{\n    return 1; /* one */\n}\n")
        self.assertEqual(tareplay.changed_lines("tools/replay/x.c", "main"), {1})
        self.commit("tools/replay/x.c", "int f(void)\n{\n    return 1; /* one */\n}\n")
        self.commit("tools/replay/x.c", "int f(void)\n{\n    return 2; /* one */\n}\n")
        self.assertEqual(tareplay.changed_lines("tools/replay/x.c", "main"), {3})

    def test_check(self):
        self.commit("README", "r2\n")
        self.assertEqual(tareplay.landing_check("main"), 0)       # nothing gated
        self.commit("tools/replay/x.c", "int f(void)\n{\n    return 2;\n}\n")
        self.assertEqual(tareplay.landing_check("main"), 1)       # no note
        sh(self.root, "git", "notes", "append", "-m",
           "replay-roundtrip: dll=x range=main..y features=- fixtures=- seeds=- reached=1/1 "
           "result=pass date=2026-10-01", "HEAD")
        self.assertEqual(tareplay.landing_check("main"), 0)
        self.commit("README", "r3\n")
        self.assertEqual(tareplay.landing_check("main"), 0)       # nothing gated since
        self.commit("tools/replay/x.c", "int f(void)\n{\n    return 3;\n}\n")
        self.assertEqual(tareplay.landing_check("main"), 1)       # gated since the note


# ---- a recording's segments, from the format alone (tagpu_recfmt.h) -----------------------------

def tlv(tag, data):
    """A header or end entry, or a record: u16 tag or kind, u16 length, the bytes, to a dword."""
    return struct.pack("<HH", tag, len(data)) + data + bytes(-len(data) % 4)


def block(kind, first, last, payload):
    """A stored block: its header with both CRCs, then the payload."""
    head = struct.pack("<IHHIIIII", tarec.REC_BLOCK_MAGIC, kind, tarec.RBF_STORED, first, last,
                       len(payload), len(payload), zlib.crc32(payload))
    return head + struct.pack("<I", zlib.crc32(head)) + payload


def header(timeline=None, slots=4, seats=((0, 1), (1, 2)), viewed=0):
    """A header with what World and the chain's checks read: a 2 x 2 grid, so a 2-byte explored
    map, the seats and the options, and RT_TIMELINE for a fork."""
    p = tlv(tarec.RT_FORMAT, struct.pack("<I", tarec.REC_FORMAT))
    p += tlv(tarec.RT_MODE, struct.pack("<I", 2))
    p += tlv(tarec.RT_SLOTS, struct.pack("<I", slots))
    p += tlv(tarec.RT_FEATURES, struct.pack("<IIIII", 2, 2, 8, 0, 0))
    p += tlv(tarec.RT_PROJECTILES, struct.pack("<I", 4))
    if timeline:
        p += tlv(tarec.RT_TIMELINE, struct.pack("<III", *timeline))
    for index, controller in seats:
        p += tlv(tarec.RT_PLAYER, bytes([index, controller, 0, index]) + bytes(12) +
                 bytes(0xB9) + b"seat")
    p += tlv(tarec.RT_OPTIONS, struct.pack("<IIIIHBB", 0, 0, 1, 0, len(seats), viewed, viewed))
    return block(tarec.RB_HEADER, 0, 0, p)


def stream(first, last, unit=None, opens=True):
    """Iterations first..last; when it `opens` a segment, the first carries the explored map
    whole, as a segment's first iteration does, and with `unit` a unit image in that slot."""
    p = b""
    for t in range(first, last + 1):
        p += tlv(tarec.RK_TICK, struct.pack("<I", t))
        if t == first and opens:
            if unit:
                p += tlv(tarec.RK_UNIT, struct.pack("<HH", unit, 0) + bytes(tarec.REC_UNIT_IMAGE))
            p += tlv(tarec.RK_MAPPED, struct.pack("<I", 0) + bytes(2))
    return block(tarec.RB_STREAM, first, last, p)


def end_block(last, reason=1):
    return block(tarec.RB_END, last, last, tlv(tarec.RT_END_REASON, struct.pack("<I", reason)) +
                 tlv(tarec.RT_LAST, struct.pack("<I", last)))


class Segments(unittest.TestCase):
    """tarec's segments, chains and loads, as the DLL's reader takes them (tagpu_recreader.c)."""

    ORIGINAL = (header(), stream(1, 3, unit=1), stream(4, 6, opens=False), end_block(6))

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.n = 0

    def tearDown(self):
        self.tmp.cleanup()

    def file(self, *parts):
        self.n += 1
        p = Path(self.tmp.name) / f"r{self.n}.imprec"
        p.write_bytes(tarec.REC_MAGIC + b"".join(parts))
        return str(p)

    def segs(self, path):
        return tarec.file_segments(path)[0]

    def test_one(self):
        path = self.file(*self.ORIGINAL)
        self.assertEqual([(s["id"], s["first"], s["last"], s["end_reason"])
                          for s in self.segs(path)], [(0, 1, 6, 1)])
        h, bl, stop = tarec.load(path)
        self.assertEqual((len(bl), stop), (4, None))
        self.assertEqual(tarec.stream_to(h, bl).tick, 6)

    def test_two_and_three(self):
        fork1 = (header((1, 0, 4), seats=((0, 2), (1, 1)), viewed=1), stream(4, 5, unit=2),
                 end_block(5))
        fork2 = (header((2, 1, 5)), stream(5, 7, unit=3), end_block(7))
        path = self.file(*self.ORIGINAL, *fork1, *fork2)
        segs = self.segs(path)
        self.assertEqual([(s["id"], s["parent"], s["fork_at"], s["first"], s["last"])
                          for s in segs], [(0, 0, 0, 1, 6), (1, 0, 4, 4, 5), (2, 1, 5, 5, 7)])
        self.assertEqual(segs[1]["range"][0], segs[0]["range"][1])
        h, bl, _ = tarec.load(path)
        self.assertEqual(len(bl), 4)                    # timeline 0: the original alone
        self.assertEqual(tarec.stream_to(h, bl).tick, 6)
        self.assertEqual([s["id"] for s in tarec.chain(segs, 2)], [0, 1, 2])
        h, bl, _ = tarec.load(path, timeline=1)
        self.assertEqual([b[1] for b in bl if b[0] == tarec.RB_BOUNDARY], [4])
        w = tarec.stream_to(h, bl, want=3)              # the parent's, before the fork
        self.assertEqual((w.tick, sorted(w.units)), (3, [1]))
        w = tarec.stream_to(h, bl)                      # the child replaces the world whole
        self.assertEqual((w.tick, sorted(w.units), w.ctl[:2], w.view), (5, [2], [2, 1], [1, 1]))
        h, bl, _ = tarec.load(path, timeline=2)
        self.assertEqual([b[1] for b in bl if b[0] == tarec.RB_BOUNDARY], [4, 5])
        self.assertEqual(tarec.stream_to(h, bl, want=4).tick, 4)
        w = tarec.stream_to(h, bl)
        self.assertEqual((w.tick, sorted(w.units)), (7, [3]))
        with redirect_stdout(io.StringIO()):
            self.assertEqual(tarec.main(["tarec", "check", path, "--timeline", "2"]), 0)
            self.assertEqual(tarec.main(["tarec", "compare-fork", path, "1"]), 1)
            self.assertEqual(tarec.main(["tarec", "segments", path]), 0)
            self.assertEqual(tarec.main(["tarec", "info", path]), 0)

    def test_timeline_metadata_and_deleted_ancestor(self):
        fork1 = (header((1, 0, 4)), stream(4, 5, unit=2), end_block(5))
        fork2 = (header((2, 1, 5)), stream(5, 7, unit=3), end_block(7))
        rows = b"".join(struct.pack("<IB3xQ", tid, state, used)
                        for tid, state, used in ((0, 1, 10), (1, 2, 20), (2, 1, 30), (9, 2, 0)))
        path = self.file(*self.ORIGINAL, *fork1, *fork2,
                         block(tarec.RB_TIMELINES, 0, 0, rows))
        segs = self.segs(path)
        self.assertEqual([(s["state"], s["last_used"]) for s in segs], [(1, 10), (2, 20), (1, 30)])
        self.assertEqual(segs[0]["max_id"], 9)          # removed high ID is not reused
        with self.assertRaisesRegex(tarec.Unreadable, "deleted"):
            tarec.chain(segs, 1)
        self.assertEqual([s["id"] for s in tarec.chain(segs, 2)], [0, 1, 2])
        h, bl, _ = tarec.load(path, timeline=2)
        self.assertEqual(tarec.stream_to(h, bl).tick, 7)
        with redirect_stdout(io.StringIO()):
            self.assertEqual(tarec.cmd_info(path), 0)
        newer = struct.pack("<IB3xQ", 0, 1, 10) + struct.pack("<IB3xQ", 1, 1, 40)
        path = self.file(*self.ORIGINAL, *fork1, block(tarec.RB_TIMELINES, 0, 0, rows[:32]),
                         block(tarec.RB_TIMELINES, 0, 0, newer))
        self.assertEqual(self.segs(path)[1]["state"], 1)

    def test_bad_timeline_metadata(self):
        valid = struct.pack("<IB3xQ", 0, 1, 1)
        for rows in (b"", valid + b"x", valid * 2, struct.pack("<IB3xQ", 0, 3, 1),
                     struct.pack("<IB3xQ", 0xFFFFFFFF, 2, 0), valid[:5] + b"x" + valid[6:]):
            with self.assertRaises(tarec.Unreadable):
                self.segs(self.file(*self.ORIGINAL, block(tarec.RB_TIMELINES, 0, 0, rows)))
        with self.assertRaisesRegex(tarec.Unreadable, "no segment"):
            self.segs(self.file(*self.ORIGINAL, block(tarec.RB_TIMELINES, 0, 0,
                                                     struct.pack("<IB3xQ", 8, 1, 0))))

    def test_lru_protects_the_playing(self):
        parts = list(self.ORIGINAL)
        for tid in range(1, 4):
            parts += [header((tid, 0, 4)), stream(4, 5), end_block(5)]
        rows = b"".join(struct.pack("<IB3xQ", tid, 1, used)
                        for tid, used in ((0, 0), (1, 30), (2, 10), (3, 20)))
        segs = self.segs(self.file(*parts, block(tarec.RB_TIMELINES, 0, 0, rows)))
        self.assertEqual(tarec.fork_prediction(segs), 2)
        self.assertEqual(tarec.fork_prediction(segs, 2), 3)

    def test_prediction_refuses_child_boundary_and_capacity(self):
        segs = self.segs(self.file(*self.ORIGINAL, header((1, 0, 4)),
                                   stream(4, 6), end_block(6)))
        with self.assertRaisesRegex(tarec.Unreadable, "first iteration"):
            tarec.fork_prediction(segs, 1, 4)
        self.assertIsNone(tarec.fork_prediction(segs, 1, 5))
        self.assertIsNone(tarec.fork_prediction(segs, 0, 4))
        rows = struct.pack("<IB3xQ", 0, 1, 1) + struct.pack("<IB3xQ", 0xFFFFFFFE, 2, 0)
        segs = self.segs(self.file(*self.ORIGINAL, block(tarec.RB_TIMELINES, 0, 0, rows)))
        with self.assertRaisesRegex(tarec.Unreadable, "exhausted"):
            tarec.fork_prediction(segs, 0, 4)
        parts = list(self.ORIGINAL)
        for tid in range(1, 64):
            parts += [header((tid, 0, 4)), stream(4, 6), end_block(6)]
        segs = self.segs(self.file(*parts))
        with self.assertRaisesRegex(tarec.Unreadable, "64 timelines"):
            tarec.fork_prediction(segs, 0, 4)

    def test_torn_tail_and_open_segment(self):
        torn = block(tarec.RB_STREAM, 6, 6, b"x" * 8)[:20]
        path = self.file(*self.ORIGINAL, header((1, 0, 4)), stream(4, 5, unit=2), torn)
        self.assertEqual([(s["id"], s["last"], s["end_reason"]) for s in self.segs(path)],
                         [(0, 6, 1), (1, 5, None)])
        self.assertIn("cut short", tarec.load(path, timeline=1)[2])
        self.assertIsNone(tarec.load(path)[2])          # the original reads clean

    def test_end_no_header_follows(self):
        path = self.file(*self.ORIGINAL, stream(7, 8))
        self.assertEqual([s["id"] for s in self.segs(path)], [0])

    def test_refusals(self):
        def refused(*fork, n=1):
            path = self.file(*self.ORIGINAL, *fork)
            with self.assertRaises(tarec.Unreadable):
                tarec.chain(self.segs(path), n)
        refused(header((1, 0, 9)), stream(9, 10), end_block(10))      # past the parent's last
        refused(header((1, 0, 4)), stream(5, 6), end_block(6))        # first is not fork_at
        refused(header((1, 0, 4), slots=8), stream(4, 5), end_block(5))   # another shape
        refused(header((1, 0, 4)), stream(4, 5), end_block(5), n=3)   # no such timeline
        for parts in ((*self.ORIGINAL, header((1, 0, 4)), stream(4, 5), end_block(5),
                       header((1, 0, 4)), stream(4, 5), end_block(5)),     # an id twice
                      (*self.ORIGINAL, header((1, 2, 4)), stream(4, 5)),   # a later parent
                      (*self.ORIGINAL, header(), stream(4, 5)),        # a fork with no timeline
                      (header(), stream(1, 3), header((1, 0, 2)))):    # a header inside one
            path = self.file(*parts)
            with self.assertRaises(tarec.Unreadable):
                self.segs(path)



class TakeoverFixtures(unittest.TestCase):
    """fixtures.json's takeover entries, and the processes they make a fixture run."""

    def test_entries(self):
        fixes = tareplay.fixtures()
        took = {n: f["takeover"] for n, f in fixes.items() if "takeover" in f}
        self.assertIn("battle", took)
        self.assertIn("takeover-end", took)
        known = {"player", "order", "switches", "type", "dump", "ms", "checks"}
        for name, t in took.items():
            self.assertLessEqual(set(t), known, name)
            self.assertTrue(t["player"] == "controlled" or isinstance(t["player"], int), name)
            self.assertTrue(t["checks"], name)
            self.assertLessEqual(set(t["checks"]), set(tareplay.TAKEOVER_CHECKS), name)
            self.assertTrue((tareplay.SCENARIOS / fixes[name]["load"]).exists(), name)
            self.assertEqual(tareplay.procs(fixes[name]), ("L", "P", "R", "Q", "K", "F") +
                             (("W",) if fixes[name].get("watch") else ()) +
                             (("X",) if fixes[name].get("forks") else ()) +
                             (("U",) if fixes[name].get("rewind") else ()))
            if "order" in t:
                self.assertEqual(len(t["order"]), 2, name)
        self.assertEqual(tareplay.procs(fixes["forest-fire"]), ("L", "P", "R", "Q"))
        end = json.loads((tareplay.SCENARIOS / fixes["takeover-end"]["load"]).read_text())
        self.assertFalse(end["setup"]["clear_existing"])     # the commanders keep the economy
        self.assertIn("victory", took["takeover-end"]["checks"])
        # the GameTimes a takeover-end run draws leave T2 inside the recording
        Ts = tareplay.gametimes("takeover-end", fixes["takeover-end"]["end"], "x")
        self.assertLess(Ts[1], fixes["takeover-end"]["end"])


class AiCompare(unittest.TestCase):
    """The takeover's `ai` check against the original timeline (tareplay.ai_compare)."""

    def test_every_maker_made_again(self):
        self.assertEqual(tareplay.ai_compare({1, 2, 3}, {1, 3}, {1, 3}), ([1, 3], [], [2]))

    def test_a_maker_that_made_nothing_fails(self):
        self.assertEqual(tareplay.ai_compare({1, 2, 3}, {1, 2}, {2}), ([1, 2], [1], [3]))

    def test_a_fork_only_maker_is_skipped(self):
        self.assertEqual(tareplay.ai_compare({1, 2}, {1}, {1, 2}), ([1], [], [2]))

    def test_no_maker_in_the_original_is_no_comparison(self):
        self.assertEqual(tareplay.ai_compare({1, 2}, set(), {1}), ([], [], [1, 2]))
        self.assertEqual(tareplay.ai_compare(set(), {1}, {1}), ([], [], []))

    def test_a_seat_not_ai_in_the_fork_is_not_compared(self):
        self.assertEqual(tareplay.ai_compare({2}, {0, 2}, {2}), ([2], [], []))


if __name__ == "__main__":
    unittest.main(verbosity=1)
