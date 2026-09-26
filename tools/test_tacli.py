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

import base64
import contextlib
import fnmatch
import hashlib
import io
import json
import ntpath
import re
import tempfile
import types
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

    def test_the_last_row_stays_inside_the_hit_box(self):
        # The box stops four pixels short of the bottom edge (0x4A3AE6).
        lb = listbox(rect=[10, 20, 100, 34], rowpitch=15, count=2, items=["a", "b"])
        point, why = tacli._ui_row_click(lb, 1)
        self.assertIsNone(why)
        self.assertLessEqual(point[1], 20 + 34 - 5)

    def test_unknown_pitch_is_refused_rather_than_estimated(self):
        lb = listbox(rowpitch=0, count=3, items=["a", "b", "c"])
        point, why = tacli._ui_row_click(lb, 0)
        self.assertIsNone(point)
        self.assertIn("row pitch", why)


def slider(**kw):
    g = {"i": 2, "id": 4, "type": "slider", "name": "S", "assoc": 9,
         "rect": [100, 50, 120, 16], "click": [160, 58], "active": 1,
         "attribs": 1, "help": "", "pos": 0, "range": 107, "knobsize": 10,
         "thick": 64, "horizontal": 1, "nomouse": 0, "value": 0}
    g.update(kw)
    return g


class SliderValue(unittest.TestCase):
    """pos is pixels, value is what the engine acts on; never confuse them."""

    def test_zero_is_zero(self):
        self.assertEqual(tacli._ui_slider_pos(slider(), 0), 0)

    def test_full_scale_lands_on_the_last_pixel(self):
        # value == thick must reach range-1, or the top of the scale is unreachable.
        self.assertEqual(tacli._ui_slider_pos(slider(), 64), 106)

    def test_the_inverse_round_trips(self):
        # The engine truncates on the way out, so ceil on the way in must not
        # undershoot: every value has to read back as itself.
        g = slider()
        rng, thick = g["range"], g["thick"]
        for v in range(thick + 1):
            pos = tacli._ui_slider_pos(g, v)
            self.assertEqual(pos * thick // (rng - 1), v, f"value {v}")

    def test_pos_never_leaves_the_track(self):
        g = slider()
        for v in range(g["thick"] + 1):
            self.assertTrue(0 <= tacli._ui_slider_pos(g, v) <= g["range"] - 1)


class SliderDrag(unittest.TestCase):
    """The knob moves one pixel per unit, so the release is the grab plus delta."""

    def test_horizontal_grab_lands_on_the_knob(self):
        g = slider(pos=20, knobsize=10, rect=[100, 50, 120, 16])
        grab, _ = tacli._ui_slider_drag(g, 20)
        self.assertEqual(grab, (100 + 20 + 1 + 5, 50 + 8))

    def test_release_is_the_grab_shifted_by_the_delta(self):
        g = slider(pos=20)
        grab, release = tacli._ui_slider_drag(g, 50)
        self.assertEqual(release[0] - grab[0], 30)
        self.assertEqual(release[1], grab[1])

    def test_vertical_starts_two_pixels_in_and_moves_in_y(self):
        g = slider(horizontal=0, pos=20, knobsize=10, rect=[100, 50, 16, 120])
        grab, release = tacli._ui_slider_drag(g, 10)
        self.assertEqual(grab, (100 + 8, 50 + 20 + 2 + 5))
        self.assertEqual(release[0], grab[0])
        self.assertEqual(release[1] - grab[1], -10)

    def test_no_movement_when_already_there(self):
        g = slider(pos=33)
        grab, release = tacli._ui_slider_drag(g, 33)
        self.assertEqual(grab, release)


class SliderBinding(unittest.TestCase):
    """A scrollbar is a slave of its list; only an unbound slider holds a value."""

    def test_a_matching_listbox_makes_it_a_scrollbar(self):
        snap = snapshot([listbox(assoc=9), slider(assoc=9)])
        self.assertEqual(tacli._ui_bound_list(snap, slider(assoc=9))["name"], "L")

    def test_an_unmatched_slider_is_a_value(self):
        snap = snapshot([listbox(assoc=1), slider(assoc=243)])
        self.assertIsNone(tacli._ui_bound_list(snap, slider(assoc=243)))

    def test_state_shows_the_value_not_the_pixels(self):
        self.assertEqual(tacli._ui_state(slider(pos=53, value=31)), "val=31/64")

    def test_a_rangeless_slider_says_so(self):
        self.assertIn("no value", tacli._ui_state(slider(value=None, pos=7)))


class Selectable(unittest.TestCase):
    """Two independent ways a row refuses the selection, from two engine sites."""

    def test_an_ordinary_row_is_selectable(self):
        lb = listbox(count=2, items=["a", "b"], separator=[0, 0])
        self.assertIsNone(tacli._ui_item_selectable(lb, 1))

    def test_a_separator_row_is_not(self):
        lb = listbox(count=2, items=["a", "b"], separator=[0, 1])
        self.assertIn("separator", tacli._ui_item_selectable(lb, 1))

    def test_a_disabled_row_is_not(self):
        lb = listbox(count=2, items=["a", "b"], separator=[0, 0], itemflags=[1, 0])
        self.assertIn("disabled", tacli._ui_item_selectable(lb, 1))

    def test_absent_flag_arrays_mean_no_objection(self):
        # Most screens have neither array; that is not a reason to refuse.
        self.assertIsNone(tacli._ui_item_selectable(
            listbox(count=1, items=["a"]), 0))


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


# --------------------------------------------------------------------- scenarios
#
# The scenario compiler is the one component that decides what reaches the
# engine, so it is the one worth testing exhaustively: a wrong answer here is
# 400 units in the wrong place, or a file that silently spawns nothing.


def scn(**kw):
    """A scenario document with the one required key filled in."""
    doc = {"format": tacli.SCN_FORMAT}
    doc.update(kw)
    return doc


def group(**kw):
    g = {"id": "wave", "owner": 1, "at": [1000, 1200],
         "pattern": {"kind": "grid", "cols": 20, "spacing": 24},
         "composition": [{"type": "ARMPW", "count": 200}]}
    g.update(kw)
    return g


def unit(**kw):
    u = {"type": "ARMPW", "owner": 1, "pos": [900, 1200]}
    u.update(kw)
    return u


def compile_doc(doc, catalogue=None):
    """Validate + expand, the whole pipeline a file goes through before the wire."""
    return tacli._scn_expand(tacli._scn_validate(doc, "test.json", catalogue))


CATALOGUE = {"units": ["ARMPW", "ARMROCK", "CORAK", "ARMCOM"],
             "features": ["ARMCOM_DEAD", "TREE1"],
             "maps": ["Two Continents", "Painted Desert"]}


class ScenarioSchema(unittest.TestCase):
    """Unknown keys are errors: these files are written by agents, and a typo'd
    "unit" for "units" under a permissive schema is an empty map."""

    def test_unknown_top_level_key_is_fatal(self):
        with refuses(self):
            compile_doc(scn(unit=[]))

    def test_a_foreign_format_is_refused(self):
        with refuses(self):
            compile_doc({"format": "ta-scenario/2"})

    def test_a_missing_required_key_is_fatal(self):
        with refuses(self):
            compile_doc(scn(units=[{"owner": 1, "pos": [10, 10]}]))     # no type

    def test_null_means_unset_not_a_type_error(self):
        exp = compile_doc(scn(units=[unit(facing=None, health=None)]))
        self.assertIsNone(exp["units"][0]["facing"])

    def test_a_string_where_a_number_belongs_is_fatal(self):
        with refuses(self):
            compile_doc(scn(units=[unit(health="60")]))

    def test_out_of_map_coordinates_are_fatal(self):
        with refuses(self):
            compile_doc(scn(units=[unit(pos=[900, 99999])]))

    def test_a_position_must_be_a_pair(self):
        with refuses(self):
            compile_doc(scn(units=[unit(pos=[900, 1200, 30])]))

    def test_duplicate_handles_are_fatal(self):
        with refuses(self):
            compile_doc(scn(units=[unit(id="a"), unit(id="a", pos=[10, 10])]))

    def test_an_author_handle_cannot_use_the_synthesized_marker(self):
        # '#' is reserved so a synthesized handle can never collide with a written one.
        with refuses(self):
            compile_doc(scn(units=[unit(id="wave#3")]))

    def test_facing_is_normalized_into_a_circle(self):
        self.assertEqual(compile_doc(scn(units=[unit(facing=-90)]))["units"][0]["facing"],
                         270)

    def test_out_of_range_facing_is_fatal_rather_than_wrapped(self):
        with refuses(self):
            compile_doc(scn(units=[unit(facing=9000)]))


class ScenarioOrders(unittest.TestCase):
    def test_attack_move_names_the_idiom_that_replaces_it(self):
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()) as err:
            compile_doc(scn(units=[unit(orders=[{"cmd": "attack-move", "to": [10, 10]}])]))
        self.assertIn("no such order", err.getvalue())
        self.assertIn('"attack"', err.getvalue())

    def test_unknown_orders_are_fatal(self):
        with refuses(self):
            compile_doc(scn(units=[unit(orders=[{"cmd": "harvest", "to": [10, 10]}])]))

    def test_guard_is_tas_defend(self):
        exp = compile_doc(scn(units=[unit(orders=[{"cmd": "guard", "to": [10, 10]}])]))
        self.assertEqual(exp["units"][0]["orders"][0]["cmd"], "defend")

    def test_an_order_needs_exactly_one_target(self):
        with refuses(self):
            compile_doc(scn(units=[unit(orders=[{"cmd": "attack"}])]))
        with refuses(self):
            compile_doc(scn(units=[unit(orders=[{"cmd": "attack", "to": [1, 1],
                                                 "target": "x"}])]))

    def test_stop_takes_none(self):
        self.assertEqual(len(compile_doc(scn(units=[unit(orders=[{"cmd": "stop"}])]))
                             ["units"][0]["orders"]), 1)
        with refuses(self):
            compile_doc(scn(units=[unit(orders=[{"cmd": "stop", "to": [1, 1]}])]))

    def test_a_target_may_be_declared_later_in_the_file(self):
        doc = scn(units=[unit(id="a", orders=[{"cmd": "attack", "target": "b"}]),
                         unit(id="b", pos=[10, 10])])
        self.assertEqual(compile_doc(doc)["units"][0]["orders"][0]["target"], "b")

    def test_an_undeclared_target_is_fatal(self):
        with refuses(self):
            compile_doc(scn(units=[unit(orders=[{"cmd": "attack", "target": "ghost"}])]))

    def test_a_group_is_not_a_targetable_entity(self):
        with refuses(self):
            compile_doc(scn(groups=[group()],
                            units=[unit(orders=[{"cmd": "attack", "target": "wave"}])]))

    def test_a_feature_is_targetable(self):
        # Reclaiming a wreck is the whole point of naming features.
        doc = scn(units=[unit(orders=[{"cmd": "reclaim", "target": "wreck"}])],
                  features=[{"id": "wreck", "type": "ARMCOM_DEAD", "pos": [10, 10]}])
        self.assertEqual(compile_doc(doc)["units"][0]["orders"][0]["target"], "wreck")


class ScenarioSetup(unittest.TestCase):
    def test_shootall_is_on_without_being_asked_for(self):
        self.assertTrue(compile_doc(scn())["setup"]["switches"]["shootall"])

    def test_an_explicit_false_wins(self):
        exp = compile_doc(scn(setup={"switches": {"shootall": False}}))
        self.assertFalse(exp["setup"]["switches"]["shootall"])

    def test_asking_for_resources_warns_that_apply_cannot_honour_them(self):
        # Measured live: the applier's write lands and TA recomputes storage from
        # the player's own units within about a second. `load` sets them for real,
        # through the registry, before the game exists — so the warning is about
        # the verb that cannot, and a file using it must not look like it worked.
        exp = compile_doc(scn(setup={"players": [{"slot": 0, "metal": 5000}]},
                              units=[unit(owner=0)]))
        self.assertTrue(any("metal/energy" in w for w in exp["warnings"]))

    def test_launching_does_not_warn_about_what_launching_can_do(self):
        doc = scn(setup={"players": [{"slot": 0, "metal": 5000}]},
                  units=[unit(owner=0)])
        norm = tacli._scn_validate(doc, "t", None, launching=True)
        self.assertEqual(norm["warnings"], [])

    def test_declaring_a_player_without_resources_is_quiet(self):
        exp = compile_doc(scn(setup={"players": [{"slot": 0}]}, units=[unit(owner=0)]))
        self.assertEqual(exp["warnings"], [])

    def test_an_unknown_switch_is_fatal(self):
        with refuses(self):
            compile_doc(scn(setup={"switches": {"godmode": True}}))

    def test_clear_existing_defaults_true(self):
        # A scenario contains exactly what the file says, commanders included.
        self.assertTrue(compile_doc(scn())["setup"]["clear_existing"])

    def test_duplicate_player_slots_are_fatal(self):
        with refuses(self):
            compile_doc(scn(setup={"players": [{"slot": 1}, {"slot": 1}]}))

    def test_an_owner_must_be_a_declared_player(self):
        with refuses(self):
            compile_doc(scn(setup={"players": [{"slot": 1}]},
                            units=[unit(owner=3)]))

    def test_owners_are_range_checked_even_with_no_players_declared(self):
        with refuses(self):
            compile_doc(scn(units=[unit(owner=99)]))

    def test_slot_zero_is_a_real_player(self):
        # Measured 2026-09-01: TA's own registry keys are Player0Controller..
        # Player9Controller and the roster reports own=0 for the first of them,
        # so the slots are the engine's 0-based Players[] indices. A schema that
        # started at 1 could not name the human seat a plain launch uses.
        exp = compile_doc(scn(setup={"players": [{"slot": 0}]},
                              units=[unit(owner=0)]))
        self.assertEqual(exp["counts"]["by_owner"], {"0": 1})

    def test_slot_ten_is_not(self):
        with refuses(self):
            compile_doc(scn(setup={"players": [{"slot": 10}]}))

    def test_a_malformed_resolution_is_fatal(self):
        with refuses(self):
            compile_doc(scn(setup={"res": "1024*"}))


class ScenarioCatalogue(unittest.TestCase):
    """Layer 2: names checked against the live game, when one has been asked."""

    def test_an_unknown_unit_is_fatal(self):
        with refuses(self):
            compile_doc(scn(units=[unit(type="NOSUCH")]), CATALOGUE)

    def test_a_known_unit_passes_and_case_does_not_matter(self):
        self.assertEqual(compile_doc(scn(units=[unit(type="armpw")]),
                                     CATALOGUE)["units"][0]["type"], "ARMPW")

    def test_an_unknown_feature_is_fatal(self):
        with refuses(self):
            compile_doc(scn(features=[{"type": "NOPE", "pos": [10, 10]}]), CATALOGUE)

    def test_an_unknown_map_is_fatal(self):
        with refuses(self):
            compile_doc(scn(setup={"map": "Tw Continents"}), CATALOGUE)

    def test_without_a_catalogue_layer_two_does_not_run(self):
        self.assertEqual(compile_doc(scn(units=[unit(type="NOSUCH")]))["counts"]["units"], 1)

    def test_a_richer_catalogue_entry_still_yields_its_name(self):
        # Phase B's catalogue carries footprints too; layer 2 must not care.
        cat = {"units": [{"name": "ARMPW", "footprint": [2, 2]}]}
        self.assertEqual(compile_doc(scn(units=[unit()]), cat)["units"][0]["type"],
                         "ARMPW")


class ScenarioOnError(unittest.TestCase):
    """`on_error: "skip"` opts into best effort, and layer 2 has to honour it."""

    def test_abort_is_the_default_and_an_unknown_name_is_fatal(self):
        with refuses(self):
            compile_doc(scn(units=[unit(type="ARMNOPE")]), CATALOGUE)

    def test_skip_turns_it_into_a_warning(self):
        exp = compile_doc(scn(on_error="skip",
                              units=[unit(id="a", type="ARMPW"),
                                     unit(id="b", type="ARMNOPE", pos=[901, 1200])]),
                          CATALOGUE)
        self.assertEqual([u["type"] for u in exp["units"]], ["ARMPW", "ARMNOPE"])
        self.assertTrue(any("ARMNOPE" in w for w in exp["warnings"]))

    def test_the_unknown_name_reaches_the_wire_untouched(self):
        # Best effort is per-entity, and only the fork can do it per-entity: it
        # re-checks every name against the live game anyway. So the CLI passes the
        # name through rather than dropping the entity and reshuffling ordinals.
        exp = compile_doc(scn(on_error="skip", units=[unit(type="ARMNOPE")]), CATALOGUE)
        wire = tacli._scn_wire(exp)
        self.assertIn("onerror=skip", wire)
        self.assertIn("unit 0 ARMNOPE", wire)

    def test_skip_does_not_excuse_a_malformed_name(self):
        # Layer 1 is still absolute: "not a name" is different from "not in this game".
        with refuses(self):
            compile_doc(scn(on_error="skip", units=[unit(type="not a name!")]), CATALOGUE)


class ScenarioCanonicalNames(unittest.TestCase):
    """TA's own tables disagree about case — units are ARMPW, wrecks are
    armlab_dead — so the catalogue's spelling is the one that reaches the wire."""

    def test_the_catalogue_spelling_wins(self):
        cat = {"features": ["armlab_dead"]}
        exp = compile_doc(scn(features=[{"type": "ARMLAB_DEAD", "pos": [10, 10]}]), cat)
        self.assertEqual(exp["features"][0]["type"], "armlab_dead")

    def test_without_a_catalogue_the_author_spelling_is_kept(self):
        # Inventing a case here would be inventing a name.
        exp = compile_doc(scn(features=[{"type": "armlab_dead", "pos": [10, 10]}]))
        self.assertEqual(exp["features"][0]["type"], "armlab_dead")

    def test_a_map_name_is_canonicalized_too(self):
        exp = compile_doc(scn(setup={"map": "two continents"}), CATALOGUE)
        self.assertEqual(exp["setup"]["map"], "Two Continents")

    def test_names_come_out_of_objects_or_strings(self):
        self.assertEqual(tacli._scn_names({"units": ["ARMPW"]}, "units"),
                         {"ARMPW": "ARMPW"})
        self.assertEqual(tacli._scn_names({"units": [{"name": "armpw"}]}, "units"),
                         {"ARMPW": "armpw"})

    def test_no_list_is_not_an_empty_list(self):
        # "the catalogue does not carry maps" and "this game has no maps" are
        # different answers: only the second may fail a scenario.
        self.assertIsNone(tacli._scn_names({"units": ["ARMPW"]}, "maps"))


class ScenarioFootprints(unittest.TestCase):
    """A grid tighter than the units are wide is not the layout the file asks for."""

    CAT = {"units": [{"name": "ARMPW", "footprint": [2, 2]},
                     {"name": "ARMFLASH", "footprint": [3, 3]}]}

    def warnings(self, spacing, **pattern):
        p = {"kind": "grid", "cols": 5, "spacing": spacing}
        p.update(pattern)
        return compile_doc(scn(groups=[group(
            pattern=p, composition=[{"type": "ARMPW", "count": 10}])]),
            self.CAT)["warnings"]

    def test_a_tight_grid_warns_with_the_number_that_would_fix_it(self):
        w = self.warnings(12)
        self.assertEqual(len(w), 1)
        self.assertIn("32 world units", w[0])       # 2 cells x 16
        self.assertIn("ARMPW", w[0])

    def test_room_enough_says_nothing(self):
        self.assertEqual(self.warnings(40), [])

    def test_the_worst_unit_in_the_composition_sets_the_bar(self):
        w = compile_doc(scn(groups=[group(
            pattern={"kind": "grid", "cols": 5, "spacing": 40},
            composition=[{"type": "ARMPW", "count": 2},
                         {"type": "ARMFLASH", "count": 2}])]), self.CAT)["warnings"]
        self.assertIn("ARMFLASH", w[0])             # 3 cells x 16 = 48 > 40

    def test_a_random_scatter_has_no_spacing_to_judge(self):
        self.assertEqual(compile_doc(scn(groups=[group(
            pattern={"kind": "random", "size": [100, 100]},
            composition=[{"type": "ARMPW", "count": 10}])]), self.CAT)["warnings"], [])

    def test_a_catalogue_with_no_footprints_cannot_warn(self):
        # At the menu TA has parsed the names but not the FBIs: footprints read
        # 0x0 there, and a zero is "unknown", never "fits anywhere".
        cat = {"units": [{"name": "ARMPW", "footprint": [0, 0]}]}
        self.assertEqual(tacli._scn_footprints(cat), {})
        self.assertEqual(compile_doc(scn(groups=[group(
            pattern={"kind": "grid", "cols": 5, "spacing": 1},
            composition=[{"type": "ARMPW", "count": 10}])]), cat)["warnings"], [])


