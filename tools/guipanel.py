#!/usr/bin/env python3
"""guipanel.py — the render-options drop-down: its frame, and its trigger.

TWO WAYS TO GET ONE, and `--nine` is the one that should ship.

G18c: the screen needs a panel background no stock frame can provide -- none
of the runtime frames has seven `h20` recesses, and none puts a label beside
its control instead of above it. `guiart.py` splices one out of `visualsrt`'s
pixels so the old vertical layout could be looked at; that output is a
derivative of the game's art and can never ship.

    .venv-undither/bin/python tools/guipanel.py --nine     # the engine's kit
    .venv-undither/bin/python tools/guipanel.py            # drawn from nothing
    .venv-undither/bin/python tools/guipanel.py --trigger  # what OPENS it

`--nine` COMPOSES the ground from `anims/frontend.gaf`'s own nine-slice. The
owner chose `back` (the shell's mottled 64x64 panelling) over `dia` and the
`hybrid` of the two, so that is the default; the dialog kit is
`diaul/diau/diaur · dial/diatile/diar · diall/diabottom/dialr`, 32x32 each --
which is what TA's floating dialogs (MSGBOX, YESORNO, EXITMENU) are made of:
they declare no `id=12` at all and the engine composes their ground the same
way (tools/guisurvey.py measured this 2026-09-09). The recesses come from the
`text16` three-slice, which IS a sunken well. So the drop-down can be TA's own
dialog, at our size, with no art invented and none shipped -- the DLL composes
it at runtime from the player's own install and appends the result to the
gadget GAF blob, where an ordinary `id=12` names it.

Without `--nine` it draws the frame from nothing, which needs no install and
is the fallback if composing turns out to be unavailable at the moment the
screen is built.

Either way it writes the drop-down frame of renderers.md §2.10 at 304x240 --
`renderdd-nine-0.png` composed, `renderdd-0.png` drawn -- and prints the recess
table the lab and the `.GUI` both lay out to.

`--trigger` writes the four frames of the frameless sprocket that opens the
drop-down. It shares nothing with the panel and needs no install: the mask is
generated and the ink is nine palette indices. See `# the trigger` below for
what was measured off the Cavedog logo and off the real top bar.

Both land in a gitignored directory. The drawn one would be ours to ship; the
composed one is the game's pixels and is not, which is exactly why the DLL
composes rather than carries it (gui-gadgets.md §10.1, and the one-common-GAF
finding of 2026-09-09: there is a single gadget GAF at `gi+0x04`, so a new
frame is appended to that blob in memory rather than loaded from a file).
"""

import argparse
import importlib.util
import math
import struct
import sys
from importlib.machinery import SourceFileLoader
from pathlib import Path

from PIL import Image, ImageDraw

TOOLS = Path(__file__).resolve().parent

# ---------------------------------------------------------------- geometry --
# Panel id=0 at (704, 32), right-aligned 16 px in, hanging from the top bar's
# underside. Panel-local coordinates throughout.
#
# NO APPLY BUTTON -- decided with the owner 2026-09-09. A stage button IS the
# setting; there is no edit buffer for an Apply to commit, so a row writes its
# key the moment it is clicked and the panel is dismissed by the trigger or by
# clicking away, the way a drop-down is. That is not only a visual choice: it
# means `OnCommand` writes the cfg per row and there is no eighth gadget to
# gather state from. The panel loses the 40 px the action row occupied.
W, H = 304, 240
ROWS = 7
ROW_Y0, ROW_PITCH, ROW_H = 34, 28, 20
CTL_X, CTL_W = 166, 120          # stagebuttnN's own frame size -- fixed
PAD = 2                          # the reveal of recess around a plate
DIV_TOP, DIV_BOT = 30, 230       # the caption rule, and the panel's own floor

# ----------------------------------------------------------------- palette --
# Ours. An olive-tinted gunmetal: warm enough to sit under the cream lettering,
# dark enough that a lit green stage bar is the brightest thing on the panel.
EDGE = (12, 10, 7)               # the outer keyline
BEVEL_HI = (122, 106, 78)        # top-left of the outer bevel
BEVEL_LO = (58, 49, 35)          # bottom-right of it
GROUND_HI = (74, 65, 48)         # panel face, lit end of the vertical ramp
GROUND_LO = (46, 40, 29)         # ...and its shaded end
STREAK = (0, 0, 0)               # brushed grain, laid on at low alpha
RECESS_HI = (36, 31, 22)         # recess floor, top
RECESS_LO = (23, 20, 14)         # recess floor, bottom
RECESS_DARK = (10, 9, 6)         # its sunken top/left lip
RECESS_LIGHT = (104, 91, 67)     # its lit bottom/right lip
BOLT_HI = (168, 152, 118)
BOLT_LO = (52, 45, 33)


