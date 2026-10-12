#!/usr/bin/env python3
"""Tests of the replay harness's own logic (tafeat.py, tareplay.py's landing tier, tarec.py's
segments and world): the C parser against the coverage build's map, the features' regions, the
comment filter, `landing --check`, a recording's segments and chains, what format 14's block walk
refuses and which cells are images, read from files built from the format alone, the fixtures'
takeover entries and the W lane's edited header copies. No game runs. The files are read through
reccodec.py, which needs numpy, numba and zstandard: devpy.ensure() re-executes this under the
shared venv when they are missing.

    python3 tools/replay/test_tareplay.py
"""
import io
import json
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zlib
from unittest.mock import Mock, patch
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import tafeat  # noqa: E402
import reccache  # noqa: E402
import tarec  # noqa: E402
import tareplay  # noqa: E402
import tarewind  # noqa: E402

MAPS = [tafeat.DDRAW / "ddraw-cover.map", tafeat.DDRAW / "ddraw-stocklimits-cover.map"]


class KernelCompileAcrossThreads(unittest.TestCase):
    """tareplay decodes from several threads of one process: the first decodings, together, each
    find every kernel replaced once, none decorated twice."""

    def test_threads_compile_together(self):
        code = """
import sys, threading
sys.path.insert(0, sys.argv[1])
import reccodec
gate, errors = threading.Barrier(16), []
def go():
    gate.wait()
    try:
        reccodec._compile()
    except Exception as e:
        errors.append(repr(e))
threads = [threading.Thread(target=go) for _ in range(16)]
for t in threads: t.start()
for t in threads: t.join()
plain = [n for n, _ in reccodec._KERNELS if not hasattr(reccodec.__dict__[n], "py_func")]
print(errors, plain)
"""
        out = subprocess.run([sys.executable, "-c", code, str(Path(__file__).parent)],
                             capture_output=True, text=True, timeout=120)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertEqual(out.stdout.strip(), "[] []")


class NativeCosmeticImagePolicy(unittest.TestCase):
    RANGES = ((0, 0x1491B, 0x1AB8F), (0, 0x1AB9F, 0x1E88F),
              (1, 0x511DF0 - tarec.REC_DATA_BEGIN, 0x511F80 - tarec.REC_DATA_BEGIN))

    def gate(self, positions, accepted=False, name="stock-secondary-orders", cap=300):
        names = ("l1", "lT", "n1", "n2", "p1", "pT", "r1", "q1")
        images = {name: {0: bytearray(tarec.REC_MAIN_BYTES),
                         1: bytearray(tarec.REC_DATA_BYTES)} for name in names}
        for which, at in positions:
            images["l1"][which][at] = 1
        gen = Mock()
        gen.coverage.return_value = {}
        output = io.StringIO()
        with patch.object(tarec, "header_only", return_value={"projectiles": cap}), \
                patch.object(tarec, "dump_images", side_effect=lambda name: (
                    201 if name in ("lT", "pT") else 101, "digest", images[name])), \
                patch.object(tarec, "blocks", return_value=([(0, 0, 0, b"header")], None)), \
                patch.object(tarec, "header", return_value={"frame": 202}), \
                patch.object(tarec, "globals_list", return_value=(gen, [], [], "digest")), \
                redirect_stdout(output):
            result = tarec.cmd_images_gate(f"{name}.imprec", ("l1", "lT"), ("n1", "n2"),
                                          ("p1", "pT"), "r1", "q1",
                                          stock_secondary_cosmetics=accepted)
        return result, output.getvalue()

    def test_default_still_fails_inside_all_three_pools(self):
        positions = [(which, at) for which, lo, hi in self.RANGES for at in (lo, hi - 1)]
        result, output = self.gate(positions)
        self.assertEqual(result, 1)
        self.assertIn("6 bytes FAIL", output)

    def test_approved_pool_boundaries_are_reported_as_known_gap(self):
        positions = [(which, at) for which, lo, hi in self.RANGES for at in (lo, hi - 1)]
        result, output = self.gate(positions, accepted=True)
        self.assertEqual(result, 0)
        self.assertIn("6 native cosmetic-pool bytes", output)
        self.assertIn("known-gap", output)

    def test_adjacent_bytes_and_unit_state_remain_strict(self):
        for which, lo, hi in self.RANGES:
            for at in (lo - 1, hi):
                with self.subTest(which=which, at=at):
                    result, output = self.gate([(which, lo), (which, at)], accepted=True)
                    self.assertEqual(result, 1)
                    self.assertIn("1 bytes FAIL", output)
        self.assertEqual(self.gate([(0, 0x1B63)], accepted=True)[0], 1)

    def test_other_fixture_and_guarded_cap_are_refused(self):
        self.assertEqual(self.gate([], accepted=True, name="stock-resource-mex")[0], 2)
        self.assertEqual(self.gate([], accepted=True, cap=3000)[0], 2)

    def test_harness_only_selects_named_stock_control(self):
        for name, dll, accepted in (("stock-secondary-orders", "stock", True),
                                   ("stock-secondary-orders", "normal", False),
                                   ("stock-resource-mex", "stock", False),
                                   ("resource-upgrade", "normal", False)):
            with self.subTest(name=name, dll=dll):
                self.assertEqual(tareplay.image_gate_options({"name": name, "fix": {"dll": dll}}),
                                 ["--stock-secondary-cosmetics"] if accepted else [])

    def test_cli_refuses_exception_on_other_commands(self):
        with redirect_stderr(io.StringIO()):
            self.assertEqual(tarec.main(["tarec", "check", "missing.imprec",
                                         "--stock-secondary-cosmetics"]), 2)


class OpaqueGlobals(unittest.TestCase):
    """replay-globals.tsv's `opaque` value class: the parser's rules, and the address scan passing
    over an opaque slot of the globals image while it still fails a plain one."""
    ROW = "data\t0x51FC88\t4\t-\t{carried}\t{puppet}\t{name}\t-\tevidence"

    def setUp(self):
        self.gen = tarec._generator("replay-globals-gen")

    def parse(self, *rows):
        return self.gen.parse("\n".join(rows))

    def test_opaque_is_a_globals_rows_optional_tenth_column(self):
        gen = self.gen
        rows = self.parse(self.ROW.format(carried="globals", puppet="write", name="seed") + "\topaque")
        self.assertTrue(rows[0]["opaque"])
        self.assertTrue(gen.slots(rows)[0][0]["opaque"])
        self.assertFalse(self.parse(self.ROW.format(carried="globals", puppet="write",
                                                    name="seed"))[0]["opaque"])
        for bad in (self.ROW.format(carried="globals", puppet="write", name="seed") + "\trandom",
                    self.ROW.format(carried="leave", puppet="-", name="seed") + "\topaque"):
            with self.subTest(row=bad), self.assertRaises(gen.Invalid):
                self.parse(bad)
        lo = "data\t0x51FC88\t4\t0xFFFF\tglobals\twrite\tlo\t-\tevidence\topaque"
        hi = "data\t0x51FC88\t4\t0xFFFF0000\tglobals\twrite\thi\t-\tevidence"
        with self.assertRaises(gen.Invalid):
            gen.slots(self.parse(lo, hi))

    def test_the_scan_passes_over_an_opaque_slot_only(self):
        slots = [dict(dword=0, size=4, opaque=True, names="seed"),
                 dict(dword=1, size=4, opaque=False, names="plain")]
        world = Mock(tables={}, units={}, players=[], proj=b"", phead=None,
                     globals=struct.pack("<II", 0x1800, 0x1800))
        output = io.StringIO()
        with patch.object(tarec, "header_only", return_value={"globals": "digest"}), \
                patch.object(tarec, "dump_world", return_value=({}, world)), \
                patch.object(tarec, "dump_images", return_value=(301, "digest", None)), \
                patch.object(tarec, "scan_ranges", return_value=[(0x1000, 0x2000, "the main block", 1)]), \
                patch.object(tarec, "globals_list", return_value=(None, [], slots, "digest")), \
                redirect_stdout(output):
            result = tarec.cmd_scan("fixture.imprec", "l301")
        self.assertEqual(result, 1)
        self.assertIn("globals plain holds 0x00001800", output.getvalue())
        self.assertNotIn("seed", output.getvalue())
        self.assertIn("1 VALUES THAT ARE ADDRESSES", output.getvalue())


class CompletionCheckpoints(unittest.TestCase):
    def test_completed_moho_is_compared_for_every_commit_seed(self):
        for revision in range(256):
            times = tareplay.gametimes("resource-core-upgrade", 12000, str(revision), last_min=7800)
            self.assertEqual(times, sorted(set(times)))
            self.assertEqual(len(times), 4)
            self.assertTrue(all(t % 30 == 1 and 301 <= t <= 9600 for t in times))
            self.assertGreaterEqual(times[-1], 7800)

    def test_unreachable_completion_is_refused(self):
        with self.assertRaises(tareplay.CouldNotRun):
            tareplay.gametimes("too-short", 2400, "revision", last_min=7800)

    def test_ordinary_fixture_keeps_its_sampling(self):
        self.assertEqual(tareplay.gametimes("battle", 2400, "revision"),
                         tareplay.gametimes("battle", 2400, "revision", last_min=None))


class TurboLanes(unittest.TestCase):
    def test_the_rule(self):
        fx = tareplay.fixtures()
        lanes = {n: "".join(p for p in "LPRQKFWXU" if tareplay.turbo_lane(fx[n], p, 2))
                 for n in ("c3-veterancy", "secondary-orders", "battle", "tenseat",
                           "resource-geo-upgrade", "wpn-badtgt")}
        self.assertEqual(lanes, {"c3-veterancy": "LPRKFXU", "secondary-orders": "LPRKFXU",
                                 "battle": "LPRKFXU", "tenseat": "LPRKFXU",
                                 "resource-geo-upgrade": "LPRKFXU", "wpn-badtgt": "LPRKFXU"})
        self.assertEqual(tareplay.turbo_lane(fx["c3-veterancy"], "P", 0), 0)

    def test_the_held_lanes_are_the_turbo_lanes_whatever_k(self):
        fx = tareplay.fixtures()
        for k in (0, 1):
            self.assertEqual("".join(p for p in "LPRQKFWXU" if tareplay.held_lane(fx["battle"], p)),
                             "LPRKFXU")
            self.assertTrue(all(tareplay.turbo_lane(fx["battle"], p, k) in (0, k)
                                for p in "LPRQKFWXU"))

    def test_a_cost_game_is_held_and_keeps_its_own_pacing(self):
        cost = {"load": "replay-four-ai.json", "cost": True, "end": 18000}
        self.assertTrue(tareplay.held_lane(cost, "L"))
        self.assertEqual(tareplay.turbo_lane(cost, "L", 1), 0)


class RunTo(unittest.TestCase):
    """run_to on a held level: a step whose GameTime a script ran past could not run, named; a
    level not held is refused before anything is armed."""

    def log(self, at, held=True):
        log = Mock()
        log.held, log.at, log.pending = held, at, None
        return log

    def test_a_passed_step_is_named(self):
        with patch.object(tareplay, "hold_at") as hold_at:
            with self.assertRaisesRegex(tareplay.CouldNotRun, r"`keys` at GameTime 120 had "
                                                              r"passed: the level is held at 300"):
                tareplay.run_to(Mock(), self.log(300), 900, [(120, "keys", ["a"])])
            hold_at.assert_not_called()

    def test_an_unheld_level_is_refused(self):
        with self.assertRaisesRegex(tareplay.CouldNotRun, "not held"):
            tareplay.run_to(Mock(), self.log(1, held=False), 900, [])

    def test_steps_at_the_held_tick_run_without_moving_it(self):
        ran = []
        with patch.object(tareplay, "step", side_effect=lambda w, kind, arg: ran.append(arg)), \
             patch.object(tareplay, "hold_at") as hold_at:
            self.assertEqual(tareplay.run_to(Mock(), self.log(1), 1, [(1, "apply", "a.json"),
                                                                      (1, "keys", ["b"])]), 1)
        self.assertEqual(ran, ["a.json", ["b"]])
        hold_at.assert_not_called()


class FailedWorkerCleanup(unittest.TestCase):
    def test_failed_game_is_stopped_even_if_artifact_collection_fails(self):
        run = tareplay.Run.__new__(tareplay.Run)
        run.game_speed = tareplay.TEST_GAME_SPEED
        run.turbo = tareplay.TURBO
        run.fx = {"naval": {"fix": {}, "Ts": [1], "out": Path("unused"), "rec": Path("rec"),
                            "procs": {}, "problems": []}}
        worker = Mock(name="worker")
        worker.name = "owned"
        worker.collect.side_effect = OSError("artifact write failed")
        with patch.object(tareplay, "watch_lane", side_effect=tareplay.Failed("COB STOP")), \
             patch.object(tareplay, "say"):
            self.assertIsNot(run.job(worker, "naval", "W"), False)
        worker.stop.assert_called_once_with(hard=True)
        self.assertEqual(run.fx["naval"]["problems"][0], ("W", "FAILED", "COB STOP"))

    def test_worker_is_retired_if_failed_game_cannot_be_stopped(self):
        run = tareplay.Run.__new__(tareplay.Run)
        run.game_speed = tareplay.TEST_GAME_SPEED
        run.turbo = tareplay.TURBO
        run.fx = {"naval": {"fix": {}, "Ts": [1], "out": Path("unused"), "rec": Path("rec"),
                            "procs": {}, "problems": []}}
        worker = Mock(name="worker")
        worker.name = "owned"
        worker.stop.side_effect = tareplay.CouldNotRun("still running")
        with patch.object(tareplay, "watch_lane", side_effect=tareplay.Failed("COB STOP")), \
             patch.object(tareplay, "say"):
            self.assertIs(run.job(worker, "naval", "W"), False)
        self.assertIn("cleanup", run.fx["naval"]["problems"][-1][2])


