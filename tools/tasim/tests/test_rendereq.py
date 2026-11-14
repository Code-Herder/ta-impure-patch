"""rendereq.py's comparison offline: two renderers' runs set side by side, on synthetic traces and
image digests. No game and no recording.

    python3 tools/tasim/tests/test_rendereq.py
"""

import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import rendereq  # noqa: E402

TRACE = [{"scenario": "s", "run": 0, "t0": 91}, {"t": 0, "u": {"a": [1, 2]}}, {"t": 5, "u": {"a": [1, 1]}}]


def img(t_diff=None, off=0x10):
    out = {}
    for t in range(6):
        b = bytearray(0x118)
        if t_diff is not None and t >= t_diff:
            b[off] = 1
        out[t] = (str(bytes(b)), {3: bytes(b)})
    return out


class RendererEquality(unittest.TestCase):
    def run_(self, ta, tb, ia, ib):
        with mock.patch.object(rendereq, "traces", side_effect=[ta, tb]), \
                mock.patch.object(rendereq, "image_digests", side_effect=[ia, ib]):
            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = rendereq.compare(Path("g"), Path("v"), 1)
        return rc, buf.getvalue()

    def test_the_heads_absolute_gametime_is_not_compared(self):
        later = [dict(TRACE[0], t0=391)] + TRACE[1:]
        self.assertIsNone(rendereq.first_trace_difference(rendereq.comparable(TRACE), rendereq.comparable(later)))

    def test_equal_runs_pass_and_report_their_images(self):
        k = ("scenarios/calib/exact.json", 0)
        t = {k: (rendereq.comparable(TRACE), "ok")}
        rc, said = self.run_(t, t, {k: img()}, {k: img(t_diff=3, off=0x5C)})
        self.assertEqual(rc, 0, said)
        self.assertIn("EQUAL", said)
        self.assertIn("first differs at t 3: unit offsets +0x5C", said)

    def test_a_differing_trace_fails_at_its_line(self):
        k = ("scenarios/calib/exact.json", 0)
        b = rendereq.comparable(TRACE)[:2] + [{"t": 5, "u": {"a": [1, 0]}}]
        rc, said = self.run_({k: (rendereq.comparable(TRACE), "ok")}, {k: (b, "ok")}, {k: img()}, {k: img()})
        self.assertEqual(rc, 1)
        self.assertIn("DIFFERS: scenarios/calib/exact.json run 0: trace line 2", said)

    def test_the_pacing_comparison_names_its_two_sides(self):
        k0, k1 = ("s", 0), ("s", 1)
        t = rendereq.comparable(TRACE)
        with mock.patch.object(rendereq, "traces", side_effect=[{k0: (t, "ok"), k1: (t, "ok")}, {k0: (t, "ok")}]), \
                mock.patch.object(rendereq, "image_digests", side_effect=[{}, {}]):
            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = rendereq.compare(Path("d"), Path("n"), 1, rendereq.PACINGS)
        self.assertEqual(rc, 2)
        self.assertIn("MISSING: s run 1: played by drawn only", buf.getvalue())
        self.assertIn("(2 drawn runs, 1 no-draw runs)", buf.getvalue())

    def test_a_run_one_renderer_lacks_or_a_failed_start_check_could_not_run(self):
        k0, k1 = ("s", 0), ("s", 1)
        t = rendereq.comparable(TRACE)
        rc, said = self.run_({k0: (t, "ok"), k1: (t, "ok")}, {k0: (t, "ok")}, {}, {})
        self.assertEqual(rc, 2)
        self.assertIn("MISSING: s run 1: played by GDI only", said)
        rc, _ = self.run_({k0: (t, "R4: x")}, {k0: (t, "ok")}, {}, {})
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
