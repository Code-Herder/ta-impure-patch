#!/usr/bin/env python3
"""Tests of ImpAI's offline tools: l1_run's levers removed however the run ends, impai_check's (b)
known digests compared tick by tick and (e) each level's patches judged against that level's own metal
dump, the logs read in part order, and roles_check's engine lines read with their water damage. No game
runs.

    python3 tools/impai/test_impai_tools.py
"""
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import impai_check  # noqa: E402
import l1_run  # noqa: E402
import roles_check  # noqa: E402


class L1RunLevers(unittest.TestCase):
    """Every lever l1_run writes into the worker's game folder is gone when it returns, whether the
    game ran or could not."""

    def run_main(self, gamedir, out, fail):
        worker = SimpleNamespace(gamedir=gamedir, name="tareplay-test", held=False, turbo=0,
                                 place=lambda fix, dll: None, close=lambda: None, stop=lambda hard: None)
        argv = ["l1_run.py", str(HERE / "unused.json"), "--end", "30", "--out", str(out),
                "--lever", "impai_check.on", "--lever", "impaiov.on=1:air"]
        seen = {}

        def tacli(*a, **k):
            seen["levers"] = sorted(p.name for p in gamedir.glob("tagpu_*"))
            if fail:
                return 1, "refused"
            raise AssertionError("the test stops at the load")
        with patch.object(sys, "argv", argv), patch.object(l1_run.runlock, "hold"), \
             patch.object(l1_run.tr, "Worker", return_value=worker), \
             patch.object(l1_run.tr, "Log"), patch.object(l1_run.tr, "tacli", side_effect=tacli), \
             patch.object(l1_run.talog, "run_text", return_value=""), patch("builtins.print"):
            try:
                rc = l1_run.main()
            except AssertionError:
                rc = None
        return rc, seen

    def test_removed_when_the_game_cannot_run(self):
        with tempfile.TemporaryDirectory() as d:
            g = Path(d) / "gamedir"
            g.mkdir()
            rc, seen = self.run_main(g, Path(d) / "out", fail=True)
            self.assertEqual(rc, 2)
            self.assertEqual(seen["levers"], ["tagpu_impai_check.on", "tagpu_impaiov.on"])
            self.assertEqual(list(g.glob("tagpu_*")), [])

    def test_removed_on_an_unexpected_error(self):
        with tempfile.TemporaryDirectory() as d:
            g = Path(d) / "gamedir"
            g.mkdir()
            rc, seen = self.run_main(g, Path(d) / "out", fail=False)
            self.assertIsNone(rc)
            self.assertEqual(len(seen["levers"]), 2)
            self.assertEqual(list(g.glob("tagpu_*")), [])

    def test_a_link_is_refused_and_what_was_written_is_listed(self):
        with tempfile.TemporaryDirectory() as d:
            g = Path(d)
            target = g / "install_file"
            target.write_text("keep")
            (g / "tagpu_impaiov.on").symlink_to(target)
            written = []
            with self.assertRaises(l1_run.tr.CouldNotRun):
                l1_run.write_levers(g, ["impai_check.on", "impaiov.on=1"], written)
            self.assertEqual([p.name for p in written], ["tagpu_impai_check.on"])
            l1_run.remove_levers(written)
            self.assertFalse((g / "tagpu_impai_check.on").exists())
            self.assertEqual(target.read_text(), "keep")


def ov(seat, tick, known, ovd):
    return f"impai: ov seat={seat} tick={tick} known={known:016X} ov={ovd:016X} items=3"


