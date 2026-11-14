"""tavfs's running image and profile (tools/wiki/tavfs.py, *The running image and the profile*) on game folders made
here from the pristine retail exe: retail's names, a Patch Loader's list applied, and each refusal.

    python3 tools/wiki/test_tavfs_profile.py

The exe is read at test time from the main checkout's `pristine/TotalA.exe.pristine` (gitignored); without it this
prints `SKIPPED: no pristine exe` and exits 0. The Patch Loader here is synthetic: bytes holding the loader's two
markers and a list, no code.
"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import tavfs  # noqa: E402


def pristine_path() -> Path:
    common = subprocess.run(["git", "rev-parse", "--git-common-dir"], cwd=HERE, capture_output=True, text=True)
    if common.returncode != 0:
        return None
    return (HERE / common.stdout.strip()).resolve().parent / "pristine" / "TotalA.exe.pristine"


# The list Total Mayhem's loader carries, cut to what the profile reads: the first pattern's format and argument
# ("hem" at file offset 0x100ED8, VA 0x5028D8), both weapon strings' last byte to `M` (0x503932, 0x50393A), and 11.3.
LIST = (b"[Settings]\r\nGp3FileName=may%s.GP3\r\nMultiplayerVersionMajor=11\r\nMultiplayerVersionMinor=3\r\n"
        b"[00100ED8]\r\npatch=68 65 6D\r\n[00101F32]\r\npatch=4D\r\n[00101F3A]\r\npatch=4D\r\n\0")


def loader_dll(text: bytes = LIST) -> bytes:
    return b"MZ synthetic\0" + text + b"Failed to apply game patches. Error on line %d.\n%s\0"


class Profile(unittest.TestCase):
    exe = None

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "TotalA.exe").write_bytes(self.exe)

    def tearDown(self):
        self.tmp.cleanup()

    def patch_exe(self, va: int, data: bytes):
        blob = bytearray(self.exe)
        off = tavfs._modpack().Pe(self.exe).off(va)
        blob[off:off + len(data)] = data
        (self.root / "TotalA.exe").write_bytes(bytes(blob))

    def test_retail(self):
        p = tavfs.profile(self.root)
        self.assertEqual(p["image"], "TotalA.exe")
        self.assertEqual(p["patterns"], ["rev31.GP3", "*.CCX", "*.UFO", "*.HPI"])
        self.assertEqual((p["units"], p["weapons"], p["weapon_pattern"], p["gamedata"], p["download"], p["gui"]),
                         ("units", "Weapons", "Weapons\\*.tdf", "gamedata", "download", "guis"))
        self.assertEqual((p["version_major"], p["version_minor"], p["archive_only"]), (3, 1, 1))
        self.assertEqual((p["all_pattern"], p["unit_ext"], p["tdf_ext"], p["gui_ext"]), ("*", "FBI", "TDF", "GUI"))
        self.assertEqual((p["features"], p["feature_pattern"], p["language"]), ("features", "*.tdf", "english"))
        self.assertEqual(p["copyright"].find(p["copyright_year"]), 10)
        self.assertEqual(p["mounts"], [])

    def test_loader_list(self):
        (self.root / "dplayx.dll").write_bytes(loader_dll())
        p = tavfs.profile(self.root)
        self.assertEqual(p["image"], "TotalA.exe + DPLAYX.dll's Patch Loader list")
        self.assertEqual(p["patterns"][0], "mayhem.GP3")
        self.assertEqual((p["weapons"], p["weapon_pattern"]), ("WeaponM", "WeaponM\\*.tdf"))
        self.assertEqual((p["version_major"], p["version_minor"]), (11, 3))

    def test_loader_gate(self):
        """A byte flipped at 0x410000: the loader's DllMain exits the game, so there is no image to read."""
        (self.root / "dplayx.dll").write_bytes(loader_dll())
        self.patch_exe(0x410000, b"\x15")
        with self.assertRaisesRegex(tavfs.VfsError, "refuses this exe"):
            tavfs.profile(self.root)

    def test_site(self):
        self.patch_exe(0x42AA26, (0x503921).to_bytes(4, "little"))
        with self.assertRaisesRegex(tavfs.VfsError, r"^units: 0x42AA25 holds 68 21 39 50 00"):
            tavfs.profile(self.root)

    def test_managed_mod(self):
        (self.root / "Impure.cfg").write_bytes(b"gpu=auto\r\n  MOD = x \r\n")
        with self.assertRaisesRegex(tavfs.VfsError, r"managed mod \(impure.cfg mod=x\)"):
            tavfs.profile(self.root)
        (self.root / "Impure.cfg").write_bytes(b"mod=x\r\n# mod=y\r\nmod=\r\n")
        self.assertEqual(tavfs.profile(self.root)["image"], "TotalA.exe")

    def test_not_imported(self):
        """A list in a DLL the exe does not import is never loaded, so it writes nothing."""
        (self.root / "tdraw.dll").write_bytes(loader_dll())
        p = tavfs.profile(self.root)
        self.assertEqual((p["image"], p["patterns"][0], p["weapons"]), ("TotalA.exe", "rev31.GP3", "Weapons"))

    def test_format(self):
        (self.root / "dplayx.dll").write_bytes(loader_dll(LIST.replace(b"may%s", b"may%d")))
        with self.assertRaisesRegex(tavfs.VfsError, "a format other than one %s"):
            tavfs.profile(self.root)


def main():
    exe = pristine_path()
    if exe is None or not exe.is_file():
        print("SKIPPED: no pristine exe")
        return 0
    Profile.exe = exe.read_bytes()
    r = unittest.main(argv=sys.argv[:1], exit=False)
    return 0 if r.result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
