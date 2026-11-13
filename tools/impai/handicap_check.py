#!/usr/bin/env python3
"""handicap_check -- ImpAI's exemption from the Difficulty handicap, measured in the engine.

    tools/impai/handicap_check.py <instance> [--difficulty easy,medium,hard] [--gpu]

One launch a Difficulty, one at a time under the run lock (tools/runlock.py), each of
scenarios/impai-handicap.json with its `difficulty` replaced: an idle human seat, seat 1 a CORE
computer seat OTA AI plays, seat 2 a CORE computer seat ImpAI plays (`seatai=2`). The launch is
held (tagpu_hold.on, THE LAUNCH'S HOLD), advanced to GameTime 61 by exact ticks, and the two
records' last economy pass read in one peek: energy income +0x90 and metal income +0x9C, floats
(the economy pass 0x401360 writes them every 30 ticks, every seat on the same tick; scout H1).
Both commanders are the same side and nothing they could build finishes by tick 61 [INFERRED: the
cheapest structure's buildtime over a commander's workertime is several hundred ticks], so the
commanders' own terms are the whole income, and the Difficulty scales them for OTA AI only
(research/notes/engine/sim.md, Difficulty and the AI handicap):

    easy    OTA AI's == ImpAI's x 0.5, bit for bit (x * 0.5 is exact in a float)
    medium  |OTA AI's - 0.7 x ImpAI's| <= 1e-6 x ImpAI's
    hard    bit for bit equal

ImpAI's energy income must be above 0; a metal income of 0 on both seats (a commander with no
metalmake) passes and is reported. Each launch also checks the setup it measures: main+0x37EEE
holds the Difficulty, records 1 and 2 are controller 2, the level init's line names seat 2 only,
the seat table marks record 2 and not record 1, the sixteen handicap sites begin with E9 and
the Deathmatch respawn's two type tests, rows 15 and 16, are stock (80)
(tagpu_impai.c).

Then the rows that need more than the commanders. The launch applies the `impai-handicap` test
group's economy structures (impai-handicap-economy.json: a solar collector, a wind generator, a
metal extractor, a metal maker and a tidal generator for each computer seat) and two Fleas for the
human seat (impai-handicap-purge.json), orders each Flea to attack the computer seat's commander
nearest it, and the human seat gives each computer seat 100 energy and 100 metal (`+give`, the
console's Give, mask 1). Every handicap stub counts the type-2 records that take each of its exits
(tagpu_impai.c, s_diag.hits, at the address the attach line names); the level is advanced until
every row of REACHED -- the economy's 1 to 5, 7 and 8, the transfers' 13 and 14 and the purge's
18 -- has sent a record down both, at most ROWS_TICKS ticks. Seat 1 is the only unmarked type-2
record and seat 2 the only marked one, so a scaled count is OTA AI's and an exempt one ImpAI's. A row
counts once per pass of its site, so a seat with more units of a kind counts more on that row (ImpAI
builds a second wind generator early: at Hard, GameTime 961, row 4 counted 30 scaled and 53 exempt);
the check asks only that each exit is taken.

Row 18 is the purge's type test 0x406FC3 in the on-hit handler 0x406F80: a hit on a unit whose def has
cancapture (+0x245 bit 12, 0x406FA8..0x406FB7) and whose owner record is seated (0x406FB9..0x406FC1)
takes the scaled continuation when the owner's +0x73 is 2 (0x406FC3..0x406FC7: the planner pause
0x406FC9..0x406FEE and the purge 0x439EB0 at 0x406FF1). Nothing in 0x406F80..0x406FF6 reads the
Difficulty at main+0x37EEE, so OTA AI's commander takes it at every Difficulty, Hard included, and the
row needs a hit to land on that commander. The Fleas' line of sight decides whether one does. Under
true line of sight a Flea fires only at what its own player sees, and its sight is 263, so it walks in
past its range, 300, toward the D-gun's 240, which the stock AI fires by itself (the weapon scan's
argument 1; engine/orders.md): MEASURED 2026-11-13 at Hard, the Flea ordered onto OTA AI's commander
stood 276 from it at GameTime 181 with row 18's scaled count 0 and was dead by 241, never having hit,
so the rows ran to their bound at 961 and the Flea walk began there. The check launches with permanent
line of sight (the scenario's `los`, set in the launch's copy only), and that does not keep a Flea out
of the D-gun's reach either: MEASURED 2026-11-13 at Medium, the Flea on OTA AI's commander stood 276
from it at GameTime 181 and about 221 at 241, the commander having walked toward it, with row 18's
scaled count still 0, and was dead by 301. A Flea of 75 hit points dead within one 60-tick read at more
than the laser's 200 is the D-gun's. So row 18's scaled count on OTA AI's seat is a race between the
Flea's first hit and the stock AI's D-gun, which the Flea usually wins (Easy and Hard at 8e6936f1) and
can lose. That race is an accepted limit of the check, the owner's decision of 2026-11-13
(research/notes/impai/decisions.md): a Flea that dies before its seat has counted fails the check at
once, by name (fleas_lost), and a re-run is the answer. No surer hitter exists: a seat's own striker
cannot be certain (engine/weapons.md, The ballistic reach and A line-of-sight shot).

Then the Fleas are ended (fleas_killed). A Flea's beam, range 300, outranges both of a CORE
commander's weapons, its laser, 200, and its D-gun, 240 (ARMFLEA_WEAPON, CORCOMLASER,
CORE_DISINTEGRATOR), so a Flea that attacks one stands where neither reaches it and lives: it is
walked onto its commander, its order re-aimed at the commander's position every REAIM ticks, until the
commander's laser, 70 a shot every second against the Flea's 75, ends it, within FLEA_TICKS.

Then the reclaims (rows 11 and 12, the payout; 17, the reclaimed unit's credit), which need a seat's own
order to hold, applied once both Fleas are dead so that no seat has an enemy in reach:
impai-handicap-reclaims.json gives each computer seat a Fark (ARMFARK) beside a Dragon's Teeth feature
and another beside a radar tower (CORRAD) of its own, each ordered once to reclaim it. The unit is no
Dragon's Teeth: CORDRAG's FBI has isfeature 1, and the create of a finished unit with it sets its death
kind +0xF5 to 7 and +0x110 bit 14 (0x486161..0x486171), which the unit tick's reaper makes a death and a
feature at once (0x48AFB9..0x48AFD1); v2's roster listed every kbot and no CORDRAG.

The reclaimer is a Fark because nothing re-orders it on either seat while it reclaims, and a reclaim
that is re-ordered does not pay. OTA AI's builder planner 0x408100, due every 90 ticks (GameTime + 90 at
0x40811B), replaces the head order of every builder of its group 4 (0x43ADC0 with no Shift,
0x408292..0x4082B0) unless the builder's def has a build-list count of 0 at +0x152 (0x408143..0x408151)
or its head has +0x42 bit 0x8 (0x408198); its second loop orders only an idle one, no head or a head
with +0x42 bit 0x4000, to patrol (0x4082E1..0x408421). A Fark is no structure and has the builder bit,
so the group assignment 0x408830 puts it in group 4 (0x408894..0x4088FA), and the one rebalance,
0x407560, moves units only out of groups 3 and 7 (engine/orders.md, The ten slots); its count is 0,
since it has no [CANBUILD] entry and no download entry (engine/units.md, A builder's build list; the
install's sidedata.tdf and download TDFs read). ImpAI orders only an idle builder (impai_brain.c,
idle_head), and its military only the units it classes as army, never a builder (impai_table.c,
impai_class_of). A replace is fatal to a feature's reclaim: the countdown is the order node's +0x36
(0x404BBA..0x404BDC), so it starts over, and the node's destructor starts StopBuilding (0x43A251), which
folds the arms back before the new order's StartBuilding can unfold them. A construction kbot's fold and
unfold are 83 and 68 ticks of sleeps (CORCK's close() and open(); a sleep is ms x 30 / 1000 ticks,
truncated, engine/cob.md), so a reclaim re-issued every 45 ticks started over before it paid (MEASURED
2026-11-13, at every Difficulty: ImpAI's CORCK stood beside its feature with the Reclaim at its head for
3600 ticks and never paid).

The Fark and not CORE's own count-0 builder, the Necro (CORNECRO), which reclaims nothing here. The
resolver's reclaim case turns a reclaim of a feature into RESURRECT when the reclaimer's def has
canresurrect (+0x245 bit 0x800, 0x43F5A5), the feature's cell is in the viewed player's sight (the
explored word main+0x14273, bit main+0x2A43, 0x43F5D1..0x43F5FF) and the FeatureDef's +0xFE has bit 7,
reclaimable (0x43F609; engine/orders.md). All three hold here: the scenario sets `"mapping": true`, so
every cell is explored, and the Dragon's Teeth is reclaimable. RESURRECT's handler 0x404DB0 never
reaches the payout 0x4237D0; and
its FBI has no builddistance, so +0x212 is 0 and ReclaimUnit's range test (0x404953: the squared
distance against (+0x212 + the target def's +0x184) squared) leaves it short of the radar tower
[INFERRED from the code; the step that fails was not traced]. MEASURED 2026-11-13, 900 ticks at Easy and
Medium: both feature Necros' reclaims ended without a payout, OTA AI's then at action 34 (RepairPatrol
by the descriptors' name order [INFERRED]) and ImpAI's at Standby, and both unit Necros held ReclaimUnit with their radar towers whole. The Fark
has no canresurrect and a builddistance of 60, and the check names each reclaimer's head order right
after the apply, so an order that is not Reclaim and ReclaimUnit fails there by name.

Once it holds, each reclaim is short: the walk, at most 64 wu at 2.1 a tick (ARMFARK's maxvelocity)
after 12 ticks of acceleration, 37 ticks; the unfold, 23 ticks of sleeps in activatescr() and a torso
turn of at most 180 degrees at 160 a second, 34; then the Dragon's Teeth's countdown, 15 + (energy 0 +
metal 8) / 2, two a working call two ticks apart, 20 ticks (0x404BBA..0x404C89, engine/orders.md), or
the radar tower's 51 hit points at 180 x 51 x 15 / (50 x 300) = 9 a hit (0x438650, engine/weapons.md), a
hit every 16 ticks (the counter +0x3A gains 2 a call, a call every 2 ticks, and fires at 15:
0x40496E..0x404A62), six hits, 96. The level is advanced until rows 11, 12 and 17 have each sent a
record down both, at most RECLAIM_TICKS ticks; a row still lacking a seat then has each seat's feature
cell, Farks and radar tower read and printed (reclaim_state). At every Difficulty the compare a row
counts at comes before the Difficulty's own switch (rows 11 and 12 at 0x4238C4 and 0x423918, 17 at
0x486CDA; the switch on main+0x37EEE follows, Hard taking its unscaled amount), so OTA AI's record
reaches the scaled count at Hard too.

Every row's counts are printed, and the rows this test does not reach are named with their
reason: 9 and 10, the refunds of a cancelled build and of an unbuild, which no step here scripts;
6 needs a mobile unit with a negative EnergyUse, which no unit of the retail game has, and
handicap_row6.py reaches it on Escalation. Rows 15 and 16 have no site: ImpAI leaves the
Deathmatch respawn's grants stock (tagpu_impai.c, HSITES).

Last, the AI command, with cheats on (its mask is 4): `+ai 2` makes ImpAI's seat a human's
(0x4162FE) and clears its mark, and a second `+ai 2` hands it to the stock AI (0x4162F4) with
the mark still clear. Exit 0 when every Difficulty passes, 1 on a failed check, 2 when a launch
could not be run.

The games render with GDI (tagpu_gdi.on in the game folder, put back as the run found it however the
run ends) and take no GPU token; --gpu renders them with the default renderer and takes one.
"""
import argparse
import json
import re
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

