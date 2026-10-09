#!/usr/bin/env python3
"""Tests for tools/tarun.py: the run files, liveness, nesting, the estimate and the shell commands.
Every test writes under a temporary TARUN_DIR; nothing starts a game."""

import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tarun  # noqa: E402


def lanes(*ids, pool="game"):
    """tareplay's shape: L first, the rest after it, F after K."""
    out = []
    for k, i in enumerate(ids):
        after = [] if i == "L" else ["K"] if i == "F" else ["L"]
        out.append({"id": i, "after": after, "pool": pool, "order": k})
    return out


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="tarun-test-"))
        self.env = mock.patch.dict(os.environ, {"TARUN_DIR": str(self.tmp),
                                                "CLAUDE_CODE_SESSION_ID": "s1",
                                                "CLAUDE_PID": str(os.getpid())})
        self.env.start()
        os.environ.pop("TARUN_RUN", None)

    def tearDown(self):
        self.env.stop()
        shutil.rmtree(self.tmp)

    def doc(self, run):
        return json.loads(run.path.read_text())


class Files(Base):
    def test_a_planned_run_is_one_whole_file_with_every_step_planned(self):
        run = tarun.begin("tareplay", [("battle", lanes("L", "P", "K", "F"))], kind="landing",
                          pools={"game": 2})
        d = self.doc(run)
        self.assertEqual(d["session"], "s1")
        self.assertEqual(d["state"], "running")
        self.assertEqual([s["state"] for s in d["items"][0]["steps"]], ["planned"] * 4)
        self.assertTrue(tarun.alive(d))
        self.assertEqual(list(self.tmp.glob("sessions/s1/*.tmp")), [])

    def test_steps_end_and_the_run_ends_with_what_never_ran_unrun(self):
        run = tarun.begin("tareplay", [("battle", lanes("L", "P"))])
        run.start("battle", "L", instance="tareplay-1", display=":12")
        run.finish("battle", "L", "done")
        run.end("stopped")
        d = self.doc(run)
        L, P = d["items"][0]["steps"]
        self.assertEqual((L["state"], L["instance"]), ("done", "tareplay-1"))
        self.assertEqual(P["state"], "unrun")
        self.assertEqual((d["state"], d["eta"]), ("stopped", None))

    def test_checks_name_every_failure_and_fail_the_item(self):
        run = tarun.begin("tareplay", [("battle", lanes("L"))])
        run.start("battle", "L")
        run.finish("battle", "L", "done")
        run.checks("battle", 40, failed=["P compare --puppet: unit 412"])
        it = self.doc(run)["items"][0]
        self.assertEqual(it["checks"]["fail"], ["P compare --puppet: unit 412"])
        self.assertEqual(it["state"], "fail")

    def test_a_dead_owner_is_not_alive_and_a_reused_pid_is_not_it(self):
        p = subprocess.Popen(["sleep", "30"])
        run = tarun.begin("tacli", [("", [{"id": "game"}])], owner="instance", pid=p.pid)
        d = self.doc(run)
        self.assertTrue(tarun.alive(d))
        self.assertFalse(tarun.alive(dict(d, pid_start=d["pid_start"] + 1)))
        p.kill()
        p.wait()
        self.assertFalse(tarun.alive(d))

    def test_an_error_switches_the_journal_off_and_never_raises(self):
        run = tarun.begin("tareplay", [("battle", lanes("L"))])
        err = io.StringIO()
        with redirect_stderr(err):
            run.start("battle", "no such lane")
            run.finish("battle", "L", "done")       # off: does nothing, raises nothing
        self.assertTrue(run.off)
        self.assertIn("journal off", err.getvalue())
        self.assertEqual(self.doc(run)["items"][0]["steps"][0]["state"], "planned")


