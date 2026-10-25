#!/usr/bin/env python3
"""gfx.py -- the in-game wiki mockup's game graphics, rendered from the player's own install.

    .venv-undither/bin/python tools/wiki/gfx.py              # the install found as below
    .venv-undither/bin/python tools/wiki/gfx.py --gamedir <TA install>

Writes tools/wiki/mockup/gfx/ -- gitignored, because everything in it is derived from the
game's own files and nothing derived from the game is ever committed (CLAUDE.md, *Nothing from
the original game, ever*). The mockup's `gfx=game` switch reads gfx/index.json and draws these
instead of its whitebox; without the folder it stays on the whitebox.

What it writes, all at one scale (`ppu` canvas pixels a world unit; the page scales to its stage):

  units/<name>.png   every unit an art/wiki/entries/*.json `units` names: its 3DO in the rest pose,
                     at TA's own oblique camera (screen y = world z - height / 2, the shear of 0.5
                     tascene and the engine use), facing 0 (TA's default heading, toward the
                     viewer), flat-shaded per face by the engine's PALETTE.SHD rows as tascene's
                     parity lane does it, textured with each GAF's frame 0 -- player 0's colours,
                     the ARM blue. `anchor` is the pixel of the unit's world position.
  ground.png         the most used tileable 32 x 32 tile of a lush map's flat, featureless ground
  metalground.png    the same from a metal-world map, for metal ground at SurfaceMetal or broad
                     above-background regions
  metal.png          the lush metal deposit feature's sprite (3 x 3 cells, `anchor` = its hotspot)
  vent.png           the lush geothermal vent feature's sprite, frame 0 (1 x 1 cell)
  cursors/<n>.png    cursornormal, cursormove, cursorselect, cursorfindsite, cursorgrn, cursorred
                     from anims/cursors.gaf, frame 0, at their own pixels with their hotspots
  index.json         all of the above with sizes and anchors

The readers are the repository's own: tools/ta3do (HPI, palette, GAF, 3DO, the COB Create-hide
scan) and tools/tascene (TNT, feature definitions, GAF hotspots, the SHD calibration). Archives
load in ta3do's order, then rev31.gp3's files again, so its copy wins over any .ufo.
Colour is palette colour: no restorer runs.
"""

import argparse
import importlib.util
import json
import math
import os
import sys
from collections import Counter
from importlib.machinery import SourceFileLoader
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
REPO = TOOLS.parent
OUT = HERE / "mockup" / "gfx"
ENTRIES = REPO / "art" / "wiki" / "entries"


def _load(name, path):
    loader = SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


tascene = _load("tascene", TOOLS / "tascene")
ta3do = tascene.ta3do


def die(msg):
    print(f"gfx.py: {msg}", file=sys.stderr)
    raise SystemExit(1)


def find_install(arg):
    """--gamedir, else TA3DO_GAMEDIR, else Steam's default library, else ta3do's gamedir."""
    for cand in (arg, os.environ.get("TA3DO_GAMEDIR"),
                 Path.home() / ".local/share/Steam/steamapps/common/Total Annihilation"):
        if cand and Path(cand).expanduser().is_dir():
            return Path(cand).expanduser()
    return ta3do.game_dir()


def open_assets(root):
    assets = ta3do.Assets(root)
    for arc in assets.archives:                 # rev31.gp3 wins, .ufo included
        if arc.path.name.lower() == "rev31.gp3":
            for name in arc.files:
                assets.index[name] = arc
    return assets


# --------------------------------------------------------------------------- shading

SH_V = (0.0, 0.8944, -0.4472)      # tagpu_render3do.c: the model-space toward-camera axis
SH_L = (-0.35, 0.80, -0.49)        # and the light (tascene-view.html, the parity lane)


def shade_k(shd, palette, neutral):
    """tagpu_render3do.c's s_shadeK, as tascene-view.html's shadeK: per SHD row, the least-squares
    slope through the origin of the shaded colour against the unshaded one over every channel the
    row does not clip (>= 250), normalised so the neutral row is 1.0."""
    raw = []
    for r in range(32):
        sxy = sxx = 0.0
        for i in range(256):
            y = palette[shd[r * 256 + i]]
            for c in range(3):
                if y[c] >= 250:
                    continue
                sxy += palette[i][c] * y[c]
                sxx += palette[i][c] * palette[i][c]
        raw.append(sxy / sxx if sxx > 0 else 1.0)
    n = raw[neutral] if raw[neutral] > 0 else 1.0
    return [v / n for v in raw]


