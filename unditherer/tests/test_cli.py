import json
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

from unditherer.cli import main, output_paths
from unditherer.paths import MODELS, REPO
from .conftest import psnr


def run(argv, capsys):
    rc = main(argv)
    out, err = capsys.readouterr()
    return rc, out, err


def load(p):
    return np.asarray(Image.open(p).convert("RGB"))


def test_presets_command(capsys):
    rc, out, _ = run(["presets"], capsys)
    assert rc == 0
    for name in ("spec", "tuned", "tight", "split", "learned", "learned-tiny"):
        assert name in out
    assert "5x5 disc" in out and "luma/chroma" in out
    rc, out, _ = run(["presets", "--json"], capsys)
    j = json.loads(out)
    assert j["classical"]["tuned"]["sigma_s"] == 1.2 and j["kernels"]["7x7"] == 1.8


def test_restore_default_output_next_to_input(dithered_png, clean, capsys):
    rc, out, err = run(["restore", str(dithered_png)], capsys)
    assert rc == 0
    dest = dithered_png.with_name("dith.undithered.png")
    assert dest.exists()
    assert "tuned 5x5 disc x2 l1" in err and "undither=True" in err
    assert psnr(load(dest), clean) > psnr(load(dithered_png), clean) + 3


@pytest.mark.parametrize("preset", ["spec", "tuned", "tight", "split"])
def test_every_classical_preset(preset, tmp_path, dithered_png, clean, capsys):
    dest = tmp_path / f"{preset}.png"
    rc, out, err = run(["restore", str(dithered_png), "-o", str(dest), "--preset", preset, "--json"], capsys)
    assert rc == 0 and dest.exists()
    rep = json.loads(out)
    assert rep["preset"] == preset and rep["stages"][0] == "undither"
    assert psnr(load(dest), clean) > psnr(load(dithered_png), clean) + 3


def test_kernel_metric_passes_k(tmp_path, dithered_png, capsys):
    dest = tmp_path / "k.png"
    rc, out, _ = run(["restore", str(dithered_png), "-o", str(dest), "--kernel", "7x7", "--metric", "RGB-L1",
                      "--passes", "3", "--k", "1.1", "--json", "-q"], capsys)
    assert rc == 0
    k = json.loads(out)["kernel"]
    assert k["size"] == "7x7 disc" and k["sigma_s"] == 1.8 and k["passes"] == 3 and k["k"] == 1.1 and k["metric"] == "l1"
    assert k["sigma_r"] == pytest.approx(1.1 * json.loads(out)["q_used"])


def test_kernel_and_sigma_s_are_exclusive(dithered_png, capsys):
    with pytest.raises(SystemExit) as e:
        main(["restore", str(dithered_png), "--kernel", "5x5", "--sigma-s", "1.0"])
    assert e.value.code == 2


def test_metric_aliases_and_errors(tmp_path, dithered_png, capsys):
    for alias, want in (("rgbl1", "l1"), ("luma-chroma", "split"), ("split", "split")):
        rc, out, _ = run(["restore", str(dithered_png), "-o", str(tmp_path / f"{alias}.png"), "--metric", alias,
                          "--json", "-q"], capsys)
        assert rc == 0 and json.loads(out)["kernel"]["metric"] == want
    rc, _, err = run(["restore", str(dithered_png), "-o", str(tmp_path / "x.png"), "--metric", "hsl"], capsys)
    assert rc == 2 and "unknown metric" in err


def test_passes_differ(tmp_path, dithered_png, capsys):
    outs = []
    for n in (1, 2):
        dest = tmp_path / f"p{n}.png"
        assert run(["restore", str(dithered_png), "-o", str(dest), "--passes", str(n), "-q"], capsys)[0] == 0
        outs.append(load(dest))
    assert not np.array_equal(*outs)


def test_undither_off_deband_off_is_identity(tmp_path, dithered_png, capsys):
    dest = tmp_path / "id.png"
    rc, out, _ = run(["restore", str(dithered_png), "-o", str(dest), "--undither", "off", "--deband", "off",
                      "--json", "-q"], capsys)
    assert rc == 0 and json.loads(out)["stages"] == []
    assert np.array_equal(load(dest), load(dithered_png))


def test_undither_auto_follows_detector(tmp_path, dithered_png, capsys):
    rc, out, _ = run(["restore", str(dithered_png), "-o", str(tmp_path / "a.png"), "--undither", "auto",
                      "--json", "-q"], capsys)
    rep = json.loads(out)
    assert rep["decisions"]["undither"] == rep["analysis"]["dither_present"]


