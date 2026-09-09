#!/usr/bin/env python3
"""guifont.py — set a string in the engine's own GUI font.

TWO FONT FORMATS, and the engine uses both.

`anims/hattfont11.gaf` / `hattfont12.gaf` are GAF fonts: one entry, 256 frames,
one per character code, full palette colour with a per-frame (xpos, ypos)
hotspot whose `ypos` IS THE BASELINE. This is what the gadget dispatcher draws
every label and caption with (loaded at init through 0x4AEDD0, glyphs read with
GetGlyph 0x4B7F30).

`fonts/*.fnt` are the other twenty faces -- briefings, the console, the button
legends, Courier and a script face. Format, decoded here 2026-09-09:

    u16 height          the line box, every glyph the same
    u16 flags           1..3; tracks with weight, purpose unclear
    u16 offset[256]     0 = no glyph, else absolute file offset
    per glyph:  u8 width, then ceil(width*height/8) bytes, MSB first,
                row-major, ONE BIT PER PIXEL

They carry no baseline and no colour -- descenders are baked into the fixed
box, and the engine colours them from the gadget's `colorf`. So they are set
here in one ink with an optional dropped shadow, which is how TA draws them.

The lab draws captions as images because a browser has no such face; the game
will not -- there the caption is an ordinary `id=5` label and the engine sets
it. So this exists to make the lab honest, and to answer "how wide is that
string" before a rect is chosen.

    .venv-undither/bin/python tools/guifont.py --list
    .venv-undither/bin/python tools/guifont.py "Rendering Options"
    .venv-undither/bin/python tools/guifont.py "Done" --face hatt14 --out x.png
    .venv-undither/bin/python tools/guifont.py "Rendering Options" --specimen

`--face` takes `gaf11`/`gaf12` (the GAF fonts, the default) or any `.fnt` stem.
`--specimen` sets the string in every face at once, for choosing one.

Prints the pixel width, which is the number a gadget rect has to fit.

THE GLYPHS ARE THE ORIGINAL GAME'S. Output goes to the gitignored tmp/ like
everything else the lab eats.
"""

import argparse
import importlib.util
import struct
from importlib.machinery import SourceFileLoader
from pathlib import Path

from PIL import Image, ImageDraw

TOOLS = Path(__file__).resolve().parent


