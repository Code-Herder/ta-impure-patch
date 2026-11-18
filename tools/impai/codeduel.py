#!/usr/bin/env python3
"""codeduel -- code against code: two builds of ImpAI play each other in one network game, each peer on its
own ddraw.dll and playing the one computer seat it added.

    tools/impai/codeduel.py --a DLL_A --b DLL_B --out DIR [--map NAME] [--sides aa|ac|ca|cc] [--cap T] [--port N]

One game of two instances (twopeers.py: a private display, a DirectPlay port leased for the game), under the run
lock for two game lanes on GDI, no GPU memory (tools/runlock.py). The host H runs DLL_A and the joiner J DLL_B,
both launched with `--netai impai`; in the battle room the host adds a computer seat, A, and the joiner one, B.
The game plays until an AI seat holds no unit or the host's GameTime reaches the cap, each peer then leaves
through its menus, so its teardown writes ImpAI's level-end lines, and DIR/result.json is the game's result in
bar.py's shape (`outcome` A, B or cap, `draw`, `end_tick`, `value`), so bar.outcome_of reads its points, a game
at the cap's by THE VALUE AT THE CAP.

THE MAP is one of bar.py's (a key of bar.MAPS, or a map's own name); the battle room loads it by name. With
fixed locations the humans take the map's first two start positions and the AI seats the next two, in row
order, so the map needs four; the result names each seat's start.

THE HUMANS STAY, ALLIED AND IDLE. A network game seats each peer's human beside the AIs, and the engine ends a
network game once no human still playing holds a unit, whatever the AI seats hold (0x4655C0..0x4655E0: mode 3,
ActiveCommanderDeath not 2, and 0x457CB0 -- the records of a human, type 1 or type 3 with PlayerInfo +0x94 1,
not watching, with a unit alive or none yet created -- answers 0). MEASURED on Lava Mania: with both humans
watchers from the battle room (WATCHING allowed, SIDE0 at its third stage) the level ended at GameTime 35; with
both commanders self-destructed (ctrl+c ctrl+d) the game went on after the first death -- the dead human's peer
showed the watch notice MSGBOX.GUI, whose OK dismisses it, and kept running its AI seat, whose units alive
rose from 1 to 2 -- and the level ended within 600 ticks of the second order. So the humans cannot be removed
without a change in the DLL, and each stays: on a team with its own peer's AI (the TEAMICONS gadgets; an AI
allies only by its team, engine/network.md), its commander never ordered. A's team is 1 and B's 0
(seat_room). The other side's AI treats that commander as an enemy unit, so a game is not the two AI seats
alone: the judge reads the AI seats' records only, and a game the engine ends because both commanders died
with both AI seats alive is `ended`, not an outcome.

THE SPEED. A network game starts at Normal speed whatever the registry's `gamespeed` says: 30.1 ticks a second
(MEASURED on three games), and 45.0 once the host alone had pressed `+` five times. So both peers press it
ten times as soon as the game is in play; the result's `ticks_per_s` is the host's GameTime over the wall
time of the play.

THE LOOKS. Every LOOK_S seconds, and every NEAR_S within NEAR ticks of the cap, each peer's GameTime and its
seated player records are read with one `tacli peek`, which the DLL logs line by line (`peek: <spec> @<address>
= <value>`), so a peer's log holds its own looks and the judge needs the two logs alone for the outcome. The
first look reads all ten records with their team, their alliances, their start position and their unit block;
the others the seated records' `[rec]`, DirectPlay ID +4, controller +0x73, units alive +0x144 and commanders
lost +0x106. A round of looks is the host's and then the joiner's, so the two logs hold the same rounds.

THE CLOCKS. Each peer counts its own GameTime, and the two run apart. MEASURED 2026-11-17 on four duels at once
to a cap of 20 000: in one the joiner's GameTime stayed 15 ticks ahead of the host's all game; in three it
gained on it at every look, 5 to 17 ticks in a hundred, up to 1 003, 1 011 and 1 308 ticks, where it stayed (the
host at about 57 ticks a second where the joiner played about 60). So:
  - the cap is the host's GameTime, and the joiner's looks are read to their end, whatever its clock says;
  - the two peers' GameTimes of one end are compared with the joiner's lead taken off, the lead being the
    difference of the two clocks at the last round of looks (the result's `clock`: each peer's GameTime there);
  - a seat plays on its own peer's clock, so at the host's cap the seat of the peer ahead has played that many
    more ticks: `clock` is in every result for whoever weighs the game.

THE VALUE AT THE CAP is bar.py's value at the limit, read from the game's memory on both peers: the value of
each AI seat's finished units alive, a unit's type's metal + energy / 60 by the peer's own type table (`tacli
units`), summed by bar.value_at. A network game keeps no recording (`recorder: recording` is in neither
peer's log) and refuses the pacing's `pause at T` (`pause: at 6000 refused: a network game`, MEASURED on both
peers), so the harness stops the game with its own Pause key and reads the units themselves:
  - once the host's look is at or past the cap, the Pause key on the host (the joiner if the host's does
    not take), and both peers looked at until their GameTime stands at two looks in a row. A network game's
    pause is every peer's: MEASURED on Lava Mania, the key on the host at GameTime 6 059 (the joiner at
    6 147) and both stood, at 6 087 and 6 171, through two reads 6 s apart, and ran on at the second press;
  - on each peer, each seat's unit block (its player record's first and last unit, +0x67 and +0x6B, 0x118 a
    unit) is peeked from its first slot on, the bytes +0xA6..+0x113 of a slot: its type's index +0xA6, its
    owner +0xFF, the fraction left to build +0x104 (0.0 finished) and the alive bit 0x10000000 of +0x110,
    until the peek's own count of the seat's units alive (+0x144) is found or the block ends. One peek is one
    frame visit's read, and its trigger file holds SLOTS_A_PEEK slots;
  - the judge takes each seat's value from its own peer (a peer simulates the units of the seats it added) and
    the other peer's as the check: more than VALUE_TOL (2 %) of the larger apart, the game is a `desync`,
    named with both figures. A read before the cap or more than CAP_LATE ticks past it by the host's clock
    (THE CLOCKS) is no value at the cap, and the game then does not score. The result's `value` is {A, B:
    {live, t: the cap, at: its own peer's GameTime at the read, other: the other peer's figure}} and
    `cap_read` each peer's read; DIR/cap-units.json lists the units.
MEASURED 2026-11-17 on Lava Mania with the Pause key and on Acid Foursome without it (a read each peer, 250 to
400 ticks apart): both peers gave both seats the same value to the cent (31 639.35 and 32 250.73; 31 641.15
and 31 701.63), every live unit within the first 200 slots of its block of 1 500, and the joiner's own pulse
for its seat at GameTime 7 206 said 31 702; and on the fourteen later reads of paused games, 1 to 60 units a
seat at caps of 5 000, 6 000 and 20 000 on eight maps, the two peers' figures were equal to the cent in every
one, the host's read 32 to 179 ticks past the cap. A game the Pause key does not stop is read as it runs
(`paused` none), under the same test.

THE JUDGE (`judge`), from the two logs:
  - the seats: each peer's level init logged one network seat line (`impai: seats`, tagpu_impai.c) that names
    netai=impai and marks one record, its own AI seat; A is the host's and B the joiner's, each known on the
    other peer by its DirectPlay ID (a row index differs between peers), where it is controller 3 and among
    the other peers' ImpAI seats;
  - the teams: on each peer A's team is its own human's and not B's, and each AI seat allies its human and
    not the other seat;
  - each peer's view: the first look at which an AI seat that held a unit holds none names the loser (both at
    one look: a draw, written as bar.py writes it, `cap` with `draw`); for the host, neither at its first look
    at or past the cap: `cap`, and no later look of its is read; the joiner, with no seat gone at any look, is
    at the cap with it (THE CLOCKS). A seat whose record is freed is a peer that left, and a level that ended
    before either is `ended`;
  - the outcome: both peers' views agree -- the same winner, its loser gone on both within SLACK ticks, the
    joiner's lead taken off -- or the game is `desync`, the failure named with both views. `left`, `ended` and
    a failed check below are `could not run`;
  - the brain of each seat: at its level end each peer logged ImpAI turns (`impai: calls`) for its own marked
    seat and no other, and no line of its log begins with a line stem only the other peer's DLL holds (THE
    FINGERPRINT);
  - a game at the cap: its `value` from the harness's read of both peers (THE VALUE AT THE CAP, cap_value),
    which bar.by_value scores;
  - `pulse`: each seat's `vlive` of its own peer's last pulse line before the end (`impai: pulse`, once a
    game minute), with that pulse's GameTime: the seat's own brain's figure, up to 1800 ticks old, kept
    beside the value and never scored. `lead` is the seat ahead by bar.VALUE_LEAD on the value, on the pulses
    where the game has none.

THE FINGERPRINT. A build logs no line naming itself in tagpu.log, so the proof that a peer ran its own DLL is
its log's lines against the two files: `stems` takes from a DLL every printable string that starts as a log
line does (`module: text`, up to its first conversion), and `only_stems` the ones one DLL holds and the other
does not. A line that begins with a stem only the other DLL holds cannot have come from this peer's DLL. The
result carries, for each peer, how many of its lines begin with a stem only its own DLL holds (`own`) and only
the other's (`other`, which must be 0); two DLLs no line tells apart fail the check.

Exit 0 when the game has an outcome (A, B or cap), 1 for a desync, 2 when it could not be run or judged.
"""
import argparse
import hashlib
import json
import re
import signal
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TREE / "tools" / "replay"))
sys.path.insert(0, str(TREE / "tools"))
import bar  # noqa: E402
import netai_check  # noqa: E402

