#!/usr/bin/env python3
"""Tests of the replay harness's own logic (tafeat.py, tareplay.py's landing tier, tarec.py's
segments and world): the C parser against the coverage build's map, the features' regions, the
comment filter, `landing --check`, a sim file's prelude and the reach it asks of each build, a
recording's segments and chains, what the block walk refuses, its runs and snapshot chains, and
which cells are images, read from files built from the format alone, compare-fork's view of a
takeover's fresh ImpAI seat (D2 (e)), the fixtures' takeover entries, the W lane's edited header copies and its resource-bar row,
compare-counts' script slice and exact COB children, the puppet's native owner's query check, and a
multiplayer dump's sort chains (tarec.sort_chains), a worker's display under clients that come
and go (one Xvfb, started as a worker starts it), --resume's kept results on stand-in groups, NO
DRAW's lever, acknowledgement and account, and nodraweq.py's verdict. No game runs. The files are read through
reccodec.py, which needs numpy, numba and zstandard: devpy.ensure() re-executes this under the
shared venv when they are missing.

    python3 tools/replay/test_tareplay.py
"""
import hashlib
import importlib.util
import io
import json
import os
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest
import zlib
from unittest.mock import Mock, patch
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import tafeat  # noqa: E402
import reccache  # noqa: E402
import tarec  # noqa: E402
import tareplay  # noqa: E402
import tarewind  # noqa: E402

MAPS = [tafeat.DDRAW / "ddraw-cover.map", tafeat.DDRAW / "ddraw-stocklimits-cover.map"]


class KernelCompileAcrossThreads(unittest.TestCase):
    """tareplay decodes from several threads of one process: the first decodings, together, each
    find every kernel replaced once, none decorated twice."""

    def test_threads_compile_together(self):
        code = """
import sys, threading
sys.path.insert(0, sys.argv[1])
import reccodec
gate, errors = threading.Barrier(16), []
def go():
    gate.wait()
    try:
        reccodec._compile()
    except Exception as e:
        errors.append(repr(e))
threads = [threading.Thread(target=go) for _ in range(16)]
for t in threads: t.start()
for t in threads: t.join()
plain = [n for n, _ in reccodec._KERNELS if not hasattr(reccodec.__dict__[n], "py_func")]
print(errors, plain)
"""
        out = subprocess.run([sys.executable, "-c", code, str(Path(__file__).parent)],
                             capture_output=True, text=True, timeout=120)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertEqual(out.stdout.strip(), "[] []")


class CapRefusalFailsTheGame(unittest.TestCase):
    """A refusal of the GPU memory cap in a game's log fails that game by name, as a COB stop does:
    the cap is too small for it (talog.cap_refusals)."""

    def test_a_cap_refusal_in_the_log_fails_the_game(self):
        with tempfile.TemporaryDirectory() as d:
            gamedir = Path(d)
            (gamedir / "log").mkdir()
            log = gamedir / "log" / "tagpu.log"
            log.write_text("log: run R1 part 1 of tagpu.log, started\n")
            lg = tareplay.Log(SimpleNamespace(gamedir=gamedir))         # reads from the launch on
            with log.open("a") as f:
                f.write("vk: gpumem: this device's allocations are capped at 320 MiB\n")
            self.assertEqual(len(lg.read()), 1)
            with log.open("a") as f:
                f.write("vk: gpumem: 304 MiB held of the 320 MiB cap (a new high water)\n"
                        "vk: gpumem: the cap refuses an allocation of 33554432 bytes - 304 MiB held "
                        "of 320 MiB, 210 allocations\n")
            with self.assertRaisesRegex(tareplay.Failed, "the GPU memory cap refused an allocation: "
                                                         "vk: gpumem: the cap refuses an allocation of 33554432"):
                lg.read()

    REFUSAL = "vk: gpumem: the cap refuses an allocation of 33554432 bytes - 304 MiB held of 320 MiB"

    def folder(self, d):
        gamedir = Path(d)
        (gamedir / "log").mkdir()
        log = gamedir / "log" / "tagpu.log"
        log.write_bytes(b"log: run R1 part 1 of tagpu.log, started\r\n")
        return gamedir, log

    def test_the_watch_reads_every_launch_and_the_lines_after_the_last_read(self):
        """talog.CapWatch: a refusal in a game the next launch rotated away, and one in a line the
        game was still writing when it ended, are both read, each named once."""
        with tempfile.TemporaryDirectory() as d:
            gamedir, log = self.folder(d)
            caps = tareplay.talog.CapWatch(gamedir)
            with log.open("ab") as f:
                f.write(self.REFUSAL.encode() + b"\r\n")
            log.rename(gamedir / "log" / "tagpu.1.log")          # the next launch
            log.write_bytes(b"log: run R2 part 1 of tagpu.log, started\r\nvk: up\r\n")
            self.assertEqual(caps.failure("g"), "the GPU memory cap refused g an allocation: "
                                                + self.REFUSAL)
            self.assertIsNone(caps.failure("g"))
            with log.open("ab") as f:
                f.write(b"vk: up\r\n" + self.REFUSAL.encode() + b"\r\n" + self.REFUSAL.encode())
            self.assertEqual(caps.failure(), "the GPU memory cap refused an allocation (2 refusals): "
                                             + self.REFUSAL)

    def test_a_watch_begins_at_its_start(self):
        with tempfile.TemporaryDirectory() as d:
            gamedir, log = self.folder(d)
            with log.open("ab") as f:
                f.write(self.REFUSAL.encode() + b"\r\n")
            self.assertIsNone(tareplay.talog.CapWatch(gamedir).failure())

    def lane(self, gamedir, after_last_wait, lane_fails=False):
        """Run.job on a W lane whose game logs `after_last_wait` once its lane function returns or
        raises; the fixture's problems and lanes."""
        run = tareplay.Run.__new__(tareplay.Run)
        run.game_speed, run.turbo = tareplay.TEST_GAME_SPEED, tareplay.TURBO
        run.fx = {"g": {"fix": {}, "Ts": [1], "out": gamedir / "out", "rec": Path("rec"),
                        "procs": {}, "problems": []}}
        w = tareplay.Worker.__new__(tareplay.Worker)
        w.name, w.gamedir, w.last_collection, w.lane = "tareplay-test", gamedir, None, None
        w.xvfb = SimpleNamespace(display=":9")
        w.collect = w.stop = Mock()

        def lane(*a):
            with (gamedir / "log" / "tagpu.log").open("ab") as f:
                f.write(after_last_wait.encode() + b"\r\n")
            if lane_fails:
                raise tareplay.Failed("the HUD's seek: no answer")
            return []
        with patch.object(tareplay, "watch_lane", lane), patch.object(tareplay, "say"):
            run.job(w, "g", "W")
        return run.fx["g"]

    def test_a_lane_whose_game_refused_after_its_last_wait_fails_by_name(self):
        with tempfile.TemporaryDirectory() as d:
            fx = self.lane(self.folder(d)[0], self.REFUSAL)
        self.assertEqual(fx["procs"], {"W": "ran"}, "its checks still run")
        self.assertEqual(fx["problems"], [("W", "FAILED", "the GPU memory cap refused an allocation: "
                                                          + self.REFUSAL)])

    def test_a_failed_lane_names_the_refusal_beside_its_failure(self):
        with tempfile.TemporaryDirectory() as d:
            fx = self.lane(self.folder(d)[0], self.REFUSAL, lane_fails=True)
        self.assertEqual([p[2] for p in fx["problems"]],
                         ["the HUD's seek: no answer",
                          "the GPU memory cap refused an allocation: " + self.REFUSAL])

    def test_a_lane_with_no_refusal_passes(self):
        with tempfile.TemporaryDirectory() as d:
            fx = self.lane(self.folder(d)[0], "vk: gpumem: 304 MiB held of the 320 MiB cap")
        self.assertEqual((fx["procs"], fx["problems"]), ({"W": "ran"}, []))


class EffectPoolsImageGate(unittest.TestCase):
    """The engine's effect pools are not recorded: the images gate classes them as effects, in
    every recording, and stays strict one byte outside them."""
    RANGES = ((0, 0x1491B, 0x1AB8F), (0, 0x1AB9F, 0x33A0F),
              (1, 0x511DF0 - tarec.REC_DATA_BEGIN, 0x511F80 - tarec.REC_DATA_BEGIN))

    def gate(self, positions, name="stock-battle-allied"):
        names = ("l1", "lT", "n1", "n2", "p1", "pT", "r1", "q1")
        images = {name: {0: bytearray(tarec.REC_MAIN_BYTES),
                         1: bytearray(tarec.REC_DATA_BYTES)} for name in names}
        for which, at in positions:
            images["l1"][which][at] = 1
        gen = Mock()
        gen.coverage.return_value = {}
        output = io.StringIO()
        with patch.object(tarec, "dump_images", side_effect=lambda name: (
                    201 if name in ("lT", "pT") else 101, "digest", images[name])), \
                patch.object(tarec, "blocks", return_value=([(0, 0, 0, b"header")], None)), \
                patch.object(tarec, "header", return_value={"frame": 202}), \
                patch.object(tarec, "globals_list", return_value=(gen, [], [], "digest")), \
                redirect_stdout(output):
            result = tarec.cmd_images_gate(f"{name}.imprec", ("l1", "lT"), ("n1", "n2"),
                                          ("p1", "pT"), "r1", "q1")
        return result, output.getvalue()

    def test_the_ranges_are_the_ones_a_seek_clears(self):
        self.assertEqual(tarec.EFFECT_POOLS, self.RANGES)

    def test_every_pool_boundary_is_effects(self):
        positions = [(which, at) for which, lo, hi in self.RANGES for at in (lo, hi - 1)]
        for name in ("stock-battle-allied", "battle-allied", "stock-secondary-orders"):
            with self.subTest(name=name):
                result, output = self.gate(positions, name)
                self.assertEqual(result, 0, output)
                self.assertIn("PASS", output)

    def test_adjacent_bytes_and_unit_state_remain_strict(self):
        for which, lo, hi in self.RANGES:
            for at in (lo - 1, hi):
                with self.subTest(which=which, at=at):
                    result, output = self.gate([(which, lo), (which, at)])
                    self.assertEqual(result, 1)
                    self.assertIn("1 bytes FAIL", output)
        self.assertEqual(self.gate([(0, 0x1B63)])[0], 1)

    def test_the_old_exception_flag_is_gone(self):
        with redirect_stderr(io.StringIO()), redirect_stdout(io.StringIO()):
            self.assertNotEqual(tarec.main(["tarec", "images-gate", "missing.imprec",
                                            "--stock-secondary-cosmetics"]), 0)
        self.assertFalse(hasattr(tareplay, "image_gate_options"))


class OpaqueGlobals(unittest.TestCase):
    """replay-globals.tsv's `opaque` value class: the parser's rules, and the address scan passing
    over an opaque slot of the globals image while it still fails a plain one."""
    ROW = "data\t0x51FC88\t4\t-\t{carried}\t{puppet}\t{name}\t-\tevidence"

    def setUp(self):
        self.gen = tarec._generator("replay-globals-gen")

    def parse(self, *rows):
        return self.gen.parse("\n".join(rows))

    def test_opaque_is_a_globals_rows_optional_tenth_column(self):
        gen = self.gen
        rows = self.parse(self.ROW.format(carried="globals", puppet="write", name="seed") + "\topaque")
        self.assertTrue(rows[0]["opaque"])
        self.assertTrue(gen.slots(rows)[0][0]["opaque"])
        self.assertFalse(self.parse(self.ROW.format(carried="globals", puppet="write",
                                                    name="seed"))[0]["opaque"])
        for bad in (self.ROW.format(carried="globals", puppet="write", name="seed") + "\trandom",
                    self.ROW.format(carried="leave", puppet="-", name="seed") + "\topaque"):
            with self.subTest(row=bad), self.assertRaises(gen.Invalid):
                self.parse(bad)
        lo = "data\t0x51FC88\t4\t0xFFFF\tglobals\twrite\tlo\t-\tevidence\topaque"
        hi = "data\t0x51FC88\t4\t0xFFFF0000\tglobals\twrite\thi\t-\tevidence"
        with self.assertRaises(gen.Invalid):
            gen.slots(self.parse(lo, hi))

    def test_the_scan_passes_over_an_opaque_slot_only(self):
        slots = [dict(dword=0, size=4, opaque=True, names="seed"),
                 dict(dword=1, size=4, opaque=False, names="plain")]
        world = Mock(tables={}, units={}, players=[], proj=b"", phead=None,
                     globals=struct.pack("<II", 0x1800, 0x1800))
        output = io.StringIO()
        with patch.object(tarec, "header_only", return_value={"globals": "digest"}), \
                patch.object(tarec, "dump_world", return_value=({}, world)), \
                patch.object(tarec, "dump_images", return_value=(301, "digest", None)), \
                patch.object(tarec, "scan_ranges", return_value=[(0x1000, 0x2000, "the main block", 1)]), \
                patch.object(tarec, "globals_list", return_value=(None, [], slots, "digest")), \
                redirect_stdout(output):
            result = tarec.cmd_scan("fixture.imprec", "l301")
        self.assertEqual(result, 1)
        self.assertIn("globals plain holds 0x00001800", output.getvalue())
        self.assertNotIn("seed", output.getvalue())
        self.assertIn("1 VALUES THAT ARE ADDRESSES", output.getvalue())


class ListHashes(unittest.TestCase):
    """The exclusion file's and the positive list's hashes (each generator's THE FILE'S HASH and
    THE LIST'S HASH): names, writers, evidence and spelling leave them, every column that decides
    what the readers read moves them, and v0.5.1's whole-file hash is accepted only while that
    file's rows and this generator read the bytes v0.5.1's tables read, and then reads in a header
    as this checkout's hash; a generator that moves a byte fails the build. The build checks the
    frozen copy of v0.5.1's file against the hash its header carried and the frozen tables against
    what this generator renders; this checks, where git has the tag, that the copy is byte for byte
    the published file and the tables are the published header's and generator's. One of the
    acceptance's sites (each generator's ACCEPTANCE)."""
    GLOBALS = ("data\t0x51FC88\t4\t-\tglobals\twrite\tseed\t0x4B6C87\tevidence\topaque",
               "data\t0x51FC8C\t4\t0xFF\tglobals\thold\tnext\t-\tevidence",
               "data\t0x51FC90\t8\t-\tleave\t-\tclock\t-\tevidence")
    LAYOUT = ("unit\t0x0\t4\t-\tid:type\tdef\t0x401000\tevidence",
              "unit\t0x10\t2\t0xFF\tcache\tlow\t0x401000\tevidence")

    def setUp(self):
        self.gens = {"globals": tarec._generator("replay-globals-gen"),
                     "layout": tarec._generator("replay-layout-gen")}

    def digest(self, which, rows):
        gen = self.gens[which]
        return gen.semantic_digest(gen.parse("\n".join(rows)))

    @staticmethod
    def edit(rows, i, col, value, *more):
        """`rows` with row i's column col set to value, and each further (col, value) pair."""
        out = [r.split("\t") for r in rows]
        out[i][col] = value
        for c, v in zip(more[::2], more[1::2]):
            out[i][c] = v
        return ["\t".join(r) for r in out]

    def test_names_evidence_and_spelling_leave_the_hash(self):
        for which, rows, same in (
                ("globals", self.GLOBALS, [(0, 6, "rng"), (0, 7, "-"), (1, 8, "other words"),
                                           (0, 1, "0x51fc88"), (1, 3, "0xff"), (0, 3, "0xFFFFFFFF")]),
                ("layout", self.LAYOUT, [(0, 5, "type"), (1, 6, "0x401004"), (1, 7, "other words"),
                                         (1, 3, "0xff"), (0, 1, "0x00")])):
            want = self.digest(which, rows)
            self.assertEqual(self.digest(which, ["# a comment", ""] + list(rows)), want)
            for i, col, value in same:
                with self.subTest(which=which, row=i, column=col):
                    self.assertEqual(self.digest(which, self.edit(rows, i, col, value)), want)

    def test_what_is_recorded_moves_the_hash(self):
        # the where edit keeps the field's offset in its image (.data's 0x51FC88 is 0x1EC88 from
        # its start), so only where moves
        for which, rows, moved in (
                ("globals", self.GLOBALS, [(1, 1, "0x51FC8D"), (2, 2, "12"), (1, 3, "0xF0"),
                                           (2, 4, "tick"), (1, 5, "write"),
                                           (0, 0, "main", 1, "0x1EC88")]),
                ("layout", self.LAYOUT, [(1, 1, "0x12"), (1, 2, "1"), (1, 3, "0x0F"),
                                         (1, 4, "timer"), (0, 4, "id:unit"), (1, 0, "player")])):
            want = self.digest(which, rows)
            for i, col, value, *more in moved:
                with self.subTest(which=which, row=i, column=col):
                    self.assertNotEqual(self.digest(which, self.edit(rows, i, col, value, *more)),
                                        want)
        plain = [self.GLOBALS[0].rsplit("\t", 1)[0]] + list(self.GLOBALS[1:])
        self.assertNotEqual(self.digest("globals", plain), self.digest("globals", self.GLOBALS))

    def checkout(self, which):
        """This checkout's rows of `which`, and the hash v0.5.1's recordings carry for them."""
        gen = self.gens[which]
        with open(gen.TSV, encoding="utf-8") as f:
            rows = gen.parse(f.read())
        return rows, gen.published(rows)

    def test_the_frozen_copies_are_the_published_files(self):
        root = Path(__file__).resolve().parents[2]
        for which, gen in self.gens.items():
            tagged = f"{gen.PUBLISHED}:{Path(gen.TSV).relative_to(root).as_posix()}"
            try:
                published = subprocess.run(["git", "-C", str(root), "show", tagged],
                                           capture_output=True, check=True).stdout
            except (OSError, subprocess.CalledProcessError) as e:
                self.skipTest(f"git cannot show {tagged}: {e}")
            with self.subTest(which=which):
                self.assertEqual(Path(gen.PUBLISHED_TSV).read_bytes(), published)

    def test_the_frozen_tables_are_the_published_headers_and_generators(self):
        """Each frozen table is what v0.5.1 rendered: the layout's, its header's table; the
        globals', its header's slots and image size, and the opacity and rows its generator
        derived from its file."""
        root = Path(__file__).resolve().parents[2]

        def show(rel):
            try:
                return subprocess.run(["git", "-C", str(root), "show", f"v0.5.1:{rel}"],
                                      capture_output=True, check=True).stdout.decode("utf-8")
            except (OSError, subprocess.CalledProcessError) as e:
                self.skipTest(f"git cannot show v0.5.1:{rel}: {e}")

        def c_mask(expr):
            """A mask as the generators write it in C (mask_c): runs of set bits, shifted."""
            v = 0
            for term in expr.split("|"):
                m = re.fullmatch(r"\s*(?:\((0x[0-9a-fA-F]+)u << (\d+)\)|(0x[0-9a-fA-F]+)u)\s*", term)
                v |= int(m.group(1), 16) << int(m.group(2)) if m.group(1) else int(m.group(3), 16)
            return v

        lgen, ggen = self.gens["layout"], self.gens["globals"]
        entries = []
        for m in re.finditer(r'^    \{ RL_(\w+), RL_(\w+), RL_T_(\w+), (0x[0-9a-f]+), (\d+), '
                             r'([^,]+), "[^"]*" \},$', show("tagpu/ddraw/inc/tagpu_replay_layout.h"),
                             re.M):
            target = m.group(3).lower()
            entries.append((m.group(1).lower(), m.group(2).lower(),
                            "" if target == "none" else target, int(m.group(4), 16),
                            int(m.group(5)), c_mask(m.group(6))))
        self.assertTrue(entries)
        self.assertEqual(Path(lgen.PUBLISHED_TABLES).read_text(encoding="ascii"),
                         lgen.tables(entries))

        header = show("tagpu/ddraw/inc/tagpu_replay_globals.h")
        slots = [(m.group(1).lower(), int(m.group(2)), int(m.group(3)), int(m.group(4), 16),
                  c_mask(m.group(5)), c_mask(m.group(6)), c_mask(m.group(7)))
                 for m in re.finditer(r'^    \{ RG_(\w+), (\d+), (\d+), (0x[0-9a-f]+), '
                                      r'([^,]+), ([^,]+), ([^,]+), "[^"]*" \},$', header, re.M)]
        dwords = int(re.search(r"#define REC_GLOBALS_DWORDS (\d+)", header).group(1))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "replay_globals_gen_v051.py"
            path.write_text(show("tools/replay-globals-gen.py"), encoding="utf-8")
            old = importlib.util.spec_from_file_location("replay_globals_gen_v051", path)
            v051 = importlib.util.module_from_spec(old)
            old.loader.exec_module(v051)
        rows = v051.parse(Path(ggen.PUBLISHED_TSV).read_text(encoding="utf-8"))
        sl, n = v051.slots(rows)
        self.assertEqual(([(s["where"], s["dword"], s["size"], s["offset"], s["write"], s["hold"],
                            s["react"]) for s in sl], n), (slots, dwords))
        self.assertEqual(Path(ggen.PUBLISHED_TABLES).read_text(encoding="ascii"),
                         ggen.tables(sl, n, rows, v051.DATA_BEGIN))

    def test_a_generator_that_moves_a_byte_fails_the_build(self):
        """A generator edit that packs two 2-byte slots into one dword, or that emits a full mask
        where the table holds 0, renders this checkout's rows and v0.5.1's alike, so only the
        frozen tables catch it: the build's check fails, naming them."""
        gen = self.gens["globals"]
        real = gen.slots

        def packed(rows):
            sl, dwords = real(rows)
            out, saved = [], 0
            for s in sl:
                s = dict(s, dword=s["dword"] - saved)
                if out and s["size"] <= 2 and out[-1]["size"] <= 2 and \
                        out[-1]["dword"] + 1 == s["dword"] and not out[-1].get("shared"):
                    s.update(dword=out[-1]["dword"], shared=True)
                    saved += 1
                out.append(s)
            return out, dwords - saved

        rows, _ = self.checkout("globals")
        self.assertNotEqual(packed(rows)[1], real(rows)[1])
        self.assert_build_fails(gen, "slots", packed, "dwords\t")

        lgen = self.gens["layout"]
        plain = lgen.table

        def full_masks(rows):
            return [(*e[:5], e[5] or (1 << (8 * e[4])) - 1 if e[4] <= 4 else e[5])
                    for e in plain(rows)]

        self.assert_build_fails(lgen, "table", full_masks, "\t0xffffffff")

    def assert_build_fails(self, gen, name, edit, line):
        """The generator `gen` with `name` replaced by `edit`: its build check exits 1 on the
        frozen tables, quoting a line that holds `line`."""
        err = io.StringIO()
        with patch.object(gen, name, edit), patch.object(sys, "argv", ["gen", "--check"]), \
                redirect_stderr(err):
            self.assertEqual(gen.main(), 1)
        self.assertIn(Path(gen.PUBLISHED_TABLES).name, err.getvalue())
        self.assertIn("this generator reads other bytes", err.getvalue())
        self.assertIn(line.replace("\t", "\\t"), err.getvalue())

    def test_v051_is_accepted_only_with_its_rows(self):
        for which, gen in self.gens.items():
            rows, legacy = self.checkout(which)
            self.assertEqual(tarec.list_id(which, legacy), gen.semantic_digest(rows))
            moved = [dict(r) for r in rows]
            moved[-1]["offset"] += 1
            with self.subTest(which=which), self.assertRaises(gen.Invalid):
                gen.published(moved)

    def test_a_header_reads_v051s_hashes_as_this_checkouts(self):
        entry = HeaderCopy.entry
        current, legacy = {}, {}
        for which, gen in self.gens.items():
            rows, legacy[which] = self.checkout(which)
            current[which] = gen.semantic_digest(rows)
        def header(layout, globals_):
            return tarec.header(entry(tarec.RT_FORMAT, struct.pack("<I", tarec.REC_FORMAT)) +
                                entry(tarec.RT_LAYOUT, layout.encode()) +
                                entry(tarec.RT_GLOBALS, globals_.encode()))
        old = header(legacy["layout"], legacy["globals"])
        new = header(current["layout"], current["globals"])
        self.assertEqual((old["layout"], old["globals"]), (current["layout"], current["globals"]))
        self.assertEqual((old["layout_raw"], old["globals_raw"]),
                         (legacy["layout"], legacy["globals"]))
        self.assertEqual(tarec.list_shown(old["layout"], old["layout_raw"]),
                         f"{current['layout'][:12]} (recorded as {legacy['layout'][:12]})")
        self.assertEqual(tarec.list_shown(new["layout"], new["layout_raw"]),
                         current["layout"][:12])
        self.assertIsNone(tarec.shape_differs(old, new))
        self.assertEqual(tarec.globals_list(old["globals"])[3], current["globals"])
        other = ("1" if current["globals"][0] != "1" else "2") + current["globals"][1:]
        forked = header(current["layout"], other)
        self.assertEqual(forked["globals"], other)
        self.assertEqual(tarec.shape_differs(forked, old), "globals")
        with self.assertRaises(tarec.Unreadable):
            tarec.globals_list(other)


class PuppetEngineChecks(unittest.TestCase):
    """A puppet's dump holds no native owner-only start and no native owner's query: `tarec counts`
    names each query call site's count as an owner's query, and the R/Q check fails on one."""

    def test_counts_names_a_query_site(self):
        counts = {(0x4113B1, 0, 0): 1, (0x4113EF, 0, 0): 0, (0x12345, 0, 0): 2}
        with patch.object(tarec, "header_only", return_value={}), \
                patch.object(tarec, "counts_of", return_value=(900, counts, (0, 0, 1), {})):
            out = io.StringIO()
            with redirect_stdout(out):
                tarec.cmd_counts("rec", "dump")
        text = out.getvalue()
        self.assertIn("owner's query 0x4113B1 from 0x000000: 1", text)
        self.assertIn("owner-only start 0x4113EF from 0x000000: 0", text)
        self.assertIn("site 0x012345 from 0x000000: 2", text)

    def test_a_native_query_fails_the_puppet(self):
        def run(lines):
            with patch.object(tareplay, "tarec_run", return_value=(0, "\n".join(lines))):
                return {label: verdict for label, verdict, _ in
                        tareplay.puppet_counts("R", "rec", Path("dump-rt1c.bin"))}
        clean = run(["dump: GameTime 900", "  owner's query 0x411A35 from 0x000000: 0"])
        self.assertEqual(clean["R: native owner's queries on a puppet"], tareplay.PASS)
        hit = run(["dump: GameTime 900", "  owner's query 0x411A35 from 0x000000: 1"])
        self.assertEqual(hit["R: native owner's queries on a puppet"], tareplay.FAIL)
        self.assertEqual(hit["R: native owner-only starts on a puppet"], tareplay.PASS)
        self.assertEqual(set(tarec.OWNER_QUERIES), {0x4027FB, 0x4113B1, 0x411A35, 0x411AA6,
                                                    0x411BF5, 0x411CF1})


class CompletionCheckpoints(unittest.TestCase):
    def test_completed_moho_is_compared_for_every_commit_seed(self):
        for revision in range(256):
            times = tareplay.gametimes("resource-core-upgrade", 12000, str(revision), last_min=7800)
            self.assertEqual(times, sorted(set(times)))
            self.assertEqual(len(times), 4)
            self.assertTrue(all(t % 30 == 1 and 301 <= t <= 9600 for t in times))
            self.assertGreaterEqual(times[-1], 7800)

    def test_unreachable_completion_is_refused(self):
        with self.assertRaises(tareplay.CouldNotRun):
            tareplay.gametimes("too-short", 2400, "revision", last_min=7800)

    def test_points_stay_clear_of_the_typed_work(self):
        fix = tareplay.fixtures()["impai-handicap"]
        typed = [s[0] for s in fix["steps"] if s[1] in ("type", "run", "keys")]
        for revision in range(256):
            times = tareplay.gametimes("impai-handicap", fix["end"], str(revision), 3, fix["first"],
                                       None, fix["last_max"])
            self.assertEqual(len(times), 4)
            self.assertFalse([t for t in typed if times[0] <= t <= times[-1]], (revision, times))

    def test_ordinary_fixture_keeps_its_sampling(self):
        self.assertEqual(tareplay.gametimes("battle", 2400, "revision"),
                         tareplay.gametimes("battle", 2400, "revision", last_min=None, last_max=None))


class TurboLanes(unittest.TestCase):
    def test_the_rule(self):
        fx = tareplay.fixtures()
        lanes = {n: "".join(p for p in "LPRQKFWXU" if tareplay.turbo_lane(fx[n], p, 2))
                 for n in ("c3-veterancy", "secondary-orders", "battle", "tenseat",
                           "resource-geo-upgrade", "wpn-badtgt")}
        self.assertEqual(lanes, {"c3-veterancy": "LPRKFXU", "secondary-orders": "LPRKFXU",
                                 "battle": "LPRKFXU", "tenseat": "LPRKFXU",
                                 "resource-geo-upgrade": "LPRKFXU", "wpn-badtgt": "LPRKFXU"})
        self.assertEqual(tareplay.turbo_lane(fx["c3-veterancy"], "P", 0), 0)

    def test_the_held_lanes_are_the_turbo_lanes_whatever_k(self):
        fx = tareplay.fixtures()
        for k in (0, 1):
            self.assertEqual("".join(p for p in "LPRQKFWXU" if tareplay.held_lane(fx["battle"], p)),
                             "LPRKFXU")
            self.assertTrue(all(tareplay.turbo_lane(fx["battle"], p, k) in (0, k)
                                for p in "LPRQKFWXU"))

    def test_a_cost_game_is_held_and_keeps_its_own_pacing(self):
        cost = {"load": "replay-four-ai.json", "cost": True, "end": 18000}
        self.assertTrue(tareplay.held_lane(cost, "L"))
        self.assertEqual(tareplay.turbo_lane(cost, "L", 1), 0)


class RunTo(unittest.TestCase):
    """run_to on a held level: a step whose GameTime a script ran past could not run, named; a
    level not held is refused before anything is armed."""

    def log(self, at, held=True):
        log = Mock()
        log.held, log.at, log.pending = held, at, None
        return log

    def test_a_passed_step_is_named(self):
        with patch.object(tareplay, "hold_at") as hold_at:
            with self.assertRaisesRegex(tareplay.CouldNotRun, r"`keys` at GameTime 120 had "
                                                              r"passed: the level is held at 300"):
                tareplay.run_to(Mock(), self.log(300), 900, [(120, "keys", ["a"])])
            hold_at.assert_not_called()

    def test_an_unheld_level_is_refused(self):
        with self.assertRaisesRegex(tareplay.CouldNotRun, "not held"):
            tareplay.run_to(Mock(), self.log(1, held=False), 900, [])

    def test_steps_at_the_held_tick_run_without_moving_it(self):
        ran = []
        with patch.object(tareplay, "step", side_effect=lambda w, kind, arg, log=None: ran.append(arg)), \
             patch.object(tareplay, "hold_at") as hold_at:
            self.assertEqual(tareplay.run_to(Mock(), self.log(1), 1, [(1, "apply", "a.json"),
                                                                      (1, "keys", ["b"])]), 1)
        self.assertEqual(ran, ["a.json", ["b"]])
        hold_at.assert_not_called()


class WatchHudBar(unittest.TestCase):
    """watch_hud's resource-bar row against a modelled game: paused at one GameTime, Sight
    toggles the viewed record between the first row's seat and record 10, and each draw that
    follows writes the memo's byte 0 and, when the build re-reads them, the four rates."""

    S0, VIEWER, TICK = 0, 10, 300

    def game(self, rates, rereads=True):
        memo = {"byte0": self.VIEWER, "rates": rates[self.VIEWER]}
        state = {"viewed": self.VIEWER}
        lines = []
        base, memo_at = 0x1B63, tareplay.BAR_MEMO
        offs = {m: k for k, (m, _) in enumerate(tareplay.BAR_RATES)}
        recs = {r: k for k, (_, r) in enumerate(tareplay.BAR_RATES)}

        def peek(w, spec):
            off, size = spec.split("+")[1].split(":")
            off = int(off, 16)
            if off == 0x2A43:
                return state["viewed"]
            if off in (0x1431F, 0x14323):
                return 7
            if off == memo_at:
                return memo["byte0"]
            if off - memo_at in offs:
                return memo["rates"][offs[off - memo_at]]
            seat, field = divmod(off - base, 0x14B)
            return seat if field == 0x146 else rates[seat][recs[field]]

        def hud(w, verb, key, **kw):
            if key == "sight:0":
                seat = self.S0 if state["viewed"] == self.VIEWER else self.VIEWER
                state["viewed"] = seat
                lines.append(f"replay: sight is seat {seat} at GameTime {self.TICK}; death "
                             f"vision 0 made, 0 refused")
                memo["byte0"] = seat
                if rereads:
                    memo["rates"] = rates[seat]
            else:
                # the other controls are not under test: one line every one of their waits finds
                lines.append("replay: x: follow seat 0, epoch 1; replay: x: jump to seat 0, epoch "
                             "1; free cam; replay: overlay cursors off; replay: overlay cursors on")

        def wait(pattern, timeout, n):
            return next((x for x in log.lines[n:] if re.search(pattern, x)), None)

        log = SimpleNamespace(lines=lines, wait=wait)
        w = SimpleNamespace(gametime=lambda: self.TICK)
        header = {"players": [{"index": self.S0}], "options": {"controlled": self.S0}}
        return w, log, peek, hud, header

    def run_hud(self, rates, rereads=True):
        w, log, peek, hud, header = self.game(rates, rereads)
        with patch.object(tareplay, "peek", peek), patch.object(tareplay, "hud", hud), \
             patch.object(tareplay, "nap"), \
             patch.object(tareplay.tarec, "header_only", return_value=header):
            return tareplay.watch_hud(w, log, "rec")

    def test_rates_that_differ_pass_and_name_the_telling_visits(self):
        rows = self.run_hud({self.S0: (0x41480000, 0x3F800000, 0x3F000000, 0),
                             self.VIEWER: (0, 0, 0, 0)})
        self.assertEqual([r[1] for r in rows], [tareplay.PASS, tareplay.PASS])
        self.assertIn("1 (record 0), 2 (record 10), 3 (record 0), 4 (record 10)", rows[1][2])

    def test_equal_rates_could_not_run(self):
        rows = self.run_hud({self.S0: (0, 0, 0, 0), self.VIEWER: (0, 0, 0, 0)})
        self.assertEqual(rows[0][1], tareplay.PASS)
        self.assertEqual(rows[1][1], tareplay.UNRUN)

    def test_a_bar_that_keeps_the_last_rates_fails(self):
        with self.assertRaisesRegex(tareplay.Failed, "other rates under Sight on seat 0"):
            self.run_hud({self.S0: (0x41480000, 0, 0, 0), self.VIEWER: (0, 0, 0, 0)},
                         rereads=False)


class FailedWorkerCleanup(unittest.TestCase):
    def test_failed_game_is_stopped_even_if_artifact_collection_fails(self):
        run = tareplay.Run.__new__(tareplay.Run)
        run.game_speed = tareplay.TEST_GAME_SPEED
        run.turbo = tareplay.TURBO
        run.fx = {"naval": {"fix": {}, "Ts": [1], "out": Path("unused"), "rec": Path("rec"),
                            "procs": {}, "problems": []}}
        worker = Mock(name="worker")
        worker.name = "owned"
        worker.cap_failure.return_value = None
        worker.collect.side_effect = OSError("artifact write failed")
        with patch.object(tareplay, "watch_lane", side_effect=tareplay.Failed("COB STOP")), \
             patch.object(tareplay, "say"):
            self.assertIsNot(run.job(worker, "naval", "W"), False)
        worker.stop.assert_called_once_with(hard=True)
        self.assertEqual(run.fx["naval"]["problems"][0], ("W", "FAILED", "COB STOP"))

    def test_worker_is_retired_if_failed_game_cannot_be_stopped(self):
        run = tareplay.Run.__new__(tareplay.Run)
        run.game_speed = tareplay.TEST_GAME_SPEED
        run.turbo = tareplay.TURBO
        run.fx = {"naval": {"fix": {}, "Ts": [1], "out": Path("unused"), "rec": Path("rec"),
                            "procs": {}, "problems": []}}
        worker = Mock(name="worker")
        worker.name = "owned"
        worker.cap_failure.return_value = None
        worker.stop.side_effect = tareplay.CouldNotRun("still running")
        with patch.object(tareplay, "watch_lane", side_effect=tareplay.Failed("COB STOP")), \
             patch.object(tareplay, "say"):
            self.assertIs(run.job(worker, "naval", "W"), False)
        self.assertIn("cleanup", run.fx["naval"]["problems"][-1][2])


class ImpaiLevers(unittest.TestCase):
    """place() removes every ImpAI test lever another tool left in the game folder before a fixture's
    content writes its own, and leaves the levers that are no ImpAI's."""

    def worker(self, gamedir):
        w = tareplay.Worker.__new__(tareplay.Worker)
        w.game_speed, w.turbo, w.held, w.exact = tareplay.TEST_GAME_SPEED, 0, False, False
        w.gdi = False
        w.name, w.gamedir, w.last_collection = "tareplay-test", gamedir, None
        return w

    def place(self, gamedir, fix, gdi=False):
        reg = json.dumps({"values": [{"value": tareplay.TEST_GAME_SPEED, "type": 4}]})
        dll = gamedir.parent / "ddraw.dll"
        dll.write_bytes(b"MZ")
        with patch.object(tareplay, "tacli", return_value=(0, reg)), patch.object(tareplay, "say"), \
             patch.object(tareplay._CLI_API, "_cat_path", return_value=gamedir / "catalogue.json"), \
             patch.object(tareplay._CLI_API, "Instance"):
            w = self.worker(gamedir)
            w.gdi = gdi
            w.place(fix, dll)

    def test_the_gdi_lever_follows_the_worker(self):
        with tempfile.TemporaryDirectory() as d:
            g = Path(d) / "gamedir"
            g.mkdir()
            target = Path(d) / "install_file"
            target.write_text("keep")
            (g / tareplay.GDI_LEVER).symlink_to(target)
            self.place(g, {}, gdi=True)
            self.assertTrue((g / tareplay.GDI_LEVER).is_file())
            self.assertFalse((g / tareplay.GDI_LEVER).is_symlink())
            self.assertEqual(target.read_text(), "keep")
            self.place(g, {}, gdi=False)
            self.assertFalse((g / tareplay.GDI_LEVER).exists())

    def test_left_levers_go(self):
        with tempfile.TemporaryDirectory() as d:
            g = Path(d) / "gamedir"
            g.mkdir()
            for name in ("tagpu_impai_check.on", "tagpu_impaiov.on", "tagpu_impai_budget.on", "tagpu_seatai.on"):
                (g / name).write_text("")
            self.place(g, {})
            self.assertEqual(sorted(p.name for p in g.glob("tagpu_impai*.on")), [])
            self.assertTrue((g / "tagpu_seatai.on").exists())

    def test_fixture_content_stays(self):
        with tempfile.TemporaryDirectory() as d:
            g = Path(d) / "gamedir"
            g.mkdir()
            (g / "tagpu_impai_budget.on").write_text("")
            fix = tareplay.fixtures()["impai-observe"]
            self.place(g, fix)
            self.assertEqual(sorted(p.name for p in g.glob("tagpu_impai*.on")),
                             ["tagpu_impai_check.on", "tagpu_impaiov.on"])

    def test_link_is_removed_not_followed(self):
        with tempfile.TemporaryDirectory() as d:
            target = Path(d) / "install_file"
            target.write_text("keep")
            g = Path(d) / "gamedir"
            g.mkdir()
            (g / "tagpu_impai_check.on").symlink_to(target)
            tareplay.clear_impai_levers(g)
            self.assertFalse((g / "tagpu_impai_check.on").is_symlink())
            self.assertEqual(target.read_text(), "keep")


