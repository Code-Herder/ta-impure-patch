#!/usr/bin/env python3
"""genparts.py -- the Mods screen's parts in the wiki's kit, generated from the kit's own art.

    python3 tools/mods/genparts.py

Writes art/wiki/kit/parts/<name>.png and each part's entry in art/wiki/kit/kit.json: the checkbox, the radio, the
progress bar's track and fill, the three tabs and the four badges of tagpu_modslay.c. Nothing here is painted: each part
is resampled and tinted from `button`, `button_down`, `row`, `badge` and `i_dot`, so it carries their bevels and wear, and
a run on unchanged inputs rewrites the same bytes. A painted part replaces a generated one with no code change: its PNG
and its entry under the same name,.

A new part is appended to kit.json, so every part already there keeps its index.

tools/hudart/fit.py --set wiki rewrites art/wiki/kit/ from art/wiki-full/kit/ and removes every part that folder lacks,
these among them: run this after it. tagpu_modslay_load refuses a kit without them, by name.
"""
import json
from pathlib import Path
from PIL import Image, ImageDraw

KIT = Path(__file__).resolve().parents[2] / "art" / "wiki" / "kit"
SS = 4              # a drawn shape is drawn this many times over and reduced, for its edge
BOX = 40            # a checkbox's and a radio's side, in texels: 12 units at the kit's 3.333 texels a unit
SCALE = 3.333
BADGE_TINT = {"badge_active": (70, 176, 74), "badge_update": (226, 160, 44), "badge_grey": (128, 128, 122),
              "badge_conflict": (232, 40, 28)}
FILL_TINT = (118, 214, 96)


def part(name):
    return Image.open(KIT / "parts" / f"{name}.png").convert("RGBA")


def nine(im, sl, w, h):
    """`im` drawn at w x h by its slices (t r b l, in texels): the corners kept, the edges and the middle stretched."""
    t, r, b, l = sl
    sx, sy = (0, l, im.width - r, im.width), (0, t, im.height - b, im.height)
    dx, dy = (0, l, w - r, w), (0, t, h - b, h)
    out = Image.new("RGBA", (w, h))
    for j in range(3):
        for i in range(3):
            if dx[i + 1] > dx[i] and dy[j + 1] > dy[j]:
                cell = im.crop((sx[i], sy[j], sx[i + 1], sy[j + 1])).resize((dx[i + 1] - dx[i], dy[j + 1] - dy[j]), Image.LANCZOS)
                out.paste(cell, (dx[i], dy[j]))
    return out


def tinted(im, rgb, k=1.0):
    """`im` multiplied by `rgb`, its alpha kept: its grain stays and its colour is the tint's, `k` of the way there."""
    def px(p):
        return tuple(int(p[i] * (1 - k + k * rgb[i] / 255) + 0.5) for i in range(3)) + (p[3],)
    out = Image.new("RGBA", im.size)
    out.putdata([px(p) for p in im.getdata()])
    return out


def rim_recoloured(im, rgb):
    """The badge with its red rim in `rgb`: how red a pixel is says how much of the tint it takes; the dark well, the worn
    metal and the alpha stay as painted."""
    def px(p):
        r, g, b, a = p
        k = min(1.0, max(0.0, (r - max(g, b)) / max(1, r)) * 1.5)
        lum = r / 255 * 1.1
        return tuple(min(255, int(p[i] * (1 - k) + rgb[i] * min(1.0, lum) * k + 0.5)) for i in range(3)) + (a,)
    out = Image.new("RGBA", im.size)
    out.putdata([px(p) for p in im.getdata()])
    return out


def brightest(im):
    """The lightest opaque colour of `im`: a mark in the tree's own ink."""
    return max((p for p in im.getdata() if p[3] > 200), key=lambda p: p[0] + p[1] + p[2])[:3]