GAMES = 2                               # the host and the joiner: the run lock's lanes
CAP = 36000
LOOK_S = 3.0
NEAR, NEAR_S = 600, 0.5                 # THE LOOKS near the cap: ten seconds of play before it, twice a second
SETTLE_S = 20                           # how long the other peer is looked at for the end the first one saw
SPEED_KEYS = 10                         # the speed's steps from Normal to its fastest
SLACK = 900                             # ticks between the two peers' first look at a seat gone, the joiner's
                                        # lead taken off (THE CLOCKS): the looks' spacing at a network game's 60
                                        # ticks a second, five times over
PEERS = ("H", "J")
SEAT_PEER = {"A": "H", "B": "J"}
MAIN, GAMETIME = 0x511DE8, 0x38A47      # tareplay.GAMETIME
REC, STRIDE, RECORDS = 0x1B63, 0x14B, 10        # the player records (twopeers.py)
PLAY = {0: "active", 4: "dpid", 0x73: "control", bar.ALIVE: "alive", bar.LOST: "lost"}
SEATING = {0x13F: "team", 0x108: "allies", 0x147: "start",     # engine/units.md: the lobby team, the alliance
           0x67: "first", 0x6B: "last"}                         # bytes (the first four), the start position,
                                                                # the unit block's first and last unit