class NoDraw(unittest.TestCase):
    """NO DRAW's harness side (tareplay.NODRAW_LEVER): the Worker places and removes the lever, refuses it
    without GDI and a turbo k, the acknowledgement takes only `nodraw: on at 0x4969CD`, and the account lines
    of a launch's levels add up."""

    def place(self, gamedir, nodraw, gdi=True, turbo=1, shadow=False, posecheck=False):
        reg = json.dumps({"values": [{"value": tareplay.TEST_GAME_SPEED, "type": 4}]})
        dll = gamedir.parent / "ddraw.dll"
        dll.write_bytes(b"MZ")
        with patch.object(tareplay, "tacli", return_value=(0, reg)), patch.object(tareplay, "say"), \
             patch.object(tareplay._CLI_API, "_cat_path", return_value=gamedir / "catalogue.json"), \
             patch.object(tareplay._CLI_API, "Instance"):
            w = tareplay.Worker.__new__(tareplay.Worker)
            w.game_speed, w.turbo, w.held, w.exact = tareplay.TEST_GAME_SPEED, turbo, False, False
            w.gdi, w.nodraw, w.shadow, w.posecheck = gdi, nodraw, shadow, posecheck
            w.name, w.gamedir, w.last_collection = "tareplay-test", gamedir, None
            w.place({}, dll)

    def test_the_shadow_and_pose_levers_follow_the_worker(self):
        with tempfile.TemporaryDirectory() as d:
            g = Path(d) / "gamedir"
            g.mkdir()
            self.place(g, False, shadow=True, posecheck=True)
            self.assertTrue((g / tareplay.SHADOW_LEVER).is_file())
            self.assertEqual((g / tareplay.POSE_LEVER).read_text(), "300")
            self.assertFalse((g / tareplay.NODRAW_LEVER).exists())
            self.place(g, False)
            self.assertFalse((g / tareplay.SHADOW_LEVER).exists())
            self.assertFalse((g / tareplay.POSE_LEVER).exists())

    def test_the_lever_follows_the_worker_and_never_writes_through_a_link(self):
        with tempfile.TemporaryDirectory() as d:
            g = Path(d) / "gamedir"
            g.mkdir()
            target = Path(d) / "install_file"
            target.write_text("keep")
            (g / tareplay.NODRAW_LEVER).symlink_to(target)
            self.place(g, True)
            self.assertTrue((g / tareplay.NODRAW_LEVER).is_file())
            self.assertFalse((g / tareplay.NODRAW_LEVER).is_symlink())
            self.assertEqual(target.read_text(), "keep")
            self.place(g, False)
            self.assertFalse((g / tareplay.NODRAW_LEVER).exists())

    def test_close_leaves_no_test_lever(self):
        # a ladder's or a fixture's income lever and ImpAI's levels die with the worker, not in the next launch
        with tempfile.TemporaryDirectory() as d:
            g = Path(d) / "gamedir"
            g.mkdir()
            for name in ("tagpu_impai_income.on", "tagpu_impai_level.on", tareplay.NODRAW_LEVER, "keep.txt"):
                (g / name).write_text("x")
            w = tareplay.Worker.__new__(tareplay.Worker)
            w.name, w.gamedir = "tareplay-test", g
            w.hold_lever, w.stop_display, w.lock = Mock(), Mock(), Mock()
            with patch.object(tareplay, "tacli", return_value=(0, "")):
                w.close()
            self.assertEqual(sorted(x.name for x in g.iterdir()), ["keep.txt"])

    def closed(self, keep):
        """The tacli commands a worker's close() sends, with `keep` set."""
        with tempfile.TemporaryDirectory() as d:
            w = tareplay.Worker.__new__(tareplay.Worker)
            w.name, w.gamedir, w.keep = "tareplay-test", Path(d), keep
            w.hold_lever, w.stop_display, w.lock = Mock(), Mock(), Mock()
            with patch.object(tareplay, "tacli", return_value=(0, "")) as tacli:
                w.close()
            w.lock.close.assert_called_once()
            return [c.args[:2] for c in tacli.call_args_list]

    def test_close_removes_the_instance_unless_kept(self):
        # THE INSTANCE'S END: a worker whose lanes passed leaves no instance; a failed lane or --keep keeps it
        self.assertIn(("rm", "tareplay-test"), self.closed(False))
        self.assertNotIn(("rm", "tareplay-test"), self.closed(True))

    def test_a_worker_has_no_draw_unless_it_asks(self):
        self.assertFalse(tareplay.Worker.nodraw)

    def test_no_draw_without_gdi_or_a_turbo_could_not_run(self):
        with tempfile.TemporaryDirectory() as d:
            g = Path(d) / "gamedir"
            g.mkdir()
            for gdi, turbo in ((False, 1), (True, 0)):
                with self.assertRaisesRegex(tareplay.CouldNotRun, "no-draw needs GDI and a turbo k"):
                    self.place(g, True, gdi=gdi, turbo=turbo)
                self.assertFalse((g / tareplay.NODRAW_LEVER).exists())

    def ack(self, lines, nodraw=True):
        log = tareplay.Log.__new__(tareplay.Log)
        log.lines, log.nodraw_on = list(lines), False
        tareplay.nodraw_ack(SimpleNamespace(nodraw=nodraw), log)
        return log.nodraw_on

    def test_the_ack_takes_the_on_line_alone(self):
        on = "replay: the frame visit at 0x4969CD in (skippable)\nnodraw: on at 0x4969CD, marked at 0x49692D"
        self.assertTrue(self.ack(on.split("\n")))
        self.assertFalse(self.ack(["nodraw: refused line never read"], nodraw=False))
        for line in ("nodraw: UNAVAILABLE: 0x494930 holds other bytes",
                     "nodraw: UNAVAILABLE: the skippable stub at 0x4969CD is not in",
                     "nodraw: tagpu_nodraw.on refused: the renderer is not GDI (tagpu_gdi.on)",
                     "nodraw: tagpu_nodraw.on refused: no tagpu_turbo.on k (no-draw keeps the turbo's batch)"):
            with self.assertRaisesRegex(tareplay.CouldNotRun, re.escape(line)):
                self.ack([line])

    def test_the_accounts_add_up_over_the_launchs_levels(self):
        lines = ["nodraw: from GameTime 2 to 9001, 8800 running frames not drawn, 12 drawn for the scoreboard, "
                 "40 drawn otherwise",
                 "nodraw: from GameTime 1 to 301, 290 running frames not drawn, 0 drawn for the scoreboard, "
                 "10 drawn otherwise"]
        acc = None
        for ln in lines:
            acc = tareplay.nodraw_add(acc, tareplay.NODRAW_ACCOUNT.search(ln))
        self.assertEqual(acc, {"from": 2, "to": 301, "skipped": 9090, "board": 12, "drawn": 50})
        self.assertAlmostEqual(tareplay.nodraw_share(acc), 9090 / 9152)
        self.assertIsNone(tareplay.nodraw_share(None))
        self.assertIsNone(tareplay.nodraw_share({"from": 1, "to": 1, "skipped": 0, "board": 0, "drawn": 0}))


class Shadow(unittest.TestCase):
    """THE SHADOW's verdict over a lane's log (tareplay.shadow_verdict), and the group that plays under it."""

    ON = "nodraw: shadow on at 0x469A00 and 0x469BA3"
    LEVEL = ("nodraw: shadow from GameTime 2 to 2400, 1200 frames compared, 0 differed, 3 not checked, "
             "50000 units drawn in them")
    ACCOUNT = ("nodraw: from GameTime 2 to 2400, 1100 running frames not drawn, 0 drawn for the scoreboard, "
               "5 drawn otherwise, 1100 drawn for the shadow")

    def verdict(self, lines, undrawn=False):
        (_, v, detail), = tareplay.shadow_verdict("x", "\n".join(lines), undrawn)
        return v, detail

    def test_a_lane_with_frames_compared_and_none_differing_passes(self):
        v, detail = self.verdict([self.ON, self.LEVEL])
        self.assertEqual(v, tareplay.PASS)
        self.assertEqual(detail, "1200 frames compared, 0 differed, 3 not checked, 50000 units drawn in them")
        v, detail = self.verdict([self.ON, self.ACCOUNT, self.LEVEL], undrawn=True)
        self.assertEqual(v, tareplay.PASS, detail)
        self.assertIn("1100 frames not drawn", detail)

    def test_a_difference_fails_by_name(self):
        d = ("nodraw: shadow DIFFERS at GameTime 700: the draw passed 40 units, the selection 39; first at 12: "
             "the draw's slot 300 at site A, the selection's slot 301 at site A")
        v, detail = self.verdict([self.ON, d, self.LEVEL.replace("0 differed", "1 differed")])
        self.assertEqual(v, tareplay.FAIL)
        self.assertIn("GameTime 700", detail)

    def test_progress_lines_stand_for_a_level_that_never_ended(self):
        progress = ("nodraw: shadow at GameTime 900, 600 frames compared, 0 differed, 0 not checked, "
                    "900 units drawn in them")
        v, detail = self.verdict([self.ON, progress])
        self.assertEqual((v, detail),
                         (tareplay.PASS, "600 frames compared, 0 differed, 0 not checked, 900 units drawn in them"))
        v, detail = self.verdict([self.ON, progress, self.LEVEL, progress])
        self.assertEqual(detail, "1800 frames compared, 0 differed, 3 not checked, 50900 units drawn in them")

    def test_no_shadow_or_nothing_compared_could_not_run_and_no_undrawn_frame_fails(self):
        v, detail = self.verdict(["nodraw: tagpu_ndshadow.on refused: the renderer is not GDI (tagpu_gdi.on)"])
        self.assertEqual(v, tareplay.UNRUN)
        self.assertIn("refused", detail)
        self.assertEqual(self.verdict([self.ON])[0], tareplay.UNRUN)
        self.assertEqual(self.verdict([self.ON, self.LEVEL.replace("50000 units", "0 units")])[0], tareplay.UNRUN)
        v, detail = self.verdict([self.ON, self.LEVEL, self.ACCOUNT.replace("1100 running", "0 running")],
                                 undrawn=True)
        self.assertEqual(v, tareplay.FAIL)
        self.assertIn("no frame went undrawn", detail)

    def test_the_group_nodraw_shadow_plays_its_lanes_under_the_shadow(self):
        fix = tareplay.fixtures()["nodraw-shadow"]
        self.assertIs(fix["nodraw"], True)
        self.assertEqual(tareplay.procs(fix), ("L", "P", "R", "Q"))


class NoDrawEq(unittest.TestCase):
    """nodraweq.py's verdict over four lanes' dumps and pose checks, the dump comparison stood in for: a field is a
    difference line less its values, the mixed pairs are held to the null twin's fields, and an undrawn lane's pose
    check to the drawn twins' where they agree."""

    def setUp(self):
        import nodraweq
        self.N = nodraweq

    POSE = [40, 0, "0123456789abcdef", "fedcba9876543210"]

    def lanes(self, share=0.99, dumps=("1300", "4000"), poses=("999", "1200")):
        return {n: {"share": share if nodraw else None,
                    "dumps": {T: f"{n}-{T}.bin" for T in dumps},
                    "pose": {G: list(self.POSE) for G in poses}} for n, nodraw in self.N.LANES}

    def verdict(self, lanes, diff):
        def fields(rec, a, b, T0):
            self.assertEqual(T0, 1000)
            return diff(a.name.split("-")[0], b.name.split("-")[0], a.name.split("-")[1][:-4])
        with patch.object(self.N, "fields", fields):
            return self.N.judge(Path("rec.imprec"), lanes, 1000)

    def test_a_field_is_its_line_less_its_values(self):
        self.assertEqual(self.N.field("globals meteorp1: recording 01000000, dump 02000000"), "globals meteorp1")
        self.assertEqual(self.N.field("unit 12 +0x05C: recording 00000000, dump 01000000"), "unit 12 +0x05C")
        self.assertEqual(self.N.field("unit 7: live in the dump only"), "unit 7: live in the dump only")

    def test_equal_lanes_pass(self):
        r = self.verdict(self.lanes(), lambda x, y, T: set())
        self.assertEqual(r["verdict"], "PASS", r["why"])
        self.assertEqual(sorted(r["at"]), ["1300", "4000"])

    def test_a_field_only_a_mixed_pair_differs_in_fails(self):
        r = self.verdict(self.lanes(), lambda x, y, T: {"unit 3 +0x10"} if (x, y) == ("A", "C") and T == "4000"
                         else set())
        self.assertEqual(r["verdict"], "FAIL")
        self.assertIn("T 4000: (A,C) differs beyond (A,B)", r["why"][0])

    def test_a_null_twin_that_differs_is_inconclusive_the_meteor_points_too(self):
        r = self.verdict(self.lanes(), lambda x, y, T: {"unit 3 +0x10"})
        self.assertEqual(r["verdict"], "INCONCLUSIVE")
        r = self.verdict(self.lanes(), lambda x, y, T: {"globals meteorp1"})
        self.assertEqual(r["verdict"], "INCONCLUSIVE")

    def test_a_pose_check_is_read_from_the_log(self):
        got = self.N.poses(["[t] posecheck: every 300 GameTime",
                            "[t] posecheck: GameTime 999 units 40 skipped 1 pose 0123456789abcdef aim "
                            "fedcba9876543210"])
        self.assertEqual(got, {"999": [40, 1, "0123456789abcdef", "fedcba9876543210"]})

    def test_an_undrawn_pose_the_drawn_twins_agree_against_fails(self):
        lanes = self.lanes()
        lanes["C"]["pose"]["1200"][2] = "0000000000000000"
        r = self.verdict(lanes, lambda x, y, T: set())
        self.assertEqual(r["verdict"], "FAIL")
        self.assertIn("GameTime 1200: C's pose check", r["why"][0])
        self.assertEqual(r["poses"], {"judged": 2, "null_differs": []})

    def test_poses_where_the_drawn_twins_differ_are_not_judged(self):
        lanes = self.lanes()
        lanes["B"]["pose"]["999"][3] = "0000000000000000"
        lanes["C"]["pose"]["999"][3] = "1111111111111111"
        r = self.verdict(lanes, lambda x, y, T: set())
        self.assertEqual(r["verdict"], "PASS", r["why"])
        self.assertEqual(r["poses"], {"judged": 1, "null_differs": ["999"]})
        lanes["B"]["pose"]["1200"][3] = "0000000000000000"
        r = self.verdict(lanes, lambda x, y, T: set())
        self.assertEqual(r["verdict"], "INCONCLUSIVE")
        lanes = self.lanes()
        del lanes["D"]["pose"]
        r = self.verdict(lanes, lambda x, y, T: set())
        self.assertEqual(r["verdict"], "INCONCLUSIVE")

    def test_a_low_undrawn_share_fails_and_no_common_dump_is_inconclusive(self):
        r = self.verdict(self.lanes(share=0.5), lambda x, y, T: set())
        self.assertEqual(r["verdict"], "FAIL")
        lanes = self.lanes()
        lanes["C"]["dumps"] = {}
        r = self.verdict(lanes, lambda x, y, T: set())
        self.assertEqual(r["verdict"], "INCONCLUSIVE")


def x_connections(display, threads=4, each=150):
    """`threads` loops each opening `each` raw X connections to `display`, taking the server's
    setup answer and closing, so clients keep leaving while others connect: the answers by kind
    ("accepted", or how the server dropped one)."""
    answers, lock = {}, threading.Lock()

    def one():
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(30)
        try:
            s.connect(f"/tmp/.X11-unix/X{display.lstrip(':')}")
            s.sendall(struct.pack("<BxHHHHxx", ord("l"), 11, 0, 0, 0))
            head = b""
            while len(head) < 8:
                chunk = s.recv(8 - len(head))
                if not chunk:
                    return "closed before the answer"
                head += chunk
            return "accepted" if head[0] == 1 else f"refused ({head[0]})"
        except OSError as e:
            return type(e).__name__
        finally:
            s.close()

    def loop():
        for _ in range(each):
            answer = one()
            with lock:
                answers[answer] = answers.get(answer, 0) + 1

    workers = [threading.Thread(target=loop) for _ in range(threads)]
    for t in workers:
        t.start()
    for t in workers:
        t.join()
    return answers


class PredictorAnswers(unittest.TestCase):
    """predictor_verdict: the fight predictor's answers from the military's level-end lines."""

    @staticmethod
    def mil(seat, made, refused):
        return (f"impai: mil seat={seat} level=2 decisions=186 refused=0 predictions={made} "
                f"refused_predictions={refused} roots=403 refused_orders=0 left_out=0\n")

    def test_predictions_made_pass(self):
        (label, verdict, detail), = tareplay.predictor_verdict("L", self.mil(1, 53, 0), True)
        self.assertEqual(verdict, tareplay.PASS)
        self.assertIn("seat 1: 53 predictions, 0 refused", detail)

    def test_a_refusal_fails_where_none_is_asked_for(self):
        (label, verdict, detail), = tareplay.predictor_verdict(
            "K", self.mil(1, 3, 0) + self.mil(2, 4, 1), False)
        self.assertEqual(verdict, tareplay.FAIL)
        self.assertIn("seat 2: 4 predictions, 1 refused", detail)

    def test_no_prediction_fails_only_when_asked_for(self):
        (label, verdict, detail), = tareplay.predictor_verdict("L", self.mil(1, 0, 0), True)
        self.assertEqual(verdict, tareplay.FAIL)
        (label, verdict, detail), = tareplay.predictor_verdict("L", self.mil(1, 0, 0), False)
        self.assertEqual(verdict, tareplay.PASS)

    def test_no_military_line(self):
        self.assertEqual(tareplay.predictor_verdict("P", "replay: nothing\n", False), [])
        (label, verdict, detail), = tareplay.predictor_verdict("L", "replay: nothing\n", True)
        self.assertEqual(verdict, tareplay.UNRUN)

    def test_impai_fight_asks_for_predictions(self):
        self.assertTrue(tareplay.fixtures()["impai-fight"].get("predicts"))

    def test_every_prediction_refused_fails(self):
        """a tree with no fight content refuses every prediction: impai-fight's L line on one"""
        line = ("impai: mil seat=1 level=2 decisions=186 refused=0 predictions=50 refused_predictions=50 "
                "roots=403 refused_orders=58 left_out=0 attacks=1 raids=0 retreats=0 scouts=1 defends=0 "
                "safe=0 dguns=0 army_max=12/512 enemies_max=7/2048 contacts_max=1/32 predict_max=1/12 "
                "roots_max=12/40\n")
        (label, verdict, detail), = tareplay.predictor_verdict("L", line, True)
        self.assertEqual(verdict, tareplay.FAIL)
        self.assertIn("50 predictions, 50 refused", detail)


class NavalAnswers(unittest.TestCase):
    """naval_verdict: the naval rule played, from the level-end military and economy lines."""

    @staticmethod
    def lines(seat, naval, plants):
        return (f"impai: eco seat={seat} decisions=360 orders=40 queued=8 failed=0 army=6 mex=3 energy=2 makers=0 "
                f"factories=2 cons=4 metal_in=8.0 energy_in=60.0 gametime=5400 sea_plants={plants}\n"
                f"impai: mil seat={seat} level=2 decisions=180 refused=0 predictions=6 refused_predictions=0 "
                f"roots=90 refused_orders=0 left_out=0 attacks=0 raids=0 retreats=0 scouts=1 defends=1 safe=0 "
                f"dguns=0 army_max=6/512 enemies_max=6/2048 contacts_max=1/32 predict_max=1/12 roots_max=8/40 "
                f"naval={naval} unreached=0 cross=1\n")

    def test_naval_with_a_sea_plant_passes(self):
        (label, verdict, detail), = tareplay.naval_verdict("L", self.lines(1, 1, 1))
        self.assertEqual(verdict, tareplay.PASS)
        self.assertIn("seat 1: naval=1 sea_plants=1", detail)

    def test_not_naval_fails(self):
        (label, verdict, detail), = tareplay.naval_verdict("L", self.lines(1, 0, 0))
        self.assertEqual(verdict, tareplay.FAIL)

    def test_naval_without_a_sea_plant_fails(self):
        (label, verdict, detail), = tareplay.naval_verdict("L", self.lines(1, 1, 0))
        self.assertEqual(verdict, tareplay.FAIL)

    def test_no_military_line(self):
        (label, verdict, detail), = tareplay.naval_verdict("L", "replay: nothing\n")
        self.assertEqual(verdict, tareplay.UNRUN)

    def test_the_mil_event_line_is_not_the_level_end(self):
        line = "impai: mil seat=1 t=900 attack=0 raid=0 scout=1 retreat=0 safe=0 dgun=0 army=6\n"
        (label, verdict, detail), = tareplay.naval_verdict("L", line)
        self.assertEqual(verdict, tareplay.UNRUN)

    def test_impai_naval_asks_for_the_rule(self):
        fixes = tareplay.fixtures()
        for name in ("impai-naval", "stock-impai-naval"):
            with self.subTest(name=name):
                self.assertTrue(fixes[name].get("naval"))
                self.assertTrue(fixes[name].get("predicts"))


class WorkerDisplay(unittest.TestCase):
    """A worker's display accepts every client that connects while others leave: it never
    regenerates (start_xvfb, -noreset). A server that does drops some, which is how a launch lost
    its display and ended with code 0 before a window."""

    def test_every_client_is_accepted_while_others_leave(self):
        x = tareplay.start_xvfb()
        try:
            self.assertEqual(x_connections(x.display), {"accepted": 600})
            self.assertIsNone(x.poll())
        finally:
            tareplay.stop_xvfb(x)


class OfflineOrder(unittest.TestCase):
    """tareplay.Ordered: checks handed to the offline pool read back in the order asked, and the
    first failure in that order is the one raised, before the caller's own later one."""

    def setUp(self):
        super().setUp()
        # a stand-in run: each game takes the Vulkan lane weight the run lock gives the players'
        # renderer (THE LANE WEIGHTS), as the run these tests model would have recorded
        p = patch.object(tareplay.runlock, "game_slots", lambda: 2)
        p.start()
        self.addCleanup(p.stop)

    @staticmethod
    def slow(value, s=0.2):
        time.sleep(s)
        return [value]

    @staticmethod
    def fail(why, s=0.0):
        time.sleep(s)
        raise tareplay.Failed(why)

    def test_results_keep_the_order_asked(self):
        r = tareplay.Ordered()
        r.add("a")
        r.later(self.slow, "b")
        r.add("c", "d")
        r.later(self.slow, "e", 0)
        self.assertEqual(r.results(), ["a", "b", "c", "d", "e"])
        self.assertEqual(r.results(), ["a", "b", "c", "d", "e"])

    def test_the_first_failure_in_order_is_raised(self):
        r = tareplay.Ordered()
        r.add("a")
        r.later(self.fail, "first", 0.2)
        r.later(self.fail, "second")
        r.add("b")
        for _ in range(2):
            with self.assertRaisesRegex(tareplay.Failed, "first"):
                r.results()
        self.assertEqual(r.settled(), ["a"])

    def test_an_earlier_failure_wins_over_the_callers_own(self):
        with self.assertRaisesRegex(tareplay.Failed, "the check"):
            with tareplay.Ordered() as r:
                r.later(self.fail, "the check", 0.2)
                raise tareplay.CouldNotRun("the next game")
        with self.assertRaisesRegex(tareplay.CouldNotRun, "the next game"):
            with tareplay.Ordered() as r:
                r.later(self.slow, "passes")
                raise tareplay.CouldNotRun("the next game")

    def test_then_runs_in_order_in_the_callers_thread(self):
        r, seen = tareplay.Ordered(), []
        got = r.start(self.slow, 7)
        r.add("a")
        r.then(lambda: seen.append(threading.current_thread()) or [got.result()[0] + 1])
        r.later(self.slow, "b", 0)
        self.assertEqual(r.results(), ["a", 8, "b"])
        self.assertEqual(seen, [threading.current_thread()])

    def test_the_pool_asks_a_worker_a_game_and_runs_no_more_than_granted(self):
        self.assertEqual(tareplay.offline_workers(8), max(2, min(8, self.gib())))
        self.assertEqual(tareplay.offline_workers(1), 2)
        with patch.object(tareplay, "_POOL", None), \
                patch.object(tareplay.runlock, "holding", lambda: True), \
                patch.object(tareplay.runlock, "workers", lambda: 3), \
                patch.object(tareplay.runlock, "games", lambda: 8), \
                patch.object(tareplay.runlock, "slots", lambda: 19):
            pool = tareplay.offline(8)
            try:
                granted = min(3, tareplay.offline_workers(8))
                self.assertEqual(pool.gate.n, granted)
                # widened to every slot the run holds, a thread for each as memory allows
                self.assertEqual(pool.most, granted + tareplay.runlock.game_slots() * 8)
                self.assertEqual(pool.threads, max(granted, min(pool.most, self.gib())))
                pool.widen(100)
                self.assertEqual(pool.gate.n, pool.most, "never past the slots the run holds")
            finally:
                pool.shutdown()

    def test_a_run_that_declared_no_workers_has_none_to_widen_into(self):
        """Nested in a `runlock.py run --slots 8`: workers() is the games' own slots."""
        with patch.object(tareplay, "_POOL", None), \
                patch.object(tareplay.runlock, "holding", lambda: True), \
                patch.object(tareplay.runlock, "workers", lambda: 8), \
                patch.object(tareplay.runlock, "games", lambda: 4), \
                patch.object(tareplay.runlock, "slots", lambda: 8):
            pool = tareplay.offline(8)
            try:
                granted = pool.gate.n
                pool.widen(tareplay.runlock.game_slots() * 4)
                self.assertEqual(pool.gate.n, granted)
                self.assertLessEqual(pool.gate.n, 8)
            finally:
                pool.shutdown()

    def test_the_pool_runs_no_more_than_its_gate_until_widened(self):
        pool = tareplay.OfflinePool(6, 2, 6)
        self.addCleanup(pool.shutdown)
        lock, live, peak, go = threading.Lock(), [0], [0], threading.Event()

        def task(barrier=None):
            with lock:
                live[0] += 1
                peak[0] = max(peak[0], live[0])
            if barrier is not None:
                barrier.wait()          # all of them at once, or this times out
            else:
                go.wait(WAIT_S)
            with lock:
                live[0] -= 1
            return True
        first = [pool.submit(task) for _ in range(3)]
        deadline = time.monotonic() + WAIT_S
        while live[0] < 2 and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertEqual(live[0], 2)
        go.set()
        self.assertTrue(all(f.result(WAIT_S) for f in first))
        self.assertEqual(peak[0], 2)
        pool.widen(4)
        together = threading.Barrier(6, timeout=WAIT_S)
        self.assertTrue(all(f.result(WAIT_S) for f in [pool.submit(task, together)
                                                        for _ in range(6)]))
        self.assertEqual(peak[0], 6)

    @staticmethod
    def gib():
        with open("/proc/meminfo") as f:
            return next(int(ln.split()[1]) for ln in f if ln.startswith("MemAvailable:")) >> 20


class WidenedPoolSameReport(unittest.TestCase):
    """THE GAME SLOTS GO TO THE CHECKS: a group's report through a pool widened while its checks
    run is the serial pool's -- the same summary lines, verdicts in the same order, and the same
    dumps kept -- whatever order its tarec calls end in."""

    TS = [301, 331, 361, 391]

    def group(self, root):
        """A group whose four lanes ran, every file its checks name present and empty."""
        out = root / "g"
        rec = out / "L" / "g.imprec"
        names = {"L": [f"l{self.TS[0]}", f"l{self.TS[-1]}", "ln1", "ln2"] + list(tareplay.NULL_VIEWS),
                 "P": [f"p{self.TS[0]}", f"p{self.TS[-1]}"],
                 "R": [f"r{self.TS[0]}"] + [f"rt{T}{x}" for T in self.TS[1:] for x in ("c", "-a", "-b")],
                 "Q": [f"q{self.TS[0]}"]}
        for proc, ns in names.items():
            (out / proc).mkdir(parents=True)
            (out / proc / "tagpu.log").write_text("")
            for n in ns:
                (out / proc / f"dump-{n}.bin").write_bytes(b"")
        rec.write_bytes(b"")
        return {"name": "g", "fix": {"end": 900}, "Ts": self.TS, "out": out, "rec": rec,
                "procs": {p: "ran" for p in "LPRQ"}, "want": "LPRQ", "problems": [], "passed": [],
                "held": [], "game_speed": 20, "turbo": [],
                "rt": {f"rt{T}": f"replay: g: round trip rt{T}: 35 tables poisoned, residue 0"
                       for T in self.TS[1:]}}

    @staticmethod
    def tarec(*args):
        """A pass, a FAIL or a could-not-run, by the arguments alone, ending at a random time."""
        key = " ".join(Path(str(a)).name for a in args)
        h = zlib.crc32(key.encode())
        time.sleep((h % 7) / 1000)
        return (0, 0, 0, 1, 2)[h % 5], f"{key}: first\n{key}: {h}"

    def report_on(self, pool, widen=0):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        fx = self.group(root)
        self.addCleanup(pool.shutdown)
        with patch.object(tareplay, "_POOL", pool), patch.object(tareplay, "tarec_run", self.tarec):
            if widen:
                # the lanes end while the checks run: the pool widens beside them
                t = threading.Thread(target=lambda: (time.sleep(0.005), pool.widen(widen)))
                t.start()
                self.addCleanup(t.join, WAIT_S)
            lines, failed, unrun = tareplay.fixture_report(fx, keep=False)
        kept = sorted(str(f.relative_to(root)) for f in root.rglob("*.bin"))
        verdicts = [(label, v, detail.replace(str(root), "ROOT")) for label, v, detail in fx["verdicts"]]
        return [ln.replace(str(root), "ROOT") for ln in lines], failed, unrun, verdicts, kept

    def test_a_widened_pool_reports_what_a_serial_one_does(self):
        serial = self.report_on(tareplay.OfflinePool(1, 1))
        self.assertTrue(serial[1] and serial[2], "the stand-in grades a FAIL and a could not run")
        self.assertGreater(len(serial[3]), 30)
        for _ in range(3):
            self.assertEqual(self.report_on(tareplay.OfflinePool(12, 2, 12), widen=10), serial)


class Scheduling(unittest.TestCase):
    """Run's queue: the long lanes first, and a fixture's report started when its last lane
    ends, once."""

    def run_(self, *fixtures):
        run = tareplay.Run.__new__(tareplay.Run)
        run.q, run.seq, run.lock = tareplay.queue.PriorityQueue(), tareplay.itertools.count(), \
            threading.Lock()
        run.pending, run.left, run.reports, run.measure, run.keep = 0, {}, {}, False, False
        run.keys, run.kept = {}, []
        run.report_pool = tareplay.ThreadPoolExecutor(max_workers=2)
        run.slots = tareplay.GameSlots(2)
        run.fx = {name: {"name": name, "want": want, "fix": {"end": end}}
                  for name, want, end in fixtures}
        return run

    def drain(self, run):
        out = []
        while not run.q.empty():
            out.append(run.q.get()[2:])
        return out

    def test_long_lanes_first_then_l_then_short(self):
        run = self.run_(("a", "LPRQ", 900), ("b", "LPRQKFXU", 600), ("c", "LPRQW", 2000))
        self.assertEqual(tareplay.first_lanes(run.fx), ["b", "c", "a"])
        for name in tareplay.first_lanes(run.fx):
            run.put(name, "L")
        self.assertEqual(run.q.get()[2:], ("b", "L"))
        for p in "PRQKXU":
            run.put("b", p)
        self.assertEqual(self.drain(run), [("b", "X"), ("b", "U"), ("b", "K"), ("c", "L"),
                                           ("a", "L"), ("b", "R"), ("b", "P"), ("b", "Q")])

    def test_the_report_starts_when_the_last_lane_ends(self):
        run = self.run_(("a", "LPKF", 900))
        with patch.object(tareplay, "fixture_report", return_value=(["== a"], False, False)) as rep:
            run.put("a", "L")
            run.put("a", "P")               # L queues its followers before it ends
            run.done("a")
            run.put("a", "F")               # so does K
            run.done("a")
            self.assertFalse(run.reports)
            self.assertFalse(run.slots.lanes_over)
            run.done("a")
            self.assertTrue(run.slots.lanes_over, "no lane is left: the slots go to the checks")
            self.assertEqual(run.report_of("a").result(), (["== a"], False, False))
            self.assertEqual(run.report_of("a"), run.reports["a"])
        rep.assert_called_once_with(run.fx["a"], False)

    def test_a_stopped_fixture_is_reported_by_report(self):
        run = self.run_(("a", "LPRQ", 900))
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        run.out = Path(tmp.name)
        run.put("a", "L")                   # never taken: the run stopped
        with patch.object(tareplay, "fixture_report", return_value=(["== a"], False, True)), \
                redirect_stdout(io.StringIO()):
            self.assertEqual(tareplay.report(run), 2)
        self.assertEqual((run.out / "summary.txt").read_text(),
                         "renderer: the players' (the default)\n== a\n")

    def test_a_gdi_run_says_so_and_takes_no_gpu_token(self):
        run = self.run_(("a", "LPRQ", 900))
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        run.out, run.gdi = Path(tmp.name), True
        run.put("a", "L")
        with patch.object(tareplay, "fixture_report", return_value=(["== a"], False, True)), \
                redirect_stdout(io.StringIO()):
            tareplay.report(run)
        self.assertTrue((run.out / "summary.txt").read_text().startswith("renderer: GDI (--gdi)"))
        run.j, run.what = 0, "tareplay run a --gdi"
        with patch.object(tareplay.runlock, "hold") as hold,                 patch.object(tareplay.runlock, "games", return_value=0),                 patch.object(tareplay, "offline"), patch.object(tareplay, "journal"),                 patch.object(tareplay, "watch_signals"):
            tareplay.run_threads(run)
        self.assertEqual(hold.call_args.kwargs, {"gpu": False, "suite": "replay",
                                                 "workers": tareplay.offline_workers(hold.call_args.args[2])})


class StandInRun(unittest.TestCase):
    """A run of stand-in groups -- one L lane each, whose job writes a coverage file and fails or
    passes as the test says, and a report made of the lanes' outcomes -- through the real Run,
    run_threads, report and verdict files. No game runs."""

    GROUPS = ("a", "b", "c")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.dll = self.root / "ddraw-cover.dll"
        self.dll.write_bytes(b"dll")
        self.tree = "c0ffee"
        self.played = []
        self.outcome = {}                   # group -> "pass", "fail" or "unrun" for its next lane
        self.widened = []                   # the workers the run's game slots gave the pool
        self.pool = SimpleNamespace(widen=self.widened.append)
        fixes = {n: {"load": f"{n}.json", "end": 900} for n in self.GROUPS}
        test = self

        def job(run, w, name, proc):
            test.played.append(name)
            fx = run.fx[name]
            (fx["out"] / proc).mkdir(parents=True, exist_ok=True)
            (fx["out"] / proc / f"cover-{proc}.bin").write_bytes(name.encode())
            how = test.outcome.get(name, "pass")
            if how == "pass":
                fx["procs"][proc] = "ran"
            elif how == "retire":               # its game could not be stopped (Run.job)
                fx["procs"][proc] = "could not run"
                fx["problems"].append((proc, "could not run", "failed game cleanup"))
                return False
            else:
                kind = "FAILED" if how == "fail" else "could not run"
                fx["procs"][proc] = kind
                fx["problems"].append((proc, kind, f"{name} {how}"))
        for target, name, value in ((tareplay, "fixtures", lambda: fixes),
                                    (tareplay, "refuse_unended", lambda fxs: None),
                                    (tareplay, "counts_options", lambda fix: None),
                                    (tareplay, "chosen_dll", lambda *a: self.dll),
                                    (tareplay, "gametimes", lambda *a: [301, 331, 361, 391]),
                                    (tareplay, "procs", lambda fix: ("L",)),
                                    (tareplay, "tree_key", lambda: self.tree),
                                    (tareplay, "Worker",
                                     lambda out: SimpleNamespace(close=lambda: None)),
                                    (tareplay, "fixture_report", self.report),
                                    (tareplay, "journal", lambda run: tareplay.tarun.NULL),
                                    (tareplay, "offline", lambda games=1: self.pool),
                                    (tareplay.runlock, "hold", lambda *a, **k: True),
                                    (tareplay.runlock, "games", lambda: 2),
                                    (tareplay, "watch_signals", lambda: None),
                                    (tareplay.Run, "job", job)):
            p = patch.object(target, name, value)
            p.start()
            self.addCleanup(p.stop)
        tareplay.STOP.clear()
        self.addCleanup(tareplay.STOP.clear)

    @staticmethod
    def report(fx, keep):
        """A group's report from its lanes alone: a FAILED lane fails it, any other problem leaves
        it could not run; its verdicts are one per lane."""
        failed = any(kind == "FAILED" for _, kind, _ in fx["problems"])
        unrun = any(kind != "FAILED" for _, kind, _ in fx["problems"])
        fx["verdicts"] = [(f"lane {p}", tareplay.PASS if s == "ran" else
                           tareplay.FAIL if s == "FAILED" else tareplay.UNRUN, s)
                          for p, s in sorted(fx["procs"].items())]
        lines = [f"== {fx['name']}: {fx['out']}"] + [f"   {v[1]:4}  {v[0]}: {v[2]}"
                                                     for v in fx["verdicts"]]
        return lines, failed, unrun

    def sitting(self, out, resume=False, turbo=1, multiplayer=None):
        """One sitting of `run a b c` into `out`, as cmd_run plays it, with the two-player groups
        `multiplayer` ({build: DLL}) beside it as the landing plays them: (exit code, summary)."""
        args = SimpleNamespace(cmd="run", what="test", keep=False, game_speed=20, turbo=turbo,
                               cover=False, gdi=False, dll=None, stock_dll=None,
                               resume=str(out) if resume else None)
        out.mkdir(parents=True, exist_ok=True)
        self.played = []
        with redirect_stdout(io.StringIO()) as said:
            run = tareplay.Run(list(self.GROUPS), args, out)
            run.j = 2
            run.multiplayer = multiplayer or {}
            tareplay.run_threads(run)
            status = tareplay.report(run)
        self.said, self.run = said.getvalue(), run
        return status, (out / "summary.txt").read_text()

    @staticmethod
    def groups_of(summary):
        """The summary without the line a resumed run adds: what the run graded."""
        return [ln for ln in summary.splitlines() if not ln.startswith("resumed:")]


