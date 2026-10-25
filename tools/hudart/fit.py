"""Fits every part of art/hud/ to the most the game ever draws of it: each PNG resampled down until no more than
0.9 device pixel falls on one of its texels at the HUD's densest, a 3840x2160 screen at HUD 150 (4.5 device pixels a
unit), and never up. The parts are cut at about 10 px a unit, more than twice what any of them is drawn at, and the
DLL embeds them all (tools/hudpak.py).
usage: fit.py

THE DENSITY A PART NEEDS IS MEASURED, not assumed: the mockup draws laycheck.py's cases and the housing without the
proposed features (which only the mockup draws), and every piece of every part is taken at its device pixels against
its texels, scaled to 4.5 px a unit from the case's own density. laycheck.py proves the game draws the same pieces.
  - Along a stretch band -- a 9-slice's middle, a grid's xs or ys band -- a piece is uniform by construction, so
    that axis is not counted.
  - A band that repeats (`xtile`) is counted at the most a copy can be stretched: 1.5 x the density of the part's
    fixed cells, because the copies are the nearest whole number of periods, at least one (tagpu_hudlay.c
    `copies`). A sampled case would show only the lengths it happens to draw.
  - A part no case draws stops the run: an unmeasured part cannot be fitted.

Each part with a need under 0.97 x 0.9 is resampled by f = need / 0.9 (Lanczos, which Pillow applies to RGBA on
premultiplied colour) and its kit entry rescaled with it: `scale` always, a 9-slice's `slice`, a repeating grid
band's `xs`. A repeating band is resampled on its own, wrapped, so its copies still meet without a seam, and a
repeating 9-slice's end slices are made to land on whole texels. A fitted part measures 0.9 again, so a second run
changes nothing; a part filed again by file.py is fitted again from its new painting."""
import json, sys, tempfile
from pathlib import Path
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import laycheck

ART = laycheck.ROOT / 'art' / 'hud'
DENSEST = 4.5           # device px a unit: 2160 / 720 x 150 %, tagpu_hudstate.c
FILL = 0.9              # the most device pixels a texel may cover there
COPY_STRETCH = 1.5      # the most an xtile copy is drawn over its painted length
EXTRA = [("4k-150-no-proposed", "res=3840x2160&game=150&hud=150&np=10&nt=2&t=70&prop=0&paused", None, "")]


def bands(e, pw, ph):
    """A part's stretch bands as texture fractions along x and y, and its repeating band along x or None."""
    if e['kind'] == 'nine':
        t, r, b, l = e['slice']
        mid = (l / pw, 1 - r / pw)
        return ([] if e.get('xtile') else [mid]), [(t / ph, 1 - b / ph)], (mid if e.get('xtile') else None)
    if e['kind'] == 'grid':
        D = e['design']
        xs = [(a / D[0], c / D[0]) for a, c in e['xs']]
        return ([] if e.get('xtile') else xs), [(a / D[1], c / D[1]) for a, c in e['ys']], (xs[0] if e.get('xtile') else None)
    return [], [], None


def within(a, b, bl):
    lo, hi = min(a, b), max(a, b)
    return any(lo >= x - 1e-4 and hi <= y + 1e-4 for x, y in bl)


def measure(kit, size):
    """Each part's need: the most device pixels one texel covers at DENSEST, over every case and piece."""
    need, where = {}, {}
    httpd = laycheck.serve()
    try:
        with tempfile.TemporaryDirectory(prefix='hudfit') as tmp:
            for name, query, hover, held, *dpr in laycheck.CASES + EXTRA:
                dom = laycheck.mockup(httpd.server_address[1], query, hover, held, Path(tmp), *dpr)
                norm = DENSEST / (dom['u'] * dom['d'])
                for el in dom['els']:
                    if el['kind'] != 'part' or el['part'] not in kit: continue
                    n = el['part']; e = kit[n]; pw, ph = size[n]
                    if el['quads']:
                        pieces = el['quads']
                    else:       # a tiled fill: scaled to its box's height
                        x0, y0, x1, y1 = el['dev']
                        pieces = [[0, 0, (y1 - y0) * pw / ph, y1 - y0, 0, 0, 1, 1]]
                    xb, yb, rep = bands(e, pw, ph)
                    ms, fixed_x = [], 0
                    for x0, y0, x1, y1, u0, v0, u1, v1 in pieces:
                        tx, ty = abs(u1 - u0) * pw, abs(v1 - v0) * ph
                        if tx < 1e-6 or ty < 1e-6 or x1 <= x0 or y1 <= y0: continue
                        mx, my = (x1 - x0) / tx * norm, (y1 - y0) / ty * norm
                        copy = rep is not None and within(u0, u1, [rep])
                        if not copy and not within(u0, u1, xb): fixed_x = max(fixed_x, mx); ms.append(mx)
                        if copy: ms.append(mx)
                        if not within(v0, v1, yb): ms.append(my)
                    if rep is not None: ms.append(COPY_STRETCH * fixed_x)
                    m = max(ms, default=0)
                    if m > need.get(n, 0): need[n], where[n] = m, name
    finally:
        httpd.shutdown()
    return need, where


