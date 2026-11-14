"""The calibration runner's offline pieces on synthetic data: the trace, its checks, the metrics.

    python3 tools/tasim/tests/test_calib.py

No game and no recording: a Run is fed iterations built here byte by byte, in the order the
recorder writes them (engtrace.py, THE ITERATION OF AN EVENT MADE ON A PAUSED FRAME).
"""

import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import calibref  # noqa: E402
import engtrace  # noqa: E402
import metrics  # noqa: E402

FX = 1 << 16
VALUES = {"ARMPW": {"metal": 45.0, "energy": 900.0, "maxdamage": 300, "id": 5, "footprint": [2, 2]},
          "CORAK": {"metal": 40.0, "energy": 600.0, "maxdamage": 250, "id": 6, "footprint": [2, 2]}}
IDS = {5: "ARMPW", 6: "CORAK"}
R = 60.0
STORE = {"metal": 1000.0, "energy": 2000.0}


def unit_img(x, depth, hp, nano=0.0, alt=73):
    b = bytearray(0x148)
    struct.pack_into("<iii", b, engtrace.U_POS, x * FX, alt * FX, depth * FX)
    struct.pack_into("<f", b, engtrace.U_NANO, nano)
    struct.pack_into("<h", b, engtrace.U_HP, hp)
    return bytes(b)


# the six fields the economy pass recomputes: income, expense, energy then metal, and the maxima
PASS_FIELDS = (0x90, 0x94, 0x9C, 0xA0, 0xA4, 0xA8)


def player_img(deadline=91, fields=None):
    b = bytearray(0x208)
    struct.pack_into("<f", b, engtrace.PL_BASE_M, STORE["metal"])
    struct.pack_into("<f", b, engtrace.PL_BASE_E, STORE["energy"])
    struct.pack_into("<f", b, engtrace.PL_M, STORE["metal"])
    struct.pack_into("<f", b, engtrace.PL_E, STORE["energy"])
    b[engtrace.PL_BASEFLAG] = 1
    struct.pack_into("<i", b, engtrace.PL_PASS, deadline)
    for at, v in (fields or {}).items():
        struct.pack_into("<f", b, at, v)
    return bytes(b)


def create(slot, typ, owner, x=0, depth=0):
    return ("create", struct.pack("<IHBBIiiiBBH", slot + 1, typ, owner, 0, 1, x, 73, depth, 0, 0, 0))


def death(slot, killer_slot, img):
    return ("death", struct.pack("<IIBbBB", slot + 1, killer_slot + 1 if killer_slot is not None
                                 else 0, 0, 0, 0, 0) + img)


def shot(shooter_slot, target_slot, weapon):
    b = bytearray(36)
    b[0x19] = weapon & 0xFF
    b[0x23] = (weapon >> 8) << 4
    struct.pack_into("<HH", b, 0x1F, target_slot + 1, shooter_slot + 1)
    return ("shot", bytes(b))


def scenario(swap):
    owner = (lambda o: 1 - o) if swap else (lambda o: o)
    players = [{"slot": s, "storage": dict(STORE), "metal": int(STORE["metal"]),
                "energy": int(STORE["energy"])} for s in (0, 1)]
    return {"setup": {"players": players},
            "units": [{"id": "a", "type": "ARMPW", "owner": owner(0),
                       "orders": [{"cmd": "attack", "to": None, "target": "b", "after": 0}]},
                      {"id": "b", "type": "CORAK", "owner": owner(1),
                       "orders": [{"cmd": "attack", "to": None, "target": "a", "after": 0}]}]}


BLOCK = {"kind": ["winrate", "distribution"], "metrics": ["winner", "ttk", "survivors"],
         "limit": 600, "end_units": {"0": 0, "1": 0}}


