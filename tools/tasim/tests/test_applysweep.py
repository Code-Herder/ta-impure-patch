"""applysweep.py offline: the plan on synthetic and on the tree's own scenarios, the answers' parsing,
and the levels' plumbing on a fake tareplay that answers like the game.

    .venv-undither/bin/python tools/tasim/tests/test_applysweep.py
"""

import json
import sys
import tempfile
import threading
import types
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import applysweep as S  # noqa: E402

SC = S.SCENARIOS


def doc(map_=None, orders=(), players=(), after=0):
    units = [{"id": f"u{i}", "type": "ARMPW", "orders": [dict(o, after=after) for o in ords]}
             for i, ords in enumerate(orders)]
    setup = {"clear_existing": True}
    if map_:
        setup["map"] = map_
    if players:
        setup["players"] = [{"slot": s, "controller": "human"} for s in players]
    return {"format": "ta-scenario/1", "setup": setup, "units": units}


def P(name):
    return (SC / name).resolve()


ATTACK = {"cmd": "attack", "target": "u1"}


class Plan(unittest.TestCase):
    def setUp(self):
        self.docs = {
            P("host.json"): doc("Show Down"),
            P("wave.json"): doc(None, [[ATTACK], []]),
            P("other.json"): doc(None, [[{"cmd": "move", "to": [1, 2]}]]),
            P("a.json"): doc("Two Continents", [[ATTACK]], players=(0, 1)),
            P("b.json"): doc("two continents", [[ATTACK]], players=(1, 0)),
            P("c.json"): doc("Two Continents", [[ATTACK]], players=(0,)),
            P("calib/arena.json"): doc("Cavedog Links CC"),
            P("calib/duel.json"): doc(None, [[ATTACK]]),
            P("lost.json"): doc(None, [[ATTACK]]),
            P("noorders.json"): doc("Show Down"),
        }
        self.fx = {"fixtures": {"f1": {"load": "host.json",
                                       "steps": [[30, "apply", "wave.json"], [60, "apply", "wave.json"],
                                                 [90, "apply", "other.json"],
                                                 [900, "apply", "noorders.json"]]}},
                   "broad": {}}

    def test_every_ordered_scenario_once(self):
        levels, unhosted = S.plan(self.docs, self.fx)
        reported = [i["path"] for lv in levels for i in lv["items"] if i["report"]]
        self.assertEqual(sorted(reported), sorted(
            p for p, d in self.docs.items() if S.orders_of(d) and p != P("lost.json")))
        self.assertEqual(len(reported), len(set(reported)))
        self.assertEqual(unhosted, {P("lost.json"): {"could_not_run": "no map, and no fixture applies it"}})

    def test_fixture_applies_at_their_ticks_on_the_load(self):
        levels, _ = S.plan(self.docs, self.fx)
        lv = next(lv for lv in levels if lv["items"][0]["path"] == P("host.json"))
        self.assertEqual([(S.rel(i["path"]), i["at"], i["report"]) for i in lv["items"]],
                         [("host.json", None, False), ("wave.json", 30, True),
                          ("wave.json", 60, False), ("other.json", 90, True)])

    def test_same_launch_shares_a_level(self):
        levels, _ = S.plan(self.docs, self.fx)
        tc = [lv for lv in levels if lv["launch"]["map"] == "two continents"]
        self.assertEqual(sorted(len(lv["items"]) for lv in tc), [1, 2])     # c seats one player
        levels, _ = S.plan(self.docs, self.fx, no_group=True)
        self.assertEqual(len([lv for lv in levels if lv["launch"]["map"] == "two continents"]), 3)

    def test_a_seat_writer_loads_its_own_level(self):
        docs = dict(self.docs)
        for name in ("d.json", "e.json"):
            d = doc("Two Continents", [[ATTACK]], players=(0, 1))
            d["setup"]["players"][1]["ai"] = "off"
            docs[P(name)] = d
        levels, _ = S.plan(docs, self.fx)
        tc = [[S.rel(i["path"]) for i in lv["items"]] for lv in levels
              if lv["launch"]["map"] == "two continents"]
        self.assertEqual(sorted(tc), [["a.json", "b.json"], ["c.json"], ["d.json"], ["e.json"]])

    def test_a_map_only_content_installs_brings_its_content(self):
        content = {"run": ["tools/resource_fixture.py", "{gamedir}"], "files": ["resource-test.ufo"]}
        docs = dict(self.docs)
        docs[P("rs-host.json")] = doc("RS mixed")
        docs[P("rs-a.json")] = doc("RS mixed", [[ATTACK]])
        docs[P("rs-b.json")] = doc("rs mixed", [[ATTACK]])
        fx = {"fixtures": dict(self.fx["fixtures"],
                               rs={"load": "rs-host.json", "content": content},
                               tc={"load": "a.json", "content": content},
                               tc2={"load": "c.json"}),
              "broad": {}}
        self.assertEqual(list(S.map_content(docs, fx)), ["rs mixed"])
        levels, _ = S.plan(docs, fx)
        lv = next(lv for lv in levels if lv["launch"]["map"] == "rs mixed")
        self.assertEqual(lv["fix"][0], "rs")
        self.assertEqual(lv["launch"]["content"], "rs")
        self.assertEqual([S.rel(i["path"]) for i in lv["items"]], ["rs-a.json", "rs-b.json"])
        # two fixtures that install different content for one map: none is taken
        fx["fixtures"]["rs2"] = {"load": "rs-host.json", "content": dict(content, run=["x"])}
        self.assertEqual(S.map_content(docs, fx), {})

    def test_the_unit_limit_is_always_named(self):
        levels, _ = S.plan(self.docs, self.fx)
        for lv in levels:
            self.assertIsInstance(lv["launch"]["unit_limit"], int)
        f1 = next(lv for lv in levels if lv["fix"] and lv["fix"][0] == "f1")
        self.assertEqual(f1["launch"]["unit_limit"], S.FIXTURE_LIMIT)     # as tareplay launches it
        a = next(lv for lv in levels if lv["items"][0]["path"] == P("a.json"))
        self.assertEqual(a["launch"]["unit_limit"], S.DEFAULT_LIMIT)

    def army(self, n, limit, clear=True, owner=0):
        d = doc("Two Continents", [[ATTACK]], players=(0, 1))
        d["units"][0]["owner"] = owner
        d["groups"] = [{"owner": owner, "composition": [{"type": "ARMPW", "count": n - 1}]}]
        d["setup"]["unit_limit"] = limit
        d["setup"]["clear_existing"] = clear
        return d

    def test_the_room_the_hold_leaves(self):
        docs = {P("big.json"): self.army(300, 500), P("big2.json"): self.army(300, 500),
                P("small.json"): self.army(150, 500), P("full.json"): self.army(500, 500),
                P("keep.json"): self.army(10, 500, clear=False)}
        levels, skipped = S.plan(docs, {"fixtures": {}})
        tc = sorted([S.rel(i["path"]) for i in lv["items"]] for lv in levels)
        # big2 would find 300 slots held by big's units at its own clear: a level of its own;
        # small fits behind it (300 + 150 <= 500); keep keeps what it finds: alone
        self.assertEqual(tc, [["big.json"], ["big2.json", "small.json"], ["keep.json"]])
        # 500 creates on a 500 limit: the start commander's slot is held by the clear
        self.assertIn("does_not_fit", skipped[P("full.json")])
        self.assertIn("player 0 needs 500 slots", skipped[P("full.json")]["does_not_fit"])
        self.assertIsNone(S.misfit({}, self.army(499, 500), 500))
        self.assertIsNotNone(S.misfit({}, self.army(500, 500), 500))
        self.assertEqual(S.room_after({0: 7, 1: 1}, self.army(10, 500, clear=False)), {0: 17, 1: 1})
        rc, lines = S.verdict({}, skipped)
        self.assertEqual(rc, 1)
        self.assertTrue(lines[0].startswith("DOES NOT FIT full.json: "))

    def test_a_mod_scenario_takes_the_content_that_installs_its_types(self):
        stock = ({"ARMPW", "CORAK"}, {"armpw_dead"})
        tx = doc("Show Down", [[ATTACK]])
        tx["units"][0]["type"] = "TXCOM1"
        wx = doc("Show Down", [[ATTACK]])
        wx["units"][0]["type"] = "CORBATSX"
        tz = doc("Show Down", [[ATTACK]])
        tz["units"][0]["type"] = "CORET1ASKBOT"
        docs = {P("tx.json"): tx, P("wx.json"): wx, P("compat-tz.json"): tz,
                P("keys-host.json"): doc("Show Down")}
        fx = {"fixtures": {"b": {"load": "keys-host.json", "content": {"run": ["keys"], "files": []}},
                           "a": {"load": "keys-host.json", "content": {"run": ["other"], "files": []}}}}
        asked = []

        def provides(name, fix):
            asked.append(name)
            return {"TXCOM1"} if fix["content"]["run"] == ["keys"] else set()
        levels, skipped = S.plan(docs, fx, stock=stock, compat={"compat-tz": "tazero-alpha5"},
                                 provides=provides)
        lv = next(lv for lv in levels if lv["items"][0]["path"] == P("tx.json"))
        self.assertEqual(lv["fix"][0], "b")
        self.assertEqual(lv["launch"]["content"], "b")
        self.assertEqual(lv["launch"]["unit_limit"], S.DEFAULT_LIMIT)  # the content alone is lent
        self.assertEqual(skipped[P("wx.json")],
                         {"could_not_run": "needs CORBATSX, which no fixture's content installs"})
        self.assertIn("the compatibility suite plays it in tazero-alpha5",
                      skipped[P("compat-tz.json")]["could_not_run"])
        self.assertEqual(S.verdict({}, {P("wx.json"): skipped[P("wx.json")]})[0], 2)
        self.assertEqual(S.needs_mod(tx, None), [])

    def test_the_limits_match_the_launchers(self):
        sys.path.insert(0, str(S.TREE / "tools" / "replay"))
        import tareplay
        self.assertEqual(S.FIXTURE_LIMIT, tareplay.UNIT_LIMIT)
        self.assertEqual(S.DEFAULT_LIMIT, tareplay._CLI_API.SCN_DEFAULT_LIMIT)

    def test_calibration_on_the_arena(self):
        levels, _ = S.plan(self.docs, self.fx)
        lv = next(lv for lv in levels if lv["launch"]["map"] == "cavedog links cc")
        self.assertEqual([(S.rel(i["path"]), i["report"]) for i in lv["items"]],
                         [("calib/arena.json", False), ("calib/duel.json", True)])

    def test_the_trees_plan(self):
        docs = S.scenario_docs()
        levels, unhosted = S.plan(docs, json.loads(S.FIXTURES.read_text()))
        reported = [i["path"] for lv in levels for i in lv["items"] if i["report"]]
        ordered = {p for p, d in docs.items() if S.orders_of(d)}
        self.assertEqual(sorted(reported + list(unhosted)), sorted(ordered))
        self.assertEqual(len(reported), len(set(reported)))
        for lv in levels:
            self.assertTrue(lv["items"][0]["at"] is None)
            self.assertTrue((docs[lv["items"][0]["path"]].get("setup") or {}).get("map"))


