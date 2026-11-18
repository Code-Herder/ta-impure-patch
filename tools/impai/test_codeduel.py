#!/usr/bin/env python3
"""test_codeduel -- codeduel.py's judge on small fixture logs, offline: two peers' logs to a result, a
disagreement between the peers, a game at the cap and its value, a level the engine ended, and the fingerprint;
and the cap's read of both peers' units on a fake game.

    python3 tools/impai/test_codeduel.py

A fixture log is what a peer's tagpu.log holds of a game: its network seat line, its looks as the DLL logs a
`tacli peek` (one line a value), its pulse lines and its level-end lines. The host seats its human in record 0,
the joiner's in 1, seat A in 2 and seat B in 3; the joiner lists its own human first and the AI seats the other
way round, so a seat is found by its DirectPlay ID and not by its row. The fake game (FakeGame) answers
`tacli peek`, `keys` and `units` from each peer's memory: its clock, its records and its unit blocks."""
import json
import struct
import sys
import unittest
from pathlib import Path
from unittest import mock

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
            allies = sum(1 << (8 * j) for j, other in enumerate(rows)
                         if other == who or team[other] == team[who] != cd.NO_TEAM)
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


COSTS = {1: (2500.0, 25000.0), 2: (100.0, 1200.0), 3: (50.0, 600.0)}    # a commander, two cheaper types
BLOCK0, SLOTS = 0x05000000, 300


def unit_bytes(owner: int, typ: int, left: float = 0.0, alive: bool = True) -> str:
    """A slot's peeked bytes, as the DLL logs an x read."""
    b = bytearray(cd.U_SPAN)
    struct.pack_into("<H", b, 0, typ)
    b[bar.UNIT_OWNER - cd.U_FROM] = owner
    struct.pack_into("<f", b, bar.UNIT_LEFT - cd.U_FROM, left)
    struct.pack_into("<I", b, cd.U_STATE - cd.U_FROM, cd.U_ALIVE if alive else 0x20)
    return " ".join(f"{x:02X}" for x in b)


