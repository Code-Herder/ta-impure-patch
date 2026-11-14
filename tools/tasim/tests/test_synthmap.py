"""The packed lane's generated maps (tools/tasim/synthmap.py), read back through the repository's own
readers: the archive through tools/ta3do's HPI reader, the TNT through tools/tascene's checked reader,
the .ota through tavfs.tdf_sections, and the meteor and start-position rules through arenascan's.

    python3 tools/tasim/tests/test_synthmap.py

No install, except MountedLikeTheEngine, which mounts the archive by the exe's own patterns and skips
without one.
"""

import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parents[1] / "wiki"))
import arenascan  # noqa: E402
import calibref  # noqa: E402
import engrun  # noqa: E402
import synthmap as S  # noqa: E402
import tavfs  # noqa: E402

ARENAS = {"land": HERE.parent / "calib-arena-packed.json",
          "naval": HERE.parent / "calib-arena-packed-naval.json"}


def read_back(kind):
    """(the archive's file names, the TNT, the .ota's GlobalHeader section) of build(kind)'s archive."""
    with tempfile.TemporaryDirectory() as d:
        arc = tavfs.ta3do.HpiArchive(S.install(Path(d), kind))
        stem = f"maps/{S.map_name(kind)}"
        tnt = tavfs.tascene.Tnt(arc.read(stem + ".tnt"), stem)
        top = tavfs.tdf_sections(arc.read(stem + ".ota"))
        return set(arc.files), tnt, top


class Built(unittest.TestCase):
    def test_byte_identical_across_builds(self):
        for kind in S.KINDS:
            first = S.build(kind)
            S.build.cache_clear()
            self.assertEqual(S.build(kind), first, kind)

    def test_kinds_differ_and_unknown_refused(self):
        self.assertNotEqual(S.sha256("land"), S.sha256("naval"))
        with self.assertRaises(ValueError):
            S.build("lava")

    def test_tnt_reads_back(self):
        for kind in S.KINDS:
            names, tnt, _ = read_back(kind)
            self.assertEqual(names, {f"maps/{S.map_name(kind)}.tnt", f"maps/{S.map_name(kind)}.ota"})
            self.assertEqual((tnt.w16, tnt.h16), (1280, 1280))
            self.assertEqual(tnt.w16 * 16, 20480)
            self.assertEqual(tnt.sealevel, 75)
            self.assertEqual(tnt.feature_indices(), [])
            self.assertEqual(set(tnt.tilemap()), {0})

    def test_heights(self):
        _, land, _ = read_back("land")
        self.assertEqual(set(land.heights()), {85})
        _, naval, _ = read_back("naval")
        hs, n = naval.heights(), 1024 // 16
        for z in (0, n - 1):
            self.assertEqual(set(hs[z * 1280:z * 1280 + n]), {85})
            self.assertEqual(set(hs[z * 1280 + n:(z + 1) * 1280]), {0})
        self.assertEqual(set(hs[n * 1280:]), {0})
        self.assertEqual(hs.count(85), n * n)

    def test_ota_keys(self):
        want = {"land": ("Green planet", "1500", "5000"), "naval": ("Metal", "0", "4000")}
        for kind in S.KINDS:
            _, _, top = read_back(kind)
            (head,) = top
            k = head["keys"]
            planet, lo, hi = want[kind]
            self.assertEqual((k["planet"], k["minwindspeed"], k["maxwindspeed"], k["gravity"]),
                             (planet, lo, hi, "112"), kind)
            self.assertEqual(k["missionname"], S.map_name(kind))
            sch = arenascan.schemas(S.build(kind)[1].encode("latin-1"))
            self.assertEqual(len(sch), 1)
            self.assertFalse(arenascan.meteors_on(sch[0][1]), kind)
            self.assertEqual(arenascan.start_positions(sch[0][2]), 2)


