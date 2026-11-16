#!/usr/bin/env python3
"""income_check.py -- the `income-mult` test group's assertion (tools/replay/fixtures.json).

    tools/impai/income_check.py check <instance>

Run by tareplay as the test group's `run` step, on the live lane held at GameTime 75. The group's content wrote
tagpu_impai_income.on with `1=2.5` (tools/impai/level_lever.py --income), so the income lever
(tagpu/ddraw/src/tagpu_income.c) multiplies seat 1's production by 2.5 and leaves seat 2's; both are OTA AI on
CORE with the same commander (scenarios/income-mult.json), and at GameTime 35 the idle human gave each of them
100 energy and 100 metal (`+give`). The economy pass falls at GameTime 1 (mod 30), so the last pass before 75 is
61's: nothing has finished building by then, each seat's production is its commander's alone, and that pass's
rates also carry the give, which the pass folds in from the player's ledger (0x464C44, 0x464D77 credit it;
0x401914 and 0x40191F read it) and the lever leaves unmultiplied. So with G = 100:

    energy  seat 1's rate +0x90 - G == 2.5 x (seat 2's - G), within RTOL of seat 1's rate
    metal   the same on +0x9C

and seat 2's energy rate less G is above 0 (a commander makes energy). A give that the lever multiplied would put
seat 1's rate 1.5 x G = 150 above that, far outside the tolerance, and so would a lever that did nothing (the
ratio 1). The log must hold `income: seat=1 mult=2.500 from=lever` and no such line for seat 2, and
`income: installed`.

Exit 0 when every check holds, 1 with the first that does not, 2 when it could not run. The puppet lanes then
show the recording carries the multiplied rates and stores.
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
PL_E_RATE, PL_M_RATE = 0x90, 0x9C
SEAT_X, SEAT_1 = 1, 2           # the multiplied seat and the stock one (scenarios/income-mult.json)
MULT = 2.5                      # the content's `--income 1=2.5`
GIVE = 100.0                    # the fixture's `+give <seat> 100 energy` and `... metal`
RTOL = 1e-5                     # float32 sums of a few terms of order 100
ACK_S = 60
LEVER_LINE = re.compile(r"income: seat=(\d) mult=(\d+\.\d{3}) from=lever")
INSTALLED = "income: installed"


class Fail(Exception):
    pass


class CannotRun(Exception):
    pass


def f32(u):
    return struct.unpack("<f", struct.pack("<I", u & 0xFFFFFFFF))[0]


def rate_spec(seat, off):
    return f"*0x{MAIN:X}+0x{OFF_PLAYERS + seat * PL_STRIDE + off:X}:4"


def ratio_error(rate_x, rate_1, mult=MULT, give=GIVE):
    """How far seat X's rate is from the lever's rule against the stock seat's: (rate_x - give) - mult x (rate_1 -
    give), as a fraction of rate_x's size (at least 1)."""
    return abs((rate_x - give) - mult * (rate_1 - give)) / max(1.0, abs(rate_x))


def judge(e_x, e_1, m_x, m_1, lines, mult=MULT, give=GIVE):
    """The failures, a list of strings, from the two seats' energy and metal rates and the run's log lines."""
    fails = []
    if not e_1 - give > 0:
        fails.append(f"the stock seat's energy rate less the give is {e_1 - give!r}: no commander production")
    for what, x, one in (("energy", e_x, e_1), ("metal", m_x, m_1)):
        err = ratio_error(x, one, mult, give)
        if err > RTOL:
            fails.append(f"{what}: seat {SEAT_X}'s rate {x!r} is not {give} + {mult} x ({one!r} - {give}) "
                         f"(off by {err:.2e} of it)")
    if not any(INSTALLED in ln for ln in lines):
        fails.append("no `income: installed` line: the call site 0x46555A is not redirected")
    armed = {}
    for ln in lines:
        m = LEVER_LINE.search(ln)
        if m:
            armed[int(m.group(1))] = m.group(2)
    if armed.get(SEAT_X) != f"{mult:.3f}":
        fails.append(f"the level init armed seat {SEAT_X} at {armed.get(SEAT_X)}, not {mult:.3f}")
    if SEAT_1 in armed:
        fails.append(f"the level init armed seat {SEAT_1}, which the lever does not name")
    return fails


def peek(instance, *specs):
    """The values of `specs` (tacli peek's grammar), as unsigned ints."""
    spec = spec_from_loader("_income_cli", SourceFileLoader("_income_cli", str(TACLI)))
    cli = module_from_spec(spec)
    sys.modules[spec.name] = cli
    spec.loader.exec_module(cli)
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


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2 or argv[0] != "check":
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    try:
        check(argv[1])
    except Fail as e:
        print(f"income_check: FAIL: {e}")
        return 1
    except (CannotRun, OSError, subprocess.SubprocessError, ValueError) as e:
        print(f"income_check: could not run: {e}")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
