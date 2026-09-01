#!/usr/bin/env python3
"""Offline tests for tacli's pure logic — no game, no wine, no instance.

Everything under test here is a function that turns a snapshot dict (the JSON the
fork writes) into a decision: which gadget a selector names, where to click a
listbox row, what to type, what the table says. Those are exactly the places a
wrong answer is expensive and silent — a click lands somewhere plausible and the
session drifts — and they are the only parts that can be checked without a
running game.

    python3 tools/test_tacli.py [-v]
"""

import contextlib
import io
import unittest
from importlib.machinery import SourceFileLoader
from pathlib import Path

tacli = SourceFileLoader("tacli", str(Path(__file__).with_name("tacli"))).load_module()


@contextlib.contextmanager
def refuses(case):
    """Assert the call dies, and keep its message off the test run's output."""
    with case.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
        yield


def gadget(**kw):
    """A gadget with the common fields filled in, overridable per test."""
    g = {"i": 1, "id": 1, "type": "button", "name": "B", "assoc": 0,
         "rect": [0, 0, 60, 20], "click": [30, 10], "active": 1, "attribs": 0,
         "help": "", "text": "", "stages": 0, "stage": 0, "quickkey": 0,
         "grayed": 0}
    g.update(kw)
    return g


def listbox(**kw):
    g = {"i": 1, "id": 2, "type": "listbox", "name": "L", "assoc": 5,
         "rect": [10, 20, 100, 50], "click": [60, 45], "active": 1,
         "attribs": 0x10, "help": "", "selected": 0, "top": 0, "maxtop": 0,
         "count": 0, "itemheight": 15, "rowpitch": 15, "items": []}
    g.update(kw)
    return g


def snapshot(gadgets, **kw):
    s = {"ok": True, "frame": 1, "surface": [640, 480], "gui": "TEST.GUI",
         "enter": "", "esc": "", "focus": "", "uichange": -1,
         "panel": [0, 0, 640, 480], "under": [], "gadgets": gadgets}
    s.update(kw)
    return s


class Kind(unittest.TestCase):
    """A button's stage count is what makes it a toggle, a cycle, or neither."""

    def test_stages_decide_the_kind(self):
        self.assertEqual(tacli._ui_kind(gadget(stages=0)), "button")
        self.assertEqual(tacli._ui_kind(gadget(stages=2)), "toggle")
        self.assertEqual(tacli._ui_kind(gadget(stages=3)), "cycle")

    def test_button2_is_still_a_button(self):
        self.assertEqual(tacli._ui_kind(gadget(type="button2", stages=2)), "toggle")

    def test_listbox_renders_as_list(self):
        self.assertEqual(tacli._ui_kind(listbox()), "list")


class Actionability(unittest.TestCase):
    """TA expresses "you cannot have this" two different ways; both must block."""

    def test_active_and_grayed_are_separate_refusals(self):
        self.assertIsNone(tacli._ui_blocked(gadget()))
        self.assertEqual(tacli._ui_blocked(gadget(active=0)), "inactive")
        self.assertEqual(tacli._ui_blocked(gadget(grayed=1)), "grayed out")

    def test_a_grayed_gadget_is_blocked_even_while_active(self):
        # ARMLAB1.GUI's IGPATCH reads exactly this live.
        self.assertEqual(tacli._ui_blocked(gadget(active=1, grayed=1)), "grayed out")

    def test_zero_sized_is_blocked(self):
        self.assertEqual(tacli._ui_blocked(gadget(rect=[0, 0, 0, 20])), "zero-sized")


class Selectors(unittest.TestCase):
    def setUp(self):
        self.snap = snapshot([
            gadget(i=1, name="TEXT"),
            gadget(i=2, name="TEXT", type="label"),
            gadget(i=3, name=""),
        ])

    def test_name_match_is_case_insensitive(self):
        self.assertEqual([g["i"] for g in tacli._ui_match(self.snap, "text")], [1, 2])

    def test_type_prefix_disambiguates(self):
        self.assertEqual([g["i"] for g in tacli._ui_match(self.snap, "button:TEXT")], [1])
        self.assertEqual([g["i"] for g in tacli._ui_match(self.snap, "label:TEXT")], [2])

    def test_index_selector_reaches_an_unnamed_gadget(self):
        # The scrollbar arrows have empty names; #index is the only way in.
        self.assertEqual([g["i"] for g in tacli._ui_match(self.snap, "#3")], [3])

    def test_absent_name_matches_nothing(self):
        self.assertEqual(tacli._ui_match(self.snap, "nope"), [])


