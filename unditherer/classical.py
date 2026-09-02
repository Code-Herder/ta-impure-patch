"""The classical (non-learned) restoration stages, stage for stage the code that
tools/undither/restore.py, tools/undither/prep.py and the WebGL viewer are
built on.  restore.py now imports everything from here.

Pipeline: expand -> analyze -> undither (bilateral) -> deband -> [enhance] -> QA.

  analyze()          dither amplitude q, lag-1 autocorrelation, the detector verdict
  band_step()        the palette step in banded (undithered) regions; gates deband
  undither()         two passes of an edge-preserving bilateral, sigma_r = k*q,
                     with named kernel PRESETS (spec 7x7, tuned 5x5, tight 3x3,
                     split = 5x5 with a luma/chroma-split range metric)
  deband()           blurs single-step palette contours, protects stronger edges
  enhance_procedural() local contrast + saturation-aware vibrance (invents; optional)
  qa()               edge retention, HF-energy reduction, mean colour shift

Evidence for every constant: research/notes/undither.md.
"""
import json
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

# --------------------------------------------------------------------------- stage 0


def load_rgba(path):
    """Lossless expansion: palette index -> RGB, plus an alpha/colorkey mask."""
    im = Image.open(path)
    info = {"mode": im.mode, "size": im.size, "has_palette": im.mode == "P"}
    transparency = im.info.get("transparency")
    rgba = np.array(im.convert("RGBA"))
    if im.mode == "P" and isinstance(transparency, int):
        idx = np.array(im)
        rgba[..., 3] = np.where(idx == transparency, 0, 255).astype(np.uint8)
        info["colorkey_index"] = transparency
    return rgba, info


def flat_mask(rgb, blur=3.0, thr=4.0):
    """Pixels whose low-pass gradient is small: where dither/banding lives, not edges."""
    g = cv2.cvtColor(rgb, cv2.COLOR_RGB2GRAY).astype(np.float32)
    gb = cv2.GaussianBlur(g, (0, 0), blur)
    mag = cv2.magnitude(cv2.Sobel(gb, cv2.CV_32F, 1, 0), cv2.Sobel(gb, cv2.CV_32F, 0, 1))
    return mag < thr


def analyze(rgb):
    """Estimate dither amplitude q (RGB distance) and whether dither is present."""
    m = flat_mask(rgb)
    f = rgb.astype(np.float32)
    res = np.linalg.norm(f - cv2.GaussianBlur(f, (0, 0), 1.2), axis=-1)
    a = res[m]
    g = cv2.cvtColor(rgb, cv2.COLOR_RGB2GRAY).astype(np.float32)
    hf = g - cv2.GaussianBlur(g, (0, 0), 1.0)
    mm = m[:, 1:] & m[:, :-1]
    ac1 = float((hf[:, 1:] * hf[:, :-1])[mm].mean() / (np.var(hf[m]) + 1e-6)) if mm.any() else 0.0
    q = float(2.0 * np.median(a)) if a.size else 0.0
    return {
        "q": q,
        "flat_hf_energy": float(np.mean(a ** 2)) if a.size else 0.0,
        "lag1_autocorr": ac1,
        "flat_fraction": float(m.mean()),
        "dither_present": bool(q > 8.0 and ac1 < -0.2),
    }


def band_step(rgb):
    """Median non-zero neighbour distance: the palette step in banded (undithered) regions."""
    f = rgb.astype(np.float32)
    dx = np.linalg.norm(f[:, 1:] - f[:, :-1], axis=-1)
    dy = np.linalg.norm(f[1:] - f[:-1], axis=-1)
    d = np.concatenate([dx.ravel(), dy.ravel()])
    d = d[d > 0]
    return float(np.median(d)) if d.size else 0.0


