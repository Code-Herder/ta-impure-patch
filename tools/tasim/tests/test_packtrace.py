"""The packed lane's read-back (tools/tasim/engtrace.py, THE PACKED LANE) and its run's bookkeeping
(tools/tasim/engrun.py, THE PACKED LANE): a batch fed iteration by iteration as Lane.stream feeds it, each
cell's trace against the unpacked run it stands for, the P checks, and the plan, its resume and the launch's
reads.

    python3 tools/tasim/tests/test_packtrace.py

Synthetic iterations need no install and no game: the one duel of scenarios/calib, twice in one batch (index 0
and the swapped index 1), on the generated land arena.
"""

import hashlib
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import calibref  # noqa: E402
import engtrace  # noqa: E402
import metrics  # noqa: E402
import pack  # noqa: E402
import perturb  # noqa: E402

FX = 1 << 16
DUEL = (calibref.TREE / "scenarios" / "calib" / "duel-armpw-corak.json").resolve()
LAND = calibref.arena(calibref.PACKED_ARENA)
VALUES = {"ARMPW": {"metal": 45.0, "energy": 900.0, "maxdamage": 300, "id": 5, "footprint": [2, 2], "bmcode": 1},
          "CORAK": {"metal": 40.0, "energy": 600.0, "maxdamage": 250, "id": 6, "footprint": [2, 2], "bmcode": 1}}
IDS = {5: "ARMPW", 6: "CORAK"}
FACTS = {t: {"sense": 300, "range": 200, "aoe": 50, "bmcode": 1, "shot_cost": [0.0, 0.0], "upkeep": 0.0,
             "foot": 2, "foot_min": 2, "heal": False, "slots": 1, "bursts": 0,
             "proj_records": {"land": 7, "naval": 7}, "unbounded": None} for t in VALUES}
FIRST = {0: 1, 1: 1501}             # each seat's block at limit 1500: seat 0 from slot 1, seat 1 from 1501
OFFSETS = [(-3200, 512), (512, 512)]
T = 91
NAMES = {0: "pfcalls", 1: "pfexamined", 2: "pfexamined+4"}
PF = {"calls": 0, 0: 1, 1: 2}
S = 10


def cells():
    geoms, refused = pack.load_fights([DUEL], LAND, FACTS)
    assert not refused, refused
    members = [(str(DUEL), 0), (str(DUEL), 1)]
    placed = {k: (k, dx, dz) for k, (dx, dz) in enumerate(OFFSETS)}
    return pack._cells(members, geoms, LAND, 2, placed), geoms


def unit_img(x, depth, hp=200):
    b = bytearray(0x148)
    struct.pack_into("<iii", b, engtrace.U_POS, x * FX, 73 * FX, depth * FX)
    struct.pack_into("<h", b, engtrace.U_HP, hp)
    return bytes(b)


def player_img(p, live):
    b = bytearray(0x208)
    struct.pack_into("<f", b, engtrace.PL_BASE_M, p["storage"]["metal"])
    struct.pack_into("<f", b, engtrace.PL_BASE_E, p["storage"]["energy"])
    struct.pack_into("<f", b, engtrace.PL_M, p["metal"])
    struct.pack_into("<f", b, engtrace.PL_E, p["energy"])
    b[engtrace.PL_BASEFLAG] = 1
    struct.pack_into("<H", b, engtrace.PL_LIVE, live)
    return bytes(b)


def create(slot, typ, owner, x, depth):
    return ("create", struct.pack("<IHBBIiiiBBH", slot + 1, typ, owner, 0, 1, x * FX, 73 * FX, depth * FX,
                                  0, 0, 0))


def death(slot, killer, img):
    return ("death", struct.pack("<IIBbBB", slot + 1, killer + 1 if killer is not None else 0, 0, 0, 0, 0) + img)


def proj(attacker_slot, x, z, burst=0):
    b = bytearray(0x6C)
    struct.pack_into("<i", b, engtrace.PROJ_X, x * FX)
    struct.pack_into("<i", b, engtrace.PROJ_Z, z * FX)
    struct.pack_into("<I", b, engtrace.PROJ_ATTACKER, attacker_slot + 1 if attacker_slot is not None else 0)
    struct.pack_into("<H", b, engtrace.PROJ_BURST, burst)
    return bytes(b)