def duel(swap, t0=61, t_end=70, a_dies=True, fields=None):
    """A duel run: a (scenario owner 0) and b, b hits a twice and kills it in step t_end, and the
    clear takes b; with `a_dies` false neither dies and the clear takes both."""
    cleared = [3] if a_dies else [2, 3]
    row = {"scenario": "x.json", "index": 1 if swap else 0, "swap": swap, "t_apply": t0,
           "t_end": t_end, "clear": {"cleared_slots": cleared}, "delayed": None,
           "apply": {"units": {"a": [2, 100, 200, 73], "b": [3, 400, 200, 73]}, "features": []}}
    exp = scenario(swap)
    run = engtrace.Run(row, exp, BLOCK, VALUES, IDS)
    seat_a, seat_b = (1, 0) if swap else (0, 1)
    players = [player_img(fields=fields) for _ in range(10)]
    run.empty_point({}, 0, b"", {}, b"")
    run.events(t0, [create(2, 5, seat_a, 100, 200), create(3, 6, seat_b, 400, 200)])
    hp_a = {t0 + 1: 300, t0 + 2: 300, t0 + 3: 150}
    prev = players
    for L in range(t0 + 1, t_end + 1):
        units = {2: unit_img(100, 200, hp_a.get(L, 150)), 3: unit_img(400, 200, 250)}
        run.images(L, units, players, prev, set(), {"feature_defs": []} if L == t0 + 1 else {})
        prev = players
        evs = []
        if L == t0 + 2:
            evs.append(shot(3, 2, 0x123))
        if L == t_end:
            if a_dies:
                evs.append(death(2, 3, unit_img(100, 200, 0)))
                evs.append(death(3, None, unit_img(410, 205, 240)))
            else:
                evs.append(death(2, None, unit_img(100, 200, 150)))
                evs.append(death(3, None, unit_img(410, 205, 240)))
        run.events(L, evs)
    return run


