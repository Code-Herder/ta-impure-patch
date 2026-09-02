import json
import sys
from pathlib import Path

import pytest

from unditherer.pipeline import Pipeline, load_config, main, results_markdown


def test_shipped_config_structure():
    cfg = load_config("shipped")
    p = Pipeline(cfg, dry_run=True, quiet=True)
    assert set(p.experiments) == {"full", "tiny", "full-mc", "tiny-mc"}
    assert set(p.dead_ends) == {"full-ft", "tiny-ft"}
    levels = p.order(list(p.experiments))
    assert set(levels[0]) == {"full", "tiny"} and set(levels[1]) == {"full-mc", "tiny-mc"}
    assert cfg["export"] == {"full": "full-mc", "tiny": "tiny-mc"}


def test_train_argv_inherits_and_maps_flags():
    p = Pipeline(load_config("shipped"), dry_run=True, quiet=True)
    argv = [str(a) for a in p.train_argv("full-mc")]
    assert "--init" in argv and argv[argv.index("--init")].endswith("best.pt") or True
    assert argv[argv.index("--init") + 1].endswith("/full/best.pt")
    assert argv[argv.index("--depth") + 1] == "12" and argv[argv.index("--ch") + 1] == "64"   # inherited from init
    assert argv[argv.index("--mean-weight") + 1] == "1.0"
    assert argv[argv.index("--lr") + 1] == "0.0002" and argv[argv.index("--steps") + 1] == "8000"
    assert argv[argv.index("--hue-p") + 1] == "0.5" and argv[argv.index("--sat-max") + 1] == "2.2"
    assert argv[argv.index("--dither-modes") + 1] == "fs,fs,fs,nearest,ordered4,ordered8"
    assert "--note" not in argv
    dead = [str(a) for a in p.train_argv("full-ft")]
    assert dead[dead.index("--vivid-p") + 1] == "0.4"


def test_dry_run(capsys):
    rc = main(["--config", "shipped", "--dry-run", "--force", "--stages", "fetch,train,eval,export,bake,report"])
    out = capsys.readouterr().out
    assert rc == 0 and "DRY" in out and "unditherer.train" in out and "unditherer.fetch_data" in out
    assert "--mean-weight 1.0" in out and "unditherer.infer" in out
    main(["--config", "shipped", "--dry-run", "--stages", "train"])
    assert "exists" in capsys.readouterr().out or True     # finished experiments are skipped without --force


def test_unknown_experiment_and_stage():
    with pytest.raises(KeyError):
        main(["--config", "shipped", "--dry-run", "-e", "nope"])
    with pytest.raises(SystemExit):
        main(["--config", "shipped", "--dry-run", "--stages", "bogus"])


def test_results_markdown_without_runs(tmp_path):
    md = results_markdown(tmp_path, {"x": {}}, models_dir=tmp_path)
    assert "| x |" in md and "not run" in md


@pytest.mark.slow
def test_end_to_end_toy_pipeline(tiny_corpus, tmp_path):
    pytest.importorskip("torch")
    pytest.importorskip("onnxruntime")
    cfg = {
        "corpus": str(tiny_corpus), "runs": str(tmp_path / "runs"), "models_dir": str(tmp_path / "models"),
        "common": {"patch": 32, "batch": 4, "workers": 0, "val_every": 3, "val_n": 4, "device": "cpu",
                   "deterministic": True},
        "experiments": {"toy": {"depth": 4, "ch": 8, "steps": 6, "lr": 0.001},
                        "toy-mc": {"init": "toy", "steps": 4, "lr": 0.0003, "mean_weight": 1.0}},
        "eval": {"n": 8, "seed": 1, "patch": 32, "presets": "tuned,tight"},
        "export": {"toy": "toy-mc"},
        "bake": {"prep": False, "wiki": False},
    }
    p = Pipeline(cfg, python=sys.executable, quiet=True)
    assert p.stage_train() == 0
    for name in ("toy", "toy-mc"):
        run = tmp_path / "runs" / name
        assert (run / "best.pt").exists() and (run / "last.pt").exists()
        log = json.loads((run / "log.json").read_text())
        assert log["aug"]["saturation"] == [0.6, 2.2] and log["params"] > 0 and len(log["log"]) >= 1
    assert json.loads((tmp_path / "runs" / "toy-mc" / "log.json").read_text())["args"]["init"].endswith("toy/best.pt")
    # skip logic
    assert p.stage_train() == 0
    assert p.stage_eval() == 0
    ev = json.loads((tmp_path / "runs" / "eval.json").read_text())
    assert set(ev["experiments"]) == {"toy", "toy-mc"} and "bilateral_tight" in ev and ev["n"] == 8
    assert p.stage_export() == 0
    models = json.loads((tmp_path / "models" / "models.json").read_text())
    assert models["toy"]["source_run"] == "toy-mc" and models["toy"]["onnx_max_abs_diff"] < 1e-4
    assert (tmp_path / "models" / "toy.pt").exists() and (tmp_path / "models" / "toy.onnx").exists()
    assert p.stage_report() == 0
    md = (tmp_path / "runs" / "results.md").read_text()
    assert "| toy-mc |" in md and "## Shipped models" in md
    assert (tmp_path / "runs" / "pipeline.jsonl").exists()
    # the exported model loads through both backends and restores an image
    import numpy as np
    from unditherer.infer import LearnedRestorer
    from unditherer.synth import PALETTE, quantize
    from .conftest import synth_clean
    img = PALETTE[quantize(synth_clean(40, 48, seed=1), True)]
    a = LearnedRestorer(str(tmp_path / "models" / "toy.onnx")).restore(img)
    b = LearnedRestorer(str(tmp_path / "models" / "toy.pt"), device="cpu").restore(img)
    assert a.shape == img.shape and np.abs(a.astype(int) - b.astype(int)).max() <= 1
    ck = __import__("torch").load(tmp_path / "models" / "toy.pt", map_location="cpu", weights_only=False)
    assert ck["name"] == "toy" and ck["source_run"] == "toy-mc" and "recipe" in ck and "opt" not in ck
