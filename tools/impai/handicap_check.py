#!/usr/bin/env python3
"""handicap_check -- ImpAI's exemption from the Difficulty handicap, measured in the engine.

    tools/impai/handicap_check.py <instance> [--difficulty easy,medium,hard] [--gpu]

One launch a Difficulty, one at a time under the run lock (tools/runlock.py), each of
scenarios/impai-handicap.json with its `difficulty` replaced: an idle human seat, seat 1 a CORE
computer seat OTA AI plays, seat 2 a CORE computer seat ImpAI plays (`seatai=2`). The launch is
held (tagpu_hold.on, THE LAUNCH'S HOLD, at GameTime 1), row 18's strike made (below), advanced to
GameTime 61 by exact ticks, and the two
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
metal extractor, a metal maker and a tidal generator for each computer seat), and the human seat
gives each computer seat 100 energy and 100 metal (`+give`, the
console's Give, mask 1). Every handicap stub counts the type-2 records that take each of its exits
(tagpu_impai.c, s_diag.hits, at the address the attach line names); the level is advanced until
every row of REACHED -- the economy's 1 to 5, 7 and 8, the transfers' 13 and 14 and the purge's
18, which the strike has already counted -- has sent a record down both, at most ROWS_TICKS ticks. Seat 1 is the only unmarked type-2
record and seat 2 the only marked one, so a scaled count is OTA AI's and an exempt one ImpAI's. A row
counts once per pass of its site, so a seat with more units of a kind counts more on that row (ImpAI
builds a second wind generator early: at Hard, GameTime 961, row 4 counted 30 scaled and 53 exempt);
the check asks only that each exit is taken.

Row 18 is the purge's type test 0x406FC3 in the on-hit handler 0x406F80: a hit on a unit whose def has
cancapture (+0x245 bit 12, 0x406FA8..0x406FB7) and whose owner record is seated (0x406FB9..0x406FC1)
takes the scaled continuation when the owner's +0x73 is 2 (0x406FC3..0x406FC7: the planner pause
0x406FC9..0x406FEE and the purge 0x439EB0 at 0x406FF1). The test reads the victim only, and nothing in
0x406F80..0x406FF6 reads the Difficulty at main+0x37EEE, so OTA AI's commander takes it at every
Difficulty and any hit on each computer seat's commander counts that seat. The check makes that hit
certain, whatever either AI does, with a striker of the commander's own seat (STRIKER, STRIKES):

- **Where it is.** The check's copy of the scenario creates, at GameTime 0, one CORPUN for each computer
  seat beside its commander's start, on hold fire, facing it, the line between them clear of features
  (Town & Country's feature grid, read when the sites were chosen). At the launch's hold, GameTime 1,
  the check reads each commander at its start, each striker where it was put, and orders the striker,
  through the same scripted order as every other order here, to attack the ground at its commander's
  position read then.
- **The order fires.** The resolver's attack mode on no target or an own one is the ground attack,
  SUPPRESS for a unit that does not fly (0x43F154..0x43F1B4): the only test after the canattack bit is
  weapon 1's toairweapon (0x43F17E..0x43F189), which CORFIXED_GUN does not have; no owner of anything
  at the point is compared (engine/orders.md, What each resolver mode answers). SUPPRESS's handler
  0x4038A0 refuses only a VTOL (0x4039D9..0x4039F0) and writes the point into the weapon slots
  (0x48A0A0, 0x40396B..0x4039CE). AutoAim 0x49E1A0 then aims and fires with no test of the fire state
  (+0x110 bits 20-21 are not read in 0x49E1A0..0x49E541), its only gates the reload, 0 since the create
  (0x49E099), and the player's energy and metal against weapon +0xC0 and +0xC4 (0x49E3ED..0x49E417),
  both 0 for CORFIXED_GUN. Hold fire stops only the AI's weapon scan, which needs fire state 2
  (0x408A01..0x408A41), and the standby search.
- **Nothing replaces it.** OTA AI gives a structure to group 1, or 5 with +0x110 bit 31 (0x408830),
  whose ticks order only makers and factories (0x4086D0) and nothing at all (0x407380, `ret`); the
  rebalance moves units out of groups 3 and 7 only (0x407560), and the builder planner orders builders
  (0x408100). ImpAI orders its commander and constructors and its factories (impai_brain.c, the census;
  impai_econ.c) and its army, which skips every unit that is not mobile (impai_mil.c, the census), so
  never a CORPUN.
- **Nothing stops the shell.** A shell detonates on a unit only of another owner than its own
  (0x49B1C7..0x49B275), so on neither the commander nor anything its seat builds; the other seats' units
  are thousands of wu away at GameTime 1 (the human commander at (352, 4384), the other computer seat's
  start more than 4 000 away), and no feature stands on the line before the point. It detonates on the
  ground at its point, or in the step past it.
- **The blast reaches the commander.** Area damage 0x49A120 takes its radius as the weapon's
  areaofeffect halved (0x49A149: 85 -> 42) and tests each unit of the cells of its rectangle by the
  distance from the point to the unit's box, the def's extents +0x15E..+0x172 about its position
  (0x49A2AA..0x49A3A4), skipping only the shooter (0x49A259). Every unit inside calls 0x499CD0, which
  calls the hit 0x489BB0 with kind 1, a weapon's (0x499E1D..0x499E37), whatever the amount, 0 included:
  neither it nor the apply 0x489CE0 tests the amount before the handler's call at 0x489DA2, which only
  kinds 0x0A and 0x0B and a dead or dying victim skip (0x489D3F..0x489D97). A commander standing on the
  ground is inside when its walk since the order, at most CORCOM's maxvelocity a tick, plus the step past
  the point keeps it within the radius: STRIKE_TICKS, below.
- **It cannot kill.** One shell is at most CORFIXED_GUN's 320, against the commander's 3000; the striker
  is stopped within a tick of its seat's count, long before its reload, 2.8 s or 84 ticks, would let it
  fire again, and holding fire it fires no more.

So a striker whose shell lands within STRIKE_TICKS of its order counts its seat, and that time is the
engine's alone: the order, the aim, the reload and the flight, read from the defs and the code above.
A seat not counted STRIKE_TICKS after the order fails the check at once, by name.

Then the reclaims (rows 11 and 12, the payout; 17, the reclaimed unit's credit), which need a seat's own
order to hold:
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
import math
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
ROSTER_S = 30                           # the roster is logged every 5 s (tagpu_packet_pub.c)
REACHED = (1, 2, 3, 4, 5, 7, 8, 13, 14, 18)  # structures, commanders, gives and the strikes
RECLAIMS = "impai-handicap-reclaims.json"
RECLAIMED = (11, 12, 17)                # the rows a seat's own reclaim orders reach
# The reclaims' bound. Once it holds, the longer reclaim, the radar tower's, is at most 37 + 23 + 34 + 96 =
# 190 ticks (the docstring) and the feature's 114; the order arrives with the apply, on the held level. The
# turns a walk makes on the way are not in the sum, and 900 leaves more than four times it.
RECLAIM_TICKS = 900
STRIKER = "CORPUN"                      # row 18's striker, of the commander's own seat (docstring)
# Each seat's commander start, its striker's point and facing (degrees, the applier's heading_of: 0x8000 +
# deg x 0x10000 / 360). AimPrimary's heading is atan2(dx, dz) of the slot's 0x43E2E0 point minus its
# target (0x49E570 subtracts its second point from its first; 0x49E255..0x49E264) less the unit's heading
# +0x66 (0x49E269), so facing 0 (0x8000) is 0 for a target due +z and 90 (0xC000) for one due +x: each
# striker starts aimed at its commander. The lines, from Town & Country's TNT: seat 1's north of its start
# runs down columns 524 and 525, seat 2's west of it along rows 18 and 19, every cell between striker and
# commander height 80 with no feature (seat 2's Car18 stands at cell (271, 18), past the point, where a
# detonation is the step past it); a CORPUN is 4 x 3 cells, its footprint clear of features and 16 wu
# from the commander's.
STRIKES = ((OTA, (8400, 4384), (8400, 4328), 0),
           (IMPAI, (4336, 304), (4272, 304), 90))
START_SLACK = 2                         # wu: a commander at its start at the launch's hold, GameTime 1
PLACED_SLACK = 8                        # wu: a 3-cell side's centre lands on a cell centre
# The heading the turret turns at most, at 30 degrees a second (CORPUN's AimPrimary: turret <30>, sleeves
# <45>, both turning at once): atan(8 / 64) is 7.1 degrees, 8 ticks.
AIM_DEG = 8
# STRIKE_TICKS, the invariant. Area damage's radius is 85 >> 1 = 42 (0x49A149) against the distance from
# the detonation to the commander's box (0x49A2AA..0x49A3A4), which holds its position (min <= 0 <= max on
# each axis, read at the order). The detonation is at the point or one step past it, 350 / 30 = 11.7 wu a
# tick [the beam's pool record moves 29.3 a step at velocity 900, engine/weapons.md]; the ground within
# 64 wu of each start is 80 to 84 (the TNT), so the height adds at most 6. The commander walks at most
# CORCOM's maxvelocity, 1.07, a tick [INFERRED: the ground mover never exceeds it; no transport or
# teleport exists by then]. So 1.07 T + 12 < sqrt(42^2 - 6^2) = 41.5 holds for T <= 27: a shell landing
# within 27 ticks of the order finds the commander inside its radius whatever either AI ordered.
# What the engine takes: SUPPRESS's two states, 2 ticks (0x4039D9, 0x40396B) [INFERRED: one a tick];
# the aim, AIM_DEG at 30 a second and the pitch of a 64-wu shot at 45, at most 8 ticks; the fire once the
# script returns, at reload 0, 2 [INFERRED]; the flight, 64 / 11.7, 6: about 18. A seat not counted by
# STRIKE_TICKS is the engine taking longer than that, which the check names.
STRIKE_TICKS = 27
STRIKE_RADIUS = 42
CORCOM_VMAX = 1.07
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
# the unit's def +0x92 and, in the def, the box area damage measures to: x, y, z minima at +0x15E,
# +0x162, +0x166 and maxima at +0x16A, +0x16E, +0x172, 16.16 offsets from the unit's position (0x49A2AD..)
U_DEF, DEF_BOX = 0x92, 0x15E
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
    doc.setdefault("units", []).extend(
        {"id": f"striker{seat}", "type": STRIKER, "owner": seat, "pos": list(at), "facing": facing,
         "fire": "hold"} for seat, _start, at, facing in STRIKES)
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
            text = api.talog.run_text(gamedir).splitlines()
            where = next((m for m in map(ATTACH.search, text) if m), None)
            if where is None:
                raise OSError("no `impai: seam ... in; seats at ...` line: ImpAI is not in")
            table, hits_at = (int(g, 16) for g in where.groups())
            struck = strike(c, hits_at, out)
            if c.tick < AT:
                c.advance(AT - c.tick)
            specs = [field(OTA, 0x90), field(IMPAI, 0x90), field(OTA, 0x9C), field(IMPAI, 0x9C),
                     field(OTA, 0x73, 1), field(IMPAI, 0x73, 1), DIFFICULTY]
            e_ota, e_imp, m_ota, m_imp, c1, c2, diff = c.values(*specs)
            ops = c.values(*(f"0x{va:X}:1" for va in SITES + STOCK))
            seats = [ln for ln in text if "impai: seats" in ln or "impai: unavailable" in ln]
            marks = c.values(f"0x{table + OTA:08X}:1", f"0x{table + IMPAI:08X}:1")
            fails += struck
            if not struck:
                fails += rows_reached(c, instance, hits_at, out)
                fails += reclaims_reached(c, instance, hits_at, out)
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


def signed(u32):
    return u32 - (1 << 32) if u32 & 0x80000000 else u32


def strike(c, hits_at, out):
    """Row 18 on both computer seats at the launch's hold (the docstring): each commander read at its
    start and its striker where it was put, the commander's box holding its position, each striker
    ordered to attack the ground at its commander, then the level advanced a tick at a time until each
    seat has counted, a striker stopped the read its seat counts; the failures of each seat not counted
    within STRIKE_TICKS. A placement the invariant does not cover could not run."""
    deadline = time.monotonic() + ROSTER_S
    while True:
        units = c.call("roster", "--json")["units"]
        found = {(u["type"].upper(), u["owner"]): u for u in units}
        if all(("CORCOM", seat) in found and (STRIKER, seat) in found for seat, *_ in STRIKES):
            break
        if time.monotonic() > deadline:
            raise OSError(f"the roster at GameTime {c.tick} lacks a computer seat's commander or {STRIKER} "
                          f"after {ROSTER_S} s")
        time.sleep(1)
    base = c.values(f"*0x511DE8+0x{OFF_UNIT_BEGIN:X}")[0]
    exit_of = {OTA: 0, IMPAI: 1}            # scaled for OTA AI's seat, exempt for ImpAI's
    pending = []
    for seat, start, at, _facing in STRIKES:
        com, gun = found[("CORCOM", seat)], found[(STRIKER, seat)]
        cx, cz = com["world"][0], com["world"][1]
        gx, gz = gun["world"][0], gun["world"][1]
        if math.hypot(cx - start[0], cz - start[1]) > START_SLACK:
            raise OSError(f"seat {seat}'s commander stands at ({cx}, {cz}) at GameTime {c.tick}, not at its "
                          f"start {start}: the strike's line was chosen for the start")
        if math.hypot(gx - at[0], gz - at[1]) > PLACED_SLACK:
            raise OSError(f"seat {seat}'s {STRIKER} stands at ({gx}, {gz}), not at {at}")
        # the turn the turret makes: between the line it faces, from its point to the start, and the one
        # to the commander as it stands
        fx, fz = start[0] - at[0], start[1] - at[1]
        tx, tz = cx - gx, cz - gz
        turn = abs(math.degrees(math.atan2(fx * tz - fz * tx, fx * tx + fz * tz)))
        along = math.hypot(tx, tz)
        if turn > AIM_DEG:
            raise OSError(f"seat {seat}'s {STRIKER} must turn {turn:.1f} degrees to its commander, more than "
                          f"AIM_DEG ({AIM_DEG})")
        u = base + com["engine_index"] * UNIT_STRIDE
        defp = c.values(f"0x{u + U_DEF:08X}")[0]
        box = [signed(v) for v in c.values(*(f"0x{defp + DEF_BOX + 4 * k:08X}" for k in range(6)))]
        if not (box[0] <= 0 <= box[3] and box[2] <= 0 <= box[5] and box[1] <= 0 <= box[4]):
            raise OSError(f"seat {seat}'s commander's box {box} does not hold its position")
        c.call("order", "attack", "pos", cx, cz, "--unit", gun["engine_index"], "--expect", STRIKER)
        out.append(f"  {STRIKER} {gun['engine_index']} of seat {seat} attacks the ground at its commander "
                   f"{com['engine_index']}, ({cx}, {cz}), from {along:.0f} at GameTime {c.tick}")
        pending.append((seat, gun["engine_index"], com["engine_index"]))
    ordered = c.tick
    while pending and c.tick - ordered < STRIKE_TICKS:
        c.advance(1)
        hits = counts(c, hits_at)
        for seat, gun, com in list(pending):
            if hits[18][exit_of[seat]]:
                c.call("order", "stop", "--unit", gun, "--expect", STRIKER)
                out.append(f"  seat {seat}'s commander {com} hit by GameTime {c.tick}, {c.tick - ordered} "
                           f"ticks after the order; {STRIKER} {gun} stopped")
                pending.remove((seat, gun, com))
    return [f"row 18 counted no record of seat {seat} by GameTime {c.tick}, STRIKE_TICKS ({STRIKE_TICKS}) "
            f"after {STRIKER} {gun} was ordered onto its commander {com}: the engine took longer than "
            f"the invariant allows" for seat, gun, com in pending]


def rows_reached(c, instance, hits_at, out):
    """The structures and the gives, then the level advanced until every REACHED row has sent a record
    down both exits; the list of failed checks."""
    apply(instance, STRUCTURES)
    for seat in (OTA, IMPAI):
        for what in ("energy", "metal"):
            c.type(f"+give {seat} 100 {what}")
    start = c.tick
    hits = counts(c, hits_at)
    while not all(both(hits, (r,)) for r in REACHED) and c.tick - start < ROWS_TICKS:
        c.advance(STEP)
        hits = counts(c, hits_at)
    out.append(f"  rows scaled/exempt at GameTime {c.tick}: "
               + " ".join(f"{r}={s}/{e}" for r, (s, e) in hits.items()))
    out += [f"  row {r}: not reached, {why}" for r, why in NOT_HERE.items()]
    short = [r for r in REACHED if not both(hits, (r,))]
    return [f"row {r} sent no {'OTA AI' if not s else 'ImpAI'} record down its "
            f"{'scaled continuation' if not s else 'unscaled branch'} by GameTime {c.tick} "
            f"(scaled {s}, exempt {e})"
            for r, (s, e) in ((r, hits[r]) for r in short)]


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


def reclaims_reached(c, instance, hits_at, out):
    """The reclaims, each ordered once by the apply, the level advanced until rows 11, 12 and 17 have
    each sent a record down both; the list of failed checks."""
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
