#!/usr/bin/env python3
"""Tests of ImpAI's offline tools: l1_run's levers removed however the run ends and its unit limit the
scenario's, impai_check's (b)
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


class L1RunUnitLimit(unittest.TestCase):
    """The limit is the scenario's own unless one is given: impai-cost.json's armies need its 1500."""

    def test_the_scenarios_limit(self):
        cost = HERE.parents[1] / "scenarios" / "impai-cost.json"
        self.assertEqual(l1_run.unit_limit(cost, None)[0], 1500)
        self.assertEqual(l1_run.unit_limit(cost, 800)[0], 800)

    def test_not_an_object_or_a_number(self):
        with tempfile.TemporaryDirectory() as d:
            sc = Path(d) / "s.json"
            for text in ('[1500]', '{"setup": [1500]}', '{"setup": {"unit_limit": true}}'):
                sc.write_text(text)
                self.assertEqual(l1_run.unit_limit(sc, None)[0], l1_run.tr.UNIT_LIMIT, text)

    def test_no_limit_in_the_scenario(self):
        with tempfile.TemporaryDirectory() as d:
            sc = Path(d) / "s.json"
            sc.write_text('{"setup": {"map": "x"}}')
            self.assertEqual(l1_run.unit_limit(sc, None)[0], l1_run.tr.UNIT_LIMIT)


def ov(seat, tick, known, ovd):
    return f"impai: ov seat={seat} tick={tick} known={known:016X} ov={ovd:016X} items=3"


def kn(seat, tick, known, ovd=None):
    tail = "none" if ovd == "none" else f"{(known + 7 if ovd is None else ovd):016X} items=3"
    return f"impai: known seat={seat} tick={tick} known={known:016X} ov={tail}"


def game(ov_offset=0, seats=(1, 2), n=25, drawn=None):
    """One game's (b) lines: each seat's knowledge and overlay digests at every 30th GameTime, seat s
    knowing 1000 s + k at GameTime 30 k, its overlay that plus 7; and the drawn overlay's lines of seat
    1 at `drawn` of those GameTimes (all by default), `ov_offset` ticks later."""
    lines = [kn(s, 30 * k, 1000 * s + k) for k in range(1, n + 1) for s in seats]
    lines += [ov(1, 30 * k + ov_offset, 1000 + k, 1007 + k) for k in (drawn or range(1, n + 1))]
    return lines


class KnownByTick(unittest.TestCase):
    """(b): the two games' knowledge and overlay digests agree on every GameTime both logged, whatever
    ticks and however many the drawn overlay's rebuilds fell on."""

    def test_same_knowledge_passes(self):
        fails, _ = impai_check.judge_ov(game(), game())
        self.assertEqual(fails, [])

    def test_overlay_rebuilds_at_other_ticks_pass(self):
        fails, _ = impai_check.judge_ov(game(ov_offset=27), game(ov_offset=4))
        self.assertEqual(fails, [])

    def test_few_drawn_overlays_pass(self):
        fails, _ = impai_check.judge_ov(game(drawn=[3, 9]), game(drawn=[]))
        self.assertEqual(fails, [])

    def test_a_tick_known_differently_fails(self):
        live, puppet = game(), game()
        puppet[puppet.index(kn(2, 210, 2007))] = kn(2, 210, 99)
        fails, _ = impai_check.judge_ov(live, puppet)
        self.assertTrue(any("GameTime 210 seat 2" in f for f in fails), fails)

    def test_an_overlay_drawn_differently_fails(self):
        live, puppet = game(), game()
        puppet[puppet.index(kn(2, 210, 2007))] = kn(2, 210, 2007, 5)
        fails, _ = impai_check.judge_ov(live, puppet)
        self.assertTrue(any("GameTime 210 seat 2" in f for f in fails), fails)
        self.assertTrue(any("known 00000000000007D7: overlay" in f for f in fails), fails)

    def test_a_drawn_overlay_off_its_gametime_fails(self):
        live = game()
        live[live.index(ov(1, 90, 1003, 1010))] = ov(1, 90, 1003, 4)
        fails, _ = impai_check.judge_ov(live, game())
        self.assertTrue(any("drawn overlay at tick 90" in f for f in fails), fails)

    def test_no_overlay_digest_fails(self):
        live = game()
        live[live.index(kn(1, 300, 1010))] = kn(1, 300, 1010, "none")
        fails, _ = impai_check.judge_ov(live, game())
        self.assertTrue(any("no overlay digest" in f for f in fails), fails)

    def test_a_seat_one_game_did_not_log_fails(self):
        fails, _ = impai_check.judge_ov(game(), game(seats=(1,)))
        self.assertTrue(any("seat 2" in f and "puppet none" in f for f in fails), fails)

    def test_too_few_shared_ticks_fail(self):
        puppet = [ln.replace(" tick=", " tick=1") if "known seat" in ln else ln for ln in game()]
        fails, _ = impai_check.judge_ov(game(), puppet)
        self.assertTrue(any("GameTimes logged by both games, under" in f for f in fails), fails)

    def test_a_gametime_one_game_skipped_fails(self):
        puppet = [ln for ln in game() if " tick=300 " not in ln or "known seat" not in ln]
        fails, _ = impai_check.judge_ov(game(), puppet)
        self.assertTrue(any("GameTime 300: logged by the live game only" in f for f in fails), fails)

    def test_a_shorter_puppet_passes(self):
        puppet = [ln for ln in game() if "known seat" not in ln or int(ln.split("tick=")[1].split()[0]) <= 600]
        fails, _ = impai_check.judge_ov(game(), puppet)
        self.assertEqual(fails, [])

    def test_overlay_ticks_are_not_knowledge(self):
        live = [ln for ln in game() if "impai: ov" in ln]
        fails, _ = impai_check.judge_ov(live, list(live))
        self.assertTrue(any(f.startswith("(b) 0 GameTimes") for f in fails), fails)


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


def checks(stale="stale=0"):
    """The (a) and (c) lines of a game that passes, the radar line's stale count as given."""
    return ["impai: work seat=1 ticks=900 refused=0 slots_max=1/2 visits_max=1/2 cells_max=1/2 flood_max=0/2 "
            "scans=3 clusters=0 gametime=900",
            "impai: cost seat=1 ticks=900 max_us=1 mean_us=1 p99_us=1",
            f"impai: check los seat=1 evaluated={impai_check.LOS_MIN} mismatch=0",
            f"impai: check radar passes={impai_check.RADAR_PASSES} evaluated=99 mismatch=0 {stale}".rstrip()]


class RadarStale(unittest.TestCase):
    """(a): the radar line's stale count is there and 0."""

    def test_none_stale_passes(self):
        fails, _ = impai_check.judge(checks(), "true")
        self.assertEqual(fails, [])

    def test_stale_bits_fail(self):
        fails, _ = impai_check.judge(checks("stale=2"), "true")
        self.assertTrue(any("2 radar bits" in f for f in fails), fails)

    def test_no_count_fails(self):
        fails, _ = impai_check.judge(checks(""), "true")
        self.assertTrue(any("no stale count" in f for f in fails), fails)


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
