#!/usr/bin/env python3
"""synthmap.py -- the packed calibration lane's two flat maps, generated from nothing.

    synthmap.py install GAMEDIR land|naval      # writes GAMEDIR/impure-calib-<kind>.ufo
    synthmap.py sha256 [land|naval]             # the archive's hash, which keys the arena

A packed batch translates up to K fights into regions of one map that no unit, shot or sensor of
one can reach from another, which the retail arenas cannot hold more than about three of (Links
CC's flat rectangle is 5584 x 1600 world units, Seven Islands' water rectangle 5000 x 1600). So the
lane plays on one of these:

| | land: impure-calib-land | naval: impure-calib-naval |
|---|---|---|
| size | 1280 x 1280 height cells = 20 480 world units square, Seven Islands' size | the same |
| heights | 85 everywhere | 0 everywhere, a dry plateau of 85 over the corner (0, 0), 1024 x 1024 world units |
| sea level | 75, Links CC's | 75, Seven Islands' |
| gravity | 112, Links CC's | 112, Seven Islands' |
| wind | 1500 to 5000, Links CC's | 0 to 4000, Seven Islands' |
| planet | Green planet, Links CC's | Metal, Seven Islands' |
| line of sight, mapping | 1, 1 as Links CC's .ota | 0, 0 as Seven Islands' .ota |
| meteors | off: no meteorweapon, density 0 | off |
| features | none | none |
| start positions | two, on the corner region's diagonal | two, on the plateau |

The retail values are the scalar settings of each arena's .ota (READ through tools/wiki/tavfs.py from
the install, 2026-11-13); nothing of a retail file is copied, and every byte here is written by
build(). The corner region (0, 0)-(1024, 1024) holds the start positions on both maps, and the arena
configs (calib-arena-packed*.json) name it `keepout`: the packer places no fight there, so the
skirmish's commanders, which the launch's own apply removes, never stand on a fight's ground.

The TNT is the layout tools/resource_fixture.py writes and tools/tascene's Tnt reader checks
(research/notes/file-formats.md §6): the header, a tile map of u16 per 32-wu cell all naming tile 0,
the attributes `<BHB` (height, feature 0xFFFF = none, 0) per 16-wu cell, one 32 x 32 tile of one
palette index, no feature names, and a 32 x 32 minimap. The archive is tools/hpipack.py's.

Deterministic: build(kind) is a function of `kind` alone, byte-identical across calls and machines
(hpipack writes no time or path), so sha256(kind) names the map an arena hash covers
(engrun.arena_sha256): a change here re-keys every packed reference.

The .ufo goes into an instance's game folder under a NEW name, never over an existing one: an
instance's files link into the Steam install, and a write onto a linked name writes through into it.
install() writes a temporary file and renames it into place, which replaces a directory entry and
never writes through one.
"""

import functools
import hashlib
import os
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import hpipack  # noqa: E402

KINDS = ("land", "naval")
CELLS = 1280                    # height cells a side, 16 world units each
WU = CELLS * 16                 # 20 480 world units a side
SEALEVEL = 75
GROUND = {"land": 85, "naval": 0}
PLATEAU = 85                    # the naval map's dry corner
CORNER = 1024                   # world units a side of the corner region: the start positions,
                                # the naval plateau, the arenas' keepout
TILE_INDEX = 40                 # the one palette index of the one tile and of the minimap
MINI = 32                       # minimap side: tagpu_patches.c's map_picture_shrink bounds a
                                # minimap under the stock 252 (resource_fixture.py's maps are 32 too)
STARTS = ((256, 256), (768, 768))   # world units, on the corner region's diagonal

OTA = {
    "land": {"planet": "Green planet", "lineofsight": 1, "mapping": 1,
             "minwindspeed": 1500, "maxwindspeed": 5000, "gravity": 112},
    "naval": {"planet": "Metal", "lineofsight": 0, "mapping": 0,
              "minwindspeed": 0, "maxwindspeed": 4000, "gravity": 112},
}


def map_name(kind: str) -> str:
    """The map's name: its .tnt/.ota stem, which a scenario's setup.map names."""
    _kind(kind)
    return f"impure-calib-{kind}"


def ufo_name(kind: str) -> str:
    return map_name(kind) + ".ufo"


def _kind(kind: str):
    if kind not in KINDS:
        raise ValueError(f"no synthetic map {kind!r}: {' or '.join(KINDS)}")


def heights(kind: str) -> bytes:
    """One height byte per 16-wu cell, row-major (z rows of x)."""
    _kind(kind)
    ground = GROUND[kind]
    if kind == "land":
        return bytes([ground]) * (CELLS * CELLS)
    n = CORNER // 16
    plateau_row = bytes([PLATEAU]) * n + bytes([ground]) * (CELLS - n)
    return plateau_row * n + bytes([ground]) * (CELLS * (CELLS - n))


