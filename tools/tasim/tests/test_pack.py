"""The packed lane's planner (tools/tasim/pack.py): the stagger's positions, the sites and the gap rule, the
projectile bound, the bounds, the plan's determinism and coverage, the batch's expanded scenario and its pause
clause.

    python3 tools/tasim/tests/test_pack.py

Synthetic fights need no install. Corpus runs the plan over an unpacked reference generation
(default <calibration root>/../calib-rt, or $TACALIB_PACK_CORPUS) at K = 4, 8 and 16 on both
generated maps, with and without the sight-cap lever, and skips without one or without an install
(it reads the definitions through native.py).
"""

import copy
import os
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import calibref  # noqa: E402
import pack as P  # noqa: E402
import synthmap  # noqa: E402

ARENAS = {"land": HERE.parent / "calib-arena-packed.json",
          "naval": HERE.parent / "calib-arena-packed-naval.json"}
LAND = calibref.arena(ARENAS["land"])
SITES = synthmap.sites("land")


def geom(box, reach=500, rng=200, units=(2, 2), shot=(0.0, 1.0), upkeep=0.0, runs=25, extent=72, heal=False,
         proj=8):
    """A fight's geometry as fight_geometry gives it."""
    return {"box": box, "sense": 300, "range": rng, "aoe": 50, "reach": reach,
            "units": {0: units[0], 1: units[1]}, "types": ["X"], "extent": extent, "heal": heal,
            "proj_records": proj,
            "shot_cost": {0: list(shot), 1: list(shot)}, "upkeep": {0: upkeep, 1: upkeep},
            "block": {"runs": runs, "end_units": {"0": 0, "1": 0}, "kind": ["winrate"],
                      "area": "group/land/t1/x", "mapping": 1}}


def corpus(n=12, runs=5):
    """n synthetic fights of different sizes and lengths."""
    geoms, prior = {}, {}
    for i in range(n):
        p = f"/fights/fight-{i:03d}.json"
        w, h = 300 + 97 * i, 100 + 31 * (i % 5)
        geoms[p] = geom((4800, 1100, 4800 + w, 1100 + h), reach=400 + 50 * i, units=(1 + i % 6, 2 + i % 4),
                        runs=runs, extent=24 + 16 * (i % 4))
        prior[f"fight-{i:03d}"] = {"length": 300 + 113 * ((i * 7) % 11)}
    return geoms, prior


def check_plan(test, batches, geoms, prior, arena, bounds, K, jobs):
    seen = []
    flat = arena["flat"]["rect"]
    flat = (flat[0], flat[1], flat[0] + flat[2], flat[1] + flat[3])
    kx, kz, kw, kh = arena["keepout"]
    keep = (kx, kz, kx + kw, kz + kh)
    sites = synthmap.sites(arena["synthmap"])
    where = {}                                      # fight -> its place relative to its site's origin
    for b in batches:
        test.assertLessEqual(len(b), K)
        test.assertEqual([c.k for c in b], list(range(len(b))))
        fights = [c.job[0] for c in b]
        test.assertEqual(len(set(fights)), len(fights), "a batch holds two runs of one fight")
        test.assertEqual(len({c.site for c in b}), len(b), "two cells share a site")
        test.assertIsNone(P.bounds_problem([tuple(c.job) for c in b], geoms, bounds, K))
        lengths = [P._length(prior, c.job[0]) for c in b]
        test.assertEqual(lengths, sorted(lengths, reverse=True), "cell 0 is the longest")
        phase = P.batch_phase([tuple(c.job) for c in b], geoms)
        ps = P.positions(arena["unit_limit"], K, phase)
        for c in b:
            test.assertEqual(c.at, phase * c.k)
            test.assertEqual(c.offset[0] % P.GRID, 0)
            test.assertEqual(c.offset[1] % P.GRID, 0)
            test.assertEqual(c.pos, {0: ps[c.k], 1: ps[c.k]})
            sx, sz, sw, sh = sites[c.site]
            test.assertTrue(P._inside(P._grow(c.box, P.WALL_CLEAR), (sx, sz, sx + sw, sz + sh)),
                            f"{c.job}: its box ⊕ {P.WALL_CLEAR} leaves site {c.site}")
            rel = (c.offset[0] - sx, c.offset[1] - sz)
            test.assertEqual(where.setdefault(c.job[0], rel), rel, f"{c.job}: another place in its site")
            outer = P._grow(c.box, P.E + c.reach)
            test.assertTrue(P._inside(outer, flat), f"{c.job}: {outer} leaves the flat region")
            test.assertFalse(P._overlaps(outer, keep), f"{c.job}: {outer} reaches the keepout")
            for seat, lo, hi, _n in c.ranges:
                test.assertGreaterEqual(lo, ps[c.k])
                test.assertLess(hi, ps[c.k] + P.capacity(arena["unit_limit"], K, phase))
        for i, cj in enumerate(b):
            for ck in b[i + 1:]:
                xj, xk = geoms[cj.job[0]]["extent"], geoms[ck.job[0]]["extent"]
                test.assertGreater(P.gap(P._grow(cj.box, P.E), P._grow(ck.box, P.E)),
                                   max(cj.reach, ck.reach) + max(xj, xk), f"{cj.job} and {ck.job}")
        seen += [tuple(c.job) for c in b]
    test.assertEqual(sorted(seen), sorted((str(p), i) for p, i in jobs), "every job exactly once")