SIZE = {0: 4, 4: 4, 0x73: 1, bar.ALIVE: 2, bar.LOST: 2, 0x13F: 1, 0x108: 4, 0x147: 1, 0x67: 4, 0x6B: 4}
DUEL_LEVER = "tagpu_impai_duel.on"      # THE HUMANS GONE: tagpu_patches.c, patch_duel_lever
DUEL_LINE = f"duel: TEST LEVER {DUEL_LEVER} -- installed"
# THE VALUE AT THE CAP
UNIT = 0x118                            # a unit's bytes (engine/units.md)
U_FROM, U_SPAN = bar.UNIT_TYPE, 0x6E    # the bytes of a unit a peek reads: +0xA6 .. +0x113
U_STATE, U_ALIVE = 0x110, 0x10000000
SLOTS_A_PEEK = 240                      # a peek's trigger file is read 4095 bytes at most, 15 a slot's spec
CAP_LATE = 900                          # ticks past the cap a read still is the cap's: fifteen seconds of play
VALUE_TOL = 0.02                        # the two peers' figures for a seat, apart by this much of the larger
STAND_LOOKS, STAND_S = 5, 0.5           # the looks a Pause key is given to stand, and their spacing
NO_TEAM = 5
PEEK = re.compile(rf"^peek: \*0x{MAIN:X}\+0x([0-9A-F]+):\d @0x[0-9A-F]+ = (\d+) ", re.M)
LEVEL_END = re.compile(rf"^(?:impai: calls seat=|{re.escape(bar.LEVEL_END)})", re.M)
STEM = re.compile(rb"[a-z][a-z0-9_]*: [\x20-\x24\x26-\x5b\x5d-\x7e]{10,}")     # no conversion, no escape
PULSE_TICKS = 1800


# ---- THE FINGERPRINT ----

def stems(dll: bytes) -> set:
    """Every string of `dll` that starts as a log line does, up to its first conversion."""
    return {m.group().decode() for m in STEM.finditer(dll)}


def only_stems(a: bytes, b: bytes) -> dict:
    """{"H": the stems only the host's DLL `a` holds, "J": only the joiner's `b`}."""
    sa, sb = stems(a), stems(b)
    return {"H": sa - sb, "J": sb - sa}


def stem_lines(text: str, some: set) -> int:
    """How many lines of `text` begin with a stem of `some`."""
    heads = tuple(some)
    return sum(1 for ln in text.splitlines() if ln.startswith(heads)) if heads else 0


# ---- THE LOOKS ----

def spec(rec: int, off: int) -> str:
    return f"*0x{MAIN:X}+0x{REC + rec * STRIDE + off:X}:{SIZE[off]}"


def look_specs(recs, seating: bool = False) -> list:
    """One look's peeks: GameTime first, which is where the judge starts a look."""
    offs = list(PLAY) + (list(SEATING) if seating else [])
    return [f"*0x{MAIN:X}+0x{GAMETIME:X}:4"] + [spec(i, off) for i in recs for off in offs]


def play_text(text: str) -> str:
    """`text` up to its level end: a record read after the teardown holds no count of the game's."""
    m = LEVEL_END.search(text)
    return text[:m.start()] if m else text


def looks(text: str) -> list:
    """The looks a log holds, in order: [{"t": GameTime, "recs": {i: {field: value}}}], a record kept when the
    look read every PLAY field of it."""
    out, cur = [], None
    names = {**PLAY, **SEATING}
    for m in PEEK.finditer(text):
        addr, val = int(m.group(1), 16), int(m.group(2))
        if addr == GAMETIME:
            cur = {"t": val, "recs": {}}
            out.append(cur)
        elif cur is not None and REC <= addr < REC + RECORDS * STRIDE:
            i, off = divmod(addr - REC, STRIDE)
            if off in names:
                cur["recs"].setdefault(i, {})[names[off]] = val
    for lk in out:
        lk["recs"] = {i: r for i, r in lk["recs"].items() if all(n in r for n in PLAY.values())}
    return [lk for lk in out if lk["recs"]]


def record_of(look: dict, dpid: int):
    """The index of the look's seated record with DirectPlay ID `dpid`, or None."""
    for i, r in look["recs"].items():
        if r["active"] and r["dpid"] == dpid:
            return i
    return None


def view(lks: list, dpid: dict, cap) -> dict:
    """One peer's view of the game from its looks: {"outcome": A | B | draw | cap | left | ended | unfinished,
    "t": the GameTime of the look that decided it, "gone": {seat: GameTime}, "last": {"t", seat: units alive},
    "lost": {seat: the GameTime of the first look with a commander lost}}. `dpid` is {A, B: DirectPlay ID};
    `cap` is None for the joiner, whose clock is not the cap's (THE CLOCKS); `ended` is given by the caller,
    who knows whether the log's level ended."""
    held, gone, lost, last = set(), {}, {}, {}
    for lk in lks:
        alive = {}
        for s in ("A", "B"):
            i = record_of(lk, dpid[s])
            if i is None:
                return {"outcome": "left", "who": s, "t": lk["t"], "gone": gone, "last": last, "lost": lost}
            alive[s] = lk["recs"][i]["alive"]
            if lk["recs"][i]["lost"] and s not in lost:
                lost[s] = lk["t"]
            if alive[s]:
                held.add(s)
            elif s in held:
                gone[s] = lk["t"]
        last = {"t": lk["t"], **alive}
        if gone:
            who = "draw" if len(gone) == 2 else "B" if "A" in gone else "A"
            return {"outcome": who, "t": lk["t"], "gone": gone, "last": last, "lost": lost}
        if cap is not None and lk["t"] >= cap:
            return {"outcome": "cap", "t": lk["t"], "gone": gone, "last": last, "lost": lost}
    return {"outcome": "unfinished", "t": last.get("t"), "gone": gone, "last": last, "lost": lost}


def last_pulse(text: str, seat: int, upto: int):
    """{"live", "t"} of record `seat`'s last pulse line at or before GameTime `upto`, or None."""
    best = None
    for m in bar.PULSE.finditer(text):
        if int(m.group(1)) == seat and int(m.group(2)) <= upto:
            best = {"live": float(m.group(10)), "t": int(m.group(2))}
    return best