REAL_WATCH_SIGNALS = tareplay.watch_signals     # StandInRun patches it away


class TheStop(unittest.TestCase):
    """THE STOP: the signal handler does nothing, so a signal that comes while the main thread holds
    a lock still stops the run; STOP is set before the line; and once it is set, a tacli call starts
    only to stop a game, in a session of its own."""

    def setUp(self):
        tareplay.STOP.clear()
        self.addCleanup(tareplay.STOP.clear)

    def test_a_signal_while_the_main_thread_holds_print_reaches_the_stop(self):
        got = threading.Event()
        stop = tareplay.SignalStop(lambda signum: got.set())
        self.addCleanup(stop.close)
        with tareplay.PRINT:
            os.kill(os.getpid(), signal.SIGINT)
            os.kill(os.getpid(), signal.SIGTERM)
            self.assertTrue(got.wait(10), "the stop thread was not woken")

    def test_stop_sets_stop_before_its_line(self):
        seen = []
        with patch.object(tareplay, "say", lambda *a: seen.append(tareplay.STOP.is_set())):
            tareplay.stop_run(signal.SIGINT)
            tareplay.stop_run(signal.SIGTERM)
        self.assertEqual(seen, [True], "one line, said with STOP already set")

    def test_a_closed_output_does_not_keep_the_halt_from_running(self):
        halted = []
        w = SimpleNamespace(name="w", halt=lambda: halted.append(1))

        def broken(*a):
            raise BrokenPipeError
        with patch.object(tareplay, "say", broken), patch.object(tareplay, "LIVE", {id(w): w}):
            tareplay.stop_run(signal.SIGINT)
        self.assertTrue(tareplay.STOP.is_set())
        self.assertEqual(halted, [1])

    def test_once_stopped_only_a_stop_starts(self):
        calls = []

        def run(argv, **kw):
            calls.append((argv[2], kw.get("start_new_session")))
            return SimpleNamespace(returncode=0, stdout="", stderr="")
        with patch.object(tareplay.subprocess, "run", run):
            tareplay.tacli("ui", "w", "--json")
            tareplay.STOP.set()
            with self.assertRaises(tareplay.CouldNotRun):
                tareplay.tacli("scenario", "load", "w", "x.json")
            tareplay.tacli("stop", "w", "--hard")
        self.assertEqual(calls, [("ui", True), ("stop", True)])

    def test_the_halt_stops_the_display_when_the_game_stop_fails(self):
        w = object.__new__(tareplay.Worker)
        w.name, w.display_lock, w.xvfb = "w", threading.Lock(), Mock()

        def late(*a, **k):
            raise subprocess.TimeoutExpired("tacli", 120)
        with patch.object(tareplay, "tacli", late), redirect_stdout(io.StringIO()):
            w.halt()
            w.stop_display()
        w.xvfb.terminate.assert_called()
        self.assertEqual(w.xvfb.wait.call_count, 2)


class StoppedRun(StandInRun):
    """A signal in the middle of a stand-in run: every worker in a lane is halted at once, its lane
    ends, no group starts after it, every worker is closed and the run returns."""

    def setUp(self):
        super().setUp()
        self.made, test = [], self
        both = threading.Barrier(2, timeout=10)

        class Stand:
            def __init__(self, out):
                self.name = f"w{len(test.made)}"
                self.halted, self.closed = threading.Event(), False
                test.made.append(self)

            def halt(self):
                self.halted.set()

            def close(self):
                self.closed = True

        def job(run, w, name, proc):
            test.played.append(name)
            # both lanes are taken, so both workers are live, before the one signal
            if both.wait() == 0:
                os.kill(os.getpid(), signal.SIGINT)
            fx = run.fx[name]
            fx["procs"][proc] = "could not run"
            fx["problems"].append((proc, "could not run",
                                   "halted" if w.halted.wait(10) else "never halted"))
        for target, name, value in ((tareplay, "Worker", Stand), (tareplay.Run, "job", job),
                                    (tareplay, "watch_signals", REAL_WATCH_SIGNALS)):
            p = patch.object(target, name, value)
            p.start()
            self.addCleanup(p.stop)
        self.addCleanup(tareplay.unwatch_signals)

    def test_a_signal_halts_every_worker_and_the_run_returns(self):
        t0 = time.monotonic()
        status, summary = self.sitting(self.root / "run")
        self.assertLess(time.monotonic() - t0, 10)
        self.assertTrue(tareplay.STOP.is_set())
        self.assertEqual(status, 2)
        self.assertEqual(len(self.made), 2)
        self.assertTrue(all(w.halted.is_set() and w.closed for w in self.made))
        self.assertFalse(tareplay.LIVE, "every worker leaves the live set when it closes")
        self.assertEqual(len(self.played), 2, "no group starts after the stop")
        self.assertNotIn("never halted", summary)
        self.assertEqual(self.said.count("stopping on SIGINT"), 1)