class ScenarioGrid(unittest.TestCase):
    """`at` is the centre of the formation, everywhere `at` appears."""

    def setUp(self):
        self.units = compile_doc(scn(groups=[group()]))["units"]

    def test_every_member_is_expanded(self):
        self.assertEqual(len(self.units), 200)

    def test_the_block_is_centred_on_at(self):
        xs = [u["pos"][0] for u in self.units]
        ys = [u["pos"][1] for u in self.units]
        self.assertEqual((min(xs) + max(xs)) // 2, 1000)
        self.assertEqual((min(ys) + max(ys)) // 2, 1200)

    def test_rows_are_row_major_at_the_declared_pitch(self):
        self.assertEqual(self.units[1]["pos"][0] - self.units[0]["pos"][0], 24)
        self.assertEqual(self.units[1]["pos"][1], self.units[0]["pos"][1])
        self.assertEqual(self.units[20]["pos"][1] - self.units[0]["pos"][1], 24)
        self.assertEqual(self.units[20]["pos"][0], self.units[0]["pos"][0])

    def test_a_short_group_does_not_sit_off_to_one_side(self):
        # 3 units with cols=20 is one short row, still centred on `at`.
        u = compile_doc(scn(groups=[group(
            composition=[{"type": "ARMPW", "count": 3}])]))["units"]
        self.assertEqual([p["pos"] for p in u],
                         [[976, 1200], [1000, 1200], [1024, 1200]])

    def test_composition_order_is_the_layout_order(self):
        u = compile_doc(scn(groups=[group(composition=[
            {"type": "ARMPW", "count": 2}, {"type": "ARMROCK", "count": 1}])]))["units"]
        self.assertEqual([p["type"] for p in u], ["ARMPW", "ARMPW", "ARMROCK"])

    def test_members_are_handled_by_group_and_index(self):
        self.assertEqual(self.units[7]["id"], "wave#7")
        self.assertEqual(self.units[7]["group"], "wave")

    def test_jitter_never_moves_a_unit_further_than_asked(self):
        g = group(pattern={"kind": "grid", "cols": 20, "spacing": 24, "jitter": 6})
        jittered = compile_doc(scn(groups=[g]))["units"]
        for a, b in zip(self.units, jittered):
            self.assertLessEqual(abs(a["pos"][0] - b["pos"][0]), 6)
            self.assertLessEqual(abs(a["pos"][1] - b["pos"][1]), 6)

    def test_a_group_pushed_off_the_map_is_caught_after_expansion(self):
        with refuses(self):
            compile_doc(scn(groups=[group(at=[50, 500])]))


class ScenarioPatterns(unittest.TestCase):
    def line(self, **pattern):
        p = {"kind": "line", "spacing": 10}
        p.update(pattern)
        return compile_doc(scn(groups=[group(
            pattern=p, composition=[{"type": "ARMPW", "count": 3}])]))["units"]

    def test_a_line_runs_along_x_and_is_centred(self):
        self.assertEqual([u["pos"] for u in self.line()],
                         [[990, 1200], [1000, 1200], [1010, 1200]])

    def test_the_layout_angle_turns_it(self):
        self.assertEqual([u["pos"] for u in self.line(angle=90)],
                         [[1000, 1190], [1000, 1200], [1000, 1210]])

    def test_random_stays_inside_its_rectangle(self):
        u = compile_doc(scn(groups=[group(
            pattern={"kind": "random", "size": [100, 40]},
            composition=[{"type": "ARMPW", "count": 50}])]))["units"]
        for p in u:
            self.assertTrue(950 <= p["pos"][0] <= 1050, p["pos"])
            self.assertTrue(1180 <= p["pos"][1] <= 1220, p["pos"])

    def test_each_kind_takes_only_its_own_keys(self):
        with refuses(self):      # random has no spacing
            compile_doc(scn(groups=[group(pattern={"kind": "random", "spacing": 10})]))
        with refuses(self):      # grid needs cols
            compile_doc(scn(groups=[group(pattern={"kind": "grid", "spacing": 10})]))

    def test_an_unknown_pattern_is_fatal(self):
        with refuses(self):
            compile_doc(scn(groups=[group(pattern={"kind": "spiral", "spacing": 10})]))


class ScenarioDeterminism(unittest.TestCase):
    """An expansion is a published artifact: two runs, two machines, months apart."""

    def test_the_same_document_expands_identically(self):
        doc = scn(seed=7, groups=[group(pattern={"kind": "grid", "cols": 20,
                                                 "spacing": 24, "jitter": 6})])
        a, b = compile_doc(doc), compile_doc(doc)
        self.assertEqual(json.dumps(a, sort_keys=True), json.dumps(b, sort_keys=True))

    def test_a_different_seed_lays_it_out_differently(self):
        g = group(pattern={"kind": "grid", "cols": 20, "spacing": 24, "jitter": 6})
        a = compile_doc(scn(seed=1, groups=[g]))["units"]
        b = compile_doc(scn(seed=2, groups=[g]))["units"]
        self.assertNotEqual([u["pos"] for u in a], [u["pos"] for u in b])

    def test_an_unseeded_file_is_still_deterministic(self):
        doc = scn(groups=[group()])
        self.assertEqual(compile_doc(doc)["seed"], compile_doc(doc)["seed"])

    def test_the_derived_seed_survives_reformatting(self):
        a = scn(description="x", units=[unit()])
        b = {"units": [unit()], "description": "x", "format": tacli.SCN_FORMAT}
        self.assertEqual(compile_doc(a)["seed"], compile_doc(b)["seed"])

    def test_changing_the_situation_changes_the_derived_seed(self):
        self.assertNotEqual(compile_doc(scn(units=[unit()]))["seed"],
                            compile_doc(scn(units=[unit(pos=[10, 10])]))["seed"])

    def test_the_generator_is_a_pure_function_of_its_seed(self):
        a, b = tacli._ScnRandom(99), tacli._ScnRandom(99)
        self.assertEqual([a.next() for _ in range(5)], [b.next() for _ in range(5)])

    def test_seed_zero_does_not_freeze_the_stream(self):
        r = tacli._ScnRandom(0)
        self.assertNotEqual(r.next(), r.next())


class ScenarioLimits(unittest.TestCase):
    def test_over_the_declared_unit_limit_is_fatal(self):
        with refuses(self):
            compile_doc(scn(setup={"unit_limit": 100}, groups=[group()]))

    def test_over_tas_stock_cap_only_warns(self):
        exp = compile_doc(scn(groups=[group(composition=[{"type": "ARMPW",
                                                          "count": 300}])]))
        self.assertTrue(any("250" in w for w in exp["warnings"]))

    def test_units_are_counted_per_player_not_in_total(self):
        exp = compile_doc(scn(setup={"unit_limit": 250},
                              groups=[group(id="a", owner=1),
                                      group(id="b", owner=2, at=[2400, 1200])]))
        self.assertEqual(exp["counts"]["by_owner"], {"1": 200, "2": 200})


class ScenarioCamera(unittest.TestCase):
    def test_a_group_target_compiles_to_a_coordinate(self):
        exp = compile_doc(scn(groups=[group()], camera={"center_on": "wave"}))
        self.assertEqual(exp["camera"]["at"], [1000, 1200])

    def test_an_entity_target_stays_a_handle_for_the_fork_to_resolve(self):
        # It must centre on where the unit actually landed after the terrain snap.
        exp = compile_doc(scn(units=[unit(id="hero")], camera={"center_on": "hero"}))
        self.assertEqual(exp["camera"]["on"], ["unit", 0, "hero"])
        self.assertIsNone(exp["camera"]["at"])

    def test_a_feature_target_carries_its_own_ordinal(self):
        exp = compile_doc(scn(features=[{"id": "w", "type": "TREE1", "pos": [10, 10]}],
                              camera={"center_on": "w"}))
        self.assertEqual(exp["camera"]["on"], ["feat", 0, "w"])

    def test_exactly_one_of_at_and_center_on(self):
        with refuses(self):
            compile_doc(scn(camera={}))
        with refuses(self):
            compile_doc(scn(units=[unit(id="hero")],
                            camera={"at": [1, 1], "center_on": "hero"}))

    def test_pin_defaults_off(self):
        self.assertFalse(compile_doc(scn(camera={"at": [10, 10]}))["camera"]["pin"])


class ScenarioWire(unittest.TestCase):
    """The private line format the fork scans. Columns are positional; `-` is unset."""

    def wire(self, doc):
        return tacli._scn_wire(compile_doc(doc)).splitlines()

    def test_a_unit_line_carries_every_column_in_order(self):
        lines = self.wire(scn(units=[unit(id="hero", type="ARMCOM", facing=90,
                                          health=60, stance="hold")]))
        self.assertIn("unit 0 ARMCOM 1 900 1200 - 90 60 hold -", lines)

    def test_unset_fields_are_dashes_not_guesses(self):
        self.assertIn("unit 0 ARMPW 1 900 1200 - - - - -", self.wire(scn(units=[unit()])))

    def test_the_header_carries_the_seed_and_the_counts(self):
        head = self.wire(scn(seed=7, units=[unit()]))[1]
        self.assertIn("seed=7", head)
        self.assertIn("units=1", head)
        self.assertIn("onerror=abort", head)

    def test_a_map_name_keeps_its_spaces(self):
        self.assertIn("map Two Continents", self.wire(scn(setup={"map": "Two Continents"})))

    def test_switches_are_emitted_in_a_stable_order(self):
        line = [l for l in self.wire(scn(setup={"switches": {"noshake": True}}))
                if l.startswith("sw ")][0]
        self.assertEqual(line, "sw noshake=1 shootall=1")

    def test_orders_come_after_every_entity(self):
        # The fork creates the whole situation first, so an order can name a unit
        # that is spawned later in the same tick.
        lines = self.wire(scn(units=[unit(id="a", orders=[{"cmd": "attack",
                                                           "target": "b"}]),
                                     unit(id="b", pos=[10, 10])]))
        self.assertLess(max(i for i, l in enumerate(lines) if l.startswith("unit ")),
                        min(i for i, l in enumerate(lines) if l.startswith("order ")))

    def test_the_three_order_shapes(self):
        lines = self.wire(scn(
            units=[unit(id="a", orders=[{"cmd": "attack", "to": [2400, 1200]},
                                        {"cmd": "defend", "target": "b"},
                                        {"cmd": "reclaim", "target": "w"},
                                        {"cmd": "stop"}]),
                   unit(id="b", pos=[10, 10])],
            features=[{"id": "w", "type": "TREE1", "pos": [20, 20]}]))
        self.assertIn("order 0 attack pos 2400 1200", lines)
        self.assertIn("order 0 defend unit 1", lines)
        self.assertIn("order 0 reclaim feat 0", lines)
        self.assertIn("order 0 stop", lines)

    def test_ordinals_are_per_kind_and_start_at_zero(self):
        lines = self.wire(scn(units=[unit()],
                              features=[{"type": "TREE1", "pos": [20, 20]}]))
        self.assertTrue(any(l.startswith("unit 0 ") for l in lines))
        self.assertTrue(any(l.startswith("feat 0 ") for l in lines))

    def test_the_camera_line_takes_both_shapes(self):
        self.assertIn("cam pos 1700 1200 pin=1",
                      self.wire(scn(camera={"at": [1700, 1200], "pin": True})))
        self.assertIn("cam feat 0 pin=0",
                      self.wire(scn(features=[{"id": "w", "type": "TREE1",
                                               "pos": [20, 20]}],
                                    camera={"center_on": "w"})))

    def test_it_ends_with_a_marker_so_a_truncated_file_is_visible(self):
        self.assertEqual(self.wire(scn(units=[unit()]))[-1], "end")


class ScenarioResults(unittest.TestCase):
    """The fork answers by ordinal; the CLI puts the author's handles back."""

    def result(self, **kw):
        r = {"ok": 1, "units": {"0": {"engine_index": 7}}, "features": {}}
        r.update(kw)
        return r

    def test_ordinals_become_handles(self):
        exp = compile_doc(scn(units=[unit(id="hero")],
                              features=[{"id": "wreck", "type": "TREE1",
                                         "pos": [10, 10]}]))
        res = self.result(units={"0": {"engine_index": 7}},
                          features={"0": {"def": 3}})
        tacli._scn_rekey(res, exp)
        self.assertEqual(set(res["units"]), {"hero"})
        self.assertEqual(set(res["features"]), {"wreck"})
        self.assertEqual(res["units"]["hero"]["ordinal"], 0)

    def test_an_ordinal_with_no_entity_stays_visible(self):
        # A stale result against a re-edited scenario must not vanish silently.
        exp = compile_doc(scn(units=[unit(id="hero")]))
        res = self.result(units={"0": {}, "9": {}})
        tacli._scn_rekey(res, exp)
        self.assertEqual(set(res["units"]), {"hero", "#9"})

    def test_a_missing_section_is_left_alone(self):
        res = {"ok": 0, "errors": ["nothing was created"]}
        tacli._scn_rekey(res, compile_doc(scn()))
        self.assertEqual(res, {"ok": 0, "errors": ["nothing was created"]})


class Roster(unittest.TestCase):
    """The roster line carries UnitInGameIndex; logs without it still have to parse."""

    def parse(self, line):
        return tacli.ROSTER_RX.match(line)

    def test_it_reads_the_engine_index(self):
        m = self.parse("  u001 ARMCOM       own=0 idx=17 world=(1600,1600,91) "
                       "screen=(512,384) nano=0.00")
        self.assertEqual((m.group(2), int(m.group(4)), int(m.group(5))),
                         ("ARMCOM", 17, 1600))

    def test_a_line_written_before_idx_existed_still_parses(self):
        m = self.parse("  u001 ARMCOM       own=0 world=(1600,1600,91) "
                       "screen=(512,384) nano=0.00")
        self.assertIsNotNone(m)
        self.assertIsNone(m.group(4))


class LoadComposition(unittest.TestCase):
    """`scenario load` is composition: what it decides before anything launches."""

    def flags(self, players):
        setup = tacli._scn_setup({"setup": {"players": players}}, "t")
        return tacli._scn_player_flags(setup)

    def test_a_slot_becomes_the_launch_flag_a_human_would_type(self):
        self.assertEqual(self.flags([{"slot": 0, "controller": "human",
                                      "side": "arm", "color": 3}]), ["0:1:0:3"])

    def test_core_is_side_one_and_ai_is_controller_two(self):
        self.assertEqual(self.flags([{"slot": 1, "controller": "ai",
                                      "side": "core"}]), ["1:2:1"])

    def test_an_undeclared_field_is_left_alone_rather_than_guessed(self):
        # --player's fields are positional, so a colour with no side needs the
        # empty middle field: the alternative is writing a side nobody asked for.
        self.assertEqual(self.flags([{"slot": 2, "color": 4}]), ["2:2::4"])

    def test_a_player_with_nothing_but_a_slot_still_names_its_controller(self):
        self.assertEqual(self.flags([{"slot": 5}]), ["5:2"])

    def test_no_players_means_no_flags(self):
        self.assertEqual(self.flags([]), [])


class PlayerSpecs(unittest.TestCase):
    """`--player` is positional, and the compiler writes it: both ends, one test."""

    def test_the_three_keys_are_written_in_order(self):
        self.assertEqual(tacli.player_keys("0:1:0:3"),
                         [("Player0Controller", "1"), ("Player0Side", "0"),
                          ("Player0Color", "3")])

    def test_an_omitted_tail_writes_nothing_extra(self):
        self.assertEqual(tacli.player_keys("5:2"), [("Player5Controller", "2")])

    def test_an_empty_field_is_skipped_not_written_blank(self):
        # `wine reg add` with empty data would set the key to nothing at all.
        self.assertEqual(tacli.player_keys("0:1::3"),
                         [("Player0Controller", "1"), ("Player0Color", "3")])

    def test_a_spec_with_no_controller_is_refused_rather_than_ignored(self):
        with refuses(self):
            tacli.player_keys("2")

    def test_a_field_that_is_not_a_number_is_refused(self):
        # reg add fails quietly, so this would read back as a rule nobody set.
        with refuses(self):
            tacli.player_keys("0:human")

    def test_starting_resources_are_the_last_two_fields(self):
        # Measured live: TA reads these as the starting level *and* the storage,
        # which is why the same figure written into a running game does not stick.
        self.assertEqual(tacli.player_keys("0:1:0:0:5000:4000")[-2:],
                         [("Player0Metal", "5000"), ("Player0Energy", "4000")])

    def test_energy_can_be_set_without_metal(self):
        self.assertEqual(tacli.player_keys("0:1::::4000"),
                         [("Player0Controller", "1"), ("Player0Energy", "4000")])

    def test_a_seventh_field_is_refused(self):
        with refuses(self):
            tacli.player_keys("0:1:0:3:1000:1000:9")

    def test_every_flag_the_compiler_emits_parses(self):
        # The one seam between the two halves: setup.players compiles to these
        # strings and nothing else reads them.
        setup = tacli._scn_setup({"setup": {"players": [
            {"slot": 0, "controller": "human", "side": "arm", "color": 0,
             "metal": 5000, "energy": 5000},
            {"slot": 1, "controller": "ai", "side": "core"},
            {"slot": 2, "color": 4},
            {"slot": 3, "energy": 2000},
            {"slot": 9, "controller": "off"}]}}, "t")
        for flag in tacli._scn_player_flags(setup):
            with self.subTest(flag=flag):
                self.assertTrue(tacli.player_keys(flag))


class LoadedMap(unittest.TestCase):
    """TA falls back silently on a map it does not have, so `load` reads it back."""

    def test_the_tnt_file_names_the_map_the_scenario_asked_for(self):
        self.assertTrue(tacli._scn_map_matches("Two Continents.tnt", "Two Continents"))

    def test_a_directory_and_a_case_difference_are_not_a_mismatch(self):
        self.assertTrue(tacli._scn_map_matches("maps\\TWO CONTINENTS.TNT",
                                               "Two Continents"))

    def test_a_different_map_is_a_mismatch(self):
        self.assertFalse(tacli._scn_map_matches("Comet Catcher.tnt", "Two Continents"))

    def test_a_name_that_merely_starts_the_same_is_a_mismatch(self):
        self.assertFalse(tacli._scn_map_matches("Two Continents 2.tnt",
                                                "Two Continents"))


class PeekValues(unittest.TestCase):
    """The peek log line is the only channel; parse it, do not re-derive it."""

    def test_a_dword_reads_as_its_decimal_half(self):
        self.assertEqual(tacli._peek_uint({"value": "6947272 (0x0069FBC8)"}), 6947272)

    def test_a_string_reads_as_its_quoted_text(self):
        self.assertEqual(tacli._peek_str({"value": '"maps\\Two Continents.tnt"'}),
                         "maps\\Two Continents.tnt")

    def test_an_unreadable_address_is_not_a_number(self):
        self.assertIsNone(tacli._peek_uint({"value": "<unreadable>"}))
        self.assertIsNone(tacli._peek_str({"value": "<unreadable pointer>"}))

    def test_a_missing_row_is_not_a_crash(self):
        self.assertIsNone(tacli._peek_uint(None))
        self.assertIsNone(tacli._peek_str(None))


class LiveSignal(unittest.TestCase):
    """A world in memory, told from the overlay's own count."""

    def test_a_non_zero_count_is_a_live_game(self):
        m = tacli.SCN_LIVE_RX.search("units: alive=2 onscreen=1 eye=(0,7312) me=0")
        self.assertEqual(int(m.group(1)), 2)

    def test_zero_units_is_not(self):
        # Measured: this line appears at the menu, where the struct is readable
        # and the world is not there yet. Waiting on it would apply into nothing.
        self.assertIsNone(
            tacli.SCN_LIVE_RX.search("units: alive=0 onscreen=0 eye=(0,0) me=0"))


class TotalaIni(unittest.TestCase):
    """TA reads UnitLimit from its own INI, and clamps it itself to [20, 1500]."""

    def write(self, sound, limit=None):
        with tempfile.TemporaryDirectory() as d:
            inst = types.SimpleNamespace(gamedir=Path(d))
            tacli.write_totala_ini(inst, sound, limit)
            path = Path(d) / "totala.ini"
            # bytes: TA's INI is CRLF and read_text() would translate that away.
            return path.read_bytes().decode("ascii") if path.exists() else None

    def test_silence_and_a_limit_share_one_section(self):
        self.assertEqual(self.write(False, 500),
                         "[Preferences]\r\nNoDirectSound=1\r\n"
                         "UseWindowsSound=0\r\nUnitLimit=500\r\n")

    def test_a_limit_survives_sound_being_turned_on(self):
        self.assertEqual(self.write(True, 500), "[Preferences]\r\nUnitLimit=500\r\n")

    def test_nothing_to_say_leaves_no_file(self):
        self.assertIsNone(self.write(True))


class ScenarioFiles(unittest.TestCase):
    """The scenarios that ship in the repo have to stay compilable."""

    def test_a_bare_name_resolves_into_the_scenarios_directory(self):
        self.assertEqual(tacli._scn_path("200v200"), tacli.SCENARIOS / "200v200.json")

    def test_a_path_is_left_alone(self):
        self.assertEqual(tacli._scn_path("/tmp/x.json"), Path("/tmp/x.json"))
        self.assertEqual(tacli._scn_path("sub/x.json"), Path("sub/x.json"))

    def test_the_shipped_200v200_compiles_to_the_situation_it_describes(self):
        exp = tacli._scn_compile("200v200")
        self.assertEqual(exp["counts"], {"units": 401, "features": 1,
                                         "by_owner": {"0": 201, "1": 200}})
        self.assertEqual(exp["warnings"], [])
        self.assertEqual(exp["camera"]["on"], ["feat", 0, "the_wreck"])

    def test_it_expands_byte_identically_every_time(self):
        a = json.dumps(tacli._scn_compile("200v200"), sort_keys=True)
        b = json.dumps(tacli._scn_compile("200v200"), sort_keys=True)
        self.assertEqual(a, b)

    def test_every_shipped_scenario_validates(self):
        for path in sorted(tacli.SCENARIOS.glob("*.json")):
            with self.subTest(scenario=path.stem):
                tacli._scn_compile(path.stem)


class OrderVerb(unittest.TestCase):
    """The wire an `order` produces, and the refusals it makes before sending one.

    The wire is the contract with the fork: a wrong subject token or a target
    written in screen pixels would be accepted by the parser and issued against
    the wrong thing, silently. Everything here is pure string work — no game.
    """

    def subject(self, **kw):
        args = types.SimpleNamespace(sel=False, unit=None, expect=None)
        args.__dict__.update(kw)
        return tacli._order_subject(args)

    def test_sel_is_its_own_subject(self):
        self.assertEqual(self.subject(sel=True), "sel")

    def test_an_index_becomes_an_at_reference(self):
        self.assertEqual(self.subject(unit=2), "@2")

    def test_the_type_guard_rides_on_the_reference(self):
        self.assertEqual(self.subject(unit=2, expect="ARMCOM"), "@2:ARMCOM")

    def test_it_refuses_two_subjects_or_none(self):
        with refuses(self):
            self.subject(sel=True, unit=2)
        with refuses(self):
            self.subject()

    def test_it_refuses_a_guard_that_guards_nothing(self):
        with refuses(self):
            self.subject(sel=True, expect="ARMCOM")

    def test_it_refuses_a_negative_index(self):
        with refuses(self):
            self.subject(unit=-1)

    def test_order_names_and_their_aliases(self):
        self.assertEqual(tacli._order_cmd("MOVE"), "move")
        self.assertEqual(tacli._order_cmd("guard"), "defend")

    def test_attack_move_is_named_as_the_thing_ta_lacks(self):
        with refuses(self):
            tacli._order_cmd("attack-move")
        with refuses(self):
            tacli._order_cmd("fly")

    def test_targets(self):
        self.assertEqual(tacli._order_target("move", ["pos", "10", "20"], ""),
                         "pos 10 20")
        self.assertEqual(tacli._order_target("attack", ["unit", "251"], ""),
                         "unit @251")
        self.assertEqual(tacli._order_target("attack", ["unit", "251"], "CORCOM"),
                         "unit @251:CORCOM")
        self.assertEqual(tacli._order_target("stop", [], ""), "")

    def test_a_target_that_is_missing_malformed_or_surplus(self):
        for cmd, words in (("move", []), ("move", ["pos", "10"]),
                           ("move", ["pos", "x", "20"]), ("move", ["unit"]),
                           ("move", ["unit", "-1"]), ("move", ["screen", "10", "20"]),
                           ("stop", ["pos", "10", "20"])):
            with self.subTest(cmd=cmd, words=words), refuses(self):
                tacli._order_target(cmd, words, "")

    def test_a_live_feature_is_refused_with_the_way_to_say_it(self):
        with refuses(self):
            tacli._order_target("reclaim", ["feat", "87"], "")

    def test_a_type_guard_cannot_smuggle_a_second_wire_line(self):
        """The wire is line-delimited, so a newline in --expect would split it."""
        for bad in ("ARM\nCOM", "ARM COM", "ARM:COM", "", "A" * 32, "ARM\tCOM"):
            with self.subTest(bad=bad), refuses(self):
                tacli._order_type(bad, "--expect")
        self.assertEqual(tacli._order_type("ARMCOM", "--expect"), "ARMCOM")
        self.assertEqual(tacli._order_type("CORE_2b", "--expect"), "CORE_2b")

    def test_integers_are_exactly_what_the_fork_accepts(self):
        for good in ("0", "7", "-40", "1988"):
            self.assertTrue(tacli._is_int(good), good)
        for bad in ("+5", " 7 ", "1_0", "0x10", "", "7.0"):
            self.assertFalse(tacli._is_int(bad), bad)

    def test_a_guard_on_a_position_is_refused(self):
        with refuses(self):
            tacli._order_target("move", ["pos", "10", "20"], "ARMCOM")


class WindowTiling(unittest.TestCase):
    """A tile must land inside the screen — GNOME never maps a window that does
    not, so the game runs invisibly and looks like it failed to start."""

    SCREEN = (3840, 2160)

    def test_a_small_window_keeps_the_historical_grid(self):
        for slot, tile in ((0, (0, 0)), (1, (1064, 0)), (2, (2128, 0)), (3, (0, 848))):
            self.assertEqual(tacli.tile_for(slot, (1024, 768), self.SCREEN), tile)

    def test_every_slot_lands_inside_the_screen(self):
        for res in ((1024, 768), (1280, 1024), (1920, 1080)):
            for slot in range(64):
                x, y = tacli.tile_for(slot, res, self.SCREEN)
                with self.subTest(res=res, slot=slot):
                    self.assertGreaterEqual(min(x, y), 0)
                    self.assertLessEqual(x + res[0], self.SCREEN[0])
                    self.assertLessEqual(y + res[1], self.SCREEN[1])

    def test_a_window_taller_than_the_screen_is_pinned_to_the_origin(self):
        self.assertEqual(tacli.tile_for(7, (4000, 3000), self.SCREEN), (0, 0))



class SettingsStore(unittest.TestCase):
    """`ensure_store`: an instance always has a store; an explicit --res replaces
    its `resolution=`, the recorded size fills one in only where there is none,
    and a size the menu picked is kept (and read back by `store_res`)."""

    def _inst(self, tmp):
        return types.SimpleNamespace(gamedir=Path(tmp) / "gamedir")

    def test_a_missing_store_is_created_empty(self):
        with tempfile.TemporaryDirectory() as tmp:
            inst = self._inst(tmp)
            tacli.ensure_store(inst)
            self.assertEqual((inst.gamedir / "impure.cfg").read_text(), "")

    def test_res_replaces_only_the_resolution_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            inst = self._inst(tmp)
            inst.gamedir.mkdir()
            (inst.gamedir / "impure.cfg").write_text("gamma=15\nresolution=native\nshadows=off\n")
            tacli.ensure_store(inst, (1024, 768))
            lines = (inst.gamedir / "impure.cfg").read_text().splitlines()
            self.assertEqual(lines, ["gamma=15", "shadows=off", "resolution=1024x768"])

    def test_the_recorded_size_fills_an_empty_store(self):
        with tempfile.TemporaryDirectory() as tmp:
            inst = self._inst(tmp)
            tacli.ensure_store(inst)                        # what create does
            tacli.ensure_store(inst, None, default_res=(800, 600))
            self.assertEqual((inst.gamedir / "impure.cfg").read_text(), "resolution=800x600\n")

    def test_a_menu_pick_survives_a_relaunch(self):
        with tempfile.TemporaryDirectory() as tmp:
            inst = self._inst(tmp)
            inst.gamedir.mkdir()
            for picked in ("resolution=3840x2160\n", "resolution=native\n"):
                (inst.gamedir / "impure.cfg").write_text("gamma=12\n" + picked)
                tacli.ensure_store(inst, None, default_res=(1024, 768))
                self.assertEqual((inst.gamedir / "impure.cfg").read_text(), "gamma=12\n" + picked)

    def test_store_res_reads_back_a_size_and_not_native(self):
        with tempfile.TemporaryDirectory() as tmp:
            inst = self._inst(tmp)
            self.assertIsNone(tacli.store_res(inst))           # no store at all
            inst.gamedir.mkdir()
            cfg = inst.gamedir / "impure.cfg"
            cfg.write_text("gamma=12\nresolution=2560x1440\n")
            self.assertEqual(tacli.store_res(inst), (2560, 1440))
            cfg.write_text("resolution=native\n")
            self.assertIsNone(tacli.store_res(inst))
            cfg.write_text("resolution=junk\n")
            self.assertIsNone(tacli.store_res(inst))

    def test_the_placement_writes_vsync_on_unless_asked(self):
        with tempfile.TemporaryDirectory() as tmp:
            inst = self._inst(tmp)
            inst.gamedir.mkdir()
            cfg = inst.gamedir / "impure.cfg"
            cfg.write_text("gamma=12\nvsync=off\nmaxfps=60\n")   # a menu's choice; a dead key
            tacli.write_placement(inst, (10, 20))
            self.assertEqual(cfg.read_text().splitlines(),
                             ["gamma=12", "display=window", "window=10,20,0,0", "vsync=on"])
            tacli.write_placement(inst, (10, 20), vsync="off")
            self.assertEqual(cfg.read_text().splitlines()[-1], "vsync=off")
            self.assertEqual(sum(ln.startswith("vsync=") for ln in cfg.read_text().splitlines()), 1)

    def test_vsync_is_a_harness_key(self):
        # a --shipped launch hands it to the player with the rest of the placement
        self.assertIn("vsync", tacli.HARNESS_KEYS)


# ----------------------------------------------------------- remote instances

taremote = tacli.taremote


class PowerShellScript(unittest.TestCase):
    """`ps_script`, the one function that makes a remote script: one complete
    statement a line, a blank line at the end, and a DONE marker per statement."""

    TOKEN = "0123456789ab"

    def refused(self, stmt):
        with self.assertRaises(taremote.PSScriptError):
            taremote.ps_script([stmt], self.TOKEN, 1)

    def test_a_script_is_one_line_a_statement_and_ends_with_a_blank_line(self):
        s = taremote.ps_script(["Write-Output 'a'", "$x = 1; Write-Output $x"], self.TOKEN, 7)
        self.assertTrue(s.endswith("\n\n"))
        lines = s[:-2].split("\n")
        # the marker function, the sequence counter, a guarded line per statement, END
        self.assertEqual(len(lines), 5)
        for ln in lines:
            taremote.ps_check_statement(ln)
        self.assertTrue(lines[0].startswith("function __m($t) {"))
        self.assertEqual(lines[1], "$__seq = 0")
        self.assertTrue(lines[2].startswith("if ($__seq -eq 0) {"))
        self.assertIn(f"$__seq = 1; __m '@@{self.TOKEN} DONE 7 0'", lines[2])
        self.assertTrue(lines[3].startswith("if ($__seq -eq 1) {"))
        self.assertIn(f"$__seq = 2; __m '@@{self.TOKEN} DONE 7 1'", lines[3])
        self.assertEqual(lines[4], f"__m '@@{self.TOKEN} END 7'")

    def test_every_line_resets_the_error_preference_and_reports_the_inner_exception(self):
        line = taremote.ps_script(["Write-Output 'a'"], self.TOKEN, 1).split("\n")[2]
        self.assertIn("$ErrorActionPreference = 'Stop'; try {", line)
        self.assertIn("while ($__e.InnerException) { $__e = $__e.InnerException }", line)

    def test_a_statement_that_would_span_lines_is_refused(self):
        for stmt in ("Write-Output 'a'\nWrite-Output 'b'",    # two lines
                     "Write-Output 'a'\r",                     # a CR
                     "foreach ($i in 1..3) {",                 # an open block
                     "if ($x) { Write-Output (1 + 2 }",        # a bracket that never closes
                     "Write-Output 'unterminated",             # a string that runs on
                     "Get-ChildItem |",                        # a pipe that continues
                     "$a = 1,",                                # a list that continues
                     "Write-Output 1 `",                       # an explicit continuation
                     "$s = @'",                                # a here-string
                     ""):                                      # a blank line ends the script
            with self.subTest(stmt=stmt):
                self.refused(stmt)

    def test_what_tacli_never_needs_is_refused_too(self):
        for stmt in ('Write-Output "a $x"',     # double quotes expand $ and backticks
                     "Write-Output 1 # note",   # a comment (and <# spans lines)
                     "Write-Output 'é'",        # non-ASCII: the console code page
                     "Write-Output )(", "Write-Output (]"):
            with self.subTest(stmt=stmt):
                self.refused(stmt)

    def test_brackets_and_quotes_inside_a_string_do_not_count(self):
        taremote.ps_check_statement("Write-Output '{ ( [ # ` \" |'")
        taremote.ps_check_statement("Write-Output 'it''s'")

    def test_the_marker_token_is_checked(self):
        with self.assertRaises(taremote.PSScriptError):
            taremote.ps_script(["Write-Output 1"], "not hex!", 1)


class PowerShellQuoting(unittest.TestCase):
    """`ps_str`: any string becomes one-line PowerShell whose value is that string."""

    def decode(self, expr):
        if expr.startswith("'"):
            self.assertTrue(expr.endswith("'"))
            return expr[1:-1].replace("''", "'")
        b64 = re.search(r"FromBase64String\('([A-Za-z0-9+/=]*)'\)", expr).group(1)
        return base64.b64decode(b64).decode("utf-8")

    def test_every_string_round_trips_on_one_checked_line(self):
        for s in (r"C:\Program Files (x86)\Total Annihilation",
                  r"D:\Test Folder\it's here",
                  "a $var and `tick` and \"quotes\"",
                  "Zoë's map", "two\nlines", "tab\there", ""):
            with self.subTest(s=s):
                expr = taremote.ps_str(s)
                self.assertEqual(self.decode(expr), s)
                taremote.ps_check_statement(f"Write-Output {expr}")

    def test_printable_ascii_stays_a_literal_that_expands_nothing(self):
        self.assertEqual(taremote.ps_str("a $b"), "'a $b'")
        self.assertEqual(taremote.ps_str("it's"), "'it''s'")

    def test_a_newline_never_reaches_the_script(self):
        self.assertNotIn("\n", taremote.ps_str("a\nb"))


class RemoteFolders(unittest.TestCase):
    """What a remote path can name: the test folder and below, never the player's."""

    SPEC = {"ssh": "tester@example-host", "key": None,
            "player": r"C:\Games\TA Player", "folder": r"D:\tacli\r1"}

    def test_a_test_folder_overlapping_the_players_is_refused(self):
        for folder in (r"C:\Games\TA Player", r"C:\Games\TA Player\test",
                       r"C:\Games", r"c:\games\ta player\"".rstrip('"')):
            with self.subTest(folder=folder), self.assertRaises(ValueError):
                taremote.Remote(dict(self.SPEC, folder=folder))

    def test_a_relative_folder_is_refused(self):
        with self.assertRaises(ValueError):
            taremote.Remote(dict(self.SPEC, folder=r"tacli\r1"))

    def test_the_ssh_login_is_checked(self):
        for ssh in ("example-host", "a b@host", "user@host;rm"):
            with self.subTest(ssh=ssh), self.assertRaises(ValueError):
                taremote.Remote(dict(self.SPEC, ssh=ssh))

    def test_a_remote_path_stays_under_the_test_folder(self):
        root = taremote.Remote(self.SPEC).root
        self.assertEqual((root / "log/tagpu.log").win, r"D:\tacli\r1\log\tagpu.log")
        for bad in ("..", "C:", r"a\b", "x*", ""):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                root / bad

    def test_a_remote_path_is_not_a_local_path(self):
        p = taremote.Remote(self.SPEC).root / "ddraw.dll"
        with self.assertRaises(TypeError):
            open(p)


# A PowerShell string as ps_str writes it: a single-quoted literal, or the base64 form.
PS_EXPR = (r"(?:\(\[Text\.Encoding\]::UTF8\.GetString\(\[Convert\]::FromBase64String\("
           r"'[A-Za-z0-9+/=]*'\)\)\)|'(?:[^']|'')*')")
_PS_EXPR_RX = re.compile(r"\(\[Text\.Encoding\]::UTF8\.GetString\(\[Convert\]::"
                         r"FromBase64String\('([A-Za-z0-9+/=]*)'\)\)\)|'((?:[^']|'')*)'")


def ps_value(expr: str) -> str:
    """The value of one ps_str expression."""
    m = _PS_EXPR_RX.fullmatch(expr.strip())
    assert m, expr
    if m.group(1) is not None:
        return base64.b64decode(m.group(1)).decode("utf-8")
    return m.group(2).replace("''", "'")


def b64(s) -> str:
    return base64.b64encode(s if isinstance(s, (bytes, bytearray)) else s.encode("utf-8")).decode()


def sha(data) -> str:
    return hashlib.sha256(bytes(data)).hexdigest().upper()


class FakePSError(Exception):
    """A statement failing on the fake machine, as PowerShell would see it. `wrapped`:
    a .NET method's exception, which PowerShell hands the catch block wrapped in a
    MethodInvocationException; a `throw` is not wrapped."""

    def __init__(self, kind, msg, wrapped=True):
        super().__init__(msg)
        self.kind, self.msg, self.wrapped = kind, msg, wrapped


class FakePS:
    """PowerShell running a script `ps_script` made, line by line: the `__m` function,
    the `$__seq` guard, and each statement's try/catch -- whose ERR line names the
    exception the catch clause reaches (the innermost only when the clause walks
    InnerException). A line in `skip` is one PowerShell never ran."""

    GUARDED = re.compile(r"^if \(\$__seq -eq (\d+)\) \{ \$ErrorActionPreference = 'Stop'; "
                         r"try \{ (.*); \$__seq = (\d+); __m '(@@[0-9a-f]+) DONE (\d+) (\d+)' \} "
                         r"catch \{ (.*) \} \}$")

    def __init__(self, script, answer, stdout, skip=()):
        self.lines = script.split("\n")
        self.answer, self.stdout = answer, stdout
        self.skip = set(skip)
        self.seq = None
        self.vars = {}

    def run(self):
        for n, line in enumerate(self.lines):
            if n in self.skip or not line.strip():
                continue
            if line == "function __m($t) { Write-Output $t }":
                continue
            if line == "$__seq = 0":
                self.seq = 0
            elif line.startswith("__m '") and line.endswith("'"):
                self.stdout(line[5:-1])
            else:
                m = self.GUARDED.match(line)
                if not m:
                    raise AssertionError(f"not a line ps_script makes: {line[:120]}")
                i, stmt, nxt, tag, b = int(m[1]), m[2], int(m[3]), m[4], m[5]
                assert nxt == i + 1 and int(m[6]) == i, line[:120]
                if self.seq != i:
                    continue
                try:
                    out = self.answer(stmt, self.vars)
                except FakePSError as e:
                    walks = "while ($__e.InnerException) { $__e = $__e.InnerException }" in m[7]
                    inner = walks or not e.wrapped
                    kind = e.kind if inner else "MethodInvocationException"
                    msg = e.msg if inner else f"Exception calling a method: {e.msg}"
                    self.seq = -1
                    self.stdout(f"{tag} ERR {b} {i} {kind} {b64(msg)}")
                else:
                    for o in out:
                        self.stdout(o)
                    self.seq = nxt
                    self.stdout(f"{tag} DONE {b} {i}")


TEST_DLL = b"MZ our build " + b" ".join(taremote.TEST_MODE_MARKS) + b" and the rest"
R_CLOSED = ("registry: test mode -- " + taremote.TEST_MODE_R_CLOSED
            + " (jump table 0x49F494, cases 9 and 22 -> 0x49F461)")
R_REFUSED = ("registry: TEST MODE, but the -r switch's jump table at 0x49F494 is not the one "
             "this DLL knows, so the switch could write the real registry: the game is not run")


class FakeWindows(taremote.Session):
    """A remote machine in memory, behind the REAL Session: every batch is made by
    `ps_script`, run by FakePS line by line, and its marker lines are parsed by
    `Session._send` as the SSH ones are. The machine holds files, processes,
    scheduled tasks that start TotalA.exe, and TA's real registry key -- which tacli
    may read (`remote add`'s seed) and which nothing may write: `registry_writes`
    records any statement that would."""

    def __init__(self, folder, player):
        super().__init__("tester@example-host")
        self.folder, self.player = folder, player
        self.files, self.mtime, self.dirs = {}, {}, set()
        self.clock = 638000000000000000
        self.sent = []                          # every statement tacli sent, in order
        self.procs = []                         # (pid, path)
        self.next_pid = 4242
        self.key = {("", "SkirmishMap"): (1, b"the player's map\0"),
                    ("", "Interface Type"): (4, (0).to_bytes(4, "little")),
                    ("Skirmish", "Player1"): (4, (1).to_bytes(4, "little"))}
        self.registry_writes = []
        self.replaced = []                      # files put in place by [IO.File]::Replace
        self.tasks, self.action, self.settings = {}, None, None
        self.task_folders = set()               # the Task Scheduler's folders, lower-case
        self.skip_lines = set()
        self.reparse = set()                    # folders that are junctions
        self.robocopy_fails = False
        self.task_error = None                  # the task starts no game: this LastTaskResult
        self.game_exits = False                 # the game exits at once, code 1
        # the DLL's registry line after the run's header; None: it logs none
        self.dll_says = ("registry: TEST MODE, entered by the -xtacli-test token and the "
                         "tacli-state folder -- TotalA.exe's registry is tacli-state\\registry.txt: "
                         "3 keys, 79 values loaded; hooks: TotalA.exe 9 of 9 registry imports, "
                         "win32.dll 2 of 2")
        # the -r closure's line, which a served run logs next (tagpu_patches.c); None: none
        self.dll_then = R_CLOSED
        self.vanish_after_listing = None        # a log file deleted right after a listing
        self.vanish_during_listing = None       # a log file renamed while a listing opens it
        self.extra_listing = []                 # rows a listing adds, as the remote sends them

    # -- the transport the real Session drives
    def _open(self):
        self.proc = types.SimpleNamespace()

    def _write(self, script):
        FakePS(script, self.answer, self.q.put, self.skip_lines).run()

    def close(self):
        self.proc = None

    def kill(self):
        self.proc = None

    # -- files
    def put(self, path, data: bytes):
        self.clock += 10_000_000
        self.files[path.lower()] = bytearray(data)
        self.mtime[path.lower()] = self.clock

    def get(self, path):
        return self.files.get(path.lower())

    def has_dir(self, p):
        p = p.lower().rstrip("\\")
        return p in self.dirs or any(k.startswith(p + "\\") for k in self.files)

    def under(self, d, exclude_ours=False):
        d = d.lower().rstrip("\\") + "\\"
        keys = [k for k in self.files if k.startswith(d)]
        if exclude_ours:
            keys = [k for k in keys if k != d + "tacli-test-folder.txt"
                    and not k.startswith(d + "tacli-state\\")]
        return keys

    def read_or_fail(self, p):
        data = self.get(p)
        if data is None:
            if not self.has_dir(ntpath.dirname(p)):
                raise FakePSError("DirectoryNotFoundException", f"Could not find a part of the path '{p}'.")
            raise FakePSError("FileNotFoundException", f"Could not find file '{p}'.")
        return data

    def store(self, folder=None) -> "taremote.RegStore":
        return taremote.RegStore.parse(bytes(self.get((folder or self.folder)
                                                      + r"\tacli-state\registry.txt")))

    # -- the statements taremote sends
    def answer(self, st, v):
        self.sent.append(st)
        E = PS_EXPR

        def after(prefix, nth=0):
            found = re.findall(re.escape(prefix) + "(" + E + ")", st)
            assert len(found) > nth, (prefix, st[:160])
            return ps_value(found[nth])

        def thrown():
            return ps_value(re.search(r"throw (" + E + ")", st).group(1))

        if st in taremote.SESSION_INIT:
            return []
        if re.search(r"reg(\.exe)? (add|delete|import)|New-ItemProperty|Set-ItemProperty|"
                     r"Remove-ItemProperty|CreateSubKey|SetValue|DeleteValue|HKCU:", st):
            self.registry_writes.append(st)
        # -- processes and the machine
        if st.startswith("Get-Process TotalA -ErrorAction SilentlyContinue | ForEach-Object"):
            return [f"{pid}|{b64(path)}" for pid, path in self.procs]
        if "Win32_ComputerSystem).UserName" in st or "WindowsIdentity]::GetCurrent().Name" in st:
            return [b64(r"HOST\tester")]
        if "$env:USERPROFILE" in st:
            return [b64(r"D:\Profiles\tester")]
        if st.startswith("Stop-Process -Id "):
            pid = int(re.search(r"-Id (\d+)", st).group(1))
            self.procs = [p for p in self.procs if p[0] != pid]
            for t in self.tasks.values():
                if t.get("pid") == pid:
                    t.update(state="Ready", result=1)
            return []
        if st.startswith("Write-Output ('' + (Get-Process -Id"):
            pid = int(re.search(r"-Id (\d+)", st).group(1))
            task = next(t for t in self.tasks.values() if t.get("pid") == pid)
            return ["Normal" if "-Priority 4" in task["settings"] else "BelowNormal"]
        # -- the registry: read only
        if "[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey(" in st:
            assert "$false" in st and "SetValue" not in st
            rows, root = [], "HKEY_CURRENT_USER\\Software\\Cavedog Entertainment"
            if self.key:
                rows.append("K|" + b64(root))
                subs = sorted({sub for sub, _ in self.key})
                for sub in subs:
                    path = root + "\\Total Annihilation" + ("\\" + sub if sub else "")
                    rows.append("K|" + b64(path))
                    for (s2, name), (kind, data) in self.key.items():
                        if s2 == sub:
                            rows.append(f"V|{b64(path)}|{b64(name)}|{kind}|{b64(data)}")
            return rows
        # -- scheduled tasks
        if st.startswith("$a = New-ScheduledTaskAction"):
            self.action = {"execute": after("-Execute "), "dir": after("-WorkingDirectory "),
                           "args": after("-Argument ") if "-Argument " in st else ""}
            return []
        if st.startswith("$pr = New-ScheduledTaskPrincipal"):
            return []
        if st.startswith("$st = New-ScheduledTaskSettingsSet"):
            self.settings = st
            return []
        if st.startswith("Register-ScheduledTask"):
            self.tasks[after("-TaskName ")] = {"state": "Ready", "action": self.action,
                                              "settings": self.settings, "result": 0x41303,
                                              "path": after("-TaskPath ")}
            self.task_folders.add(after("-TaskPath ").strip("\\").lower())
            return []
        if st.startswith("Start-ScheduledTask"):
            task = self.tasks[after("-TaskName ")]
            if self.task_error is not None:
                task.update(state="Ready", result=self.task_error)
                return []
            exe = task["action"]["execute"]
            pid, self.next_pid = self.next_pid, self.next_pid + 1
            task.update(state="Running", result=taremote.TASK_RUNNING, pid=pid)
            if self.game_exits:
                task.update(state="Ready", result=1)
                return []
            self.procs.append((pid, exe))
            self.put(ntpath.dirname(exe) + r"\log\tagpu.log",
                     f"log: run R{pid} part 1 of tagpu.log, started\n".encode()
                     + b"".join(ln.encode() + b"\n" for ln in (self.dll_says, self.dll_then)
                                if ln is not None))
            return []
        if st.startswith("$t = Get-ScheduledTask") and "Unregister-ScheduledTask" in st:
            self.tasks.pop(after("-TaskName "), None)
            return []
        if st.startswith("$__s = New-Object -ComObject Schedule.Service"):
            folder = after("$_.Name -eq ").lower()
            if folder not in self.task_folders:
                return ["absent"]
            if any(t["path"].strip("\\").lower() == folder for t in self.tasks.values()):
                return ["kept"]
            self.task_folders.discard(folder)
            return ["removed"]
        if st.startswith("$t = Get-ScheduledTask") and "Get-ScheduledTaskInfo" in st:
            task = self.tasks.get(after("-TaskName "))
            return [] if task is None else [f"{task['state']}|{task['result']}"]
        # -- the DLL
        if "GetEncoding(28591).GetString([IO.File]::ReadAllBytes(" in st:
            data = self.get(after("[IO.File]::Exists("))
            marks = [ps_value(m) for m in re.findall(r"\$__t\.Contains\((" + E + r")\)", st)]
            return [str(data is not None and bool(marks)
                        and all(m.encode("latin-1") in bytes(data) for m in marks))]
        # -- the folders (`remote add`, `rm`)
        if st.startswith("$__p = [IO.Path]::GetFullPath("):
            p = ntpath.normpath(after("GetFullPath("))
            for r in self.reparse:
                if (p.lower() + "\\").startswith(r.lower() + "\\"):
                    raise FakePSError("RuntimeException", f"{r} is a junction or a link", wrapped=False)
            v["real"] = p
            return []
        if st.startswith("$__a = $__real;") or st.startswith("$__b = $__real;"):
            v[st[3]] = v["real"]
            return [b64(v["real"])]
        if st.startswith("$__da = $__a.Substring(0, 2)"):
            return []
        if st.startswith("if ($__real -ne "):
            if v["real"].lower() != after("if ($__real -ne ").lower():
                raise FakePSError("RuntimeException", "the test folder now resolves elsewhere",
                                  wrapped=False)
            return []
        if "the player''s folder has no TotalA.exe" in st:
            if self.get(after("[IO.File]::Exists(")) is None:
                raise FakePSError("RuntimeException", "the player's folder has no TotalA.exe",
                                  wrapped=False)
            return []
        if "is itself a tacli test folder" in st:
            return []
        if st.startswith("if (Test-Path -LiteralPath") and "the test folder already exists" in st:
            p = after("Test-Path -LiteralPath ")
            if self.has_dir(p) or self.get(p) is not None:
                raise FakePSError("RuntimeException", "the test folder already exists", wrapped=False)
            return []
        if st.startswith("$m = Get-ChildItem -LiteralPath") and "Measure-Object" in st:
            keys = self.under(after("-LiteralPath "), exclude_ours="Where-Object" in st)
            return [f"{len(keys)} {sum(len(self.files[k]) for k in keys)}"]
        if st.startswith("$d = (Get-Item"):
            return ["999999999999"]
        if st.startswith("[IO.File]::WriteAllText("):
            m = re.fullmatch(r"\[IO\.File\]::WriteAllText\((" + E + "), (" + E + r")\)", st)
            self.put(ps_value(m.group(1)), ps_value(m.group(2)).encode())
            return []
        if "& robocopy.exe" in st:
            if self.robocopy_fails:
                raise FakePSError("RuntimeException", "robocopy exited 8", wrapped=False)
            m = re.search(r"& robocopy\.exe (" + E + ") (" + E + ") /E", st)
            src, dst = ps_value(m.group(1)), ps_value(m.group(2))
            for k in self.under(src):
                self.put(dst + "\\" + k[len(src) + 1:], bytes(self.files[k]))
            return []
        if "[IO.File]::WriteAllLines(" in st and "Get-FileHash" in st:
            dst = after("$n = (")
            lines = [sha(self.files[k]) + " " + k[len(dst) + 1:] for k in self.under(dst, True)]
            self.put(after("WriteAllLines("), "\r\n".join(lines).encode())
            return []
        if " copied') } else { [IO.File]::WriteAllBytes(" in st:
            f = after("[IO.File]::Exists(")
            if self.get(f) is not None:
                self.put(f + ".tacli-original", bytes(self.get(f)))
                return [ntpath.basename(f) + " copied"]
            self.put(f + ".tacli-absent", b"")
            return [ntpath.basename(f) + " absent"]
        if "no tacli marker in the test folder" in st:
            if self.get(after("[IO.File]::Exists(")) is None:
                raise FakePSError("RuntimeException", thrown(), wrapped=False)
            return []
        if st.startswith("$t = [IO.File]::ReadAllText("):
            text = bytes(self.get(after("ReadAllText("))).decode()
            if not text.startswith(after("$t.StartsWith(")):
                raise FakePSError("RuntimeException", "the marker names another instance",
                                  wrapped=False)
            return []
        if st.startswith("$__st = New-Object Collections.Stack"):
            folder = after("$__st.Push(").lower()
            for r in self.reparse:
                if r.lower().startswith(folder + "\\"):
                    raise FakePSError("RuntimeException", f"{r} is a junction or link inside the "
                                      f"test folder", wrapped=False)
            return []
        if st.startswith("[IO.Directory]::Delete("):
            folder = after("[IO.Directory]::Delete(").lower()
            for k in self.under(folder):
                self.files.pop(k)
            self.dirs = {d for d in self.dirs if not (d + "\\").startswith(folder + "\\")}
            return []
        # -- files
        if st.startswith("Write-Output ([IO.File]::Exists("):
            p = after("[IO.File]::Exists(")
            return [str(self.get(p) is not None or ("Directory" in st and self.has_dir(p)))]
        if st.startswith("$i = New-Object IO.FileInfo"):
            data = self.get(after("IO.FileInfo "))
            if data is None:
                return ["absent"]
            return [f"{len(data)} {self.mtime[after('IO.FileInfo ').lower()]}"]
        if "$n = [Math]::Min(256, $f.Length); $h = New-Object" in st:
            data = self.read_or_fail(after("[IO.File]::Open("))
            pos, want = map(int, re.search(r"Seek\((\d+), 'Begin'\); \$b = New-Object byte\[\] (\d+)",
                                           st).groups())
            return [b64(bytes(data[:256])), b64(bytes(data[pos:pos + want]))]
        if st.startswith("$f = [IO.File]::Open(") and "[void]$f.Seek(" in st:
            data = self.read_or_fail(after("[IO.File]::Open("))
            pos, want = map(int, re.search(r"Seek\((\d+), 'Begin'\); \$b = New-Object byte\[\] (\d+)",
                                           st).groups())
            return [b64(bytes(data[pos:pos + want]))]
        if "no record of the original beside it" in st:
            p, b = after("[IO.File]::Exists("), after("[IO.File]::Exists(", 1)
            if (self.get(p) is not None and self.get(b) is None
                    and self.get(after("[IO.File]::Exists(", 2)) is None):
                raise FakePSError("RuntimeException", "refusing to replace " + p, wrapped=False)
            return []
        if "$__row = @([IO.File]::ReadAllLines(" in st:
            p, b = after("[IO.File]::Exists("), after("[IO.File]::Exists(", 1)
            listed, rel = after("ReadAllLines("), after("$_.Substring(65) -eq ")
            player = ps_value(re.search(r"elseif \(\[IO\.File\]::Exists\((" + E + ")", st).group(1))
            if self.get(p) is not None and self.get(b) is None and self.get(listed) is not None:
                rows = [ln for ln in bytes(self.get(listed)).decode().splitlines()
                        if len(ln) > 65 and ln[65:].lower() == rel.lower()]
                if rows:
                    want = rows[0][:64]
                    if sha(self.get(p)) == want:
                        src = p
                    elif self.get(player) is not None and sha(self.get(player)) == want:
                        src = player
                    else:
                        raise FakePSError("RuntimeException", f"refusing to replace {p}: neither "
                                          f"it nor the player's copy is still the original",
                                          wrapped=False)
                    self.put(b, bytes(self.get(src)))
            return []
        if st.startswith("[IO.File]::WriteAllBytes("):
            m = re.fullmatch(r"\[IO\.File\]::WriteAllBytes\((" + E + r"), \[Convert\]::"
                             r"FromBase64String\('([A-Za-z0-9+/=]*)'\)\)", st)
            self.put(ps_value(m.group(1)), base64.b64decode(m.group(2)))
            return []
        if st.startswith("$s = [IO.File]::Open(") and "'Append'" in st:
            self.files[after("[IO.File]::Open(").lower()] += base64.b64decode(
                re.search(r"FromBase64String\('([A-Za-z0-9+/=]*)'\)", st).group(1))
            return []
        if st.startswith("Move-Item -LiteralPath"):
            data = self.files.pop(after("-LiteralPath ").lower())
            self.put(after("-Destination "), bytes(data))
            return []
        if "[IO.File]::Replace(" in st:
            dst, tmp = after("[IO.File]::Exists("), after("[IO.File]::Replace(")
            old = dst + ".tacli-old"
            if self.get(dst) is None and self.get(old) is not None:
                self.put(dst, bytes(self.files.pop(old.lower())))   # a half-failed replace
            else:
                self.files.pop(old.lower(), None)
            if self.get(dst) is not None:
                self.replaced.append(dst)       # the name was never free
            self.put(dst, bytes(self.files.pop(tmp.lower())))
            return []
        if "'busy'" in st:
            p, tmp = after("[IO.File]::Exists("), after("[IO.File]::Delete(")
            if self.get(p) is not None:
                self.files.pop(tmp.lower(), None)
                return ["busy"]
            self.put(p, bytes(self.files.pop(tmp.lower())))
            return ["placed"]
        if "'gone'" in st:
            if self.files.pop(after("[IO.File]::Exists(").lower(), None) is None:
                return ["absent"]
            return ["gone"]
        if st.startswith("[void][IO.Directory]::CreateDirectory("):
            self.dirs.add(after("CreateDirectory(").lower().rstrip("\\"))
            return []
        if "-Filter *.log -File" in st:
            logdir = after("[IO.Directory]::Exists(").lower() + "\\"
            if self.vanish_during_listing:
                gone, self.vanish_during_listing = self.vanish_during_listing, None
                self.files.pop(gone.lower(), None)
                raise FakePSError("FileNotFoundException", f"Could not find file '{gone}'.")
            rows = [f"{b64(k[len(logdir):])}|{len(v_)}|{b64(bytes(v_[:256]))}"
                    for k, v_ in self.files.items()
                    if k.startswith(logdir) and k.endswith(".log") and "\\" not in k[len(logdir):]]
            if self.vanish_after_listing:
                self.files.pop(self.vanish_after_listing.lower(), None)
                self.vanish_after_listing = None
            return rows + self.extra_listing
        if "Get-ChildItem -LiteralPath" in st and "-Filter" in st:
            d = after("-LiteralPath ").lower() + "\\"
            pat = re.compile(fnmatch.translate(after("-Filter ").lower()))
            return [b64(k[len(d):]) for k in self.files
                    if k.startswith(d) and "\\" not in k[len(d):] and pat.match(k[len(d):])]
        if "Get-FileHash" in st and "MD5" in st:
            return [hashlib.md5(bytes(self.get(after("-LiteralPath ")))).hexdigest().upper()]
        raise AssertionError(f"FakeWindows has no answer for: {st[:160]}")


class FakeClock:
    """time.time and time.sleep for a test: sleeping moves the clock and is recorded."""

    def __init__(self):
        self.t = 1_000_000.0
        self.slept = []

    def time(self):
        return self.t

    def sleep(self, s):
        self.slept.append(round(s, 3))
        self.t += max(s, 0.0)


class FakeCapturePass(FakeWindows):
    """The fake machine plus the gui pass's A/B latch, kept the way the DLL keeps it:
    a capture latches, and the latch clears only once the pass's own poll has seen
    the lever gone -- modelled as the lever having been gone for POLL seconds."""

    POLL = 1.0

    def __init__(self, folder, player, now):
        super().__init__(folder, player)
        self.now = now
        self.latched = False
        self.gone_at = None
        self.captures = 0

    def answer(self, st, v):
        lever = (self.folder + r"\tagpu_gui.ab").lower()
        before = lever in self.files
        out = super().answer(st, v)
        after = lever in self.files
        if before and not after:
            self.gone_at = self.now()
        elif after and not before:
            if self.latched and (self.gone_at is None or self.now() - self.gone_at < self.POLL):
                return out                  # no poll has seen it gone: still latched
            self.latched = True
            self.captures += 1
            self.put(self.folder + r"\tagpu_gui_vk.ppm", b"P6\n4 2\n255\n" + bytes(24))
            self.files[(self.folder + r"\log\tagpu.log").lower()] += \
                b"vk: shot: wrote tagpu_gui_vk.ppm, 4x2\n"
        return out


class RemoteRouting(unittest.TestCase):
    """A remote instance answers the G21c verbs through the remote machine, and every
    other verb refuses before touching anything."""

    FOLDER = r"D:\Test Folder\r1"
    PLAYER = r"C:\Games\TA Player"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.saved = (tacli.INSTANCES, taremote.SESSION_FACTORY)
        self.addCleanup(self.restore)
        tacli.INSTANCES = Path(self.tmp.name)
        taremote._SESSIONS.clear()
        self.win = FakeWindows(self.FOLDER, self.PLAYER)
        taremote.SESSION_FACTORY = lambda ssh, key: self.win
        self.write_meta("r1", self.FOLDER)
        self.fill_test_folder(self.FOLDER)
        self.before = dict(self.win.key)

    def fill_test_folder(self, folder):
        """What `remote add` leaves: the copied files, their hash list, the protected
        records. The player's folder holds the same files."""
        copied = {"ddraw.dll": b"x", "impure.cfg": b"x",
                  "tagpu_vk.gpus": b"1 the player's GPU\n",      # the DLL rewrites it at start
                  "ErrorLog.txt": b"the player's crash\n", "TotalA.exe": b"MZ"}
        for name, data in copied.items():
            self.win.put(f"{folder}\\{name}", data)
            self.win.put(f"{self.PLAYER}\\{name}", data)
        for name in ("impure.cfg", "ddraw.dll"):
            self.win.put(f"{folder}\\{name}.tacli-original", b"x")
        self.win.put(f"{folder}\\totala.ini.tacli-absent", b"")
        self.win.put(f"{folder}\\tacli-test-folder.txt",
                     f"instance={ntpath.basename(folder)}\nplayer={self.PLAYER}\n".encode())
        self.win.put(f"{folder}\\tacli-state\\copied.txt", "\r\n".join(
            f"{sha(data)} {name}" for name, data in copied.items()).encode())
        # the registry store `remote add` seeds, and the DLL a launch deployed earlier
        seed = taremote.RegStore()
        seed.set_sz(taremote.STORE_TA, "SkirmishMap", "the player's map")
        seed.set_dword(taremote.STORE_TA + r"\Skirmish", "Player1", 1)
        self.win.put(f"{folder}\\tacli-state\\registry.txt", seed.format())
        self.win.put(f"{folder}\\ddraw.dll", TEST_DLL)

    def restore(self):
        tacli.INSTANCES, taremote.SESSION_FACTORY = self.saved
        taremote._SESSIONS.clear()

    def write_meta(self, name, folder, **extra):
        d = Path(self.tmp.name) / name
        d.mkdir(exist_ok=True)
        meta = {"name": name, "type": "remote",
                "remote": {"ssh": "tester@example-host", "key": None,
                           "player": self.PLAYER, "folder": folder,
                           "task": f"\\tacli\\{name}", "console_user": r"HOST\tester"},
                "gamedir": folder, "shield": True, "defaults": False}
        meta.update(extra)
        (d / "instance.json").write_text(json.dumps(meta))

    def meta_now(self, name="r1"):
        return json.loads((Path(self.tmp.name) / name / "instance.json").read_text())

    def main(self, *argv):
        with contextlib.redirect_stdout(io.StringIO()) as so, \
                contextlib.redirect_stderr(io.StringIO()) as se:
            try:
                tacli.main(list(argv))
                code = 0
            except SystemExit as e:
                code = e.code
        return code, so.getvalue(), se.getvalue()

    def local_files(self, name="r1"):
        return sorted(p.name for p in (Path(self.tmp.name) / name).rglob("*"))

    # -- routing
    def test_every_other_verb_refuses_and_sends_nothing(self):
        for argv in (["create", "r1"], ["click", "r1", "1", "2"], ["wheel", "r1", "1"],
                     ["order", "r1", "stop", "--sel"], ["gui", "r1", "on"],
                     ["peek", "r1", "*0x511DE8"], ["weapons", "r1"], ["shot", "r1"],
                     ["roster", "r1"], ["wait", "r1", "x"], ["units", "r1"],
                     ["features", "r1"], ["maps", "r1"], ["switches", "r1"],
                     ["catalogue", "r1"], ["scenario", "apply", "r1", "x"]):
            with self.subTest(argv=argv):
                code, _, err = self.main(*argv)
                self.assertEqual(code, 1)
                self.assertIn("does not reach a remote instance", err)
                self.assertEqual(self.win.sent, [])

    def test_a_positional_named_like_a_verb_does_not_reroute(self):
        # `order`'s positional order is `cmd`, which once read as the `stop` verb
        self.win.procs = [(4242, self.FOLDER + r"\TotalA.exe")]
        code, _, err = self.main("order", "r1", "stop", "--sel")
        self.assertEqual(code, 1)
        self.assertIn("`order` does not reach", err)
        self.assertEqual(self.win.procs, [(4242, self.FOLDER + r"\TotalA.exe")])

    def test_the_minimum_verbs_route_to_their_remote_forms(self):
        parser = tacli.build_parser()
        for argv, func in ((["launch", "r1"], tacli.cmd_remote_launch),
                           (["stop", "r1"], tacli.cmd_remote_stop),
                           (["rm", "r1"], tacli.cmd_remote_rm),
                           (["arm", "r1", "gui.on"], tacli.cmd_arm),
                           (["keys", "r1", "tab"], tacli.cmd_keys),
                           (["ui", "r1"], tacli.cmd_ui),
                           (["eye", "r1", "1", "2"], tacli.cmd_eye),
                           (["scenario", "load", "r1", "x"], tacli.cmd_scenario_load),
                           (["log", "r1"], tacli.cmd_log),
                           (["ab", "r1", "gui"], tacli.cmd_ab),
                           (["crash", "r1"], tacli.cmd_crash),
                           (["shield", "r1", "off"], tacli.cmd_shield)):
            with self.subTest(argv=argv):
                args = parser.parse_args(argv)
                tacli._route_remote(args)
                self.assertIs(args.func, func)

    def test_a_local_instance_is_not_rerouted(self):
        d = Path(self.tmp.name) / "l1"
        d.mkdir()
        (d / "instance.json").write_text(json.dumps({"name": "l1"}))
        args = tacli.build_parser().parse_args(["launch", "l1"])
        tacli._route_remote(args)
        self.assertIs(args.func, tacli.cmd_launch)

    def test_unusable_remote_metadata_is_skipped_by_the_loops_over_every_instance(self):
        d = Path(self.tmp.name) / "broken"
        d.mkdir()
        (d / "instance.json").write_text(json.dumps({"name": "broken", "type": "remote",
                                                     "remote": {"ssh": "no host"}}))
        code, out, err = self.main("ls")
        self.assertEqual(code, 0)
        self.assertIn("r1", out)
        self.assertIn("broken: its remote metadata is unusable", err)
        self.assertEqual(self.main("eye", "broken", "1", "2")[0], 1)

    def test_metadata_that_does_not_read_is_refused_by_every_verb(self):
        # read as {} it would look local, and `rm` would delete the directory
        d = Path(self.tmp.name) / "torn"
        d.mkdir()
        (d / "instance.json").write_text('{"name": "torn", "type": "rem')
        for argv in (["rm", "torn"], ["launch", "torn"], ["stop", "torn"],
                     ["eye", "torn", "1", "2"], ["arm", "torn", "gui.on"]):
            with self.subTest(argv=argv):
                code, _, err = self.main(*argv)
                self.assertEqual(code, 1)
                self.assertIn("does not read", err)
        self.assertTrue((d / "instance.json").exists())
        code, out, err = self.main("ls")
        self.assertEqual(code, 0)
        self.assertIn("r1", out)
        self.assertIn("torn: its metadata", err)
        self.assertEqual(self.win.sent, [])

    def test_metadata_is_replaced_whole(self):
        inst = tacli.Instance("r1")
        before = inst.meta_path.read_text()
        with self.assertRaises(TypeError):
            inst.save_meta({"name": "r1", "bad": object()})     # fails while writing
        self.assertEqual(inst.meta_path.read_text(), before)
        self.assertEqual(sorted(p.name for p in inst.dir.iterdir()), ["instance.json"])
        inst.save_meta(dict(json.loads(before), shield=False))
        self.assertFalse(self.meta_now()["shield"])
        self.assertEqual(sorted(p.name for p in inst.dir.iterdir()), ["instance.json"])

    def test_metadata_saved_by_two_commands_at_once_stays_whole(self):
        # a temporary name shared by both writers let one truncate what the other was
        # moving; each writer's own name leaves one whole file or the other
        import threading
        inst = tacli.Instance("r1")
        bad = []

        def saver(tag):
            for i in range(60):
                try:
                    inst.save_meta({"name": "r1", "tag": tag, "i": i, "pad": tag * 20000})
                    json.loads(inst.meta_path.read_text())
                except (OSError, ValueError) as e:
                    bad.append(e)

        threads = [threading.Thread(target=saver, args=(t,)) for t in "abcd"]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(bad, [])
        self.assertIn(json.loads(inst.meta_path.read_text())["tag"], "abcd")
        self.assertEqual(sorted(p.name for p in inst.dir.iterdir()), ["instance.json"])

    # -- the file channels
    def test_arm_and_disarm_write_the_lever_in_the_test_folder(self):
        self.assertEqual(self.main("arm", "r1", "native.on=all wrecks")[0], 0)
        self.assertEqual(bytes(self.win.get(self.FOLDER + r"\tagpu_native.on")), b"all wrecks\n")
        self.assertEqual(self.main("arm", "r1", "native.on=off")[0], 0)
        self.assertIsNone(self.win.get(self.FOLDER + r"\tagpu_native.on"))
        self.assertEqual(self.local_files(), ["instance.json"])

    def test_eye_and_shield(self):
        self.main("eye", "r1", "100", "200")
        self.assertEqual(bytes(self.win.get(self.FOLDER + r"\tagpu_eye.txt")), b"100 200\n")
        self.main("eye", "r1", "--release")
        self.assertIsNone(self.win.get(self.FOLDER + r"\tagpu_eye.txt"))
        self.win.put(self.FOLDER + r"\tagpu_shield.on", b"")
        self.main("shield", "r1", "off")
        self.assertIsNone(self.win.get(self.FOLDER + r"\tagpu_shield.on"))
        self.assertFalse(self.meta_now()["shield"])

    def test_keys_wait_for_the_last_batch_and_never_append(self):
        self.main("keys", "r1", "tab", "tab")
        self.assertEqual(bytes(self.win.get(self.FOLDER + r"\tagpu_keys.txt")), b"tab tab\n")
        self.assertFalse(any("AppendAllText" in s for s in self.win.sent))

    def test_crash_reads_the_test_folders_error_log(self):
        self.win.put(self.FOLDER + r"\ErrorLog.txt",
                     (f"{self.FOLDER}\\TotalA.exe\nmodule TotalA.exe at 0001:00012345\n").encode())
        code, out, _ = self.main("crash", "r1")
        self.assertEqual(code, 0)
        self.assertIn("r1 CRASHED in TotalA.exe", out)

    def test_a_non_ascii_process_path_still_matches_the_test_folder(self):
        folder = "D:\\Test Folder\\Zoë"
        self.write_meta("z1", folder)
        self.fill_test_folder(folder)
        self.win.procs = [(4242, folder + "\\TotalA.exe")]
        self.assertEqual(tacli.Instance("z1").pid(), 4242)

    # -- the log mirror
    def test_log_reads_the_current_run_through_a_local_mirror(self):
        self.win.put(self.FOLDER + r"\log\tagpu.1.log", b"log: run OLD part 1 of tagpu.log, x\nold\n")
        self.win.put(self.FOLDER + r"\log\tagpu.log", b"log: run NEW part 1 of tagpu.log, x\nhello\n")
        code, out, _ = self.main("log", "r1")
        self.assertEqual(code, 0)
        self.assertIn("hello", out)
        self.assertNotIn("old", out)
        # a second read fetches only what was appended
        self.win.files[(self.FOLDER + r"\log\tagpu.log").lower()] += b"world\n"
        self.win.sent.clear()
        code, out, _ = self.main("log", "r1")
        self.assertIn("world", out)
        reads = [s for s in self.win.sent if "Seek(" in s]
        self.assertEqual(len(reads), 1)
        self.assertIn("Seek(42, 'Begin')", reads[0])

    def test_an_unchanged_log_is_neither_fetched_nor_rewritten(self):
        self.win.put(self.FOLDER + r"\log\tagpu.log", b"log: run NEW part 1 of tagpu.log, x\nhello\n")
        self.main("log", "r1")
        mirror = Path(self.tmp.name) / "r1" / "remote-log" / "log" / "tagpu.log"
        stamp = mirror.stat().st_mtime_ns
        self.win.sent.clear()
        self.main("log", "r1")
        self.assertFalse([s for s in self.win.sent if "Seek(" in s])
        self.assertEqual(mirror.stat().st_mtime_ns, stamp)

    def test_a_log_that_vanishes_after_the_listing_restarts_the_sync(self):
        self.win.put(self.FOLDER + r"\log\tagpu.1.log", b"log: run NEW part 1 of tagpu.log, x\nold part\n")
        self.win.put(self.FOLDER + r"\log\tagpu.log", b"log: run NEW part 2 of tagpu.log, x\nhello\n")
        self.win.vanish_after_listing = self.FOLDER + r"\log\tagpu.1.log"
        code, out, err = self.main("log", "r1")
        self.assertEqual(code, 0, err)
        self.assertIn("hello", out)

    def test_a_log_rotated_inside_the_listing_restarts_the_sync(self):
        self.win.put(self.FOLDER + r"\log\tagpu.log", b"log: run NEW part 1 of tagpu.log, x\nhello\n")
        self.win.vanish_during_listing = self.FOLDER + r"\log\tagpu.1.log"
        code, out, err = self.main("log", "r1")
        self.assertEqual(code, 0, err)
        self.assertIn("hello", out)

    def test_a_listed_name_that_is_not_a_log_file_name_is_ignored(self):
        self.win.put(self.FOLDER + r"\log\tagpu.log", b"log: run NEW part 1 of tagpu.log, x\nhello\n")
        head = b64(b"log: run NEW part 1 of tagpu.log, x\n")
        self.win.extra_listing = [f"{b64('..' + chr(92) + 'escape.log')}|40|{head}",
                                  f"{b64('../escape.log')}|40|{head}"]
        code, _, err = self.main("log", "r1")
        self.assertEqual(code, 0, err)
        self.assertEqual(sorted(p.name for p in (Path(self.tmp.name) / "r1").rglob("*.log")),
                         ["tagpu.log"])

    # -- the .ab capture
    def capture_machine(self):
        """Swap in the machine with a gui pass on it, a running game and a fake clock."""
        clock = FakeClock()
        for name in ("time", "sleep"):
            self.addCleanup(setattr, tacli.time, name, getattr(tacli.time, name))
            setattr(tacli.time, name, getattr(clock, name))
        win = FakeCapturePass(self.FOLDER, self.PLAYER, clock.time)
        win.files, win.mtime = self.win.files, self.win.mtime
        win.procs = [(4242, self.FOLDER + r"\TotalA.exe")]
        win.put(self.FOLDER + r"\log\tagpu.log", b"log: run NEW part 1 of tagpu.log, x\n")
        self.win = win
        taremote.SESSION_FACTORY = lambda ssh, key: win
        taremote._SESSIONS.clear()
        return win, clock

    def test_ab_fetches_the_capture_and_leaves_no_lever(self):
        win, clock = self.capture_machine()
        code, out, err = self.main("ab", "r1", "gui")
        self.assertEqual(code, 0, err)
        self.assertIn("gui: 4x2 ->", out)
        fetched = Path(self.tmp.name) / "r1" / "ab" / "tagpu_gui_vk.ppm"
        self.assertEqual(fetched.read_bytes(), bytes(win.get(self.FOLDER + r"\tagpu_gui_vk.ppm")))
        self.assertIsNone(win.get(self.FOLDER + r"\tagpu_gui.ab"))
        self.assertEqual(clock.slept, [])       # nothing to settle on a first capture

    def test_a_second_ab_waits_for_the_pass_to_see_the_lever_gone(self):
        # The pass re-arms only once its own poll has seen the lever absent; an `ab`
        # straight after another would otherwise re-create it inside that window,
        # find the latch still set, and time out.
        win, clock = self.capture_machine()
        self.assertEqual(self.main("ab", "r1", "gui")[0], 0)
        code, _, err = self.main("ab", "r1", "gui")
        self.assertEqual(code, 0, err)
        self.assertEqual(win.captures, 2)
        self.assertEqual(clock.slept, [2.0])

    def test_ab_clears_a_leftover_lever_before_arming(self):
        win, clock = self.capture_machine()
        win.latched = True                      # an earlier arming, never removed
        win.put(self.FOLDER + r"\tagpu_gui.ab", b"")
        code, _, err = self.main("ab", "r1", "gui", "--settle", "1.5")
        self.assertEqual(code, 0, err)
        self.assertEqual(win.captures, 1)
        self.assertEqual(clock.slept, [1.5])

    # -- launch, stop, and TA's registry as a file
    def use_fake_clock(self):
        clock = FakeClock()
        for name in ("time", "sleep"):
            self.addCleanup(setattr, tacli.time, name, getattr(tacli.time, name))
            setattr(tacli.time, name, getattr(clock, name))
        return clock

    def use_built_dll(self, data):
        with tempfile.NamedTemporaryFile(delete=False, suffix=".dll") as f:
            f.write(data)
        self.addCleanup(Path(f.name).unlink)
        self.addCleanup(setattr, tacli, "BUILT_DLL", tacli.BUILT_DLL)
        tacli.BUILT_DLL = Path(f.name)

    def test_launch_refuses_beside_a_game_it_did_not_start(self):
        self.win.procs = [(77, self.PLAYER + r"\TotalA.exe")]
        code, _, err = self.main("launch", "r1", "--keep-dll")
        self.assertEqual(code, 1)
        self.assertIn("may be the player's game", err)
        self.assertFalse(any(s.startswith(("Register-ScheduledTask", "Start-ScheduledTask"))
                             for s in self.win.sent))

    def test_launch_refuses_local_only_flags(self):
        code, _, err = self.main("launch", "r1", "--window", "800x600")
        self.assertEqual(code, 1)
        self.assertIn("--window", err)
        self.assertEqual(self.win.sent, [])

    def test_launch_puts_the_test_values_into_the_store_and_never_the_registry(self):
        code, out, err = self.main("launch", "r1", "--keep-dll", "--map", "Two Continents",
                                   "--player", "2:2:1")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.win.registry_writes, [])
        self.assertEqual(self.win.key, self.before)
        store = self.win.store()
        ta = taremote.STORE_TA
        self.assertEqual(store.get(ta, "SkirmishMap")[1:], (1, b"Two Continents\0"))
        self.assertEqual(store.get(ta, "Interface Type")[1:], (4, (1).to_bytes(4, "little")))
        self.assertEqual(store.get(ta, "musicvol")[1:], (4, bytes(4)))
        self.assertEqual(store.get(ta + r"\Skirmish", "Player2Controller")[1:],
                         (4, (2).to_bytes(4, "little")))
        self.assertEqual(store.get(ta + r"\Skirmish", "Player1")[1:],
                         (4, (1).to_bytes(4, "little")))           # the seed's, kept
        self.assertIn("TA's settings key there is not written", out)
        # the task runs the game itself, from the test folder, at normal priority, with
        # the token that tells the DLL this is a test launch
        task = self.win.tasks["r1"]
        self.assertEqual(task["path"], "\\tacli\\")
        self.assertEqual(task["action"]["execute"], self.FOLDER + r"\TotalA.exe")
        self.assertEqual(task["action"]["dir"], self.FOLDER)
        self.assertEqual(task["action"]["args"], taremote.TEST_TOKEN)
        self.assertIn("-Priority 4", task["settings"])
        self.assertIn("priority Normal", out)
        # the shield is on by default, as it is locally
        self.assertIsNotNone(self.win.get(self.FOLDER + r"\tagpu_shield.on"))
        self.assertIsNotNone(self.win.get(self.FOLDER + r"\tagpu_nowarp.on"))
        self.assertIsNotNone(self.win.get(self.FOLDER + r"\tagpu_defaults.off"))

    def test_a_launch_that_changes_no_value_leaves_the_store_file_alone(self):
        self.assertEqual(self.main("launch", "r1", "--keep-dll")[0], 0)
        self.main("stop", "r1")
        stamp = self.win.mtime[(self.FOLDER + r"\tacli-state\registry.txt").lower()]
        self.assertEqual(self.main("launch", "r1", "--keep-dll")[0], 0)
        self.assertEqual(self.win.mtime[(self.FOLDER + r"\tacli-state\registry.txt").lower()], stamp)

    def test_launch_refuses_a_test_folder_without_a_store(self):
        self.win.files.pop((self.FOLDER + r"\tacli-state\registry.txt").lower())
        self.use_built_dll(TEST_DLL + b" v2")
        code, _, err = self.main("launch", "r1")
        self.assertEqual(code, 1)
        self.assertIn("would not run the game", err)
        self.assertEqual(self.win.tasks, {})
        # read first: nothing was written, the DLL not deployed
        self.assertEqual(bytes(self.win.get(self.FOLDER + r"\ddraw.dll")), TEST_DLL)
        self.assertFalse(any("WriteAllBytes(" in st for st in self.win.sent))

    def test_the_store_is_put_in_place_without_ever_being_missing(self):
        # Move-Item -Force deletes the target and then moves: a launch in between would
        # find no store. Replace swaps the file in; the name never goes free.
        self.assertEqual(self.main("launch", "r1", "--keep-dll", "--map", "Two Continents")[0], 0)
        self.assertIn(self.FOLDER + r"\tacli-state\registry.txt", self.win.replaced)
        self.assertFalse(any(st.startswith("Move-Item") and "registry.txt" in st
                             for st in self.win.sent))
        self.assertIsNone(self.win.get(self.FOLDER + r"\tacli-state\registry.txt.tacli-old"))
        self.assertIsNone(self.win.get(self.FOLDER + r"\tacli-state\registry.txt.tacli-tmp"))

    def test_launch_fails_when_the_dll_refuses_its_run(self):
        # the refused run wrote its header, and its dying process can still be seen
        self.win.dll_says = ("registry: TEST MODE, entered by the -xtacli-test token, but "
                             "tacli-state\\registry.txt did not load whole (the line above "
                             "says why): the game is not run")
        code, out, err = self.main("launch", "r1", "--keep-dll")
        self.assertEqual(code, 1)
        self.assertIn("the DLL did not run the game: " + self.win.dll_says, err)
        self.assertNotIn("pid=", out)

    def test_launch_fails_when_the_run_logs_no_registry_line(self):
        clock = self.use_fake_clock()
        self.win.dll_says = self.win.dll_then = None
        code, _, err = self.main("launch", "r1", "--keep-dll", "--timeout", "5")
        self.assertEqual(code, 1)
        self.assertIn("logged no registry line in 5s", err)
        self.assertTrue(clock.slept)

    def test_launch_fails_when_the_run_serves_the_store_but_says_nothing_of_minus_r(self):
        self.use_fake_clock()
        self.win.dll_then = None
        code, _, err = self.main("launch", "r1", "--keep-dll", "--timeout", "5")
        self.assertEqual(code, 1)
        self.assertIn("said the store is served but not that the -r switch is closed in 5s", err)

    def test_launch_fails_when_the_minus_r_closure_refuses_after_the_store_is_served(self):
        self.win.dll_then = R_REFUSED
        code, _, err = self.main("launch", "r1", "--keep-dll")
        self.assertEqual(code, 1)
        self.assertIn("the DLL did not run the game: " + self.win.dll_says + " | " + R_REFUSED, err)

    def test_launch_fails_on_a_run_that_is_not_in_test_mode(self):
        self.win.dll_says = ("registry: real (no -xtacli-test token, and no tacli-state "
                             "folder beside TotalA.exe)")
        code, _, err = self.main("launch", "r1", "--keep-dll")
        self.assertEqual(code, 1)
        self.assertIn("did not enter test mode", err)

    def test_the_store_is_read_after_the_running_game_check(self):
        # a game still running (or exiting) owns the store: nothing reads it before then
        self.win.files.pop((self.FOLDER + r"\tacli-state\registry.txt").lower())
        self.win.procs = [(77, self.PLAYER + r"\TotalA.exe")]
        code, _, err = self.main("launch", "r1", "--keep-dll")
        self.assertEqual(code, 1)
        self.assertIn("may be the player's game", err)
        self.win.procs = [(78, self.FOLDER + r"\TotalA.exe")]
        code, out, err = self.main("launch", "r1", "--keep-dll")
        self.assertEqual(code, 0, err)
        self.assertFalse(any("registry.txt" in st for st in self.win.sent))

    def test_a_half_failed_replace_is_named_and_healed(self):
        # ReplaceFile's error 1177 leaves the target renamed to .tacli-old
        store = self.FOLDER + r"\tacli-state\registry.txt"
        self.win.put(store + ".tacli-old", bytes(self.win.files.pop(store.lower())))
        code, _, err = self.main("launch", "r1", "--keep-dll")
        self.assertEqual(code, 1)
        self.assertIn("registry.txt.tacli-old", err)
        self.assertEqual(self.win.tasks, {})
        # the next write through _write_stmts puts the old file back first
        root = taremote.Remote(self.meta_now()["remote"])
        (root.root / "tacli-state" / "registry.txt").write_bytes(b"new\r\n")
        self.assertEqual(bytes(self.win.get(store)), b"new\r\n")
        self.assertIn(store, self.win.replaced)
        self.assertIsNone(self.win.get(store + ".tacli-old"))

    def test_launch_refuses_the_r_and_d_switches_by_their_second_character(self):
        # the engine dispatches on the character after the dash, whatever follows it
        for a in ("-r", "/R", "-register", "-r12", "/Reg", "-d", "-df", "/Display",
                  "-dprinton", "-DebugHelper", "-disableimagehlplines"):
            with self.subTest(a=a):
                for argv in (["launch", "r1", "--keep-dll", "--arg=" + a],
                             ["launch", "nosuch", "--arg=" + a]):
                    code, _, err = self.main(*argv)
                    self.assertEqual(code, 1)
                    self.assertIn(f"refusing {a!r}", err)
        self.assertEqual(self.win.tasks, {})
        for argv in (["launch", "r1", "--keep-dll", "--arg=" + taremote.TEST_TOKEN],
                     ["launch", "nosuch", "--arg=" + taremote.TEST_TOKEN]):
            code, _, err = self.main(*argv)
            self.assertEqual(code, 1)
            self.assertIn("marks a tacli test launch", err)

    def test_launch_refuses_a_store_that_does_not_parse(self):
        self.win.put(self.FOLDER + r"\tacli-state\registry.txt", b"HKLM\\Software\\x\r\n")
        code, _, err = self.main("launch", "r1", "--keep-dll")
        self.assertEqual(code, 1)
        self.assertIn("does not parse", err)
        self.assertEqual(bytes(self.win.get(self.FOLDER + r"\tacli-state\registry.txt")),
                         b"HKLM\\Software\\x\r\n")
        self.assertEqual(self.win.tasks, {})

    def test_launch_refuses_a_dll_without_the_store(self):
        # --keep-dll on the player's copy: a shipped DLL would serve the real registry
        self.win.put(self.FOLDER + r"\ddraw.dll", b"MZ a shipped build")
        code, _, err = self.main("launch", "r1", "--keep-dll")
        self.assertEqual(code, 1)
        self.assertIn("is not one that fails closed", err)
        self.assertEqual(self.win.tasks, {})
        # a build of a tree without it is refused before it is uploaded; so is a build with
        # the store but not the fail-closed test mode and the -r closure
        for old in (b"MZ an older build", b"MZ registry: TEST MODE -- an earlier store"):
            with self.subTest(old=old):
                self.use_built_dll(old)
                code, _, err = self.main("launch", "r1")
                self.assertEqual(code, 1)
                self.assertIn("is not a DLL that fails closed", err)
                self.assertEqual(bytes(self.win.get(self.FOLDER + r"\ddraw.dll")),
                                 b"MZ a shipped build")

    def test_launch_deploys_a_dll_with_the_store(self):
        self.use_built_dll(TEST_DLL + b" v2")
        code, out, err = self.main("launch", "r1")
        self.assertEqual(code, 0, err)
        self.assertEqual(bytes(self.win.get(self.FOLDER + r"\ddraw.dll")), TEST_DLL + b" v2")
        self.assertIn("deployed", out)

    def test_a_task_that_starts_no_game_reports_its_result(self):
        self.use_fake_clock()
        self.win.task_error = 0x80070002
        code, _, err = self.main("launch", "r1", "--keep-dll")
        self.assertEqual(code, 1)
        self.assertIn("no TotalA.exe from the test folder is running", err)
        self.assertIn("last result 0x80070002", err)

    def test_a_game_gone_before_it_was_seen_is_reported_by_its_exit_code(self):
        self.use_fake_clock()
        self.win.game_exits = True
        code, _, err = self.main("launch", "r1", "--keep-dll")
        self.assertEqual(code, 1)
        self.assertIn("no TotalA.exe from the test folder is running", err)
        self.assertIn("is Ready, last result 0x00000001", err)

    def test_stop_ends_the_game_and_writes_no_registry(self):
        self.main("launch", "r1", "--keep-dll", "--map", "Two Continents")
        code, out, err = self.main("stop", "r1")
        self.assertEqual(code, 0, err)
        self.assertIn("stopped r1 (pid 4242)", out)
        self.assertEqual(self.win.procs, [])
        self.assertEqual(self.win.registry_writes, [])
        self.assertEqual(self.win.key, self.before)

    def test_launch_waits_through_a_test_folder_with_no_log_yet(self):
        # a folder from before the log sink: reading log\tagpu.log fails inside a .NET
        # method, which PowerShell wraps; the missing file must still read as "none yet"
        self.assertFalse(self.win.has_dir(self.FOLDER + r"\log"))
        code, out, err = self.main("launch", "r1", "--keep-dll")
        self.assertEqual(code, 0, err)
        self.assertIn("pid=4242", out)

    def test_rm_keeps_the_task_folder_while_another_task_is_in_it(self):
        other = r"D:\Test Folder\r2"
        self.write_meta("r2", other)
        self.fill_test_folder(other)
        self.assertEqual(self.main("launch", "r1", "--keep-dll")[0], 0)
        self.main("stop", "r1")
        self.assertEqual(self.main("launch", "r2", "--keep-dll")[0], 0)
        code, out, err = self.main("rm", "r1")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.win.task_folders, {"tacli"})
        self.assertIn("kept: it holds another task", out)
        code, out, err = self.main("rm", "r2", "--force")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.win.task_folders, set())

    # -- the store's format: the DLL's (tagpu_regstore.h), byte for byte
    STORE_TEXT = (b"# a comment\r\n"
                  b"HKCU\\Software\\Cavedog Entertainment\r\n"
                  b"HKCU\\Software\\Cavedog Entertainment\\Total Annihilation\r\n"
                  b"HKCU\\Software\\Cavedog Entertainment\\Total Annihilation\tGamma\tdword\t12\r\n"
                  b"HKCU\\Software\\Cavedog Entertainment\\Total Annihilation\t\tsz\t\r\n"
                  b"HKCU\\Software\\Cavedog Entertainment\\Total Annihilation\tSkirmishMap\tsz\t"
                  b"100%25 Caf%E9%09x\r\n"
                  b"HKCU\\Software\\Cavedog Entertainment\\Total Annihilation\tblob\thex(3)\t00ff10\r\n"
                  b"HKCU\\Software\\Cavedog Entertainment\\Total Annihilation\tnone\thex(0)\t\n"
                  b"HKCU\\Software\\Cavedog Entertainment\\Total Annihilation\\Skirmish\tP%251\t"
                  b"hex(1)\t410042\r\n")

    def test_the_store_reads_every_type_and_escape(self):
        s = taremote.RegStore.parse(self.STORE_TEXT)
        ta = taremote.STORE_TA
        self.assertEqual(s.get(ta, "gamma"), (b"Gamma", 4, (12).to_bytes(4, "little")))
        self.assertEqual(s.get(ta, ""), (b"", 1, b"\0"))
        self.assertEqual(s.get(ta, "SkirmishMap")[2], b"100% Caf\xe9\tx\0")
        self.assertEqual(s.get(ta, "blob")[1:], (3, b"\x00\xff\x10"))
        self.assertEqual(s.get(ta, "none")[1:], (0, b""))
        self.assertEqual(s.get(ta + r"\Skirmish", "P%1")[1:], (1, b"A\0B"))   # an sz with a NUL
        self.assertEqual(len(s.keys()), 3)

    def test_the_store_writes_what_it_reads(self):
        s = taremote.RegStore.parse(self.STORE_TEXT)
        again = taremote.RegStore.parse(s.format())
        self.assertEqual(again.format(), s.format())
        self.assertTrue(s.format().startswith(b"# tacli registry store"))
        self.assertIn(b"\tSkirmishMap\tsz\t100%25 Caf%E9%09x\r\n", s.format())
        self.assertIn(b"\\Skirmish\tP%251\thex(1)\t410042\r\n", s.format())

    def test_the_store_refuses_what_the_dll_refuses(self):
        for bad in (b"HKLM\\Software\\x",                              # outside the root
                    b"HKCU\\Software\\Cavedog Entertainment\\\\x",     # an empty component
                    b"HKCU\\Software\\Cavedog Entertainment\\x\\",
                    b"HKCU\\Software\\Cavedog Entertainment\tn\tdword\t4294967296",
                    b"HKCU\\Software\\Cavedog Entertainment\tn\tdword\t-1",
                    b"HKCU\\Software\\Cavedog Entertainment\tn\tsz\ta%00b",
                    b"HKCU\\Software\\Cavedog Entertainment\tn\tsz\t50%",
                    b"HKCU\\Software\\Cavedog Entertainment\tn\thex(1)\t0",
                    b"HKCU\\Software\\Cavedog Entertainment\tn\tqword\t1",
                    b"HKCU\\Software\\Cavedog Entertainment\tn\tsz",
                    b"HKCU\\Software\\Cavedog Entertainment\tn\tsz\t\xe9"):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):
                    taremote.RegStore.parse(bad + b"\r\n")

    def test_the_store_holds_to_the_dlls_limits(self):
        # the DLL's buffers (tagpu_regstore.c): measured on its rs_unesc, a key path of 511
        # bytes, a name of 1023 and an sz of 65535 load, one byte more does not
        root = taremote.STORE_ROOT.encode("ascii")
        key = lambda n: root + b"\\" + b"k" * (n - len(root) - 1)
        line = lambda k, rest=b"": k + rest + b"\r\n"
        ok = {"key": line(key(511)),
              "name": line(root, b"\t" + b"n" * 1023 + b"\tdword\t1"),
              "sz": line(root, b"\tn\tsz\t" + b"a" * 65535),
              "hex": line(root, b"\tn\thex(3)\t" + b"00" * 65536),
              "values": b"".join(line(root, b"\tv%d\tdword\t1" % i) for i in range(512)),
              "keys": b"".join(line(root + b"\\k%d" % i) for i in range(1023))}
        over = {"key": line(key(512)),
                "name": line(root, b"\t" + b"n" * 1024 + b"\tdword\t1"),
                "sz": line(root, b"\tn\tsz\t" + b"a" * 65536),
                "hex": line(root, b"\tn\thex(3)\t" + b"00" * 65537),
                "values": b"".join(line(root, b"\tv%d\tdword\t1" % i) for i in range(513)),
                "keys": b"".join(line(root + b"\\k%d" % i) for i in range(1024)),
                "file": b"#" * (4 << 20) + b"\r\n"}
        for what, data in ok.items():
            with self.subTest(ok=what):
                store = taremote.RegStore.parse(data)
                self.assertEqual(taremote.RegStore.parse(store.format()).format(), store.format())
        self.assertEqual(len(taremote.RegStore.parse(ok["keys"]).keys()), 1024)
        for what, data in over.items():
            with self.subTest(over=what):
                with self.assertRaises(ValueError):
                    taremote.RegStore.parse(data)
        # the writer refuses to make what the reader refuses
        store = taremote.RegStore()
        for i in range(512):
            store.set_dword(taremote.STORE_ROOT, f"v{i}", i)
        with self.assertRaises(ValueError):
            store.set_dword(taremote.STORE_ROOT, "one more", 1)
        store.set_dword(taremote.STORE_ROOT, "V7", 8)       # an existing name, any case: fine
        with self.assertRaises(ValueError):
            store.set(taremote.STORE_ROOT, "big", 3, bytes(65537))

    # -- writing into the test folder
    def test_a_protected_file_without_its_backup_is_not_replaced(self):
        self.win.files.pop((self.FOLDER + r"\ddraw.dll.tacli-original").lower())
        before = bytes(self.win.get(self.FOLDER + r"\ddraw.dll"))
        with tempfile.NamedTemporaryFile() as f:
            f.write(b"new dll")
            f.flush()
            with self.assertRaises(taremote.RemoteError):
                (tacli.Instance("r1").gamedir / "ddraw.dll").upload(Path(f.name))
        self.assertEqual(bytes(self.win.get(self.FOLDER + r"\ddraw.dll")), before)

    def test_the_first_overwrite_of_a_copied_file_keeps_the_players_original(self):
        gpus = self.FOLDER + r"\tagpu_vk.gpus"
        self.main("arm", "r1", "vk.gpus=0 test")
        self.assertEqual(bytes(self.win.get(gpus + ".tacli-original")), b"1 the player's GPU\n")
        self.main("arm", "r1", "vk.gpus=0 again")            # the backup stays the original
        self.main("arm", "r1", "vk.gpus=off")
        self.assertIsNone(self.win.get(gpus))
        self.assertEqual(bytes(self.win.get(gpus + ".tacli-original")), b"1 the player's GPU\n")

    def test_a_copied_file_the_game_rewrote_is_backed_up_from_the_players_folder(self):
        gpus = self.FOLDER + r"\tagpu_vk.gpus"
        self.win.put(gpus, b"1 what the DLL wrote at start\n")
        self.assertEqual(self.main("arm", "r1", "vk.gpus=0 test")[0], 0)
        self.assertEqual(bytes(self.win.get(gpus + ".tacli-original")), b"1 the player's GPU\n")

    def test_a_copied_file_with_no_original_left_anywhere_is_not_replaced(self):
        gpus = self.FOLDER + r"\tagpu_vk.gpus"
        self.win.put(gpus, b"1 what the DLL wrote at start\n")
        self.win.put(self.PLAYER + r"\tagpu_vk.gpus", b"1 the player's new GPU\n")
        code, _, err = self.main("arm", "r1", "vk.gpus=0 test")
        self.assertEqual(code, 1)
        self.assertIn("neither it nor the player's copy", err)
        self.assertEqual(bytes(self.win.get(gpus)), b"1 what the DLL wrote at start\n")

    def test_a_file_tacli_made_gets_no_backup(self):
        self.main("arm", "r1", "gui.on")
        self.main("arm", "r1", "gui.on=off")
        self.assertIsNone(self.win.get(self.FOLDER + r"\tagpu_gui.on.tacli-original"))

    def test_the_crash_report_rotation_keeps_the_players_copy(self):
        self.main("launch", "r1", "--keep-dll")
        self.assertEqual(bytes(self.win.get(self.FOLDER + r"\ErrorLog.txt.tacli-original")),
                         b"the player's crash\n")
        self.assertEqual(bytes(self.win.get(self.FOLDER + r"\ErrorLog.txt.prev")),
                         b"the player's crash\n")

    def test_an_upload_is_checked_by_its_md5(self):
        with tempfile.NamedTemporaryFile() as f:
            f.write(bytes(range(256)) * 5000)
            f.flush()
            (tacli.Instance("r1").gamedir / "ddraw.dll").upload(Path(f.name))
        self.assertEqual(bytes(self.win.get(self.FOLDER + r"\ddraw.dll")), bytes(range(256)) * 5000)

    # -- remote add and rm
    def test_add_copies_into_a_folder_it_marked_first_and_seeds_the_store(self):
        self.win.put(self.PLAYER + r"\maps\x.tnt", b"map")
        code, out, err = self.main("remote", "add", "a1", "--ssh", "tester@example-host",
                                   "--from", self.PLAYER, "--to", r"D:\Test Folder\a1")
        self.assertEqual(code, 0, err)
        meta = self.meta_now("a1")
        self.assertNotIn("state", meta)
        self.assertEqual(bytes(self.win.get(r"D:\Test Folder\a1\maps\x.tnt")), b"map")
        listed = bytes(self.win.get(r"D:\Test Folder\a1\tacli-state\copied.txt")).decode()
        self.assertIn(f"{sha(b'map')} maps\\x.tnt", listed)
        self.assertNotIn("tacli-test-folder.txt", listed)
        self.assertNotIn("registry.txt", listed)
        self.assertIsNotNone(self.win.get(r"D:\Test Folder\a1\ddraw.dll.tacli-original"))
        # the store holds the player's key, read and not written
        store = self.win.store(r"D:\Test Folder\a1")
        ta = taremote.STORE_TA
        self.assertEqual(store.get(ta, "SkirmishMap")[1:], (1, b"the player's map\0"))
        self.assertEqual(store.get(ta + r"\Skirmish", "Player1")[1:], (4, (1).to_bytes(4, "little")))
        self.assertEqual(meta["registry_seed"], {"keys": 3, "values": 3})
        self.assertEqual(self.win.registry_writes, [])
        self.assertIn("seeded from the player's key", out)

    def test_add_with_no_player_key_seeds_an_empty_store(self):
        self.win.key = {}
        code, _, err = self.main("remote", "add", "a1", "--ssh", "tester@example-host",
                                 "--from", self.PLAYER, "--to", r"D:\Test Folder\a1")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.win.store(r"D:\Test Folder\a1").keys(), [])

    def test_an_add_that_fails_half_way_leaves_what_rm_removes(self):
        self.win.robocopy_fails = True
        code, _, err = self.main("remote", "add", "a1", "--ssh", "tester@example-host",
                                 "--from", self.PLAYER, "--to", r"D:\Test Folder\a1")
        self.assertEqual(code, 1)
        self.assertIn("tacli rm a1", err)
        self.assertEqual(self.meta_now("a1")["state"], "adding")
        self.assertIsNotNone(self.win.get(r"D:\Test Folder\a1\tacli-test-folder.txt"))
        self.assertEqual(self.main("launch", "a1", "--keep-dll")[0], 1)
        code, _, err = self.main("rm", "a1")
        self.assertEqual(code, 0, err)
        self.assertFalse(self.win.has_dir(r"D:\Test Folder\a1"))
        self.assertFalse((Path(self.tmp.name) / "a1").exists())
        self.assertEqual(bytes(self.win.get(self.PLAYER + r"\ddraw.dll")), b"x")

    def test_add_refuses_a_folder_reached_through_a_junction(self):
        self.win.reparse.add(r"D:\Linked")
        code, _, err = self.main("remote", "add", "a1", "--ssh", "tester@example-host",
                                 "--from", self.PLAYER, "--to", r"D:\Linked\a1")
        self.assertEqual(code, 1)
        self.assertIn("junction or a link", err)
        self.assertFalse((Path(self.tmp.name) / "a1").exists())
        self.assertFalse(self.win.has_dir(r"D:\Linked\a1"))

    def test_rm_removes_the_test_folder_and_never_the_players(self):
        self.main("launch", "r1", "--keep-dll")
        code, out, err = self.main("rm", "r1", "--force")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.win.procs, [])
        self.assertFalse(self.win.has_dir(self.FOLDER))
        self.assertNotIn("r1", self.win.tasks)
        self.assertEqual(self.win.task_folders, set())      # its last task: the folder goes too
        self.assertIn("removed, as it held no other task", out)
        self.assertEqual(bytes(self.win.get(self.PLAYER + r"\tagpu_vk.gpus")), b"1 the player's GPU\n")
        self.assertEqual(self.win.registry_writes, [])
        self.assertEqual(self.win.key, self.before)


