#!/usr/bin/env python3
"""test_codeduel -- codeduel.py's judge on small fixture logs, offline: two peers' logs to a result, a
disagreement between the peers, a game at the cap, a level the engine ended, and the fingerprint.

    python3 tools/impai/test_codeduel.py

A fixture log is what a peer's tagpu.log holds of a game: its network seat line, its looks as the DLL logs a
`tacli peek` (one line a value), its pulse lines and its level-end lines. The host seats its human in record 0,
the joiner's in 1, seat A in 2 and seat B in 3; the joiner lists its own human first and the AI seats the other
way round, so a seat is found by its DirectPlay ID and not by its row."""
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bar  # noqa: E402
import codeduel as cd  # noqa: E402

DPID = {"human H": 0x100, "human J": 0x101, "A": 0x102, "B": 0x103}
ROWS = {"H": ("human H", "human J", "A", "B"), "J": ("human J", "human H", "B", "A")}
TEAM = {"human H": 0, "A": 0, "human J": 1, "B": 1}
START = {"human H": 0, "human J": 1, "A": 2, "B": 3}
OWN = {"H": ("human H", "A"), "J": ("human J", "B")}


def peek(off: int, val: int) -> str:
    return f"peek: *0x{cd.MAIN:X}+0x{off:X}:4 @0x0A000000 = {val} (0x{val:08X})\n"


def look(peer: str, t: int, alive: dict, lost=(), seating=False, gone=(), team=TEAM) -> str:
    """One look of `peer` at GameTime t: `alive` {seat: units}, humans 1; `gone` names the seats whose record
    is freed; `seating` adds the first look's fields."""
    out = peek(cd.GAMETIME, t)
    rows = ROWS[peer]
    for i, who in enumerate(rows):
        base = cd.REC + i * cd.STRIDE
        free = who in gone
        control = 0 if free else (1 if who.startswith("human") else 2) if who in OWN[peer] else 3
        vals = {0: 0 if free else 1, 4: 0xFFFFFFFF if free else DPID[who], 0x73: control,
                bar.ALIVE: alive.get(who, 1), bar.LOST: 1 if who in lost else 0}
        if seating:
            allies = sum(1 << (8 * j) for j, other in enumerate(rows) if team[other] == team[who])
            vals.update({0x13F: team[who], 0x108: allies, 0x147: START[who]})
        out += "".join(peek(base + off, v) for off, v in vals.items())
    return out


SEATS_LINE = ("impai: seats 2 (netai=impai, a network game, level 2) of 4 records; other peers' AI seats 3, "
              "ImpAI's 3\n")                # each peer's own AI seat is its record 2 and the other's its 3


def pulse(seat: int, t: int, vlive: int) -> str:
    return (f"impai: pulse seat={seat} t={t} built=3 mroots=0 eroots=2 army=0 squads=- metal=10 energy=100 "
            f"vlive={vlive} vbuilt={vlive}\n")


def level_end(seat: int, t: int) -> str:
    return (f"impai: calls seat={seat} n={t} gametime={t}\n"
            f"impai: handicap rows scaled/exempt gametime={t}: 1=0/0\n")


def log(peer: str, looks_: list, pulses=((7, 30000), (1807, 31000)), end=4000, extra="", turns=2, team=TEAM) -> str:
    """A peer's log: `looks_` is [(t, {seat: alive}, kwargs)], the first written with the seating fields."""
    text = "log: run 1 part 1 of tagpu.log\n" + extra + SEATS_LINE
    for k, (t, alive, kw) in enumerate(looks_):
        text += look(peer, t, alive, seating=not k, **({"team": team} if not k else {}), **kw)
        text += "".join(pulse(2, pt, v) for pt, v in pulses if (looks_[k - 1][0] if k else -1) < pt <= t)
    # a look after the teardown reads no count of the game's: the judge stops at the level end
    return text + level_end(turns, end) + look(peer, end, {"A": 0, "B": 0})


PLAYED = [(100, {"A": 1, "B": 1}, {}), (2000, {"A": 9, "B": 14}, {})]


class Looks(unittest.TestCase):
    def test_a_look_is_read_from_the_log_and_stops_at_the_level_end(self):
        lks = cd.looks(cd.play_text(log("J", PLAYED)))
        self.assertEqual([lk["t"] for lk in lks], [100, 2000])
        self.assertEqual(lks[0]["recs"][3], {"active": 1, "dpid": DPID["A"], "control": 3, "alive": 1, "lost": 0,
                                             "team": 0, "allies": 0x01000100, "start": 2})
        self.assertEqual(cd.record_of(lks[1], DPID["B"]), 2)
        self.assertEqual(lks[1]["recs"][2]["alive"], 14)

    def test_the_specs_a_look_asks_are_the_ones_the_judge_reads(self):
        specs = cd.look_specs([0, 3], seating=True)
        text = "".join(f"peek: {s} @0x0 = 1 \n" for s in specs)
        (lk,) = cd.looks(text)
        self.assertEqual(sorted(lk["recs"]), [0, 3])
        self.assertEqual(set(lk["recs"][3]), set(cd.PLAY.values()) | set(cd.SEATING.values()))


