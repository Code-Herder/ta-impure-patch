#!/usr/bin/env python3
"""G15-0 — the Classic++ restorer on the UI art, offline, before any engine code.

Pulls the UI art straight out of the game archives (no game running), runs the shipped
learned model on it exactly as the game's GLSL port does, and lays original beside restored
on one contact sheet per class so the owner can judge what the model does to art it was
never trained on: bevelled panels, buttons, glyph-shaped edges, unit pictures. Each frame
also gets the unditherer's own numbers -- the measured dither amplitude q, edge retention,
mean colour shift and the dither-consistency score (re-quantise the output and count how
many pixels land back on their source index). The output of this run is the default
`uirestore` exclude list of research/notes/gui-renderer.md 3.9.

    ../../.venv-undither/bin/python tools/undither/uiart.py --out /tmp/uiart
    ../../.venv-undither/bin/python tools/undither/uiart.py --out /tmp/uiart --sheets research/notes/assets/shots

Classes (gui-renderer.md 3.9): bg (shell backgrounds and dialogs, bitmaps/*.pcx >= 320 wide),
small (the rest of bitmaps/, button strips and fonts), hud (the six in-game panel tiles), gaf
(commongui.gaf and the shell/in-game screens' own anims/<screen>.gaf), cursors (the expected
exclusion), unitpics (a sample), screens (captured 640x480 menu surfaces, the cautionary
whole-screen column: what text becomes when it is NOT excluded).
"""
import argparse
import hashlib
import io
import json
import re
import subprocess
import sys
from importlib.machinery import SourceFileLoader
import importlib.util
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
CHECKOUT = TOOLS.parent


