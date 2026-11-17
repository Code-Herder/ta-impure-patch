"""The compatibility suite's ImpAI checks, offline: what a stage game's and a network game's tagpu.log must say of
an ImpAI seat -- its seat lines, and THE CLAIM: by its last pulse it gave orders and completed a unit since its first;
and THE FIGHT STAYS OFF THE SEATS: the network game's waves stand clear of every start its records take."""
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parent / 'compat'))
import tacompat


def pulse(seat, t, built, mroots, eroots):
    return (f"impai: pulse seat={seat} t={t} built={built} mroots={mroots} eroots={eroots} army=0 squads=- "
            f"metal=0 energy=0 vlive=0 vbuilt=0")


STAGE = "\n".join([
    "impai: seam 0x465031 installed",
    "impai: seats 1 (impure.cfg) of 3 records",
    "impai: work seat=1 ticks=40 refused=0 slots_max=1/1",
    pulse(1, 7, 1, 0, 0),
    pulse(1, 1807, 2, 0, 9),
    pulse(1, 3607, 6, 4, 21),
    "impai: handicap rows 0",
])


def net(role_seat, other):
    return "\n".join([
        f"impai: seats {role_seat} (netai=impai, a network game, level 2) of 4 records; other peers' AI seats "
        f"{other}, ImpAI's {other}",
        pulse(role_seat, 7, 1, 0, 0),
        pulse(role_seat, 1807, 1, 0, 5),
        pulse(role_seat, 3607, 3, 2, 11),
    ])


class Claim(unittest.TestCase):
    def test_a_stage_that_played_meets_the_claim(self):
        st = tacompat.impai_stage_read(STAGE)
        self.assertEqual(st["pulses"][1][-1], (3607, 6, 25))
        self.assertEqual(tacompat.impai_stage_misses(st, "impai"), (True, []))

    def test_a_seat_that_took_turns_but_built_nothing_fails(self):
        # the commander alone: the first pulse already counts it (MEASURED, every seat's first pulse)
        text = STAGE.replace(pulse(1, 1807, 2, 0, 9), pulse(1, 1807, 1, 0, 9)).replace(
            pulse(1, 3607, 6, 4, 21), pulse(1, 3607, 1, 85, 0))
        available, miss = tacompat.impai_stage_misses(tacompat.impai_stage_read(text), "impai")
        self.assertTrue(available)
        self.assertEqual(miss, ["the stage: ImpAI's seat 1 completed no unit from GameTime 7 to 3607 (built=1 at both)"])

    def test_a_seat_that_gave_no_order_fails(self):
        text = STAGE.replace(pulse(1, 3607, 6, 4, 21), pulse(1, 3607, 2, 0, 0))
        _, miss = tacompat.impai_stage_misses(tacompat.impai_stage_read(text), "impai")
        self.assertEqual(miss, ["the stage: ImpAI's seat 1 gave no order by GameTime 3607"])

    def test_a_stage_with_no_pulse_fails_but_the_survey_asks_none(self):
        text = "\n".join(ln for ln in STAGE.splitlines() if "pulse" not in ln)
        _, miss = tacompat.impai_stage_misses(tacompat.impai_stage_read(text), "impai")
        self.assertEqual(miss, ["the stage: ImpAI's seat 1 wrote 0 pulse(s), not one after the level's first"])
        first_only = "\n".join(ln for ln in STAGE.splitlines() if " t=1807 " not in ln and " t=3607 " not in ln)
        _, miss = tacompat.impai_stage_misses(tacompat.impai_stage_read(first_only), "impai")
        self.assertEqual(miss, ["the stage: ImpAI's seat 1 wrote 1 pulse(s), not one after the level's first"])
        self.assertEqual(tacompat.impai_stage_misses(tacompat.impai_stage_read(text), "impai", survey=True),
                         (True, []))

    def test_the_ota_harness_asks_no_claim(self):
        text = STAGE.replace("impai: seats 1 (impure.cfg)", "impai: seats none (impure.cfg)")
        self.assertEqual(tacompat.impai_stage_misses(tacompat.impai_stage_read(text), "ota"), (None, []))

    def test_each_peer_of_a_network_game_is_held_to_the_claim_on_its_own_seat(self):
        self.assertEqual(tacompat.impai_mp_misses({"host": net(2, 3), "join": net(3, 2)}), [])
        # the joiner measured 2026-11-16: orders from the military alone, nothing completed past the commander
        lazy = net(3, 2).replace(pulse(3, 1807, 1, 0, 5), pulse(3, 1807, 1, 25, 0)).replace(
            pulse(3, 3607, 3, 2, 11), pulse(3, 3607, 1, 85, 0))
        self.assertEqual(tacompat.impai_mp_misses({"host": net(2, 3), "join": lazy}),
                         ["join: ImpAI's seat 3 completed no unit from GameTime 7 to 3607 (built=1 at both)"])
        silent = "\n".join(ln for ln in net(2, 3).splitlines() if "pulse" not in ln)
        miss = tacompat.impai_mp_misses({"host": silent, "join": net(3, 2)})
        self.assertEqual(len(miss), 1)
        self.assertEqual(miss, ["host: ImpAI's seat 2 wrote 0 pulse(s), not one after the level's first"])

    def test_the_claim_needs_two_game_minutes(self):
        self.assertGreater(tacompat.IMPAI_CLAIM_TICKS, 2 * 1800)