def weapon(flags, velocity=0, start=0, rng=0, reload=0, timer=0, burst=0, burstrate=0, decay=0, flight=0,
           section="W"):
    """A weapon entry as `tasim_run defs` gives it: its image rows (offset, size, hex, mask)."""
    rows = [(P.W_FLAGS, 4, flags), (P.W_VELOCITY, 4, velocity), (P.W_STARTVELOCITY, 4, start),
            (P.W_RANGE, 4, rng), (P.W_RELOAD, 2, reload), (P.W_TIMER, 2, timer), (P.W_BURST, 2, burst),
            (P.W_BURSTRATE, 2, burstrate), (P.W_DECAY, 2, decay), (P.W_FLIGHTTIME, 2, flight)]
    return {"section": section,
            "image": [[o, n, v.to_bytes(n, "little").hex(), -1] for o, n, v in rows]}


class Stagger(unittest.TestCase):
    def test_positions_at_1500_and_1000(self):
        self.assertEqual(P.scan_step(1500), 51)
        self.assertEqual(P.positions(1500, 4), [0, 30, 60, 90])
        self.assertEqual(P.capacity(1500, 16), 30)
        self.assertEqual(P.scan_step(1000), 34)
        self.assertEqual(P.positions(1000, 3), [0, 20, 40])
        self.assertEqual(P.capacity(1000, 8), 20)

    def test_a_healing_type_staggers_by_120(self):
        geoms = {"/f/a.json": geom((0, 0, 1, 1)), "/f/b.json": geom((0, 0, 1, 1), heal=True)}
        self.assertEqual(P.batch_phase([("/f/a.json", 0)], geoms), 30)
        self.assertEqual(P.batch_phase([("/f/a.json", 0), ("/f/b.json", 0)], geoms), 120)
        self.assertEqual(P.positions(1500, 4, 120), [0, 120, 240, 360])
        self.assertEqual(P.capacity(1500, 16, 120), 60)

    def test_the_identity(self):
        """A unit at P_k + p made phase·k steps after the reset is scanned on the steps p is."""
        for limit, phase in ((1500, 30), (1000, 30), (900, 30), (250, 30), (1500, 120), (1000, 120)):
            s, B = P.scan_step(limit), limit
            for k, pk in enumerate(P.positions(limit, 5, phase)):
                d = phase * k
                for p in (0, 3, 7):
                    if p >= P.capacity(limit, 5, phase):
                        continue
                    for j in range(200):
                        # the step's window is [s·j, s·j + s) mod B
                        alone = (p - s * j) % B < s
                        packed = (pk + p - s * (j + d)) % B < s
                        self.assertEqual(alone, packed, (limit, k, p, j))


