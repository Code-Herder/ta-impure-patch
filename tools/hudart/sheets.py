"""Render the parts sheets the 9-slice parts and icons are painted from: grey cells on the flat magenta key,
each labelled; the corners a part may shape are marked dark and its slice lines dashed.
usage: [PLAIN=1] sheets.py <parts spec> <out dir>   -> parts_<sheet>.png, parts_icons.png and sheets.json
<parts spec> names a module in specs/ (parts_look, parts_buttons). PLAIN=1 draws flat cells with no corner
marks: an image model reads a dark corner square as a place for hardware, or paints it as a dark block."""
import importlib, json, os, sys
from PIL import Image, ImageDraw, ImageFont
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'specs'))
_k = importlib.import_module(sys.argv[1]); SHEETS, ICONS, ICON_LABEL = _k.SHEETS, _k.ICONS, _k.ICON_LABEL
F = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
MAG = (255, 0, 255)
OUT = sys.argv[2]

def dashed(d, x0, y0, x1, y1, col, dash=6):
    if x0 == x1:
        for y in range(int(y0), int(y1), dash * 2): d.line((x0, y, x0, min(y + dash, y1)), fill=col, width=2)
    else:
        for x in range(int(x0), int(x1), dash * 2): d.line((x, y0, min(x + dash, x1), y0), fill=col, width=2)

def sheet(name, parts, s, W=1680):
    gap, lab = 14 * s, 11 * s
    font = ImageFont.truetype(F, 6 * s)
    # shelf packing, tallest first
    order = sorted(parts, key=lambda p: -p[2])
    x, y, row_h, place = gap, gap, 0, {}
    for p in order:
        w, h = p[1] * s, p[2] * s
        tag = ' (fixed)' if p[5] == 'fixed' else ' (fill bar)' if p[5] == 'core' else ''
        slot = max(w, int(font.getlength(p[4] + tag)))
        if x + slot + gap > W: x, y, row_h = gap, y + row_h + lab + gap, 0
        place[p[0]] = (x, y + lab, w, h); x += slot + gap; row_h = max(row_h, h)
    H = y + row_h + lab + gap
    im = Image.new('RGB', (W, H), MAG); d = ImageDraw.Draw(im)
    for n, w_, h_, b, label, fl in parts:
        x, y, w, h = place[n]
        d.rectangle((x, y, x + w - 1, y + h - 1), fill=(128, 128, 128))
        tag = ' (fixed)' if fl == 'fixed' else ' (fill bar)' if fl == 'core' else ''
        d.text((x, y - lab + s), label + tag, font=font, fill=(255, 255, 255))
        if b and not os.environ.get('PLAIN'):
            B = b * s
            for cx, cy in [(x, y), (x + w - B, y), (x, y + h - B), (x + w - B, y + h - B)]: d.rectangle((cx, cy, cx + B - 1, cy + B - 1), fill=(64, 64, 64))
            for xx in (x + B, x + w - B): dashed(d, xx, y, xx, y + h, (200, 200, 200))
            for yy in (y + B, y + h - B): dashed(d, x, yy, x + w, yy, (200, 200, 200))
    im.save(f'{OUT}/parts_{name}.png')
    return {'size': [W, H], 'scale': s, 'cells': {n: place[n] for n, *_ in parts}}

def icons(s=6, cols=8):
    cell, gapx, lab = 16 * s, 16 * s, 9 * s
    font = ImageFont.truetype(F, 4 * s)
    rows = (len(ICONS) + cols - 1) // cols
    W = cols * (cell + gapx) + gapx; H = rows * (cell + lab + gapx) + gapx
    im = Image.new('RGB', (W, H), MAG); d = ImageDraw.Draw(im); place = {}
    for i, n in enumerate(ICONS):
        x = gapx + (i % cols) * (cell + gapx); y = gapx + (i // cols) * (cell + lab + gapx)
        for (a, b2, c, e) in [(x, y, x + cell, y), (x, y + cell, x + cell, y + cell), (x, y, x, y + cell), (x + cell, y, x + cell, y + cell)]: dashed(d, a, b2, c, e, (40, 40, 40))
        d.text((x, y + cell + s), ICON_LABEL.get(n, n), font=font, fill=(255, 255, 255))
        place['i_' + n] = (x, y, cell, cell)
    im.save(f'{OUT}/parts_icons.png')
    return {'size': [W, H], 'scale': s, 'cells': place}

meta = {k: sheet(k, *v) for k, v in SHEETS.items()}
if ICONS: meta['icons'] = icons()
json.dump(meta, open(f'{OUT}/sheets.json', 'w'), indent=1)
print({k: v['size'] for k, v in meta.items()})