def is_tileable(rgb, thr=12.0):
    lr = np.abs(rgb[:, 0].astype(int) - rgb[:, -1].astype(int)).mean()
    tb = np.abs(rgb[0].astype(int) - rgb[-1].astype(int)).mean()
    return bool(lr < thr and tb < thr)


def distinct_colours(rgb):
    return int(len(np.unique(rgb.reshape(-1, 3), axis=0)))

# --------------------------------------------------------------------------- stage 1: undither


def undither_bilateral(rgb, q, sigma_s=1.8, passes=2, wrap=False, k=1.3):
    """Edge-preserving smoothing. sigma_r = k*q sits above dither amplitude, below edge contrast."""
    pad = 8
    x = np.pad(rgb, ((pad, pad), (pad, pad), (0, 0)), mode="wrap" if wrap else "reflect")
    for _ in range(passes):
        x = cv2.bilateralFilter(x, d=0, sigmaColor=k * q, sigmaSpace=sigma_s)
    return x[pad:-pad, pad:-pad]


def undither_guided(rgb, q, radius=4, k=1.3):
    """Faster O(n) alternative; slightly weaker on strong ordered dither (needs opencv-contrib)."""
    return cv2.ximgproc.guidedFilter(rgb, rgb, radius, (k * q) ** 2)


# Kernel presets for the undither stage.  "spec" is the handover document's kernel.
# On TA screenshots its 7x7 disc (radius = round(1.5*sigma_s)) outvotes single-pixel
# speckle texture -- painted grass, sand grit -- and flattens it to mud; a 5x5 disc
# resolves a period-2 dither just as well and keeps 3px+ structure ("tuned").
# "split" scores luma and chroma differences separately: tight on luma so light/dark
# texture survives, loose on chroma so off-hue dither flecks average out.
# Evidence: research/notes/undither.md, "Why the spec kernel muddies TA grass".
PRESETS = {
    "spec":  dict(sigma_s=1.8, passes=2, k=1.3, metric="l1"),
    "tuned": dict(sigma_s=1.2, passes=2, k=1.3, metric="l1"),
    "tight": dict(sigma_s=0.9, passes=2, k=1.3, metric="l1"),
    "split": dict(sigma_s=1.2, passes=2, k=1.3, metric="split"),
}
# Named kernel sizes: sigma_s -> OpenCV disc radius round(1.5*sigma_s)
KERNELS = {"3x3": 0.9, "5x5": 1.2, "7x7": 1.8}
SPLIT_Y_FRAC, SPLIT_C_FRAC = 0.31, 1.15      # sigma_y, sigma_c as fractions of k*q
LUMA_W = np.array([0.299, 0.587, 0.114], np.float32)


def kernel_radius(sigma_s):
    """OpenCV's disc radius for bilateralFilter(d=0)."""
    return max(1, int(round(1.5 * sigma_s)))


def kernel_name(sigma_s):
    r = kernel_radius(sigma_s)
    return f"{2 * r + 1}x{2 * r + 1} {'cross' if r == 1 else 'disc'}"


def _shift(a, dy, dx, wrap):
    """a translated by (dy, dx) with reflect-101 (or wrap) border, like cv2."""
    r = max(abs(dy), abs(dx))
    if r == 0:
        return a
    p = np.pad(a, ((r, r), (r, r)) + ((0, 0),) * (a.ndim - 2), mode="wrap" if wrap else "reflect")
    h, w = a.shape[:2]
    return p[r + dy:r + dy + h, r + dx:r + dx + w]