TREE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TREE / "tools"))
from resource_regression import TACLI, Check, hold_instance, tacli_api  # noqa: E402

SCENARIO = TREE / "scenarios/impai-handicap.json"
LOAD_S = 900                            # a launch to a live map, a cold prefix included
LEVELS = {"easy": 0, "medium": 1, "hard": 2}
AT = 61                                 # two economy passes in, nothing built yet
ROWS_TICKS = 900                        # the rows' bound: 30 economy passes
STEP = 60                               # ticks between two reads of the counts: two passes
OTA, IMPAI = 1, 2                       # the scenario's records
STRUCTURES = "impai-handicap-economy.json"
ATTACKERS = "impai-handicap-purge.json"
ROSTER_S = 30                           # the roster is logged every 5 s (tagpu_packet_pub.c)
REACHED = (1, 2, 3, 4, 5, 7, 8, 13, 14, 18)  # structures, commanders, gives and the Fleas' hits
RECLAIMS = "impai-handicap-reclaims.json"
RECLAIMED = (11, 12, 17)                # the rows a seat's own reclaim orders reach
# The reclaims' bound. Once it holds, the longer reclaim, the radar tower's, is at most 37 + 23 + 34 + 96 =
# 190 ticks (the docstring) and the feature's 114; the order arrives with the apply, on the held level. The
# turns a walk makes on the way are not in the sum, and 900 leaves more than four times it.
RECLAIM_TICKS = 900
REAIM = 30                              # ticks between two re-aims of a Flea at its commander
# The Fleas' bound. When the rows are reached each Flea has hit its commander, so it attacks from within
# its range, 300, or nearer (the docstring: a Flea closed to 221 of one), and a re-aim targets
# where the commander stood at most REAIM ticks before, at most 32 px off at CORCOM's 1.07 a tick.
# Closing 3 - 1.07 = 1.93 a tick from 332 to the laser's 200 takes at most 69 ticks, the Flea's 21 of
# acceleration (0.14 a tick to 3) and one re-aim on top: 120. Two laser hits, 70 each against its 75,
# one a second: 60 more, and 30 for the laser to turn and fire its first. 210 in all; FLEA_TICKS
# leaves 90 over it. A Flea alive past it falsifies this derivation from the defs and the weapon scan's
# code, a finding about the engine, so it is a FAIL by name (fleas_killed), not a launch that could
# not run; the reclaims then do not run, an enemy being in reach.
FLEA_TICKS = 300
RECLAIMER = "ARMFARK"                   # a builder with no build list: no planner re-orders it (docstring)
# the reclaims' Farks and targets as the scenario places them: (seat, the feature's Fark, the
# feature's world point, the unit's Fark, the unit's world point)
RECLAIM_PAIRS = ((OTA, (7056, 1744), (7056, 1808), (7136, 1744), (7136, 1808)),
                 (IMPAI, (5896, 6776), (5896, 6840), (5976, 6776), (5976, 6840)))