def _load_ta3do():
    loader = SourceFileLoader("ta3do", str(TOOLS / "ta3do"))
    spec = importlib.util.spec_from_loader("ta3do", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


ta3do = _load_ta3do()

HUD_TILES = {f"bitmaps/{side}gui{part}tile.pcx" for side in ("arm", "cor") for part in ("top", "side", "bot")}
# anims/<screen>.gaf holds that screen's button art; commongui.gaf the panels (ARMPAN/CORPAN).
GAF_SCREENS = ["commongui", "oldmain", "single", "skirmish", "selmap", "loadgame", "prefs", "visuals",
               "sounds", "control", "speeds", "armcom1", "corcom1", "armopt", "coropt", "armaap1"]
MIN_GAF = 8            # frames below this on either side carry nothing to restore or to judge
ZOOM = 3
OUT_SUFFIX = ".undithered"
REUSE = False


class Frame:
    def __init__(self, cls, name, image, transparent=None, source=""):
        self.cls, self.name, self.image, self.transparent, self.source = cls, name, image, transparent, source

    @property
    def size(self):
        return self.image.size


def _fill_index(im):
    """A PCX sheet's key fill: one palette index covering over a third of the canvas and
    all four corners. The engine never draws it (it cuts sprites by rect), so the spike
    marks it transparent and the restorer inpaints it, as the game's GLSL path would."""
    a = np.asarray(im)
    counts = np.bincount(a.ravel(), minlength=256)
    dom = int(counts.argmax())
    corners = {int(a[0, 0]), int(a[0, -1]), int(a[-1, 0]), int(a[-1, -1])}
    if counts[dom] / a.size > 0.35 and corners == {dom}:
        return dom
    return None


def pcx_frame(assets, path, cls):
    im = Image.open(io.BytesIO(assets.read(path)))
    if im.mode != "P":
        im = im.convert("P", palette=Image.ADAPTIVE, colors=256)
    im.load()
    key = _fill_index(im)
    if cls == "hud" and key is not None:
        # the six "tile" files are 640x480 canvases holding one small strip
        a = np.asarray(im)
        ys, xs = np.nonzero(a != key)
        im = im.crop((int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1))
    if key is not None:
        im.info["transparency"] = key
    return Frame(cls, Path(path).stem, im, key, path)


def gaf_frames(assets, gaf_path, palette, cls, seen):
    blob = assets.read(gaf_path)
    out = []
    for name, off in ta3do.gaf_entries(blob).items():
        try:
            fr = ta3do.gaf_frame(blob, off)
        except Exception:
            continue
        if fr.width < MIN_GAF or fr.height < MIN_GAF:
            continue
        digest = hashlib.sha1(bytes(fr.pixels)).hexdigest()
        if digest in seen:
            continue
        seen.add(digest)
        png = ta3do.write_indexed_png(fr.width, fr.height, fr.pixels, palette, transparent=fr.transparent)
        im = Image.open(io.BytesIO(png))
        im.load()
        out.append(Frame(cls, f"{Path(gaf_path).stem}.{name.lower()}", im, fr.transparent, gaf_path))
    return out


def collect(assets, sample):
    palette = ta3do.load_palette(assets)
    frames = []
    for p in assets.glob("bitmaps/*.pcx"):
        if "/glamour/" in p:
            continue
        cls = "hud" if p in HUD_TILES else None
        fr = pcx_frame(assets, p, cls or "bg")
        if cls is None:
            fr.cls = "bg" if fr.size[0] >= 320 else "small"
        frames.append(fr)
    seen = set()
    for screen in GAF_SCREENS:
        path = f"anims/{screen}.gaf"
        if assets.has(path):
            frames += gaf_frames(assets, path, palette, "gaf", seen)
    frames += gaf_frames(assets, "anims/cursors.gaf", palette, "cursors", set())
    pics = assets.glob("unitpics/*.pcx")
    step = max(1, len(pics) // max(1, sample))
    for p in pics[::step][:sample]:
        frames.append(pcx_frame(assets, p, "unitpics"))
    for p in sorted((HERE / "screens").glob("*.png")):
        im = Image.open(p)
        im.load()
        frames.append(Frame("screens", p.stem, im, None, str(p.relative_to(CHECKOUT))))
    return frames


def run_restorer(frames, out: Path, preset: str, python: str):
    src = out / "in"
    dst = out / "out"
    src.mkdir(parents=True, exist_ok=True)
    dst.mkdir(parents=True, exist_ok=True)
    paths = []
    for fr in frames:
        stem = re.sub(r"[^A-Za-z0-9_.-]", "_", f"{fr.cls}__{fr.name}")
        p = src / f"{stem}.png"
        fr.image.save(p)
        fr.stem = stem
        paths.append(p)
    if all((dst / f"{fr.stem}{OUT_SUFFIX}.png.json").exists() for fr in frames) and REUSE:
        print("reusing the restorer's outputs in", dst, file=sys.stderr)
        cmd = None
    else:
        cmd = [python, "-m", "unditherer", "restore", *[str(p) for p in paths], "-o", str(dst),
               "--preset", preset, "--report", "--consistency", "--quiet"]
    if cmd:
        proc = subprocess.run(cmd, cwd=str(CHECKOUT), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode != 0:
            sys.exit("unditherer failed: " + proc.stderr.decode("latin-1", "replace")[-2000:])
    for fr in frames:
        fr.restored = dst / f"{fr.stem}{OUT_SUFFIX}.png"
        rep = json.loads((dst / f"{fr.stem}{OUT_SUFFIX}.png.json").read_text())
        qa, dc = rep.get("qa", {}), rep.get("dither_consistency", {}).get("fs", {})
        fr.numbers = {
            "q": round(float(rep.get("q_used", 0.0)), 2),
            "edges": round(float(qa.get("edge_retention", 0.0)), 3),
            "shift": round(float(qa.get("mean_color_shift", 0.0)), 2),
            "match": round(float(dc.get("exact_index_match", 0.0)), 3),
            "hist": round(float(dc.get("histogram_overlap", 0.0)), 3),
        }


# ----------------------------------------------------------------------------- sheets

def _rgb(im):
    return im.convert("RGBA")


def _busiest_window(im, w, h, stride=8):
    """Top-left of the w x h window with the most local detail (where a dither shows),
    skipping windows that are mostly one flat colour -- a PCX sheet's key fill is not art."""
    g = np.asarray(im.convert("L"), dtype=np.float32)
    idx = np.asarray(im.convert("RGB")).astype(np.uint32)
    idx = (idx[..., 0] << 16) | (idx[..., 1] << 8) | idx[..., 2]
    H, W = g.shape
    if H <= h or W <= w:
        return 0, 0
    lap = np.abs(np.diff(g, axis=1))[:-1, :] + np.abs(np.diff(g, axis=0))[:, :-1]
    best, bx, by = -1.0, 0, 0
    for y in range(0, H - h + 1, stride):
        for x in range(0, W - w + 1, stride):
            block = idx[y:y + h, x:x + w]
            _, counts = np.unique(block, return_counts=True)
            flat = counts.max() / block.size
            if flat > 0.5:
                continue
            score = float(lap[y:y + h - 1, x:x + w - 1].sum())
            if score > best:
                best, bx, by = score, x, y
    if best < 0:            # everything is flat: fall back to the plain maximum
        ii = np.pad(lap, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
        hh, ww = min(h, lap.shape[0]), min(w, lap.shape[1])
        sums = ii[hh:, ww:] - ii[:-hh, ww:] - ii[hh:, :-ww] + ii[:-hh, :-ww]
        by, bx = np.unravel_index(int(np.argmax(sums)), sums.shape)
    return int(bx), int(by)


def _label(draw, xy, lines, fill=(235, 235, 235)):
    x, y = xy
    for ln in lines:
        draw.text((x, y), ln, fill=fill)
        y += 13


def _checker(size):
    w, h = size
    tile = np.zeros((h, w, 4), np.uint8)
    yy, xx = np.mgrid[0:h, 0:w]
    dark = ((xx // 8 + yy // 8) % 2).astype(bool)
    tile[..., :3] = np.where(dark[..., None], 70, 100)
    tile[..., 3] = 255
    return Image.fromarray(tile, "RGBA")


def _paste_over_checker(im):
    bg = _checker(im.size)
    bg.alpha_composite(_rgb(im))
    return bg.convert("RGB")


def grid_sheet(frames, path: Path, zoom=ZOOM, cols=3, cap=(160, 160), limit=60):
    """Small art: whole frame, original beside restored, zoom x, a few per row; rows take
    their own height, so one tall panel frame does not stretch every row."""
    frames = frames[:limit]
    cells = []
    for fr in frames:
        a = _paste_over_checker(fr.image)
        b = _paste_over_checker(Image.open(fr.restored))
        w, h = a.size
        z = zoom if w <= cap[0] and h <= cap[1] else max(1, min(zoom, cap[0] // w, cap[1] // h))
        a = a.resize((w * z, h * z), Image.NEAREST)
        b = b.resize((w * z, h * z), Image.NEAREST)
        cells.append((fr, a, b))
    if not cells:
        return None
    cw = max(max(a.width * 2 + 6 for _, a, _ in cells), 300)
    rows = [cells[i:i + cols] for i in range(0, len(cells), cols)]
    heights = [max(a.height for _, a, _ in row) + 44 for row in rows]
    sheet = Image.new("RGB", (cols * (cw + 8) + 8, sum(heights) + 8 * (len(rows) + 1)), (24, 24, 24))
    d = ImageDraw.Draw(sheet)
    y0 = 8
    for row, rh in zip(rows, heights):
        for j, (fr, a, b) in enumerate(row):
            x0 = 8 + j * (cw + 8)
            sheet.paste(a, (x0, y0))
            sheet.paste(b, (x0 + a.width + 6, y0))
            n = fr.numbers
            _label(d, (x0, y0 + a.height + 4),
                   [f"{fr.name} {fr.size[0]}x{fr.size[1]}  x{a.width // fr.size[0]}",
                    f"q={n['q']} edges={n['edges']} shift={n['shift']}",
                    f"consistency: match={n['match']} hist={n['hist']}"])
        y0 += rh + 8
    _save(sheet, path)
    return sheet.size


def detail_sheet(frames, path: Path, zoom=ZOOM, window=(160, 120), micro=(32, 24), micro_zoom=8, limit=12):
    """Big art: a thumbnail for context, the busiest window at zoom x original|restored,
    and an 8x micro-crop of its centre, one row per frame, worst consistency first."""
    frames = frames[:limit]
    if not frames:
        return None
    tw, th = 160, 120
    ww, wh = window
    mw, mh = micro
    row_h = max(th, wh * zoom) + 46
    col_x = [8, 8 + tw + 8, 8 + tw + 8 + ww * zoom + 6, 8 + tw + 8 + 2 * (ww * zoom + 6), 8 + tw + 8 + 2 * (ww * zoom + 6) + mw * micro_zoom + 6]
    width = col_x[-1] + mw * micro_zoom + 8
    sheet = Image.new("RGB", (width, 8 + len(frames) * (row_h + 8)), (24, 24, 24))
    d = ImageDraw.Draw(sheet)
    for i, fr in enumerate(frames):
        y0 = 8 + i * (row_h + 8)
        a = _paste_over_checker(fr.image)
        b = _paste_over_checker(Image.open(fr.restored))
        thumb = a.copy()
        thumb.thumbnail((tw, th), Image.BOX)
        sheet.paste(thumb, (col_x[0], y0))
        x, y = _busiest_window(a, ww, wh)
        wa = a.crop((x, y, x + ww, y + wh)).resize((ww * zoom, wh * zoom), Image.NEAREST)
        wb = b.crop((x, y, x + ww, y + wh)).resize((ww * zoom, wh * zoom), Image.NEAREST)
        sheet.paste(wa, (col_x[1], y0))
        sheet.paste(wb, (col_x[2], y0))
        mx, my = x + (ww - mw) // 2, y + (wh - mh) // 2
        ma = a.crop((mx, my, mx + mw, my + mh)).resize((mw * micro_zoom, mh * micro_zoom), Image.NEAREST)
        mb = b.crop((mx, my, mx + mw, my + mh)).resize((mw * micro_zoom, mh * micro_zoom), Image.NEAREST)
        sheet.paste(ma, (col_x[3], y0))
        sheet.paste(mb, (col_x[4], y0))
        # mark the window on the thumbnail
        sx, sy = thumb.width / a.width, thumb.height / a.height
        d.rectangle((col_x[0] + x * sx, y0 + y * sy, col_x[0] + (x + ww) * sx, y0 + (y + wh) * sy), outline=(255, 220, 0))
        n = fr.numbers
        _label(d, (col_x[0], y0 + max(th, wh * zoom) + 4),
               [f"{fr.name}  {fr.size[0]}x{fr.size[1]}   window ({x},{y}) x{zoom}, micro x{micro_zoom}    "
                f"q={n['q']}  edges={n['edges']}  shift={n['shift']}  consistency match={n['match']} hist={n['hist']}"])
        _label(d, (col_x[1], y0 + max(th, wh * zoom) + 17), ["original"])
        _label(d, (col_x[2], y0 + max(th, wh * zoom) + 17), ["restored (learned)"])
        _label(d, (col_x[3], y0 + max(th, wh * zoom) + 17), ["original, micro"])
        _label(d, (col_x[4], y0 + max(th, wh * zoom) + 17), ["restored, micro"])
    _save(sheet, path)
    return sheet.size


def _save(sheet, path: Path):
    if path.suffix.lower() == ".webp":
        sheet.save(path, lossless=True, quality=100, method=6)
    else:
        sheet.save(path, optimize=True)


GRID_CLASSES = {"small", "gaf", "cursors", "unitpics", "hud"}
DETAIL_CLASSES = {"bg", "screens"}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True, help="work directory (inputs, outputs, report)")
    ap.add_argument("--sheets", help="directory for the contact sheets (default: <out>/sheets)")
    ap.add_argument("--sample", type=int, default=24, help="unit pictures to sample (default 24)")
    ap.add_argument("--preset", default="learned", help="unditherer preset (default learned)")
    ap.add_argument("--limit", type=int, default=12, help="rows per detail sheet (default 12)")
    ap.add_argument("--python", default=sys.executable, help="interpreter with the unditherer environment")
    ap.add_argument("--grid-limit", type=int, default=60, help="frames per grid sheet, worst consistency first (default 60)")
    ap.add_argument("--reuse", action="store_true", help="reuse the restorer's outputs already in <out>/out")
    ap.add_argument("--format", default="png", choices=["png", "webp"], help="sheet format (webp is lossless)")
    a = ap.parse_args()
    global REUSE
    REUSE = a.reuse
    out = Path(a.out)
    sheets = Path(a.sheets) if a.sheets else out / "sheets"
    sheets.mkdir(parents=True, exist_ok=True)

    assets = ta3do.Assets()
    frames = collect(assets, a.sample)
    by = {}
    for fr in frames:
        by.setdefault(fr.cls, []).append(fr)
    print("frames:", {k: len(v) for k, v in by.items()}, file=sys.stderr)
    run_restorer(frames, out, a.preset, a.python)

    report = {"preset": a.preset, "classes": {}}
    print("| class | frame | size | q | edges | shift | match | hist |")
    print("|---|---|---|---|---|---|---|---|")
    for cls, lst in by.items():
        lst.sort(key=lambda f: (f.numbers["match"], f.numbers["hist"]))
        path = sheets / f"uiart-{cls}.{a.format}"
        size = (grid_sheet(lst, path, cols=4 if cls == "gaf" else 3, limit=a.grid_limit)
                if cls in GRID_CLASSES else detail_sheet(lst, path, limit=a.limit))
        if cls == "gaf":
            # the 128x352 side panels cannot be judged in a 1x grid cell: a detail row each
            tall = [f for f in lst if f.size[1] > 160]
            if tall:
                detail_sheet(tall, sheets / f"uiart-gaf-panels.{a.format}", window=(96, 96), micro=(24, 18))
        matches = [f.numbers["match"] for f in lst]
        report["classes"][cls] = {
            "frames": [{"name": f.name, "source": f.source, "size": list(f.size), **f.numbers} for f in lst],
            "sheet": str(path.resolve().relative_to(CHECKOUT)) if path.resolve().is_relative_to(CHECKOUT) else str(path),
            "sheet_size": size,
            "match_min": min(matches), "match_median": float(np.median(matches)),
        }
        for f in lst:
            n = f.numbers
            print(f"| {cls} | {f.name} | {f.size[0]}x{f.size[1]} | {n['q']} | {n['edges']} | {n['shift']} | {n['match']} | {n['hist']} |")
    (out / "uiart-report.json").write_text(json.dumps(report, indent=2))
    print(file=sys.stderr)
    for cls, r in report["classes"].items():
        print(f"{cls:9s} n={len(r['frames']):3d} match min={r['match_min']:.3f} median={r['match_median']:.3f} "
              f"sheet={r['sheet']} {r['sheet_size']}", file=sys.stderr)


if __name__ == "__main__":
    main()
