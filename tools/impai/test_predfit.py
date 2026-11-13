"""The fight fit's offline parts (predfit.py): the coefficients file it writes, the split, a scenario read as a
fight. The fit itself is `predfit.py selftest`, which needs the native library."""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import predfit  # noqa: E402


def test_emit_start_is_the_tracked_file():
    assert predfit.emit_c(predfit.START, "`tools/impai/predfit.py emit --start` writes them: the fit's starting "
                          "point, fitted to no reference.") == predfit.PARAMS.read_text()


def test_params_read_back_bit_exact(tmp_path):
    theta = [predfit.f32(v) for v in predfit.SYN_TRUE]
    f = tmp_path / "p.c"
    f.write_text(predfit.emit_c(theta, "a test."))
    assert predfit.read_params(f) == theta
    assert predfit.read_params(predfit.PARAMS) == [predfit.f32(v) for v in predfit.START]


def test_split_is_by_stem():
    assert predfit.fnv1a32("") == 0x811C9DC5
    halves = [predfit.in_check_half(f"group-{i:03d}") for i in range(150)]
    assert 50 < sum(halves) < 100
    assert halves == [predfit.in_check_half(f"group-{i:03d}") for i in range(150)]


def test_scenario_fight():
    c = predfit.Content(["NONE", "ARMPW", "CORAK"], bytes(40 * 3), bytes(36), b"")
    doc = {"units": [
        {"id": "a0", "type": "ARMPW", "owner": 0, "pos": [100, 0], "orders": [{"cmd": "attack", "target": "b0"}]},
        {"id": "a1", "type": "armpw", "owner": 0, "pos": [100, 40], "health": 50},
        {"id": "b0", "type": "CORAK", "owner": 1, "pos": [500, 20]}]}
    f = predfit.scenario_fight(doc, c, "t")
    assert f["a"] == [(1, 1, 32768), (1, 1, 65535)] and f["b"] == [(2, 1, 65535)]
    assert f["a_closes"] and not f["b_closes"] and f["dist"] == 400
    doc["units"][2]["owner"] = 2
    try:
        predfit.scenario_fight(doc, c, "t")
    except predfit.CouldNotRun:
        pass
    else:
        raise AssertionError("an owner past 1 was taken")


def test_judge_counts_decisive_only():
    fights = [{"W": 24, "n": 25}, {"W": 12, "n": 25}, {"W": 1, "n": 25}]
    agree, dec, bad = predfit.judge(fights, [0.3, -0.9, 0.2])
    assert (agree, dec) == (1, 2) and bad[0][0] is fights[2]


def test_content_is_written_and_read_back(tmp_path):
    """cmd_content's end, offline: the tables checked and written, and Content.load reads the same bytes."""
    t = bytes(range(40)) * 3
    w = bytes(36) * 2
    d = bytes(8)
    names = ["NONE", "ARMPW", "CORAK"]
    dll = tmp_path / "ddraw.dll"
    dll.write_bytes(b"dll")
    digest = predfit.Content(names, t, w, d).digest()
    out = tmp_path / "c.json"
    assert predfit.finish_content(out, names, t, w, d, digest, dll, compare=lambda c: []) == 0
    back = predfit.Content.load(out)
    assert (back.t, back.w, back.d, back.names, back.digest()) == (t, w, d, names, digest)
    doc = json.loads(out.read_text())
    assert doc["schema"] == predfit.SCHEMA and len(doc["tree"]) == 40 and len(doc["dll_sha256"]) == 64
    assert predfit.finish_content(tmp_path / "x.json", names, t, w, d, digest ^ 1, dll, compare=lambda c: []) == 1
    assert predfit.finish_content(tmp_path / "y.json", names, t[:-1], w, d, digest, dll, compare=lambda c: []) == 1
    assert predfit.finish_content(tmp_path / "z.json", names, t, w, d, digest, dll, compare=lambda c: ["x"]) == 1
    assert not (tmp_path / "x.json").exists() and not (tmp_path / "y.json").exists()
