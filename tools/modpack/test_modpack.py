#!/usr/bin/env python3
"""test_modpack.py -- modpack.py's offline rules, no mod files or game needed:

    python3 tools/modpack/test_modpack.py

The schema check refuses what the catalog must not ship (a missing sha256, base or permission, an unsafe path, a
mutator with an exe), the committed catalog passes it, a Patch Loader's list is read as patches.c reads it, the pack holds
one entry per XOR and is read to its exact end, and the package's tar holds its files with fixed headers.
"""
import copy
import json
import struct
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import modpack as m

GOOD = {"version": 1, "mods": [
    {"id": "amod", "kind": "mod", "name": "A", "author": "B", "version": "1", "summary": "S", "homepage": "https://example.invalid/", "permission": "P",
     "packages": [{"url": "amod/a.tar.zst", "sha256": "a" * 64, "size": 5, "files": [{"path": "amod.hpi", "sha256": "b" * 64, "size": 4}]}],
     "exe": "amod", "hide": []},
    {"id": "amut", "kind": "mutator", "name": "A", "author": "B", "version": "1", "summary": "S", "homepage": "https://example.invalid/", "permission": "P",
     "packages": [{"url": "amut/a.tar.zst", "sha256": "c" * 64, "size": 5, "files": [{"path": "amut.hpi", "sha256": "d" * 64, "size": 4}]}],
     "base": "amod", "base_hashes": {}, "files": []}]}


class Schema(unittest.TestCase):
    def broken(self, edit):
        j = copy.deepcopy(GOOD)
        edit(j)
        return m.check_catalog(j)

    def test_good(self):
        self.assertEqual(m.check_catalog(GOOD), [])

    def test_committed(self):
        self.assertEqual(m.check_catalog(json.loads((m.MODS / "catalog.json").read_text())), [])

    def test_refusals(self):
        cases = {
            "sha256": lambda j: j["mods"][0]["packages"][0].pop("sha256"),
            "sha256 ": lambda j: j["mods"][0]["packages"][0].update(sha256="xyz"),
            "permission": lambda j: j["mods"][0].pop("permission"),
            "names no base": lambda j: j["mods"][1].pop("base"),
            "no mod of the catalog": lambda j: j["mods"][1].update(base="other"),
            "base_hashes": lambda j: j["mods"][1].pop("base_hashes"),
            "carries no exe": lambda j: j["mods"][1].update(exe="x"),
            "names no exe": lambda j: j["mods"][0].pop("exe"),
            "no packages": lambda j: j["mods"][0].update(packages=[]),
            "unsafe": lambda j: j["mods"][0]["packages"][0]["files"][0].update(path="../x"),
            "unsafe ": lambda j: j["mods"][0]["packages"][0]["files"][0].update(path="/x"),
            "unsafe  ": lambda j: j["mods"][0]["packages"][0]["files"][0].update(path="C:x"),
            "declares no files": lambda j: j["mods"][0]["packages"][0].update(files=[]),
            "twice": lambda j: j["mods"][1].update(id="amod"),
            "kind": lambda j: j["mods"][0].update(kind="other"),
            "no mod of the catalog ": lambda j: j["mods"][1].update(base="amut"),
            "id none": lambda j: j["mods"][0].update(id="none"),
        }
        for want, edit in cases.items():
            errs = self.broken(edit)
            self.assertTrue(any(want.strip() in e for e in errs), f"{want!r} not named in {errs}")


