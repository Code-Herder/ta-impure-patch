"""Cut icons from a painted icon sheet as whole shapes, in reading order.
usage: [ICON_DIL=px] iconcut.py <painted.png> <out dir> <name> [<name> ...]   (ICON_DIL joins strokes that far apart; default 12)
The N largest painted shapes (strokes joined by a small dilation) become the N icons, top row first,
left to right. Unlike cutting from widened whitebox cells, this does not depend on the painting
keeping the sheet's aspect ratio."""
import json, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from keying import despill

src, out, names = sys.argv[1], sys.argv[2], sys.argv[3:]
im = np.asarray(Image.open(src).convert('RGB')).astype(float)
r, g, b = im[..., 0], im[..., 1], im[..., 2]
mag = np.clip((np.minimum(r, b) - g - 60) / 120, 0, 1) * np.clip(1 - np.abs(r - b) / 90, 0, 1)
fg = mag < .5
lab, n = ndimage.label(ndimage.binary_dilation(fg, iterations=int(os.environ.get("ICON_DIL", 12))))
objs = ndimage.find_objects(lab); areas = ndimage.sum(fg, lab, range(1, n + 1))
pick = list(np.argsort(-areas)[:len(names)])
# reading order: icons whose vertical centres lie within half an icon height of each other share a row
rowh = max(objs[i][0].stop - objs[i][0].start for i in pick)
cy = {i: (objs[i][0].start + objs[i][0].stop) / 2 for i in pick}
row, last, rows = -1, None, {}
for i in sorted(pick, key=lambda i: cy[i]):
    if last is None or cy[i] - last > rowh / 2: row += 1
    rows[i], last = row, cy[i]
pick.sort(key=lambda i: (rows[i], objs[i][1].start))
solid = mag < .05
_, (iy, ix) = ndimage.distance_transform_edt(~solid, return_indices=True); clean = despill(im)[iy, ix]
kp = os.path.join(out, 'kit.json'); K = json.load(open(kp)) if os.path.exists(kp) else {}
for name, i in zip(names, pick):
    ys, xs = objs[i]; a = (1 - mag)[ys, xs] * (lab[ys, xs] == i + 1)
    yy, xx = np.nonzero(a > .5); y0, y1, x0, x1 = yy.min(), yy.max() + 1, xx.min(), xx.max() + 1
    al = a[y0:y1, x0:x1]; al = np.where(al > .9, 1, al)
    Image.fromarray(np.dstack([clean[ys, xs][y0:y1, x0:x1], al * 255]).clip(0, 255).astype(np.uint8), 'RGBA').save(os.path.join(out, name + '.png'))
    K[name] = {'w': int(x1 - x0), 'h': int(y1 - y0), 'scale': round(max(x1 - x0, y1 - y0) / 16, 3), 'fixed': True, 'core': False}
    print(name, x1 - x0, y1 - y0)
json.dump(K, open(kp, 'w'), indent=1)
