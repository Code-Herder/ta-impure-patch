"""`calib.py engine --resume` offline, on synthetic lane manifests: what a resume keeps, what it plays
again, what it refuses, and that `refs build` over an interrupted run plus its resume gives the
reference an uninterrupted run gives; the generation a cell's P5 fails; and `calib.py identical` over two
such runs.

    python3 tools/tasim/tests/test_resume.py

No game and no recording: refs build's per-lane streaming (calib._lane_job) is replaced by a reader
of the synthetic manifests, and the lanes it reads are still chosen by engrun.finished_lanes.
"""

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import calib  # noqa: E402
import calibref  # noqa: E402
import engrun  # noqa: E402

TREE = HERE.parents[2]
DUEL = TREE / "scenarios" / "calib" / "duel-armpw-corak.json"
EXACT = TREE / "scenarios" / "calib" / "exact-damage-armllt.json"
RUNS = None                                 # each scenario its calib block's runs: the duel's 50


class AnyType(dict):
    def __missing__(self, key):
        return {"metal": 1.0, "energy": 10.0}


class Synthetic:
    """An engine run's folder written by hand: lane-launches, finished or not, of chosen runs."""

    def __init__(self, out: Path, dll: Path, cfg: dict):
        self.out, self.dll, self.cfg = out, dll, cfg
        self.keys = {}
        for key, path, _b, idx, how in engrun.jobs_of([DUEL, EXACT], RUNS, "reload", cfg):
            for i in idx:
                self.keys[engrun.scenario_name(path), i] = (list(key), how)

    def head(self, k, n, renderer="gdi", key=None, dll_sha=None, gravity=8155, setup=None, pacing="nodraw"):
        h = {"lane": k, "launch": n, "recording": f"r{k}-{n}.imprec", "setup_key": key,
             "renderer": renderer, "pacing": pacing, "arena_sha256": engrun.arena_sha256(self.cfg),
             "setup": setup, "engine_reads": {"unit_cap": 1500, "speed_ceiling": 20, "gravity": gravity},
             "dll": {"build": "b0", "sha256": dll_sha or calibref.sha256_file(self.dll)},
             "catalogue_sha256": "c"}
        if renderer is None:
            del h["renderer"]
        if pacing is None:
            del h["pacing"]
        return h

    @staticmethod
    def row(path, i, how):
        name = engrun.scenario_name(path)
        return {"scenario": name, "scenario_sha256": calibref.sha256_file(path), "index": i,
                "seed": 1000 + i, "swap": i % 2, "mode": how, "perturbation": {"seed": 1000 + i},
                "wire_sha256": hashlib.sha256(f"{name}:{i}".encode()).hexdigest(), "t_apply": 91,
                "t_end": 300 + i, "end": "limit", "wall": {"total": 1.0}}

    def lane(self, k, n, path, indices, finished=True, **kw):
        name = engrun.scenario_name(path)
        key, how = self.keys[name, indices[0]]
        d = self.out / f"lane{k}-{n}"
        d.mkdir(parents=True)
        rows = [kw.pop("head", None) or self.head(k, n, key=key, **kw)]
        rows += [self.row(path, i, self.keys[name, i][1]) for i in indices]
        f = "manifest.jsonl" if finished else "manifest.partial.jsonl"
        (d / f).write_text("".join(json.dumps(r) + "\n" for r in rows))
        return d


def fake_lane_jobs(runs_dir, j, all_traces=False):
    """calib.lane_jobs with each lane read from its manifest instead of streamed from a recording;
    the lanes are the ones the real one reads."""
    cfg = calibref.arena()
    lanes = engrun.finished_lanes(runs_dir)
    got = []
    for d in lanes:
        rows = [json.loads(ln) for ln in (d / "manifest.jsonl").read_text().splitlines()]
        runs = []
        for r in rows[1:]:
            path = TREE / r["scenario"]
            block = calibref.calib_block(json.loads(path.read_text()), path.name, cfg)
            runs.append({"row": r, "start_check": "ok", "checks": {"R1": "ok"}, "failures": {},
                         "exogenous": {"rng": r["seed"]}, "block": block,
                         "metrics": {"ttk": r["t_end"] - r["t_apply"]}})
        got.append({"lane": str(d), "head": rows[0], "values": AnyType(), "catalogue": {"units": []},
                    "runs": runs, "run_ticks": 1, "seconds": 1.0})
    return lanes, got


