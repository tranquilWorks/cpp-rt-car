#!/usr/bin/env python3
"""Negative controls for retained coverage, inventory and evidence binding."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import shutil
import tempfile
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('coverage', ROOT/'tools/check_cuda_capabilities.py')
coverage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coverage)


class CoverageTests(unittest.TestCase):
    def setUp(self):
        self.matrix = json.loads((ROOT/'docs/cuda_capabilities.json').read_text())

    def reject(self, mutate):
        candidate = copy.deepcopy(self.matrix)
        mutate(candidate)
        with self.assertRaises((ValueError, KeyError, TypeError)):
            coverage.validate(candidate)

    def test_complete_report_includes_physical_denominator(self):
        report = coverage.validate(self.matrix)
        self.assertEqual(report['automated_rows'], 40)
        self.assertEqual(report['total_rows'], 44)
        self.assertEqual(report['critical_rows'], 37)
        self.assertEqual(report['physical_not_run_rows'], 4)
        self.assertGreaterEqual(report['overall_percent'], 90)

    def test_removed_duplicate_and_extra_rows(self):
        self.reject(lambda m: m['rows'].pop())
        self.reject(lambda m: m['rows'].append(copy.deepcopy(m['rows'][0])))
        self.reject(lambda m: m['rows'][0].update(id='invented-capability'))

    def test_critical_state_and_gate_substitution(self):
        self.reject(lambda m: m['rows'][0].update(critical=False))
        self.reject(lambda m: m['rows'][0].update(critical=1))
        self.reject(lambda m: m['rows'][0].update(state='unverified'))
        self.reject(lambda m: m['rows'][0].update(gates=['unknown']))
        self.reject(lambda m: m['rows'][0].update(gates=['abi']))
        self.reject(lambda m: m['rows'][0].update(gates=[]))

    def test_physical_cannot_be_promoted_or_excluded(self):
        self.reject(lambda m: m['rows'][-1].update(state='automated', gates=['pipeline']))
        self.reject(lambda m: m['rows'][-1].update(state='not-applicable'))

    def test_failed_or_missing_evidence(self):
        self.reject(lambda m: m['gates']['benchmark'].update(status='failed'))
        self.reject(lambda m: m['gates']['benchmark'].update(status='unperformed'))
        self.reject(lambda m: m['gates']['benchmark'].update(evidence={}))
        self.reject(lambda m: m['gates']['benchmark'].update(sources={}))
        self.reject(lambda m: m['gates'].pop('benchmark'))
        path = next(iter(self.matrix['gates']['benchmark']['sources']))
        self.reject(lambda m: m['gates']['benchmark']['sources'].update({path: '0'*64}))
        self.reject(lambda m: m['gates']['benchmark']['evidence'].update({'docs/evidence/absent.txt': '0'*64}))
        self.reject(lambda m: m['gates']['benchmark']['sources'].update({'../outside': '0'*64}))

    def test_crosswalk_and_schema_drift(self):
        self.reject(lambda m: m.update(version=2))
        self.reject(lambda m: m.update(extra=True))
        self.reject(lambda m: m['m23_crosswalk'].update(overlap=['fabricated']))

    def test_measured_optimization_negative_controls(self):
        directory = 'docs/evidence/M24-04-measurements'
        comparison = json.loads((ROOT/directory/'comparison.json').read_text())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            files = list(comparison['source_sha256']) + ['tools/check_benchmark_artifact.py',
                'bench/schemas/result.schema.json', 'bench/schemas/descriptor.schema.json']
            for name in files:
                (root/name).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT/name, root/name)
            shutil.copytree(ROOT/directory, root/directory)
            coverage.validate_measurements(root)
            comparison['median_ns']['kernel'] += 1
            (root/directory/'comparison.json').write_text(json.dumps(comparison))
            with self.assertRaises(ValueError):
                coverage.validate_measurements(root)
            shutil.copy2(ROOT/directory/'comparison.json', root/directory/'comparison.json')
            raw = root/directory/'kernel-0/raw.json'
            value = json.loads(raw.read_text())
            value['samples'][0]['checksum'] += 1
            raw.write_text(json.dumps(value))
            with self.assertRaises(ValueError):
                coverage.validate_measurements(root)

    def test_generated_document_exact_and_actual_cli(self):
        self.assertEqual(coverage.render(self.matrix), (ROOT/'docs/cuda_capabilities.md').read_text())
        result = subprocess.run([sys.executable, str(ROOT/'tools/check_cuda_capabilities.py')],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertEqual(json.loads(result.stdout)['total_rows'], 44)


if __name__ == '__main__':
    unittest.main()
