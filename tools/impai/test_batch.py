#!/usr/bin/env python3
"""test_batch -- the per-batch loop offline, on a fake player: the regression list as tracked, the gates' games
and their pieces (a game of two gates played once, the setups' pieces beside the base game's), every gate passed,
fail fast (a game lost stops every gating piece still playing, never the held-out one, and the margin test is not
played), a piece with a game that could not run played once more, a piece past its deadline stopped, the margin
test's verdicts, the held-out check reported or, with --gate-held-out, gating, and a directory of other DLLs
refused. No game runs.

    python3 tools/impai/test_batch.py"""
import argparse
import json
import signal
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bar  # noqa: E402
import batch  # noqa: E402
import margin  # noqa: E402


class Fake:
    """A piece on a fake player: it is admitted at once, and after `ends` polls writes each game's result as
    outcome(id, setup) gives it ("A", "B", "cap", "cnr" for a game that could not run on its first play) and ends.
    A SIGINT or SIGTERM before then writes every game as stopped and ends it."""

    def __init__(self, test, piece):
        self.t, self.p, self.polls, self.rc = test, piece, 0, None
        test.launched.append(piece["name"])
        test.played += [(g, piece["setup"]) for g in piece["ids"]]

    def admitted(self):
        return True

    def write(self, stopped=False):
        self.p["out"].mkdir(parents=True, exist_ok=True)
        for gid in self.p["ids"]:
            o = "could_not_run" if stopped else self.t.outcome(gid, self.p["setup"])
            if o == "cnr":
                o = "could_not_run" if self.t.played.count((gid, self.p["setup"])) == 1 else "A"
            r = {"id": gid, "outcome": o, "end_tick": 30000, "seed": bar.game_seed(gid)}
            if o == "cap":
                r["value"] = {"A": {"live": 900.0, "t": bar.CAP}, "B": {"live": 1000.0, "t": bar.CAP}}
            (self.p["out"] / f"{gid}.json").write_text(json.dumps(r))
        self.rc = 2 if stopped or any(self.t.outcome(g, self.p["setup"]) == "cnr" for g in self.p["ids"]) else 0

    def poll(self):
        if self.rc is None:
            self.polls += 1
            if self.polls >= self.t.ends(self.p):
                self.write()
        return self.rc

    def signal(self, sig):
        self.t.signals.append((self.p["name"], sig))
        if self.rc is None and sig in (signal.SIGINT, signal.SIGTERM):
            self.write(stopped=True)


