"""Cut a painted parts sheet into its parts: shapes on the magenta key, matched to the sheet's cells, and
each part's 9-slice insets measured off the painting.
usage: [PRESSED_DARK=<w>] cut.py <parts spec> <painted.png> <sheet name> <sheets.json> <out dir>
    -> <out dir>/<part>.png and its <out dir>/kit.json entry
<parts spec> names a module in specs/; <sheet name> is a key of its SHEETS (col). For icons use iconcut.py.
A part repainted on its own is cut with `one:<part>` in place of <sheets.json>: one cell, the whole painting."""
import importlib, json, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage
from scipy.optimize import linear_sum_assignment
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, 'specs'))
from keying import despill
SHEETS = importlib.import_module(sys.argv[1]).SHEETS

src, name, meta_p, out = sys.argv[2:6]
if meta_p.startswith('one:'):
    W0, H0 = Image.open(src).size
    meta = {'size': [W0, H0], 'cells': {meta_p[4:]: [0, 0, W0, H0]}}
else:
    meta = json.load(open(meta_p))[name]
spec = {p[0]: p for p in SHEETS[name][0]} if name in SHEETS else {}
im = np.asarray(Image.open(src).convert('RGB')).astype(float)
H, W, _ = im.shape
WW, WH = meta['size']; sx, sy = W / WW, H / WH
r, g, b = im[..., 0], im[..., 1], im[..., 2]
# magenta-ness: 1 on the key, 0 on paint
mag = np.clip((np.minimum(r, b) - g - 60) / 120, 0, 1) * np.clip(1 - np.abs(r - b) / 90, 0, 1)
fg = mag < .5
icons = name == 'icons'
fgd = ndimage.binary_dilation(fg, iterations=10 if icons else 2)
lab, n = ndimage.label(fgd)
objs = ndimage.find_objects(lab)
areas = ndimage.sum(fg, lab, range(1, n + 1))
shapes = [(i + 1, o) for i, o in enumerate(objs) if areas[i] > 60]
cells = list(meta['cells'].items())
cost = np.full((len(cells), len(shapes)), 1e6)
for ci, (cn, (x, y, w, h)) in enumerate(cells):
    ex, ey, ew, eh = x * sx, y * sy, w * sx, h * sy
    for si, (li, (ys, xs)) in enumerate(shapes):
        bx, by, bw, bh = xs.start, ys.start, xs.stop - xs.start, ys.stop - ys.start
        d = np.hypot((bx + bw / 2) - (ex + ew / 2), (by + bh / 2) - (ey + eh / 2)) / max(W, H) * 8
        ar = abs(np.log((bw / bh) / (ew / eh)))
        sz = abs(np.log(np.sqrt(bw * bh) / np.sqrt(ew * eh)))
        cost[ci, si] = d + ar * 1.5 + sz * .5
if os.environ.get('PRESSED_DARK'):
    # PRESSED_DARK=<weight>: a sheet of buttons and their pressed versions, where pressed is the darker of each pair:
    # a pressed part prefers the darker shapes and a raised one the lighter, so a sheet with extra or
    # shuffled shapes cannot hand a raised face to a pressed slot
    Ls = np.array([im[ys_, xs_][lab[ys_, xs_] == li].mean() for li, (ys_, xs_) in shapes])
    z = (Ls - Ls.mean()) / max(Ls.std(), 1)
    for ci, (cn, _) in enumerate(cells): cost[ci] += float(os.environ['PRESSED_DARK']) * (z if cn.endswith('_down') else -z)
ri, si_ = linear_sum_assignment(cost)
os.makedirs(out, exist_ok=True)
kp = os.path.join(out, 'kit.json'); kit = json.load(open(kp)) if os.path.exists(kp) else {}
alpha_all = 1 - mag
# colour de-fringe: every pixel takes the colour of its nearest solid pixel
solid = mag < .05
_, (iy, ix) = ndimage.distance_transform_edt(~solid, return_indices=True)
clean = despill(im)[iy, ix]
rep = []
pairs = list(zip(ri, si_))
if icons:   # an icon may be several separate strokes: take everything painted inside its cell, widened by 45%
    pairs = []
    for c, (cn, (x, y, w, h)) in enumerate(cells):
        ex, ey = int(max(0, (x - .45 * w) * sx)), int(max(0, (y - .45 * h) * sy))
        pairs.append((c, (slice(ey, int(min(H, (y + 1.45 * h) * sy))), slice(ex, int(min(W, (x + 1.45 * w) * sx))))))
