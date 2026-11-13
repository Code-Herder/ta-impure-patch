#!/usr/bin/env python3
"""test_handicap_check -- the handicap check's peeks and its verdicts, offline.

    python3 tools/impai/test_handicap_check.py

The table is s_diag.hits[18][2] of u32 (tagpu_impai.c): 144 bytes. tagpu_peek.c reads the N of an
x<N> size with parse_hex and clamps it to 256, so the request must spell 144 in hex; spelt in
decimal it asks 0x144 bytes, the peek answers 256 and the counts are misread. The rest runs the check's
steps on fake levels: a dead unit's orders are not read, an unreadable peek could not run, the GDI lever is
left as the run found it, row 18's strike stops each striker and names a seat it did not count, and
the strike probe logs a striker's shots and names a seat it did not count."""
import contextlib
import io
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import handicap_check as H  # noqa: E402


def peek_size(spec):
    """The bytes tagpu_peek.c reads for `spec`'s x<N> size: N in hex, clamped to 256."""
    n = int(spec.rsplit(":x", 1)[1], 16)
    return min(n, H.PEEK_MAX)


class Table:
    """A held level whose count table holds row r's pair as (10 r, 10 r + 1)."""

    def __init__(self):
        self.asked = []

    def bytes(self, spec):
        self.asked.append(spec)
        words = []
        for k in range(H.HIT_ROWS):
            words += [10 * (k + 1), 10 * (k + 1) + 1]
        return struct.pack(f"<{len(words)}I", *words)[:peek_size(spec)]


class HitsSpec(unittest.TestCase):
    def test_the_request_is_the_table_in_hex(self):
        self.assertEqual(H.hits_spec(0x75ED5F48), "0x75ED5F48:x90")
        self.assertEqual(peek_size(H.hits_spec(0x75ED5F48)), H.HIT_ROWS * 2 * 4)

    def test_counts_reads_each_row_pair(self):
        t = Table()
        hits = H.counts(t, 0x75ED5F48)
        self.assertEqual(t.asked, ["0x75ED5F48:x90"])
        self.assertEqual(sorted(hits), list(H.ROWS))
        self.assertEqual(hits[1], (10, 11))
        self.assertEqual(hits[17], (170, 171))
        self.assertEqual(hits[18], (180, 181))

    def test_a_short_answer_is_refused(self):
        class Short(Table):
            def bytes(self, spec):
                return super().bytes(spec)[:100]
        with self.assertRaises(OSError):
            H.counts(Short(), 0x75ED5F48)


class Memory:
    """A held level as a map of addresses: a dword or a byte by `values`, a run of bytes by `bytes`."""

    def __init__(self, words, blobs=()):
        self.words, self.blobs = dict(words), dict(blobs)

    def values(self, *specs):
        out = []
        for s in specs:
            addr, _, size = s.partition(":")
            v = self.words[int(addr, 16)]
            out.append(v & 0xFF if size == "1" else v)
        return out

    def bytes(self, spec):
        return self.blobs[int(spec.split(":")[0], 16)]


class HeadName(unittest.TestCase):
    UNIT, NODE, DESC, NAME = 0x1000, 0x2000, 0x3000, 0x4000

    def level(self, head, act, count):
        return Memory({self.UNIT + H.U_ORDERS: head, self.NODE + 4: act,
                       H.DESC_BEGIN: self.DESC, H.DESC_END: self.DESC + count * H.DESC_STRIDE,
                       self.DESC + act * H.DESC_STRIDE + H.DESC_NAME: self.NAME},
                      {self.NAME: b"ReclaimUnit\0garbage"})

    def test_the_head_is_named_through_the_descriptor(self):
        self.assertEqual(H.head_name(self.level(self.NODE, 33, 50), self.UNIT), "ReclaimUnit")

    def test_an_empty_list_is_none(self):
        self.assertEqual(H.head_name(self.level(0, 33, 50), self.UNIT), "none")

    def test_an_action_past_the_count_is_not_looked_up(self):
        self.assertEqual(H.head_name(self.level(self.NODE, 50, 50), self.UNIT), "action 50")


class Live(unittest.TestCase):
    def test_a_pending_death_is_dead(self):
        self.assertTrue(H.live(H.ST_ALIVE))
        self.assertFalse(H.live(H.ST_ALIVE | H.ST_DYING))
        self.assertFalse(H.live(0))


