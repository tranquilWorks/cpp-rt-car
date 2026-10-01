#!/usr/bin/env python3
"""Always-on parser/identity/clock negatives; no optional Python dependencies."""
import copy
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import unittest

MODULE, EXAMPLE = sys.argv[1:3]
sys.argv = sys.argv[:1]
spec = importlib.util.spec_from_file_location('telemetry_export', MODULE)
e = importlib.util.module_from_spec(spec); spec.loader.exec_module(e)
RAW = subprocess.check_output([EXAMPLE], timeout=30)
ROWS = [json.loads(line) for line in RAW.splitlines()]


def encode(rows):
    return ('\n'.join(json.dumps(s) for s in rows) + '\n').encode()


class SpoolTests(unittest.TestCase):
    def reject(self, data):
        with self.assertRaises((ValueError, RecursionError)):
            e.load_spool(io.BytesIO(data))

    def test_real_entrypoint(self):
        rows = e.load_spool(io.BytesIO(RAW))
        self.assertEqual(len(rows), 4)
        self.assertEqual(len({s['runtime_id'] for s in rows}), 2)
        self.assertEqual(sum(s['metrics']['samples'][0]['value'] for s in rows), 6)
        self.assertEqual(sum(len(s['trace']['events']) for s in rows), 30)

    def test_syntax_bounds(self):
        for data in (b'', RAW[:-1], RAW.replace(b'"spool_schema":1', b'"spool_schema":1,"spool_schema":1', 1),
                     b'[' * 1100 + b']' * 1100 + b'\n', b' ' * (e.MAX_INPUT + 1),
                     RAW.replace(b'"uncertainty_ns":0', b'"uncertainty_ns":NaN', 1)):
            with self.subTest(size=len(data)):
                self.reject(data)
        self.reject(encode([ROWS[0]] * 65))

    def test_record_mutations(self):
        mutations = [lambda s: s.update(schema_version=3), lambda s: s.update(runtime_id=True),
                     lambda s: s.update(session='bad identity'), lambda s: s.update(extra=1),
                     lambda s: s.update(unix_anchor_ns=e.U64),
                     lambda s: s['metrics'].update(window='cumulative'),
                     lambda s: s['metrics']['samples'][0].update(value=-1),
                     lambda s: s['metrics']['samples'][0].update(name='forged'),
                     lambda s: s['metrics']['samples'][0].update(kind=1),
                     lambda s: s['trace']['events'][0].update(type=15),
                     lambda s: s['trace']['events'][0].update(status=1),
                     lambda s: s['trace']['events'][0].update(callback=1 << 32),
                     lambda s: s['trace']['events'].append(s['trace']['events'][0]),
                     lambda s: s['trace'].update(events=s['trace']['events'] * 257)]
        for mutation in mutations:
            rows = copy.deepcopy(ROWS); mutation(rows[0])
            with self.subTest(mutation=mutation): self.reject(encode(rows))

    def test_identity_and_replay(self):
        for key, value in [('batch_sequence', 1), ('batch_sequence', 3), ('config_id', 42),
                           ('runtime_anchor_ns', 500), ('queue_full', -1)]:
            rows = copy.deepcopy(ROWS); rows[1][key] = value
            with self.subTest(key=key): self.reject(encode(rows))
        rows = copy.deepcopy(ROWS); rows[1]['metrics']['start_ns'] += 1
        self.reject(encode(rows))
        self.reject(encode([ROWS[0], ROWS[0]]))

    def test_native_integer_limits(self):
        rows = copy.deepcopy(ROWS); rows[0]['metrics']['samples'][0]['value'] = e.U64
        self.assertEqual(e.load_spool(io.BytesIO(encode(rows)))[0]['metrics']['samples'][0]['value'], e.U64)
        with self.assertRaises(ValueError): e.native_bounds(rows)
        c = dict(runtime_anchor_ns=100, unix_anchor_ns=1)
        with self.assertRaises(ValueError): e.timestamp(c, 0)
        c = dict(runtime_anchor_ns=1, unix_anchor_ns=e.U64)
        with self.assertRaises(ValueError): e.timestamp(c, 2)

    def test_loss_conservation(self):
        rows = copy.deepcopy(ROWS)
        rows[1]['trace']['events'].pop(0)
        self.reject(encode(rows))
        rows[1]['trace']['lost_events'] += 1
        self.assertEqual(len(e.load_spool(io.BytesIO(encode(rows)))), 4)
        rows[1]['trace']['lost_events'] += 1
        self.reject(encode(rows))
        rows = copy.deepcopy(ROWS)
        rows[0]['trace']['lost_events'] = 1
        self.reject(encode(rows))
        rows = copy.deepcopy(ROWS)
        rows[1]['trace_capacity'] += 1
        self.reject(encode(rows))
        rows = copy.deepcopy(ROWS)
        rows[0]['trace_capacity'] = 0
        self.reject(encode(rows))


if __name__ == '__main__': unittest.main()