class Nesting(Base):
    def test_a_gate_inside_an_open_landing_files_under_its_running_step(self):
        landing = tarun.begin("landing", [("", [{"id": "build"}, {"id": "replay"}])],
                              owner="session", adopts=True)
        landing.start("", "replay")
        gate = tarun.begin("tareplay", [("battle", lanes("L"))], gate=True)
        self.assertEqual(self.doc(gate)["parent"], {"run": landing.doc["id"], "step": "replay"})
        self.assertEqual(os.environ["TARUN_RUN"], gate.doc["id"])
        gate.end("pass")
        self.assertNotIn("TARUN_RUN", os.environ)

    def test_a_landing_whose_session_is_gone_adopts_nothing(self):
        with mock.patch.dict(os.environ, {"CLAUDE_PID": "999999999"}):
            tarun.begin("landing", [("", [{"id": "replay"}])], owner="session", adopts=True)
        gate = tarun.begin("tareplay", [("battle", lanes("L"))])
        self.assertIsNone(self.doc(gate)["parent"])

    def test_a_remote_game_adopts_nothing(self):
        tarun.tacli_launched("winbox", 4242, remote=True)
        self.assertEqual(tarun.read_runs()[0]["owner"], "session")
        gate = tarun.begin("tareplay", [("battle", lanes("L"))])
        self.assertIsNone(self.doc(gate)["parent"])

    def test_tacli_under_a_gate_writes_nothing(self):
        with mock.patch.dict(os.environ, {"TARUN_RUN": "a-gate"}):
            tarun.tacli_launched("tareplay-1", os.getpid())
            tarun.tacli_command("tareplay-1", ["keys", "x"])
        self.assertEqual(tarun.read_runs(), [])

    def test_a_tacli_game_is_a_run_from_launch_to_stop(self):
        tarun.tacli_launched("probe", os.getpid(), ":5")
        tarun.tacli_command("probe", ["ui", "click", "OK"])
        r = tarun.instance_run("probe")
        self.assertEqual((r["last"], r["items"][0]["steps"][0]["state"]), ("ui click OK", "running"))
        tarun.tacli_stopped("probe")
        self.assertIsNone(tarun.instance_run("probe"))
        self.assertEqual(tarun.read_runs()[0]["state"], "pass")

    def test_a_relaunch_ends_the_previous_game_of_the_name(self):
        p = subprocess.Popen(["sleep", "30"])
        try:
            tarun.tacli_launched("probe", p.pid)
            tarun.tacli_launched("probe", os.getpid())
            states = sorted(r["state"] for r in tarun.read_runs())
            self.assertEqual(states, ["running", "unrun"])
        finally:
            p.kill()
            p.wait()


