#!/usr/bin/env python3
"""artcheck.py — prove that art/ui/ is drawn, not copied from the game.

    .venv-undither/bin/python tools/artcheck.py [--game DIR]... [art/ui/<style> ...]

Run it before committing new art (research/notes/tadr-port/demo-recorder-s5-ui.md,
*Drawing it*). Each PNG under art/ui/ is mapped to palette indices the way the DLL's
bake maps it (tools/artbake.py), and every horizontal run of RUN indices that holds at
least DISTINCT different values is looked up among the runs of the game's own UI art:
every PCX under bitmaps/ and every frame of every GAF under anims/, of each game
directory given (default: tagpu/gamedir in the main checkout, retail). A mod's own art
is checked by giving its directory too, for example a compatibility setup's.

Flat or two-tone runs are not counted: a bevel line of one colour is a coincidence any
painter makes. A run of RUN pixels with DISTINCT or more colours in the same order is not.
Exit 0 when nothing matches, 1 with each match listed.

The game's files are read here and never written anywhere: this tool reports
coordinates, not pixels."""
import argparse
import io
import struct
import sys
from importlib.machinery import SourceFileLoader
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import artbake  # noqa: E402

RUN, DISTINCT = 12, 3


def load_ta3do():
    return SourceFileLoader("ta3do", str(HERE / "ta3do")).load_module()


def runs(pixels, w, h, key=None):
    """Every qualifying run of a w*h index plane, as bytes -> (x, y)."""
    out = {}
    for y in range(h):
        row = pixels[y * w:(y + 1) * w]
        for x in range(0, w - RUN + 1):
            r = row[x:x + RUN]
            if key is not None and key in r:
                continue
            if len(set(r)) >= DISTINCT:
                out.setdefault(bytes(r), (x, y))
    return out


def game_runs(ta3do, root):
    """(runs, files read) of a game directory's UI art, in palette indices."""
    assets = ta3do.Assets(root)
    found, n = {}, 0
    for name in assets.glob("bitmaps/*.pcx"):
        try:
            im = Image.open(io.BytesIO(assets.read(name)))
        except Exception:
            continue
        if im.mode != "P":
            continue
        for k in runs(im.tobytes(), im.width, im.height):
            found.setdefault(k, name)
        n += 1
    for name in assets.glob("anims/*.gaf"):
        blob = assets.read(name)
        try:
            sig, count, _ = struct.unpack_from("<III", blob, 0)
            if sig != 0x00010100:
                continue
            for off in struct.unpack_from(f"<{count}I", blob, 12):
                frames = struct.unpack_from("<H", blob, off)[0]
                for f in range(frames):
                    fo = struct.unpack_from("<I", blob, off + 40 + 8 * f)[0]
                    fr = ta3do.gaf_frame(blob, fo)
                    for k in runs(fr.pixels, fr.width, fr.height, fr.transparent):
                        found.setdefault(k, name)
        except (struct.error, ta3do.GafError, IndexError):
            continue
        n += 1
    return found, n


def art_planes(paths, cmap):
    for root in paths:
        for png in sorted(Path(root).rglob("*.png")):
            w, h, rows = artbake.read_png(png)
            plane = bytearray()
            for row in rows:
                for (r, g, b, a) in row:
                    plane.append(cmap.get((r, g, b), 0) if a else 256 - 1)
            yield png, w, h, plane


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("art", nargs="*", default=[str(HERE.parent / "art/ui")])
    ap.add_argument("--game", action="append", default=[])
    a = ap.parse_args()
    ta3do = load_ta3do()
    games = [Path(g) for g in a.game] or [ta3do.game_dir()]
    cmap, _ = artbake.palette_map(HERE.parent / "unditherer/ta-palette.json")
    ref = {}
    for g in games:
        found, n = game_runs(ta3do, g)
        print(f"artcheck: {n} files of {g.name}, {len(found)} runs")
        for k, v in found.items():
            ref.setdefault(k, f"{g.name}:{v}")
    hits = 0
    for png, w, h, plane in art_planes(a.art, cmap):
        for k, (x, y) in runs(plane, w, h, 255).items():
            if k in ref:
                hits += 1
                if hits <= 40:
                    print(f"MATCH {png} at ({x},{y}): a {RUN}-pixel run of {ref[k]}")
    print(f"artcheck: {'FAILED, ' + str(hits) + ' matching runs' if hits else 'clean'}")
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
