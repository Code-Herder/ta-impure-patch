#!/usr/bin/env python3
"""scatter_check.py -- the build-scatter test groups' assertions (tools/replay/fixtures.json).

    tools/replay/scatter_check.py walked <instance>    sites A and B, held at GameTime 990
    tools/replay/scatter_check.py gaveup <instance>    the third site, held at GameTime 1802
    tools/replay/scatter_check.py guarded <instance>   the guard's seat, held at GameTime 1300 (build-scatter-guard)

Run by tareplay as the test groups' `run` steps, on the live lane held at their GameTimes. Build scatter
(tagpu/ddraw/src/tagpu_scatter.c) moves a builder's idle units off the site its build order was refused on, and
says so once a plan: `scatter: GameTime N: site builder=.. type=.. cell=X,Z WxH on=.. moved=.. (failed=..) ...`,
`failed` the moved units that were idle by a move the engine had failed. The checks
read those lines of the run's log and one roster dump begun after the step was entered.

WALKED (scenarios/build-scatter.json). Site A is at cell 57,470 and site B at 72,468, a solar collector's 5 x 5
each, four idle ARMPW on each.

    site A's first line moved all four          the cursor accepted the site under them, and the refusal moved them
    a later line of site A moved exactly one    the unit ordered back onto the site by hand at the step after the
                                                refusal stood idle on it at a later refusal and was moved again
    site B's line moved all four                the four were sent together to the site's centre at the start, so
                                                some stand under a move the engine has failed: idle by that rule
    an ARMPW stands within NEAR of AWAY         the unit ordered away by hand went where the player sent it
    an ARMSOLAR stands on site A                the site cleared and the build started

GAVEUP (build-scatter-clear.json, then build-scatter-wreck.json). Every unit was removed while site B's four
walked and their builder waited; the third site is where site A stood, its four ARMPW on its corners and a wreck
on its middle.

    a line of cell 57,470 after the clear's GameTime moved all four
    no ARMSOLAR stands anywhere                 the wreck kept the site refused
    the seat's ARMCK and its four ARMPW live, none of the four on the site
    the ARMCK's order list is empty or headed by its type's default order (the order controller's own, the
    byte at def +0x230)                         the build order was abandoned, as stock abandons it

GUARDED (scenarios/build-scatter-guard.json, the build-scatter-guard test groups). An ARMCOM guards an ARMVP and
the factory guard seats it on what the test group then makes a solar collector's site, at site B's cells.

    the site's line counts one unit of the guard's and moves none     the cursor accepted the site under the
                                                seated commander, and build scatter left it to its manager
    the log holds the guard's `site ... given up` line                the guard gave the seat up for the site
    an ARMSOLAR stands on the site, and the ARMCOM does not           it was seated off the site, and the build began

Exit 0 when every check holds, 1 with the first that does not, 2 when it could not run.
"""
import json
import re
import subprocess
import sys
import time
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path

TREE = Path(__file__).resolve().parents[2]
TACLI = TREE / "tools" / "tacli"
sys.path.insert(0, str(TREE / "tools"))
import talog  # noqa: E402

SITE_A, SITE_B = (57, 470), (72, 468)       # first cells; a solar collector is 5 x 5
SITE_CELLS = 5
AWAY = (1000, 7300)                         # where the fixture's `order` step sends the first ARMPW
CLEAR_AT = 992                             # the GameTime of the fixture's build-scatter-clear.json step
NEAR = 24                                   # px: a unit stops within a cell or so of its move's point
ROSTER_S = 60                               # a dump comes every 5 s of wall time, held or not
ACK_S = 60
MAIN = 0x511DE8
OFF_UNITS, UNIT_STRIDE = 0x14357, 0x118     # the unit array's first record, and a record's size
U_ORDERS, U_DEF = 0x5C, 0x92                # a unit's order list head and its type's definition
N_ACTION, UD_DEFAULT = 0x04, 0x230          # an order node's action byte; a definition's default order
SITE_LINE = re.compile(r"scatter: GameTime (\d+): site builder=(\d+) type=\d+ cell=(\d+),(\d+) \d+x\d+ "
                       r"on=(\d+) moved=(\d+) \(failed=(\d+)\) noway=(\d+) busy=(\d+) other=(\d+) full=(\d+) "
                       r"guard=(\d+)")
GUARD_GAVE_UP = re.compile(r"guard: site builder=\d+ plant=\d+ seat=-?\d+,-?\d+ given up")