def pe_image(stamp=1, checksum=2, export_stamp=3, code=b"\x90"):
    """A minimal PE32 DLL: the file header at 0x80, one section at file 0x200 holding the export
    directory, the three fields a link writes its time into (dllkey) set as given, and `code`
    after the export directory."""
    b = bytearray(0x400)
    b[:2] = b"MZ"
    struct.pack_into("<I", b, 0x3C, 0x80)
    b[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", b, 0x84, 0x14C, 1, stamp, 0, 0, 0xE0, 0x2102)
    opt = 0x98
    struct.pack_into("<H", b, opt, 0x10B)
    struct.pack_into("<I", b, opt + 64, checksum)
    struct.pack_into("<II", b, opt + 96, 0x1000, 0x28)              # the export directory
    struct.pack_into("<8sIIII", b, opt + 0xE0, b".edata", 0x100, 0x1000, 0x200, 0x200)
    struct.pack_into("<I", b, 0x204, export_stamp)
    b[0x240:0x240 + len(code)] = code
    return bytes(b)


class DllKey(unittest.TestCase):
    """dllkey.digest: a DLL's hash less the link's time, so a relink of the same tree keeps a
    resumed run's key and any other byte changes it."""

    def digest(self, data):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "ddraw.dll"
            f.write_bytes(data)
            return tareplay.dllkey.digest(f)

    def test_the_three_link_time_fields_are_left_out(self):
        self.assertEqual(tareplay.dllkey.stamp_fields(pe_image()), [0x88, 0x98 + 64, 0x204])
        self.assertEqual(self.digest(pe_image(1, 2, 3)), self.digest(pe_image(7, 8, 9)))

    def test_any_other_byte_changes_it(self):
        self.assertNotEqual(self.digest(pe_image()), self.digest(pe_image(code=b"\xCC")))

    def test_a_file_that_is_no_pe_image_is_hashed_whole(self):
        self.assertEqual(tareplay.dllkey.stamp_fields(b"dll"), [])
        self.assertEqual(self.digest(b"dll"), hashlib.md5(b"dll").hexdigest())

    def test_no_file_has_no_hash(self):
        self.assertIsNone(tareplay.dllkey.digest(Path("/nonexistent/ddraw.dll")))


class Resume(StandInRun):
    """--resume (tareplay, *resuming a run*): a sitting's settled groups read back under their key,
    the rest played."""

    def setUp(self):
        super().setUp()
        # a stand-in run: each game takes the Vulkan lane weight the run lock gives the players'
        # renderer (THE LANE WEIGHTS), as the run these tests model would have recorded
        p = patch.object(tareplay.runlock, "game_slots", lambda: 2)
        p.start()
        self.addCleanup(p.stop)

    def test_a_partial_run_resumed_gives_the_full_runs_verdict(self):
        self.outcome = {"b": "fail"}
        full = self.sitting(self.root / "full")
        self.assertEqual(full[0], 1)
        self.outcome = {"b": "fail", "c": "unrun"}          # c cut short
        part = self.root / "part"
        self.assertEqual(self.sitting(part)[0], 1)
        self.assertEqual(sorted(p.parent.name for p in part.glob(f"*/{tareplay.VERDICT}")),
                         ["a", "b"])
        self.outcome = {}                                   # b would pass if it were played again
        status, summary = self.sitting(part, resume=True)
        self.assertEqual(self.played, ["c"])
        self.assertEqual(status, full[0])
        self.assertEqual(self.groups_of(summary), self.groups_of(full[1]))
        self.assertIn("resumed: kept from the earlier sitting: a b", summary)

    def test_a_fail_is_kept_as_a_fail(self):
        self.outcome = {"b": "fail"}
        out = self.root / "run"
        self.sitting(out)
        self.outcome = {}
        status, summary = self.sitting(out, resume=True)
        self.assertEqual(self.played, [])
        self.assertEqual(status, 1)
        self.assertIn("FAIL  lane L: FAILED", summary)

    def test_a_key_that_differs_is_refused_by_name_and_played_again(self):
        out = self.root / "run"
        self.sitting(out)
        self.tree = "beef"
        self.sitting(out, resume=True)
        self.assertEqual(sorted(self.played), list(self.GROUPS))
        self.assertIn("a: the kept result is refused, its key differs: tree; played again",
                      self.said)
        self.sitting(out, resume=True, turbo=2)
        self.assertIn("its key differs: turbo;", self.said)
        (self.root / "ddraw-cover.dll").write_bytes(b"rebuilt")
        self.sitting(out, resume=True, turbo=2)
        self.assertIn(f"its key differs: dlls {self.dll}", self.said)
        self.assertEqual(sorted(self.played), list(self.GROUPS))

    def test_a_relink_of_the_same_tree_keeps_the_key(self):
        """`make` between sittings relinks the DLL with a new time in its header: the result is
        kept. A change to any other byte refuses it (above)."""
        self.dll.write_bytes(pe_image(stamp=100, checksum=200, export_stamp=100))
        out = self.root / "run"
        self.sitting(out)
        self.dll.write_bytes(pe_image(stamp=160, checksum=260, export_stamp=160))
        self.sitting(out, resume=True)
        self.assertEqual(self.played, [])
        self.dll.write_bytes(pe_image(stamp=160, checksum=260, export_stamp=160, code=b"\xCC"))
        self.sitting(out, resume=True)
        self.assertEqual(sorted(self.played), list(self.GROUPS))

    def test_changed_coverage_files_are_refused(self):
        out = self.root / "run"
        self.sitting(out)
        (out / "a" / "L" / "cover-L.bin").write_bytes(b"other")
        self.sitting(out, resume=True)
        self.assertEqual(self.played, ["a"])
        self.assertIn("a: the kept result is refused, its coverage files are not the ones it "
                      "recorded", self.said)

    def test_a_stopped_run_keeps_nothing_it_settles_after_the_stop(self):
        out = self.root / "run"
        with patch.object(tareplay, "fixture_report",
                          lambda fx, keep: (tareplay.STOP.set(), self.report(fx, keep))[1]):
            self.sitting(out)
        self.assertEqual(list(out.glob(f"*/{tareplay.VERDICT}")), [])

    def test_a_two_player_group_is_kept_under_its_key(self):
        out = self.root / "run"
        self.sitting(out)
        game = out / tareplay.mp_item("raised") / "game"
        game.mkdir(parents=True)
        (game / "cover-mb-H.bin").write_bytes(b"c")
        reach = {("src/tagpu_recorder.c", "mp_start_send")}
        self.run.settle_multiplayer("raised", self.dll, reach, [])
        stock = out / tareplay.mp_item("stock")
        stock.mkdir()
        self.sitting(out, resume=True)
        with redirect_stdout(io.StringIO()):
            got = self.run.kept_multiplayer({"raised": self.dll, "stock": self.dll})
        self.assertEqual(got, {"raised": (reach, [])})
        self.assertIn("multiplayer-raised", self.run.kept)
        self.assertFalse(stock.exists(), "a group with no kept result is played from nothing")
        other = self.root / "other.dll"
        other.write_bytes(b"other")
        with redirect_stdout(io.StringIO()) as said:
            self.assertEqual(self.run.kept_multiplayer({"raised": other}), {})
        self.assertIn("its key differs: dll; played again", said.getvalue())

    def test_a_verdict_file_that_cannot_be_written_is_said_and_the_result_stands(self):
        self.outcome = {"b": "fail"}
        real = tareplay.write_atomic

        def write(path, text):
            if path.parent.name == "b":
                raise OSError(28, "No space left on device")
            real(path, text)
        with patch.object(tareplay, "write_atomic", write):
            status, summary = self.sitting(self.root / "run")
        self.assertEqual(status, 1)
        self.assertIn("FAIL  lane L: FAILED", summary)
        self.assertIn(f"b: its verdict file {self.root / 'run' / 'b' / tareplay.VERDICT} was not "
                      f"written (OSError: [Errno 28] No space left on device); the result stands",
                      self.said)
        self.assertEqual(sorted(p.parent.name for p in (self.root / "run").glob(
            f"*/{tareplay.VERDICT}")), ["a", "c"])

    def test_a_group_played_again_keeps_its_earlier_folder_beside_it(self):
        self.outcome = {"c": "unrun"}
        out = self.root / "run"
        self.sitting(out)
        (out / "c" / "K").mkdir(parents=True, exist_ok=True)
        (out / "c" / "K" / "tagpu.log").write_text("why K could not run\n")
        self.outcome = {}
        self.sitting(out, resume=True)
        self.assertEqual((out / "c.earlier" / "K" / "tagpu.log").read_text(), "why K could not run\n")
        self.assertFalse((out / "c" / "K" / "tagpu.log").exists())
        self.assertFalse((out / "a.earlier").exists())

    def test_a_dirty_tree_is_never_resumed(self):
        out = self.root / "run"
        self.sitting(out)
        self.tree = None
        with self.assertRaisesRegex(tareplay.CouldNotRun, "uncommitted or untracked"):
            self.sitting(out, resume=True)

    def test_a_folder_that_does_not_exist_yet_starts_fresh(self):
        out = self.root / "new" / "run"
        with patch.dict(tareplay.os.environ):              # out_dir points the decode cache at it
            made = tareplay.out_dir(SimpleNamespace(resume=str(out), out=None))
        self.assertEqual(made, out.resolve())
        self.assertTrue(made.is_dir())
        status, summary = self.sitting(made, resume=True)
        self.assertEqual(sorted(self.played), list(self.GROUPS))
        self.assertEqual(status, 0)
        self.assertNotIn("resumed:", summary)


class ResumeRefusedAtOnce(unittest.TestCase):
    """main() refuses a --resume it cannot go on with before a command selects anything: a
    landing's plan takes minutes, and a refusal after it costs them."""

    def refusal(self, resume, tree="c0ffee"):
        with patch.object(tareplay, "tree_key", lambda: tree), \
                patch.object(tareplay, "landing_plan", side_effect=AssertionError("selected")), \
                redirect_stderr(io.StringIO()) as err, self.assertRaises(SystemExit) as stop:
            tareplay.main(["landing", "--resume", str(resume)])
        self.assertEqual(stop.exception.code, 2)
        return err.getvalue()

    def test_a_folder_that_cannot_be_one_is_refused_at_once(self):
        with tempfile.TemporaryDirectory() as d:
            taken = Path(d) / "a-file"
            taken.write_text("")
            self.assertIn(f"--resume {taken}: not a folder this run can play in", self.refusal(taken))

    def test_a_dirty_tree_is_refused_at_once_and_makes_no_folder(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "run"
            self.assertIn("uncommitted or untracked changes", self.refusal(out, tree=None))
            self.assertFalse(out.exists())


class MultiplayerNames(unittest.TestCase):
    """The two-player games' instances: tools/mp_lobby.sh types each one's name as its player's
    nickname, so every name the landing can make fits SELGAME's NICKNAME field, and none is another
    run's or the other build's."""

    def test_every_name_the_landing_makes_fits_the_nickname_and_is_its_own(self):
        import recmp_create_begin_test as recmp
        runlock = tareplay.runlock
        tags = [runlock.slot_tag(k) for k in range(runlock.BUDGET)] + [runlock.FLOCK_TAG]
        self.assertEqual(sorted(tareplay.MP_LABELS), sorted(tareplay.MP_BUILDS))
        names = [name for tag in tags for build in tareplay.MP_BUILDS
                 for name in recmp.instance_names(tag, recmp.label_arg(tareplay.MP_LABELS[build]))]
        self.assertEqual(len(names), 2 * len(tags) * len(tareplay.MP_BUILDS))
        self.assertEqual([n for n in names if len(n) > recmp.NICKNAME_CHARS], [])
        self.assertEqual(len({n.upper() for n in names}), len(names))

    def test_each_builder_stands_off_the_maps_features(self):
        """placement keeps the builder's spot when its cells hold no feature and nothing stands on
        the pair's cells, and otherwise takes the nearest spot that does; grid_reader reads the
        window from peeks of at most 19 cells"""
        import recmp_create_begin_test as recmp
        w, h = 64, 64
        grid = {(x, z): bytearray(b"\x00\x00\xff\xff\x54\x54\x54\x00\xff\xff\x00\x00\x50")
                for x in range(w) for z in range(h)}
        cell = lambda x, z: bytes(grid[(x, z)]) if (x, z) in grid else None
        bx, bz = 300, 500                      # the builder on cells 17-18 x 30-31, its solar 21-25
        self.assertEqual(recmp.placement(cell, bx, bz), (bx, bz))
        grid[(26, 31)][8:10] = b"\x07\x00"     # a feature past the solar: no matter
        grid[(20, 31)][8:10] = b"\x07\x00"     # one between them: no matter
        self.assertEqual(recmp.placement(cell, bx, bz), (bx, bz))
        grid[(18, 31)][8:10] = b"\x08\x00"     # one under the builder: moved off it
        got = recmp.placement(cell, bx, bz)
        self.assertNotEqual(got, (bx, bz))
        self.assertNotIn(18, recmp.footprint(got[0], recmp.BUILDER_FOOT))
        grid[(22, 30)][12] |= 2                # a yard where the solar would go: moved again
        cols, rows = recmp.pair_cells(*recmp.placement(cell, bx, bz))
        self.assertFalse(22 in cols and 30 in rows)
        self.assertIsNone(recmp.placement(lambda x, z: None, bx, bz))     # off the grid

        base = 0x10000000
        reads = []

        def peek(specs):
            reads.append(specs)
            if specs == list(recmp.GRID):
                return [f"{base} (0x{base:08X})", f"{w} (0x{w:08X})", f"{h} (0x{h:08X})"]
            out = []
            for spec in specs:
                addr, n = spec.split(":x")
                at, n = (int(addr, 16) - base) // 13, int(n, 16) // 13
                self.assertLessEqual(n, 19)
                out.append(" ".join(f"{v:02X}" for i in range(n) for v in grid[(at % w + i, at // w)]))
            return out
        read = recmp.grid_reader(peek, bx, bz)
        self.assertEqual(len(reads), 2)
        cols, rows = recmp.pair_cells(bx, bz)
        self.assertEqual(read(18, 31), bytes(grid[(18, 31)]))
        self.assertEqual(read(cols[-1] + recmp.SHIFT_CELLS, rows[0] - recmp.SHIFT_CELLS),
                         bytes(grid[(cols[-1] + recmp.SHIFT_CELLS, rows[0] - recmp.SHIFT_CELLS)]))
        self.assertIsNone(read(cols[-1] + recmp.SHIFT_CELLS + 1, rows[0]))

    def test_the_impai_form_reads_each_peers_seat_line(self):
        import recmp_create_begin_test as recmp
        line = ("impai: seats {} (netai=impai, a network game, level 2) of 4 records; "
                "other peers' AI seats {}, ImpAI's {}")
        good = {"H": line.format(2, 3, 3), "J": line.format(3, 2, 2)}
        self.assertEqual(recmp.impai_seat_problems(good), [])
        bad = recmp.impai_seat_problems({"H": line.format("none", 3, 3), "J": line.format(3, 2, "none")})
        self.assertEqual(bad, ["H: ImpAI seats none, not its own AI row 2",
                               "J: the other peers' ImpAI seats none, not the other peer's AI row 2"])
        self.assertEqual(recmp.impai_seat_problems({"H": good["H"] + "\n" + good["H"], "J": ""}),
                         ["H: 2 network ImpAI seat lines, not one", "J: 0 network ImpAI seat lines, not one"])
        rows = recmp.room_rows({"gadgets": [{"name": "PLAYER2", "text": "ImpAI:MB-S24-I-H"},
                                            {"name": "READY0", "text": "x"}, {"name": "PLAYER3"}]})
        self.assertEqual(rows, {2: "ImpAI:MB-S24-I-H", 3: ""})

    def test_the_impai_takeover_needs_every_ai_seat_marked(self):
        import recmp_play_test as play
        line = "impai: seats {} (the recording's and impure.cfg's, a takeover) of 4 records; fresh {}"
        ok, why = play.impai_takeover([line.format("1,2,3", "1,2,3")], {1, 2, 3})
        self.assertTrue(ok, why)
        ok, why = play.impai_takeover([line.format("2,3", "3")], {1, 2, 3})
        self.assertFalse(ok)
        self.assertIn("not ImpAI's [1]", why)
        self.assertFalse(play.impai_takeover([], {1})[0])
        self.assertFalse(play.impai_takeover([line.format("none", "none")], set())[0])

    def test_the_lobby_refuses_a_name_its_nickname_cannot_hold_before_any_menu(self):
        r = subprocess.run(["bash", str(HERE.parent / "mp_lobby.sh"), "mb-s00-raised-H", "mb-s00-J"],
                           capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 2)
        self.assertIn("'mb-s00-raised-H' is 15 characters and its nickname holds 10", r.stderr)
        self.assertEqual(r.stdout, "")                      # the port check, its first step, never ran


class MultiplayerBeside(StandInRun):
    """The landing's two-player groups on the run's game slots (Run.multiplayer_lane, GameSlots):
    both builds at once and beside the lanes, never more games than the grant."""

    def setUp(self):
        super().setUp()
        # a stand-in run: each game takes the Vulkan lane weight the run lock gives the players'
        # renderer (THE LANE WEIGHTS), as the run these tests model would have recorded
        p = patch.object(tareplay.runlock, "game_slots", lambda: 2)
        p.start()
        self.addCleanup(p.stop)

    def test_both_builds_play_at_once_within_the_grant(self):
        both = threading.Barrier(2, timeout=WAIT_S)
        seen = []

        def group(build, dll, out, keep, game_over, impai=False):
            both.wait()                         # both builds in their game at once, or this fails
            game_over()
            (out / tareplay.mp_item(build)).mkdir()
            seen.append(build)
            return {("src/tagpu_recorder.c", build)}, []
        with patch.object(tareplay, "multiplayer_group", group), \
                patch.object(tareplay.runlock, "games", lambda: 4):
            self.sitting(self.root / "run", multiplayer={"raised": self.dll, "stock": self.dll})
        self.assertEqual(sorted(seen), ["raised", "stock"], self.run.mp_results)
        self.assertEqual(self.run.mp_results["stock"], ({("src/tagpu_recorder.c", "stock")}, []))
        self.assertEqual(self.run.slots.lent, 4, "a slot was not given back")
        self.assertEqual(sum(self.widened), tareplay.runlock.game_slots() * 4)
        self.assertTrue((self.root / "run" / "multiplayer-raised" / tareplay.VERDICT).is_file())

    def test_a_grant_of_two_games_plays_one_group_at_a_time(self):
        lock, live, peak = threading.Lock(), [0], [0]

        def group(build, dll, out, keep, game_over, impai=False):
            with lock:
                live[0] += 1
                peak[0] = max(peak[0], live[0])
            time.sleep(0.2)
            with lock:
                live[0] -= 1
            return set(), []
        with patch.object(tareplay, "multiplayer_group", group), \
                patch.object(tareplay.Run, "settle_multiplayer", lambda *a: None):
            self.sitting(self.root / "run", multiplayer={"raised": self.dll, "stock": self.dll})
        self.assertEqual(peak[0], 1)

    def test_a_grant_of_one_game_refuses_the_groups_before_a_game(self):
        played = []
        with patch.object(tareplay, "multiplayer_group", lambda *a: played.append(a)), \
                patch.object(tareplay.runlock, "games", lambda: 1):
            self.sitting(self.root / "run", multiplayer={"raised": self.dll, "stock": self.dll})
        self.assertEqual(played, [])
        self.assertEqual(sorted(self.run.mp_results), ["raised", "stock"])
        reach, problems = self.run.mp_results["stock"]
        self.assertEqual(reach, set())
        self.assertIn("MULTIPLAYER stock: could not run: the two-player game: its 2 games wait on "
                      "each other and this run may play 1 at once", problems[0])
        self.assertEqual(sorted(self.played), list(self.GROUPS), "the test groups still play")

    def test_a_group_whose_verdict_file_fails_keeps_its_result(self):
        def group(build, dll, out, keep, game_over, impai=False):
            (out / tareplay.mp_item(build)).mkdir()
            return {("src/tagpu_recorder.c", build)}, []
        with patch.object(tareplay, "multiplayer_group", group), \
                patch.object(tareplay, "write_atomic", side_effect=OSError("read-only")):
            self.sitting(self.root / "run", multiplayer={"raised": self.dll})
        self.assertEqual(self.run.mp_results["raised"], ({("src/tagpu_recorder.c", "raised")}, []))
        self.assertIn("multiplayer-raised: its verdict file", self.said)
        self.assertIn("(OSError: read-only); the result stands", self.said)

    def test_a_retired_worker_keeps_its_slot(self):
        """A worker whose game could not be stopped keeps its slot to the run's end: the game may
        still hold the GPU memory and seat the slot stands for."""
        self.outcome = {"a": "retire"}
        self.sitting(self.root / "run")
        # the retired slot is neither free nor lent: its game may still be running
        self.assertEqual((self.run.slots.total, self.run.slots.free, self.run.slots.lent),
                         (0, 0, 1))
        self.assertEqual(sum(self.widened), tareplay.runlock.game_slots())
        self.assertEqual(sorted(self.played), list(self.GROUPS), "the other worker plays on")

    def test_a_group_left_too_few_slots_by_a_retirement_is_refused(self):
        """Both workers retire; the group, which asks only once they have, meets a run with no slot
        left and is refused by name instead of waiting for slots that never come free."""
        self.outcome = {"a": "retire", "b": "retire"}
        played, both_retired = [], threading.Event()
        take, retire = tareplay.GameSlots.take, tareplay.GameSlots.retire

        def retiring(slots, n):
            retire(slots, n)
            if slots.total == 0:
                both_retired.set()

        def taking(slots, n, lane):
            if not lane:
                both_retired.wait(WAIT_S)
            return take(slots, n, lane)
        with patch.object(tareplay, "multiplayer_group", lambda *a: played.append(a)), \
                patch.object(tareplay.GameSlots, "retire", retiring), \
                patch.object(tareplay.GameSlots, "take", taking):
            self.sitting(self.root / "run", multiplayer={"raised": self.dll})
        self.assertEqual(played, [])
        self.assertEqual(self.run.mp_results["raised"],
                         (set(), ["MULTIPLAYER raised: could not run: 2 game slots asked of a run "
                                  "of 0"]))

    def test_a_group_that_throws_is_a_problem_by_name(self):
        def group(*a):
            raise OSError("no Xvfb")
        with patch.object(tareplay, "multiplayer_group", group):
            self.sitting(self.root / "run", multiplayer={"raised": self.dll})
        self.assertEqual(self.run.mp_results["raised"],
                         (set(), ["MULTIPLAYER raised: could not run: OSError: no Xvfb"]))


WAIT_S = 20                     # a bound on a step that takes milliseconds: past it the test is wrong


class GameSlotsOrder(unittest.TestCase):
    """GameSlots: a waiting lane before a two-player group, a group's two at once, no wait past a
    stop."""

    def setUp(self):
        tareplay.STOP.clear()
        self.addCleanup(tareplay.STOP.clear)

    def started(self, fn):
        t = threading.Thread(target=fn)
        t.start()
        self.addCleanup(t.join, WAIT_S)
        return t

    def test_a_waiting_lane_goes_before_a_group(self):
        slots, order = tareplay.GameSlots(3), []
        self.assertEqual(slots.take(3, lane=True), 3)
        group = self.started(lambda: order.append(("group", slots.take(2, lane=False))))
        lane = self.started(lambda: order.append(("lane", slots.take(1, lane=True))))
        deadline = time.monotonic() + WAIT_S
        while slots.lanes_waiting == 0 and time.monotonic() < deadline:
            time.sleep(0.01)
        slots.give(2)                   # two free: the group's two, but a lane waits
        lane.join(WAIT_S)
        self.assertEqual(order, [("lane", 1)])
        self.assertTrue(group.is_alive())
        slots.give(1)
        group.join(WAIT_S)
        self.assertEqual(order, [("lane", 1), ("group", 2)])
        self.assertEqual(slots.free, 0)

    def test_more_slots_than_the_run_has_are_refused(self):
        with self.assertRaises(ValueError):
            tareplay.GameSlots(1).take(2, lane=False)

    def test_a_retirement_refuses_a_waiting_group_the_run_can_no_longer_seat(self):
        slots, got = tareplay.GameSlots(2), []
        self.assertEqual(slots.take(1, lane=True), 1)

        def group():
            try:
                slots.take(2, lane=False)
            except ValueError as e:
                got.append(str(e))
        t = self.started(group)
        slots.retire(1)
        t.join(WAIT_S)
        self.assertEqual(got, ["2 game slots asked of a run of 1"])
        self.assertEqual((slots.total, slots.free), (1, 1))

    def test_a_stop_ends_the_wait_with_no_slot(self):
        slots, got = tareplay.GameSlots(1), []
        slots.take(1, lane=True)
        t = self.started(lambda: got.append(slots.take(1, lane=False)))
        tareplay.STOP.set()
        t.join(WAIT_S)
        self.assertEqual(got, [0])

    def test_nothing_is_lent_while_a_lane_may_still_come(self):
        lent = []
        slots = tareplay.GameSlots(3, spare=lent.append)
        slots.give(slots.take(2, lane=True))
        self.assertEqual((lent, slots.free, slots.total), ([], 3, 3))

    def test_the_slots_no_game_takes_again_are_lent_past_the_groups_two(self):
        """After the lanes, a free slot goes to the pool unless a group that has not asked yet
        needs it; a retired slot never does; the group still finds its two."""
        lent = []
        slots = tareplay.GameSlots(5, groups=1, spare=lent.append)
        slots.retire(slots.take(1, lane=True))      # its game may still run
        self.assertEqual(slots.take(1, lane=True), 1)
        slots.lanes_end()                           # one lane still plays, no other will come
        self.assertEqual((lent, slots.free), ([1], 2), "three free, two kept for the group")
        self.assertEqual(slots.take(2, lane=False), 2)
        slots.give(1)                               # the last lane ends
        slots.give(2)                               # the group ends
        self.assertEqual(lent, [1, 1, 2])
        self.assertEqual((slots.total, slots.free, slots.lent), (0, 0, 4))

    def test_a_refused_group_reserves_nothing(self):
        lent = []
        slots = tareplay.GameSlots(1, groups=1, spare=lent.append)
        slots.lanes_end()
        self.assertEqual(lent, [], "the group's two are kept until it asks")
        with self.assertRaises(ValueError):
            slots.take(2, lane=False)
        self.assertEqual((lent, slots.lent), ([1], 1))


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

    def test_the_sight_byte_is_the_wide_counter_capped(self):
        from types import SimpleNamespace
        w = SimpleNamespace(sight={0: bytearray([0, 255, 3]), 1: bytearray([7])},
                            sightw={0: [0, 300, 3]})
        self.assertEqual(tarec.sight_wide_faults(w), [])
        w.sightw[0][2] = 4
        faults = tarec.sight_wide_faults(w)
        self.assertEqual(len(faults), 1)
        self.assertIn("first 2: byte 3, W 4", faults[0])
        w.sightw[1] = [7, 7]
        self.assertIn("2 cells, its byte counters 1", tarec.sight_wide_faults(w)[1])

    def test_seated_sight_remains_exact(self):
        from types import SimpleNamespace
        def world():
            return SimpleNamespace(lists={}, players=[bytearray(tarec.REC_PLAYER_IMAGE)
                for _ in range(tarec.REC_PLAYERS)], sight={0: bytearray(b"a"), 4: bytearray(b"b")},
                sightw={4: [98]}, ctl=[], view=[0, 0], units={})
        a, b = world(), world()
        b.sight[0] = bytearray(b"c")
        b.sight.pop(4)
        h = {"players": [{"index": 0, "controller": 1}]}
        tarec.fork_view(a, b, h, h)
        self.assertNotIn(4, a.sight)
        self.assertNotIn(4, b.sight)
        self.assertNotIn(4, a.sightw)
        self.assertNotEqual(a.sight[0], b.sight[0])


class ForkFreshSeat(unittest.TestCase):
    """D2 (e): compare-fork leaves out ImpAI's mark, brain, military and unit-block jobs of a seat
    the takeover newly gave ImpAI, at the fork's first capture only (tarec.fork_fresh_seats)."""
    SLOTS, FORK_AT = 40, 601
    TABLES = {0: ("src/tagpu_impai.c:s_seat", 1, 10), 1: ("src/tagpu_impai.c:s_brain", 8, 10),
              2: ("src/tagpu_impai.c:s_mil", 8, 10), 3: ("src/tagpu_impai.c:s_job", 4, 40),
              4: ("src/tagpu_impai.c:s_known", 8, 40)}
    BLOCKS = {0: (1, 10), 1: (11, 20)}        # each seat's unit block, inclusive slots

    def header(self, ai, fork=True):
        h = {"players": [{"index": i, "controller": 1 if i == 0 else 2, "ai": ai.get(i, 0)}
                         for i in (0, 1)],
             "tables": {ix: {"name": n, "stride": s} for ix, (n, s, _) in self.TABLES.items()}}
        if fork:
            h["timeline"] = (1, 0, self.FORK_AT)
        return h

    def world(self, h, tick):
        w = tarec.World(self.SLOTS, h["tables"], h, dims=(0, 0, 0, 0))
        w.tick = tick
        for ix, (_, stride, rows) in self.TABLES.items():
            w.tables[ix] = bytearray(stride * rows)
        for seat, (lo, hi) in self.BLOCKS.items():
            struct.pack_into("<II", w.players[seat], tarec.PL_BLOCKFIRST, lo + 1, hi + 1)
        return w

    def compare(self, writes, parent_ai=None, tick=FORK_AT, fork=True):
        """(the difference count, the lines) after fork_view, the fork's header marking seat 1,
        and `writes` (table, row, byte) set in the fork's world only."""
        ha, hb = self.header(parent_ai or {}), self.header({1: 1}, fork)
        a, b = self.world(ha, tick), self.world(hb, tick)
        for ix, row, value in writes:
            b.tables[ix][row * self.TABLES[ix][1]] = value
        tarec.fork_view(a, b, ha, hb)
        lines, n = a.differences(b, ha["tables"], hb["tables"])
        return n, lines

    FRESH = [(0, 1, 1), (1, 1, 7), (2, 1, 9), (3, 11, 3), (3, 20, 4)]

    def test_a_fresh_seats_rows_pass_at_the_first_capture(self):
        self.assertEqual(self.compare(self.FRESH), (0, []))

    def test_the_same_rows_fail_at_the_second_capture(self):
        n, lines = self.compare(self.FRESH, tick=self.FORK_AT + 1)
        self.assertEqual(n, 5)
        for name in ("s_seat", "s_brain", "s_mil", "s_job"):
            self.assertTrue(any(f"src/tagpu_impai.c:{name} " in ln for ln in lines), name)

    def test_another_seats_rows_fail_at_the_first_capture(self):
        for write in ((0, 0, 1), (1, 0, 7), (2, 0, 9), (3, 10, 3), (3, 21, 3), (4, 11, 5)):
            with self.subTest(write=write):
                n, lines = self.compare([write])
                self.assertEqual(n, 1, lines)
                self.assertIn(self.TABLES[write[0]][0], lines[0])

    def test_a_seat_the_parent_marked_is_not_masked(self):
        self.assertEqual(tarec.fork_fresh_seats(self.header({1: 1}), self.header({1: 1}),
                                                self.world(self.header({}), self.FORK_AT)), set())
        n, _ = self.compare(self.FRESH, parent_ai={1: 1})
        self.assertEqual(n, 5)

    def test_a_header_with_no_fork_masks_nothing(self):
        n, _ = self.compare(self.FRESH, fork=False)
        self.assertEqual(n, 5)


def counts_compare(live, puppet, rewind=False, cob_exact=False):
    """compare-counts over a window whose first dumps count nothing: `live` and `puppet` are the
    second dumps' counts. (exit code, report)"""
    meta = (0, (1 << len(tarec.COUNT_SINKS)) - 1, 1)
    samples = [(10, {}, meta, {}), (20, live, meta, {123: "walk"}),
               (10, {}, meta, {}), (20, puppet, meta, {})]
    out = io.StringIO()
    with patch.object(tarec, "header_only", return_value={}), \
         patch.object(tarec, "counts_of", side_effect=samples), \
         patch.object(tarec, "absent_rows", return_value=((5, 2), [])), \
         redirect_stdout(out):
        result = tarec.cmd_compare_counts("rec", "a", "b", "c", "d", "rows", rewind, cob_exact)
    return result, out.getvalue()


class SliceRoot(unittest.TestCase):
    """The script slice 0x48ADEB, compared per placement (aux) exactly."""

    def counts(self, by_aux):
        return {(tarec.SLICE_ROOT, 0, aux): n for aux, n in by_aux.items()}

    def test_equal_per_aux(self):
        c = {0: 100, 1: 7}
        self.assertEqual(counts_compare(self.counts(c), self.counts(c))[0], 0)

    def test_a_slice_moved_to_the_steps_end_fails_by_name(self):
        for rewind in (False, True):
            with self.subTest(rewind=rewind):
                result, report = counts_compare(self.counts({0: 100, 1: 7}),
                                                self.counts({0: 100, 1: 6, 2: 1}), rewind)
                self.assertEqual(result, 1)
                self.assertIn("the script slice 0x48ADEB at its recorded place: live 7, puppet 6",
                              report)
                self.assertIn("the script slice 0x48ADEB at the step's end: live 0, puppet 1",
                              report)
                self.assertNotIn("in the unit tick", report)


class CobExact(unittest.TestCase):
    """--cob-exact: the VM's START and CALL children by script name, with no band."""

    def test_a_one_count_child_difference_fails_only_with_the_flag(self):
        for caller, what in ((0x4B18C0, "START"), (0x4B192D, "CALL")):
            with self.subTest(caller=what):
                key = (0x4B08C0, caller, 123)
                self.assertEqual(counts_compare({key: 100}, {key: 99})[0], 0)
                result, report = counts_compare({key: 100}, {key: 99}, cob_exact=True)
                self.assertEqual(result, 1)
                self.assertIn(f'COB child {what} "walk": live 100, puppet 99', report)

    def test_a_refused_index_stays_with_the_band(self):
        key = (0x4B08C0, 0x4B18C0, 0)
        self.assertEqual(counts_compare({key: 100}, {key: 99}, cob_exact=True)[0], 0)

    def test_other_callers_keep_the_band(self):
        key = (0x4B08C0, 0x4B0B0D, 123)
        self.assertEqual(counts_compare({key: 100}, {key: 99}, cob_exact=True)[0], 0)

    def test_rewind_and_cob_exact_exclude_each_other(self):
        self.assertEqual(counts_compare({}, {}, rewind=True, cob_exact=True)[0], 2)
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = tarec.main(["tarec.py", "compare-counts", "rec", "a", "b", "c", "d", "--rewind",
                             "--cob-exact"])
        self.assertEqual(rc, 2)

    def test_the_fixture_key_reaches_the_live_against_playing_counts_alone(self):
        self.assertEqual(tareplay.counts_options({"cob": "exact"}), ["--cob-exact"])
        self.assertEqual(tareplay.counts_options({}), [])
        with self.assertRaises(tareplay.CouldNotRun):
            tareplay.counts_options({"cob": "banded"})
        fx = {"rec": "rec.imprec", "Ts": [301, 2401], "out": Path("/nonexistent"),
              "procs": {"P": "ran"}, "fix": {"cob": "exact"}, "name": "x"}
        calls = []

        class Res:
            def add(self, *a): pass
            def later(self, fn, *a):
                calls.append(a)
                return Mock()
            def then(self, fn): pass
        with patch.object(tareplay, "Ordered", Res), \
             patch.object(Path, "exists", return_value=True):
            tareplay.checks(fx)
        compares = [a for a in calls if len(a) > 1 and a[1] == "compare-counts"]
        self.assertEqual(len(compares), 1)
        self.assertEqual(compares[0][-1], "--cob-exact")
        self.assertFalse([a for a in calls if "--cob-exact" in a and a[1] != "compare-counts"])


class CobFlarePolicy(unittest.TestCase):
    child = (0x472630, 0x420AE6, 0x4812D3)
    parent = (0x420A30, 0x4812D3, 0)

    def compare(self, values, child=None, parent_counted=True):
        pl, pp, cl, cp = values
        key = self.child if child is None else child
        sinks = (1 << len(tarec.COUNT_SINKS)) - 1
        if not parent_counted:
            sinks &= ~(1 << tarec.COUNT_SINKS.index(self.parent[0]))
        meta = (0, sinks, 1)
        samples = [(10, {}, meta, {}), (20, {self.parent: pl, key: cl}, meta, {}),
                   (10, {}, meta, {}), (20, {self.parent: pp, key: cp}, meta, {})]
        out = io.StringIO()
        with patch.object(tarec, "header_only", return_value={}), \
             patch.object(tarec, "counts_of", side_effect=samples), \
             patch.object(tarec, "absent_rows", return_value=((5, 2), [])), \
             redirect_stdout(out):
            result = tarec.cmd_compare_counts("rec", "a", "b", "c", "d", "rows")
        return result, out.getvalue()

    def test_accepted_parent_shortfall_bounds_child_and_reports_both(self):
        for values in ((201, 197, 33, 30), (197, 201, 30, 33)):
            result, report = self.compare(values)
            self.assertEqual(result, 0)
            self.assertIn("accepted bounded COB flare", report)
            self.assertIn(f"parent live {values[0]}, puppet {values[1]}", report)
            self.assertIn(f"flare live {values[2]}, puppet {values[3]}", report)

    def test_rejects_unexplained_or_invalid_shortfalls(self):
        for values in ((201, 197, 33, 28), (201, 201, 33, 30), (201, 197, 30, 33),
                       (201, 180, 33, 30), (20, 17, 13, 10), (10, 9, 14, 11),
                       (-1, 3, 0, 3), (201, 197, -1, -4)):
            with self.subTest(values=values):
                result, report = self.compare(values)
                self.assertEqual(result, 1)
                self.assertNotIn("accepted bounded COB flare", report)

    def test_only_named_parent_child_path_with_observed_parent(self):
        values = (201, 197, 33, 30)
        for key in ((0x472630, 0x420AE6, 0x4812D4), (0x472630, 0x420AE7, 0x4812D3),
                    (0x472530, 0x420AE6, 0x4812D3)):
            self.assertEqual(self.compare(values, child=key)[0], 1)
        result, report = self.compare(values, parent_counted=False)
        self.assertNotEqual(result, 0)
        self.assertNotIn("accepted bounded COB flare", report)


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

    def test_untested_marks(self):
        text = ("static void a(void)\n{\n}\n"
                'SIM_UNTESTED(a, "only a fault reaches it");\n'
                '/* SIM_UNTESTED(b, "a comment marks nothing"); */\n')
        self.assertEqual(tafeat.untested(text), {"a": "only a fault reaches it"})
        self.assertEqual(tafeat.untested_problems(text, "x.c"), [])
        bad = text + 'SIM_UNTESTED(a);\nSIM_UNTESTED(gone, "why");\n'
        self.assertEqual(len(tafeat.untested_problems(bad, "x.c")), 2)

    def test_the_two_player_game_is_a_test_group_name(self):
        # a feature only a network game runs names the landing's two-player game: a known name,
        # in no fixtures.json entry, so the landing plays multiplayer_group rather than a recording
        self.assertIn(tafeat.MULTIPLAYER, tafeat.fixture_names())
        self.assertNotIn(tafeat.MULTIPLAYER, tareplay.fixtures())
        feats, problems = tafeat.all_features()
        self.assertEqual([f["fixtures"] for f in feats if f["name"] == "unit_stream"],
                         [[tafeat.MULTIPLAYER]])
        self.assertFalse([p for p in problems if "unit_stream" in p])

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

    def test_a_test_only_block_is_in_no_build(self):
        text = ("a\n#ifdef RECF_TEST_COPY\nb\n#ifndef TAGPU_LIMITS_STOCK\nc\n#endif\n#else\nd\n"
                "#endif\ne\n")
        b = tafeat.builds(text)
        self.assertEqual([sorted(b[i]) for i in (1, 3, 5, 8, 10)],
                         [["raised", "stock"], [], [], ["raised", "stock"], ["raised", "stock"]])

    @unittest.skipUnless(all(m.exists() for m in MAPS), "no coverage build (make COVER=1)")
    def test_the_maps_hold_every_dll_function_and_no_test_one(self):
        mapped = set()
        for m in MAPS:
            mapped |= set(tafeat.read_map(m))
        for rel in tareplay.covered_sources():
            text = (tafeat.DDRAW / rel).read_text()
            built = tafeat.builds(text)
            for name, first, _ in tafeat.functions(text):
                if built[first]:
                    self.assertIn((rel, name), mapped, f"{rel}: {name} is in no map")
                else:                   # a test program's function (tafeat.TEST_ONLY)
                    self.assertNotIn((rel, name), mapped, f"{rel}: {name} is in a DLL's map")


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

    def test_an_include_line_needs_no_feature(self):
        for line in ('#include "tagpu_simreg.h"', '  #  include <windows.h>'):
            self.assertTrue(tareplay.INCLUDE_RX.match(line), line)
        for line in ("#define X 1", "static int includes;", "#if INCLUDED"):
            self.assertFalse(tareplay.INCLUDE_RX.match(line), line)

    def test_only_a_new_files_include_line_is_passed_over(self):
        self.assertTrue(tareplay.skip_include('#include "tagpu_simreg.h"', True))
        self.assertFalse(tareplay.skip_include('#include "tagpu_limits_stock.h"', False))
        self.assertFalse(tareplay.skip_include("#define X 1", True))

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
    def block(kind, payload, flags=tarec.RBF_STORED):
        head = struct.pack("<IHHIIIII", tarec.REC_BLOCK_MAGIC, kind, flags, 1, 9, len(payload),
                           len(payload), zlib.crc32(payload))
        return head + struct.pack("<I", zlib.crc32(head)) + payload

    @staticmethod
    def entry(tag, data):
        raw = struct.pack("<HH", tag, len(data)) + data
        return raw + b"\0" * (-len(raw) % 4)

    def test_edit(self):
        entry = self.entry
        layout, smap = b"ab" * 32 + b"\0", b"Coast To Coast\0"
        globals_ = b"cd" * 32 + b"\0"
        head = (entry(tarec.RT_FORMAT, struct.pack("<I", tarec.REC_FORMAT)) +
                entry(tarec.RT_LAYOUT, layout) + entry(tarec.RT_SETTINGS_MAP, smap) +
                entry(tarec.RT_GLOBALS, globals_))
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "a.imprec", Path(d) / "b.imprec"
            src.write_bytes(tarec.REC_MAGIC + self.block(tarec.RB_HEADER, head)
                            + self.block(tarec.RB_STREAM, b"\1\2\3\4"))
            tareplay.header_copy(src, dst, {tarec.RT_SETTINGS_MAP: tareplay.no_such_map})
            got, why = tarec.blocks(dst)
            self.assertIsNone(why)
            self.assertEqual(got[1], (tarec.RB_STREAM, 1, 9, b"\1\2\3\4"))
            ents = dict(tarec.entries(got[0][3]))
            self.assertEqual(ents[tarec.RT_SETTINGS_MAP], b"Q" * 14 + b"\0")
            self.assertEqual(ents[tarec.RT_LAYOUT], layout)
            tareplay.header_copy(src, dst, {tarec.RT_LAYOUT: tareplay.other_hash})
            ents = dict(tarec.entries(tarec.blocks(dst)[0][0][3]))
            self.assertEqual(ents[tarec.RT_LAYOUT], b"1" + layout[1:])
            self.assertEqual(ents[tarec.RT_SETTINGS_MAP], smap)
            tareplay.header_copy(src, dst, {tarec.RT_GLOBALS: tareplay.other_hash})
            ents = dict(tarec.entries(tarec.blocks(dst)[0][0][3]))
            self.assertEqual((ents[tarec.RT_GLOBALS], ents[tarec.RT_LAYOUT]),
                             (b"1" + globals_[1:], layout))
            tareplay.header_copy(src, dst, {tarec.RT_LAYOUT: tareplay.other_hash,
                                            tarec.RT_SETTINGS_MAP: tareplay.no_such_map})
            ents = dict(tarec.entries(tarec.blocks(dst)[0][0][3]))
            self.assertEqual((ents[tarec.RT_LAYOUT], ents[tarec.RT_SETTINGS_MAP]),
                             (b"1" + layout[1:], b"Q" * 14 + b"\0"))
            with self.assertRaisesRegex(tareplay.CouldNotRun, "no header entry"):
                tareplay.header_copy(src, dst, {tarec.RT_WALLCLOCK: tareplay.started_at(0)})

    def test_the_start(self):
        """started_at rewrites RT_WALLCLOCK's FILETIME and keeps the offset, which tarec shows
        as the local time"""
        entry = self.entry
        head = (entry(tarec.RT_FORMAT, struct.pack("<I", tarec.REC_FORMAT)) +
                entry(tarec.RT_WALLCLOCK, struct.pack("<Qi", 0, 120)))
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "a.imprec", Path(d) / "b.imprec"
            src.write_bytes(tarec.REC_MAGIC + self.block(tarec.RB_HEADER, head))
            tareplay.header_copy(src, dst, {tarec.RT_WALLCLOCK: tareplay.started_at(1792764309)})
            h = tarec.header(tarec.blocks(dst)[0][0][3])
            self.assertEqual((h["wallclock"], h["utc_offset"]),
                             ((1792764309 + 11644473600) * 10 ** 7, 120))
            self.assertEqual(tarec.started(h), "2026-10-23 16:05:09 +02:00")

    def test_format24_copy(self):
        """format24_copy writes format 24's header shape: RT_FORMAT 24 and an 8-byte RT_WALLCLOCK
        at the start given, the other entries and the blocks after the header as they were"""
        entry = self.entry
        smap = b"Coast To Coast\0"
        head = (entry(tarec.RT_FORMAT, struct.pack("<I", tarec.REC_FORMAT)) +
                entry(tarec.RT_WALLCLOCK, struct.pack("<Qi", 0, 120)) +
                entry(tarec.RT_SETTINGS_MAP, smap))
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "a.imprec", Path(d) / "b.imprec"
            tail = self.block(tarec.RB_STREAM, b"\1\2\3\4")
            src.write_bytes(tarec.REC_MAGIC + self.block(tarec.RB_HEADER, head) + tail)
            tareplay.format24_copy(src, dst, 1792764309)
            data = dst.read_bytes()
            magic, kind, flags, _f, _u, raw, packed, crc_body, crc_head = \
                struct.unpack_from("<IHHIIIIII", data, 8)
            body = data[40:40 + packed]
            self.assertEqual((kind, flags, raw), (tarec.RB_HEADER, tarec.RBF_STORED, packed))
            self.assertEqual(zlib.crc32(body), crc_body)
            self.assertEqual(zlib.crc32(data[8:36]), crc_head)
            self.assertEqual(data[40 + packed:], tail)
            ents = dict(tarec.entries(body))
            self.assertEqual(ents[tarec.RT_FORMAT], struct.pack("<I", 24))
            self.assertEqual(ents[tarec.RT_WALLCLOCK],
                             struct.pack("<Q", (1792764309 + 11644473600) * 10 ** 7))
            self.assertEqual(ents[tarec.RT_SETTINGS_MAP], smap)
            # an 8-byte entry is no start to tarec either, as to the DLL's reader
            ours = entry(tarec.RT_FORMAT, struct.pack("<I", tarec.REC_FORMAT)) + \
                entry(tarec.RT_WALLCLOCK, ents[tarec.RT_WALLCLOCK])
            self.assertNotIn("wallclock", tarec.header(ours))

    def test_a_packed_header_is_refused(self):
        head = self.entry(tarec.RT_SETTINGS_MAP, b"Coast To Coast\0")
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "a.imprec"
            src.write_bytes(tarec.REC_MAGIC + self.block(tarec.RB_HEADER, head, 0))
            with self.assertRaisesRegex(tareplay.CouldNotRun, "not stored"):
                tareplay.header_copy(src, Path(d) / "b.imprec",
                                     {tarec.RT_SETTINGS_MAP: tareplay.no_such_map})


class TakeoverRoster(unittest.TestCase):
    """A cached loader roster, one from before the judge's hold and a partial new dump cannot
    supply the takeover's order."""

    class Log:
        def __init__(self, lines):
            self.lines = lines

        def wait(self, rx, timeout, start=0):
            return next((line for line in self.lines[start:] if re.search(rx, line)), None)

    old = "  u001 armck own=0 idx=99 world=(1,2,3) screen=(4,5) nano=0.00\r"
    mark = "pause: paused at GameTime 1681\r"
    new = "  u001 armck own=0 idx=344 world=(4260,1170,85) screen=(4388,1159) nano=0.00\r"
    head = "units: alive=1 onscreen=0 eye=(0,0) me=0\r"

    def test_only_the_roster_after_the_acknowledgement_is_used(self):
        log = self.Log([self.old, self.head, self.mark, self.head, self.new, self.head])
        self.assertEqual(tareplay.takeover_roster(log, 1681, 0),
                         [{"type": "armck", "owner": 0, "engine_index": 344,
                           "world": [4260, 1170]}])

    def test_the_old_roster_does_not_complete_the_wait(self):
        with self.assertRaisesRegex(tareplay.CouldNotRun, "no roster dump began"):
            tareplay.takeover_roster(self.Log([self.old, self.head, self.mark, self.head]),
                                     1681, 0)

    def test_a_partial_dump_does_not_complete_the_wait(self):
        with self.assertRaisesRegex(tareplay.CouldNotRun, "has no header"):
            tareplay.takeover_roster(self.Log([self.old, self.head, self.mark, self.new]),
                                     1681, 0)

    def test_a_dump_begun_before_the_start_is_skipped_whole(self):
        log = self.Log([self.head, self.old, self.old, self.head, self.new, self.head])
        self.assertEqual([u["engine_index"] for u in tareplay.roster_after(log, 2, "t")], [344])


class OrderStep(unittest.TestCase):
    """A fixture's `order` step names its units from a roster dumped after the step began: on a
    turbo level the newest dump can predate the scenario's units (MEASURED 2026-11-12: `no ARMPW
    of rank 0 (0 in the roster)` at GameTime 900 on a turbo lane, its newest dump listing none of
    the scenario's six)."""

    class Log:
        """The log as the step reads it first, then with a dump the game logs afterwards."""

        def __init__(self, lines, later):
            self.lines, self.later, self.reads = lines, later, 0

        def read(self):
            self.reads += 1
            if self.reads > 1:
                self.lines += self.later
                self.later = []

        def wait(self, rx, timeout, start=0):
            self.read()
            return next((line for line in self.lines[start:] if re.search(rx, line)), None)

    head = "units: alive=2 onscreen=0 eye=(0,0) me=0\r"
    com = "  u001 corcom       own=1 idx=501 world=(4944,647,55) screen=(4,5) nano=0.00\r"
    pw = "  u001 armpw        own=0 idx=2 world=(5090,1683,55) screen=(4,5) nano=0.00\r"
    com2 = "  u002 corcom       own=1 idx=501 world=(4944,647,55) screen=(4,5) nano=0.00\r"

    def test_the_units_come_from_a_dump_after_the_step(self):
        log = self.Log([self.com, self.head], [self.pw, self.com2, self.head])
        arg = {"unit": ["ARMPW", 0], "args": ["attack", "unit", ["CORCOM", 0]]}
        with patch.object(tareplay, "tacli", return_value=(0, "")) as cli:
            tareplay.step(Mock(name="w"), "order", arg, log)
        argv = cli.call_args.args
        self.assertEqual(argv[2:5], ("attack", "unit", "501"))
        self.assertEqual(argv[argv.index("--unit") + 1], "2")

    def test_no_log_is_could_not_run(self):
        arg = {"unit": ["ARMPW", 0], "args": ["stop"]}
        with self.assertRaisesRegex(tareplay.CouldNotRun, "needs the lane's log"):
            tareplay.step(Mock(name="w"), "order", arg)


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

    def test_native_driver_changes_select_every_fixture(self):
        fx = {"one": {"load": "one.json"}, "two": {"load": "two.json"}}
        with patch.object(tareplay, "fixtures", return_value=fx), \
             patch.object(tareplay, "sim_paths", return_value=set()):
            selected, regions, functions, headers, why = tareplay.gated({"tools/tacli"}, "main")
        self.assertEqual(selected, set(fx))
        self.assertFalse(regions or functions or headers)
        self.assertTrue(why)

    def test_a_test_program_selects_every_fixture_and_no_function(self):
        fx = {"one": {"load": "one.json"}}
        with patch.object(tareplay, "fixtures", return_value=fx), \
             patch.object(tareplay, "sim_paths", return_value=set()):
            selected, regions, functions, headers, why = tareplay.gated(
                {"tools/replay/recmp_x_test.c", tareplay.DD + "src/tagpu_replay.c"}, "main")
        self.assertEqual(selected, set(fx))
        self.assertEqual(functions, {tareplay.DD + "src/tagpu_replay.c"})

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

    def test_check_takes_the_owners_acceptance(self):
        """a note the owner's decision wrote (every lane passed, its UNCOVERED landed on their word)
        meets the gate; any other result does not"""
        self.commit("tools/replay/x.c", "int f(void)\n{\n    return 2;\n}\n")
        sh(self.root, "git", "notes", "append", "-m",
           "replay-roundtrip: dll=x range=main..y lanes=all-pass result=fail date=2026-11-22", "HEAD")
        self.assertEqual(tareplay.landing_check("main"), 1)
        sh(self.root, "git", "notes", "append", "-m",
           "replay-roundtrip: dll=x range=main..y lanes=all-pass result=uncovered-accepted-by-owner "
           "date=2026-11-22 (run R: 1 UNCOVERED landed on the owner's word)", "HEAD")
        self.assertEqual(tareplay.landing_check("main"), 0)

    def test_a_stale_build_is_built_before_the_run(self):
        """the build's DLL, missing or older than its sources, is built through the run lock; a DLL
        the command gave is never rebuilt, only refused when stale"""
        made = []

        def make(argv, **kw):
            made.append(argv[2:])
            dll.parent.mkdir(parents=True, exist_ok=True)
            dll.write_bytes(b"MZ")
            return subprocess.CompletedProcess(argv, 0, "", "")
        dll = tareplay.build_dll(True, True)[0]
        args = types.SimpleNamespace(dll=None, stock_dll=None)
        with patch.object(tareplay, "sources_mtime", return_value=0), \
             patch.object(tareplay.subprocess, "run", side_effect=make):
            self.assertEqual(tareplay.chosen_dll("g", args, True, True), dll.resolve())
            self.assertEqual(made, [["make", "--", "-C", "tagpu/ddraw", "LIMITS=stock", "COVER=1"]])
            self.assertEqual(tareplay.chosen_dll("g", args, True, True), dll.resolve())
            self.assertEqual(len(made), 1)                        # current: not built again
        with patch.object(tareplay, "sources_mtime", return_value=dll.stat().st_mtime + 10), \
             patch.object(tareplay.subprocess, "run", side_effect=make):
            args.stock_dll = str(dll)
            with self.assertRaises(tareplay.CouldNotRun):
                tareplay.chosen_dll("g", args, True, True)
            self.assertEqual(len(made), 1)

    def test_check_asks_for_the_game_when_only_the_multiplayer_recorder_changed(self):
        path = self.root / tareplay.DD / "src" / "tagpu_recmp_x.c"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("int f(void)\n{\n    return 1;\n}\n")
        sh(self.root, "git", "add", "-A")
        sh(self.root, "git", "commit", "-qm", "recmp")
        with patch.object(tareplay, "fixtures", return_value={}), \
             patch.object(tareplay, "sim_paths", return_value=set()):
            self.assertEqual(tareplay.landing_check("main"), 1)


class Prelude(unittest.TestCase):
    """A sim file's prelude -- the lines above its first SIM_FEATURE -- in the landing tier, in
    Landing's throwaway repository."""

    setUp, tearDown, commit = Landing.setUp, Landing.tearDown, Landing.commit
    SRC = "src/tagpu_new.c"
    PATH = tareplay.DD + SRC
    BODY = ('SIM_FEATURE(newfeat, SIM_PUPPET_RUNS, "one");\n'
            "\n"
            "static int f(void)\n{\n    return 1;\n}\n")
    # a comment that opens on an #include and closes on the next line, as tagpu_patches.c has
    PRELUDE = ("/* tagpu_new.c -- a new sim file. */\n"
               "\n"
               "#include <string.h>   /* the first line of a comment\n"
               "                         that ends here */\n"
               '#include "tagpu_simreg.h"\n'
               "#define K(x) \\\n"
               "    ((x) + 1)\n"
               "\n")
    FIXTURES = {"one": {"load": "one.json"}, "stock-one": {"load": "one.json", "dll": "stock"}}

    def plan(self, includes=frozenset()):
        """landing_plan against main, the temporary file the only sim file."""
        feats, problems = tafeat.features_of(self.root / self.PATH, self.SRC)
        with patch.object(tareplay, "fixtures", return_value=self.FIXTURES), \
             patch.object(tareplay, "sim_paths", return_value={self.PATH}), \
             patch.object(tareplay, "includes_of", return_value=set(includes)), \
             patch.object(tafeat, "all_features", return_value=(feats, problems)):
            return tareplay.landing_plan("main")

    def add(self, path, text):
        (self.root / path).parent.mkdir(parents=True, exist_ok=True)
        (self.root / path).write_text(text)
        sh(self.root, "git", "add", "-A")
        sh(self.root, "git", "commit", "-qm", path)

    def to_main(self):
        """The work so far as the base, so that what follows is the landing."""
        sh(self.root, "git", "checkout", "-q", "main")
        sh(self.root, "git", "merge", "-q", "--ff-only", "work")
        sh(self.root, "git", "checkout", "-q", "work")

    @staticmethod
    def where(problems):
        """`KIND place` of each problem, its explanation dropped."""
        return [": ".join(q.split(": ")[:1]) for q in problems]

    def test_comment_or_directive_lines(self):
        text = self.PRELUDE + "static int g;\n" + '"s /*"\n' + "/* x */ int h;"
        n = self.PRELUDE.count("\n")
        self.assertEqual(tafeat.comment_or_directive_lines(text), set(range(1, n + 1)))

    def test_a_new_sim_files_prelude_is_covered(self):
        self.add(self.PATH, self.PRELUDE + self.BODY)
        fx, funcs, regions, problems, _, gated_any = self.plan()
        self.assertEqual(problems, [])
        self.assertTrue(gated_any)
        self.assertEqual(fx, {"one", tareplay.STOCK_FIXTURE})
        self.assertEqual(set(funcs), {(self.SRC, "f")})
        key = f"{self.SRC}:prelude"
        self.assertEqual(set(regions), {key, "newfeat"})   # newfeat: its SIM_FEATURE line
        self.assertNotIn(" ", key)              # the note's features= is split on spaces
        self.assertEqual(regions[key][:2], ([(self.SRC, "f")], tafeat.BOTH))
        got = {"raised": {(self.SRC, "f")}, "stock": {(self.SRC, "f")}}
        self.assertEqual(tareplay.uncovered(funcs, regions, got, fx, False), ([], 3, 3))

    def test_code_above_the_first_feature_fails_by_name(self):
        self.add(self.PATH, self.PRELUDE + "typedef int T;\nstatic int g;\n" + self.BODY)
        problems = self.plan()[3]
        n = self.PRELUDE.count("\n")
        self.assertEqual(self.where(problems), [f"UNFEATURED {self.PATH}:{n + 1}",
                                                f"UNFEATURED {self.PATH}:{n + 2}"])

    def test_each_build_must_reach_a_function_of_the_file(self):
        self.add(self.PATH, self.PRELUDE + self.BODY)
        self.to_main()
        self.commit(self.PATH, self.PRELUDE.replace("<string.h>", "<stdlib.h>") + self.BODY)
        fx, funcs, regions, problems, _, _ = self.plan()
        self.assertEqual((problems, funcs), ([], {}))
        self.assertEqual(fx, {"one", tareplay.STOCK_FIXTURE})
        per = {"one": {(self.SRC, "f")}, "stock-one": set()}
        got = tareplay.reached_by_build(per, self.FIXTURES)
        self.assertEqual(got, {"raised": {(self.SRC, "f")}, "stock": set()})
        missed, k, total = tareplay.uncovered(funcs, regions, got, fx, False)
        self.assertEqual((self.where(missed), k, total), ([f"UNCOVERED {self.SRC}:prelude"], 0, 1))
        self.assertTrue(missed[0].endswith("ran in the builds raised+stock"), missed[0])
        per["stock-one"] = {(self.SRC, "f")}
        got = tareplay.reached_by_build(per, self.FIXTURES)
        self.assertEqual(tareplay.uncovered(funcs, regions, got, fx, False), ([], 1, 1))

    def test_a_changed_header_asks_the_same_of_each_build(self):
        self.add(self.PATH, self.PRELUDE + self.BODY)
        self.to_main()
        header = tareplay.DD + "inc/tagpu_new.h"
        self.add(header, "#define NEW 1\n")
        fx, funcs, regions, problems, _, _ = self.plan(includes={header})
        self.assertEqual((problems, funcs), ([], {}))
        self.assertEqual(fx, {"one", tareplay.STOCK_FIXTURE})
        key = f"{self.SRC}:headers"
        self.assertEqual(set(regions), {key})
        self.assertEqual(regions[key][:2], ([(self.SRC, "f")], tafeat.BOTH))
        got = tareplay.reached_by_build({"one": {(self.SRC, "f")}}, self.FIXTURES)
        missed, _, _ = tareplay.uncovered(funcs, regions, got, fx, False)
        self.assertEqual(self.where(missed), [f"UNCOVERED {self.SRC}:headers"])


    def test_a_header_changed_in_comments_alone_selects_nothing(self):
        header = tareplay.DD + "inc/tagpu_new.h"
        self.add(self.PATH, self.PRELUDE + self.BODY)
        self.add(header, "#define NEW 1   /* one */\n")
        self.to_main()
        self.commit(header, "/* a note */\n#define NEW 1   /* one, said again */\n")
        fx, funcs, regions, problems, why, gated_any = self.plan(includes={header})
        self.assertEqual((fx, funcs, regions, problems, gated_any), (set(), {}, {}, [], False))
        self.assertTrue(any("only its comments or whitespace" in w for w in why), why)
        self.commit(header, "/* a note */\n#define NEW 2   /* one, said again */\n")
        self.assertEqual(set(self.plan(includes={header})[2]), {f"{self.SRC}:headers"})

    def test_unreached_behind_a_failed_two_player_game_is_said_under_it(self):
        funcs = {(self.SRC, "f"): ("newfeat", tafeat.BOTH), (self.SRC, "g"): ("newfeat", tafeat.BOTH)}
        got = {"raised": {(self.SRC, "f"), (self.SRC, "g")}, "stock": {(self.SRC, "g")}}
        missed, k, total = tareplay.uncovered(funcs, {}, got, set(), True, frozenset({"stock"}))
        self.assertEqual((self.where(missed), k, total),
                         (["UNREACHED BEHIND THE FAILED TWO-PLAYER GAME stock"], 1, 2))
        self.assertTrue(missed[0].endswith(f"{self.SRC}:f"), missed[0])
        missed, _, _ = tareplay.uncovered(funcs, {}, got, set(), True)
        self.assertEqual(self.where(missed), [f"UNCOVERED {self.SRC}:f (feature newfeat; builds "
                                              f"raised+stock; fixtures none; and the two-player game)"])
        got["raised"] = {(self.SRC, "g")}                # missing on a build whose game passed
        missed, _, _ = tareplay.uncovered(funcs, {}, got, set(), True, frozenset({"stock"}))
        self.assertTrue(self.where(missed)[0].startswith(f"UNCOVERED {self.SRC}:f"), missed)


class SimBuilds(unittest.TestCase):
    """tafeat.sim_builds: the builds whose simulation can run an engine-free file, from the lines
    of the other instrumented sources that name its functions."""

    FREE = ("int vm_run(int x)\n{\n    return helper(x);\n}\n"
            "static int\nhelper(int x)\n{\n    return x;\n}\n")

    def builds(self, callers, free=("src/vm.c",)):
        texts = {"src/vm.c": self.FREE, **callers}
        with patch.object(tafeat, "engine_free_files", return_value=set(free)):
            return tafeat.sim_builds("src/vm.c", texts)

    def test_a_caller_on_one_build_asks_that_build(self):
        raised = "#ifndef TAGPU_LIMITS_STOCK\nvoid g(void)\n{\n    vm_run(1);\n}\n#endif\n"
        self.assertEqual(self.builds({"src/a.c": raised}), frozenset({"raised"}))
        stock = "#ifdef TAGPU_LIMITS_STOCK\nstatic int (*p)(int) = vm_run;\n#endif\n"
        self.assertEqual(self.builds({"src/a.c": raised, "src/b.c": stock}), tafeat.BOTH)

    def test_a_name_in_a_comment_or_a_static_name_is_no_caller(self):
        both = "/* vm_run(1) */\nint helper2(void) { return 0; }\n"
        raised = "#ifndef TAGPU_LIMITS_STOCK\nvoid g(void) { vm_run(1); helper(2); }\n#endif\n"
        self.assertEqual(self.builds({"src/a.c": raised, "src/b.c": both}), frozenset({"raised"}))
        self.assertEqual(tafeat.external_functions(self.FREE), ["vm_run"])

    def test_no_caller_or_not_engine_free_asks_both(self):
        raised = "#ifndef TAGPU_LIMITS_STOCK\nvoid g(void) { vm_run(1); }\n#endif\n"
        self.assertEqual(self.builds({}), tafeat.BOTH)
        self.assertEqual(self.builds({"src/a.c": raised}, free=()), tafeat.BOTH)

    def test_the_cob_vm_runs_in_the_raised_game_alone(self):
        """the tree's own case: tagpu_patches.c's interpreter swap is the simulation's only caller
        of tagpu_cobvm.c, and it is compiled out of the stock-limits build"""
        d = tafeat.DDRAW
        texts = {r: (d / r).read_text() for r in tareplay.covered_sources() if (d / r).exists()}
        self.assertEqual(tafeat.sim_builds("src/tagpu_cobvm.c", texts), frozenset({"raised"}))


class EngineFree(unittest.TestCase):
    """ImpAI's engine-free half in the landing tier: the files `engine-free sim` in the tree's
    thread-split.allow, a change to one planned from the file itself in Landing's throwaway
    repository."""

    def test_the_brain_is_sim(self):
        """Every source of the Makefile's IMPAI_BRAIN_SRCS and the table's is sim, which the
        Makefile's COVERED reads as tafeat does, so the coverage build instruments it and the
        landing plans a change to it by its features. The list is make's own expansion of the
        variable, whatever form its rule takes, read in a scratch directory whose `src` is the
        tree's: the Makefile writes inc/git.h when it is parsed, and that write goes to the
        scratch directory, never into a tree a build may be reading."""
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "src").symlink_to(tafeat.DDRAW / "src")
            (Path(tmp) / "inc").mkdir()
            r = subprocess.run(["make", "-s", "--no-print-directory", "-f", str(tafeat.DDRAW / "Makefile"),
                                "-f", "-", "print-brain-srcs"],
                               input="print-brain-srcs:\n\t@echo $(IMPAI_BRAIN_SRCS)\n", cwd=tmp,
                               capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        srcs = set(r.stdout.split()) | {"src/impai_table.c"}
        self.assertTrue(all(re.fullmatch(r"src/impai_\w+\.c", x) for x in srcs), srcs)
        self.assertGreater(len(srcs), 7)
        self.assertLessEqual(srcs, set(tafeat.sim_files()))
        self.assertLessEqual({tareplay.DD + s for s in srcs}, tareplay.sim_paths())

    setUp, tearDown, commit = Landing.setUp, Landing.tearDown, Landing.commit
    add, to_main = Prelude.add, Prelude.to_main

    def plan(self, rel):
        path = tareplay.DD + rel
        feats, problems = tafeat.features_of(self.root / path, rel)
        with patch.object(tareplay, "fixtures", return_value=tareplay.fixtures()), \
             patch.object(tareplay, "sim_paths", return_value={path}), \
             patch.object(tareplay, "includes_of", return_value=set()), \
             patch.object(tafeat, "all_features", return_value=(feats, problems)):
            return feats, tareplay.landing_plan("main")

    def edit(self, rel, old, new):
        """The tree's `rel` as the base, then the landing: `old` replaced by `new` once."""
        text = (tafeat.DDRAW / rel).read_text()
        self.assertEqual(text.count(old), 1, old)
        self.add(tareplay.DD + rel, text)
        self.to_main()
        self.commit(tareplay.DD + rel, text.replace(old, new))

    def test_a_military_change_runs_its_groups_and_asks_its_function(self):
        self.edit("src/impai_mil.c", "#define REACH_PAD    384", "#define REACH_PAD    385")
        rel = "src/impai_mil.c"
        feats, (fx, funcs, regions, problems, _, gated_any) = self.plan(rel)
        feat = tafeat.feature_at(feats, rel, feats[0]["line"])
        self.assertEqual(problems, [])
        self.assertTrue(gated_any)
        self.assertIn("impai-fight", feat["fixtures"])
        self.assertEqual(fx, set(feat["fixtures"]) | {tareplay.STOCK_FIXTURE})
        self.assertEqual((funcs, set(regions)), ({}, {feat["name"]}))
        self.commit(tareplay.DD + rel, (self.root / tareplay.DD / rel).read_text().replace(
            "#define REACH_PAD    385", "#define REACH_PAD    384").replace(
            "(uint64_t)(dx * dx + dz * dz));", "(uint64_t)(dx * dx + dz * dz + 0));"))
        fx, funcs, regions, problems, _, _ = self.plan(rel)[1]
        self.assertEqual(problems, [])
        self.assertEqual(set(funcs), {(rel, "dist")})
        self.assertEqual(funcs[(rel, "dist")], (feat["name"], tafeat.BOTH))

    def test_a_table_alone_runs_its_groups_and_asks_no_function(self):
        rel = "src/impai_fitparams.c"
        focus = re.search(r"^ *\S+,   /\* focus \*/$", (tafeat.DDRAW / rel).read_text(), re.M).group(0)
        self.edit(rel, focus, focus.replace(",", " * 0.5f,", 1))
        feats, (fx, funcs, regions, problems, why, gated_any) = self.plan(rel)
        self.assertEqual(problems, [])
        self.assertTrue(gated_any)
        self.assertEqual(fx, set(feats[0]["fixtures"]) | {tareplay.STOCK_FIXTURE})
        self.assertEqual((funcs, regions), ({}, {}))
        self.assertTrue(any("holds no function" in w for w in why), why)

    def test_a_table_alones_prelude_and_headers_ask_no_function(self):
        rel = "src/impai_fitparams.c"
        header = tareplay.DD + "src/impai_fight.h"
        self.edit(rel, '#include "impai_fight.h"', '#include "impai_fight.h"\n#define FIT 1')
        self.add(header, "#define IMPAI_FIGHT_H_CHANGED 1\n")
        path = tareplay.DD + rel
        feats, problems = tafeat.features_of(self.root / path, rel)
        with patch.object(tareplay, "fixtures", return_value=tareplay.fixtures()), \
             patch.object(tareplay, "sim_paths", return_value={path}), \
             patch.object(tareplay, "includes_of", return_value={header}), \
             patch.object(tafeat, "all_features", return_value=(feats, problems)):
            fx, funcs, regions, problems, why, _ = tareplay.landing_plan("main")
        self.assertEqual((problems, funcs, regions), ([], {}, {}))
        self.assertEqual(fx, set(feats[0]["fixtures"]) | {tareplay.STOCK_FIXTURE})
        self.assertEqual(sum("the file holds no function" in w for w in why), 2, why)


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


def flagged(kind, first, last, payload, flags):
    """A block with these flags whose body is `payload` as it stands, packed or not."""
    head = struct.pack("<IHHIIIII", tarec.REC_BLOCK_MAGIC, kind, flags, first, last,
                       len(payload), len(payload), zlib.crc32(payload))
    return head + struct.pack("<I", zlib.crc32(head)) + payload


def sized(kind, payload, flags, raw, packed):
    """A block whose header claims `raw` and `packed` bytes, whatever its payload holds."""
    head = struct.pack("<IHHIIIII", tarec.REC_BLOCK_MAGIC, kind, flags, 1, 1, raw, packed,
                       zlib.crc32(payload))
    return head + struct.pack("<I", zlib.crc32(head)) + payload


def zstd_frame(data, wlog):
    """One zstd frame of `data` in a raw block, its header asking for a window of 2^wlog: no
    single segment, a 4-byte content size."""
    n = len(data)
    return (struct.pack("<IBBI", 0xFD2FB528, 0x80, (wlog - 10) << 3, n) +
            bytes([(1 | n << 3) & 0xFF, n >> 5 & 0xFF, n >> 13 & 0xFF]) + data)


class FormatBlocks(unittest.TestCase):
    """What the block walk refuses by name before any payload is read, as tagpu_recreader.c's
    block_flags_bad and segs_scan refuse it (tagpu_recfmt.h, RecBlock)."""

    def refused(self, *parts, why, decodes=(True, False)):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "f.imprec"
            path.write_bytes(tarec.REC_MAGIC + b"".join(parts))
            for decode in decodes:
                with self.assertRaisesRegex(tarec.Unreadable, why):
                    tarec.blocks(path, decode=decode)

    def test_flags(self):
        snap = tlv(tarec.RK_TICK, struct.pack("<I", 1))
        S, P, R, C = tarec.RBF_STORED, tarec.RBF_SECTIONS, tarec.RBF_FRAME_RESET, tarec.RBF_CHAINED
        for kind, flags in ((tarec.RB_HEADER, 0), (tarec.RB_SNAPSHOT, S | P),
                            (tarec.RB_STREAM, S | R), (tarec.RB_STREAM, S | 32),
                            (tarec.RB_SNAPSHOT, S | tarec.RBF_REFRESH),
                            (tarec.RB_EXACT, S | tarec.RBF_REFRESH),
                            (tarec.RB_STREAM, S | P), (tarec.RB_STREAM, S | P | R),
                            (tarec.RB_STREAM, C), (tarec.RB_SNAPSHOT, S | C)):
            with self.subTest(kind=kind, flags=flags):
                self.refused(header(), flagged(kind, 1, 1, snap, flags), why="block flags")

    def test_a_packed_body_of_no_bytes(self):
        self.refused(header(), flagged(tarec.RB_SNAPSHOT, 1, 1, b"", 0), why="no bytes")

    def test_a_sectioned_block_without_the_codec_entries(self):
        # refused where it is decoded: a walk that skips the stream (decode=False), like the
        # DLL's segment scan, does not read it
        self.refused(header(), flagged(tarec.RB_STREAM, 1, 1, bytes(40),
                                       tarec.RBF_SECTIONS | tarec.RBF_FRAME_RESET),
                     why="without RT_CODEC and RT_PHYSICS", decodes=(True,))

    def test_a_segment_of_other_slots(self):
        self.refused(header(slots=4), end_block(0), header(timeline=(1, 0, 2), slots=6),
                     why="a segment of 6 slots after 4")

    def test_a_block_past_what_it_may_hold(self):
        # as tagpu_recreader.c's block_head refuses it: a header that passed its CRC is what the
        # writer wrote, not a tear, whether the file holds its bytes or not
        big = 64 << 20
        body = b"\0" * 16
        for raw, packed in ((big + 1, len(body)), (len(body), big + 1)):
            with self.subTest(raw=raw, packed=packed):
                self.refused(header(), sized(tarec.RB_SNAPSHOT, body, 0, raw, packed),
                             why="past what it may hold")

    def test_a_zstd_window_past_2_27(self):
        # as tagpu_reccodec.c's reccodec_unpack refuses it (codectest.c checks the C side)
        import reccodec
        snap = tlv(tarec.RK_TICK, struct.pack("<I", 1))
        self.assertEqual(reccodec.unpack(zstd_frame(snap, 27), len(snap)), snap)
        with self.assertRaisesRegex(tarec.Unreadable, "past 2\\^27"):
            reccodec.unpack(zstd_frame(snap, 28), len(snap))
        frame = zstd_frame(snap, 28)
        self.refused(header(), sized(tarec.RB_SNAPSHOT, frame, 0, len(snap), len(frame)),
                     why="past 2\\^27", decodes=(True,))

    @staticmethod
    def snapshots(n):
        """n snapshot payloads, each a little different from the last"""
        return [tlv(tarec.RK_TICK, struct.pack("<I", 1 + i)) + bytes(range(i, i + 200)) for i in range(n)]

    @staticmethod
    def chained(snaps, keys):
        """the snapshots as blocks: those at `keys` packed alone, the rest against the one before"""
        import zstandard
        out = []
        for i, p in enumerate(snaps):
            if i in keys:
                out.append(sized(tarec.RB_SNAPSHOT, zstandard.ZstdCompressor(level=19).compress(p), 0,
                                 len(p), len(zstandard.ZstdCompressor(level=19).compress(p))))
                continue
            dic = zstandard.ZstdCompressionDict(snaps[i - 1], dict_type=zstandard.DICT_TYPE_RAWCONTENT)
            z = zstandard.ZstdCompressor(level=19, dict_data=dic, write_dict_id=False).compress(p)
            out.append(sized(tarec.RB_SNAPSHOT, z, tarec.RBF_CHAINED, len(p), len(z)))
        return out

    def test_a_snapshot_chain(self):
        # tagpu_reccodec.c's reccodec_unpack_chained and tagpu_recreader.c's snapshot_chain: each
        # link unpacks against the snapshot before it in its segment, at most 9 after a key
        snaps = self.snapshots(11)
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "f.imprec"
            path.write_bytes(tarec.REC_MAGIC + header() + b"".join(self.chained(snaps[:10], {0})))
            got = [p for k, _, _, p in tarec.blocks(path)[0] if k == tarec.RB_SNAPSHOT]
        self.assertEqual([bytes(p) for p in got], snaps[:10])
        self.refused(header(), *self.chained(snaps, {0}), why="past its key's 9 links", decodes=(True,))
        self.refused(header(), *self.chained(snaps[:2], set()), why="no snapshot before it",
                     decodes=(True,))
        # a header begins a segment: the snapshot before it is no prefix of the next one's
        self.refused(header(), self.chained(snaps[:1], {0})[0], end_block(1),
                     header(timeline=(1, 0, 1)), self.chained(snaps[:2], {0})[1],
                     why="no snapshot before it", decodes=(True,))

    def test_a_run(self):
        # tagpu_reccodec.c's reccodec_run_unpack: a run begins at a chunk whose header is the
        # writer's and goes on chunk by chunk; the C side is codectest.c's outcomes
        import reccodec
        import zstandard
        data = [bytes((i * 7 + j * 13) & 0xFF for j in range(5000)) for i in range(3)]

        def chunks(wlog=20, **kw):
            prm = zstandard.ZstdCompressionParameters.from_level(
                19, window_log=wlog, hash_log=20, chain_log=21, write_content_size=False,
                write_checksum=False, **kw)
            co = zstandard.ZstdCompressor(compression_params=prm).compressobj()
            return [co.compress(p) + co.flush(zstandard.COMPRESSOBJ_FLUSH_BLOCK) for p in data]
        c = chunks()
        run = reccodec.Run()
        self.assertEqual([run.feed(x, len(p), i == 0) for i, (x, p) in enumerate(zip(c, data))], data)
        self.assertEqual(run.before().feed(c[2], len(data[2]), False), data[2])
        with self.assertRaisesRegex(tarec.Unreadable, "did not start"):
            reccodec.Run().feed(c[1], len(data[1]), False)
        with self.assertRaisesRegex(tarec.Unreadable, "header is not the writer's"):
            reccodec.Run().feed(c[1], len(data[1]), True)
        with self.assertRaisesRegex(tarec.Unreadable, "header is not the writer's"):
            reccodec.Run().feed(chunks(21)[0], len(data[0]), True)
        with self.assertRaisesRegex(tarec.Unreadable, "next 4999 bytes"):
            reccodec.Run().feed(c[0], len(data[0]) - 1, True)
        run = reccodec.Run()
        run.feed(c[0], len(data[0]), True)
        try:
            self.assertNotEqual(run.feed(c[2], len(data[2]), False), data[2])
        except tarec.Unreadable:
            pass                                    # a chunk out of its run: refused or wrong
        run = reccodec.Run()
        run.feed(c[0], len(data[0]), True)
        with self.assertRaisesRegex(tarec.Unreadable, "not its run's next"):
            run.feed(c[1][:len(c[1]) // 2], len(data[1]), False)
        with self.assertRaisesRegex(tarec.Unreadable, "did not start"):
            run.feed(c[1], len(data[1]), False)          # the run ended at the refusal


class MultiplayerGroup(unittest.TestCase):
    """The two-player game in the landing tier, its tools standing in: what selects it, and that
    a failed step fails by name instead of reading as a function nobody reached."""

    REACHED = {("src/tagpu_recorder.c", "mp_start_send"), ("src/tagpu_recmp.c", "not_graded")}

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.out = Path(self.tmp.name)
        self.dll = self.out / "ddraw-cover.dll"
        self.covers = []

    def tearDown(self):
        self.tmp.cleanup()

    def fake_step(self, game=0, play=0, covers=2, impai=False, takeover=0):
        self.steps = []

        def step(argv, log_path):
            argv = [str(a) for a in argv]
            log_path.write_text("first line\nthe last line\n")
            d = Path(argv[argv.index("--out") + 1])
            d.mkdir(parents=True)
            if "recmp_create_begin_test.py" in argv[0]:
                self.steps.append("game")
                (d / "native.imprec").write_text("r")
                (d / "tested-ddraw.dll").write_text("d")
                for k in range(covers):
                    (d / f"cover-{k}.bin").write_text("c")
                self.assertIn("--chat", argv)
                self.assertIn("--select", argv)
                self.assertEqual(argv[argv.index("--label") + 1], tareplay.MP_LABELS["raised"])
                self.assertEqual("--impai" in argv, impai)
                return game
            if "--takeover" in argv:
                self.steps.append("takeover")
                self.assertTrue(impai, "a takeover in the stock form")
                self.assertEqual(argv[argv.index("--takeover") + 1], "450:0")
                self.assertEqual(argv[argv.index("--seatai") + 1], tareplay.MP_SEATAI)
                self.assertEqual(argv[argv.index("--cover") + 1], "takeover")
                (d / "cover-takeover.bin").write_text("c")
                (d / "fork-native.imprec").write_text("f")
                return takeover
            self.steps.append("play")
            self.assertEqual(argv[argv.index("--cover") + 1], "play")
            self.assertIn("--to-end", argv)
            (d / "native.imprec").write_text("r")
            (d / "cover-play.bin").write_text("c")
            return play
        return step

    def group(self, step, check=(0, "0 corrections"), keep=False, read=None, impai=False):
        def reached(mapping, covers):
            self.covers = list(covers)
            return self.REACHED
        with patch.object(tareplay, "mp_step", step), \
             patch.object(tareplay, "tarec_run", return_value=check), \
             patch.object(tareplay, "mp_takeover_at", return_value=450), \
             patch.object(tareplay, "covered_sources", return_value={"src/tagpu_recorder.c"}), \
             patch.object(tareplay.tafeat, "read_map", side_effect=read, return_value={}), \
             patch.object(tareplay.tafeat, "reached", reached):
            return tareplay.multiplayer_group("raised", self.dll, self.out, keep, impai=impai)

    def test_what_selects_it(self):
        want = tareplay.wants_multiplayer
        self.assertTrue(want([tareplay.DD + "src/tagpu_recorder.c"]))
        self.assertTrue(want([tareplay.DD + "src/tagpu_recmp_units.c"]))
        self.assertTrue(want([tareplay.DD + "inc/tagpu_recmp.h"]))
        self.assertTrue(want(["tools/replay/tarec.py"]))
        self.assertTrue(want([tareplay.DD + "src/impai_mil.c"]))
        self.assertFalse(want([tareplay.DD + "src/tagpu_zoom.c"]))
        self.assertFalse(want(["README.md"]))

    def test_what_selects_the_impai_form(self):
        want = tareplay.wants_impai_multiplayer
        for p in ("src/impai_mil.c", "src/impai_seats.h", "src/tagpu_impai.c", "src/tagpu_skirmish.c"):
            self.assertTrue(want([tareplay.DD + p]), p)
        for p in (tareplay.DD + "src/tagpu_recorder.c", tareplay.DD + "tests/impai_brain_test.c",
                  "tools/impai/predfit.py"):
            self.assertFalse(want([p]), p)

    def test_the_impai_form_takes_the_recording_over_and_counts_its_coverage(self):
        reach, problems = self.group(self.fake_step(impai=True), impai=True)
        self.assertEqual(problems, [])
        self.assertEqual(self.steps, ["game", "play", "takeover"])
        self.assertEqual(sorted(c.name for c in self.covers),
                         ["cover-0.bin", "cover-1.bin", "cover-play.bin", "cover-takeover.bin"])
        self.assertFalse((self.out / "multiplayer-raised" / "takeover" / "fork-native.imprec").exists())

    def test_a_failed_takeover_is_a_failure_by_name(self):
        reach, problems = self.group(self.fake_step(impai=True, takeover=1), impai=True)
        self.assertEqual(reach, set())
        self.assertIn("MULTIPLAYER raised: the takeover failed: the last line", problems[0])

    def test_the_stock_form_takes_nothing_over(self):
        _, problems = self.group(self.fake_step())
        self.assertEqual(problems, [])
        self.assertEqual(self.steps, ["game", "play"])

    def test_a_passing_group_reports_the_graded_functions_and_drops_the_big_files(self):
        reach, problems = self.group(self.fake_step())
        self.assertEqual(problems, [])
        self.assertEqual(reach, {("src/tagpu_recorder.c", "mp_start_send")})
        self.assertEqual(sorted(c.name for c in self.covers),
                         ["cover-0.bin", "cover-1.bin", "cover-play.bin"])
        game = self.out / "multiplayer-raised" / "game"
        self.assertFalse((game / "native.imprec").exists())
        self.assertFalse((game / "tested-ddraw.dll").exists())
        self.assertTrue((game / "cover-0.bin").exists())

    def test_keep_keeps_the_recording(self):
        _, problems = self.group(self.fake_step(), keep=True)
        self.assertEqual(problems, [])
        self.assertTrue((self.out / "multiplayer-raised" / "game" / "native.imprec").exists())

    def test_a_failed_game_is_a_failure_by_name_and_reaches_nothing(self):
        reach, problems = self.group(self.fake_step(game=1))
        self.assertEqual(reach, set())
        self.assertEqual(len(problems), 1)
        self.assertIn("MULTIPLAYER raised: the game failed: the last line", problems[0])

    def test_a_failed_playing_is_a_failure(self):
        reach, problems = self.group(self.fake_step(play=1))
        self.assertEqual(reach, set())
        self.assertIn("MULTIPLAYER raised: the playing failed", problems[0])

    def test_a_failed_tarec_check_is_a_failure(self):
        reach, problems = self.group(self.fake_step(), check=(1, "a correction\n3 corrections"))
        self.assertEqual(reach, set())
        self.assertIn("MULTIPLAYER raised: tarec check: 3 corrections", problems[0])

    def test_a_missing_coverage_file_is_a_failure(self):
        reach, problems = self.group(self.fake_step(covers=1))
        self.assertEqual(reach, set())
        self.assertIn("2 coverage files", problems[0])

    def test_an_unreadable_map_is_a_failure(self):
        reach, problems = self.group(self.fake_step(), read=OSError("no map"))
        self.assertEqual(reach, set())
        self.assertIn("the coverage: no map", problems[0])


class CodecCheck(unittest.TestCase):
    """The gate's stream codec verdict from `tarec info`'s lines (tareplay.codec_check)."""

    @staticmethod
    def info(declined="0", mismatched="0", blocks=40, sectioned=38):
        return (f"  stream blocks: {blocks}, {sectioned} sectioned (3 restarting the frame), "
                f"{blocks - sectioned} records\n"
                f"  the writer's codec: {declined} blocks declined, {mismatched} mismatched\n")

    def test_verdicts(self):
        for info, verdict in ((self.info(), tareplay.PASS),
                              (self.info(declined="2"), tareplay.PASS),
                              (self.info(mismatched="1"), tareplay.FAIL),
                              (self.info(mismatched="absent"), tareplay.FAIL),
                              (self.info(sectioned=0), tareplay.FAIL),
                              (self.info(blocks=0, sectioned=0), tareplay.FAIL),
                              ("  ended: complete\n", tareplay.UNRUN)):
            with self.subTest(info=info):
                self.assertEqual(tareplay.codec_check(info)[1], verdict)

    def test_the_decline_lever_needs_a_declined_block(self):
        # a fixture under tagpu_rec.decline whose recording declined nothing never ran the fallback
        for info, verdict in ((self.info(declined="9"), tareplay.PASS),
                              (self.info(declined="0"), tareplay.FAIL),
                              (self.info(declined="absent"), tareplay.FAIL)):
            with self.subTest(info=info):
                self.assertEqual(tareplay.codec_check(info, decline=True)[1], verdict)
        self.assertIn("declined no block", tareplay.codec_check(self.info(), decline=True)[2])

    def test_a_codec_that_never_ran_is_named(self):
        # a writer without memory for the frame: every block records, the end block counts 0 and 0
        label, verdict, detail = tareplay.codec_check(self.info(sectioned=0))
        self.assertEqual(verdict, tareplay.FAIL)
        self.assertIn("never ran", detail)


class LedgerSum(unittest.TestCase):
    """The unit frame's ledger rule adds singles in integers (reccodec.f32_add, the C codec's
    reccodec_f32_add): the sum numpy's float32 gives, for every finite result."""

    def test_matches_float32(self):
        import numpy as np
        import reccodec
        rng = np.random.default_rng(12)
        a = rng.integers(0, 1 << 32, 20000, dtype=np.uint64).astype(np.uint32)
        b = rng.integers(0, 1 << 32, 20000, dtype=np.uint64).astype(np.uint32)
        # half the pairs near each other's exponent, a quarter near cancellation
        b[::2] = (a[::2] & 0xFF800000) ^ (b[::2] & 0x807FFFFF) ^ ((b[::2] % 24) << 23).astype(np.uint32)
        b[1::4] = (a[1::4] ^ 0x80000000) + (b[1::4] % 64) - 32
        with np.errstate(over="ignore", invalid="ignore"):
            want = (a.view(np.float32) + b.view(np.float32)).view(np.uint32)
        finite = lambda v: (v >> 23 & 0xFF) != 0xFF
        checked = 0
        for x, y, w in zip(a.tolist(), b.tolist(), want.tolist()):
            if finite(x) and finite(y) and finite(w):
                checked += 1
                self.assertEqual(reccodec.f32_add(x, y), w, f"{x:08x} + {y:08x}")
        self.assertGreater(checked, 15000)


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
        with self.assertRaisesRegex(tarec.Unreadable, "timeline 1's first iteration.* can is 5"):
            tarec.fork_prediction(segs, 1, 4)
        self.assertIsNone(tarec.fork_prediction(segs, 1, 5))
        self.assertIsNone(tarec.fork_prediction(segs, 0, 4))
        # the original's first iteration has none before it to append after
        with self.assertRaisesRegex(tarec.Unreadable, "timeline 0's first iteration.* can is 2"):
            tarec.fork_prediction(segs, 0, 1)
        self.assertIsNone(tarec.fork_prediction(segs, 0, 2))
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


def hold_entry(refresh=30, flags=0, unit_held=(), node_held=()):
    """RT_HOLD: the refresh interval, the flags, a zero SHA-256 and the two masks, with each
    dword of `unit_held` and `node_held` held whole."""
    unit, node = bytearray(tarec.REC_UNIT_IMAGE), bytearray(tarec.REC_ORDER_IMAGE)
    for d in unit_held:
        unit[4 * d:4 * d + 4] = b"\xff" * 4
    for d in node_held:
        node[4 * d:4 * d + 4] = b"\xff" * 4
    return tlv(tarec.RT_HOLD, struct.pack("<II", refresh, flags) + bytes(32) + unit + node)


def held_header(timeline=None, **hold):
    """header(timeline), RT_HOLD appended: its entries are read in any order."""
    raw = header(timeline)
    p = raw[32:] + hold_entry(**hold)
    return block(tarec.RB_HEADER, 0, 0, p)


def delta(slot, values):
    """An RK_UNIT_DELTA of `slot` naming {dword: value}."""
    bits = sum(1 << d for d in values)
    head = struct.pack("<H", slot) + bits.to_bytes(tarec.REC_UNIT_BITS, "little")
    head += bytes(tarec.REC_UNIT_DELTA_HEAD - len(head))
    return tlv(tarec.RK_UNIT_DELTA, head + b"".join(struct.pack("<I", values[d])
                                                      for d in sorted(values)))


class LossyStream(unittest.TestCase):
    """tarec's lossy-check, --exact and refresh_at against a file whose stream holds unit dword
    HELD between the refreshes at GameTime 2 and 31 (RT_HOLD's interval 30; 1 and 2 are the
    segment's first two iterations), and whose exact stream (RB_EXACT) does not."""

    HELD = 50

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

    @staticmethod
    def ticks(first, last, extra):
        """Iterations first..last, `extra` {GameTime: records} after each one's RK_TICK."""
        return b"".join(tlv(tarec.RK_TICK, struct.pack("<I", t)) + extra.get(t, b"")
                        for t in range(first, last + 1))

    def parts(self, lossy3=None, lossy31=None, refresh31=True, refresh2=True, exact=True,
              header=None, last=31, gone=None):
        """The file: iteration 1 makes unit 1, the exact stream changes its dword HELD and dword
        10 at 3; the lossy stream carries `lossy3` there (dword 10 alone by default) and `lossy31`
        at the refresh 31 (dword HELD's change by default). The second iteration ends a block of
        its own, a refresh block unless `refresh2` is false; the stream runs to `last`. With
        `gone`, both streams end unit 1 at that GameTime."""
        S, F = tarec.RBF_STORED, tarec.RBF_REFRESH
        first = (tlv(tarec.RK_UNIT, struct.pack("<HH", 1, 0) + bytes(tarec.REC_UNIT_IMAGE)) +
                 tlv(tarec.RK_MAPPED, struct.pack("<I", 0) + bytes(2)))
        x = {3: delta(1, {self.HELD: 7, 10: 9})}
        lossy = {3: delta(1, {10: 9}) if lossy3 is None else lossy3,
                 31: delta(1, {self.HELD: 7}) if lossy31 is None else lossy31}
        if gone is not None:
            x[gone] = lossy[gone] = tlv(tarec.RK_UNIT_GONE, struct.pack("<HH", 1, 0))
        x, lossy = self.ticks(3, last, x), self.ticks(3, last, lossy)
        out = [header or held_header(unit_held=(self.HELD,)),
               flagged(tarec.RB_STREAM, 1, 1, self.ticks(1, 1, {1: first}), S | F),
               flagged(tarec.RB_EXACT, 1, 1, self.ticks(1, 1, {1: first}), S),
               flagged(tarec.RB_STREAM, 2, 2, self.ticks(2, 2, {}), S | (F if refresh2 else 0)),
               flagged(tarec.RB_EXACT, 2, 2, self.ticks(2, 2, {}), S),
               flagged(tarec.RB_STREAM, 3, last, lossy, S | (F if refresh31 else 0))]
        if exact:
            out.append(flagged(tarec.RB_EXACT, 3, last, x, S))
        return out + [end_block(last)]

    def lossy_check(self, path, *args):
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            rc = tarec.main(["tarec", "lossy-check", path, *args])
        return rc, out.getvalue()

    def test_a_held_dword_written_at_the_refresh_passes(self):
        rc, o = self.lossy_check(self.file(*self.parts()))
        self.assertEqual(rc, 0, o)
        self.assertIn("31 iterations to GameTime 31, 3 refreshes", o)

    def test_an_iteration_whose_only_unit_record_ends_a_unit_passes(self):
        """a held table and an iteration that touches no unit live in both streams afterwards"""
        rc, o = self.lossy_check(self.file(*self.parts(last=5, gone=5, refresh31=False)))
        self.assertEqual(rc, 0, o)
        self.assertIn("5 iterations to GameTime 5", o)

    def test_to_stops_the_walk(self):
        rc, o = self.lossy_check(self.file(*self.parts()), "--to", "10")
        self.assertEqual(rc, 0, o)
        self.assertIn("10 iterations to GameTime 10", o)

    def test_a_shown_dword_held_fails(self):
        rc, o = self.lossy_check(self.file(*self.parts(lossy3=b"")))
        self.assertEqual(rc, 1, o)
        self.assertIn("GameTime 3: slot 1's shown bytes differ", o)

    def test_a_held_value_made_up_fails(self):
        rc, o = self.lossy_check(self.file(*self.parts(lossy3=delta(1, {10: 9, self.HELD: 3}))))
        self.assertEqual(rc, 1, o)
        self.assertIn("held bytes are neither the stream's last nor the exact ones", o)

    def test_a_refresh_that_misses_a_held_change_fails(self):
        rc, o = self.lossy_check(self.file(*self.parts(lossy31=b"")))
        self.assertEqual(rc, 1, o)
        self.assertIn("GameTime 31: the stream's world is not the exact copy at a refresh", o)

    def test_a_refresh_off_its_block_fails(self):
        rc, o = self.lossy_check(self.file(*self.parts(refresh31=False)))
        self.assertEqual(rc, 1, o)
        self.assertIn("GameTime 31: a refresh iteration that does not end a refresh block", o)

    def test_the_second_iteration_is_a_refresh(self):
        rc, o = self.lossy_check(self.file(*self.parts(refresh2=False)))
        self.assertEqual(rc, 1, o)
        self.assertIn("GameTime 2: a refresh iteration that does not end a refresh block", o)

    def test_the_grid(self):
        """the writer's rule (tagpu_recrules.h, reclossy_is_refresh): the first two iterations,
        then 1 modulo the interval, or the first iteration past one a gap skips"""
        self.assertEqual([tarec.is_refresh(n, t, p, 30) for n, t, p in
                          ((1, 500, 0), (2, 501, 500), (3, 502, 501), (3, 511, 501),
                           (3, 512, 501), (5, 535, 511))], [True, True, False, True, True,
                                                             False])
        refresh, prev = [], 0
        for n, t in enumerate([1, 2, 3, 30, 31, 32, 60, 62, 91], 1):
            if tarec.is_refresh(n, t, prev, 30):
                refresh.append(t)
                prev = t
        self.assertEqual(refresh, [1, 2, 31, 62, 91])

    def test_no_exact_stream_cannot_be_checked(self):
        path = self.file(*self.parts(exact=False))
        rc, o = self.lossy_check(path)
        self.assertEqual(rc, 2, o)
        self.assertIn("recorded without tagpu_rec.exact", o)
        with self.assertRaisesRegex(tarec.Unreadable, "recorded without tagpu_rec.exact"):
            tarec.load(path, exact=True)

    def test_a_skirmish_without_rt_hold_fails(self):
        rc, o = self.lossy_check(self.file(*self.parts(header=header())))
        self.assertEqual(rc, 1, o)
        self.assertIn("a skirmish header without RT_HOLD", o)

    def test_exact_reads_the_exact_records(self):
        path = self.file(*self.parts())
        for exact, held in ((False, 0), (True, 7)):
            h, bl, _ = tarec.load(path, exact=exact)
            w = tarec.stream_to(h, bl, want=3)
            self.assertEqual(struct.unpack_from("<I", w.units[1], 4 * self.HELD)[0], held)
        h, bl, _ = tarec.load(path)
        w = tarec.stream_to(h, bl, want=31)
        self.assertEqual(struct.unpack_from("<I", w.units[1], 4 * self.HELD)[0], 7)

    def test_refresh_at(self):
        path = self.file(*self.parts())
        # a request inside the first interval rounds to the second iteration
        self.assertEqual([tarec.refresh_at(path, 0, T) for T in (1, 2, 3, 30, 31)],
                         [1, 2, 2, 2, 31])
        with self.assertRaisesRegex(tarec.Unreadable, "outside timeline 0's chain"):
            tarec.refresh_at(path, 0, 32)

    def test_rt_hold_bounds(self):
        for hold, why in (({"refresh": 20}, "refresh interval 20"),
                          ({"refresh": 301}, "refresh interval 301"),
                          ({"flags": 4}, "flags 0x4"),
                          ({"unit_held": (0x6C // 4,)}, "unit position")):
            with self.subTest(**hold):
                with self.assertRaisesRegex(tarec.Unreadable, why):
                    tarec.header(held_header(**hold)[32:])
        with self.assertRaisesRegex(tarec.Unreadable, "of 12 bytes"):
            tarec.header(header()[32:] + tlv(tarec.RT_HOLD, bytes(12)))


class SegmentRules(unittest.TestCase):
    """What a chain's segments may differ in (tarec.shape_differs, as tagpu_recreader.c's), which
    segment holds nothing (holds_nothing) and where a takeover starts (takeover_at): T itself in a
    segment that holds nothing, else the refresh at or before T."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    @staticmethod
    def parsed(raw):
        return tarec.header(raw[32:])

    def mp(self, raw):
        return dict(self.parsed(raw), multiplayer=b"roster", mp_seats=b"seats", mode=3)

    def test_shape_rules(self):
        cells = self.parsed(held_header(flags=tarec.RECL_CELLS))
        self.assertIsNone(tarec.shape_differs(cells, self.mp(header())))
        self.assertEqual(tarec.shape_differs(self.mp(header()), cells), "multiplayer roster")
        self.assertEqual(tarec.shape_differs(cells, self.parsed(held_header(flags=0))),
                         "held cells")
        self.assertEqual(tarec.shape_differs(cells, self.parsed(header())), "held cells")
        self.assertIsNone(tarec.shape_differs(self.parsed(held_header(flags=0)),
                                              self.parsed(header())))

    def test_holds_nothing(self):
        for raw, nothing in ((header(), True), (held_header(flags=0), True),
                             (held_header(flags=tarec.RECL_CELLS), False),
                             (held_header(flags=tarec.RECL_SPRAYS), False),
                             (held_header(unit_held=(5,)), False)):
            with self.subTest(nothing=nothing):
                self.assertEqual(tarec.holds_nothing(self.parsed(raw)), nothing)

    def test_takeover_at(self):
        S, F = tarec.RBF_STORED, tarec.RBF_REFRESH
        first = (tlv(tarec.RK_UNIT, struct.pack("<HH", 1, 0) + bytes(tarec.REC_UNIT_IMAGE)) +
                 tlv(tarec.RK_MAPPED, struct.pack("<I", 0) + bytes(2)))
        ticks = LossyStream.ticks
        root = [header(), flagged(tarec.RB_STREAM, 1, 1, ticks(1, 1, {1: first}), S | F),
                flagged(tarec.RB_STREAM, 2, 40, ticks(2, 40, {}), S), end_block(40, 13)]
        child = [held_header(timeline=(1, 0, 21), flags=0, unit_held=(5,)),
                 flagged(tarec.RB_STREAM, 21, 21, ticks(21, 21, {21: first}), S | F),
                 flagged(tarec.RB_STREAM, 22, 22, ticks(22, 22, {}), S | F),
                 flagged(tarec.RB_STREAM, 23, 50, ticks(23, 50, {}), S | F), end_block(50)]
        path = Path(self.tmp.name) / "r.imprec"
        path.write_bytes(tarec.REC_MAGIC + b"".join(root + child))
        path = str(path)
        # the root holds nothing: every iteration of its part is a start
        self.assertEqual([tarec.takeover_at(path, 1, T) for T in (2, 10, 20)], [2, 10, 20])
        # the child holds dword 5: a request rounds to its refresh at or before, and none starts
        # at its first iteration
        self.assertEqual([tarec.takeover_at(path, 1, T) for T in (22, 30, 50)], [22, 22, 50])
        with self.assertRaisesRegex(tarec.Unreadable, "timeline 1's first iteration.*is 22"):
            tarec.takeover_at(path, 1, 21)
        with self.assertRaisesRegex(tarec.Unreadable, "timeline 0's first iteration.*is 2"):
            tarec.takeover_at(path, 1, 1)
        self.assertEqual(tarec.refresh_at(path, 1, 10), 1)
        self.assertTrue(tarec.holds_nothing(tarec.segment_at(path, 1, 20)["header"]))
        self.assertFalse(tarec.holds_nothing(tarec.segment_at(path, 1, 21)["header"]))
        with self.assertRaisesRegex(tarec.Unreadable, "outside timeline 1's chain"):
            tarec.takeover_at(path, 1, 51)

    def test_event_check_yards_per_segment(self):
        ec = tarec.EventCheck()
        for mode, yards in ((3, False), (2, True), (3, False)):
            ec.on_record(SimpleNamespace(header={"mode": mode}), tarec.RK_TICK, b"")
            self.assertEqual(ec.yards, yards)
        ec = tarec.EventCheck(yards=True)
        ec.on_record(SimpleNamespace(header={"mode": 3}), tarec.RK_TICK, b"")
        self.assertTrue(ec.yards)


class LossyCells(unittest.TestCase):
    """lossy-check's cells clause (RT_HOLD's RECL_CELLS) on the 2 x 2 grid of header(): the
    exact stream gives cell (0, 0) slot A 1 and byte 5 the value 9 at GameTime 3, and the tick's
    input at 4 a slot of cell (1, 0) alone; the lossy stream carries cell (0, 0) with its slot
    held until the refresh at 31 by default, and drops that input."""

    CB = tarec.REC_CELL_BYTES

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def run_check(self, *parts):
        p = Path(self.tmp.name) / "c.imprec"
        p.write_bytes(tarec.REC_MAGIC + b"".join(parts))
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            rc = tarec.main(["tarec", "lossy-check", str(p)])
        return rc, out.getvalue()

    def cell(self, x, z, slot=0, b5=0):
        c = bytearray(self.CB)
        struct.pack_into("<H", c, 0, slot)
        c[5] = b5
        return tlv(tarec.RK_CELLS, struct.pack("<HHHH", x, z, 1, 0) + bytes(c))

    def tick_input(self, run):
        call = bytes([tarec.FR_TICK, 0, 0, 0]) + bytes(4)
        return (tlv(tarec.RK_EV_FEATURE, call) + run + tlv(tarec.RK_EV_FEATURE_END, call))

    def parts(self, lossy3=None, lossy4=None, lossy31=None):
        S, F = tarec.RBF_STORED, tarec.RBF_REFRESH
        image = b"".join(tlv(tarec.RK_CELLS, struct.pack("<HHHH", 0, z, 2, 0) + bytes(2 * self.CB))
                         for z in (0, 1))
        first = LossyStream.ticks(1, 1, {1: image})
        x = LossyStream.ticks(3, 31, {3: self.cell(0, 0, 1, 9),
                                      4: self.tick_input(self.cell(1, 0, 2))})
        lossy = LossyStream.ticks(3, 31, {
            3: self.cell(0, 0, 0, 9) if lossy3 is None else lossy3,
            4: self.tick_input(b"") if lossy4 is None else lossy4,
            31: self.cell(0, 0, 1, 9) if lossy31 is None else lossy31})
        return [held_header(flags=tarec.RECL_CELLS),
                flagged(tarec.RB_STREAM, 1, 1, first, S | F),
                flagged(tarec.RB_EXACT, 1, 1, first, S),
                flagged(tarec.RB_STREAM, 2, 2, LossyStream.ticks(2, 2, {}), S | F),
                flagged(tarec.RB_EXACT, 2, 2, LossyStream.ticks(2, 2, {}), S),
                flagged(tarec.RB_STREAM, 3, 31, lossy, S | F),
                flagged(tarec.RB_EXACT, 3, 31, x, S), end_block(31)]

    def test_held_slots_written_at_the_refresh_pass(self):
        rc, o = self.run_check(*self.parts())
        self.assertEqual(rc, 0, o)
        self.assertIn("31 iterations to GameTime 31, 3 refreshes", o)

    def test_a_shown_byte_of_a_cell_held_fails(self):
        rc, o = self.run_check(*self.parts(lossy3=self.cell(0, 0, 0, 0)))
        self.assertEqual(rc, 1, o)
        self.assertIn("GameTime 3: cell 0's shown bytes differ", o)

    def test_a_slot_made_up_fails(self):
        rc, o = self.run_check(*self.parts(lossy3=self.cell(0, 0, 3, 9)))
        self.assertEqual(rc, 1, o)
        self.assertIn("GameTime 3: cell 0's held bytes are neither", o)

    def test_an_input_of_held_bytes_alone_written_fails(self):
        rc, o = self.run_check(*self.parts(lossy4=self.tick_input(self.cell(1, 0, 2))))
        self.assertEqual(rc, 1, o)
        self.assertIn("GameTime 4: the tick's input runs are not the exact stream's", o)

    def test_a_refresh_that_misses_a_held_slot_fails(self):
        rc, o = self.run_check(*self.parts(lossy31=b""))
        self.assertEqual(rc, 1, o)
        self.assertIn("GameTime 31: the stream's world is not the exact copy at a refresh", o)

    def test_held_cells_beside_spray_sessions_pass(self):
        parts = self.parts()
        parts[0] = held_header(flags=tarec.RECL_CELLS | tarec.RECL_SPRAYS)
        rc, o = self.run_check(*parts)
        self.assertEqual(rc, 0, o)


class Occupancy(unittest.TestCase):
    """tarec.occupancy on a 6 x 4 grid: THE OCCUPANCY RULE, the one rule wherever a playing's
    grid is the engine's (a multiplayer segment, and a skirmish segment that holds its cells after
    a seek)."""

    CB = tarec.REC_CELL_BYTES

    @staticmethod
    def unit(x, z, w=1, h=1, state=1):
        u = bytearray(tarec.REC_UNIT_IMAGE)
        struct.pack_into("<hh", u, 0x76, x, z)
        struct.pack_into("<hh", u, 0x7E, w, h)
        struct.pack_into("<I", u, tarec.U_STATE, state)
        return u

    def worlds(self, units, names):
        """(expected, dump): `units` {slot: image} live in both, the dump's cells naming
        {(x, z): (slot A, slot B)}"""
        a = tarec.World(8, [], dims=(6, 4, 1, 1))
        b = tarec.World(8, [], dims=(6, 4, 1, 1))
        for slot, u in units.items():
            a.units[slot] = bytearray(u)
            b.units[slot] = bytearray(u)
        for (x, z), pair in names.items():
            struct.pack_into("<HH", b.grid, (z * 6 + x) * self.CB, *pair)
        return a, b

    def test_a_unit_named_passes_and_the_slots_are_taken(self):
        a, b = self.worlds({1: self.unit(1, 1)}, {(1, 1): (1, 0)})
        self.assertEqual(tarec.occupancy(a, b), [])
        self.assertEqual(a.grid, b.grid)

    def test_overlap_names_the_holder(self):
        # two layer-1 units overlapping at (2, 1), which names the holder: the contest's marks
        # make it a state of the stamp's, and without them it is none (a contest unmarked)
        units = {1: self.unit(1, 1, 2, 1, state=1 << 26 | 1),
                 2: self.unit(2, 1, 2, 1, state=1 << 27 | 1)}
        names = {(1, 1): (1, 0), (2, 1): (1, 0), (3, 1): (2, 0)}
        a, b = self.worlds(units, names)
        self.assertEqual(tarec.occupancy(a, b), [])
        units = {1: self.unit(1, 1, 2, 1), 2: self.unit(2, 1, 2, 1)}
        a, b = self.worlds(units, names)
        out = tarec.occupancy(a, b)
        self.assertEqual(len(out), 1, out)
        self.assertIn("cell (2, 1) of mobile unit 2's footprint does not name it in slot A", out[0])

    def test_contest_the_stamp_bound(self):
        # parked: past the last column, past the last row, west of the first column
        a, b = self.worlds({1: self.unit(5, 1, 2), 2: self.unit(1, 3, 1, 2),
                            3: self.unit(-1, 0, 2)}, {})
        self.assertEqual(tarec.occupancy(a, b), [])
        # the last column itself is stamped (fix_last_cell)
        a, b = self.worlds({1: self.unit(5, 1)}, {})
        self.assertEqual(len(tarec.occupancy(a, b)), 1)
        # a carried unit, layer 0, holds no cell
        a, b = self.worlds({1: self.unit(1, 1, state=0)}, {})
        self.assertEqual(tarec.occupancy(a, b), [])

    def test_contest_a_loser_under_the_holder(self):
        units = {1: self.unit(1, 1, 2, 1, state=1 << 26 | 1), 2: self.unit(2, 1, state=1 << 27 | 1)}
        a, b = self.worlds(units, {(1, 1): (1, 0), (2, 1): (1, 0)})
        self.assertEqual(tarec.occupancy(a, b), [])

    def test_contest_a_holder_without_its_mark_fails(self):
        units = {1: self.unit(1, 1, 2, 1), 2: self.unit(2, 1, state=1 << 27 | 1)}
        a, b = self.worlds(units, {(1, 1): (1, 0), (2, 1): (1, 0)})
        out = tarec.occupancy(a, b)
        self.assertEqual(len(out), 1, out)
        self.assertIn("without the holder's mark", out[0])

    def test_contest_a_loser_s_empty_slot_under_another_layer_fails(self):
        # ground unit 2 lost nothing in slot A; an aircraft overhead fills slot B
        units = {1: self.unit(2, 1, state=1 << 26 | 2), 2: self.unit(2, 1, state=1 << 27 | 1)}
        a, b = self.worlds(units, {(2, 1): (0, 1)})
        out = tarec.occupancy(a, b)
        self.assertEqual(len(out), 1, out)
        self.assertIn("does not name it in slot A", out[0])

    def test_contest_a_structure_holds_slot_a_whatever_its_layer(self):
        structure = tarec.STRUCTURE | 1 << 26 | 2
        units = {1: self.unit(1, 1, 2, 1, state=structure), 2: self.unit(2, 1, state=1 << 27 | 1)}
        a, b = self.worlds(units, {(1, 1): (1, 0), (2, 1): (1, 0)})
        self.assertEqual(tarec.occupancy(a, b), [])
        # a layer-2 unit never contests slot A
        units[2] = self.unit(2, 1, state=1 << 27 | 2)
        a, b = self.worlds(units, {(1, 1): (1, 0), (2, 1): (1, 0)})
        self.assertEqual(len(tarec.occupancy(a, b)), 1)

    def test_an_open_yard_leaves_its_loser_over_an_empty_cell(self):
        # MEASURED 2026-11-17 (tarec.occupancy, AN OPEN YARD): the lab opened its yard and let go
        # the cell its closed yard had taken from the walker, which keeps bit 27
        lab = tarec.STRUCTURE | 1 << 26 | 1 << 27 | 1
        units = {1: self.unit(1, 1, 2, 1, state=lab), 2: self.unit(2, 1, state=1 << 27 | 1)}
        units[1][tarec.U_YARD] = 4
        a, b = self.worlds(units, {(1, 1): (1, 0)})
        self.assertEqual(tarec.occupancy(a, b), [])
        # and the lab's product made on the freed cell takes it unmarked
        units[3] = self.unit(2, 1)
        a, b = self.worlds(units, {(1, 1): (1, 0), (2, 1): (3, 0)})
        self.assertEqual(tarec.occupancy(a, b), [])
        units[1][tarec.U_YARD] = 0
        a, b = self.worlds(units, {(1, 1): (1, 0), (2, 1): (3, 0)})
        self.assertIn("without the holder's mark", tarec.occupancy(a, b)[0])

    def test_an_empty_cell_under_a_loser_fails_where_no_yard_is_open(self):
        lab = tarec.STRUCTURE | 1 << 26 | 1
        for yard, walker in ((0, 1 << 27 | 1), (4, 1)):
            units = {1: self.unit(1, 1, 2, 1, state=lab), 2: self.unit(2, 1, state=walker)}
            units[1][tarec.U_YARD] = yard
            a, b = self.worlds(units, {(1, 1): (1, 0)})
            out = tarec.occupancy(a, b)
            self.assertEqual(len(out), 1, (yard, out))
            self.assertIn("does not name it in slot A", out[0])
        # outside the open yard's footprint, and in slot B, the loser's cell must still be named
        units = {1: self.unit(1, 1, state=lab), 2: self.unit(3, 1, state=1 << 27 | 1),
                 3: self.unit(1, 1, state=1 << 27 | 2)}
        units[1][tarec.U_YARD] = 4
        a, b = self.worlds(units, {(1, 1): (1, 0)})
        self.assertEqual(len(tarec.occupancy(a, b)), 2, tarec.occupancy(a, b))

    def test_contest_a_structure_named_in_slot_b_fails(self):
        a, b = self.worlds({1: self.unit(1, 1, state=tarec.STRUCTURE | 1)}, {(1, 1): (0, 1)})
        out = tarec.occupancy(a, b)
        self.assertTrue(any("names structure 1 in slot B" in line for line in out), out)

    def test_contest_the_layer_s_slot(self):
        # layer 1 filed in slot B, and layer 2 in slot A
        a, b = self.worlds({1: self.unit(1, 1)}, {(1, 1): (0, 1)})
        out = tarec.occupancy(a, b)
        self.assertTrue(any("does not name it in slot A" in line for line in out), out)
        self.assertTrue(any("in slot B, a unit of layer 1" in line for line in out), out)
        a, b = self.worlds({1: self.unit(1, 1, state=2)}, {(1, 1): (1, 0)})
        out = tarec.occupancy(a, b)
        self.assertTrue(any("does not name it in slot B" in line for line in out), out)
        a, b = self.worlds({1: self.unit(1, 1, state=2)}, {(1, 1): (0, 1)})
        self.assertEqual(tarec.occupancy(a, b), [])

    def test_contest_a_unit_not_marked_the_loser_fails(self):
        units = {1: self.unit(1, 1, 2, 1), 2: self.unit(2, 1)}
        a, b = self.worlds(units, {(1, 1): (1, 0), (2, 1): (1, 0)})
        out = tarec.occupancy(a, b)
        self.assertEqual(len(out), 1, out)
        self.assertIn("mobile unit 2's footprint does not name it in slot A", out[0])

    def test_contest_a_cell_naming_a_unit_elsewhere_fails(self):
        units = {1: self.unit(1, 1), 2: self.unit(3, 1, state=1 << 27 | 1)}
        a, b = self.worlds(units, {(1, 1): (1, 0), (3, 1): (1, 0)})
        out = tarec.occupancy(a, b)
        self.assertTrue(any("names slot 1, outside its footprint" in line for line in out), out)
        self.assertTrue(any("mobile unit 2's footprint" in line for line in out), out)

    def test_contest_the_marks_are_the_dump_s(self):
        a, b = self.worlds({1: self.unit(1, 1)}, {(1, 1): (1, 0)})
        struct.pack_into("<I", b.units[1], tarec.U_STATE, 1 << 26 | 1)
        struct.pack_into("<I", a.units[1], tarec.U_STATE, 1 << 27 | 1)
        self.assertEqual(tarec.occupancy(a, b), [])
        self.assertEqual(a.units[1], b.units[1])


class SortChains(unittest.TestCase):
    """tarec.sort_chains: a multiplayer dump's sort buckets checked by the engine's
    invariants instead of compared with the owners' images, whose +0x8E need not form chains."""

    @staticmethod
    def unit(bucket, nxt=0, carrier=0):
        u = bytearray(tarec.REC_UNIT_IMAGE)
        struct.pack_into("<III", u, 0x82, bucket, carrier, 0)
        struct.pack_into("<I", u, 0x8E, nxt)
        return u

    def worlds(self, expected, dump):
        """(expected, dump) worlds of {slot: image}"""
        a = tarec.World(8, [], dims=(6, 4, 1, 1))
        b = tarec.World(8, [], dims=(6, 4, 1, 1))
        a.units = {slot: bytearray(u) for slot, u in expected.items()}
        b.units = {slot: bytearray(u) for slot, u in dump.items()}
        return a, b

    def test_the_owners_two_tails_against_a_chain_pass(self):
        # two owners' images each end bucket 7 at their own unit; the host chains 1 -> 3
        a, b = self.worlds({1: self.unit(7), 3: self.unit(7)},
                           {1: self.unit(7, 4), 3: self.unit(7)})
        self.assertEqual(tarec.sort_chains(a, b), [])
        self.assertEqual(a.units, b.units)

    def test_two_heads_fail(self):
        a, b = self.worlds({}, {1: self.unit(7), 3: self.unit(7)})
        out = tarec.sort_chains(a, b)
        self.assertEqual(len(out), 1, out)
        self.assertIn("both head bucket 7", out[0])

    def test_a_next_in_another_bucket_fails(self):
        a, b = self.worlds({}, {1: self.unit(7, 4), 3: self.unit(8)})
        out = tarec.sort_chains(a, b)
        self.assertTrue(any("is no unit of its bucket 7" in line for line in out), out)

    def test_a_unit_named_twice_fails(self):
        a, b = self.worlds({}, {1: self.unit(7, 4), 2: self.unit(7, 4), 3: self.unit(7)})
        out = tarec.sort_chains(a, b)
        self.assertTrue(any("both name unit 3 next" in line for line in out), out)

    def test_a_headless_cycle_fails(self):
        # 2 -> 3 -> 2 beside the head 1: each unit named once and one head, but its walk
        # reaches 1 of the 3
        a, b = self.worlds({}, {1: self.unit(7), 2: self.unit(7, 4), 3: self.unit(7, 3)})
        out = tarec.sort_chains(a, b)
        self.assertEqual(len(out), 1, out)
        self.assertIn("reach 1 of the 3", out[0])

    def test_a_carried_unit_s_next_is_compared(self):
        # +0x8E of a carried unit is its carrier's chain: it is left for the comparison
        a, b = self.worlds({2: self.unit(7, 6, carrier=5)}, {2: self.unit(7, 7, carrier=5)})
        self.assertEqual(tarec.sort_chains(a, b), [])
        self.assertNotEqual(a.units[2], b.units[2])

class SourceTest(unittest.TestCase):
    """FS_SOURCE inside the tick's feature call: a recording that keeps the engine's slots on its
    playing records it -- a multiplayer one, or one that holds its cells (RECL_CELLS)."""

    def world(self, flags):
        h = tarec.header(held_header(flags=flags)[32:])
        w = tarec.World(4, [], h)
        w.apply(tarec.RK_TICK, struct.pack("<I", 1))
        w.apply(tarec.RK_EV_FEATURE, bytes([tarec.FR_TICK, 0, 0, 0]) + bytes(4))
        return w

    def test_held_cells_take_it(self):
        w = self.world(tarec.RECL_CELLS)
        w.apply(tarec.RK_EV_DRAW, bytes([tarec.FS_SOURCE, 0, 0, 0]) + struct.pack("<II", 3, 0))

    def test_without_them_it_is_malformed(self):
        w = self.world(0)
        with self.assertRaisesRegex(tarec.Unreadable, "source test outside"):
            w.apply(tarec.RK_EV_DRAW, bytes([tarec.FS_SOURCE, 0, 0, 0]) + struct.pack("<II", 3, 0))


def spray(uid, x, y, z):
    """An RK_EV_SPRAY of unit ID `uid` at the point (x, y, z), its box and layer fixed."""
    return tlv(tarec.RK_EV_SPRAY, struct.pack("<IIiii", uid, 0, x, y, z) +
               struct.pack("<6i", *range(6)) + struct.pack("<I", 6))


def session(uid, mode):
    return tlv(tarec.RK_SPRAY_SESSION, struct.pack("<IBxxx", uid, mode))


def unit_at(slot, x, y, z):
    """An RK_UNIT of `slot` at the position (x, y, z), +0x6A"""
    img = bytearray(tarec.REC_UNIT_IMAGE)
    struct.pack_into("<iii", img, 0x6A, x, y, z)
    return tlv(tarec.RK_UNIT, struct.pack("<HH", slot, 0) + img)


class LossySprays(unittest.TestCase):
    """tarec's spray sessions (Sprays) against a file whose stream writes its nano sprays as
    sessions (RT_HOLD's RECL_SPRAYS) and whose exact stream (RB_EXACT) carries every spray: unit 1
    (ID 2) sprays the same point at GameTime 3 to 6, and its session, opened at 4, implies 5 and
    6 and closes at 7, where it makes none."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.n = 0

    def tearDown(self):
        self.tmp.cleanup()

    def file(self, exact, lossy, flags=tarec.RECL_SPRAYS):
        """Iterations 1 and 2 refresh blocks of their own, 3..31 one block whose last is a
        refresh; `exact` and `lossy` {GameTime: records} after each RK_TICK of 3..31."""
        S, F = tarec.RBF_STORED, tarec.RBF_REFRESH
        first = (unit_at(1, 0, 0, 0) +
                 tlv(tarec.RK_MAPPED, struct.pack("<I", 0) + bytes(2)))
        one = LossyStream.ticks(1, 1, {1: first})
        two = LossyStream.ticks(2, 2, {})
        parts = [held_header(flags=flags),
                 flagged(tarec.RB_STREAM, 1, 1, one, S | F), flagged(tarec.RB_EXACT, 1, 1, one, S),
                 flagged(tarec.RB_STREAM, 2, 2, two, S | F), flagged(tarec.RB_EXACT, 2, 2, two, S),
                 flagged(tarec.RB_STREAM, 3, 31, LossyStream.ticks(3, 31, lossy), S | F),
                 flagged(tarec.RB_EXACT, 3, 31, LossyStream.ticks(3, 31, exact), S),
                 end_block(31)]
        self.n += 1
        path = Path(self.tmp.name) / f"s{self.n}.imprec"
        path.write_bytes(tarec.REC_MAGIC + b"".join(parts))
        return str(path)

    A = spray(2, 100, 200, 300)
    EXACT = {3: A, 4: A, 5: A, 6: A}
    LOSSY = {3: A, 4: A + session(2, 1), 7: session(2, 0)}

    def lossy_check(self, path):
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            rc = tarec.main(["tarec", "lossy-check", path])
        return rc, out.getvalue()

    def test_implied_sprays_pass(self):
        rc, o = self.lossy_check(self.file(self.EXACT, self.LOSSY))
        self.assertEqual(rc, 0, o)
        self.assertIn("sprays 2 written and 2 implied", o)

    def test_the_world_carries_the_implied_sprays(self):
        h, bl, _ = tarec.load(self.file(self.EXACT, self.LOSSY))
        got = []
        tarec.stream_to(h, bl, on_record=lambda w, k, b: got.append(w.tick)
                        if k == tarec.RK_EV_SPRAY else None)
        self.assertEqual(got, [3, 4, 5, 6])

    def test_following_its_unit(self):
        exact = {3: spray(2, 100, 200, 300),
                 4: unit_at(1, 10, 0, -20) + spray(2, 110, 200, 280),
                 5: unit_at(1, 15, 1, -20) + spray(2, 115, 201, 280)}
        lossy = {3: exact[3], 4: exact[4] + session(2, 1 | tarec.REC_SPRAY_FOLLOW),
                 5: unit_at(1, 15, 1, -20), 6: session(2, 0)}
        rc, o = self.lossy_check(self.file(exact, lossy))
        self.assertEqual(rc, 0, o)
        self.assertIn("sprays 2 written and 1 implied", o)

    def test_a_spray_the_game_did_not_make_fails(self):
        exact = dict(self.EXACT)
        del exact[6]
        rc, o = self.lossy_check(self.file(exact, self.LOSSY))
        self.assertEqual(rc, 1, o)
        self.assertIn("GameTime 6: the nano sprays, 1 with the implied ones, are not the exact "
                      "stream's 0", o)

    def test_a_session_from_no_spray_fails(self):
        lossy = dict(self.LOSSY)
        lossy[3] = self.A + session(3, 1)
        rc, o = self.lossy_check(self.file(self.EXACT, lossy))
        self.assertEqual(rc, 1, o)
        self.assertIn("GameTime 3: the spray session of unit 3 at GameTime 3 predicts from no "
                      "single spray", o)

    def test_a_session_without_recl_sprays_fails(self):
        rc, o = self.lossy_check(self.file(self.EXACT, self.LOSSY, flags=0))
        self.assertEqual(rc, 1, o)
        self.assertIn("a spray session in a stream without RECL_SPRAYS", o)
        h, bl, _ = tarec.load(self.file(self.EXACT, self.LOSSY, flags=0))
        with self.assertRaisesRegex(tarec.Unreadable, "a spray session outside a stream"):
            tarec.stream_to(h, bl)

    AFTER = {**LOSSY, 4: A + session(2, 1) + spray(3, 1, 2, 3)}       # a record after the sessions
    UNORDERED = {**LOSSY, 4: A + session(3, 0) + session(2, 1)}       # sessions out of slot order

    def test_a_record_after_the_sessions_is_unreadable(self):
        h, bl, _ = tarec.load(self.file(self.EXACT, self.AFTER))
        with self.assertRaisesRegex(tarec.Unreadable, "record kind 28 after the iteration's spray "
                                                      "sessions"):
            tarec.stream_to(h, bl)

    def test_sessions_out_of_slot_order_are_unreadable(self):
        h, bl, _ = tarec.load(self.file(self.EXACT, self.UNORDERED))
        with self.assertRaisesRegex(tarec.Unreadable, "the spray session of unit 2 after unit 3's"):
            tarec.stream_to(h, bl)

    def test_a_malformed_session_is_unreadable(self):
        for mode in (3, tarec.REC_SPRAY_FOLLOW, 0x11):
            with self.subTest(mode=mode):
                lossy = dict(self.LOSSY)
                lossy[4] = self.A + session(2, mode)
                h, bl, _ = tarec.load(self.file(self.EXACT, lossy))
                with self.assertRaisesRegex(tarec.Unreadable, "a malformed spray session"):
                    tarec.stream_to(h, bl)


class SeekStart(unittest.TestCase):
    """tarec.seek_start: where the DLL's reader builds a seek from (recr_seek), from the file's
    index alone."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def file(self, *parts):
        p = Path(self.tmp.name) / "r.imprec"
        p.write_bytes(tarec.REC_MAGIC + b"".join(parts))
        return str(p)

    def test_the_last_snapshot_more_than_the_lead_before(self):
        path = self.file(header(), stream(1, 10, unit=1), block(tarec.RB_SNAPSHOT, 10, 10, b"s"),
                         stream(11, 20, opens=False), block(tarec.RB_SNAPSHOT, 20, 20, b"s"),
                         stream(21, 30, opens=False), end_block(30))
        self.assertEqual(tarec.seek_start(path, 0, 25, 3), (20, True))
        self.assertEqual(tarec.seek_start(path, 0, 23, 3), (10, True))     # 20 + 3 is not < 23
        self.assertEqual(tarec.seek_start(path, 0, 13, 3), (1, False))     # 10 + 3 is not < 13
        with self.assertRaises(tarec.Unreadable):
            tarec.seek_start(path, 0, 31, 3)

    def test_a_chain_takes_only_each_segments_part(self):
        fork = (header((1, 0, 15)), stream(15, 30, unit=2), block(tarec.RB_SNAPSHOT, 25, 25, b"s"),
                stream(26, 30, opens=False), end_block(30))
        path = self.file(header(), stream(1, 10, unit=1), block(tarec.RB_SNAPSHOT, 10, 10, b"s"),
                         stream(11, 20, opens=False), block(tarec.RB_SNAPSHOT, 20, 20, b"s"),
                         stream(21, 30, opens=False), end_block(30), *fork)
        # the parent's snapshot at 20 lies past the fork: timeline 1 never reads it
        self.assertEqual(tarec.seek_start(path, 1, 24, 3), (10, True))
        self.assertEqual(tarec.seek_start(path, 1, 29, 3), (25, True))
        self.assertEqual(tarec.seek_start(path, 0, 24, 3), (20, True))
        # no snapshot before: the first segment of the chain whose part reaches T - lead
        self.assertEqual(tarec.seek_start(path, 1, 13, 3), (1, False))
        path = self.file(header(), stream(1, 20, unit=1), end_block(20), *fork)
        self.assertEqual(tarec.seek_start(path, 1, 18, 2), (15, False))
        self.assertEqual(tarec.seek_start(path, 1, 16, 2), (1, False))


class SeekBlocks(unittest.TestCase):
    """tarec.seek_blocks: the blocks the DLL's reader decodes for a seek (recr_seek), from the
    file's blocks alone: a snapshot or a first iteration, every stream block holding an iteration
    read, a chain's crossings, and a base read first."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def file(self, *parts):
        p = Path(self.tmp.name) / "r.imprec"
        p.write_bytes(tarec.REC_MAGIC + b"".join(parts))
        return str(p)

    @staticmethod
    def of(*bs, base=0):
        n = sum(len(b) - 32 for b in bs)                # stored: raw and packed alike
        return len(bs), n, n, base

    def test_from_a_snapshot_or_the_first_iteration(self):
        s1, s2, s3 = stream(1, 10, unit=1), stream(11, 20, opens=False), stream(21, 30, opens=False)
        k10, k20 = block(tarec.RB_SNAPSHOT, 10, 10, b"s"), block(tarec.RB_SNAPSHOT, 20, 20, b"ss")
        path = self.file(header(), s1, k10, s2, k20, s3, end_block(30))
        self.assertEqual(tarec.seek_blocks(path, 0, 25, 3, 0), self.of(k20, s3))
        self.assertEqual(tarec.seek_blocks(path, 0, 25, 3, 1), self.of(s1, k20, s3, base=1))
        # a fresh playing holds the root's base
        self.assertEqual(tarec.seek_blocks(path, 0, 25, 3, None), self.of(k20, s3))
        self.assertEqual(tarec.seek_blocks(path, 0, 13, 3, 0), self.of(s1, s2))
        self.assertEqual(tarec.seek_blocks(path, 0, 10, 3, 0), self.of(s1))
        self.assertEqual(tarec.seek_blocks(path, 0, 20, 3, 0), self.of(k10, s2))

    def test_a_chain_reads_each_segments_part(self):
        s1, s2, s3 = stream(1, 10, unit=1), stream(11, 20, opens=False), stream(21, 30, opens=False)
        k10 = block(tarec.RB_SNAPSHOT, 10, 10, b"s")
        f1, k25, f2 = (stream(15, 24, unit=2), block(tarec.RB_SNAPSHOT, 24, 24, b"f"),
                       stream(25, 30, opens=False))
        path = self.file(header(), s1, k10, s2, block(tarec.RB_SNAPSHOT, 20, 20, b"s"), s3,
                         end_block(30), header((1, 0, 15)), f1, k25, f2, end_block(30))
        # from the parent's snapshot at 10 across the fork at 15; its 21..30 block is never read
        self.assertEqual(tarec.seek_blocks(path, 1, 22, 3, 0), self.of(k10, s2, f1))
        self.assertEqual(tarec.seek_blocks(path, 1, 29, 3, 0), self.of(k25, f2))
        self.assertEqual(tarec.seek_blocks(path, 1, 29, 3, 1), self.of(f1, k25, f2, base=1))
        # a fresh playing of timeline 1 holds the root's base, not the fork's
        self.assertEqual(tarec.seek_blocks(path, 1, 29, 3, None), self.of(f1, k25, f2, base=1))
        self.assertEqual(tarec.seek_blocks(path, 1, 22, 3, None), self.of(k10, s2, f1))
        with self.assertRaises(tarec.Unreadable):
            tarec.seek_blocks(path, 1, 31, 3, 0)

    @staticmethod
    def snap(last, chained):
        """a packed snapshot, chained to the one before it or a key"""
        return flagged(tarec.RB_SNAPSHOT, last, last, b"s" * last,
                       tarec.RBF_CHAINED if chained else 0)

    def test_a_chained_snapshot_decodes_its_chain_from_its_key(self):
        s1, s2, s3, s4 = (stream(1, 10, unit=1), stream(11, 20, opens=False),
                          stream(21, 30, opens=False), stream(31, 40, opens=False))
        k10, c20, c30 = self.snap(10, False), self.snap(20, True), self.snap(30, True)
        path = self.file(header(), s1, k10, s2, c20, s3, c30, s4, end_block(40))
        # every link from the key is a block decoded, the key's included
        self.assertEqual(tarec.seek_blocks(path, 0, 35, 3, 0), self.of(k10, c20, c30, s4))
        self.assertEqual(tarec.seek_blocks(path, 0, 35, 3, 1),
                         self.of(s1, k10, c20, c30, s4, base=1))
        self.assertEqual(tarec.seek_blocks(path, 0, 25, 3, 0), self.of(k10, c20, s3))
        self.assertEqual(tarec.seek_blocks(path, 0, 15, 3, 0), self.of(k10, s2))
        # a chain with no key before it in its segment, as the reader refuses it
        path = self.file(header(), s1, self.snap(10, True), s2, end_block(20))
        with self.assertRaisesRegex(tarec.Unreadable, "no snapshot before it"):
            tarec.seek_blocks(path, 0, 15, 3, 0)

    def test_a_chain_longer_than_its_bound_is_refused(self):
        n = tarec._codec().CHAIN_MAX
        parts, links = [header(), stream(1, 10, unit=1)], []
        for i in range(n + 2):
            links.append(self.snap(10 * (i + 1), i > 0))
            parts += [links[-1], stream(10 * (i + 1) + 1, 10 * (i + 2), opens=False)]
        last = 10 * (n + 3)
        path = self.file(*parts, end_block(last))
        # the key and REC_CHAIN_MAX links read; one more is past the bound
        T = 10 * (n + 1) + 5
        self.assertEqual(tarec.seek_blocks(path, 0, T, 3, 0)[0], n + 2)
        with self.assertRaisesRegex(tarec.Unreadable, "chains past"):
            tarec.seek_blocks(path, 0, last, 3, 0)

    @staticmethod
    def chunk(first, last, reset):
        """a sectioned stream block: a chunk of its run, the first when `reset`"""
        flags = tarec.RBF_SECTIONS | (tarec.RBF_FRAME_RESET if reset else 0)
        return flagged(tarec.RB_STREAM, first, last, b"c" * (last - first + 1), flags)

    def test_a_run_is_decoded_from_its_start(self):
        r1, r2 = self.chunk(1, 10, True), self.chunk(11, 20, False)
        r3, r4 = self.chunk(21, 30, True), self.chunk(31, 40, False)
        k20 = self.snap(20, False)
        path = self.file(header(), r1, r2, k20, r3, r4, end_block(40))
        # each chunk counts as its block; a seek starts where a run does
        self.assertEqual(tarec.seek_blocks(path, 0, 35, 3, 0), self.of(k20, r3, r4))
        self.assertEqual(tarec.seek_blocks(path, 0, 35, 3, 1), self.of(r1, k20, r3, r4, base=1))
        self.assertEqual(tarec.seek_blocks(path, 0, 15, 3, 0), self.of(r1, r2))
        # a seek from the first iteration reads every run from its start
        self.assertEqual(tarec.seek_blocks(path, 0, 35, 40, 0), self.of(r1, r2, r3, r4))
        # a chunk that continues its run past a snapshot: a read through it steps over the
        # snapshot unread with the run still valid (block_skip), a seek from the snapshot would
        # join the run in its middle
        c3 = self.chunk(21, 30, False)
        path = self.file(header(), r1, r2, k20, c3, end_block(30))
        self.assertEqual(tarec.seek_blocks(path, 0, 25, 30, 0), self.of(r1, r2, c3))
        with self.assertRaisesRegex(tarec.Unreadable, "continues a run"):
            tarec.seek_blocks(path, 0, 25, 3, 0)
        path = self.file(header(), r1, stream(11, 20, opens=False), self.chunk(21, 30, False),
                         end_block(30))
        with self.assertRaisesRegex(tarec.Unreadable, "continues a run"):
            tarec.seek_blocks(path, 0, 25, 3, 0)


class SeekWork(unittest.TestCase):
    """The seek's work line (tagpu_replay.c, sk_work_log) parsed and judged: W1 against the file,
    W2 by its counts, W3 against the file's blocks."""

    LINE = ("replay: battle: the seek to 225's work: {I} iterations read to GameTime 225 from "
            "the snapshot at GameTime {S}; the blocks: {B} decoded ({Bb} for the base), {Bp} "
            "bytes read, {Bp} unpacked; the image: 10 units live, {M} made, {F} refused, 2 "
            "killed, {A} left alive; cpu {C} ms (user 1100, kernel 0) of the game thread, 1104 ms "
            "wall")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / "r.imprec"
        snap, rest = block(tarec.RB_SNAPSHOT, 10, 10, b"s"), stream(11, 300, opens=False)
        self.read = len(snap) + len(rest) - 64          # the payloads past their 32-byte headers
        self.path.write_bytes(tarec.REC_MAGIC + header() + stream(1, 10, unit=1) + snap + rest +
                              end_block(300))

    def tearDown(self):
        self.tmp.cleanup()

    def work(self, I=215, S=10, M=10, F=0, A=0, C=1100, B=2, Bb=0, Bp=None):
        line = self.LINE.format(I=I, S=S, M=M, F=F, A=A, C=C, B=B, Bb=Bb,
                                Bp=self.read if Bp is None else Bp)
        got = tareplay.work_lines("x\n" + line + "\r\n")
        self.assertEqual(len(got), 1)
        return got[0]

    def test_a_good_line(self):
        w = self.work()
        self.assertEqual((w["T"], w["R"], w["S"], w["kind"], w["B"], w["Bp"], w["C"], w["W"]),
                         (225, 225, 10, "the snapshot", 2, self.read, 1100, 1104))
        self.assertEqual(tareplay.work_verdict(w, str(self.path), 0, True), [])

    def test_each_rule(self):
        verdict = lambda w, **kw: tareplay.work_verdict(w, str(self.path), 0, **kw)
        self.assertRegex(verdict(self.work(S=1))[0], "W1: built from the snapshot at 1")
        self.assertRegex(verdict(self.work(I=217))[0], "W1: 217 iterations read from 10 to 225")
        self.assertRegex(verdict(self.work(M=9))[0], "W2: 9 made and 0 refused of 10")
        self.assertEqual(verdict(self.work(M=9, F=1)), [])
        self.assertRegex(verdict(self.work(M=9, F=1), takeover=True)[0], "W2: 1 creates refused")
        self.assertRegex(verdict(self.work(A=3))[0], "W2: 3 units left alive")
        self.assertEqual(verdict(self.work(C=40001)), [])          # time is reported, not judged
        self.assertRegex(verdict(self.work(B=3))[0], "W3: 3 blocks decoded")
        self.assertRegex(verdict(self.work(Bp=self.read + 1))[0], "where the file holds 2, ")
        self.assertRegex(verdict(self.work(Bb=1))[0], "where the file holds 3, ")
        # a fresh process holds the root's base: the line's base is checked, not trusted
        self.assertEqual(verdict(self.work(), fresh=True), [])
        self.assertRegex(verdict(self.work(Bb=1), fresh=True)[0],
                         "W3: 1 blocks for the base where a fresh process reads 0")

    def test_a_log_with_no_line_could_not_run(self):
        log = Path(self.tmp.name) / "tagpu.log"
        log.write_text("nothing\n")
        self.assertEqual(tareplay.work_check("R", log, str(self.path), 0, False)[0][1],
                         tareplay.UNRUN)
        log.write_text(self.LINE.format(I=215, S=10, M=10, F=0, A=0, C=5, B=2, Bb=0,
                                        Bp=self.read) + "\n")
        self.assertEqual(tareplay.work_check("R", log, str(self.path), 0, False)[0][1],
                         tareplay.PASS)


class DecodeCache(unittest.TestCase):
    """reccache.py under tarec.blocks(): a hit returns what the walk returned without decoding; a
    changed file byte or decoder version misses, and an entry that is cut short, corrupt or not
    its piece is ignored and written again. A file that appends a segment to a cached one decodes
    the segment alone, and returns and refuses what the uncached walk does."""

    TORN = block(tarec.RB_STREAM, 7, 7, b"x" * 8)[:20]
    ORIGINAL = header() + stream(1, 3, unit=1) + stream(4, 6, opens=False) + end_block(6)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.cache = self.dir / "cache"
        self.codec = tarec._codec()
        env = patch.dict("os.environ", {reccache.ENV: str(self.cache)})
        env.start()
        self.addCleanup(env.stop)
        self.addCleanup(self.tmp.cleanup)

    def file(self, name, unit=1, tail=TORN):
        p = self.dir / name
        p.write_bytes(tarec.REC_MAGIC + header() + stream(1, 3, unit=unit) +
                      stream(4, 6, opens=False) + end_block(6) + tail)
        return p

    def fork(self, name, *forks, original=ORIGINAL):
        """The original and a segment for each (timeline, unit) of `forks`, forked at 4."""
        p = self.dir / name
        p.write_bytes(tarec.REC_MAGIC + original + b"".join(
            header((tl, 0, 4)) + stream(4, 5, unit=unit) + end_block(5) for tl, unit in forks))
        return p

    def read(self, path):
        """(tarec.blocks' four lists and stop, the `start` of each decoding walk the codec ran:
        None for one from the file's first block)."""
        sizes, offsets, flags = [], [], []
        decoded = []
        real = self.codec.iter_blocks

        def spy(*a, **k):
            if k.get("decode", True):
                decoded.append(k.get("start"))
            return real(*a, **k)
        with patch.object(self.codec, "iter_blocks", side_effect=spy):
            out, stop = tarec.blocks(path, sizes, offsets, flags)
        return (out, sizes, offsets, flags, stop), decoded

    def uncached(self, path):
        with patch.dict("os.environ", {reccache.ENV: ""}):
            return self.read(path)[0]

    def entries(self):
        return sorted(self.cache.glob("*.dec"))

    def test_a_hit_is_what_the_walk_returned(self):
        path = self.file("r.imprec")
        cold, decoded = self.read(path)
        self.assertEqual(decoded, [None])
        self.assertIn("cut short", cold[-1])
        self.assertEqual(len(self.entries()), 1)
        self.assertEqual(self.read(path), (cold, []))
        self.assertEqual(self.uncached(path), cold)
        self.assertEqual(list(self.cache.iterdir()), self.entries())   # no temporary file left

    def test_a_changed_byte_misses(self):
        a, b = self.file("a.imprec", unit=1), self.file("b.imprec", unit=2)
        self.assertEqual(len(a.read_bytes()), len(b.read_bytes()))
        want_a, _ = self.read(a)
        got_b, decoded = self.read(b)
        self.assertEqual(decoded, [None])
        self.assertNotEqual(got_b, want_a)
        self.assertEqual(len(self.entries()), 2)
        self.assertEqual(self.read(a), (want_a, []))

    def test_a_changed_version_misses_and_rewrites(self):
        path = self.file("r.imprec")
        want, _ = self.read(path)
        entry, = self.entries()
        with patch.object(reccache, "_version", bytes(32)):
            self.assertEqual(self.read(path), (want, [None]))
            self.assertEqual(entry.read_bytes()[8:40], bytes(32))
            self.assertEqual(self.read(path), (want, []))
        self.assertEqual(self.read(path), (want, [None]))

    def test_a_corrupt_entry_is_ignored_and_rewritten(self):
        path = self.file("r.imprec")
        want, _ = self.read(path)
        entry, = self.entries()
        good = entry.read_bytes()
        mid = 72 + (len(good) - 72) // 2
        import zstandard        # here: the module's imports run before devpy.ensure() re-executes
        content = zstandard.ZstdDecompressor().stream_reader(good[72:]).read()
        n = 1 + reccache.BLOCK.size - 4                         # the first block's payload length

        def repacked(c):        # a sound zstd frame around content its own digest refuses
            return good[:72] + zstandard.ZstdCompressor(write_checksum=True).compress(c)
        k = content.index(tlv(tarec.RK_TICK, struct.pack("<I", 1))) + 4   # the first tick's value
        for why, bad in (("empty", b""), ("cut short", good[:mid]), ("its prefix", b"x" + good[1:]),
                         ("a packed byte", good[:mid] + bytes([good[mid] ^ 0x40]) + good[mid + 1:]),
                         ("its frame's tail", good[:-4] + bytes(4)),
                         ("a payload byte", repacked(content[:k] + b"\x02" + content[k + 1:])),
                         ("its digest", repacked(content[:-1] + bytes([content[-1] ^ 1]))),
                         ("a byte after its end", repacked(content + b"B")),
                         ("a length past its end",
                          repacked(content[:n] + b"\xff\xff\xff\x7f" + content[n + 4:]))):
            with self.subTest(why=why):
                entry.write_bytes(bad)
                self.assertEqual(self.read(path), (want, [None]))
                self.assertEqual(entry.read_bytes(), good)
                self.assertEqual(self.read(path), (want, []))

    def test_an_appended_segment_decodes_alone(self):
        original = self.file("a.imprec", tail=b"")
        self.read(original)
        size = original.stat().st_size
        one = self.fork("one.imprec", (1, 2))
        got, decoded = self.read(one)
        self.assertEqual(decoded, [(size, 4, 4)])           # four blocks before it; four slots
        self.assertEqual(got, self.uncached(one))
        self.assertEqual(len(self.entries()), 2)
        two = self.fork("two.imprec", (1, 2), (2, 3))
        got, decoded = self.read(two)
        self.assertEqual(decoded, [(one.stat().st_size, 7, 4)])
        self.assertEqual(got, self.uncached(two))
        self.assertEqual(self.read(two), (got, []))

    def test_a_piece_is_its_prefix_bytes(self):
        self.read(self.fork("one.imprec", (1, 2)))
        other = self.fork("other.imprec", (1, 3))           # another fork after the same original
        got, decoded = self.read(other)
        self.assertEqual(decoded, [(len(tarec.REC_MAGIC + self.ORIGINAL), 4, 4)])
        self.assertEqual(got, self.uncached(other))
        changed = self.fork("changed.imprec", (1, 2), original=header() + stream(1, 3, unit=3) +
                            stream(4, 6, opens=False) + end_block(6))
        got, decoded = self.read(changed)                   # the same fork after another original
        self.assertEqual(decoded, [None])
        self.assertEqual(got, self.uncached(changed))

    def test_an_entry_that_is_not_its_piece_is_ignored(self):
        path = self.fork("one.imprec", (1, 2))
        want, _ = self.read(path)
        bl, _ = reccache.collect(self.codec.iter_blocks(str(path)))
        for entry in self.entries():                        # each entry holding the other piece
            for blocks in (bl[:4], bl[4:], bl[:3]):
                with self.subTest(entry=entry.name, blocks=len(blocks)):
                    good = entry.read_bytes()
                    reccache._write(str(self.cache), str(entry), good[:72], blocks)
                    if entry.read_bytes() == good:
                        continue
                    got, decoded = self.read(path)
                    self.assertEqual(got, want)
                    self.assertTrue(decoded)
                    self.assertEqual(entry.read_bytes(), good)

    def test_a_refused_segment_is_refused_after_a_cached_one(self):
        self.read(self.file("a.imprec", tail=b""))
        path = self.dir / "slots.imprec"
        path.write_bytes(tarec.REC_MAGIC + self.ORIGINAL + header((1, 0, 4), slots=6) +
                         stream(4, 5) + end_block(5))
        for cache in (str(self.cache), ""):
            with patch.dict("os.environ", {reccache.ENV: cache}), \
                    self.assertRaisesRegex(tarec.Unreadable, "a segment of 6 slots after 4"):
                tarec.blocks(path)


def pov(player=0, cam=(100, 200), zoom=65536, flags=tarec.REC_POV_WORLD, every=3):
    """A RecPov of a 640 x 480 view with its cursor at the camera."""
    return tarec.REC_POV.pack(player, every, flags, 0, 640, 480, zoom, *cam, cam[0], 0, cam[1])


def selection(player, units):
    return tarec.REC_SEL.pack(player, tarec.REC_SEL_BEGIN | tarec.REC_SEL_END, len(units), 0,
                              len(units)) + b"".join(tarec.REC_SEL_ENTRY.pack(*u) for u in units)


def block_player(first_id=2, last_id=3):
    """Player 0's image with its unit block, IDs first_id..last_id (slots one less)."""
    img = bytearray(tarec.REC_PLAYER_IMAGE)
    struct.pack_into("<II", img, tarec.PL_BLOCKFIRST, first_id, 0)
    struct.pack_into("<I", img, tarec.PL_BLOCKLAST, last_id)
    return tlv(tarec.RK_PLAYER, struct.pack("<HH", 0, 0) + bytes(img))


class RecmpFilePerspective(unittest.TestCase):
    """The multiplayer reader's camera sample takes the flags tarec does, and no others."""

    def put(self, flags):
        import recmp_file
        history = object.__new__(recmp_file.History)
        stream = b"s" * 8
        history.source_maps = {1: {"stream": stream}}
        history.maps = {1: {}}
        history.source_pov = {}
        history.pov = {}
        history.zoom_min = 1
        body = pov(player=recmp_file.RECMP_POV_PLAYER, flags=flags)
        history._perspective(1, 3, 1, 13, stream, body, 3, False, 0)
        return history.pov[1][-1]["pov"]

    def test_a_cut_sample_reads(self):
        flags = tarec.REC_POV_WORLD | tarec.REC_POV_CUT
        self.assertEqual(self.put(flags)[2], flags)

    def test_an_unknown_flag_is_refused(self):
        with self.assertRaisesRegex(ValueError, "fields/provenance"):
            self.put(8)


class Perspective(unittest.TestCase):
    """tarec's perspective records and `pov --check`, on files built from the format alone."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def head(self, fmt=None):
        h = bytearray(header())
        # the RK_HEADER block's payload begins after its 32-byte head; RT_FORMAT is its first entry
        p = bytes(h[32:])
        p = p.replace(tlv(tarec.RT_FORMAT, struct.pack("<I", tarec.REC_FORMAT)),
                      tlv(tarec.RT_FORMAT, struct.pack("<I", fmt or tarec.REC_FORMAT)), 1)
        p += tlv(tarec.RT_UNITS_EACH, struct.pack("<I", 2))
        return block(tarec.RB_HEADER, 0, 0, p)

    def run_pov(self, *parts):
        path = Path(self.tmp.name) / "p.imprec"
        path.write_bytes(tarec.REC_MAGIC + b"".join(parts))
        out = io.StringIO()
        with redirect_stdout(out):
            code = tarec.cmd_pov(str(path), True)
        return code, out.getvalue()

    def good(self, second=None, snap_sel=((1, 0x11),), snap_pov=None):
        first = pov()
        second = second or pov(cam=(130, 200))
        s1 = (tlv(tarec.RK_TICK, struct.pack("<I", 1)) + block_player() +
              tlv(tarec.RK_MAPPED, struct.pack("<I", 0) + bytes(2)) +
              tlv(tarec.RK_POV, first) + tlv(tarec.RK_SELECT, selection(0, [(1, 0x11)])) +
              tlv(tarec.RK_CLICK, tarec.REC_CLICK.pack(0, 0, 1, 2, 2, 0, 4, 100, 0, 200)) +
              tlv(tarec.RK_TICK, struct.pack("<I", 2)) +
              tlv(tarec.RK_TICK, struct.pack("<I", 4)) + tlv(tarec.RK_POV, second))
        snap_pov = snap_pov or [(1, first), (4, second)]
        snap = (tlv(tarec.RK_TICK, struct.pack("<I", 4)) + block_player() +
                b"".join(tlv(tarec.RK_POV_AT, struct.pack("<I", t) + b) for t, b in snap_pov) +
                tlv(tarec.RK_SELECT, selection(0, list(snap_sel))) +
                tlv(tarec.RK_MAPPED, struct.pack("<I", 0) + bytes(2)))
        s2 = tlv(tarec.RK_TICK, struct.pack("<I", 5))
        return (self.head(), block(tarec.RB_STREAM, 1, 4, s1), block(tarec.RB_SNAPSHOT, 4, 4, snap),
                block(tarec.RB_STREAM, 5, 5, s2), end_block(5))

    def test_well_formed(self):
        code, out = self.run_pov(*self.good())
        self.assertEqual(code, 0, out)
        self.assertIn("2 samples, 1 clicks, 1 selections; 1 snapshots", out)
        self.assertIn("left click at (100, 0, 200)", out)

    def test_spacing_and_change(self):
        parts = list(self.good(second=pov()))
        code, out = self.run_pov(*parts)
        self.assertEqual(code, 1)
        self.assertIn("equal to the last one", out)
        code, out = self.run_pov(*self.good(second=pov(cam=(130, 200), every=4)))
        self.assertEqual(code, 1)
        self.assertIn("under its every 4", out)

    def test_snapshot_must_rebuild_the_stream(self):
        code, out = self.run_pov(*self.good(snap_sel=()))
        self.assertEqual(code, 1)
        self.assertIn("player 0's selection: 1 units in the recording, 0 in the snapshot", out)
        code, out = self.run_pov(*self.good(snap_pov=[(4, pov(cam=(130, 200)))]))
        self.assertEqual(code, 1)
        self.assertIn("player 0's samples: recording at GameTime [1, 4], snapshot at [4]", out)

    def test_clock_and_fit(self):
        """The mirror of tagpu_persp.c on the plan's rules (persp_test.py checks it against the C
        on random cases)."""
        held = [(1, pov()), (4, pov(cam=(130, 170)))]
        self.assertEqual(tarec.persp_sample([], 9), (tarec.PERSP_NONE, None))
        how, f = tarec.persp_sample(held, 7)                 # t = 4: the latest
        self.assertEqual((how, f[7], f[8]), (tarec.PERSP_LATEST, 130, 170))
        how, f = tarec.persp_sample(held, 5)                 # t = 2: a third of the way
        self.assertEqual((how, f[7], f[8]), (tarec.PERSP_BETWEEN, 110, 190))
        how, f = tarec.persp_sample([(1, pov()), (4, pov(cam=(130, 160)))], 5)
        self.assertEqual(f[8], 187)                          # -40/3 truncated toward zero
        how, f = tarec.persp_sample(held, 3)                 # t = 0: before the earlier one
        self.assertEqual((how, f[7]), (tarec.PERSP_EARLIER, 100))
        how, f = tarec.persp_sample([(1, pov()), (8, pov(cam=(130, 170)))], 9)
        self.assertEqual((how, f[7]), (tarec.PERSP_EARLIER, 100))   # 7 apart > 2 * every
        cut = [(1, pov()), (4, pov(cam=(130, 170), flags=tarec.REC_POV_WORLD | tarec.REC_POV_CUT))]
        how, f = tarec.persp_sample(cut, 5)                  # t = 2: a cut is not blended into
        self.assertEqual((how, f[7]), (tarec.PERSP_EARLIER, 100))
        how, f = tarec.persp_sample(cut, 7)                  # t = 4: and is there at its tick
        self.assertEqual((how, f[7]), (tarec.PERSP_LATEST, 130))
        f = tarec.REC_POV.unpack(pov(cam=(1000, 800)))
        zmin = tarec.REC_POV_ZOOM_MIN
        self.assertEqual(tarec.persp_follow(f, 640, 480, zmin), (680, 560, 65536))
        self.assertEqual(tarec.persp_follow(f, 1000, 600, zmin), (500, 500, 81920))
        self.assertEqual(tarec.persp_follow(f, 700, 500, zmin)[2], 68266)
        self.assertIsNone(tarec.persp_follow(f, 0, 480, zmin))
        # the bottom is the recording's own: half the view at 0.25 is 0.125 in format 27, and
        # format 26's 0.25
        f = tarec.REC_POV.unpack(pov(cam=(1000, 800), zoom=16384))
        self.assertEqual(tarec.persp_follow(f, 320, 240, tarec.pov_zoom_min(27))[2], 8192)
        self.assertEqual(tarec.persp_follow(f, 320, 240, tarec.pov_zoom_min(26))[2], 16384)

    def test_eye(self):
        path = Path(self.tmp.name) / "p.imprec"
        path.write_bytes(tarec.REC_MAGIC + b"".join(self.good()))
        out = io.StringIO()
        with redirect_stdout(out):
            code = tarec.main(["tarec.py", "pov", str(path), "--eye", "5", "--viewer", "800,600"])
        self.assertEqual(code, 0, out.getvalue())
        self.assertIn("sample between of the samples at [1, 4]: camera (110, 200)",
                      out.getvalue())
        self.assertIn("on 800x600: eye (-290, -100), level 81920/65536", out.getvalue())
        with redirect_stdout(io.StringIO()):
            self.assertEqual(tarec.main(["tarec.py", "pov", str(path), "--eye", "3", "--viewer",
                                         "800,600"]), 1)   # no iteration 3
            self.assertEqual(tarec.main(["tarec.py", "pov", str(path), "--eye", "5", "--viewer",
                                         "800,600", "--seat", "1"]), 1)   # no samples

    def test_refusals(self):
        for bad in (selection(0, [(2, 1), (1, 1)]),          # descending
                    selection(0, [(3, 1)]),                  # outside the block [1, 2]
                    selection(0, [(1, 1), (2, 1), (2, 2)]),  # past units_each
                    pov(zoom=1)):                            # a zoom no level has
            kind = tarec.RK_POV if len(bad) == tarec.REC_POV.size else tarec.RK_SELECT
            parts = list(self.good())
            parts[3] = block(tarec.RB_STREAM, 5, 5, tlv(tarec.RK_TICK, struct.pack("<I", 5)) +
                             tlv(kind, bad))
            code, out = self.run_pov(*parts)
            self.assertEqual(code, 1, out)
            self.assertIn("unreadable", out)
        parts = list(self.good())
        parts[0] = self.head(fmt=7)
        with self.assertRaisesRegex(tarec.Unreadable,
                                    f"format 7, not one of {', '.join(map(str, tarec.REC_FORMATS))}"):
            self.run_pov(*parts)

    def test_zoom_minimum_is_the_files(self):
        """A sample's smallest zoom is its file's format's (tagpu_recpov.h, rec_pov_zoom_min):
        1/64 in format 27, 0.25 in format 26, which a reader still takes."""
        for fmt, zoom, ok in ((27, 1024, True), (27, 1023, False), (26, 16384, True),
                              (26, 16383, False)):
            parts = list(self.good(second=pov(cam=(130, 200), zoom=zoom)))
            parts[0] = self.head(fmt=fmt)
            code, out = self.run_pov(*parts)
            self.assertEqual(code == 0, ok, (fmt, zoom, out))
            if not ok:
                self.assertIn("unreadable", out)


class CellsAfterEvents(unittest.TestCase):
    """tarec's World takes an RK_CELLS as an image only before its iteration's first event, as
    the DLL's reader does (tagpu_recreader.c, apply): after it, only the tick's feature call may
    hold cells, as its input, and a cell outside that call is a malformed file."""

    @staticmethod
    def cells(x, fill):
        return struct.pack("<HHHH", x, 0, 1, 0) + bytes([fill]) * tarec.REC_CELL_BYTES

    def world(self):
        w = tarec.World(4, [], dims=(2, 2, 1, 1))
        w.apply(tarec.RK_TICK, struct.pack("<I", 1))
        return w

    def test_image_before_the_first_event(self):
        w = self.world()
        self.assertIsNone(w.apply(tarec.RK_CELLS, self.cells(1, 7)))
        self.assertEqual(w.grid[tarec.REC_CELL_BYTES:2 * tarec.REC_CELL_BYTES],
                         bytes([7]) * tarec.REC_CELL_BYTES)

    def test_tick_input_inside_the_tick_call(self):
        w = self.world()
        w.apply(tarec.RK_EV_FEATURE, bytes([tarec.FR_TICK, 0, 0, 0]) + bytes(4))
        self.assertEqual(w.apply(tarec.RK_CELLS, self.cells(0, 9))[0], tarec.RK_CELLS)
        w.apply(tarec.RK_EV_FEATURE_END, bytes([tarec.FR_TICK, 0, 0, 0]) + bytes(4))
        self.assertEqual(w.grid, bytearray(4 * tarec.REC_CELL_BYTES))
        with self.assertRaisesRegex(tarec.Unreadable, "after an event"):
            w.apply(tarec.RK_CELLS, self.cells(0, 9))

    def test_refused_after_an_event_until_the_next_tick(self):
        w = self.world()
        w.apply(tarec.RK_EV_STATE, struct.pack("<IB3x", 1, 2))
        with self.assertRaisesRegex(tarec.Unreadable, "after an event"):
            w.apply(tarec.RK_CELLS, self.cells(0, 5))
        w.apply(tarec.RK_TICK, struct.pack("<I", 2))
        self.assertIsNone(w.apply(tarec.RK_CELLS, self.cells(0, 5)))


class ScriptSlice(unittest.TestCase):
    """RK_EV_SLICE in tarec's World, as the DLL's reader takes it (tagpu_recreader.c, event_take):
    an event, outside every bracket, naming a slot of the level's."""

    def world(self):
        w = tarec.World(4, [], dims=(2, 2, 1, 1))
        w.apply(tarec.RK_TICK, struct.pack("<I", 1))
        return w

    def test_an_event_at_the_top_level(self):
        w = self.world()
        self.assertEqual(w.apply(tarec.RK_EV_SLICE, struct.pack("<I", 2))[0], tarec.RK_EV_SLICE)
        with self.assertRaisesRegex(tarec.Unreadable, "after an event"):
            w.apply(tarec.RK_CELLS, CellsAfterEvents.cells(0, 5))

    def test_refused_inside_a_feature_call_or_a_death(self):
        w = self.world()
        w.apply(tarec.RK_EV_FEATURE, bytes([tarec.FR_TICK, 0, 0, 0]) + bytes(4))
        with self.assertRaisesRegex(tarec.Unreadable, "inside a feature call or a death"):
            w.apply(tarec.RK_EV_SLICE, struct.pack("<I", 2))
        w = self.world()
        w.apply(tarec.RK_EV_DEATH, struct.pack("<II4x", 2, 0) + bytes(tarec.REC_UNIT_IMAGE))
        with self.assertRaisesRegex(tarec.Unreadable, "inside a feature call or a death"):
            w.apply(tarec.RK_EV_SLICE, struct.pack("<I", 3))

    def test_refused_outside_the_slots(self):
        for unit in (0, 1, 5):              # slot -1, slot 0 (NULL's), slot 4 = slots
            with self.subTest(unit=unit):
                with self.assertRaisesRegex(tarec.Unreadable, "outside the array"):
                    self.world().apply(tarec.RK_EV_SLICE, struct.pack("<I", unit))


class SliceEvents(unittest.TestCase):
    """EventCheck: a script slice names a unit live at its point, and a slot once an iteration."""

    @staticmethod
    def ev(kind, unit):
        return kind, struct.pack("<I", unit) + bytes(24 if kind == tarec.RK_EV_CREATE else 0)

    def notes(self, was, events):
        ec = tarec.EventCheck()
        ec.slices({slot: None for slot in was}, events, 10)
        return ec.lines

    def test_a_unit_in_the_images(self):
        self.assertEqual(self.notes([2], [self.ev(tarec.RK_EV_SLICE, 3)]), [])

    def test_a_slot_with_no_unit(self):
        lines = self.notes([2], [self.ev(tarec.RK_EV_SLICE, 4)])
        self.assertEqual(len(lines), 1)
        self.assertIn("slot 3, which holds no live unit", lines[0])

    def test_a_unit_created_earlier_in_the_iteration(self):
        self.assertEqual(self.notes([], [self.ev(tarec.RK_EV_CREATE, 4),
                                         self.ev(tarec.RK_EV_SLICE, 4)]), [])
        self.assertEqual(len(self.notes([], [self.ev(tarec.RK_EV_SLICE, 4),
                                              self.ev(tarec.RK_EV_CREATE, 4)])), 1)

    def test_a_slot_sliced_twice(self):
        lines = self.notes([2], [self.ev(tarec.RK_EV_SLICE, 3), self.ev(tarec.RK_EV_SLICE, 3)])
        self.assertEqual(len(lines), 1)
        self.assertIn("slot 2's script slice twice", lines[0])


class OwnerQuery(unittest.TestCase):
    """RK_EV_QUERY in tarec's World, as the DLL's reader takes it (tagpu_recreader.c, event_take):
    an event, outside every bracket, naming a slot of the level's and a query it knows, its pad
    zero; and EventCheck notes one whose unit is not live at its point."""

    @staticmethod
    def query(unit, which=1, pad=b"\0\0\0"):
        return struct.pack("<IB", unit, which) + pad

    def world(self):
        return ScriptSlice.world(self)

    def test_an_event_at_the_top_level(self):
        w = self.world()
        for which in range(tarec.REC_QUERIES):
            self.assertEqual(w.apply(tarec.RK_EV_QUERY, self.query(2, which))[0],
                             tarec.RK_EV_QUERY)
        with self.assertRaisesRegex(tarec.Unreadable, "after an event"):
            w.apply(tarec.RK_CELLS, CellsAfterEvents.cells(0, 5))

    def test_refused_inside_a_feature_call_or_a_death(self):
        w = self.world()
        w.apply(tarec.RK_EV_FEATURE, bytes([tarec.FR_TICK, 0, 0, 0]) + bytes(4))
        with self.assertRaisesRegex(tarec.Unreadable, "inside a feature call or a death"):
            w.apply(tarec.RK_EV_QUERY, self.query(2))
        w = self.world()
        w.apply(tarec.RK_EV_DEATH, struct.pack("<II4x", 2, 0) + bytes(tarec.REC_UNIT_IMAGE))
        with self.assertRaisesRegex(tarec.Unreadable, "inside a feature call or a death"):
            w.apply(tarec.RK_EV_QUERY, self.query(3))

    def test_refused_outside_the_slots_or_the_queries(self):
        for unit in (0, 1, 5):
            with self.subTest(unit=unit):
                with self.assertRaisesRegex(tarec.Unreadable, "outside the array"):
                    self.world().apply(tarec.RK_EV_QUERY, self.query(unit))
        for body in (self.query(2, tarec.REC_QUERIES), self.query(2, 0, b"\0\1\0"),
                     self.query(2)[:4]):
            with self.subTest(body=body):
                with self.assertRaisesRegex(tarec.Unreadable, "an owner's query of"):
                    self.world().apply(tarec.RK_EV_QUERY, body)

    def test_its_unit_live_at_its_point(self):
        ec = tarec.EventCheck()
        ec.slices({2: None}, [(tarec.RK_EV_QUERY, self.query(3)),
                              (tarec.RK_EV_QUERY, self.query(3)),
                              (tarec.RK_EV_QUERY, self.query(4))], 10)
        self.assertEqual(len(ec.lines), 1)
        self.assertIn("an owner's query of slot 3, which holds no live unit", ec.lines[0])


class YardEvent(unittest.TestCase):
    """RK_EV_YARD in tarec's World, as the DLL's reader takes it (tagpu_recreader.c, event_take):
    an event, in a bracket or not, naming a slot of the level's and a yard of 0 or 1, its pad
    zero, where THE YARD RULE lets it stand (tagpu_recfmt.h); and EventCheck's yards: a unit's
    +0x10F bit 2 changes between two images only with a yard event whose last one for it is the
    new yard, and a unit made in the step has its last yard event's yard, or the closed one."""

    @staticmethod
    def yard(unit, open_=1, pad=b"\0\0\0"):
        return struct.pack("<IB", unit, open_) + pad

    @staticmethod
    def death(unit):
        return struct.pack("<II4x", unit, 0) + bytes(tarec.REC_UNIT_IMAGE)

    @staticmethod
    def create(unit):
        return struct.pack("<I", unit) + bytes(24)

    def world(self, live=(1, 2)):
        """slots 1 and 2 live in the iteration's images (unit IDs 2 and 3)"""
        w = ScriptSlice.world(self)
        for slot in live:
            w.apply(tarec.RK_UNIT, struct.pack("<H2x", slot) + bytes(tarec.REC_UNIT_IMAGE))
        return w

    def end(self, w):
        w.apply(tarec.RK_TICK, struct.pack("<I", 2))

    def test_a_live_unit_in_or_out_of_a_bracket(self):
        w = self.world()
        self.assertEqual(w.apply(tarec.RK_EV_YARD, self.yard(2))[0], tarec.RK_EV_YARD)
        with self.assertRaisesRegex(tarec.Unreadable, "after an event"):
            w.apply(tarec.RK_CELLS, CellsAfterEvents.cells(0, 5))
        w = self.world()
        w.apply(tarec.RK_EV_DEATH, self.death(2))
        self.assertEqual(w.apply(tarec.RK_EV_YARD, self.yard(3, 0))[0], tarec.RK_EV_YARD)
        w.apply(tarec.RK_EV_DEATH_END, struct.pack("<I", 2))
        self.end(w)

    def test_a_yard_its_create_opens(self):
        w = self.world(live=())
        w.apply(tarec.RK_EV_YARD, self.yard(3))
        w.apply(tarec.RK_EV_YARD, self.yard(3, 0))
        w.apply(tarec.RK_EV_CREATE, self.create(3))
        w.apply(tarec.RK_EV_YARD, self.yard(3))         # live from its create on
        self.end(w)
        w.whole()

    def test_a_yard_whose_create_does_not_follow(self):
        w = self.world(live=())
        w.apply(tarec.RK_EV_YARD, self.yard(3))
        with self.assertRaisesRegex(tarec.Unreadable, "whose create does not follow"):
            w.whole()
        with self.assertRaisesRegex(tarec.Unreadable, "whose create does not follow"):
            self.end(w)

    def test_a_dead_unit_out_of_a_bracket(self):
        w = self.world()
        w.apply(tarec.RK_EV_DEATH, self.death(2))
        w.apply(tarec.RK_EV_DEATH_END, struct.pack("<I", 2))
        w.apply(tarec.RK_EV_YARD, self.yard(2))
        with self.assertRaisesRegex(tarec.Unreadable, "slot 1, not live, whose create"):
            self.end(w)

    def test_a_yard_inside_its_own_death(self):
        w = self.world()
        w.apply(tarec.RK_EV_DEATH, self.death(2))
        with self.assertRaisesRegex(tarec.Unreadable, "inside its own death"):
            w.apply(tarec.RK_EV_YARD, self.yard(2))

    def test_a_yard_inside_a_feature_call(self):
        w = self.world()
        w.apply(tarec.RK_EV_FEATURE, bytes([tarec.FR_TICK, 0, 0, 0]) + bytes(4))
        with self.assertRaisesRegex(tarec.Unreadable, "a yard inside a feature call"):
            w.apply(tarec.RK_EV_YARD, self.yard(2))

    def test_a_death_before_the_create_its_yard_waits_for(self):
        w = self.world(live=())
        w.apply(tarec.RK_EV_YARD, self.yard(3))
        with self.assertRaisesRegex(tarec.Unreadable, "whose create's yard waits"):
            w.apply(tarec.RK_EV_DEATH, self.death(3))

    def test_refused_outside_the_slots_or_the_yards(self):
        for unit in (0, 1, 5):
            with self.subTest(unit=unit):
                with self.assertRaisesRegex(tarec.Unreadable, "outside the array"):
                    self.world().apply(tarec.RK_EV_YARD, self.yard(unit))
        for body in (self.yard(2, 2), self.yard(2, 1, b"\0\1\0"), self.yard(2)[:4]):
            with self.subTest(body=body):
                with self.assertRaisesRegex(tarec.Unreadable, "a yard of"):
                    self.world().apply(tarec.RK_EV_YARD, body)

    @staticmethod
    def unit(yard):
        return (bytes(4), bytes(4), 0, 0x05 if yard else 0x01)

    def notes(self, a, b, events, yards=True):
        ec = tarec.EventCheck(yards=yards)
        ec.transition({1: self.unit(a)}, events, {1: self.unit(b)}, 10)
        return ec.lines

    def test_a_yard_explained_by_its_event(self):
        self.assertEqual(self.notes(0, 1, [(tarec.RK_EV_YARD, self.yard(2, 1))]), [])
        self.assertEqual(self.notes(1, 0, [(tarec.RK_EV_YARD, self.yard(2, 1)),
                                           (tarec.RK_EV_YARD, self.yard(2, 0))]), [])
        self.assertEqual(self.notes(1, 1, [(tarec.RK_EV_YARD, self.yard(2, 1))]), [])

    def test_a_yard_changed_with_no_event_or_another(self):
        lines = self.notes(0, 1, [])
        self.assertEqual(len(lines), 1)
        self.assertIn("unit 1's yard went 0 -> 1 with no yard event", lines[0])
        lines = self.notes(0, 1, [(tarec.RK_EV_YARD, self.yard(2, 1)),
                                  (tarec.RK_EV_YARD, self.yard(2, 0))])
        self.assertIn("its last yard event at 0", lines[0])

    def test_a_network_recording_is_not_asked(self):
        self.assertEqual(self.notes(0, 1, [], yards=False), [])

    def made(self, events, yard):
        ec = tarec.EventCheck(yards=True)
        ec.transition({}, [(tarec.RK_EV_CREATE, self.create(2))] + events, {1: self.unit(yard)},
                      10)
        return ec.lines

    def test_a_made_unit_with_its_create_s_yard(self):
        self.assertEqual(self.made([], 0), [])
        self.assertEqual(self.made([(tarec.RK_EV_YARD, self.yard(2, 1))], 1), [])

    def test_a_made_unit_whose_yard_is_not_its_events(self):
        lines = self.made([], 1)
        self.assertEqual(len(lines), 1)
        self.assertIn("unit 1 appears with its yard 1 and no yard event", lines[0])
        lines = self.made([(tarec.RK_EV_YARD, self.yard(2, 1))], 0)
        self.assertIn("unit 1 appears with its yard 0 and its last yard event at 1", lines[0])


class ContestOrderEvent(unittest.TestCase):
    """RK_EV_TURN, RK_EV_LINK and RK_EV_PLACE in tarec's World, as the writer and the DLL's
    reader take them (tagpu_recfmt.h, THE CONTEST ORDER): a key stands just before a top-level
    footprint event, never gives the key the event has without it and never takes the step's
    keys back; a link names a child slot and a parent that is none or another slot, a move a slot
    and two layers, inside no bracket."""

    world = YardEvent.world
    death, create = staticmethod(YardEvent.death), staticmethod(YardEvent.create)

    @staticmethod
    def turn(key):
        return struct.pack("<HH", key, 0)

    @staticmethod
    def link(child, parent, pad=b"\0\0"):
        return struct.pack("<IIBB", child, parent, 3, 1) + pad

    @staticmethod
    def place(unit, before=1, after=1, pad=b"\0\0"):
        return struct.pack("<I3i3iBB", unit, 100, 5, 200, 900, 5, 800, before, after) + pad

    def test_a_move_takes_the_key_of_its_turn(self):
        w = self.world()
        w.apply(tarec.RK_EV_TURN, self.turn(1))
        self.assertEqual(w.apply(tarec.RK_EV_PLACE, self.place(3))[0], tarec.RK_EV_PLACE)
        self.assertEqual(w.turn_held, 1)
        with self.assertRaisesRegex(tarec.Unreadable, "one the event has without it"):
            w.apply(tarec.RK_EV_TURN, self.turn(1))
            w.apply(tarec.RK_EV_PLACE, self.place(2))

    def test_a_move_outside_the_array_or_of_another_layer(self):
        for unit in (1, 5):
            with self.subTest(unit=unit):
                with self.assertRaisesRegex(tarec.Unreadable, "outside the array"):
                    self.world().apply(tarec.RK_EV_PLACE, self.place(unit))
        for body in (self.place(2, before=4), self.place(2, after=4), self.place(2, pad=b"\1\0"),
                     self.place(2)[:31]):
            with self.subTest(body=body.hex()):
                with self.assertRaisesRegex(tarec.Unreadable, "a teleport move of"):
                    self.world().apply(tarec.RK_EV_PLACE, body)

    def test_the_implicit_keys(self):
        w = self.world()
        w.apply(tarec.RK_EV_DEATH, self.death(3))           # key 2, its own slot
        w.apply(tarec.RK_EV_DEATH_END, struct.pack("<I", 3))
        self.assertEqual(w.turn_held, 2)
        w.apply(tarec.RK_EV_CREATE, self.create(2))         # the step's last key
        self.assertEqual(w.apply(tarec.RK_EV_TURN, self.turn(tarec.REC_TURN_AFTER))[0],
                         tarec.RK_EV_TURN)
        w.apply(tarec.RK_EV_LINK, self.link(2, 0))
        self.assertEqual(w.turn_held, tarec.REC_TURN_AFTER)
        YardEvent.end(self, w)
        self.assertEqual(w.turn_held, 0)

    def test_a_key_the_event_has_without_it(self):
        w = self.world()
        w.apply(tarec.RK_EV_TURN, self.turn(2))
        with self.assertRaisesRegex(tarec.Unreadable, "one the event has without it"):
            w.apply(tarec.RK_EV_DEATH, self.death(3))

    def test_the_keys_never_go_back(self):
        w = self.world()
        w.apply(tarec.RK_EV_DEATH, self.death(3))
        w.apply(tarec.RK_EV_DEATH_END, struct.pack("<I", 3))
        w.apply(tarec.RK_EV_TURN, self.turn(1))
        with self.assertRaisesRegex(tarec.Unreadable, "a contest key 1 after 2"):
            w.apply(tarec.RK_EV_CREATE, self.create(2))

    def test_a_key_with_no_footprint_event_after_it(self):
        w = self.world()
        w.apply(tarec.RK_EV_TURN, self.turn(3))
        with self.assertRaisesRegex(tarec.Unreadable, "no footprint event"):
            w.apply(tarec.RK_EV_SLICE, struct.pack("<I", 2))
        w = self.world()
        w.apply(tarec.RK_EV_TURN, self.turn(3))
        with self.assertRaisesRegex(tarec.Unreadable, "no footprint event"):
            YardEvent.end(self, w)
        w = self.world()
        w.apply(tarec.RK_EV_TURN, self.turn(3))
        with self.assertRaisesRegex(tarec.Unreadable, "no footprint event"):
            w.apply(tarec.RK_EV_TURN, self.turn(3))

    def test_a_key_past_the_slots(self):
        with self.assertRaisesRegex(tarec.Unreadable, "past the slots"):
            self.world().apply(tarec.RK_EV_TURN, self.turn(4))
        with self.assertRaisesRegex(tarec.Unreadable, "a contest key of"):
            self.world().apply(tarec.RK_EV_TURN, struct.pack("<HH", 1, 1))

    def test_a_key_or_link_inside_a_death_or_a_feature_call(self):
        for kind, body in ((tarec.RK_EV_TURN, self.turn(3)), (tarec.RK_EV_LINK, self.link(2, 0)),
                           (tarec.RK_EV_PLACE, self.place(2))):
            with self.subTest(kind=kind):
                w = self.world()
                w.apply(tarec.RK_EV_DEATH, self.death(3))
                with self.assertRaisesRegex(tarec.Unreadable, "inside a feature call or a death"):
                    w.apply(kind, body)
                w = self.world()
                w.apply(tarec.RK_EV_FEATURE, bytes([tarec.FR_TICK, 0, 0, 0]) + bytes(4))
                with self.assertRaisesRegex(tarec.Unreadable, "inside a feature call or a death"):
                    w.apply(kind, body)

    def test_a_link_outside_the_array(self):
        for child, parent in ((1, 0), (5, 0), (2, 2), (2, 5)):
            with self.subTest(child=child, parent=parent):
                with self.assertRaisesRegex(tarec.Unreadable, "outside the array"):
                    self.world().apply(tarec.RK_EV_LINK, self.link(child, parent))
        with self.assertRaisesRegex(tarec.Unreadable, "a carry link of"):
            self.world().apply(tarec.RK_EV_LINK, self.link(2, 3, b"\0\1"))


class PlayingEnd(unittest.TestCase):
    """A wait for a GameTime armed on a playing ends at the playing's end line (wait_playing):
    past it the level runs on with its units stopped, and the DLL answers nothing armed beyond."""

    STOP = ("replay: tenseat cannot be read on from GameTime 180: a malformed record of kind 33 "
            "at GameTime 181, in the block before byte 255260; the units stop here")
    END = "replay: tenseat played to its end at GameTime 1980: the end block"
    TRIP = "replay: tenseat: round trip rt601 at 601: TRANSIENT 0 bytes reset"

    @staticmethod
    def log(*lines):
        """a log whose running wait returns the first of `lines` from `start` that matches"""
        def wait_running(rx, start=0):
            return next(ln for ln in lines[start:] if re.search(rx, ln))
        log = Mock()
        log.wait_running.side_effect = wait_running
        return log

    def wait(self, *lines):
        return tareplay.wait_playing(self.log(*lines), r"round trip rt601 at 601(: | not run)",
                                     "round trip rt601 at 601", 0)

    def test_the_armed_line(self):
        self.assertEqual(self.wait(self.TRIP, self.STOP), self.TRIP)

    def test_a_playing_that_cannot_be_read_on_fails(self):
        with self.assertRaises(tareplay.Failed) as e:
            self.wait(self.STOP, self.TRIP)
        self.assertIn("round trip rt601 at 601: the playing stopped before it", str(e.exception))
        self.assertIn("kind 33", str(e.exception))

    def test_a_refused_seek_back_fails(self):
        with self.assertRaises(tareplay.Failed):
            self.wait("replay: tenseat cannot be read back to GameTime 181: why; the units stop here")

    def test_a_recording_that_ends_first_could_not_run(self):
        with self.assertRaises(tareplay.CouldNotRun) as e:
            self.wait(self.END)
        self.assertIn("the recording ended before it", str(e.exception))

    def test_lines_before_the_start_are_not_read(self):
        got = tareplay.wait_playing(self.log(self.STOP, self.TRIP),
                                    r"round trip rt601 at 601(: | not run)", "rt601", 1)
        self.assertEqual(got, self.TRIP)

    def test_a_playing_begins_after_its_seek(self):
        """W: an end line of the playing a seek replaced, before the seek's answer, is not the
        sought playing's"""
        seek = "replay: tenseat: at GameTime 331, sought from 200"
        dump = ("recorder: dump w331 at GameTime 331 -> replays\\dump-w331.bin (8962308 bytes of "
                "records); 0 records with a body past 0xFFFF bytes not written")
        lines = [self.STOP, seek, dump]
        start = tareplay.after_line(lines, seek, 0)
        self.assertEqual(start, 2)
        tareplay.wait_dumps(self.log(*lines), ["w331"], running=True, playing=start)
        with self.assertRaises(tareplay.Failed):
            tareplay.wait_dumps(self.log(*lines), ["w331"], running=True, playing=0)

    def test_a_dump_on_a_playing(self):
        log = self.log(self.STOP)
        with self.assertRaises(tareplay.Failed) as e:
            tareplay.wait_dumps(log, ["p331"], running=True, playing=0)
        self.assertIn("dump p331", str(e.exception))


class YardChecks(unittest.TestCase):
    """The harness's verdict on the yards a recorded create opened (tagpu_replay.c, yard_check),
    from a lane's counts lines."""

    @staticmethod
    def line(checked, differed, unchecked):
        return (f"replay: x at its level's end, GameTime 9: ... 3 yards changed, 0 refused; "
                f"{checked} yards checked at their create, {differed} differed, {unchecked} not "
                f"checked; 0 control groups not set\n")

    def test_every_yard_as_recorded(self):
        (label, verdict, detail), = tareplay.yard_verdict("P", self.line(2, 0, 0))
        self.assertEqual(verdict, tareplay.PASS)
        self.assertIn("2 checked", detail)

    def test_a_difference_is_named(self):
        for counts in ((2, 1, 0), (2, 0, 1)):
            with self.subTest(counts=counts):
                text = self.line(0, 0, 0) + self.line(*counts)
                (label, verdict, detail), = tareplay.yard_verdict("P", text)
                self.assertEqual(verdict, tareplay.FAIL)
                self.assertIn(f"{counts[1]} differed, {counts[2]} not checked", detail)

    def test_no_counts_line(self):
        (label, verdict, detail), = tareplay.yard_verdict("P", "replay: nothing\n")
        self.assertEqual(verdict, tareplay.UNRUN)

    @staticmethod
    def dropped(n):
        return f"recorder: {n} yard events kept out of the file by THE YARD RULE (tagpu_recfmt.h)\n"

    def test_none_kept_out_of_the_file(self):
        (label, verdict, detail), = tareplay.yard_dropped_verdict("L", self.dropped(0))
        self.assertEqual(verdict, tareplay.PASS)

    def test_one_kept_out_fails(self):
        (label, verdict, detail), = tareplay.yard_dropped_verdict(
            "L", self.dropped(0) + self.dropped(2))
        self.assertEqual(verdict, tareplay.FAIL)
        self.assertIn("2 yard events kept out of 2 recordings", detail)
        (label, verdict, detail), = tareplay.yard_dropped_verdict("L", "recorder: nothing\n")
        self.assertEqual(verdict, tareplay.UNRUN)

    @staticmethod
    def unordered(n):
        return (f"recorder: {n} footprint calls a playing cannot put in their order (THE CONTEST "
                f"ORDER, tagpu_recfmt.h)\n")

    def test_no_unordered_call(self):
        (label, verdict, detail), = tareplay.unordered_verdict("L", self.unordered(0))
        self.assertEqual(verdict, tareplay.PASS)

    def test_an_unordered_call_fails(self):
        (label, verdict, detail), = tareplay.unordered_verdict(
            "L", self.unordered(0) + self.unordered(3))
        self.assertEqual(verdict, tareplay.FAIL)
        self.assertIn("3 footprint calls no playing can order in 2 recordings", detail)
        (label, verdict, detail), = tareplay.unordered_verdict("L", "recorder: nothing\n")
        self.assertEqual(verdict, tareplay.UNRUN)

    def test_a_lane_that_did_not_record_has_no_verdict(self):
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "tagpu.log"
            log.write_text("replay: x: nothing recorded here\n")
            self.assertEqual(tareplay.unordered_check("P", log), [])
            self.assertEqual(tareplay.unordered_check("L", log)[0][1], tareplay.UNRUN)
            log.write_text(self.unordered(2))
            self.assertEqual(tareplay.unordered_check("K", log)[0][1], tareplay.FAIL)

    @staticmethod
    def played(full=0, unmoved=0, unlinked=0, ordered=9, applied=9):
        return (f"replay: x at its level's end, GameTime 9: {applied} iterations applied (0 late); 3 "
                f"carry links made, {unlinked} refused; order nodes 1 built\n"
                f"replay: x: {ordered} steps played in the contest order, 12 units placed at their "
                f"turn, {full} steps whose events the contest order could not hold (THE CONTEST "
                f"ORDER); 2 Teleport moves played, {unmoved} refused\n")

    def test_the_contest_order_played_whole(self):
        (label, verdict, detail), = tareplay.contest_verdict(
            "P", self.played() + self.played(ordered=4, applied=4), True)
        self.assertEqual(verdict, tareplay.PASS)
        self.assertIn("13 steps played in the order", detail)
        self.assertIn("2 playings", detail)

    def test_a_step_not_held_a_move_or_a_link_refused_fails(self):
        for kw, what in (({"full": 1}, "steps the contest order could not hold"),
                         ({"unmoved": 2}, "Teleport moves refused"),
                         ({"unlinked": 3}, "carry links refused")):
            with self.subTest(what=what):
                (label, verdict, detail), = tareplay.contest_verdict("P", self.played(**kw), True)
                self.assertEqual(verdict, tareplay.FAIL)
                self.assertIn(what, detail)
        (label, verdict, detail), = tareplay.contest_verdict("P", "replay: x: nothing\n", True)
        self.assertEqual(verdict, tareplay.UNRUN)

    def test_an_order_that_never_engaged_could_not_run(self):
        (label, verdict, detail), = tareplay.contest_verdict("P", self.played(ordered=0), True)
        self.assertEqual(verdict, tareplay.UNRUN)
        self.assertIn("applied 9 steps and played none", detail)
        # a playing that applied nothing, and a recording whose playing keeps no cells, engage none
        self.assertEqual(tareplay.contest_verdict(
            "P", self.played(ordered=0, applied=0), True)[0][1], tareplay.PASS)
        self.assertEqual(tareplay.contest_verdict(
            "P", self.played(ordered=0), False)[0][1], tareplay.PASS)

    def test_every_step_played_or_counted(self):
        """held: ordered + not held == applied, so a path around the order for some steps fails"""
        self.assertEqual(tareplay.contest_verdict(
            "P", self.played(ordered=7, full=2), True)[0][1], tareplay.FAIL)
        (label, verdict, detail), = tareplay.contest_verdict("P", self.played(ordered=7), True)
        self.assertEqual(verdict, tareplay.FAIL)
        self.assertIn("2 steps applied that the contest order neither played nor counted", detail)
        self.assertEqual(tareplay.contest_verdict(
            "P", self.played(ordered=7), False)[0][1], tareplay.PASS)

    def test_a_contest_line_needs_its_counts_line(self):
        contest = self.played().splitlines(keepends=True)[1]
        (label, verdict, detail), = tareplay.contest_verdict(
            "P", self.played() + contest, True)
        self.assertEqual(verdict, tareplay.UNRUN)
        self.assertIn("2 contest lines and 1 counts lines", detail)


class TakeoverFixtures(unittest.TestCase):
    """fixtures.json's takeover entries, and the processes they make a fixture run."""

    def test_entries(self):
        fixes = tareplay.fixtures()
        took = {n: f["takeover"] for n, f in fixes.items() if "takeover" in f}
        self.assertIn("battle", took)
        self.assertIn("takeover-end", took)
        known = {"player", "order", "switches", "type", "dump", "checks", "at", "seatai"}
        for name, t in took.items():
            self.assertLessEqual(set(t), known, name)
            self.assertTrue(t["player"] == "controlled" or isinstance(t["player"], int), name)
            self.assertTrue(all(isinstance(s, int) and 0 <= s <= 9 for s in t.get("seatai", [])),
                            name)
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
        # tenseat's takeover is pinned to the tick after its typed LOS command, where the
        # takeover's contact bits must come from the recording (tagpu_replay.c, R14b)
        self.assertEqual(took["tenseat"].get("at"), 901)
        self.assertIn([900, "type", "+los"], fixes["tenseat"]["steps"])
        fx = {"name": "tenseat", "fix": fixes["tenseat"], "Ts": [361, 541, 691, 841]}
        self.assertEqual(tareplay.tk_point(fx, {"options": {"controlled": 0}}), (901, 0))
        self.assertEqual(tareplay.tk_point(fx, {"options": {"controlled": 0}}, None, 931), (931, 0))
        bad = dict(fx, fix=dict(fixes["tenseat"], takeover=dict(took["tenseat"], at=5000)))
        with self.assertRaises(tareplay.CouldNotRun):
            tareplay.tk_point(bad, {"options": {"controlled": 0}})
        end = json.loads((tareplay.SCENARIOS / fixes["takeover-end"]["load"]).read_text())
        self.assertFalse(end["setup"]["clear_existing"])     # the commanders keep the economy
        self.assertIn("victory", took["takeover-end"]["checks"])
        # the GameTimes a takeover-end run draws leave T2 inside the recording
        Ts = tareplay.gametimes("takeover-end", fixes["takeover-end"]["end"], "x")
        self.assertLess(Ts[1], fixes["takeover-end"]["end"])

    def test_a_fresh_impai_seat_on_both_builds(self):
        # a two-player recording whose takeover gives ImpAI the seat OTA AI played, so the
        # takeover's seat_fresh runs (tagpu_impai.c) on each build, which the feature names
        fixes = tareplay.fixtures()
        self.assertEqual(fixes["stock-impai-takeover-fresh"].get("dll"), "stock")
        self.assertIsNone(fixes["impai-takeover-fresh"].get("dll"))
        src = (tafeat.DDRAW / "src" / "tagpu_impai.c").read_text()
        seats = re.search(r'SIM_FEATURE\(impai_seats, [A-Z_]+, "([^"]*)"\)', src)[1].split()
        for name in ("impai-takeover-fresh", "stock-impai-takeover-fresh"):
            with self.subTest(name=name):
                self.assertIn(name, seats)
                t = fixes[name]["takeover"]
                setup = json.loads((tareplay.SCENARIOS / fixes[name]["load"]).read_text())["setup"]
                ctl = {p["slot"]: p["controller"] for p in setup["players"]}
                self.assertEqual(sorted(s for s, c in ctl.items() if c != "off"), [0, 1])
                self.assertTrue(t["seatai"])
                for seat in t["seatai"]:
                    self.assertEqual(ctl[seat], "ai")
                    self.assertNotEqual(seat, t["player"])
                self.assertIn("fork", t["checks"])

    def test_stock_twins_play_their_groups_on_both_builds(self):
        # impai-fight's, place-basic's and impai-naval's stock-limits twins play the same games, so what
        # only those groups reach (the military's fight and the predictor, the placement shapes, the
        # naval rule), every line of which compiles in both builds, can be reached on each
        fixes = tareplay.fixtures()
        features = {f["name"]: f for f in tafeat.all_features()[0]}
        for group, covered in (
                ("impai-fight", (("src/impai_fight.c", "impai_fight"), ("src/impai_mil.c", "impai_mil_decide"),
                                 ("src/impai_fitparams.c", "impai_fitparams"),
                                 ("src/tagpu_impai.c", "impai_mil"))),
                ("place-basic", (("src/tagpu_placegeo.c", "place_shapes"), ("src/tagpu_worldq.c", "worldq"))),
                ("impai-naval", (("src/impai_brain.c", "impai_brain"), ("src/impai_econ.c", "impai_econ"),
                                 ("src/impai_build.c", "impai_build"), ("src/impai_maps.c", "impai_maps"),
                                 ("src/impai_mil.c", "impai_mil_decide"), ("src/impai_fight.c", "impai_fight"),
                                 ("src/impai_fitparams.c", "impai_fitparams"), ("src/tagpu_impai.c", "impai_play"),
                                 ("src/tagpu_impai.c", "impai_mil")))):
            twin = dict(fixes["stock-" + group])
            with self.subTest(group=group):
                self.assertEqual(twin.pop("dll"), "stock")
                self.assertEqual(twin, fixes[group])
            for rel, feat in covered:
                with self.subTest(group=group, feature=feat):
                    self.assertEqual(features[feat]["file"], rel)
                    self.assertLessEqual({group, "stock-" + group}, set(features[feat]["fixtures"]))


class AiActivity(unittest.TestCase):
    def lines(self, inactive=None, dispatched=True):
        return [f"recorder: AI activity test seat {s} orders "
                f"{0 if s == inactive else 7} executed "
                f"{0 if s == inactive or not dispatched else 3}"
                for s in range(10)] + ["recorder: AI activity test done [100,1900) bad 0"]

    def test_each_ai_issues_and_executes(self):
        self.assertTrue(tarewind.ai_activity(self.lines(), "test", 100, 1900, {1, 2, 3})[0])

    def test_inactive_ai_cannot_be_hidden_by_other_seats(self):
        ok, detail = tarewind.ai_activity(self.lines(inactive=2), "test", 100, 1900, {1, 2, 3})
        self.assertFalse(ok)
        self.assertIn("missing [2]", detail)

    def test_issued_but_never_dispatched_does_not_pass(self):
        self.assertFalse(tarewind.ai_activity(self.lines(dispatched=False), "test",
                                            100, 1900, {1, 2, 3})[0])

    def test_incomplete_wrong_window_or_observer_error_fails(self):
        lines = self.lines()
        for bad in (lines[:-1], lines[1:], lines + [lines[0]],
                    ["recorder: AI activity test done [100,1899) bad 0"] + lines,
                    lines[:-1] + ["recorder: AI activity test done [100,1900) bad 1"]):
            self.assertFalse(tarewind.ai_activity(bad, "test", 100, 1900, {1, 2, 3})[0])
        self.assertFalse(tarewind.ai_activity(lines, "test", 101, 1901, {1, 2, 3})[0])


class TakeoverAi(unittest.TestCase):
    def check(self, inactive=None, window=True, fraction=.92):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            (out / "K").mkdir()
            lines = AiActivity().lines(inactive=inactive)
            (out / "K" / "tagpu.log").write_text(
                "\n".join(line.replace("activity test", "activity takeover") for line in lines))
            k = {"T": 90, "P": 0, "file": "unused", "timeline": 1}
            if window:
                k["ai_window"] = [100, 1900]
            fx = {"fix": {"takeover": {"checks": ["ai"]}},
                  "procs": {"K": "ran"}, "out": out, "tk": k}
            segs = [{"id": 0, "last": 2000}, {"id": 1, "parent": 0, "last": 2000}]
            with patch.object(tareplay, "tk_ai", return_value=({1, 2, 3}, set(), fraction, 978)), \
                 patch.object(tarec, "file_segments", return_value=(segs, None)):
                return tareplay.tk_checks(fx).results()[0]

    def test_active_seats_pass_without_construction(self):
        _, verdict, detail = self.check()
        self.assertEqual(verdict, tareplay.PASS, detail)
        self.assertIn("construction observed for seats [] (reported only)", detail)

    def test_inactive_seat_fails_despite_movement(self):
        _, verdict, detail = self.check(inactive=2)
        self.assertEqual(verdict, tareplay.FAIL, detail)
        self.assertIn("missing [2]", detail)

    def test_missing_observation_is_unrun(self):
        self.assertEqual(self.check(window=False)[1], tareplay.UNRUN)

    def test_movement_threshold_stays_gated(self):
        self.assertEqual(self.check(fraction=.24)[1], tareplay.FAIL)

    def test_no_mobile_units_is_unrun(self):
        self.assertEqual(self.check(fraction=None)[1], tareplay.UNRUN)


class CrashEndsTheWait(unittest.TestCase):
    """A crash ends a log wait at once instead of after its timeout."""

    class Worker:
        def __init__(self, gamedir, up):
            self.gamedir, self.up = gamedir, list(up)

        def running(self):
            return self.up.pop(0) if len(self.up) > 1 else self.up[0]

    def log(self, gamedir, up):
        log = tareplay.Log(self.Worker(gamedir, up))
        log.ALIVE_EVERY = 0
        return log

    def test_an_errorlog_ends_the_wait(self):
        with tempfile.TemporaryDirectory() as d:
            log = self.log(Path(d), [True])
            (Path(d) / "ErrorLog.txt").write_text(
                "TotalA caused an Access Violation in\nmodule TotalA.exe at 0023:004c74c7.\n")
            t0 = time.time()
            with self.assertRaisesRegex(tareplay.Failed, "crashed: module TotalA.exe at 0023:004c74c7"):
                log.wait(r"never", 60)
            self.assertLess(time.time() - t0, 5)

    def test_a_process_that_exits_ends_the_wait(self):
        with tempfile.TemporaryDirectory() as d:
            log = self.log(Path(d), [True, False])
            with self.assertRaisesRegex(tareplay.Failed, "process exited"):
                log.wait(r"never", 60)

    def test_a_process_not_yet_seen_running_does_not(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(self.log(Path(d), [False]).wait(r"never", 1))


class LogWaitNap(unittest.TestCase):
    """A log wait sees the line the game appends after any number of reads, across a rotation and
    through a line the sink is still writing, and its nap decides only how soon: at 0.4 s and at
    Log.NAP_S it returns the same line. The game writes on a fake clock that each nap advances."""

    HEAD = "log: run R{} part {} of tagpu.log, started now\n"

    class Worker:
        def __init__(self, gamedir):
            self.gamedir = gamedir

        def running(self):
            return True

    @staticmethod
    def append(path, text):
        with open(path, "a", newline="") as f:
            f.write(text)

    def rotate(self, g, run="1", part=2):
        """The sink's rotation: the current file becomes .1, a new current file starts with its
        header."""
        log = g / "log"
        (log / "tagpu.log").rename(log / "tagpu.1.log")
        (log / "tagpu.log").write_text(self.HEAD.format(run, part))

    def play(self, nap_s, script, rx, timeout=60):
        """Log.wait(rx) while the game runs `script`, [(t, text or a callable of the game folder)]
        in time order: (the line returned, the fake time it returned at, the reads it took)."""
        with tempfile.TemporaryDirectory() as d:
            g = Path(d)
            (g / "log").mkdir()
            (g / "log" / "tagpu.log").write_text(self.HEAD.format("1", 1))
            log = tareplay.Log(self.Worker(g))
            log.NAP_S = nap_s
            clock, reads, todo = [0.0], [0], list(script)

            def nap(s):
                reads[0] += 1
                clock[0] += s
                while todo and todo[0][0] <= clock[0] + 1e-9:
                    act = todo.pop(0)[1]
                    act(g) if callable(act) else self.append(g / "log" / "tagpu.log", act)

            with patch.object(tareplay, "nap", nap), patch.object(tareplay.time, "time", lambda: clock[0]):
                got = log.wait(rx, timeout)
            return got, round(clock[0], 6), reads[0]

    def both(self, script, rx):
        slow, fast = self.play(0.4, script, rx), self.play(tareplay.Log.NAP_S, script, rx)
        self.assertEqual(slow[0], fast[0])
        return slow, fast

    def test_the_nap_is_the_fast_one(self):
        self.assertEqual(tareplay.Log.NAP_S, 0.02)

    def test_a_line_appended_after_many_reads_is_seen_as_soon_as_it_is_written(self):
        slow, fast = self.both([(1.0, "pause: armed for GameTime 3000\n")], r"pause: armed")
        self.assertEqual(fast[0], "pause: armed for GameTime 3000")
        self.assertEqual((slow[1], slow[2]), (1.2, 3))
        self.assertEqual(fast[1], 1.0)
        self.assertGreater(fast[2], 40)

    def test_the_first_match_wins_at_either_nap(self):
        rx = r"pause: paused at GameTime \d+"
        slow, _ = self.both([(0.5, "pause: paused at GameTime 3000\n"),
                             (0.55, "pause: paused at GameTime 6000\n")], rx)
        self.assertEqual(slow[0], "pause: paused at GameTime 3000")

    def test_a_line_half_written_at_a_read_is_taken_whole(self):
        slow, _ = self.both([(0.3, "pause: paused at GameTime 30"), (0.5, "00\n")],
                            r"pause: paused at GameTime \d+")
        self.assertEqual(slow[0], "pause: paused at GameTime 3000")

    def test_a_rotation_loses_no_line(self):
        rx = r"recorder: dump d\d armed"
        # written to the old file just before the rotation: read under .1 by its key
        old, _ = self.both([(0.29, "recorder: dump d1 armed\n"), (0.3, self.rotate),
                            (0.5, "recorder: dump d2 armed\n")], rx)
        self.assertEqual(old[0], "recorder: dump d1 armed")
        # written only after it
        new, _ = self.both([(0.3, self.rotate), (0.31, "units: alive=2\n"),
                            (0.5, "recorder: dump d2 armed\n")], rx)
        self.assertEqual(new[0], "recorder: dump d2 armed")

    def test_a_timeout_is_wall_time_at_either_nap(self):
        slow, fast = self.both([], r"never")
        self.assertIsNone(slow[0])
        for (_, at, _), nap_s in ((slow, 0.4), (fast, tareplay.Log.NAP_S)):
            self.assertGreaterEqual(at, 60 - 1e-6)
            self.assertLessEqual(at, 60 + nap_s + 1e-6)


class WaitRunningStall(unittest.TestCase):
    """Log.wait_running's bound (Stall, tools/stall.py) never stands between a wait and its line:
    a line already logged, or logged within Stall.EVERY, is returned with no GameTime read asked
    and no tacli process started; a level whose GameTime stands still is still named, as could
    not run. A fake game answers the peek trigger at each nap of a fake clock."""

    class Worker:
        lane = None

        def __init__(self, gamedir):
            self.gamedir = gamedir

        def running(self):
            return True

    def wait(self, lines, ticks, line_at=None):
        """wait_running(r"pause: paused") with `lines` logged at its start and the pause's line at
        `line_at` (None: never), the game answering each read with ticks(t): (the line or the
        failure, the fake time, the reads answered, whether a request was left)."""
        with tempfile.TemporaryDirectory() as d:
            g = Path(d)
            (g / "log").mkdir()
            logf = g / "log" / "tagpu.log"
            logf.write_bytes(b"log: run R1 part 1 of tagpu.log, started now\n")
            log = tareplay.Log(self.Worker(g))
            log.ALIVE_EVERY = 0
            with open(logf, "ab") as f:
                f.write("".join(ln + "\r\n" for ln in lines).encode())
            trigger = g / "tagpu_peek.trigger"
            clock, answered, written = [0.0], [0], [False]

            def nap(s):
                clock[0] += s
                with open(logf, "ab") as f:
                    if trigger.exists():
                        spec = trigger.read_text().strip()
                        trigger.unlink()
                        answered[0] += 1
                        f.write(f"peek: {spec} @0x00512345 = {ticks(clock[0])} (0x0)\r\n".encode())
                    if line_at is not None and not written[0] and clock[0] >= line_at:
                        written[0] = True
                        f.write(b"pause: paused at GameTime 900\r\n")

            def no_tacli(*a, **k):
                raise AssertionError(f"a tacli process during the wait: {a}")

            with patch.object(tareplay, "nap", nap), patch.object(tareplay, "tacli", no_tacli), \
                    patch.object(tareplay.stall.time, "monotonic", lambda: clock[0]):
                log.read()
                try:
                    got = log.wait_running(r"pause: paused")
                except tareplay.CouldNotRun as e:
                    got = str(e)
            return got, round(clock[0], 6), answered[0], trigger.exists()

    def test_a_line_already_logged_asks_nothing(self):
        got, at, answered, left = self.wait(["units: alive=3", "pause: paused at GameTime 900"],
                                            lambda t: 900)
        self.assertEqual(got.strip(), "pause: paused at GameTime 900")
        self.assertEqual((at, answered, left), (0.0, 0, False))

    def test_a_line_within_every_asks_nothing(self):
        got, at, answered, left = self.wait([], lambda t: int(t * 30), line_at=3.0)
        self.assertEqual(got.strip(), "pause: paused at GameTime 900")
        self.assertAlmostEqual(at, 3.0, delta=tareplay.Log.NAP_S + 1e-6)
        self.assertEqual((answered, left), (0, False))

    def test_a_later_line_comes_with_its_reads_answered(self):
        got, at, answered, left = self.wait([], lambda t: int(t * 30), line_at=12.0)
        self.assertEqual(got.strip(), "pause: paused at GameTime 900")
        self.assertEqual((answered, left), (2, False))

    def test_a_level_standing_still_could_not_run(self):
        got, at, answered, _ = self.wait([], lambda t: 40)
        self.assertIn("the level stood at GameTime 40 for 120 s while it ran", got)
        self.assertGreater(at, tareplay.STALL_S)
        self.assertLessEqual(at, tareplay.STALL_S + tareplay.Stall.EVERY + 1)


class ObjectiveLeftOff(unittest.TestCase):
    """A fixture whose scenarios leave the level's end test disarmed is refused before a game: the
    last scenario its lanes apply that names `objective` must set it on (objective_left_off)."""

    def scenarios(self, d):
        for name, setup in (("off.json", {"objective": "off"}), ("on.json", {"objective": "on"}),
                            ("plain.json", {}), (tareplay.END_SCENARIO.name, {"clear_existing": True})):
            (Path(d) / name).write_text(json.dumps({"format": "ta-scenario/1", "setup": setup}))
        return patch.object(tareplay, "SCENARIOS", Path(d))

    def test_an_objective_off_with_no_rearm_is_refused(self):
        with tempfile.TemporaryDirectory() as d, self.scenarios(d):
            why = tareplay.objective_left_off("x", {"load": "off.json", "end": 900})
            self.assertIn("x: off.json sets `objective` off", why)
            self.assertIn(tareplay.REARM_SCENARIO, why)
            # a re-arm after the end never runs, and a later step that disarms again wins
            self.assertTrue(tareplay.objective_left_off(
                "x", {"load": "off.json", "end": 900, "steps": [[901, "apply", "on.json"]]}))
            self.assertTrue(tareplay.objective_left_off(
                "x", {"load": "plain.json", "end": 900,
                      "steps": [[600, "apply", "off.json"], [300, "apply", "on.json"]]}))
            # U's paused scenario is taken as last when it disarms
            self.assertTrue(tareplay.objective_left_off(
                "x", {"load": "plain.json", "end": 900, "rewind_paused": "off.json"}))

    def test_a_rearm_at_or_before_the_end_is_accepted(self):
        with tempfile.TemporaryDirectory() as d, self.scenarios(d):
            for T in (899, 900):
                self.assertIsNone(tareplay.objective_left_off(
                    "x", {"load": "off.json", "end": 900, "steps": [[T, "apply", "on.json"]]}))
            self.assertIsNone(tareplay.objective_left_off("x", {"load": "plain.json", "end": 900}))

    def test_the_run_refuses_before_a_game(self):
        with tempfile.TemporaryDirectory() as d, self.scenarios(d):
            with self.assertRaisesRegex(tareplay.CouldNotRun, "x: off.json sets `objective` off"):
                tareplay.Run(["x"], SimpleNamespace(), Path(d),
                             fixes={"x": {"load": "off.json", "end": 900}})

    def test_every_fixture_of_both_tiers_ends(self):
        fixes = {**tareplay.fixtures(), **tareplay.broad_fixtures()}
        self.assertEqual(tareplay.scenario_objective(fixes["calib-applier"]["load"]), "off")
        self.assertEqual({n: tareplay.objective_left_off(n, f) for n, f in fixes.items()
                          if tareplay.objective_left_off(n, f)}, {})


class LevelEndBound(unittest.TestCase):
    """A wait for the level's end fails by name once a peek reads a GameTime past the bound the
    end was given with and the log shows no teardown (Log.wait_end)."""

    class Worker:
        def __init__(self, gamedir):
            self.gamedir = gamedir

        def running(self):
            return True

    class Stall:
        """A read answered at every look: the next answer; a (GameTime, line) pair logs the line
        before the answer, as the game logs what ran before the peek's read"""
        answers = []

        def __init__(self, log):
            self.log = log

        def check(self):
            g = self.answers.pop(0)
            if isinstance(g, tuple):
                g, line = g
                self.log.lines.append(line)
            return g

        def retire(self):
            pass

    def wait(self, answers, lines=(), at=5400):
        with tempfile.TemporaryDirectory() as d:
            log = tareplay.Log(self.Worker(Path(d)))
            log.ALIVE_EVERY = 0
            log.lines = ["scenario: applied=1 failed=0 cleared=31"] + list(lines)
            log.at = at
            with patch.object(self.Stall, "answers", list(answers)), \
                    patch.object(tareplay, "Stall", self.Stall), patch.object(tareplay, "nap"):
                return log.wait_end(r"recorder: writer: .* closed", 1)

    def test_a_level_that_runs_past_the_bound_fails_by_name(self):
        limit = 5400 + tareplay.LEVEL_END_TICKS
        with self.assertRaises(tareplay.Failed) as e:
            self.wait([5450, 5560, limit, limit + 1, limit + 900],
                      ["units: alive=1", "impai: pulse seat=1 army=0"])
        self.assertEqual(str(e.exception), f"the level did not end by GameTime {limit} after the "
                                           f"end scenario: a peek read GameTime {limit + 1}")

    def test_a_level_that_ends_in_the_bound_returns_its_line(self):
        closed = "recorder: writer: x.imprec closed: 5580 iterations"
        self.assertEqual(self.wait([5500, (5580, "recorder: ended after GameTime 5580: the "
                                               "level ended"), (5580, closed)]), closed)

    def test_the_shell_clock_after_a_teardown_is_no_failure(self):
        closed = "recorder: writer: x.imprec closed: 5580 iterations"
        torn = "reclaim: level teardown (gen 1 -> 2) on thread 7 (game 7): flushed 0 queued"
        self.assertEqual(self.wait([(9000, torn), 9100, (9200, closed)]), closed)
        # the teardown logged before the answer that read past the bound
        self.assertEqual(self.wait([(9000, torn), (9100, closed)]), closed)


class HudState(unittest.TestCase):
    """tareplay.hud_state_parse: the trigger's three `hud state` lines, as tagpu_replay.c's
    hud_state_log formats them"""

    def test_rows_and_states(self):
        lines = [
            "12:00:01 replay: forks: hud state timelines 0:0@0..5000/L/c1* 1:0@316..760/D/c1 "
            "2:0@1201..1650/L/c1* 5:2@1202..1650/L/c1*; axis 1..5000; span 1..1650; watched 5\r",
            "12:00:01 replay: forks: hud state cache serial 3",
            "12:00:01 replay: forks: hud state rewind idle available 0 span 0..0 G 0 T 0",
            "12:00:01 replay: forks: hud state result 7 fork op 2 ok: exported timeline 4",
        ]
        got = tareplay.hud_state_parse(lines)
        self.assertEqual(got["name"], "forks")
        self.assertEqual([r["id"] for r in got["timelines"]], [0, 1, 2, 5])
        self.assertEqual(got["timelines"][1], {"id": 1, "parent": 0, "fork_at": 316, "last": 760,
                                               "live": False, "controlled": 1, "on_chain": False})
        self.assertEqual([r["id"] for r in got["timelines"] if r["on_chain"]], [0, 2, 5])
        self.assertEqual((got["axis"], got["span"], got["watched"], got["cache"]),
                         ((1, 5000), (1, 1650), 5, 3))
        self.assertEqual(got["rewind"], {"state": "idle", "available": False, "span": (0, 0),
                                         "G": 0, "T": 0})
        self.assertEqual(got["result"], {"serial": 7, "kind": "fork", "op": 2, "ok": True,
                                         "why": "exported timeline 4"})

    def test_none_and_refused(self):
        lines = [
            "replay: -: hud state timelines none; axis 0..0; span 0..0; watched 0",
            "replay: -: hud state cache serial 0",
            "replay: -: hud state rewind open available 0 span 1..900 G 900 T 0",
            "replay: -: hud state result 3 rewind op -1 refused: refused: no rewind is open",
        ]
        got = tareplay.hud_state_parse(lines)
        self.assertEqual(got["timelines"], [])
        self.assertEqual(got["result"]["op"], -1)
        self.assertFalse(got["result"]["ok"])
        self.assertEqual(got["result"]["why"], "refused: no rewind is open")

    def test_missing_line(self):
        self.assertIsNone(tareplay.hud_state_parse(
            ["replay: -: hud state timelines none; axis 0..0; span 0..0; watched 0"]))
        self.assertIsNone(tareplay.hud_state_parse([
            "replay: x: hud state timelines 0:0@0..9/Q/c1; axis 0..9; span 0..9; watched 0",
            "replay: x: hud state cache serial 1",
            "replay: x: hud state rewind idle available 1 span 0..0 G 0 T 0",
            "replay: x: hud state result 0 none op 0 refused: "]))


class GameLanes(unittest.TestCase):
    """tareplay.game_lanes: the lanes a run declares to the run lock (tools/runlock.py)"""

    def run_of(self, j, *wants, multiplayer=(), kept=()):
        return SimpleNamespace(j=j, fx={f"f{k}": {"want": w} for k, w in enumerate(wants)},
                               multiplayer=multiplayer, kept=list(kept))

    def test_its_workers_no_more_than_the_lanes_it_queues(self):
        self.assertEqual(tareplay.game_lanes(self.run_of(12, ("L", "P", "R", "Q"), ("L",))), 5)
        self.assertEqual(tareplay.game_lanes(self.run_of(4, ("L", "P", "R", "Q"), ("L",))), 4)
        self.assertEqual(tareplay.game_lanes(self.run_of(2, ("L", "K", "F"))), 2)

    def test_each_two_player_game_is_two_lanes_at_least(self):
        self.assertEqual(tareplay.game_lanes(self.run_of(12, ("L",), multiplayer=["raised"])), 2)
        self.assertEqual(tareplay.game_lanes(self.run_of(12, multiplayer=["raised", "stock"])), 4)
        self.assertEqual(tareplay.game_lanes(self.run_of(12, ("L", "P", "R"),
                                                          multiplayer=["raised"])), 3)

    def test_a_kept_group_queues_no_lane(self):
        self.assertEqual(tareplay.game_lanes(self.run_of(12, ("L", "P"), ("L", "P", "R"),
                                                          kept=["f1"])), 2)


class Deaths(unittest.TestCase):
    """tarec.deaths reads each RK_EV_DEATH with the step it is in, and killall_check holds a
    commander's side to the step its slots give."""

    @staticmethod
    def rec(kind, body):
        return struct.pack("<HH", kind, len(body)) + body + bytes(-len(body) % 4)

    def death(self, slot, owner, utype, killer=0, kplayer=10):
        image = bytearray(tarec.REC_UNIT_IMAGE)
        struct.pack_into("<H", image, 0xA6, utype)
        image[0xFF] = owner
        return self.rec(tarec.RK_EV_DEATH,
                        tarec.DEATH_HEAD.pack(slot + 1, killer, kplayer, 0, 0, 0) + bytes(image))

    def tick(self, t):
        return self.rec(tarec.RK_TICK, struct.pack("<I", t))

    def test_deaths_and_the_kill_all(self):
        import killall_check
        end = lambda slot: self.rec(tarec.RK_EV_DEATH_END, struct.pack("<I", slot + 1))  # noqa: E731
        stream = (self.tick(1) + self.death(0, 1, 34) + end(0)                    # the apply's clear
                  + self.tick(40) + self.death(5, 1, 70, killer=3, kplayer=0)     # the commander
                  + self.death(9, 1, 80, killer=10, kplayer=1) + end(9) + end(5)  # nested, later slot
                  + self.tick(41) + self.death(2, 1, 80, killer=3, kplayer=1) + end(2))
        bl = [(tarec.RB_HEADER, 0, 0, b""), (tarec.RB_STREAM, 1, 41, stream)]
        with patch.object(tarec, "load", return_value=({}, bl, None)):
            rows = tarec.deaths("x.imprec")
        self.assertEqual([(r["gametime"], r["slot"], r["type"], r["owner"], r["killer"],
                           r["killer_player"], r["nested"]) for r in rows],
                         [(1, 0, 34, 1, None, None, 0), (40, 5, 70, 1, 2, 0, 0),
                          (40, 9, 80, 1, 9, 1, 1), (41, 2, 80, 1, 2, 1, 0)])
        ok, rep = killall_check.check(rows, 1, count=2)
        self.assertTrue(ok, rep)
        self.assertEqual(rep["commander"]["slot"], 5)
        late = [dict(r, gametime=42) if r["slot"] == 2 else r for r in rows]
        ok, rep = killall_check.check(late, 1)
        self.assertFalse(ok)
        self.assertEqual([(d["slot"], d["expected"], d["ok"]) for d in rep["deaths"]],
                         [(9, 40, True), (2, 41, False)])
        self.assertFalse(killall_check.check(rows, 1, count=3)[0])
        self.assertIsNone(killall_check.check(rows[:1], 1)[0])


if __name__ == "__main__":
    import devpy
    devpy.ensure()
    unittest.main(verbosity=1)