class FakePeers:
    """tacli's answers to a peek, in order: each a list of values, one a spec asked, or fewer."""

    def __init__(self, answers):
        self.answers, self.asked = list(answers), 0

    def tacli(self, verb, name, *args, timeout=None):
        specs = [a for a in args if a.startswith("*")]
        vals = self.answers[min(self.asked, len(self.answers) - 1)]
        self.asked += 1
        rows = [{"spec": s, "addr": 0, "value": f"{v} (0x{v:08X})"} for s, v in zip(specs, vals)]
        return "peeked\n" + json.dumps({"name": name, "peeks": rows})


class LiveLook(unittest.TestCase):
    FULL = [77] + [1, 0x102, 2, 9, 0]

    def test_a_look_is_read_back_as_the_log_holds_it(self):
        tp = FakePeers([self.FULL])
        self.assertEqual(cd.look(tp, "x-H", [2]),
                         {"t": 77, "recs": {2: {"active": 1, "dpid": 0x102, "control": 2, "alive": 9, "lost": 0}}})

    def test_a_short_answer_is_asked_again_and_three_give_the_game_up(self):
        tp = FakePeers([self.FULL[:3], self.FULL])
        self.assertEqual(cd.look(tp, "x-H", [2])["t"], 77)
        self.assertEqual(tp.asked, 2)
        from twopeers import CouldNotRun
        with self.assertRaises(CouldNotRun):
            cd.look(FakePeers([[]]), "x-H", [2])


class Judge(unittest.TestCase):
    def test_both_peers_see_a_gone_and_b_wins(self):
        h = log("H", PLAYED + [(3900, {"A": 0, "B": 30}, {"lost": ("A",)})])
        j = log("J", PLAYED + [(3950, {"A": 0, "B": 30}, {"lost": ("A",)})])
        r = cd.judge(h, j, 6000)
        self.assertEqual((r["outcome"], r["draw"], r["end_tick"], r["failed"]), ("B", False, 3900, []))
        self.assertEqual(r["seats"]["A"]["record"], {"H": 2, "J": 3})
        self.assertEqual(r["seats"]["B"]["record"], {"H": 3, "J": 2})
        self.assertEqual((r["seats"]["A"]["start"], r["seats"]["B"]["start"]), (2, 3))
        self.assertEqual(r["peers"]["J"]["gone"], {"A": 3950})
        self.assertEqual(r["peers"]["H"]["lost"], {"A": 3900})
        self.assertEqual(r["value"]["A"], {"live": 31000.0, "t": 1807})
        self.assertEqual(bar.POINTS[bar.outcome_of(r)[0]], 0.0)        # gauntlet.py's points are A's

    def test_a_seat_is_never_gone_before_it_held_a_unit(self):
        early = [(5, {"A": 0, "B": 0}, {})] + PLAYED + [(3900, {"A": 12, "B": 0}, {})]
        r = cd.judge(log("H", early), log("J", early), 6000)
        self.assertEqual((r["outcome"], r["end_tick"]), ("A", 3900))

    def test_the_peers_disagree(self):
        h = log("H", PLAYED + [(3900, {"A": 0, "B": 30}, {}), (5000, {"A": 0, "B": 31}, {})])
        j = log("J", PLAYED + [(3950, {"A": 2, "B": 30}, {}), (5050, {"A": 2, "B": 31}, {})])
        r = cd.judge(h, j, 6000)
        self.assertEqual(r["outcome"], "desync")
        self.assertIn("the host's records say B at GameTime 3900, the joiner's ended at GameTime 5050",
                      r["reason"])
        self.assertIsNone(bar.outcome_of(r)[0])

    def test_the_peers_disagree_on_the_winner_and_on_when(self):
        h = log("H", PLAYED + [(3900, {"A": 0, "B": 30}, {})])
        r = cd.judge(h, log("J", PLAYED + [(3950, {"A": 3, "B": 0}, {})]), 6000)
        self.assertEqual(r["outcome"], "desync")
        late = log("J", PLAYED + [(3950, {"A": 2, "B": 30}, {}), (3900 + cd.SLACK + 1, {"A": 0, "B": 30}, {})])
        r = cd.judge(h, late, 6000)
        self.assertEqual(r["outcome"], "desync")
        self.assertIn("on when", r["reason"])

    def test_a_game_at_the_cap(self):
        at = PLAYED + [(6010, {"A": 20, "B": 40}, {})]
        pulses = ((7, 30000), (1807, 31000), (5407, 40000))
        r = cd.judge(log("H", at, pulses, end=6100), log("J", at, ((7, 30000), (5399, 60000)), end=6100), 6000)
        self.assertEqual((r["outcome"], r["draw"], r["end_tick"]), ("cap", False, 6000))
        self.assertEqual(r["value"], {"A": {"live": 40000.0, "t": 5407}, "B": {"live": 60000.0, "t": 5399}})
        self.assertEqual(r["lead"], "B")
        # the pulses are the seats' own, before the cap: bar.py's value at the limit is not claimed
        self.assertIsNone(bar.outcome_of(r)[0])

    def test_both_gone_at_one_look_is_a_draw_at_the_cap(self):
        both = PLAYED + [(3900, {"A": 0, "B": 0}, {})]
        r = cd.judge(log("H", both), log("J", both), 6000)
        self.assertEqual((r["outcome"], r["draw"]), ("cap", True))
        self.assertEqual(bar.outcome_of(r)[0], "draw")

    def test_a_level_the_engine_ended_is_not_an_outcome(self):
        r = cd.judge(log("H", PLAYED), log("J", PLAYED), 6000)
        self.assertEqual(r["outcome"], "could not run")
        self.assertIn("its level ended with both AI seats holding units", r["reason"])

    def test_a_peer_that_left(self):
        h = log("H", PLAYED + [(3000, {"A": 9, "B": 14}, {"gone": ("B", "human J")})])
        r = cd.judge(h, log("J", PLAYED), 6000)
        self.assertEqual(r["outcome"], "could not run")
        self.assertIn("seat B's record is freed there", r["reason"])

    def test_turns_for_another_seat_fail_the_game(self):
        won = PLAYED + [(3900, {"A": 0, "B": 30}, {})]
        r = cd.judge(log("H", won), log("J", won, turns=3), 6000)
        self.assertEqual(r["outcome"], "could not run")
        self.assertIn("J: ImpAI's turns at its level end {3: 4000}", r["reason"])

    def test_the_seats_must_sit_on_their_humans_teams(self):
        won = PLAYED + [(3900, {"A": 0, "B": 30}, {})]
        none = dict.fromkeys(TEAM, cd.NO_TEAM)
        r = cd.judge(log("H", won, team=none), log("J", won), 6000)
        self.assertEqual(r["outcome"], "could not run")
        self.assertIn("H: the teams are", r["reason"])

    def test_no_network_seat_line(self):
        won = PLAYED + [(3900, {"A": 0, "B": 30}, {})]
        r = cd.judge(log("H", won).replace("netai=impai", "netai=ota"), log("J", won), 6000)
        self.assertEqual(r["outcome"], "could not run")
        self.assertIn("H: its level init read netai=ota", r["reason"])


