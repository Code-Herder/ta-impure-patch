#!/usr/bin/env python3
"""income_check.py -- the income lever's test groups' assertions (tools/replay/fixtures.json).

    tools/impai/income_check.py check <instance>     `income-mult`: the multiplier and the gives, held at GameTime 75
    tools/impai/income_check.py stall <instance>     `income-stall`: a stalled seat's bonus spent, from GameTime 330

Run by tareplay as the test groups' `run` steps, on the live lane held at their GameTimes. The income lever
(tagpu/ddraw/src/tagpu_income.c) credits an armed seat (m - 1) times its units' production of the previous pass,
through the player's ledger, before each economy pass. The economy pass falls at GameTime 1 (mod 30): the
loader's pass at 1, then 31, 61 and so on.

CHECK (scenarios/income-mult.json, content `--income 1=2.5`). Seats 1 and 2 are OTA AI on CORE with the same
commander, and at GameTime 35 the idle human gave each of them 100 energy and 100 metal (`+give`). Nothing has
finished building by 75, each seat's production is its commander's alone, so seat 1's pass at 31 was credited 1.5
x its pass at 1 and its pass at 61 1.5 x its pass at 31; and the pass at 61 also carries the give, which the pass
folds in from the ledger (0x464C44, 0x464D77 credit it; 0x401914 and 0x40191F read it) and the lever leaves
unmultiplied. So with G = 100:

    energy  seat 1's rate +0x90 - G == 2.5 x (seat 2's - G), within RTOL of seat 1's rate
    metal   the same on +0x9C

and seat 2's energy rate less G is above 0 (a commander makes energy). A give that the lever multiplied would put
seat 1's rate 1.5 x G = 150 above that, and a lever that did nothing would give the ratio 1.

STALL (scenarios/income-stall.json, content `--income 1=5`). Seats 1 and 2 are CORE with the AI off, a storage of
10 energy, and the same demand: an Automatic Radar Targeting (CORTARG, EnergyUse 1300) and their commander's 1.
Seat 1 makes 45 a pass (its commander's 25 and one solar collector's 20) at x5; seat 2 makes 225 (its commander's
and ten solar collectors'), what x5 makes of seat 1's. Both stall. From GameTime 330 to 330 + STALL_TICKS the
check reads each seat's energy consumed, the produced total +0xAC less the change in the store +0x8C less the
wasted total +0xCC (doubles but the store), and asks:

    both seats' rates equal at both ends (seat 1's 45 + 4 x 45 against seat 2's 225, exact in float)
    |consumed by seat 1 - consumed by seat 2| <= 2 x (rate + the storage maximum)

The bound is the two ends' phase: a pass consumes between 0 and its rate plus its store, and the two seats' passes
need not be at the same point of the demand's cycle when the window opens and closes. A bonus credited after the
pass would have been clamped to the storage of 10 and wasted, leaving seat 1 at most 45 + 10 a pass, under a
quarter of seat 2's.

Each also asks the log for `income: installed` and the level init's arming lines: exactly the seats its content
armed, at their multipliers. Exit 0 when every check holds, 1 with the first that does not, 2 when it could not run.
The puppet lanes then show the recording carries the rates, the stores and the lever's own rows.
"""
import json
import re
import struct
import subprocess
import sys
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path

TREE = Path(__file__).resolve().parents[2]
TACLI = TREE / "tools" / "tacli"
sys.path.insert(0, str(TREE / "tools"))
import talog  # noqa: E402

MAIN = 0x511DE8
OFF_PLAYERS, PL_STRIDE = 0x1B63, 0x14B
PL_E_STORED, PL_E_RATE, PL_M_RATE, PL_E_MAX = 0x8C, 0x90, 0x9C, 0xA4
PL_E_TOTAL, PL_E_WASTED = 0xAC, 0xCC          # doubles
SEAT_X, SEAT_1 = 1, 2           # the multiplied seat and the stock one (both test groups' scenarios)
MULT = 2.5
ARMED = {SEAT_X: "2.500"}       # income-mult's content, `--income 1=2.5`
STALL_ARMED = {SEAT_X: "5.000"}  # income-stall's, `--income 1=5`
GIVE = 100.0                    # the fixture's `+give <seat> 100 energy` and `... metal`
RTOL = 1e-5                     # float32 sums of a few terms of order 100
STALL_TICKS = 600               # 20 passes, three of the stalled seats' six-pass cycles
ACK_S = 60
LEVER_LINE = re.compile(r"income: seat=(\d) mult=(\d+\.\d{3}) from=lever")
INSTALLED = "income: installed"