SCENARIOS = Path(__file__).resolve().parents[1] / "scenarios"


def install_starts(mapname):
    """{N: (x, z)} of `mapname` in the installed game, None when no install is found to read."""
    sys.path[:0] = [str(Path(__file__).parent / "wiki"), str(Path(__file__).parent / "impai")]
    import tavfs
    import bar
    try:
        return bar.start_positions(mapname, tacompat.MP_RECORDS, tavfs.Vfs(tavfs.find_install(None)))
    except (tavfs.VfsError, bar.BarError, OSError, SystemExit):
        return None


def halves():
    """[(host, join)] of every network game's halves: the default pair and each setup's own."""
    out = {(tacompat.DEFAULT_SCENARIOS["host"], tacompat.DEFAULT_SCENARIOS["join"])}
    for s in tacompat.SETUPS:
        sc = tacompat.scenarios(s)
        out.add((sc["host"], sc["join"]))
    return sorted(out)


def load(name):
    return json.loads((SCENARIOS / f"{name}.json").read_text())


class Waves(unittest.TestCase):
    STARTS = {1: (0, 0), 2: (5000, 0), 3: (0, 5000), 4: (5000, 5000)}

    def wave(self, at):
        return {"groups": [{"id": "w", "at": at, "pattern": {"kind": "grid", "cols": 6, "spacing": 40, "jitter": 6},
                            "composition": [{"type": "CORAK", "count": 30}]}]}

    def test_a_wave_inside_the_clearance_of_a_start_is_named(self):
        miss = tacompat.wave_misses(self.wave([400, 0]), self.STARTS)
        self.assertEqual(len(miss), 1)
        self.assertIn("StartPos1", miss[0])

    def test_the_reach_of_the_formation_counts(self):
        # 6 x 5 at 40 with a jitter of 6: hypot(100, 80) = 128.1 from its centre to a corner unit, 8.5 more for the jitter
        self.assertAlmostEqual(tacompat.group_reach(self.wave([0, 0])["groups"][0]), 128.06 + 8.49, places=1)
        self.assertTrue(tacompat.wave_misses(self.wave([1500 + 130, 0]), self.STARTS))
        self.assertEqual(tacompat.wave_misses(self.wave([1500 + 140, 0]), self.STARTS), [])

    def test_only_the_seated_records_starts_are_kept_clear(self):
        starts = {**self.STARTS, 5: (2500, 2500)}
        self.assertEqual(tacompat.wave_misses(self.wave([2500, 2500]), starts), [])
        self.assertTrue(tacompat.wave_misses(self.wave([2500, 2500]), starts, records=5))

    def test_a_start_the_map_lacks_is_named(self):
        self.assertEqual(tacompat.wave_misses({"groups": []}, {1: (0, 0)}),
                         ["the map has no StartPos2", "the map has no StartPos3", "the map has no StartPos4"])

    def test_each_half_attacks_the_other_halfs_wave(self):
        for host, join in halves():
            h, j = load(host)["groups"], load(join)["groups"]
            for mine, theirs in ((h, j), (j, h)):
                for g in mine:
                    to = [o["to"] for o in g.get("orders", []) if o["cmd"] == "attack"]
                    self.assertTrue(to, f"{g['id']} attacks nothing")
                    self.assertTrue(any(tacompat.math.dist(to[0], t["at"]) <= tacompat.group_reach(t) for t in theirs),
                                    f"{host}/{join}: {g['id']} attacks {to[0]}, not the other half's wave")

    def test_every_half_is_clear_of_the_seats_on_the_installed_map(self):
        for host, join in halves():
            for name in (host, join):
                scen = load(name)
                starts = install_starts(scen["setup"]["map"])
                if starts is None:
                    self.skipTest("no installed game to read the map's starts from")
                self.assertEqual(tacompat.wave_misses(scen, starts), [], name)


if __name__ == "__main__":
    unittest.main()