class TraceAndChecks(unittest.TestCase):

    def test_sides_follow_the_scenario_owner_under_the_swap(self):
        for swap in (False, True):
            run = duel(swap)
            self.assertIsNone(run.failed, run.failed)
            head = run.trace()[0]
            self.assertEqual(head["sides"], {"A": ["a"], "B": ["b"]})
            self.assertEqual(head["units"]["a"]["seat"], 1 if swap else 0)

    def test_the_clear_is_a_survivor_row_not_a_death(self):
        run = duel(False)
        last = run.trace()[-1]
        self.assertEqual(last["t"], 9)
        self.assertEqual(last["ev"], [["death", "a", "b"]])
        self.assertEqual(last["u"]["b"], [410 * FX, 73 * FX, 205 * FX, 240, 0.0])

    def test_the_apply_is_t0_and_images_lag_one_iteration(self):
        lines = duel(False).trace()
        self.assertEqual(lines[1]["t"], 0)
        self.assertEqual([e[0] for e in lines[1]["ev"]], ["create", "create"])
        self.assertEqual(lines[1]["u"]["a"][metrics.U_HP], 300)
        shot_line = next(ln for ln in lines[1:] if any(e[0] == "shot" for e in ln.get("ev", ())))
        self.assertEqual(shot_line["t"], 2)
        self.assertEqual(shot_line["ev"][0], ["shot", "b", 0x123, "a"])

    def test_a_live_unit_at_the_empty_point_fails_R1(self):
        row_run = duel(False)
        run = engtrace.Run(row_run.row, scenario(False), BLOCK, VALUES, IDS)
        run.empty_point({7: b""}, 0, b"", {}, b"")
        self.assertTrue(run.failed.startswith("R1"))

    def test_a_projectile_at_the_empty_point_fails_R2(self):
        run = engtrace.Run(duel(False).row, scenario(False), BLOCK, VALUES, IDS)
        run.empty_point({}, 3, b"", {}, b"")
        self.assertTrue(run.failed.startswith("R2"))

    def test_a_projectile_at_the_apply_fails_R2(self):
        run = engtrace.Run(duel(False).row, scenario(False), BLOCK, VALUES, IDS)
        run.apply_point(1)
        self.assertEqual(run.failed, "R2: 1 projectile(s) at the apply")
        run = engtrace.Run(duel(False).row, scenario(False), BLOCK, VALUES, IDS)
        run.apply_point(0)
        self.assertIsNone(run.failed)

    def test_a_create_the_result_does_not_name_fails_R1(self):
        run = engtrace.Run(duel(False).row, scenario(False), BLOCK, VALUES, IDS)
        run.empty_point({}, 0, b"", {}, b"")
        run.events(61, [create(2, 5, 0), create(9, 6, 1)])
        self.assertIn("result names", run.failed)

    def test_a_cleared_slot_that_died_twice_fails_R1(self):
        run = duel(False)
        run.failed = None
        run.events(70, [death(3, None, unit_img(0, 0, 1)), death(3, None, unit_img(0, 0, 1))])
        self.assertIn("cleared slot 3 has 2 deaths", run.failed)

    def test_R4_compares_deadlines_modulo_30_from_the_apply(self):
        a = [player_img(deadline=91)]
        self.assertEqual(engtrace.phased_diff(a, [player_img(deadline=181)], 61, 151), [])
        self.assertIn("phase", engtrace.phased_diff(a, [player_img(deadline=182)], 61, 151)[0])

    def test_R4_names_every_differing_field(self):
        a, b = bytearray(player_img()), bytearray(player_img())
        b[0x10], b[0x13], b[0x40] = 1, 1, 1
        self.assertEqual(engtrace.phased_diff([bytes(a)], [bytes(b)], 61, 61),
                         ["seat image 0 differs at +0x10", "seat image 0 differs at +0x40"])

    def test_R4_needs_the_applys_reset_of_the_pass_results(self):
        """Two runs whose seats carry the previous runs' pass results (stale and different) fail
        R4 on exactly those fields; with the applier's reset (tagpu_scenario.c, THE SEATS' PASS
        RESULTS: rates 0, maxima from the units after the apply and the base storage) they pass."""
        launch = dict(zip(PASS_FIELDS, (25.0, 1.0, 1.0, 0.0, 1000100.0, 1000000.0)))
        reload = dict(zip(PASS_FIELDS, (0.3, 10.0, 0.0, 0.0, 1000000.0, 1000000.0)))
        r0, r1 = duel(False, fields=launch), duel(False, fields=reload)
        r1.compare_run0(r0, {}, {}, False)
        self.assertEqual(sorted({f.split(" at ")[1] for f in r1.fields["R4"]}),
                         ["+0x90", "+0x94", "+0x9C", "+0xA4"])
        reset = dict(zip(PASS_FIELDS, (0.0, 0.0, 0.0, 0.0, STORE["energy"], STORE["metal"])))
        r0, r1 = duel(False, fields=reset), duel(False, fields=reset)
        r1.compare_run0(r0, {}, {}, False)
        self.assertEqual(r1.checks["R4"], "ok")

    def test_R6_fails_an_empty_or_mismatched_globals_image(self):
        r0, r1 = duel(False), duel(False)
        r0.empty["globals"], r1.empty["globals"] = bytes(8), b""
        r1.compare_run0(r0, {}, {}, False)
        self.assertIn("the globals image", r1.checks["R6"])
        r1.empty["globals"] = bytes(12)
        r1.compare_run0(r0, {}, {}, False)
        self.assertIn("8 bytes in run 0, 12 here", r1.checks["R6"])

    def test_R4_compares_the_wires_levels_as_f32(self):
        """0.1 is not an f32: the engine stores the nearest one, which must match."""
        players = [player_img() for _ in range(10)]
        exp = scenario(False)
        for p in exp["setup"]["players"]:
            p["storage"] = {"metal": 0.1, "energy": STORE["energy"]}
        img = bytearray(players[0])
        struct.pack_into("<f", img, engtrace.PL_BASE_M, 0.1)
        players[0] = players[1] = bytes(img)
        run = engtrace.Run(duel(False).row, exp, BLOCK, VALUES, IDS)
        run.after_apply({2: unit_img(100, 200, 300), 3: unit_img(400, 200, 250)}, players,
                        {"feature_defs": []})
        self.assertNotIn("R4", run.checks)

    def test_R6_names_a_global_outside_the_advancing_set(self):
        r0, r1 = duel(False), duel(False)
        g0 = bytes(8)
        r0.empty["globals"], r1.empty["globals"] = g0, b"\x01" + bytes(7)
        r1.compare_run0(r0, {0: "rngseed", 1: "camerax"}, {}, False)
        self.assertNotIn("R6", (r1.failed or ""))
        r1.empty["globals"] = bytes(4) + b"\x01" + bytes(3)
        r1.failed = None
        r1.compare_run0(r0, {0: "rngseed", 1: "camerax"}, {}, False)
        self.assertIn("camerax", r1.failed)

    def test_R6s_advancing_set_names_rows_of_the_checkouts_list(self):
        sys.path.insert(0, str(HERE.parents[1] / "replay"))
        import tarec
        rows = tarec.globals_list()[1]
        engtrace.advancing_rows(rows)
        renamed = [dict(r, name="pfweightx") if r["name"] == "pfweight" else r for r in rows]
        with self.assertRaisesRegex(engtrace.calibref.CalibError, r"\bpfweight\b"):
            engtrace.advancing_rows(renamed)