def wrapped(im, w, h):
    """A seamless period resampled with its own other end as each end's neighbour."""
    W = im.width
    tri = Image.new(im.mode, (3 * W, im.height))
    for i in range(3): tri.paste(im, (i * W, 0))
    return tri.resize((3 * w, h), Image.LANCZOS).crop((w, 0, 2 * w, h))


def columns(im, cuts, widths, h, rep):
    """The image cut at x `cuts`, each column resampled on its own to its width, column `rep` wrapped."""
    edges = [0, *cuts, im.width]
    out = Image.new(im.mode, (sum(widths), h))
    x = 0
    for i, w in enumerate(widths):
        col = im.crop((edges[i], 0, edges[i + 1], im.height))
        out.paste(wrapped(col, w, h) if i == rep else col.resize((w, h), Image.LANCZOS), (x, 0))
        x += w
    return out


def whole_texels(f, lengths):
    """The factor from f up to 2 % above it that lands every length nearest a whole number of texels. No more
    than 2 %: the part then measures at least FILL / 1.02, which the next run's 0.97 keeps as it is."""
    cands = [f * (1 + i / 2000) for i in range(41)]
    return min(cands, key=lambda c: (round(max(abs(L * c - round(L * c)) for L in lengths), 3), c))


def fit(im, e, f):
    """The part resampled by about f, and its kit entry rescaled to match."""
    pw, ph = im.size
    e = dict(e)
    H = max(1, round(ph * f))
    if e['kind'] == 'nine' and e.get('xtile'):
        t, r, b, l = e['slice']
        f = whole_texels(f, [l, r])
        H = max(1, round(ph * f))
        L, R, M = max(1, round(l * f)), max(1, round(r * f)), max(1, round((pw - l - r) * f))
        out = columns(im, [l, pw - r], [L, M, R], H, 1)
        e['slice'] = [round(t * H / ph, 3), R, round(b * H / ph, 3), L]
        e['scale'] = round(e['scale'] * f, 3)
    elif e['kind'] == 'grid' and e.get('xtile'):
        D = e['design'][0]
        X0, X1 = round(e['xs'][0][0] * pw / D), round(e['xs'][0][1] * pw / D)
        ws = [max(1, round(X0 * f)), max(1, round((X1 - X0) * f)), max(1, round((pw - X1) * f))]
        out = columns(im, [X0, X1], ws, H, 1)
        W = sum(ws)
        e['xs'] = [[round(ws[0] * D / W, 3), round((ws[0] + ws[1]) * D / W, 3)]]
        e['scale'] = round(W / D, 3)
    elif e['kind'] == 'tile':
        out = wrapped(im, max(1, round(pw * f)), H)
        e['scale'] = round(e['scale'] * H / ph, 3)
    else:
        W = max(1, round(pw * f))
        out = im.resize((W, H), Image.LANCZOS)
        if e['kind'] == 'nine':
            t, r, b, l = e['slice']
            e['slice'] = [round(t * H / ph, 3), round(r * W / pw, 3), round(b * H / ph, 3), round(l * W / pw, 3)]
        e['scale'] = round(e['scale'] * W / pw, 3)
    return out, e


def main():
    kp = ART / 'kit.json'
    K = json.load(open(kp))
    kit = K['parts']
    size = {n: Image.open(ART / 'parts' / f'{n}.png').size for n in kit}
    need, where = measure(kit, size)
    missing = sorted(set(kit) - set(need))
    if missing: sys.exit(f'fit: no case draws {", ".join(missing)}; add one to laycheck.py CASES or EXTRA here')
    before = after = 0
    for n in sorted(kit, key=lambda n: -need[n]):
        p = ART / 'parts' / f'{n}.png'
        before += p.stat().st_size
        f = need[n] / FILL
        note = 'kept'
        if f < 0.97:
            im = Image.open(p).convert('RGBA')
            out, kit[n] = fit(im, kit[n], f)
            out.save(p, optimize=True, compress_level=9)
            note = f'{im.width}x{im.height} -> {out.width}x{out.height}'
        after += p.stat().st_size
        print(f'{n:22s} {kit[n]["kind"]:5s} need {need[n]:5.3f} ({where[n]}) {note}')
    json.dump(K, open(kp, 'w'), indent=1)
    print(f'{len(kit)} parts, {before / 1e6:.2f} MB -> {after / 1e6:.2f} MB')


if __name__ == '__main__':
    main()