class Fail(Exception):
    pass


class CannotRun(Exception):
    pass


def f32(u):
    return struct.unpack("<f", struct.pack("<I", u & 0xFFFFFFFF))[0]


def addr(seat, off):
    return f"*0x{MAIN:X}+0x{OFF_PLAYERS + seat * PL_STRIDE + off:X}"


def rate_spec(seat, off):
    return f"{addr(seat, off)}:4"


def ratio_error(rate_x, rate_1, mult=MULT, give=GIVE):
    """How far seat X's rate is from the lever's rule against the stock seat's: (rate_x - give) - mult x (rate_1 -
    give), as a fraction of rate_x's size (at least 1)."""
    return abs((rate_x - give) - mult * (rate_1 - give)) / max(1.0, abs(rate_x))


def lever_fails(lines, expect=ARMED) -> list:
    fails = []
    if not any(INSTALLED in ln for ln in lines):
        fails.append("no `income: installed` line: the call site 0x46555A is not redirected")
    armed = {}
    for ln in lines:
        m = LEVER_LINE.search(ln)
        if m:
            armed[int(m.group(1))] = m.group(2)
    if armed != expect:
        fails.append(f"the level init armed {armed}, not {expect}")
    return fails


def judge(e_x, e_1, m_x, m_1, lines, mult=MULT, give=GIVE):
    """The failures of CHECK, a list of strings, from the two seats' energy and metal rates and the run's log."""
    fails = []
    if not e_1 - give > 0:
        fails.append(f"the stock seat's energy rate less the give is {e_1 - give!r}: no commander production")
    for what, x, one in (("energy", e_x, e_1), ("metal", m_x, m_1)):
        err = ratio_error(x, one, mult, give)
        if err > RTOL:
            fails.append(f"{what}: seat {SEAT_X}'s rate {x!r} is not {give} + {mult} x ({one!r} - {give}) "
                         f"(off by {err:.2e} of it)")
    return fails + lever_fails(lines)


def consumed(t0: dict, t1: dict) -> float:
    """Energy consumed between two reads of a seat: produced, less what the store gained, less what was wasted."""
    return (t1["total"] - t0["total"]) - (t1["stored"] - t0["stored"]) - (t1["wasted"] - t0["wasted"])


def judge_stall(x0: dict, x1: dict, o0: dict, o1: dict) -> tuple:
    """(failures, the consumption line) of STALL from the stalled seats' reads at both ends: x the armed seat,
    o the one that makes its own; each read {rate, stored, max, total, wasted}."""
    fails = []
    for when, x, o in (("start", x0, o0), ("end", x1, o1)):
        if x["rate"] != o["rate"]:
            fails.append(f"at the {when} seat {SEAT_X}'s energy rate {x['rate']!r} is not seat {SEAT_1}'s "
                         f"{o['rate']!r}")
    cx, co = consumed(x0, x1), consumed(o0, o1)
    bound = 2 * (o0["rate"] + o0["max"])
    line = (f"consumed {cx:.1f} by seat {SEAT_X} against {co:.1f} by seat {SEAT_1} over {STALL_TICKS} ticks, "
            f"bound {bound:.1f}; wasted {x1['wasted'] - x0['wasted']:.1f} and {o1['wasted'] - o0['wasted']:.1f}")
    if not co > 0:
        fails.append(f"seat {SEAT_1} consumed {co:.1f}: no stall")
    if abs(cx - co) > bound:
        fails.append(f"seat {SEAT_X}'s bonus was not all spent: {line}")
    return fails, line


