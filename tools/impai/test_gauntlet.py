#!/usr/bin/env python3
"""test_gauntlet -- the champion gauntlet's verdicts, its run and resume with a fake player, bar.py's gauntlet pair
and its per-seat settings override, offline.

    python3 tools/impai/test_gauntlet.py

The fake player writes each piece's results as bar.py would, from a rule on the pool member a game is against (the
B settings of its gauntlet pair), so the run's members, its state, its resume and its refusals run as on games."""
import argparse
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bar  # noqa: E402
import gauntlet  # noqa: E402


class FakePlayer:
    """play(pieces, launch): a result for every id of every piece, A's outcome `rule(B's settings, gid)`: "A", "B" or
    "draw"; leaves out the ids in `lose`."""

    def __init__(self, rule):
        self.rule, self.played, self.lose = rule, [], set()

    def __call__(self, ps, launch):
        for p in ps:
            self.played.append(p["name"])
            Path(p["out"]).mkdir(parents=True, exist_ok=True)
            for gid in p["ids"]:
                if gid in self.lose:
                    continue
                _a, b = bar.gauntlet_rows(gid.split("-")[1])
                who = self.rule(b, gid)
                r = {"id": gid, "outcome": "cap" if who == "draw" else who, "draw": who == "draw"}
                (Path(p["out"]) / f"{gid}.json").write_text(json.dumps(r))
        return []


class Verdict(unittest.TestCase):
    def test_the_three_verdicts(self):
        self.assertEqual(gauntlet.member_verdict([1.0] * 20)["verdict"], "beats")
        self.assertEqual(gauntlet.member_verdict([0.0] * 20)["verdict"], "loses")
        self.assertEqual(gauntlet.member_verdict([1.0, 0.0] * 10)["verdict"], "even")
        self.assertEqual(gauntlet.member_verdict([0.5] * 20)["verdict"], "even")

    def test_equal_outcomes_keep_a_width(self):
        v = gauntlet.member_verdict([1.0] * 2)
        self.assertGreater(v["hi"] - v["lo"], 0.0)
        self.assertEqual(v["mean"], 1.0)
        # two wins are not enough to call it at 95 %; three are (a coin's spread: half = 1.645 x 0.5 / sqrt(n))
        self.assertEqual(v["verdict"], "even")
        self.assertEqual(gauntlet.member_verdict([1.0] * 3)["verdict"], "beats")
        # draws narrow nothing: twenty draws and a coin's twenty games give one width
        self.assertAlmostEqual(gauntlet.member_verdict([0.5] * 20)["hi"],
                               gauntlet.member_verdict([1.0, 0.0] * 10)["hi"])

    def test_overall_fails_on_any_loss(self):
        rc, line = gauntlet.overall({"L3": {"verdict": "beats"}, "L4": {"verdict": "even"}})
        self.assertEqual(rc, gauntlet.PASS)
        self.assertIn("beats L3; even with L4; loses to none", line)
        self.assertEqual(gauntlet.overall({"L3": {"verdict": "beats"}, "L4": {"verdict": "loses"}})[0],
                         gauntlet.FAIL)