NEAR = 40                               # world units: a roster unit at a placed point
NOT_HERE = {6: "no retail unit is mobile with a negative EnergyUse (handicap_row6.py: "
                "Escalation)",
            9: "no step here cancels a build",
            10: "no step here unbuilds"}
ATTACH = re.compile(r"impai: seam .* in; seats at 0x([0-9A-F]{8}), handicap counts at "
                    r"0x([0-9A-F]{8})")
REC = 0x1B63                            # record 0 in main; 0x14B a record (tagpu_engine.h)
# what a missing reclaim row's report reads (tagpu_engine.h): the feature grid, 13 bytes a 16-px cell, its
# u16 at +8 a feature index below 0xFFFB, or 0xFFFE for a cell of a larger feature's footprint, any other
# value none (the payout's own tests, 0x42381E..0x42383F and 0x423868); the unit array, 0x118 a slot,
# its order list +0x5C (a node's action byte +4), the build fraction remaining +0x104 and the state +0x110
OFF_MAP_W16, OFF_FEATMAP, FT_STRIDE = 0x14233, 0x14287, 0x0D
OFF_UNIT_BEGIN, UNIT_STRIDE = 0x14357, 0x118
U_ORDERS, U_NANO, U_STATE, ST_ALIVE = 0x5C, 0x104, 0x110, 0x10000000
# +0x110 bit 14, pending death: 0x489BB0's hit sets it when the HP reaches 0 (0x489EE2) and the unit tick
# reaps the unit later, its alive bit set until then (engine/memory.md); the applier refuses an order to it
# (tagpu_scenario.c, live_by_index)
ST_DYING = 0x4000
# the order descriptors (*0x512344, stride 0x19, to *0x512348): a node's action byte (+4) indexes them,
# and +0x15 is the name 0x438760 looks an action up by
DESC_BEGIN, DESC_END, DESC_STRIDE, DESC_NAME = 0x512344, 0x512348, 0x19, 0x15
U_X, U_Z = 0x6C, 0x74                   # the position's integer halves, world units (engine/units.md)
STRIDE = 0x14B
DIFFICULTY = "*0x511DE8+0x37EEE"
ROWS = (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 17, 18)   # the rows with a site
HIT_ROWS = 18                           # s_diag.hits[IMPAI_ROWS][2], u32 (tagpu_impai.c)
HITS_BYTES = HIT_ROWS * 2 * 4
PEEK_MAX = 256                          # bytes one x<N> read may ask (tagpu_peek.c)
SITES = (0x401435, 0x4014B2, 0x4014F3, 0x40157F, 0x4015F3, 0x40167C, 0x4016F2, 0x401753,
         0x40269F, 0x41BBB1, 0x4238C4, 0x423918, 0x464C06, 0x464D36, 0x486CDA, 0x406FC3)
