"""Corpus listing, the seeded train/val split, and the on-the-fly pair stream."""
import json
import math
import time
from pathlib import Path

import numpy as np
import torch
from PIL import Image
from torch.utils.data import IterableDataset

from .synth import DEFAULT_AUG, make_pair

IMAGE_EXTS = (".jpg", ".jpeg", ".png", ".webp")


def corpus_files(root):
    """Every image the manifest lists (or, while a fetch is still running, whatever
    is already under images/)."""
    root = Path(root)
    mpath = root / "manifest.json"
    if mpath.exists():
        files = sorted(root / m["file"] for m in json.loads(mpath.read_text()))
    else:
        files = sorted(p for p in (root / "images").rglob("*") if p.suffix.lower() in IMAGE_EXTS)
    return [f for f in files if f.exists()]


def split(files, val_frac=0.05, seed=0):
    """Seeded held-out split by SOURCE IMAGE.  For the 1,548-image corpus this
    holds out 77 images, exactly the split every shipped run used; tiny corpora
    (tests, smoke runs) keep at least one image on each side."""
    if len(files) < 2:
        raise ValueError("need at least two corpus images to split")
    rng = np.random.default_rng(seed)
    idx = rng.permutation(len(files))
    n_val = max(16, int(len(files) * val_frac))
    n_val = min(n_val, len(files) - 1)
    return [files[i] for i in idx[n_val:]], [files[i] for i in idx[:n_val]]


class PairStream(IterableDataset):
    """Infinite stream of (dithered, clean, is_dither) tensors, synthesised in
    the DataLoader workers from a small per-worker cache of decoded images."""

    def __init__(self, files, patch, seed=0, cache=40, aug=DEFAULT_AUG, deterministic=False):
        self.files, self.patch, self.seed, self.cache_n, self.aug = files, patch, seed, cache, aug
        self.deterministic = deterministic

    def __iter__(self):
        wi = torch.utils.data.get_worker_info()
        wid = wi.id if wi else 0
        salt = 0 if self.deterministic else int(time.time()) % 100000
        rng = np.random.default_rng(self.seed * 1000 + wid + salt)
        cache = {}
        while True:
            f = self.files[int(rng.integers(len(self.files)))]
            img = cache.get(f)
            if img is None:
                try:
                    img = Image.open(f).convert("RGB")
                    img.load()
                except Exception:
                    continue
                if len(cache) >= self.cache_n:
                    cache.pop(next(iter(cache)))
                cache[f] = img
            if min(img.size) < self.patch + 2 * self.aug.margin:
                continue
            d, c, mode = make_pair(img, self.patch, rng, aug=self.aug)
            yield (torch.from_numpy(d).permute(2, 0, 1).float() / 255.0,
                   torch.from_numpy(c).permute(2, 0, 1).float() / 255.0,
                   torch.tensor(0.0 if mode == "nearest" else 1.0))   # dithered, so its local mean is truth


def make_val(files, patch, n, seed=1, aug=DEFAULT_AUG):
    """A fixed validation set: n (dithered, clean) uint8 patches from `files`."""
    rng = np.random.default_rng(seed)
    ds, cs = [], []
    while len(ds) < n:
        f = files[int(rng.integers(len(files)))]
        img = Image.open(f).convert("RGB")
        if min(img.size) < patch + 2 * aug.margin:
            continue
        d, c, _ = make_pair(img, patch, rng, aug=aug)
        ds.append(d); cs.append(c)
    return np.stack(ds), np.stack(cs)


def psnr(a, b):
    mse = np.mean((a.astype(np.float32) - b.astype(np.float32)) ** 2)
    return 10 * math.log10(255.0 ** 2 / max(mse, 1e-6))


def bilateral_baseline(d_u8, preset="tuned"):
    """The classical kernel on the validation inputs, for the PSNR table."""
    from .classical import analyze, undither
    out = []
    for d in d_u8:
        st = analyze(d)
        out.append(undither(d, st["q"], preset) if st["q"] > 0 else d)
    return np.stack(out)
