"""The live probe must distinguish a missing API from a passing call count."""
import unittest

import cob_getter_probe as probe


class ProbeTests(unittest.TestCase):
    def test_fixture_reaches_all_eight_getters_in_small_frames(self):
        report = probe.cob_audit.audit_blob(probe.probe_cob(["base"]))
        self.assertFalse(any(s["issues"] for s in report["scripts"]))
        self.assertEqual({g for s in report["scripts"] for g in s["gets"]}, set(probe.GETTERS))
        self.assertLessEqual(report["peak_words"], 5)
        self.assertEqual(len(report["scripts"][0]["calls"]), 8)

    def test_stock_zero_fails_the_port_contract(self):
        rows = {(u, g): 0 for u in (1, 1501) for g in probe.GETTERS}
        checks = probe.judge(rows, {1: 0, 1501: 1}, 15001, "implemented")
        self.assertEqual(sum(not r["ok"] for r in checks), 11)
        self.assertTrue(all(r["ok"] for r in probe.judge(rows, {1: 0, 1501: 1}, 15001, "stock-zero")))

    def test_absent_return_does_not_count_as_zero(self):
        self.assertFalse(any(r["ok"] for r in probe.judge({}, {1: 0}, 15001, "stock-zero")))

    def test_parse_values_and_reject_ambiguous_duplicate(self):
        trace = "S\t1\t1\tCOBFPROBE\tProbe69\t1\tL:0\t\nR\t2\t1\t1\tProbe69\t1\n"
        self.assertEqual(probe.returns(trace), {(1, 69): 1})
        with self.assertRaises(ValueError):
            probe.returns(trace + trace)


if __name__ == "__main__":
    unittest.main()
