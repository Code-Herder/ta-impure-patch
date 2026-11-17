"""A test runner may only adopt the X server it actually started; a setup's instance readied for a test launch
holds a DirectPlay port of its own."""
from pathlib import Path
import json
import socket
import struct
import subprocess
import sys
import tempfile
import threading
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

    def test_every_client_is_accepted_while_others_leave(self):
        """The server never regenerates (-noreset): one that does drops some of the clients that
        connect as others leave, and a game whose Wine loses its display ends before a window."""
        x = tacompat.start_xvfb(None)
        answers, lock = {}, threading.Lock()

        def one():
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(30)
            try:
                s.connect(f'/tmp/.X11-unix/X{x.tacompat_display}')
                s.sendall(struct.pack('<BxHHHHxx', ord('l'), 11, 0, 0, 0))
                head = b''
                while len(head) < 8:
                    chunk = s.recv(8 - len(head))
                    if not chunk:
                        return 'closed before the answer'
                    head += chunk
                return 'accepted' if head[0] == 1 else f'refused ({head[0]})'
            except OSError as e:
                return type(e).__name__
            finally:
                s.close()

        def loop():
            for _ in range(150):
                answer = one()
                with lock:
                    answers[answer] = answers.get(answer, 0) + 1

        try:
            workers = [threading.Thread(target=loop) for _ in range(4)]
            for t in workers:
                t.start()
            for t in workers:
                t.join()
            self.assertEqual(answers, {'accepted': 600})
            self.assertIsNone(x.poll())
        finally:
            tacompat.stop_xvfb(x)



class TestInstanceTests(unittest.TestCase):
    def test_test_instance_leases_its_dplay_port(self):
        """The parked store is put back, native DirectPlay marked and moved to a leased port, which no second lease
        can take until the instance's lease is closed."""
        with tempfile.TemporaryDirectory() as tmp, \
                patch.object(tacompat, 'INSTANCES', Path(tmp) / 'instances'), \
                patch.object(tacompat.dpport, 'LEASES', Path(tmp)):
            gamedir = Path(tmp) / 'gamedir'
            (gamedir / (tacompat.taremote.STATE_DIR + '.off')).mkdir(parents=True)
            meta_path = Path(tmp) / 'instances' / 'compat-x' / 'instance.json'
            meta_path.parent.mkdir(parents=True)
            meta_path.write_text(json.dumps({'name': 'compat-x'}))
            lease = tacompat.as_test_instance({'name': 'compat-x', 'gamedir': str(gamedir)})
            try:
                meta = json.loads(meta_path.read_text())
                self.assertTrue(meta['dplay'])
                self.assertEqual(meta['dplay_port'], lease.port)
                self.assertIn(lease.port, tacompat.dpport.LEASE_PORTS)
                self.assertTrue((gamedir / tacompat.taremote.STATE_DIR).is_dir())
                with self.assertRaises(SystemExit):
                    tacompat.dpport.lease(lease.port)
            finally:
                lease.close()
            tacompat.dpport.lease(lease.port).close()


if __name__ == '__main__':
    unittest.main()
