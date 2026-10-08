"""Render the whitebox Codex paints a one-piece part from: the plate in grey, each recess dark and numbered,
each text zone (text written on the plate itself, optional `TEXT` in the spec) outlined, the legend under it,
all on the flat magenta key.
usage: whitebox.py <spec> <out base>    -> <out base>.png, and <out base>.json for gridslice.py
<spec> names a module in specs/ (row, head, plate, window, transport, timeline)."""
import importlib, json, os, sys
from PIL import Image, ImageDraw, ImageFont
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'specs'))
spec = importlib.import_module(sys.argv[1]); out = sys.argv[2]
DESIGN, S, CELL, ZONES, LEGEND = spec.DESIGN, spec.S, spec.CELL, spec.ZONES, spec.LEGEND
F = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'; f = lambda n: ImageFont.truetype(F, n)
x0, y0 = CELL; w, h = round(DESIGN[0] * S), round(DESIGN[1] * S)
W, H = 1680, max(560, y0 + h + 60 + 30 * len(LEGEND))
im = Image.new('RGB', (W, H), (255, 0, 255)); d = ImageDraw.Draw(im)
d.text((x0, y0 - 40), spec.TITLE, font=f(22), fill=(255, 255, 255))
d.rectangle((x0, y0, x0 + w - 1, y0 + h - 1), fill=(128, 128, 128), outline=(40, 40, 40), width=2)
for i, (k, (x, y, zw, zh)) in enumerate(ZONES.items(), 1):
    r = (round(x0 + x * S), round(y0 + y * S), round(x0 + (x + zw) * S) - 1, round(y0 + (y + zh) * S) - 1)
    d.rectangle(r, fill=(52, 52, 52), outline=(30, 30, 30), width=2)
    d.text(((r[0] + r[2]) / 2, (r[1] + r[3]) / 2), str(i), font=f(20), fill=(200, 200, 200), anchor='mm')
TEXT = getattr(spec, 'TEXT', {})
for i, (k, (x, y, zw, zh)) in enumerate(TEXT.items(), len(ZONES) + 1):
    r = (round(x0 + x * S), round(y0 + y * S), round(x0 + (x + zw) * S) - 1, round(y0 + (y + zh) * S) - 1)
    for j in range(0, 3): d.rectangle((r[0] + j, r[1] + j, r[2] - j, r[3] - j), outline=(232, 232, 232))
    d.text(((r[0] + r[2]) / 2, (r[1] + r[3]) / 2), str(i), font=f(20), fill=(232, 232, 232), anchor='mm')
for i, (k, t) in enumerate(LEGEND.items(), 1):
    d.text((x0, y0 + h + 30 + (i - 1) * 30), f'{i}  {t}', font=f(19), fill=(255, 255, 255))
im.save(out + '.png')
json.dump({'cell': [x0, y0, w, h], 'design': DESIGN, 'S': S, 'zones': ZONES, 'xb': spec.XB, 'yb': spec.YB, **({'text': TEXT} if TEXT else {})}, open(out + '.json', 'w'))
print(im.size)