class ItemIndex(unittest.TestCase):
    def setUp(self):
        self.lb = listbox(count=3, items=["Alpha", "Beta", "Gamma"])

    def test_by_text_is_case_insensitive(self):
        self.assertEqual(tacli._ui_item_index(self.lb, "beta"), 1)

    def test_by_index(self):
        self.assertEqual(tacli._ui_item_index(self.lb, "#2"), 2)

    def test_unknown_text_is_fatal(self):
        with refuses(self):
            tacli._ui_item_index(self.lb, "Delta")

    def test_out_of_range_index_is_fatal(self):
        with refuses(self):
            tacli._ui_item_index(self.lb, "#3")

    def test_duplicate_text_is_strict(self):
        dup = listbox(count=2, items=["Same", "Same"])
        with refuses(self):
            tacli._ui_item_index(dup, "Same")


class RowGeometry(unittest.TestCase):
    """Rows sit at a fixed pitch from the top, not spread over the height."""

    def test_first_row_is_two_pixels_below_the_top(self):
        lb = listbox(rect=[10, 20, 100, 50], rowpitch=15, count=3,
                     items=["a", "b", "c"])
        point, why = tacli._ui_row_click(lb, 0)
        self.assertIsNone(why)
        self.assertEqual(point, (60, 20 + 2 + 7))

    def test_rows_step_by_the_pitch(self):
        lb = listbox(rect=[10, 20, 100, 50], rowpitch=15, count=3,
                     items=["a", "b", "c"])
        self.assertEqual(tacli._ui_row_click(lb, 2)[0][1],
                         tacli._ui_row_click(lb, 0)[0][1] + 30)

    def test_scroll_offset_is_subtracted(self):
        lb = listbox(rect=[10, 20, 100, 50], rowpitch=15, top=2, count=5,
                     items=list("abcde"))
        self.assertEqual(tacli._ui_row_click(lb, 2), tacli._ui_row_click(
            listbox(rect=[10, 20, 100, 50], rowpitch=15, count=5,
                    items=list("abcde")), 0))

    def test_a_row_below_the_fold_is_refused_not_guessed(self):
        lb = listbox(rect=[10, 20, 100, 32], rowpitch=15, count=5,
                     items=list("abcde"))
        point, why = tacli._ui_row_click(lb, 4)
        self.assertIsNone(point)
        self.assertIn("scrolled out of view", why)

    def test_a_row_above_the_fold_is_refused(self):
        lb = listbox(rect=[10, 20, 100, 32], rowpitch=15, top=3, count=5,
                     items=list("abcde"))
        self.assertIsNone(tacli._ui_row_click(lb, 0)[0])

    def test_unknown_pitch_is_refused_rather_than_estimated(self):
        lb = listbox(rowpitch=0, count=3, items=["a", "b", "c"])
        point, why = tacli._ui_row_click(lb, 0)
        self.assertIsNone(point)
        self.assertIn("row pitch", why)


class TextTokens(unittest.TestCase):
    """No key tokens: a held shift capitalises everything behind it in a batch."""

    def test_no_shift_token_is_ever_emitted(self):
        for tok in tacli._ui_text_tokens("Claude"):
            self.assertNotIn("shift", tok)

    def test_letters_digits_and_punctuation_all_go_as_chars(self):
        self.assertEqual(tacli._ui_text_tokens("Ab9_-"),
                         ["char:A", "char:b", "char:9", "char:_", "char:-"])

    def test_space_keeps_its_key_token(self):
        self.assertEqual(tacli._ui_text_tokens(" "), ["space"])

    def test_non_ascii_is_refused(self):
        with refuses(self):
            tacli._ui_text_tokens("café")


class Batching(unittest.TestCase):
    """The fork reads the token file in one bounded gulp and deletes it."""

    def split(self, tokens):
        batches, cur = [], ""
        for w in tokens:
            if cur and len(cur) + 1 + len(w) > tacli.KEYS_BATCH:
                batches.append(cur)
                cur = w
            else:
                cur = f"{cur} {w}" if cur else w
        batches.append(cur)
        return batches

    def test_every_batch_fits_the_forks_buffer(self):
        batches = self.split(["backspace"] * 200)
        self.assertGreater(len(batches), 1)
        for b in batches:
            self.assertLessEqual(len(b) + 1, tacli.KEYS_BATCH)

    def test_no_token_is_lost_or_split(self):
        tokens = ["char:x"] * 300
        self.assertEqual(" ".join(self.split(tokens)).split(), tokens)


