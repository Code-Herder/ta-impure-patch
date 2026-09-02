"""Synthesise training pairs: true-colour crop -> TA palette with dither.

The model learns to invert what a 1997 art pipeline did to true-colour source
art: reduce to the game's single 256-entry palette (ta-palette.json, read from
the captured frames -- every environment shares it) with error-diffusion dither.
Nobody knows which quantiser Cavedog used, so the dither is drawn at random per
sample from Floyd-Steinberg (PIL, in C), Bayer ordered dither, and plain
nearest-colour with no dither at all -- the last one is what banding is, so a
model trained this way replaces both the undither and the deband stage.

Spatial scale matters more than subject: TA tiles are 32x32 texels of painted
detail, so 1K photographic textures are downscaled by a random 1-6x before
quantising, which puts fleck-scale detail at the pixel scale.

Every augmentation knob is a field of `Aug`; the defaults are exactly what the
shipped models were trained with (see LEARNINGS.md for how they were arrived
at).  `Aug()` consumes the random stream in the same order as the original
training script, so seeded validation sets are reproducible.
"""
import json
from dataclasses import dataclass, asdict, fields
from pathlib import Path

import numpy as np
from PIL import Image, ImageEnhance

from .paths import PALETTE as PALETTE_PATH

PALETTE = np.array(json.loads(Path(PALETTE_PATH).read_text()), np.uint8)   # 256 x 3

_pal_img = None


def pal_image():
    global _pal_img
    if _pal_img is None:
        _pal_img = Image.new("P", (1, 1))
        _pal_img.putpalette(PALETTE.ravel().tolist())
    return _pal_img


def quantize(rgb_u8, dither=True):
    """Palette indices for an RGB uint8 array, Floyd-Steinberg or nearest."""
    mode = Image.Dither.FLOYDSTEINBERG if dither else Image.Dither.NONE
    im = Image.fromarray(np.ascontiguousarray(rgb_u8), "RGB").quantize(palette=pal_image(), dither=mode)
    return np.asarray(im, dtype=np.uint8)


def bayer(n):
    m = np.array([[0, 2], [3, 1]], np.float32)
    while m.shape[0] < n:
        m = np.block([[4 * m, 4 * m + 2], [4 * m + 3, 4 * m + 1]])
    return m / (m.shape[0] ** 2)


_BAYER = {4: bayer(4), 8: bayer(8)}