def test_strength_blend(tmp_path, dithered_png, capsys):
    full = tmp_path / "full.png"; half = tmp_path / "half.png"
    assert run(["restore", str(dithered_png), "-o", str(full), "--deband", "off", "-q"], capsys)[0] == 0
    rc, out, _ = run(["restore", str(dithered_png), "-o", str(half), "--deband", "off", "--strength", "0.5",
                      "--json", "-q"], capsys)
    assert rc == 0 and "strength 0.5" in json.loads(out)["stages"]
    a, b, c = load(dithered_png).astype(int), load(full).astype(int), load(half).astype(int)
    assert np.abs(c - (a + b) / 2).max() <= 1


def test_q_override_changes_sigma_r(tmp_path, dithered_png, capsys):
    rc, out, _ = run(["restore", str(dithered_png), "-o", str(tmp_path / "q.png"), "--q", "20", "--json", "-q"], capsys)
    rep = json.loads(out)
    assert rep["q_used"] == 20 and rep["kernel"]["sigma_r"] == pytest.approx(26.0)


def test_report_sidecar(tmp_path, dithered_png, capsys):
    dest = tmp_path / "r.png"
    rc, _, _ = run(["restore", str(dithered_png), "-o", str(dest), "--report", "--consistency", "-q"], capsys)
    assert rc == 0
    side = tmp_path / "r.png.json"
    assert side.exists()
    rep = json.loads(side.read_text())
    for key in ("analysis", "q_used", "band_step", "decisions", "kernel", "qa", "stages", "options", "seconds",
                "dither_consistency", "source", "output", "bytes"):
        assert key in rep, key
    assert 0 <= rep["dither_consistency"]["fs"]["histogram_overlap"] <= 1


def test_multiple_inputs_into_directory(tmp_path, dithered_png, capsys):
    second = tmp_path / "second.png"
    Image.open(dithered_png).save(second)
    outdir = tmp_path / "out"
    rc, out, _ = run(["restore", str(dithered_png), str(second), "-o", str(outdir), "--json", "-q"], capsys)
    assert rc == 0
    assert (outdir / "dith.undithered.png").exists() and (outdir / "second.undithered.png").exists()
    assert len(json.loads(out)) == 2


def test_webp_output_is_lossless(tmp_path, dithered_png, capsys):
    png = tmp_path / "a.png"; webp = tmp_path / "a.webp"
    assert run(["restore", str(dithered_png), "-o", str(png), "-q"], capsys)[0] == 0
    assert run(["restore", str(dithered_png), "-o", str(webp), "-q"], capsys)[0] == 0
    assert np.array_equal(load(png), load(webp))
    outdir = tmp_path / "w"
    assert run(["restore", str(dithered_png), "-o", str(outdir) + "/", "--format", "webp", "-q"], capsys)[0] == 0
    assert (outdir / "dith.undithered.webp").exists()


def test_missing_input(tmp_path, capsys):
    rc, _, err = run(["restore", str(tmp_path / "nope.png")], capsys)
    assert rc == 1 and "no such file" in err
    rc, _, err = run(["analyze", str(tmp_path / "nope.png")], capsys)
    assert rc == 1


def test_colourkey_alpha_preserved(tmp_path, colourkey_png, capsys):
    dest = tmp_path / "keyed.out.png"
    assert run(["restore", str(colourkey_png), "-o", str(dest), "-q"], capsys)[0] == 0
    im = Image.open(dest)
    assert im.mode == "RGBA"
    a = np.asarray(im)
    assert (a[:8, :8, 3] == 0).all() and (a[8:, 8:, 3] == 255).all()
    assert (a[:8, :8, :3] == 0).all()


def test_analyze_command(dithered_png, capsys):
    rc, out, _ = run(["analyze", str(dithered_png)], capsys)
    assert rc == 0 and "q=" in out and "detector=" in out
    rc, out, _ = run(["analyze", str(dithered_png), "--json"], capsys)
    j = json.loads(out)
    assert j["analysis"]["q"] > 0 and "auto_decisions" in j and j["info"]["mode"] == "P"


def test_enhance_stage(tmp_path, dithered_png, capsys):
    rc, out, _ = run(["restore", str(dithered_png), "-o", str(tmp_path / "e.png"), "--enhance", "--json", "-q"], capsys)
    assert rc == 0 and "enhance" in json.loads(out)["stages"]


