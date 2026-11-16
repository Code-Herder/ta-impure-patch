"""The fight fit's offline parts (predfit.py): the coefficients file it writes, the split, a scenario read as a
fight. The fit itself is `predfit.py selftest`, which needs the native library."""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import predfit  # noqa: E402


def test_emit_is_the_tracked_file():
    """the tracked coefficients file is exactly what `emit` writes for its own coefficients and its own provenance
    line, whichever fit wrote it, so its includes and its SIM_FEATURE line are the emitter's of today"""
    import re
    text = predfit.PARAMS.read_text()
    what = re.match(r"/\* impai_fitparams\.c -- [^\n]*, as\n   (.*?) \*/\n", text, re.S)
    assert what
    assert predfit.emit_c(predfit.read_params(predfit.PARAMS), what.group(1)) == text


def test_params_read_back_bit_exact(tmp_path):
    for theta in ([predfit.f32(v) for v in predfit.SYN_TRUE], [predfit.f32(v) for v in predfit.START]):
        f = tmp_path / "p.c"
        f.write_text(predfit.emit_c(theta, "a test."))
        assert predfit.read_params(f) == theta


def test_split_is_by_stem():
    assert predfit.fnv1a32("") == 0x811C9DC5
    halves = [predfit.in_check_half(f"group-{i:03d}") for i in range(150)]
    assert 50 < sum(halves) < 100
    assert halves == [predfit.in_check_half(f"group-{i:03d}") for i in range(150)]


def test_scenario_fight():
    c = predfit.Content(["NONE", "ARMPW", "CORAK"], bytes(predfit.SIZES[0] * 3), bytes(36), b"")
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
    t = bytes(range(predfit.SIZES[0])) * 3
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


def _row(off, width, value, mask=-1):
    return [off, width, (value & ((1 << 8 * width) - 1)).to_bytes(width, "little").hex(), mask]


def _defs_case():
    """A content and the `tasim_run defs` document of the same files, equal in every compared field: ARMPW with
    one weapon; CORAK naming weapon3 behind an empty weapon2, as retail's commanders do; ARMNUKE whose weapon is
    a stockpile, left out of the content. EMG's [DAMAGE] rows name ARMPW and, in another case,
    CORAK."""
    import struct
    names = ["None", "ARMPW", "CORAK", "ARMNUKE"]
    units = [("ARMPW", 300, 1 << 16, 2, 2, [1] + [-1] * 9), ("CORAK", 350, 1 << 15, -3, 300, [1, -1, 2] + [-1] * 7),
             ("ARMNUKE", 5000, 0, 6, 6, [3] + [-1] * 9)]
    engine_slots = {"ARMPW": [1] + [0] * 9, "CORAK": [1, 0, 2] + [0] * 7, "ARMNUKE": [0] * 10}
    t = bytes(predfit.SIZES[0])
    doc = {"counts": {"weapon_slots": 4096}, "units": [], "weapons": []}
    for un, hp, speed, fx, fz, slots in units:
        t += struct.pack("<Ifi10HBBBBHBB10H", hp, 1.0, speed, *engine_slots[un], 0, min(max(fx, 0), 255),
                         min(max(fz, 0), 255), 0, 0, 0, 0, *([0] * 10))
        doc["units"].append({"unitname": un, "slots": slots, "image": [
            _row(0x1FA, 4, hp), _row(0x192, 4, speed), _row(0x14A, 2, fx), _row(0x14C, 2, fz)]})
    weapons = [(1, "EMG", 0, [["ARMPW", 10], ["corak", 7], ["DEFAULT", 5]]), (2, "GUN2", 0, []),
               (3, "NUKE", predfit.WF_STOCKPILE, [])]
    w = bytes(36)
    for wid, section, flags, damage in weapons:
        w += struct.pack("<iiHHHHfHHIIHBB", 180, 0, 30, 1, 5, 8, 0.5, 300, 100, flags | predfit.WF_NOTTOAIR,
                         0, 2 if wid == 1 else 0, 0, 0)
        doc["weapons"].append({"id": wid, "section": section, "damage_rows": len(damage), "damage": damage, "image": [
            _row(0xDC, 4, 180), _row(0x68, 4, 0), _row(0xE4, 2, 30), _row(0xEA, 2, 1), _row(0xD4, 2, 5),
            _row(0xD6, 2, 8), _row(0xD8, 4, struct.unpack("<I", struct.pack("<f", 0.5))[0]), _row(0x104, 2, 300),
            _row(0xEE, 2, 100), _row(0x111, 4, flags, 0x7FFFFFFF)]})
    d = struct.pack("<HHiHHi", 1, 0, 10, 2, 0, 7)
    return names, bytearray(t), bytearray(w), bytearray(d), doc


