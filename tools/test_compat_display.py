"""A test runner may only adopt the X server it actually started."""
from pathlib import Path
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parent / 'compat'))
import tacompat


class DisplayTests(unittest.TestCase):
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
