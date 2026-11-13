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
        self.assertEqual(unhosted, [P("lost.json")])

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
        self.assertEqual(sorted(reported + unhosted), sorted(ordered))
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
        ok = {P("a.json"): {"refused": [], "other_errors": [], "creates_failed": 0}}
        self.assertEqual(S.verdict(ok, [])[0], 0)
        self.assertEqual(S.verdict(ok, [P("lost.json")])[0], 2)
        bad = {P("a.json"): {"refused": [{"unit": "pw", "order": "attack", "target": "ak"}],
                             "other_errors": [], "creates_failed": 0},
               P("b.json"): {"could_not_run": "crash"}}
        rc, lines = S.verdict(bad, [])
        self.assertEqual(rc, 1)
        self.assertTrue(any(ln.startswith("REFUSED a.json: pw") for ln in lines))


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
        return 1, json.dumps(self.answers.get(S.rel(path), {"ok": 1, "orders": {}}))

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
        rc, _lines = S.verdict(done, [])
        self.assertEqual(rc, 1)

    def test_a_failed_level_hands_its_mapped_scenarios_on(self):
        R = FakeR({}, crash={"m2.json"})
        done = self.run_fake(R, j=1)
        self.assertIn("could_not_run", done[P("m2.json")])
        self.assertEqual(done[P("m3.json")]["refused"], [])
        self.assertEqual(done[P("m1.json")]["refused"], [])
        loads = [c for c in R.calls if c[0] == "load"]
        self.assertIn(("load", "m3.json"), loads)          # m3 on a level of its own

    def test_parallel_levels_cover_everything(self):
        R = FakeR({})
        done = self.run_fake(R, no_group=True, j=4)
        self.assertEqual(set(done), set(self.docs))


if __name__ == "__main__":
    unittest.main()
