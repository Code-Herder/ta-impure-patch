#!/usr/bin/env python3
"""fonts.py -- the replay HUD's faces, as art/hud/fonts/ holds them.

    $PY tools/hudart/fonts.py <Oswald[wght].ttf> <BarlowCondensed-SemiBold.ttf>

The inputs are the Google Fonts releases (github.com/google/fonts, ofl/oswald and
ofl/barlowcondensed). Oswald is a variable font; the HUD draws it at weights 500 and 700, so
each is instanced to a static face, the outlines the browser draws at those weights. Every face
is then cut down to the characters the HUD can draw -- ASCII, Latin-1 (players' names) and the
four signs its strings use, U+00B7, U+00D7, U+03A3, U+2212 -- keeping its kerning and its names.
The hinting goes: the DLL's rasteriser (stb_truetype) does not read it.

Needs fontTools, which the shared venv carries. The OFL asks for each family's copyright notice
and the licence beside the faces: art/hud/fonts/OFL-*.txt, copied from the same releases.
"""
import sys
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

OUT = Path(__file__).resolve().parents[2] / "art" / "hud" / "fonts"
TEXT = "".join(map(chr, range(0x20, 0x7F))) + "".join(map(chr, range(0xA0, 0x100))) + "·×Σ−"


def cut(font, out):
    o = subset.Options()
    o.layout_features = ["kern"]
    o.name_IDs = ["*"]
    o.name_languages = ["*"]
    o.notdef_outline = True
    o.hinting = False
    s = subset.Subsetter(o)
    s.populate(text=TEXT)
    s.subset(font)
    font.save(out)
    print(f"{out.name}: {out.stat().st_size} bytes, {len(font.getGlyphOrder())} glyphs")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    OUT.mkdir(parents=True, exist_ok=True)
    for w in (500, 700):
        cut(instancer.instantiateVariableFont(TTFont(sys.argv[1]), {"wght": w}), OUT / f"oswald-{w}.ttf")
    cut(TTFont(sys.argv[2]), OUT / "barlowcondensed-600.ttf")


if __name__ == "__main__":
    main()