class Metrics(unittest.TestCase):

    def test_winner_ttk_survivors(self):
        lines = duel(False).trace()
        m = metrics.compute(lines, BLOCK, VALUES, R)
        self.assertEqual(m["winner"], "B")
        self.assertEqual(m["ttk"], {"value": 9, "censored": False})
        self.assertAlmostEqual(m["survivors"], 240 / 250)

    def test_the_metrics_are_per_side_under_the_swap(self):
        a = metrics.compute(duel(False).trace(), BLOCK, VALUES, R)
        b = metrics.compute(duel(True).trace(), BLOCK, VALUES, R)
        self.assertEqual(a, b)

    def test_no_elimination_ends_at_the_limit_censored(self):
        lines = duel(False, a_dies=False).trace()
        lines[0]["limit"] = 9
        m = metrics.compute(lines, dict(BLOCK, limit=9), VALUES, R)
        self.assertEqual(m["winner"], "B")         # a at half HP, b at 240/250
        self.assertEqual(m["ttk"], {"value": 9, "censored": True})

    def test_a_draw_scores_side_a_survivors(self):
        head = {"schema": "ta-calib-trace/1", "limit": 10,
                "units": {"a": {"type": "ARMPW", "side": "A"}, "b": {"type": "ARMPW", "side": "B"}}}
        lines = [head, {"t": 0, "ev": [["create", "a", "ARMPW", "A", 1, 0, 0, 0],
                                       ["create", "b", "ARMPW", "B", 1, 0, 0, 0]],
                        "u": {"a": [0, 0, 0, 300, 0.0], "b": [0, 0, 0, 300, 0.0]}},
                 {"t": 10, "u": {"a": [0, 0, 0, 150, 0.0], "b": [0, 0, 0, 150, 0.0]}}]
        m = metrics.combat(lines, VALUES, R)
        self.assertEqual(m["winner"], "draw")
        self.assertEqual(m["survivors"], 0.5)
        self.assertTrue(m["ttk"]["censored"])

    def test_a_nanoframe_counts_its_built_fraction(self):
        self.assertEqual(metrics.hv([0, 0, 0, 300, 0.25], 60.0, 300), 45.0)

    def test_income_is_the_difference_from_t0_per_side(self):
        head = {"schema": "ta-calib-trace/1", "limit": 3600, "units": {}}
        p = lambda prod: [0.0] * 6 + [prod, prod * 2]     # noqa: E731
        lines = [head, {"t": 0, "p": {"A": p(10.0), "B": p(0.0)}},
                 {"t": 1800, "p": {"A": p(40.0), "B": p(5.0)}},
                 {"t": 3600, "p": {"A": p(90.0), "B": p(5.0)}}]
        self.assertEqual(metrics.income(lines, "metal", 1800), {"A": 60.0, "B": 10.0})
        self.assertEqual(metrics.income(lines, "energy", 3600), {"A": 80.0, "B": 5.0})

    def test_travel_censors_the_units_that_never_arrive(self):
        head = {"schema": "ta-calib-trace/1", "limit": 100,
                "units": {h: {"type": "ARMPW", "side": "A", "dest": [1000, 0]} for h in "xyz"}}
        row = lambda x: [x * FX, 0, 0, 300, 0.0]           # noqa: E731
        lines = [head, {"t": 0, "u": {"x": row(0), "y": row(0), "z": row(0)}},
                 {"t": 10, "u": {"x": row(990)}}, {"t": 20, "u": {"y": row(960)}}]
        tr = metrics.travel(lines, 2, 100)
        # R_arr = 16 · 3 · ⌈√3⌉ = 96: x at 10, y at 20, z never
        self.assertEqual(tr["first"], {"value": 10, "censored": False})
        self.assertEqual(tr["median"], {"value": 20, "censored": False})
        self.assertEqual(tr["last"], {"value": 100, "censored": True})

    def test_exact_damage_series_and_its_first_difference(self):
        lines = duel(False).trace()
        s = metrics.exact_series(lines, "damage")
        self.assertEqual(s[0], [2, "a", 300, 150])
        other = [list(x) for x in s]
        other[0][3] = 151
        self.assertEqual(metrics.first_difference(s, other)[0], 0)
        self.assertIsNone(metrics.first_difference(s, [list(x) for x in s]))


