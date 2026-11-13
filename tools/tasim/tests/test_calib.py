"""The calibration runner's offline pieces on synthetic data: the trace, its checks, the metrics.

    python3 tools/tasim/tests/test_calib.py

No game and no recording: a Run is fed iterations built here byte by byte, in the order the
recorder writes them (engtrace.py, THE ITERATION OF AN EVENT MADE ON A PAUSED FRAME).
"""

import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
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


if __name__ == "__main__":
    unittest.main()
