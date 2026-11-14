#!/usr/bin/env python3
"""Tests of ImpAI's offline tools: l1_run's levers removed however the run ends, its unit limit the
scenario's, its renderer GDI unless --gpu asks, and its end armed and its recording kept, impai_check's (b)
known digests compared tick by tick and (e) each level's patches judged against that level's own metal
dump, the logs read in part order, roles_check's engine lines read with their water damage, and
netai_check's judgement of a two-peer game's seat lines and records, netprobe's read of an instance store's
Difficulty, and level_lever's persona lever. No game runs.

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
import level_lever  # noqa: E402
import netai_check  # noqa: E402
import netroom_check  # noqa: E402
import netprobe  # noqa: E402
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


class L1RunRenderer(unittest.TestCase):
    """The game renders with GDI and takes no GPU token unless --gpu asks for the default renderer;
    run.txt names the one it played on."""

    def run_main(self, d, extra):
        worker = SimpleNamespace(gamedir=Path(d), name="tareplay-test", held=False, turbo=0, gdi=False,
                                 place=lambda fix, dll: None, close=lambda: None, stop=lambda hard: None)
        out = Path(d) / "out"
        argv = ["l1_run.py", str(HERE / "unused.json"), "--end", "30", "--out", str(out)] + extra
        with patch.object(sys, "argv", argv), patch.object(l1_run.runlock, "hold") as hold, \
             patch.object(l1_run.tr, "Worker", return_value=worker), \
             patch.object(l1_run.tr, "Log"), patch.object(l1_run.tr, "tacli", return_value=(1, "refused")), \
             patch.object(l1_run.talog, "run_text", return_value=""), patch("builtins.print"):
            self.assertEqual(l1_run.main(), 2)
        return hold.call_args.kwargs["gpu"], worker.gdi, (out / "run.txt").read_text()

    def test_gdi_by_default(self):
        with tempfile.TemporaryDirectory() as d:
            gpu, gdi, text = self.run_main(d, [])
            self.assertEqual((gpu, gdi), (False, True))
            self.assertIn("renderer gdi\n", text)

    def test_gpu_asks_for_the_default_renderer(self):
        with tempfile.TemporaryDirectory() as d:
            gpu, gdi, text = self.run_main(d, ["--gpu"])
            self.assertEqual((gpu, gdi), (True, False))
            self.assertIn("renderer vulkan\n", text)


class L1RunEnd(unittest.TestCase):
    """THE END: on the held tick the end test is armed before the end scenario, whatever the scenario
    set, and the recording is copied once the writer has closed it."""

    def test_the_end_test_is_armed_before_the_end_scenario_and_the_recording_kept(self):
        with tempfile.TemporaryDirectory() as d:
            g, out = Path(d) / "gamedir", Path(d) / "out"
            (g / "replays").mkdir(parents=True)
            (g / "replays" / "2026-11-14 game.imprec").write_bytes(b"rec")
            worker = SimpleNamespace(gamedir=g, name="tareplay-test", held=False, turbo=0, gdi=False,
                                     place=lambda fix, dll: None, close=lambda: None,
                                     stop=lambda hard: None, cap_failure=lambda: None)
            calls, waits = [], []
            log = SimpleNamespace(lines=["x"], read=lambda: None,
                                  wait_end=lambda rx, n: waits.append(rx))
            scenario = l1_run.TREE / "scenarios" / "impai" / "samson-line.json"
            argv = ["l1_run.py", str(scenario), "--end", "30", "--out", str(out)]
            with patch.object(sys, "argv", argv), patch.object(l1_run.runlock, "hold"), \
                 patch.object(l1_run.tr, "Worker", return_value=worker), \
                 patch.object(l1_run.tr, "Log", return_value=log), \
                 patch.object(l1_run.tr, "tacli", side_effect=lambda *a, **k: (calls.append(a), (0, ""))[1]), \
                 patch.object(l1_run.tr, "held_start"), patch.object(l1_run.tr, "run_to"), \
                 patch.object(l1_run.tr, "resume"), \
                 patch.object(l1_run.talog, "run_text", return_value=""), patch("builtins.print"):
                self.assertEqual(l1_run.main(), 0)
            applied = [Path(a[3]).name for a in calls if a[:2] == ("scenario", "apply")]
            self.assertEqual(applied, [l1_run.tr.REARM_SCENARIO, l1_run.tr.END_SCENARIO.name])
            self.assertEqual(waits[-2:], [r"recorder: ended after", r"recorder: writer: .* closed"])
            self.assertEqual((out / "samson-line.imprec").read_bytes(), b"rec")

    def test_no_recording_could_not_run(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "replays").mkdir()
            with self.assertRaises(l1_run.tr.CouldNotRun):
                l1_run.keep_recording(d, Path(d) / "x.imprec")


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


def checks(stale="stale=0", passes="impai: check pass classes=15 evaluated=2764800 mismatch=0"):
    """The (a) and (c) lines of a game that passes, the radar line's stale count and the content check's line as
    given."""
    return ["impai: work seat=1 ticks=900 refused=0 slots_max=1/2 visits_max=1/2 cells_max=1/2 flood_max=0/2 "
            "scans=3 clusters=0 gametime=900",
            "impai: cost seat=1 ticks=900 max_us=1 mean_us=1 p99_us=1",
            f"impai: check los seat=1 evaluated={impai_check.LOS_MIN} mismatch=0",
            f"impai: check radar passes={impai_check.RADAR_PASSES} evaluated=99 mismatch=0 {stale}".rstrip(),
            *([passes] if passes else [])]


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


class ContentPass(unittest.TestCase):
    """(a): the content map's check at the level init is there, evaluated entries and found no mismatch."""

    def test_agreeing_passes(self):
        fails, _ = impai_check.judge(checks(), "true")
        self.assertEqual(fails, [])

    def test_mismatch_fails(self):
        fails, _ = impai_check.judge(checks(passes="impai: check pass classes=15 evaluated=99 mismatch=3"), "true")
        self.assertTrue(any("mismatch=3" in f for f in fails), fails)

    def test_nothing_evaluated_fails(self):
        fails, _ = impai_check.judge(checks(passes="impai: check pass classes=0 evaluated=0 mismatch=0"), "true")
        self.assertTrue(any("evaluated=0" in f for f in fails), fails)

    def test_missing_fails(self):
        fails, _ = impai_check.judge(checks(passes=None), "true")
        self.assertTrue(any("no `impai: check pass`" in f for f in fails), fails)


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


