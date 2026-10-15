"""Make a cut part's middle repeat along x instead of stretching, so a part drawn wider than it was painted
keeps the painting's proportions.
usage: seamband.py <kit dir> <part> [<part> ...]
Reads <kit dir>/<part>.png as cut.py or gridslice.py wrote it, rewrites it and marks its kit.json entry `xtile`.

A stretched middle widens every scratch and chip with it: a 9-slice painted 140 units long and drawn 280 shows
its wear twice as wide as tall. Here the middle becomes ONE PERIOD of a seamless tile, which the renderer
repeats a whole number of times (hud-layout.md, *Parts*). The tile is learned from the middle itself by
tile.py's quilting, so it wraps continuously by construction. Its two joins to the fixed ends are minimum-error
seams through an overlap of `ov` pixels taken from the middle's own paint, so the ends keep their bevels:
  - a 9-slice's middle [l, W - r) gives up `ov` pixels at each side to the joins, and is replaced by the tile;
    the slice grows by `ov` on each side and the PNG becomes l + ov + L + ov + r pixels wide;
  - a grid part keeps its length, so its wells stay where they were measured: one period of the tile is
    joined into its x stretch band, `ov` pixels in from the band's start, and `xs` becomes that period; the
    rest of the painting is left as painted.
fill() is the operation both use, and a job script may call it to join separately painted pieces."""
import json, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tile import tile


def seam_join(A, B):
    """A and B: the same h x o columns from the left piece and the right piece. Returns them joined along the
    cheapest top-to-bottom path of their difference, feathered over 3 pixels: A left of it, B right of it."""
    h, o = A.shape[:2]
    rgbA, rgbB = A[..., :3] * A[..., 3:], B[..., :3] * B[..., 3:]
    E = ((rgbA - rgbB) ** 2).sum(2)
    lo, hi = min(3, o // 4), max(min(3, o // 4) + 1, o - 4)        # keep the seam off the overlap's ends
    M = np.full_like(E, np.inf); M[0, lo:hi] = E[0, lo:hi]
    for y in range(1, h):
        prev = np.minimum(np.minimum(M[y - 1], np.roll(M[y - 1], 1)), np.roll(M[y - 1], -1))
        M[y, lo:hi] = E[y, lo:hi] + prev[lo:hi]
    path = np.zeros(h, int); path[-1] = int(np.argmin(M[-1]))
    for y in range(h - 2, -1, -1):
        c = path[y + 1]; cand = [x for x in (c - 1, c, c + 1) if lo <= x < hi]
        path[y] = min(cand, key=lambda x: M[y, x])
    m = (np.arange(o)[None, :] > path[:, None]).astype(float)       # 1 = take B
    m = ndimage.uniform_filter1d(m, size=3, axis=1)[..., None]
    return (1 - m) * A + m * B


def fill(img, x0, x1, t, ov):
    """img (h x W x 4 floats, alpha 0..1) with columns [x0, x1) painted by the seamless tile t cycled from its
    first column at x0, joined to img at both ends through the overlaps [x0 - ov, x0) and [x1, x1 + ov), which
    must hold paint that continues across them (a plate, a rail), never a bevel."""
    L = t.shape[1]
    F = lambda a, b: t[:, np.arange(a - x0, b - x0) % L]
    out = img.copy()
    out[:, x0:x1] = F(x0, x1)
    out[:, x0 - ov:x0] = seam_join(img[:, x0 - ov:x0], F(x0 - ov, x0))
    out[:, x1:x1 + ov] = seam_join(F(x1, x1 + ov), img[:, x1:x1 + ov])
    return out


def nine(a, sl):
    t_, r, b_, l = sl
    W = a.shape[1]; n = W - r - l
    ov = max(8, n // 12)
    t, _ = tile(a[:, l + ov:W - r - ov])
    L = t.shape[1]
    body = np.concatenate([a[:, :l + ov], np.zeros_like(t), a[:, W - r - ov:]], 1)
    return fill(body, l + ov, l + ov + L, t, ov), [t_, r + ov, b_, l + ov], L, ov


def grid(a, e):
    (p, q), = e['xs']                                               # one x stretch band, in design units
    s = a.shape[1] / e['design'][0]
    x0, x1 = round(p * s), round(q * s); ov = max(8, (x1 - x0) // 12)
    t, _ = tile(a[:, x0 + ov:x1 - ov])
    L = t.shape[1]
    out = fill(a, x0 + ov, x0 + ov + L, t, ov)
    return out, [[round((x0 + ov) / s, 2), round((x0 + ov + L) / s, 2)]], L, ov


if __name__ == '__main__':
    kit, parts = sys.argv[1], sys.argv[2:]
    kp = os.path.join(kit, 'kit.json'); K = json.load(open(kp))
    for part in parts:
        a = np.asarray(Image.open(os.path.join(kit, part + '.png')).convert('RGBA')).astype(float)
        a[..., 3] /= 255
        e = K[part]
        if e.get('grid'):
            out, e['xs'], L, ov = grid(a, e)
        else:
            out, e['slice'], L, ov = nine(a, e['slice'])
        e['xtile'] = True
        if 'w' in e: e['w'] = out.shape[1]
        im = np.dstack([out[..., :3], out[..., 3:] * 255]).clip(0, 255).astype(np.uint8)
        Image.fromarray(im, 'RGBA').save(os.path.join(kit, part + '.png'))
        print(f"{part}: middle is one period of {L} px ({L / e['scale']:.1f} units), joins over {ov} px; "
              f"PNG {a.shape[1]} -> {im.shape[1]} px wide")
    json.dump(K, open(kp, 'w'), indent=1)
