#!/usr/bin/env python3
"""guisurvey.py — the GAF kits a new screen can be built from.

G18c needs a ground for the drop-down (renderers.md §2.10) and the obvious
answer -- reuse a stock panel -- does not work: none of the runtime frames has
seven `h20` recesses, and none puts a label beside its control. Drawing one
from nothing was the fallback. This asks the archives first, and the archives
answer better than the fallback:

  TA'S FLOATING DIALOGS HAVE NO BACKGROUND GADGET AT ALL.  `guis/msgbox.gui`,
  `yesorno.gui` and `exitmenu.gui` declare `panel=` empty and carry no `id=12`
  -- the engine composes their ground itself, from a NINE-SLICE kit in
  `anims/frontend.gaf`:

        diaul   diau       diaur          all 32x32
        dial    diatile    diar           `diatile` is the tileable centre
        diall   diabottom  dialr

  so a dialog panel of any size is nine named frames and some repetition, and
  `text16lend/inner/rend` + `text32*` are the same trick for a sunken well --
  which is what a recess is. There is a second, 64x64 kit (`back*`) for the
  shell's full-screen ground, in both `frontend.gaf` and `commongui.gaf`.

    .venv-undither/bin/python tools/guisurvey.py            # -> tmp/gafui/

Writes the kit frames as PNGs, `survey-kits.png` (every kit laid out in the
arrangement it assembles in) and `survey-panels.png` (the full-size runtime
frames, for comparison), and prints what it found.

EVERY PIXEL IT TOUCHES IS THE ORIGINAL GAME'S. The output directory is
gitignored and must stay that way. What ships is the COMPOSITOR --
`tools/guipanel.py --nine` builds the panel from these frames the way the DLL
will at runtime, so the art stays in the player's own install and never in
this repository.
"""

import argparse
import importlib.util
import struct
import sys
from importlib.machinery import SourceFileLoader
from pathlib import Path

from PIL import Image, ImageDraw

TOOLS = Path(__file__).resolve().parent

# The kits, in the arrangement they assemble in. None means "no frame there".
NINE_DIA = [["diaul", "diau", "diaur"],
            ["dial", "diatile", "diar"],
            ["diall", "diabottom", "dialr"]]
NINE_BACK = [["backul", "backu", "backur"],
             ["backl", "backtile", "backr"],
             ["backll", "backbottom", "backlr"]]
THREE = {"text16": ["text16lend", "text16inner", "text16rend"],
         "text32": ["text32lend", "text32inner", "text32rend"],
         "sliderhoriz": ["sliderhorizlend", "sliderhorizinner", "sliderhorizrend"],
         "slidervert": ["sliderverttop", "slidervertinner", "slidervertbottom"]}
PANELS = ["visualsrt", "soundsrt", "musicrt", "speedsrt", "igopt",
          "armpan", "armpan2", "frontpan", "lightbar"]
UI_GAFS = ["anims/frontend.gaf", "anims/commongui.gaf"]


