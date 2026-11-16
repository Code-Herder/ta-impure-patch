"""Makes the stall lamp's two parts, lit and dark, from their paintings: a kit directory that file.py files.
usage: lamp.py <lit.png> <dark.png> <out dir>

The lamp is drawn on an ally's chip (layout.json proposed.allies.lamp) `W` x `H` units. The lit part carries its
glow: the painting's alpha blurred twice and tinted, as the shared-resources prototype drew it with CSS
`drop-shadow(0 0 3px #ff2a1a) drop-shadow(0 0 5px #ff2a1a90)` at 1.5 px a unit (a blur radius r is a Gaussian of
sigma r / 2, and the second shadow is cast by the first one's result). The dark part is the same size with nothing
round the painting, so one box holds either and the lamp blinks in place. Both are `whole` parts filed at `SCALE` px a
unit; fit.py fits them down from there."""
import json, sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageFilter

W, H = 9.333, 6.0           # the painting's box, units: the prototype's 14 x 9 px at 1.5 px a unit
MARGIN = 7.0                # the glow's room round it, units: past where the chained shadows round to 0
SCALE = 12.0                # px a unit as filed
GLOW = ((1.0, 1.0), (5 / 3, 0x90 / 255))    # (sigma in units, alpha) of each shadow, in order
TINT = (0xFF, 0x2A, 0x1A)


def shadow(im, sigma, alpha):
    a = im.getchannel('A').filter(ImageFilter.GaussianBlur(sigma * SCALE))
    s = Image.new('RGBA', im.size, TINT + (0,))
    s.putalpha(a.point(lambda v: round(v * alpha)))
    s.alpha_composite(im)
    return s


def main(lit, dark, out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    size = (round(W * SCALE), round(H * SCALE))
    pad = round(MARGIN * SCALE)
    canvas = (size[0] + 2 * pad, size[1] + 2 * pad)
    parts = {}
    for name, src, glow in (('lamp_on', lit, True), ('lamp_off', dark, False)):
        im = Image.new('RGBA', canvas, (0, 0, 0, 0))
        im.alpha_composite(Image.open(src).convert('RGBA').resize(size, Image.LANCZOS), (pad, pad))
        if glow:
            for sigma, alpha in GLOW: im = shadow(im, sigma, alpha)
        assert not np.asarray(im)[[0, -1], :, 3].any() and not np.asarray(im)[:, [0, -1], 3].any(), f'{name}: the glow reaches the edge'
        im.save(out / f'{name}.png')
        parts[name] = {'fixed': True, 'scale': SCALE}
    json.dump(parts, open(out / 'kit.json', 'w'), indent=1)
    print(f'lamp_on, lamp_off: {canvas[0]} x {canvas[1]} px, {canvas[0] / SCALE:.3f} x {canvas[1] / SCALE:.3f} units')


if __name__ == '__main__':
    if len(sys.argv) != 4: sys.exit(__doc__)
    main(*sys.argv[1:])
