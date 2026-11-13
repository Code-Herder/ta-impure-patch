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


if __name__ == "__main__":
    unittest.main()
