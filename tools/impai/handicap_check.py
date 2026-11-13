#!/usr/bin/env python3
"""handicap_check -- ImpAI's exemption from the Difficulty handicap, measured in the engine.

    tools/impai/handicap_check.py <instance> [--difficulty easy,medium,hard]

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
record and seat 2 the only marked one, so a scaled count is OTA AI's and an exempt one ImpAI's.

Then the reclaims (rows 11 and 12, the payout; 17, the reclaimed unit's credit), which need a
seat's own order to hold: impai-handicap-reclaims.json gives each computer seat a construction kbot
beside a Dragon's Teeth feature and another beside a Dragon's Teeth unit of its own, each ordered to
reclaim it. ImpAI's seat keeps its orders (no stock planner runs on it, and its brain tasks only an
idle builder). OTA AI's builder planner 0x408100 orders every builder of its group 4 again every 90
ticks (its due time GameTime + 90 at 0x40811B; engine/orders.md, The ten slots), so each reclaim is
re-issued every REISSUE ticks (45) while its row lacks the seat: some re-issue then lands within 45
ticks after a planner run and has at least 45 ticks before the next, and the feature's reclaim pays
20 ticks after it starts (8 metal: the countdown 15 + (energy + metal) / 2, two a working call,
0x404BBA..0x404C89); a unit's reclaim keeps its progress on the unit across re-issues. The level is
advanced until rows 11, 12 and 17 have each sent a record down both, at most RECLAIM_TICKS ticks.

Every row's counts are printed, and the rows this test does not reach are named with their
reason: 9 and 10, the refunds of a cancelled build and of an unbuild, which no step here scripts;
6 needs a mobile unit with a negative EnergyUse, which no unit of the retail game has, and
handicap_row6.py reaches it on Escalation. Rows 15 and 16 have no site: ImpAI leaves the
Deathmatch respawn's grants stock (tagpu_impai.c, HSITES).

Last, the AI command, with cheats on (its mask is 4): `+ai 2` makes ImpAI's seat a human's
(0x4162FE) and clears its mark, and a second `+ai 2` hands it to the stock AI (0x4162F4) with
the mark still clear. Exit 0 when every Difficulty passes, 1 on a failed check, 2 when a launch
could not be run.
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
import runlock  # noqa: E402
from resource_regression import TACLI, Check, tacli_api  # noqa: E402

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
RECLAIM_TICKS = 3600                    # the reclaims' bound: two minutes of simulation
REISSUE = 45                            # ticks between two re-issues: half the builder planner's 90
# the reclaims' kbots and targets as the scenario places them: (seat, the feature's kbot, the
# feature's world point, the unit's kbot, the unit's world point)
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


def run(api, instance, level, out):
    """One launch at `level`; the list of failed checks."""
    gamedir = api.Instance(instance).gamedir
    lever = gamedir / "tagpu_hold.on"
    had = lever.exists()
    doc = json.loads(SCENARIO.read_text())
    doc["setup"]["difficulty"] = level
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
            fails += rows_reached(c, instance, hits_at, out)
            fails += reclaims_reached(c, instance, hits_at, out)
            fails += ai_command(c, table, out)
        finally:
            try:
                subprocess.run([sys.executable, str(TACLI), "stop", instance],
                               capture_output=True, text=True, timeout=300)
            finally:
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
    """Each Flea ordered to attack the CORE commander nearest it, once the roster lists both."""
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
    for f in fleas:
        com = min(coms, key=lambda u: (u["world"][0] - f["world"][0]) ** 2
                  + (u["world"][1] - f["world"][1]) ** 2)
        c.call("order", "attack", "unit", com["engine_index"], "--unit", f["engine_index"],
               "--expect", "ARMFLEA")
        out.append(f"  Flea {f['engine_index']} attacks seat {com['owner']}'s commander "
                   f"{com['engine_index']}")


def rows_reached(c, instance, hits_at, out):
    """The structures, the Fleas' attacks and the gives, then the level advanced until every
    REACHED row has sent a record down both exits; the list of failed checks."""
    apply(instance, STRUCTURES)
    apply(instance, ATTACKERS)
    attack_commanders(c, out)
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
    fails = [f"row {r} sent no {'OTA AI' if not s else 'ImpAI'} record down its "
             f"{'scaled continuation' if not s else 'unscaled branch'} by GameTime {c.tick} "
             f"(scaled {s}, exempt {e})"
             for r, (s, e) in ((r, hits[r]) for r in REACHED) if not (s and e)]
    return fails


def placed(roster, owner, kind, at):
    """The engine index of the roster's `kind` of `owner` within NEAR of world point `at`, or None."""
    for u in roster:
        if (u["owner"] == owner and u["type"].upper() == kind and u["engine_index"]
                and abs(u["world"][0] - at[0]) <= NEAR and abs(u["world"][1] - at[1]) <= NEAR):
            return u["engine_index"]
    return None


