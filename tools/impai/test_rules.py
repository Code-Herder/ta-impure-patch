#!/usr/bin/env python3
"""test_rules.py -- rules.py against a small source of the brain's layout, offline."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rules  # noqa: E402

SOURCE = """\
#include "impai.h"
#define CAP 4

/* THE HOLD: while the store is low the seat holds every spend that is not economy (THE PRESSURE lifts it). */
static void hold_read(IMPAI_DEC* d)
{
    d->hold = d->e.metal < 10 && !d->pressed;   /* not a rule: the store alone */
}

static int short_one(const IMPAI_DEC* d) { return d->hold ? 1 : 0; }

/* the governor obeys THE HOLD; AT THE RALLY, a squad fights. */
static void governor(IMPAI_DEC* d,
                     int n)
{
    if (short_one(d)) d->queued++;
    d->busy[n] = 1;
    hold_read(d);
}
"""


class Rules(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        (Path(self.dir.name) / "impai_x.c").write_text(SOURCE)
        self.fns = rules.load(Path(self.dir.name))

    def tearDown(self):
        self.dir.cleanup()

    def by(self, name):
        return next(f for f in self.fns if f.name == name)

    def test_functions(self):
        self.assertEqual([f.name for f in self.fns], ["hold_read", "short_one", "governor"])
        self.assertEqual((self.by("short_one").start, self.by("short_one").end), (10, 10))
        self.assertEqual(self.by("governor").end, 19)

    def test_fields(self):
        self.assertEqual(self.by("hold_read").writes, {"hold"})
        self.assertEqual(self.by("hold_read").reads, {"e", "pressed"})
        self.assertEqual(self.by("governor").writes, {"queued", "busy"})

    def test_rules_and_where_they_open(self):
        self.assertEqual(self.by("hold_read").rules, {"THE HOLD": True, "THE PRESSURE": False})
        self.assertEqual(self.by("governor").rules, {"THE HOLD": False, "AT THE RALLY": True})
        out = rules.show_rule(self.fns, "THE HOLD")
        self.assertIn("* impai_x.c:5 hold_read", out[1])
        self.assertIn("governor", out[2])

    def test_calls_and_field_answer(self):
        self.assertEqual(self.by("governor").calls & {"short_one", "hold_read"}, {"short_one", "hold_read"})
        text = "\n".join(rules.show_fn(self.fns, "short_one") + rules.show_field(self.fns, "hold"))
        self.assertIn("called by: governor", text)
        self.assertIn("w impai_x.c:5 hold_read", text)
        self.assertIn("r impai_x.c:10 short_one", text)

    def test_main(self):
        self.assertEqual(rules.main(["--src", self.dir.name, "THE HOLD", "hold"]), 0)
        self.assertEqual(rules.main(["--src", self.dir.name, "nothing_here"]), 1)


if __name__ == "__main__":
    unittest.main()