class RolesLine(unittest.TestCase):
    def test_engine_line_carries_water_and_tech(self):
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "tagpu.log"
            line = ("impai: role {n} sub water value=1 dps_ground=0 dps_air=0 dps_water={w} "
                    "range_ground=0 flags=0x80 tech=2")
            log.write_text("\n".join([line.format(n="None", w=0), line.format(n="ARMSUB", w="4.5"),
                                      "impai: threat types=2"]) + "\n")
            got = roles_check.engine_roles([log])
            self.assertEqual(got["ARMSUB"]["dps_water"], 4.5)
            self.assertEqual(got["ARMSUB"]["tech"], 2)


class NetaiJudge(unittest.TestCase):
    """netai_check.judge on the two-player game's records: the host's rows 0 (human) and 2 (its AI), the
    joiner's 1 (human) and 3 (its AI); the joiner holds the host's AI on its row 4 and the host the joiner's on
    its row 3. DirectPlay IDs: host 0xA, joiner 0xB, host AI 0xC, joiner AI 0xD."""

    HOST_RECS = {0: (True, 0xA, 1, True), 1: (True, 0xB, 3, False), 2: (True, 0xC, 2, True),
                 3: (True, 0xD, 3, False), **{i: (False, 0, 0, False) for i in range(4, 10)}}
    JOIN_RECS = {0: (True, 0xA, 3, False), 1: (True, 0xB, 1, True), 2: (False, 0, 0, False),
                 3: (True, 0xD, 2, True), 4: (True, 0xC, 3, False), **{i: (False, 0, 0, False) for i in range(5, 10)}}
    HOST_LOG = ("impai: seats 2 (netai=impai, a network game, level 2) of 4 records; other peers' AI seats 3, "
                "ImpAI's none\n"
                "impai: calls seat=2 n=5999 gametime=6000\n")
    JOIN_LOG = ("impai: seats none (netai=ota, a network game, level 2) of 4 records; other peers' AI seats 4, "
                "ImpAI's 4\n")

    def peers(self, host_log=None, join_log=None, host_recs=None, join_recs=None):
        return ({"log": self.HOST_LOG if host_log is None else host_log, "records": host_recs or self.HOST_RECS},
                {"log": self.JOIN_LOG if join_log is None else join_log, "records": join_recs or self.JOIN_RECS})

    def test_the_game_as_it_should_be(self):
        self.assertEqual(netai_check.judge(*self.peers()), [])

    def test_pulse_required_and_missing(self):
        bad = netai_check.judge(*self.peers(), pulse=True)
        self.assertTrue(any("pulse lines" in b for b in bad), bad)
        host_log = self.HOST_LOG + "impai: pulse seat=2 t=1800 built=3 mroots=0 eroots=4 army=0 squads= metal=1 energy=2\n"
        self.assertEqual(netai_check.judge(*self.peers(host_log=host_log), pulse=True), [])

    def test_joiner_marked_or_turned(self):
        join_log = self.JOIN_LOG.replace("seats none", "seats 3") + "impai: calls seat=3 n=10 gametime=6000\n"
        bad = netai_check.judge(*self.peers(join_log=join_log))
        self.assertTrue(any("join: marked [3]" in b for b in bad), bad)
        self.assertTrue(any("join: ImpAI's turns" in b for b in bad), bad)

    def test_host_marks_the_remote_seat(self):
        host_log = self.HOST_LOG.replace("seats 2 (", "seats 3 (").replace("calls seat=2", "calls seat=3")
        bad = netai_check.judge(*self.peers(host_log=host_log))
        self.assertTrue(any("marked record 3" in b for b in bad), bad)
        self.assertTrue(any("the joiner's AI seat" in b for b in bad), bad)

    def test_remote_seat_not_listed(self):
        join_log = self.JOIN_LOG.replace("AI seats 4", "AI seats none")
        bad = netai_check.judge(*self.peers(join_log=join_log))
        self.assertTrue(any("not among its other peers' AI seats" in b for b in bad), bad)

    def test_wrong_store_value_and_missing_line(self):
        bad = netai_check.judge(*self.peers(host_log=self.HOST_LOG.replace("netai=impai", "netai=ota")))
        self.assertTrue(any("netai=ota, not impai" in b for b in bad), bad)
        bad = netai_check.judge(*self.peers(join_log=""))
        self.assertEqual(bad, ["join: 0 network seat lines, not one"])

    def test_content_past_its_tables(self):
        host_log = ("impai: fight none: the engine's types, weapons or [DAMAGE] rows do not fit the tables "
                    "(480 types, 4096 weapon slots, 65536 rows)\n" + self.HOST_LOG.replace("seats 2", "seats none"))
        bad = netai_check.judge(*self.peers(host_log=host_log))
        self.assertTrue(any(b.startswith("host: ImpAI's content did not fit its tables: impai: fight none:")
                            for b in bad), bad)
        bad = netai_check.judge(*self.peers(join_log="impai: menus past their tables (side 1, builder 9: 2048 "
                                                     "menus, 9 types)\n" + self.JOIN_LOG))
        self.assertTrue(any(b.startswith("join: ImpAI's content did not fit its tables: impai: menus past")
                            for b in bad), bad)

    def test_names_across_peers(self):
        bad = netai_check.judge(*self.peers(join_log=self.JOIN_LOG.replace("ImpAI's 4", "ImpAI's none")))
        self.assertTrue(any("not record 4, the host's ImpAI seat" in b for b in bad), bad)
        bad = netai_check.judge(*self.peers(host_log=self.HOST_LOG.replace("ImpAI's none", "ImpAI's 3")))
        self.assertTrue(any("host: other peers' ImpAI seats [3]" in b for b in bad), bad)

    def test_joiner_level_from_its_difficulty(self):
        bad = netai_check.judge(*self.peers(join_log=self.JOIN_LOG.replace("level 2", "level 0")))
        self.assertEqual(bad, ["join: ImpAI's level 0 in a network game, not Hard"])

    def test_turns_for_another_seat(self):
        host_log = self.HOST_LOG + "impai: calls seat=0 n=3 gametime=6000\n"
        bad = netai_check.judge(*self.peers(host_log=host_log))
        self.assertTrue(any("not for its marked seat 2 alone" in b for b in bad), bad)