def vgrad(dr, box, top, bot):
    """A vertical ramp -- the face light every TA panel has."""
    x0, y0, x1, y1 = box
    span = max(1, y1 - y0)
    for y in range(y0, y1):
        t = (y - y0) / span
        dr.line([(x0, y), (x1 - 1, y)],
                fill=tuple(round(a + (b - a) * t) for a, b in zip(top, bot)))


def recess(dr, x, y, w, h):
    """A sunken band: dark lip above and left, lit lip below and right."""
    vgrad(dr, (x + 1, y + 1, x + w - 1, y + h - 1), RECESS_HI, RECESS_LO)
    dr.line([(x, y), (x + w - 1, y)], fill=RECESS_DARK)
    dr.line([(x, y), (x, y + h - 1)], fill=RECESS_DARK)
    dr.line([(x, y + h - 1), (x + w - 1, y + h - 1)], fill=RECESS_LIGHT)
    dr.line([(x + w - 1, y), (x + w - 1, y + h - 1)], fill=RECESS_LIGHT)


def bolt(dr, cx, cy):
    """A domed rivet: the detail that says 'this is a plate bolted down'."""
    dr.ellipse([cx - 3, cy - 3, cx + 3, cy + 3], fill=BOLT_LO)
    dr.ellipse([cx - 2, cy - 2, cx + 1, cy + 1], fill=BOLT_HI)


def divider(dr, y):
    """A scored line across the face -- the rule between title, rows, action."""
    dr.line([(10, y), (W - 11, y)], fill=RECESS_DARK)
    dr.line([(10, y + 1), (W - 11, y + 1)], fill=BEVEL_HI)


def draw_panel():
    im = Image.new("RGB", (W, H), GROUND_LO)
    dr = ImageDraw.Draw(im)

    # the face, then the brushed grain over it
    vgrad(dr, (0, 0, W, H), GROUND_HI, GROUND_LO)
    grain = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gd = ImageDraw.Draw(grain)
    for x in range(0, W, 3):
        gd.line([(x, 0), (x, H)], fill=(*STREAK, 26))
    im = Image.alpha_composite(im.convert("RGBA"), grain).convert("RGB")
    dr = ImageDraw.Draw(im)

    # the outer frame: a black keyline, then a bevel lit from the top left
    dr.rectangle([0, 0, W - 1, H - 1], outline=EDGE)
    dr.rectangle([1, 1, W - 2, H - 2], outline=EDGE)
    dr.line([(2, 2), (W - 3, 2)], fill=BEVEL_HI)
    dr.line([(2, 2), (2, H - 3)], fill=BEVEL_HI)
    dr.line([(3, H - 3), (W - 3, H - 3)], fill=BEVEL_LO)
    dr.line([(W - 3, 3), (W - 3, H - 3)], fill=BEVEL_LO)

    for cx, cy in ((9, 9), (W - 10, 9), (9, H - 10), (W - 10, H - 10)):
        bolt(dr, cx, cy)

    divider(dr, DIV_TOP)
    divider(dr, DIV_BOT)

    rec = []
    for n in range(ROWS):
        y = ROW_Y0 + ROW_PITCH * n
        recess(dr, CTL_X - PAD, y - PAD, CTL_W + 2 * PAD, ROW_H + 2 * PAD)
        rec.append((y, ROW_H))
    return im, rec


# ------------------------------------------------------- the engine's kit --
# MEASURED 2026-09-09, tools/guisurvey.py: `diatile` is ONE colour -- flat
# black -- and the dia* edges are grey bevel strips, so TA's dialog is a black
# panel in a chiselled frame. `back*` is the shell's mottled ground, 33 colours
# around (23,19,15)-(35,31,19), and tiles at 64. `hybrid` puts that texture
# inside the dialog's own frame, which is the one that looks like a TA panel
# rather than a TA dialog.
NINE = {
 "dia":  [["diaul", "diau", "diaur"],
          ["dial", "diatile", "diar"],
          ["diall", "diabottom", "dialr"]],
 "back": [["backul", "backu", "backur"],
          ["backl", "backtile#4", "backr"],      # 0 has a lit bottom edge
          ["backll", "backbottom", "backlr"]],
}
# text16* is a BLUE text-field well, not a neutral recess -- measured, and the
# reason the recesses below are drawn over whichever ground is chosen.
WELL = ["text16lend", "text16inner", "text16rend"]


