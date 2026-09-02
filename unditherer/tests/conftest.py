"""Shared fixtures: a synthetic clean/dithered image pair and a tiny corpus."""
import numpy as np
import pytest
from PIL import Image

from unditherer.synth import PALETTE, quantize


def synth_clean(h=96, w=128, seed=0):
    """Smooth ramps + a sharp vertical edge + soft texture: what dither and
    banding both act on, with a real edge to protect."""
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:h, 0:w]
    img = np.stack([40 + 120 * xx / w, 60 + 100 * yy / h, 90 + 60 * np.sin(xx / 9.0)], -1)
    img[:, w // 2:] += 60
    img += rng.normal(0, 2.0, img.shape)
    return np.clip(img, 0, 255).astype(np.uint8)


@pytest.fixture(scope="session")
def clean():
    return synth_clean()


@pytest.fixture(scope="session")
def dither_idx(clean):
    return quantize(clean, True)


@pytest.fixture(scope="session")
def dithered(dither_idx):
    return PALETTE[dither_idx]


@pytest.fixture(scope="session")
def banded(clean):
    return PALETTE[quantize(clean, False)]


def indexed_image(idx):
    im = Image.fromarray(idx, "P")
    im.putpalette(PALETTE.ravel().tolist())
    return im


@pytest.fixture
def dithered_png(tmp_path, dither_idx):
    """The dithered image saved as an indexed (palette) PNG, like a TA frame."""
    p = tmp_path / "dith.png"
    indexed_image(dither_idx).save(p)
    return p


@pytest.fixture
def colourkey_png(tmp_path, dither_idx):
    """Indexed PNG with a transparent colour-key index, like a unit sprite."""
    p = tmp_path / "keyed.png"
    idx = dither_idx.copy()
    idx[:8, :8] = 0                       # a block of colourkey pixels
    indexed_image(idx).save(p, transparency=0)
    return p


@pytest.fixture(scope="session")
def tiny_corpus(tmp_path_factory):
    """A 6-image true-colour corpus with a manifest, enough to train a toy net."""
    root = tmp_path_factory.mktemp("corpus")
    (root / "images" / "synthetic").mkdir(parents=True)
    manifest = []
    for i in range(6):
        img = synth_clean(160, 160, seed=100 + i)
        rel = f"images/synthetic/img{i}.png"
        Image.fromarray(img).save(root / rel)
        manifest.append({"file": rel, "source": "synthetic", "id": f"img{i}", "licence": "test",
                         "url": "https://example.invalid/img", "authors": ["tests"], "bytes": 1})
    import json
    (root / "manifest.json").write_text(json.dumps(manifest))
    return root


def psnr(a, b):
    return 10 * np.log10(255.0 ** 2 / max(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2), 1e-9))