# --------------------------------------------------------------------------- the 3DO raster

def model_faces(assets, bank, name):
    """[(points[(x,y,z)], texture frame or None, flat palette index)] in model space, the rest
    pose: pieces at their offsets, what the script hides at Create left out, the selection
    primitive skipped -- tascene's build_units rule."""
    label, blob = ta3do.resolve_model(assets, name)
    root = ta3do.parse_3do(blob)
    hidden = {h.lower() for h in ta3do.script_hidden(assets, label) or ()}
    faces = []

    def emit(piece, ox, oy, oz):
        px, py, pz = ox + piece.offset[0], oy + piece.offset[1], oz + piece.offset[2]
        if piece.name.lower() not in hidden:
            for prim in piece.primitives:
                if prim.is_selection or len(prim.indices) < 3:
                    continue
                if any(i >= len(piece.vertices) for i in prim.indices):
                    continue
                frame = bank.frame(prim.texture) if prim.texture else None
                if prim.texture and frame is None:
                    continue                    # names a texture we cannot resolve
                pts = [(px + piece.vertices[i][0], py + piece.vertices[i][1],
                        pz + piece.vertices[i][2]) for i in prim.indices]
                faces.append((pts, frame, prim.color & 0xFF))
        for child in piece.children:
            emit(child, px, py, pz)

    emit(root, 0.0, 0.0, 0.0)
    return label, faces


def corner_uv(slot, n):
    """The engine maps a whole GAF frame across a face's four corners; an n-gon collapses onto
    them (tascene build_units)."""
    c = slot if n <= 4 else slot * 4 // n
    return (1.0 if c in (1, 2) else 0.0, 1.0 if c >= 2 else 0.0)