def load_ta3do():
    loader = SourceFileLoader("ta3do", str(TOOLS / "ta3do"))
    spec = importlib.util.spec_from_loader("ta3do", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


class Font:
    """The 256 frames of a hattfont GAF, indexed by character code.

    THE BASELINE IS IN THE FRAME HEADER. Every GAF frame carries a signed
    (xpos, ypos) hotspot that `ta3do.gaf_frame` reads and discards; for a font
    those are the glyph's bearing and its BASELINE -- `ypos` is the row, counted
    from the glyph's own top, that sits on the baseline. Measured on
    hattfont12: 'A' h=12 ypos=11 (bottom row on the baseline), 'x' h=10 ypos=9
    (same), 'p'/'y'/'g'/'q' h=12 ypos=9 (two rows BELOW it -- the descender),
    'j' h=14 ypos=11 xpos=1, and "'" h=6 ypos=11, which places it six rows
    above the baseline where an apostrophe belongs.

    Bottom-aligning the frames instead -- which is what this did first -- puts
    every descender back on the baseline and lifts the apostrophe onto it.
    """

    def __init__(self, assets, ta3do, size=12):
        blob = assets.read(f"anims/hattfont{size}.gaf")
        sig, count, _ = struct.unpack_from("<III", blob, 0)
        if sig != 0x00010100 or count < 1:
            raise SystemExit("not a GAF")
        off = struct.unpack_from("<I", blob, 12)[0]
        n = struct.unpack_from("<H", blob, off)[0]
        self.name = blob[off + 8:off + 40].split(b"\0")[0].decode("latin-1")
        self.glyphs, self.metrics = {}, {}
        for i in range(n):
            fo = struct.unpack_from("<I", blob, off + 40 + i * 8)[0]
            try:
                self.glyphs[i] = ta3do.gaf_frame(blob, fo)
            except Exception:
                continue
            # width, height, xpos, ypos -- the two the frame reader drops
            _w, _h, xp, yp = struct.unpack_from("<HHhh", blob, fo)
            self.metrics[i] = (xp, yp)
        # Over PRINTABLE ASCII only. The 256 frames include unused slots with
        # junk metrics -- frame 127 reads ypos=121 xpos=54 on hattfont12 -- and
        # taking the max over all of them makes every line 25 px tall. 32..126
        # gives ascent 13, descent 3, so the line box is 17.
        pr = [i for i in self.glyphs if 32 <= i <= 126]
        self.ascent = max((self.metrics[i][1] for i in pr), default=0)
        self.descent = max((self.glyphs[i].height - 1 - self.metrics[i][1]
                            for i in pr), default=0)
        self.height = self.ascent + 1 + self.descent

    def width(self, text, tracking=0):
        """Pen advance, spaces included -- frame 32's width is the space."""
        return sum(self.glyphs[ord(c)].width + tracking
                   for c in text if ord(c) in self.glyphs)

    def render(self, text, flat, tracking=0, key=None):
        """An indexed image of `text` on a single baseline."""
        w = max(1, self.width(text, tracking))
        im = Image.new("P", (w, self.height), key if key is not None else 0)
        im.putpalette(flat)
        x = 0
        for c in text:
            code = ord(c)
            g = self.glyphs.get(code)
            if g is None:
                continue
            # frame 32 holds a placeholder box, not a blank -- the engine
            # advances the pen without drawing it, and so do we
            if c == " ":
                x += g.width + tracking
                continue
            gi = Image.new("P", (g.width, g.height))
            gi.putdata(bytes(g.pixels))
            gi.putpalette(flat)
            mask = None
            if g.transparent is not None:
                mask = gi.point(lambda v, t=g.transparent: 0 if v == t else 255, "L")
            xp, yp = self.metrics[code]
            im.paste(gi, (x + xp, self.ascent - yp), mask)
            x += g.width + tracking
        if key is not None:
            im.info["transparency"] = key
        return im


class FntFont:
    """A `fonts/*.fnt` bitmap face: fixed box, 1 bit per pixel, no baseline."""

    def __init__(self, assets, stem):
        d = assets.read(f"fonts/{stem}.fnt")
        self.name = stem
        self.height, self.flags = struct.unpack_from("<HH", d, 0)
        self.ascent = self.height - 1
        self.descent = 0
        table = struct.unpack_from("<256H", d, 4)
        self.glyphs = {}
        for code, off in enumerate(table):
            if not 0 < off < len(d):
                continue
            w = d[off]
            need = (w * self.height + 7) // 8
            bits = d[off + 1:off + 1 + need]
            if len(bits) < need:
                continue
            rows, i = [], 0
            for _ in range(self.height):
                row = []
                for _ in range(w):
                    row.append((bits[i >> 3] >> (7 - (i & 7))) & 1)
                    i += 1
                rows.append(row)
            self.glyphs[code] = (w, rows)

    def width(self, text, tracking=0):
        return sum(self.glyphs[ord(c)][0] + tracking
                   for c in text if ord(c) in self.glyphs)

    def render(self, text, flat, tracking=0, key=None, ink=255, shadow=None):
        w = max(1, self.width(text, tracking))
        im = Image.new("P", (w, self.height), key if key is not None else 0)
        im.putpalette(flat)
        px = im.load()
        x = 0
        for c in text:
            g = self.glyphs.get(ord(c))
            if g is None:
                continue
            gw, rows = g
            if c != " ":
                for dx, dy, col in (((1, 1, shadow) if shadow is not None else (0, 0, ink)),
                                    (0, 0, ink)):
                    for ry, row in enumerate(rows):
                        for rx, on in enumerate(row):
                            if on and 0 <= x + rx + dx < w and 0 <= ry + dy < self.height:
                                px[x + rx + dx, ry + dy] = col
            x += gw + tracking
        if key is not None:
            im.info["transparency"] = key
        return im


def faces(assets):
    """Every face the engine carries: the two GAF ones, then the .fnt files."""
    out = ["gaf12", "gaf11"]
    out += sorted(Path(p).stem for p in assets.glob("fonts/*.fnt"))
    return out


def open_face(assets, ta3do, spec):
    if spec in ("gaf11", "gaf12"):
        return Font(assets, ta3do, 11 if spec == "gaf11" else 12)
    return FntFont(assets, spec)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("text", nargs="?", default="Rendering Options")
    ap.add_argument("--face", default="gaf12", help="gaf11|gaf12|<.fnt stem>")
    ap.add_argument("--list", action="store_true", help="every face, with metrics")
    ap.add_argument("--specimen", action="store_true",
                    help="set the string in every face, one sheet")
    ap.add_argument("--tracking", type=int, default=0)
    ap.add_argument("--scale", type=int, default=1)
    ap.add_argument("--ink", type=int, default=255, help=".fnt ink palette index")
    ap.add_argument("--shadow", type=int, default=None, help=".fnt shadow index")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()

    ta3do = load_ta3do()
    assets = ta3do.Assets()
    palette = ta3do.load_palette(assets)
    flat = []
    for c in palette[:256]:
        flat += list(c)[:3]
    flat += [0] * (768 - len(flat))

    def make(spec):
        f = open_face(assets, ta3do, spec)
        if isinstance(f, FntFont):
            return f, f.render(a.text, flat, a.tracking, key=0,
                               ink=a.ink, shadow=a.shadow)
        return f, f.render(a.text, flat, a.tracking, key=0)

    if a.list or a.specimen:
        rows = []
        for spec in faces(assets):
            try:
                f, im = make(spec)
            except Exception as exc:
                print(f"  {spec:16s} FAILED: {exc}")
                continue
            kind = "GAF" if isinstance(f, Font) else "fnt"
            print(f"  {spec:16s} {kind}  box {f.height:>2}  "
                  f"glyphs {len(f.glyphs):>3}  width {im.width:>4}")
            rows.append((spec, im))
        if a.specimen and rows:
            s_ = max(1, a.scale)
            pad, lab = 8, 96
            W = lab + max(i.width for _, i in rows) * s_ + pad * 2
            H = pad + sum(i.height * s_ + pad for _, i in rows)
            sheet = Image.new("RGB", (W, H), (17, 14, 10))
            dr = ImageDraw.Draw(sheet)
            y = pad
            for spec, im in rows:
                big = im.convert("RGB").resize((im.width * s_, im.height * s_),
                                               Image.NEAREST)
                dr.text((pad, y + big.height // 2 - 4), spec, fill=(201, 188, 134))
                sheet.paste(big, (lab, y))
                y += big.height + pad
            dest = Path(a.out) if a.out else Path("tmp/gafui") / "specimen.png"
            dest.parent.mkdir(parents=True, exist_ok=True)
            sheet.save(dest)
            print(f"\n-> {dest}")
        return

    f, im = make(a.face)
    if a.scale > 1:
        im = im.resize((im.width * a.scale, im.height * a.scale), Image.NEAREST)
    dest = Path(a.out) if a.out else Path("tmp/gafui") / "caption.png"
    dest.parent.mkdir(parents=True, exist_ok=True)
    im.save(dest)
    print(f'{f.name}  h={f.height}  "{a.text}"  {im.width}x{im.height} -> {dest}')


if __name__ == "__main__":
    main()