def undither_split(rgb, q, sigma_s=1.2, passes=2, wrap=False, k=1.3,
                   y_frac=SPLIT_Y_FRAC, c_frac=SPLIT_C_FRAC):
    """Bilateral with a luma/chroma-split range kernel.  Same disc footprint and
    border rule as cv2.bilateralFilter, uint8 hand-off between passes like it too."""
    radius = kernel_radius(sigma_s)
    sy, sc = y_frac * k * q, c_frac * k * q
    gs, gy, gc = -0.5 / sigma_s ** 2, -0.5 / sy ** 2, -0.5 / sc ** 2
    f = rgb.astype(np.float32)
    for _ in range(passes):
        y = f @ LUMA_W
        c = f - y[..., None]
        acc = np.zeros_like(f)
        wsum = np.zeros(f.shape[:2] + (1,), np.float32)
        for dy in range(-radius, radius + 1):
            for dx in range(-radius, radius + 1):
                r2 = dx * dx + dy * dy
                if r2 > radius * radius:
                    continue
                nb = _shift(f, dy, dx, wrap)
                d_y = np.abs(_shift(y, dy, dx, wrap) - y)
                d_c = np.abs(_shift(c, dy, dx, wrap) - c).sum(-1)
                w = np.exp(r2 * gs + d_y * d_y * gy + d_c * d_c * gc)[..., None]
                acc += w * nb
                wsum += w
        f = np.clip(acc / wsum + 0.5, 0, 255).astype(np.uint8).astype(np.float32)
    return f.astype(np.uint8)


def undither(rgb, q, preset="spec", wrap=False, **override):
    """Run the undither stage with a named preset, any field overridable
    (sigma_s, passes, k, metric)."""
    p = dict(PRESETS[preset])
    p.update({kk: v for kk, v in override.items() if v is not None})
    fn = undither_split if p.pop("metric") == "split" else undither_bilateral
    return fn(rgb, q, wrap=wrap, **p)


def blend(orig, filtered, strength):
    """The viewer's Strength knob: lerp original -> filtered, uint8 out."""
    if strength >= 1.0:
        return filtered
    if strength <= 0.0:
        return orig
    f = orig.astype(np.float32) * (1.0 - strength) + filtered.astype(np.float32) * strength
    return np.clip(f + 0.5, 0, 255).astype(np.uint8)

# --------------------------------------------------------------------------- stage 2: deband


def deband(rgb, q, sigma=6.0, edge_factor=1.5):
    """Smooth across single-step palette bands while protecting stronger real edges."""
    f = rgb.astype(np.float32)
    dx = np.zeros(f.shape[:2], np.float32)
    dy = np.zeros_like(dx)
    dx[:, :-1] = np.linalg.norm(f[:, 1:] - f[:, :-1], axis=-1)
    dy[:-1, :] = np.linalg.norm(f[1:] - f[:-1], axis=-1)
    strong = (np.maximum(dx, dy) > edge_factor * q).astype(np.uint8)
    strong = cv2.dilate(strong, np.ones((5, 5), np.uint8)) > 0
    mask = cv2.GaussianBlur((~strong).astype(np.float32), (0, 0), 2.0)[..., None]
    blurred = cv2.GaussianBlur(f, (0, 0), sigma)
    return (f * (1 - mask) + blurred * mask).clip(0, 255).astype(np.uint8)

# --------------------------------------------------------------------------- stage 3: enhance


def enhance_procedural(rgb, amount=0.5, clarity=0.5, vibrance=0.6):
    """Deterministic stand-in for a learned enhancer: local contrast + saturation-aware vibrance.
    No invented tonal drift, so it stays consistent across a texture set."""
    f = rgb.astype(np.float32)
    y = 0.3 * f[..., 0] + 0.59 * f[..., 1] + 0.11 * f[..., 2]
    yb = cv2.GaussianBlur(y, (0, 0), 3.0)
    f = f + ((y - yb) * clarity * amount)[..., None]
    mx, mn = f.max(-1), f.min(-1)
    sat = (mx - mn) / (mx + 1.0)
    lum = (0.3 * f[..., 0] + 0.59 * f[..., 1] + 0.11 * f[..., 2])[..., None]
    gain = (1.0 + vibrance * amount * (1.0 - sat))[..., None]
    f = lum + (f - lum) * gain
    return f.clip(0, 255).astype(np.uint8)

