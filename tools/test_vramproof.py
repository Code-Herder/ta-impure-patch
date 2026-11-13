#!/usr/bin/env python3
"""Offline tests of tools/vramproof.py's verdict: what it reads of a game's log, the nvidia-smi
XML it samples, and the judgement against the run lock's figures. No game runs.

    python3 tools/test_vramproof.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vramproof  # noqa: E402

CFG = {"reserve_mib": 3072, "cap_mib": 768, "driver_mib": 32, "margin_mib": 32}
TOTAL = 12282

LOG = """vk: up in 226 ms on "NVIDIA GeForce RTX 4070"
vk: gpumem: this device's allocations are capped at 768 MiB of device-local memory (impure.cfg gpumem=, memory types 0x2)
vk: gpumem: 16 MiB held of the 768 MiB cap (a new high water)
vk: gpumem: 640 MiB held of the 768 MiB cap (a new high water)
vk: terr: the 7106x7106 neighbourhood atlas (43658 keys) does not fit: 192 MB against half of the 120 MB left under the 768 MB cap (impure.cfg gpumem=) - the terrain restores per tile
"""

XML = """<?xml version="1.0" ?>
<nvidia_smi_log><gpu id="00000000:01:00.0">
<fb_memory_usage><total>12282 MiB</total><reserved>0 MiB</reserved><used>6067 MiB</used></fb_memory_usage>
<processes>
<process_info><pid>2668</pid><type>G</type><process_name>Xorg</process_name><used_memory>1103 MiB</used_memory></process_info>
<process_info><pid>4242</pid><type>C+G</type><process_name>TotalA.exe</process_name><used_memory>701 MiB</used_memory></process_info>
<process_info><pid>4243</pid><type>G</type><process_name>x</process_name><used_memory>N/A</used_memory></process_info>
</processes></gpu></nvidia_smi_log>
"""


class VramProof(unittest.TestCase):

    def test_a_log_says_its_cap_its_ledger_and_the_terrains_choice(self):
        got = vramproof.read_log(LOG)
        self.assertEqual(got["cap"], 768)
        self.assertEqual(got["ledger_peak"], 640)
        self.assertEqual(got["refused"], [])
        self.assertEqual(len(got["terrain"]), 1)
        down = LOG + "vk: gpumem: the device is down - it held at most 652 MiB of the 768 MiB cap in at most 310 allocations, and the cap refused 0\n"
        self.assertEqual(vramproof.read_log(down)["ledger_peak"], 652)
        self.assertIsNone(vramproof.read_log("vk: up\n")["cap"])

    def test_the_sample_reads_nvidia_smis_xml(self):
        done = subprocess.CompletedProcess([], 0, XML, "")
        with patch.object(vramproof.subprocess, "run", return_value=done):
            used, total, procs = vramproof.gpu_processes()
        self.assertEqual((used, total), (6067, 12282))
        self.assertEqual(procs, {2668: 1103, 4242: 701})

    def judge(self, samples, logs):
        return vramproof.judge(samples, logs, CFG, TOTAL)

    def test_games_within_the_bound_pass(self):
        logs = {"g0": vramproof.read_log(LOG), "g1": vramproof.read_log(LOG)}
        samples = [{"t": 0, "used": 0, "total": TOTAL, "games": {"g0": 300, "g1": 700}},
                   {"t": 1, "used": 0, "total": TOTAL, "games": {"g0": 800, "g1": 790}}]
        ok, lines = self.judge(samples, logs)
        self.assertTrue(ok, lines)
        self.assertTrue(any("g0: peak 800 MiB of the 800 MiB bound" in ln and "160 MiB outside" in ln
                            for ln in lines), lines)

    def test_a_game_past_the_bound_fails(self):
        logs = {"g0": vramproof.read_log(LOG)}
        ok, lines = self.judge([{"t": 0, "used": 0, "total": TOTAL, "games": {"g0": 801}}], logs)
        self.assertFalse(ok)
        self.assertTrue(lines[0].startswith("FAIL g0: peak 801"), lines)

    def test_games_together_past_the_device_less_the_reserve_fail(self):
        logs = {f"g{k}": vramproof.read_log(LOG) for k in range(12)}
        samples = [{"t": 0, "used": 0, "total": TOTAL, "games": {f"g{k}": 780 for k in range(12)}}]
        ok, lines = self.judge(samples, logs)
        self.assertFalse(ok)
        self.assertTrue(lines[-1].startswith("FAIL together: at most 9360 MiB"), lines)

    def test_a_game_unseen_uncapped_or_refused_fails(self):
        refused = LOG + "vk: gpumem: the cap refuses an allocation of 4194304 bytes - 766 MiB held of 768 MiB, 300 allocations\n"
        cases = {"unseen": (LOG, {}), "uncapped": ("vk: up\n", {"g0": 300}),
                 "other cap": (LOG.replace("capped at 768", "capped at 512"), {"g0": 300}),
                 "refused": (refused, {"g0": 300})}
        for what, (text, games) in cases.items():
            ok, lines = self.judge([{"t": 0, "used": 0, "total": TOTAL, "games": games}],
                                   {"g0": vramproof.read_log(text)})
            self.assertFalse(ok, what)
            self.assertTrue(any(ln.startswith("FAIL g0:") for ln in lines), (what, lines))


    def test_a_watched_suite_reports_its_peaks_per_game_and_over_all(self):
        refused = LOG + "vk: gpumem: the cap refuses an allocation of 4194304 bytes - 766 MiB held of 768 MiB, 300 allocations\n"
        down = LOG + "vk: gpumem: the device is down - it held at most 652 MiB of the 768 MiB cap in at most 310 allocations, and the cap refused 0\n"
        samples = [{"t": 0, "used": 0, "total": TOTAL, "games": {"/i/a": 300, "/i/b": 500}},
                   {"t": 1, "used": 0, "total": TOTAL, "games": {"/i/a": 420, "/i/b": 450}}]
        ledgers = {"/i/a": {"R1": vramproof.read_log(down), "R2": vramproof.read_log(LOG)},
                   "/i/b": {"R7": vramproof.read_log(refused)}}
        lines, figures = vramproof.suite_report(samples, ledgers)
        self.assertEqual((figures["process_peak"], figures["ledger_peak"], figures["together"], figures["games"]),
                         (500, 652, 870, 2))
        self.assertEqual(len(figures["refused"]), 1)
        self.assertTrue(lines[0].startswith("a: a process at most 420 MiB; the ledger at most 652 MiB over 2 run(s), "
                                            "capped at 768"), lines)
        self.assertTrue(lines[1].endswith("the cap refused 1"), lines)
        self.assertTrue(lines[-1].startswith("REFUSED b: vk: gpumem: the cap refuses"), lines)

    def test_watch_samples_every_game_of_any_instance(self):
        with tempfile.TemporaryDirectory() as d:
            inst = Path(d) / "tareplay-3"
            (inst / "prefix").mkdir(parents=True)
            game = subprocess.Popen([sys.executable, "-c", "import sys, time; time.sleep(30)", "TotalA.exe"],
                                    env=dict(os.environ, WINEPREFIX=str(inst / "prefix") + "/"))
            self.addCleanup(game.wait)
            self.addCleanup(game.kill)
            self.assertEqual(vramproof.game_pids(None).get(game.pid), str(inst))
            self.assertNotIn(game.pid, vramproof.game_pids(["elsewhere"]))


# A stand-in run: holds slot argv[2] and seat argv[3] of the runslots folder argv[1] as a run does,
# its holder line naming itself, and starts a stand-in game -- a process named TotalA.exe on its
# command line, in the prefix argv[4] -- that holds the seat as a game's process does; prints the
# game's pid, then waits for its stdin to close.
RUN = """import fcntl, os, subprocess, sys
d = sys.argv[1]
slot = open(os.path.join(d, sys.argv[2]), 'a+'); fcntl.flock(slot, fcntl.LOCK_EX)
slot.seek(0); slot.truncate(); slot.write(f'pid {os.getpid()}, a run of 2 slots, stand-in\\\\n'); slot.flush()
seat = open(os.path.join(d, sys.argv[3]), 'a+'); fcntl.flock(seat, fcntl.LOCK_EX)
game = subprocess.Popen([sys.executable, '-c', 'import sys; sys.stdin.read()', 'TotalA.exe'],
                        stdin=subprocess.PIPE, pass_fds=(seat.fileno(),),
                        env=dict(os.environ, WINEPREFIX=sys.argv[4]))