class Run(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(__import__("shutil").rmtree, self.tmp, True)
        self.dll = self.tmp / "x.dll"
        self.dll.write_bytes(b"dll")

    def run_(self, player, cand="L5", pool="L0b,L3", blocks=2, dll=None):
        a = argparse.Namespace(out=str(self.tmp / "out"), dll=str(dll or self.dll), cand=cand, pool=pool,
                               blocks=blocks, lanes=10)
        with mock.patch.dict("os.environ", {"TARUN_DIR": str(self.tmp / "runs")}), \
                mock.patch("builtins.print") as said:
            rc = gauntlet.run(a, launch=lambda p: None, play=player)
        self.said = "\n".join(str(c.args[0]) for c in said.call_args_list)
        return rc

    def state(self):
        return json.loads((self.tmp / "out" / gauntlet.STATE).read_text())

    def test_a_loss_to_one_member_fails_the_gauntlet(self):
        # the candidate beats the harshest row and loses to L3 every game: the intransitive regression shows
        p = FakePlayer(lambda b, gid: "A" if b == "L0b" else "B")
        self.assertEqual(self.run_(p), gauntlet.FAIL)
        st = self.state()
        self.assertEqual({m: v["verdict"] for m, v in st["members"].items()}, {"L0b": "beats", "L3": "loses"})
        self.assertEqual(st["members"]["L0b"]["pair"], "g:L5.L0b")
        self.assertEqual(len(st["members"]["L3"]["ids"]), 20)       # two blocks of ten
        self.assertEqual(len(p.played), 4)                          # a piece a block, two members
        self.assertIn("loses to L3", self.said)

    def test_no_loss_passes(self):
        p = FakePlayer(lambda b, gid: "A" if b == "L0b" else "draw")
        self.assertEqual(self.run_(p), gauntlet.PASS)
        self.assertEqual(self.state()["members"]["L3"]["verdict"], "even")
        self.assertEqual(gauntlet.judge(self.tmp / "out"), gauntlet.PASS)

    def test_resume_plays_only_the_members_left(self):
        p = FakePlayer(lambda b, gid: "A")
        p.lose = {g for g in bar.ids("g:L5.L3", blocks=2)}
        self.assertEqual(self.run_(p), gauntlet.COULD_NOT)          # L3's games missing twice: could not judge
        self.assertEqual(list(self.state()["members"]), ["L0b"])
        p2 = FakePlayer(lambda b, gid: "A")
        self.assertEqual(self.run_(p2), gauntlet.PASS)
        self.assertEqual(p2.played, ["g:L5.L3-b1", "g:L5.L3-b2"])

    def test_another_gauntlet_in_the_directory_is_refused(self):
        p = FakePlayer(lambda b, gid: "A")
        self.assertEqual(self.run_(p), gauntlet.PASS)
        self.assertEqual(self.run_(p, cand="L4"), gauntlet.COULD_NOT)
        self.assertEqual(self.run_(p, pool="L0b"), gauntlet.COULD_NOT)
        other = self.tmp / "y.dll"
        other.write_bytes(b"other")
        self.assertEqual(self.run_(p, dll=other), gauntlet.COULD_NOT)

    def test_bad_settings_are_refused(self):
        p = FakePlayer(lambda b, gid: "A")
        self.assertEqual(self.run_(p, cand="L9"), gauntlet.COULD_NOT)
        self.assertEqual(self.run_(p, pool="L3,L3"), gauntlet.COULD_NOT)
        self.assertEqual(p.played, [])


class Bar(unittest.TestCase):
    def test_the_gauntlet_pair(self):
        diff, a, b = bar.pair_spec("g:L3.H")
        self.assertEqual((diff, a, b), ("hard", ("impai", "hard", bar.PERSONA_ROWS["L3"]), ("impai", "hard", None)))
        self.assertEqual(bar.gauntlet_pair("L5", "L0b"), "g:L5.L0b")
        self.assertTrue(bar.block_pair("g:L5.L0b") and bar.persona_pair("g:L5.L0b"))
        self.assertEqual(bar.run_blocks("g:L5.L0b"), bar.GAUNTLET_BLOCKS)
        self.assertEqual(bar.pool("g:L5.L0b"), bar.DEV_MAPS)
        g = bar.parse_id(bar.ids("g:L5.L0b")[0])
        self.assertEqual(g["seats"]["B"]["persona"], bar.PERSONA_ROWS["L0b"])
        for bad in ("g:L5", "g:L5.L9", "g:L5.L0b.L3", "g:"):
            with self.assertRaises(bar.BarError):
                bar.pair_spec(bad)

    def test_the_settings_override_keeps_the_id_and_seed(self):
        g = bar.parse_id(bar.ids("hota")[0])
        over = bar.with_settings(g, bar.settings_override(["A=L3"]))
        self.assertEqual((over["id"], over["seed"]), (g["id"], g["seed"]))
        self.assertEqual(over["seats"]["A"]["persona"], bar.PERSONA_ROWS["L3"])
        self.assertNotIn("persona", g["seats"]["A"])                 # the game given is not changed
        self.assertNotIn("persona", bar.with_settings(over, bar.settings_override(["A=H"]))["seats"]["A"])
        with self.assertRaises(bar.BarError):
            bar.with_settings(g, bar.settings_override(["B=L3"]))    # B is OTA AI in hota
        for bad in (["A=L9"], ["C=L3"], ["A=L3", "A=L4"]):
            with self.assertRaises(bar.BarError):
                bar.settings_override(bad)


if __name__ == "__main__":
    unittest.main()