STOCK = (0x465437, 0x4654A3)            # rows 15 and 16, `cmp byte [eax+0x73],2`: 80 78 73 02


def field(rec, off, size=4):
    return f"*0x511DE8+0x{REC + rec * STRIDE + off:X}:{size}"


def as_float(u32):
    return struct.unpack("<f", struct.pack("<I", u32))[0]


def float32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


class Held(Check):
    """resource_regression's lockstep, with no fixture roster to wait for."""

    def __init__(self, name):
        self.name = name
        self.lockstep()

    def type(self, line):
        """A chat line as tareplay's `type` step sends it: one return opens the chat, the
        characters, return sends it; each call returns once the engine has consumed its events."""
        self.keys("return")
        self.keys(*("space" if ch == " " else f"char:{ch}" for ch in line))
        self.keys("return")


def hits_spec(at):
    """The peek of the whole count table at `at`: one x<N> read, N its size. tagpu_peek.c parses
    N as hex (parse_hex) and clamps it to PEEK_MAX, so the 144 bytes are `x90`."""
    assert HITS_BYTES <= PEEK_MAX
    return f"0x{at:08X}:x{HITS_BYTES:X}"


def counts(c, at):
    """Each row's (scaled, exempt) counts, a pair a row id, read in one peek of the table."""
    raw = c.bytes(hits_spec(at))
    if len(raw) != HITS_BYTES:
        raise OSError(f"the count table's peek answered {len(raw)} bytes, not {HITS_BYTES}")
    v = struct.unpack(f"<{HITS_BYTES // 4}I", raw)
    return {row: (v[2 * (row - 1)], v[2 * (row - 1) + 1]) for row in ROWS}


def both(hits, rows):
    return sum(hits[r][0] for r in rows) > 0 and sum(hits[r][1] for r in rows) > 0


