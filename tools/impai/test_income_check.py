#!/usr/bin/env python3
"""test_income_check -- the income lever's item rule and the `income-mult` assertion's verdicts, offline.

    python3 tools/impai/test_income_check.py

level_lever.income_error must take exactly what tagpu_income.c's lever_item takes (a seat digit, a decimal with
at most three places, 1..8), since an item the DLL refuses is logged and skipped and the seat then plays at x1.
income_check.judge must pass the lever's rule with the gives left out, and fail a multiplied give, a lever that
did nothing, a seat armed at the wrong multiplier or armed when the lever names it not, and a missing install.
income_check.judge_stall must pass a stalled armed seat that spends what the seat making it all itself spends, and
fail one whose bonus was clamped and wasted, as a credit after the pass would leave it."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import income_check as C  # noqa: E402
import level_lever as L  # noqa: E402

LINES = ["income: installed: the economy pass's call 0x46555A; the lever names seats",
         "income: seat=1 mult=2.500 from=lever"]


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def rates(units, mult=C.MULT, give=C.GIVE, multiply_give=False):
    """Seat X's and the stock seat's rates as the DLL leaves them, in float32."""
    one = f32(give + units)
    base = one if multiply_give else f32(one - give)
    return f32(one + f32((mult - 1.0) * base)), one


class IncomeItem(unittest.TestCase):
    def test_taken(self):
        for item in ("1=2.5", "1=2.500", "0=1", "9=8", "3=8.000", "2=1.001", "1=07.5"):
            self.assertIsNone(L.income_error(item), item)

    def test_refused(self):
        for item in ("1=0.5", "1=8.001", "1=9", "10=2", "a=2", "1=2.5000", "1=2.", "1=.5", "1=-2", "1=2,5", "1="):
            self.assertIsNotNone(L.income_error(item), item)

    def test_cli_writes_the_lever(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(L.main([d, "--income", "1=2.5", "--income", "3=1.25"]), 0)
            self.assertEqual((Path(d) / L.INCOME_LEVER).read_text(), "1=2.5\n3=1.25\n")
            self.assertEqual(L.main([d, "--income", "1=9"]), 2)


class Judge(unittest.TestCase):
    def test_lever_rule_passes(self):
        ex, e1 = rates(25.0)
        mx, m1 = rates(1.5)
        self.assertEqual(C.judge(ex, e1, mx, m1, LINES), [])

    def test_no_metal_production_passes(self):
        ex, e1 = rates(25.0)
        self.assertEqual(C.judge(ex, e1, f32(C.GIVE), f32(C.GIVE), LINES), [])

    def test_multiplied_give_fails(self):
        ex, e1 = rates(25.0, multiply_give=True)
        mx, m1 = rates(1.5)
        fails = C.judge(ex, e1, mx, m1, LINES)
        self.assertEqual(len(fails), 1)
        self.assertTrue(fails[0].startswith("energy"))

    def test_lever_that_did_nothing_fails(self):
        e = f32(C.GIVE + 25.0)
        m = f32(C.GIVE + 1.5)
        fails = C.judge(e, e, m, m, LINES)
        self.assertEqual([f.split(":")[0] for f in fails], ["energy", "metal"])

    def test_no_production_fails(self):
        g = f32(C.GIVE)
        self.assertTrue(any("no commander production" in f for f in C.judge(g, g, g, g, LINES)))

    def test_lines(self):
        ex, e1 = rates(25.0)
        mx, m1 = rates(1.5)
        self.assertTrue(any("installed" in f for f in C.judge(ex, e1, mx, m1, LINES[1:])))
        wrong = [LINES[0], "income: seat=1 mult=2.000 from=lever"]
        self.assertTrue(any("armed" in f for f in C.judge(ex, e1, mx, m1, wrong)))
        extra = LINES + ["income: seat=2 mult=1.500 from=lever"]
        self.assertTrue(any("armed" in f for f in C.judge(ex, e1, mx, m1, extra)))
        self.assertTrue(any("armed" in f for f in C.judge(ex, e1, mx, m1, LINES[:1])))

    def test_the_stall_group_arms_at_five(self):
        stall = [LINES[0], "income: seat=1 mult=5.000 from=lever"]
        self.assertEqual(C.lever_fails(stall, C.STALL_ARMED), [])
        self.assertTrue(C.lever_fails(LINES, C.STALL_ARMED))

    def test_ratio_error_scale(self):
        self.assertAlmostEqual(C.ratio_error(162.5, 125.0), 0.0)
        self.assertGreater(C.ratio_error(162.5 + 150.0, 125.0), 0.4)


def stall_run(rate, mx, passes, bonus_after=0.0):
    """A seat stalled on a demand above everything it has: each pass its rate is produced, the store and the rate
    paid out, and `bonus_after` credited after the pass, the store clamped to mx and the rest wasted. The reads
    {rate, stored, max, total, wasted} at the start and after `passes`."""
    stored, total, wasted = 0.0, 0.0, 0.0
    read = lambda: {"rate": rate, "stored": stored, "max": mx, "total": total, "wasted": wasted}
    start = read()
    for _ in range(passes):
        total += rate + bonus_after
        stored = bonus_after + 0.0
        if stored > mx:
            wasted += stored - mx
            stored = mx
    return start, read()


class Stall(unittest.TestCase):
    def test_bonus_spent_passes(self):
        x0, x1 = stall_run(225.0, 10.0, 20)
        o0, o1 = stall_run(225.0, 10.0, 20)
        fails, line = C.judge_stall(x0, x1, o0, o1)
        self.assertEqual(fails, [])
        self.assertIn("consumed 4500.0", line)

    def test_phase_within_bound_passes(self):
        x0, x1 = stall_run(225.0, 10.0, 20)
        x1 = dict(x1, total=x1["total"] + 200.0)
        o0, o1 = stall_run(225.0, 10.0, 20)
        self.assertEqual(C.judge_stall(x0, x1, o0, o1)[0], [])

    def test_bonus_after_the_pass_fails(self):
        x0, x1 = stall_run(45.0, 10.0, 20, bonus_after=180.0)
        o0, o1 = stall_run(225.0, 10.0, 20)
        x0, x1 = dict(x0, rate=225.0), dict(x1, rate=225.0)
        fails, _ = C.judge_stall(x0, x1, o0, o1)
        self.assertEqual(len(fails), 1)
        self.assertIn("not all spent", fails[0])

    def test_rates_differ_fails(self):
        x0, x1 = stall_run(45.0, 10.0, 20)
        o0, o1 = stall_run(225.0, 10.0, 20)
        fails, _ = C.judge_stall(x0, x1, o0, o1)
        self.assertTrue(any("start" in f for f in fails) and any("end" in f for f in fails))

    def test_no_stall_fails(self):
        z0 = {"rate": 0.0, "stored": 0.0, "max": 10.0, "total": 0.0, "wasted": 0.0}
        self.assertTrue(any("no stall" in f for f in C.judge_stall(z0, z0, z0, z0)[0]))

    def test_stall_read(self):
        v = [f"{struct.unpack('<I', struct.pack('<f', 225.0))[0]} (0x0)", "0 (0x0)",
             f"{struct.unpack('<I', struct.pack('<f', 10.0))[0]} (0x0)",
             struct.pack("<d", 4500.0).hex(), struct.pack("<d", 1.5).hex()]
        self.assertEqual(C.stall_read(v), {"rate": 225.0, "stored": 0.0, "max": 10.0, "total": 4500.0,
                                           "wasted": 1.5})


if __name__ == "__main__":
    unittest.main()
