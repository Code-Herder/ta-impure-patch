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


def test_the_distance_is_the_front_gap():
    """e*, B's unit nearest A's centroid (0, 100), is (300, 0); A's unit nearest it is 300 away. The centroids
    are 658 apart."""
    assert predfit.front_gap([[0, 0], [0, 200]], [[300, 0], [1000, 0]]) == 300


def test_the_gravity_table(tmp_path):
    t = tmp_path / "g.json"
    t.write_text(json.dumps({"maps": {"cavedog links cc": 8155}}))
    assert predfit.load_gravity(t) == {"cavedog links cc": 8155}
    for bad in ({"maps": {"x": 0}}, {"maps": {"x": 65537}}, {"maps": {"x": 8155.0}}, {"gravity": {}}):
        t.write_text(json.dumps(bad))
        try:
            predfit.load_gravity(t)
        except predfit.CouldNotRun:
            continue
        raise AssertionError(f"took {bad}")


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
    gravity = {land["map"]: 8155, naval["map"]: 8155}
    fights, skipped, refused = predfit.load_fights(tmp_path / "refs", c, gravity, arenas)
    assert sorted(f["id"] for f in fights) == ["group-001", "naval-001"], (skipped, refused)
    assert not skipped and not refused and all(f["gravity"] == 8155 for f in fights)
    # a map the gravity table lacks is refused by name
    fights, skipped, refused = predfit.load_fights(tmp_path / "refs", c, {land["map"]: 8155}, arenas)
    assert [f["id"] for f in fights] == ["group-001"] and [s for s, _w in refused] == ["naval-001"]
    assert "gravity table lacks" in refused[0][1]
    # a naval fight played on the land arena, and a group fight laid out off the land arena's rectangle
    _ref(tmp_path, "naval-002", land["map"], naval["a"], naval["b"], ["B"] * 25)
    _ref(tmp_path, "group-002", land["map"], naval["a"], land["b"], ["A"] * 25)
    fights, skipped, refused = predfit.load_fights(tmp_path / "refs", c, gravity, arenas)
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


def test_the_fit_ends_on_a_bound_it_is_pulled_past():
    """A loss whose minimum lies past BOX's upper bound for hit.straight.bias and below splash's lower bound:
    every stage keeps to the box, so the fit ends on both bounds and never beyond them."""
    import numpy as np
    bias, splash = predfit.NAMES.index("hit.straight.bias"), predfit.NAMES.index("splash")
    want = {bias: 9.0, splash: -1.0, predfit.TAU: 0.5}

    class Pulled(predfit.Fitter):
        def __init__(self):
            self.start, self.say, self.evals = list(predfit.START), lambda *_a: None, 0
            self.free, self.held, self.fights, self.prior = sorted(want), {}, [], 0.0

        def loss(self, x):
            self.evals += 1
            th = self.theta(x)
            return float(sum((th[k] - v) ** 2 for k, v in want.items()))

    th, _loss = Pulled().run()
    assert th[bias] == predfit.BOX[bias][1] and th[splash] == predfit.BOX[splash][0]
    assert np.isclose(th[predfit.TAU], 0.5, atol=1e-3)


def _train(n_by_kind):
    """training-half fights by kind, as many as asked: stems whose FNV-1a 32 is even"""
    out, i = [], 0
    for kind, n in n_by_kind.items():
        got = 0
        while got < n:
            stem = f"{kind}-{i:03d}"
            i += 1
            if not predfit.in_check_half(stem):
                out.append({"id": stem, "kind": kind})
                got += 1
    return out


def test_the_folds_are_stratified_deterministic_and_check_free():
    train = _train({"group": 66, "duel": 20, "defence": 14, "naval": 17})
    for rep in range(predfit.CV_REPS):
        fold = predfit.folds_of(train, predfit.CV_K, rep)
        assert fold == predfit.folds_of(train, predfit.CV_K, rep)
        assert set(fold) == {f["id"] for f in train}
        for kind in ("group", "duel", "defence", "naval"):
            sizes = [sum(1 for f in train if f["kind"] == kind and fold[f["id"]] == k) for k in range(predfit.CV_K)]
            assert max(sizes) - min(sizes) <= 1, (kind, sizes)
    assert predfit.folds_of(train, predfit.CV_K, 0) != predfit.folds_of(train, predfit.CV_K, 1)


