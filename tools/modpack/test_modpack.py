#!/usr/bin/env python3
"""test_modpack.py -- modpack.py's offline rules, no mod files or game needed:

    python3 tools/modpack/test_modpack.py

The schema check refuses what the catalog must not ship (a missing sha256, base or permission, an unsafe path, a
mutator with an exe), the committed catalog passes it, a Patch Loader's list is read as patches.c reads it, the pack holds
one entry per XOR and is read to its exact end, tagpu_modcat.c reads each mod's options as the schema does, and the package's tar holds its files with fixed headers.
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
     "exe": "amod", "hide": [],
     "options": {"resource_snapping": {"allowed": True, "default": 3, "max": 3}, "quick_build": {"allowed": True, "default": True}}},
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
            "has no options": lambda j: j["mods"][0].pop("options"),
            "off with no reason": lambda j: j["mods"][0]["options"]["quick_build"].update(allowed=False),
            "outside 0..its max": lambda j: j["mods"][0]["options"]["resource_snapping"].update(default=4),
            "outside 0..8": lambda j: j["mods"][0]["options"]["resource_snapping"].update(max=9),
            "whether it is allowed": lambda j: j["mods"][0]["options"]["quick_build"].pop("allowed"),
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
    STOCK = b"s" * 32

    def test_one_entry_per_xor(self):
        x, y = {100: 5}, {200: 6, 201: 7}
        known = [(0, "a exe", b"k" * 32, dict(x)), (1, "a loader", b"l" * 32, None), (0, "b exe", b"m" * 32, dict(y))]
        pack = m.read_patches(m.pack_patches(self.STOCK, [("amod", b"h" * 32, x, 1)], known, [(50, 4), (60, 2)], [b"o" * 32]))
        self.assertEqual(len(pack["xors"]), 2, "a mod and its exe share one XOR")
        self.assertEqual(pack["mods"], [("amod", b"h" * 32, 0, 1)])
        self.assertEqual(pack["known"], [(0, b"k" * 32, 0), (1, b"l" * 32, -1), (0, b"m" * 32, 1)])
        self.assertEqual(pack["xors"][1], [(200, bytes([6, 7]))])
        self.assertEqual(pack["mask"], [(50, 4), (60, 2)])
        self.assertEqual(pack["originals"], [b"o" * 32])
        self.assertEqual(pack["stock_key"], self.STOCK)

    def test_truncated(self):
        blob = m.pack_patches(self.STOCK, [("amod", b"h" * 32, {100: 5}, 0)], [], [], [])
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


class Rules(unittest.TestCase):
    """tagpu_modcat.c, compiled natively, reads each mod's options as the schema does."""
    MAIN = (b'#include <stdio.h>\n#include <stdlib.h>\n#include "tagpu_modcat.h"\n'
            b'int main(int c, char** v) { static char b[1 << 22], e[200]; FILE* f = fopen(v[1], "rb");\n'
            b'  size_t n = fread(b, 1, sizeof b, f); MODRULES r; int got = tagpu_modcat_rules(b, n, v[2], &r, e, sizeof e);\n'
            b'  if (got == 1) printf("%d %d %d %d %d %s|%s\\n", r.manual, r.radius, r.maximum, r.quick, r.quick_default, r.manual_reason, r.quick_reason);\n'
            b'  else printf("%d %s\\n", got, e); return 0; }\n')

    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory()
        src = m.ROOT / "tagpu/ddraw/src"
        (Path(cls.dir.name) / "main.c").write_bytes(cls.MAIN)
        cls.exe = Path(cls.dir.name) / "rules"
        m.subprocess.run(["gcc", "-std=c99", "-I", str(src), "-I", str(m.ROOT / "tagpu/ddraw/inc"), "-o", str(cls.exe),
                          str(Path(cls.dir.name) / "main.c"), str(src / "tagpu_modcat.c"), str(src / "tagpu_json.c"), str(src / "tagpu_modtar.c"), str(src / "tagpu_modpak.c")], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.dir.cleanup()

    def run_on(self, j, mid):
        p = Path(self.dir.name) / "cat.json"
        p.write_text(json.dumps(j))
        return m.subprocess.run([str(self.exe), str(p), mid], capture_output=True, text=True, check=True).stdout.strip()

    def test_committed(self):
        j = json.loads((m.MODS / "catalog.json").read_text())
        for e in (x for x in j["mods"] if x["kind"] == "mod"):
            o = e["options"]
            snap, quick = o["resource_snapping"], o["quick_build"]
            want = (f'{int(snap["allowed"])} {snap["default"]} {snap["max"]} {int(quick["allowed"])} {int(quick["default"])} '
                    f'{snap.get("reason", "")}|{quick.get("reason", "")}')
            self.assertEqual(self.run_on(j, e["id"]), want)

    def test_refusals(self):
        self.assertEqual(self.run_on(GOOD, "other"), "0")
        for edit in (lambda o: o["resource_snapping"].update(default=4),
                     lambda o: o["resource_snapping"].update(max=9),
                     lambda o: o["quick_build"].update(allowed=False),
                     lambda o: o.pop("quick_build")):
            j = copy.deepcopy(GOOD)
            edit(j["mods"][0]["options"])
            self.assertTrue(self.run_on(j, "amod").startswith("-1 catalog: amod:"), self.run_on(j, "amod"))


class Places(unittest.TestCase):
    """What a file's place decides: the TDF grammar, and that a rename undoes to the same bytes."""

    def test_comments_and_end_of_text_hide_sections(self):
        text = b"[a]\r\n{\r\nx=1;\r\n}\r\n/*\r\n[hidden]\r\n{\r\n}\r\n*/\r\n[b]\r\n{\r\n[sub]\r\n{\r\n}\r\n}\r\n}\r\n[after]\r\n{\r\n}"
        self.assertEqual([n for n, _ in m.tdf_sections(text)], ["a", "b"])
        self.assertEqual([n for n, _ in m.tdf_sections(b"[a]{x=1;}\0[b]{}")], ["a"])

    def test_a_tdf_rename_undoes(self):
        text = b"[a]\r\n{\r\nx=1;\r\n}\r\n[b]\r\n{\r\ny=[2];\r\n}\r\n"
        done = m.rename_tdf(text, {"b"})
        self.assertEqual([n for n, _ in m.tdf_sections(done)], ["a", "b~"])
        self.assertEqual(m.undo_rename("x.tdf", done), text)

    def test_a_gaf_rename_undoes(self):
        name = b"wheel".ljust(32, b"\0")
        gaf = struct.pack("<III", 0x00010100, 1, 0) + struct.pack("<I", 16) + struct.pack("<HHI", 1, 0, 0) + name + b"frames"
        done = m.rename_gaf(gaf, {"wheel"})
        self.assertEqual([n for n, _ in m.gaf_entries(done)], ["wheel~"])
        self.assertEqual(m.undo_rename("x.gaf", done), gaf)

    def test_a_gaf_that_does_not_fit_has_no_names(self):
        self.assertEqual(m.gaf_entries(b"GAF"), [])
        self.assertEqual(m.gaf_entries(struct.pack("<III", 0x00010100, 1000, 0)), [])
        self.assertEqual(m.gaf_entries(struct.pack("<IIII", 0x00010100, 1, 0, 9999)), [])


class InstallerPaths(unittest.TestCase):
    def test_the_rule(self):
        for good in ("a.txt", "dir/a.txt", "COM10", "conx", "Icon/AIR.PCX"):
            self.assertTrue(m.installer_takes(good), good)
        for bad in ("", "../a", "/a", "a//b", "a\\b", "c:/a", "NUL", "com1.txt", "lpt0", "conin$", "aux/x", "nul .txt", "a.", "a ", "a\xe9"):
            self.assertFalse(m.installer_takes(bad), bad)


class CPack(unittest.TestCase):
    """tagpu_modpak.c reads what pack_patches writes, refuses a cut or lengthened pack, and its SHA-256 is SHA-256."""
    MAIN = (b'#include <stdio.h>\n#include <stdlib.h>\n#include "tagpu_modpak.h"\n'
            b'int main(int c, char** v) { static uint8_t b[1 << 16]; FILE* f = fopen(v[1], "rb"); size_t n = fread(b, 1, sizeof b, f);\n'
            b'  MODPAK p; uint8_t h[32]; MODSHA s; int i; if (c > 2) { modsha_init(&s); modsha_add(&s, b, n); modsha_end(&s, h);\n'
            b'  for (i = 0; i < 32; i++) printf("%02x", h[i]); return 0; }\n'
            b'  if (!tagpu_modpak_read(b, n, &p)) { puts("refused"); return 0; }\n'
            b'  printf("%u %u %u %u %u %s %d %d %d", p.nxors, p.nmods, p.nknown, p.nmask, p.noriginals, p.mods[0].id, p.mods[0].loader, p.known[1].xor_, tagpu_modpak_is_original(&p, p.originals));\n'
            b'  return 0; }\n')

    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory()
        src = m.ROOT / "tagpu/ddraw/src"
        (Path(cls.dir.name) / "main.c").write_bytes(cls.MAIN)
        cls.exe = Path(cls.dir.name) / "pak"
        m.subprocess.run(["gcc", "-std=c99", "-I", str(src), "-o", str(cls.exe), str(Path(cls.dir.name) / "main.c"), str(src / "tagpu_modpak.c")], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.dir.cleanup()

    def run_on(self, blob, *more):
        p = Path(self.dir.name) / "in"
        p.write_bytes(blob)
        return m.subprocess.run([str(self.exe), str(p), *more], capture_output=True, text=True, check=True).stdout.strip()

    def test_read(self):
        known = [(0, "a", b"k" * 32, {100: 5}), (1, "b", b"l" * 32, None)]
        blob = m.pack_patches(b"s" * 32, [("amod", b"h" * 32, {100: 5}, 1)], known, [(50, 4)], [b"o" * 32])
        self.assertEqual(self.run_on(blob), "1 1 2 1 1 amod 1 -1 1")
        self.assertEqual(self.run_on(blob[:-1]), "refused")
        self.assertEqual(self.run_on(blob + b"x"), "refused")
        self.assertEqual(self.run_on(blob[:40]), "refused")

    def test_sha(self):
        for data in (b"abc", b"x" * 55, b"y" * 56, b"z" * 64, b"w" * 1000):
            self.assertEqual(self.run_on(data, "sha"), m.hashlib.sha256(data).hexdigest())


class Runs(unittest.TestCase):
    def test_runs(self):
        self.assertEqual(m.runs_of({5: 1, 6: 2, 9: 3}), [(5, b"\x01\x02"), (9, b"\x03")])


if __name__ == "__main__":
    unittest.main(verbosity=1)