class Fingerprint(unittest.TestCase):
    A = b"\x00impai: seats %s of %u records\x00oldfix: armed at the old site 0x%X\x00shared: a line of both %d\x00"
    B = b"\x00impai: seats %s of %u records\x00newfix: installed: the site 0x%X\x00shared: a line of both %d\x00"

    def test_the_stems_one_dll_holds_and_the_other_does_not(self):
        only = cd.only_stems(self.A, self.B)
        self.assertEqual(only, {"H": {"oldfix: armed at the old site 0x"}, "J": {"newfix: installed: the site 0x"}})
        self.assertEqual(cd.stem_lines("newfix: installed: the site 0x4655C0\nshared: a line of both 3\n", only["J"]), 1)
        self.assertEqual(cd.stem_lines("anything\n", set()), 0)

    def test_each_log_is_its_own_dlls(self):
        won = PLAYED + [(3900, {"A": 0, "B": 30}, {})]
        only = cd.only_stems(self.A, self.B)
        h, j = log("H", won), log("J", won, extra="newfix: installed: the site 0x4655C0\n")
        r = cd.judge(h, j, 6000, only)
        self.assertEqual(r["outcome"], "B")
        self.assertEqual(r["peers"]["J"]["fingerprint"], {"own": 1, "other": 0})
        self.assertEqual(r["peers"]["H"]["fingerprint"], {"own": 0, "other": 0})

    def test_a_peer_on_the_other_dll_fails(self):
        won = PLAYED + [(3900, {"A": 0, "B": 30}, {})]
        extra = "newfix: installed: the site 0x4655C0\n"
        r = cd.judge(log("H", won, extra=extra), log("J", won, extra=extra), 6000, cd.only_stems(self.A, self.B))
        self.assertEqual(r["outcome"], "could not run")
        self.assertIn("H: 1 lines of its log begin with a stem only the other peer's DLL holds", r["failed"])

    def test_two_dlls_nothing_tells_apart_fail(self):
        won = PLAYED + [(3900, {"A": 0, "B": 30}, {})]
        r = cd.judge(log("H", won), log("J", won), 6000, cd.only_stems(self.A, self.A))
        self.assertEqual(r["outcome"], "could not run")
        self.assertIn("nothing tells the builds apart", r["reason"])


class Maps(unittest.TestCase):
    def test_a_map_by_key_or_name(self):
        self.assertEqual(cd.map_of("lavamania"), ("lavamania", "Lava Mania"))
        self.assertEqual(cd.map_of("Acid Foursome"), ("acidfoursome", "Acid Foursome"))
        with self.assertRaises(ValueError):
            cd.map_of("Two Continents")


if __name__ == "__main__":
    unittest.main()
