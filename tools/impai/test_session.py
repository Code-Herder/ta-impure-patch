#!/usr/bin/env python3
"""test_session -- the session tier offline, on a fake player: the games (the situations and the confirm set as
tracked, its unmapped openings among them), the champion's games in a folder named by its DLL's content and
played once, a game its folder lacks played alone, each verdict word, an opening's word by its figures, --neutral,
--only, the exit codes, a game that could not run played once more and then said so, a folder of another DLL
refused, and a piece's command. No game runs.

    python3 tools/impai/test_session.py"""
import argparse
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bar  # noqa: E402
import diag  # noqa: E402
import exploit  # noqa: E402
import opening  # noqa: E402
import session  # noqa: E402

LOG = "\n".join([
    "impai: seam 0x465031 in; seats at 0x74E6C3D4",
    "impai: reach classes=15 us={us}",
    "impai: pulse seat=1 t=7 built=1 army=0",
    "impai: mil seat=1 t=2400 attack={attack} raid=0",
    "impai: tail seat=1 over_us=1000 worst_us={us} gametime=3000",
    "impai: eco seat=1 decisions=40 reclaims=0 gametime=4000",
])
PLANT = "impai: plant seat={seat} gametime={t} want=first T=1 reach=ffffc86f chose=ARMVP s=0.521 next=none s=none"
WIN, CAP_AHEAD, CAP_BEHIND, LOSS = ("A", 30000), ("cap", (1200.0, 1000.0)), ("cap", (900.0, 1000.0)), ("B", 20000)
OPENINGS = ("mapped-steeljungle-2", "mapped-troutfarm-2", "mapped-luschaven-3", "unmapped-luschaven-3")


class Fake:
    """A piece on a fake player: admitted at once, and at its first poll it writes each of its games as bar's
    lanes would -- the result test.result(role, id) gives, ImpAI's seat holding test.alive(role, id) units, and a
    log with test.attack(role, id) in its one line of decision, the wall-clock fields never twice the same, and
    a plant line at test.plant(role, id) when it names a GameTime -- and ends."""

    def __init__(self, test, piece):
        self.t, self.p, self.rc = test, piece, None
        test.launched.append(piece["name"])

    def admitted(self):
        return True

    def poll(self):
        if self.rc is None:
            self.p["out"].mkdir(parents=True, exist_ok=True)
            for i in self.p["items"]:
                g, role = i["game"], self.p["role"]
                self.t.played.append((role, i["id"]))
                outcome, how = self.t.result(role, i["id"])
                if outcome == "cnr" and self.t.played.count((role, i["id"])) > self.t.fails:
                    outcome, how = WIN
                r = {"id": i["id"], "outcome": outcome, "setup": i["setup"], "cap": g["cap"], "renderer": "gdi",
                     "pacing": "nodraw", "seed": g["seed"]}
                if outcome == "cnr":
                    r.update(outcome="could_not_run", reason="the game did not launch")
                elif outcome == "cap":
                    r.update(end_tick=g["cap"], value={"A": {"live": how[0], "t": g["cap"]},
                                                       "B": {"live": how[1], "t": g["cap"]}})
                else:
                    r["end_tick"] = how
                r["looks"] = [{"at": r.get("end_tick"), "seats": {
                    "A": {"alive": self.t.alive(role, i["id"]), "lost": int(outcome == "B")},
                    "B": {"alive": 3, "lost": 0}}}]
                if i["opening"]:
                    r["seats"] = {s: {"seat": k} for s, k in g["seat"].items()}
                (self.p["out"] / f"{i['id']}.json").write_text(json.dumps(r))
                self.t.clock += 1
                log = LOG.format(us=self.t.clock, attack=self.t.attack(role, i["id"]))
                if self.t.plant(role, i["id"]):
                    log = PLANT.format(seat=g["seat"]["A"], t=self.t.plant(role, i["id"])) + "\n" + log
                (self.p["out"] / f"{i['id']}.tagpu.log").write_text(log)
            self.rc = 0
        return self.rc

    def signal(self, sig):
        self.t.signals.append((self.p["name"], sig))