class KnownByTick(unittest.TestCase):
    """(b): the two games' known digests agree on every tick both logged."""

    def test_same_knowledge_passes(self):
        live = [ov(1, 30 * k, k, 100 + k) for k in range(25)]
        fails, _ = impai_check.judge_ov(live, list(live))
        self.assertEqual(fails, [])

    def test_a_tick_known_differently_fails(self):
        live = [ov(1, 30 * k, k, 100 + k) for k in range(25)]
        puppet = list(live)
        puppet[7] = ov(1, 210, 99, 199)      # the same tick, other knowledge (and its own overlay)
        fails, _ = impai_check.judge_ov(live, puppet)
        self.assertTrue(any("tick 210" in f for f in fails), fails)

    def test_too_few_shared_ticks_fail(self):
        live = [ov(1, 30 * k, k, 100 + k) for k in range(25)]
        puppet = [ov(1, 30 * k + 1, k, 100 + k) for k in range(25)]
        fails, _ = impai_check.judge_ov(live, puppet)
        self.assertTrue(any("ticks logged by both" in f for f in fails), fails)


def level_end(cells, patches, w=8, h=8, th=3):
    """One level end's lines: its patch lines, then the dump of a w x h map whose metal is 9 at `cells`."""
    rows = [[0] * w for _ in range(h)]
    for x, z in cells:
        rows[z][x] = 9
    out = [f"impai: patch seat=1 ax={p[0]} az={p[1]} x0={p[2]} z0={p[3]} x1={p[4]} z1={p[5]} cells={p[6]}"
           for p in patches]
    out.append(f"impai: metal w={w} h={h} threshold={th}")
    for z, row in enumerate(rows):
        runs, x = [], 0
        while x < w:
            n = 1
            while x + n < w and row[x + n] == row[x]:
                n += 1
            runs.append(f"{row[x]:X}*{n}")
            x += n
        out.append(f"impai: metal z={z} x=0 " + " ".join(runs))
    return out


A = (2, 2, 2, 2, 3, 2, 2)       # the two cells (2, 2) and (3, 2)
B = (5, 5, 5, 5, 5, 6, 2)       # the two cells (5, 5) and (5, 6)


class PatchesByLevel(unittest.TestCase):
    """(e): each level end's patches are judged against that level end's own dump."""

    def test_each_level_its_own_map(self):
        lines = level_end([(2, 2), (3, 2)], [A]) + level_end([(5, 5), (5, 6)], [B])
        fails, note = impai_check.judge_patches(lines, True)
        self.assertEqual(fails, [], note)

    def test_a_patch_of_another_level_fails(self):
        # level 1 published A, which only level 1's map holds; the last dump alone would refuse it, and
        # the first dump alone would refuse B
        lines = level_end([(2, 2), (3, 2)], [A]) + level_end([(5, 5), (5, 6)], [A])
        fails, _ = impai_check.judge_patches(lines, True)
        self.assertEqual(len(fails), 1, fails)
        self.assertIn("level end 2", fails[0])

    def test_patches_with_no_dump_after_them_fail(self):
        lines = level_end([(2, 2), (3, 2)], [A]) + [level_end([], [B])[0]]
        fails, _ = impai_check.judge_patches(lines, True)
        self.assertTrue(any("no `impai: metal` dump after them" in f for f in fails), fails)

    def test_logs_read_in_part_order(self):
        with tempfile.TemporaryDirectory() as d:
            one, two = Path(d) / "tagpu.2.log", Path(d) / "tagpu.log"
            one.write_text("log: run abc part 1 of tagpu.log, started x\n")
            two.write_text("log: run abc part 2 of tagpu.log, started y\n")
            self.assertEqual(impai_check.log_order([two, one]), [one, two])
            plain = Path(d) / "plain.log"
            plain.write_text("no header\n")
            self.assertEqual(impai_check.log_order([two, plain, one]), [two, plain, one])


class RolesWater(unittest.TestCase):
    def test_engine_line_carries_water(self):
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "tagpu.log"
            line = ("impai: role {n} sub water value=1 dps_ground=0 dps_air=0 dps_water={w} "
                    "range_ground=0 flags=0x80")
            log.write_text("\n".join([line.format(n="None", w=0), line.format(n="ARMSUB", w="4.5"),
                                      "impai: threat types=2"]) + "\n")
            got = roles_check.engine_roles([log])
            self.assertEqual(got["ARMSUB"]["dps_water"], 4.5)


if __name__ == "__main__":
    unittest.main(verbosity=1)
