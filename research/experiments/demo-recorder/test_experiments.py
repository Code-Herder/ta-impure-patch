"""Tests for the research measurements, not for game replay fidelity."""
import struct
import unittest
import zlib

from blocks import HEADER, LIMIT, RECORD, encode, read_block, recover
from trace import decode, split


class DecodeTests(unittest.TestCase):
    def test_binary_chat_envelope_is_not_trimmed(self):
        message = bytes([5, 0, 74]) + bytes(range(1, 35)) + bytes(28)
        plain = bytearray(b'\x03\0\0' + struct.pack('<I', 42) + message)
        for i in range(3, len(plain) - 3):
            plain[i] ^= i & 255
        struct.pack_into('<H', plain, 1, sum(plain[3:-3]) & 0xFFFF)
        self.assertEqual(list(split(decode(plain))), [message])
        plain[4] ^= 1
        with self.assertRaisesRegex(ValueError, 'checksum'):
            decode(plain)

    def test_bad_message_lengths(self):
        for payload in (b'\x2c\0\0', b'\x2c\xff\xff', b'\x05\0', b'\x80'):
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                list(split(payload))

    def test_bad_compression_reference(self):
        # A reference into output which has not yet been produced.
        wire = bytearray(b'\x04\0\0\x01\xf0\xff\0\0\0')
        for i in range(3, len(wire) - 3):
            wire[i] ^= i & 255
        struct.pack_into('<H', wire, 1, sum(wire[3:-3]) & 0xFFFF)
        with self.assertRaisesRegex(ValueError, 'reference beyond'):
            decode(wire)


class BlockTests(unittest.TestCase):
    def setUp(self):
        self.payload = bytes([5, 0, 74]) + bytes(range(62))
        self.raw = RECORD.pack(5, 0xFEDCBA98, 0, len(self.payload), 1) + self.payload
        self.block = encode(self.raw, 5, 5, 1, 3)

    def test_every_truncation_recovers_only_complete_prefix(self):
        for cut in range(len(self.block)):
            self.assertEqual(recover(self.block + self.block[:cut])[0], len(self.block))
        offset, recovered = recover(self.block * 2)
        self.assertEqual(offset, len(self.block) * 2)
        self.assertEqual(recovered[0][0][-1], self.payload)

    def test_single_bit_corruption_never_changes_accepted_events(self):
        for at in range(len(self.block)):
            for bit in range(8):
                changed = bytearray(self.block)
                changed[at] ^= 1 << bit
                offset, recovered = recover(self.block + changed)
                if offset > len(self.block):
                    self.assertEqual(recovered[1], recovered[0])

    def test_header_budget_checked_before_decompression(self):
        bad = HEADER.pack(b'DXB1', 0, LIMIT + 1, 5, 5, 1, 0)
        bad += struct.pack('<I', zlib.crc32(bad))
        with self.assertRaisesRegex(ValueError, 'header bounds'):
            read_block(bad, 0)

    def test_record_count_and_length_verified(self):
        with self.assertRaisesRegex(ValueError, 'count'):
            read_block(encode(self.raw, 5, 5, 2, 3), 0)
        wrong_length = RECORD.pack(5, 1, 0, 100, 1) + self.payload
        with self.assertRaisesRegex(ValueError, 'bounds'):
            read_block(encode(wrong_length, 5, 5, 1, 3), 0)


if __name__ == '__main__':
    unittest.main()