for c, s in pairs:
    if icons: cn, (x, y, w, h) = cells[c]; ys, xs = s; m = np.ones((ys.stop - ys.start, xs.stop - xs.start), bool)
    else:
        cn, (x, y, w, h) = cells[c]; li, (ys, xs) = shapes[s]
        m = (lab[ys, xs] == li)
    a = alpha_all[ys, xs] * m
    yy, xx = np.nonzero(a > .5)
    if not len(yy): continue
    y0, y1, x0, x1 = yy.min(), yy.max() + 1, xx.min(), xx.max() + 1
    rgb = clean[ys, xs][y0:y1, x0:x1]; al = a[y0:y1, x0:x1]
    al = np.where(al > .9, 1, al)
    piece = np.dstack([rgb, al * 255]).clip(0, 255).astype(np.uint8)
    ph, pw = piece.shape[:2]
    p = spec.get(cn)
    uw, uh = (p[1], p[2]) if p else (16, 16)
    scale = (ph / uh) if uh <= uw else (pw / uw)      # one scale per piece, across its short side
    ent = {'w': int(pw), 'h': int(ph), 'scale': round(float(scale), 3), 'uw': uw, 'uh': uh,
           'fixed': bool(not p or p[5] == 'fixed'), 'core': bool(p and p[5] == 'core'), 'cost': 0.0 if icons else round(float(cost[c, s]), 2)}
    if p and p[5] not in ('fixed', 'tile'):     # a tile strip has no insets: tile.py makes it repeat along x
        bpx = p[3] * scale
        rgbf = rgb.astype(float)
        def inset(A):
            # A: the piece as a stack of cross-sections along one axis, from one edge inward. A plain stretch
            # has the same cross-section all along (a slow brightness gradient only shifts it, so each section's
            # mean is removed); an end plate, bracket or bolt changes its shape. The slice line goes where the
            # sections stop matching the typical one from the middle third, scanning outward from the middle.
            X = A.mean(2); X = X - X.mean(1, keepdims=True); n_ = len(X)
            ref = np.median(X[n_ // 3: 2 * n_ // 3], axis=0)
            dev = ndimage.median_filter(np.abs(X - ref).mean(1), size=3, mode='nearest')
            thr = max(6, 4 * np.median(dev[n_ // 3: 2 * n_ // 3]) + 3)
            k = n_ // 2
            while k > 0 and dev[k - 1] < thr: k -= 1
            return k
        cols, rows = rgbf.transpose(1, 0, 2), rgbf
        ins = [inset(rows), inset(cols[::-1]), inset(rows[::-1]), inset(cols)]
        ins = [max(v, .5 * bpx) for v in ins]
        ins = [min(ins[0], .4 * ph), min(ins[1], .4 * pw), min(ins[2], .4 * ph), min(ins[3], .4 * pw)]
        if uh <= 14 and uw > 3 * uh:     # a thin bar: equal end caps
            cap = max(ins[1], ins[3]); ins[1] = ins[3] = cap
        ent['slice'] = [int(round(v)) for v in ins]
    Image.fromarray(piece, 'RGBA').save(os.path.join(out, cn + '.png'))
    kit[cn] = ent; rep.append((cn, ent['cost'], pw, ph))
json.dump(kit, open(kp, 'w'), indent=1)
missing = [cn for cn, _ in cells if cn not in kit]
print(f'{name}: {len(rep)}/{len(cells)} cut, {len(shapes)} shapes; worst costs', sorted(rep, key=lambda t: -t[1])[:4], 'missing', missing)
