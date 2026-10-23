#!/usr/bin/env python3
"""Evidence-tool contracts with synthetic records; no host/GUI acceptance claim."""
import contextlib
import copy
import io
import json
import os
import time
from datetime import datetime, timezone
from unittest.mock import patch
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

import prepare_plugin_dualhost_gui as gui

REPO = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'scripts'))
from prepare_plugin_dualhost import prepare, OPERATIONS


class Contracts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='dualhost-contract-中文-')
        self.root = Path(self.temp.name)
        # Synthetic records test the evidence gate; they are never presented as
        # actual host exits. The live-PID negative control below uses this Python PID.
        self.pid_patch = patch.object(gui, 'pid_present', return_value=False)
        self.pid_patch.start()
        self.home_patch = patch.object(gui.Path, 'home', return_value=self.root)
        self.home_patch.start()
        self.crashes = self.root / 'Library/Logs/DiagnosticReports'
        self.crashes.mkdir(parents=True)
        with contextlib.redirect_stdout(io.StringIO()):
            prepare(self.root / 'canonical')
        self.binary = self.root / 'script-contract-only.bin'
        self.binary.write_bytes(b'Synthetic file for evidence-tool validation only; not a runtime.')
        self.profile = self.root / 'profile'
        self.profile.mkdir()
        self.storage = self.profile / 'storage'
        self.storage.mkdir()
        self.request = self.root / 'request.json'
        gui.write(self.request, {'test': 'evidence contract only'})
        self.report = self.root / 'report.json'
        Path(str(self.report) + '.startup').write_text('post_init complete; no modal dialogs\n', encoding='utf-8')
        self.case = 'pages/pages/installed-disabled'
        self.obs = {'case': self.case, 'phase': 'first', 'passed': True,
                    'operations': [{'action': action, 'outcome': 'ok', 'pid': 42} for action in ['install', 'disable']],
                    'errors': [], 'returns': [], 'configuration': {}, 'artifacts': {}, 'resource_consumer': {},
                    'storage': str(self.storage), 'capability_enabled': False}
        self.record = {'pid': 42, 'ui_thread': True, 'normal_window_close': True, 'normal_close_completed': True,
                       'interpreter_stopped': True, 'data_dir': str(self.profile), 'ok': True,
                       'dualhost': {'passed': True, 'cases': {'pages': self.obs}}}
        self.exit = {'pid': 42, 'on_exit': True, 'result': 0}
        self.phase = {'phase': 'first', 'binary': str(self.binary), 'report': str(self.report), 'request': str(self.request),
                      'request_sha256': gui.sha(self.request), 'profile': str(self.profile)}
        self.run = {'repo': str(REPO), 'manifest': str(self.root / 'canonical/manifest.json'),
                    'manifest_sha256': gui.sha(self.root / 'canonical/manifest.json'),
                    'binary': str(self.binary), 'binary_sha256': gui.sha(self.binary),
                    'cases': [{'id': self.case, 'group': 'pages/installed-disabled', 'phases': [self.phase]}]}
        self.args = SimpleNamespace(run=self.root / 'run.json', case=self.case, phase='first')
        self.save()

    def tearDown(self):
        self.home_patch.stop()
        self.pid_patch.stop()
        self.temp.cleanup()

    def save(self):
        gui.write(self.args.run, self.run)
        gui.write(self.report, self.record)
        gui.write(str(self.report) + '.exit', self.exit)

    def finish(self):
        with contextlib.redirect_stdout(io.StringIO()):
            gui.finish(self.args)

    def test_disabled_first_phase_has_no_invented_callback_or_close(self):
        self.finish()
        actual = gui.read(self.root / 'first-final.json')
        self.assertEqual(actual['storage_trace_lines'], 0)
        self.assertEqual([op['action'] for op in actual['observations']['pages']['operations']], ['install', 'disable'])
        self.assertEqual(actual['traces'][0]['last_line'], 0)
        with self.assertRaisesRegex(ValueError, 'already finalized'):
            self.finish()

    def test_wrong_exit_pid_rejected(self):
        self.exit['pid'] = 43
        self.save()
        with self.assertRaisesRegex(ValueError, 'matching-PID'):
            self.finish()

    def test_unfinished_normal_close_rejected(self):
        self.record['normal_close_completed'] = False
        self.save()
        with self.assertRaisesRegex(ValueError, 'matching-PID'):
            self.finish()

    def test_wrong_profile_rejected(self):
        self.record['data_dir'] = str(self.root / 'another-profile')
        self.save()
        with self.assertRaisesRegex(ValueError, 'prepared profile'):
            self.finish()

    def test_changed_request_rejected(self):
        gui.write(self.request, {'test': 'changed'})
        with self.assertRaisesRegex(ValueError, 'request changed'):
            self.finish()

    def test_changed_binary_rejected(self):
        self.binary.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'executable changed'):
            self.finish()

    def test_multi_group_cannot_omit_texture(self):
        other = copy.deepcopy(self.run['cases'][0])
        other['id'] = 'multi/texture/installed-disabled'
        self.run['cases'][0]['id'] = 'multi/pages/installed-disabled'
        self.obs['case'] = 'multi/pages/installed-disabled'
        self.run['cases'].append(other)
        self.args.case = 'multi/pages/installed-disabled'
        self.save()
        with self.assertRaisesRegex(ValueError, 'omitted a prepared capability'):
            self.finish()

    def test_restart_cannot_reuse_pid(self):
        self.finish()
        self.args.phase = 'resume'
        self.phase['phase'] = 'resume'
        self.obs['phase'] = 'resume'
        self.obs['operations'] = [{'action': 'restart', 'outcome': 'ok', 'pid': 42},
                                  {'action': 'verify_disabled', 'outcome': 'ok', 'pid': 42}]
        self.save()
        with self.assertRaisesRegex(ValueError, 'reused the first host process'):
            self.finish()

    def test_unknown_case_cannot_select_an_arbitrary_path(self):
        self.args.case = '../../another-profile'
        with self.assertRaisesRegex(ValueError, 'not prepared'):
            self.finish()

    def test_actual_current_pid_is_rejected_without_terminating_it(self):
        self.pid_patch.stop()
        try:
            with self.assertRaisesRegex(ValueError, 'still present'):
                gui.verify_process_exit(os.getpid(), time.time(), crash_directory=self.crashes)
            self.assertTrue(gui.pid_present(os.getpid()))
        finally:
            self.pid_patch.start()

    def crash(self, pid, name='Snapmaker_Orca-contract.ips', timestamp=None):
        timestamp = timestamp or datetime.now(timezone.utc).isoformat()
        path = self.crashes / name
        path.write_text(json.dumps({'app_name': 'Snapmaker_Orca', 'timestamp': timestamp}) +
                        '\n{\n  "pid" : ' + str(pid) + ',\n  "unreadDetails" : "not inspected"\n}\n', encoding='utf-8')
        return path

    def test_matching_pid_crash_rejects_finish(self):
        self.crash(42)
        with self.assertRaisesRegex(ValueError, 'has a macOS crash report'):
            self.finish()
        self.assertFalse((self.root / 'first-final.json').exists())
        self.assertTrue(self.report.exists())

    def test_late_crash_rejected_on_second_exit_check(self):
        self.finish()
        collector = gui.tools(REPO)
        with contextlib.redirect_stdout(io.StringIO()):
            collector.scaffold(SimpleNamespace(manifest=self.root / 'canonical/manifest.json', binary=self.binary, output=self.root / 'plan'))
        self.crash(42)
        with self.assertRaisesRegex(ValueError, 'has a macOS crash report'):
            gui.collect(SimpleNamespace(run=self.args.run, revision='synthetic tool contract only', output=self.root / 'observations'))
        self.assertFalse((self.root / 'observations').exists())

    def test_unrelated_pid_and_old_crash_do_not_fail_this_process(self):
        self.crash(43)
        self.crash(42, 'Snapmaker_Orca-old.ips', '2020-01-01T00:00:00+00:00')
        self.finish()
        self.assertTrue((self.root / 'first-final.json').exists())

    def test_all_groups_keep_28_cases_and_need_27_actual_processes(self):
        manifest = gui.read(self.root / 'canonical/manifest.json')
        cases = [case for case in manifest['cases'] if case['capability'] in ['pages', 'texture']]
        groups = gui.group_gui_cases(cases, {case['id'] for case in cases})
        self.assertEqual(len(cases), 28)
        self.assertEqual(len(groups), 21)
        processes = sum(2 if form in ['installed-disabled', 'config-restart'] else 1 for label, form in groups)
        self.assertEqual(processes, 27)
        self.assertTrue(all(len(values) == 2 for (label, form), values in groups.items() if label == 'multi'))
        self.assertEqual(sum(len(OPERATIONS[case['form']]) for values in groups.values() for case in values),
                         sum(len(OPERATIONS[case['form']]) for case in cases))


if __name__ == '__main__':
    unittest.main()