def render_model(faces, palette, shd_k, neutral, direction, ppu, ss):
    """RGBA at ppu pixels a world unit, and the anchor (the model origin's pixel).

    Facing 0 is TA's default build heading 0x8000, half a turn: posed (x, y, z) = (-x, y, -z).
    Screen: x = -x_model... i.e. sx = posed x, sy = -posed z - y / 2 (tascene-view.html). The
    depth key is 2y - posed z, larger nearer, per pixel. Quads take per-pixel bilinear texture
    coordinates (the Vulkan path's quad mapping), other faces fan from vertex 0 with affine ones."""
    k = ppu * ss
    tris = []
    allx, ally = [], []
    for pts, frame, col in faces:
        posed = [(-x, y, -z) for x, y, z in pts]
        scr = [(p[0] * k, (-p[2] - p[1] * 0.5) * k, 2 * p[1] - p[2]) for p in posed]
        # per-face flat shade, tagpu_render3do.c's rule as tascene-view.html ports it
        a, b, c = posed[0], posed[1], posed[2]
        e1 = [b[i] - a[i] for i in range(3)]
        e2 = [c[i] - a[i] for i in range(3)]
        nx = e1[1] * e2[2] - e1[2] * e2[1]
        ny = e1[2] * e2[0] - e1[0] * e2[2]
        nz = e1[0] * e2[1] - e1[1] * e2[0]
        if nx * SH_V[0] + ny * SH_V[1] + nz * SH_V[2] < 0:
            nx, ny, nz = -nx, -ny, -nz
        nl = math.sqrt(nx * nx + ny * ny + nz * nz)
        row = neutral
        if nl > 1e-6:
            inten = (nx * SH_L[0] + ny * SH_L[1] + nz * SH_L[2]) / nl
            row = max(0, min(31, neutral + direction * math.floor(inten * 12 + 0.5)))
        tris.append((scr, frame, col, shd_k[row]))
        allx += [s[0] for s in scr]
        ally += [s[1] for s in scr]
    if not tris:
        return None, (0, 0)
    pad = 2 * ss
    x0 = math.floor(min(allx)) - pad
    y0 = math.floor(min(ally)) - pad
    x0 -= x0 % ss
    y0 -= y0 % ss
    w = math.ceil(max(allx)) + pad - x0
    h = math.ceil(max(ally)) + pad - y0
    w += (-w) % ss
    h += (-h) % ss
    rgb = np.zeros((h, w, 3), np.float32)
    cov = np.zeros((h, w), np.float32)
    depth = np.full((h, w), -np.inf, np.float32)
    pal = np.array(palette, np.float32)

    for scr, frame, col, shade in tris:
        n = len(scr)
        P = [(s[0] - x0, s[1] - y0, s[2]) for s in scr]
        uv = [corner_uv(i, n) for i in range(n)]
        bx0 = max(0, int(math.floor(min(p[0] for p in P))))
        by0 = max(0, int(math.floor(min(p[1] for p in P))))
        bx1 = min(w, int(math.ceil(max(p[0] for p in P))) + 1)
        by1 = min(h, int(math.ceil(max(p[1] for p in P))) + 1)
        if bx1 <= bx0 or by1 <= by0:
            continue
        gx, gy = np.meshgrid(np.arange(bx0, bx1) + 0.5, np.arange(by0, by1) + 0.5)
        tex = None
        if frame is not None:
            tex = np.frombuffer(bytes(frame.pixels), np.uint8).reshape(frame.height, frame.width)
        quad_uv = bilinear_uv(P, gx, gy) if (n == 4 and tex is not None) else None
        for kk in range(1, n - 1):
            A, B, C = P[0], P[kk], P[kk + 1]
            area = (B[0] - A[0]) * (C[1] - A[1]) - (B[1] - A[1]) * (C[0] - A[0])
            if abs(area) < 1e-9:
                continue
            w0 = ((B[0] - gx) * (C[1] - gy) - (B[1] - gy) * (C[0] - gx)) / area
            w1 = ((C[0] - gx) * (A[1] - gy) - (C[1] - gy) * (A[0] - gx)) / area
            w2 = 1.0 - w0 - w1
            inside = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
            if not inside.any():
                continue
            z = w0 * A[2] + w1 * B[2] + w2 * C[2]
            sub = depth[by0:by1, bx0:bx1]
            win = inside & (z > sub)
            if tex is not None:
                if quad_uv is not None:
                    u, v = quad_uv
                else:
                    u = w0 * uv[0][0] + w1 * uv[kk][0] + w2 * uv[kk + 1][0]
                    v = w0 * uv[0][1] + w1 * uv[kk][1] + w2 * uv[kk + 1][1]
                tx = np.clip((u * frame.width).astype(np.int32), 0, frame.width - 1)
                ty = np.clip((v * frame.height).astype(np.int32), 0, frame.height - 1)
                idx = tex[ty, tx]
                if frame.transparent is not None:
                    win &= idx != frame.transparent
                colour = pal[idx]
            else:
                colour = np.broadcast_to(pal[col], gx.shape + (3,))
            if not win.any():
                continue
            sub[win] = z[win]
            rgb[by0:by1, bx0:bx1][win] = np.clip(colour[win] * shade, 0, 255)
            cov[by0:by1, bx0:bx1][win] = 1.0

    # down to ppu: coverage-weighted colour, coverage as alpha
    H, W = h // ss, w // ss
    c4 = cov.reshape(H, ss, W, ss).sum(axis=(1, 3))
    r4 = (rgb * cov[..., None]).reshape(H, ss, W, ss, 3).sum(axis=(1, 3))
    out = np.zeros((H, W, 4), np.uint8)
    nz = c4 > 0
    out[..., :3][nz] = np.clip(r4[nz] / c4[nz][:, None] + 0.5, 0, 255).astype(np.uint8)
    out[..., 3] = np.clip(c4 / (ss * ss) * 255 + 0.5, 0, 255).astype(np.uint8)
    return out, (-x0 / ss, -y0 / ss)