def run(api, instance, level, out, gpu=False):
    """One launch at `level`, with GDI unless `gpu`; the list of failed checks."""
    gamedir = api.Instance(instance).gamedir
    gdi = gamedir / "tagpu_gdi.on"
    had_gdi = gdi.exists()                  # the lever is its presence alone (tagpu_cfg.c)
    gdi.unlink(missing_ok=True)
    if not gpu:
        gdi.write_text("")
    lever = gamedir / "tagpu_hold.on"
    had = lever.exists()
    doc = json.loads(SCENARIO.read_text())
    doc["setup"]["difficulty"] = level
    doc["setup"]["los"] = "permanent"       # row 18: a Flea fires from its range, 300 (the docstring)
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        scn = Path(tmp) / f"impai-handicap-{level}.json"
        scn.write_text(json.dumps(doc))
        lever.write_text("")
        try:
            r = subprocess.run([sys.executable, str(TACLI), "scenario", "load", instance, str(scn),
                                "--defaults", "--restart"], capture_output=True, text=True,
                               timeout=LOAD_S)
            if r.returncode:
                raise OSError(f"scenario load: {(r.stdout + r.stderr).strip()[-400:]}")
            c = Held(instance)
            c.hold()
            if c.tick < AT:
                c.advance(AT - c.tick)
            specs = [field(OTA, 0x90), field(IMPAI, 0x90), field(OTA, 0x9C), field(IMPAI, 0x9C),
                     field(OTA, 0x73, 1), field(IMPAI, 0x73, 1), DIFFICULTY]
            e_ota, e_imp, m_ota, m_imp, c1, c2, diff = c.values(*specs)
            ops = c.values(*(f"0x{va:X}:1" for va in SITES + STOCK))
            text = api.talog.run_text(gamedir).splitlines()
            seats = [ln for ln in text if "impai: seats" in ln or "impai: unavailable" in ln]
            where = next((m for m in map(ATTACH.search, text) if m), None)
            if where is None:
                raise OSError("no `impai: seam ... in; seats at ...` line: ImpAI is not in")
            table, hits_at = (int(g, 16) for g in where.groups())
            marks = c.values(f"0x{table + OTA:08X}:1", f"0x{table + IMPAI:08X}:1")
            rows_fails, fleas = rows_reached(c, instance, hits_at, out)
            fails += rows_fails
            fails += reclaims_reached(c, instance, hits_at, fleas, out)
            fails += ai_command(c, table, out)
        finally:
            try:
                subprocess.run([sys.executable, str(TACLI), "stop", instance],
                               capture_output=True, text=True, timeout=300)
            finally:
                gdi.unlink(missing_ok=True)
                if had_gdi:
                    gdi.write_text("")
                if not had:
                    lever.unlink(missing_ok=True)
    if marks != [0, 1]:
        fails.append(f"the seat table marks records 1 and 2 as {marks}, not [0, 1]")
    e_ota, e_imp, m_ota, m_imp = map(as_float, (e_ota, e_imp, m_ota, m_imp))
    out.append(f"  {level}: GameTime {AT}, energy OTA {e_ota!r} ImpAI {e_imp!r}, "
               f"metal OTA {m_ota!r} ImpAI {m_imp!r}")
    if diff != LEVELS[level]:
        fails.append(f"main+0x37EEE holds {diff}, not {LEVELS[level]}")
    if (c1, c2) != (2, 2):
        fails.append(f"records 1 and 2 are controllers {c1} and {c2}, not 2 and 2")
    if not any("impai: seats 2 (impure.cfg)" in ln for ln in seats):
        fails.append(f"the level init's line is {seats[-1] if seats else 'missing'}, "
                     f"not `impai: seats 2 (impure.cfg) ...`")
    unpatched = [f"0x{va:X}" for va, op in zip(SITES, ops) if op != 0xE9]
    written = [f"0x{va:X}" for va, op in zip(STOCK, ops[len(SITES):]) if op != 0x80]
    if written:
        fails.append(f"the respawn's sites are not stock: {', '.join(written)}")
    if unpatched:
        fails.append(f"sites not patched: {', '.join(unpatched)}")
    if not e_imp > 0:
        fails.append(f"ImpAI's energy income is {e_imp!r}")
    for what, ota, imp in (("energy", e_ota, e_imp), ("metal", m_ota, m_imp)):
        if level == "easy":
            ok = ota == float32(imp * 0.5)
            want = "half of ImpAI's, exactly"
        elif level == "medium":
            ok = abs(ota - 0.7 * imp) <= 1e-6 * abs(imp)
            want = "0.7 of ImpAI's"
        else:
            ok = ota == imp
            want = "ImpAI's, exactly"
        if not ok:
            fails.append(f"{what}: OTA AI's {ota!r} is not {want} ({imp!r})")
        elif ota == imp == 0:
            out.append(f"  {level}: {what} income 0 on both seats (passes trivially)")
    return fails


def apply(instance, name):
    r = subprocess.run([sys.executable, str(TACLI), "scenario", "apply", instance,
                        str(TREE / "scenarios" / name)], capture_output=True, text=True,
                       timeout=300)
    if r.returncode:
        raise OSError(f"scenario apply {name}: {(r.stdout + r.stderr).strip()[-400:]}")