class NetroomJudge(unittest.TestCase):
    """netroom_check's reads of the room's labels and of the level's seat lines."""

    def test_ai_labels(self):
        snap = {"gadgets": [{"name": "PLAYER0", "text": "Alice"}, {"name": "PLAYER2", "text": "ImpAI:Alice"},
                            {"name": "PLAYER3", "text": "AI:Bob"}, {"name": "PLAYER4", "text": "Open"},
                            {"name": "READY0", "text": "AI:x"}]}
        self.assertEqual(netroom_check.ai_labels(snap), {2: "ImpAI:Alice", 3: "AI:Bob"})
        self.assertEqual([netroom_check.kind(t) for t in ("ImpAI:A", "AI:A", "Alice")], ["impai", "ota", "none"])

    LINE = ("impai: seats {m} (netai=impai, a network game, level 2) of 4 records; other peers' AI seats {r}, "
            "ImpAI's {i}\n")

    def test_level_as_chosen(self):
        host = self.LINE.format(m="none", r="3", i="3")
        join = self.LINE.format(m="3", r="2", i="none")
        self.assertEqual(netroom_check.judge_level(host, join), [])

    def test_level_not_as_chosen(self):
        host = self.LINE.format(m="2", r="3", i="none")
        join = self.LINE.format(m="none", r="2", i="2")
        bad = netroom_check.judge_level(host, join)
        self.assertEqual(len(bad), 4, bad)
        self.assertEqual(netroom_check.judge_level("", join), ["host: not exactly one network seat line"])