def test_a_fit_refuses_a_check_half_fight():
    stem = next(f"group-{i:03d}" for i in range(1000) if predfit.in_check_half(f"group-{i:03d}"))
    try:
        predfit.Fitter(None, None, [{"id": stem, "kind": "group"}])
    except predfit.CouldNotRun as e:
        assert stem in str(e)
    else:
        raise AssertionError("a fit took a check-half fight")


def _scored(n, right):
    """n decisive fights (A wins all 25 runs), `right` of them given a positive margin"""
    return [({"id": f"group-{k}", "kind": "group", "W": 25, "n": 25, "limit": 0}, 1.0 if k < right else -1.0)
            for k in range(n)]


def test_the_choice_is_the_lowest_validation_loss_never_the_accuracy():
    rows = []
    for rep in range(4):
        rows.append((30.0, "homing", 900.0, _scored(10, 10)))       # every fight right, the worst loss
        rows.append((100.0, "homing", 700.0, _scored(10, 8)))
        rows.append((300.0, "homing", 500.0, _scored(10, 7)))       # plainly the best loss
        rows.append((1000.0, "homing", 650.0, _scored(10, 9)))
    table = predfit.cv_table(rows)
    choice = predfit.cv_choice(table)
    assert choice["prior"] == 300.0 and choice["val_loss"] == 2000.0
    assert (choice["right"], choice["decisive"]) == (28, 40)
    assert choice["kinds"] == {"group": [28, 40]} and choice["classes"] == {"ended": [28, 40]}


def test_a_choice_at_an_end_extends_the_priors_that_way():
    assert predfit.cv_extend([30.0, 100.0, 300.0], 100.0) is None
    assert predfit.cv_extend([30.0, 100.0, 300.0], 300.0) == [30.0, 100.0, 300.0, 900.0]
    assert predfit.cv_extend([30.0, 100.0, 300.0], 30.0) == [10.0, 30.0, 100.0, 300.0]


def test_the_fight_classes():
    f = {"n": 25}
    assert predfit.fight_class({**f, "limit": 0}) == "ended"
    assert predfit.fight_class({**f, "limit": 19}) == "long"
    assert predfit.fight_class({**f, "limit": 20}) == "stalemate"


def test_held_coefficients_are_not_free_and_are_listed_with_their_values():
    lib = predfit.Lib()
    content = predfit.synthetic_content()
    fights = [f for f in predfit.synthetic_fights(lib, content, predfit.START) if not predfit.in_check_half(f["id"])]
    held = {**predfit.HOLD, "splash": 0.75}
    ft = predfit.Fitter(lib, content, fights, say=lambda *_a: None, prior=300.0, held=held)
    names = [predfit.NAMES[k] for k in ft.free]
    for n, v in held.items():
        assert n not in names and ft.held[n] == v and ft.start[predfit.NAMES.index(n)] == v
    assert "hit.straight.bias" in names and "tau" in names


def test_emit_declares_the_file_to_the_replay_gate():
    """the generated file carries the simulation include and its SIM_FEATURE line, with impai_fight.c's test
    groups, the stock-limits twin among them: nothing at the link refuses a file with no function that lacks
    them"""
    import re
    text = predfit.emit_c(predfit.START, "a test.")
    assert '#include "tagpu_simreg.h"\n' in text
    got = re.search(r'^SIM_FEATURE\(impai_fitparams, SIM_PUPPET_OWNER, "([^"]*)"\);$', text, re.M)
    assert got
    fight = (predfit.TREE / "tagpu/ddraw/src/impai_fight.c").read_text()
    want = re.search(r'^SIM_FEATURE\(impai_fight, SIM_PUPPET_OWNER, "([^"]*)"\);$', fight, re.M)
    assert want and got.group(1) == want.group(1) == predfit.fight_groups()
    assert "stock-impai-fight" in got.group(1).split()
