import numpy as np
import pytest

from unditherer import classical as C
from .conftest import psnr


def test_analyze_detects_dither(dithered, clean):
    st = C.analyze(dithered)
    assert st["q"] > 8.0
    assert st["lag1_autocorr"] < 0                      # error diffusion anticorrelates neighbours
    assert 0 < st["flat_fraction"] <= 1
    assert C.analyze(clean)["q"] < st["q"]


def test_band_step_larger_on_banding(clean, banded):
    assert C.band_step(banded) > C.band_step(clean)


def test_presets_and_kernel_names():
    assert set(C.PRESETS) == {"spec", "tuned", "tight", "split"}
    assert C.kernel_name(1.8) == "7x7 disc" and C.kernel_name(1.2) == "5x5 disc" and C.kernel_name(0.9) == "3x3 cross"
    assert C.KERNELS == {"3x3": 0.9, "5x5": 1.2, "7x7": 1.8}
    for p in C.PRESETS.values():
        assert set(p) == {"sigma_s", "passes", "k", "metric"}


@pytest.mark.parametrize("preset", ["spec", "tuned", "tight", "split"])
def test_undither_improves_psnr(preset, dithered, clean):
    q = C.analyze(dithered)["q"]
    out = C.undither(dithered, q, preset)
    assert out.shape == dithered.shape and out.dtype == np.uint8
    assert psnr(out, clean) > psnr(dithered, clean) + 3.0
    assert C.edge_retention(dithered, out) > 0.8


def test_undither_overrides_change_output(dithered):
    q = C.analyze(dithered)["q"]
    a = C.undither(dithered, q, "tuned")
    b = C.undither(dithered, q, "tuned", passes=1)
    c = C.undither(dithered, q, "tuned", sigma_s=1.8)
    d = C.undither(dithered, q, "tuned", k=0.6)
    assert not np.array_equal(a, b) and not np.array_equal(a, c) and not np.array_equal(a, d)
    assert np.array_equal(C.undither(dithered, q, "tuned", sigma_s=1.8), C.undither(dithered, q, "spec"))


def test_split_metric_matches_bilateral_footprint(dithered):
    """The split kernel with a huge chroma/luma tolerance degenerates toward a
    spatial-only blur, so it should smooth at least as much as the L1 one."""
    q = C.analyze(dithered)["q"]
    l1 = C.undither(dithered, q, "tuned")
    sp = C.undither_split(dithered, q, y_frac=50, c_frac=50)
    assert C.analyze(sp)["flat_hf_energy"] <= C.analyze(l1)["flat_hf_energy"] + 1e-6


def test_wrap_border_differs_only_at_edges(dithered):
    q = C.analyze(dithered)["q"]
    a = C.undither(dithered, q, "tuned", wrap=False)
    b = C.undither(dithered, q, "tuned", wrap=True)
    inner = (slice(8, -8), slice(8, -8))
    assert np.array_equal(a[inner], b[inner])


def test_blend_endpoints(dithered, clean):
    assert C.blend(dithered, clean, 0.0) is dithered
    assert C.blend(dithered, clean, 1.0) is clean
    mid = C.blend(dithered, clean, 0.5)
    assert mid.dtype == np.uint8
    assert abs(mid.astype(int) - (dithered.astype(int) + clean.astype(int)) // 2).max() <= 1


def test_deband_smooths_staircase(banded, clean):
    step = C.band_step(banded)
    out = C.deband(banded, step)
    assert psnr(out, clean) > psnr(banded, clean)
    assert C.edge_retention(banded, out) > 0.7


def test_enhance_changes_but_keeps_range(clean):
    out = C.enhance_procedural(clean)
    assert out.dtype == np.uint8 and out.shape == clean.shape
    assert not np.array_equal(out, clean)


def test_qa_keys(dithered, clean):
    st = C.analyze(dithered)
    q = C.qa(dithered, clean, st)
    assert set(q) == {"hf_energy_reduction", "mean_color_shift", "edge_retention", "residual_lag1_autocorr"}
    assert 0 <= q["edge_retention"] <= 1


def test_is_tileable_and_distinct_colours(dithered):
    tile = np.tile(dithered[:32, :32], (2, 2, 1))
    assert C.is_tileable(tile) is True or C.is_tileable(tile) is False   # bool, not numpy
    assert C.distinct_colours(dithered) <= 256


def test_process_writes_png_and_sidecar(tmp_path, dithered_png):
    rep = C.process(dithered_png, tmp_path, preset="tuned", force_undither=True)
    assert (tmp_path / "dith.png").exists() and (tmp_path / "dith.json").exists()
    assert rep["decisions"]["undither"] is True and rep["decisions"]["preset"] == "tuned"


def test_mean_colour_shift_is_exact_on_a_1080p_frame():
    """Regression: a float32 mean over 2M pixels was off by whole levels and
    depended on memory layout."""
    rng = np.random.default_rng(0)
    orig = rng.integers(2, 254, (1080, 1920, 3), dtype=np.uint8)
    out = (orig.astype(np.int16) + np.array([1, 0, -2], np.int16)).astype(np.uint8)
    assert abs(C.mean_colour_shift(orig, out) - 2.0) < 1e-6
    chw_view = np.ascontiguousarray(out.transpose(2, 0, 1)).transpose(1, 2, 0)      # like a network output
    assert not chw_view.flags["C_CONTIGUOUS"]
    assert abs(C.mean_colour_shift(orig, chw_view) - 2.0) < 1e-6
    st = C.analyze(orig[:64, :64])
    assert abs(C.qa(orig[:64, :64], out[:64, :64], st)["mean_color_shift"] - 2.0) < 1e-6