def attack_commanders(c, out):
    """Each Flea ordered to attack the CORE commander nearest it, once the roster lists both; each Flea's
    engine index with its commander's and the commander's seat."""
    deadline = time.monotonic() + ROSTER_S
    while True:
        units = c.call("roster", "--json")["units"]
        fleas = [u for u in units if u["type"].upper() == "ARMFLEA"]
        coms = [u for u in units if u["type"].upper() == "CORCOM"]
        if len(fleas) == 2 and len(coms) == 2:
            break
        if time.monotonic() > deadline:
            raise OSError(f"the roster lists {len(fleas)} Fleas and {len(coms)} CORE commanders "
                          f"after {ROSTER_S} s, not 2 and 2")
        time.sleep(1)
    pairs = []
    for f in fleas:
        com = min(coms, key=lambda u: (u["world"][0] - f["world"][0]) ** 2
                  + (u["world"][1] - f["world"][1]) ** 2)
        c.call("order", "attack", "unit", com["engine_index"], "--unit", f["engine_index"],
               "--expect", "ARMFLEA")
        out.append(f"  Flea {f['engine_index']} attacks seat {com['owner']}'s commander "
                   f"{com['engine_index']}")
        pairs.append((f["engine_index"], com["engine_index"], com["owner"]))
    return pairs


def rows_reached(c, instance, hits_at, out):
    """The structures, the Fleas' attacks and the gives, then the level advanced until every
    REACHED row has sent a record down both exits; the list of failed checks and the Fleas with their
    commanders."""
    apply(instance, STRUCTURES)
    apply(instance, ATTACKERS)
    fleas = attack_commanders(c, out)
    for seat in (OTA, IMPAI):
        for what in ("energy", "metal"):
            c.type(f"+give {seat} 100 {what}")
    start = c.tick
    hits = counts(c, hits_at)
    dead = []
    while not all(both(hits, (r,)) for r in REACHED) and c.tick - start < ROWS_TICKS and not dead:
        c.advance(STEP)
        hits = counts(c, hits_at)
        dead = fleas_lost(c, fleas, hits)
    out.append(f"  rows scaled/exempt at GameTime {c.tick}: "
               + " ".join(f"{r}={s}/{e}" for r, (s, e) in hits.items()))
    out += [f"  row {r}: not reached, {why}" for r, why in NOT_HERE.items()]
    short = [r for r in REACHED if not both(hits, (r,))]
    if dead:
        # stopped early: a row still short had not had its bound, so only the Flea's loss failed
        if short:
            out.append(f"  stopped at GameTime {c.tick}, {c.tick - start} ticks in, with rows "
                       f"{', '.join(map(str, short))} not yet counted for both seats")
        return dead, fleas
    fails = [f"row {r} sent no {'OTA AI' if not s else 'ImpAI'} record down its "
             f"{'scaled continuation' if not s else 'unscaled branch'} by GameTime {c.tick} "
             f"(scaled {s}, exempt {e})"
             for r, (s, e) in ((r, hits[r]) for r in short)]
    return fails, fleas


def fleas_lost(c, fleas, hits):
    """The failures of each Flea that is dead while row 18 still lacks its commander's seat: no other unit
    of the check attacks that commander, so the row cannot count the seat any more. A dead Flea's slot holds
    no other unit when it is read: a slot belongs to its owner's unit block (the player record's +0x67..
    +0x6B, engine/units.md), and the human seat, the Fleas' owner, creates nothing until the reclaims'
    apply, which comes after the Fleas are dead."""
    units = c.values(f"*0x511DE8+0x{OFF_UNIT_BEGIN:X}")[0]
    exit_of = {OTA: 0, IMPAI: 1}            # scaled for OTA AI's seat, exempt for ImpAI's
    lost = []
    for f, com, seat in fleas:
        if hits[18][exit_of[seat]]:
            continue
        if not live(c.values(f"0x{units + f * UNIT_STRIDE + U_STATE:08X}")[0]):
            lost.append(f"Flea {f} died by GameTime {c.tick} before it hit seat {seat}'s commander {com}, "
                        f"so row 18 cannot count that seat")
    return lost


def placed(roster, owner, kind, at):
    """The engine index of the roster's `kind` of `owner` within NEAR of world point `at`, or None."""
    for u in roster:
        if (u["owner"] == owner and u["type"].upper() == kind and u["engine_index"]
                and abs(u["world"][0] - at[0]) <= NEAR and abs(u["world"][1] - at[1]) <= NEAR):
            return u["engine_index"]
    return None