class EngineLabel(unittest.TestCase):
    """calibref.engine_label: a reference names the commit the DLL embeds, and says when the tree
    has moved past it, on this repository's own history"""

    def test_the_dlls_commit_is_named_and_a_moved_head_is_said(self):
        head = calibref.git_commit()
        parent = subprocess.run(["git", "-C", str(calibref.TREE), "rev-parse", "HEAD~1"],
                                capture_output=True, text=True, check=True).stdout.strip()
        same = calibref.engine_label(head[:8], "ab" * 32)
        self.assertEqual((same["commit"], same["head"], same["head_matches"]), (head, head, True))
        self.assertEqual(same["dll_sha256"], "ab" * 32)
        older = calibref.engine_label(parent[:8], "cd" * 32)
        self.assertEqual((older["build"], older["commit"], older["head_matches"]),
                         (parent[:8], parent, False))

    def test_a_dirty_unknown_or_missing_build_is_refused(self):
        head = calibref.git_commit()
        for build in (head[:8] + "-dirty", "0123456789ab", None, ""):
            with self.assertRaises(calibref.CalibError):
                calibref.engine_label(build, "ab" * 32)


class SetupRoot(unittest.TestCase):
    """THE ROOT INVARIANT (calibref.calib_dir): a setup's root is exactly its own, retail's none of theirs."""

    SETUP = "twilight-2.0b98"

    def guard(self, setup, root):
        env = {k: v for k, v in os.environ.items() if k not in ("TACALIB_DIR", calibref.SETUP_ENV)}
        if setup:
            env[calibref.SETUP_ENV] = setup
        if root is not None:
            env["TACALIB_DIR"] = str(root)
        with mock.patch.dict(os.environ, env, clear=True):
            return calibref.calib_dir()

    def test_the_matrix(self):
        mod, other = calibref.setup_root(self.SETUP), calibref.setup_root("escalation")
        retail = calibref.main_checkout() / ".data" / "calib"
        elsewhere = Path(tempfile.gettempdir()) / "calib-elsewhere"
        self.assertEqual(self.guard(self.SETUP, mod), mod)
        for root in (retail, None, other, elsewhere, calibref.mods_root()):
            with self.assertRaises(calibref.CalibError, msg=str(root)):
                self.guard(self.SETUP, root)
        self.assertEqual(self.guard(None, retail), retail)
        self.assertEqual(self.guard(None, None), retail)
        self.assertEqual(self.guard(None, elsewhere), elsewhere)
        for root in (mod, other, calibref.mods_root()):
            with self.assertRaises(calibref.CalibError, msg=str(root)):
                self.guard(None, root)

    def test_a_setup_that_cannot_be_calibrated_is_refused(self):
        for name in ("no-such-setup", "../calib"):
            with self.assertRaises(calibref.CalibError):
                self.guard(name, calibref.setup_root(name))
        rows = json.loads(calibref.SETUPS.read_text())["setups"]
        for bad in (lambda s: s["goal"].get("outcome") != "impure-active", lambda s: s.get("levers")):
            name = next(s["name"] for s in rows if bad(s))
            with self.assertRaises(calibref.CalibError, msg=name):
                self.guard(name, calibref.setup_root(name))

    def test_scenarios_of_another_root_are_refused(self):
        with mock.patch.dict(os.environ, {calibref.SETUP_ENV: self.SETUP,
                                          "TACALIB_DIR": str(calibref.setup_root(self.SETUP))}):
            gen = calibref.gen_dir()
            calibref.check_scenario_root([gen / "group-000.json"])
            with self.assertRaises(calibref.CalibError):
                calibref.check_scenario_root([calibref.TREE / "scenarios" / "calib" / "duel-armpw-corak.json"])
        with mock.patch.dict(os.environ, {k: v for k, v in os.environ.items() if k != calibref.SETUP_ENV},
                             clear=True):
            calibref.check_scenario_root([calibref.TREE / "scenarios" / "calib" / "duel-armpw-corak.json"])
            with self.assertRaises(calibref.CalibError):
                calibref.check_scenario_root([calibref.setup_root(self.SETUP) / "scenarios" / "gen" / "g.json"])

    def test_the_arena_overlay_and_the_end(self):
        import engrun
        rec = {"name": "x", "goal": {"outcome": "impure-active"}, "calib": {"unit_limit": 1000,
                                                                          "end_type": "ARMT1SOLAR"}}
        with mock.patch.dict(os.environ, {calibref.SETUP_ENV: "x"}), \
                mock.patch.object(calibref, "setup_record", return_value=rec):
            cfg = calibref.arena()
            self.assertEqual((cfg["setup"], cfg["unit_limit"], cfg["end_type"]), ("x", 1000, "ARMT1SOLAR"))
            self.assertTrue(calibref.default_arena())
            with tempfile.TemporaryDirectory() as tmp:
                doc = json.loads(engrun.end_scenario(cfg, Path(tmp)).read_text())
                self.assertEqual((doc["units"][0]["type"], doc["units"][0]["pos"]),
                                 ("ARMT1SOLAR", json.loads(engrun.END.read_text())["units"][0]["pos"]))
                naval = calibref.arena(calibref.NAVAL_ARENA)
                doc = json.loads(engrun.end_scenario(naval, Path(tmp)).read_text())
                self.assertEqual((doc["units"][0]["type"], doc["units"][0]["pos"]),
                                 ("ARMT1SOLAR", naval["end"]["pos"]))
            land = calibref.arena(calibref.ARENA)
            self.assertNotEqual(engrun.arena_sha256(land), engrun.arena_sha256(
                {k: v for k, v in land.items() if k != "setup"}))
        with mock.patch.dict(os.environ, {k: v for k, v in os.environ.items() if k != calibref.SETUP_ENV},
                             clear=True):
            self.assertEqual(engrun.end_scenario(calibref.arena(calibref.ARENA), Path("/nonexistent")), engrun.END)