UNITS = 0x50000000                          # the unit array's base in the fakes below


class Level(Memory):
    """A held level whose unit array starts at UNITS; it advances by `advance` and answers orders."""

    def __init__(self, words, blobs=(), tick=301):
        super().__init__(words, blobs)
        self.tick, self.orders = tick, []

    def values(self, *specs):
        return [UNITS if s == f"*0x511DE8+0x{H.OFF_UNIT_BEGIN:X}" else super().values(s)[0] for s in specs]

    def advance(self, n):
        self.tick += n

    def call(self, *args):
        self.orders.append(args)

    def type(self, line):
        pass


def state_of(slot):
    return UNITS + slot * H.UNIT_STRIDE + H.U_STATE


class ReclaimState(unittest.TestCase):
    PAIRS = ((H.OTA, 7, (16, 16), 8, 9),)

    def test_a_dead_fark_is_named_dead_and_its_head_is_not_read(self):
        level = Level({state_of(7): H.ST_ALIVE | H.ST_DYING, state_of(8): 0, state_of(9): 0,
                       UNITS + 9 * H.UNIT_STRIDE + H.U_NANO: 0})
        saved = H.feature_words
        H.feature_words = lambda c, pairs: [0x12]
        try:
            line, = H.reclaim_state(level, self.PAIRS, [0x12])
        finally:
            H.feature_words = saved
        self.assertIn("feature Fark 7 dead;", line)
        self.assertIn("unit Fark 8 dead;", line)


class Patched(unittest.TestCase):
    """Runs with some of handicap_check's names replaced, each put back afterwards."""

    def patch(self, **names):
        for k, v in names.items():
            self.addCleanup(setattr, H, k, getattr(H, k))
            setattr(H, k, v)


class Main(Patched):
    def test_an_unreadable_peek_could_not_run(self):
        def run(*_args):
            raise ValueError("invalid literal for int() with base 10: '<unreadable'")
        self.patch(run=run, tacli_api=lambda: None, hold_instance=lambda *a, **k: None)
        saved = sys.argv
        sys.argv = ["handicap_check.py", "x", "--difficulty", "easy"]
        try:
            with contextlib.redirect_stdout(io.StringIO()) as said:
                self.assertEqual(H.main(), 2)
            self.assertIn("COULD NOT RUN", said.getvalue())
        finally:
            sys.argv = saved


class GdiLever(Patched):
    """The GDI lever is as the run found it, however the run ends (here: a load that fails)."""

    def launch(self, had, gpu):
        import tempfile
        import types
        with tempfile.TemporaryDirectory() as d:
            gamedir = Path(d)
            if had:
                (gamedir / "tagpu_gdi.on").write_text("")
            api = types.SimpleNamespace(Instance=lambda _name: types.SimpleNamespace(gamedir=gamedir))
            failed = types.SimpleNamespace(returncode=1, stdout="", stderr="no")
            self.patch(subprocess=types.SimpleNamespace(run=lambda *a, **k: failed,
                                                        TimeoutExpired=H.subprocess.TimeoutExpired))
            with self.assertRaises(OSError):
                H.run(api, "x", "easy", [], gpu)
            return (gamedir / "tagpu_gdi.on").exists(), (gamedir / "tagpu_hold.on").exists()

    def test_a_lever_the_instance_had_stays(self):
        self.assertEqual(self.launch(True, False), (True, False))
        self.assertEqual(self.launch(True, True), (True, False))

    def test_a_lever_the_run_wrote_goes(self):
        self.assertEqual(self.launch(False, False), (False, False))


