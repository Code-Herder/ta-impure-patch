"""The live probe must distinguish a missing API from a passing call count."""
import unittest
from unittest.mock import patch
from pathlib import Path
import tempfile

import cob_getter_probe as probe


class ProbeTests(unittest.TestCase):
    def test_ceiling_setup_handles_ini_with_or_without_explicit_limit(self):
        for content in ('[Preferences]\nUnitLimit=1000\n', '[Preferences]\nIntro=0\n'):
            with self.subTest(content=content), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                ini = root / 'TAESC.ini'
                ini.write_text(content)
                with patch.object(probe.subprocess, 'run') as run:
                    probe.configure_skirmish_ceiling(dict(gamedir=root, prefix=root / 'private-prefix'))
                self.assertEqual(ini.read_text().count('UnitLimit=1500'), 1)
                self.assertEqual(run.call_count, 17)
                self.assertTrue(any('Player3Controller' in call.args[0] for call in run.call_args_list))

    def test_ceiling_fixture_keeps_commander_and_fills_last_skirmish_block(self):
        scenario = probe.skirmish_ceiling_scenario()
        self.assertFalse(scenario['setup']['clear_existing'])
        last_owner = [u for u in scenario['units'] if u['owner'] == 3]
        self.assertEqual(len(last_owner), 1499)
        self.assertEqual(last_owner[-1]['type'], probe.TYPE)
        self.assertEqual(last_owner[-1]['kills'], 10)
        checks = probe.judge({(6000, 71): 6000, (6000, 72): 3,
                              (6000, 70): 15000, (6000, 32): 1000},
                             {6000: 3}, 15001, 'implemented', extended=True)
        self.assertTrue(all(c['ok'] for c in checks if c['getter'] in (32, 70, 71, 72)))

    def test_thread_exhaustion_fixtures_fill_exactly_the_stack_capacity(self):
        for mode in ('start', 'call'):
            report = probe.cob_audit.audit_blob(probe.probe_cob(['base'], thread_exhaustion=mode))
            self.assertEqual(report['peak_words'], 128)
            self.assertFalse(any(s['issues'] for s in report['scripts']))
            self.assertIn('CapacityWorker', {s['script'] for s in report['scripts']})

    def test_incomplete_trace_is_not_a_passing_oracle(self):
        with self.assertRaises(ValueError):
            probe.returns('# INCOMPLETE: trace thread capacity or TLS unavailable\n')

    def test_extended_probe_checks_all_arguments_without_truncating_trace(self):
        report = probe.cob_audit.audit_blob(probe.probe_cob(['base'], extended=True))
        names = {s['script'] for s in report['scripts']}
        self.assertIn('Probe901', names)
        args = ','.join(str(-2147483648 + i) for i in range(100))
        trace = f'S\t1\t2\tCOBFPROBE\tProbe901\t1\tL:0\t{args}\n'
        self.assertTrue(probe.arguments_match(trace, [2]))
        self.assertFalse(probe.arguments_match(trace[:512], [2]))
        self.assertFalse(probe.arguments_match(trace, [2, 1002]))

    def test_zero_extended_unit_id_is_not_a_self_query(self):
        checks = probe.judge({(2, 7302): -1, (2, 7502): 0}, {2: 0},
                             10001, 'implemented', extended=True)
        self.assertTrue(all(c['ok'] for c in checks if c['getter'] in (7302, 7502)))

    def test_options_navigation_acknowledges_delayed_tab_menu(self):
        panels = iter(['gui ARMMAIN2.GUI', 'gui ARMMAIN2.GUI',
                       'gui TABMENU.GUI', 'gui ARMOPT.GUI'])
        actions = []

        def command(*args, **kwargs):
            if args == ('ui', 'probe'):
                return next(panels)
            actions.append(args)
            return ''

        with patch.object(probe, 'command', command):
            probe.open_options('probe')
        self.assertEqual(actions, [('keys', 'probe', 'tab', 'tab'),
                                   ('ui', 'probe', 'click', 'OPTIONS')])

    def test_options_navigation_keeps_existing_options_panel(self):
        with patch.object(probe, 'command', return_value='gui ARMOPT.GUI') as command:
            probe.open_options('probe')
        command.assert_called_once_with('ui', 'probe')

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
