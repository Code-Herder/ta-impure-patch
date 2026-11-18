#!/usr/bin/env python3
"""test_gauntlet -- the champion gauntlet's verdicts, its run and resume with a fake player, bar.py's gauntlet pair
and its per-seat settings override, and the code gauntlet's duels with a fake duel player, offline.

    python3 tools/impai/test_gauntlet.py

The fake player writes each piece's results as bar.py would, from a rule on the pool member a game is against (the
B settings of its gauntlet pair), so the run's members, its state, its resume and its refusals run as on games.
The fake duel player writes each duel's result.json as codeduel.py would, from a rule on the game and its try."""
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


class FakeDuels:
    """play(games, launch): each game's result.json in its try's directory, `rule(game, try number)`: the
    result's keys as codeduel.py writes them, or None for a duel that left no result."""

    def __init__(self, rule):
        self.rule, self.played = rule, []

    def __call__(self, games, launch):
        for g in games:
            k = int(Path(g["dir"]).name[1:])
            self.played.append((g["id"], k))
            r = self.rule(g, k)
            Path(g["dir"]).mkdir(parents=True)          # as codeduel.py makes it: a try's directory is new
            if r is not None:
                (Path(g["dir"]) / "result.json").write_text(json.dumps(dict({"draw": False, "reason": "said"}, **r)))
        return []


def won(seat):
    return {"outcome": seat, "end_tick": 30000}


def at_cap(a, b, cap=600, t=None):
    t = cap if t is None else t
    return {"outcome": "cap", "cap": cap, "end_tick": cap,
            "value": {"A": {"live": a, "t": t}, "B": {"live": b, "t": t}}}


def cand_wins(g, k):
    return won("A" if g["role"] == "h" else "B")


