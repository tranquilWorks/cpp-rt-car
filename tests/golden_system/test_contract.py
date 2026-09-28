"""Behavioral contract mutations; no golden Runtime implementation in M26-01."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('golden', ROOT/'tools/check_golden_contract.py')
golden = importlib.util.module_from_spec(spec)
spec.loader.exec_module(golden)


class ContractTests(unittest.TestCase):
    def setUp(self):
        self.data = golden.read()

    def reject(self, change, message):
        value = copy.deepcopy(self.data)
        change(value)
        # Bypass only the final freeze hash so each semantic guard is exercised.
        with self.assertRaisesRegex(ValueError, message):
            golden.validate(value, frozen=False)

    def test_frozen_and_independent_oracles(self):
        result = golden.validate(self.data)
        self.assertEqual(result['reference_records'], 27)
        self.assertEqual(result['default_phase_calls'], 108)
        self.assertEqual(result['maximum_position_abs'], 2165760)
        self.assertEqual(result['maximum_velocity_abs'], 4160)
        self.assertEqual(result['maximum_nested_tasks'], 771)
        self.assertEqual(result['maximum_channel_age_ticks'], {'plant_sensor':0,'sensor_controller':1,'controller_actuator':0,'actuator_input':3,'plant_aggregate':0})
        self.assertFalse(result['scenario_executed'])

    def test_missing_unknown_nested_and_root(self):
        self.reject(lambda d:d.pop('faults'), 'missing/unknown')
        self.reject(lambda d:d.update(extra='x'), 'missing/unknown')
        self.reject(lambda d:d['channels'][0].update(extra='x'), 'missing/unknown')

    def test_wrong_scalar_and_boolean_integer(self):
        self.reject(lambda d:d['state'].update(capacity=True), 'wrong scalar')
        self.reject(lambda d:d['clock'].update(default_ticks=24.0), 'wrong scalar')

    def test_duplicate_identity(self):
        self.reject(lambda d:d['channels'][1].update(identity=d['channels'][0]['identity']), 'duplicate')
        self.reject(lambda d:d['phases'].append(d['phases'][0]), 'duplicate')

    def test_dangling_and_cyclic_graph(self):
        self.reject(lambda d:d['edges'].append(['absent','physics']), 'dangling')
        self.reject(lambda d:d['edges'].append(['stage','input']), 'cyclic')

    def test_unordered_resource_and_cross_domain_dependency(self):
        self.reject(lambda d:d['edges'].remove(['physics','stage']), 'unordered resource')
        self.reject(lambda d:d['edges'].append(['stage','sensor']), 'cross-domain')

    def test_rates_counts_and_horizon(self):
        self.reject(lambda d:d['rates'][0].update(period_ticks=0), 'rate/budget')
        self.reject(lambda d:d['rates'][0].update(default_releases=23), 'release count')
        self.reject(lambda d:d['clock'].update(supercycle_ticks=7), 'supercycle')
        self.reject(lambda d:d['phases'][0].update(default_calls=23), 'phase count')

    def test_state_capacity_and_arithmetic(self):
        self.reject(lambda d:d['state'].update(capacity=257), 'entity capacity')
        self.reject(lambda d:d['state']['fields'][0].update(maximum_abs=2**31), 'field bounds')
        self.reject(lambda d:d['state']['fields'][0].update(maximum_abs=2165759), 'arithmetic envelope')
        self.reject(lambda d:d['state'].update(plant_bytes=1), 'SoA bytes')
        self.reject(lambda d:d['state']['fields'][1].update(offset_bytes=0), 'SoA field offsets')

    def test_channel_geometry_age_encoding(self):
        self.reject(lambda d:d['channels'][0].update(capacity_payload_bytes=1), 'geometry')
        self.reject(lambda d:d['channels'][3].update(max_age_ticks=2), 'maximum age')
        self.reject(lambda d:d['channels'][0].update(scale=[1,0]), 'encoding/scale')
        self.reject(lambda d:d['channels'][0].update(ring_slots=1), 'ring capacity')
        self.reject(lambda d:d['channels'][0].update(device_timeout_ns=0), 'clock/timeout')

    def test_same_domain_and_device_device_channel(self):
        self.reject(lambda d:d['channels'][0].update(consumer='physics'), 'cross-rate endpoints')
        self.reject(lambda d:d['variants'][3]['device_phases'].append('stage'), 'device/device')

    def test_bounded_nested_jobs_and_cleanup(self):
        self.reject(lambda d:d['execution'].update(task_slots=770), 'nested capacity')
        self.reject(lambda d:d['execution'].update(cleanup_attempts=1000), 'unbounded cleanup')

    def test_control_schema_bounds_and_payload(self):
        self.reject(lambda d:d['controls']['schemas'][0]['fields'][0].update(default=65), 'control bounds')
        self.reject(lambda d:d['controls'].update(payload_stride=32), 'payload geometry')
        self.reject(lambda d:d['controls']['schemas'][1]['fields'][0].update(maximum=5), 'unsafe controller')
        self.reject(lambda d:d['controls']['schemas'][0]['fields'][0].update(offset=2), 'field offset')

    def test_control_target_must_exist_in_first_cycle(self):
        self.reject(lambda d:d['controls']['boundaries'][1].update(tick=6), 'first-cycle')
        self.reject(lambda d:d['controls']['boundaries'][1].update(tick=2), 'first-cycle')
        self.reject(lambda d:d['controls']['boundaries'][0].update(kind='missing'), 'boundary reference')

    def test_fault_inventory_detection_and_claim(self):
        self.reject(lambda d:d['faults'].pop(), 'fault coverage')
        self.reject(lambda d:d['faults'][0].update(detection_releases=0), 'fault bound')
        self.reject(lambda d:d['faults'][0].update(status='passed'), 'fault bound/claim')

    def test_benchmark_inventory_and_claim(self):
        self.reject(lambda d:d['benchmarks']['cases'].pop(), 'subsystem benchmark')
        self.reject(lambda d:d['benchmarks'].update(retain_raw=False), 'retention')
        self.reject(lambda d:d['benchmarks']['cases'][0].update(status='passed'), 'benchmark claim')

    def test_levers_and_telemetry_coverage(self):
        self.reject(lambda d:d['levers'].pop(), 'lever coverage')
        self.reject(lambda d:d['telemetry'].update(global_schema=3), 'telemetry schema')
        self.reject(lambda d:d['telemetry']['fields'].remove('gap_count'), 'observability')

    def test_artifact_shape_and_success_overclaim(self):
        self.reject(lambda d:d['artifacts'][0]['fields'].remove('oracle'), 'artifact claim/shape')
        self.reject(lambda d:d['artifacts'][0].update(status='passed'), 'artifact claim')
        self.reject(lambda d:d['artifacts'][0].update(path='../run.json'), 'artifact claim/shape/path')

    def test_variant_and_global_claims(self):
        self.reject(lambda d:d['variants'][5].update(status='passed'), 'variant execution')
        self.reject(lambda d:d.update(status='implemented'), 'implementation overclaim')
        self.reject(lambda d:d['nonclaims'].remove('hardware_qualification'), 'claim boundary')

    def test_source_link_drift_and_escape(self):
        self.reject(lambda d:d['sources'][0]['tokens'].append('nonexistent_m26_api'), 'source link drift')
        self.reject(lambda d:d['sources'][0].update(path='absent.hpp'), 'missing source')
        self.reject(lambda d:d['sources'][0].update(path='../AGENTS.md'), 'path escape')

    def test_frozen_prose_is_also_identity(self):
        self.data['state']['oracle'] += ' changed semantics'
        with self.assertRaisesRegex(ValueError, 'frozen contract identity'):
            golden.validate(self.data)

    def test_duplicate_json_key(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'bad.json'; path.write_text('{"version":1,"version":2}', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'duplicate JSON key'):
                golden.read(path)

    def test_cli_accepts_and_rejects_without_traceback(self):
        command = [sys.executable,str(ROOT/'tools/check_golden_contract.py')]
        run = subprocess.run(command, capture_output=True,text=True)
        self.assertEqual(run.returncode,0,run.stderr)
        self.assertIn('no Runtime scenario execution',run.stdout)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'bad.json'; path.write_text('{',encoding='utf-8')
            run = subprocess.run(command+['--contract',str(path)],capture_output=True,text=True)
            self.assertEqual(run.returncode,1)
            self.assertNotIn('Traceback',run.stderr)
            self.assertIn('FAIL: golden contract',run.stderr)


if __name__ == '__main__':
    unittest.main()