class RemoteProtocol(unittest.TestCase):
    """`Session._send` reads the markers `ps_script` writes: output, an error with
    its type and its (base64) message, a line PowerShell skipped, and a marker of
    another batch."""

    T = "@@0123456789ab"

    def session(self, lines):
        s = taremote.Session("tester@example-host")
        s.token = "0123456789ab"
        s.proc = types.SimpleNamespace(stdin=io.BytesIO())
        for ln in lines:
            s.q.put(ln)
        return s

    def test_output_between_the_markers_is_returned(self):
        s = self.session(["hello", f"{self.T} DONE 1 0", f"{self.T} END 1"])
        self.assertEqual(s._send(["Write-Output 'hello'"], 5), ["hello"])

    def test_an_error_carries_its_type_and_message(self):
        msg = base64.b64encode("Accès refusé".encode()).decode()
        s = self.session([f"{self.T} ERR 1 0 UnauthorizedAccessException {msg}", f"{self.T} END 1"])
        with self.assertRaises(taremote.RemoteError) as cm:
            s._send(["Remove-Item -LiteralPath 'C:\\x'"], 5)
        self.assertIn("Accès refusé", str(cm.exception))
        self.assertEqual(cm.exception.kind, "UnauthorizedAccessException")

    def test_a_missing_done_is_an_error_not_an_empty_answer(self):
        s = self.session([f"{self.T} DONE 1 0", f"{self.T} END 1"])
        with self.assertRaises(taremote.RemoteError) as cm:
            s._send(["Write-Output 1", "Write-Output 2"], 5)
        self.assertIn("did not run statement 1", str(cm.exception))

    def test_another_batchs_markers_are_not_this_ones(self):
        # a late DONE 0 of batch 0 must not stand for batch 1's statement 0
        s = self.session([f"{self.T} DONE 0 0", f"{self.T} END 0", "x", f"{self.T} END 1"])
        with self.assertRaises(taremote.RemoteError) as cm:
            s._send(["Write-Output 'x'"], 5)
        self.assertIn("did not run statement 0", str(cm.exception))

    def machine(self):
        win = FakeWindows(r"D:\Test Folder\p1", r"C:\Games\TA Player")
        taremote._SESSIONS.clear()
        return win

    def test_a_skipped_line_stops_every_line_after_it(self):
        # PowerShell skips a line that does not parse as one line; the lines after it
        # must not run -- a Remove-Item after a skipped check would run unchecked
        win = self.machine()
        ran = []
        win.answer = lambda st, v: ran.append(st) or []
        win.run(["Write-Output 'warm up'"])
        ran.clear()
        script = taremote.ps_script(["Write-Output 'a'", "Write-Output 'b'", "Write-Output 'c'"],
                                    win.token, win.batch + 1)
        lines = script.split("\n")
        win.skip_lines = {next(i for i, ln in enumerate(lines) if "__seq -eq 1" in ln)}
        with self.assertRaises(taremote.RemoteError) as cm:
            win.run(["Write-Output 'a'", "Write-Output 'b'", "Write-Output 'c'"])
        self.assertIn("did not run statement 1", str(cm.exception))
        self.assertEqual(ran, ["Write-Output 'a'"])

    def test_a_wrapped_dotnet_exception_is_reported_by_its_inner_type(self):
        win = self.machine()
        win.put(r"D:\Test Folder\p1\x", b"")
        root = taremote.Remote({"ssh": "tester@example-host", "player": r"C:\Games\TA Player",
                                "folder": r"D:\Test Folder\p1"}).root
        taremote.SESSION_FACTORY, saved = (lambda ssh, key: win), taremote.SESSION_FACTORY
        try:
            with self.assertRaises(FileNotFoundError):
                (root / "missing.log").read_bytes()
        finally:
            taremote.SESSION_FACTORY = saved
            taremote._SESSIONS.clear()

    def test_a_statement_that_fails_stops_the_lines_after_it(self):
        win = self.machine()
        ran = []

        def answer(st, v):
            ran.append(st)
            if "boom" in st:
                raise FakePSError("IOException", "the file is in use")
            return []
        win.answer = answer
        with self.assertRaises(taremote.RemoteError) as cm:
            win.run(["Write-Output 'boom'", "Remove-Item -LiteralPath 'D:\\x'"])
        self.assertEqual(cm.exception.kind, "IOException")
        self.assertNotIn("Remove-Item -LiteralPath 'D:\\x'", ran)