def fact():
    """A type's facts as pack.unit_facts gives them."""
    return {"sense": 300, "range": 200, "aoe": 50, "bmcode": 1, "shot_cost": [0.0, 0.0], "upkeep": 0.0,
            "foot": 2, "foot_min": 2, "heal": False, "slots": 1, "proj_records": {"land": 7, "naval": 7},
            "unbounded": None}


class GeneratedArena(unittest.TestCase):
    """calib.py on the packed lane's generated maps: arena_check through synthmap.check, laid_out against the
    retail twin, the packed admission and the lane end's open cell."""

    DUEL = calibref.TREE / "scenarios" / "calib" / "duel-armpw-corak.json"
    FACTS = {"ARMPW": fact(), "CORAK": fact()}

    def setUp(self):
        import calib
        self.calib = calib
        self.land = calibref.arena(calibref.PACKED_ARENA)
        self.naval = calibref.arena(calibref.PACKED_NAVAL_ARENA)

    def test_arena_check_is_synthmaps(self):
        import synthmap
        for kind, cfg in (("land", self.land), ("naval", self.naval)):
            line = self.calib.arena_check(cfg)
            self.assertTrue(line.startswith(f"a generated map ({kind}, sha256 {synthmap.sha256(kind)[:12]}): "), line)
            self.assertIn(f"{len(synthmap.sites(kind))} sites", line)
        with mock.patch.object(synthmap, "check", side_effect=ValueError("land sites 0 and 1: closer")):
            with self.assertRaisesRegex(calibref.CalibError, "closer"):
                self.calib.arena_check(self.land)

    def test_laid_out_on_the_twin_with_the_admission_and_the_end(self):
        self.calib.laid_out(self.land, [self.DUEL], self.FACTS)
        with self.assertRaisesRegex(calibref.CalibError, "void"):
            self.calib.laid_out(dict(self.land, end={"type": "CORFUS", "pos": [6020, 1184]}), [self.DUEL],
                                self.FACTS)
        with self.assertRaisesRegex(calibref.CalibError, "lane end"):
            self.calib.laid_out({k: v for k, v in self.land.items() if k != "end"}, [self.DUEL], self.FACTS)
        with self.assertRaisesRegex(calibref.CalibError, "seven islands"):          # its retail twin's rectangle
            self.calib.laid_out(self.naval, [self.DUEL], self.FACTS)
        with mock.patch.object(calibref, "check_positions"):
            with self.assertRaisesRegex(calibref.CalibError, "a land fight on the naval arena"):
                self.calib.laid_out(self.naval, [self.DUEL], self.FACTS)
        with self.assertRaisesRegex(calibref.CalibError, "footprint 0"):
            self.calib.laid_out(self.land, [self.DUEL], dict(self.FACTS, CORAK=dict(fact(), foot_min=0)))
        with tempfile.TemporaryDirectory() as tmp:
            off = json.loads(self.DUEL.read_text())
            off["units"][0]["pos"] = [100, 100]             # inside the generated map, outside the retail rectangle
            path = Path(tmp) / "duel-off.json"
            path.write_text(json.dumps(off))
            with self.assertRaisesRegex(calibref.CalibError, "outside the arena's flat rectangle"):
                self.calib.laid_out(self.land, [path], self.FACTS)


