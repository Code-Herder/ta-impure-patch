"""Compile the production handover gate and exercise lost/skipped delivery."""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import unittest


class Gate(C.Structure):
    _fields_ = [('owed', C.c_int), ('recorded', C.c_int)]


class GuiResetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='gui-reset-')
        source = Path(cls.temp.name) / 'gate.c'
        source.write_text('''#include "tagpu_gui_reset.h"
void arm(TagpuGuiReset* g) { tagpu_gui_reset_arm(g); }
int begin(TagpuGuiReset* g) { return tagpu_gui_reset_begin(g); }
int ready(TagpuGuiReset* g) { return tagpu_gui_reset_ready(g); }
void record(TagpuGuiReset* g) { tagpu_gui_reset_record(g); }
void deliver(TagpuGuiReset* g, int lost) { tagpu_gui_reset_deliver(g, lost); }
''')
        library = source.with_suffix('.so')
        inc = Path(__file__).resolve().parents[1] / 'tagpu/ddraw/inc'
        subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-shared',
                        '-fPIC', '-I', str(inc), str(source), '-o', str(library)], check=True)
        cls.lib = C.CDLL(str(library))
        for name in ('arm', 'begin', 'ready', 'record'):
            getattr(cls.lib, name).argtypes = [C.POINTER(Gate)]
        cls.lib.deliver.argtypes = [C.POINTER(Gate), C.c_int]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_lost_or_skipped_reset_cannot_release_suffix_history(self):
        for delivery in ('lost', 'skipped', 'success'):
            with self.subTest(delivery=delivery):
                gate = Gate()
                ref = C.byref(gate)
                self.lib.arm(ref)
                self.assertEqual(self.lib.begin(ref), 0)
                self.assertEqual(self.lib.ready(ref), 0)
                self.lib.record(ref)
                self.assertEqual(self.lib.ready(ref), 1)
                if delivery != 'skipped':
                    self.lib.deliver(ref, delivery == 'lost')
                self.assertEqual(self.lib.begin(ref), delivery != 'success')
                self.assertEqual(self.lib.ready(ref), delivery == 'success')
                if delivery != 'success':
                    self.lib.record(ref)
                    self.lib.deliver(ref, 0)
                    self.assertEqual(self.lib.begin(ref), 0)
                    self.assertEqual(self.lib.ready(ref), 1)
                self.lib.arm(ref)
                self.assertEqual(self.lib.ready(ref), 0)

    def test_waiting_frames_do_not_request_a_reset_storm(self):
        gate = Gate()
        ref = C.byref(gate)
        self.lib.arm(ref)
        for _ in range(100):
            self.assertEqual(self.lib.begin(ref), 0)
            self.assertEqual(self.lib.ready(ref), 0)


if __name__ == '__main__':
    unittest.main()