# ----------------------------------------------------------- a local instance's registry

# TA's key as a wine user.reg holds it, with the forms wine writes: dwords, strings with
# escapes, a binary value continued over lines, an expand string, a default value, a key
# with no value, and keys outside the root (a sibling whose name only starts the same).
HIVE = (
    "WINE REGISTRY Version 2\n"
    ";; All keys relative to REGISTRY\\\\User\\\\S-1-5-21-0-0-0-1000\n\n"
    "#arch=win64\n\n"
    "[Software\\\\Cavedog Entertainment\\\\Total Annihilation] 1790456990\n"
    "#time=1dd4dfb5e0d56de\n"
    "\"CDLISTS\"=hex:00,01,02,03,04,05,06,07,08,09,0a,0b,0c,0d,0e,0f,10,11,12,13,14,\\\n"
    "  15,16\n"
    "\"DisplaymodeWidth\"=dword:00000400\n"
    "\"gamespeed\"=dword:00000014\n"
    "\"Nickname\"=\"C2NET0\"\n"
    "\"Game Name\"=\"a \\\"quoted\\\" \\\\ name\"\n"
    "\"Path\"=str(2):\"%TEMP%\\\\x\"\n"
    "@=\"the default\"\n\n"
    "[Software\\\\Cavedog Entertainment\\\\Total Annihilation\\\\Skirmish] 1790368740\n"
    "\"Player0Controller\"=dword:00000001\n"
    "\"Player1Side\"=dword:00000001\n\n"
    "[Software\\\\Cavedog Entertainment\\\\Empty] 1790368740\n\n"
    "[Software\\\\Cavedog Entertainment Else] 1790368740\n"
    "\"x\"=dword:00000001\n\n"
    "[Software\\\\Wine\\\\X11 Driver] 1790368740\n"
    "\"UseXRandR\"=\"N\"\n")


