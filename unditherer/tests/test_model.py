import numpy as np
import pytest

torch = pytest.importorskip("torch")

from unditherer.model import Restorer, count_params, flops_per_pixel   # noqa: E402


def test_shipped_sizes():
    assert count_params(Restorer(12, 64)) == 373_443
    assert count_params(Restorer(6, 24)) == 22_251
    assert Restorer(12, 64).receptive_field() == 25
    assert Restorer(6, 24).receptive_field() == 13


def test_starts_as_identity():
    m = Restorer(4, 8).eval()
    x = torch.rand(2, 3, 16, 20)
    with torch.no_grad():
        assert torch.allclose(m(x), x)


def test_forward_shape_any_size():
    m = Restorer(4, 8).eval()
    with torch.no_grad():
        assert m(torch.rand(1, 3, 37, 53)).shape == (1, 3, 37, 53)


def test_flops_estimate():
    assert flops_per_pixel(12, 64) == 2 * 9 * (3 * 64 + 64 * 64 * 10 + 64 * 3)
    f = flops_per_pixel(12, 64) * 1920 * 1080
    assert 1.4e12 < f < 1.7e12          # ~1.5 TFLOPs per 1080p frame (2 FLOPs per multiply-add)
    assert 8e10 < flops_per_pixel(6, 24) * 1920 * 1080 < 1.1e11   # tiny: ~90 GFLOPs