class TestBatch(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(__import__("shutil").rmtree, self.tmp, True)
        self.launched, self.played, self.signals, self.margins = [], [], [], []
        self.outcome = lambda gid, setup: "A"
        self.ends = lambda piece: 1
        self.margin_rc = margin.ACCEPT
        self.now = 0.0

    def run_(self, **kw):
        dll = self.tmp / "x.dll"
        dll.write_bytes(b"dll")
        a = argparse.Namespace(out=str(self.tmp / "out"), base=str(dll), test=str(dll), mode="notworse",
                               margin_lanes=6, **dict({"gate_held_out": False}, **kw))

        def sleep(s):
            self.now += s

        def play_margin(a, out):
            self.margins.append(a.mode)
            return self.margin_rc
        with mock.patch.dict("os.environ", {"TARUN_DIR": str(self.tmp / "runs")}), \
                mock.patch("builtins.print") as said:
            rc = batch.run(a, launch=lambda p: Fake(self, p), play_margin=play_margin,
                           clock=lambda: self.now, sleep=sleep)
        self.said = "\n".join(str(c.args[0]) for c in said.call_args_list)
        return rc

    def test_the_regression_list_and_the_gates_games(self):
        rs = batch.regress()
        self.assertEqual(len(rs), 19)                                    # 14, three extras, ProTA's two
        self.assertEqual([g for g in rs if g[1]], [("bar-nota-lavamania-ca-1", "prota-4.8"),
                                                   ("bar-nota-pincushion-ac-2", "prota-4.8")])
        self.assertEqual(len(batch.HOTA), 40)
        self.assertEqual(batch.HOTA, [g for g in bar.ids("hota") if bar.parse_id(g)["rep"] in (1, 2)])
        bad = self.tmp / "r.txt"
        bad.write_text("bar-hota-pincushion-aa-1  # a comment\nbar-hota-pincushion-aa-1\n")
        with self.assertRaisesRegex(bar.BarError, "twice"):
            batch.regress(bad)
        bad.write_text("bar-hota-nowhere-aa-1\n")
        with self.assertRaises(bar.BarError):
            batch.regress(bad)

    def test_the_pieces_play_each_game_once_and_the_setups_beside_the_base_game(self):
        games = batch.gate_games()
        ps = batch.pieces(games, self.tmp)
        ids = [(g, p["setup"]) for p in ps for g in p["ids"]]
        self.assertEqual(len(ids), len(set(ids)))                        # a game of two gates is played once
        every = {g for k in ("regression", "hota", "mods", batch.HELD) for g in games[k]}
        self.assertEqual(set(ids), every)
        self.assertEqual(len(every), 19 + 40 + 12 + 8 - 9 - 2)          # 9 hota games and ProTA's 2 shared
        self.assertTrue(all(len(p["ids"]) <= p["lanes"] for p in ps))    # one wave a piece
        names = [p["name"] for p in ps]
        self.assertEqual(names[:6], ["base-1", "prota-4.8-1", "loader+tadr-ota-1", "392+tadr-2026.8.6-1",
                                     "base-2", batch.HELD])
        self.assertFalse(next(p for p in ps if p["name"] == batch.HELD)["gating"])

    def test_every_gate_passed(self):
        self.assertEqual(self.run_(), 0)
        self.assertEqual(self.margins, ["notworse"])
        self.assertEqual(sorted(self.played), sorted(set(self.played)))
        text = (self.tmp / "out" / "summary.txt").read_text()
        for line in ("regression PASS    19 of 19 won", "hota       PASS    40 of 40 won", "margin     PASS",
                     "mods       PASS    12 of 12 won", "held-out   DONE    reports, judges nothing",
                     "bar-p:L5-lavaalley-aa-8: won at 30000", "batch: PASS"):
            self.assertIn(line, text)
        self.assertEqual(json.loads((self.tmp / "out" / "summary.json").read_text())["verdict"], "pass")

    def test_a_game_lost_stops_every_gating_piece_and_the_margin_test(self):
        self.outcome = lambda gid, setup: "B" if gid == "bar-hota-troutfarm-ac-1" else "A"     # in base-2
        self.ends = lambda piece: 1 if piece["name"] == "base-2" else 3
        self.assertEqual(self.run_(), 1)
        self.assertEqual(self.margins, [])
        stopped = {n for n, s in self.signals if s == signal.SIGINT}
        self.assertNotIn(batch.HELD, stopped)
        self.assertNotIn("base-2", stopped)
        self.assertIn("base-1", stopped)
        text = (self.tmp / "out" / "summary.txt").read_text()
        self.assertIn("regression FAIL    18 of 19 won; not won: bar-hota-troutfarm-ac-1", text)
        self.assertIn("margin     SKIPPED", text)
        self.assertIn("UNRUN   stopped: regression failed", text)
        self.assertIn("bar-p:L5-lavaalley-aa-8: won", text)              # the held-out piece played on
        self.assertIn("batch: FAIL", text)

    def test_a_piece_with_a_game_that_could_not_run_is_played_once_more(self):
        self.outcome = lambda gid, setup: "cnr" if gid == "bar-nota-lavamania-ca-2" else "A"
        self.assertEqual(self.run_(), 0)
        self.assertEqual(self.launched.count("loader+tadr-ota-1"), 2)
        self.assertEqual(self.launched.count("base-1"), 1)

    def test_a_piece_past_its_deadline_is_stopped_and_its_gate_could_not_run(self):
        self.ends = lambda piece: 200 if piece["name"] == "base-7" else 1     # 1000 s after its admission
        self.assertEqual(self.run_(), 2)
        self.assertIn(("base-7", signal.SIGTERM), self.signals)
        self.assertEqual(self.margins, [])
        self.assertIn(f"past {batch.DEADLINE} s and stopped: base-7", (self.tmp / "out" / "summary.txt").read_text())

    def test_the_margin_tests_verdicts(self):
        for rc, want, line in ((margin.REJECT, 1, "FAIL    rejected"), (margin.UNDECIDED, 1, "no decision"),
                               (margin.COULD_NOT, 2, "UNRUN   could not judge")):
            self.setUp()
            self.margin_rc = rc
            self.assertEqual(self.run_(), want)
            self.assertIn(line, (self.tmp / "out" / "summary.txt").read_text())

    def test_the_held_out_check_reports_and_gates_only_when_asked(self):
        self.outcome = lambda gid, setup: "cap" if gid.startswith("bar-p:L5") else "A"
        self.assertEqual(self.run_(), 0)
        self.assertIn("bar-p:L5-steeljungle-ca-1: reached the cap not ahead, value A/B 0.90",
                      (self.tmp / "out" / "summary.txt").read_text())
        self.setUp()
        self.outcome = lambda gid, setup: "cap" if gid.startswith("bar-p:L5") else "A"
        self.assertEqual(self.run_(gate_held_out=True), 1)
        self.assertEqual(self.margins, [])

    def test_a_directory_of_other_dlls_is_refused(self):
        other = self.tmp / "y.dll"
        other.write_bytes(b"other")
        margin.fix_dlls(self.tmp / "out", other, other)
        self.assertEqual(self.run_(), 2)
        self.assertEqual(self.launched, [])


if __name__ == "__main__":
    unittest.main()