def face(im, frac):
    """The colour down the middle of `im` at `frac` of its height."""
    return im.getpixel((im.width // 2, int(im.height * frac)))[:3]


def shape(draw):
    """A BOX x BOX layer drawn by `draw(g, S)` at S times the size, reduced."""
    big = Image.new("RGBA", (BOX * SS, BOX * SS), (0, 0, 0, 0))
    draw(ImageDraw.Draw(big), SS)
    return big.resize((BOX, BOX), Image.LANCZOS)


def over(a, b):
    out = a.copy()
    out.alpha_composite(b)
    return out


def radio(well, top, bottom):
    """A sunk disc: a rim shaded from the button's top face down to its bottom bevel, the well inside it."""
    def rim(y):
        f = y / (BOX * SS)
        return tuple(int(top[i] * (1 - f) + bottom[i] * f) for i in range(3)) + (255,)
    big = Image.new("RGBA", (BOX * SS, BOX * SS), (0, 0, 0, 0))
    g = ImageDraw.Draw(big)
    e = 2 * SS
    n = BOX * SS - 2 * e
    mask = Image.new("L", big.size, 0)
    ImageDraw.Draw(mask).ellipse((e, e, e + n, e + n), fill=255)
    for y in range(big.height):
        g.line((0, y, big.width, y), fill=rim(y))
    big.putalpha(mask)
    w = 6 * SS
    ImageDraw.Draw(big).ellipse((w, w, big.width - w, big.height - w), fill=well + (255,))
    return big.resize((BOX, BOX), Image.LANCZOS)


def build():
    button, down, row, badge, dot = part("button"), part("button_down"), part("row"), part("badge"), part("i_dot")
    well, ink, rim_top, rim_bot = face(down, 0.5), brightest(dot), face(button, 0.3), face(button, 0.95)
    box = nine(down, (6, 6, 4, 6), BOX, BOX)
    tick = shape(lambda g, S: g.line([(10 * S, 21 * S), (17 * S, 29 * S), (31 * S, 11 * S)], fill=ink + (255,), width=5 * S, joint="curve"))
    disc = radio(well, rim_top, rim_bot)
    pip = shape(lambda g, S: g.ellipse((13 * S, 13 * S, (BOX - 13) * S, (BOX - 13) * S), fill=ink + (255,)))
    whole = {"kind": "whole", "scale": SCALE}
    TAB, BAR = (5, 16, 5, 16), (3, 8, 3, 8)
    tab = {"kind": "nine", "scale": SCALE, "slice": list(TAB)}
    out = {
        "check_off": (box, whole), "check_on": (over(box, tick), whole),
        "radio_off": (disc, whole), "radio_on": (over(disc, pip), whole),
        "progress_track": (nine(down, (4, 8, 3, 8), 120, 27), {"kind": "nine", "scale": SCALE, "slice": [4, 8, 3, 8]}),
        "progress_fill": (tinted(nine(button, BAR, 120, 17), FILL_TINT), {"kind": "nine", "scale": SCALE, "slice": list(BAR)}),
        "tab": (nine(row, TAB, 200, 60), tab),
        "tab_selected": (nine(button, TAB, 200, 60), tab),
        "tab_down": (nine(down, TAB, 200, 60), tab),
    }
    sb = {"kind": "nine", "scale": 5.022, "slice": [5.423, 52, 5.915, 52], "xtile": True}
    for name, rgb in BADGE_TINT.items():
        out[name] = (rim_recoloured(badge, rgb), sb)
    return out


def main():
    parts = build()
    path = KIT / "kit.json"
    K = json.loads(path.read_text())
    for name, (im, entry) in parts.items():
        im.save(KIT / "parts" / f"{name}.png", optimize=True, compress_level=9)
        K["parts"][name] = entry
    path.write_text(json.dumps(K, indent=1))
    print(f"genparts: {len(parts)} parts in {KIT.relative_to(KIT.parents[2])}, {len(K['parts'])} in the kit")


if __name__ == "__main__":
    main()
