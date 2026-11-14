"""classcheck.py's plumbing with no game: a synthetic install read through its readers, the core run on it, and a fake
engine memory answering the peek grammar (tagpu/ddraw/inc/tagpu_peek.h) in tagpu_peek.c's value formats.

    python3 tools/tasim/tests/test_classcheck.py

The engine's memory is made from the core's own answer -- its grid with the bytes the comparison must not read
scrambled (flags bits 2 and 7, the back-reference of an empty cell, the class maps' bits past the last row), the
FeatureDef table with two defs more than the map's, the class records at 0x512358 -- and then changed one way per
test: a class entry, the last row's extremes, a watermark, a unit, the defs' order, the schema, an unmapped page.
Nothing here is from the game: the map, its features and its classes are made up below.
"""

import random
import re
import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import classcheck as cc  # noqa: E402

W16, H16, SEA = 24, 20, 40

FEATURES_TDF = """
[Tree1] { footprintx=2; footprintz=2; blocking=1; filename=trees; seqname=tree1; }
[Rock3D] { footprintx=1; footprintz=1; blocking=1; object=rock; }
[Moho] { footprintx=1; footprintz=1; indestructible=1; metal=45; filename=x; seqname=y; }
[Bush] { footprintx=1; footprintz=1; blocking=0; filename=b; seqname=b; }
[Crystal] { footprintx=3; footprintz=2; blocking=1; object=crystal; }
"""

MOVEINFO_TDF = """
[CLASS0] { name=TANKSH2; footprintx=2; footprintz=2; maxwaterdepth=20; maxslope=16; maxwaterslope=16; }
[CLASS2] { name=KBOTS1; footprintx=1; footprintz=1; maxwaterdepth=15; maxslope=36; maxwaterslope=36; }
[CLASS5] { name=BOATS3; footprintx=3; footprintz=3; minwaterdepth=8; maxwaterdepth=255; maxwaterslope=255; }
"""

OTA = """
[GlobalHeader]
{
 missionname=Synthetic;
 [Schema 0]
 {
  Type=Network 1;
  SurfaceMetal=3;
  [features]
  {
   [feature0] { Featurename=crystal; XPos=12; ZPos=6; }
   [feature1] { Featurename=TREE1; XPos=3; ZPos=15; }
   [feature2] { Featurename=Bush; XPos=-1; ZPos=4; }
  }
 }
 [Schema 1] { SurfaceMetal=7; }
}
"""