def ordered_dither(rgb_u8, n=4, amp=32.0):
    """Threshold-matrix dither against the fixed palette: offset then nearest."""
    h, w = rgb_u8.shape[:2]
    m = _BAYER[n]
    t = (np.tile(m, (h // n + 1, w // n + 1))[:h, :w] - 0.5) * amp
    f = np.clip(rgb_u8.astype(np.float32) + t[..., None], 0, 255).astype(np.uint8)
    return quantize(f, dither=False)


DITHER_MODES = ("fs", "fs", "fs", "nearest", "ordered4", "ordered8")


def dither_mode(rgb_u8, mode, rng):
    """Quantise with one named mode.  Returns (indices, mode)."""
    if mode == "fs":
        return quantize(rgb_u8, True), mode
    if mode == "nearest":
        return quantize(rgb_u8, False), mode
    if mode in ("ordered4", "ordered8"):
        amp = float(rng.uniform(*_current_amp))
        return ordered_dither(rgb_u8, 4 if mode == "ordered4" else 8, amp=amp), mode
    raise ValueError(f"unknown dither mode {mode!r}")


_current_amp = (16.0, 48.0)


def dither_random(rgb_u8, rng, modes=DITHER_MODES, amp=(16.0, 48.0)):
    """Draw a dither mode per sample.  Listing "fs" three times makes it the
    mode half the time, which is the shipped mix."""
    global _current_amp
    _current_amp = amp
    mode = modes[int(rng.integers(len(modes)))]
    return dither_mode(rgb_u8, mode, rng)


@dataclass(frozen=True)
class Aug:
    """Augmentation recipe.  Defaults = the shipped models' recipe."""
    scale_range: tuple = (1.0, 6.0)      # random downscale before quantising
    jitter_p: float = 0.7                # p(brightness/contrast/saturation jitter)
    brightness: tuple = (0.7, 1.25)
    contrast: tuple = (0.8, 1.2)
    saturation: tuple = (0.6, 2.2)       # wide: TA's water and lava out-saturate any photo
    hue_p: float = 0.5                   # p(rotate hue through the full circle)
    vivid_p: float = 0.0                 # dead end (LEARNINGS.md 3.5): push S toward 255, inside the hue branch
    dither_modes: tuple = DITHER_MODES
    ordered_amp: tuple = (16.0, 48.0)
    margin: int = 8                      # dither warm-up border around the patch

    def to_dict(self):
        return asdict(self)

    @classmethod
    def from_dict(cls, d):
        d = dict(d or {})
        for f in fields(cls):
            if f.name in d and isinstance(d[f.name], list):
                d[f.name] = tuple(d[f.name])
        if isinstance(d.get("dither_modes"), str):
            d["dither_modes"] = tuple(m.strip() for m in d["dither_modes"].split(",") if m.strip())
        return cls(**d)


DEFAULT_AUG = Aug()


def random_crop(img, patch, margin, rng, scale_range=(1.0, 6.0), aug=DEFAULT_AUG):
    """A (patch + 2*margin)^2 true-colour crop at a random downscale, with jitter."""
    s = float(rng.uniform(*scale_range))
    side = patch + 2 * margin
    need = int(np.ceil(side * s))
    w, h = img.size
    if need > min(w, h):
        s = min(w, h) / side
        need = int(side * s)
    x = int(rng.integers(0, w - need + 1))
    y = int(rng.integers(0, h - need + 1))
    crop = img.crop((x, y, x + need, y + need))
    if s > 1.001:
        crop = crop.resize((side, side), Image.BOX if rng.random() < 0.5 else Image.LANCZOS)
    elif crop.size != (side, side):
        crop = crop.resize((side, side), Image.LANCZOS)
    k = int(rng.integers(4))
    if k:
        crop = crop.transpose([Image.ROTATE_90, Image.ROTATE_180, Image.ROTATE_270][k - 1])
    if rng.random() < 0.5:
        crop = crop.transpose(Image.FLIP_LEFT_RIGHT)
    if rng.random() < aug.jitter_p:
        crop = ImageEnhance.Brightness(crop).enhance(float(rng.uniform(*aug.brightness)))
        crop = ImageEnhance.Contrast(crop).enhance(float(rng.uniform(*aug.contrast)))
        crop = ImageEnhance.Color(crop).enhance(float(rng.uniform(*aug.saturation)))
    if rng.random() < aug.hue_p:
        # the corpus is ground; TA also has water, lava, ice and purple crystal.
        # Rotating hue puts every texture into every region of the palette, so
        # the model learns the palette's geometry everywhere, not just in browns.
        hsv = np.array(crop.convert("HSV"), dtype=np.uint8)
        hsv[..., 0] = (hsv[..., 0].astype(np.int16) + int(rng.integers(0, 256))) % 256
        if aug.vivid_p > 0 and rng.random() < aug.vivid_p:
            # The "vivid saturation push" tried on 2026-09-01 (runs full-ft /
            # tiny-ft) and removed: it made Water World's colour drift worse.
            # This is the removed code verbatim -- it sat inside the hue branch,
            # so it touched p(hue) * vivid_p of the crops -- kept, off by
            # default, so the dead end can be re-run rather than re-discovered.
            keep = float(rng.uniform(0.0, 0.5))
            hsv[..., 1] = (255 - (255 - hsv[..., 1].astype(np.float32)) * keep).astype(np.uint8)
        crop = Image.fromarray(hsv, "HSV").convert("RGB")
    return np.array(crop.convert("RGB"), dtype=np.uint8)      # a writable copy: torch wants one


def make_pair(img, patch, rng, margin=None, aug=DEFAULT_AUG):
    """(dithered RGB, clean RGB, mode) uint8 patch^2 arrays; the dither is run on
    a margin-padded crop so error diffusion has warmed up before the patch."""
    margin = aug.margin if margin is None else margin
    clean = random_crop(img, patch, margin, rng, aug.scale_range, aug)
    idx, mode = dither_random(clean, rng, aug.dither_modes, aug.ordered_amp)
    dithered = PALETTE[idx]
    sl = slice(margin, margin + patch)
    return dithered[sl, sl], clean[sl, sl], mode


def expand(indexed_png):
    """Indexed PNG -> RGB uint8 through its own palette (the game frames)."""
    im = Image.open(indexed_png)
    if im.mode == "P":
        pal = np.array(im.getpalette()).reshape(-1, 3)[:256].astype(np.uint8)
        return pal[np.asarray(im)]
    return np.asarray(im.convert("RGB"), dtype=np.uint8)


def nearest_index(rgb_u8):
    """Exact nearest-palette index per pixel (PIL's quantize uses an approximate
    colour cache, which is fine for synthesis but not for scoring)."""
    flat = rgb_u8.reshape(-1, 3).astype(np.int32)
    packed = (flat[:, 0] << 16) | (flat[:, 1] << 8) | flat[:, 2]
    uniq, inv = np.unique(packed, return_inverse=True)
    cols = np.stack([(uniq >> 16) & 255, (uniq >> 8) & 255, uniq & 255], 1).astype(np.int32)
    pal = PALETTE.astype(np.int32)
    best = np.empty(len(cols), np.int64)
    for i in range(0, len(cols), 4096):
        d = ((cols[i:i + 4096, None, :] - pal[None, :, :]) ** 2).sum(-1)
        best[i:i + 4096] = d.argmin(1)
    return best[inv].reshape(rgb_u8.shape[:2]).astype(np.uint8)


def dither_consistency(orig_rgb, out_rgb, dither=True):
    """How well `out_rgb` dithers back into the frame it came from.

    Re-quantise the candidate to the TA palette (Floyd-Steinberg, or nearest
    when dither=False), then compare palette-index statistics with the
    original's own indices: histogram overlap (sum of per-index minima of the
    two normalised histograms) and the fraction of pixels landing on exactly
    the original index.  A restoration that cannot be dithered back into its
    source is not a restoration of that source."""
    i0 = nearest_index(orig_rgb)            # the original is on-palette, so this is its own index
    i1 = quantize(out_rgb, True) if dither else nearest_index(out_rgb)
    h0 = np.bincount(i0.ravel(), minlength=256) / i0.size
    h1 = np.bincount(i1.ravel(), minlength=256) / i1.size
    return {"histogram_overlap": float(np.minimum(h0, h1).sum()),
            "exact_index_match": float((i0 == i1).mean())}