class OfflineOrder(unittest.TestCase):
    """tareplay.Ordered: checks handed to the offline pool read back in the order asked, and the
    first failure in that order is the one raised, before the caller's own later one."""

    @staticmethod
    def slow(value, s=0.2):
        time.sleep(s)
        return [value]

    @staticmethod
    def fail(why, s=0.0):
        time.sleep(s)
        raise tareplay.Failed(why)

    def test_results_keep_the_order_asked(self):
        r = tareplay.Ordered()
        r.add("a")
        r.later(self.slow, "b")
        r.add("c", "d")
        r.later(self.slow, "e", 0)
        self.assertEqual(r.results(), ["a", "b", "c", "d", "e"])
        self.assertEqual(r.results(), ["a", "b", "c", "d", "e"])

    def test_the_first_failure_in_order_is_raised(self):
        r = tareplay.Ordered()
        r.add("a")
        r.later(self.fail, "first", 0.2)
        r.later(self.fail, "second")
        r.add("b")
        for _ in range(2):
            with self.assertRaisesRegex(tareplay.Failed, "first"):
                r.results()
        self.assertEqual(r.settled(), ["a"])

    def test_an_earlier_failure_wins_over_the_callers_own(self):
        with self.assertRaisesRegex(tareplay.Failed, "the check"):
            with tareplay.Ordered() as r:
                r.later(self.fail, "the check", 0.2)
                raise tareplay.CouldNotRun("the next game")
        with self.assertRaisesRegex(tareplay.CouldNotRun, "the next game"):
            with tareplay.Ordered() as r:
                r.later(self.slow, "passes")
                raise tareplay.CouldNotRun("the next game")

    def test_then_runs_in_order_in_the_callers_thread(self):
        r, seen = tareplay.Ordered(), []
        got = r.start(self.slow, 7)
        r.add("a")
        r.then(lambda: seen.append(threading.current_thread()) or [got.result()[0] + 1])
        r.later(self.slow, "b", 0)
        self.assertEqual(r.results(), ["a", 8, "b"])
        self.assertEqual(seen, [threading.current_thread()])

    def test_the_pool_leaves_two_cores_to_each_game(self):
        with patch("os.cpu_count", return_value=32):
            self.assertEqual(tareplay.offline_workers(8), min(16, self.gib()))
            self.assertEqual(tareplay.offline_workers(16), 2)

    @staticmethod
    def gib():
        with open("/proc/meminfo") as f:
            return next(int(ln.split()[1]) for ln in f if ln.startswith("MemAvailable:")) >> 20


class Scheduling(unittest.TestCase):
    """Run's queue: the long lanes first, and a fixture's report started when its last lane
    ends, once."""

    def run_(self, *fixtures):
        run = tareplay.Run.__new__(tareplay.Run)
        run.q, run.seq, run.lock = tareplay.queue.PriorityQueue(), tareplay.itertools.count(), \
            threading.Lock()
        run.pending, run.left, run.reports, run.measure, run.keep = 0, {}, {}, False, False
        run.report_pool = tareplay.ThreadPoolExecutor(max_workers=2)
        run.fx = {name: {"name": name, "want": want, "fix": {"end": end}}
                  for name, want, end in fixtures}
        return run

    def drain(self, run):
        out = []
        while not run.q.empty():
            out.append(run.q.get()[2:])
        return out

    def test_long_lanes_first_then_l_then_short(self):
        run = self.run_(("a", "LPRQ", 900), ("b", "LPRQKFXU", 600), ("c", "LPRQW", 2000))
        self.assertEqual(tareplay.first_lanes(run.fx), ["b", "c", "a"])
        for name in tareplay.first_lanes(run.fx):
            run.put(name, "L")
        self.assertEqual(run.q.get()[2:], ("b", "L"))
        for p in "PRQKXU":
            run.put("b", p)
        self.assertEqual(self.drain(run), [("b", "X"), ("b", "U"), ("b", "K"), ("c", "L"),
                                           ("a", "L"), ("b", "R"), ("b", "P"), ("b", "Q")])

    def test_the_report_starts_when_the_last_lane_ends(self):
        run = self.run_(("a", "LPKF", 900))
        with patch.object(tareplay, "fixture_report", return_value=(["== a"], False, False)) as rep:
            run.put("a", "L")
            run.put("a", "P")               # L queues its followers before it ends
            run.done("a")
            run.put("a", "F")               # so does K
            run.done("a")
            self.assertFalse(run.reports)
            run.done("a")
            self.assertEqual(run.report_of("a").result(), (["== a"], False, False))
            self.assertEqual(run.report_of("a"), run.reports["a"])
        rep.assert_called_once_with(run.fx["a"], False)

    def test_a_stopped_fixture_is_reported_by_report(self):
        run = self.run_(("a", "LPRQ", 900))
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        run.out = Path(tmp.name)
        run.put("a", "L")                   # never taken: the run stopped
        with patch.object(tareplay, "fixture_report", return_value=(["== a"], False, True)), \
                redirect_stdout(io.StringIO()):
            self.assertEqual(tareplay.report(run), 2)
        self.assertEqual((run.out / "summary.txt").read_text(), "== a\n")


class RewindPolicy(unittest.TestCase):
    def compare(self, key, rewind, puppet=False):
        meta = (0, (1 << len(tarec.COUNT_SINKS)) - 1, 1)
        samples = [(10, {}, meta, {}), (20, {} if puppet else {key: 100}, meta, {}),
                   (10, {}, meta, {}), (20, {key: 100} if puppet else {}, meta, {})]
        out = io.StringIO()
        with patch.object(tarec, "header_only", return_value={}), \
             patch.object(tarec, "counts_of", side_effect=samples), \
             patch.object(tarec, "absent_rows", return_value=((5, 2), [])), \
             redirect_stdout(out):
            result = tarec.cmd_compare_counts("rec", "a", "b", "c", "d", "rows", rewind)
        return result, out.getvalue()

    def test_only_rewind_vm_children_are_accepted(self):
        for caller in (0x4B18C0, 0x4B192D):
            key = (0x4B08C0, caller, 123)
            self.assertEqual(self.compare(key, False)[0], 1)
            result, report = self.compare(key, True)
            self.assertEqual(result, 0)
            self.assertIn("accepted rewind COB child", report)
        for key in ((0x4B08C0, 0x80001000, 123), (0x4B08C0, 0x123456, 123),
                    (0x420A30, 0x4B18C0, 0), (0x4861B3, 0x4B18C0, 0)):
            self.assertEqual(self.compare(key, True)[0], 1)
        self.assertEqual(self.compare((0x480B57, 0, 0), True, puppet=True)[0], 1)

    def test_seated_sight_remains_exact(self):
        from types import SimpleNamespace
        def world():
            return SimpleNamespace(lists={}, players=[bytearray(tarec.REC_PLAYER_IMAGE)
                for _ in range(tarec.REC_PLAYERS)], sight={0: bytearray(b"a"), 4: bytearray(b"b")},
                ctl=[], view=[0, 0], units={})
        a, b = world(), world()
        b.sight[0] = bytearray(b"c")
        b.sight.pop(4)
        h = {"players": [{"index": 0, "controller": 1}]}
        tarec.fork_view(a, b, h, h)
        self.assertNotIn(4, a.sight)
        self.assertNotIn(4, b.sight)
        self.assertNotEqual(a.sight[0], b.sight[0])


class CobFlarePolicy(unittest.TestCase):
    child = (0x472630, 0x420AE6, 0x4812D3)
    parent = (0x420A30, 0x4812D3, 0)

    def compare(self, values, child=None, parent_counted=True):
        pl, pp, cl, cp = values
        key = self.child if child is None else child
        sinks = (1 << len(tarec.COUNT_SINKS)) - 1
        if not parent_counted:
            sinks &= ~(1 << tarec.COUNT_SINKS.index(self.parent[0]))
        meta = (0, sinks, 1)
        samples = [(10, {}, meta, {}), (20, {self.parent: pl, key: cl}, meta, {}),
                   (10, {}, meta, {}), (20, {self.parent: pp, key: cp}, meta, {})]
        out = io.StringIO()
        with patch.object(tarec, "header_only", return_value={}), \
             patch.object(tarec, "counts_of", side_effect=samples), \
             patch.object(tarec, "absent_rows", return_value=((5, 2), [])), \
             redirect_stdout(out):
            result = tarec.cmd_compare_counts("rec", "a", "b", "c", "d", "rows")
        return result, out.getvalue()

    def test_accepted_parent_shortfall_bounds_child_and_reports_both(self):
        for values in ((201, 197, 33, 30), (197, 201, 30, 33)):
            result, report = self.compare(values)
            self.assertEqual(result, 0)
            self.assertIn("accepted bounded COB flare", report)
            self.assertIn(f"parent live {values[0]}, puppet {values[1]}", report)
            self.assertIn(f"flare live {values[2]}, puppet {values[3]}", report)

    def test_rejects_unexplained_or_invalid_shortfalls(self):
        for values in ((201, 197, 33, 28), (201, 201, 33, 30), (201, 197, 30, 33),
                       (201, 180, 33, 30), (20, 17, 13, 10), (10, 9, 14, 11),
                       (-1, 3, 0, 3), (201, 197, -1, -4)):
            with self.subTest(values=values):
                result, report = self.compare(values)
                self.assertEqual(result, 1)
                self.assertNotIn("accepted bounded COB flare", report)

    def test_only_named_parent_child_path_with_observed_parent(self):
        values = (201, 197, 33, 30)
        for key in ((0x472630, 0x420AE6, 0x4812D4), (0x472630, 0x420AE7, 0x4812D3),
                    (0x472530, 0x420AE6, 0x4812D3)):
            self.assertEqual(self.compare(values, child=key)[0], 1)
        result, report = self.compare(values, parent_counted=False)
        self.assertNotEqual(result, 0)
        self.assertNotIn("accepted bounded COB flare", report)


class Parser(unittest.TestCase):
    def test_blank_keeps_lines(self):
        text = 'a /* { */ "}" \'{\' // }\n#define X {\\\n }\nint f(void) { return 0; }\n'
        b = tafeat.blank(text)
        self.assertEqual(b.count("\n"), text.count("\n"))
        self.assertNotIn("{ */", b)
        self.assertEqual(b.count("{"), 1)

    def test_functions(self):
        text = ("static int t[] = { 1, 2 };\n"
                "struct s { int a; };\n"
                "static void a(int x)\n{\n    if (x) { x++; }\n}\n"
                "int __cdecl b(void* E) { return 0; }\n")
        self.assertEqual(tafeat.functions(text), [("a", 3, 6), ("b", 7, 7)])

    def test_blank_comments_keeps_code(self):
        text = '#define A 1 /* x */\n"s/*t" // c\nint x; /* y */\n'
        self.assertEqual(tafeat.blank_comments(text),
                         '#define A 1        \n"s/*t"     \nint x;        \n')

    def test_builds(self):
        text = ("a\n#ifndef TAGPU_LIMITS_STOCK\nb\n#else\nc\n#endif\n"
                "#if defined(TAGPU_LIMITS_STOCK)\nd\n#endif\n#ifdef OTHER\ne\n#endif\n")
        b = tafeat.builds(text)
        self.assertEqual([sorted(b[i]) for i in (1, 3, 5, 8, 11)],
                         [["raised", "stock"], ["raised"], ["stock"], ["stock"],
                          ["raised", "stock"]])

    def test_a_test_only_block_is_in_no_build(self):
        text = ("a\n#ifdef RECF_TEST_COPY\nb\n#ifndef TAGPU_LIMITS_STOCK\nc\n#endif\n#else\nd\n"
                "#endif\ne\n")
        b = tafeat.builds(text)
        self.assertEqual([sorted(b[i]) for i in (1, 3, 5, 8, 10)],
                         [["raised", "stock"], [], [], ["raised", "stock"], ["raised", "stock"]])

    @unittest.skipUnless(all(m.exists() for m in MAPS), "no coverage build (make COVER=1)")
    def test_the_maps_hold_every_dll_function_and_no_test_one(self):
        mapped = set()
        for m in MAPS:
            mapped |= set(tafeat.read_map(m))
        for rel in tareplay.covered_sources():
            text = (tafeat.DDRAW / rel).read_text()
            built = tafeat.builds(text)
            for name, first, _ in tafeat.functions(text):
                if built[first]:
                    self.assertIn((rel, name), mapped, f"{rel}: {name} is in no map")
                else:                   # a test program's function (tafeat.TEST_ONLY)
                    self.assertNotIn((rel, name), mapped, f"{rel}: {name} is in a DLL's map")