def make_tnt():
    """A v2 TNT tascene.Tnt accepts: its block offsets each where the previous block's size puts it."""
    rng = random.Random(7)
    names = ["Tree1", "Rock3D", "Moho", "Bush"]
    attr = bytearray()
    for z in range(H16):
        for x in range(W16):
            hgt = max(0, min(255, 20 + 2 * x + rng.randrange(0, 3) - (60 if 8 <= z <= 11 else 0)))
            attr += bytes([hgt, 0xFF, 0xFF, 0])
    for x, z, d in ((2, 2, 0), (6, 3, 1), (7, 3, 1), (10, 10, 2), (15, 5, 3), (3, 3, 1), (4, 16, 0), (13, 7, 0),
                    (1, 9, 0xFFFC), (22, 18, 0)):
        attr[(z * W16 + x) * 4 + 1:(z * W16 + x) * 4 + 3] = struct.pack("<H", d)
    mapdata = bytes((W16 // 2) * (H16 // 2) * 2)
    p_attr = 64 + len(mapdata)
    p_gfx = p_attr + len(attr)
    p_anim = p_gfx
    p_mini = p_anim + 132 * len(names)
    head = struct.pack("<3i2I I 2i I i I i 4i", 0x2000, W16, H16, 64, p_attr, p_gfx, 0, len(names), p_anim, SEA,
                       p_mini, 0, 0, 0, 0, 0)
    anims = b"".join(struct.pack("<i", i) + n.encode().ljust(128, b"\0") for i, n in enumerate(names))
    return head + mapdata + bytes(attr) + anims + struct.pack("<ii", 0, 0)


class FakeVfs:
    """The three methods classcheck reads an install through."""

    def __init__(self):
        self.files = {"maps/synthetic.tnt": make_tnt(), "maps/synthetic.ota": OTA.encode(),
                      "features/all trees/forest.tdf": FEATURES_TDF.encode(),
                      "features/corpses/dead.tdf": b"[Tree1] { footprintx=9; footprintz=9; }",
                      "gamedata/moveinfo.tdf": MOVEINFO_TDF.encode()}

    def open(self, path):
        b = self.files.get(path.lower())
        return None if b is None else (b, "synthetic.hpi")

    def enumerate(self, top, pattern, recursive):
        return sorted((p, "synthetic.hpi") for p in self.files if p.startswith(top + "/") and p.endswith(".tdf"))

    def read_from(self, path, where):
        return self.files[path]


class FakeMem:
    def __init__(self):
        self.regions = []

    def put(self, base, data):
        self.regions.append((base, bytearray(data)))

    def find(self, addr, n):
        for base, buf in self.regions:
            if base <= addr and addr + n <= base + len(buf):
                return buf, addr - base
        return None, 0

    def read(self, addr, n):
        buf, o = self.find(addr, n)
        return None if buf is None else bytes(buf[o:o + n])

    def write(self, addr, data):
        buf, o = self.find(addr, len(data))
        buf[o:o + len(data)] = data


SPEC = re.compile(r"^(\*)?([0-9A-Fa-f]+|0x[0-9A-Fa-f]+)((?:\+(?:0x)?[0-9A-Fa-f]+)*)(?::([sx]?)((?:0x)?[0-9A-Fa-f]+))?$")


class FakePeek:
    """tagpu_peek.c's eval: the spec grammar, its size cap, its value formats, `<unreadable>`."""

    def __init__(self, mem):
        self.mem, self.calls = mem, []

    def __call__(self, specs):
        assert len(specs) <= cc.BATCH
        self.calls.append(len(specs))
        return [self.one(s) for s in specs]

    def one(self, spec):
        m = SPEC.match(spec)
        assert m, spec
        addr = int(m.group(2), 16)
        if m.group(1):
            d = self.mem.read(addr, 4)
            if d is None:
                return "<unreadable pointer>"
            addr = int.from_bytes(d, "little")
        for off in filter(None, m.group(3).split("+")):
            addr += int(off, 16)
        kind, n = m.group(4) or "u", int(m.group(5), 16) if m.group(5) else 4
        assert n <= cc.PEEK_MAX, spec
        b = self.mem.read(addr, n)
        if b is None:
            return "<unreadable>"
        if kind == "u":
            v = int.from_bytes(b, "little")
            return f"{v} (0x{v:0{2 * n}X})"
        if kind == "s":
            t = b.split(b"\0")[0]
            return '"' + "".join(chr(c) if 0x20 <= c < 0x7F else "." for c in t) + '"'
        return " ".join(f"{c:02X}" for c in b)


MAIN, GRID, SCH, FDEFS, MAPS = 0x00A00000, 0x01000000, 0x02000000, 0x03000000, 0x04000000


class Level:
    """The synthetic map read and run through the core, and an engine memory built from the core's answer."""

    def __init__(self, strip=()):
        vfs = FakeVfs()
        self.inp = cc.map_inputs(vfs, "Synthetic", 0)
        self.defs, self.placed = cc.resolve_defs(self.inp["names"], self.inp["schema"], cc.feature_sections(vfs))
        self.classes = cc.class_sections(vfs)
        self.exe = cc.build()
        self.core = cc.run_core(self.exe, cc.driver_input(self.inp, self.defs, self.placed, self.classes, strip))
        w, h = self.inp["w"], self.inp["h"]
        rng = random.Random(11)
        grid = bytearray(self.core["grid"])
        for c in range(w * h):
            grid[c * 13 + 12] |= rng.choice((0, 0x04, 0x80, 0x84))
            if grid[c * 13 + 8:c * 13 + 10] == b"\xff\xff":
                grid[c * 13 + 10:c * 13 + 12] = bytes((rng.randrange(256), rng.randrange(256)))
        m = FakeMem()
        m.put(0x511DE8, struct.pack("<I", MAIN))
        main = bytearray(0x39200)
        struct.pack_into("<II", main, cc.M_W, w, h)
        main[cc.M_SEA] = self.inp["sea"]
        struct.pack_into("<I", main, cc.M_GRID, GRID)
        struct.pack_into("<I", main, cc.M_FDEF, FDEFS)
        struct.pack_into("<I", main, cc.M_FCOUNT, len(self.defs) + 2)
        struct.pack_into("<I", main, cc.M_SCHEMA, SCH)
        m.put(MAIN, main)
        m.put(GRID, grid)
        sch = bytearray(0x1000)
        struct.pack_into("<iii", sch, cc.S_METAL, self.inp["surface"], 0, 0)
        struct.pack_into("<i", sch, cc.S_LAVA, self.inp["lava"])
        struct.pack_into("<i", sch, cc.S_NFEAT, len(self.inp["schema"]))
        m.put(SCH, sch)
        fd = bytearray(cc.FDEF_SIZE * (len(self.defs) + 2))
        for k, n in enumerate([d[0] for d in self.defs] + ["armcom_dead", "corcom_dead"]):
            fd[cc.FDEF_SIZE * k:cc.FDEF_SIZE * k + len(n)] = n.encode()
        m.put(FDEFS, fd)
        recs = bytearray(cc.CLASS_SIZE * cc.NCLASS)
        for i, c in self.core["classes"].items():
            r = cc.CLASS_SIZE * i
            struct.pack_into("<IhhIIIIII", recs, r, 0x00700000 + i, c["fx"], c["fz"], 0, 0, w, h, MAPS + 0x10000 * i,
                             c["wm"])
            mp = bytearray(c["map"])
            for d in range(((h + 15) >> 4) * w - w, ((h + 15) >> 4) * w):
                v = int.from_bytes(mp[4 * d:4 * d + 4], "little")
                v |= rng.getrandbits(32) & ~((1 << 2 * (h & 15)) - 1) & 0xFFFFFFFF
                mp[4 * d:4 * d + 4] = v.to_bytes(4, "little")
            m.put(MAPS + 0x10000 * i, mp)
        m.put(cc.CLASSES, recs)
        self.mem, self.peek = m, FakePeek(m)

    def code(self, i, x, z, v):
        """Set entry (x, z) of class i in the engine's map."""
        a = MAPS + 0x10000 * i + 4 * ((z >> 4) * self.inp["w"] + x)
        d = int.from_bytes(self.mem.read(a, 4), "little")
        d = (d & ~(3 << 2 * (z & 15))) | (v << 2 * (z & 15))
        self.mem.write(a, d.to_bytes(4, "little"))

    def run(self):
        eng = cc.engine_read(self.peek, len(self.defs))
        return cc.check(self.inp, self.defs, self.placed, self.classes, eng, self.exe, 20)


class Readers(unittest.TestCase):

    def test_atol(self):
        for text, want in ((" 12abc", 12), ("-3", -3), ("+7", 7), ("", 0), ("x1", 0), ("4294967297", 1),
                           ("2147483648", -2147483648)):
            self.assertEqual(cc.atol(text), want, text)

    def test_inputs(self):
        vfs = FakeVfs()
        inp = cc.map_inputs(vfs, "Synthetic", 0)
        self.assertEqual((inp["w"], inp["h"], inp["sea"], inp["surface"], inp["lava"]), (W16, H16, SEA, 3, 0))
        self.assertEqual(inp["schema"], [("crystal", 12, 6), ("TREE1", 3, 15), ("", -1, 4)])
        self.assertEqual(cc.map_inputs(vfs, "Synthetic", 1)["surface"], 7)
        defs, placed = cc.resolve_defs(inp["names"], inp["schema"], cc.feature_sections(vfs))
        # the TNT's names in order, then the schema's one new name; a name met again without case is the old def
        self.assertEqual([d[0] for d in defs], ["Tree1", "Rock3D", "Moho", "Bush", "crystal"])
        self.assertEqual(placed, [(4, 12, 6), (0, 3, 15), (-1, -1, 4)])
        # the first file enumerated that holds a section wins: forest.tdf's Tree1, not the corpse file's
        self.assertEqual(defs[0][1]["footprintx"], "2")
        self.assertEqual(sorted(cc.class_sections(vfs)), [0, 2, 5])
        with self.assertRaises(cc.CouldNotRun):
            cc.resolve_defs(["Nowhere"], [], cc.feature_sections(vfs))
        with self.assertRaises(cc.CouldNotRun):
            cc.map_inputs(vfs, "Synthetic", 4)

    def test_scenario(self):
        s = cc.scenario("Synthetic")["setup"]
        self.assertEqual((s["map"], s["clear_existing"], s["clear_features"], s["objective"]),
                         ("Synthetic", True, False, "off"))
        self.assertEqual(s["players"][1]["ai"], "off")


class Renderer(unittest.TestCase):
    """THE RENDERER: the launch renders with GDI unless --gpu, the hold takes no GPU memory for it, and the lever's
    state is put back after."""

    def test_the_lever_is_set_for_the_launch_and_put_back(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            lever = Path(d) / cc.GDI_LEVER
            restore = cc.renderer_lever(Path(d), gdi=True)
            self.assertTrue(lever.is_file())
            restore()
            self.assertFalse(lever.exists())
            lever.write_text("kept")
            restore = cc.renderer_lever(Path(d), gdi=False)
            self.assertFalse(lever.exists())
            restore()
            self.assertEqual(lever.read_text(), "kept")
            # a link the instance holds is replaced, never written through
            target = Path(d) / "elsewhere"
            target.write_text("theirs")
            lever.unlink()
            lever.symlink_to(target)
            cc.renderer_lever(Path(d), gdi=True)
            self.assertFalse(lever.is_symlink())
            self.assertEqual(target.read_text(), "theirs")

    def test_main_holds_no_gpu_memory_for_a_gdi_launch(self):
        import tempfile
        from unittest import mock
        sys.path.insert(0, str(HERE.parents[1]))
        import resource_regression
        import tavfs
        seen = {}

        class Inst:
            def __init__(self, gamedir):
                self.gamedir = gamedir

        def hold(what, instance, lanes=1, gpu=True, suite=None):
            seen["gpu"] = gpu

        def load(*args, timeout=None):
            seen["lever"] = (seen["dir"] / cc.GDI_LEVER).is_file()
            raise cc.CouldNotRun("stop after the launch")

        for argv, gpu in (([], False), (["--gpu"], True)):
            with tempfile.TemporaryDirectory() as d:
                seen.clear()
                seen["dir"] = Path(d)
                api = mock.Mock()
                api.Instance.return_value = Inst(Path(d))
                with mock.patch.object(resource_regression, "hold_instance", hold), \
                        mock.patch.object(tavfs, "Vfs"), \
                        mock.patch.object(resource_regression, "tacli_api", return_value=api), \
                        mock.patch.object(cc, "map_inputs", return_value={"names": [], "schema": [], "where": "x",
                                                                        "w": 1, "h": 1, "sea": 0}), \
                        mock.patch.object(cc, "resolve_defs", return_value=([], [])), \
                        mock.patch.object(cc, "feature_sections"), mock.patch.object(cc, "class_sections"), \
                        mock.patch.object(cc, "build"), mock.patch.object(cc, "tacli", load), \
                        mock.patch.object(sys, "argv", ["classcheck.py", "inst", "--map", "M"] + argv), \
                        mock.patch("builtins.print"):
                    self.assertEqual(cc.main(), 2)
                self.assertEqual((seen["gpu"], seen["lever"]), (gpu, not gpu))
                self.assertFalse((Path(d) / cc.GDI_LEVER).exists(), "the lever's state before is put back")


class Comparison(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.base = Level()
        c = cls.base.core
        # the level is not trivial: features on the grid, a 3DO's record, every class with all three codes
        words = {int.from_bytes(c["grid"][k * 13 + 8:k * 13 + 10], "little") for k in range(W16 * H16)}
        assert {0, 1, 2, 3, 4, 0xFFFE, 0xFFFD, 0xFFFC} <= words, words
        for i, m in c["classes"].items():
            codes = {(int.from_bytes(m["map"][4 * d:4 * d + 4], "little") >> 2 * k) & 3
                     for d in range(len(m["map"]) // 4) for k in range(16) if (d // W16) * 16 + k < H16}
            assert codes == {0, 1, 3}, (i, codes)

    def test_equal(self):
        code, lines = Level().run()
        self.assertEqual(code, 0, "\n".join(lines))
        self.assertIn("classcheck: PASS, 0 class map entries and 0 grid field(s) differ", lines)
        self.assertIn(f"classcheck: CLASS5 (3x3): {W16 * H16} of {W16 * H16} entries equal", lines)

    def test_batches(self):
        lv = Level()
        lv.run()
        self.assertTrue(all(n <= cc.BATCH for n in lv.peek.calls))
        self.assertGreater(len(lv.peek.calls), 3)

    def test_entry(self):
        lv = Level()
        ours = int.from_bytes(lv.core["classes"][2]["map"][4 * 5:4 * 5 + 4], "little") >> 14 & 3
        theirs = 3 if ours == 0 else 0
        lv.code(2, 5, 7, theirs)
        code, lines = lv.run()
        self.assertEqual(code, 1)
        self.assertIn(f"  CLASS2 (5, 7) core {ours} engine {theirs}", lines)
        self.assertIn(f"classcheck: CLASS2 (1x1): {W16 * H16 - 1} of {W16 * H16} entries equal", lines)

    def test_strip(self):
        # the engine's last row holds extremes LoadMap never wrote (its last column too, but the border mask's first
        # pass blocks every cell there); its maps are the fill over them
        strip = [((H16 - 1) * W16 + x, 60, 58) for x in range(0, W16 - 2, 3)]
        lv = Level()
        eng = Level(strip)
        for i, c in eng.core["classes"].items():
            lv.mem.write(MAPS + 0x10000 * i, c["map"])
        for cell, mx, mn in strip:
            lv.mem.write(GRID + cell * 13 + 5, bytes((mx, mn)))
        code, lines = lv.run()
        self.assertEqual(code, 1)
        self.assertIn(f"classcheck: the last column and row: {len(strip)} of {W16 + H16 - 1} cells hold extremes in "
                      f"the engine (LoadMap writes none; the core holds 0)", lines)
        self.assertTrue(any(s.endswith("remain -- every difference is the strip's") for s in lines), lines)
        self.assertTrue(any("grid extremes" in s and "0 of" not in s for s in lines))

    def test_grid_word(self):
        lv = Level()
        lv.mem.write(GRID + (10 * W16 + 10) * 13 + 8, b"\xff\xff")
        code, lines = lv.run()
        self.assertEqual(code, 1)
        self.assertTrue(any(s.startswith("  grid feature: (10, 10) word core 0002 engine FFFF") for s in lines), lines)

    def test_def_order(self):
        lv = Level()
        lv.mem.write(FDEFS, b"Rock3D\0".ljust(16, b"\0"))
        code, lines = lv.run()
        self.assertEqual(code, 1)
        self.assertIn("  feature defs: def 0 core 'Tree1' engine 'Rock3D'", lines)

    def test_watermark(self):
        lv = Level()
        lv.mem.write(cc.CLASSES + cc.CLASS_SIZE * 2 + 0x1C, struct.pack("<I", 90))
        code, lines = lv.run()
        self.assertEqual(code, 2)
        self.assertIn("watermarks {2: 90}", lines[0])

    def test_unit(self):
        lv = Level()
        lv.mem.write(GRID + (4 * W16 + 9) * 13, b"\x03\x00")
        code, lines = lv.run()
        self.assertEqual(code, 2)
        self.assertIn("1 cell(s) hold a unit, the first (9, 4)", lines[0])

    def test_schema(self):
        lv = Level()
        lv.mem.write(SCH + cc.S_METAL, struct.pack("<i", 7))
        code, lines = lv.run()
        self.assertEqual(code, 2)
        self.assertIn("--schema names another", lines[0])

    def test_unreadable(self):
        lv = Level()
        lv.mem.write(cc.CLASSES + cc.CLASS_SIZE * 5 + 0x18, struct.pack("<I", 0x7FFF0000))
        with self.assertRaises(cc.CouldNotRun):
            lv.run()


if __name__ == "__main__":
    unittest.main()