def reclaim_units(c):
    """Each pair's kbots and Dragon's Teeth unit by engine index, once one roster lists them all:
    the roster is a dump logged every 5 s (tagpu_packet_pub.c) and the level is held, so the first
    dump that lists every unit the apply placed is the held level's."""
    deadline = time.monotonic() + ROSTER_S
    while True:
        roster = c.call("roster", "--json")["units"]
        pairs = [(seat, placed(roster, seat, "CORCK", fk), fp, placed(roster, seat, "CORCK", uk),
                  placed(roster, seat, "CORDRAG", up)) for seat, fk, fp, uk, up in RECLAIM_PAIRS]
        missing = [f"seat {p[0]}" for p in pairs if None in p]
        if not missing:
            return pairs
        if time.monotonic() > deadline:
            raise OSError(f"the roster lacks the reclaims' units of {', '.join(missing)} after "
                          f"{ROSTER_S} s")
        time.sleep(1)


def reissue(instance, *args):
    """One reclaim order through the scenario applier, on the held level. A refusal is not a failure
    of the check: the row's count decides, and the next re-issue tries again."""
    subprocess.run([sys.executable, str(TACLI), "order", instance, "reclaim", *map(str, args)],
                   capture_output=True, text=True, timeout=150)


def reclaims_reached(c, instance, hits_at, out):
    """The reclaims, re-issued every REISSUE ticks while a row lacks the seat's exit, until rows 11,
    12 and 17 have each sent a record down both; the list of failed checks."""
    apply(instance, RECLAIMS)
    pairs = reclaim_units(c)
    exit_of = {OTA: 0, IMPAI: 1}            # scaled for OTA AI's seat, exempt for ImpAI's
    start = c.tick
    hits = counts(c, hits_at)
    while not all(both(hits, (r,)) for r in RECLAIMED) and c.tick - start < RECLAIM_TICKS:
        for seat, fcons, fpos, ucons, drag in pairs:
            k = exit_of[seat]
            if not (hits[11][k] and hits[12][k]):
                reissue(instance, "pos", fpos[0], fpos[1], "--unit", fcons, "--expect", "CORCK")
            if not hits[17][k]:
                reissue(instance, "unit", drag, "--unit", ucons, "--expect", "CORCK",
                        "--expect-target", "CORDRAG")
        c.advance(REISSUE)
        hits = counts(c, hits_at)
    out.append(f"  reclaim rows scaled/exempt at GameTime {c.tick}: "
               + " ".join(f"{r}={hits[r][0]}/{hits[r][1]}" for r in RECLAIMED))
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
    a = p.parse_args()
    levels = [x.strip() for x in a.difficulty.split(",") if x.strip()]
    bad = [x for x in levels if x not in LEVELS]
    if bad or not levels:
        print(f"handicap_check: {', '.join(bad) or 'nothing'} is not easy, medium or hard",
              file=sys.stderr)
        return 2
    api = tacli_api()
    runlock.hold(TREE, f"handicap_check on {a.instance}")
    failed = 0
    for level in levels:
        out = []
        try:
            fails = run(api, a.instance, level, out)
        except (AssertionError, OSError, subprocess.TimeoutExpired) as e:
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