EXP = {"units": [{"id": "pw", "type": "ARMPW", "owner": 0,
                  "orders": [{"cmd": "move", "to": [10, 20], "target": None, "after": 0},
                             {"cmd": "attack", "to": None, "target": "ak", "after": 0}]},
                 {"id": "ak", "type": "CORAK", "owner": 1,
                  "orders": [{"cmd": "reclaim", "to": [5, 6], "target": None, "after": 40}]}]}


class Answers(unittest.TestCase):
    def test_result_in_noisy_text(self):
        text = 'launching...\n{"x": 1}\n{"ok": 0, "orders": {"issued": 1, "failed": 1}}\ndone'
        self.assertEqual(S.result_of(text)["ok"], 0)
        self.assertIsNone(S.result_of("no json"))

    def test_refusal_named(self):
        res = {"errors": ["order 1: this unit cannot take that order (ARMPW, cmd 3)",
                          "unit 4 (X): the engine refused to create it"]}
        refused, other = S.refusals(res, EXP)
        self.assertEqual([(r["unit"], r["order"], r["target"]) for r in refused],
                         [("pw", "attack", "ak")])
        self.assertEqual(other, ["unit 4 (X): the engine refused to create it"])

    def test_delayed_refusal_named(self):
        res = {"units": {"pw": {"engine_index": 3}, "ak": {"engine_index": 503}}}
        lines = ["scenario: delayed order at GameTime 140: slot 503 cannot take cmd 12",
                 "scenario: delayed orders: issued 0 of 1, refused 1, cancelled 0 (deaths 0, "
                 "cleared 0, skipped 0)"]
        got = S.delayed_refusals(lines, res, EXP)
        self.assertEqual([(r["unit"], r["order"], r["after"]) for r in got],
                         [("ak", "reclaim", 40)])

    def test_verdict(self):
        clean = {"refused": [], "other_errors": [], "creates_failed": 0, "ok": True, "applied": 3}
        ok = {P("a.json"): clean}
        self.assertEqual(S.verdict(ok, {})[0], 0)
        self.assertEqual(S.verdict(ok, {P("lost.json"): {"could_not_run": "x"}})[0], 2)
        bad = {P("a.json"): dict(clean, ok=False,
                                 refused=[{"unit": "pw", "order": "attack", "target": "ak"}]),
               P("b.json"): {"could_not_run": "crash"}}
        rc, lines = S.verdict(bad, {})
        self.assertEqual(rc, 1)
        self.assertTrue(any(ln.startswith("REFUSED a.json: pw") for ln in lines))
        self.assertFalse(any(ln.startswith("FAILED a.json") for ln in lines))

    def test_an_apply_with_errors_or_nothing_created_fails(self):
        clean = {"refused": [], "other_errors": [], "creates_failed": 0, "ok": True, "applied": 3}
        abort = dict(clean, ok=False, applied=0, other_errors=[
            "player 1: ai off refused: not an AI seat other than the controlled one (type 1)",
            "1 entities did not resolve and on_error is abort: nothing was created"])
        for row in (abort, dict(clean, applied=0), dict(clean, ok=False),
                    dict(clean, ok=False, creates_failed=1), {"refused": [], "ok": True}):
            rc, lines = S.verdict({P("a.json"): row}, {})
            self.assertEqual(rc, 1, row)
            self.assertTrue(lines[0].startswith("FAILED a.json: "), lines)
            self.assertIn("0 scenario(s) applied clean", lines[-1])
        _rc, lines = S.verdict({P("a.json"): abort}, {})
        self.assertIn("FAILED a.json: nothing was created", lines)