def bilinear_uv(P, gx, gy):
    """Inverse bilinear (u, v) of every pixel in the quad P0 (0,0) P1 (1,0) P2 (1,1) P3 (0,1)."""
    a = np.array(P[0][:2]), np.array(P[1][:2]), np.array(P[2][:2]), np.array(P[3][:2])
    e = a[1] - a[0]
    f = a[3] - a[0]
    g = a[0] - a[1] + a[2] - a[3]
    hx, hy = gx - a[0][0], gy - a[0][1]
    cross = lambda p, q: p[0] * q[1] - p[1] * q[0]
    k2 = cross(g, f)
    k1 = cross(e, f) + (hx * g[1] - hy * g[0])
    k0 = hx * e[1] - hy * e[0]
    if abs(k2) < 1e-6:
        v = np.where(np.abs(k1) > 1e-9, -k0 / np.where(np.abs(k1) > 1e-9, k1, 1), 0)
    else:
        disc = np.sqrt(np.maximum(k1 * k1 - 4 * k0 * k2, 0))
        v1 = (-k1 - disc) / (2 * k2)
        v2 = (-k1 + disc) / (2 * k2)
        v = np.where((v1 >= -0.01) & (v1 <= 1.01), v1, v2)
    den_x = e[0] + g[0] * v
    den_y = e[1] + g[1] * v
    u = np.where(np.abs(den_x) > np.abs(den_y),
                 (hx - f[0] * v) / np.where(np.abs(den_x) > 1e-9, den_x, 1),
                 (hy - f[1] * v) / np.where(np.abs(den_y) > 1e-9, den_y, 1))
    return np.clip(u, 0, 0.9999), np.clip(v, 0, 0.9999)


# --------------------------------------------------------------------------- GAF and tiles

def indexed_rgba(pixels, w, h, palette, key=None):
    idx = np.frombuffer(bytes(pixels), np.uint8).reshape(h, w)
    pal = np.array(palette, np.uint8)
    out = np.zeros((h, w, 4), np.uint8)
    out[..., :3] = pal[idx]
    out[..., 3] = 255
    if key is not None:
        out[..., 3][idx == key] = 0
    return out


def upscale(rgba, k):
    return np.repeat(np.repeat(rgba, k, axis=0), k, axis=1)


def flat_tile(assets, palette, pick):
    """The most used tileable 32 x 32 tile under flat, featureless ground, from the first map
    `pick` accepts by its feature names: (map, tile index, its pixels)."""
    for path in assets.glob("maps/*.tnt"):
        try:
            tnt = tascene.Tnt(assets.read(path), path)
        except Exception:
            continue
        if not pick([n.lower() for n in tnt.feature_names()]):
            continue
        attr = tnt.blob[tnt.p_mapattr:tnt.p_mapattr + tnt.w16 * tnt.h16 * 4]
        tiles = tnt.tilemap()
        use = Counter()
        for r in range(tnt.mh):
            for c in range(tnt.mw):
                cells = [((2 * r + j) * tnt.w16 + 2 * c + i) * 4 for j in (0, 1) for i in (0, 1)]
                hs = {attr[o] for o in cells}
                if len(hs) != 1 or any((attr[o + 1] | attr[o + 2] << 8) < 0xFFFB for o in cells):
                    continue
                use[tiles[2 * (r * tnt.mw + c)] | tiles[2 * (r * tnt.mw + c) + 1] << 8] += 1
        for t, _n in use.most_common(40):
            px = tnt.blob[tnt.p_tilegfx + t * 1024:tnt.p_tilegfx + (t + 1) * 1024]
            if tascene._tileable(px, 32, 32, -1, palette):
                return path, t, px
    return None


def gaf_sprite(gafs, seq, palette):
    got = gafs.frame(seq)
    if not got:
        return None
    frame, hx, hy = got
    return indexed_rgba(frame.pixels, frame.width, frame.height, palette, frame.transparent), (hx, hy)


CURSORS = {"arrow": "cursornormal", "move": "cursormove", "select": "cursorselect",
           "findsite": "cursorfindsite", "green": "cursorgrn", "red": "cursorred"}


def cursor_sprites(assets, palette):
    blob = assets.read("anims/cursors.gaf")
    entries = ta3do.gaf_entries(blob)
    out = {}
    for key, seq in CURSORS.items():
        off = entries.get(seq.upper())
        if off is None:
            continue
        frame = ta3do.gaf_frame(blob, off)
        hx, hy = tascene.gaf_hotspot(blob, off)
        out[key] = (seq, indexed_rgba(frame.pixels, frame.width, frame.height, palette,
                                      frame.transparent), (hx, hy))
    return out