class Code(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(__import__("shutil").rmtree, self.tmp, True)
        self.dlls = {}
        for name in ("new", "old1", "old2", "other"):
            self.dlls[name] = self.tmp / f"{name}.dll"
            self.dlls[name].write_bytes(name.encode())

    def run_(self, player, cand="new", pool=("old1", "old2"), games=4, maps="acidfoursome,luschaven", cap=600,
             no_humans=False):
        a = argparse.Namespace(out=str(self.tmp / "out"), code=True, cand=str(self.dlls[cand]),
                               pool=",".join(f"{n}={self.dlls[n]}" for n in pool), games=games, maps=maps, cap=cap,
                               no_humans=no_humans)
        with mock.patch.dict("os.environ", {"TARUN_DIR": str(self.tmp / "runs")}), \
                mock.patch("builtins.print") as said:
            rc = gauntlet.run_code(a, launch=lambda g: None, play=player)
        self.said = "\n".join(str(c.args[0]) for c in said.call_args_list)
        return rc

    def state(self):
        return json.loads((self.tmp / "out" / gauntlet.STATE).read_text())

    def test_the_games_split_the_peers_and_go_round_the_maps_and_sides(self):
        gs = gauntlet.duel_games("old1", 8, ["m1", "m2", "m3"], 600)
        self.assertEqual([g["role"] for g in gs], list("hjhjhjhj"))
        self.assertEqual([(g["map"], g["sides"]) for g in gs[::2]],
                         [("m1", "aa"), ("m2", "ac"), ("m3", "ca"), ("m1", "ac")])
        # every map meets every side pairing over four rounds, whatever the number of maps
        for maps in (["m1"], list(gauntlet.DUEL_MAPS), list(bar.DEV_MAPS)):
            n = 8 * len(maps)
            self.assertEqual(len({(g["map"], g["sides"]) for g in gauntlet.duel_games("old1", n, maps, 600)}),
                             4 * len(maps))
        self.assertEqual([(g["map"], g["sides"]) for g in gs[1::2]], [(g["map"], g["sides"]) for g in gs[::2]])
        self.assertEqual(len({g["id"] for g in gs}), 8)
        for n in (0, 1, 5):
            with self.assertRaises(gauntlet.CouldNotJudge):
                gauntlet.duel_games("old1", n, ["m1"], 600)

    def test_the_host_is_seat_a_and_the_sides_follow_the_candidate(self):
        dlls = {"new": Path("n.dll"), "old1": Path("o.dll")}
        g = dict(gauntlet.duel_games("old1", 4, ["lavamania"], 600)[2], dir=Path("d"))     # round 2: ac, host
        self.assertEqual((g["role"], g["sides"]), ("h", "ac"))
        cmd = gauntlet.duel_command(g, dlls, "new")
        self.assertEqual(cmd[cmd.index("--a") + 1:cmd.index("--a") + 4], ["n.dll", "--b", "o.dll"])
        self.assertEqual(cmd[cmd.index("--sides") + 1], "ac")
        cmd = gauntlet.duel_command(dict(g, role="j"), dlls, "new")
        self.assertEqual(cmd[cmd.index("--a") + 1:cmd.index("--a") + 4], ["o.dll", "--b", "n.dll"])
        self.assertEqual(cmd[cmd.index("--sides") + 1], "ca")       # the candidate is Arm on either peer
        self.assertEqual(cmd[cmd.index("--cap") + 1], "600")
        self.assertNotIn("--no-humans", cmd)
        gone = dict(gauntlet.duel_games("old1", 4, ["lavamania"], 600, no_humans=True)[2], dir=Path("d"))
        self.assertEqual(gauntlet.duel_command(gone, dlls, "new")[-1], "--no-humans")

    def test_a_directory_is_a_gauntlet_with_the_humans_or_without(self):
        self.assertEqual(self.run_(FakeDuels(cand_wins), pool=("old1",)), gauntlet.PASS)
        self.assertNotIn("no_humans", self.state())
        self.assertEqual(self.run_(FakeDuels(cand_wins), pool=("old1",), no_humans=True), gauntlet.COULD_NOT)
        self.assertIn("is another gauntlet (no_humans None)", self.said)
        __import__("shutil").rmtree(self.tmp / "out")
        self.assertEqual(self.run_(FakeDuels(cand_wins), pool=("old1",), no_humans=True), gauntlet.PASS)
        self.assertIs(self.state()["no_humans"], True)
        self.assertEqual(self.run_(FakeDuels(cand_wins), pool=("old1",)), gauntlet.COULD_NOT)
        self.assertIn("is another gauntlet (no_humans True)", self.said)

    def test_the_points_are_the_candidates_on_either_peer(self):
        self.assertEqual(self.run_(FakeDuels(cand_wins)), gauntlet.PASS)
        st = self.state()
        self.assertEqual({m: (v["verdict"], v["mean"], v["n"]) for m, v in st["members"].items()},
                         {"old1": ("beats", 1.0, 4), "old2": ("beats", 1.0, 4)})
        self.assertEqual([r["role"] for r in st["members"]["old1"]["rows"]], list("hjhj"))
        self.assertIn("4 won, 0 drawn, 0 lost", self.said)
        self.assertIn("as host 1.000, as joiner 1.000", self.said)

    def test_the_host_winning_every_game_is_even(self):
        self.assertEqual(self.run_(FakeDuels(lambda g, k: won("A")), pool=("old1",)), gauntlet.PASS)
        v = self.state()["members"]["old1"]
        self.assertEqual(([r["points"] for r in v["rows"]], v["verdict"]), ([1.0, 0.0, 1.0, 0.0], "even"))
        self.assertIn("as host 1.000, as joiner 0.000", self.said)

    def test_a_loss_to_one_member_fails(self):
        rule = lambda g, k: cand_wins(g, k) if g["member"] == "old1" else won("B" if g["role"] == "h" else "A")
        self.assertEqual(self.run_(FakeDuels(rule)), gauntlet.FAIL)
        self.assertEqual(self.state()["members"]["old2"]["verdict"], "loses")
        self.assertIn("loses to old2", self.said)

    def test_a_game_at_the_cap_scores_by_its_value(self):
        # seat A leads by the value at the cap in every game: the host's points, whoever the candidate is
        self.assertEqual(self.run_(FakeDuels(lambda g, k: at_cap(40000.0, 20000.0)), pool=("old1",)), gauntlet.PASS)
        v = self.state()["members"]["old1"]
        self.assertEqual([r["points"] for r in v["rows"]], [1.0, 0.0, 1.0, 0.0])
        self.assertEqual(v["rows"][0]["value"], {"A": 40000.0, "B": 20000.0})
        self.assertIn("4 at the cap, scored by value", self.said)
        self.assertIn("value A 40000, B 20000", self.said)

    def test_a_cap_with_no_lead_is_half_a_point(self):
        self.assertEqual(self.run_(FakeDuels(lambda g, k: at_cap(30000.0, 29000.0)), pool=("old1",)), gauntlet.PASS)
        self.assertEqual([r["points"] for r in self.state()["members"]["old1"]["rows"]], [0.5] * 4)

    def test_a_cap_with_no_value_at_the_cap_is_played_again(self):
        # a value read before the cap is not the cap's (bar.by_value): the game is unscored
        p = FakeDuels(lambda g, k: at_cap(40000.0, 20000.0, t=400) if k == 1 else at_cap(40000.0, 20000.0))
        self.assertEqual(self.run_(p, pool=("old1",)), gauntlet.PASS)
        self.assertEqual(len(p.played), 8)
        self.assertEqual({r["try"] for r in self.state()["members"]["old1"]["rows"]}, {"t2"})

    def test_a_desync_is_played_once_more(self):
        bad = {"outcome": "desync", "reason": "the peers disagree"}
        p = FakeDuels(lambda g, k: bad if (g["id"].startswith("old1-01h") and k == 1) else cand_wins(g, k))
        self.assertEqual(self.run_(p, pool=("old1",)), gauntlet.PASS)
        self.assertEqual([x for x in p.played if x[1] == 2], [("old1-01h-acidfoursome-aa", 2)])
        self.assertEqual(len(p.played), 5)
        rows = self.state()["members"]["old1"]["rows"]
        self.assertEqual([r["try"] for r in rows], ["t2", "t1", "t1", "t1"])
        self.assertIn("played once more (old1-01h-acidfoursome-aa: t1: desync: the peers disagree)", self.said)

    def test_a_game_with_an_unverified_check_counts_and_is_named(self):
        said = "J: its level ended at GameTime 3960 with no look of seat A gone"
        p = FakeDuels(lambda g, k: dict(cand_wins(g, k), unverified=[said] if g["id"].startswith("old1-01h") else []))
        self.assertEqual(self.run_(p, pool=("old1",)), gauntlet.PASS)
        self.assertEqual(len(p.played), 4)                      # no game is played again for it
        rows = self.state()["members"]["old1"]["rows"]
        self.assertEqual([r["unverified"] for r in rows], [[said], [], [], []])
        self.assertEqual(rows[0]["points"], 1.0)
        self.assertIn("; 1 counted with a check its evidence could not make, named below", self.said)
        self.assertIn(f"      unverified: {said}", self.said)

    def test_two_failures_are_could_not_judge_and_run_again_goes_on(self):
        lost = "old2-02j-luschaven-ac"
        p = FakeDuels(lambda g, k: (None if k == 1 else {"outcome": "could not run", "reason": "no lobby"})
                      if g["id"] == lost else cand_wins(g, k))
        self.assertEqual(self.run_(p), gauntlet.COULD_NOT)
        self.assertEqual(list(self.state()["members"]), ["old1"])
        self.assertEqual(p.played.count((lost, 1)) + p.played.count((lost, 2)), 2)
        self.assertEqual(len(p.played), 4 + 4 + 1)
        self.assertIn(f"COULD NOT JUDGE: new against old2 lacks 1 of its duels ({lost}: t2: could not run: no lobby)",
                      self.said)
        self.assertEqual(gauntlet.judge(self.tmp / "out"), gauntlet.COULD_NOT)
        # the run again: old1 is judged, old2's three scored games are kept, and the one lacking is played
        p2 = FakeDuels(cand_wins)
        self.assertEqual(self.run_(p2), gauntlet.PASS)
        self.assertEqual(p2.played, [(lost, 3)])
        self.assertEqual(self.state()["members"]["old2"]["rows"][3]["try"], "t3")
        with mock.patch("builtins.print"):
            self.assertEqual(gauntlet.judge(self.tmp / "out"), gauntlet.PASS)

    def test_another_gauntlet_in_the_directory_is_refused(self):
        p = FakeDuels(cand_wins)
        self.assertEqual(self.run_(p), gauntlet.PASS)
        n = len(p.played)
        self.assertEqual(self.run_(p, games=6), gauntlet.COULD_NOT)
        self.assertEqual(self.run_(p, pool=("old1",)), gauntlet.COULD_NOT)
        self.assertEqual(self.run_(p, cap=900), gauntlet.COULD_NOT)
        self.dlls["old1"].write_bytes(b"rebuilt")
        self.assertEqual(self.run_(p), gauntlet.COULD_NOT)
        self.assertIn("is another build", self.said)
        self.assertEqual(len(p.played), n)

    def test_bad_arguments_are_refused(self):
        p = FakeDuels(cand_wins)
        self.assertEqual(self.run_(p, games=3), gauntlet.COULD_NOT)
        self.assertEqual(self.run_(p, maps="Two Continents"), gauntlet.COULD_NOT)
        self.assertEqual(self.run_(p, pool=("old1", "old1")), gauntlet.COULD_NOT)
        self.dlls["other"].write_bytes(b"new")              # the candidate's build under another name
        self.assertEqual(self.run_(p, pool=("other",)), gauntlet.COULD_NOT)
        self.assertIn("are one build", self.said)
        self.assertEqual(p.played, [])
        self.assertEqual(gauntlet.named_dll("x/ddraw-1234.dll"), ("ddraw-1234", Path("x/ddraw-1234.dll")))
        self.assertEqual(gauntlet.named_dll("champ=x/d.dll"), ("champ", Path("x/d.dll")))


class Handle:
    """A duel's process, done once the clock passes `ends`; None never ends by itself."""

    def __init__(self, clock, ends):
        self.clock, self.ends, self.signals = clock, ends, []

    def admitted(self):
        return True

    def poll(self):
        return 0 if self.signals or (self.ends is not None and self.clock() >= self.ends) else None

    def signal(self, sig):
        self.signals.append(sig)


class PlayDuels(unittest.TestCase):
    def setUp(self):
        self.now = 0.0

    def clock(self):
        return self.now

    def sleep(self, s):
        self.now += s

    def test_no_more_than_at_once_and_in_order(self):
        most, order, made = [0], [], []

        def launch(g):
            h = Handle(self.clock, self.now + 100)
            order.append(g["id"])
            made.append(h)
            most[0] = max(most[0], sum(1 for x in made if x.poll() is None))
            return h
        games = [{"id": f"g{k}", "cap": 600} for k in range(7)]
        self.assertEqual(gauntlet.play_duels(games, launch, self.clock, self.sleep, at_once=3), [])
        self.assertEqual(order, [f"g{k}" for k in range(7)])
        self.assertEqual(most[0], 3)

    def test_a_game_past_its_deadline_is_stopped(self):
        hs = {}

        def launch(g):
            hs[g["id"]] = Handle(self.clock, 50 if g["id"] == "quick" else None)
            return hs[g["id"]]
        games = [{"id": "quick", "cap": 600}, {"id": "hung", "cap": 600}]
        self.assertEqual(gauntlet.play_duels(games, launch, self.clock, self.sleep), ["hung"])
        self.assertEqual(hs["hung"].signals, [signal.SIGTERM])
        self.assertEqual(hs["quick"].signals, [])
        self.assertGreater(self.now, gauntlet.duel_deadline(600))


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
