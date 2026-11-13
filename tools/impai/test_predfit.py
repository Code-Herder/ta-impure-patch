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
    names = ["None", "ARMPW", "CORAK"]
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
    assert not (tmp_path / "z.json").exists(), "a failed comparison wrote the file"
    out.write_text("an earlier run's")
    assert predfit.finish_content(out, names, t, w, d, digest, dll, compare=lambda c: ["x"]) == 1
    assert not out.exists(), "a failed run left an earlier file in place"


def test_type_0_is_the_reserved_none():
    names = ["None", "ARMPW"]
    c = predfit.Content(names, bytes(80), bytes(36), b"")
    fake = {"units": [{"unitname": "ARMPW", "image": [[0x1FA, 4, "00000000", -1], [0x192, 4, "00000000", -1]]}],
            "weapons": []}
    orig = predfit.native.run
    try:
        predfit.native.run = lambda *a, **k: (0, json.dumps(fake), "")
        assert predfit.compare_defs(c) == []
        c.names[0] = "ARMPW"
        assert any("reserved" in f for f in predfit.compare_defs(c))
    finally:
        predfit.native.run = orig


def _ref(tmp_path, stem, played, a_pos, b_pos, winners):
    """A reference directory for scenario `stem`, played on map `played`."""
    import hashlib
    scn = tmp_path / f"{stem}.json"
    scn.write_text(json.dumps({"units": [
        {"id": "a0", "type": "ARMPW", "owner": 0, "pos": a_pos, "orders": [{"cmd": "attack", "target": "b0"}]},
        {"id": "b0", "type": "CORAK", "owner": 1, "pos": b_pos}]}))
    d = tmp_path / "refs" / f"{stem}@x"
    d.mkdir(parents=True)
    (d / "c.json").write_text(json.dumps({
        "created": "2026-11-13", "setup": {"map": played},
        "scenario": {"file": str(scn), "sha256": hashlib.sha256(scn.read_bytes()).hexdigest(),
                     "calib": {"metrics": ["winner", "margin"], "area": f"{stem.split('-')[0]}/x"}},
        "runs": [{"start_check": "ok", "metrics": {"winner": w, "margin": 0.5 if w == "A" else -0.5}}
                 for w in winners]}))


def test_each_reference_on_its_own_arena(tmp_path):
    arenas = predfit.fight_arenas()
    land, naval = arenas[""], arenas["naval"]
    c = predfit.Content(["None", "ARMPW", "CORAK"], bytes(40 * 3), bytes(36), b"")
    _ref(tmp_path, "group-001", land["map"], land["a"], land["b"], ["A"] * 25)
    _ref(tmp_path, "naval-001", naval["map"], naval["a"], naval["b"], ["B"] * 25)
    fights, skipped, refused = predfit.load_fights(tmp_path / "refs", c, arenas)
    assert sorted(f["id"] for f in fights) == ["group-001", "naval-001"], (skipped, refused)
    assert not skipped and not refused
    # a naval fight played on the land arena, and a group fight laid out off the land arena's rectangle
    _ref(tmp_path, "naval-002", land["map"], naval["a"], naval["b"], ["B"] * 25)
    _ref(tmp_path, "group-002", land["map"], naval["a"], land["b"], ["A"] * 25)
    fights, skipped, refused = predfit.load_fights(tmp_path / "refs", c, arenas)
    assert sorted(f["id"] for f in fights) == ["group-001", "naval-001"]
    assert sorted(s for s, _w in refused) == ["group-002", "naval-002"]
    assert "a naval fight's arena" in dict(refused)["naval-002"]
    assert "outside the arena" in dict(refused)["group-002"]


def _check_half(kind, n, wrong):
    """n check-half fights of `kind`, every one decisive for A, the first `wrong` predicted for B."""
    out, i = [], 0
    while len(out) < n:
        sid = f"{kind}-{i:04d}"
        i += 1
        if predfit.in_check_half(sid):
            out.append(({"id": sid, "kind": kind, "area": f"{kind}/x", "W": 25, "n": 25},
                        -0.5 if len(out) < wrong else 0.5))
    return out


def _report(*parts):
    rows = [r for p in parts for r in p]
    lines = []
    ok, cnr, doc = predfit.check_report([f for f, _m in rows], [m for _f, m in rows], say=lines.append)
    return ok, cnr, doc, lines


def test_each_kind_with_20_decisive_must_reach_90():
    # overall 82/85 passes, the group fights' 22/25 does not: the check fails and names the kind
    ok, cnr, doc, lines = _report(_check_half("duel", 60, 0), _check_half("group", 25, 3))
    assert not ok and not cnr and doc["kinds"]["group"] == {
        "fights": 25, "decisive": 25, "agree": 22, "judged": True, "passed": False}
    assert lines[-1].startswith("predfit check: FAIL") and lines[-1].endswith("below 90%: group")
    # 18/20 is exactly 90 %: judged and passed
    ok, _cnr, doc, _l = _report(_check_half("duel", 60, 0), _check_half("group", 20, 2))
    assert ok and doc["kinds"]["group"]["judged"] and doc["kinds"]["group"]["passed"]


def test_a_kind_under_20_decisive_is_reported_never_judged():
    # 19 naval fights all wrong: too few to judge, and the overall 100/119 = 84 % is what fails
    ok, _cnr, doc, lines = _report(_check_half("duel", 100, 0), _check_half("naval", 19, 19))
    assert doc["kinds"]["naval"] == {"fights": 19, "decisive": 19, "agree": 0, "judged": False, "passed": False}
    assert any(ln.startswith("kind naval") and "too few to judge" in ln for ln in lines)
    assert not ok and "below 90%:" not in lines[-1]
    # 9 wrong naval fights beside 100 right duels: overall 100/109 passes, naval cannot fail it alone
    ok, _cnr, doc, _l = _report(_check_half("duel", 100, 0), _check_half("naval", 9, 9))
    assert ok and not doc["kinds"]["naval"]["judged"]