# --------------------------------------------------------------------------- main

def entry_units():
    names = []
    for path in sorted(ENTRIES.glob("*.json")):
        for name in json.loads(path.read_text()).get("units", {}):
            if name not in names:
                names.append(name)
    return names


def save(rel, rgba):
    path = OUT / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgba, "RGBA").save(path, optimize=True)
    return {"file": rel, "w": int(rgba.shape[1]), "h": int(rgba.shape[0])}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gamedir", help="the Total Annihilation install (default: TA3DO_GAMEDIR, "
                                      "else Steam's default library)")
    ap.add_argument("--ppu", type=int, default=3, help="pixels a world unit (default 3)")
    ap.add_argument("--ss", type=int, default=3, help="supersampling of the 3DO raster (default 3)")
    args = ap.parse_args(argv)

    root = find_install(args.gamedir)
    assets = open_assets(root)
    palette = ta3do.load_palette(assets)
    shd = assets.read("palettes/palette.shd")
    neutral, direction = tascene.shd_neutral(shd, palette)
    shd_k = shade_k(shd, palette, neutral)
    bank = ta3do.TextureBank(assets)
    defs = tascene.FeatureDefs(assets)
    gafs = tascene.GafBank(assets)
    ppu = args.ppu
    OUT.mkdir(parents=True, exist_ok=True)
    index = {"format": 1, "ppu": ppu,
             "note": "derived from the local install by tools/wiki/gfx.py; gitignored, never committed",
             "units": {}, "cursors": {}}

    for name in entry_units():
        label, faces = model_faces(assets, bank, name)
        img, (ax, ay) = render_model(faces, palette, shd_k, neutral, direction, ppu, args.ss)
        if img is None:
            print(f"gfx.py: {name}: no faces", file=sys.stderr)
            continue
        rec = save(f"units/{name.lower()}.png", img)
        rec.update(anchor=[round(ax, 2), round(ay, 2)], model=label, faces=len(faces))
        index["units"][name] = rec
        print(f"  {name:9} {rec['w']}x{rec['h']}  anchor {rec['anchor']}  {len(faces)} faces")

    for key, pick in (("ground", lambda ns: any(n.startswith("lushmetal") for n in ns)
                       and any(n.startswith("lushvent") for n in ns)),
                      ("metalground", lambda ns: any(n.startswith("metalvent") for n in ns))):
        got = flat_tile(assets, palette, pick)
        if not got:
            print(f"gfx.py: no tileable flat tile for {key}", file=sys.stderr)
            continue
        path, tile, px = got
        rec = save(f"{key}.png", upscale(indexed_rgba(px, 32, 32, palette), ppu))
        rec.update(world=32, map=Path(path).stem, tile=tile)
        index[key] = rec
        print(f"  {key:11} {Path(path).stem}, tile {tile}")

    for key, feat in (("metal", "lushmetal3"), ("vent", "lushvent01")):
        body = defs.get(feat) or {}
        got = gaf_sprite(gafs, body.get("seqname", ""), palette)
        if not got:
            print(f"gfx.py: no sprite for {feat}", file=sys.stderr)
            continue
        img, (hx, hy) = got
        rec = save(f"{key}.png", upscale(img, ppu))
        rec.update(anchor=[hx * ppu, hy * ppu], feature=feat,
                   footprint=[int(body.get("footprintx", 1)), int(body.get("footprintz", 1))])
        if body.get("metal"):
            rec["metal"] = int(body["metal"])
        index[key] = rec
        print(f"  {key:11} {feat} ({body.get('filename')}.gaf {body.get('seqname')})")

    for key, (seq, img, (hx, hy)) in cursor_sprites(assets, palette).items():
        rec = save(f"cursors/{seq}.png", img)
        rec.update(hot=[hx, hy], gaf=seq)
        index["cursors"][key] = rec
    print(f"  cursors     {', '.join(index['cursors'])}")

    (OUT / "index.json").write_text(json.dumps(index, indent=1) + "\n")
    print(f"gfx.py: wrote {OUT.relative_to(REPO)}/ ({len(index['units'])} units)")


if __name__ == "__main__":
    main()