def _compare(names, t, w, d, doc):
    orig = predfit.native.run
    try:
        predfit.native.run = lambda *a, **k: (0, json.dumps(doc), "")
        return predfit.compare_defs(predfit.Content(names, bytes(t), bytes(w), bytes(d)))
    finally:
        predfit.native.run = orig


def test_type_0_is_the_reserved_none():
    names, t, w, d, doc = _defs_case()
    assert _compare(names, t, w, d, doc) == []
    names[0] = "ARMPW"
    assert any("reserved" in f for f in _compare(names, t, w, d, doc))


def test_compare_defs_names_each_field():
    """one difference at a time, each named; CORAK's weapon3 behind its empty weapon2, the stockpile left out,
    the NOTTOAIR bit and a [DAMAGE] name differing only by case all pass"""
    import struct
    R = predfit.SIZES[0]
    cases = [
        ("footprintx", lambda t, w, d, doc: t.__setitem__(R + 33, 3)),
        ("CORAK footprintz", lambda t, w, d, doc: t.__setitem__(2 * R + 34, 254)),
        ("CORAK weapon3", lambda t, w, d, doc: struct.pack_into("<H", t, 2 * R + 12 + 4, 0)),
        ("ARMNUKE weapon1", lambda t, w, d, doc: struct.pack_into("<H", t, 3 * R + 12, 3)),
        # a scripted reload on a slot whose weapon is no turret, past slot 2, at or below the reload
        ("ARMPW weapon1: a scripted reload 40", lambda t, w, d, doc: struct.pack_into("<H", t, R + 40, 40)),
        ("CORAK weapon4: a scripted reload 40", lambda t, w, d, doc: struct.pack_into("<H", t, 2 * R + 40 + 6, 40)),
        ("ARMPW weapon1: a scripted reload 30", lambda t, w, d, doc: (
            struct.pack_into("<I", w, 36 + 24, predfit.WF_NOTTOAIR | predfit.WF_TURRET),
            doc["weapons"][0]["image"][-1].__setitem__(2, predfit.WF_TURRET.to_bytes(4, "little").hex()),
            struct.pack_into("<H", t, R + 40, 30))),
        ("CORAK weapon1", lambda t, w, d, doc: doc["units"][1].__setitem__("slots", [2, -1, 2] + [-1] * 7)),
        ("EMG accuracy", lambda t, w, d, doc: struct.pack_into("<H", w, 36 + 20, 301)),
        ("EMG spray", lambda t, w, d, doc: struct.pack_into("<H", w, 36 + 22, 99)),
        ("EMG flags", lambda t, w, d, doc: struct.pack_into("<I", w, 36 + 24, predfit.WF_NOTTOAIR | 2)),
        ("EMG [DAMAGE]: engine 2 rows, files 2; row 1: engine CORAK 8", lambda t, w, d, doc:
            struct.pack_into("<i", d, 12, 8)),
        ("EMG [DAMAGE]: engine 1 rows, files 2", lambda t, w, d, doc: struct.pack_into("<H", w, 36 + 32, 1)),
        ("EMG [DAMAGE]: rows 0..3 past", lambda t, w, d, doc: struct.pack_into("<H", w, 36 + 32, 3)),
        ("EMG [DAMAGE]: two rows fold to armpw", lambda t, w, d, doc:
            doc["weapons"][0]["damage"].append(["armPW", 10])),
        ("EMG [DAMAGE]: engine 2 rows, files 3; row 2: engine none, files ARMNUKE 4", lambda t, w, d, doc:
            doc["weapons"][0]["damage"].append(["armnuke", 4])),
        ("CORAK: tasim_run defs gives no 10 weapon slots", lambda t, w, d, doc: doc["units"][1].pop("slots")),
    ]
    for want, edit in cases:
        names, t, w, d, doc = _defs_case()
        edit(t, w, d, doc)
        fails = _compare(names, t, w, d, doc)
        assert len(fails) == 1 and want in fails[0], (want, fails)
    names, t, w, d, doc = _defs_case()
    struct.pack_into("<I", w, 36 + 24, 0)
    doc["weapons"][0]["damage"][1][0] = "CORAK"
    assert _compare(names, t, w, d, doc) == []


