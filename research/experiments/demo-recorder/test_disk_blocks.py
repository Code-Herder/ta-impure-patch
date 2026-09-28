"""File-boundary and index invariants for the disposable IO experiment."""
import io
from pathlib import Path
import tempfile
import unittest

from disk_blocks import (BLOCK, DATA, FILE, LIMIT, checked_header,
                         file_header, index_tail, indexed_read, open_index,
                         read_block, recover, write_file)
from disk_crash import fixture


class DiskTests(unittest.TestCase):
    def setUp(self):
        self.raws = [bytes(range(256)), b'next state' * 12, b'final state' * 18]
        self.image, self.phases, self.ends, _ = fixture(self.raws)

    def with_file(self, image, action):
        with tempfile.TemporaryFile() as f:
            f.write(image)
            f.flush()
            return action(f)

    def test_all_truncations_preserve_exact_contiguous_prefix(self):
        for end in range(len(self.image)+1):
            actual = self.with_file(self.image[:end], recover)
            count = sum(at <= end for at in self.ends)
            self.assertEqual(actual['blocks'], count, end)
            self.assertEqual(actual['complete'], end == len(self.image), end)

    def test_indexed_reads_verify_identity_and_data(self):
        def check(f):
            entries, offset = open_index(f)
            for i in (2, 0, 1):
                self.assertEqual(indexed_read(f, entries, offset, i), self.raws[i])
            with self.assertRaises(ValueError):
                indexed_read(f, entries, offset, -1)
            with self.assertRaises(ValueError):
                indexed_read(f, [(0, entries[1][1]), *entries[1:]], offset, 0)
        self.with_file(self.image, check)

    def test_corrupt_middle_never_recovers_later_valid_blocks(self):
        damaged = bytearray(self.image)
        damaged[self.ends[0] + 33] ^= 0x80
        actual = self.with_file(damaged, recover)
        self.assertEqual(actual['blocks'], 1)
        self.assertFalse(actual['complete'])
        # Fast metadata open remains allowed, but cannot claim data verification.
        def check(f):
            entries, offset = open_index(f)
            with self.assertRaises(ValueError):
                indexed_read(f, entries, offset, 1)
        self.with_file(damaged, check)

    def test_forged_size_rejected_before_decode(self):
        forged = checked_header(BLOCK.pack(b'DISK', 1, LIMIT+1, 0, 0, DATA)) + b'x'
        f = io.BytesIO(file_header()+forged)
        with self.assertRaisesRegex(ValueError, 'bounds'):
            read_block(f, FILE.size, FILE.size+len(forged))

    def test_index_must_cover_contiguous_sequence_and_start(self):
        prefix = self.image[:self.ends[-1]]
        for entries in ([(1, FILE.size)], [(0, FILE.size+1)], [(0, FILE.size), (1, FILE.size)]):
            index, footer = index_tail(entries, len(prefix))
            with self.assertRaises(ValueError):
                self.with_file(prefix+index+footer, open_index)

    def test_unknown_kind_is_not_skipped(self):
        forged = checked_header(BLOCK.pack(b'DISK', 1, 1, 0, 0, 99)) + b'x'
        result = self.with_file(file_header()+forged+self.image[FILE.size:], recover)
        self.assertEqual(result['blocks'], 0)
        self.assertFalse(result['complete'])

    def test_writer_refuses_overwrite_and_closes_valid_file(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)/'one.bin'
            write_file(p, self.raws, 1)
            with p.open('rb') as f:
                self.assertTrue(recover(f)['complete'])
            with self.assertRaises(FileExistsError):
                write_file(p, self.raws, 1)


if __name__ == '__main__':
    unittest.main()