class Fail(Exception):
    pass


class CannotRun(Exception):
    pass


def cli_module():
    spec = spec_from_loader("_scatter_cli", SourceFileLoader("_scatter_cli", str(TACLI)))
    cli = module_from_spec(spec)
    sys.modules[spec.name] = cli
    spec.loader.exec_module(cli)
    return cli


def site_lines(lines: list) -> list:
    """Every plan the log holds, in order: {tick, builder, cell, on, moved}."""
    out = []
    for ln in lines:
        m = SITE_LINE.search(ln)
        if m:
            out.append({"tick": int(m[1]), "builder": int(m[2]), "cell": (int(m[3]), int(m[4])),
                        "on": int(m[5]), "moved": int(m[6]), "failed": int(m[7]),
                        "guard": int(m[12])})
    return out


def roster_block(lines: list, start: int, rx):
    """The units of the first whole roster dump that begins at line `start` or later, or None while the log holds
    none: a dump is one block of rows closed by its `units:` header, and a block already begun before `start` was
    filled before the step and is passed over."""
    i = start
    while i < len(lines):
        if not rx.match(lines[i]):
            i += 1
            continue
        end = i
        while end < len(lines) and rx.match(lines[end]):
            end += 1
        if end == len(lines):
            return None                      # the block is still being written
        if i == start and start > 0 and rx.match(lines[start - 1]):
            i = end
            continue
        if not lines[end].startswith("units:"):
            i = end
            continue
        units = []
        for ln in lines[i:end]:
            m = rx.match(ln)
            units.append({"type": m.group(2).upper(), "owner": int(m.group(3)),
                          "slot": int(m.group(4)) if m.group(4) else None,
                          "world": (int(m.group(5)), int(m.group(6)))})
        return units
    return None


def peek(cli, instance, *specs) -> list:
    """The values of `specs` (tacli peek's grammar), as unsigned ints."""
    r = subprocess.run([sys.executable, str(TACLI), "peek", instance, "--json", *specs],
                       capture_output=True, text=True, timeout=ACK_S + 30)
    if r.returncode:
        raise CannotRun(f"tacli peek: {(r.stdout + r.stderr).strip()[-200:]}")
    rows = json.loads(r.stdout)["peeks"]
    out = [cli._peek_uint(row) for row in rows]
    if len(out) != len(specs) or None in out:
        raise CannotRun(f"tacli peek {specs}: {[row['value'] for row in rows]}")
    return out


def head_and_default(cli, instance, slot: int) -> tuple:
    """(the action of the head of unit `slot`'s order list, None when the list is empty; its type's default
    order)."""
    base, = peek(cli, instance, f"*0x{MAIN:X}+0x{OFF_UNITS:X}:4")
    unit = base + slot * UNIT_STRIDE
    head, udef = peek(cli, instance, f"0x{unit + U_ORDERS:X}:4", f"0x{unit + U_DEF:X}:4")
    if not udef:
        raise CannotRun(f"unit {slot} has no definition")
    dflt, = peek(cli, instance, f"0x{udef + UD_DEFAULT:X}:1")
    if not head:
        return None, dflt
    action, = peek(cli, instance, f"0x{head + N_ACTION:X}:1")
    return action, dflt


def read(instance):
    """The run's site lines, a roster dump begun after this call, the tacli module and the log's lines."""
    cli = cli_module()
    gamedir = cli.Instance(instance).gamedir
    start = len(talog.run_text(gamedir).splitlines())
    deadline = time.monotonic() + ROSTER_S
    while True:
        lines = talog.run_text(gamedir).splitlines()
        units = roster_block(lines, start, cli.ROSTER_RX)
        if units is not None:
            return site_lines(lines), units, cli, lines
        if time.monotonic() > deadline:
            raise CannotRun(f"no roster dump began in {ROSTER_S} s")
        time.sleep(0.25)


def on_site(world, cell) -> bool:
    """A point of the world, in px, lies on the solar collector's site whose first cell is `cell`."""
    return all(16 * c <= w < 16 * (c + SITE_CELLS) for w, c in zip(world, cell))


def near(world, point) -> bool:
    return all(abs(w - p) <= NEAR for w, p in zip(world, point))


def mine(units, kind):
    return [u for u in units if u["owner"] == 0 and u["type"] == kind]