print(game.pid, flush=True)
sys.stdin.read()
game.stdin.close(); game.wait()
"""


class Watch(unittest.TestCase):
    """watch counts the watched run's games alone, launches them at its --cap, and never runs
    inside another run."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        self.lock = vramproof.runlock.lock_path(self.root)

    def run_(self, slot, tag, inst):
        (inst / "prefix").mkdir(parents=True)
        p = subprocess.Popen([sys.executable, "-c", RUN, str(vramproof.runlock.slots_dir(self.lock)),
                              f"slot-{slot:02d}", f"seat-{tag}-00", str(inst / "prefix")],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

        def end():
            p.stdin.close()
            p.wait()
            p.stdout.close()
        self.addCleanup(end)
        return p, int(p.stdout.readline())

    def test_only_the_watched_runs_games_are_counted(self):
        watched, game = self.run_(2, "s02", self.root / "inst-a")
        other, other_game = self.run_(5, "s05", self.root / "inst-b")
        pids, tags = vramproof.watched_games(watched.pid, self.lock)
        self.assertEqual(tags, {"s02"})
        self.assertEqual(pids, {game: str(self.root / "inst-a")})
        self.assertIn(other_game, vramproof.game_pids(None))     # on the machine, and not counted
        # a command that holds no run counts no game
        idle = subprocess.Popen(["sleep", "60"])
        self.addCleanup(idle.wait)
        self.addCleanup(idle.kill)
        self.assertEqual(vramproof.watched_games(idle.pid, self.lock), ({}, set()))

    def watch(self, *argv):
        out = self.root / "out"
        with patch.object(vramproof, "gpu_processes", return_value=(0, 12282, {})):
            return vramproof.main(["watch", "--out", str(out), *argv]), out

    def test_the_watched_run_plays_at_the_cap_and_no_tracked_file_changes(self):
        said = self.root / "said"
        status = ["git", "-C", str(vramproof.TREE), "status", "--porcelain"]
        before = subprocess.run(status, capture_output=True, text=True).stdout
        held = os.environ.pop(vramproof.runlock.ENV, None)
        self.addCleanup(lambda: held is not None and os.environ.update({vramproof.runlock.ENV: held}))
        code, out = self.watch("--cap", "2000", "--", sys.executable, "-c",
                               f"import os; open({str(said)!r}, 'w').write(os.environ['RUNLOCK_CAP_MIB'])")
        self.assertEqual(code, 0)
        self.assertEqual(said.read_text(), "2000")
        report = json.loads((out / "report.json").read_text())
        self.assertEqual((report["cap"], report["runs"]), (2000, []))
        self.assertTrue(report["lines"][0].startswith("no run of the command held GPU memory"))
        self.assertEqual(subprocess.run(status, capture_output=True, text=True).stdout, before)
        for bad in ("32", "2000000"):
            self.assertEqual(self.watch("--cap", bad, "--", "true")[0], 2)

    def test_watch_refuses_inside_a_run(self):
        with patch.dict(os.environ, {vramproof.runlock.ENV: json.dumps(
                {"slots": 2, "gpu_mib": 1024, "games": 1, "tag": "s00", "resources": [],
                 "path": str(self.lock), "cap_mib": 384})}):
            code, out = self.watch("--", "true")
        self.assertEqual(code, 2)
        self.assertFalse((out / "report.json").exists())


class Instances(unittest.TestCase):
    """vramproof looks for its games where tacli makes them, wherever it runs from."""

    def where(self, tacli=None, env=None):
        """vramproof's INSTANCES in a fresh process, reading `tacli` in place of its own."""
        code = (f"import sys; sys.path.insert(0, {str(vramproof.HERE)!r}); import vramproof; "
                + (f"from pathlib import Path; vramproof.TACLI = Path({str(tacli)!r}); "
                   f"print(vramproof.tacli_instances())" if tacli else "print(vramproof.INSTANCES)"))
        e = {k: v for k, v in os.environ.items() if k != "TACLI_ROOT"}
        e.update(env or {})
        r = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, env=e,
                           timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        return Path(r.stdout.strip().splitlines()[-1])

    def test_tacli_root_moves_both(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self.where(env={"TACLI_ROOT": d}), Path(d).resolve() / "tagpu" / "instances")

    def test_from_a_linked_worktree_the_instances_are_the_main_checkouts(self):
        with tempfile.TemporaryDirectory() as d:
            main = Path(d) / "main"
            for sub in ("tagpu/gamedir", "pristine"):
                (main / sub).mkdir(parents=True)
            git = ["git", "-c", "user.name=t", "-c", "user.email=t@t", "-C", str(main)]
            subprocess.run(git[:5] + ["init", "-q", str(main)], check=True)
            subprocess.run(git + ["commit", "-q", "--allow-empty", "-m", "x"], check=True)
            wt = main / ".claude" / "worktrees" / "scratch"
            subprocess.run(git + ["worktree", "add", "-q", str(wt)], check=True, capture_output=True)
            for sub in ("tagpu", "pristine", "tools"):
                (wt / sub).mkdir()
            shutil.copy(vramproof.TACLI, wt / "tools" / "tacli")
            self.assertEqual(self.where(wt / "tools" / "tacli"), main.resolve() / "tagpu" / "instances")


if __name__ == "__main__":
    unittest.main()
