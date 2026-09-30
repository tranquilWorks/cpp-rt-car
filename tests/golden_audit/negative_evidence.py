"""Reject edited journals and checksum-repaired false state after a real run."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--kit', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--manual', type=Path, required=True)
    parser.add_argument('--config', default='Release')
    args = parser.parse_args()
    sys.path.insert(0, str(args.kit))
    spec = importlib.util.spec_from_file_location('audited_runner', args.kit / 'run.py')
    run = importlib.util.module_from_spec(spec); spec.loader.exec_module(run)
    def verify():
        return run.verify(args.evidence, args.build, args.config,
                          args.kit / 'provenance.json', args.manual)
    verify()
    results = []
    def reject(name, path, change, diagnostic):
        original = path.read_bytes()
        try:
            path.write_bytes(change(original))
            try: verify()
            except ValueError as error:
                if diagnostic not in str(error): raise RuntimeError(f'{name}: unexpected rejection {error}')
                results.append(dict(name=name, rejected=str(error)))
                print('PASS negative', name, ':', error, flush=True)
            else: raise RuntimeError('false evidence accepted: ' + name)
        finally: path.write_bytes(original)
    def edit(fn):
        def mutate(data):
            value = json.loads(data); fn(value); return run.catalog.encoded(value)
        return mutate
    journal = args.evidence / 'execution.json'
    reject('omitted cycle', journal, edit(lambda v: v['records'].pop()), 'execution record')
    reject('duplicate cycle', journal, edit(lambda v: v['records'].__setitem__(2, v['records'][1])), 'execution record')
    reject('failed prefix', journal, edit(lambda v: v['records'][1].__setitem__('status', 'failed')), 'failed/incomplete')
    reject('incomplete publication', journal, edit(lambda v: v.__setitem__('completed', False)), 'incomplete execution')
    reject('changed source', journal, edit(lambda v: v['source']['audit'].__setitem__('source_commit', '0' * 40)), 'source/plan')
    reject('foreign executable', journal, edit(lambda v: v['binaries'].__setitem__('golden_xdma', '0' * 64)), 'executable bytes')
    reject('changed requested work', journal, edit(lambda v: v['plan'][0].__setitem__('ticks', 1)), 'source/plan')
    reject('false physical completion', args.evidence / 'soak.json', edit(lambda v: v.__setitem__('physical', 'PASS')), 'soak result')
    first = run.matrix(run.catalog.load(journal)['cycles'])[0]['id']
    execution = args.evidence / 'runs' / first / 'execution.json'
    reject('false backend counters', execution, edit(lambda v: v['phase_calls'].__setitem__(0, 0)), 'phase counts')
    def corrupt_state(data):
        value = bytearray(data)
        struct.pack_into('<i', value, 512, struct.unpack_from('<i', value, 512)[0] + 1)
        value[480:488] = bytes(8)
        struct.pack_into('<Q', value, 480, run.a.fnv(value))
        return bytes(value)
    reject('checksum repaired wrong state', args.evidence / 'runs' / first / 'state.bin',
           corrupt_state, 'every-field independent state oracle')
    raw = args.evidence / 'runs' / first / 'trusted.bin'
    def foreign_replay(data):
        value = bytearray(data)
        struct.pack_into('<Q', value, 32, struct.unpack_from('<Q', value, 32)[0] + 1)
        value[24:32] = bytes(8); struct.pack_into('<Q', value, 24, run.a.fnv(value))
        return bytes(value)
    reject('checksum repaired foreign replay', raw, foreign_replay, 'trusted runtime/transcript')
    verify()
    # Keep the negative report outside the strict successful evidence inventory.
    (args.evidence.parent / (args.evidence.name + '-negatives.json')).write_text(json.dumps(results, indent=2) + '\n')
    print('PASS', len(results), 'independent audit evidence negatives; originals restored')


if __name__ == '__main__':
    main()
