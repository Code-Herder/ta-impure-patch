#!/usr/bin/env python3
"""sweepart.py -- draw the reclaim sweep's stand-in art (tagpu_sweep.c, tagpu_gui_surf.c's
sharp_sweep) into art/ui/retail/, where tools/artbake.py bakes it into the DLL.

    python3 tools/sweepart.py            # writes art/ui/retail/sweep-*

THE ART IS DRAWN HERE, from rectangles and a 5 x 7 pixel font written out below: no pixel comes
from the game or from a font file. It follows the mockup's look (.data/reclaim-ui-mockup,
reclaim-sweep.js): a dark plate framed black outside and bronze inside, a tongue with a beige
arrow tying it to the RECLAIM row, a dark recessed bar per kind with one pip slot at its right end
and the pip lit green for the kind in force, the labels in beige (brighter under the pointer), and
the cursor's tag in the kind's colour. Owner art replaces it when it exists; escalation and
twilight carry none yet, so they draw retail's (tagpu_art.h).

THE COLOURS ARE TA'S PALETTE'S, each the nearest entry artbake accepts to the colour the mockup
names (artbake's VARIES set excluded, so ENERGY's yellow is not 208), and a pixel is drawn or it is
not: no partial alpha. THE STANDARD LIBRARY ONLY, as artbake.

The sizes are the popup geometry's (tagpu_sweep.c, popup_rect): the plate 124 x 72, the tongue
6 x 18, a bar 112 x 18. A label is a whole row, 112 x 18, transparent but for its text, so it is
drawn over its bar at the bar's rect."""
import json
import struct
import sys
import zlib
from pathlib import Path

TREE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TREE / "tools"))
from artbake import VARIES  # noqa: E402  the indices a mod or the shell changes

PALETTE = TREE / "unditherer/ta-palette.json"
OUT = TREE / "art/ui/retail"

# the mockup's colours (reclaim-sweep.js), and the plate's and bar's shades beside them
WANT = {
    "black": (0, 0, 0),
    "plate": (35, 33, 28),
    "plate_hi": (58, 54, 44),
    "bronze": (90, 76, 44),
    "well": (11, 13, 16),
    "bar": (20, 22, 24),
    "bar_lo": (6, 6, 6),
    "bar_hi": (74, 69, 56),
    "slot": (0, 0, 0),
    "green": (95, 211, 95),
    "green_hi": (156, 240, 156),
    "beige": (232, 220, 160),
    "beige_hi": (255, 244, 184),
    "metal": (211, 219, 255),
    "energy": (255, 255, 0),     # the bar's yellow, held again at 251 outside VARIES
}

FONT = {  # 5 x 7, one string a row, '#' drawn
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    "G": [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "M": ["#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"],
    "N": ["#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"],
    "R": ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "Y": ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
}


def palette_colours():
    """{name: (r, g, b)}: each wanted colour moved to the nearest entry artbake keeps -- one at an
    index outside VARIES, which artbake maps that colour to."""
    pal = [tuple(c[:3]) for c in json.loads(PALETTE.read_text())]
    ok = {c for i, c in enumerate(pal) if i not in VARIES}
    out = {}
    for name, (r, g, b) in WANT.items():
        out[name] = min(ok, key=lambda c: ((c[0] - r) ** 2 + (c[1] - g) ** 2 + (c[2] - b) ** 2, c))
    return out


class Image:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[None] * w for _ in range(h)]

    def fill(self, x0, y0, w, h, c):
        for y in range(max(y0, 0), min(y0 + h, self.h)):
            for x in range(max(x0, 0), min(x0 + w, self.w)):
                self.px[y][x] = c

    def frame(self, x0, y0, w, h, c):
        self.fill(x0, y0, w, 1, c)
        self.fill(x0, y0 + h - 1, w, 1, c)
        self.fill(x0, y0, 1, h, c)
        self.fill(x0 + w - 1, y0, 1, h, c)

    def text(self, x, y, s, c, outline=None):
        """`s` with its top-left at (x, y), a 1-px gap between glyphs; with `outline`, every
        empty pixel beside a drawn one in that colour first."""
        cells = []
        for ch in s:
            for gy, row in enumerate(FONT[ch]):
                for gx, bit in enumerate(row):
                    if bit == "#":
                        cells.append((x + gx, y + gy))
            x += 6
        if outline:
            for cx, cy in cells:
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        if 0 <= cx + dx < self.w and 0 <= cy + dy < self.h:
                            self.px[cy + dy][cx + dx] = outline
        for cx, cy in cells:
            self.px[cy][cx] = c

    def png(self, path):
        raw = bytearray()
        for row in self.px:
            raw.append(0)
            for c in row:
                raw += bytes((0, 0, 0, 0)) if c is None else bytes((*c, 255))

        def chunk(kind, body):
            return (struct.pack(">I", len(body)) + kind + body +
                    struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))
        data = (b"\x89PNG\r\n\x1a\n" +
                chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def text_width(s):
    return 6 * len(s) - 1


def plate(P):
    im = Image(124, 72)
    im.fill(0, 0, 124, 72, P["plate"])
    im.fill(1, 1, 122, 1, P["plate_hi"])
    im.frame(0, 0, 124, 72, P["black"])
    im.frame(2, 2, 120, 68, P["bronze"])
    return im


def tongue(P):
    im = Image(6, 18)
    im.fill(0, 0, 6, 18, P["bronze"])
    im.fill(1, 1, 5, 16, P["well"])
    for i in range(4):              # the arrow, pointing at the button: (2, 9) to (5, 5..13)
        im.fill(2 + i, 9 - i, 1, 2 * i + 1, P["beige"])
    return im


def bar(P, lit):
    im = Image(112, 18)
    im.fill(0, 0, 112, 18, P["bar"])
    im.fill(0, 0, 112, 1, P["bar_lo"])      # recessed: dark above and left, light below and right
    im.fill(0, 0, 1, 18, P["bar_lo"])
    im.fill(0, 17, 112, 1, P["bar_hi"])
    im.fill(111, 0, 1, 18, P["bar_hi"])
    im.fill(102, 2, 8, 14, P["slot"])        # the one pip slot at the right end
    im.fill(102, 15, 8, 1, P["bar_hi"])
    if lit:
        im.fill(104, 3, 4, 11, P["green"])
        im.fill(105, 5, 2, 6, P["green_hi"])
    return im


def label(P, s, hot):
    im = Image(112, 18)
    im.text(6, 6, s, P["beige_hi" if hot else "beige"])
    return im


def tag(P, s, colour):
    im = Image(text_width(s) + 2, 9)
    im.text(1, 1, s, P[colour], outline=P["black"])
    return im


def main():
    P = palette_colours()
    plate(P).png(OUT / "sweep-plate.png")
    tongue(P).png(OUT / "sweep-tongue.png")
    bar(P, False).png(OUT / "sweep-bar/0-off.png")
    bar(P, True).png(OUT / "sweep-bar/1-on.png")
    for i, name in enumerate(("METAL", "ENERGY", "ALL")):
        label(P, name, False).png(OUT / f"sweep-label/{2 * i}-{name.lower()}.png")
        label(P, name, True).png(OUT / f"sweep-label/{2 * i + 1}-{name.lower()}-hot.png")
    for i, (s, colour) in enumerate((("M", "metal"), ("E", "energy"), ("ALL", "green"))):
        tag(P, s, colour).png(OUT / f"sweep-tag/{i}-{s.lower()}.png")
    print(f"sweepart: wrote {OUT.relative_to(TREE)}/sweep-*")


if __name__ == "__main__":
    main()
