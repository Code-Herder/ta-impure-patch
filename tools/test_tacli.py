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
import json
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
                                         "by_owner": {"1": 201, "2": 200}})
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


if __name__ == "__main__":
    unittest.main()