def reclaim_units(c):
    """Each pair's kbots and radar tower by engine index, once one roster lists them all:
    the roster is a dump logged every 5 s (tagpu_packet_pub.c) and the level is held, so the first
    dump that lists every unit the apply placed is the held level's."""
    deadline = time.monotonic() + ROSTER_S
    while True:
        roster = c.call("roster", "--json")["units"]
        pairs = [(seat, placed(roster, seat, RECLAIMER, fk), fp, placed(roster, seat, RECLAIMER, uk),
                  placed(roster, seat, "CORRAD", up)) for seat, fk, fp, uk, up in RECLAIM_PAIRS]
        missing = [f"seat {p[0]}" for p in pairs if None in p]
        if not missing:
            return pairs
        if time.monotonic() > deadline:
            raise OSError(f"the roster lacks the reclaims' units of {', '.join(missing)} after "
                          f"{ROSTER_S} s")
        time.sleep(1)


def live(state):
    """A unit the game still plays: alive and not pending its death."""
    return bool(state & ST_ALIVE) and not state & ST_DYING


def head_name(c, unit):
    """The name of the action at the head of the unit at `unit`'s order list, "none" for an empty list; an
    action byte past the descriptor count is printed as its number, never looked up."""
    head = c.values(f"0x{unit + U_ORDERS:08X}")[0]
    if not head:
        return "none"
    act = c.values(f"0x{head + 4:08X}:1")[0]
    beg, end = c.values(f"0x{DESC_BEGIN:08X}", f"0x{DESC_END:08X}")
    if act >= (end - beg) // DESC_STRIDE:
        return f"action {act}"
    name = c.values(f"0x{beg + act * DESC_STRIDE + DESC_NAME:08X}")[0]
    return c.bytes(f"0x{name:08X}:x20").split(b"\0", 1)[0].decode("latin-1")


def reclaim_orders(c, pairs):
    """Each Fark's head order by name, and the failures of the ones that are not the reclaim the rows count:
    Reclaim for the feature's (0x404AD0, whose payout is 0x4237D0), ReclaimUnit for the radar tower's."""
    units = c.values(f"*0x511DE8+0x{OFF_UNIT_BEGIN:X}")[0]
    line, fails = [], []
    for seat, fcons, _fpos, ucons, _rad in pairs:
        for slot, want in ((fcons, "Reclaim"), (ucons, "ReclaimUnit")):
            name = head_name(c, units + slot * UNIT_STRIDE)
            line.append(f"seat {seat} Fark {slot} {name}")
            if name != want:
                fails.append(f"seat {seat}'s Fark {slot} was ordered {name}, not {want}")
    return "  reclaim orders at GameTime " + f"{c.tick}: " + "; ".join(line), fails


def feature_words(c, pairs):
    """Each pair's feature cell word, the def index of the feature at its point (one peek each)."""
    grid, w16 = c.values(f"*0x511DE8+0x{OFF_FEATMAP:X}", f"*0x511DE8+0x{OFF_MAP_W16:X}")
    return [c.values(f"0x{grid + ((fpos[1] >> 4) * w16 + (fpos[0] >> 4)) * FT_STRIDE + 8:08X}:2")[0]
            for _seat, _fcons, fpos, _ucons, _rad in pairs]


def reclaim_state(c, pairs, placed_words):
    """For each pair, what a reclaim that never paid leaves to see: the feature cell's word against its
    word when the reclaims were applied (a paid reclaim of the Dragon's Teeth, a 3DO feature, swaps it
    for its featurereclamate through 0x423710, so a changed word is a paid reclaim and not a standing
    feature), each Fark's state and head order, and the radar tower's state."""
    units = c.values(f"*0x511DE8+0x{OFF_UNIT_BEGIN:X}")[0]
    lines = []
    for (seat, fcons, fpos, ucons, rad), feat, was in zip(pairs, feature_words(c, pairs), placed_words):
        heads = []
        for slot in (fcons, ucons):
            u = units + slot * UNIT_STRIDE
            state = c.values(f"0x{u + U_STATE:08X}")[0]
            heads.append(f"alive, head {head_name(c, u)}" if live(state) else "dead")
        r = units + rad * UNIT_STRIDE
        rstate, rnano = c.values(f"0x{r + U_STATE:08X}", f"0x{r + U_NANO:08X}")
        lines.append(f"  seat {seat}: feature cell {feat:#06x} "
                     f"({'as applied' if feat == was else f'applied as {was:#06x}'}); "
                     f"feature Fark {fcons} {heads[0]}; "
                     f"unit Fark {ucons} {heads[1]}; radar {rad} "
                     f"{'alive' if live(rstate) else 'dead'}, {as_float(rnano):.3f} unbuilt")
    return lines


