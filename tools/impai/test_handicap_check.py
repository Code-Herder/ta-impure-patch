#!/usr/bin/env python3
"""test_handicap_check -- the handicap check's peek of the stubs' count table, offline.

    python3 tools/impai/test_handicap_check.py

The table is s_diag.hits[18][2] of u32 (tagpu_impai.c): 144 bytes. tagpu_peek.c reads the N of an
x<N> size with parse_hex and clamps it to 256, so the request must spell 144 in hex; spelt in
decimal it asks 0x144 bytes, the peek answers 256 and the counts are misread."""
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


if __name__ == "__main__":
    unittest.main()
