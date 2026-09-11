#!/usr/bin/env python3
"""PROTOTYPE — throwaway. Renders the terminal in each TERMINAL_STYLES look.

The question: the terminal is on screen alone for the first seven seconds of the
film and it looked unpolished. Which treatment ships?

    promo/prototype-cuts/terminal-styles.py [outdir]

Writes one full-frame PNG per style plus a side-by-side sheet. Pick one, set
`terminal_style` in promo/tacli-promo.json, delete this file.
"""

import os
import sys
from importlib.machinery import SourceFileLoader
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
tm = SourceFileLoader("tamontage", str(REPO / "tools" / "tamontage")).load_module()

# The moment worth judging: command fully typed, output printed, cursor up.
AT = 6.4
W, H = 2560, 1440

NOTES = {
    "bash": "the shipped look: bash titlebar with Chrome window controls",
    "hero": "big type, block composed against the window — the command IS the shot",
    "classic": "what shipped: traffic lights, hard 1px border, tight padding",
    "soft": "rounded, muted chrome, title only, bar cursor, generous padding",
    "chromeless": "no titlebar at all; syntax-coloured command, big padding",
    "phosphor": "amber/green CRT with scanlines — nods to the 1997 engine",
}


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else \
        Path(os.environ.get("TMPDIR", "/tmp")) / "tacli-terminal-styles"
    out.mkdir(parents=True, exist_ok=True)

    shots = []
    for style in tm.TERMINAL_STYLES:
        m = tm.Montage(REPO / "promo" / "tacli-promo.json", "wire", None,
                       Path(os.environ.get("TMPDIR", "/tmp")) / "tamontage-cache",
                       30.0, aspect=H / W)
        for win in m.windows:
            if win.term is not None:
                win.term.st = tm.TERMINAL_STYLES[style]
        img = m.render_frame(AT, W, H)
        p = out / f"terminal-{style}.png"
        img.save(p)
        shots.append((style, img))
        print(f"  {style:12s} -> {p.name}")

    # side-by-side sheet, two across
    tw, th = W // 2, H // 2
    lbl = 54
    sheet = Image.new("RGB", (tw * 2 + 36, (th + lbl) * 2 + 36), (10, 12, 15))
    d = ImageDraw.Draw(sheet)
    try:
        import subprocess
        fp = subprocess.run(["fc-match", "-f", "%{file}", "DejaVu Sans:style=Bold"],
                            capture_output=True, text=True).stdout.strip()
        f1 = ImageFont.truetype(fp, 26)
        f2 = ImageFont.truetype(fp, 17)
    except Exception:
        f1 = f2 = ImageFont.load_default()

    for i, (style, img) in enumerate(shots):
        cx, cy = 12 + (i % 2) * (tw + 12), 12 + (i // 2) * (th + lbl + 12)
        sheet.paste(img.resize((tw, th), Image.LANCZOS), (cx, cy))
        d.rectangle([cx, cy, cx + tw - 1, cy + th - 1], outline=(40, 46, 55))
        d.text((cx + 4, cy + th + 8), style, font=f1, fill=(235, 240, 246))
        d.text((cx + 4, cy + th + 34), NOTES[style], font=f2, fill=(140, 150, 162))

    sp = out / "terminal-styles-sheet.png"
    sheet.save(sp)
    print(f"\n  sheet -> {sp}")


if __name__ == "__main__":
    main()