def test_the_constants_are_the_headers():
    """the flag bits, the slot count and the weapon bound compare_defs works with are the C's"""
    import re
    roles = (predfit.TREE / "tagpu/ddraw/src/tasim_roles.h").read_text()
    for name, v in (("STOCKPILE", predfit.WF_STOCKPILE), ("INTERCEPTOR", predfit.WF_INTERCEPTOR),
                    ("NOTTOAIR", predfit.WF_NOTTOAIR)):
        m = re.search(rf"#define TASIM_WF_{name}\s+\(1u << (\d+)\)", roles)
        assert m and 1 << int(m.group(1)) == v, name
    assert re.search(rf"#define TASIM_ROLE_WEAPONS {predfit.FW_SLOTS}\b", roles)
    fight = (predfit.TREE / "tagpu/ddraw/src/impai_fight.h").read_text()
    assert re.search(rf"#define IMPAI_FWEAPONS\s+{predfit.FWEAPONS}\b", fight)


def _ref(tmp_path, stem, played, a_pos, b_pos, winners, mod=None, gravity=None):
    """A reference directory for scenario `stem`, played on map `played` (by setup `mod`, its lanes reading
    `gravity`: neither recorded when None, as before the reads)."""
    import hashlib
    scn = tmp_path / f"{stem}.json"
    scn.write_text(json.dumps({"units": [
        {"id": "a0", "type": "ARMPW", "owner": 0, "pos": a_pos, "orders": [{"cmd": "attack", "target": "b0"}]},
        {"id": "b0", "type": "CORAK", "owner": 1, "pos": b_pos}]}))
    d = tmp_path / "refs" / f"{stem}@x"
    d.mkdir(parents=True)
    (d / "c.json").write_text(json.dumps({
        "created": "2026-11-13", "setup": {"map": played, **({"gravity": gravity} if gravity is not None else {})},
        **({"engine": {"mod": mod}} if mod else {}),
        "scenario": {"file": str(scn), "sha256": hashlib.sha256(scn.read_bytes()).hexdigest(),
                     "calib": {"metrics": ["winner", "margin"], "area": f"{stem.split('-')[0]}/x"}},
        "runs": [{"start_check": "ok", "metrics": {"winner": w, "margin": 0.5 if w == "A" else -0.5}}
                 for w in winners]}))


def test_each_reference_on_its_own_arena(tmp_path):
    arenas = predfit.fight_arenas()
    land, naval = arenas[""][0], arenas["naval"][0]
    c = predfit.Content(["None", "ARMPW", "CORAK"], bytes(predfit.SIZES[0] * 3), bytes(36), b"")
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
    assert "a naval fight's arenas" in dict(refused)["naval-002"]
    assert "outside the arena" in dict(refused)["group-002"]


def test_a_packed_reference_is_its_kinds_twin(tmp_path):
    """the packed lane's references name the generated maps and keep the retail coordinates in their files; a
    naval fight on the generated land map is refused, and the gravity table cross-checks the packed lanes' reads"""
    import json as _json
    arenas = predfit.fight_arenas()
    (land, packed_land), (naval, packed_naval) = arenas[""], arenas["naval"]
    assert (packed_land["map"], packed_naval["map"]) == ("impure-calib-land", "impure-calib-naval")
    c = predfit.Content(["None", "ARMPW", "CORAK"], bytes(predfit.SIZES[0] * 3), bytes(36), b"")
    _ref(tmp_path, "group-001", packed_land["map"], land["a"], land["b"], ["A"] * 25, gravity=8155)
    _ref(tmp_path, "naval-001", packed_naval["map"], naval["a"], naval["b"], ["B"] * 25, gravity=8155)
    _ref(tmp_path, "naval-002", packed_land["map"], naval["a"], naval["b"], ["B"] * 25, gravity=8155)
    _ref(tmp_path, "group-002", packed_land["map"], land["a"], land["b"], ["A"] * 25, gravity=9000)
    table = {land["map"]: 8155, naval["map"]: 8155, packed_land["map"]: 8155, packed_naval["map"]: 8155}
    fights, skipped, refused = predfit.load_fights(tmp_path / "refs", c, table, arenas)
    assert sorted((f["id"], f["map"], f["gravity"]) for f in fights) == [
        ("group-001", "impure-calib-land", 8155), ("naval-001", "impure-calib-naval", 8155)], (skipped, refused)
    why = dict(refused)
    assert sorted(why) == ["group-002", "naval-002"]
    assert "a naval fight's arenas" in why["naval-002"]
    assert "the map gravity table says 8155" in why["group-002"]
    _ok, _cnr, doc = predfit.check_report(fights, [0.5, -0.5], say=lambda *_a: None)
    assert _json.loads(_json.dumps(doc))["arenas"] == {"impure-calib-land": 1, "impure-calib-naval": 1}