def fleas_killed(c, fleas, out):
    """Each Flea walked onto its commander until it is dead, its move re-aimed at the commander's position
    (unit +0x6C and +0x74, the integer halves of +0x6A and +0x72) every REAIM ticks, within FLEA_TICKS
    (above). The commander's laser fires on an enemy in its range by itself: it is no CommandFire weapon,
    so the weapon scan 0x4089A0 aims it whatever its argument (0x408A88..0x408A96, engine/orders.md). A
    Flea is dead from its pending-death bit on (live), the bit the applier refuses an order for, and both
    are read on the held level, so no order goes to a Flea the laser has ended, before the walk or during
    it. A Flea's slot holds no other unit in the meantime: a slot belongs to its owner's unit block (the
    player record's +0x67..+0x6B, engine/units.md), and the human seat creates nothing during the walk.
    The failures: a Flea still alive FLEA_TICKS after the walk began (above)."""
    start = c.tick
    units = c.values(f"*0x511DE8+0x{OFF_UNIT_BEGIN:X}")[0]
    state = {f: f"0x{units + f * UNIT_STRIDE + U_STATE:08X}" for f, _com, _seat in fleas}
    while True:
        alive = [(f, com) for (f, com, _seat), v in zip(fleas, c.values(*state.values())) if live(v)]
        if not alive:
            break
        if c.tick - start >= FLEA_TICKS:
            return [f"Flea {', '.join(str(f) for f, _ in alive)} still lives at GameTime {c.tick}, "
                    f"{FLEA_TICKS} ticks after it was walked onto its commander, past the bound FLEA_TICKS "
                    f"rests on"]
        for f, com in alive:
            u = units + com * UNIT_STRIDE
            x, z = c.values(f"0x{u + U_X:08X}:2", f"0x{u + U_Z:08X}:2")
            c.call("order", "move", "pos", x, z, "--unit", f, "--expect", "ARMFLEA")
        c.advance(REAIM)
    out.append(f"  the Fleas dead by GameTime {c.tick}, {c.tick - start} ticks after the walk began")
    return []


def reclaims_reached(c, instance, hits_at, fleas, out):
    """The reclaims, once no Flea is left (fleas_killed), each ordered once by the apply, the level
    advanced until rows 11, 12 and 17 have each sent a record down both; the list of failed checks."""
    alive = fleas_killed(c, fleas, out)
    if alive:
        return alive
    apply(instance, RECLAIMS)
    pairs = reclaim_units(c)
    placed_words = feature_words(c, pairs)
    line, fails = reclaim_orders(c, pairs)
    out.append(line)
    if fails:
        return fails
    start = c.tick
    hits = counts(c, hits_at)
    while not all(both(hits, (r,)) for r in RECLAIMED) and c.tick - start < RECLAIM_TICKS:
        c.advance(STEP)
        hits = counts(c, hits_at)
    out.append(f"  reclaim rows scaled/exempt at GameTime {c.tick}: "
               + " ".join(f"{r}={hits[r][0]}/{hits[r][1]}" for r in RECLAIMED))
    if not all(both(hits, (r,)) for r in RECLAIMED):
        out += reclaim_state(c, pairs, placed_words)
    return [f"row {r} sent no {'OTA AI' if not s else 'ImpAI'} record down its "
            f"{'scaled continuation' if not s else 'unscaled branch'} by GameTime {c.tick} "
            f"(scaled {s}, exempt {e})"
            for r, (s, e) in ((r, hits[r]) for r in RECLAIMED) if not (s and e)]


def ai_command(c, table, out):
    """`+ai 2` twice with cheats on: a human's seat with no mark, then the stock AI's."""
    fails = []
    c.call("switches", "cheats=on")
    mark = f"0x{table + IMPAI:08X}:1"
    for want, site in ((1, "0x4162FE"), (2, "0x4162F4")):
        c.type(f"+ai {IMPAI}")
        ctl, m = c.values(field(IMPAI, 0x73, 1), mark)
        out.append(f"  `+ai {IMPAI}` ({site}): record {IMPAI} controller {ctl}, mark {m}")
        if (ctl, m) != (want, 0):
            fails.append(f"after `+ai {IMPAI}` ({site}) record {IMPAI} is controller {ctl} with "
                         f"mark {m}, not controller {want} with no mark")
    return fails


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("instance")
    p.add_argument("--difficulty", default="easy,medium,hard",
                   help="the Difficulties to measure, comma-separated")
    p.add_argument("--gpu", action="store_true",
                   help="render with the default renderer and take a GPU token, not GDI")
    a = p.parse_args()
    levels = [x.strip() for x in a.difficulty.split(",") if x.strip()]
    bad = [x for x in levels if x not in LEVELS]
    if bad or not levels:
        print(f"handicap_check: {', '.join(bad) or 'nothing'} is not easy, medium or hard",
              file=sys.stderr)
        return 2
    api = tacli_api()
    hold_instance(f"handicap_check on {a.instance}", a.instance, gpu=a.gpu)
    failed = 0
    for level in levels:
        out = []
        try:
            fails = run(api, a.instance, level, out, a.gpu)
        except (AssertionError, OSError, ValueError, subprocess.TimeoutExpired) as e:
            print("\n".join(out))
            print(f"handicap_check {level}: COULD NOT RUN: {e}")
            return 2
        print("\n".join(out))
        for f in fails:
            print(f"  {level}: FAIL {f}")
        print(f"handicap_check {level}: {'FAIL' if fails else 'PASS'}")
        failed += bool(fails)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