class Loader(unittest.TestCase):
    INI = (b"[Settings]\r\n; a comment\r\nRegistryPath=Ab\r\nConfigFileName=x.ini\r\nGp3FileName=m%s.GP3\r\nDownloadPath=dl\r\nGameVersionString=1\r\n"
           b"MultiplayerVersionMajor=11\r\nMultiplayerVersionMinor=3\r\nCursorReclaim=No\r\nChangePathfindingSearch=Yes\r\n"
           b"\r\n; UNKNOWN\r\n[0000084C]\r\npatch=74\r\n[000FBA7F]\r\npatch=D4 ; note\r\n[0010C3FD]\r\npatch=41 62 00\r\n\0tail")

    def test_list(self):
        t = m.loader_table(b"junk" + self.INI)
        self.assertEqual(t[0x50DDFD], ord("A"))
        self.assertEqual(t[0x509EB8], ord("A"))
        self.assertEqual(t[0x50DDFD + 2], 0)
        self.assertEqual(t[0x49E9C0], 11)
        self.assertEqual(t[0x49E9C9], 3)
        self.assertEqual(bytes(t[0x40EAD6 + i] for i in range(4)), struct.pack("<I", 66650))
        self.assertNotIn(0x43E786, t)           # CursorReclaim=No
        self.assertEqual(t[m.file_offset_to_va(0x84C)], 0x74)     # .text: file offset + 0xC00
        self.assertEqual(t[0x4FCC7F], 0xD4)     # .rdata: + 0x1200
        self.assertEqual(t[0x401064], 1)        # the proxy byte

    def test_clash(self):
        with self.assertRaises(SystemExit):
            m.loader_table(b"[Settings]\r\n[0000084C]\r\npatch=74\r\n[0000084C]\r\npatch=75\r\n\0")

    def test_offsets(self):
        self.assertIsNone(m.file_offset_to_va(0x3FF))
        self.assertIsNone(m.file_offset_to_va(0x0FF600 + 0x10A00))
        self.assertEqual(m.file_offset_to_va(0x400), 0x401000)
        self.assertEqual(m.file_offset_to_va(0x0FF600), 0x501000)


class Package(unittest.TestCase):
    def test_tar(self):
        with tempfile.TemporaryDirectory() as d:
            a, b = Path(d) / "a.tar.zst", Path(d) / "b.tar.zst"
            m.write_tar({"b.hpi": b"xy", "Icon/i.pcx": b"z"}, a)
            m.write_tar({"Icon/i.pcx": b"z", "b.hpi": b"xy"}, b)
            self.assertEqual(a.read_bytes(), b.read_bytes(), "the same inputs give the same package")
            m.subprocess.run(["zstd", "-dqf", str(a), "-o", d + "/p.tar"], check=True)
            with tarfile.open(d + "/p.tar") as t:
                self.assertEqual(t.getnames(), ["Icon/i.pcx", "b.hpi"])
                self.assertTrue(all(x.mtime == 0 for x in t.getmembers()))


class Pack(unittest.TestCase):
    def test_one_entry_per_xor(self):
        x, y = {100: 5}, {200: 6, 201: 7}
        known = [(0, "a exe", b"k" * 32, dict(x)), (1, "a loader", b"l" * 32, None), (0, "b exe", b"m" * 32, dict(y))]
        pack = m.read_patches(m.pack_patches([("amod", b"h" * 32, x)], known))
        self.assertEqual(len(pack["xors"]), 2, "a mod and its exe share one XOR")
        self.assertEqual(pack["mods"], [("amod", b"h" * 32, 0)])
        self.assertEqual(pack["known"], [(0, b"k" * 32, 0), (1, b"l" * 32, -1), (0, b"m" * 32, 1)])
        self.assertEqual(pack["xors"][1], [(200, bytes([6, 7]))])

    def test_truncated(self):
        blob = m.pack_patches([("amod", b"h" * 32, {100: 5})], [])
        with self.assertRaises(SystemExit):
            m.read_patches(blob + b"x")

    def test_xor_drops_what_stays(self):
        class Stock:
            data = bytes(range(16))
            def off(self, va):
                return va
        self.assertEqual(m.xor_of(Stock(), {2: 2, 3: 7}), {3: 7 ^ 3})


class Cased(unittest.TestCase):
    def test_insert_keeps_the_name_in_one_folder(self):
        tree = {}
        m.insert_cased(tree, "Maps/A Plethora.ufo", b"1")
        m.insert_cased(tree, "maps/b.ufo", b"2")
        self.assertEqual(tree, {"Maps": {"A Plethora.ufo": b"1", "b.ufo": b"2"}})


class Runs(unittest.TestCase):
    def test_runs(self):
        self.assertEqual(m.runs_of({5: 1, 6: 2, 9: 3}), [(5, b"\x01\x02"), (9, b"\x03")])


if __name__ == "__main__":
    unittest.main(verbosity=1)