class LogLines(unittest.TestCase):
    """The lane's readers of the DLL's lines, on lines written as the DLL writes them."""

    class Log:
        def __init__(self, lines):
            self.lines = lines

        def wait(self, rx, timeout, start=0):
            import re
            return next((ln for ln in self.lines[start:] if re.search(rx, ln)), None)

    DELAY = ("scenario: delayed orders: issued {} of {}, refused 0, cancelled {} (deaths {}, cleared 0, "
             "skipped 0)")
    SCHED = ("scenario: scheduled creates of the apply at GameTime {}: made {} of {}, refused 0, skipped {}; "
             "orders issued 0, delayed 0, refused 0, skipped 0")

    def test_the_pause_ends(self):
        import engrun
        m = engrun.PAUSE_RX.search("pause: paused at GameTime 931 (every cell decided)")
        self.assertEqual((m.group(2), m.group(3), m.group(5)), ("931", None, "every cell decided"))
        m = engrun.PAUSE_RX.search("pause: paused at GameTime 931 (units of seat 1 <= 0)")
        self.assertEqual((m.group(2), m.group(3), m.group(4), m.group(5)), ("931", "1", "0", None))
        m = engrun.PAUSE_RX.search("pause: paused at GameTime 931")
        self.assertEqual((m.group(2), m.group(3), m.group(5)), ("931", None, None))
        self.assertIsNone(engrun.PAUSE_RX.search("pause: at 931 not taken: x").group(2))

    def test_every_delayed_report_is_summed(self):
        import engrun
        log = self.Log(["x", self.DELAY.format(2, 2, 0, 0), "y", self.DELAY.format(1, 3, 2, 2)])
        total, lines = engrun.delay_reports(log, 0, 5, "t")
        self.assertEqual((total["issued"], total["of"], total["cancelled"], len(lines)), (3, 5, 2, 2))
        with self.assertRaises(engrun.CouldNotRun):
            engrun.delay_reports(log, 0, 4, "t")       # the reports count 5: past the wire's 4

    def test_the_scheduled_report_is_its_applys(self):
        import engrun
        log = self.Log([self.SCHED.format(91, 2, 4, 2), self.SCHED.format(421, 4, 4, 0)])
        self.assertEqual(engrun.sched_report(log, 0, 4, "t", 421)["made"], 4)
        with self.assertRaises(engrun.CouldNotRun):
            engrun.sched_report(log, 0, 4, "t", 91)

    SEEDS = ("enginefix: run seeds before the creates: the wire's seed {} at GameTime {} seeds the simulation "
             "RNG, the game thread's CRT stream, the wind and the meteors; the wind draws on the next step and "
             "the next shower is at {}")
    # a DLL that seeds after the creates (08bc1561 up to the packed lane's S1) writes this line
    SEEDS_AFTER = ("enginefix: run seeds: the wire's seed {} at GameTime {} seeds the simulation RNG, the game "
                   "thread's CRT stream, the wind and the meteors; the wind draws on the next step and the next "
                   "shower is at {}")

    def test_a_dll_that_seeds_after_the_creates_cannot_play_a_lane(self):
        import engrun
        log = self.Log([self.SEEDS_AFTER.format(5, 91, 991)])
        with self.assertRaisesRegex(engrun.CouldNotRun, "no run-seeds line"):
            engrun.seed_report(log, 0, 5, "t", 91)
        src = (Path(__file__).resolve().parents[3] / "tagpu" / "ddraw" / "src" / "tagpu_patches.c").read_text()
        self.assertIn('"enginefix: run seeds before the creates: the wire\'s seed %u at GameTime %u seeds "', src)

    def test_the_seed_line_is_the_applys_with_the_wires_seed(self):
        import engrun
        # the previous clear's line (another GameTime) may come after the apply's first log index
        log = self.Log([self.SEEDS.format(7, 61, 961), "x", self.SEEDS.format(4294967295, 91, 991)])
        engrun.seed_report(log, 0, 4294967295, "t", 91)
        with self.assertRaisesRegex(engrun.CouldNotRun, "the wire's 5"):
            engrun.seed_report(log, 0, 5, "t", 91)
        with self.assertRaisesRegex(engrun.CouldNotRun, "no run-seeds line"):
            engrun.seed_report(log, 0, 7, "t", 121)                         # the switch off: no line
        twice = self.Log(log.lines + [self.SEEDS.format(4294967295, 91, 991)])
        with self.assertRaisesRegex(engrun.CouldNotRun, "2 run-seeds"):
            engrun.seed_report(twice, 0, 4294967295, "t", 91)


if __name__ == "__main__":
    unittest.main()
