"""Make a cut strip repeat seamlessly along x, so a fill can cover any length without being stretched.
usage: tile.py <kit dir> <part> [<part> ...]
Reads <kit dir>/<part>.png as cut.py wrote it, writes the tile over it and marks its kit.json entry `tile`.

The tile is a slice seg[0:L] of the painted strip whose first o columns are replaced by the strip's own
continuation seg[L:L+o], joined to seg[0:o] along a minimum-error seam. The wrap is continuous by
construction: the tile's last column is seg[L-1] and its first is seg[L], neighbours in the painting; the
seam, inside the tile, joins two stretches of paint that were chosen to look alike there. L is the length
whose overlap matches best; the seam is the cheapest top-to-bottom path through the overlap (image
quilting), feathered over a few pixels. Nothing is blended across the whole overlap, so the wear keeps its
contrast instead of ghosting."""
import json, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage

def tile(a):
    """a: the strip as h x w x 4 floats; returns the seamless tile and its seam numbers."""
    cov = a[..., 3].mean(0)
    full = np.nonzero(cov >= .97 * np.median(cov))[0]
    x0, x1 = full.min(), full.max() + 1
    trim = int(.04 * (x1 - x0)); seg = a[:, x0 + trim: x1 - trim]       # the painted ends are left out
    h, n, _ = seg.shape; o = max(24, int(.12 * n))
    rgb = seg[..., :3] * seg[..., 3:]                                   # compare paint, not key
    best = None
    for L in range(int(.55 * n), n - o):
        e = float(((rgb[:, L:L + o] - rgb[:, :o]) ** 2).mean())
        if best is None or e < best[0]: best = (e, L)
    L = best[1]
    E = ((rgb[:, L:L + o] - rgb[:, :o]) ** 2).sum(2)
    lo, hi = 3, o - 4                                                   # keep the seam off the overlap's ends
    M = np.full_like(E, np.inf); M[0, lo:hi] = E[0, lo:hi]
    for y in range(1, h):
        prev = np.minimum(np.minimum(M[y - 1], np.roll(M[y - 1], 1)), np.roll(M[y - 1], -1))
        M[y, lo:hi] = E[y, lo:hi] + prev[lo:hi]
    path = np.zeros(h, int); path[-1] = int(np.argmin(M[-1]))
    for y in range(h - 2, -1, -1):
        c = path[y + 1]; cand = [x for x in (c - 1, c, c + 1) if lo <= x < hi]
        path[y] = min(cand, key=lambda x: M[y, x])
    m = (np.arange(o)[None, :] > path[:, None]).astype(float)          # 1 = take the strip's start
    m = ndimage.uniform_filter1d(m, size=3, axis=1)                     # a 3-pixel feather; the ends stay 0 and 1
    t = seg[:, :L].copy()
    t[:, :o] = (1 - m[..., None]) * seg[:, L:L + o] + m[..., None] * seg[:, :o]
    adj = np.abs(np.diff(t[..., :3], axis=1)).mean((0, 2))
    return t, {'length': L, 'overlap': o, 'of': n, 'wrap_step': round(float(np.abs(t[:, 0, :3] - t[:, -1, :3]).mean()), 1),
               'median_step': round(float(np.median(adj)), 1), 'seam_err': round(best[0] ** .5, 1)}

if __name__ == '__main__':
    kit, parts = sys.argv[1], sys.argv[2:]
    kp = os.path.join(kit, 'kit.json'); K = json.load(open(kp))
    for part in parts:
        a = np.asarray(Image.open(os.path.join(kit, part + '.png')).convert('RGBA')).astype(float)
        a[..., 3] /= 255
        t, info = tile(a)
        out = np.dstack([t[..., :3], t[..., 3:] * 255]).clip(0, 255).astype(np.uint8)
        Image.fromarray(out, 'RGBA').save(os.path.join(kit, part + '.png'))
        e = K[part]; e.update({'w': out.shape[1], 'h': out.shape[0], 'tile': True, 'fixed': False, 'core': False})
        e.pop('slice', None)
        print(f"{part}: tile {out.shape[1]} x {out.shape[0]} px from {info['of']}, overlap {info['overlap']}; "
              f"wrap step {info['wrap_step']} against a median step of {info['median_step']} (0-255)")
    json.dump(K, open(kp, 'w'), indent=1)
