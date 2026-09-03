#!/usr/bin/env python3
"""extra_weapons_fixture — rebuild scenarios/content/wpn-test.ufo from the game.

The archive holds the two test units the extra-weapons scenarios spawn:

    ARMPW4    a Peewee with Weapon4=EMG (four slots, two real guns)
    ARMLLT10  a Light Laser Tower with Weapon1 and Weapon4..10 = ARM_LIGHTLASER

A second archive, `wpn-badtgt.ufo`, holds one more unit on its own:

    ARMPW4B   the same Peewee whose Weapon4 carries w4_badTargetCategory=ENERGY

Both reuse the stock model and get their COB from `cobclone` (per-weapon copies
of the primary scripts with their own signal bits), their FBI from the stock one
plus the extra keys. Nothing here is hand-authored art; it is the smallest
content that exercises every slot the engine module can drive.

    tools/extra_weapons_fixture.py            # writes scenarios/content/wpn-test.ufo
    TA_GAMEDIR=/path/to/steam/TA tools/extra_weapons_fixture.py

Then `tacli arm <inst> weapons.on`, copy the .ufo into the instance's gamedir
(a symlink will do) and `tacli scenario load <inst> wpn-llt10 --restart`.
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path
from importlib.machinery import SourceFileLoader

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
hpipack = SourceFileLoader("hpipack", str(TOOLS / "hpipack.py")).load_module()

GAMEDIR = Path(os.environ.get("TA_GAMEDIR", "")) if os.environ.get("TA_GAMEDIR") else None
if GAMEDIR is None:
    for cand in (ROOT / "tagpu" / "gamedir",
                 Path.home() / ".local/share/Steam/steamapps/common/Total Annihilation"):
        if (cand / "totala1.hpi").exists():
            GAMEDIR = cand
            break
if GAMEDIR is None:
    sys.exit("extra_weapons_fixture: no totala1.hpi found; set TA_GAMEDIR")

OUT = ROOT / "scenarios" / "content" / "wpn-test.ufo"
OUT_BAD = ROOT / "scenarios" / "content" / "wpn-badtgt.ufo"


def main():
    arc = hpipack.Archive(GAMEDIR / "totala1.hpi")
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        pw_fbi = arc.read("units/armpw.fbi").decode("latin-1")
        llt_fbi = arc.read("units/armllt.fbi").decode("latin-1")
        (tmp / "armpw.cob").write_bytes(arc.read("scripts/armpw.cob"))
        (tmp / "armllt.cob").write_bytes(arc.read("scripts/armllt.cob"))

        pw = (pw_fbi.replace("UnitName=ARMPW;", "UnitName=ARMPW4;", 1)
                    .replace("Name=Peewee;", "Name=Peewee X4;", 1)
                    .replace("Description=Infantry Kbot;", "Description=Four-gun Kbot;", 1)
                    .replace("Weapon1=EMG;", "Weapon1=EMG;\r\n\tWeapon4=EMG;\r\n\tw4_badTargetCategory=VTOL;", 1))
        assert "Weapon4=EMG;" in pw and "UnitName=ARMPW4;" in pw
        extra = "".join(f"\r\n\tWeapon{n}=ARM_LIGHTLASER;\r\n\tw{n}_badTargetCategory=VTOL;" for n in range(4, 11))
        llt = (llt_fbi.replace("UnitName=ARMLLT;", "UnitName=ARMLLT10;", 1)
                      .replace("Name=L.L.T.;", "Name=Tenfold L.L.T.;", 1)
                      .replace("Description=Light Laser Tower;", "Description=Ten-laser Tower;", 1)
                      .replace("Weapon1=ARM_LIGHTLASER;", "Weapon1=ARM_LIGHTLASER;" + extra, 1))
        assert "Weapon10=ARM_LIGHTLASER;" in llt and "UnitName=ARMLLT10;" in llt
        (tmp / "ARMPW4.fbi").write_bytes(pw.encode("latin-1"))
        (tmp / "ARMLLT10.fbi").write_bytes(llt.encode("latin-1"))

        subprocess.run([sys.executable, str(TOOLS / "cobclone.py"), str(tmp / "armpw.cob"),
                        str(tmp / "ARMPW4.cob"), "--weapons", "4"], check=True)
        subprocess.run([sys.executable, str(TOOLS / "cobclone.py"), str(tmp / "armllt.cob"),
                        str(tmp / "ARMLLT10.cob"), "--weapons", "10"], check=True)

        OUT.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([sys.executable, str(TOOLS / "hpipack.py"), str(OUT),
                        f"units/ARMPW4.fbi={tmp / 'ARMPW4.fbi'}",
                        f"scripts/ARMPW4.cob={tmp / 'ARMPW4.cob'}",
                        f"units/ARMLLT10.fbi={tmp / 'ARMLLT10.fbi'}",
                        f"scripts/ARMLLT10.cob={tmp / 'ARMLLT10.cob'}"], check=True)

        # The bad-target probe, in its own archive so that linking it into an
        # instance is a separate decision and the type counts the multiplayer
        # notes quote stay put. Weapon1 and Weapon4 are the *same* EMG and only
        # the mask differs, so whatever the two slots shoot differently is the
        # mask and nothing else. ENERGY is a category CORSOLAR carries and
        # CORRAD does not, which makes those two the readable pair of targets.
        bad = (pw_fbi.replace("UnitName=ARMPW;", "UnitName=ARMPW4B;", 1)
                     .replace("Name=Peewee;", "Name=Peewee BadTgt;", 1)
                     .replace("Description=Infantry Kbot;",
                              "Description=Two-EMG Kbot, weapon 4 refuses ENERGY;", 1)
                     .replace("Weapon1=EMG;",
                              "Weapon1=EMG;\r\n\tWeapon4=EMG;\r\n\tw4_badTargetCategory=ENERGY;", 1))
        assert "w4_badTargetCategory=ENERGY;" in bad and "UnitName=ARMPW4B;" in bad
        (tmp / "ARMPW4B.fbi").write_bytes(bad.encode("latin-1"))
        subprocess.run([sys.executable, str(TOOLS / "cobclone.py"), str(tmp / "armpw.cob"),
                        str(tmp / "ARMPW4B.cob"), "--weapons", "4"], check=True)
        subprocess.run([sys.executable, str(TOOLS / "hpipack.py"), str(OUT_BAD),
                        f"units/ARMPW4B.fbi={tmp / 'ARMPW4B.fbi'}",
                        f"scripts/ARMPW4B.cob={tmp / 'ARMPW4B.cob'}"], check=True)
    check = hpipack.Archive(OUT)
    assert sorted(check.files) == ["scripts/armllt10.cob", "scripts/armpw4.cob",
                                   "units/armllt10.fbi", "units/armpw4.fbi"]
    assert sorted(hpipack.Archive(OUT_BAD).files) == ["scripts/armpw4b.cob",
                                                      "units/armpw4b.fbi"]
    print(f"{OUT}: OK ({OUT.stat().st_size} bytes)")
    print(f"{OUT_BAD}: OK ({OUT_BAD.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
