"""The packed lane's planner (tools/tasim/pack.py): the stagger's positions, the layout's gap rule, the
bounds, the plan's determinism and coverage, the batch's expanded scenario and its pause clause.

    python3 tools/tasim/tests/test_pack.py

Synthetic fights need no install. Corpus runs the plan over an unpacked reference generation
(default <calibration root>/../calib-rt, or $TACALIB_PACK_CORPUS) at K = 4, 8 and 16 on both
generated maps, with and without the sight-cap lever, and skips without one or without an install
(it reads the definitions through native.py). The projectile peaks there are a stand-in, four per
unit: the planner's invariants do not depend on the values, and the measured peaks are an input.
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

ARENAS = {"land": HERE.parent / "calib-arena-packed.json",
          "naval": HERE.parent / "calib-arena-packed-naval.json"}
LAND = calibref.arena(ARENAS["land"])


def geom(box, reach=500, rng=200, units=(2, 2), shot=(0.0, 1.0), upkeep=0.0, runs=25, extent=72, heal=False):
    """A fight's geometry as fight_geometry gives it."""
    return {"box": box, "sense": 300, "range": rng, "aoe": 50, "reach": reach,
            "units": {0: units[0], 1: units[1]}, "types": ["X"], "extent": extent, "heal": heal,
            "shot_records": 1,
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
        prior[f"fight-{i:03d}"] = {"length": 300 + 113 * ((i * 7) % 11), "proj_peak": 20}
    return geoms, prior


def check_plan(test, batches, geoms, prior, arena, bounds, K, jobs):
    seen = []
    flat = arena["flat"]["rect"]
    flat = (flat[0], flat[1], flat[0] + flat[2], flat[1] + flat[3])
    kx, kz, kw, kh = arena["keepout"]
    keep = (kx, kz, kx + kw, kz + kh)
    for b in batches:
        test.assertLessEqual(len(b), K)
        test.assertEqual([c.k for c in b], list(range(len(b))))
        fights = [c.job[0] for c in b]
        test.assertEqual(len(set(fights)), len(fights), "a batch holds two runs of one fight")
        test.assertIsNone(P.bounds_problem([tuple(c.job) for c in b], geoms, prior, bounds, K))
        lengths = [P._length(prior, c.job[0]) for c in b]
        test.assertEqual(lengths, sorted(lengths, reverse=True), "cell 0 is the longest")
        phase = P.batch_phase([tuple(c.job) for c in b], geoms)
        ps = P.positions(arena["unit_limit"], K, phase)
        for c in b:
            test.assertEqual(c.at, phase * c.k)
            test.assertEqual(c.offset[0] % P.GRID, 0)
            test.assertEqual(c.offset[1] % P.GRID, 0)
            test.assertEqual(c.pos, {0: ps[c.k], 1: ps[c.k]})
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


class Layout(unittest.TestCase):
    def test_gap_rule_and_grid(self):
        geoms, _prior = corpus(10)
        cells = list(enumerate(geoms.values()))
        offsets = P.layout(cells, LAND)
        self.assertEqual(len(offsets), 10)
        boxes = []
        for k, g in cells:
            dx, dz = offsets[k]
            self.assertEqual((dx % P.GRID, dz % P.GRID), (0, 0))
            b = g["box"]
            boxes.append(((b[0] + dx - P.E, b[1] + dz - P.E, b[2] + dx + P.E, b[3] + dz + P.E), g["reach"],
                          g["extent"]))
        for i, (bj, rj, xj) in enumerate(boxes):
            for bk, rk, xk in boxes[i + 1:]:
                self.assertGreater(P.gap(bj, bk), max(rj, rk) + max(xj, xk))

    def test_the_largest_footprint_widens_the_gap(self):
        a = geom((4800, 1100, 5000, 1200), reach=300, extent=24)
        b = geom((4800, 1100, 5000, 1200), reach=300, extent=72)
        narrow = P.layout([(0, a), (1, a)], LAND)
        wide = P.layout([(0, a), (1, b)], LAND)
        self.assertGreaterEqual(wide[1][0] - wide[0][0], narrow[1][0] - narrow[0][0])
        bx = [P._grow((g["box"][0] + o[0], g["box"][1] + o[1], g["box"][2] + o[0], g["box"][3] + o[1]), P.E)
              for g, o in ((a, wide[0]), (b, wide[1]))]
        self.assertGreater(P.gap(*bx), 300 + 72)

    def test_extent_is_eight_per_cell_plus_the_ring(self):
        facts = {"ARMBATS": {"sense": 0, "range": 0, "aoe": 0, "foot": 6, "heal": False, "shot_records": 1,
                             "shot_cost": [0, 0], "upkeep": 0.0},
                 "ARMPW": {"sense": 0, "range": 0, "aoe": 0, "foot": 2, "heal": False, "shot_records": 4,
                           "shot_cost": [0, 0], "upkeep": 0.0}}
        exp = {"units": [{"id": "a", "type": "ARMBATS", "owner": 0, "pos": [0, 0], "orders": []},
                         {"id": "b", "type": "ARMPW", "owner": 1, "pos": [9, 9], "orders": []}]}
        g = P.fight_geometry(exp, facts)
        self.assertEqual(g["extent"], 8 * 6 + 8 + 16)
        self.assertEqual(g["shot_records"], 4)

    def test_a_far_reach_stands_alone_and_too_much_does_not_fit(self):
        far = geom((4800, 1100, 9100, 1340), reach=6700)
        small = geom((4800, 1100, 5300, 1200), reach=600)
        offsets = P.layout([(0, far), (1, small), (2, small)], LAND)
        self.assertIsNotNone(offsets)
        self.assertNotEqual(offsets[0][1], offsets[1][1])
        self.assertIsNone(P.layout([(k, far) for k in range(4)], LAND))

    def test_keepout_is_clear(self):
        offsets = P.layout([(0, geom((4800, 1100, 5000, 1200), reach=200))], LAND)
        b = geom((4800, 1100, 5000, 1200))["box"]
        dx, dz = offsets[0]
        outer = P._grow((b[0] + dx, b[1] + dz, b[2] + dx, b[3] + dz), P.E + 200)
        self.assertFalse(P._overlaps(outer, (0, 0, 1024, 1024)))


class Bounds(unittest.TestCase):
    def setUp(self):
        self.bounds = P.default_bounds(LAND)

    def test_viewed_seat_follows_the_swap(self):
        g = {"/f/a.json": geom((0, 0, 1, 1), units=(12, 1)), "/f/b.json": geom((0, 0, 1, 1), units=(12, 1))}
        prior = {"a": {"proj_peak": 1}, "b": {"proj_peak": 1}}
        self.assertIn("viewed", P.bounds_problem([("/f/a.json", 0), ("/f/b.json", 0)], g, prior, self.bounds, 4))
        self.assertIsNone(P.bounds_problem([("/f/a.json", 0), ("/f/b.json", 1)], g, prior, self.bounds, 4))

    def test_projectiles_units_storage_capacity(self):
        g = {"/f/a.json": geom((0, 0, 1, 1), units=(5, 5))}
        self.assertIn("projectile", P.bounds_problem([("/f/a.json", 0)], g, {"a": {"proj_peak": 1501}},
                                                     self.bounds, 4))
        b = dict(self.bounds, units=9)
        self.assertIn("units", P.bounds_problem([("/f/a.json", 0)], g, {"a": {"proj_peak": 1}}, b, 4))
        g2 = {"/f/a.json": geom((0, 0, 1, 1), shot=(0.0, 100.0))}
        self.assertIn("energy", P.bounds_problem([("/f/a.json", 0)], g2, {"a": {"proj_peak": 1}},
                                                 self.bounds, 4))
        g3 = {"/f/a.json": geom((0, 0, 1, 1), units=(31, 1))}
        self.assertIn("the cell holds 30", P.bounds_problem([("/f/a.json", 0)], g3, {"a": {"proj_peak": 1}},
                                                            dict(self.bounds, viewed=127), 4))

    def test_a_missing_peak_is_refused(self):
        g = {"/f/a.json": geom((0, 0, 1, 1))}
        with self.assertRaises(P.PackError):
            P.bounds_problem([("/f/a.json", 0)], g, {"a": {"length": 9}}, self.bounds, 4)


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

    def test_an_unmapped_arena_is_refused(self):
        geoms, prior = corpus(2, runs=1)
        with self.assertRaises(P.PackError):
            P.plan([(p, 0) for p in geoms], dict(LAND, mapping=0), 2, P.default_bounds(LAND), prior, geoms)

    def test_a_fight_too_big_for_any_batch_is_refused(self):
        geoms = {"/f/a.json": geom((0, 0, 1, 1), units=(31, 1))}
        with self.assertRaises(P.PackError):
            P.plan([("/f/a.json", 0)], LAND, 4, P.default_bounds(LAND, 127), {"a": {"proj_peak": 1}}, geoms)

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
        return [P.Cell(job=(str(DUEL), 0), k=0, at=0, offset=(1280, 2560), pos={0: 0, 1: 0}, box=(0, 0, 1, 1),
                       reach=1, zone=(0, 0, 1, 1), ranges=[(0, 0, 0, 0), (1, 0, 0, 0)]),
                P.Cell(job=(str(DUEL), 1), k=1, at=30, offset=(8192, 2560), pos={0: 30, 1: 30}, box=(0, 0, 1, 1),
                       reach=1, zone=(0, 0, 1, 1), ranges=[(0, 30, 30, 0), (1, 30, 30, 0)])]

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


class Admission(unittest.TestCase):
    def exp(self, cmd="attack", features=()):
        return {"units": [{"id": "a", "orders": [{"cmd": cmd, "target": "b"}]}], "features": list(features)}

    def test_refusals(self):
        self.assertIsNone(P.admissible(self.exp()))
        self.assertIn("build", P.admissible(self.exp("build")))
        self.assertIn("feature", P.admissible(self.exp(features=[{}])))
        course = {"kind": ["distribution"], "end_units": {}}
        self.assertIn("course", P.admissible(self.exp("move"), course))
        unmapped = {"kind": ["winrate"], "end_units": {"0": 0}, "mapping": 0}
        self.assertIn("mapping", P.admissible(self.exp(), unmapped))
        self.assertIn("exact", P.admissible(self.exp(), {"kind": ["exact"], "end_units": {"0": 0}}))


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
            prior = P.with_peaks(self.prior, {Path(p).stem: 4 * sum(g["units"].values())
                                              for p, g in geoms.items()})
            jobs = P.jobs_of(geoms)
            for K in (4, 8, 16):
                for viewed in (P.SIGHT_CAP, 127):
                    with self.subTest(kind=kind, K=K, viewed=viewed):
                        bounds = P.default_bounds(arena, viewed)
                        a = P.plan(jobs, arena, K, bounds, prior, geoms)
                        self.assertEqual(a, P.plan(jobs, arena, K, bounds, prior, geoms))
                        check_plan(self, a, geoms, prior, arena, bounds, K, jobs)


if __name__ == "__main__":
    unittest.main()
