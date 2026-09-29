"""A test runner may only adopt the X server it actually started."""
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parent / 'compat'))
import tacompat


class DisplayTests(unittest.TestCase):
    def test_readiness_accepts_fragmented_line(self):
        with patch.object(tacompat.select, 'select', return_value=([7], [], [])), \
             patch.object(tacompat.os, 'read', side_effect=[b'1', b'2', b'\n']) as read:
            self.assertEqual(tacompat.xvfb_reply(7), b'12\n')
            self.assertEqual(read.call_count, 3)

    def test_readiness_bounds_reply_and_stops_on_eof_or_timeout(self):
        for chunks, expected in (([b'8', b''], b'8'), ([b'1' * 32], b'1' * 32)):
            with patch.object(tacompat.select, 'select', return_value=([7], [], [])), \
                 patch.object(tacompat.os, 'read', side_effect=chunks):
                self.assertEqual(tacompat.xvfb_reply(7), expected)
        with patch.object(tacompat.select, 'select', return_value=([], [], [])), \
             patch.object(tacompat.os, 'read') as read:
            self.assertEqual(tacompat.xvfb_reply(7), b'')
            read.assert_not_called()

    def test_servers_reserve_distinct_displays_and_failed_claim_preserves_owner(self):
        first = tacompat.start_xvfb(None)
        second = None
        try:
            second = tacompat.start_xvfb(None)
            self.assertNotEqual(first.tacompat_display, second.tacompat_display)
            with self.assertRaises(RuntimeError):
                tacompat.start_xvfb(first.tacompat_display)
            self.assertIsNone(first.poll())
            result = subprocess.run(['xdpyinfo', '-display', f':{first.tacompat_display}'],
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0)
        finally:
            if second:
                tacompat.stop_xvfb(second)
            tacompat.stop_xvfb(first)


if __name__ == '__main__':
    unittest.main()
