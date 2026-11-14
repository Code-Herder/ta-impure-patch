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
            if "end" in cfg:
                ex, ez = cfg["end"]["pos"]
                self.assertTrue(kx <= ex <= kx + kw and ky <= ez <= ky + kh)

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


if __name__ == "__main__":
    unittest.main()
