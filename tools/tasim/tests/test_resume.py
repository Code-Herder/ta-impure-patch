"""`calib.py engine --resume` offline, on synthetic lane manifests: what a resume keeps, what it plays
again, what it refuses, and that `refs build` over an interrupted run plus its resume gives the
reference an uninterrupted run gives.

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

    def head(self, k, n, renderer="gdi", key=None, dll_sha=None):
        h = {"lane": k, "launch": n, "recording": f"r{k}-{n}.imprec", "setup_key": key,
             "renderer": renderer, "arena_sha256": engrun.arena_sha256(self.cfg),
             "dll": {"build": "b0", "sha256": dll_sha or calibref.sha256_file(self.dll)},
             "catalogue_sha256": "c"}
        if renderer is None:
            del h["renderer"]
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
                    "runs": runs, "ticks": 1, "seconds": 1.0})
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

    def done(self, s, gpu=False, dll=None):
        return engrun.done_runs(s.out, [DUEL, EXACT], RUNS, "reload", self.cfg, dll or self.dll, gpu)

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

    def refs(self, s):
        out = self.root / "refs" / s.out.name
        with mock.patch.object(calib, "lane_jobs", fake_lane_jobs), \
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

    def test_refs_build_refuses_two_renderers(self):
        s = self.run_dir("mixed")
        s.lane(0, 1, DUEL, list(range(10)))
        s.lane(1, 1, DUEL, list(range(10, 50)), renderer="vulkan")
        _refs, failed = self.refs(s)
        self.assertIn("more than one renderer", next(iter(failed.values())))


if __name__ == "__main__":
    unittest.main()
