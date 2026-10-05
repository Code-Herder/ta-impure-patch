#!/usr/bin/env python3
"""Generate small original terrain fixtures for resource input/replay tests.

Generated maps and feature definitions reference the installed vent animation.
The optional geo upgrade derives a private test unit from installed content;
all generated archives stay inside isolated game folders.
"""
import argparse
import re
import struct
from pathlib import Path

import hpipack


def terrain(mode):
    w = h = 64
    attrs = bytearray()
    for z in range(h):
        for x in range(w):
            feature = 0 if mode == "uniform" else 0xffff
            if mode == "mixed" and 22 <= x <= 24 and 22 <= z <= 24:
                feature = 1
            if (x, z) == (40, 24):
                feature = 2
            attrs += struct.pack("<BHB", 32, feature, 0)
    tiles = bytes(w * h // 2)
    gfx = bytes([40]) * 1024
    names = b"".join(struct.pack("<i128s", i, n.encode()) for i, n in
                     enumerate(("RSMetal", "RSPatch", "Geothermal")))
    attr = 64 + len(tiles)
    tilegfx = attr + len(attrs)
    anim = tilegfx + len(gfx)
    mini = anim + len(names)
    header = struct.pack("<16i", 0x2000, w, h, 64, attr, tilegfx, 1, 3, anim, 0,
                         mini, 1, 0, 0, 0, 0)
    return header + tiles + attrs + gfx + names + struct.pack("<ii", 32, 32) + bytes([40]) * 1024


def metadata(name, mode):
    starts = "".join(f"[special{i}]{{specialwhat=StartPos{i+1};XPos={128+i*64};ZPos=800;}}"
                     for i in range(10))
    return (f"[GlobalHeader]{{missionname={name};missiondescription=Resource regression;"
            "planet=Lush;lineofsight=1;mapping=1;tidalstrength=20;solarstrength=20;"
            "gravity=112;minwindspeed=2500;maxwindspeed=4000;numplayers=2;size=2 x 2;"
            "SCHEMACOUNT=1;[Schema 0]{Type=Network 1;aiprofile=DEFAULT;"
            f"SurfaceMetal={20 if mode == 'mixed' else 5};MohoMetal=40;"
            f"HumanMetal=5000;ComputerMetal=5000;HumanEnergy=5000;ComputerEnergy=5000;"
            f"[specials]{{{starts}}}}}}}").encode()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("gamedir", type=Path)
    p.add_argument("--policy", choices=("on", "locked", "inherit"), default="on")
    p.add_argument("--classic", action="store_true")
    p.add_argument("--geo-upgrade", action="store_true")
    a = p.parse_args()
    tree = {}
    for mode in ("plain", "uniform", "mixed"):
        name = f"RS {mode}"
        hpipack.insert(tree, f"maps/{name.lower()}.tnt", terrain(mode))
        hpipack.insert(tree, f"maps/{name.lower()}.ota", metadata(name, mode))
    features = "".join(f"[{name}]{{world=greenworld;category=steamvents;description=Test metal;"
                       "footprintx=1;footprintz=1;height=0;filename=geotherm;seqname=geotherm;"
                       f"metal={value};metalspot=1;indestructible=1;reclaimable=0;}}"
                       for name, value in (("RSMetal", 20), ("RSPatch", 223)))
    hpipack.insert(tree, "features/green/resource-test.tdf", features.encode())
    (a.gamedir / "resource-test.ufo").write_bytes(hpipack.build(tree, method=2))
    profiles = a.gamedir / "profiles"
    profiles.mkdir(exist_ok=True)
    on = a.policy != "locked"
    (profiles / "retail.ini").write_text(f"manual_snapping={int(on)}\nquick_build={int(on)}\n"
                                        f"default_radius={3 if on else 0}\nmax_radius={3 if on else 0}\n"
                                        f"quick_build_default={int(on)}\n")
    override = profiles / "retail@76332e31.ini"
    override.unlink(missing_ok=True)
    if a.policy == "inherit":
        override.write_text("quick_build=0\ndefault_radius=1\nmax_radius=1\n")
    # The registry is the isolated instance's stopped-game store. Both mouse
    # interfaces are exercised by otherwise identical resource fixtures.
    registry = a.gamedir / "tacli-state/registry.txt"
    lines = registry.read_text().splitlines()
    lines = [line for line in lines if len(line.split("\t")) < 2 or
             line.split("\t")[1].lower() != "interface type"]
    lines.append(f"HKCU\\Software\\Cavedog Entertainment\\Total Annihilation\t"
                 f"Interface Type\tdword\t{0 if a.classic else 1}")
    registry.write_text("\n".join(lines) + "\n")
    if a.geo_upgrade:
        # Derived test content stays inside the private game folder. Reuse
        # the installed unit's model/script, with a stronger output and short
        # build time so the replay contains reclamation and completed creation.
        archive = hpipack.Archive(a.gamedir / "rev31.gp3")
        fbi = archive.read("units/corgeo.fbi").decode("latin-1")
        for key, value in {"UnitName": "RSUPGEO", "EnergyMake": 600, "BuildTime": 300,
                           "BuildCostMetal": 100, "BuildCostEnergy": 100}.items():
            fbi, n = re.subn(rf"\b{key}\s*=[^;]*;", f"{key}={value};", fbi, count=1, flags=re.I)
            assert n == 1, f"stock geothermal fixture lacks {key}"
        extra = {}
        hpipack.insert(extra, "units/rsupgeo.fbi", fbi.encode("latin-1"))
        hpipack.insert(extra, "scripts/rsupgeo.cob", archive.read("scripts/corgeo.cob"))
        hpipack.insert(extra, "download/rsupgeo.tdf", b"[MENUENTRY1]{unitmenu=CORCK;unitname=RSUPGEO;MENU=6;BUTTON=0;}")
        (a.gamedir / "resource-geo.ufo").write_bytes(hpipack.build(extra, method=2))


if __name__ == "__main__":
    main()