class FakeGame:
    """Two peers at the cap. `units` is {peer: {who: [(type, fraction left) or None for a free slot]}} for the
    seats A and B; a peer's clock runs 30 ticks a GameTime peek unless the game is paused, which the Pause key
    toggles on the peers of `takes`."""

    def __init__(self, units, takes=("H", "J"), t=None):
        self.units, self.takes = units, takes
        self.t = dict(t or {"H": 6010, "J": 6090})
        self.paused, self.keys, self.peeks = False, [], 0

    def block(self, peer, who):
        first = BLOCK0 + ROWS[peer].index(who) * SLOTS * cd.UNIT
        return first, first + (SLOTS - 1) * cd.UNIT

    def answer(self, peer, sp):
        if sp == f"*0x{cd.MAIN:X}+0x{cd.GAMETIME:X}:4":
            if not self.paused:
                self.t[peer] += 30
            return str(self.t[peer])
        if sp.startswith("*"):
            i, off = divmod(int(sp.split("+0x")[1].split(":")[0], 16) - cd.REC, cd.STRIDE)
            if i >= len(ROWS[peer]):
                return "0"
            who = ROWS[peer][i]
            human = who.startswith("human")
            mine = self.units[peer].get(who, [(1, 0.0)])
            return str({0: 1, 4: DPID[who], 0x73: (1 if human else 2) if who in OWN[peer] else 3,
                        bar.ALIVE: sum(1 for u in mine if u), bar.LOST: 0, 0x13F: TEAM[who], 0x108: 0,
                        0x147: START[who], 0x67: self.block(peer, who)[0], 0x6B: self.block(peer, who)[1]}[off])
        slot, rest = divmod(int(sp.split(":")[0], 16) - cd.U_FROM - BLOCK0, cd.UNIT)
        assert rest == 0 and sp.endswith(f":x{cd.U_SPAN:X}"), sp
        who = ROWS[peer][slot // SLOTS]
        mine = self.units[peer].get(who, [])
        u = mine[slot % SLOTS] if slot % SLOTS < len(mine) else None
        return unit_bytes(ROWS[peer].index(who), *u) if u else unit_bytes(0, 0, alive=False)

    def tacli(self, verb, name, *args, timeout=None):
        peer = name[-1]
        if verb == "keys":
            self.keys.append((peer,) + args)
            if peer in self.takes:
                self.paused = not self.paused
            return ""
        if verb == "units":
            return json.dumps({"units": [{"id": i, "name": f"T{i}", "cost": list(c)} for i, c in COSTS.items()]})
        specs = [a for a in args if a != "--json"]
        self.peeks += 1
        assert sum(len(x) + 1 for x in specs) < 4096, "a peek's trigger file is read 4095 bytes at most"
        return json.dumps({"peeks": [{"spec": x, "addr": 0, "value": self.answer(peer, x)} for x in specs]})

    host, join = "x-H", "x-J"


def army(n: int, typ: int = 2) -> list:
    return [(1, 0.0)] + [(typ, 0.0)] * n


class CapRead(unittest.TestCase):
    def setUp(self):
        sleep = mock.patch("time.sleep")
        sleep.start()
        self.addCleanup(sleep.stop)

    def read(self, game):
        first = {n: cd.look(game, n, range(cd.RECORDS), seating=True) for n in (game.host, game.join)}
        recs = {n: sorted(i for i, r in first[n]["recs"].items() if r["active"]) for n in first}
        seen = {n: [first[n], cd.look(game, n, recs[n])] for n in first}
        return cd.read_cap(game, first, {"A": DPID["A"], "B": DPID["B"]}, recs, seen)

    def test_a_slot_s_bytes(self):
        self.assertEqual(cd.unit_spec(0x05000000), "0x50000A6:x6E")
        self.assertEqual(cd.unit_of(unit_bytes(3, 2, 0.25)), (3, 2, 0.25))
        self.assertIsNone(cd.unit_of(unit_bytes(3, 2, alive=False)))
        self.assertIsNone(cd.unit_of("<unreadable>".encode().hex()))

    def test_both_peers_read_the_same_units_on_the_paused_game(self):
        # A: a commander, four finished units and one being built; B: a commander and nine; a free slot between
        a = army(4) + [None, (3, 0.5)]
        both = {"A": a, "B": army(9, 3)}
        game = FakeGame({"H": both, "J": both})
        at_cap, units = self.read(game)
        self.assertEqual(game.keys, [("H", "pause"), ("H", "pause")])      # paused for the read, then on
        self.assertFalse(game.paused)
        va = 2500 + 25000 / 60 + 4 * (100 + 1200 / 60)
        vb = 2500 + 25000 / 60 + 9 * (50 + 600 / 60)
        for p in cd.PEERS:
            self.assertEqual(at_cap[p]["alive"], {"A": 6, "B": 10})
            self.assertAlmostEqual(at_cap[p]["value"]["A"], va, places=1)
            self.assertAlmostEqual(at_cap[p]["value"]["B"], vb, places=1)
        self.assertEqual(at_cap["paused"], "H")
        self.assertEqual((at_cap["H"]["t"], at_cap["J"]["t"]), (game.t["H"], game.t["J"]))     # the clocks stood
        self.assertEqual(len(units["J"]["A"]), 6)
        value, why = cd.cap_value(at_cap, 6000)
        self.assertIsNone(why)
        self.assertEqual(value["B"]["t"], 6000)

    def test_a_block_is_read_on_until_its_count_is_found(self):
        # units alive past one peek's slots of the block: the read goes on, a peek at a time, and stops at the count
        many = [(1, 0.0)] + [None] * 150 + [(2, 0.0)] * 130
        game = FakeGame({"H": {"A": many, "B": army(2)}, "J": {"A": many, "B": army(2)}})
        blocks = {s: (ROWS["H"].index(s),) + game.block("H", s) for s in ("A", "B")}
        game.paused = True
        read = cd.cap_units(game, "x-H", blocks)
        self.assertEqual((len(read["units"]["A"]), len(read["units"]["B"])), (131, 3))
        self.assertEqual(read["alive"], {"A": 131, "B": 3})
        self.assertEqual(game.peeks, 2)         # 120 slots a seat, then A's next 240
        self.assertEqual(read["t"], 6010)

    def test_a_block_ends_the_read_of_a_count_it_does_not_hold(self):
        game = FakeGame({"H": {"A": army(2), "B": army(2)}})
        game.answer = lambda peer, sp, real=game.answer: "9" if sp.endswith(":2") else real(peer, sp)
        blocks = {s: (ROWS["H"].index(s),) + game.block("H", s) for s in ("A", "B")}
        read = cd.cap_units(game, "x-H", blocks)
        self.assertEqual((len(read["units"]["A"]), read["alive"]["A"]), (3, 9))
        self.assertEqual(game.peeks, 3)         # 120 slots a seat a peek, to the end of both blocks of 300

    def test_the_joiner_s_key_when_the_host_s_does_not_take(self):
        both = {"A": army(1), "B": army(1)}
        game = FakeGame({"H": both, "J": both}, takes=("J",))
        at_cap, _units = self.read(game)
        self.assertEqual(game.keys, [("H", "pause"), ("J", "pause"), ("J", "pause")])
        self.assertEqual(at_cap["paused"], "J")

    def test_a_game_no_key_stops_is_read_as_it_runs(self):
        both = {"A": army(1), "B": army(1)}
        game = FakeGame({"H": both, "J": both}, takes=())
        at_cap, _units = self.read(game)
        self.assertEqual(game.keys, [("H", "pause"), ("J", "pause")])
        self.assertIsNone(at_cap["paused"])
        self.assertIsNone(cd.cap_value(at_cap, 6000)[1])       # both still read the same units: it scores
        self.assertEqual(cd.cap_value(at_cap, 5000)[0], {})    # but a clock that ran past the cap's ticks does not

    def test_the_peers_holding_other_units_are_a_desync(self):
        game = FakeGame({"H": {"A": army(4), "B": army(9)}, "J": {"A": army(4), "B": army(30)}})
        at_cap, _units = self.read(game)
        value, why = cd.cap_value(at_cap, 6000)
        self.assertIsNone(value)
        self.assertIn("seat B is", why)

    def test_a_unit_of_a_type_the_table_lacks_cannot_run(self):
        from twopeers import CouldNotRun
        both = {"A": army(1, 77), "B": army(1)}
        with self.assertRaises(CouldNotRun):
            self.read(FakeGame({"H": both, "J": both}))


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
        self.assertEqual(r["pulse"]["A"], {"live": 31000.0, "t": 1807})
        self.assertEqual(r["value"], {})
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
        t = 3900 + cd.SLACK + 1
        h = log("H", PLAYED + [(3900, {"A": 0, "B": 30}, {}), (t, {"A": 0, "B": 30}, {})])
        late = log("J", PLAYED + [(3950, {"A": 2, "B": 30}, {}), (t, {"A": 0, "B": 30}, {})])
        r = cd.judge(h, late, 6000)
        self.assertEqual(r["outcome"], "desync")
        self.assertIn("on when", r["reason"])

    def test_the_joiner_s_clock_ahead_is_taken_off(self):
        # the joiner's GameTime runs 1300 ahead by the end: one end, seen in one round of looks, is one end
        h = log("H", PLAYED + [(3900, {"A": 0, "B": 30}, {"lost": ("A",)})])
        j = log("J", PLAYED + [(5200, {"A": 0, "B": 30}, {"lost": ("A",)})])
        r = cd.judge(h, j, 6000)
        self.assertEqual((r["outcome"], r["end_tick"], r["clock"]), ("B", 3900, {"H": 3900, "J": 5200}))
        # and a joiner past the cap on its own clock is not at the cap: the host's clock is the game's
        j = log("J", PLAYED + [(6500, {"A": 0, "B": 30}, {"lost": ("A",)})])
        r = cd.judge(h, j, 6000)
        self.assertEqual((r["outcome"], r["peers"]["J"]["outcome"]), ("B", "B"))

    def test_the_cap_is_the_host_s_whatever_the_joiner_s_clock(self):
        for tj in (5000, 7300):         # the joiner short of the cap by its clock, and long past it
            at = PLAYED + [(tj, {"A": 20, "B": 40}, {})]
            r = cd.judge(log("H", self.AT, end=6100), log("J", at, end=tj + 90), 6000, None,
                         {"H": {"t": 6040, "value": {"A": 40000.0, "B": 61000.0}},
                          "J": {"t": tj + 30, "value": {"A": 40000.0, "B": 61000.0}}})
            self.assertEqual((r["outcome"], r["peers"]["J"]["outcome"], r["clock"]["J"]), ("cap", "cap", tj))
            self.assertEqual(r["value"]["B"], {"live": 61000.0, "t": 6000, "at": tj + 30, "other": 61000.0})
            self.assertEqual(bar.outcome_of(r)[0], "B")

    def test_a_game_at_the_cap(self):
        at = PLAYED + [(6010, {"A": 20, "B": 40}, {})]
        pulses = ((7, 30000), (1807, 31000), (5407, 40000))
        r = cd.judge(log("H", at, pulses, end=6100), log("J", at, ((7, 30000), (5399, 60000)), end=6100), 6000)
        self.assertEqual((r["outcome"], r["draw"], r["end_tick"]), ("cap", False, 6000))
        self.assertEqual(r["pulse"], {"A": {"live": 40000.0, "t": 5407}, "B": {"live": 60000.0, "t": 5399}})
        self.assertEqual(r["lead"], "B")
        # the pulses are the seats' own, before the cap: with no read of the units the game has no value at
        # the limit and does not score
        self.assertEqual(r["value"], {})
        self.assertIn("with no value (no read of the units at the cap)", r["reason"])
        self.assertIsNone(bar.outcome_of(r)[0])

    AT = PLAYED + [(6010, {"A": 20, "B": 40}, {})]

    def capped(self, at_cap, cap=6000):
        return cd.judge(log("H", self.AT, end=6100), log("J", self.AT, end=6100), cap, None, at_cap)

    def test_the_value_at_the_cap_scores_the_game(self):
        r = self.capped({"paused": "H", "H": {"t": 6010, "value": {"A": 40000.0, "B": 61000.5}},
                         "J": {"t": 6090, "value": {"A": 40000.0, "B": 61000.5}}})
        self.assertEqual((r["outcome"], r["end_tick"], r["lead"]), ("cap", 6000, "B"))
        # each seat's figure is its own peer's, stamped with the cap; the other peer's is beside it
        self.assertEqual(r["value"], {"A": {"live": 40000.0, "t": 6000, "at": 6010, "other": 40000.0},
                                      "B": {"live": 61000.5, "t": 6000, "at": 6090, "other": 61000.5}})
        self.assertEqual(bar.outcome_of(r), ("B", "value A 40000, B 61000"))
        self.assertIn("value A 40000, B 61000 on both peers", r["reason"])

    def test_a_cap_with_no_lead_is_a_draw(self):
        r = self.capped({"H": {"t": 6010, "value": {"A": 40000.0, "B": 45000.0}},
                         "J": {"t": 6090, "value": {"A": 40000.0, "B": 45000.0}}})
        self.assertEqual(bar.outcome_of(r)[0], "draw")
        self.assertEqual(r["lead"], "draw")

    def test_the_peers_within_the_tolerance_agree(self):
        near = 40000.0 * (1 - cd.VALUE_TOL) + 1
        r = self.capped({"H": {"t": 6010, "value": {"A": 40000.0, "B": 61000.0}},
                         "J": {"t": 6090, "value": {"A": near, "B": 61000.0}}})
        self.assertEqual((r["outcome"], r["value"]["A"]["live"], r["value"]["A"]["other"]), ("cap", 40000.0, near))

    def test_the_peers_apart_on_the_value_are_a_desync(self):
        r = self.capped({"H": {"t": 6010, "value": {"A": 40000.0, "B": 61000.0}},
                         "J": {"t": 6090, "value": {"A": 40000.0, "B": 58000.0}}})
        self.assertEqual(r["outcome"], "desync")
        self.assertEqual(r["reason"], "the peers disagree on the value at the cap: seat B is 58000 on its own peer "
                                      "J and 61000 on H, more than 2% apart")
        self.assertIsNone(bar.outcome_of(r)[0])

    def test_a_read_away_from_the_cap_is_no_value(self):
        for t in (5990, 6000 + cd.CAP_LATE + 1):
            r = self.capped({"H": {"t": t, "value": {"A": 40000.0, "B": 61000.0}},
                             "J": {"t": 6090, "value": {"A": 40000.0, "B": 61000.0}}})
            self.assertEqual((r["outcome"], r["value"]), ("cap", {}))
            self.assertIn(f"not read within {cd.CAP_LATE} ticks after the cap 6000: the host's GameTime there "
                          f"was {t}", r["reason"])
            self.assertIsNone(bar.outcome_of(r)[0])

    def test_no_look_past_the_cap_is_read(self):
        # a seat gone at a look after the first one at the cap: the game is the cap's, as both peers saw it there
        after = self.AT + [(6200, {"A": 0, "B": 40}, {})]
        r = cd.judge(log("H", after, end=6300), log("J", self.AT, end=6300), 6000)
        self.assertEqual((r["outcome"], r["peers"]["H"]["t"], r["peers"]["H"]["gone"]), ("cap", 6010, {}))

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


NONE = dict.fromkeys(TEAM, cd.NO_TEAM)
ARMED = cd.DUEL_LINE + ": a network game no longer ends for want of a human (0x4655E0 jne -> jmp)\n"
OUT = {"human H": 0, "human J": 0}


def gone_log(peer: str, looks_: list, extra=ARMED, team=NONE) -> str:
    return log(peer, looks_, extra=extra, team=team)


class NoHumans(unittest.TestCase):
    """THE HUMANS GONE: the judge of a duel played with --no-humans."""

    WON = [(100, {"A": 1, "B": 1, **OUT}, {}), (2000, {"A": 9, "B": 14, **OUT}, {}),
           (3900, {"A": 0, "B": 30, **OUT}, {})]

    def test_the_seats_alone_play_and_b_wins(self):
        r = cd.judge(gone_log("H", self.WON), gone_log("J", self.WON), 6000, no_humans=True)
        self.assertEqual((r["outcome"], r["end_tick"], r["failed"]), ("B", 3900, []))
        self.assertEqual((r["humans"], r["humans_out"]), ("none", {"H": 100, "J": 100}))

    def test_the_default_judges_the_humans_idle(self):
        won = PLAYED + [(3900, {"A": 0, "B": 30}, {})]
        r = cd.judge(log("H", won), log("J", won), 6000)
        self.assertEqual((r["outcome"], r["humans"]), ("B", "idle"))
        self.assertNotIn("humans_out", r)
        # and a seating with no team is not its seating
        r = cd.judge(gone_log("H", self.WON), gone_log("J", self.WON), 6000)
        self.assertIn("H: the teams are", r["reason"])

    def test_a_peer_whose_lever_is_not_installed_fails(self):
        r = cd.judge(gone_log("H", self.WON), gone_log("J", self.WON, extra=""), 6000, no_humans=True)
        self.assertEqual(r["outcome"], "could not run")
        self.assertIn("J: its log has no `duel: TEST LEVER tagpu_impai_duel.on -- installed`", r["reason"])

    def test_a_human_holding_a_unit_at_the_last_look_fails(self):
        held = [(t, dict(alive, **{"human J": 1}), kw) for t, alive, kw in self.WON]
        r = cd.judge(gone_log("H", held), gone_log("J", self.WON), 6000, no_humans=True)
        self.assertEqual(r["outcome"], "could not run")
        self.assertIn("H: a record that is no AI seat's holds a unit at its last look", r["reason"])
        self.assertEqual(r["humans_out"], {"H": None, "J": 100})

    def test_the_humans_are_out_from_the_first_look_none_holds_a_unit(self):
        late = [(100, {"A": 1, "B": 1}, {})] + self.WON[1:]
        r = cd.judge(gone_log("H", late), gone_log("J", self.WON), 6000, no_humans=True)
        self.assertEqual((r["outcome"], r["humans_out"]), ("B", {"H": 2000, "J": 100}))

    def test_the_seats_must_not_be_allied(self):
        one = dict.fromkeys(TEAM, 0)
        r = cd.judge(gone_log("H", self.WON, team=one), gone_log("J", self.WON), 6000, no_humans=True)
        self.assertEqual(r["outcome"], "could not run")
        self.assertIn("H: seat A's alliance bytes", r["reason"])

    def test_a_level_that_ended_under_the_lever(self):
        played = self.WON[:2]
        r = cd.judge(gone_log("H", played), gone_log("J", played), 6000, no_humans=True)
        self.assertEqual(r["outcome"], "could not run")
        self.assertTrue(r["reason"].endswith("its level ended with both AI seats holding units"))


class Maps(unittest.TestCase):
    def test_a_map_by_key_or_name(self):
        self.assertEqual(cd.map_of("lavamania"), ("lavamania", "Lava Mania"))
        self.assertEqual(cd.map_of("Acid Foursome"), ("acidfoursome", "Acid Foursome"))
        with self.assertRaises(ValueError):
            cd.map_of("Two Continents")


if __name__ == "__main__":
    unittest.main()