class NetprobeRegistry(unittest.TestCase):
    def test_store_difficulty(self):
        sys.path.insert(0, str(HERE.parent))
        import taremote
        with tempfile.TemporaryDirectory() as d:
            store = Path(d) / taremote.STATE_DIR / taremote.STORE
            store.parent.mkdir()
            reg = taremote.RegStore()
            reg.set_dword(taremote.STORE_TA, "gamespeed", 20)
            store.write_bytes(reg.format())
            self.assertIsNone(netprobe.registry_difficulty(Path(d)))
            reg.set_dword(taremote.STORE_TA, "Difficulty", 1)
            store.write_bytes(reg.format())
            self.assertEqual(netprobe.registry_difficulty(Path(d)), 1)


class LevelLeverPersona(unittest.TestCase):
    """level_lever's --persona writes the persona lever one line a seat, holds each item to impai_persona_ok's
    rules, and removes a link at the lever's name rather than writing through it."""

    EASY = "1=60,90,45,10,3,15,3,4,2"

    def test_written(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(level_lever.main([d, "1=easy", "--persona", self.EASY,
                                               "--persona", "2=15,30,0,0,0,0,0,0,0"]), 0)
            self.assertEqual((Path(d) / level_lever.LEVER).read_text(), "1=easy\n")
            self.assertEqual((Path(d) / level_lever.PERSONA_LEVER).read_text(),
                             f"{self.EASY}\n2=15,30,0,0,0,0,0,0,0\n")

    def test_rules(self):
        for item in ("1=50,90,0,0,0,0,0,0,0",       # a look not a multiple of 15
                     "1=165,90,0,0,0,0,0,0,0",      # a look past 150
                     "1=60,90,46,0,0,0,0,0,0",      # react past the shorter look less 15
                     "1=60,90,0,601,3,0,0,0,0",     # a rate past 600
                     "1=60,90,0,10,0,0,0,0,0",      # a rate with no burst
                     "1=60,90,0,0,3,0,0,0,0",       # a burst with no rate
                     "1=60,90,0,10,61,0,0,0,0",     # a burst past 60
                     "1=60,90,0,0,0,0,0,33,0",      # attention past the builders' table
                     "1=60,90,0,0,0,0,0,0,17",      # attention past the squad table
                     "1=60,90,0,0,0,0,0,0",         # eight integers
                     "12=60,90,0,0,0,0,0,0,0"):     # a seat of two digits
            with self.subTest(item=item), tempfile.TemporaryDirectory() as d:
                self.assertEqual(level_lever.main([d, "--persona", item]), 2)
                self.assertFalse((Path(d) / level_lever.PERSONA_LEVER).exists())
        for row in ("1=60,90,45,10,3,15,3,4,2", "1=30,45,15,24,4,36,6,8,3", "1=15,30,0,0,0,0,0,0,0"):
            self.assertIsNone(level_lever.persona_error(row), row)

    def test_link_removed(self):
        with tempfile.TemporaryDirectory() as d:
            target = Path(d) / "target"
            target.write_text("kept\n")
            (Path(d) / level_lever.PERSONA_LEVER).symlink_to(target)
            self.assertEqual(level_lever.main([d, "--persona", self.EASY]), 0)
            self.assertEqual(target.read_text(), "kept\n")
            self.assertFalse((Path(d) / level_lever.PERSONA_LEVER).is_symlink())


if __name__ == "__main__":
    unittest.main(verbosity=1)