def judge_walked(sites: list, units: list) -> list:
    fails = []
    a = [s for s in sites if s["cell"] == SITE_A]
    b = [s for s in sites if s["cell"] == SITE_B]
    if not a or (a[0]["on"], a[0]["moved"]) != (4, 4):
        fails.append(f"site A's first plan is {a[0] if a else None}, not four units on it and four moved")
    if not any(s["moved"] == 1 for s in a[1:]):
        fails.append(f"no later plan of site A moved the one unit ordered back onto it: {a[1:]}")
    if not b or (b[0]["on"], b[0]["moved"]) != (4, 4):
        fails.append(f"site B's first plan is {b[0] if b else None}, not four units on it and four moved")
    elif not b[0]["failed"]:
        fails.append("none of site B's four was idle by a failed move: the bunch did not form")
    if not any(near(u["world"], AWAY) for u in mine(units, "ARMPW")):
        fails.append(f"no ARMPW within {NEAR} px of {AWAY}, where one was ordered by hand: "
                     f"{[u['world'] for u in mine(units, 'ARMPW')]}")
    if not any(on_site(u["world"], SITE_A) for u in mine(units, "ARMSOLAR")):
        fails.append(f"no ARMSOLAR on site A: {[u['world'] for u in mine(units, 'ARMSOLAR')]}")
    return fails


def judge_gaveup(sites: list, units: list, head=None, dflt=None) -> list:
    """`head` and `dflt`: head_and_default of the seat's ARMCK."""
    fails = []
    third = [s for s in sites if s["cell"] == SITE_A and s["tick"] > CLEAR_AT]
    if not third or (third[0]["on"], third[0]["moved"]) != (4, 4):
        fails.append(f"the third site's plan is {third[0] if third else None}, not four units on it and four moved")
    if mine(units, "ARMSOLAR"):
        fails.append(f"an ARMSOLAR stands at {[u['world'] for u in mine(units, 'ARMSOLAR')]}: the wreck did not "
                     f"keep the site refused")
    pws = mine(units, "ARMPW")
    if len(mine(units, "ARMCK")) != 1 or len(pws) != 4:
        fails.append(f"{len(mine(units, 'ARMCK'))} ARMCK and {len(pws)} ARMPW live, not 1 and 4")
    if any(on_site(u["world"], SITE_A) for u in pws):
        fails.append(f"an ARMPW still stands on the site: {[u['world'] for u in pws]}")
    if head is not None and head != dflt:
        fails.append(f"the ARMCK's list is headed by action {head}, not its default {dflt}: the build order "
                     f"was not abandoned")
    return fails


def judge_guarded(sites: list, units: list, lines: list) -> list:
    fails = []
    b = [s for s in sites if s["cell"] == SITE_B]
    if not b or (b[0]["on"], b[0]["moved"], b[0]["guard"]) != (1, 0, 1):
        fails.append(f"the site's first plan is {b[0] if b else None}, not one unit on it, the guard's, and none "
                     f"moved")
    if not any(GUARD_GAVE_UP.search(ln) for ln in lines):
        fails.append("the log holds no `guard: site ... given up` line: the guard kept the seat")
    if not any(on_site(u["world"], SITE_B) for u in mine(units, "ARMSOLAR")):
        fails.append(f"no ARMSOLAR on the site: {[u['world'] for u in mine(units, 'ARMSOLAR')]}")
    coms = mine(units, "ARMCOM")
    if len(coms) != 1 or on_site(coms[0]["world"], SITE_B):
        fails.append(f"the ARMCOM is not one unit standing off the site: {[u['world'] for u in coms]}")
    return fails


def main(argv):
    if len(argv) != 3 or argv[1] not in ("walked", "gaveup", "guarded"):
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    try:
        sites, units, cli, lines = read(argv[2])
        if argv[1] == "walked":
            fails = judge_walked(sites, units)
        elif argv[1] == "guarded":
            fails = judge_guarded(sites, units, lines)
        else:
            cks = mine(units, "ARMCK")
            if len(cks) != 1 or cks[0]["slot"] is None:
                raise Fail(f"{len(cks)} ARMCK of seat 0 in the roster, not 1 with its slot")
            fails = judge_gaveup(sites, units, *head_and_default(cli, argv[2], cks[0]["slot"]))
    except Fail as e:
        fails = [str(e)]
    except CannotRun as e:
        print(f"scatter_check: could not run: {e}")
        return 2
    if fails:
        print(f"scatter_check {argv[1]}: " + "; ".join(fails))
        return 1
    print(f"scatter_check {argv[1]}: holds ({len(sites)} plans in the log, {len(units)} units in the roster)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
