"""The install loader (tools/tasim/tasim_load.c) through `tasim_run roles` and `defs` on a synthetic install, read by a
hand-written profile (tasim_load.h, THE PROFILE): the renamed folders, a weapon listing and its opens in two folders,
the pre-scan's three drops against the profile's version, flag and reference, a type defined by the file named after
its UnitName, the key 0x4B6BA0, the language's folder, a 0-byte GUI page, the move class record winning over a unit's
own keys, a weapon reference resolved by its slot, the [DAMAGE] rows by type, Impure's `nottoair`, the report's
classes, and every refusal of the profile file.

    python3 tools/tasim/tests/test_load.py

No install: every file is written here, into a temporary game folder: the units in an archive of their own, as the
pre-scan drops a loose FBI, and the rest loose.
"""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parents[1]))
import native  # noqa: E402
import hpipack  # noqa: E402

# the reference the pre-scan holds `Copyright` to, any four characters as its year (tasim_load.c, prescan_dropped)
REFERENCE = "Copyright 0000 Humongous Entertainment. All rights reserved."
COPYRIGHT = "Copyright=Copyright 1997 Humongous Entertainment. All rights reserved.; "

UNITS = {
    "TCOM": "UnitName=TCOM; Side=ARM; Version=3.1; Commander=1; Builder=1; CanMove=1; BMcode=1; MaxVelocity=1;"
            " BuildCostMetal=1000; MaxDamage=3000; TurnRate=500; BrakeRate=0.1; FootprintX=2; FootprintZ=2;"
            " Category=COMMANDER;",
    # its own keys say a 1x1 land unit; its class says a 3x3 one that needs 15 of water
    "TSUB": "UnitName=TSUB; MovementClass=TDEEP; FootprintX=1; FootprintZ=1; MinWaterDepth=0; MaxWaterDepth=0;"
            " CanMove=1; BMcode=1; MaxVelocity=2; BuildCostMetal=100; MaxDamage=500; TurnRate=500;"
            " BrakeRate=0.1; Category=SHIP;",
    "TPLANE": "UnitName=TPLANE; Version=1.2; CanFly=1; CanMove=1; BMcode=1; MaxVelocity=8; BuildCostMetal=50;"
              " MaxDamage=100; Category=VTOL;",
    "TGUN": "UnitName=TGUN; CanMove=1; BMcode=1; MaxVelocity=1.5; BuildCostMetal=120; MaxDamage=800; TurnRate=500;"
            " BrakeRate=0.1; FootprintX=2; FootprintZ=2; Category=TANK; Weapon1=TFLAK; Weapon2=TNOAIR;"
            " Weapon3=TBIGID; Frobnicate=1;",
    # (3.3 - 3) * 10 is 2.9999999999999982 as doubles: minor 2, above the game's 3.1
    "TNEWER": "UnitName=TNEWER; Version=3.3; CanMove=1; BMcode=1; MaxVelocity=1; Category=TANK;",
    "TMAJOR": "UnitName=TMAJOR; Version=4; CanMove=1; BMcode=1; MaxVelocity=1; Category=TANK;",
    # minor 1: kept
    "TMINOR": "UnitName=TMINOR; Version=3.19; CanMove=1; BMcode=1; MaxVelocity=1; Category=TANK;",
    # named after no file: the level defines it by TCOM.fbi, the pre-scan's side and key stay its own
    "tdup": "UnitName=TCOM; Side=CORE; Commander=1; CanMove=1; BMcode=1; MaxVelocity=1; MaxDamage=1;"
            " Category=COMMANDER;",
}
# without a Copyright the pre-scan holds its default to the reference; with another owner's it differs too
NO_COPYRIGHT = {
    "TBARE": "UnitName=TBARE; CanMove=1; BMcode=1; MaxVelocity=1; Category=TANK;",
    "TOTHER": "UnitName=TOTHER; Copyright=Copyright 1997 Someone Else. All rights reserved.; CanMove=1; BMcode=1;"
              " MaxVelocity=1; Category=TANK;",
}
# a loose FBI, with every key the archive's units have
LOOSE = {"TLOOSE": COPYRIGHT + "UnitName=TLOOSE; CanMove=1; BMcode=1; MaxVelocity=1; Category=TANK;"}
# TCOM in the language's folder, which 0x4290F0 tries first
KLINGON_TCOM = COPYRIGHT + UNITS["TCOM"].replace("MaxDamage=3000", "MaxDamage=4444")