def _kit():
    """name -> PIL image, from the UI GAFs of the player's own install."""
    loader = SourceFileLoader("ta3do", str(TOOLS / "ta3do"))
    spec = importlib.util.spec_from_loader("ta3do", loader)
    ta3do = importlib.util.module_from_spec(spec)
    loader.exec_module(ta3do)
    assets = ta3do.Assets()
    palette = ta3do.load_palette(assets)
    flat = []
    for c in palette[:256]:
        flat += list(c)[:3]
    flat += [0] * (768 - len(flat))

    out = {}
    for path in ("anims/frontend.gaf", "anims/commongui.gaf"):
        if not assets.has(path):
            continue
        blob = assets.read(path)
        sig, count, _ = struct.unpack_from("<III", blob, 0)
        if sig != 0x00010100:
            continue
        for off in struct.unpack_from(f"<{count}I", blob, 12):
            n = struct.unpack_from("<H", blob, off)[0]
            if n <= 0:
                continue
            name = blob[off + 8:off + 40].split(b"\0")[0].decode("latin-1").lower()
            if name in out:
                continue
            frames = []
            for i in range(n):
                fr = ta3do.gaf_frame(blob, struct.unpack_from("<I", blob, off + 40 + i * 8)[0])
                im = Image.new("P", (fr.width, fr.height))
                im.putdata(bytes(fr.pixels))
                im.putpalette(flat)
                frames.append(im.convert("RGB"))
            out[name] = frames
    return out


def pick(kit, spec):
    """`name` or `name#i` -- an entry may bundle several frames (backtile has
    nine texture variants, three of them carrying a bevel edge)."""
    name, _, idx = spec.partition("#")
    frames = kit[name]
    return frames[int(idx) if idx else 0]


def _tile(dst, src, box):
    """Repeat src across box, clipped -- what a nine-slice edge does."""
    x0, y0, x1, y1 = box
    if x1 <= x0 or y1 <= y0:
        return
    for y in range(y0, y1, src.height):
        for x in range(x0, x1, src.width):
            piece = src.crop((0, 0, min(src.width, x1 - x), min(src.height, y1 - y)))
            dst.paste(piece, (x, y))


def nine_slice(kit, w, h, names=NINE):
    """A panel of any size from a 3x3 kit: corners fixed, edges and centre tiled."""
    missing = [n for row in names for n in row if n.partition("#")[0] not in kit]
    if missing:
        raise SystemExit("missing GAF frames: " + ", ".join(missing))
    ul, u, ur = (pick(kit, n) for n in names[0])
    l, c, r = (pick(kit, n) for n in names[1])
    ll, b, lr = (pick(kit, n) for n in names[2])
    cw, ch = ul.width, ul.height
    im = Image.new("RGB", (w, h), (0, 0, 0))
    _tile(im, c, (cw, ch, w - cw, h - ch))            # centre
    _tile(im, u, (cw, 0, w - cw, ch))                 # edges
    _tile(im, b, (cw, h - ch, w - cw, h))
    _tile(im, l, (0, ch, cw, h - ch))
    _tile(im, r, (w - cw, ch, w, h - ch))
    im.paste(ul, (0, 0))                              # corners
    im.paste(ur, (w - cw, 0))
    im.paste(ll, (0, h - ch))
    im.paste(lr, (w - cw, h - ch))
    return im


def well(kit, dst, x, y, w, h, names=WELL):
    """A sunken track of any length from the text16 three-slice -- a recess."""
    le, inner, re = (pick(kit, n) for n in names)
    dst.paste(le.crop((0, 0, le.width, min(le.height, h))), (x, y))
    _tile(dst, inner, (x + le.width, y, x + w - re.width, y + h))
    dst.paste(re.crop((0, 0, re.width, min(re.height, h))), (x + w - re.width, y))


def compose_panel(ground="back", kit=None):
    """The drop-down, built out of the engine's own kits.

    dia    TA's dialog exactly: flat black in a grey bevel
    back   the shell's mottled ground, corners and edges included
    hybrid the back texture inside the dia frame -- a TA *panel*, not a dialog
    """
    kit = kit or _kit()
    if ground == "hybrid":
        im = nine_slice(kit, W, H, NINE["back"])          # the texture
        frame = nine_slice(kit, W, H, NINE["dia"])        # ...in the dialog frame
        m = 8                                             # keep only its border
        im.paste(frame.crop((0, 0, W, m)), (0, 0))
        im.paste(frame.crop((0, H - m, W, H)), (0, H - m))
        im.paste(frame.crop((0, 0, m, H)), (0, 0))
        im.paste(frame.crop((W - m, 0, W, H)), (W - m, 0))
    else:
        im = nine_slice(kit, W, H, NINE[ground])
    dr = ImageDraw.Draw(im)
    for n in range(ROWS):
        y = ROW_Y0 + ROW_PITCH * n
        recess(dr, CTL_X - PAD, y - PAD, CTL_W + 2 * PAD, ROW_H + 2 * PAD)
    return im


