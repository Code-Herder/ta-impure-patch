"""research/build_wiki.py regenerates the undither page's learned bakes: the
staleness rule it uses, tested on temp files."""
import importlib.util
import os
import time
from pathlib import Path

import pytest

from unditherer.paths import REPO

pytest.importorskip("markdown")
_spec = importlib.util.spec_from_file_location("build_wiki", REPO / "research" / "build_wiki.py")
build_wiki = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(build_wiki)


def _touch(p, t):
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(b"x")
    os.utime(p, (t, t))


def test_stale_bakes_rule(tmp_path):
    now = time.time()
    frames = [tmp_path / "shots" / f"{n}.png" for n in ("a", "b", "c")]
    for f in frames:
        _touch(f, now - 300)
    model = tmp_path / "full.onnx"
    _touch(model, now - 200)
    out = tmp_path / "learned"
    # nothing baked yet: everything is stale
    assert build_wiki.stale_bakes(frames, out, [model]) == frames
    for f in frames:
        _touch(out / (f.stem + ".webp"), now - 100)
    _touch(out / "model.json", now - 100)
    assert build_wiki.stale_bakes(frames, out, [model]) == []
    # a newer model makes every bake stale; a newer frame only its own
    _touch(model, now - 50)
    assert build_wiki.stale_bakes(frames, out, [model]) == frames
    _touch(model, now - 200)
    _touch(frames[1], now - 10)
    assert build_wiki.stale_bakes(frames, out, [model]) == [frames[1]]
    # a missing model.json means the bake never finished
    (out / "model.json").unlink()
    assert build_wiki.stale_bakes(frames, out, [model]) == frames
    # a missing model file is not "newer"
    assert build_wiki.stale_bakes(frames, out, [tmp_path / "nope.pt"]) == frames




def test_bake_paths_point_at_the_page_assets():
    assert build_wiki.BAKE_SHOTS == REPO / "research" / "notes" / "assets" / "undither" / "shots"
    assert build_wiki.BAKE_OUT.name == "learned"
    assert any(m.exists() for m in build_wiki.BAKE_MODEL)
    assert Path(build_wiki.find_python()).exists()
