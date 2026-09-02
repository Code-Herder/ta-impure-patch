import json

import numpy as np
import pytest

from unditherer.infer import LearnedRestorer, export_onnx, resolve_model
from unditherer.paths import MODELS

ort = pytest.importorskip("onnxruntime")


def has_torch():
    try:
        import torch  # noqa: F401
        return True
    except ImportError:
        return False


@pytest.fixture(scope="module")
def tiny_onnx():
    p = MODELS / "tiny.onnx"
    if not p.exists():
        pytest.skip("shipped tiny.onnx missing (run the pipeline export stage)")
    return p


def test_resolve_model_names_and_paths(tiny_onnx):
    p, b = resolve_model("tiny", "onnx")
    assert p == tiny_onnx and b == "onnx"
    p, b = resolve_model(str(tiny_onnx))
    assert b == "onnx"
    with pytest.raises(ValueError):
        resolve_model(str(tiny_onnx), backend="torch")
    with pytest.raises(FileNotFoundError):
        resolve_model("/nonexistent/model.onnx")
    with pytest.raises(FileNotFoundError):
        resolve_model("no-such-model")


def test_onnx_metadata_and_restore(tiny_onnx, dithered, clean):
    r = LearnedRestorer("tiny", backend="onnx")
    info = r.info()
    assert info["depth"] == 6 and info["ch"] == 24 and info["params"] == 22_251
    assert info["backend"] == "onnx" and info["receptive_field"] == 13
    out = r.restore(dithered)
    assert out.shape == dithered.shape and out.dtype == np.uint8
    from .conftest import psnr
    assert psnr(out, clean) > psnr(dithered, clean) + 2.0


def test_tiled_equals_whole(tiny_onnx, dithered):
    whole = LearnedRestorer("tiny", backend="onnz" if False else "onnx", tile=1088).restore(dithered)
    tiled = LearnedRestorer("tiny", backend="onnx", tile=48, overlap=8).restore(dithered)
    assert np.abs(whole.astype(int) - tiled.astype(int)).max() <= 1
    with pytest.raises(ValueError):
        LearnedRestorer("tiny", backend="onnx", tile=48, overlap=4)     # narrower than the receptive radius


def test_wrap_padding_matches_interior(tiny_onnx, dithered):
    a = LearnedRestorer("tiny", backend="onnx", wrap=False).restore(dithered)
    b = LearnedRestorer("tiny", backend="onnx", wrap=True).restore(dithered)
    inner = (slice(8, -8), slice(8, -8))
    assert np.abs(a[inner].astype(int) - b[inner].astype(int)).max() <= 1
    assert not np.array_equal(a, b)


def test_restore_batch(tiny_onnx, dithered):
    r = LearnedRestorer("tiny", backend="onnx")
    batch = np.stack([dithered[:64, :64], dithered[:64, 64:128]])
    out = r.restore_batch(batch)
    assert out.shape == batch.shape
    assert np.array_equal(out[0], r.restore(dithered[:64, :64]))


@pytest.mark.skipif(not has_torch(), reason="torch not installed")
def test_torch_and_onnx_agree(tiny_onnx, dithered):
    a = LearnedRestorer("tiny", backend="torch", device="cpu").restore(dithered)
    b = LearnedRestorer("tiny", backend="onnx").restore(dithered)
    assert np.abs(a.astype(int) - b.astype(int)).max() <= 1
    assert (a == b).mean() > 0.95


@pytest.mark.skipif(not has_torch(), reason="torch not installed")
def test_export_onnx_roundtrip(tmp_path):
    import torch
    from unditherer.model import Restorer
    m = Restorer(4, 8)
    torch.nn.init.normal_(m.net[-1].weight, std=0.05)      # not the identity, so parity means something
    ck = tmp_path / "toy.pt"
    torch.save({"model": m.state_dict(), "depth": 4, "ch": 8, "step": 7, "val_psnr": 1.5}, ck)
    info = export_onnx(ck, tmp_path / "toy.onnx", meta={"name": "toy"})
    assert info["max_abs_diff_vs_torch"] < 1e-4
    assert not (tmp_path / "toy.onnx.data").exists()
    r = LearnedRestorer(str(tmp_path / "toy.onnx"))
    assert r.depth == 4 and r.ch == 8 and r.step == 7 and r.meta["name"] == "toy"


def test_models_json_matches_files():
    mj = MODELS / "models.json"
    if not mj.exists():
        pytest.skip("no models.json yet")
    models = json.loads(mj.read_text())
    for name, m in models.items():
        for fname, size in m["files"].items():
            assert (MODELS / fname).exists(), fname
            assert (MODELS / fname).stat().st_size == size
        assert m["onnx_max_abs_diff"] is None or m["onnx_max_abs_diff"] < 1e-3