class Groups(unittest.TestCase):
    """assoc is on every gadget, so only a shared one is worth showing."""

    def test_a_lone_toggle_is_not_a_group(self):
        self.assertEqual(tacli._ui_groups(snapshot([
            gadget(i=1, name="A", assoc=7, stages=2)])), {})

    def test_two_toggles_sharing_an_assoc_are(self):
        self.assertIn(7, tacli._ui_groups(snapshot([
            gadget(i=1, name="A", assoc=7, stages=2),
            gadget(i=2, name="B", assoc=7, stages=2)])))

    def test_a_scrollbar_bound_to_its_list_is(self):
        self.assertIn(5, tacli._ui_groups(snapshot([
            listbox(i=1, assoc=5),
            gadget(i=2, type="slider", id=4, name="S", assoc=5)])))

    def test_plain_buttons_sharing_an_assoc_are_not(self):
        # MAINMENU's buttons all carry an assoc and none of it means anything.
        self.assertEqual(tacli._ui_groups(snapshot([
            gadget(i=1, name="A", assoc=126),
            gadget(i=2, name="B", assoc=126)])), {})


class Items(unittest.TestCase):
    """Unknown and empty are different answers and must read differently."""

    def test_empty_text_list_says_empty(self):
        self.assertIn("(empty)", tacli._ui_render(snapshot([listbox()])))

    def test_picture_list_says_unknown(self):
        out = tacli._ui_render(snapshot([listbox(items=None, attribs=0x80)]))
        self.assertIn("items unknown", out)
        self.assertNotIn("(empty)", out)

    def test_state_column_distinguishes_them(self):
        self.assertIn("n=0", tacli._ui_state(listbox()))
        self.assertIn("n=?", tacli._ui_state(listbox(items=None)))

    def test_selected_row_is_marked(self):
        line = tacli._ui_item_line(listbox(count=3, selected=1,
                                           items=["a", "b", "c"]))
        self.assertIn("*1 b", line)
        self.assertIn("0 a", line)

    def test_long_lists_are_truncated_with_a_count(self):
        lb = listbox(count=12, items=[str(i) for i in range(12)])
        self.assertIn("+4 more", tacli._ui_item_line(lb))


class Paging(unittest.TestCase):
    """Build pages are <UNIT><n>.GUI, but not everything ending in a digit is one."""

    def test_a_build_page_reports_its_number(self):
        self.assertEqual(tacli._ui_page({"gui": "ARMCOM2.GUI"}), ("ARMCOM", 2))

    def test_the_stats_screen_is_not_a_build_page(self):
        self.assertEqual(tacli._ui_page({"gui": "ARMMAIN2.GUI"}), (None, None))

    def test_an_ordinary_screen_is_not_a_build_page(self):
        self.assertEqual(tacli._ui_page({"gui": "SKIRMISH.GUI"}), (None, None))


class QuickKeys(unittest.TestCase):
    def test_upper_case_ascii_becomes_a_lower_case_token(self):
        self.assertEqual(tacli._ui_keyname(ord("S")), "s")

    def test_quickkey_is_ascii_not_a_virtual_key(self):
        self.assertEqual(tacli._ui_keyname(ord("7")), "7")
        # 0x70 is 'p' here — ARMPATROL's accelerator — not VK_F1.
        self.assertEqual(tacli._ui_keyname(0x70), "p")

    def test_a_non_ascii_quickkey_has_no_token(self):
        # One stock gadget declares quickkey=-68, which is 0xBC as a u8.
        self.assertIsNone(tacli._ui_keyname(0xBC))

    def test_named_keys(self):
        self.assertEqual(tacli._ui_keyname(13), "return")
        self.assertEqual(tacli._ui_keyname(8), "backspace")

    def test_no_quickkey_is_none(self):
        self.assertIsNone(tacli._ui_keyname(0))


class Labels(unittest.TestCase):
    def test_hover_reports_only_text_that_appeared(self):
        before = snapshot([gadget(i=1, type="label", name="A", text="one")])
        after = snapshot([gadget(i=1, type="label", name="A", text="one"),
                          gadget(i=2, type="label", name="B", text="two")])
        self.assertEqual(tacli._ui_label_diff(before, after), ["two"])

    def test_nothing_new_is_an_empty_list(self):
        snap = snapshot([gadget(i=1, type="label", name="A", text="one")])
        self.assertEqual(tacli._ui_label_diff(snap, snap), [])


if __name__ == "__main__":
    unittest.main()