class Walls(unittest.TestCase):
    def test_check_passes_and_counts_the_sites(self):
        self.assertEqual(len(S.sites("land")), 32)
        self.assertEqual(len(S.sites("naval")), 36)
        self.assertEqual({s[2:] for s in S.sites("land")}, {(6528, 1664)})
        self.assertEqual({s[2:] for s in S.sites("naval")}, {(4864, 1920)})
        self.assertNotIn((256, 384, 6528, 1664), S.sites("land"))     # the one over the corner
        self.assertIn((128, 1152, 4864, 1920), S.sites("naval"))       # 128 below the corner
        for kind in S.KINDS:
            self.assertIn(f"{len(S.sites(kind))} sites", S.check(kind))

    def test_void_exactly_off_the_open_cells_read_back(self):
        for kind in S.KINDS:
            _, tnt, _ = read_back(kind)
            a = tnt.blob[tnt.p_mapattr:tnt.p_mapattr + tnt.w16 * tnt.h16 * 4]
            words = [a[i] | a[i + 1] << 8 for i in range(1, len(a), 4)]
            cells = S.open_cells(kind)
            self.assertEqual(set(words), {S.NONE, S.VOID}, kind)
            self.assertEqual(bytes(int(w == S.NONE) for w in words), cells, kind)
            for x, z, w, h in S.sites(kind):
                self.assertEqual(cells[(z // 16) * 1280 + x // 16], 1)
                self.assertEqual(cells[((z + h) // 16 - 1) * 1280 + (x + w) // 16 - 1], 1)
                self.assertEqual(cells[(z // 16 - 1) * 1280 + x // 16], 0)        # the wall above
                self.assertEqual(cells[(z // 16) * 1280 + (x + w) // 16], 0)      # the wall to the right
            self.assertEqual(cells[0], 1)                                         # the corner region
            self.assertEqual(cells[63 * 1280 + 63], 1)
            self.assertEqual(cells[64 * 1280 + 64], 0)
            self.assertEqual(cells[1279 * 1280 + 1279], 0)                         # the border strips

    def test_check_names_a_broken_lattice(self):
        real = dict(S.LATTICE)
        try:
            S.open_cells.cache_clear()
            S.LATTICE["land"] = (256, 384, 6528, 1792, 6528, 1664, 3, 11)       # columns touching
            with self.assertRaisesRegex(ValueError, "closer than 128"):
                S.check("land")
            S.LATTICE["land"] = (256, 384, 6656, 1792, 6528, 1664, 3, 12)       # a row past the extent
            with self.assertRaisesRegex(ValueError, "outside"):
                S.check("land")
            S.LATTICE["land"] = (200, 384, 6656, 1792, 6528, 1664, 3, 11)
            with self.assertRaisesRegex(ValueError, "grid"):
                S.check("land")
        finally:
            S.LATTICE.clear()
            S.LATTICE.update(real)
            S.open_cells.cache_clear()
            S.build.cache_clear()
        self.assertIn("32 sites", S.check("land"))


class ClassMaps(unittest.TestCase):
    """The core's class maps (tools/tasim/classmap.c, classcheck's core side) on each generated map, for every
    class the install's moveinfo.tdf declares: every void cell's entry is 0, and an 8-connected flood over the
    entries of code >= 1 from each site's centre stays inside that site (THE WALLS). Skips without an install,
    a C compiler or scipy."""

    @classmethod
    def setUpClass(cls):
        import shutil
        try:
            import numpy  # noqa: F401
            from scipy import ndimage  # noqa: F401
        except ImportError as e:
            raise unittest.SkipTest(f"no numpy or scipy: {e}")
        if not shutil.which("gcc"):
            raise unittest.SkipTest("no gcc to build the core's classmap")
        import classcheck
        try:
            vfs = tavfs.Vfs(tavfs.find_install(None))
            cls.classes = classcheck.class_sections(vfs)
        except Exception as e:                      # noqa: BLE001 -- no install found
            raise unittest.SkipTest(f"no install: {e}")
        cls.cc = classcheck
        cls.exe = classcheck.build()

    def entries(self, core, i):
        """The class's map as an (H, W) array of 2-bit codes (classcheck's layout: entry (x, z) at bits
        2 (z & 15) of dword (z >> 4) * W + x)."""
        import numpy as np
        d = np.frombuffer(core["classes"][i]["map"], dtype="<u4").reshape(-1, 1280)
        shifts = (2 * np.arange(16, dtype=np.uint32))[None, :, None]
        return ((d[:, None, :] >> shifts) & 3).reshape(-1, 1280)[:1280]

    def test_void_is_zero_and_no_flood_leaves_its_site(self):
        import numpy as np
        from scipy import ndimage
        for kind in S.KINDS:
            t = tavfs.tascene.Tnt(S.build(kind)[2], S.map_name(kind))
            inp = {"w": t.w16, "h": t.h16, "sea": t.sealevel & 0xFF, "version": t.version,
                   "attr": t.blob[t.p_mapattr:t.p_mapattr + t.w16 * t.h16 * 4], "surface": 0, "lava": 0}
            core = self.cc.run_core(self.exe, self.cc.driver_input(inp, [], [], self.classes))
            self.assertEqual(sorted(core["classes"]), sorted(self.classes), kind)
            void = np.frombuffer(S.open_cells(kind), dtype=np.uint8).reshape(1280, 1280) == 0
            for i in sorted(core["classes"]):
                m = self.entries(core, i)
                self.assertFalse((m[void] != 0).any(), f"{kind} class {i}: a void cell's entry is not 0")
                labels, _n = ndimage.label(m >= 1, structure=np.ones((3, 3), dtype=int))
                spans = ndimage.find_objects(labels)          # each component's (z slice, x slice)
                for x, z, w, h in S.sites(kind):
                    lab = labels[(z + h // 2) // 16, (x + w // 2) // 16]
                    if not lab:
                        continue                    # the class cannot stand at the centre: no flood
                    zs, xs = spans[lab - 1]
                    self.assertTrue(x // 16 <= xs.start and xs.stop <= (x + w) // 16
                                    and z // 16 <= zs.start and zs.stop <= (z + h) // 16,
                                    f"{kind} class {i}: the flood from site {(x, z)} leaves it")


class Installed(unittest.TestCase):
    def test_install_keeps_identical_and_replaces_a_link_without_writing_through(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            p = S.install(d, "land")
            ino = p.stat().st_ino
            self.assertEqual(S.install(d, "land").stat().st_ino, ino)
            p.unlink()
            target = d / "elsewhere.bin"
            target.write_bytes(b"not ours")
            p.symlink_to(target)
            S.install(d, "land")
            self.assertFalse(p.is_symlink())
            self.assertEqual(target.read_bytes(), b"not ours")
            self.assertEqual(p.read_bytes(), S.build("land")[0])

    def test_place_holds_one_kind(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            S.place(d, "land")
            S.place(d, "naval")
            self.assertEqual(sorted(p.name for p in d.iterdir()), [S.ufo_name("naval")])
            S.place(d, None)
            self.assertEqual(list(d.iterdir()), [])


class Arenas(unittest.TestCase):
    def test_configs_validate_and_name_their_map(self):
        for kind, path in ARENAS.items():
            cfg = calibref.arena(path)
            self.assertEqual(cfg["synthmap"], kind)
            self.assertEqual(cfg["map"], S.map_name(kind))
            # the pathfinder reads the explored grid (0x40D817): a packed arena is mapped
            self.assertEqual(cfg["mapping"], 1)
            scn = json.loads((engrun.TREE / cfg["scenario"]).read_text())
            self.assertEqual(scn["setup"]["map"], S.map_name(kind))
            self.assertTrue(scn["setup"]["mapping"])
            x, y, w, h = cfg["flat"]["rect"]
            self.assertTrue(0 <= x and 0 <= y and x + w <= S.WU - 32 and y + h <= S.WU - 128)
            kx, ky, kw, kh = cfg["keepout"]
            for sx, sz in S.STARTS:
                self.assertTrue(kx <= sx <= kx + kw and ky <= sz <= ky + kh)
            # the lane end's unit on the corner region, an open cell (the retail end's point is void)
            ex, ez = cfg["end"]["pos"]
            self.assertTrue(kx <= ex <= kx + kw and ky <= ez <= ky + kh)
            self.assertEqual(S.open_cells(kind)[(ez // 16) * S.CELLS + ex // 16], 1)

    def test_seats_are_the_retail_arenas(self):
        for retail, packed in (("arena.json", "arena-packed.json"),
                               ("arena-naval.json", "arena-packed-naval.json")):
            a = json.loads((engrun.TREE / "scenarios/calib" / retail).read_text())["setup"]
            b = json.loads((engrun.TREE / "scenarios/calib" / packed).read_text())["setup"]
            self.assertEqual({k: v for k, v in a.items() if k != "map"},
                             {k: v for k, v in b.items() if k != "map"})

    def test_the_arena_hash_covers_the_map(self):
        cfg = calibref.arena(ARENAS["land"])
        before = engrun.arena_sha256(cfg)
        real = S.sha256
        try:
            S.sha256 = lambda kind: "0" * 64
            self.assertNotEqual(engrun.arena_sha256(cfg), before)
        finally:
            S.sha256 = real
        self.assertEqual(engrun.arena_sha256(cfg), before)


class MountedLikeTheEngine(unittest.TestCase):
    def test_the_exe_patterns_mount_it(self):
        try:
            exe = tavfs.find_install(None) / "TotalA.exe"
        except Exception as e:                      # noqa: BLE001 -- no install found
            self.skipTest(f"no install: {e}")
        if not exe.is_file():
            self.skipTest("no TotalA.exe in the install")
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / "TotalA.exe").symlink_to(exe)
            for kind in S.KINDS:
                S.install(d, kind)
            vfs = tavfs.Vfs(d)
            maps = {Path(p).stem for p, _w in vfs.enumerate("maps", "*.ota", False)}
            self.assertEqual(maps, {S.map_name(k) for k in S.KINDS})
            # a lane on a compat setup: the map opens from the archive synthmap installed, and a loose copy
            # that would shadow it is "could not run"
            lane = engrun.Lane.__new__(engrun.Lane)
            lane.w = type("W", (), {"gamedir": d})()
            lane.setup = {"name": "stub"}
            lane.arena = calibref.arena(ARENAS["land"])
            lane.arena_against_setup()
            self.assertTrue(lane.arena_checked)
            (d / "maps").mkdir()
            (d / "maps" / f"{S.map_name('land')}.tnt").write_bytes(S.build("land")[2])
            with self.assertRaisesRegex(engrun.CouldNotRun, "opens from 'loose'"):
                lane.arena_against_setup()


if __name__ == "__main__":
    unittest.main()