class FakeR:
    """tareplay as the sweep uses it, answering from a table: each scenario's apply result, a
    crash on a named scenario, and the delayed lines logged when a follow passes their tick."""

    class CouldNotRun(Exception):
        pass

    class Failed(Exception):
        pass

    LAUNCH_TIMEOUT = 1
    STOP = threading.Event()

    def __init__(self, answers, crash=(), delayed=()):
        self.answers, self.crash, self.delayed = answers, set(crash), list(delayed)
        self.calls, self.lock = [], threading.Lock()
        R = self

        class Worker:
            def __init__(self, out):
                self.name = f"fake{id(self)}"

            def place(self, fix, dll):
                R.log_call(("place", fix.get("name")))

            def stop(self, hard=False):
                pass

            def watch_caps(self):
                pass

            def cap_failure(self):
                return ""

            def close(self):
                pass

        class Log:
            def __init__(self, w):
                self.lines, self.at = [], 1

            def read(self):
                return []

        self.Worker, self.Log = Worker, Log
        self._CLI_API = types.SimpleNamespace(_scn_compile=lambda path, *a: EXP)

    def log_call(self, c):
        with self.lock:
            self.calls.append(c)

    def say(self, *a):
        pass

    def last(self, text):
        return text.strip().splitlines()[-1] if text.strip() else ""

    def tacli(self, *args, timeout=0):
        path = Path(str(args[4] if args[1] == "load" else args[4]))
        self.log_call((args[1], S.rel(path)))
        if S.rel(path) in self.crash:
            return 1, "the game crashed"
        return 1, json.dumps(self.answers.get(S.rel(path), {"ok": 1, "applied": 1, "orders": {}}))

    def held_start(self, w, log):
        log.at = 1

    def hold_at(self, w, log, T):
        log.pending = T

    def turbo_engage(self, w, log):
        pass

    def resume(self, w, log):
        pass

    def wait_paused(self, w, log, T):
        self.log_call(("held", T))
        log.lines += [ln for t, ln in self.delayed if log.at < t <= T]
        log.at = T


