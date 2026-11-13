#!/usr/bin/env python3
"""Offline tests of tools/wiki/stage3dcheck.py's walk through the mockup page (Mockup.view): it waits on the page's
own signals, never on a time, so how long the page takes to settle never changes the verdict; a page that cannot
start, or an eval that throws, is named by its reason and the page console's errors, never as an empty answer. The
browser is a stand-in that answers playwright-cli's calls as the page would: no browser, no server, no GPU.

    python3 tools/test_stage3dcheck.py
"""
import base64
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent / "wiki"))
import stage3dcheck  # noqa: E402

CONSOLE = ".playwright-cli/console-test.log"


def png(w=4, h=3):
    from PIL import Image
    b = io.BytesIO()
    Image.new("RGBA", (w, h), (10, 20, 30, 255)).save(b, "PNG")
    return "data:image/png;base64," + base64.b64encode(b.getvalue()).decode()


class Page:
    """playwright-cli on the mockup page, as stage3dcheck.Mockup.run calls it: `pending` slices of the page's settling
    (a host build of `pending` slices), then `settled`; the demo stepped to the tick asked; the stage's pack settled; the
    image. `threw` makes the first eval of the page's settling answer with an error, as an eval on a page with no
    WikiPage does. Every call is kept in `calls`."""

    def __init__(self, tmp, pending=0, settled="ready", threw=None, console=""):
        self.tmp, self.pending, self.settled, self.threw = tmp, pending, settled, threw
        self.calls, self.tick = [], None
        (tmp / CONSOLE).parent.mkdir(parents=True, exist_ok=True)
        (tmp / CONSOLE).write_text(console)

    @staticmethod
    def answer(value):
        return mock.Mock(returncode=0, stdout=f"### Result\n{json.dumps(value)}\n### Ran Playwright code\n```js\n```\n",
                         stderr="")

    def __call__(self, *a):
        cmd, js = a[0], a[1] if len(a) > 1 else ""
        if cmd == "console":
            self.calls.append("console")
            return mock.Mock(returncode=0, stdout=f"### Result\n- [Console]({CONSOLE})\n", stderr="")
        if cmd != "eval":
            self.calls.append(cmd)
            return mock.Mock(returncode=0, stdout="", stderr="")
        if "WikiPageFailed" in js:
            self.calls.append("settle page")
            if self.threw:
                return mock.Mock(returncode=0, stdout=f"### Error\n{self.threw}\n", stderr="")
            if self.pending:
                self.pending -= 1
                return self.answer("pending: the tree, its entries and the sidebar fixtures")
            return self.answer(self.settled)
        if "#c-replay" in js:
            self.calls.append("demo")
            self.tick = int(js.split("sim.tick < ", 1)[1].split(" ", 1)[0])
            return self.answer(json.dumps({"tick": self.tick}))
        if "WikiGfx.settled(" in js:
            self.calls.append("settle stage")
            return self.answer("ready")
        if "toDataURL" in js:
            self.calls.append("image")
            return self.answer(json.dumps({"tick": self.tick, "url": png(), "renderer": "stand-in"}))
        raise AssertionError(f"an eval the stand-in page does not know: {js[:80]}")


class View(unittest.TestCase):
    def setUp(self):
        t = tempfile.TemporaryDirectory()
        self.addCleanup(t.cleanup)
        self.tmp = Path(t.name)

    def mockup(self, page):
        mk = stage3dcheck.Mockup.__new__(stage3dcheck.Mockup)
        mk.port, mk.tmp, mk.run = 1, self.tmp, page
        return mk

    def test_the_verdict_does_not_depend_on_how_long_the_page_takes(self):
        """The gate's first page waits on serve.py building the simulation host, minutes under load: any number of
        pending slices gives the same image at the same tick, and the steps run in the same order."""
        seen = set()
        for pending in (0, 1, 200):
            page = Page(self.tmp, pending=pending)
            img, tick, renderer = self.mockup(page).image("buildshapes", 1, 180, 1, "terrain,features,units")
            self.assertEqual((img.size, img.mode, tick, renderer), ((4, 3), "RGBA", 180, "stand-in"))
            self.assertEqual(page.calls, ["goto"] + ["settle page"] * (pending + 1) + ["demo", "settle stage", "image"])
            seen.add((img.tobytes(), tick))
        self.assertEqual(len(seen), 1)

    def test_a_page_that_did_not_start_is_named_with_its_console(self):
        page = Page(self.tmp, pending=3, settled="the page did not start: WikiFrame is not defined",
                    console="Error: the simulation host cannot start: wikibuild.mjs host returned 1")
        with self.assertRaises(stage3dcheck.CannotRun) as x:
            self.mockup(page).image("buildshapes", 2, 100, 2, "terrain,features,units")
        msg = str(x.exception)
        self.assertIn("buildshapes demo 2 tick 100 ss 2", msg)
        self.assertIn("the page did not start: WikiFrame is not defined", msg)
        self.assertIn("the simulation host cannot start", msg)
        self.assertNotIn("demo", page.calls)

    def test_an_eval_that_threw_is_named_by_its_error(self):
        page = Page(self.tmp, threw="Error: page._evaluateFunction: ReferenceError: WikiPage is not defined")
        with self.assertRaises(stage3dcheck.CannotRun) as x:
            self.mockup(page).image("buildshapes", 1, 180, 1, "terrain,features,units")
        self.assertIn("ReferenceError: WikiPage is not defined", str(x.exception))
        self.assertNotIn("no answer", str(x.exception))

    def test_a_page_that_never_settles_fails_at_the_bound_naming_what_it_waited_for(self):
        page = Page(self.tmp, pending=10 ** 9)
        clock = iter(range(0, 10 ** 6, 100))
        with mock.patch.object(stage3dcheck, "SETTLE_S", 1000), \
                mock.patch.object(stage3dcheck.time, "monotonic", lambda: next(clock)):
            with self.assertRaises(stage3dcheck.CannotRun) as x:
                self.mockup(page).image("buildshapes", 1, 180, 1, "terrain,features,units")
        self.assertIn("still pending: the tree, its entries and the sidebar fixtures after 1000 s", str(x.exception))

    def test_section_reads_one_answer_section(self):
        out = "### Error\nError: boom\n  at x\n### Ran Playwright code\nawait page.evaluate()\n"
        self.assertEqual(stage3dcheck.section(out, "Error"), "Error: boom\n  at x")
        self.assertEqual(stage3dcheck.section(out, "Result"), "")


if __name__ == "__main__":
    unittest.main()