class FakeGame:
    """`wine TotalA.exe` in a local gamedir: logs the run the DLL at gamedir/ddraw.dll would
    log -- a new run header, then its `registry: ` line -- and runs, or ends at attach.
    Every other command (`cp -al`, git) is started for real."""

    REAL_POPEN = tacli.subprocess.Popen

    def __init__(self, test):
        self.test = test
        self.argv = None
        self.env = None
        self.exited = None           # an exit code: the DLL ended the process at attach
        self.says = "served"         # served | refused | real | None (a DLL with no store)
        self.then = R_CLOSED         # what a served run logs next: the -r closure's line
        self.before = []             # lines the store logs before its decision
        self.runs = 0

    def __call__(self, argv, cwd=None, env=None, **kw):
        if list(argv[:2]) != ["wine", "TotalA.exe"]:
            return self.REAL_POPEN(argv, cwd=cwd, env=env, **kw)
        self.argv, self.env, self.runs = list(argv), env, self.runs + 1
        gamedir = Path(cwd)
        dll = (gamedir / "ddraw.dll").read_bytes()
        lines = []
        if tacli.dll_serves_regstore(dll):
            token = taremote.TEST_TOKEN in argv
            folder = (gamedir / "tacli-state").is_dir()
            store = (gamedir / "tacli-state" / "registry.txt").exists()
            if self.says == "real" or not (token or folder):
                lines = ["registry: real (no -xtacli-test token, and no tacli-state folder "
                         "beside TotalA.exe)"]
            elif self.says == "refused" or not store:
                lines = ["registry: TEST MODE, entered by the -xtacli-test token, but there is "
                         "no tacli-state\\registry.txt beside TotalA.exe: the game is not run"]
            elif not token:              # tagpu_regstore.c: the folder alone is refused
                lines = ["registry: TEST MODE, entered by the tacli-state folder beside "
                         "TotalA.exe, but no -xtacli-test token on the command line: a launch by "
                         "a tacli from before the per-instance store, or by hand -- launch it "
                         "with the current tacli: the game is not run"]
            else:
                lines = ["registry: TEST MODE, entered by the -xtacli-test token and the "
                         "tacli-state folder -- TotalA.exe's registry is tacli-state\\registry.txt: "
                         "3 keys, 12 values loaded; hooks: TotalA.exe 9 of 9 registry imports, "
                         "win32.dll 2 of 2"]
                if self.then:
                    lines.append(self.then)
            lines = self.before + lines
            if lines[-1].endswith(taremote.TEST_MODE_REFUSED):
                self.exited = 1
        log = gamedir / "log"
        log.mkdir(exist_ok=True)
        if (log / "tagpu.log").exists():
            (log / "tagpu.log").rename(log / "tagpu.1.log")
        (log / "tagpu.log").write_bytes(
            f"# log: run R{self.runs} part 1 of tagpu.log, started\n".encode()
            + b"".join(ln.encode() + b"\n" for ln in lines) + b"tagpu: attached\n")
        self.test.running = self.exited is None
        return types.SimpleNamespace(pid=4242, poll=lambda: self.exited)


