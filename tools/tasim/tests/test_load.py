"""The install loader (tools/tasim/tasim_load.c) through `tasim_run roles` and `defs` on a synthetic install: the move
class record winning over a unit's own keys, a weapon reference resolved by its slot as the engine resolves it, the
[DAMAGE] rows by type, Impure's `nottoair`, the folders a mod renames, the menu-time pre-scan's three drops, and the
report's classes.

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
COPYRIGHT = "Copyright=Copyright 1997 Humongous Entertainment. All rights reserved.; "

UNITS = {
    "TCOM": "UnitName=TCOM; Version=3.1; Commander=1; CanMove=1; BMcode=1; MaxVelocity=1; BuildCostMetal=1000;"
            " MaxDamage=3000; TurnRate=500; BrakeRate=0.1; FootprintX=2; FootprintZ=2; Category=COMMANDER;",
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
}
# without a Copyright the pre-scan holds its default to the reference; with another owner's it differs too
NO_COPYRIGHT = {
    "TBARE": "UnitName=TBARE; CanMove=1; BMcode=1; MaxVelocity=1; Category=TANK;",
    "TOTHER": "UnitName=TOTHER; Copyright=Copyright 1997 Someone Else. All rights reserved.; CanMove=1; BMcode=1;"
              " MaxVelocity=1; Category=TANK;",
}
# a loose FBI, with every key the archive's units have
LOOSE = {"TLOOSE": COPYRIGHT + "UnitName=TLOOSE; CanMove=1; BMcode=1; MaxVelocity=1; Category=TANK;"}

WEAPONS = """
[TFLAK] { ID=1; Range=500; ReloadTime=1; ToAirWeapon=1;
  [DAMAGE] { default=5; tplane=60; TNOSUCH=999; } }
[TNOAIR] { ID=2; Range=400; ReloadTime=1; ToAirWeapon=1; NotToAir=1;
  [DAMAGE] { default=30; } }
[TBIGID] { ID=5000; Range=9999; ReloadTime=1;
  [DAMAGE] { default=1000; } }
"""

MOVEINFO = "[CLASS0] { Name=TDEEP; FootprintX=3; FootprintZ=3; MinWaterDepth=15; MaxWaterDepth=255; }\n"


def fbi(keys):
    return ("[UNITINFO]\n{\n" + keys.replace("; ", ";\n") + "\n}\n").encode()


def write_install(root: Path, gamedata: str):
    tree = {}
    for name, keys in UNITS.items():
        hpipack.insert(tree, f"units/{name}.fbi", fbi(COPYRIGHT + keys))
    for name, keys in NO_COPYRIGHT.items():
        hpipack.insert(tree, f"units/{name}.fbi", fbi(keys))
    (root / "units.ufo").write_bytes(hpipack.build(tree))
    (root / "units").mkdir()
    for name, keys in LOOSE.items():
        (root / "units" / f"{name}.fbi").write_bytes(fbi(keys))
    (root / "weapons").mkdir()
    (root / "weapons" / "test.tdf").write_text(WEAPONS)
    (root / gamedata).mkdir()
    (root / gamedata / "moveinfo.tdf").write_text(MOVEINFO)


class Load(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exe = native.build()
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name) / "game"
        cls.root.mkdir()
        write_install(cls.root, "gamedatX")             # a mod's renamed gamedata folder
        cls.mounts = Path(cls.tmp.name) / "units.mounts"
        cls.mounts.write_text(f"{cls.root / 'units.ufo'}\n")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_cmd(self, cmd, *extra):
        r = subprocess.run([str(self.exe), cmd, "--cwd", str(self.root), "--archives", str(self.mounts),
                            *extra, "--json"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return json.loads(r.stdout)

    def roles(self, *extra):
        return {u["name"]: u for u in self.run_cmd("roles", *extra)["units"]}

    def test_class_record_wins(self):
        u = self.roles("--folders", "gamedata=gamedatX")["TSUB"]
        self.assertEqual(u["footprint"], [3, 3])
        self.assertEqual(u["layer"], "sea")

    def test_folders(self):
        """Without the folder its class is not found, so its own keys stand, as 0x42CD36 copies them."""
        u = self.roles()["TSUB"]
        self.assertEqual(u["footprint"], [1, 1])
        self.assertEqual(u["layer"], "land")

    def test_weapons(self):
        u = self.roles("--folders", "gamedata=gamedatX")["TGUN"]
        self.assertEqual(u["dps_air"], 60 / 30)           # TFLAK's row for TPLANE, matched without case
        self.assertEqual(u["dps_ground"], 30 / 30)        # TNOAIR: nottoair makes its toairweapon a ground slot
        self.assertEqual(u["range_ground"], 400)          # TBIGID's ID is past the slots: its weapon is the null one

    def test_prescan(self):
        """The pre-scan's verdict: Version above 3.1, a loose file, and a Copyright other than the reference drop."""
        d = self.run_cmd("defs")
        self.assertEqual(d["counts"]["units_dropped"], {"version": 2, "loose": 1, "copyright": 2})
        self.assertEqual(d["counts"]["units"], 5)

    def test_defs(self):
        d = self.run_cmd("defs", "--folders", "gamedata=gamedatX")
        self.assertEqual([u["unitname"] for u in d["units"]], ["TCOM", "TGUN", "TMINOR", "TPLANE", "TSUB"])
        sub = next(u for u in d["units"] if u["unitname"] == "TSUB")
        image = {off: data for off, _w, data, _m in sub["image"]}
        self.assertEqual(image[0x14A], "0300")
        self.assertEqual(image[0x1C0], "0f00")
        self.assertEqual(sub["class"], 0)
        self.assertEqual(d["counts"]["weapons_refused"], 1)
        gun = next(u for u in d["units"] if u["unitname"] == "TGUN")
        self.assertEqual(gun["weapons"]["0x1EE"], "TFLAK")
        self.assertEqual(gun["weapons"]["0x1F6"], "")
        rep = {(r["class"], r["what"]): r for r in d["unmodelled"]}
        self.assertIn(("ignored", "Frobnicate"), rep)
        self.assertEqual(rep[("later", "nottoair")]["landing"], "T2")

    def test_bad_folders(self):
        r = subprocess.run([str(self.exe), "roles", "--cwd", str(self.root), "--archives", str(self.mounts),
                            "--folders", "maps=x", "--json"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 2)


if __name__ == "__main__":
    unittest.main()
