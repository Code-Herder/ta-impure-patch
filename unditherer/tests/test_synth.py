import numpy as np
import pytest
from PIL import Image

from unditherer.synth import (DEFAULT_AUG, DITHER_MODES, PALETTE, Aug, bayer, dither_consistency,
                              dither_mode, expand, make_pair, ordered_dither, quantize)
from .conftest import synth_clean


def test_palette_shape_and_content():
    assert PALETTE.shape == (256, 3) and PALETTE.dtype == np.uint8
    assert len(np.unique(PALETTE, axis=0)) == 242        # the game's palette: 242 distinct entries


def test_quantize_lands_on_palette(clean):
    for dither in (True, False):
        idx = quantize(clean, dither)
        assert idx.shape == clean.shape[:2] and idx.dtype == np.uint8
        rgb = PALETTE[idx]
        # nearest-colour is at least as close as FS per pixel on average
        assert np.abs(rgb.astype(int) - clean.astype(int)).mean() < 40


def test_bayer_matrix_is_a_permutation():
    for n in (4, 8):
        m = bayer(n)
        assert m.shape == (n, n)
        vals = np.sort((m * n * n).ravel())
        assert np.array_equal(vals, np.arange(n * n))


def test_ordered_dither_amplitude_changes_output(clean):
    a = ordered_dither(clean, 4, amp=16.0)
    b = ordered_dither(clean, 4, amp=48.0)
    assert a.shape == clean.shape[:2]
    assert (a != b).mean() > 0.05


def test_every_dither_mode_runs(clean):
    rng = np.random.default_rng(0)
    for mode in set(DITHER_MODES):
        idx, m = dither_mode(clean, mode, rng)
        assert m == mode and idx.shape == clean.shape[:2]
    with pytest.raises(ValueError):
        dither_mode(clean, "nope", rng)


def test_make_pair_shapes_and_palette_membership():
    img = Image.fromarray(synth_clean(300, 300, seed=3))
    rng = np.random.default_rng(1)
    for _ in range(6):
        d, c, mode = make_pair(img, 64, rng)
        assert d.shape == (64, 64, 3) and c.shape == (64, 64, 3)
        assert d.dtype == np.uint8 and c.dtype == np.uint8
        assert mode in DITHER_MODES
        pal = {tuple(p) for p in PALETTE}
        assert all(tuple(px) in pal for px in d.reshape(-1, 3)[::97])


def test_make_pair_is_seed_reproducible():
    img = Image.fromarray(synth_clean(300, 300, seed=5))
    a = make_pair(img, 48, np.random.default_rng(7))
    b = make_pair(img, 48, np.random.default_rng(7))
    assert np.array_equal(a[0], b[0]) and np.array_equal(a[1], b[1]) and a[2] == b[2]


def test_aug_roundtrip_and_defaults():
    d = DEFAULT_AUG.to_dict()
    assert d["saturation"] == (0.6, 2.2) and d["hue_p"] == 0.5 and d["vivid_p"] == 0.0
    a = Aug.from_dict({"saturation": [0.5, 1.5], "dither_modes": "fs,nearest", "vivid_p": 0.4})
    assert a.saturation == (0.5, 1.5) and a.dither_modes == ("fs", "nearest") and a.vivid_p == 0.4


def test_vivid_push_saturates():
    img = Image.fromarray(synth_clean(300, 300, seed=9))
    sat = lambda x: (x.max(-1) - x.min(-1)).mean()
    base = [make_pair(img, 64, np.random.default_rng(s), aug=Aug(jitter_p=0, hue_p=1.0))[1] for s in range(8)]
    vivid = [make_pair(img, 64, np.random.default_rng(s), aug=Aug(jitter_p=0, hue_p=1.0, vivid_p=1.0))[1] for s in range(8)]
    assert np.mean([sat(v) for v in vivid]) > np.mean([sat(b) for b in base]) * 1.5
    # the push lives inside the hue branch: without hue rotation it never fires
    off = [make_pair(img, 64, np.random.default_rng(s), aug=Aug(jitter_p=0, hue_p=0, vivid_p=1.0))[1] for s in range(8)]
    plain = [make_pair(img, 64, np.random.default_rng(s), aug=Aug(jitter_p=0, hue_p=0))[1] for s in range(8)]
    assert all(np.array_equal(a, b) for a, b in zip(off, plain))


def test_expand_indexed_png(dithered_png, dithered):
    rgb = expand(dithered_png)
    assert rgb.shape == dithered.shape and np.array_equal(rgb, dithered)


def test_dither_consistency_identity(dithered):
    c = dither_consistency(dithered, dithered, dither=False)
    assert c["histogram_overlap"] == pytest.approx(1.0) and c["exact_index_match"] == pytest.approx(1.0)
    worse = dither_consistency(dithered, np.roll(dithered, 1, axis=1), dither=False)
    assert worse["exact_index_match"] < 1.0
