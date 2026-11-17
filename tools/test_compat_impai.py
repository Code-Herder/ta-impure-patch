"""The compatibility suite's ImpAI checks, offline: what a stage game's and a network game's tagpu.log must say of
an ImpAI seat -- its seat lines, and THE CLAIM: by its last pulse it gave orders and completed a unit."""
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
    pulse(1, 1807, 2, 0, 9),
    pulse(1, 3607, 6, 4, 21),
    "impai: handicap rows 0",
])


def net(role_seat, other):
    return "\n".join([
        f"impai: seats {role_seat} (netai=impai, a network game, level 2) of 4 records; other peers' AI seats "
        f"{other}, ImpAI's {other}",
        pulse(role_seat, 1807, 1, 0, 5),
        pulse(role_seat, 3607, 3, 2, 11),
    ])


class Claim(unittest.TestCase):
    def test_a_stage_that_played_meets_the_claim(self):
        st = tacompat.impai_stage_read(STAGE)
        self.assertEqual(st["pulses"][1][-1], (3607, 6, 25))
        self.assertEqual(tacompat.impai_stage_misses(st, "impai"), (True, []))

    def test_a_seat_that_took_turns_but_built_nothing_fails(self):
        text = STAGE.replace(pulse(1, 3607, 6, 4, 21), pulse(1, 3607, 0, 0, 21))
        available, miss = tacompat.impai_stage_misses(tacompat.impai_stage_read(text), "impai")
        self.assertTrue(available)
        self.assertEqual(miss, ["the stage: ImpAI's seat 1 completed no unit by GameTime 3607"])

    def test_a_seat_that_gave_no_order_fails(self):
        text = STAGE.replace(pulse(1, 3607, 6, 4, 21), pulse(1, 3607, 1, 0, 0))
        _, miss = tacompat.impai_stage_misses(tacompat.impai_stage_read(text), "impai")
        self.assertEqual(miss, ["the stage: ImpAI's seat 1 gave no order by GameTime 3607"])

    def test_a_stage_with_no_pulse_fails_but_the_survey_asks_none(self):
        text = "\n".join(ln for ln in STAGE.splitlines() if "pulse" not in ln)
        _, miss = tacompat.impai_stage_misses(tacompat.impai_stage_read(text), "impai")
        self.assertEqual(len(miss), 1)
        self.assertIn("wrote no pulse", miss[0])
        self.assertEqual(tacompat.impai_stage_misses(tacompat.impai_stage_read(text), "impai", survey=True),
                         (True, []))

    def test_the_ota_harness_asks_no_claim(self):
        text = STAGE.replace("impai: seats 1 (impure.cfg)", "impai: seats none (impure.cfg)")
        self.assertEqual(tacompat.impai_stage_misses(tacompat.impai_stage_read(text), "ota"), (None, []))

    def test_each_peer_of_a_network_game_is_held_to_the_claim_on_its_own_seat(self):
        self.assertEqual(tacompat.impai_mp_misses({"host": net(2, 3), "join": net(3, 2)}), [])
        lazy = net(3, 2).replace(pulse(3, 3607, 3, 2, 11), pulse(3, 3607, 0, 2, 11))
        self.assertEqual(tacompat.impai_mp_misses({"host": net(2, 3), "join": lazy}),
                         ["join: ImpAI's seat 3 completed no unit by GameTime 3607"])
        silent = "\n".join(ln for ln in net(2, 3).splitlines() if "pulse" not in ln)
        miss = tacompat.impai_mp_misses({"host": silent, "join": net(3, 2)})
        self.assertEqual(len(miss), 1)
        self.assertTrue(miss[0].startswith("host: ImpAI's seat 2 wrote no pulse"))

    def test_the_claim_needs_two_game_minutes(self):
        self.assertGreater(tacompat.IMPAI_CLAIM_TICKS, 2 * 1800)


if __name__ == "__main__":
    unittest.main()