# a `//***` banner: the engine's comment pass blanks the line, `/*` with it, before TFLAK's own block comment
WEAPONS = """
//*** the anti-air weapons
[TFLAK] { ID=1; Range=500; ReloadTime=1; ToAirWeapon=1; /* the flak */
  [DAMAGE] { default=5; tplane=60; TNOSUCH=999; } }
[TNOAIR] { ID=2; Range=400; ReloadTime=1; ToAirWeapon=1; NotToAir=1;
  [DAMAGE] { default=30; } }
[TBIGID] { ID=5000; Range=9999; ReloadTime=1;
  [DAMAGE] { default=1000; } }
"""
# the listing's folder: its test.tdf is listed and never read; only.tdf names no file of the open's folder
DECOY = "[TDECOY] { ID=7; Range=1; ReloadTime=1; }\n"
ONLY = "[TONLY] { ID=8; Range=1; ReloadTime=1; }\n"

MOVEINFO = "[CLASS0] { Name=TDEEP; FootprintX=3; FootprintZ=3; MinWaterDepth=15; MaxWaterDepth=255; }\n"
SIDEDATA = "[SIDE0] { name=ARM; nameprefix=ARM; }\n[SIDE1] { name=CORE; nameprefix=COR; }\n"
# TCOM's first page offers TGUN at button 0, which the download fill writes into record 4 of the page
DOWNLOAD = "[MENUENTRY1] { unitmenu=TCOM; menu=2; button=0; unitname=TGUN; }\n"
DL_PAGE = ("[GADGET0] { [COMMON] { id=0; name=ARMDL; active=1; } }\n" +
           "".join(f"[GADGET{i}] {{ [COMMON] {{ id=1; name=EMPTY{i}; active=1; }} }}\n" for i in range(1, 6)))

PROFILE = {
    "image": "synthetic", "language": "english", "units": "unitsX", "all_pattern": "*", "unit_ext": "FBI",
    "weapons": "weaponX", "weapon_pattern": "weaponY\\*.tdf", "tdf_ext": "TDF", "gamedata": "gamedatX",
    "moveinfo": "moveinfo", "sidedata": "sidedata", "download": "downloadX", "gui": "guiX", "gui_ext": "GUI",
    "features": "features", "feature_pattern": "*.tdf", "version_major": "3", "version_minor": "1",
    "archive_only": "1", "copyright": REFERENCE, "copyright_year": "0000",
    "copyright_default": "Run to the Village! Warn your brother!",
}


def fbi(keys):
    return ("[UNITINFO]\n{\n" + keys.replace("; ", ";\n") + "\n}\n").encode()


def key_of(data: bytes) -> str:
    """0x4B6BA0 (engine/network.md, *The key*), written here from its description."""
    s = x = s2 = x2 = 0
    for i, b in enumerate(data):
        s = (s + b) & 255
        x ^= b
        s2 = (s2 + ((i & 255) ^ b)) & 255
        x2 ^= (i + b) & 255
    return f"0x{x2 << 24 | s2 << 16 | x << 8 | s:08X}"


def write_install(root: Path):
    """The install, and {unit file name: its bytes} of the archive's FBIs."""
    tree, files = {}, {}
    for name, keys in UNITS.items():
        files[name] = fbi(COPYRIGHT + keys)
    for name, keys in NO_COPYRIGHT.items():
        files[name] = fbi(keys)
    for name, data in files.items():
        hpipack.insert(tree, f"unitsX/{name}.fbi", data)
    hpipack.insert(tree, "unitsX-klingon/TCOM.fbi", fbi(KLINGON_TCOM))
    hpipack.insert(tree, "unitsX/TGUN.ovr", b"[TA Unit Override] { Compatability=1; }\n")
    (root / "units.ufo").write_bytes(hpipack.build(tree))
    (root / "unitsX").mkdir()
    for name, keys in LOOSE.items():
        (root / "unitsX" / f"{name}.fbi").write_bytes(fbi(keys))
    for folder, files_in in (("weaponX", {"test.tdf": WEAPONS}), ("weaponY", {"test.tdf": DECOY, "only.tdf": ONLY}),
                             ("gamedatX", {"moveinfo.tdf": MOVEINFO, "sidedata.tdf": SIDEDATA}),
                             ("downloadX", {"tcom.tdf": DOWNLOAD}), ("guiX", {"ARMDL.GUI": DL_PAGE, "TCOM1.GUI": ""})):
        (root / folder).mkdir()
        for name, text in files_in.items():
            (root / folder / name).write_text(text)
    return files


def profile_text(root: Path, mounts, over=None, drop=(), extra=""):
    rows = {"root": str(root), **PROFILE, **(over or {})}
    lines = [f"{k}\t{v}" for k, v in rows.items() if k not in drop]
    lines[3:3] = [f"mount\t{m}" for m in mounts]
    return "tasim-profile/1\n" + "\n".join(lines) + "\n" + extra