class TestSession(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(__import__("shutil").rmtree, self.tmp, True)
        self.launched, self.played, self.signals = [], [], []
        self.result = lambda role, gid: WIN
        self.attack = lambda role, gid: 1
        self.alive = lambda role, gid: 5
        self.plant = lambda role, gid: None
        self.fails, self.clock = 1, 0
        self.dlls = {}
        for name in ("champion", "candidate", "other"):
            self.dlls[name] = self.tmp / f"{name}.dll"
            self.dlls[name].write_bytes(name.encode())

    def run_(self, tag="t1", base="champion", test="candidate", **kw):
        a = argparse.Namespace(**dict({"base": str(self.dlls[base]), "test": str(self.dlls[test]), "tag": tag,
                                       "out": str(self.tmp / "out" / tag), "only": None, "neutral": False,
                                       "lanes": session.LANES, "tree": []}, **kw))
        with mock.patch.dict("os.environ", {"TARUN_DIR": str(self.tmp / "runs")}), \
                mock.patch("builtins.print") as said:
            rc = session.run(a, launch=lambda p: Fake(self, p), cache=self.tmp / "cache", sleep=lambda s: None)
        self.said = "\n".join(str(c.args[0]) for c in said.call_args_list)
        return rc

    def word(self, gid):
        return next(ln.split()[0] for ln in self.said.splitlines() if ln.startswith("  ") and f" {gid} " in ln)

    def test_the_games_are_the_situations_and_the_confirm_set(self):
        its = session.items([], session.CONFIRM, self.tmp / "s")
        self.assertEqual([i["id"] for i in its][:4], [f"exploit-{n}" for n in exploit.SITUATIONS])
        self.assertEqual([(i["id"], i["setup"]) for i in its][4:],
                         [("bar-hota-redtriangle-ac-1", None), ("bar-hota-troutfarm-ca-1", None),
                          ("bar-hota-plainspasses-ac-1", None), ("bar-hota-steeljungle-ca-1", None),
                          ("bar-nota-lavamania-ca-2", "prota-4.8"), ("bar-hota-coreprime-cc-1", None),
                          ("bar-hota-metalheck-aa-2", None)] + [(g, None) for g in OPENINGS])
        self.assertEqual(its[8]["name"], "bar-nota-lavamania-ca-2 on prota-4.8")
        self.assertEqual(its[8]["game"]["setup"], "prota-4.8")
        # a mapped opening is its unmapped twin's game, seed included (opening.py, THE MAPPED TWIN)
        self.assertTrue(all(i["game"]["seed"] == bar.game_seed(re.sub(r"^mapped-", "unmapped-", i["id"])) for i in its))
        self.assertEqual([i["id"] for i in its if i["opening"]], list(OPENINGS))
        self.assertEqual([(i["game"]["cap"], bar.seat_of(i["game"])["A"]) for i in its[11:]],
                         [(opening.CAP, 2), (opening.CAP, 2), (opening.CAP, 3), (opening.CAP, 3)])
        # every other game of the tier is mapped, as the bar's template is; an opening is not (test_opening)
        self.assertIs(json.loads(bar.TEMPLATE.read_text())["setup"]["mapping"], True)
        self.assertTrue(all(i["game"]["doc"]["setup"]["mapping"] for i in its if i["situation"]))

    def test_an_opening_on_a_setup_or_of_no_map_is_refused(self):
        for line, why in (("unmapped-steeljungle-2 prota-4.8", "an opening is a game of the base game"),
                          ("unmapped-nowhere-2", "not an opening's id"),
                          ("bar-hota-nowhere-ca-1", "not a bar game id")):
            (self.tmp / "c.txt").write_text(line + "\n")
            self.assertEqual(self.run_(confirm=str(self.tmp / "c.txt")), 2)
            self.assertIn(why, self.said)
        self.assertEqual(self.launched, [])

    def test_a_piece_a_setup_and_the_short_situations_free_the_lanes(self):
        its = session.items([], session.CONFIRM, self.tmp / "s")
        ps = session.pieces("test", self.tmp / "t", self.dlls["candidate"], its, 6)
        self.assertEqual([(p["name"], p["setup"], p["lanes"], len(p["ids"])) for p in ps],
                         [("test-base", None, 6, 14), ("test-prota-4.8", "prota-4.8", 1, 1)])
        order = [i["id"] for i in ps[0]["items"]]
        self.assertTrue(all(g.startswith("bar-") for g in order[:6]))
        self.assertEqual(order[6:11], ["exploit-commander-rush", *OPENINGS])    # the shortest caps
        self.assertEqual(ps[0]["out"], self.tmp / "t" / "base")
        self.assertEqual(ps[1]["out"], self.tmp / "t" / "prota-4.8")
        cmd = session.piece_command(ps[1], self.dlls["candidate"])
        self.assertEqual(cmd[2:], ["piece", "--dll", str(self.dlls["candidate"]), "--folder",
                                   str(self.tmp / "t" / "prota-4.8"), "--scenarios", str(self.tmp / "t" / "scenarios"),
                                   "--lanes", "1", "--ids", "bar-nota-lavamania-ca-2", "--setup", "prota-4.8"])
        cmd = session.piece_command(ps[0], self.dlls["candidate"])
        self.assertEqual(cmd[cmd.index("--situations") + 1].split(",")[0], "commander-rush")
        self.assertEqual(cmd[cmd.index("--openings") + 1], ",".join(OPENINGS))
        self.assertEqual(cmd[cmd.index("--ids") + 1].count("bar-"), 6)
        self.assertNotIn("mapped", cmd[cmd.index("--ids") + 1])
        self.assertNotIn("--setup", cmd)

    def test_a_piece_plays_its_games_as_diag_plays(self):
        played = []

        def play(games, pa, out):
            played.append(([g["id"] for g in games], pa.dll, pa.lanes, pa.setup, pa.gpu, pa.draw, pa.keep_all, out))
            return 0
        starts = {n: (100 * n, 100 * n) for n in range(1, 11)}
        with mock.patch.object(bar, "play_games", play), \
                mock.patch.object(bar, "start_positions", lambda name, seats=3, vfs=None: starts):
            rc = session.main(["piece", "--dll", "d.dll", "--folder", str(self.tmp / "f"), "--scenarios",
                               str(self.tmp / "s"), "--lanes", "2", "--ids", "bar-hota-troutfarm-ca-1",
                               "--situations", "air-rush,commander-rush", "--openings", "unmapped-lavaalley-3"])
        self.assertEqual(rc, 0)
        # the bar game, then the others from the shortest cap
        self.assertEqual(played, [(["bar-hota-troutfarm-ca-1", "exploit-commander-rush", "unmapped-lavaalley-3",
                                    "exploit-air-rush"], "d.dll", 2, None, False, False, True, self.tmp / "f")])
        self.assertTrue((self.tmp / "s" / "exploit-air-rush.json").exists())
        doc = json.loads((self.tmp / "s" / "unmapped-lavaalley-3.json").read_text())
        self.assertIs(doc["setup"]["mapping"], False)
        self.assertEqual(doc["units"][-1], {"id": "cmd3", "type": "ARMCOM", "owner": 3, "pos": [400, 400],
                                            "facing": 0})

    def test_every_game_same_and_the_champions_games_played_once(self):
        self.assertEqual(self.run_(), 0)
        self.assertEqual(sorted(self.launched), ["base-base", "base-prota-4.8", "test-base", "test-prota-4.8"])
        self.assertEqual(len(self.played), 30)
        self.assertIn("session: PASS: 15 SAME", self.said)
        self.assertIn("the champion's games", self.said)
        self.assertIn("0 of 15 already played, 15 to play", self.said)
        base = [d for d in (self.tmp / "cache").iterdir()]
        self.assertEqual([d.name for d in base], [f"base-{session.digest(self.dlls['champion'])[:12]}"])
        self.assertEqual((base[0] / "dll" / "base.dll").read_bytes(), b"champion")
        self.assertTrue((base[0] / "base" / "exploit-air-rush.json").exists())
        self.assertTrue((base[0] / "prota-4.8" / "bar-nota-lavamania-ca-2.json").exists())
        self.assertTrue((self.tmp / "out" / "t1" / "base" / "bar-hota-troutfarm-ca-1.json").exists())
        doc = json.loads((self.tmp / "out" / "t1" / "summary.json").read_text())
        self.assertEqual((doc["verdict"], len(doc["games"]), doc["games"][0]["word"]), ("pass", 15, "SAME"))
        # the same session again plays nothing
        self.launched, self.played = [], []
        self.assertEqual(self.run_(), 0)
        self.assertEqual(self.launched, [])
        self.assertIn("the champion's games (" + base[0].name + "): 15 of 15 already played, 0 to play", self.said)
        self.assertIn("the candidate's games (t1): 15 of 15 already played, 0 to play", self.said)
        # another candidate against the same champion plays its own games alone
        self.assertEqual(self.run_(tag="t2", test="other"), 0)
        self.assertEqual(sorted(self.launched), ["test-base", "test-prota-4.8"])
        # another champion is another folder, and its games are played
        self.launched = []
        self.assertEqual(self.run_(tag="t3", base="other"), 0)
        self.assertEqual(sorted(self.launched), ["base-base", "base-prota-4.8", "test-base", "test-prota-4.8"])
        self.assertEqual(len(list((self.tmp / "cache").iterdir())), 2)

    def test_a_champions_folder_that_lacks_a_game_plays_that_game_alone(self):
        nine = self.tmp / "nine.txt"
        nine.write_text("".join(ln + "\n" for ln in session.CONFIRM.read_text().splitlines()
                                if not opening.is_id(ln.split(" ")[0])))
        self.assertEqual(self.run_(confirm=str(nine)), 0)
        self.assertIn("session: PASS: 11 SAME", self.said)
        self.launched, self.played = [], []
        # the confirm set has gained the openings since: each folder plays those, and the nine are read
        self.assertEqual(self.run_(), 0)
        self.assertEqual(sorted(self.launched), ["base-base", "test-base"])
        self.assertEqual(sorted(self.played), sorted((r, g) for r in ("base", "test") for g in OPENINGS))
        self.assertIn("the champion's games (base-" + session.digest(self.dlls["champion"])[:12]
                      + "): 11 of 15 already played, 4 to play", self.said)
        self.assertIn("session: PASS: 15 SAME", self.said)

    def test_an_openings_word_is_its_figures(self):
        sj, tf, la = three = OPENINGS[:3]
        # the test alone orders a plant and holds more units; the base alone does; a plant at another tick
        plants = {("test", sj): 1949, ("base", tf): 374, ("base", la): 44, ("test", la): 60}
        self.plant = lambda role, gid: plants.get((role, gid))
        self.alive = lambda role, gid: {("test", sj): 11, ("base", tf): 12}.get((role, gid), 1)
        self.assertEqual(self.run_(only=",".join(three)), 1)
        self.assertEqual([self.word(g) for g in three], ["BETTER", "WORSE", "PARTED"])
        line = next(ln for ln in self.said.splitlines() if f" {sj} " in ln)
        self.assertIn("base pass: ImpAI holds units at 30000 | test pass: ImpAI holds units at 30000; first parting "
                      "at GameTime 7; by its figures, the test alone gives a plant; units at the cap 1 | 11; "
                      "figures base order ?; plant none; extractor ?; at the cap ? extractors, 1 units, metal ? | "
                      "test order ?; plant ARMVP 1949; extractor ?; at the cap ? extractors, 11 units, metal ?", line)
        self.assertIn("by its figures, the same milestones, the units within the margin",
                      next(ln for ln in self.said.splitlines() if f" {la} " in ln))
        doc = json.loads((self.tmp / "out" / "t1" / "summary.json").read_text())
        self.assertEqual([(g["word"], g["why"]) for g in doc["games"]][1],
                         ("WORSE", "the base alone gives a plant; units at the cap 12 | 1"))
        self.assertEqual(doc["games"][0]["figures"]["test"]["plant"], [1949, "ARMVP"])
        # a seat lost is the category's word, whatever the figures; two equal games are SAME
        self.result = lambda role, gid: LOSS if (role, gid) == ("test", sj) else WIN
        self.assertEqual(self.run_(tag="t2", test="other", only=sj), 1)
        self.assertEqual(self.word(sj), "WORSE")
        self.assertNotIn("by its figures", self.said)
        self.plant = lambda role, gid: 374
        self.result = lambda role, gid: WIN
        self.assertEqual(self.run_(tag="t3", base="other", only=sj, neutral=True), 0)
        self.assertEqual(self.word(sj), "SAME")

    def test_each_verdict_word(self):
        better, worse, parted, ends = ("bar-hota-redtriangle-ac-1", "bar-hota-troutfarm-ca-1",
                                       "bar-hota-plainspasses-ac-1", "bar-hota-steeljungle-ca-1")
        table = {("base", better): CAP_BEHIND, ("test", better): CAP_AHEAD,
                 ("base", worse): WIN, ("test", worse): CAP_AHEAD,
                 ("test", ends): ("A", 31000),
                 ("base", "exploit-naval-turtle"): WIN, ("test", "exploit-naval-turtle"): ("cap", (5.0, 9.0)),
                 ("base", "exploit-air-rush"): LOSS, ("test", "exploit-air-rush"): ("cap", (5.0, 9.0))}
        self.result = lambda role, gid: table.get((role, gid), WIN)
        self.attack = lambda role, gid: 0 if (role, gid) == ("test", parted) else 1
        self.assertEqual(self.run_(), 1)
        self.assertEqual([self.word(g) for g in (better, worse, parted, ends)],
                         ["BETTER", "WORSE", "PARTED", "PARTED"])
        self.assertEqual(self.word("exploit-naval-turtle"), "WORSE")     # kills: the opponent gone, then not
        self.assertEqual(self.word("exploit-air-rush"), "BETTER")        # commander: lost, then alive at the cap
        self.assertEqual(self.word("exploit-commander-rush"), "SAME")
        text = (self.tmp / "out" / "t1" / "summary.txt").read_text()
        self.assertRegex(text, rf"WORSE +{worse} +base won at 30000 \| test reached the cap ahead, value A/B 1.20; "
                               rf"no decision line differs\n")
        self.assertRegex(text, rf"PARTED +{parted} +base won at 30000 \| test won at 30000; "
                               rf"first parting at GameTime 2400\n")
        # another end tick in one category is reported, not judged
        self.assertRegex(text, rf"PARTED +{ends} +base won at 30000 \| test won at 31000; no decision line differs")
        self.assertIn("session: FAIL: 9 SAME, 2 BETTER, 2 PARTED, 2 WORSE", text)
        self.assertNotIn("impai: mil", text)                              # the lines are --neutral's

    def test_a_situations_line_carries_both_dlls_figures(self):
        self.assertEqual(self.run_(only="naval-turtle,bar-hota-troutfarm-ca-1"), 0)
        turtle = next(ln for ln in self.said.splitlines() if " exploit-naval-turtle " in ln)
        self.assertTrue(turtle.endswith("; figures base opp ?; attack ?; army ? | test opp ?; attack ?; army ?"))
        self.assertNotIn("figures", next(ln for ln in self.said.splitlines() if " bar-hota-troutfarm-ca-1 " in ln))
        # the figures are the result's, the log's and the recording's, each DLL's own, and the word is not theirs
        item = session.items(["naval-turtle"], session.CONFIRM, self.tmp / "scenarios")[0]
        roots = {"base": self.tmp / "fb", "test": self.tmp / "ft"}
        for role, (first, attacks, army) in (("base", (6960, 3, 4)), ("test", (7260, 5, 6))):
            (roots[role] / "base").mkdir(parents=True)
            r = {"id": item["id"], "outcome": "cap", "end_tick": 20000, "cap": 20000,
                 "seats": {"A": {"seat": 1}, "B": {"seat": 2}}, "first": {"A": {"attack": first}},
                 "end": {"A": {"attacks": attacks}}, "looks": [{"at": 20000, "seats": {"A": {"lost": 0}}}]}
            (roots[role] / "base" / f"{item['id']}.json").write_text(json.dumps(r))
            (roots[role] / "base" / f"{item['id']}.tagpu.log").write_text(
                f"impai: mil seat=1 t=2400 attack=1 raid=0\nimpai: mil seat=1 level=2 attacks=3 army_max={army}/512\n")
        row = session.compare(item, roots["base"], roots["test"])
        self.assertEqual(row["word"], "SAME")                              # other figures, and no word reads them
        self.assertTrue(session.line(row, 0).endswith(
            "; figures base opp ?; attack 6960 x3; army 4 | test opp ?; attack 7260 x5; army 6"))

    def test_state_makes_neutral_a_proof_of_the_brains_whole_state(self):
        parted = {"exploit-air-rush": ((4321, "mil.naval", "0", "1"),
                                       "state parts in the step from GameTime 4320: mil.naval 0 | 1"),
                  "bar-hota-troutfarm-ca-1": (False, "state not compared: x.imprec: no such file")}
        asked = []

        def fake(rec_base, rec_test, trees):
            asked.append((rec_base.parent.parent.name, rec_test.parent.parent.name, rec_base.name, trees))
            return parted.get(rec_base.name[:-len(".imprec")], (None, "state equal at every tick to 100"))
        with mock.patch.object(session, "state_parting", fake):
            self.assertEqual(self.run_(neutral=True, state=True, tree=["/src/other"]), 1)
        self.assertEqual(len(asked), 15)                                  # every game, the bar's too
        self.assertTrue(asked[0][0].startswith("base-") and asked[0][1] == "t1" and asked[0][3] == ["/src/other"])
        # equal logs and results, and the state parts: not SAME; a state that cannot be compared proves nothing
        self.assertEqual(self.word("exploit-air-rush"), "PARTED")
        self.assertIn("; state parts in the step from GameTime 4320: mil.naval 0 | 1", self.said)
        self.assertEqual(self.word("bar-hota-troutfarm-ca-1"), "UNRUN")
        self.assertEqual(self.word("exploit-naval-turtle"), "SAME")
        self.assertIn("; state equal at every tick to 100", self.said)
        self.assertIn("session: FAIL: 13 SAME, 1 PARTED, 1 UNRUN", self.said)
        doc = json.loads((self.tmp / "out" / "t1" / "summary.json").read_text())
        self.assertEqual(doc["games"][0]["state"], parted["exploit-air-rush"][1])
        # without --state no recording is read
        asked.clear()
        with mock.patch.object(session, "state_parting", fake):
            self.assertEqual(self.run_(tag="t2", test="other", neutral=True), 0)
        self.assertEqual(asked, [])
        # the recordings that cannot be read are said, not raised
        self.assertEqual(session.state_parting(self.tmp / "no-a.imprec", self.tmp / "no-b.imprec", [])[0], False)

    def test_better_and_parted_pass_and_wall_clock_lines_part_nothing(self):
        self.result = lambda role, gid: CAP_AHEAD if (role, gid) == ("base", "bar-hota-troutfarm-ca-1") else WIN
        self.attack = lambda role, gid: 2 if (role, gid) == ("test", "exploit-air-rush") else 1
        self.assertEqual(self.run_(), 0)
        self.assertIn("session: PASS: 13 SAME, 1 BETTER, 1 PARTED", self.said)

    def test_neutral_fails_on_a_game_not_same_and_names_its_first_parting(self):
        self.assertEqual(self.run_(neutral=True), 0)
        self.attack = lambda role, gid: 0 if (role, gid) == ("test", "exploit-air-rush") else 1
        self.assertEqual(self.run_(tag="t2", test="other", neutral=True), 1)
        lines = self.said.splitlines()
        k = next(n for n, ln in enumerate(lines) if ln.startswith("  PARTED  exploit-air-rush"))
        self.assertIn("first parting at GameTime 2400", lines[k])
        self.assertEqual(lines[k + 1], "      base: impai: mil seat=1 t=2400 attack=1 raid=0")
        self.assertEqual(lines[k + 2], "      test: impai: mil seat=1 t=2400 attack=0 raid=0")
        self.assertTrue(lines[k + 3].startswith("  SAME "))                # the lines are under that game alone
        self.assertIn("session: FAIL: 14 SAME, 1 PARTED", self.said)
        self.assertIn(", neutral;", lines[2])

    def test_only_narrows_to_the_named_games(self):
        self.assertEqual(self.run_(only="air-rush,bar-nota-lavamania-ca-2"), 0)
        self.assertEqual(sorted(self.played), sorted((r, g) for r in ("base", "test")
                                                     for g in ("exploit-air-rush", "bar-nota-lavamania-ca-2")))
        self.assertIn("session: PASS: 2 SAME", self.said)
        self.launched = []
        self.assertEqual(self.run_(only="exploit-air-rush"), 0)           # by its id too, and already played
        self.assertEqual(self.launched, [])
        self.assertEqual(self.run_(only="air-raid"), 2)
        self.assertIn("COULD NOT RUN: --only air-raid: no game of the session's", self.said)

    def test_a_game_that_could_not_run_is_played_once_more_then_said(self):
        self.result = lambda role, gid: ("cnr", None) if (role, gid) == ("test", "exploit-air-rush") else WIN
        self.assertEqual(self.run_(), 0)                                  # its second play ran
        self.assertEqual(self.launched.count("test-base"), 2)
        self.assertEqual(self.played.count(("test", "exploit-air-rush")), 2)
        self.assertEqual(self.played.count(("test", "bar-hota-troutfarm-ca-1")), 1)
        self.assertIn("played once more", self.said)
        self.fails = 9
        self.assertEqual(self.run_(tag="t2", test="other"), 2)
        self.assertRegex(self.said, r"UNRUN +exploit-air-rush +base pass: .* \| test could not run: the game did "
                                    r"not launch")
        self.assertIn("session: COULD NOT RUN: 14 SAME, 1 UNRUN", self.said)
        # a game WORSE fails the session whatever could not run
        self.result = lambda role, gid: {("test", "exploit-air-rush"): ("cnr", None),
                                         ("test", "bar-hota-troutfarm-ca-1"): LOSS}.get((role, gid), WIN)
        self.assertEqual(self.run_(tag="t3", test="other"), 1)

    def test_a_game_with_no_decision_in_its_log_is_not_same(self):
        self.assertEqual(self.run_(), 0)
        (self.tmp / "out" / "t1" / "base" / "exploit-air-rush.tagpu.log").unlink()
        self.assertEqual(self.run_(), 2)
        self.assertRegex(self.said, r"UNRUN +exploit-air-rush .*test pass: .*; no decision line in its log")

    def test_a_folder_of_another_dll_or_no_dll_is_refused(self):
        self.assertEqual(self.run_(), 0)
        self.launched = []
        self.assertEqual(self.run_(test="other"), 2)                      # the tag t1 names the candidate's build
        self.assertIn("holds the games of another DLL", self.said)
        self.dlls["none"] = self.tmp / "none.dll"
        self.assertEqual(self.run_(tag="t2", base="none"), 2)
        self.assertIn("no DLL", self.said)
        self.assertEqual(self.launched, [])

    def test_the_fake_log_s_decisions(self):
        a = diag.decisions(LOG.format(us=5, attack=1))
        self.assertEqual([ln.split()[1] for ln in a], ["pulse", "mil", "eco"])
        self.assertEqual(a, diag.decisions(LOG.format(us=77, attack=1)))


if __name__ == "__main__":
    unittest.main()