def cli_module():
    spec = spec_from_loader("_income_cli", SourceFileLoader("_income_cli", str(TACLI)))
    cli = module_from_spec(spec)
    sys.modules[spec.name] = cli
    spec.loader.exec_module(cli)
    return cli


def peek(instance, *specs):
    """The values of `specs` (tacli peek's grammar), as unsigned ints, and the tacli module."""
    cli = cli_module()
    r = subprocess.run([sys.executable, str(TACLI), "peek", instance, "--json", *specs],
                       capture_output=True, text=True, timeout=ACK_S + 30)
    if r.returncode:
        raise CannotRun(f"tacli peek: {(r.stdout + r.stderr).strip()[-200:]}")
    rows = json.loads(r.stdout)["peeks"]
    if len(rows) != len(specs):
        raise CannotRun(f"tacli peek answered {len(rows)} of {len(specs)}")
    out = []
    for row in rows:
        v = cli._peek_uint(row)
        if v is None:
            raise CannotRun(f"peek {row['spec']}: {row['value']}")
        out.append(v)
    return out, cli


def check(instance):
    raw, cli = peek(instance, rate_spec(SEAT_X, PL_E_RATE), rate_spec(SEAT_1, PL_E_RATE),
                    rate_spec(SEAT_X, PL_M_RATE), rate_spec(SEAT_1, PL_M_RATE))
    e_x, e_1, m_x, m_1 = map(f32, raw)
    lines = talog.run_text(cli.Instance(instance).gamedir).splitlines()
    rates = f"energy {e_x!r} / {e_1!r}, metal {m_x!r} / {m_1!r} (seat {SEAT_X} / seat {SEAT_1})"
    fails = judge(e_x, e_1, m_x, m_1, lines)
    if fails:
        raise Fail(f"{rates}: " + "; ".join(fails))
    print(f"income_check: seat {SEAT_X} makes {MULT} x seat {SEAT_1}'s production, the gives unmultiplied: {rates}")


def stall_specs(seat) -> list:
    return [f"{addr(seat, PL_E_RATE)}:4", f"{addr(seat, PL_E_STORED)}:4", f"{addr(seat, PL_E_MAX)}:4",
            f"{addr(seat, PL_E_TOTAL)}:x8", f"{addr(seat, PL_E_WASTED)}:x8"]


def stall_read(values: list) -> dict:
    """One seat's read from its five peeks' values (stall_specs): three floats as decimal u32s, two doubles as
    hex bytes."""
    rate, stored, mx = (f32(int(v.split()[0])) for v in values[:3])
    total, wasted = (struct.unpack("<d", bytes.fromhex(v))[0] for v in values[3:])
    return {"rate": rate, "stored": stored, "max": mx, "total": total, "wasted": wasted}


def stall(instance):
    from resource_regression import Check

    class Held(Check):
        def __init__(self, name):
            self.name = name
            self.lockstep()

    c = Held(instance)
    c.hold()
    specs = stall_specs(SEAT_X) + stall_specs(SEAT_1)
    v0 = c.snapshot(*specs)
    c.advance(STALL_TICKS)
    v1 = c.snapshot(*specs)
    c.release()
    x0, o0, x1, o1 = stall_read(v0[:5]), stall_read(v0[5:]), stall_read(v1[:5]), stall_read(v1[5:])
    fails, line = judge_stall(x0, x1, o0, o1)
    lines = talog.run_text(c.gamedir).splitlines()
    fails += lever_fails(lines, STALL_ARMED)
    if fails:
        raise Fail("; ".join(fails))
    print(f"income_check: a stalled seat's bonus is spent as production: {line}")


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2 or argv[0] not in ("check", "stall"):
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    try:
        (check if argv[0] == "check" else stall)(argv[1])
    except (Fail, AssertionError) as e:
        print(f"income_check: FAIL: {e}")
        return 1
    except (CannotRun, OSError, subprocess.SubprocessError, ValueError) as e:
        print(f"income_check: could not run: {e}")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