def test_a_reference_of_another_setup_is_refused_by_name(tmp_path):
    land = predfit.fight_arenas()[""][0]
    retail = predfit.Content(["None", "ARMPW", "CORAK"], bytes(predfit.SIZES[0] * 3), bytes(36), b"")
    mod = predfit.Content(["None", "ARMPW", "CORAK"], bytes(predfit.SIZES[0] * 3), bytes(36), b"",
                          setup="twilight-2.0b98")
    _ref(tmp_path, "group-001", land["map"], land["a"], land["b"], ["A"] * 25)
    _ref(tmp_path, "group-002", land["map"], land["a"], land["b"], ["A"] * 25, mod="retail")
    _ref(tmp_path, "group-003", land["map"], land["a"], land["b"], ["A"] * 25, mod="twilight-2.0b98", gravity=8155)
    table = {land["map"]: 8155}
    fights, _skipped, refused = predfit.load_fights(tmp_path / "refs", retail, table)
    assert sorted(f["id"] for f in fights) == ["group-001", "group-002"]
    assert refused == [("group-003", "played on twilight-2.0b98, and the content is retail's")]
    fights, _skipped, refused = predfit.load_fights(tmp_path / "refs", mod, table)
    assert [f["id"] for f in fights] == ["group-003"]
    assert sorted(s for s, _w in refused) == ["group-001", "group-002"]


def test_the_gravity_the_lanes_read_against_the_table(tmp_path):
    land = predfit.fight_arenas()[""][0]
    c = predfit.Content(["None", "ARMPW", "CORAK"], bytes(predfit.SIZES[0] * 3), bytes(36), b"")
    for stem, g in (("group-001", 8155), ("group-002", 9000), ("group-003", 0), ("group-004", 70000)):
        _ref(tmp_path, stem, land["map"], land["a"], land["b"], ["A"] * 25, gravity=g)
    fights, _skipped, refused = predfit.load_fights(tmp_path / "refs", c, {land["map"]: 8155})
    assert [(f["id"], f["gravity"]) for f in fights] == [("group-001", 8155)]
    why = dict(refused)
    assert "the map gravity table says 8155" in why["group-002"]
    assert "not 1..65536" in why["group-003"] and "not 1..65536" in why["group-004"]
    # a map the table lacks is no refusal when the lanes read the gravity: the table is the cross-check
    fights, _skipped, refused = predfit.load_fights(tmp_path / "refs", c, {})
    assert [(f["id"], f["gravity"]) for f in fights] == [("group-001", 8155), ("group-002", 9000)]


def test_a_content_file_of_either_schema_is_read(tmp_path):
    t, w, d, names = bytes(predfit.SIZES[0] * 3), bytes(36), b"", ["None", "ARMPW", "CORAK"]
    dll = tmp_path / "ddraw.dll"
    dll.write_bytes(b"dll")
    digest = predfit.Content(names, t, w, d).digest()
    reads = {"unit_cap": 1500, "speed_ceiling": 20, "gravity": 8155}
    out = tmp_path / "c3.json"
    assert predfit.finish_content(out, names, t, w, d, digest, dll, compare=lambda c: [], setup="twilight-2.0b98",
                                  reads=reads) == 0
    doc = json.loads(out.read_text())
    assert (doc["schema"], doc["setup"], doc["checked"], doc["engine_reads"]) == \
        ("impai-fight-content/4", "twilight-2.0b98", ["digest", "files"], reads)
    back = predfit.Content.load(out)
    assert (back.setup, back.checked) == ("twilight-2.0b98", ["digest", "files"])
    # a /3 file's types are 40 bytes, read with every scripted reload 0
    t3 = bytes(range(40)) * 3
    (tmp_path / "c3old.json").write_text(json.dumps(dict(doc, schema="impai-fight-content/3", ftype=t3.hex())))
    back = predfit.Content.load(tmp_path / "c3old.json")
    assert back.nt == 3 and back.t == b"".join(t3[k:k + 40] + bytes(20) for k in range(0, 120, 40))
    old = {k: v for k, v in doc.items() if k not in ("setup", "checked", "engine_reads")}
    (tmp_path / "c2.json").write_text(json.dumps(dict(old, schema="impai-fight-content/2", ftype=bytes(120).hex())))
    back = predfit.Content.load(tmp_path / "c2.json")
    assert (back.setup, back.checked, back.names) == (None, ["digest", "files"], names)
    (tmp_path / "c1.json").write_text(json.dumps(dict(old, schema="impai-fight-content/1")))
    try:
        predfit.Content.load(tmp_path / "c1.json")
        raise AssertionError("a /1 file was read")
    except predfit.CouldNotRun:
        pass


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


