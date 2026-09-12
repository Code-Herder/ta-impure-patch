"""Split render_frame into WHAT to draw and HOW to draw it.

The draw list is the same for every backend: a set of source images (few) and a
set of tiles (many) that sample them. Extracting it is what makes a CPU/GPU
comparison honest -- both paths get byte-identical sources and rects.
"""
import math
from PIL import Image


def draw_list(m, t, w, h):
    """Return (sources, tiles, bg). sources: {key: PIL.Image}.

    tiles: list of (key, X0, Y0, X1, Y1, sx0, sy0, sx1, sy1, alpha)
    plus 'flats': list of (col, X0, Y0, X1, Y1) for tiles too small to sample.
    """
    s = m.stage
    cx, cy, cols = m.camera.at(t)
    scale = w / (cols * s.pitch_x)
    sources, tiles, flats = {}, [], []

    for win in m.windows:
        sx, sy, sw, sh = win.rect()
        px = (sx - cx) * scale + w / 2.0
        py = (sy - cy) * scale + h / 2.0
        pw, ph = sw * scale, sh * scale
        if px + pw < 0 or py + ph < 0 or px > w or py > h:
            continue
        iw, ih = max(1, round(pw)), max(1, round(ph))
        a = win.alpha(t)
        if a <= 0.0:
            continue
        if iw <= 3 or ih <= 3:
            flats.append((win.game.avg_color(), int(px), int(py),
                          int(px) + iw, int(py) + ih))
            continue
        X0, Y0 = math.ceil(px), math.ceil(py)
        X1, Y1 = math.floor(px + pw), math.floor(py + ph)
        if X1 - X0 < 1 or Y1 - Y0 < 1:
            continue
        cw, ch = m_canonical(m, iw, ih, getattr(win.game, "native_w", None))
        lod_w = max(1, int(iw * 1920 / w))
        gt = round((t + win.phase) * 1000) / 1000.0
        key = (id(win.game), gt if win.term else round(gt * m.fps) / m.fps,
               cw, ch, lod_w)
        if key not in sources:
            src = win.content(t, cw, ch, lod_w)
            if src is None:
                continue
            sources[key] = src
        src = sources[key]
        ssw, ssh = src.size
        sx0 = max(0.0, min((X0 - px) / pw * ssw, ssw))
        sx1 = max(sx0, min((X1 - px) / pw * ssw, ssw))
        sy0 = max(0.0, min((Y0 - py) / ph * ssh, ssh))
        sy1 = max(sy0, min((Y1 - py) / ph * ssh, ssh))
        tiles.append((key, X0, Y0, X1, Y1, sx0, sy0, sx1, sy1, a))
    return sources, tiles, flats, s.bg


def m_canonical(m, iw, ih, native_w=None):
    """canonical_size lives in the module the Montage class came from."""
    import sys
    return sys.modules[m.__class__.__module__].canonical_size(iw, ih, native_w)


def cpu_composite(sources, tiles, flats, bg, w, h, resample=Image.BILINEAR):
    """The current path, isolated: one PIL resize + paste per distinct tile."""
    img = Image.new("RGB", (w, h), bg)
    tcache = {}
    for col, X0, Y0, X1, Y1 in flats:
        img.paste(col, (X0, Y0, X1, Y1))
    for key, X0, Y0, X1, Y1, sx0, sy0, sx1, sy1, a in tiles:
        tkey = (key, X1 - X0, Y1 - Y0, round(sx0 * 4), round(sy0 * 4))
        tile = tcache.get(tkey)
        if tile is None:
            tile = sources[key].resize((X1 - X0, Y1 - Y0), resample,
                                       box=(sx0, sy0, sx1, sy1))
            tcache[tkey] = tile
        if a >= 1.0:
            img.paste(tile, (X0, Y0))
        else:
            under = img.crop((X0, Y0, X1, Y1))
            img.paste(Image.blend(under, tile, a), (X0, Y0))
    return img