class Sites(unittest.TestCase):
    def test_canonical_place_depends_on_the_fight_alone(self):
        g = geom((4800, 1100, 5310, 1290))
        for site in SITES:
            dx, dz = P.canonical(g, site)
            self.assertEqual((dx % P.GRID, dz % P.GRID), (0, 0))
            self.assertEqual((dx - site[0], dz - site[1]), P.canonical(g, (0, 0) + site[2:]))
            self.assertIsNone(P.site_problem(g, site))
            box = P._shift(g["box"], dx, dz)
            centre = ((box[0] + box[2]) / 2 - site[0], (box[1] + box[3]) / 2 - site[1])
            self.assertLessEqual(abs(centre[0] - site[2] / 2), P.GRID)
            self.assertLessEqual(abs(centre[1] - site[3] / 2), P.GRID)

    def test_site_problem_names_the_clearance(self):
        self.assertIn("does not fit", P.site_problem(geom((0, 0, 400, 500)), SITES[0]))      # 500 + 1280 > 1664
        self.assertIsNone(P.site_problem(geom((0, 0, 5248, 384)), SITES[0]))
        self.assertIn("does not fit", P.site_problem(geom((0, 0, 5249, 384)), SITES[0]))
        self.assertIn("WALL_CLEAR", P.site_problem(geom((0, 0, 10, 10), extent=200), SITES[0]))

    def test_gap_rule_grid_and_one_cell_a_site(self):
        geoms, _prior = corpus(10)
        cells = list(enumerate(geoms.values()))
        placed = P.assign(cells, LAND, SITES)
        self.assertEqual(len(placed), 10)
        self.assertEqual(len({s for s, _dx, _dz in placed.values()}), 10)
        boxes = []
        for k, g in cells:
            site, dx, dz = placed[k]
            self.assertEqual((dx, dz), P.canonical(g, SITES[site]))
            boxes.append((P._grow(P._shift(g["box"], dx, dz), P.E), g["reach"], g["extent"]))
        for i, (bj, rj, xj) in enumerate(boxes):
            for bk, rk, xk in boxes[i + 1:]:
                self.assertGreater(P.gap(bj, bk), max(rj, rk) + max(xj, xk))

    def test_first_fit_takes_the_first_free_site(self):
        g = geom((4800, 1100, 5000, 1200), reach=300)
        placed = P.assign([(0, g), (1, g), (2, g)], LAND, SITES)
        self.assertEqual([placed[k][0] for k in range(3)], [0, 1, 2])

    def test_a_far_reach_takes_a_middle_site_and_too_much_does_not_fit(self):
        far = geom((4800, 1100, 9100, 1300), reach=6700)
        small = geom((4800, 1100, 5300, 1200), reach=600)
        placed = P.assign([(0, far), (1, small), (2, small)], LAND, SITES)
        self.assertIsNotNone(placed)
        x, z, w, h = SITES[placed[0][0]]
        self.assertEqual(x, 6912, "the far reach stands in the middle column")
        self.assertIsNone(P.assign([(k, far) for k in range(4)], LAND, SITES))

    def test_keepout_is_clear(self):
        placed = P.assign([(0, geom((4800, 1100, 5000, 1200), reach=200))], LAND, SITES)
        site, dx, dz = placed[0]
        outer = P._grow(P._shift(geom((4800, 1100, 5000, 1200))["box"], dx, dz), P.E + 200)
        self.assertFalse(P._overlaps(outer, (0, 0, 1024, 1024)))


G16 = synthmap.ENGINE_GRAVITY
V10 = 10 << 16                                      # 10 world units a tick, 16.16


