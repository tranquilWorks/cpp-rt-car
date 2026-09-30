"""The bounded soak covers each required backend/executor and fault every cycle."""
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'samples/golden_audit'))
spec = importlib.util.spec_from_file_location('audit_plan_runner', ROOT / 'samples/golden_audit/run.py')
runner = importlib.util.module_from_spec(spec); spec.loader.exec_module(runner)


class SoakPlanTests(unittest.TestCase):
    def test_every_variant_and_fault_each_cycle(self):
        rows = runner.matrix(16)
        self.assertEqual(len(rows), 384)
        self.assertEqual(len({r['id'] for r in rows}), 384)
        expected = {(variant, mode) for variant in ('cpu', 'cuda-kernel', 'cuda-graph',
                    'xdma', 'combined-kernel', 'combined-graph') for mode in ('native', 'host', 'external')}
        for cycle in range(16):
            selected = [r for r in rows if r['cycle'] == cycle]
            nominal = [r for r in selected if r['fault'] == 'none' and r['campaign'] == 'nominal']
            self.assertEqual({(r['variant'], r['mode']) for r in nominal}, expected)
            faults = {r['fault'] if r['fault'] != 'none' else r['campaign'] for r in selected if r not in nominal}
            self.assertEqual(faults, {'underflow', 'overrun', 'device_loss', 'reset_failure', 'stop_failure', 'overload'})
        self.assertEqual({r['workers'] for r in rows}, {1, 2, 3})
        self.assertEqual({r['grain'] for r in rows}, {1, 4, 16, 64})

    def test_invalid_soak_bounds(self):
        for cycles in (0, 1, 33, True, 2.0, '16'):
            with self.subTest(cycles=cycles), self.assertRaisesRegex(ValueError, 'cycles must'):
                runner.matrix(cycles)


if __name__ == '__main__':
    unittest.main()
