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
| sites | 32 of 6528 x 1664 world units, a 3 x 11 lattice less the one over the corner | 36 of 4864 x 1920, 4 x 9 |
| void | every cell outside the sites and the corner region (THE WALLS) | the same |
| start positions | two, on the corner region's diagonal | two, on the plateau |

The retail values are the scalar settings of each arena's .ota (READ through tools/wiki/tavfs.py from
the install, 2026-11-13); nothing of a retail file is copied, and every byte here is written by
build(). The corner region (0, 0)-(1024, 1024) holds the start positions on both maps, and the arena
configs (calib-arena-packed*.json) name it `keepout`: the packer places no fight there, so the
skirmish's commanders, which the launch's own apply removes, never stand on a fight's ground.

The TNT is the layout tools/resource_fixture.py writes and tools/tascene's Tnt reader checks
(research/notes/file-formats.md §6): the header, a tile map of u16 per 32-wu cell all naming tile 0,
the attributes `<BHB` (height, feature word, 0) per 16-wu cell -- 0xFFFF (none) on an open cell,
0xFFFC (void) on every other --, one 32 x 32 tile of one palette index, no feature names, and a
32 x 32 minimap. The archive is tools/hpipack.py's.

THE WALLS. A packed batch stands each cell in a SITE of its own (tools/tasim/pack.py, `assign`), and
every 16-wu cell outside the sites and the corner region carries the TNT feature word VOID = 0xFFFC,
the engine's void marker. The invariant: a pathfinder search started for a unit of cell k reads
class-map entries only inside site k and on the void cells that border it. It rests on these engine
facts (DISASSEMBLED from pristine/TotalA.exe.pristine, 2026-11-13):
  - a search opens or stands on a cell only over 8-steps through cells of code >= 1: the expansion
    0x40DA70 leaves a neighbour of code 0 dead (`cmp eax,1; jae` at 0x40DC68..0x40DC6F) unless it was
    walked, and the bug walk 0x40E160 steps only onto code >= 1 (0x40E21D..0x40E225, its heads
    0x40E30E..0x40E350 and 0x40E4A9..0x40E4F4); every cell it reads is one of those or an 8-neighbour;
  - a void cell is 0 for every movement class: the per-cell rule 0x47DE60 takes a feature word of
    0xFFFB or above that is not 0xFFFE (set at 0x47DE9E) to a 0 answer (0x47DEEB..0x47DEF5) before it
    reads any class field, unit or height; the fill 0x440500 and the refresh 0x47E1F0 answer 0 for an
    entry whose footprint holds a 0 cell, so with footprints of at least 1 a void cell's entry is 0 in
    every class map after the fill and after every re-mark (pack.py refuses a mobile type of footprint 0);
  - void cells stay void: LoadMap's height loop spawns 0xFFFC on every TNT cell that carries it, on
    every load (0x483ACA..0x483ADE; the spawn writes the word and returns, 0x423C76..0x423C85),
    FEATURES_Destroy refuses a word >= 0xFFFB (0x4246EC), and the border mask stores only over 0xFFFF
    and 0xFFFE;
  - the search's map reader 0x40D7B0 answers 2 (passable) on an unexplored block (0x40D817..0x40D83B),
    and with Mapping on every in-game writer of the explored map main+0x14273 only sets bits
    (0x481A53..0x481A67, 0x481B61..0x481B75, 0x485466..0x48547A) and the refill 0x4816A0 writes 0xFFFF
    while main+0x14281 bit 0 is clear (MEASURED 2026-11-13: every word holds both seats' bits at the
    launch's hold), so that answer never fires;
  - void is invisible to everything else a fight uses: the projectile's feature hit takes a word
    >= 0xFFFB other than 0xFFFE as no feature (0x49B2BD..0x49B2E5), the area damage skips it
    (0x49A480..0x49A485), and the heights under it are the ground's, so collision, sight and the
    sheared LOS rows are those of flat ground; radar, sonar and jamming are exact distances.
Any two sites, and any site and the corner region, are at least GRID apart along x or z, so an 8-step
out of an open region lands on void, whose entry is 0 for good: a search's component of code >= 1
stays inside its site, and the entries it reads are of site k's interior (only cell k's units under
P1) or of constant void. A site of fewer than 65 536 cells keeps the search's node word (0x40DD9A) from
wrapping, since a search takes at most one node a cell. check(kind) asserts the geometry.

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
NONE = 0xFFFF                   # the TNT feature word of a cell with no feature
VOID = 0xFFFC                   # the engine's void marker (THE WALLS)
GRID = 128                      # world units: every site origin a multiple, and every gap at least one
FLAT = (WU - 32, WU - 128)      # the scroll extent: the border mask 0x4833B0 shuts the strips past it
# world units: (x0, z0, pitch x, pitch z, width, height, columns, rows) of each kind's site lattice
LATTICE = {"land": (256, 384, 6656, 1792, 6528, 1664, 3, 11),
           "naval": (128, 1152, 5120, 2048, 4864, 1920, 4, 9)}