class Features(unittest.TestCase):
    def test_regions(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "x.c"
            p.write_text('#include "tagpu_simreg.h"\n'
                         'SIM_FEATURE(one, SIM_PUPPET_RUNS, "battle");\n'
                         'static int a;\n'
                         'SIM_FEATURE(two, SIM_PUPPET_OWNER, "battle naval-push");\n'
                         'void f(void) { }\n')
            feats, problems = tafeat.features_of(p, "src/x.c")
            self.assertEqual(problems, [])
            self.assertEqual([(f["name"], f["line"], f["end"]) for f in feats],
                             [("one", 2, 3), ("two", 4, 5)])
            self.assertEqual(tafeat.feature_at(feats, "src/x.c", 5)["name"], "two")
            self.assertIsNone(tafeat.feature_at(feats, "src/x.c", 1))

    def test_unfeatured_and_malformed(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "x.c"
            p.write_text('static int a;\nSIM_FEATURE(one, SIM_PUPPET_RUNS,\n "battle");\n'
                         'void f(void) { }\n')
            _, problems = tafeat.features_of(p, "src/x.c")
            kinds = sorted(q.split()[0] for q in problems)
            self.assertEqual(kinds, ["MALFORMED", "UNFEATURED"])


def sh(cwd, *a):
    subprocess.run(a, cwd=cwd, check=True, capture_output=True, text=True)


class HeaderCopy(unittest.TestCase):
    """W's refused copies: one header entry edited, every block still read whole."""

    @staticmethod
    def block(kind, payload, flags=tarec.RBF_STORED):
        head = struct.pack("<IHHIIIII", tarec.REC_BLOCK_MAGIC, kind, flags, 1, 9, len(payload),
                           len(payload), zlib.crc32(payload))
        return head + struct.pack("<I", zlib.crc32(head)) + payload

    @staticmethod
    def entry(tag, data):
        raw = struct.pack("<HH", tag, len(data)) + data
        return raw + b"\0" * (-len(raw) % 4)

    def test_edit(self):
        entry = self.entry
        layout, smap = b"ab" * 32 + b"\0", b"Coast To Coast\0"
        head = (entry(tarec.RT_FORMAT, struct.pack("<I", tarec.REC_FORMAT)) +
                entry(tarec.RT_LAYOUT, layout) + entry(tarec.RT_SETTINGS_MAP, smap))
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "a.imprec", Path(d) / "b.imprec"
            src.write_bytes(tarec.REC_MAGIC + self.block(tarec.RB_HEADER, head)
                            + self.block(tarec.RB_STREAM, b"\1\2\3\4"))
            tareplay.header_copy(src, dst, tarec.RT_SETTINGS_MAP, tareplay.no_such_map)
            got, why = tarec.blocks(dst)
            self.assertIsNone(why)
            self.assertEqual(got[1], (tarec.RB_STREAM, 1, 9, b"\1\2\3\4"))
            ents = dict(tarec.entries(got[0][3]))
            self.assertEqual(ents[tarec.RT_SETTINGS_MAP], b"Q" * 14 + b"\0")
            self.assertEqual(ents[tarec.RT_LAYOUT], layout)
            tareplay.header_copy(src, dst, tarec.RT_LAYOUT, tareplay.other_layout)
            ents = dict(tarec.entries(tarec.blocks(dst)[0][0][3]))
            self.assertEqual(ents[tarec.RT_LAYOUT], b"1" + layout[1:])

    def test_a_packed_header_is_refused(self):
        head = self.entry(tarec.RT_SETTINGS_MAP, b"Coast To Coast\0")
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "a.imprec"
            src.write_bytes(tarec.REC_MAGIC + self.block(tarec.RB_HEADER, head, 0))
            with self.assertRaisesRegex(tareplay.CouldNotRun, "not stored"):
                tareplay.header_copy(src, Path(d) / "b.imprec", tarec.RT_SETTINGS_MAP,
                                     tareplay.no_such_map)


class TakeoverRoster(unittest.TestCase):
    """A cached loader roster, one from before the judge's hold and a partial new dump cannot
    supply the takeover's order."""

    class Log:
        def __init__(self, lines):
            self.lines = lines

        def wait(self, rx, timeout, start=0):
            return next((line for line in self.lines[start:] if re.search(rx, line)), None)

    old = "  u001 armck own=0 idx=99 world=(1,2,3) screen=(4,5) nano=0.00\r"
    mark = "pause: paused at GameTime 1681\r"
    new = "  u001 armck own=0 idx=344 world=(4260,1170,85) screen=(4388,1159) nano=0.00\r"
    head = "units: alive=1 onscreen=0 eye=(0,0) me=0\r"

    def test_only_the_roster_after_the_acknowledgement_is_used(self):
        log = self.Log([self.old, self.head, self.mark, self.head, self.new, self.head])
        self.assertEqual(tareplay.takeover_roster(log, 1681, 0),
                         [{"type": "armck", "owner": 0, "engine_index": 344,
                           "world": [4260, 1170]}])

    def test_the_old_roster_does_not_complete_the_wait(self):
        with self.assertRaisesRegex(tareplay.CouldNotRun, "no roster after takeover"):
            tareplay.takeover_roster(self.Log([self.old, self.head, self.mark, self.head]),
                                     1681, 0)

    def test_a_partial_dump_does_not_complete_the_wait(self):
        with self.assertRaisesRegex(tareplay.CouldNotRun, "roster is incomplete"):
            tareplay.takeover_roster(self.Log([self.old, self.head, self.mark, self.new]),
                                     1681, 0)


class Landing(unittest.TestCase):
    """A throwaway repository: the comment filter and --check's three answers."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        sh(self.root, "git", "init", "-q", "-b", "main")
        for key in ("user.name", "user.email"):     # the enclosing repository's identity
            value = subprocess.run(["git", "-C", str(HERE), "config", key], check=True,
                                   capture_output=True, text=True).stdout.strip()
            sh(self.root, "git", "config", key, value)
        (self.root / "tools" / "replay").mkdir(parents=True)
        (self.root / "tools" / "replay" / "x.c").write_text("int f(void)\n{\n    return 1;\n}\n")
        (self.root / "README").write_text("r\n")
        sh(self.root, "git", "add", "-A")
        sh(self.root, "git", "commit", "-q", "-m", "base")
        sh(self.root, "git", "checkout", "-q", "-b", "work")
        self.saved = tareplay.TREE
        tareplay.TREE = self.root

    def tearDown(self):
        tareplay.TREE = self.saved
        self.tmp.cleanup()

    def commit(self, path, text):
        (self.root / path).write_text(text)
        sh(self.root, "git", "commit", "-qam", path)

    def test_comment_only_changes_nothing(self):
        self.commit("tools/replay/x.c", "int f(void)\n{\n    return 1; /* one */\n}\n")
        self.assertEqual(tareplay.changed_lines("tools/replay/x.c", "main"), set())
        self.commit("tools/replay/x.c", "#define K 2\nint f(void)\n{\n    return 1; /* one */\n}\n")
        self.assertEqual(tareplay.changed_lines("tools/replay/x.c", "main"), {1})
        self.commit("tools/replay/x.c", "int f(void)\n{\n    return 1; /* one */\n}\n")
        self.commit("tools/replay/x.c", "int f(void)\n{\n    return 2; /* one */\n}\n")
        self.assertEqual(tareplay.changed_lines("tools/replay/x.c", "main"), {3})

    def test_native_driver_changes_select_every_fixture(self):
        fx = {"one": {"load": "one.json"}, "two": {"load": "two.json"}}
        with patch.object(tareplay, "fixtures", return_value=fx), \
             patch.object(tareplay, "sim_paths", return_value=set()):
            selected, regions, functions, headers, why = tareplay.gated({"tools/tacli"}, "main")
        self.assertEqual(selected, set(fx))
        self.assertFalse(regions or functions or headers)
        self.assertTrue(why)

    def test_a_test_program_selects_every_fixture_and_no_function(self):
        fx = {"one": {"load": "one.json"}}
        with patch.object(tareplay, "fixtures", return_value=fx), \
             patch.object(tareplay, "sim_paths", return_value=set()):
            selected, regions, functions, headers, why = tareplay.gated(
                {"tools/replay/recmp_x_test.c", tareplay.DD + "src/tagpu_replay.c"}, "main")
        self.assertEqual(selected, set(fx))
        self.assertEqual(functions, {tareplay.DD + "src/tagpu_replay.c"})

    def test_check(self):
        self.commit("README", "r2\n")
        self.assertEqual(tareplay.landing_check("main"), 0)       # nothing gated
        self.commit("tools/replay/x.c", "int f(void)\n{\n    return 2;\n}\n")
        self.assertEqual(tareplay.landing_check("main"), 1)       # no note
        sh(self.root, "git", "notes", "append", "-m",
           "replay-roundtrip: dll=x range=main..y features=- fixtures=- seeds=- reached=1/1 "
           "result=pass date=2026-10-01", "HEAD")
        self.assertEqual(tareplay.landing_check("main"), 0)
        self.commit("README", "r3\n")
        self.assertEqual(tareplay.landing_check("main"), 0)       # nothing gated since
        self.commit("tools/replay/x.c", "int f(void)\n{\n    return 3;\n}\n")
        self.assertEqual(tareplay.landing_check("main"), 1)       # gated since the note


# ---- a recording's segments, from the format alone (tagpu_recfmt.h) -----------------------------

def tlv(tag, data):
    """A header or end entry, or a record: u16 tag or kind, u16 length, the bytes, to a dword."""
    return struct.pack("<HH", tag, len(data)) + data + bytes(-len(data) % 4)


def block(kind, first, last, payload):
    """A stored block: its header with both CRCs, then the payload."""
    head = struct.pack("<IHHIIIII", tarec.REC_BLOCK_MAGIC, kind, tarec.RBF_STORED, first, last,
                       len(payload), len(payload), zlib.crc32(payload))
    return head + struct.pack("<I", zlib.crc32(head)) + payload


def header(timeline=None, slots=4, seats=((0, 1), (1, 2)), viewed=0):
    """A header with what World and the chain's checks read: a 2 x 2 grid, so a 2-byte explored
    map, the seats and the options, and RT_TIMELINE for a fork."""
    p = tlv(tarec.RT_FORMAT, struct.pack("<I", tarec.REC_FORMAT))
    p += tlv(tarec.RT_MODE, struct.pack("<I", 2))
    p += tlv(tarec.RT_SLOTS, struct.pack("<I", slots))
    p += tlv(tarec.RT_FEATURES, struct.pack("<IIIII", 2, 2, 8, 0, 0))
    p += tlv(tarec.RT_PROJECTILES, struct.pack("<I", 4))
    if timeline:
        p += tlv(tarec.RT_TIMELINE, struct.pack("<III", *timeline))
    for index, controller in seats:
        p += tlv(tarec.RT_PLAYER, bytes([index, controller, 0, index]) + bytes(12) +
                 bytes(0xB9) + b"seat")
    p += tlv(tarec.RT_OPTIONS, struct.pack("<IIIIHBB", 0, 0, 1, 0, len(seats), viewed, viewed))
    return block(tarec.RB_HEADER, 0, 0, p)


def stream(first, last, unit=None, opens=True):
    """Iterations first..last; when it `opens` a segment, the first carries the explored map
    whole, as a segment's first iteration does, and with `unit` a unit image in that slot."""
    p = b""
    for t in range(first, last + 1):
        p += tlv(tarec.RK_TICK, struct.pack("<I", t))
        if t == first and opens:
            if unit:
                p += tlv(tarec.RK_UNIT, struct.pack("<HH", unit, 0) + bytes(tarec.REC_UNIT_IMAGE))
            p += tlv(tarec.RK_MAPPED, struct.pack("<I", 0) + bytes(2))
    return block(tarec.RB_STREAM, first, last, p)


def end_block(last, reason=1):
    return block(tarec.RB_END, last, last, tlv(tarec.RT_END_REASON, struct.pack("<I", reason)) +
                 tlv(tarec.RT_LAST, struct.pack("<I", last)))


def flagged(kind, first, last, payload, flags):
    """A block with these flags whose body is `payload` as it stands, packed or not."""
    head = struct.pack("<IHHIIIII", tarec.REC_BLOCK_MAGIC, kind, flags, first, last,
                       len(payload), len(payload), zlib.crc32(payload))
    return head + struct.pack("<I", zlib.crc32(head)) + payload


def sized(kind, payload, flags, raw, packed):
    """A block whose header claims `raw` and `packed` bytes, whatever its payload holds."""
    head = struct.pack("<IHHIIIII", tarec.REC_BLOCK_MAGIC, kind, flags, 1, 1, raw, packed,
                       zlib.crc32(payload))
    return head + struct.pack("<I", zlib.crc32(head)) + payload


def zstd_frame(data, wlog):
    """One zstd frame of `data` in a raw block, its header asking for a window of 2^wlog: no
    single segment, a 4-byte content size."""
    n = len(data)
    return (struct.pack("<IBBI", 0xFD2FB528, 0x80, (wlog - 10) << 3, n) +
            bytes([(1 | n << 3) & 0xFF, n >> 5 & 0xFF, n >> 13 & 0xFF]) + data)


class FormatBlocks(unittest.TestCase):
    """What format 14's block walk refuses by name before any payload is read, as
    tagpu_recreader.c's block_flags_bad and segs_scan refuse it (tagpu_recfmt.h, RecBlock)."""

    def refused(self, *parts, why, decodes=(True, False)):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "f.imprec"
            path.write_bytes(tarec.REC_MAGIC + b"".join(parts))
            for decode in decodes:
                with self.assertRaisesRegex(tarec.Unreadable, why):
                    tarec.blocks(path, decode=decode)

    def test_flags(self):
        snap = tlv(tarec.RK_TICK, struct.pack("<I", 1))
        S, P, R = tarec.RBF_STORED, tarec.RBF_SECTIONS, tarec.RBF_FRAME_RESET
        for kind, flags in ((tarec.RB_HEADER, 0), (tarec.RB_SNAPSHOT, S | P),
                            (tarec.RB_STREAM, S | R), (tarec.RB_STREAM, S | 16),
                            (tarec.RB_SNAPSHOT, S | tarec.RBF_REFRESH),
                            (tarec.RB_EXACT, S | tarec.RBF_REFRESH)):
            with self.subTest(kind=kind, flags=flags):
                self.refused(header(), flagged(kind, 1, 1, snap, flags), why="block flags")

    def test_a_packed_body_of_no_bytes(self):
        self.refused(header(), flagged(tarec.RB_SNAPSHOT, 1, 1, b"", 0), why="no bytes")

    def test_a_sectioned_block_without_the_codec_entries(self):
        # refused where it is decoded: a walk that skips the stream (decode=False), like the
        # DLL's segment scan, does not read it
        self.refused(header(), flagged(tarec.RB_STREAM, 1, 1, bytes(40), tarec.RBF_STORED |
                                       tarec.RBF_SECTIONS | tarec.RBF_FRAME_RESET),
                     why="without RT_CODEC and RT_PHYSICS", decodes=(True,))

    def test_a_segment_of_other_slots(self):
        self.refused(header(slots=4), end_block(0), header(timeline=(1, 0, 2), slots=6),
                     why="a segment of 6 slots after 4")

    def test_a_block_past_what_it_may_hold(self):
        # as tagpu_recreader.c's block_head refuses it: a header that passed its CRC is what the
        # writer wrote, not a tear, whether the file holds its bytes or not
        big = 64 << 20
        body = b"\0" * 16
        for raw, packed in ((big + 1, len(body)), (len(body), big + 1)):
            with self.subTest(raw=raw, packed=packed):
                self.refused(header(), sized(tarec.RB_SNAPSHOT, body, 0, raw, packed),
                             why="past what it may hold")

    def test_a_zstd_window_past_2_27(self):
        # as tagpu_reccodec.c's reccodec_unpack refuses it (codectest.c checks the C side)
        import reccodec
        snap = tlv(tarec.RK_TICK, struct.pack("<I", 1))
        self.assertEqual(reccodec.unpack(zstd_frame(snap, 27), len(snap)), snap)
        with self.assertRaisesRegex(tarec.Unreadable, "past 2\\^27"):
            reccodec.unpack(zstd_frame(snap, 28), len(snap))
        frame = zstd_frame(snap, 28)
        self.refused(header(), sized(tarec.RB_SNAPSHOT, frame, 0, len(snap), len(frame)),
                     why="past 2\\^27", decodes=(True,))


class CodecCheck(unittest.TestCase):
    """The gate's stream codec verdict from `tarec info`'s lines (tareplay.codec_check)."""

    @staticmethod
    def info(declined="0", mismatched="0", blocks=40, sectioned=38):
        return (f"  stream blocks: {blocks}, {sectioned} sectioned (3 restarting the frame), "
                f"{blocks - sectioned} records\n"
                f"  the writer's codec: {declined} blocks declined, {mismatched} mismatched\n")

    def test_verdicts(self):
        for info, verdict in ((self.info(), tareplay.PASS),
                              (self.info(declined="2"), tareplay.PASS),
                              (self.info(mismatched="1"), tareplay.FAIL),
                              (self.info(mismatched="absent"), tareplay.FAIL),
                              (self.info(sectioned=0), tareplay.FAIL),
                              (self.info(blocks=0, sectioned=0), tareplay.FAIL),
                              ("  ended: complete\n", tareplay.UNRUN)):
            with self.subTest(info=info):
                self.assertEqual(tareplay.codec_check(info)[1], verdict)

    def test_the_decline_lever_needs_a_declined_block(self):
        # a fixture under tagpu_rec.decline whose recording declined nothing never ran the fallback
        for info, verdict in ((self.info(declined="9"), tareplay.PASS),
                              (self.info(declined="0"), tareplay.FAIL),
                              (self.info(declined="absent"), tareplay.FAIL)):
            with self.subTest(info=info):
                self.assertEqual(tareplay.codec_check(info, decline=True)[1], verdict)
        self.assertIn("declined no block", tareplay.codec_check(self.info(), decline=True)[2])

    def test_a_codec_that_never_ran_is_named(self):
        # a writer without memory for the frame: every block records, the end block counts 0 and 0
        label, verdict, detail = tareplay.codec_check(self.info(sectioned=0))
        self.assertEqual(verdict, tareplay.FAIL)
        self.assertIn("never ran", detail)


class LedgerSum(unittest.TestCase):
    """The unit frame's ledger rule adds singles in integers (reccodec.f32_add, the C codec's
    reccodec_f32_add): the sum numpy's float32 gives, for every finite result."""

    def test_matches_float32(self):
        import numpy as np
        import reccodec
        rng = np.random.default_rng(12)
        a = rng.integers(0, 1 << 32, 20000, dtype=np.uint64).astype(np.uint32)
        b = rng.integers(0, 1 << 32, 20000, dtype=np.uint64).astype(np.uint32)
        # half the pairs near each other's exponent, a quarter near cancellation
        b[::2] = (a[::2] & 0xFF800000) ^ (b[::2] & 0x807FFFFF) ^ ((b[::2] % 24) << 23).astype(np.uint32)
        b[1::4] = (a[1::4] ^ 0x80000000) + (b[1::4] % 64) - 32
        with np.errstate(over="ignore", invalid="ignore"):
            want = (a.view(np.float32) + b.view(np.float32)).view(np.uint32)
        finite = lambda v: (v >> 23 & 0xFF) != 0xFF
        checked = 0
        for x, y, w in zip(a.tolist(), b.tolist(), want.tolist()):
            if finite(x) and finite(y) and finite(w):
                checked += 1
                self.assertEqual(reccodec.f32_add(x, y), w, f"{x:08x} + {y:08x}")
        self.assertGreater(checked, 15000)


class Segments(unittest.TestCase):
    """tarec's segments, chains and loads, as the DLL's reader takes them (tagpu_recreader.c)."""

    ORIGINAL = (header(), stream(1, 3, unit=1), stream(4, 6, opens=False), end_block(6))

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.n = 0

    def tearDown(self):
        self.tmp.cleanup()

    def file(self, *parts):
        self.n += 1
        p = Path(self.tmp.name) / f"r{self.n}.imprec"
        p.write_bytes(tarec.REC_MAGIC + b"".join(parts))
        return str(p)

    def segs(self, path):
        return tarec.file_segments(path)[0]

    def test_one(self):
        path = self.file(*self.ORIGINAL)
        self.assertEqual([(s["id"], s["first"], s["last"], s["end_reason"])
                          for s in self.segs(path)], [(0, 1, 6, 1)])
        h, bl, stop = tarec.load(path)
        self.assertEqual((len(bl), stop), (4, None))
        self.assertEqual(tarec.stream_to(h, bl).tick, 6)

    def test_two_and_three(self):
        fork1 = (header((1, 0, 4), seats=((0, 2), (1, 1)), viewed=1), stream(4, 5, unit=2),
                 end_block(5))
        fork2 = (header((2, 1, 5)), stream(5, 7, unit=3), end_block(7))
        path = self.file(*self.ORIGINAL, *fork1, *fork2)
        segs = self.segs(path)
        self.assertEqual([(s["id"], s["parent"], s["fork_at"], s["first"], s["last"])
                          for s in segs], [(0, 0, 0, 1, 6), (1, 0, 4, 4, 5), (2, 1, 5, 5, 7)])
        self.assertEqual(segs[1]["range"][0], segs[0]["range"][1])
        h, bl, _ = tarec.load(path)
        self.assertEqual(len(bl), 4)                    # timeline 0: the original alone
        self.assertEqual(tarec.stream_to(h, bl).tick, 6)
        self.assertEqual([s["id"] for s in tarec.chain(segs, 2)], [0, 1, 2])
        h, bl, _ = tarec.load(path, timeline=1)
        self.assertEqual([b[1] for b in bl if b[0] == tarec.RB_BOUNDARY], [4])
        w = tarec.stream_to(h, bl, want=3)              # the parent's, before the fork
        self.assertEqual((w.tick, sorted(w.units)), (3, [1]))
        w = tarec.stream_to(h, bl)                      # the child replaces the world whole
        self.assertEqual((w.tick, sorted(w.units), w.ctl[:2], w.view), (5, [2], [2, 1], [1, 1]))
        h, bl, _ = tarec.load(path, timeline=2)
        self.assertEqual([b[1] for b in bl if b[0] == tarec.RB_BOUNDARY], [4, 5])
        self.assertEqual(tarec.stream_to(h, bl, want=4).tick, 4)
        w = tarec.stream_to(h, bl)
        self.assertEqual((w.tick, sorted(w.units)), (7, [3]))
        with redirect_stdout(io.StringIO()):
            self.assertEqual(tarec.main(["tarec", "check", path, "--timeline", "2"]), 0)
            self.assertEqual(tarec.main(["tarec", "compare-fork", path, "1"]), 1)
            self.assertEqual(tarec.main(["tarec", "segments", path]), 0)
            self.assertEqual(tarec.main(["tarec", "info", path]), 0)

    def test_timeline_metadata_and_deleted_ancestor(self):
        fork1 = (header((1, 0, 4)), stream(4, 5, unit=2), end_block(5))
        fork2 = (header((2, 1, 5)), stream(5, 7, unit=3), end_block(7))
        rows = b"".join(struct.pack("<IB3xQ", tid, state, used)
                        for tid, state, used in ((0, 1, 10), (1, 2, 20), (2, 1, 30), (9, 2, 0)))
        path = self.file(*self.ORIGINAL, *fork1, *fork2,
                         block(tarec.RB_TIMELINES, 0, 0, rows))
        segs = self.segs(path)
        self.assertEqual([(s["state"], s["last_used"]) for s in segs], [(1, 10), (2, 20), (1, 30)])
        self.assertEqual(segs[0]["max_id"], 9)          # removed high ID is not reused
        with self.assertRaisesRegex(tarec.Unreadable, "deleted"):
            tarec.chain(segs, 1)
        self.assertEqual([s["id"] for s in tarec.chain(segs, 2)], [0, 1, 2])
        h, bl, _ = tarec.load(path, timeline=2)
        self.assertEqual(tarec.stream_to(h, bl).tick, 7)
        with redirect_stdout(io.StringIO()):
            self.assertEqual(tarec.cmd_info(path), 0)
        newer = struct.pack("<IB3xQ", 0, 1, 10) + struct.pack("<IB3xQ", 1, 1, 40)
        path = self.file(*self.ORIGINAL, *fork1, block(tarec.RB_TIMELINES, 0, 0, rows[:32]),
                         block(tarec.RB_TIMELINES, 0, 0, newer))
        self.assertEqual(self.segs(path)[1]["state"], 1)

    def test_bad_timeline_metadata(self):
        valid = struct.pack("<IB3xQ", 0, 1, 1)
        for rows in (b"", valid + b"x", valid * 2, struct.pack("<IB3xQ", 0, 3, 1),
                     struct.pack("<IB3xQ", 0xFFFFFFFF, 2, 0), valid[:5] + b"x" + valid[6:]):
            with self.assertRaises(tarec.Unreadable):
                self.segs(self.file(*self.ORIGINAL, block(tarec.RB_TIMELINES, 0, 0, rows)))
        with self.assertRaisesRegex(tarec.Unreadable, "no segment"):
            self.segs(self.file(*self.ORIGINAL, block(tarec.RB_TIMELINES, 0, 0,
                                                     struct.pack("<IB3xQ", 8, 1, 0))))

    def test_lru_protects_the_playing(self):
        parts = list(self.ORIGINAL)
        for tid in range(1, 4):
            parts += [header((tid, 0, 4)), stream(4, 5), end_block(5)]
        rows = b"".join(struct.pack("<IB3xQ", tid, 1, used)
                        for tid, used in ((0, 0), (1, 30), (2, 10), (3, 20)))
        segs = self.segs(self.file(*parts, block(tarec.RB_TIMELINES, 0, 0, rows)))
        self.assertEqual(tarec.fork_prediction(segs), 2)
        self.assertEqual(tarec.fork_prediction(segs, 2), 3)

    def test_prediction_refuses_child_boundary_and_capacity(self):
        segs = self.segs(self.file(*self.ORIGINAL, header((1, 0, 4)),
                                   stream(4, 6), end_block(6)))
        with self.assertRaisesRegex(tarec.Unreadable, "timeline 1's first iteration.* can is 5"):
            tarec.fork_prediction(segs, 1, 4)
        self.assertIsNone(tarec.fork_prediction(segs, 1, 5))
        self.assertIsNone(tarec.fork_prediction(segs, 0, 4))
        # the original's first iteration has none before it to append after
        with self.assertRaisesRegex(tarec.Unreadable, "timeline 0's first iteration.* can is 2"):
            tarec.fork_prediction(segs, 0, 1)
        self.assertIsNone(tarec.fork_prediction(segs, 0, 2))
        rows = struct.pack("<IB3xQ", 0, 1, 1) + struct.pack("<IB3xQ", 0xFFFFFFFE, 2, 0)
        segs = self.segs(self.file(*self.ORIGINAL, block(tarec.RB_TIMELINES, 0, 0, rows)))
        with self.assertRaisesRegex(tarec.Unreadable, "exhausted"):
            tarec.fork_prediction(segs, 0, 4)
        parts = list(self.ORIGINAL)
        for tid in range(1, 64):
            parts += [header((tid, 0, 4)), stream(4, 6), end_block(6)]
        segs = self.segs(self.file(*parts))
        with self.assertRaisesRegex(tarec.Unreadable, "64 timelines"):
            tarec.fork_prediction(segs, 0, 4)

    def test_torn_tail_and_open_segment(self):
        torn = block(tarec.RB_STREAM, 6, 6, b"x" * 8)[:20]
        path = self.file(*self.ORIGINAL, header((1, 0, 4)), stream(4, 5, unit=2), torn)
        self.assertEqual([(s["id"], s["last"], s["end_reason"]) for s in self.segs(path)],
                         [(0, 6, 1), (1, 5, None)])
        self.assertIn("cut short", tarec.load(path, timeline=1)[2])
        self.assertIsNone(tarec.load(path)[2])          # the original reads clean

    def test_end_no_header_follows(self):
        path = self.file(*self.ORIGINAL, stream(7, 8))
        self.assertEqual([s["id"] for s in self.segs(path)], [0])

    def test_refusals(self):
        def refused(*fork, n=1):
            path = self.file(*self.ORIGINAL, *fork)
            with self.assertRaises(tarec.Unreadable):
                tarec.chain(self.segs(path), n)
        refused(header((1, 0, 9)), stream(9, 10), end_block(10))      # past the parent's last
        refused(header((1, 0, 4)), stream(5, 6), end_block(6))        # first is not fork_at
        refused(header((1, 0, 4), slots=8), stream(4, 5), end_block(5))   # another shape
        refused(header((1, 0, 4)), stream(4, 5), end_block(5), n=3)   # no such timeline
        for parts in ((*self.ORIGINAL, header((1, 0, 4)), stream(4, 5), end_block(5),
                       header((1, 0, 4)), stream(4, 5), end_block(5)),     # an id twice
                      (*self.ORIGINAL, header((1, 2, 4)), stream(4, 5)),   # a later parent
                      (*self.ORIGINAL, header(), stream(4, 5)),        # a fork with no timeline
                      (header(), stream(1, 3), header((1, 0, 2)))):    # a header inside one
            path = self.file(*parts)
            with self.assertRaises(tarec.Unreadable):
                self.segs(path)


def hold_entry(refresh=30, flags=0, unit_held=(), node_held=()):
    """RT_HOLD: the refresh interval, the flags, a zero SHA-256 and the two masks, with each
    dword of `unit_held` and `node_held` held whole."""
    unit, node = bytearray(tarec.REC_UNIT_IMAGE), bytearray(tarec.REC_ORDER_IMAGE)
    for d in unit_held:
        unit[4 * d:4 * d + 4] = b"\xff" * 4
    for d in node_held:
        node[4 * d:4 * d + 4] = b"\xff" * 4
    return tlv(tarec.RT_HOLD, struct.pack("<II", refresh, flags) + bytes(32) + unit + node)


def held_header(**hold):
    """header(), RT_HOLD appended: its entries are read in any order."""
    raw = header()
    p = raw[32:] + hold_entry(**hold)
    return block(tarec.RB_HEADER, 0, 0, p)


def delta(slot, values):
    """An RK_UNIT_DELTA of `slot` naming {dword: value}."""
    bits = sum(1 << d for d in values)
    head = struct.pack("<H", slot) + bits.to_bytes(tarec.REC_UNIT_BITS, "little")
    head += bytes(tarec.REC_UNIT_DELTA_HEAD - len(head))
    return tlv(tarec.RK_UNIT_DELTA, head + b"".join(struct.pack("<I", values[d])
                                                      for d in sorted(values)))


class LossyStream(unittest.TestCase):
    """tarec's lossy-check, --exact and refresh_at against a file whose stream holds unit dword
    HELD between the refreshes at GameTime 2 and 31 (RT_HOLD's interval 30; 1 and 2 are the
    segment's first two iterations), and whose exact stream (RB_EXACT) does not."""

    HELD = 50

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.n = 0

    def tearDown(self):
        self.tmp.cleanup()

    def file(self, *parts):
        self.n += 1
        p = Path(self.tmp.name) / f"r{self.n}.imprec"
        p.write_bytes(tarec.REC_MAGIC + b"".join(parts))
        return str(p)

    @staticmethod
    def ticks(first, last, extra):
        """Iterations first..last, `extra` {GameTime: records} after each one's RK_TICK."""
        return b"".join(tlv(tarec.RK_TICK, struct.pack("<I", t)) + extra.get(t, b"")
                        for t in range(first, last + 1))

    def parts(self, lossy3=None, lossy31=None, refresh31=True, refresh2=True, exact=True,
              header=None, last=31, gone=None):
        """The file: iteration 1 makes unit 1, the exact stream changes its dword HELD and dword
        10 at 3; the lossy stream carries `lossy3` there (dword 10 alone by default) and `lossy31`
        at the refresh 31 (dword HELD's change by default). The second iteration ends a block of
        its own, a refresh block unless `refresh2` is false; the stream runs to `last`. With
        `gone`, both streams end unit 1 at that GameTime."""
        S, F = tarec.RBF_STORED, tarec.RBF_REFRESH
        first = (tlv(tarec.RK_UNIT, struct.pack("<HH", 1, 0) + bytes(tarec.REC_UNIT_IMAGE)) +
                 tlv(tarec.RK_MAPPED, struct.pack("<I", 0) + bytes(2)))
        x = {3: delta(1, {self.HELD: 7, 10: 9})}
        lossy = {3: delta(1, {10: 9}) if lossy3 is None else lossy3,
                 31: delta(1, {self.HELD: 7}) if lossy31 is None else lossy31}
        if gone is not None:
            x[gone] = lossy[gone] = tlv(tarec.RK_UNIT_GONE, struct.pack("<HH", 1, 0))
        x, lossy = self.ticks(3, last, x), self.ticks(3, last, lossy)
        out = [header or held_header(unit_held=(self.HELD,)),
               flagged(tarec.RB_STREAM, 1, 1, self.ticks(1, 1, {1: first}), S | F),
               flagged(tarec.RB_EXACT, 1, 1, self.ticks(1, 1, {1: first}), S),
               flagged(tarec.RB_STREAM, 2, 2, self.ticks(2, 2, {}), S | (F if refresh2 else 0)),
               flagged(tarec.RB_EXACT, 2, 2, self.ticks(2, 2, {}), S),
               flagged(tarec.RB_STREAM, 3, last, lossy, S | (F if refresh31 else 0))]
        if exact:
            out.append(flagged(tarec.RB_EXACT, 3, last, x, S))
        return out + [end_block(last)]

    def lossy_check(self, path, *args):
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            rc = tarec.main(["tarec", "lossy-check", path, *args])
        return rc, out.getvalue()

    def test_a_held_dword_written_at_the_refresh_passes(self):
        rc, o = self.lossy_check(self.file(*self.parts()))
        self.assertEqual(rc, 0, o)
        self.assertIn("31 iterations to GameTime 31, 3 refreshes", o)

    def test_an_iteration_whose_only_unit_record_ends_a_unit_passes(self):
        """a held table and an iteration that touches no unit live in both streams afterwards"""
        rc, o = self.lossy_check(self.file(*self.parts(last=5, gone=5, refresh31=False)))
        self.assertEqual(rc, 0, o)
        self.assertIn("5 iterations to GameTime 5", o)

    def test_to_stops_the_walk(self):
        rc, o = self.lossy_check(self.file(*self.parts()), "--to", "10")
        self.assertEqual(rc, 0, o)
        self.assertIn("10 iterations to GameTime 10", o)

    def test_a_shown_dword_held_fails(self):
        rc, o = self.lossy_check(self.file(*self.parts(lossy3=b"")))
        self.assertEqual(rc, 1, o)
        self.assertIn("GameTime 3: slot 1's shown bytes differ", o)

    def test_a_held_value_made_up_fails(self):
        rc, o = self.lossy_check(self.file(*self.parts(lossy3=delta(1, {10: 9, self.HELD: 3}))))
        self.assertEqual(rc, 1, o)
        self.assertIn("held bytes are neither the stream's last nor the exact ones", o)

    def test_a_refresh_that_misses_a_held_change_fails(self):
        rc, o = self.lossy_check(self.file(*self.parts(lossy31=b"")))
        self.assertEqual(rc, 1, o)
        self.assertIn("GameTime 31: the stream's world is not the exact copy at a refresh", o)

    def test_a_refresh_off_its_block_fails(self):
        rc, o = self.lossy_check(self.file(*self.parts(refresh31=False)))
        self.assertEqual(rc, 1, o)
        self.assertIn("GameTime 31: a refresh iteration that does not end a refresh block", o)

    def test_the_second_iteration_is_a_refresh(self):
        rc, o = self.lossy_check(self.file(*self.parts(refresh2=False)))
        self.assertEqual(rc, 1, o)
        self.assertIn("GameTime 2: a refresh iteration that does not end a refresh block", o)

    def test_the_grid(self):
        """the writer's rule (reclossy_is_refresh): the first two iterations, then 1 modulo the
        interval, or the first iteration past one a gap skips"""
        self.assertEqual([tarec.is_refresh(n, t, p, 30) for n, t, p in
                          ((1, 500, 0), (2, 501, 500), (3, 502, 501), (3, 511, 501),
                           (3, 512, 501), (5, 535, 511))], [True, True, False, True, True,
                                                             False])
        refresh, prev = [], 0
        for n, t in enumerate([1, 2, 3, 30, 31, 32, 60, 62, 91], 1):
            if tarec.is_refresh(n, t, prev, 30):
                refresh.append(t)
                prev = t
        self.assertEqual(refresh, [1, 2, 31, 62, 91])

    def test_no_exact_stream_cannot_be_checked(self):
        path = self.file(*self.parts(exact=False))
        rc, o = self.lossy_check(path)
        self.assertEqual(rc, 2, o)
        self.assertIn("recorded without tagpu_rec.exact", o)
        with self.assertRaisesRegex(tarec.Unreadable, "recorded without tagpu_rec.exact"):
            tarec.load(path, exact=True)

    def test_a_skirmish_without_rt_hold_fails(self):
        rc, o = self.lossy_check(self.file(*self.parts(header=header())))
        self.assertEqual(rc, 1, o)
        self.assertIn("a skirmish header without RT_HOLD", o)

    def test_exact_reads_the_exact_records(self):
        path = self.file(*self.parts())
        for exact, held in ((False, 0), (True, 7)):
            h, bl, _ = tarec.load(path, exact=exact)
            w = tarec.stream_to(h, bl, want=3)
            self.assertEqual(struct.unpack_from("<I", w.units[1], 4 * self.HELD)[0], held)
        h, bl, _ = tarec.load(path)
        w = tarec.stream_to(h, bl, want=31)
        self.assertEqual(struct.unpack_from("<I", w.units[1], 4 * self.HELD)[0], 7)

    def test_refresh_at(self):
        path = self.file(*self.parts())
        # a request inside the first interval rounds to the second iteration
        self.assertEqual([tarec.refresh_at(path, 0, T) for T in (1, 2, 3, 30, 31)],
                         [1, 2, 2, 2, 31])
        with self.assertRaisesRegex(tarec.Unreadable, "outside timeline 0's chain"):
            tarec.refresh_at(path, 0, 32)

    def test_rt_hold_bounds(self):
        for hold, why in (({"refresh": 20}, "refresh interval 20"),
                          ({"refresh": 301}, "refresh interval 301"),
                          ({"flags": 4}, "flags 0x4"),
                          ({"unit_held": (0x6C // 4,)}, "unit position")):
            with self.subTest(**hold):
                with self.assertRaisesRegex(tarec.Unreadable, why):
                    tarec.header(held_header(**hold)[32:])
        with self.assertRaisesRegex(tarec.Unreadable, "of 12 bytes"):
            tarec.header(header()[32:] + tlv(tarec.RT_HOLD, bytes(12)))


def spray(uid, x, y, z):
    """An RK_EV_SPRAY of unit ID `uid` at the point (x, y, z), its box and layer fixed."""
    return tlv(tarec.RK_EV_SPRAY, struct.pack("<IIiii", uid, 0, x, y, z) +
               struct.pack("<6i", *range(6)) + struct.pack("<I", 6))


def session(uid, mode):
    return tlv(tarec.RK_SPRAY_SESSION, struct.pack("<IBxxx", uid, mode))


def unit_at(slot, x, y, z):
    """An RK_UNIT of `slot` at the position (x, y, z), +0x6A"""
    img = bytearray(tarec.REC_UNIT_IMAGE)
    struct.pack_into("<iii", img, 0x6A, x, y, z)
    return tlv(tarec.RK_UNIT, struct.pack("<HH", slot, 0) + img)


class LossySprays(unittest.TestCase):
    """tarec's spray sessions (Sprays) against a file whose stream writes its nano sprays as
    sessions (RT_HOLD's RECL_SPRAYS) and whose exact stream (RB_EXACT) carries every spray: unit 1
    (ID 2) sprays the same point at GameTime 3 to 6, and its session, opened at 4, implies 5 and
    6 and closes at 7, where it makes none."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.n = 0

    def tearDown(self):
        self.tmp.cleanup()

    def file(self, exact, lossy, flags=tarec.RECL_SPRAYS):
        """Iterations 1 and 2 refresh blocks of their own, 3..31 one block whose last is a
        refresh; `exact` and `lossy` {GameTime: records} after each RK_TICK of 3..31."""
        S, F = tarec.RBF_STORED, tarec.RBF_REFRESH
        first = (unit_at(1, 0, 0, 0) +
                 tlv(tarec.RK_MAPPED, struct.pack("<I", 0) + bytes(2)))
        one = LossyStream.ticks(1, 1, {1: first})
        two = LossyStream.ticks(2, 2, {})
        parts = [held_header(flags=flags),
                 flagged(tarec.RB_STREAM, 1, 1, one, S | F), flagged(tarec.RB_EXACT, 1, 1, one, S),
                 flagged(tarec.RB_STREAM, 2, 2, two, S | F), flagged(tarec.RB_EXACT, 2, 2, two, S),
                 flagged(tarec.RB_STREAM, 3, 31, LossyStream.ticks(3, 31, lossy), S | F),
                 flagged(tarec.RB_EXACT, 3, 31, LossyStream.ticks(3, 31, exact), S),
                 end_block(31)]
        self.n += 1
        path = Path(self.tmp.name) / f"s{self.n}.imprec"
        path.write_bytes(tarec.REC_MAGIC + b"".join(parts))
        return str(path)

    A = spray(2, 100, 200, 300)
    EXACT = {3: A, 4: A, 5: A, 6: A}
    LOSSY = {3: A, 4: A + session(2, 1), 7: session(2, 0)}

    def lossy_check(self, path):
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            rc = tarec.main(["tarec", "lossy-check", path])
        return rc, out.getvalue()

    def test_implied_sprays_pass(self):
        rc, o = self.lossy_check(self.file(self.EXACT, self.LOSSY))
        self.assertEqual(rc, 0, o)
        self.assertIn("sprays 2 written and 2 implied", o)

    def test_the_world_carries_the_implied_sprays(self):
        h, bl, _ = tarec.load(self.file(self.EXACT, self.LOSSY))
        got = []
        tarec.stream_to(h, bl, on_record=lambda w, k, b: got.append(w.tick)
                        if k == tarec.RK_EV_SPRAY else None)
        self.assertEqual(got, [3, 4, 5, 6])

    def test_following_its_unit(self):
        exact = {3: spray(2, 100, 200, 300),
                 4: unit_at(1, 10, 0, -20) + spray(2, 110, 200, 280),
                 5: unit_at(1, 15, 1, -20) + spray(2, 115, 201, 280)}
        lossy = {3: exact[3], 4: exact[4] + session(2, 1 | tarec.REC_SPRAY_FOLLOW),
                 5: unit_at(1, 15, 1, -20), 6: session(2, 0)}
        rc, o = self.lossy_check(self.file(exact, lossy))
        self.assertEqual(rc, 0, o)
        self.assertIn("sprays 2 written and 1 implied", o)

    def test_a_spray_the_game_did_not_make_fails(self):
        exact = dict(self.EXACT)
        del exact[6]
        rc, o = self.lossy_check(self.file(exact, self.LOSSY))
        self.assertEqual(rc, 1, o)
        self.assertIn("GameTime 6: the nano sprays, 1 with the implied ones, are not the exact "
                      "stream's 0", o)

    def test_a_session_from_no_spray_fails(self):
        lossy = dict(self.LOSSY)
        lossy[3] = self.A + session(3, 1)
        rc, o = self.lossy_check(self.file(self.EXACT, lossy))
        self.assertEqual(rc, 1, o)
        self.assertIn("GameTime 3: the spray session of unit 3 at GameTime 3 predicts from no "
                      "single spray", o)

    def test_a_session_without_recl_sprays_fails(self):
        rc, o = self.lossy_check(self.file(self.EXACT, self.LOSSY, flags=0))
        self.assertEqual(rc, 1, o)
        self.assertIn("a spray session in a stream without RECL_SPRAYS", o)
        h, bl, _ = tarec.load(self.file(self.EXACT, self.LOSSY, flags=0))
        with self.assertRaisesRegex(tarec.Unreadable, "a spray session outside a stream"):
            tarec.stream_to(h, bl)

    AFTER = {**LOSSY, 4: A + session(2, 1) + spray(3, 1, 2, 3)}       # a record after the sessions
    UNORDERED = {**LOSSY, 4: A + session(3, 0) + session(2, 1)}       # sessions out of slot order

    def test_a_record_after_the_sessions_is_unreadable(self):
        h, bl, _ = tarec.load(self.file(self.EXACT, self.AFTER))
        with self.assertRaisesRegex(tarec.Unreadable, "record kind 28 after the iteration's spray "
                                                      "sessions"):
            tarec.stream_to(h, bl)

    def test_sessions_out_of_slot_order_are_unreadable(self):
        h, bl, _ = tarec.load(self.file(self.EXACT, self.UNORDERED))
        with self.assertRaisesRegex(tarec.Unreadable, "the spray session of unit 2 after unit 3's"):
            tarec.stream_to(h, bl)

    def test_a_malformed_session_is_unreadable(self):
        for mode in (3, tarec.REC_SPRAY_FOLLOW, 0x11):
            with self.subTest(mode=mode):
                lossy = dict(self.LOSSY)
                lossy[4] = self.A + session(2, mode)
                h, bl, _ = tarec.load(self.file(self.EXACT, lossy))
                with self.assertRaisesRegex(tarec.Unreadable, "a malformed spray session"):
                    tarec.stream_to(h, bl)


class SeekStart(unittest.TestCase):
    """tarec.seek_start: where the DLL's reader builds a seek from (recr_seek), from the file's
    index alone."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def file(self, *parts):
        p = Path(self.tmp.name) / "r.imprec"
        p.write_bytes(tarec.REC_MAGIC + b"".join(parts))
        return str(p)

    def test_the_last_snapshot_more_than_the_lead_before(self):
        path = self.file(header(), stream(1, 10, unit=1), block(tarec.RB_SNAPSHOT, 10, 10, b"s"),
                         stream(11, 20, opens=False), block(tarec.RB_SNAPSHOT, 20, 20, b"s"),
                         stream(21, 30, opens=False), end_block(30))
        self.assertEqual(tarec.seek_start(path, 0, 25, 3), (20, True))
        self.assertEqual(tarec.seek_start(path, 0, 23, 3), (10, True))     # 20 + 3 is not < 23
        self.assertEqual(tarec.seek_start(path, 0, 13, 3), (1, False))     # 10 + 3 is not < 13
        with self.assertRaises(tarec.Unreadable):
            tarec.seek_start(path, 0, 31, 3)

    def test_a_chain_takes_only_each_segments_part(self):
        fork = (header((1, 0, 15)), stream(15, 30, unit=2), block(tarec.RB_SNAPSHOT, 25, 25, b"s"),
                stream(26, 30, opens=False), end_block(30))
        path = self.file(header(), stream(1, 10, unit=1), block(tarec.RB_SNAPSHOT, 10, 10, b"s"),
                         stream(11, 20, opens=False), block(tarec.RB_SNAPSHOT, 20, 20, b"s"),
                         stream(21, 30, opens=False), end_block(30), *fork)
        # the parent's snapshot at 20 lies past the fork: timeline 1 never reads it
        self.assertEqual(tarec.seek_start(path, 1, 24, 3), (10, True))
        self.assertEqual(tarec.seek_start(path, 1, 29, 3), (25, True))
        self.assertEqual(tarec.seek_start(path, 0, 24, 3), (20, True))
        # no snapshot before: the first segment of the chain whose part reaches T - lead
        self.assertEqual(tarec.seek_start(path, 1, 13, 3), (1, False))
        path = self.file(header(), stream(1, 20, unit=1), end_block(20), *fork)
        self.assertEqual(tarec.seek_start(path, 1, 18, 2), (15, False))
        self.assertEqual(tarec.seek_start(path, 1, 16, 2), (1, False))


class SeekBlocks(unittest.TestCase):
    """tarec.seek_blocks: the blocks the DLL's reader decodes for a seek (recr_seek), from the
    file's blocks alone: a snapshot or a first iteration, every stream block holding an iteration
    read, a chain's crossings, and a base read first."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def file(self, *parts):
        p = Path(self.tmp.name) / "r.imprec"
        p.write_bytes(tarec.REC_MAGIC + b"".join(parts))
        return str(p)

    @staticmethod
    def of(*bs, base=0):
        n = sum(len(b) - 32 for b in bs)                # stored: raw and packed alike
        return len(bs), n, n, base

    def test_from_a_snapshot_or_the_first_iteration(self):
        s1, s2, s3 = stream(1, 10, unit=1), stream(11, 20, opens=False), stream(21, 30, opens=False)
        k10, k20 = block(tarec.RB_SNAPSHOT, 10, 10, b"s"), block(tarec.RB_SNAPSHOT, 20, 20, b"ss")
        path = self.file(header(), s1, k10, s2, k20, s3, end_block(30))
        self.assertEqual(tarec.seek_blocks(path, 0, 25, 3, 0), self.of(k20, s3))
        self.assertEqual(tarec.seek_blocks(path, 0, 25, 3, 1), self.of(s1, k20, s3, base=1))
        # a fresh playing holds the root's base
        self.assertEqual(tarec.seek_blocks(path, 0, 25, 3, None), self.of(k20, s3))
        self.assertEqual(tarec.seek_blocks(path, 0, 13, 3, 0), self.of(s1, s2))
        self.assertEqual(tarec.seek_blocks(path, 0, 10, 3, 0), self.of(s1))
        self.assertEqual(tarec.seek_blocks(path, 0, 20, 3, 0), self.of(k10, s2))

    def test_a_chain_reads_each_segments_part(self):
        s1, s2, s3 = stream(1, 10, unit=1), stream(11, 20, opens=False), stream(21, 30, opens=False)
        k10 = block(tarec.RB_SNAPSHOT, 10, 10, b"s")
        f1, k25, f2 = (stream(15, 24, unit=2), block(tarec.RB_SNAPSHOT, 24, 24, b"f"),
                       stream(25, 30, opens=False))
        path = self.file(header(), s1, k10, s2, block(tarec.RB_SNAPSHOT, 20, 20, b"s"), s3,
                         end_block(30), header((1, 0, 15)), f1, k25, f2, end_block(30))
        # from the parent's snapshot at 10 across the fork at 15; its 21..30 block is never read
        self.assertEqual(tarec.seek_blocks(path, 1, 22, 3, 0), self.of(k10, s2, f1))
        self.assertEqual(tarec.seek_blocks(path, 1, 29, 3, 0), self.of(k25, f2))
        self.assertEqual(tarec.seek_blocks(path, 1, 29, 3, 1), self.of(f1, k25, f2, base=1))
        # a fresh playing of timeline 1 holds the root's base, not the fork's
        self.assertEqual(tarec.seek_blocks(path, 1, 29, 3, None), self.of(f1, k25, f2, base=1))
        self.assertEqual(tarec.seek_blocks(path, 1, 22, 3, None), self.of(k10, s2, f1))
        with self.assertRaises(tarec.Unreadable):
            tarec.seek_blocks(path, 1, 31, 3, 0)


class SeekWork(unittest.TestCase):
    """The seek's work line (tagpu_replay.c, sk_work_log) parsed and judged: W1 against the file,
    W2 by its counts, W3 against the file's blocks."""

    LINE = ("replay: battle: the seek to 225's work: {I} iterations read to GameTime 225 from "
            "the snapshot at GameTime {S}; the blocks: {B} decoded ({Bb} for the base), {Bp} "
            "bytes read, {Bp} unpacked; the image: 10 units live, {M} made, {F} refused, 2 "
            "killed, {A} left alive; cpu {C} ms (user 1100, kernel 0) of the game thread, 1104 ms "
            "wall")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / "r.imprec"
        snap, rest = block(tarec.RB_SNAPSHOT, 10, 10, b"s"), stream(11, 300, opens=False)
        self.read = len(snap) + len(rest) - 64          # the payloads past their 32-byte headers
        self.path.write_bytes(tarec.REC_MAGIC + header() + stream(1, 10, unit=1) + snap + rest +
                              end_block(300))

    def tearDown(self):
        self.tmp.cleanup()

    def work(self, I=215, S=10, M=10, F=0, A=0, C=1100, B=2, Bb=0, Bp=None):
        line = self.LINE.format(I=I, S=S, M=M, F=F, A=A, C=C, B=B, Bb=Bb,
                                Bp=self.read if Bp is None else Bp)
        got = tareplay.work_lines("x\n" + line + "\r\n")
        self.assertEqual(len(got), 1)
        return got[0]

    def test_a_good_line(self):
        w = self.work()
        self.assertEqual((w["T"], w["R"], w["S"], w["kind"], w["B"], w["Bp"], w["C"], w["W"]),
                         (225, 225, 10, "the snapshot", 2, self.read, 1100, 1104))
        self.assertEqual(tareplay.work_verdict(w, str(self.path), 0, True), [])

    def test_each_rule(self):
        verdict = lambda w, **kw: tareplay.work_verdict(w, str(self.path), 0, **kw)
        self.assertRegex(verdict(self.work(S=1))[0], "W1: built from the snapshot at 1")
        self.assertRegex(verdict(self.work(I=217))[0], "W1: 217 iterations read from 10 to 225")
        self.assertRegex(verdict(self.work(M=9))[0], "W2: 9 made and 0 refused of 10")
        self.assertEqual(verdict(self.work(M=9, F=1)), [])
        self.assertRegex(verdict(self.work(M=9, F=1), takeover=True)[0], "W2: 1 creates refused")
        self.assertRegex(verdict(self.work(A=3))[0], "W2: 3 units left alive")
        self.assertEqual(verdict(self.work(C=40001)), [])          # time is reported, not judged
        self.assertRegex(verdict(self.work(B=3))[0], "W3: 3 blocks decoded")
        self.assertRegex(verdict(self.work(Bp=self.read + 1))[0], "where the file holds 2, ")
        self.assertRegex(verdict(self.work(Bb=1))[0], "where the file holds 3, ")
        # a fresh process holds the root's base: the line's base is checked, not trusted
        self.assertEqual(verdict(self.work(), fresh=True), [])
        self.assertRegex(verdict(self.work(Bb=1), fresh=True)[0],
                         "W3: 1 blocks for the base where a fresh process reads 0")

    def test_a_log_with_no_line_could_not_run(self):
        log = Path(self.tmp.name) / "tagpu.log"
        log.write_text("nothing\n")
        self.assertEqual(tareplay.work_check("R", log, str(self.path), 0, False)[0][1],
                         tareplay.UNRUN)
        log.write_text(self.LINE.format(I=215, S=10, M=10, F=0, A=0, C=5, B=2, Bb=0,
                                        Bp=self.read) + "\n")
        self.assertEqual(tareplay.work_check("R", log, str(self.path), 0, False)[0][1],
                         tareplay.PASS)


class DecodeCache(unittest.TestCase):
    """reccache.py under tarec.blocks(): a hit returns what the walk returned without decoding; a
    changed file byte or decoder version misses, and an entry that is cut short, corrupt or not
    its piece is ignored and written again. A file that appends a segment to a cached one decodes
    the segment alone, and returns and refuses what the uncached walk does."""

    TORN = block(tarec.RB_STREAM, 7, 7, b"x" * 8)[:20]
    ORIGINAL = header() + stream(1, 3, unit=1) + stream(4, 6, opens=False) + end_block(6)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.cache = self.dir / "cache"
        self.codec = tarec._codec()
        env = patch.dict("os.environ", {reccache.ENV: str(self.cache)})
        env.start()
        self.addCleanup(env.stop)
        self.addCleanup(self.tmp.cleanup)

    def file(self, name, unit=1, tail=TORN):
        p = self.dir / name
        p.write_bytes(tarec.REC_MAGIC + header() + stream(1, 3, unit=unit) +
                      stream(4, 6, opens=False) + end_block(6) + tail)
        return p

    def fork(self, name, *forks, original=ORIGINAL):
        """The original and a segment for each (timeline, unit) of `forks`, forked at 4."""
        p = self.dir / name
        p.write_bytes(tarec.REC_MAGIC + original + b"".join(
            header((tl, 0, 4)) + stream(4, 5, unit=unit) + end_block(5) for tl, unit in forks))
        return p

    def read(self, path):
        """(tarec.blocks' four lists and stop, the `start` of each decoding walk the codec ran:
        None for one from the file's first block)."""
        sizes, offsets, flags = [], [], []
        decoded = []
        real = self.codec.iter_blocks

        def spy(*a, **k):
            if k.get("decode", True):
                decoded.append(k.get("start"))
            return real(*a, **k)
        with patch.object(self.codec, "iter_blocks", side_effect=spy):
            out, stop = tarec.blocks(path, sizes, offsets, flags)
        return (out, sizes, offsets, flags, stop), decoded

    def uncached(self, path):
        with patch.dict("os.environ", {reccache.ENV: ""}):
            return self.read(path)[0]

    def entries(self):
        return sorted(self.cache.glob("*.dec"))

    def test_a_hit_is_what_the_walk_returned(self):
        path = self.file("r.imprec")
        cold, decoded = self.read(path)
        self.assertEqual(decoded, [None])
        self.assertIn("cut short", cold[-1])
        self.assertEqual(len(self.entries()), 1)
        self.assertEqual(self.read(path), (cold, []))
        self.assertEqual(self.uncached(path), cold)
        self.assertEqual(list(self.cache.iterdir()), self.entries())   # no temporary file left

    def test_a_changed_byte_misses(self):
        a, b = self.file("a.imprec", unit=1), self.file("b.imprec", unit=2)
        self.assertEqual(len(a.read_bytes()), len(b.read_bytes()))
        want_a, _ = self.read(a)
        got_b, decoded = self.read(b)
        self.assertEqual(decoded, [None])
        self.assertNotEqual(got_b, want_a)
        self.assertEqual(len(self.entries()), 2)
        self.assertEqual(self.read(a), (want_a, []))

    def test_a_changed_version_misses_and_rewrites(self):
        path = self.file("r.imprec")
        want, _ = self.read(path)
        entry, = self.entries()
        with patch.object(reccache, "_version", bytes(32)):
            self.assertEqual(self.read(path), (want, [None]))
            self.assertEqual(entry.read_bytes()[8:40], bytes(32))
            self.assertEqual(self.read(path), (want, []))
        self.assertEqual(self.read(path), (want, [None]))

    def test_a_corrupt_entry_is_ignored_and_rewritten(self):
        path = self.file("r.imprec")
        want, _ = self.read(path)
        entry, = self.entries()
        good = entry.read_bytes()
        mid = 72 + (len(good) - 72) // 2
        import zstandard        # here: the module's imports run before devpy.ensure() re-executes
        content = zstandard.ZstdDecompressor().stream_reader(good[72:]).read()
        n = 1 + reccache.BLOCK.size - 4                         # the first block's payload length

        def repacked(c):        # a sound zstd frame around content its own digest refuses
            return good[:72] + zstandard.ZstdCompressor(write_checksum=True).compress(c)
        k = content.index(tlv(tarec.RK_TICK, struct.pack("<I", 1))) + 4   # the first tick's value
        for why, bad in (("empty", b""), ("cut short", good[:mid]), ("its prefix", b"x" + good[1:]),
                         ("a packed byte", good[:mid] + bytes([good[mid] ^ 0x40]) + good[mid + 1:]),
                         ("its frame's tail", good[:-4] + bytes(4)),
                         ("a payload byte", repacked(content[:k] + b"\x02" + content[k + 1:])),
                         ("its digest", repacked(content[:-1] + bytes([content[-1] ^ 1]))),
                         ("a byte after its end", repacked(content + b"B")),
                         ("a length past its end",
                          repacked(content[:n] + b"\xff\xff\xff\x7f" + content[n + 4:]))):
            with self.subTest(why=why):
                entry.write_bytes(bad)
                self.assertEqual(self.read(path), (want, [None]))
                self.assertEqual(entry.read_bytes(), good)
                self.assertEqual(self.read(path), (want, []))

    def test_an_appended_segment_decodes_alone(self):
        original = self.file("a.imprec", tail=b"")
        self.read(original)
        size = original.stat().st_size
        one = self.fork("one.imprec", (1, 2))
        got, decoded = self.read(one)
        self.assertEqual(decoded, [(size, 4, 4)])           # four blocks before it; four slots
        self.assertEqual(got, self.uncached(one))
        self.assertEqual(len(self.entries()), 2)
        two = self.fork("two.imprec", (1, 2), (2, 3))
        got, decoded = self.read(two)
        self.assertEqual(decoded, [(one.stat().st_size, 7, 4)])
        self.assertEqual(got, self.uncached(two))
        self.assertEqual(self.read(two), (got, []))

    def test_a_piece_is_its_prefix_bytes(self):
        self.read(self.fork("one.imprec", (1, 2)))
        other = self.fork("other.imprec", (1, 3))           # another fork after the same original
        got, decoded = self.read(other)
        self.assertEqual(decoded, [(len(tarec.REC_MAGIC + self.ORIGINAL), 4, 4)])
        self.assertEqual(got, self.uncached(other))
        changed = self.fork("changed.imprec", (1, 2), original=header() + stream(1, 3, unit=3) +
                            stream(4, 6, opens=False) + end_block(6))
        got, decoded = self.read(changed)                   # the same fork after another original
        self.assertEqual(decoded, [None])
        self.assertEqual(got, self.uncached(changed))

    def test_an_entry_that_is_not_its_piece_is_ignored(self):
        path = self.fork("one.imprec", (1, 2))
        want, _ = self.read(path)
        bl, _ = reccache.collect(self.codec.iter_blocks(str(path)))
        for entry in self.entries():                        # each entry holding the other piece
            for blocks in (bl[:4], bl[4:], bl[:3]):
                with self.subTest(entry=entry.name, blocks=len(blocks)):
                    good = entry.read_bytes()
                    reccache._write(str(self.cache), str(entry), good[:72], blocks)
                    if entry.read_bytes() == good:
                        continue
                    got, decoded = self.read(path)
                    self.assertEqual(got, want)
                    self.assertTrue(decoded)
                    self.assertEqual(entry.read_bytes(), good)

    def test_a_refused_segment_is_refused_after_a_cached_one(self):
        self.read(self.file("a.imprec", tail=b""))
        path = self.dir / "slots.imprec"
        path.write_bytes(tarec.REC_MAGIC + self.ORIGINAL + header((1, 0, 4), slots=6) +
                         stream(4, 5) + end_block(5))
        for cache in (str(self.cache), ""):
            with patch.dict("os.environ", {reccache.ENV: cache}), \
                    self.assertRaisesRegex(tarec.Unreadable, "a segment of 6 slots after 4"):
                tarec.blocks(path)


def pov(player=0, cam=(100, 200), zoom=65536, flags=tarec.REC_POV_WORLD, every=3):
    """A RecPov of a 640 x 480 view with its cursor at the camera."""
    return tarec.REC_POV.pack(player, every, flags, 0, 640, 480, zoom, *cam, cam[0], 0, cam[1])


def selection(player, units):
    return tarec.REC_SEL.pack(player, tarec.REC_SEL_BEGIN | tarec.REC_SEL_END, len(units), 0,
                              len(units)) + b"".join(tarec.REC_SEL_ENTRY.pack(*u) for u in units)


def block_player(first_id=2, last_id=3):
    """Player 0's image with its unit block, IDs first_id..last_id (slots one less)."""
    img = bytearray(tarec.REC_PLAYER_IMAGE)
    struct.pack_into("<II", img, tarec.PL_BLOCKFIRST, first_id, 0)
    struct.pack_into("<I", img, tarec.PL_BLOCKLAST, last_id)
    return tlv(tarec.RK_PLAYER, struct.pack("<HH", 0, 0) + bytes(img))


class Perspective(unittest.TestCase):
    """tarec's perspective records and `pov --check`, on files built from the format alone."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def head(self, fmt=None):
        h = bytearray(header())
        # the RK_HEADER block's payload begins after its 32-byte head; RT_FORMAT is its first entry
        p = bytes(h[32:])
        p = p.replace(tlv(tarec.RT_FORMAT, struct.pack("<I", tarec.REC_FORMAT)),
                      tlv(tarec.RT_FORMAT, struct.pack("<I", fmt or tarec.REC_FORMAT)), 1)
        p += tlv(tarec.RT_UNITS_EACH, struct.pack("<I", 2))
        return block(tarec.RB_HEADER, 0, 0, p)

    def run_pov(self, *parts):
        path = Path(self.tmp.name) / "p.imprec"
        path.write_bytes(tarec.REC_MAGIC + b"".join(parts))
        out = io.StringIO()
        with redirect_stdout(out):
            code = tarec.cmd_pov(str(path), True)
        return code, out.getvalue()

    def good(self, second=None, snap_sel=((1, 0x11),), snap_pov=None):
        first = pov()
        second = second or pov(cam=(130, 200))
        s1 = (tlv(tarec.RK_TICK, struct.pack("<I", 1)) + block_player() +
              tlv(tarec.RK_MAPPED, struct.pack("<I", 0) + bytes(2)) +
              tlv(tarec.RK_POV, first) + tlv(tarec.RK_SELECT, selection(0, [(1, 0x11)])) +
              tlv(tarec.RK_CLICK, tarec.REC_CLICK.pack(0, 0, 1, 2, 2, 0, 4, 100, 0, 200)) +
              tlv(tarec.RK_TICK, struct.pack("<I", 2)) +
              tlv(tarec.RK_TICK, struct.pack("<I", 4)) + tlv(tarec.RK_POV, second))
        snap_pov = snap_pov or [(1, first), (4, second)]
        snap = (tlv(tarec.RK_TICK, struct.pack("<I", 4)) + block_player() +
                b"".join(tlv(tarec.RK_POV_AT, struct.pack("<I", t) + b) for t, b in snap_pov) +
                tlv(tarec.RK_SELECT, selection(0, list(snap_sel))) +
                tlv(tarec.RK_MAPPED, struct.pack("<I", 0) + bytes(2)))
        s2 = tlv(tarec.RK_TICK, struct.pack("<I", 5))
        return (self.head(), block(tarec.RB_STREAM, 1, 4, s1), block(tarec.RB_SNAPSHOT, 4, 4, snap),
                block(tarec.RB_STREAM, 5, 5, s2), end_block(5))

    def test_well_formed(self):
        code, out = self.run_pov(*self.good())
        self.assertEqual(code, 0, out)
        self.assertIn("2 samples, 1 clicks, 1 selections; 1 snapshots", out)
        self.assertIn("left click at (100, 0, 200)", out)

    def test_spacing_and_change(self):
        parts = list(self.good(second=pov()))
        code, out = self.run_pov(*parts)
        self.assertEqual(code, 1)
        self.assertIn("equal to the last one", out)
        code, out = self.run_pov(*self.good(second=pov(cam=(130, 200), every=4)))
        self.assertEqual(code, 1)
        self.assertIn("under its every 4", out)

    def test_snapshot_must_rebuild_the_stream(self):
        code, out = self.run_pov(*self.good(snap_sel=()))
        self.assertEqual(code, 1)
        self.assertIn("player 0's selection: 1 units in the recording, 0 in the snapshot", out)
        code, out = self.run_pov(*self.good(snap_pov=[(4, pov(cam=(130, 200)))]))
        self.assertEqual(code, 1)
        self.assertIn("player 0's samples: recording at GameTime [1, 4], snapshot at [4]", out)

    def test_clock_and_fit(self):
        """The mirror of tagpu_persp.c on the plan's rules (persp_test.py checks it against the C
        on random cases)."""
        held = [(1, pov()), (4, pov(cam=(130, 170)))]
        self.assertEqual(tarec.persp_sample([], 9), (tarec.PERSP_NONE, None))
        how, f = tarec.persp_sample(held, 7)                 # t = 4: the latest
        self.assertEqual((how, f[7], f[8]), (tarec.PERSP_LATEST, 130, 170))
        how, f = tarec.persp_sample(held, 5)                 # t = 2: a third of the way
        self.assertEqual((how, f[7], f[8]), (tarec.PERSP_BETWEEN, 110, 190))
        how, f = tarec.persp_sample([(1, pov()), (4, pov(cam=(130, 160)))], 5)
        self.assertEqual(f[8], 187)                          # -40/3 truncated toward zero
        how, f = tarec.persp_sample(held, 3)                 # t = 0: before the earlier one
        self.assertEqual((how, f[7]), (tarec.PERSP_EARLIER, 100))
        how, f = tarec.persp_sample([(1, pov()), (8, pov(cam=(130, 170)))], 9)
        self.assertEqual((how, f[7]), (tarec.PERSP_EARLIER, 100))   # 7 apart > 2 * every
        f = tarec.REC_POV.unpack(pov(cam=(1000, 800)))
        self.assertEqual(tarec.persp_follow(f, 640, 480), (680, 560, 65536))
        self.assertEqual(tarec.persp_follow(f, 1000, 600), (500, 500, 81920))
        self.assertEqual(tarec.persp_follow(f, 700, 500)[2], 68266)
        self.assertIsNone(tarec.persp_follow(f, 0, 480))

    def test_eye(self):
        path = Path(self.tmp.name) / "p.imprec"
        path.write_bytes(tarec.REC_MAGIC + b"".join(self.good()))
        out = io.StringIO()
        with redirect_stdout(out):
            code = tarec.main(["tarec.py", "pov", str(path), "--eye", "5", "--viewer", "800,600"])
        self.assertEqual(code, 0, out.getvalue())
        self.assertIn("sample between of the samples at [1, 4]: camera (110, 200)",
                      out.getvalue())
        self.assertIn("on 800x600: eye (-290, -100), level 81920/65536", out.getvalue())
        with redirect_stdout(io.StringIO()):
            self.assertEqual(tarec.main(["tarec.py", "pov", str(path), "--eye", "3", "--viewer",
                                         "800,600"]), 1)   # no iteration 3
            self.assertEqual(tarec.main(["tarec.py", "pov", str(path), "--eye", "5", "--viewer",
                                         "800,600", "--seat", "1"]), 1)   # no samples

    def test_refusals(self):
        for bad in (selection(0, [(2, 1), (1, 1)]),          # descending
                    selection(0, [(3, 1)]),                  # outside the block [1, 2]
                    selection(0, [(1, 1), (2, 1), (2, 2)]),  # past units_each
                    pov(zoom=1)):                            # a zoom no level has
            kind = tarec.RK_POV if len(bad) == tarec.REC_POV.size else tarec.RK_SELECT
            parts = list(self.good())
            parts[3] = block(tarec.RB_STREAM, 5, 5, tlv(tarec.RK_TICK, struct.pack("<I", 5)) +
                             tlv(kind, bad))
            code, out = self.run_pov(*parts)
            self.assertEqual(code, 1, out)
            self.assertIn("unreadable", out)
        parts = list(self.good())
        parts[0] = self.head(fmt=7)
        with self.assertRaisesRegex(tarec.Unreadable, f"format 7, not {tarec.REC_FORMAT}"):
            self.run_pov(*parts)


class CellsAfterEvents(unittest.TestCase):
    """tarec's World takes an RK_CELLS as an image only before its iteration's first event, as
    the DLL's reader does (tagpu_recreader.c, apply): after it, only the tick's feature call may
    hold cells, as its input, and a cell outside that call is a malformed file."""

    @staticmethod
    def cells(x, fill):
        return struct.pack("<HHHH", x, 0, 1, 0) + bytes([fill]) * tarec.REC_CELL_BYTES

    def world(self):
        w = tarec.World(4, [], dims=(2, 2, 1, 1))
        w.apply(tarec.RK_TICK, struct.pack("<I", 1))
        return w

    def test_image_before_the_first_event(self):
        w = self.world()
        self.assertIsNone(w.apply(tarec.RK_CELLS, self.cells(1, 7)))
        self.assertEqual(w.grid[tarec.REC_CELL_BYTES:2 * tarec.REC_CELL_BYTES],
                         bytes([7]) * tarec.REC_CELL_BYTES)

    def test_tick_input_inside_the_tick_call(self):
        w = self.world()
        w.apply(tarec.RK_EV_FEATURE, bytes([tarec.FR_TICK, 0, 0, 0]) + bytes(4))
        self.assertEqual(w.apply(tarec.RK_CELLS, self.cells(0, 9))[0], tarec.RK_CELLS)
        w.apply(tarec.RK_EV_FEATURE_END, bytes([tarec.FR_TICK, 0, 0, 0]) + bytes(4))
        self.assertEqual(w.grid, bytearray(4 * tarec.REC_CELL_BYTES))
        with self.assertRaisesRegex(tarec.Unreadable, "after an event"):
            w.apply(tarec.RK_CELLS, self.cells(0, 9))

    def test_refused_after_an_event_until_the_next_tick(self):
        w = self.world()
        w.apply(tarec.RK_EV_STATE, struct.pack("<IB3x", 1, 2))
        with self.assertRaisesRegex(tarec.Unreadable, "after an event"):
            w.apply(tarec.RK_CELLS, self.cells(0, 5))
        w.apply(tarec.RK_TICK, struct.pack("<I", 2))
        self.assertIsNone(w.apply(tarec.RK_CELLS, self.cells(0, 5)))


class TakeoverFixtures(unittest.TestCase):
    """fixtures.json's takeover entries, and the processes they make a fixture run."""

    def test_entries(self):
        fixes = tareplay.fixtures()
        took = {n: f["takeover"] for n, f in fixes.items() if "takeover" in f}
        self.assertIn("battle", took)
        self.assertIn("takeover-end", took)
        known = {"player", "order", "switches", "type", "dump", "checks", "at"}
        for name, t in took.items():
            self.assertLessEqual(set(t), known, name)
            self.assertTrue(t["player"] == "controlled" or isinstance(t["player"], int), name)
            self.assertTrue(t["checks"], name)
            self.assertLessEqual(set(t["checks"]), set(tareplay.TAKEOVER_CHECKS), name)
            self.assertTrue((tareplay.SCENARIOS / fixes[name]["load"]).exists(), name)
            self.assertEqual(tareplay.procs(fixes[name]), ("L", "P", "R", "Q", "K", "F") +
                             (("W",) if fixes[name].get("watch") else ()) +
                             (("X",) if fixes[name].get("forks") else ()) +
                             (("U",) if fixes[name].get("rewind") else ()))
            if "order" in t:
                self.assertEqual(len(t["order"]), 2, name)
        self.assertEqual(tareplay.procs(fixes["forest-fire"]), ("L", "P", "R", "Q"))
        # tenseat's takeover is pinned to the tick after its typed LOS command, where the
        # takeover's contact bits must come from the recording (tagpu_replay.c, R14b)
        self.assertEqual(took["tenseat"].get("at"), 901)
        self.assertIn([900, "type", "+los"], fixes["tenseat"]["steps"])
        fx = {"name": "tenseat", "fix": fixes["tenseat"], "Ts": [361, 541, 691, 841]}
        self.assertEqual(tareplay.tk_point(fx, {"options": {"controlled": 0}}), (901, 0))
        self.assertEqual(tareplay.tk_point(fx, {"options": {"controlled": 0}}, None, 931), (931, 0))
        bad = dict(fx, fix=dict(fixes["tenseat"], takeover=dict(took["tenseat"], at=5000)))
        with self.assertRaises(tareplay.CouldNotRun):
            tareplay.tk_point(bad, {"options": {"controlled": 0}})
        end = json.loads((tareplay.SCENARIOS / fixes["takeover-end"]["load"]).read_text())
        self.assertFalse(end["setup"]["clear_existing"])     # the commanders keep the economy
        self.assertIn("victory", took["takeover-end"]["checks"])
        # the GameTimes a takeover-end run draws leave T2 inside the recording
        Ts = tareplay.gametimes("takeover-end", fixes["takeover-end"]["end"], "x")
        self.assertLess(Ts[1], fixes["takeover-end"]["end"])


class AiActivity(unittest.TestCase):
    def lines(self, inactive=None, dispatched=True):
        return [f"recorder: AI activity test seat {s} orders "
                f"{0 if s == inactive else 7} executed "
                f"{0 if s == inactive or not dispatched else 3}"
                for s in range(10)] + ["recorder: AI activity test done [100,1900) bad 0"]

    def test_each_ai_issues_and_executes(self):
        self.assertTrue(tarewind.ai_activity(self.lines(), "test", 100, 1900, {1, 2, 3})[0])

    def test_inactive_ai_cannot_be_hidden_by_other_seats(self):
        ok, detail = tarewind.ai_activity(self.lines(inactive=2), "test", 100, 1900, {1, 2, 3})
        self.assertFalse(ok)
        self.assertIn("missing [2]", detail)

    def test_issued_but_never_dispatched_does_not_pass(self):
        self.assertFalse(tarewind.ai_activity(self.lines(dispatched=False), "test",
                                            100, 1900, {1, 2, 3})[0])

    def test_incomplete_wrong_window_or_observer_error_fails(self):
        lines = self.lines()
        for bad in (lines[:-1], lines[1:], lines + [lines[0]],
                    ["recorder: AI activity test done [100,1899) bad 0"] + lines,
                    lines[:-1] + ["recorder: AI activity test done [100,1900) bad 1"]):
            self.assertFalse(tarewind.ai_activity(bad, "test", 100, 1900, {1, 2, 3})[0])
        self.assertFalse(tarewind.ai_activity(lines, "test", 101, 1901, {1, 2, 3})[0])


class TakeoverAi(unittest.TestCase):
    def check(self, inactive=None, window=True, fraction=.92):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            (out / "K").mkdir()
            lines = AiActivity().lines(inactive=inactive)
            (out / "K" / "tagpu.log").write_text(
                "\n".join(line.replace("activity test", "activity takeover") for line in lines))
            k = {"T": 90, "P": 0, "file": "unused", "timeline": 1}
            if window:
                k["ai_window"] = [100, 1900]
            fx = {"fix": {"takeover": {"checks": ["ai"]}},
                  "procs": {"K": "ran"}, "out": out, "tk": k}
            segs = [{"id": 0, "last": 2000}, {"id": 1, "parent": 0, "last": 2000}]
            with patch.object(tareplay, "tk_ai", return_value=({1, 2, 3}, set(), fraction, 978)), \
                 patch.object(tarec, "file_segments", return_value=(segs, None)):
                return tareplay.tk_checks(fx).results()[0]

    def test_active_seats_pass_without_construction(self):
        _, verdict, detail = self.check()
        self.assertEqual(verdict, tareplay.PASS, detail)
        self.assertIn("construction observed for seats [] (reported only)", detail)

    def test_inactive_seat_fails_despite_movement(self):
        _, verdict, detail = self.check(inactive=2)
        self.assertEqual(verdict, tareplay.FAIL, detail)
        self.assertIn("missing [2]", detail)

    def test_missing_observation_is_unrun(self):
        self.assertEqual(self.check(window=False)[1], tareplay.UNRUN)

    def test_movement_threshold_stays_gated(self):
        self.assertEqual(self.check(fraction=.24)[1], tareplay.FAIL)

    def test_no_mobile_units_is_unrun(self):
        self.assertEqual(self.check(fraction=None)[1], tareplay.UNRUN)


class CrashEndsTheWait(unittest.TestCase):
    """A crash ends a log wait at once instead of after its timeout."""

    class Worker:
        def __init__(self, gamedir, up):
            self.gamedir, self.up = gamedir, list(up)

        def running(self):
            return self.up.pop(0) if len(self.up) > 1 else self.up[0]

    def log(self, gamedir, up):
        log = tareplay.Log(self.Worker(gamedir, up))
        log.ALIVE_EVERY = 0
        return log

    def test_an_errorlog_ends_the_wait(self):
        with tempfile.TemporaryDirectory() as d:
            log = self.log(Path(d), [True])
            (Path(d) / "ErrorLog.txt").write_text(
                "TotalA caused an Access Violation in\nmodule TotalA.exe at 0023:004c74c7.\n")
            t0 = time.time()
            with self.assertRaisesRegex(tareplay.Failed, "crashed: module TotalA.exe at 0023:004c74c7"):
                log.wait(r"never", 60)
            self.assertLess(time.time() - t0, 5)

    def test_a_process_that_exits_ends_the_wait(self):
        with tempfile.TemporaryDirectory() as d:
            log = self.log(Path(d), [True, False])
            with self.assertRaisesRegex(tareplay.Failed, "process exited"):
                log.wait(r"never", 60)

    def test_a_process_not_yet_seen_running_does_not(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(self.log(Path(d), [False]).wait(r"never", 1))


class BusyGameIsNoStall(unittest.TestCase):
    """A running game that has not answered a peek -- its game thread inside a seek's rebuild --
    is no progress for Stall, and only STALL_S of it is a hung game; a game that is not running
    still fails at once."""

    class Worker:
        def __init__(self, answers):
            self.answers = list(answers)

        def gametime(self, wait, busy=False):
            self.busy = busy
            return self.answers.pop(0)

    def stall(self, answers):
        """A Stall over `answers`, made under the patched clock its caller holds."""
        st = tareplay.Stall(self.Worker(answers))
        st.EVERY = 0
        return st

    def test_unanswered_peeks_within_the_bound_pass(self):
        with patch("time.monotonic", side_effect=[0, 10, 20, 30, 40, 50, 60, 70, 80]):
            st = self.stall([100, None, None, 101])
            for _ in range(4):
                st.check()
        self.assertTrue(st.w.busy)
        self.assertEqual(st.g, 101)

    def test_unanswered_past_the_bound_is_a_hung_game(self):
        late = tareplay.STALL_S + 1
        with patch("time.monotonic", side_effect=[0, 0, 0, late, late]):
            st = self.stall([100, None])
            st.check()
            with self.assertRaisesRegex(tareplay.CouldNotRun, "stood at GameTime 100.*unanswered"):
                st.check()

    def test_the_peek_names_a_busy_game_only_while_it_runs(self):
        w = Mock(name="w", lane=None)
        w.name = "tareplay-1"
        busy = (1, "tacli: no peek lines \u2014 is the game past its first frame?")
        with patch.object(tareplay, "tacli", return_value=busy):
            w.running.return_value = True
            self.assertIsNone(tareplay.Worker.gametime(w, 5, busy=True))
            with self.assertRaisesRegex(tareplay.CouldNotRun, "no peek lines"):
                tareplay.Worker.gametime(w, 5)
            w.running.return_value = False
            with self.assertRaisesRegex(tareplay.CouldNotRun, "no peek lines"):
                tareplay.Worker.gametime(w, 5, busy=True)


class RunLock(unittest.TestCase):
    """tools/runlock.py: one game regression at a time, on a throwaway repository's lock"""

    TOOLS = Path(__file__).resolve().parents[1]

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.tree = Path(self.tmp.name)
        subprocess.run(["git", "init", "-q", str(self.tree)], check=True)
        self.lock = tareplay.runlock.lock_path(self.tree)

    def tearDown(self):
        self.tmp.cleanup()

    def holder(self, hold_s, env=None, wrap=()):
        code = (f"import sys, time; sys.path.insert(0, {str(self.TOOLS)!r}); import runlock; "
                f"t = time.monotonic(); runlock.hold(__import__('pathlib').Path({str(self.tree)!r}), "
                f"'test'); print(round(time.monotonic() - t, 1), flush=True); time.sleep({hold_s})")
        return subprocess.Popen([*wrap, sys.executable, "-c", code], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, env=env)

    def test_a_second_run_waits_for_the_first(self):
        a = self.holder(4)
        self.assertEqual(float(a.stdout.readline()), 0.0)
        b = self.holder(0)
        waited = float(b.stdout.readline())
        a.communicate()
        _, err = b.communicate()
        self.assertGreaterEqual(waited, 2.0)
        self.assertIn("took the run lock after", err)
        self.assertIn("test", self.lock.read_text())

    def test_a_crashed_holder_leaves_no_lock(self):
        a = self.holder(60)
        a.stdout.readline()
        a.kill(); a.communicate()
        b = self.holder(0)
        self.assertEqual(float(b.stdout.readline()), 0.0)
        b.communicate()

    def test_a_child_of_the_holder_does_not_wait(self):
        a = self.holder(4)
        a.stdout.readline()
        env = dict(__import__("os").environ, RUNLOCK_HELD=str(self.lock))
        b = self.holder(0, env=env)
        self.assertEqual(float(b.stdout.readline()), 0.0)
        a.communicate(); b.communicate()

    def test_under_flock_1_on_the_same_file_it_does_not_wait(self):
        b = self.holder(0, wrap=("flock", str(self.lock)))
        self.assertEqual(float(b.stdout.readline()), 0.0)
        b.communicate()
        self.assertEqual(b.returncode, 0)


if __name__ == "__main__":
    import devpy
    devpy.ensure()
    unittest.main(verbosity=1)
