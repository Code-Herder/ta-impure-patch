"""The effective corpus decoder reads relocated tables, not disk offsets."""
import struct
import unittest

import cob_corpus_probe as corpus


class LoadedCobTests(unittest.TestCase):
    def loaded(self):
        cob = corpus.tacob.compile_bos('piece base; Create() { hide base; }')
        blob = bytearray(corpus.tacob.write_cob(cob))
        header = list(struct.unpack_from('<11I', blob))
        address = 0x100000
        for table, count in ((header[7], header[1]), (header[8], header[2])):
            for index in range(count):
                offset = table + index * 4
                struct.pack_into('<I', blob, offset, address + struct.unpack_from('<I', blob, offset)[0])
        for index in (6, 7, 8, 9):
            header[index] += address
        struct.pack_into('<11I', blob, 0, *header)

        def read(at, size):
            offset = at - address
            if offset < 0 or offset + size > len(blob):
                raise ValueError('read exceeds fixture allocation')
            return bytes(blob[offset:offset + size])

        return cob, blob, address, read

    def test_relocated_program_matches_original(self):
        original, _, address, read = self.loaded()
        decoded = corpus.loaded_cob(read, address)
        self.assertEqual(corpus.tacob.write_cob(decoded), corpus.tacob.write_cob(original))

    def test_counts_and_table_addresses_fail_closed(self):
        for offset, value in ((4, 4097), (12, 0xffffffff), (24, 0), (28, 0xffffffff)):
            with self.subTest(offset=offset):
                _, blob, address, read = self.loaded()
                struct.pack_into('<I', blob, offset, value)
                with self.assertRaises(ValueError):
                    corpus.loaded_cob(read, address)


if __name__ == '__main__':
    unittest.main()
