"""Cut a part painted as one piece and measure it for grid slicing.
usage: gridslice.py <painted.png> <whitebox.json> <out dir> <part name>
Writes <part>.png and adds to <out dir>/kit.json: the design size (units), each recess as measured in the
painting (units), and the stretch bands along x and y (units). Only the stretch bands are stretched when
the part is drawn at another size; every other band keeps its painted size, which is what keeps bevels,
corners and recess edges intact. A band is placed inside the recesses, where the cross-section runs
straight through every recess edge and the plate between them."""
import json, sys, os
import numpy as np
from PIL import Image
from scipy import ndimage
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from keying import despill

src, wb_p, out, part = sys.argv[1:5]
wb = json.load(open(wb_p)); DW, DH = wb['design']; Z = wb['zones']
im = np.asarray(Image.open(src).convert('RGB')).astype(float)
r, g, b = im[..., 0], im[..., 1], im[..., 2]
mag = np.clip((np.minimum(r, b) - g - 60) / 120, 0, 1) * np.clip(1 - np.abs(r - b) / 90, 0, 1)
fg = ndimage.binary_dilation(mag < .5, iterations=2)
lab, n = ndimage.label(fg)
big = 1 + int(np.argmax(ndimage.sum(fg, lab, range(1, n + 1))))   # the row: the largest painted shape
ys, xs = ndimage.find_objects(lab)[big - 1]
a = (1 - mag)[ys, xs] * (lab[ys, xs] == big)
yy, xx = np.nonzero(a > .5); y0, y1, x0, x1 = yy.min(), yy.max() + 1, xx.min(), xx.max() + 1
solid = mag < .05
_, (iy, ix) = ndimage.distance_transform_edt(~solid, return_indices=True)
rgb = despill(im)[iy, ix][ys, xs][y0:y1, x0:x1]; al = a[y0:y1, x0:x1]; al = np.where(al > .9, 1, al)
ph, pw = al.shape
# one scale on both axes: the design width is the whitebox's, the height is what the painting gives it
# (a painting a little taller than asked keeps its bolts and bevels round; its stretch bands absorb the rest)
sx = sy = pw / DW; DHp = ph / sy
Image.fromarray(np.dstack([rgb, al * 255]).clip(0, 255).astype(np.uint8), 'RGBA').save(os.path.join(out, part + '.png'))

# recesses: darker than the plate around them; each is found from its whitebox spot outward
L = rgb.mean(2)
# Otsu's split of the piece's brightness: the recesses on one side, the plate on the other
hist, edges = np.histogram(L[al > .9], bins=256, range=(0, 256)); c = (edges[:-1] + edges[1:]) / 2
w0 = np.cumsum(hist); w1 = w0[-1] - w0; m0 = np.cumsum(hist * c) / np.maximum(w0, 1); m1 = (np.sum(hist * c) - np.cumsum(hist * c)) / np.maximum(w1, 1)
thr = c[int(np.argmax(w0 * w1 * (m0 - m1) ** 2))]
Lr, Lp = np.median(L[(L < thr) & (al > .9)]), np.median(L[(L >= thr) & (al > .9)])
dark = ndimage.median_filter(L < thr, size=3)
def extent(prof, c):
    lo = hi = c
    while lo > 0 and prof[lo - 1] > .5: lo -= 1
    while hi < len(prof) - 1 and prof[hi + 1] > .5: hi += 1
    return lo, hi + 1
fy_ = DHp / DH
Z = {k: [x, y * fy_, w, h * fy_] for k, (x, y, w, h) in Z.items()}
# each whitebox recess is matched to one dark region of the painting, all at once (one region per recess):
# a painting that keeps the layout but shifts or resizes a recess still pairs with the right one. Regions come
# from the dark mask eroded by one HUD unit, so grime or a shadow line along a rim cannot join two recesses
from scipy.optimize import linear_sum_assignment
rl, rn = ndimage.label(ndimage.binary_erosion(dark, iterations=max(2, int(round(sx)))))
regs = [(q, o) for q, o in enumerate(ndimage.find_objects(rl), 1) if o and (o[0].stop - o[0].start) * (o[1].stop - o[1].start) > 4 * sx * sx]
keys = list(Z)
cost = np.zeros((len(keys), len(regs)))
for zi, k in enumerate(keys):
    x, y, w, h = Z[k]
    for ri, (q, (ry, rx)) in enumerate(regs):
        X, Y, Wd, Hd = rx.start / sx, ry.start / sy, (rx.stop - rx.start) / sx, (ry.stop - ry.start) / sy
        d = np.hypot(X + Wd / 2 - x - w / 2, Y + Hd / 2 - y - h / 2) / max(DW, DHp) * 8
        cost[zi, ri] = d + abs(np.log((Wd + 2) / (w + 2))) + abs(np.log((Hd + 2) / (h + 2)))
zi_, ri_ = linear_sum_assignment(cost)
wells = {}
for zi, ri in zip(zi_, ri_):
    k = keys[zi]; q, (ry, rx) = regs[ri]
    cx, cy, rh = (rx.start + rx.stop) // 2, (ry.start + ry.stop) // 2, ry.stop - ry.start
    band = dark[ry.start + int(.3 * rh): ry.start + int(.7 * rh) + 1]
    X0, X1 = extent(band.mean(0), cx)
    col = dark[:, X0 + (X1 - X0) // 4: X1 - (X1 - X0) // 4]
    Y0, Y1 = extent(col.mean(1), cy)
    wells[k] = [round(X0 / sx, 2), round(Y0 / sy, 2), round((X1 - X0) / sx, 2), round((Y1 - Y0) / sy, 2)]
for k in keys:
    if k not in wells: print(f'  {k}: no dark region left to match')
wells = {k: wells[k] for k in keys if k in wells}

def mid(lo, hi, f=.3): return [round(lo + (hi - lo) * f, 2), round(hi - (hi - lo) * f, 2)]
def span(*ks, ax=0):
    lo = max(wells[k][ax] for k in ks); hi = min(wells[k][ax] + wells[k][ax + 2] for k in ks); return lo, hi
xb = [mid(*span(*ks)) for ks in wb['xb']]
yb = [mid(*span(*ks, ax=1)) for ks in wb['yb']]
def plain(bands, ax):
    # how far each cross-section inside a band strays from the band's median one, in 0..255 levels
    res = []
    for lo, hi in bands:
        A = rgb[:, int(lo * sx):int(hi * sx)].transpose(1, 0, 2) if ax == 0 else rgb[int(lo * sy):int(hi * sy)]
        res.append(round(float(np.abs(A - np.median(A, axis=0)).mean()), 1))
    return res
kp = os.path.join(out, 'kit.json'); kit = json.load(open(kp)) if os.path.exists(kp) else {}
kit[part] = {'w': int(pw), 'h': int(ph), 'scale': round(float(sx), 3), 'grid': True, 'design': [DW, round(float(DHp), 2)], 'wells': wells, 'xs': xb, 'ys': yb}
json.dump(kit, open(kp, 'w'), indent=1)
print(f'{part}: {pw}x{ph} px, {sx:.2f} px/unit, design {DW} x {DHp:.2f}, plate L {Lp:.0f}, recess L {Lr:.0f}')
for k, v in wells.items(): print(f'  {k:8s} measured {v}  whitebox {Z[k]}')
print('  x bands', xb, 'stray', plain(xb, 0)); print('  y bands', yb, 'stray', plain(yb, 1))
