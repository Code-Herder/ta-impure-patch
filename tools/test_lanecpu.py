#!/usr/bin/env python3
"""Offline tests of tools/lanecpu.py's verdict: the cgroup's counter read as threads, the peak's
window, and the lines judged from a lane's log. No game runs.

    python3 tools/test_lanecpu.py
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lanecpu  # noqa: E402

GDI_LOG = ("cfg: max_resolutions 0, toggle_borderless, lock_surfaces, singlecpu off, maintas; "
           "display=window vsync=1 window=default; renderer GDI (tagpu_gdi.on)\nrecorder: recording\n")


class LaneCpu(unittest.TestCase):
    def test_usage_is_read_from_cpu_stat(self):
        self.assertEqual(lanecpu.usage_usec("usage_usec 1234\nuser_usec 1000\nsystem_usec 234\n"), 1234)
        with self.assertRaises(lanecpu.CouldNotRun):
            lanecpu.usage_usec("user_usec 1\n")

    def test_threads_and_the_windowed_peak(self):
        samples = [(0.0, 0), (1.0, 2_000_000), (2.0, 3_000_000), (4.0, 7_000_000)]
        rates = lanecpu.threads(samples)
        self.assertEqual([r for _t, r in rates], [2.0, 1.0, 2.0])
        self.assertEqual(lanecpu.peak(rates, 2), 1.5)
        self.assertAlmostEqual(lanecpu.peak(rates, 10), 5 / 3)
        self.assertEqual(lanecpu.peak([], 10), 0.0)

    def test_the_verdict(self):
        rates = [(t, 1.5) for t in range(20)]
        lines, ok = lanecpu.judge(0, {"lane0-1": GDI_LOG}, rates, 2.2)
        self.assertTrue(ok, lines)
        _l, ok = lanecpu.judge(0, {"lane0-1": GDI_LOG + "vk: up in 226 ms\n"}, rates, 2.2)
        self.assertFalse(ok)                               # a Vulkan device was made
        _l, ok = lanecpu.judge(0, {"lane0-1": GDI_LOG.replace("GDI (tagpu_gdi.on)", "Vulkan")}, rates, 2.2)
        self.assertFalse(ok)
        _l, ok = lanecpu.judge(0, {"lane0-1": GDI_LOG}, [(t, 3.0) for t in range(20)], 2.2)
        self.assertFalse(ok)                               # above the Vulkan lane
        _l, ok = lanecpu.judge(2, {"lane0-1": GDI_LOG}, rates, 2.2)
        self.assertFalse(ok)
        _l, ok = lanecpu.judge(0, {}, rates, 2.2)
        self.assertFalse(ok)


if __name__ == "__main__":
    unittest.main()