class Load(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exe = native.build()
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name) / "game"
        cls.root.mkdir()
        cls.files = write_install(cls.root)
        cls.mounts = [str(cls.root / "units.ufo")]
        cls.n = 0

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def profile(self, text):
        Load.n += 1
        path = Path(self.tmp.name) / f"p{self.n}.profile"
        path.write_text(text)
        return path

    def run_raw(self, cmd, text, *extra):
        return subprocess.run([str(self.exe), cmd, "--profile", str(self.profile(text)), *extra, "--json"],
                              capture_output=True, text=True)

    def run_cmd(self, cmd, over=None, root=None, mounts=None):
        root = root or self.root
        r = self.run_raw(cmd, profile_text(root, self.mounts if mounts is None else mounts, over))
        self.assertEqual(r.returncode, 0, r.stderr)
        return json.loads(r.stdout)

    def roles(self, over=None):
        return [u for u in self.run_cmd("roles", over)["units"]]

    def by_name(self, units, name):
        return [u for u in units if u["name"] == name]

    def test_renamed_folders(self):
        """unitsX, weaponX and gamedatX: the class record in gamedatX wins over TSUB's own keys."""
        u = self.by_name(self.roles(), "TSUB")[0]
        self.assertEqual(u["footprint"], [3, 3])
        self.assertEqual(u["layer"], "sea")

    def test_no_class_folder(self):
        """Without the class file its class is not found, so its own keys stand, as 0x42CD36 copies them."""
        u = self.by_name(self.roles({"gamedata": "gamedata"}), "TSUB")[0]
        self.assertEqual(u["footprint"], [1, 1])
        self.assertEqual(u["layer"], "land")

    def test_weapon_folders(self):
        """Listed from weaponY, opened from weaponX: weaponX's test.tdf is read, only.tdf opens nothing."""
        d = self.run_cmd("defs")
        self.assertEqual([w["section"] for w in d["weapons"]], ["TFLAK", "TNOAIR"])
        self.assertEqual(d["counts"]["weapons_refused"], 1)

    def test_weapons(self):
        u = self.by_name(self.roles(), "TGUN")[0]
        self.assertEqual(u["dps_air"], 60 / 30)           # TFLAK's row for TPLANE, matched without case
        self.assertEqual(u["dps_ground"], 30 / 30)        # TNOAIR: nottoair makes its toairweapon a ground slot
        self.assertEqual(u["range_ground"], 400)          # TBIGID's ID is past the slots: its weapon is the null one

    def test_prescan(self):
        """Version above 3.1, a loose file and a Copyright other than the reference drop; the version is the
        profile's, and so is the archive flag."""
        d = self.run_cmd("defs")
        self.assertEqual(d["counts"]["units_dropped"], {"version": 2, "loose": 1, "copyright": 2})
        self.assertEqual(d["counts"]["units"], 6)
        d = self.run_cmd("defs", {"version_major": "11", "version_minor": "3"})
        self.assertEqual(d["counts"]["units_dropped"]["version"], 0)
        self.assertIn("TMAJOR", [u["unitname"] for u in d["units"]])
        d = self.run_cmd("defs", {"archive_only": "0"})
        self.assertEqual(d["counts"]["units_dropped"]["loose"], 0)
        self.assertIn("TLOOSE", [u["unitname"] for u in d["units"]])

    def test_reread(self):
        """tdup.fbi names TCOM: two TCOM types, both defined by TCOM.fbi, tdup's keeping its own side."""
        d = self.run_cmd("defs")
        self.assertEqual(d["counts"]["units_reread"], 1)
        self.assertEqual([u["unitname"] for u in d["units"]], ["TCOM", "TCOM", "TGUN", "TMINOR", "TPLANE", "TSUB"])
        tcom = self.by_name(self.roles(), "TCOM")
        self.assertEqual([u["maxdamage"] for u in tcom], [3000, 3000])
        self.assertEqual(sorted(u["side"] for u in tcom), ["ARM", "CORE"])

    def test_orphan(self):
        """A kept type whose own file does not open: the engine keeps the menu-time record, which is not modelled."""
        root = Path(self.tmp.name) / "orphan"
        root.mkdir()
        tree = {}
        hpipack.insert(tree, "unitsX/TCOM.fbi", self.files["TCOM"])
        hpipack.insert(tree, "unitsX/torphan.fbi", fbi(COPYRIGHT + "UnitName=TNOFILE; CanMove=1; Category=TANK;"))
        (root / "units.ufo").write_bytes(hpipack.build(tree))
        r = self.run_raw("defs", profile_text(root, [str(root / "units.ufo")]))
        self.assertEqual(r.returncode, 2)
        self.assertIn("type TNOFILE: unitsX\\TNOFILE.FBI does not open", r.stderr)

    def test_key(self):
        d = self.run_cmd("defs")
        got = {}
        for u in d["units"]:
            got.setdefault(u["unitname"], []).append(u["key"])
        self.assertEqual(sorted(got["TCOM"]), sorted([key_of(self.files["TCOM"]), key_of(self.files["tdup"])]))
        self.assertEqual(got["TSUB"], [key_of(self.files["TSUB"])])
        self.assertEqual(got["TGUN"], [None])                 # beside TGUN.ovr

    def test_language(self):
        """unitsX-klingon\\TCOM.fbi is read first under `language klingon`, by both TCOM types."""
        tcom = self.by_name(self.roles({"language": "klingon"}), "TCOM")
        self.assertEqual([u["maxdamage"] for u in tcom], [4444, 4444])
        d = self.run_cmd("defs", {"language": "klingon"})
        keys = [u["key"] for u in d["units"] if u["unitname"] == "TCOM"]
        self.assertIn(key_of(fbi(KLINGON_TCOM)), keys)

    def test_empty_page(self):
        """TCOM1.GUI holds 0 bytes, which every probe takes as missing: the page is the side's ARMDL.GUI, which
        offers TGUN to the commander, so TGUN is on its tech tree."""
        self.assertEqual(self.by_name(self.roles(), "TGUN")[0]["tech"], 1)

    def test_json(self):
        d = self.run_cmd("defs")
        gun = next(u for u in d["units"] if u["unitname"] == "TGUN")
        self.assertEqual(gun["slots"], [1, 2] + [-1] * 8)
        self.assertEqual(gun["weapons"]["0x1EE"], "TFLAK")
        self.assertEqual(gun["weapons"]["0x1F6"], "")
        flak = next(w for w in d["weapons"] if w["section"] == "TFLAK")
        self.assertEqual(flak["damage"], [["tplane", 60], ["TNOSUCH", 999]])
        self.assertEqual(d["profile"]["mounts"], ["units.ufo"])
        self.assertEqual((d["profile"]["units"], d["profile"]["version"], d["profile"]["archive_only"]),
                         ("unitsX", [3, 1], 1))
        sub = next(u for u in d["units"] if u["unitname"] == "TSUB")
        image = {off: data for off, _w, data, _m in sub["image"]}
        self.assertEqual(image[0x14A], "0300")
        self.assertEqual(image[0x1C0], "0f00")
        self.assertEqual(sub["class"], 0)
        rep = {(r["class"], r["what"]): r for r in d["unmodelled"]}
        self.assertIn(("ignored", "Frobnicate"), rep)
        self.assertEqual(rep[("later", "nottoair")]["landing"], "T2")

    def refused(self, text, named):
        r = self.run_raw("defs", text)
        self.assertEqual(r.returncode, 2, r.stdout[:200])
        self.assertIn(named, r.stderr)

    def test_refusals(self):
        base = profile_text(self.root, self.mounts)
        self.refused(base.replace("tasim-profile/1", "tasim-profile/2"), "not a tasim-profile/1 file")
        self.refused(base + "colour\tred\n", "an unknown key `colour`")
        self.refused(base + "units\tunitsY\n", "units given twice")
        self.refused(profile_text(self.root, self.mounts, drop=("gui_ext",)), "gui_ext is missing")
        self.refused(base + "no tab here\n", "has no TAB")
        self.refused(base.replace("unitsX\n", "unitsX\r\n"), "units: its value holds a TAB or a CR")
        self.refused(profile_text(self.root, self.mounts, {"units": "u" * 64}), "units: its value is empty")
        self.refused(profile_text(self.root, self.mounts, {"gui": ""}), "gui: its value is empty")
        self.refused(profile_text(self.root, self.mounts, {"version_major": "200"}), "version_major:")
        self.refused(profile_text(self.root, self.mounts, {"archive_only": "2"}), "archive_only:")
        self.refused(profile_text(self.root, ["units.ufo"]), "mount: line")
        self.refused(profile_text(self.root, self.mounts, {"root": "game"}), "root: its value")
        self.refused(profile_text(self.root, self.mounts, {"weapon_pattern": "*.tdf"}), "weapon_pattern:")
        self.refused(profile_text(self.root, self.mounts, {"weapon_pattern": "weaponY\\"}), "weapon_pattern:")
        self.refused(profile_text(self.root, self.mounts, {"copyright_year": "1997"}), "copyright_year:")
        self.refused(profile_text(self.root, self.mounts, {"copyright_year": "000"}), "copyright_year:")


if __name__ == "__main__":
    unittest.main()