class LocalRegistry(unittest.TestCase):
    """A local instance's TA key is its own registry store, seeded from the template's
    user.reg, which is read and never written; a DLL without the store runs on the shared
    registry, and the launch says so."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        prefix, gamedir = root / "wineprefix", root / "gamedir"
        prefix.mkdir()
        gamedir.mkdir()
        (prefix / "user.reg").write_text(HIVE)
        (gamedir / "TotalA.exe").write_bytes(b"MZ")
        self.hive = prefix / "user.reg"
        dll = root / "ddraw.dll"
        dll.write_bytes(TEST_DLL)
        self.running = False
        self.game = FakeGame(self)
        self.wine = []
        test = self
        for name, value in (
                ("INSTANCES", root / "instances"), ("TEMPLATE_PREFIX", prefix),
                ("TEMPLATE_GAMEDIR", gamedir), ("BUILT_DLL", dll),
                ("run", lambda cmd, **kw: test.wine.append(list(cmd))),
                ("default_display", lambda: ":99"),
                ("screen_size", lambda display=None: (3840, 2160)),
                ("pointer_pos", lambda display: None),
                ("place_window", lambda inst, display: inst.window()),
                ("weight_dir", lambda: root / "no-weights")):
            self.addCleanup(setattr, tacli, name, getattr(tacli, name))
            setattr(tacli, name, value)
        for name, value in (("pid", lambda inst: 4242 if test.running else None),
                            ("window", lambda inst: (7, 0, 0, 1024, 768) if test.running else None)):
            self.addCleanup(setattr, tacli.Instance, name, getattr(tacli.Instance, name))
            setattr(tacli.Instance, name, value)
        self.addCleanup(setattr, tacli.subprocess, "Popen", tacli.subprocess.Popen)
        tacli.subprocess.Popen = self.game

    def main(self, *argv):
        with contextlib.redirect_stdout(io.StringIO()) as so, \
                contextlib.redirect_stderr(io.StringIO()) as se:
            try:
                tacli.main(list(argv))
                code = 0
            except SystemExit as e:
                code = e.code
        return code, so.getvalue(), se.getvalue()

    def store(self, name="l1"):
        path = tacli.INSTANCES / name / "gamedir" / "tacli-state" / "registry.txt"
        return taremote.RegStore.parse(path.read_bytes())

    def ta_writes(self):
        """Every `wine reg add` of a value under TA's key."""
        return [c for c in self.wine if "Cavedog" in " ".join(c)]

    def use_old_dll(self):
        tacli.BUILT_DLL.write_bytes(b"MZ a build from before the store")

    # -- the seed
    def test_the_hive_reads_every_form_wine_writes(self):
        s = tacli.hive_store(HIVE)
        ta = taremote.STORE_TA
        self.assertEqual([k.decode() for k in s.keys()],
                         [taremote.STORE_ROOT, ta, ta + r"\Skirmish",
                          taremote.STORE_ROOT + r"\Empty"])
        self.assertEqual(s.get(ta, "cdlists")[1:], (3, bytes(range(0x17))))
        self.assertEqual(s.get(ta, "DisplaymodeWidth")[1:], (4, (1024).to_bytes(4, "little")))
        self.assertEqual(s.get(ta, "Game Name")[2], b'a "quoted" \\ name\0')
        self.assertEqual(s.get(ta, "Path")[1:], (2, b"%TEMP%\\x\0"))
        self.assertEqual(s.get(ta, "")[1:], (1, b"the default\0"))
        self.assertEqual(s.get(ta + r"\Skirmish", "Player1Side")[2], (1).to_bytes(4, "little"))
        self.assertEqual(s.values(taremote.STORE_ROOT + r"\Empty"), [])

    def test_the_hive_refuses_what_it_cannot_carry(self):
        head = "WINE REGISTRY Version 2\n\n[Software\\\\Cavedog Entertainment] 1\n"
        tail = "\n[Software\\\\Wine] 1\n"
        self.assertEqual(len(tacli.hive_store(head + '"x"=dword:00000001\n' + tail).keys()), 1)
        for bad in ('"x"=qword:1\n', '"x"="no end\n', '"x"="\\x4e2d"\n', 'junk\n'):
            with self.subTest(bad=bad):
                with self.assertRaisesRegex(ValueError, "line 4"):
                    tacli.hive_store(head + bad + tail)

    def test_a_hive_read_while_wine_rewrites_it_is_refused(self):
        # wine truncates a hive with several links and writes it again front to back:
        # a read sees any prefix of it, and only a key after TA's proves TA's whole
        for cut in (HIVE.index('"Nickname"'), HIVE.index("[Software\\\\Cavedog Entertainment Else]"),
                    0, 10):
            with self.subTest(cut=cut):
                with self.assertRaises(tacli.HiveCut):
                    tacli.hive_store(HIVE[:cut])
        no_ta = HIVE[:HIVE.index("[Software\\\\Cavedog")] + "[Software\\\\Wine] 1\n"
        self.assertEqual(tacli.hive_store(no_ta).keys(), [])         # whole, and no TA key
        self.hive.write_text(HIVE[:HIVE.index('"Nickname"')])
        clock = FakeClock()
        for name in ("time", "sleep"):
            self.addCleanup(setattr, tacli.time, name, getattr(tacli.time, name))
            setattr(tacli.time, name, getattr(clock, name))
        code, _, err = self.main("create", "l1")
        self.assertEqual(code, 1)
        self.assertIn("read cut short five times", err)
        self.assertFalse((tacli.INSTANCES / "l1" / "gamedir" / "tacli-state" / "registry.txt").exists())

    def test_every_cut_of_the_hive_is_refused_as_cut_or_read_whole(self):
        # wine rewrites the hive in place: a read can stop at any byte, mid-line included
        whole = tacli.hive_store(HIVE).format()
        accepted = 0
        for k in range(len(HIVE) + 1):
            with self.subTest(cut=k):
                try:
                    got = tacli.hive_store(HIVE[:k]).format()
                except tacli.HiveCut:
                    continue
                self.assertEqual(got, whole)
                accepted += 1
        self.assertGreater(accepted, 0)
        self.assertEqual(tacli.hive_store(HIVE[:HIVE.index("[Software\\\\Wine")]).format(), whole)

    def test_create_seeds_the_store_and_forces_gamespeed(self):
        before = self.hive.read_bytes()
        code, out, err = self.main("create", "l1")
        self.assertEqual(code, 0, err)
        s = self.store()
        ta = taremote.STORE_TA
        self.assertEqual(s.get(ta, "gamespeed")[2], (10).to_bytes(4, "little"))   # hive: 20
        self.assertEqual(s.get(ta, "Nickname")[2], b"C2NET0\0")
        self.assertEqual(s.get(ta, "CDLISTS")[1], 3)
        self.assertEqual(s.get(ta, "Interface Type")[2], (1).to_bytes(4, "little"))
        self.assertEqual(s.get(ta, "DisplaymodeHeight")[2], (768).to_bytes(4, "little"))
        self.assertIn("registry store created", out)
        self.assertIn(f"seeded from TA's key in {self.hive}", out)
        self.assertIn("gamespeed forced to 10", out)
        self.assertEqual(json.loads((tacli.INSTANCES / "l1" / "instance.json").read_text())
                         ["registry_seed"]["from"], str(self.hive))
        self.assertEqual(self.ta_writes(), [])
        self.assertEqual(self.hive.read_bytes(), before)

    def test_the_first_launch_of_an_instance_without_a_store_seeds_it(self):
        self.assertEqual(self.main("create", "l1")[0], 0)
        (tacli.INSTANCES / "l1" / "gamedir" / "tacli-state" / "registry.txt").unlink()
        self.hive.write_text(HIVE.replace('"Nickname"="C2NET0"', '"Nickname"="LATER"'))
        code, out, err = self.main("launch", "l1", "--no-restore-pointer")
        self.assertEqual(code, 0, err)
        self.assertIn("registry store created", out)
        self.assertEqual(self.store().get(taremote.STORE_TA, "Nickname")[2], b"LATER\0")

    def test_an_existing_store_is_the_instances_and_is_not_reseeded(self):
        self.assertEqual(self.main("create", "l1")[0], 0)
        self.assertEqual(self.main("registry", "l1", "Nickname=MINE")[0], 0)
        code, out, err = self.main("launch", "l1", "--no-restore-pointer")
        self.assertEqual(code, 0, err)
        self.assertNotIn("registry store created", out)
        self.assertEqual(self.store().get(taremote.STORE_TA, "Nickname")[2], b"MINE\0")

    # -- a launch with a DLL that serves the store
    def test_the_launch_values_go_into_the_store_and_never_the_shared_registry(self):
        before = self.hive.read_bytes()
        code, out, err = self.main("launch", "l1", "--res", "800x600", "--map", "Two Continents",
                                   "--player", "0:1:0", "--player", "1:2:1::5000",
                                   "--los", "0", "--mapping", "1", "--no-restore-pointer")
        self.assertEqual(code, 0, err)
        s, ta = self.store(), taremote.STORE_TA
        dword = lambda v: (4, int(v).to_bytes(4, "little"))
        for key, name, want in ((ta, "DisplaymodeWidth", dword(800)),
                                (ta, "DisplaymodeHeight", dword(600)),
                                (ta, "SkirmishMap", (1, b"Two Continents\0")),
                                (ta, "SkirmishLineOfSight", dword(0)),
                                (ta, "SkirmishMapping", dword(1)),
                                (ta, "musicvol", dword(0)), (ta, "PlayMovie", dword(0)),
                                (ta + r"\Skirmish", "Player1Controller", dword(2)),
                                (ta + r"\Skirmish", "Player1Metal", dword(5000)),
                                (ta + r"\Skirmish", "Player0Side", dword(0))):
            with self.subTest(name=name):
                self.assertEqual(s.get(key, name)[1:], want)
        self.assertEqual(self.ta_writes(), [])
        self.assertEqual(self.hive.read_bytes(), before)
        # Wine's own key is still the prefix's
        self.assertIn(["wine", "reg", "add", r"HKCU\Software\Wine\X11 Driver", "/v", "UseXRandR",
                       "/t", "REG_SZ", "/d", "N", "/f"], self.wine)
        # the token first, then --arg's switches; the DLL's own word that it serves the store
        self.assertEqual(self.game.argv[:3], ["wine", "TotalA.exe", taremote.TEST_TOKEN])
        self.assertIn("TA's key: tacli-state/registry.txt, served -- registry: TEST MODE", out)
        self.assertNotIn("SHARED", err)

    def test_the_launch_fails_when_the_dll_refuses_its_run(self):
        self.game.says = "refused"
        code, out, err = self.main("launch", "l1", "--no-restore-pointer")
        self.assertEqual(code, 1)
        self.assertIn("the DLL did not run the game: registry: TEST MODE", err)

    def test_the_launch_fails_on_a_run_that_is_not_in_test_mode(self):
        self.game.says = "real"
        code, out, err = self.main("launch", "l1", "--no-restore-pointer")
        self.assertEqual(code, 1)
        self.assertIn("did not enter test mode", err)
        self.assertIn("the registry every instance shares", err)

    def test_the_launch_fails_when_the_run_logs_no_registry_line(self):
        self.assertEqual(self.main("create", "l1")[0], 0)
        clock = FakeClock()
        for name in ("time", "sleep"):
            self.addCleanup(setattr, tacli.time, name, getattr(tacli.time, name))
            setattr(tacli.time, name, getattr(clock, name))
        self.game.says = None
        # a serving DLL that logs nothing: the fake game writes no registry line for it
        real = self.game.__call__

        def silent(argv, **kw):
            proc = real(argv, **kw)
            if list(argv[:2]) != ["wine", "TotalA.exe"]:
                return proc
            log = Path(kw["cwd"]) / "log" / "tagpu.log"
            log.write_bytes(b"".join(ln for ln in log.read_bytes().splitlines(True)
                                     if not ln.startswith(b"registry: ")))
            return proc
        tacli.subprocess.Popen = silent
        code, out, err = self.main("launch", "l1", "--no-restore-pointer", "--timeout", "5")
        self.assertEqual(code, 1)
        self.assertIn("logged no registry line in 5s", err)

    # -- a DLL without the store: the shared registry, said once
    def test_a_dll_without_the_store_runs_on_the_shared_registry_and_says_so(self):
        self.assertEqual(self.main("create", "l1")[0], 0)
        (tacli.INSTANCES / "l1" / "gamedir" / "ddraw.dll").write_bytes(b"MZ an old build")
        code, out, err = self.main("launch", "l1", "--keep-dll", "--res", "800x600",
                                   "--player", "1:2", "--no-restore-pointer")
        self.assertEqual(code, 0, err)
        warnings = [ln for ln in err.splitlines() if "WARNING" in ln]
        self.assertEqual(len(warnings), 1, err)
        self.assertIn("this run uses the SHARED registry", warnings[0])
        self.assertIn("TA's key: the SHARED registry", out)
        self.assertNotIn(taremote.TEST_TOKEN, self.game.argv)
        writes = {(c[3], c[5]): c[9] for c in self.ta_writes()}
        self.assertEqual(writes[(taremote.STORE_TA, "DisplaymodeWidth")], "800")
        self.assertEqual(writes[(taremote.STORE_TA + r"\Skirmish", "Player1Controller")], "2")
        self.assertEqual(writes[(taremote.STORE_TA, "Interface Type")], "1")
        # the store holds them as well: it is still the instance's registry
        self.assertEqual(self.store().get(taremote.STORE_TA, "DisplaymodeWidth")[2],
                         (800).to_bytes(4, "little"))

    def test_a_build_of_this_tree_serves_the_store_after_an_old_one(self):
        self.use_old_dll()
        self.assertEqual(self.main("launch", "l1", "--no-restore-pointer")[0], 0)
        self.assertNotEqual(self.ta_writes(), [])
        self.running, self.wine = False, []
        tacli.BUILT_DLL.write_bytes(TEST_DLL)
        code, out, err = self.main("launch", "l1", "--no-restore-pointer")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.ta_writes(), [])
        self.assertIn(taremote.TEST_TOKEN, self.game.argv)

    def test_the_seed_never_replaces_a_store_another_process_made(self):
        self.assertEqual(self.main("create", "l1")[0], 0)
        inst = tacli.Instance("l1")
        path = tacli.regstore_path(inst)
        mine = path.read_bytes()
        self.assertFalse(tacli._regstore_write(inst, b"# another\r\n", create=True))
        self.assertEqual(path.read_bytes(), mine)
        self.assertEqual(sorted(p.name for p in path.parent.iterdir()),
                         ["registry.txt", "tacli.lock"])      # no temporary left behind

    def test_create_force_keeps_the_store_and_its_record(self):
        self.assertEqual(self.main("create", "l1")[0], 0)
        seed = json.loads((tacli.INSTANCES / "l1" / "instance.json").read_text())["registry_seed"]
        self.assertEqual(self.main("registry", "l1", "Nickname=MINE")[0], 0)
        code, out, err = self.main("create", "l1", "--force")
        self.assertEqual(code, 0, err)
        self.assertNotIn("registry store created", out)
        self.assertEqual(json.loads((tacli.INSTANCES / "l1" / "instance.json").read_text())
                         ["registry_seed"], seed)
        self.assertEqual(self.store().get(taremote.STORE_TA, "Nickname")[2], b"MINE\0")

    def test_a_write_waits_for_another_tacli_holding_the_store(self):
        import threading
        self.assertEqual(self.main("create", "l1")[0], 0)
        held = tacli.regstore_lock(tacli.Instance("l1"))
        done = []
        t = threading.Thread(target=lambda: done.append(self.main("registry", "l1", "gamespeed=5")))
        t.start()
        t.join(0.5)
        self.assertTrue(t.is_alive())
        self.assertEqual(self.store().get(taremote.STORE_TA, "gamespeed")[2], (10).to_bytes(4, "little"))
        held.close()
        t.join(5)
        self.assertEqual(done[0][0], 0, done)
        self.assertEqual(self.store().get(taremote.STORE_TA, "gamespeed")[2], (5).to_bytes(4, "little"))

    def test_the_launch_holds_the_store_until_the_dll_has_it(self):
        taken = []
        real = self.game.__call__

        def game(argv, **kw):
            if list(argv[:2]) == ["wine", "TotalA.exe"]:
                # at the game's start another tacli cannot take the lock
                path = Path(kw["cwd"]) / "tacli-state" / "tacli.lock"
                with open(path, "a") as f:
                    try:
                        tacli.fcntl.flock(f, tacli.fcntl.LOCK_EX | tacli.fcntl.LOCK_NB)
                        taken.append(True)
                    except BlockingIOError:
                        taken.append(False)
            return real(argv, **kw)
        tacli.subprocess.Popen = game
        code, _, err = self.main("launch", "l1", "--no-restore-pointer")
        self.assertEqual(code, 0, err)
        self.assertEqual(taken, [False])
        with open(tacli.INSTANCES / "l1" / "gamedir" / "tacli-state" / "tacli.lock", "a") as f:
            tacli.fcntl.flock(f, tacli.fcntl.LOCK_EX | tacli.fcntl.LOCK_NB)   # released after

    # -- every line of a refused run is reported
    def test_a_refusal_is_reported_with_the_stores_own_reasons(self):
        self.game.says = "refused"
        self.game.before = ["registry: tacli-state\\registry.txt line 3 does not parse",
                            "registry: 1 line(s) of tacli-state\\registry.txt do not parse"]
        code, _, err = self.main("launch", "l1", "--no-restore-pointer")
        self.assertEqual(code, 1)
        self.assertIn("the DLL did not run the game: registry: tacli-state\\registry.txt line 3 "
                      "does not parse | registry: 1 line(s)", err)
        self.assertIn("the game is not run", err)
        self.assertNotIn("before showing a window", err)

    def test_the_minus_r_closures_refusal_after_the_served_line_is_reported(self):
        self.game.then = R_REFUSED
        code, _, err = self.main("launch", "l1", "--no-restore-pointer")
        self.assertEqual(code, 1)
        self.assertIn("the DLL did not run the game: registry: TEST MODE, entered by the "
                      "-xtacli-test token and the tacli-state folder", err)
        self.assertIn(" | " + R_REFUSED, err)
        self.assertNotIn("before showing a window", err)

    # -- the store is the DLL's while the game runs
    def test_nothing_writes_the_store_while_the_game_runs(self):
        self.assertEqual(self.main("launch", "l1", "--no-restore-pointer")[0], 0)
        path = tacli.INSTANCES / "l1" / "gamedir" / "tacli-state" / "registry.txt"
        before = path.read_bytes()
        code, _, err = self.main("registry", "l1", "gamespeed=20")
        self.assertEqual(code, 1)
        self.assertIn("is running", err)
        code, out, _ = self.main("launch", "l1", "--map", "Other")
        self.assertEqual(code, 0)
        self.assertIn("already running", out)
        code, _, err = self.main("create", "l1", "--force")
        self.assertEqual(code, 1)
        self.assertEqual(path.read_bytes(), before)

    # -- the verb that reads and sets it
    def test_registry_reads_and_sets_the_instances_values(self):
        self.assertEqual(self.main("create", "l1")[0], 0)
        code, out, _ = self.main("registry", "l1", "gamespeed", "Skirmish\\Player0Controller",
                                 "nosuch")
        self.assertEqual(code, 0)
        self.assertEqual(out.splitlines(), ["gamespeed = dword 10",
                                            "Skirmish\\Player0Controller = dword 1",
                                            "nosuch = absent"])
        code, out, err = self.main("registry", "l1", "damagebars=1", "Nickname=12",
                                   "gamespeed=0x14", "SkirmishMap=Two Continents")
        self.assertEqual(code, 0, err)
        s, ta = self.store(), taremote.STORE_TA
        self.assertEqual(s.get(ta, "damagebars")[1:], (4, (1).to_bytes(4, "little")))
        self.assertEqual(s.get(ta, "Nickname")[1:], (1, b"12\0"))    # an sz stays an sz
        self.assertEqual(s.get(ta, "gamespeed")[2], (20).to_bytes(4, "little"))
        code, _, err = self.main("registry", "l1", "gamespeed=fast")
        self.assertEqual(code, 1)
        self.assertIn("a dword is", err)
        for zero in ("gamespeed=010", "newvalue=007"):
            with self.subTest(zero=zero):
                code, out, err = self.main("registry", "l1", zero)
                self.assertEqual(code, 1)
                self.assertIn("without a leading zero", err)
                self.assertNotIn("Traceback", err)
        self.assertEqual(self.main("registry", "l1", "Nickname=007")[0], 0)   # an sz stays text
        self.assertEqual(self.store().get(taremote.STORE_TA, "Nickname")[2], b"007\0")
        code, _, err = self.main("registry", "l1", "CDLISTS=1")
        self.assertEqual(code, 1)
        self.assertIn("dword and sz values only", err)
        code, out, _ = self.main("registry", "l1", "--json")
        rows = json.loads(out)["values"]
        self.assertIn({"key": "", "name": "gamespeed", "type": 4, "value": 20,
                       "shown": "dword 20"}, rows)
        self.assertEqual(self.hive.read_text(), HIVE)