class Levels(unittest.TestCase):
    def setUp(self):
        self.docs = {P("host.json"): doc("Show Down", [[ATTACK]]),
                     P("wave.json"): doc(None, [[ATTACK]], after=40),
                     P("m1.json"): doc("Two Continents", [[ATTACK]]),
                     P("m2.json"): doc("Two Continents", [[ATTACK]]),
                     P("m3.json"): doc("Two Continents", [[ATTACK]])}
        self.fx = {"fixtures": {"f": {"load": "host.json", "steps": [[30, "apply", "wave.json"]]}}}
        self.refused = {"ok": 0, "orders": {"issued": 1, "failed": 1},
                        "errors": ["order 1: this unit cannot take that order (ARMPW, cmd 3)"],
                        "units": {"ak": {"engine_index": 503}}}

    def run_fake(self, R, no_group=False, j=2):
        levels, unhosted = S.plan(self.docs, self.fx, no_group)
        with tempfile.TemporaryDirectory() as d:
            return S.run_levels(levels, self.docs, j, Path(d), Path(d) / "ddraw.dll", R)

    def test_refusals_and_delayed_ones_reported(self):
        R = FakeR({"host.json": self.refused, "wave.json": dict(self.refused, errors=[])},
                  delayed=[(71, "scenario: delayed order at GameTime 71: slot 503 cannot take "
                                "cmd 12")])
        done = self.run_fake(R)
        self.assertEqual(set(done), set(self.docs))
        self.assertEqual([r["unit"] for r in done[P("host.json")]["refused"]], ["pw"])
        self.assertEqual([(r["unit"], r["order"]) for r in done[P("wave.json")]["refused"]],
                         [("ak", "reclaim")])
        # the wave applied at its tick, then followed past its last delayed order
        self.assertIn(("held", 30), R.calls)
        self.assertIn(("held", 30 + 40 + 2), R.calls)
        rc, _lines = S.verdict(done, {})
        self.assertEqual(rc, 1)

    def test_a_failed_level_hands_its_mapped_scenarios_on(self):
        R = FakeR({}, crash={"m2.json"})
        done = self.run_fake(R, j=1)
        self.assertIn("could_not_run", done[P("m2.json")])
        self.assertEqual(done[P("m3.json")]["refused"], [])
        self.assertEqual(done[P("m1.json")]["refused"], [])
        loads = [c for c in R.calls if c[0] == "load"]
        self.assertIn(("load", "m3.json"), loads)          # m3 on a level of its own

    def test_a_grouped_apply_waits_out_the_hold(self):
        R = FakeR({})
        self.run_fake(R, j=1)
        # m1 loads at GameTime 1; m2 and m3 follow it on its level, each HOLD_TICKS after the last
        self.assertIn(("held", 1 + S.HOLD_TICKS), R.calls)
        self.assertIn(("held", 1 + 2 * S.HOLD_TICKS), R.calls)

    def test_parallel_levels_cover_everything(self):
        R = FakeR({})
        done = self.run_fake(R, no_group=True, j=4)
        self.assertEqual(set(done), set(self.docs))


if __name__ == "__main__":
    unittest.main()