class Resume(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.dll = self.root / "ddraw.dll"
        self.dll.write_bytes(b"MZ")
        self.cfg = calibref.arena()

    def tearDown(self):
        self.tmp.cleanup()

    def run_dir(self, name):
        return Synthetic(self.root / name, self.dll, self.cfg)

    def interrupted(self, name="cut"):
        """Three finished lane-launches and one cut short: duel 0..39 and exact 0, 1 finished, duel
        40..49 in the unfinished one."""
        s = self.run_dir(name)
        s.lane(0, 1, DUEL, list(range(20)))
        s.lane(1, 1, DUEL, list(range(20, 40)))
        s.lane(2, 1, DUEL, list(range(40, 50)), finished=False)
        s.lane(3, 1, EXACT, [0, 1])
        return s

    def done(self, s, gpu=False, dll=None, draw=False):
        return engrun.done_runs(s.out, [DUEL, EXACT], RUNS, "reload", self.cfg, dll or self.dll, gpu, draw)

    def test_the_finished_runs_are_kept_and_the_rest_played(self):
        s = self.interrupted()
        done, base, replayed = self.done(s)
        duel, exact = engrun.scenario_name(DUEL), engrun.scenario_name(EXACT)
        self.assertEqual(done, {(duel, i) for i in range(40)} | {(exact, 0), (exact, 1)})
        self.assertEqual((base, replayed), (1, 10))
        left = [(engrun.scenario_name(p), idx, how)
                for _k, p, _b, idx, how in engrun.jobs_of([DUEL, EXACT], RUNS, "reload", self.cfg, done)]
        self.assertEqual(sorted(left), sorted([(duel, list(range(40, 50)), "reload"), (exact, [2], "fresh")]))

    def test_nothing_is_left_once_every_run_finished(self):
        s = self.interrupted()
        s.lane(0, 2, DUEL, list(range(40, 50)))
        s.lane(1, 2, EXACT, [2])
        done, base, _ = self.done(s)
        self.assertEqual(engrun.jobs_of([DUEL, EXACT], RUNS, "reload", self.cfg, done), [])
        self.assertEqual(base, 2)

    def refused(self, s, *what, **kw):
        with self.assertRaises(engrun.ResumeRefused) as cm:
            self.done(s, **kw)
        for w in what:
            self.assertIn(w, str(cm.exception))

    def test_another_renderer_is_refused_by_name(self):
        s = self.interrupted()
        self.refused(s, "lane0-1", "renderer gdi, this run's vulkan", gpu=True)
        s = self.run_dir("old")
        s.lane(0, 1, DUEL, list(range(10)), renderer=None)        # before the field: the default renderer
        self.refused(s, "lane0-1", "renderer vulkan, this run's gdi")

    def test_another_pacing_is_refused_by_name(self):
        self.refused(self.interrupted(), "lane0-1", "pacing nodraw, this run's draw", draw=True)
        s = self.run_dir("drawn")
        s.lane(0, 1, DUEL, list(range(10)), pacing=None)          # before the field: drawn
        self.refused(s, "lane0-1", "pacing draw, this run's nodraw")
        done, _base, _replayed = self.done(s, draw=True)
        self.assertEqual(len(done), 10)

    def test_an_unfinished_vulkan_lane_is_played_again_never_kept(self):
        s = self.run_dir("vk")
        s.lane(0, 1, DUEL, list(range(10)))
        s.lane(1, 1, DUEL, list(range(10, 20)), finished=False, renderer="vulkan")
        done, _base, replayed = self.done(s)
        self.assertEqual(len(done), 10)
        self.assertEqual(replayed, 10)
        self.assertEqual(len(engrun.jobs_of([DUEL], RUNS, "reload", self.cfg, done)), 4)

    def test_another_dll_arena_scenario_or_setup_is_refused(self):
        other = self.root / "other.dll"
        other.write_bytes(b"MZ2")
        self.refused(self.interrupted("a"), "lane0-1", "the DLL", dll=other)
        s = self.run_dir("b")
        h = s.head(0, 1, key=s.keys[engrun.scenario_name(DUEL), 0][0])
        h["arena_sha256"] = "0" * 64
        s.lane(0, 1, DUEL, list(range(10)), head=h)
        self.refused(s, "lane0-1", "another arena")
        s = self.run_dir("c")
        d = s.lane(0, 1, DUEL, [0, 1])
        rows = (d / "manifest.jsonl").read_text().splitlines()
        r = json.loads(rows[2])
        r["scenario_sha256"] = "0" * 64
        (d / "manifest.jsonl").write_text("\n".join(rows[:2] + [json.dumps(r)]) + "\n")
        self.refused(s, "lane0-1", "run 1: the scenario file changed")
        s = self.run_dir("d")
        s.lane(0, 1, DUEL, [0], head=s.head(0, 1, key=s.keys[engrun.scenario_name(DUEL), 0][0][:-2]
                                             + ["fresh", 0]))
        self.refused(s, "lane0-1", "run 0: setup")
        s = self.run_dir("e")
        s.lane(0, 1, DUEL, [0, 1])
        s.lane(1, 1, DUEL, [1, 2])
        self.refused(s, "lane1-1", "run 1: also in lane0-1")

    def refs(self, s, lane_jobs=fake_lane_jobs):
        out = self.root / "refs" / s.out.name
        with mock.patch.object(calib, "lane_jobs", lane_jobs), \
                mock.patch.object(calibref, "engine_label",
                                  return_value={"build": "b0", "commit": "c0ffee" * 6, "head": "c0ffee" * 6,
                                                "head_matches": True}), \
                mock.patch.object(calibref, "install_fingerprint", return_value="fp"), \
                mock.patch.object(calibref, "ref_path", side_effect=lambda sid, c: out / f"{sid}.json"), \
                mock.patch("builtins.print"):
            written, failed = calib.build_refs(s.out, 1, keep=True)
        refs = {}
        for p in written:
            ref = json.loads(p.read_text())
            del ref["created"]
            for r in ref["runs"]:
                del r["lane"], r["recording"]
            refs[p.name] = ref
        return refs, failed

    def test_refs_build_over_a_resume_equals_an_uninterrupted_run(self):
        s = self.interrupted()
        s.lane(0, 2, DUEL, list(range(40, 50)))
        s.lane(1, 2, EXACT, [2])
        whole = self.run_dir("whole")
        whole.lane(0, 1, DUEL, list(range(30)))
        whole.lane(1, 1, DUEL, list(range(30, 50)))
        whole.lane(2, 1, EXACT, [0, 1])
        whole.lane(3, 1, EXACT, [2])
        got, failed = self.refs(s)
        want, failed_whole = self.refs(whole)
        self.assertEqual((failed, failed_whole), ({}, {}))
        self.assertEqual(len(got), 2)
        self.assertEqual(got, want)
        self.assertEqual([r["index"] for r in next(iter(got.values()))["runs"]][:3], [0, 1, 2])

    def test_a_cells_P5_failure_refuses_every_reference(self):
        """One packed cell whose batch paused before its end (P5, CellRun.finish's verdict) fails the generation:
        the exact scenario, whose runs all passed, is refused with the duel that holds the cell."""
        s = self.run_dir("p5")
        s.lane(0, 1, DUEL, list(range(50)))
        s.lane(1, 1, EXACT, [0, 1])
        s.lane(2, 1, EXACT, [2])
        why = "the batch paused at GameTime 900, before the cell's end: no seat at its end_units, and its limit at 950"

        def p5_in_one_cell(runs_dir, j, all_traces=False):
            lanes, got = fake_lane_jobs(runs_dir, j, all_traces)
            r = next(r for g in got for r in g["runs"] if r["row"]["scenario"].endswith(DUEL.name))
            r["row"]["pack"] = {"batch": 3, "K": 8, "k": 5}
            r["checks"]["P5"] = why
            r["failures"], r["start_check"] = {"P5": [why]}, f"P5: {why}"
            return lanes, got

        refs, failed = self.refs(s, p5_in_one_cell)
        self.assertEqual(refs, {})
        self.assertEqual(sorted(failed), sorted(calibref.scenario_id(p) for p in (DUEL, EXACT)))
        for msg in failed.values():
            self.assertIn("the generation fails", msg)
            self.assertIn(f"batch 3, P5: {why}", msg)

    def test_refs_build_refuses_two_renderers(self):
        s = self.run_dir("mixed")
        s.lane(0, 1, DUEL, list(range(10)))
        s.lane(1, 1, DUEL, list(range(10, 50)), renderer="vulkan")
        _refs, failed = self.refs(s)
        self.assertIn("more than one renderer", next(iter(failed.values())))

    def test_refs_build_refuses_two_arenas_or_two_gravities(self):
        s = self.run_dir("arenas")
        h = s.head(1, 1, key=s.keys[engrun.scenario_name(DUEL), 10][0])
        h["arena_sha256"] = "0" * 64
        s.lane(0, 1, DUEL, list(range(10)))
        s.lane(1, 1, DUEL, list(range(10, 50)), head=h)
        _refs, failed = self.refs(s)
        self.assertIn("more than one arena", next(iter(failed.values())))
        s = self.run_dir("gravities")
        s.lane(0, 1, DUEL, list(range(10)))
        s.lane(1, 1, DUEL, list(range(10, 50)), gravity=8154)
        _refs, failed = self.refs(s)
        self.assertIn("more than one gravity", next(iter(failed.values())))

    def test_a_reference_carries_its_setup_and_its_gravity(self):
        s = self.run_dir("mod")
        s.lane(0, 1, DUEL, list(range(50)), setup="twilight-2.0b98")
        refs, failed = self.refs(s)
        self.assertEqual(failed, {})
        ref = next(iter(refs.values()))
        self.assertEqual((ref["engine"]["mod"], ref["setup"]["gravity"]), ("twilight-2.0b98", 8155))
        s = self.run_dir("old")
        h = s.head(0, 1, key=s.keys[engrun.scenario_name(DUEL), 0][0])
        del h["engine_reads"], h["setup"]       # a head from before the reads
        s.lane(0, 1, DUEL, list(range(50)), head=h)
        refs, failed = self.refs(s)
        ref = next(iter(refs.values()))
        self.assertEqual((ref["engine"]["mod"], "gravity" in ref["setup"]), ("retail", False))

    def test_a_resume_of_another_setup_is_refused_by_name(self):
        s = self.run_dir("setup")
        s.lane(0, 1, DUEL, list(range(10)), setup="twilight-2.0b98")
        self.refused(s, "lane0-1", "the setup twilight-2.0b98, this run's retail")


def traced(change=None):
    """fake_lane_jobs with a trace a run: a's row at t 10, b's death at its end; `change` {(index, t): row} gives
    a run another row of a at t."""
    change = change or {}

    def lane_jobs(runs_dir, j, all_traces=False):
        lanes, got = fake_lane_jobs(runs_dir, j, all_traces)
        for g in got:
            for r in g["runs"]:
                i, end = r["row"]["index"], r["row"]["t_end"] - r["row"]["t_apply"]
                rows = {t: row for (k, t), row in change.items() if k == i}
                r["trace"] = [{"units": {}}, {"t": 0, "ev": [["create", "a"], ["create", "b"]]},
                              {"t": 10, "u": {"a": rows.pop(10, [1, 0, 2, 300])}},
                              *({"t": t, "u": {"a": row}} for t, row in rows.items()),
                              {"t": end, "ev": [["death", "b", "a"]]}]
        return lanes, got
    return lane_jobs


class Identical(unittest.TestCase):
    """`calib.py identical`: two engine runs' traces of the same runs, unit rows and events at every t."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        dll = self.root / "ddraw.dll"
        dll.write_bytes(b"MZ")
        self.a = Synthetic(self.root / "a", dll, calibref.arena())
        self.b = Synthetic(self.root / "b", dll, calibref.arena())
        self.a.lane(0, 1, DUEL, [0, 1, 2])
        self.b.lane(0, 1, DUEL, [0, 1])
        self.b.lane(1, 1, DUEL, [2])

    def tearDown(self):
        self.tmp.cleanup()

    def identical(self, lane_jobs, exogenous=False):
        printed = []
        with mock.patch.object(calib, "lane_jobs", lane_jobs), \
                mock.patch("builtins.print", side_effect=lambda *a, **k: printed.append(" ".join(map(str, a)))):
            code = calib.cmd_identical(SimpleNamespace(runs_a=self.a.out, runs_b=self.b.out, j=1,
                                                       exogenous=exogenous))
        return code, printed

    def test_exogenous_inputs_are_held_equal_when_asked(self):
        def with_wind(windnext):
            def lane_jobs(d, j, all_traces=False):
                lanes, got = traced()(d, j, all_traces)
                for g in got:
                    for r in g["runs"]:
                        r["exogenous"] = {"rngseed": 7, "windnext_since_apply": 210 if d == self.a.out or
                                          r["row"]["index"] != 1 else windnext}
                return lanes, got
            return lane_jobs
        self.assertEqual(self.identical(with_wind(210), exogenous=True)[0], 0)
        code, printed = self.identical(with_wind(150), exogenous=True)
        self.assertEqual(code, 1)
        self.assertTrue(any("run 1" in ln and "exogenous windnext_since_apply 210 / 150" in ln for ln in printed),
                        printed)
        self.assertEqual(self.identical(with_wind(150))[0], 0)               # the traces alone: C8's question
        def unrecorded(d, j, all_traces=False):
            lanes, got = traced()(d, j, all_traces)
            got[0]["runs"][0]["exogenous"] = None
            return lanes, got
        code, printed = self.identical(unrecorded, exogenous=True)
        self.assertEqual(code, 1)
        self.assertTrue(any("exogenous not recorded" in ln for ln in printed), printed)

    def test_equal_traces_pass_whatever_lane_holds_them(self):
        code, printed = self.identical(traced())
        self.assertEqual(code, 0, printed)
        self.assertEqual(sum("first difference none" in ln for ln in printed), 3)

    def test_a_difference_is_named_at_its_t(self):
        lanes_a, lanes_b = traced(), traced({(2, 10): [1, 0, 2, 299], (1, 40): [5, 0, 2, 300]})
        code, printed = self.identical(lambda d, j, all_traces=False: (lanes_a if d == self.a.out else lanes_b)(
            d, j, all_traces))
        self.assertEqual(code, 1)
        self.assertTrue(any("run 2" in ln and "first difference at t 10" in ln for ln in printed), printed)
        self.assertTrue(any("run 1" in ln and "first difference at t 40" in ln for ln in printed), printed)

    def test_a_failed_check_or_a_run_in_one_alone_fails(self):
        def failing(d, j, all_traces=False):
            lanes, got = traced()(d, j, all_traces)
            if d == self.b.out:
                got[0]["runs"][0]["start_check"] = "P5: the batch paused at GameTime 900, before the cell's end"
            return lanes, got
        code, printed = self.identical(failing)
        self.assertEqual(code, 1)
        self.assertTrue(any("run 0" in ln and "B P5: the batch paused" in ln for ln in printed), printed)
        self.b.lane(2, 1, DUEL, [3])
        code, printed = self.identical(traced())
        self.assertEqual(code, 1)
        self.assertIn("duel-armpw-corak run 3: in B alone", printed)

    def test_trace_difference(self):
        a = [{}, {"t": 0, "ev": [["create", "a"]]}, {"t": 5, "u": {"a": [1]}}]
        self.assertIsNone(calib.trace_difference(a, [{"other": "head"}] + a[1:]))
        self.assertEqual(calib.trace_difference(a, a[:2]), 5)
        self.assertEqual(calib.trace_difference(a, a + [{"t": 7, "ev": [["death", "a", None]]}]), 7)


if __name__ == "__main__":
    unittest.main()