def tnt(kind: str) -> bytes:
    hs = heights(kind)
    tiles = bytes(CELLS * CELLS // 2)               # (CELLS/2)^2 u16, every one tile 0
    attrs = bytearray(len(hs) * 4)
    attrs[0::4] = hs
    attrs[1::4] = b"\xff" * len(hs)                 # feature 0xFFFF: none
    attrs[2::4] = b"\xff" * len(hs)
    gfx = bytes([TILE_INDEX]) * 1024
    attr = 64 + len(tiles)
    tilegfx = attr + len(attrs)
    anim = tilegfx + len(gfx)
    mini = anim                                     # no feature names
    header = struct.pack("<16i", 0x2000, CELLS, CELLS, 64, attr, tilegfx, 1, 0, anim, SEALEVEL,
                         mini, 1, 0, 0, 0, 0)
    return (header + tiles + bytes(attrs) + gfx + struct.pack("<ii", MINI, MINI)
            + bytes([TILE_INDEX]) * (MINI * MINI))


def ota(kind: str) -> str:
    o = OTA[kind]
    name = map_name(kind)
    starts = "".join(f"\t\t\t[special{i}]\r\n\t\t\t\t{{\r\n\t\t\t\tspecialwhat=StartPos{i + 1};\r\n"
                     f"\t\t\t\tXPos={x};\r\n\t\t\t\tZPos={z};\r\n\t\t\t\t}}\r\n"
                     for i, (x, z) in enumerate(STARTS))
    side = WU // 512
    return ("[GlobalHeader]\r\n\t{\r\n"
            f"\tmissionname={name};\r\n"
            f"\tmissiondescription=Impure calibration, generated ({kind});\r\n"
            f"\tplanet={o['planet']};\r\n"
            f"\tlineofsight={o['lineofsight']};\r\n"
            f"\tmapping={o['mapping']};\r\n"
            "\ttidalstrength=20;\r\n\tsolarstrength=20;\r\n\tlavaworld=0;\r\n"
            f"\tminwindspeed={o['minwindspeed']};\r\n"
            f"\tmaxwindspeed={o['maxwindspeed']};\r\n"
            f"\tgravity={o['gravity']};\r\n"
            "\tnumplayers=2;\r\n"
            f"\tsize={side} x {side};\r\n"
            "\tSCHEMACOUNT=1;\r\n"
            "\t[Schema 0]\r\n\t\t{\r\n"
            "\t\tType=Network 1;\r\n\t\taiprofile=DEFAULT;\r\n"
            "\t\tSurfaceMetal=0;\r\n\t\tMohoMetal=0;\r\n"
            "\t\tHumanMetal=1000;\r\n\t\tComputerMetal=1000;\r\n"
            "\t\tHumanEnergy=1000;\r\n\t\tComputerEnergy=1000;\r\n"
            "\t\tMeteorWeapon=;\r\n\t\tMeteorRadius=0;\r\n\t\tMeteorDensity=0;\r\n"
            "\t\tMeteorDuration=0;\r\n\t\tMeteorInterval=0;\r\n"
            f"\t\t[specials]\r\n\t\t\t{{\r\n{starts}\t\t\t}}\r\n"
            "\t\t}\r\n\t}\r\n")


@functools.lru_cache(maxsize=None)
def build(kind: str) -> tuple:
    """(ufo bytes, ota text, tnt bytes)."""
    _kind(kind)
    t, o = tnt(kind), ota(kind)
    tree = {}
    hpipack.insert(tree, f"maps/{map_name(kind)}.tnt", t)
    hpipack.insert(tree, f"maps/{map_name(kind)}.ota", o.encode("latin-1"))
    return hpipack.build(tree, method=2), o, t


def sha256(kind: str) -> str:
    return hashlib.sha256(build(kind)[0]).hexdigest()


def install(gamedir, kind: str) -> Path:
    """<gamedir>/impure-calib-<kind>.ufo with build(kind)'s bytes: kept when it already holds them,
    else written to a temporary name and renamed into place (the module header: never through a
    link)."""
    ufo = build(kind)[0]
    path = Path(gamedir) / ufo_name(kind)
    if path.is_file() and not path.is_symlink() and path.read_bytes() == ufo:
        return path
    tmp = path.with_name(path.name + ".tmp")
    tmp.unlink(missing_ok=True)
    tmp.write_bytes(ufo)
    os.replace(tmp, path)
    return path


def place(gamedir, kind):
    """The game folder holds `kind`'s map and no other kind's: a lane-launch on another arena of the
    same instance mounts no synthetic map it did not ask for. `kind` None removes both. Returns the
    installed path or None."""
    for other in KINDS:
        if other != kind:
            (Path(gamedir) / ufo_name(other)).unlink(missing_ok=True)
    return install(gamedir, kind) if kind else None


def main(argv=None):
    import argparse
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("install", help="write GAMEDIR/impure-calib-<kind>.ufo")
    i.add_argument("gamedir", type=Path)
    i.add_argument("kind", choices=KINDS)
    s = sub.add_parser("sha256", help="the archive's hash")
    s.add_argument("kind", nargs="?", choices=KINDS)
    a = p.parse_args(argv)
    if a.cmd == "install":
        if not a.gamedir.is_dir():
            p.error(f"no game folder at {a.gamedir}")
        print(install(a.gamedir, a.kind))
    else:
        for kind in [a.kind] if a.kind else KINDS:
            print(f"{sha256(kind)}  {ufo_name(kind)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