def load_ta3do():
    loader = SourceFileLoader("ta3do", str(TOOLS / "ta3do"))
    spec = importlib.util.spec_from_loader("ta3do", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


def entry_frames(blob):
    sig, count, _ = struct.unpack_from("<III", blob, 0)
    if sig != 0x00010100:
        return {}
    out = {}
    for off in struct.unpack_from(f"<{count}I", blob, 12):
        n = struct.unpack_from("<H", blob, off)[0]
        if n <= 0:
            continue
        name = blob[off + 8:off + 40].split(b"\0")[0].decode("latin-1").lower()
        out[name] = [struct.unpack_from("<I", blob, off + 40 + i * 8)[0] for i in range(n)]
    return out


class Kit:
    """Every UI GAF, merged: name -> (blob, [frame offsets])."""

    def __init__(self, assets, ta3do, flat):
        self.ta3do, self.flat, self.table = ta3do, flat, {}
        for path in UI_GAFS:
            if not assets.has(path):
                continue
            blob = assets.read(path)
            for name, offs in entry_frames(blob).items():
                self.table.setdefault(name, (blob, offs, path))

    def __contains__(self, name):
        return name in self.table

    def frame(self, name, i=0):
        blob, offs, _ = self.table[name]
        fr = self.ta3do.gaf_frame(blob, offs[min(i, len(offs) - 1)])
        im = Image.new("P", (fr.width, fr.height))
        im.putdata(bytes(fr.pixels))
        im.putpalette(self.flat)
        if fr.transparent is not None:
            im.info["transparency"] = fr.transparent
        return im

    def where(self, name):
        return self.table[name][2].split("/")[-1]


def sheet(blocks, path, title):
    """blocks: [(heading, [(label, image)], cols)] -> one labelled sheet."""
    PAD, LAB, HEAD = 9, 12, 20
    rows = []
    for head, items, cols in blocks:
        grid = [items[i:i + cols] for i in range(0, len(items), cols)]
        rows.append((head, grid))
    W = 0
    H = 30
    for head, grid in rows:
        H += HEAD
        for r in grid:
            W = max(W, sum(i.width + PAD for _, i in r) + PAD)
            H += max(i.height for _, i in r) + LAB + PAD
        H += PAD
    W = max(W, 520)
    out = Image.new("RGB", (W, H), (17, 14, 10))
    dr = ImageDraw.Draw(out)
    dr.text((PAD, 8), title, fill=(239, 230, 192))
    y = 30
    for head, grid in rows:
        dr.text((PAD, y + 3), head, fill=(255, 210, 63))
        y += HEAD
        for r in grid:
            x = PAD
            for label, im in r:
                out.paste(im.convert("RGB"), (x, y))
                dr.rectangle([x - 1, y - 1, x + im.width, y + im.height],
                             outline=(69, 58, 44))
                dr.text((x, y + im.height + 1), label, fill=(201, 188, 134))
                x += im.width + PAD
            y += max(i.height for _, i in r) + LAB + PAD
        y += PAD
    out.save(path)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default="tmp/gafui")
    a = ap.parse_args()

    ta3do = load_ta3do()
    assets = ta3do.Assets()
    palette = ta3do.load_palette(assets)
    flat = []
    for c in palette[:256]:
        flat += list(c)[:3]
    flat += [0] * (768 - len(flat))

    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    kit = Kit(assets, ta3do, flat)

    # ---- what the floating dialogs actually declare ----------------------
    print("TA's floating dialogs and their background gadget:")
    for name in ("msgbox", "yesorno", "exitmenu", "restart", "talk"):
        path = f"guis/{name}.gui"
        if not assets.has(path):
            print(f"  {name:9s} (absent)")
            continue
        text = assets.read(path).decode("latin-1")
        has12 = "id=12" in text.replace(" ", "")
        panel = [ln.strip() for ln in text.splitlines() if ln.strip().startswith("panel=")]
        print(f"  {name:9s} id=12: {'yes' if has12 else 'NO':3s}   "
              f"{panel[0] if panel else 'panel= absent'}")
    print("  -> the ground is composed by the engine, not named by a gadget.\n")

    blocks, saved = [], 0

    def grab(names, tag):
        nonlocal saved
        items = []
        for n in names:
            if n is None or n not in kit:
                continue
            im = kit.frame(n)
            im.save(out / f"{n}-0.png")
            saved += 1
            items.append((f"{n} {im.width}x{im.height}", im))
        return items

    for label, grid in (("dia — the DIALOG nine-slice, 32x32 (what a popup is made of)", NINE_DIA),
                        ("back — the SHELL nine-slice, 64x64", NINE_BACK)):
        rows = [grab(r, label) for r in grid]
        if all(rows):
            blocks.append((label, [it for r in rows for it in r], 3))
            print(f"{label}")
            for r, names in zip(rows, grid):
                print("   " + "  ".join(f"{n:11s}" for n in names)
                      + f"   in {kit.where(names[1])}")

    three = []
    for tag, names in THREE.items():
        three += grab(names, tag)
    if three:
        blocks.append(("three-slice wells — a sunken track of any length; "
                       "text16/32 IS a recess", three, 6))
        print("\nthree-slice wells (left end, tiled inner, right end):")
        for tag, names in THREE.items():
            if names[0] in kit:
                im = kit.frame(names[0])
                print(f"   {tag:12s} {im.width}x{im.height}   " + " ".join(names))

    stages = grab([f"stagebuttn{i}" for i in (1, 2, 3, 4)], "stage")
    if stages:
        blocks.append(("stage buttons — 120x20, the control itself", stages, 4))

    if blocks:
        sheet(blocks, out / "survey-kits.png",
              "GAF KITS — everything a new screen can be assembled from")

    panels = grab(PANELS, "panels")
    if panels:
        sheet([("runtime panel frames, for comparison — none has seven h20 "
                "recesses", panels, 5)],
              out / "survey-panels.png", "RUNTIME PANELS — the stock grounds")

    print(f"\n{saved} frames + 2 sheets -> {out}/")
    print("   survey-kits.png    the kits, in the arrangement they assemble in")
    print("   survey-panels.png  the stock runtime grounds, for comparison")
    print("\nNOTE: every pixel above is the original game's; the directory is")
    print("      gitignored. What ships is the compositor, not the pixels:")
    print("      tools/guipanel.py --nine builds the panel from these frames.")


if __name__ == "__main__":
    main()