class Fight:
    """Two duels in one batch: cell 0 created at T, cell 1 at T + 30. In cell 0 `a` kills `b` at T + 20; in cell
    1 the unit named by `loser1` dies at T + 45; the batch pauses at `te` (default T + 45) and the clear takes the
    survivors. `hooks` {L: fn(units, projs)} change one iteration's images."""

    def __init__(self, loser1="a", te=None, sight_cap=pack.LEVER_SIGHT_CAP, hooks=None, pf_skip=None):
        self.cells, self.geoms = cells()
        self.te = te or T + 45
        self.loser1, self.hooks, self.pf_skip = loser1, hooks or {}, pf_skip
        crows, self.exps = [], []
        for c in self.cells:
            exp, record, wire, _ends = perturb.run_wire(DUEL, c.job[1], VALUES, LAND)
            self.exps.append(exp)
            crows.append({"scenario": "scenarios/calib/duel-armpw-corak.json",
                          "scenario_sha256": calibref.sha256_file(DUEL), "index": c.job[1], "seed": record["seed"],
                          "swap": record["swap"], "perturbation": record,
                          "wire_sha256": hashlib.sha256(wire.encode()).hexdigest(), "proj_bound": 7,
                          "decided": None, "cell": pack.cell_json(c)})
        self.slots = {}                                     # (k, handle) -> (slot, seat, x, z) translated
        for c, exp in zip(self.cells, self.exps):
            rank = {0: 0, 1: 0}
            for u in exp["units"]:
                s = u["owner"]
                self.slots[c.k, u["id"]] = (FIRST[s] + c.pos[s] + rank[s], s, u["pos"][0] + c.offset[0],
                                            u["pos"][1] + c.offset[1], VALUES[u["type"].upper()]["id"])
                rank[s] += 1
        survivors = [self.slots[0, "a"][0], self.slots[1, "b" if loser1 == "a" else "a"][0]]
        self.row = {"batch": 0, "K": 2, "t_apply": T, "t_end": self.te, "end": "cells",
                    "wire_sha256": hashlib.sha256(pack.wire(self.cells, LAND, VALUES).encode()).hexdigest(),
                    "cells": crows, "types": {t: {"slots": 1, "bursts": 0} for t in VALUES},
                    "apply": {"features": []}, "clear": {"cleared_slots": sorted(survivors)}, "delayed": None,
                    "wall": {"total": 4.0}}
        self.head = {"pack": {"first": [FIRST[0], FIRST[1]], "proj_cap": pack.PROJ_CAP, "sight_cap": sight_cap,
                              "S": S, "K": 2, "block": 1500}}

    def run(self):
        b = engtrace.Batch(self.row, self.head, LAND, VALUES, IDS)
        setup = self.exps[0]["setup"]["players"]
        alive = {}                                          # slot -> (x, z)
        dies = {T + 20: (0, "b", "a"), T + 45: (1, self.loser1, "b" if self.loser1 == "a" else "a")}
        for L in range(T, self.te + 2):
            units = {s: unit_img(x, z) for s, (x, z) in alive.items()}
            projs = []
            if L in self.hooks:
                self.hooks[L](self, units, projs)
            live = {0: 0, 1: 0}
            for (k, _h), (slot, seat, *_r) in self.slots.items():
                if slot in alive:
                    live[seat] += 1
            players = [player_img(setup[min(i, 1)], live.get(i, 0)) for i in range(10)]
            calls = (L % 150 + (5 if self.pf_skip == L else 0)) % 150
            g = struct.pack("<III", calls, 20 * (calls + 1), 20 * (calls + 1))
            w = SimpleNamespace(units=units, phead=(len(projs), len(projs)), proj=b"".join(projs),
                                heads=(0, 0, 5, 0), globals=g, tables={}, mapped=b"", grid=b"")
            evs = []
            for c in self.cells:
                if L == T + c.at:
                    for (k, _h), (slot, seat, x, z, typ) in sorted(self.slots.items()):
                        if k == c.k:
                            evs.append(create(slot, typ, seat, x, z))
                            alive[slot] = (x, z)
            if L in dies:
                k, loser, killer = dies[L]
                slot = self.slots[k, loser][0]
                evs.append(death(slot, self.slots[k, killer][0], unit_img(*alive[slot], hp=0)))
                del alive[slot]
            if L == self.te:
                for slot in self.row["clear"]["cleared_slots"]:
                    if slot in alive:
                        evs.append(death(slot, None, unit_img(*alive[slot])))
            orders = {(self.slots[c.k, h][0], 0) for c in b.cells for h, a in c.delayed if L == c.t0 + a + 1}
            b.feed(L, w, players, evs, orders, NAMES, PF)
            for e in evs:
                if e[0] == "death":
                    alive.pop(struct.unpack_from("<I", e[1])[0] - 1, None)
        b.compare_run0(b, NAMES, {}, False, b)
        b.finish()
        b.verdicts()
        return b