# --------------------------------------------------------------------------- QA


def edge_retention(orig, out):
    g0 = cv2.cvtColor(orig, cv2.COLOR_RGB2GRAY)
    g1 = cv2.cvtColor(out, cv2.COLOR_RGB2GRAY)
    e0 = cv2.Canny(cv2.GaussianBlur(g0, (0, 0), 1.5), 60, 120) > 0
    e1 = cv2.dilate(cv2.Canny(g1, 60, 120), np.ones((3, 3), np.uint8)) > 0
    return float((e0 & e1).sum() / max(e0.sum(), 1))


def mean_colour(rgb):
    """Per-channel mean in float64.  A float32 reduction over a 1080p frame
    accumulates whole levels of error and even depends on the array's memory
    layout (found 2026-09-02: the same frame gave 0.96 or 4.03 for the same
    output); the numbers quoted before that date carry that noise."""
    return rgb.reshape(-1, 3).mean(axis=0, dtype=np.float64)


def mean_colour_shift(orig, out):
    """Largest per-channel change of the mean colour, in levels."""
    return float(np.abs(mean_colour(out) - mean_colour(orig)).max())


def qa(orig, out, before):
    after = analyze(out)
    return {
        "hf_energy_reduction": 1.0 - after["flat_hf_energy"] / max(before["flat_hf_energy"], 1e-6),
        "mean_color_shift": mean_colour_shift(orig, out),
        "edge_retention": edge_retention(orig, out),
        "residual_lag1_autocorr": after["lag1_autocorr"],
    }

# --------------------------------------------------------------------------- restore.py's driver


def process(path, out_dir, enhance="none", wrap="auto", deband_sigma=6.0,
            preset="spec", override=None, force_undither=False):
    """restore.py's per-file driver: the spec's decision tree (detector-gated
    undither, then deband if a staircase is left), PNG + JSON sidecar out."""
    rgba, info = load_rgba(path)
    rgb, alpha = rgba[..., :3].copy(), rgba[..., 3]
    opaque = alpha > 0
    if not opaque.all():
        # keep colorkey pixels from bleeding into the filter: fill them with nearest opaque colour
        rgb = cv2.inpaint(rgb, (~opaque).astype(np.uint8), 3, cv2.INPAINT_TELEA)

    st = analyze(rgb)
    tile = is_tileable(rgb) if wrap == "auto" else (wrap == "yes")
    do_undither = bool(st["dither_present"] or (force_undither and st["q"] > 0))
    kernel = dict(PRESETS[preset]); kernel.update({k: v for k, v in (override or {}).items() if v is not None})
    decisions = {"undither": do_undither, "deband": False, "enhance": enhance, "wrap": tile,
                 "preset": preset, "kernel": kernel}

    out = rgb
    q = st["q"]
    if do_undither:
        out = undither(out, q, preset, wrap=tile, **(override or {}))
        post = analyze(out)
        # residual banding after undithering shows up as single-step contours
        bs = band_step(out)
        if post["flat_hf_energy"] > 2.0 and bs > 8.0:
            decisions["deband"] = True
            out = deband(out, bs, sigma=deband_sigma)
    else:
        bs = band_step(rgb)
        if bs > 8.0:
            decisions["deband"] = True
            q = bs
            out = deband(out, bs, sigma=deband_sigma)

    if enhance == "procedural":
        out = enhance_procedural(out)

    result = np.dstack([out, alpha])
    result[~opaque, :3] = 0
    out_path = Path(out_dir) / (Path(path).stem + ".png")
    Image.fromarray(result, "RGBA").save(out_path)
    report = {"source": str(path), "output": str(out_path), "info": info, "analysis": st,
              "q_used": q, "decisions": decisions, "qa": qa(rgb, out, st)}
    with open(out_path.with_suffix(".json"), "w") as fh:
        json.dump(report, fh, indent=2)
    return report