class Strike(Patched):
    """Row 18's strike on a fake level held at GameTime 1, both seats' commanders and strikers placed."""
    DEF = 0x60000000

    def level(self, com_at=None):
        units = []
        for (seat, start, at, _facing), (com, gun) in zip(H.STRIKES, ((1501, 1510), (3001, 3010))):
            units += [{"type": "corcom", "owner": seat, "engine_index": com,
                       "world": list(com_at if com_at and seat == H.OTA else start) + [80]},
                      {"type": "corpun", "owner": seat, "engine_index": gun, "world": list(at) + [80]}]
        box = (-12 << 16, 0, -12 << 16, 12 << 16, 30 << 16, 12 << 16)
        words = {UNITS + com * H.UNIT_STRIDE + H.U_DEF: self.DEF for com in (1501, 3001)}
        words.update({self.DEF + H.DEF_BOX + 4 * k: v & 0xFFFFFFFF for k, v in enumerate(box)})
        level = Level(words, tick=1)
        roster = {"units": units}
        level.call = lambda *args: roster if args[0] == "roster" else level.orders.append(args)
        return level

    def counted_from(self, tick_of):
        """`counts` answering row 18 for each seat from its tick on, every other row empty."""
        def counts(c, at):
            hits = {r: (0, 0) for r in H.ROWS}
            hits[18] = tuple(int(tick_of.get(seat) is not None and c.tick >= tick_of[seat])
                             for seat in (H.OTA, H.IMPAI))
            return hits
        return counts

    def test_each_seat_counted_stops_its_striker(self):
        level = self.level()
        self.patch(counts=self.counted_from({H.OTA: 15, H.IMPAI: 19}))
        out = []
        self.assertEqual(H.strike(level, 0, out), [])
        attacks = [o for o in level.orders if o[1] == "attack"]
        stops = [o for o in level.orders if o[1] == "stop"]
        self.assertEqual([list(o[3:5]) for o in attacks], [list(s[1]) for s in H.STRIKES])
        self.assertEqual([o[3] for o in stops], [1510, 3010])
        self.assertEqual(level.tick, 19)

    def test_a_seat_not_counted_in_its_window_fails_by_name(self):
        level = self.level()
        self.patch(counts=self.counted_from({H.OTA: 10}))
        fails = H.strike(level, 0, [])
        self.assertEqual(len(fails), 1)
        self.assertIn(f"seat {H.IMPAI}", fails[0])
        self.assertEqual(level.tick - 1, H.STRIKE_TICKS)

    def test_a_commander_off_its_start_could_not_run(self):
        start = H.STRIKES[0][1]
        level = self.level(com_at=(start[0] + 20, start[1]))
        with self.assertRaises(OSError):
            H.strike(level, 0, [])
        self.assertEqual(level.orders, [])

    def test_the_probe_logs_shots_and_names_a_seat_not_counted(self):
        level = self.level()
        pool = 0x70000000
        for gun in (1510, 3010):
            u = UNITS + gun * H.UNIT_STRIDE
            level.words.update({u + H.U_ORDERS: 0, u + H.U_STATE: H.ST_ALIVE, u + H.U_NANO: 0, u + 0x108: 2540})
            for i in range(2):
                slot = bytearray(H.SLOT_STRIDE)
                struct.pack_into("<H", slot, 0x14, 0)
                level.blobs[u + H.U_SLOTS + i * H.SLOT_STRIDE] = bytes(slot)
        shot = bytearray(H.PROJ_STRIDE)
        struct.pack_into("<3I", shot, H.P_POS, 8400 << 16, 90 << 16, 4340 << 16)
        struct.pack_into("<I", shot, H.P_SHOOTER, UNITS + 1510 * H.UNIT_STRIDE)
        level.blobs[pool] = bytes(shot)
        plain = level.values
        pool_words = {f"*0x511DE8+0x{H.OFF_PROJ_COUNT:X}": 1, f"*0x511DE8+0x{H.OFF_PROJ_BEGIN:X}": pool}
        level.values = lambda *specs: [pool_words[s] if s in pool_words else plain(s)[0] for s in specs]
        self.patch(counts=self.counted_from({H.OTA: 5}))
        out = []
        fails = H.strike(level, 0, out, probe=10)
        self.assertEqual(len(fails), 1)
        self.assertIn(f"seat {H.IMPAI}", fails[0])
        self.assertEqual(level.tick, 11)
        self.assertEqual([o for o in level.orders if o[1] == "stop"], [])
        self.assertTrue(any("shot 0 at (8400.0, 90.0, 4340.0)" in line for line in out))
        self.assertTrue(any(f"seat {H.OTA} counted at GameTime 5" in line for line in out))

    def test_the_invariant_holds_at_the_bound(self):
        # the commander's walk and the step past the point stay inside the radius with the height's 6,
        # and inside the reach of area damage's cells
        self.assertLess((H.CORCOM_VMAX * H.STRIKE_TICKS + 12) ** 2 + 6 ** 2, H.STRIKE_RADIUS ** 2)
        self.assertLess(H.CORCOM_VMAX * H.STRIKE_TICKS + 12, H.WALK_REACH)


if __name__ == "__main__":
    unittest.main()