class Cells(unittest.TestCase):

    def test_a_clean_batch_reads_back_as_two_runs(self):
        b = Fight().run()
        self.assertIsNone(b.failed, b.failed)
        for name in engtrace.P_CHECKS:
            self.assertEqual(b.checks[name], "ok", name)
        c0, c1 = b.cells
        for c in b.cells:
            self.assertIsNone(c.failed, c.failed)
            self.assertFalse([ln["t"] for ln in c.trace()[1:] if "p" in ln])        # no seat rows
        self.assertEqual((c0.row["t_apply"], c0.row["t_end"], c0.row["end"]), (T, T + 20, "units"))
        self.assertEqual((c1.row["t_apply"], c1.row["t_end"], c1.row["end"]), (T + 30, T + 45, "units"))
        self.assertEqual(c1.row["pack"]["k"], 1)
        self.assertEqual(c0.row["wall"]["total"], 2.0)
        self.assertEqual(c0.row["pack"]["p3_headroom"], 20 - S - 1)

    def test_a_cells_trace_is_its_unpacked_runs(self):
        """Handles, sides, the creates at t = 0 and every position as the run alone writes them: the retail
        coordinates of its scenario file, t counted from the cell's own create."""
        f = Fight()
        b = f.run()
        for c, exp in zip(b.cells, f.exps):
            lines = c.trace()
            head = lines[0]
            self.assertEqual(sorted(head["units"]), ["a", "b"])
            want_sides = {"A": ["a"], "B": ["b"]}
            self.assertEqual(head["sides"], want_sides)
            self.assertEqual(head["units"]["a"]["seat"], 1 if c.swap else 0)
            creates = [e for e in lines[1]["ev"] if e[0] == "create"]
            self.assertEqual(lines[1]["t"], 0)
            self.assertEqual([e[1] for e in creates], ["a", "b"])
            for e in creates:
                u = next(u for u in exp["units"] if u["id"] == e[1])
                self.assertEqual((e[5] // FX, e[7] // FX), tuple(u["pos"]))
            rows = [ln["u"]["a"] for ln in lines[1:] if "a" in ln.get("u", {})]
            u = next(u for u in exp["units"] if u["id"] == "a")
            self.assertTrue(rows)
            self.assertTrue(all((r[0] // FX, r[2] // FX) == tuple(u["pos"]) for r in rows))
        m = metrics.compute(b.cells[0].trace(), b.cells[0].block, VALUES, 60.0)
        self.assertEqual(m["winner"], "A")
        self.assertEqual(m["ttk"]["value"], 20)

    def test_the_end_is_read_offline_and_the_trace_cut_there(self):
        b = Fight().run()
        c0 = b.cells[0]
        self.assertEqual(max(ln["t"] for ln in c0.trace()[1:]), 20)
        last = c0.trace()[-1]
        self.assertEqual(last["ev"], [["death", "b", "a"]])
        self.assertIn("a", last["u"])                       # the survivor's state at its end, as a clear writes it

    def test_P5_a_pause_before_a_cells_end(self):
        """P5 is the cell's own verdict: the batch's stays ok and never overwrites the failure, which build_refs
        reads under P_CHECKS (test_resume.py, the generation it fails)."""
        b = Fight(te=T + 40).run()
        self.assertIn("P5", engtrace.P_CHECKS)
        self.assertIn("P5", b.cells[1].failed)
        self.assertIn("paused at GameTime", b.cells[1].checks["P5"])
        self.assertIsNone(b.cells[0].failed)
        self.assertEqual((b.checks["P5"], b.cells[0].checks["P5"]), ("ok", "ok"))

    def test_a_cell_a_seat_down_to_its_end_units(self):
        b = Fight(loser1="b").run()
        self.assertEqual(b.cells[1].row["t_end"], T + 45)
        self.assertIsNone(b.failed, b.failed)


class PChecks(unittest.TestCase):

    def fails(self, b, name):
        self.assertTrue(b.checks[name].startswith("iteration"), b.checks)
        for c in b.cells:
            self.assertIn("batch 0: ", c.checks[name])
            self.assertIsNotNone(c.failed)

    def test_P1_a_unit_out_of_its_box(self):
        def hook(f, units, projs):
            slot, *_ = f.slots[0, "a"]
            units[slot] = unit_img(10, 10)
        self.fails(Fight(hooks={T + 5: hook}).run(), "P1")

    def test_P2_a_record_with_no_attacker_or_out_of_its_zone(self):
        def none(f, units, projs):
            projs.append(proj(None, 0, 0))
        self.fails(Fight(hooks={T + 5: none}).run(), "P2")

        def stray(f, units, projs):
            slot, _s, x, z, _t = f.slots[0, "a"]
            projs.append(proj(slot, x + 6000, z))
        self.fails(Fight(hooks={T + 5: stray}).run(), "P2")

        def home(f, units, projs):
            slot, _s, x, z, _t = f.slots[0, "a"]
            projs.append(proj(slot, x + 10, z))
        self.assertEqual(Fight(hooks={T + 5: home}).run().checks["P2"], "ok")

    def test_P4e_a_cell_past_its_projectile_bound(self):
        def many(f, units, projs):
            slot, _s, x, z, _t = f.slots[0, "a"]
            projs.extend(proj(slot, x, z) for _ in range(8))
        self.fails(Fight(hooks={T + 5: many}).run(), "P4e")

    def test_P4a_counts_burst_parents(self):
        def bursts(f, units, projs):
            slot, _s, x, z, _t = f.slots[0, "a"]
            projs.extend(proj(slot, x, z, burst=2) for _ in range(5))
        f = Fight(hooks={T + 5: bursts})
        f.head["pack"]["proj_cap"] = 11                     # 5 records, 2 live units' slots, 5 parents: 12
        self.fails(f.run(), "P4a")

    def test_P3_a_skipped_search_count(self):
        self.fails(Fight(pf_skip=T + 10).run(), "P3")

    def test_P4c_the_viewed_seats_deaths_against_the_sight_cap(self):
        self.assertEqual(Fight(loser1="b", sight_cap=1).run().checks["P4c"], "ok")
        # index 1 swaps the seats: its b is seat 0's, the viewed seat; cell 0's b is seat 1's
        self.fails(Fight(loser1="b", sight_cap=0).run(), "P4c")


class Plan(unittest.TestCase):

    def setUp(self):
        import engrun
        self.engrun = engrun
        self.cells, self.geoms = cells()
        self.tmp = tempfile.TemporaryDirectory()
        self.out = Path(self.tmp.name)
        self.jobs = [(str(DUEL), 0), (str(DUEL), 1)]

    def tearDown(self):
        self.tmp.cleanup()

    def test_cell_json_names_the_scenario_under_the_tree(self):
        d = pack.cell_json(self.cells[1])
        self.assertEqual(d["job"], ["scenarios/calib/duel-armpw-corak.json", 1])
        self.assertEqual(pack.cell_from_json(json.loads(json.dumps(d))), self.cells[1])

    def test_the_plan_reads_back_or_the_resume_is_refused(self):
        E = self.engrun
        E.write_plan(self.out, E.plan_doc([self.cells], 2, LAND, {"refs": "x", "lengths": {}}))
        self.assertEqual(E.read_plan(self.out, 2, LAND, self.jobs), [self.cells])
        with self.assertRaisesRegex(E.ResumeRefused, "K = 2"):
            E.read_plan(self.out, 3, LAND, self.jobs)
        with self.assertRaisesRegex(E.ResumeRefused, "other jobs"):
            E.read_plan(self.out, 2, LAND, self.jobs[:1])
        with self.assertRaisesRegex(E.ResumeRefused, "no plan.json"):
            E.read_plan(self.out / "none", 2, LAND, self.jobs)

    def test_done_batches_keeps_the_plans_batches_once(self):
        E = self.engrun
        key = E.batch_key(self.cells, self.geoms, LAND, 2)
        self.assertEqual(key[-1], 2)
        lane = self.out / "lane00-0001"
        lane.mkdir()
        head = {"dll": {"sha256": "d"}, "renderer": E.GDI, "pacing": "draw", "setup": None,
                "arena_sha256": E.arena_sha256(LAND), "pack": {"K": 2}, "setup_key": list(key)}
        row = {"batch": 0, "cells": [{"cell": pack.cell_json(c)} for c in self.cells]}
        (lane / "manifest.jsonl").write_text(json.dumps(head) + "\n" + json.dumps(row) + "\n")
        real = calibref.sha256_file
        dll = self.out / "ddraw.dll"
        with mock.patch.object(calibref, "sha256_file", side_effect=lambda f: "d" if f == dll else real(f)):
            done, base, replayed = E.done_batches(self.out, [self.cells], [key], 2, LAND, dll, False, True)
            self.assertEqual((done, base, replayed), ({0}, 1, 0))
            with self.assertRaisesRegex(E.ResumeRefused, "K 2, this run's 4"):
                E.done_batches(self.out, [self.cells], [key], 4, LAND, dll, False, True)
            with self.assertRaisesRegex(E.ResumeRefused, "not the plan's"):
                E.done_batches(self.out, [self.cells[:1]], [key], 2, LAND, dll, False, True)


class Engine(unittest.TestCase):
    """`calib.py engine`'s refusals before any game: a generated arena plays packed batches and only it does,
    and the packed lane's options need --pack."""

    def engine(self, arena, *args):
        import io
        import os
        from contextlib import redirect_stdout
        import calib
        out = io.StringIO()
        with mock.patch.dict(os.environ, {calibref.ARENA_ENV: str(arena)}), redirect_stdout(out):
            rc = calib.main(["engine", "--scenarios", "scenarios/calib/duel-armpw-corak.json", *args])
        return rc, out.getvalue()

    def test_refusals(self):
        rc, out = self.engine(calibref.ARENA, "--pack", "4")
        self.assertEqual(rc, 2)
        self.assertIn("a generated arena plays packed batches", out)
        rc, out = self.engine(calibref.PACKED_ARENA)
        self.assertEqual(rc, 2)
        self.assertIn("a generated arena plays packed batches", out)
        rc, out = self.engine(calibref.ARENA, "--nodraw")
        self.assertEqual((rc, "the packed lane's" in out), (2, True))
        rc, out = self.engine(calibref.PACKED_ARENA, "--pack", "4", "--mode", "fresh")
        self.assertEqual((rc, "reloaded batches" in out), (2, True))


class LaunchReads(unittest.TestCase):

    def reads(self, **over):
        import engrun
        base = 0x10000000
        v = {"base": base, "slots": 3002, "S": 1500, "los": 0, "cap": pack.PROJ_CAP, "sight": 127,
             "blocks": [base + 1 * 0x118, base + 1500 * 0x118, base + 1501 * 0x118, base + 3000 * 0x118]}
        v.update(over)
        vals = [v["base"], v["slots"], v["S"], v["los"], v["cap"], v["sight"], *v["blocks"]]
        with mock.patch.object(engrun, "peek_values", return_value=vals), \
                mock.patch.object(engrun.R._CLI_API, "_peek_uint", side_effect=lambda d: d["value"]):
            return engrun.pack_reads(None, None, {"unit_limit": 1500}, over.get("gravity", 8155), 127)

    def test_the_reads(self):
        import engrun
        got = self.reads()
        self.assertEqual((got["first"], got["block"], got["scan_step"], got["S"]), ([1, 1501], 1500, 51, 1500))
        with self.assertRaisesRegex(engrun.CouldNotRun, "Mapping"):
            self.reads(los=1)
        with self.assertRaisesRegex(engrun.CouldNotRun, "unit limit"):
            self.reads(blocks=[0x10000000 + 0x118, 0x10000000 + 1499 * 0x118, 0x10000000 + 1501 * 0x118,
                               0x10000000 + 3000 * 0x118])
        with self.assertRaisesRegex(engrun.CouldNotRun, "0x49C9D2"):
            self.reads(cap=2000)
        with self.assertRaisesRegex(engrun.CouldNotRun, "0x48292E"):
            self.reads(sight=20)
        with self.assertRaisesRegex(engrun.CouldNotRun, "gravity"):
            self.reads(gravity=9000)


if __name__ == "__main__":
    unittest.main()