class ProjectileBound(unittest.TestCase):
    def test_shot_life_by_kind(self):
        line = weapon(P.WF_LINE, velocity=V10, rng=500)
        self.assertEqual(P.shot_life(line, G16, 85), 52 + 1)                 # ceil(516 / 10) + 1
        auto = weapon(P.WF_LINE | (1 << 27), rng=500, timer=90)
        self.assertEqual(P.shot_life(auto, G16, 85), 90 + 1)                 # noautorange, no velocity
        burn = weapon(P.WF_SELFPROP | P.WF_BURNBLOW, velocity=V10, rng=500, decay=10)
        self.assertEqual(P.shot_life(burn, G16, 85), 52 + 5 + 1)
        g = G16 / 65536
        two = weapon(P.WF_SELFPROP | P.WF_TWOPHASE, velocity=V10, start=V10 // 2, rng=500, flight=30)
        tp = 52 + 30
        self.assertEqual(P.shot_life(two, G16, 85), tp + P._fall(10, 512 + tp * 10 - 85, g) + 1)
        ball = weapon(P.WF_BALLISTIC, velocity=V10)
        self.assertEqual(P.shot_life(ball, G16, 85), 196 + 1)                # (10 + √(100 + 2g·427)) / g
        self.assertGreater(P.shot_life(ball, G16, 0), P.shot_life(ball, G16, 85))
        for flags in (1 << 8, P.WF_BALLISTIC | (1 << 15), P.WF_LINE | P.WF_VLAUNCH, 1 << 3):
            self.assertIsNone(P.shot_life(weapon(flags, velocity=V10), G16, 85), hex(flags))

    def test_slot_records(self):
        w = weapon(P.WF_LINE, velocity=V10, rng=500, reload=30, burst=2, burstrate=6)
        # life 53, window 53 + 2·6 + 1 = 66, fires at least ⌊0.7·30⌋ = 21 apart: 4 fires of 3 records
        self.assertEqual(P.slot_records(w, G16, 85), 12)
        self.assertEqual(P.slot_records(weapon(P.WF_LINE, velocity=V10, rng=500), G16, 85), 55)   # p = 1
        self.assertIsNone(P.slot_records(weapon(1 << 8), G16, 85))

    def facts(self, slots=(16, -1, -1, -1, -1, -1, -1, -1, -1, -1), w=None):
        w = w or weapon(P.WF_LINE, velocity=V10, rng=500, reload=30, burst=2, burstrate=6, section="GUN")
        unit = {"unitname": "XGUN", "weapons": {"0x1EE": "GUN", "0x1F2": "", "0x1F6": "", "0x220": "",
                                                 "0x224": ""}, "slots": list(slots),
                "image": [[P.U_FLAGS, 1, "00", -1], [P.U_FOOTX, 2, "0200", -1], [P.U_FOOTZ, 2, "0300", -1],
                          [P.U_BMCODE, 1, "01", -1]]}
        return P.unit_facts({"units": [unit], "weapons": [w]}, {"units": []})["XGUN"]

    def test_unit_facts(self):
        f = self.facts()
        self.assertEqual((f["slots"], f["foot"], f["foot_min"], f["unbounded"]), (1, 3, 2, None))
        self.assertEqual(f["proj_records"], {"land": 12, "naval": 12})
        self.assertIn("slot 4", self.facts(slots=(16, -1, -1, 17) + (-1,) * 6)["unbounded"])
        dropped = self.facts(w=weapon(1 << 8, section="GUN"))
        self.assertIsNone(dropped["proj_records"])
        self.assertIn("GUN", dropped["unbounded"])


class Bounds(unittest.TestCase):
    def setUp(self):
        self.bounds = P.default_bounds(LAND)

    def test_viewed_seat_follows_the_swap(self):
        g = {"/f/a.json": geom((0, 0, 1, 1), units=(12, 1)), "/f/b.json": geom((0, 0, 1, 1), units=(12, 1))}
        self.assertIn("viewed", P.bounds_problem([("/f/a.json", 0), ("/f/b.json", 0)], g, self.bounds, 4))
        self.assertIsNone(P.bounds_problem([("/f/a.json", 0), ("/f/b.json", 1)], g, self.bounds, 4))

    def test_projectiles_units_storage_capacity(self):
        g = {"/f/a.json": geom((0, 0, 1, 1), units=(5, 5), proj=1501)}
        self.assertIn("projectile", P.bounds_problem([("/f/a.json", 0)], g, self.bounds, 4))
        g1 = {"/f/a.json": geom((0, 0, 1, 1), proj=750), "/f/b.json": geom((0, 0, 1, 1), proj=751)}
        self.assertIn("projectile", P.bounds_problem([("/f/a.json", 0), ("/f/b.json", 1)], g1, self.bounds, 4))
        g = {"/f/a.json": geom((0, 0, 1, 1), units=(5, 5))}
        b = dict(self.bounds, units=9)
        self.assertIn("units", P.bounds_problem([("/f/a.json", 0)], g, b, 4))
        g2 = {"/f/a.json": geom((0, 0, 1, 1), shot=(0.0, 100.0))}
        self.assertIn("energy", P.bounds_problem([("/f/a.json", 0)], g2, self.bounds, 4))
        g3 = {"/f/a.json": geom((0, 0, 1, 1), units=(31, 1))}
        self.assertIn("the cell holds 30", P.bounds_problem([("/f/a.json", 0)], g3, dict(self.bounds, viewed=127),
                                                            4))


class Plan(unittest.TestCase):
    def test_deterministic_complete_distinct_bounded(self):
        geoms, prior = corpus(12, runs=5)
        jobs = [(p, i) for p in geoms for i in range(5)]
        for K in (1, 3, 8):
            for viewed in (20, 127):
                bounds = P.default_bounds(LAND, viewed)
                a = P.plan(jobs, LAND, K, bounds, prior, geoms)
                b = P.plan(list(reversed(jobs)), LAND, K, bounds, prior, copy.deepcopy(geoms))
                self.assertEqual(a, b)
                check_plan(self, a, geoms, prior, LAND, bounds, K, jobs)

    def test_an_unmapped_or_retail_arena_is_refused(self):
        geoms, prior = corpus(2, runs=1)
        with self.assertRaises(P.PackError):
            P.plan([(p, 0) for p in geoms], dict(LAND, mapping=0), 2, P.default_bounds(LAND), prior, geoms)
        retail = {k: v for k, v in LAND.items() if k != "synthmap"}
        with self.assertRaises(P.PackError):
            P.plan([(p, 0) for p in geoms], retail, 2, P.default_bounds(LAND), prior, geoms)

    def test_a_fight_too_big_for_any_batch_is_refused(self):
        geoms = {"/f/a.json": geom((0, 0, 1, 1), units=(31, 1))}
        with self.assertRaises(P.PackError):
            P.plan([("/f/a.json", 0)], LAND, 4, P.default_bounds(LAND, 127), {}, geoms)

    def test_pause_clause(self):
        geoms, prior = corpus(3, runs=2)
        jobs = [(p, i) for p in geoms for i in range(2)]
        batches = P.plan(jobs, LAND, 3, P.default_bounds(LAND, 127), prior, geoms)
        b = batches[0]
        text = P.pause_clause(b, 31)
        head, cells = text.split(" or cells ")
        self.assertEqual(head, f"pause at {31 + b[-1].at + P.L}")
        for c, part in zip(b, cells.split(";")):
            start, end, s0, s1 = part.split(",")
            self.assertEqual((int(start), int(end)), (31 + c.at, 31 + c.at + P.L))
            self.assertEqual([s0, s1], [f"{s}:{lo}-{hi}<={n}" for s, lo, hi, n in c.ranges])


DUEL = Path(calibref.__file__).resolve().parents[2] / "scenarios" / "calib" / "duel-armpw-corak.json"
VALUES = {"ARMPW": {"bmcode": 1}, "CORAK": {"bmcode": 1}}


class Wire(unittest.TestCase):
    def batch(self):
        return [P.Cell(job=(str(DUEL), 0), k=0, at=0, offset=(1280, 2560), site=0, pos={0: 0, 1: 0},
                       box=(0, 0, 1, 1), reach=1, zone=(0, 0, 1, 1), ranges=[(0, 0, 0, 0), (1, 0, 0, 0)]),
                P.Cell(job=(str(DUEL), 1), k=1, at=30, offset=(8192, 2560), site=1, pos={0: 30, 1: 30},
                       box=(0, 0, 1, 1), reach=1, zone=(0, 0, 1, 1), ranges=[(0, 30, 30, 0), (1, 30, 30, 0)])]

    def test_cells_are_translated_staggered_and_renamed(self):
        exp = P.batch_exp(self.batch(), LAND, VALUES)
        units = {u["id"]: u for u in exp["units"]}
        self.assertEqual(sorted(units), ["c0:a", "c0:b", "c1:a", "c1:b"])
        alone, _rec, _w, _e = P.perturb.run_wire(DUEL, 1, VALUES, LAND)
        b = next(u for u in alone["units"] if u["id"] == "b")
        self.assertEqual(units["c1:b"]["pos"], [b["pos"][0] + 8192, b["pos"][1] + 2560])
        self.assertEqual(units["c1:b"]["owner"], b["owner"])          # index 1 swaps the seats
        self.assertEqual((units["c1:b"]["at"], units["c1:b"]["slot"]), (30, 30))
        self.assertEqual((units["c0:a"]["at"], units["c0:a"]["slot"]), (0, 0))
        self.assertEqual(units["c1:a"]["orders"][0]["target"], "c1:b")
        self.assertEqual(exp["features"], [])

    def test_the_wire_carries_at_and_slot_or_is_refused(self):
        real = P.perturb.CLI._scn_wire

        def without_keys(exp):
            return "\n".join(ln.split(" at=")[0].split(" slot=")[0] for ln in real(exp).splitlines()) + "\n"

        def with_keys(exp):
            units = iter(exp["units"])
            out = []
            for ln in without_keys(exp).splitlines():
                if ln.startswith("unit "):
                    u = next(units)
                    ln += (f" at={u['at']}" if u.get("at") else "") + (f" slot={u['slot']}" if "slot" in u else "")
                out.append(ln)
            return "\n".join(out) + "\n"
        try:
            P.perturb.CLI._scn_wire = without_keys
            with self.assertRaises(P.PackError):
                P.wire(self.batch(), LAND, VALUES)
            P.perturb.CLI._scn_wire = with_keys
            text = P.wire(self.batch(), LAND, VALUES)
            units = [ln for ln in text.splitlines() if ln.startswith("unit ")]
            self.assertEqual([ln.endswith(" at=30 slot=30") for ln in units], [False, False, True, True])
        finally:
            P.perturb.CLI._scn_wire = real


def fact(bmcode=1, foot_min=2, unbounded=None):
    """A type's facts as unit_facts gives them."""
    return {"sense": 300, "range": 200, "aoe": 50, "bmcode": bmcode, "shot_cost": [0.0, 0.0], "upkeep": 0.0,
            "foot": max(foot_min, 2), "foot_min": foot_min, "heal": False, "slots": 1,
            "proj_records": None if unbounded else {"land": 7, "naval": 9}, "unbounded": unbounded}


class Admission(unittest.TestCase):
    def exp(self, cmd="attack", features=(), health=None, pos=(4800, 1100)):
        return {"units": [{"id": "a", "type": "XA", "owner": 0, "pos": list(pos), "health": health,
                           "orders": [{"cmd": cmd, "target": "b"}]},
                          {"id": "b", "type": "XB", "owner": 1, "pos": [pos[0] + 300, pos[1]], "orders": []}],
                "features": list(features)}

    def test_refusals(self):
        self.assertIsNone(P.admissible(self.exp()))
        self.assertIn("build", P.admissible(self.exp("build")))
        self.assertIn("feature", P.admissible(self.exp(features=[{}])))
        course = {"kind": ["distribution"], "end_units": {}}
        self.assertIn("course", P.admissible(self.exp("move"), course))
        unmapped = {"kind": ["winrate"], "end_units": {"0": 0}, "mapping": 0}
        self.assertIn("mapping", P.admissible(self.exp(), unmapped))
        self.assertIn("exact", P.admissible(self.exp(), {"kind": ["exact"], "end_units": {"0": 0}}))

    def test_refusals_by_the_facts_and_the_site(self):
        facts = {"XA": fact(), "XB": fact()}
        self.assertIsNone(P.admissible(self.exp(), None, facts, SITES[0]))
        self.assertIn("footprint 0", P.admissible(self.exp(), None, dict(facts, XA=fact(foot_min=0))))
        self.assertIsNone(P.admissible(self.exp(), None, dict(facts, XB=fact(bmcode=0, foot_min=0))))
        self.assertIn("no projectile bound",
                      P.admissible(self.exp(), None, dict(facts, XB=fact(unbounded="a kind with no projectile "
                                                                                    "bound"))))
        self.assertIn("above 100", P.admissible(self.exp(health=101), None, facts))
        self.assertIn("no definition", P.admissible(self.exp(), None, {"XA": fact()}))
        wide = self.exp()
        wide["units"][1]["pos"] = [4800 + 5300, 1100]
        self.assertIn("does not fit", P.admissible(wide, None, facts, SITES[0]))

    def test_proj_records_sum_on_the_kind(self):
        g = P.fight_geometry(self.exp(), {"XA": fact(), "XB": fact()}, "naval")
        self.assertEqual(g["proj_records"], 18)
        self.assertEqual(P.fight_geometry(self.exp(), {"XA": fact(), "XB": fact()})["proj_records"], 14)
        self.assertEqual(g["extent"], 8 * 2 + 8 + 16)
        with self.assertRaises(P.PackError):
            P.fight_geometry(self.exp(), {"XA": fact(), "XB": fact(unbounded="x")})


def corpus_root():
    env = os.environ.get("TACALIB_PACK_CORPUS")
    if env:
        return Path(env)
    try:
        return calibref.calib_dir().parent / "calib-rt"
    except calibref.CalibError:
        return None


class Corpus(unittest.TestCase):
    """The plan over the unpacked rt generation (the module header)."""

    @classmethod
    def setUpClass(cls):
        root = corpus_root()
        gen = root / "scenarios" / "gen" if root else None
        if not gen or not gen.is_dir():
            raise unittest.SkipTest(f"no reference generation at {root}")
        try:
            cls.facts = P.install_facts()
        except P.PackError as e:
            raise unittest.SkipTest(f"no definitions: {e}")
        cls.prior = P.prior_from_refs(root / "refs")
        cls.paths = sorted(p for p in gen.glob("*-*.json"))

    def test_both_maps(self):
        for kind, path in ARENAS.items():
            arena = calibref.arena(path)
            geoms, refused = P.load_fights(self.paths, arena, self.facts)
            self.assertTrue(geoms, kind)
            for why in refused.values():
                self.assertTrue("course" in why or "fight on the" in why, why)
            jobs = P.jobs_of(geoms)
            for K in (4, 8, 16):
                for viewed in (P.SIGHT_CAP, 127):
                    with self.subTest(kind=kind, K=K, viewed=viewed):
                        bounds = P.default_bounds(arena, viewed)
                        a = P.plan(jobs, arena, K, bounds, self.prior, geoms)
                        self.assertEqual(a, P.plan(jobs, arena, K, bounds, self.prior, geoms))
                        check_plan(self, a, geoms, self.prior, arena, bounds, K, jobs)


if __name__ == "__main__":
    unittest.main()
