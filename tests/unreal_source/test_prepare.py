import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('unreal_prepare', ROOT / 'integrations/unreal/prepare.py')
PREPARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREPARE)


class PrepareTests(unittest.TestCase):
    def report(self):
        return {'succeeded': 2, 'succeededWithWarnings': 0, 'failed': 0,
                'notRun': 0, 'inProcess': 0,
                'tests': [{'fullTestPath': name, 'state': 'Success'} for name in ['first', 'second']]}

    def test_complete_report(self):
        self.assertEqual(PREPARE.validate_report(self.report(), ['first', 'second']), 2)

    def test_failed_report(self):
        report = self.report(); report['failed'] = 1
        with self.assertRaises(ValueError): PREPARE.validate_report(report, ['first', 'second'])

    def test_success_counter_cannot_hide_failed_state(self):
        report = self.report(); report['tests'][1]['state'] = 'Fail'
        with self.assertRaises(ValueError): PREPARE.validate_report(report, ['first', 'second'])

    def test_missing_or_duplicate_tests(self):
        for tests in [[], self.report()['tests'][:1], [self.report()['tests'][0]] * 2]:
            report = self.report(); report['tests'] = tests
            with self.assertRaises(ValueError): PREPARE.validate_report(report, ['first', 'second'])

    def test_boolean_is_not_a_complete_success_counter(self):
        report = self.report(); report['succeeded'] = True
        with self.assertRaises(ValueError): PREPARE.validate_report(report, ['first', 'second'])

    def test_no_warning_or_unfinished_result_promotion(self):
        for field in ['succeededWithWarnings', 'notRun', 'inProcess']:
            report = self.report(); report[field] = 1
            with self.assertRaises(ValueError): PREPARE.validate_report(report, ['first', 'second'])

    def test_original_adapter_inventory_retained(self):
        names = PREPARE.expected_tests(ROOT / 'integrations/unreal/RTFWUnreal')
        self.assertEqual(len([name for name in names if name.startswith('RTFW.M19_02.')]), 10)
        self.assertEqual(len([name for name in names if name.startswith('RTFW.M19_03.')]), 5)

    def test_incomplete_sdk_does_not_create_output(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory); out = base / 'stage'
            with self.assertRaises(ValueError): PREPARE.stage(base / 'missing-sdk', out)
            self.assertFalse(out.exists())

    def test_existing_output_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory); out = base / 'stage'; out.mkdir()
            marker = out / 'keep'; marker.write_text('prior evidence')
            with self.assertRaises(ValueError): PREPARE.stage(base / 'sdk', out)
            self.assertEqual(marker.read_text(), 'prior evidence')

    def test_staging_is_only_source_preparation(self):
        # Synthetic SDK bytes test filesystem/manifest behavior only. A separate
        # installed-package run uses the actual compiled Runtime archive.
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory); sdk = base / 'sdk'
            for name in ['include/rt/runtime.hpp', 'lib/librtfw_runtime.a',
                         'share/rtfw/integrations/host_lifecycle/lifecycle.hpp',
                         'share/rtfw/integrations/unreal/portable/rtfw_unreal/worlds.hpp',
                         'share/rtfw/integrations/unreal/portable/rtfw_unreal/sample.hpp']:
                path = sdk / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text('synthetic staging fixture')
            out = base / 'stage'
            result = PREPARE.stage(sdk, out)
            self.assertEqual(result['engine_validation'], 'NOT_RUN')
            self.assertEqual(len(result['expected_tests']), 15)
            self.assertEqual(result, json.loads((out / 'source-manifest.json').read_text()))
            self.assertTrue((out / 'project/Plugins/RTFWUnreal/Source/RTFWUnreal/Private/RTFWUnrealWorldHost.cpp').is_file())
            PREPARE.verify_stage(out, result)
            (out / 'project/RTFWUnrealTestHost.uproject').write_text('concurrent source modification')
            with self.assertRaises(ValueError): PREPARE.verify_stage(out, result)
            # Staging also runs in the existing Windows portable SDK matrix;
            # accepting its archive for source preparation is not a UE port.
            (sdk / 'lib/librtfw_runtime.a').rename(sdk / 'lib/rtfw_runtime.lib')
            windows_result = PREPARE.stage(sdk, base / 'windows-stage')
            self.assertEqual(windows_result['runtime_archive'], 'lib/rtfw_runtime.lib')

    def test_missing_engine_runner_refuses_without_creating_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'run'
            result = subprocess.run([sys.executable, str(ROOT / 'integrations/unreal/run.py'),
                '--engine-root', str(out / 'missing'), '--sdk-root', str(out / 'sdk'), '--output', str(out)],
                capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(out.exists())


if __name__ == '__main__': unittest.main()