def lead(value: dict):
    """The seat ahead by bar.VALUE_LEAD on `value`'s figures: "A", "B" or "draw"; None without both."""
    if not (value.get("A") and value.get("B")):
        return None
    for s, o in (("A", "B"), ("B", "A")):
        if value[s]["live"] > value[o]["live"] and value[s]["live"] >= bar.VALUE_LEAD * value[o]["live"]:
            return s
    return "draw"


# ---- THE VALUE AT THE CAP ----

def unit_spec(addr: int) -> str:
    """The peek of the unit at `addr`: its bytes from its type's index to its state."""
    return f"0x{addr + U_FROM:X}:x{U_SPAN:X}"


def unit_of(value: str):
    """(owner, type index, fraction left) of a slot's peeked bytes, None for a slot that holds no live unit."""
    b = bytes.fromhex(value.replace(" ", ""))
    if len(b) != U_SPAN or not struct.unpack_from("<I", b, U_STATE - U_FROM)[0] & U_ALIVE:
        return None
    return (b[bar.UNIT_OWNER - U_FROM], struct.unpack_from("<H", b, 0)[0],
            struct.unpack_from("<f", b, bar.UNIT_LEFT - U_FROM)[0])


def cap_units(tp, name: str, blocks: dict) -> dict:
    """One peer's read of the seats' units: {"t": its GameTime at the first peek, "alive": {seat: the record's
    count there}, "units": {seat: [(owner, type index, fraction left)]}}. `blocks` is {seat: (record, first
    unit, last unit)}; each peek asks the GameTime, each seat's count and the next slots of every seat still
    short of its count, and the read ends when none is or every block has."""
    from twopeers import CouldNotRun
    for s, (rec, first, last) in blocks.items():
        if not first or last < first or (last - first) % UNIT:
            raise CouldNotRun(f"{name}: seat {s}'s unit block {first:#x}..{last:#x} is not one of {UNIT:#x}-byte units")
    slots = {s: (last - first) // UNIT + 1 for s, (_rec, first, last) in blocks.items()}
    head = [f"*0x{MAIN:X}+0x{GAMETIME:X}:4"] + [spec(rec, bar.ALIVE) for rec, _f, _l in blocks.values()]
    at, units, t, alive = dict.fromkeys(blocks, 0), {s: [] for s in blocks}, None, None
    while True:
        short = [s for s in blocks if at[s] < slots[s] and (alive is None or len(units[s]) < alive[s])]
        if not short:
            return {"t": t, "alive": alive, "units": units}
        ask = [(s, blocks[s][1] + k * UNIT) for s in short
               for k in range(at[s], min(at[s] + SLOTS_A_PEEK // len(short), slots[s]))]
        specs = head + [unit_spec(a) for _s, a in ask]
        o = tp.tacli("peek", name, *specs, "--json", timeout=90)
        rows = json.loads(o[o.index("{"):])["peeks"] if "{" in o else []
        if len(rows) != len(specs):
            raise CouldNotRun(f"{name}: the peek of its units came back {len(rows)} of {len(specs)}")
        try:
            got = [int(str(r["value"]).split()[0]) for r in rows[:len(head)]]
            if alive is None:
                t, alive = got[0], dict(zip(blocks, got[1:]))
            for (s, _a), r in zip(ask, rows[len(head):]):
                u = unit_of(str(r["value"]))
                at[s] += 1
                if u and u[0] == blocks[s][0]:
                    units[s].append(u)
        except ValueError:
            raise CouldNotRun(f"{name}: the peek of its units did not read") from None


def type_costs(tp, name: str) -> dict:
    """{type index: (metal, energy)} of the peer's type table (`tacli units`)."""
    from twopeers import CouldNotRun
    o = tp.tacli("units", name, "--json", "--limit", "0", timeout=180)
    if "{" not in o:
        raise CouldNotRun(f"{name}: tacli units gave no table")
    table, _ = json.JSONDecoder().raw_decode(o[o.index("{"):])
    return {u["id"]: (float(u["cost"][0]), float(u["cost"][1])) for u in table["units"]}


def peer_value(read: dict, blocks: dict, costs: dict) -> dict:
    """{seat: its value} of one peer's read (bar.value_at: the finished units alive, by `costs`)."""
    units = {"at": read["t"], "units": [u for us in read["units"].values() for u in us]}
    got = bar.value_at(units, {s: rec for s, (rec, _f, _l) in blocks.items()}, costs)
    return {s: v["live"] for s, v in got.items()}


def cap_value(at_cap, cap: int) -> tuple:
    """THE VALUE AT THE CAP from the peers' reads, {peer: {"t", "value": {A, B}}}: (the result's `value`, None);
    ({}, why the game has no value at the cap); or (None, the desync)."""
    if not at_cap or any(p not in at_cap for p in PEERS):
        return {}, "no read of the units at the cap"
    if not cap <= at_cap["H"]["t"] <= cap + CAP_LATE:       # THE CLOCKS: the cap is the host's GameTime
        return {}, (f"the units were not read within {CAP_LATE} ticks after the cap {cap}: the host's GameTime "
                    f"there was {at_cap['H']['t']}")
    value = {}
    for s, p in SEAT_PEER.items():
        o = "J" if p == "H" else "H"
        own, there = float(at_cap[p]["value"][s]), float(at_cap[o]["value"][s])
        if abs(own - there) > VALUE_TOL * max(own, there):
            return None, (f"the peers disagree on the value at the cap: seat {s} is {own:.0f} on its own peer {p} "
                          f"and {there:.0f} on {o}, more than {VALUE_TOL:.0%} apart")
        value[s] = {"live": own, "t": cap, "at": at_cap[p]["t"], "other": there}
    return value, None


# ---- THE JUDGE ----

def seats(texts: dict, first: dict) -> tuple:
    """(marks {peer: its own AI record}, dpid {A, B}, the failures) from each peer's network seat line and its
    first look."""
    bad, marks, rimpai, dpid = [], {}, {}, {}
    for p in PEERS:
        bad += [f"{p}: ImpAI's content did not fit its tables: {m}" for m in netai_check.NOFIT.findall(texts[p])]
        got = netai_check.seats_lines(texts[p])
        if len(got) != 1:
            bad.append(f"{p}: {len(got)} network seat lines, not one")
        elif got[0][1] != "impai" or len(got[0][0]) != 1:
            bad.append(f"{p}: its level init read netai={got[0][1]} and marked {got[0][0] or 'none'}, not the "
                       f"one computer seat it added under netai=impai")
        else:
            marks[p], rimpai[p] = got[0][0][0], got[0][5]
    if len(marks) != 2:
        return marks, dpid, bad
    for s, p in SEAT_PEER.items():
        r = first[p]["recs"].get(marks[p])
        if not r or not r["active"] or r["control"] != 2:
            bad.append(f"{p}: its marked record {marks[p]} is not a seated controller-2 record at its first look")
            continue
        dpid[s] = r["dpid"]
        o = "J" if p == "H" else "H"
        twin = record_of(first[o], r["dpid"])
        if twin is None or first[o]["recs"][twin]["control"] != 3 or rimpai[o] != [twin]:
            bad.append(f"{o}: seat {s} (DirectPlay ID {r['dpid']:#x}) is record {twin} there, not a controller-3 "
                       f"record that is its one other peers' ImpAI seat {rimpai[o] or 'none'}")
    return marks, dpid, bad


def teams(first: dict, dpid: dict) -> list:
    """The failures of THE HUMANS STAY's seating, on each peer's first look: each AI seat on its own human's
    team and allied to it, and not to the other seat."""
    bad, human = [], {}
    for p in PEERS:
        own = [r["dpid"] for r in first[p]["recs"].values() if r["active"] and r["control"] == 1]
        if len(own) != 1:
            return [f"{p}: {len(own)} controller-1 records at its first look, not its one human"]
        human[p] = own[0]
    for p in PEERS:
        lk = first[p]
        idx = {s: record_of(lk, dpid[s]) for s in ("A", "B")}
        idx.update({f"human {q}": record_of(lk, human[q]) for q in PEERS})
        if None in idx.values() or any("team" not in lk["recs"][i] for i in idx.values()):
            bad.append(f"{p}: its first look does not hold the four seats' teams {idx}")
            continue
        team = {k: lk["recs"][i]["team"] for k, i in idx.items()}
        if not (team["A"] == team["human H"] != team["B"] == team["human J"]) or NO_TEAM in team.values():
            bad.append(f"{p}: the teams are {team}, not A with H's human and B with J's")
        for s, o in (("A", "B"), ("B", "A")):
            allies = lk["recs"][idx[s]]["allies"]
            mine, other = idx[f"human {SEAT_PEER[s]}"], idx[o]
            if not allies >> (8 * mine) & 1 or allies >> (8 * other) & 1:
                bad.append(f"{p}: seat {s}'s alliance bytes {allies:#010x} do not ally its human (record {mine}) "
                           f"alone of it and seat {o} (record {other})")
    return bad


def humans_out(lks: list, dpid: dict):
    """The GameTime of the first look from which no record but the two AI seats' holds a unit; None when one
    does at the last look."""
    ai, out = set(dpid.values()), None
    for lk in lks:
        if any(r["active"] and r["dpid"] not in ai and r["alive"] for r in lk["recs"].values()):
            out = None
        elif out is None:
            out = lk["t"]
    return out


def humans_gone(texts: dict, lks: dict, first: dict, dpid: dict) -> tuple:
    """({peer: humans_out}, the failures of THE HUMANS GONE): each peer's DLL installed the duel lever, no
    human holds a unit at its last look, and neither AI seat allies the other."""
    bad, out = [], {}
    for p in PEERS:
        if DUEL_LINE not in texts[p]:
            bad.append(f"{p}: its log has no `{DUEL_LINE}`: the end test was not held off")
        out[p] = humans_out(lks[p], dpid)
        if out[p] is None:
            bad.append(f"{p}: a record that is no AI seat's holds a unit at its last look")
        idx = {s: record_of(first[p], dpid[s]) for s in ("A", "B")}
        if None in idx.values() or any("allies" not in first[p]["recs"][i] for i in idx.values()):
            bad.append(f"{p}: its first look does not hold the two seats' alliances {idx}")
            continue
        for s, o in (("A", "B"), ("B", "A")):
            allies = first[p]["recs"][idx[s]]["allies"]
            if allies >> (8 * idx[o]) & 1:
                bad.append(f"{p}: seat {s}'s alliance bytes {allies:#010x} ally seat {o} (record {idx[o]})")
    return out, bad


def judge(host: str, join: str, cap: int, only: dict = None, at_cap: dict = None, no_humans: bool = False) -> dict:
    """The game's result from the two peers' logs (THE JUDGE) and, for a game at the cap, their reads of the
    units there (`at_cap`, cap_value's). `only` is only_stems of the two DLLs, None to leave the fingerprint
    out; `no_humans` judges THE HUMANS GONE's seating in place of THE HUMANS STAY's. Keys: outcome (A, B, cap,
    desync, could not run), draw, reason, end_tick, cap, value, pulse, lead, clock, seats, peers, humans,
    humans_out, failed."""
    texts = {"H": host, "J": join}
    plays = {p: play_text(texts[p]) for p in PEERS}
    lks = {p: looks(plays[p]) for p in PEERS}
    res = {"outcome": "could not run", "draw": False, "reason": "", "end_tick": None, "cap": cap, "value": {},
           "pulse": {}, "lead": None, "clock": {}, "seats": {}, "peers": {}, "failed": [],
           "humans": "none" if no_humans else "idle"}
    short = [p for p in PEERS if not lks[p]]
    if short:
        res["reason"] = f"no look in the log of {', '.join(short)}"
        return res
    first = {p: lks[p][0] for p in PEERS}
    res["clock"] = {p: lks[p][-1]["t"] for p in PEERS}      # THE CLOCKS: the last round of looks
    ahead = res["clock"]["J"] - res["clock"]["H"]
    marks, dpid, bad = seats(texts, first)
    if len(dpid) == 2:
        if no_humans:
            res["humans_out"], out = humans_gone(texts, lks, first, dpid)
            bad += out
        else:
            bad += teams(first, dpid)
        res["peers"] = {"H": view(lks["H"], dpid, cap), "J": view(lks["J"], dpid, None)}
        if res["peers"]["H"]["outcome"] == "cap" and res["peers"]["J"]["outcome"] == "unfinished":
            res["peers"]["J"]["outcome"] = "cap"
        for p in PEERS:
            if res["peers"][p]["outcome"] == "unfinished" and LEVEL_END.search(texts[p]):
                res["peers"][p]["outcome"] = "ended"
        for s, p in SEAT_PEER.items():
            r = first[p]["recs"][marks[p]]
            res["seats"][s] = {"peer": p, "dpid": dpid[s], "start": r.get("start"), "team": r.get("team"),
                               "record": {q: record_of(first[q], dpid[s]) for q in PEERS}}
    for p in PEERS:
        if p not in marks:
            continue
        turns = netai_check.calls(texts[p])
        if set(turns) != {marks[p]} or not turns[marks[p]]:
            bad.append(f"{p}: ImpAI's turns at its level end {turns or 'none'}, not for its own seat "
                       f"{marks[p]} alone")
        else:
            seat = next(s for s, q in SEAT_PEER.items() if q == p)
            res["seats"].get(seat, {})["calls"] = turns[marks[p]]
    if only is not None:
        fp = {p: {"own": stem_lines(texts[p], only[p]), "other": stem_lines(texts[p], only[o])}
              for p, o in (("H", "J"), ("J", "H"))}
        for p in PEERS:
            res["peers"].setdefault(p, {})["fingerprint"] = fp[p]
            if fp[p]["other"]:
                bad.append(f"{p}: {fp[p]['other']} lines of its log begin with a stem only the other peer's DLL "
                           f"holds")
        if not fp["H"]["own"] + fp["J"]["own"]:
            bad.append("no line of either log begins with a stem only one of the two DLLs holds: nothing tells "
                       "the builds apart")
    res["failed"] = bad
    if len(dpid) != 2:
        res["reason"] = bad[0]
        return res
    vh, vj = res["peers"]["H"], res["peers"]["J"]
    for s, p in SEAT_PEER.items():
        got = last_pulse(plays[p], marks[p], res["peers"][p]["t"])
        if got:
            res["pulse"][s] = got
    res["lead"] = lead(res["pulse"])
    said = {p: f"{res['peers'][p]['outcome']} at GameTime {res['peers'][p]['t']}" for p in PEERS}
    for p in PEERS:
        if res["peers"][p]["outcome"] == "left":
            res["reason"] = f"{p}: {said[p]}: seat {res['peers'][p]['who']}'s record is freed there: its peer left"
            return res
    if vh["outcome"] != vj["outcome"]:
        res["outcome"] = "desync"
        res["reason"] = f"the peers disagree: the host's records say {said['H']}, the joiner's {said['J']}"
        return res
    o = vh["outcome"]
    if o in ("ended", "unfinished"):
        why = {"ended": "its level ended with both AI seats holding units" + (
                   "" if no_humans else " (THE HUMANS STAY: the engine ends a network game once no human holds "
                                        "a unit)"),
               "unfinished": "its looks stop before an outcome and before the cap"}[o]
        res["reason"] = f"both peers: {said['H']} and {said['J']}: {why}"
        return res
    if o in ("A", "B", "draw") and abs(vh["t"] - (vj["t"] - ahead)) > SLACK:
        res["outcome"] = "desync"
        res["reason"] = (f"the peers disagree on when: the host's records say {said['H']}, the joiner's "
                         f"{said['J']}, more than {SLACK} ticks apart with the joiner's clock {ahead} ahead")
        return res
    if bad:
        res["reason"] = bad[0]
        return res
    said_cap = "the cap"
    if o == "cap":
        value, why = cap_value(at_cap, cap)
        if value is None:
            res["outcome"], res["reason"] = "desync", why
            return res
        res["value"] = value
        if value:
            res["lead"] = lead(value)
            said_cap += f", value A {value['A']['live']:.0f}, B {value['B']['live']:.0f} on both peers"
        else:
            said_cap += f", with no value ({why})"
    res["outcome"], res["draw"] = ("cap", o == "draw") if o in ("cap", "draw") else (o, False)
    res["end_tick"] = cap if o == "cap" else vh["t"]
    res["reason"] = {"cap": said_cap, "draw": "both seats gone at one look"}.get(
        o, f"seat {'B' if o == 'A' else 'A'} held no unit on both peers")
    return res


# ---- the game ----

def map_of(name: str) -> tuple:
    """(bar.py's key, the map's own name) of `name`, either of the two."""
    for k, v in bar.MAPS.items():
        if name in (k, v):
            return k, v
    raise ValueError(f"not one of bar.py's maps: {name!r} ({', '.join(bar.MAPS)})")


def look(tp, name: str, recs, seating: bool = False) -> dict:
    """One look at `name`, read back as the judge reads it from the log. A peek another tool makes of the
    same instance at the same moment can take this one's answer, so a short answer is asked again."""
    from twopeers import CouldNotRun
    specs = look_specs(recs, seating)
    for _ in range(3):
        o = tp.tacli("peek", name, *specs, "--json", timeout=60)
        rows = json.loads(o[o.index("{"):])["peeks"] if "{" in o else []
        if len(rows) == len(specs):
            text = "".join(f"peek: {p['spec']} @0x0 = {str(p['value']).split()[0]} \n" for p in rows)
            return looks(text)[0]
    raise CouldNotRun(f"{name}: three looks in a row came back short of their {len(specs)} peeks")


def stands(tp, recs: dict, seen: dict) -> bool:
    """Whether both peers' GameTime comes to stand, two looks in a row equal on each, within STAND_LOOKS looks."""
    for _ in range(STAND_LOOKS):
        time.sleep(STAND_S)
        for n in recs:
            seen[n].append(look(tp, n, recs[n]))
        if all(seen[n][-1]["t"] == seen[n][-2]["t"] == seen[n][-3]["t"] for n in recs):
            return True
    return False


def read_cap(tp, first: dict, dpid: dict, recs: dict, seen: dict) -> tuple:
    """THE VALUE AT THE CAP's read: ({peer: {"t", "alive", "value"}, "paused": the peer whose Pause key
    stopped the game, or None}, {peer: its units}). The game runs on at its end."""
    peers = dict(zip(PEERS, (tp.host, tp.join)))
    paused = None
    for p, n in peers.items():
        tp.tacli("keys", n, "pause")
        if stands(tp, recs, seen):
            paused = p
            break
    at_cap, units = {"paused": paused}, {}
    for p, n in peers.items():
        blocks = {}
        for s in ("A", "B"):
            i = record_of(first[n], dpid[s])
            blocks[s] = (i, first[n]["recs"][i]["first"], first[n]["recs"][i]["last"])
        read = cap_units(tp, n, blocks)
        try:
            value = peer_value(read, blocks, type_costs(tp, n))
        except ValueError as e:
            from twopeers import CouldNotRun
            raise CouldNotRun(f"{n}: no value of a seat's: {e}") from None
        at_cap[p] = {"t": read["t"], "alive": read["alive"], "value": value}
        units[p] = read["units"]
    if paused:
        tp.tacli("keys", peers[paused], "pause")
    return at_cap, units


def full_speed(tp):
    """THE SPEED: the game's `+` key, SPEED_KEYS times on each peer."""
    for name in (tp.host, tp.join):
        tp.tacli("keys", name, *["plus"] * SPEED_KEYS)


def seat_room(tp, sides: str):
    """THE HUMANS STAY: each peer's human and its AI row on one team, and each AI seat's side; a peer lists
    itself on row 0. A click steps a team from none to 0 and a second to 1, and two seats that share a team
    even for a moment stay allied one way after one of them steps on (MEASURED: the joiner's seats stepped
    through the host's team 0 to 1, and the host's rows allied all four records for the whole game, which the
    engine ended at GameTime 70). So the host's seats step to team 1 first, each through a team 0 that holds
    no other seat, and the joiner's then take team 0 in one click."""
    tp.add_ais()
    for name, row, clicks, side in ((tp.host, 2, 2, sides[0]), (tp.join, 3, 1, sides[1])):
        for gadget in ("TEAMICONS0", f"TEAMICONS{row}"):
            for _ in range(clicks):
                tp.click(name, gadget, 20)
        if side == "c":
            tp.click(name, f"SIDE{row}", 20)


def watch_room(tp, sides: str):
    """THE HUMANS GONE: the host allows watchers (WATCHING, Disallowed then Allowed), each human takes its
    side gadget to its third stage, Watch, and each AI seat its side; no seat takes a team."""
    tp.add_ais()
    tp.tacli("ui", tp.host, "set", "WATCHING", "1")
    for name, row, side in ((tp.host, 2, sides[0]), (tp.join, 3, sides[1])):
        tp.tacli("ui", name, "set", "SIDE0", "2")
        if side == "c":
            tp.click(name, f"SIDE{row}", 20)


def run(out: Path, a: Path, b: Path, map_name: str, sides: str, cap: int, port, no_humans: bool = False) -> dict:
    from twopeers import STALL_S, CouldNotRun, TwoPeers
    key, name = map_of(map_name)
    t0 = time.monotonic()
    with TwoPeers(out, a, port, "cd", join_dll=b) as tp:
        tp.launch(host_args=["--netai", "impai"], join_args=["--netai", "impai"],
                  levers=(DUEL_LEVER,) if no_humans else ())
        tp.battle_room(name)
        (watch_room if no_humans else seat_room)(tp, sides)
        tp.start()
        full_speed(tp)
        t1 = time.monotonic()
        peers = (tp.host, tp.join)
        first = {n: look(tp, n, range(RECORDS), seating=True) for n in peers}
        recs = {n: sorted(i for i, r in first[n]["recs"].items() if r["active"]) for n in peers}
        ai = {n: [r["dpid"] for r in first[n]["recs"].values() if r["active"] and r["control"] == 2]
              for n in peers}
        if any(len(v) != 1 for v in ai.values()):
            raise CouldNotRun(f"the peers' own computer seats at the first look: {ai}, not one each")
        dpid = {"A": ai[tp.host][0], "B": ai[tp.join][0]}
        seen = {n: [first[n]] for n in peers}
        last, moved, ended_at = -1, time.monotonic(), None
        while True:
            time.sleep(NEAR_S if seen[tp.host][-1]["t"] >= cap - NEAR else LOOK_S)
            for n in peers:
                seen[n].append(look(tp, n, recs[n]))
            views = {tp.host: view(seen[tp.host], dpid, cap), tp.join: view(seen[tp.join], dpid, None)}
            if views[tp.host]["outcome"] == "cap":
                break                   # THE CLOCKS: the cap is the host's, and its read follows at once
            done = [n for n in peers if views[n]["outcome"] != "unfinished"]
            if done and ended_at is None:
                ended_at = time.monotonic()
            # the end one peer sees reaches the other's records a packet later: both are looked at until both
            # show an end, so a peer that never does is the judge's disagreement and not a look that came early
            if len(done) == 2 or (ended_at and time.monotonic() - ended_at > SETTLE_S):
                break
            now = seen[tp.host][-1]["t"]
            if now != last:
                last, moved = now, time.monotonic()
            elif time.monotonic() - moved > STALL_S:
                break                   # a level that ended on its own: the judge names it from the logs
        t2 = time.monotonic()
        end = seen[tp.host][-1]["t"]
        at_cap = None
        if views[tp.host]["outcome"] == "cap" and views[tp.join]["outcome"] == "unfinished":
            at_cap, units = read_cap(tp, first, dpid, recs, seen)
            (out / "cap-units.json").write_text(json.dumps(units) + "\n")
        for n in peers:
            tp.leave(n)
        texts = {p: tp.run_log(n) for p, n in zip(PEERS, peers)}
        port = tp.port
    dlls = {"H": a, "J": b}
    data = {p: d.read_bytes() for p, d in dlls.items()}
    res = judge(texts["H"], texts["J"], cap, only_stems(data["H"], data["J"]), at_cap, no_humans)
    res["cap_read"] = at_cap
    for s, p in SEAT_PEER.items():
        res["seats"].setdefault(s, {}).update(dll=dlls[p].name, sha256=hashlib.sha256(data[p]).hexdigest())
    if tp.refused:                      # the peers have stopped (TwoPeers.refused)
        res["outcome"], res["reason"] = "could not run", tp.refused[0]
        res["failed"] += tp.refused
    import runlock
    res.update(id=f"duel-{key}-{sides}", pair="duel", map=key, sides={"A": bar.SIDE[sides[0]], "B": bar.SIDE[sides[1]]},
               port=port, slots=GAMES * runlock.lane_slots("gdi"), launch_s=round(t1 - t0, 1),
               play_s=round(t2 - t1, 1), ticks_per_s=round(end / (t2 - t1), 1))
    return res


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--a", type=Path, required=True, help="the host's ddraw.dll: seat A's brain")
    p.add_argument("--b", type=Path, required=True, help="the joiner's ddraw.dll: seat B's brain")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--map", default="lavamania", help="one of bar.py's maps, by key or name (default lavamania)")
    p.add_argument("--sides", default="aa", choices=bar.BLOCK_SIDES, help="A's side and B's: a Arm, c Core")
    p.add_argument("--cap", type=int, default=CAP, help=f"the host's GameTime the game plays to (default {CAP})")
    p.add_argument("--port", type=int, default=None, help="this DirectPlay port (default: the first free lease)")
    p.add_argument("--no-humans", action="store_true", help="THE HUMANS GONE: both humans watch, under the duel lever")
    a = p.parse_args(argv)
    try:
        map_of(a.map)
    except ValueError as e:
        p.error(str(e))
    import runlock
    import tareplay

    def stop(_signum, _frame):          # a stop from outside leaves through TwoPeers' exit: no game is left
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)
    runlock.hold(TREE, "codeduel " + " ".join(sys.argv[1:]), GAMES, gpu=False)
    why = runlock.together(GAMES, "codeduel")           # the peers meet in one lobby: both seats at once
    if why:
        print(f"could not run: {why}", file=sys.stderr)
        return 2
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    print(f"evidence: {out}", flush=True)
    try:
        res = run(out, a.a.resolve(), a.b.resolve(), a.map, a.sides, a.cap, a.port, a.no_humans)
    except (OSError, ValueError, subprocess.SubprocessError, tareplay.CouldNotRun) as e:
        print(f"COULD NOT RUN: {e}; inspect {out}", file=sys.stderr)
        return 2
    (out / "result.json").write_text(json.dumps(res, indent=1) + "\n")
    for s in ("A", "B"):
        st, v, pu = res["seats"].get(s, {}), res["value"].get(s), res["pulse"].get(s)
        print(f"  seat {s}: {st.get('dll')} on peer {st.get('peer')}, start {st.get('start')}, "
              f"{st.get('calls')} turns" + (f", its last pulse {pu['live']:.0f} at GameTime {pu['t']}" if pu else "")
              + (f", value at the cap {v['live']:.2f} at its GameTime {v['at']} ({v['other']:.2f} on the other "
                 f"peer)" if v else ""))
    if res.get("cap_read"):
        print(f"  the cap's read: {json.dumps(res['cap_read'])}")
    for q in PEERS:
        v = res["peers"].get(q, {})
        print(f"  peer {q}: {v.get('outcome')} at GameTime {v.get('t')}, last look {v.get('last')}, "
              f"fingerprint {v.get('fingerprint')}")
    print(f"  the clocks at the last round of looks: {res['clock']}")
    for f in res["failed"]:
        print(f"FAIL: {f}")
    print(f"{res['outcome'].upper()}: {res['reason']}; end tick {res['end_tick']}, lead {res['lead']}; "
          f"{res['launch_s']} s to the game, {res['play_s']} s of play at {res['ticks_per_s']} ticks a second, "
          f"{res['slots']} slots")
    return 0 if res["outcome"] in ("A", "B", "cap") else 1 if res["outcome"] == "desync" else 2


if __name__ == "__main__":
    sys.exit(main())