# ----------------------------------------------------------- the trigger --
# What OPENS the drop-down. Decided with the owner 2026-09-09 after two rounds
# of prototypes: a frameless icon on the top bar, in the register of the
# CAVEDOG LOGO -- `mainmenu.gui` GADGET5 `Credits`, which is an ordinary
# `id=1` button with `text=` empty and `attribs=1026` where every other button
# on that screen is `attribs=2`. Its art is `anims/mainmenu.gaf`, one entry,
# 80x40, five frames, and it has no plate, no bevel and no text. So the engine
# already ships the idea; this is not a new gadget kind.
#
# THE "FAINT TAN" IS A RAMP, NOT A COLOUR. TA's palette 55..63 is a dark warm
# ramp and the logo is drawn entirely inside it -- its resting frame is 778 px
# of 62, 395 of 61, 205 of 60, 113 of 59 and three of 58. The outline reads as
# faint because it is three ramp steps above its ground.
#
# AND THE STATE CHANGE IS A SLIDE ALONG THAT RAMP, not a second picture. Of the
# logo's five frames, 0/2/3 are identical, 1 drops 58 entirely and 59 falls
# 113 -> 24 (pressed), 4 gains 56/57 and more than doubles 58 (over). Nothing
# moves. That is how a button with no plate still feels like a button.
#
# THE ONE THING THAT COULD NOT BE COPIED STRAIGHT: measured on a real 1920x1080
# skirmish frame, the top bar is exactly 32 px and ITS OWN TEXTURE IS INDEX 62
# -- the modal bar pixel is (23,19,15), the same value that is 52% of the
# logo's ink. The logo's scheme laid on the bar would be invisible. So the ramp
# is re-hung around a lighter ground: the body goes BELOW the bar (63, and
# near-black at the core) and the outline ABOVE it, which keeps the logo's
# three-step relationship around a different centre.
#
# NONE OF THIS IS THE GAME'S ART. The mask is generated and the ink is nine
# palette indices, so unlike the panel ground this could be shipped outright --
# it is still composed at runtime, only because everything else is.
RAMP = {55: (95, 99, 71), 56: (91, 87, 59), 57: (83, 67, 51), 58: (71, 59, 43),
        59: (59, 51, 35), 60: (47, 43, 27), 61: (35, 31, 19), 62: (23, 19, 15),
        63: (11, 11, 7)}
BAR_H = 32                       # measured: rows 0..31, row 32 is the world
TRIG = 28                        # 2 px of bar above and below
# RIGHT-INSET, chosen by the owner over the corner itself: two icon-widths in
# from the right edge, so `trigger_at(frame_width)`. On the 1920 frame the
# quiet runs measured x 984..1327, 1497..1840 (343 px each) and 1841..1920
# (79 px, the corner) -- the inset lands inside the run that ends at 1840
# rather than hard against the edge, where a window border can clip it.
#
# The panel is right-aligned 16 px in (x = w - 320, since W is 304), so the
# trigger at w - 60 sits over the drop-down's own span and it hangs from
# underneath -- which is the whole reason for putting it on this side.
TRIG_INSET = 36                  # the gap the owner picked, right edge to icon


def trigger_at(frame_w=1024):
    return (frame_w - TRIG_INSET - TRIG, 2)


def panel_at(frame_w=1024):
    return (frame_w - W - 16, BAR_H)
# state -> (outline, inner shade, core)
TRIG_STATES = {"normal": (59, 61, 63), "over": (57, 60, 63),
               "pressed": (60, 62, 63), "greyed": (61, 62, 62)}