class AbRefusals(unittest.TestCase):
    """`tacli ab` stops waiting on the lines tagpu_vk.c logs when an arming captured
    nothing, spelled as the DLL spells them."""

    def test_every_refusal_line_is_recognised(self):
        for line in (
                "vk: 2 A/B levers claimed this frame and 3 passes drew into it - nothing captured. "
                "A Vulkan frame carries every armed pass at once",
                "vk: a world pass claimed the A/B and this frame has no world target - its "
                "capture is the gw*ss world target and the only image here is the window's "
                "client rect, which tools/vk-ab.py refuses as two sizes. Nothing captured; the "
                "line above says why the target stood down.",
                "vk: the A/B asked for a capture and this surface's images do not carry "
                "TRANSFER_SRC - nothing captured",
                "vk: the A/B capture was lost to a resize - no tagpu_<pass>_vk.ppm this arming",
                "vk: the A/B capture was lost to a device loss AND the device would not go idle",
                "vk: ab: tagpu_gui_vk.ppm could not be removed (error 32) - this arming is "
                "REFUSED rather than risk the file",
                "vk: ab: \"xyz\" is not one of the eight ported passes - nothing unlinked and "
                "no claim granted"):
            with self.subTest(line=line[:40]):
                self.assertTrue(tacli.AB_REFUSED_RX.search(line))

    def test_a_capture_line_is_not_a_refusal(self):
        self.assertFalse(tacli.AB_REFUSED_RX.search("vk: shot: wrote tagpu_gui_vk.ppm, 1920x1080"))


if __name__ == "__main__":
    unittest.main()