def test_the_tracked_params_name_their_provenance():
    assert predfit.provenance(predfit.PARAMS.read_text()) == ("defcfcdb2359", "3D002F542F3F142E")
    assert predfit.provenance(predfit.emit_c(predfit.START, "a test, fitted to no reference.")) is None


def test_check_refuses_another_reference_set(tmp_path, monkeypatch, capsys):
    """`check` runs only on the reference set and content its coefficients name: another set or another content
    could not run (exit 2), naming both; the named pair passes on to the margins"""
    import pytest
    c = predfit.Content(["None", "ARMPW", "CORAK"], bytes(predfit.SIZES[0] * 3), bytes(36), b"")
    fights = [{"ref": "group-001@x/c.json", "id": "group-001"}]
    named = predfit.refset_hash(fights)

    class Reached(Exception):
        pass

    class StubLib:
        def compiled_params(self):
            return []

        def margins(self, content, theta, fights):
            raise Reached()

    monkeypatch.setattr(predfit.Content, "load", classmethod(lambda cls, path: c))
    monkeypatch.setattr(predfit.Content, "enter_root", lambda self: None)
    monkeypatch.setattr(predfit, "load_gravity", lambda path: {})
    monkeypatch.setattr(predfit, "Lib", StubLib)
    monkeypatch.setattr(predfit, "load_fights", lambda refs, content, gravity: (fights, [], []))
    monkeypatch.setattr(predfit.calibref, "calib_dir", lambda: tmp_path)

    def check(refset, digest):
        params = tmp_path / "p.c"
        params.write_text(predfit.emit_c(predfit.START, f"fitted to the training half (1 fights) of reference set "
                                                        f"{refset}, content {digest}."))
        return predfit.main(["check", "--content", str(tmp_path / "c.json"), "--refs", str(tmp_path),
                             "--params", str(params)])

    assert check("0123456789ab", f"{c.digest():016X}") == 2
    out = capsys.readouterr().out
    assert named in out and "0123456789ab" in out and "TACALIB_DIR" in out
    assert check(named, "0123456789ABCDEF") == 2
    out = capsys.readouterr().out
    assert f"{c.digest():016X}" in out and "0123456789ABCDEF" in out
    with pytest.raises(Reached):
        check(named, f"{c.digest():016X}")


def test_an_evaluation_runs_on_another_set_and_reports_the_margins(tmp_path, monkeypatch, capsys):
    """`check --evaluation` runs coefficients on another reference set and content, says it is no check of the
    fit, exits 0 whatever the agreement and writes what packcheck's T4 reads; without it the same file could not
    run"""
    c = predfit.Content(["None", "ARMPW", "CORAK"], bytes(predfit.SIZES[0] * 3), bytes(36), b"")
    fights = [{"ref": "group-001@x/c.json", "id": "group-001", "kind": "group", "area": "group/land/t1/x",
               "W": 1.0, "n": 25, "map": "x"}]

    class StubLib:
        def compiled_params(self):
            return []

        def margins(self, content, theta, fights):
            return [(0, None, None, -0.25) for _f in fights]

    monkeypatch.setattr(predfit.Content, "load", classmethod(lambda cls, path: c))
    monkeypatch.setattr(predfit.Content, "enter_root", lambda self: None)
    monkeypatch.setattr(predfit, "load_gravity", lambda path: {})
    monkeypatch.setattr(predfit, "Lib", StubLib)
    monkeypatch.setattr(predfit, "load_fights", lambda refs, content, gravity: (fights, [], []))
    monkeypatch.setattr(predfit.calibref, "calib_dir", lambda: tmp_path)
    out_json = tmp_path / "eval.json"

    params = tmp_path / "p.c"
    params.write_text(predfit.emit_c(predfit.START, "fitted to the training half (1 fights) of reference set "
                                                    "0123456789ab, content 0123456789ABCDEF."))

    def evaluate(*flags):
        return predfit.main(["check", "--content", str(tmp_path / "c.json"), "--refs", str(tmp_path),
                             "--params", str(params), "--json", str(out_json), *flags])

    assert evaluate("--evaluation") == 0
    assert "EVALUATION" in capsys.readouterr().out
    doc = json.loads(out_json.read_text())
    assert doc["evaluation"] is True and doc["refset"] == predfit.refset_hash(fights)
    assert doc["params"]["fitted_set"] == "0123456789ab" and doc["params"]["fitted_content"] == "0123456789ABCDEF"
    assert doc["content"] == f"{c.digest():016X}"
    assert doc["margins"] == {"group-001@x": -0.25}
    assert evaluate() == 2