def test_bad_strength(dithered_png, capsys):
    rc, _, err = run(["restore", str(dithered_png), "--strength", "1.5"], capsys)
    assert rc == 2 and "strength" in err


# ---- learned presets --------------------------------------------------------

needs_onnx = pytest.mark.skipif(not (MODELS / "tiny.onnx").exists(), reason="shipped ONNX models missing")


@needs_onnx
def test_learned_tiny_via_onnx(tmp_path, dithered_png, clean, capsys):
    pytest.importorskip("onnxruntime")
    dest = tmp_path / "cnn.png"
    rc, out, err = run(["restore", str(dithered_png), "-o", str(dest), "--preset", "learned-tiny",
                        "--backend", "onnx", "--json"], capsys)
    assert rc == 0
    rep = json.loads(out)
    assert rep["stages"] == ["learned"] and rep["kernel"] is None
    assert rep["model"]["backend"] == "onnx" and rep["model"]["depth"] == 6
    assert rep["decisions"]["deband"] is False
    assert "tiny.onnx via onnx" in err
    assert psnr(load(dest), clean) > psnr(load(dithered_png), clean) + 2


@needs_onnx
def test_learned_full_via_onnx(tmp_path, dithered_png, clean, capsys):
    pytest.importorskip("onnxruntime")
    dest = tmp_path / "full.png"
    rc, out, _ = run(["restore", str(dithered_png), "-o", str(dest), "-p", "learned", "--backend", "onnx",
                      "--json", "-q"], capsys)
    assert rc == 0
    rep = json.loads(out)
    assert rep["model"]["depth"] == 12 and rep["model"]["params"] == 373_443
    assert psnr(load(dest), clean) > psnr(load(dithered_png), clean) + 3


@needs_onnx
def test_learned_model_path_and_deband_on(tmp_path, dithered_png, capsys):
    pytest.importorskip("onnxruntime")
    rc, out, _ = run(["restore", str(dithered_png), "-o", str(tmp_path / "m.png"), "-p", "learned",
                      "--model", str(MODELS / "tiny.onnx"), "--deband", "on", "--json", "-q"], capsys)
    assert rc == 0
    rep = json.loads(out)
    assert rep["model"]["depth"] == 6 and rep["stages"] == ["learned", "deband"]


@needs_onnx
def test_learned_tiled_matches_whole(tmp_path, dithered_png, capsys):
    pytest.importorskip("onnxruntime")
    a, b = tmp_path / "whole.png", tmp_path / "tiled.png"
    base = ["restore", str(dithered_png), "-p", "learned-tiny", "--backend", "onnx", "-q"]
    assert run(base + ["-o", str(a)], capsys)[0] == 0
    assert run(base + ["-o", str(b), "--tile", "40", "--overlap", "8"], capsys)[0] == 0
    assert np.abs(load(a).astype(int) - load(b).astype(int)).max() <= 1


def test_learned_bad_model(tmp_path, dithered_png, capsys):
    rc, _, err = run(["restore", str(dithered_png), "-o", str(tmp_path / "x.png"), "-p", "learned",
                      "--model", str(tmp_path / "missing.onnx")], capsys)
    assert rc == 1 and "cannot load the learned model" in err


@needs_onnx
def test_models_command(capsys):
    rc, out, _ = run(["models"], capsys)
    assert rc == 0 and "full" in out and "tiny" in out and "373,443" in out
    rc, out, _ = run(["models", "--json"], capsys)
    j = json.loads(out)
    assert j["full"]["source_run"] == "full-mc" and j["tiny"]["depth"] == 6


def test_output_paths_helper(tmp_path):
    a, b = tmp_path / "a.png", tmp_path / "b.png"
    assert output_paths([a], None, None) == [tmp_path / "a.undithered.png"]
    assert output_paths([a], str(tmp_path / "x.webp"), None) == [tmp_path / "x.webp"]
    assert output_paths([a, b], str(tmp_path / "d"), "webp") == [tmp_path / "d" / "a.undithered.webp",
                                                                 tmp_path / "d" / "b.undithered.webp"]


def test_module_entrypoint():
    r = subprocess.run([sys.executable, "-m", "unditherer", "--version"], cwd=REPO, capture_output=True, text=True)
    assert r.returncode == 0 and "unditherer" in r.stdout
    r = subprocess.run([sys.executable, "-m", "unditherer"], cwd=REPO, capture_output=True, text=True)
    assert r.returncode == 2