class Estimate(Base):
    def run_with(self, history, items, pools, variant=""):
        doc = {"pools": pools, "variant": variant,
               "items": [{"id": i, "steps": [dict({"state": "planned"}, **s) for s in st]}
                         for i, st in items]}
        return doc, {(i, s, variant): v for (i, s), v in history.items()}

    def test_nothing_known_is_no_estimate(self):
        doc, h = self.run_with({}, [("battle", lanes("L", "P"))], {"game": 2})
        self.assertEqual(tarun.schedule(doc, h, time.time()), (None, False))

    def test_lanes_wait_for_their_lane_and_share_the_pool(self):
        # L 100 then P, R, Q (10 each) on two workers: 100 + 10 + 10
        doc, h = self.run_with({("battle", "L"): [100], ("battle", "P"): [10],
                                ("battle", "R"): [10], ("battle", "Q"): [10]},
                               [("battle", lanes("L", "P", "R", "Q"))], {"game": 2})
        self.assertEqual(tarun.schedule(doc, h, time.time()), (120, False))

    def test_the_median_of_the_last_five(self):
        doc, h = self.run_with({("battle", "L"): [1000, 1, 2, 3, 4, 5]},
                               [("battle", lanes("L"))], {"game": 1})
        self.assertEqual(tarun.schedule(doc, h, time.time())[0], 3)

    def test_an_unbounded_pool_never_waits_for_a_busy_worker(self):
        steps = [{"id": "L", "pool": "game"},
                 {"id": "checks", "pool": "offline", "after": []}]
        doc, h = self.run_with({("a", "L"): [100], ("a", "checks"): [5]}, [("a", steps)],
                               {"game": 1, "offline": 0})
        now = time.time()
        doc["items"][0]["steps"][0].update(state="running", started=now)
        self.assertEqual(tarun.schedule(doc, h, now), (100, False))

    def test_a_running_lane_follows_its_gametime_rate(self):
        doc, h = self.run_with({("battle", "L"): [1000]}, [("battle", lanes("L"))], {"game": 1})
        now = time.time()
        # 1000 GameTime in 100 s, 4000 to go: 400 s, whatever the history said
        doc["items"][0]["steps"][0].update(state="running", started=now - 120, gt=2000,
                                           gt_target=6000, gt_seen=[1000, 2000, now - 100, now])
        self.assertEqual(tarun.schedule(doc, h, now)[0], 400)

    def test_a_step_with_no_history_makes_it_a_lower_bound(self):
        doc, h = self.run_with({("battle", "L"): [100]}, [("battle", lanes("L", "P"))],
                               {"game": 1})
        self.assertEqual(tarun.schedule(doc, h, time.time()), (100, True))

    def test_finished_steps_feed_the_next_runs_estimate(self):
        run = tarun.begin("tareplay", [("battle", lanes("L"))], variant="gs20-t1")
        run.start("battle", "L")
        run.steps[("battle", "L")]["started"] -= 60
        run.finish("battle", "L", "done")
        run.end("pass")
        nxt = tarun.begin("tareplay", [("battle", lanes("L"))], variant="gs20-t1")
        self.assertEqual(nxt.doc["eta"]["seconds"], 60)
        other = tarun.begin("tareplay", [("battle", lanes("L"))], variant="gs10-t0")
        self.assertIsNone(other.doc["eta"])


class Commands(Base):
    def cli(self, *argv):
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(io.StringIO()):
            rc = tarun.main(list(argv))
        return rc, out.getvalue().strip()

    def test_a_landing_driven_from_the_shell(self):
        rc, rid = self.cli("begin", "landing", "--title", "landing x -> main", "commit",
                           "build", "replay")
        self.assertEqual(rc, 0)
        self.assertEqual(self.cli("step", "landing", "commit", "start")[0], 0)
        self.assertEqual(self.cli("step", "landing", "commit", "pass")[0], 0)
        self.assertEqual(self.cli("step", "landing", "build", "skipped", "--detail", "docs")[0],
                         0)
        self.assertEqual(self.cli("step", "landing", "nope", "start")[0], 2)
        d = json.loads((tarun.session_dir() / f"{rid}.json").read_text())
        self.assertEqual([s["state"] for s in d["items"][0]["steps"]],
                         ["pass", "skipped", "planned"])
        self.assertEqual(d["items"][0]["steps"][2]["after"], ["build"])
        self.assertEqual(self.cli("end", "landing", "pass")[0], 0)
        self.assertEqual(self.cli("step", "landing", "replay", "start")[0], 2)
        self.assertIn("landing x -> main", self.cli("ls")[1])

    def test_jobs_make_the_steps_independent(self):
        _, rid = self.cli("begin", "probe", "--jobs", "4", "a", "b", "c")
        d = json.loads((tarun.session_dir() / f"{rid}.json").read_text())
        self.assertEqual(d["pools"], {"main": 4})
        self.assertTrue(all("after" not in s for s in d["items"][0]["steps"]))


class Prune(Base):
    def test_an_old_session_goes_and_the_history_stays(self):
        old = tarun.session_dir("old")
        old.mkdir(parents=True)
        (old / "x.json").write_text("{}")
        (tarun.root() / "history.jsonl").write_text("")
        t = time.time() - (tarun.PRUNE_DAYS + 1) * 86400
        os.utime(old / "x.json", (t, t))
        os.utime(old, (t, t))
        tarun.prune()
        self.assertFalse(old.exists())
        self.assertTrue((tarun.root() / "history.jsonl").exists())


if __name__ == "__main__":
    unittest.main()