NODES = 65536                   # the search's node word (0x40DD9A): a site holds fewer cells
# main+0x14263, the level's gravity in 16.16 px a tick squared, on both maps (MEASURED 2026-11-13 at the
# launch's hold of each; the retail arenas read the same, .data/impai/map-gravity.json)
ENGINE_GRAVITY = 8155
CORNER_BOX = (0, 0, CORNER, CORNER)

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


def _gap(a, b) -> int:
    """The separation of two (x0, z0, x1, z1) boxes along the axis that separates them most."""
    return max(b[0] - a[2], a[0] - b[2], b[1] - a[3], a[1] - b[3])


def _box(site) -> tuple:
    x, z, w, h = site
    return (x, z, x + w, z + h)


def sites(kind: str) -> list:
    """[(x, z, w, h)] in world units, row-major: the kind's lattice less any site within GRID of the
    corner region."""
    _kind(kind)
    x0, z0, px, pz, w, h, cols, rows = LATTICE[kind]
    out = []
    for r in range(rows):
        for c in range(cols):
            s = (x0 + c * px, z0 + r * pz, w, h)
            if _gap(_box(s), CORNER_BOX) >= GRID:
                out.append(s)
    return out


@functools.lru_cache(maxsize=None)
def open_cells(kind: str) -> bytes:
    """One byte a 16-wu cell, row-major (z rows of x): 1 inside a site or the corner region, 0 void."""
    _kind(kind)
    grid = bytearray(CELLS * CELLS)
    for x, z, w, h in sites(kind) + [(0, 0, CORNER, CORNER)]:
        run = bytes([1]) * (w // 16)
        for row in range(z // 16, (z + h) // 16):
            grid[row * CELLS + x // 16:row * CELLS + (x + w) // 16] = run
    return bytes(grid)


def _words(cells: bytes, shift: int) -> bytes:
    """One byte of each cell's feature word: NONE's on an open cell, VOID's on a void one."""
    return cells.translate(bytes([(VOID >> shift) & 0xFF, (NONE >> shift) & 0xFF]) + bytes(254))


def tnt(kind: str) -> bytes:
    hs = heights(kind)
    tiles = bytes(CELLS * CELLS // 2)               # (CELLS/2)^2 u16, every one tile 0
    attrs = bytearray(len(hs) * 4)
    attrs[0::4] = hs
    attrs[1::4] = _words(open_cells(kind), 0)       # the feature word, little-endian
    attrs[2::4] = _words(open_cells(kind), 8)
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


def check(kind: str) -> str:
    """A one-line summary of the kind's walls, or ValueError naming the invariant the map breaks (THE
    WALLS): every site's origin on the grid, inside the scroll extent, at least GRID from every other
    site and from the corner region along x or z, and under NODES cells; the TNT's feature words VOID
    exactly where open_cells is 0; the .ota's meteor shower off by the engine's rule in every schema."""
    import arenascan
    ss = sites(kind)
    for i, s in enumerate(ss):
        x, z, w, h = s
        if x % GRID or z % GRID or w % 16 or h % 16:
            raise ValueError(f"{kind} site {i} {s}: its origin is off the {GRID}-wu grid")
        if x < 0 or z < 0 or x + w > FLAT[0] or z + h > FLAT[1]:
            raise ValueError(f"{kind} site {i} {s}: outside [0, {FLAT[0]}) x [0, {FLAT[1]})")
        if w * h // 256 >= NODES:
            raise ValueError(f"{kind} site {i} {s}: {w * h // 256} cells, the node word holds {NODES}")
        if _gap(_box(s), CORNER_BOX) < GRID:
            raise ValueError(f"{kind} site {i} {s}: within {GRID} of the corner region")
        for j, t in enumerate(ss[i + 1:], i + 1):
            if _gap(_box(s), _box(t)) < GRID:
                raise ValueError(f"{kind} sites {i} and {j}: closer than {GRID} along both axes")
    t = build(kind)[2]
    at = struct.unpack_from("<16i", t)[4]
    attrs = t[at:at + CELLS * CELLS * 4]
    cells = open_cells(kind)
    if attrs[1::4] != _words(cells, 0) or attrs[2::4] != _words(cells, 8):
        raise ValueError(f"{kind}: the TNT's feature words are not VOID exactly off the open cells")
    for name, keys, _subs in arenascan.schemas(build(kind)[1].encode("latin-1")):
        if arenascan.meteors_on(keys):
            raise ValueError(f"{kind}: [{name}] switches the meteor shower on")
    void = len(cells) - cells.count(1)
    return f"{len(ss)} sites of {ss[0][2]} x {ss[0][3]} world units, {void} void cells of {len(cells)}, meteors off"


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
