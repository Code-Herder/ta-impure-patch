#!/usr/bin/env python3
"""Tests for tools/dpport.py against the DirectPlay files tools/dpinstall.sh installs.

Skipped where those files are not on the machine (TA_DIRECTPLAY_SRC, default
~/.local/share/ta-directplay): they are Microsoft's and never in the repository."""

import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dpport  # noqa: E402

SRC = Path(os.environ.get("TA_DIRECTPLAY_SRC", Path.home() / ".local/share/ta-directplay"))


@unittest.skipUnless(all((SRC / n).is_file() for n in dpport.SITES), "no DirectPlay files")
class DpPortTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="dpport-test-"))
        self.win = self.tmp / "drive_c" / "windows"
        for d in ("syswow64", "system32"):
            (self.win / d).mkdir(parents=True)
        for n in dpport.SITES:                      # a 64-bit prefix, as dpinstall.sh leaves it
            shutil.copy2(SRC / n, self.win / "syswow64" / n)
        shutil.copy2(SRC / "dplaysvr.exe", self.win / "system32" / "dplaysvr.exe")
        (self.win / "system32" / "dpwsockx.dll").write_bytes(b"wine's own 64-bit builtin")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def port_bytes(self, path):
        data = path.read_bytes()
        return {data[o:o + 2] for o in dpport.SITES[path.name][0]}

    def test_every_site_carries_the_port_and_stock_restores_the_file(self):
        done = dpport.set_port(self.tmp, 47700)
        self.assertEqual(len(done), 3)              # never wine's system32 dpwsockx.dll
        for p, _ in done:
            self.assertEqual(self.port_bytes(p), {(47700).to_bytes(2, "big")})
        dpport.set_port(self.tmp, dpport.STOCK)
        for n in dpport.SITES:
            self.assertEqual((self.win / "syswow64" / n).read_bytes(), (SRC / n).read_bytes())

    def test_a_hardlinked_copy_is_not_written_through(self):
        target = self.win / "syswow64" / "dpwsockx.dll"
        twin = self.tmp / "template-dpwsockx.dll"
        os.link(target, twin)
        dpport.set_port(self.tmp, 47701)
        self.assertEqual(twin.read_bytes(), (SRC / "dpwsockx.dll").read_bytes())
        self.assertNotEqual(target.stat().st_ino, twin.stat().st_ino)

    def test_an_unknown_build_is_refused_and_left_alone(self):
        target = self.win / "syswow64" / "dpwsockx.dll"
        data = bytearray(target.read_bytes())
        data[0x100] ^= 0xFF                         # one byte outside the sites
        target.write_bytes(data)
        with self.assertRaises(SystemExit):
            dpport.set_port(self.tmp, 47702)
        self.assertEqual(target.read_bytes(), bytes(data))

    def test_a_port_outside_the_range_is_refused(self):
        for bad in (0, 80, 70000):
            with self.assertRaises(SystemExit):
                dpport.set_port(self.tmp, bad)


if __name__ == "__main__":
    unittest.main()