def sprocket(n=TRIG, teeth=8, r_out=13.4, r_root=10.2, tooth=0.5, bore=4.6):
    """The icon, as a boolean mask.

    Generated rather than hand-gridded because the shape's whole argument is
    eight teeth on 45 degree steps: every tooth is then a mirror of another
    across an axis or a diagonal, so the rasteriser cannot make one ragged
    without making its mirror ragged identically. `tooth` is the angular width
    as a fraction of the pitch, so 0.5 is teeth and gaps of equal width -- what
    makes this read as a SPROCKET rather than a gear: square teeth, no taper.
    """
    c = (n - 1) / 2.0
    half = math.pi / teeth                     # half the tooth pitch
    m = [[False] * n for _ in range(n)]
    for y in range(n):
        for x in range(n):
            dx, dy = x - c, y - c
            r = math.hypot(dx, dy)
            if r <= bore or r > r_out:         # the bore is a hole
                continue
            if r <= r_root:                    # the solid disc
                m[y][x] = True
                continue
            # phased so a tooth points straight up, which is what makes the
            # thing read as upright at 28 px
            ang = (math.atan2(dy, dx) + math.pi / 2 + half) % (2 * math.pi)
            k = (ang % (2 * half)) / (2 * half)         # 0..1 within one pitch
            if abs(k - 0.5) * 2 <= tooth:
                m[y][x] = True
    return m


def _boundary(mask):
    """A mask split into its one-pixel 4-connected border and the rest.

    The border traces the teeth AND the bore, which is what makes the hole
    read at 28 px.
    """
    n = len(mask)
    edge = [[False] * n for _ in range(n)]
    for y in range(n):
        for x in range(n):
            if not mask[y][x]:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if not (0 <= nx < n and 0 <= ny < n) or not mask[ny][nx]:
                    edge[y][x] = True
                    break
    inner = [[mask[y][x] and not edge[y][x] for x in range(n)] for y in range(n)]
    return edge, inner


def trigger(state="normal", mask=None):
    """One RGBA frame of the trigger: outline on the ramp, body below the bar."""
    ol, ci, bi = TRIG_STATES[state]
    mask = mask or sprocket()
    n = len(mask)
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    px = im.load()
    edge, inner = _boundary(mask)
    # one more ring a step down, so the body is not a flat plate against the
    # outline -- the logo does this too, 60 sitting between 59 and 62
    ring, core = _boundary(inner)
    for y in range(n):
        for x in range(n):
            if edge[y][x]:
                px[x, y] = RAMP[ol] + (255,)
            elif ring[y][x]:
                px[x, y] = RAMP[ci] + (255,)
            elif core[y][x]:
                px[x, y] = RAMP[bi] + (255,)
    return im


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default="tmp/gafui", help="output directory")
    ap.add_argument("--nine", nargs="?", const="back", default=None,
                    choices=["dia", "back", "hybrid"],
                    help="compose from the engine's own kit (default back)")
    ap.add_argument("--trigger", action="store_true",
                    help="write the four trigger frames instead of the panel")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)

    if a.trigger:
        mask = sprocket()
        for st in TRIG_STATES:
            trigger(st, mask).save(out / f"trigger-{st}.png")
        print(f"trigger-*.png  {TRIG}x{TRIG} RGBA, {len(TRIG_STATES)} states, "
              f"nothing sampled from the game")
        for fw in (1024, 1920):
            print(f"  a frameless icon at {trigger_at(fw)} of a {fw}-wide frame"
                  f", in the {BAR_H} px top bar; panel at {panel_at(fw)}")
        print(f"  ink walks TA's palette ramp 55..63 -- outline "
              + ", ".join(f"{s}={v[0]}" for s, v in TRIG_STATES.items()))
        return

    if a.nine:
        im = compose_panel(a.nine)
        rec = [(ROW_Y0 + ROW_PITCH * n, ROW_H) for n in range(ROWS)]
        dest = out / f"renderdd-{a.nine}-0.png"
        im.save(dest)
        print(f"renderdd-{a.nine}-0.png  {W}x{H}  composed from frontend.gaf's")
        print(f"  own '{a.nine}' kit, recesses drawn over it (text16 is blue).")
        print(f"  THE PIXELS ARE THE GAME'S: this output is gitignored, and the")
        print(f"  DLL does this composition at runtime from the player's install.")
    else:
        im, rec = draw_panel()
        dest = out / "renderdd-0.png"
        im.save(dest)
        print(f"renderdd-0.png  {W}x{H}  drawn, nothing sampled from the game")
    print(f"  panel id=0 at (704, 32), the drop-down of renderers.md 2.10")
    print(f"  {len(rec)} recesses at x={CTL_X - PAD} w={CTL_W + 2 * PAD}, "
          f"for a {CTL_W}x{ROW_H} stagebuttn each:")
    for y, h in rec:
        print(f"    y={y} h={h}")
    print(f"  labels sit on the face at x=14 w=144, LEFT of the control -- the")
    print(f"  stock screens put them 16 px above, which a 7-row panel has no")
    print(f"  room for. That is the whole reason the frame has to be drawn.")
    print(f"-> {dest}")


if __name__ == "__main__":
    main()
