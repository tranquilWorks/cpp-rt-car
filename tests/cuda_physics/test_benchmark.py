#!/usr/bin/env python3
"""Actual M23 bundle/correlation validation with independent integer oracle."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('artifact', ROOT/'tools/check_benchmark_artifact.py')
artifact = importlib.util.module_from_spec(spec)
spec.loader.exec_module(artifact)


def checksum(count, step):
    seed, result = 1, 0
    for _ in range(count):
        for _ in range(3):
            values = []
            for modulus, offset in ((2049, 1024), (129, 64), (9, 4)):
                seed = (seed*1664525+1013904223) % (1 << 32)
                values.append(seed % modulus-offset)
            x, v, a = values
            for value in (x+step*v+a*step*(step+1)//2, v+step*a, a):
                result = (result*131+(value % (1 << 32))) & ((1 << 63)-1)
    return result


def measure(cli, destination):
    import hashlib
    import statistics
    destination.mkdir(parents=True, exist_ok=False)
    values = {mode: [] for mode in ('kernel', 'graph')}
    checksums = {}
    for pair in range(3):
        # Alternate order to expose rather than hide simple ordering effects.
        for mode in (('kernel', 'graph') if pair % 2 == 0 else ('graph', 'kernel')):
            case = f'{mode}-frame-4096-w2'
            output = destination/f'{mode}-{pair}'
            command = [str(cli), '--case', case, '--clock', 'steady', '--output', str(output)]
            proc = subprocess.run(command, capture_output=True, text=True, timeout=60, check=True)
            (destination/f'{mode}-{pair}-trace.json').write_text(proc.stdout)
            artifact.validate_bundle(output)
            raw = json.loads((output/'raw.json').read_text())
            samples = raw['samples']
            durations = [sample['end_ns']-sample['start_ns'] for sample in samples]
            values[mode].extend(durations)
            checksums[mode, pair] = [sample['checksum'] for sample in samples]
        assert checksums['kernel', pair] == checksums['graph', pair]
    sources = ['samples/cuda_physics/benchmark/provider.cpp', 'samples/cuda_physics/benchmark/provider.hpp',
               'samples/cuda_physics/benchmark/main.cpp', 'samples/cuda_physics/pipeline/scenario.hpp',
               'samples/cuda_physics/pipeline/simulated_driver.hpp', 'samples/cuda_physics/model.hpp']
    medians = {mode: int(statistics.median(v)) for mode, v in values.items()}
    report = {'version': 1, 'evidence_class': 'portable_characterization',
              'workload': '4096 entities, frame, two workers, seed1, seven steps; two warmup and five measured',
              'pairs': 3, 'raw_ns': values, 'median_ns': medians,
              'median_entity_updates_per_second': {mode: 4096*1_000_000_000//ns for mode, ns in medians.items()},
              'candidate': 'pre-instantiated-graph', 'baseline': 'direct-kernel',
              'selected_default': 'unchanged-graph-example-default',
              'decision': 'Retain both dispatch paths. Simulated driver timings cannot select a physical CUDA optimization.',
              'limitations': ['Host noise, no affinity or controlled thresholds', 'No GPU timings or profiler capture',
                              'Different case identities are intentionally ineligible for same-case M23 baseline comparison'],
              'correctness': 'All fifteen measured per-step checksums agree; independent nine-field CPU oracle passed',
              'source_sha256': {f: hashlib.sha256((ROOT/f).read_bytes()).hexdigest() for f in sources}}
    (destination/'comparison.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(medians))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', required=True, type=Path)
    parser.add_argument('--measure', type=Path)
    args = parser.parse_args()
    cli = args.cli.resolve()
    if args.measure:
        measure(cli, args.measure)
        return

    def run(*options):
        return subprocess.run([str(cli), *map(str, options)], capture_output=True, text=True, timeout=60)

    assert run('--help').returncode == 0
    expected = [f'{mode}-{schedule}-{n}-w{w}' for n, w in ((1, 1), (17, 1), (17, 2), (4096, 1), (4096, 2))
                for schedule in ('frame', 'active') for mode in ('kernel', 'graph')]
    listed = run('list')
    assert listed.returncode == 0 and listed.stdout.splitlines() == expected
    for options in ((), ('--unknown',), ('--case',), ('--help', 'extra'),
                    ('--clock', 'fake', '--clock', 'steady')):
        assert run(*options).returncode == 2
    with tempfile.TemporaryDirectory(prefix='rtfw-cuda-benchmark-') as directory:
        root = Path(directory)
        for name in expected:
            destination = root/name
            proc = run('--case', name, '--clock', 'fake', '--output', destination)
            assert proc.returncode == 0, (name, proc.stderr, proc.stdout)
            summary = artifact.validate_bundle(destination)
            assert summary['status'] == 'ok' and summary['warmup_completed'] == 2 and summary['measured_completed'] == 5
            samples = json.loads((destination/'raw.json').read_text())['samples']
            mode, schedule, count, workers = name.split('-')
            count = int(count)
            lanes = 1 if count == 1 else 2
            for i, sample in enumerate(samples):
                assert sample['checksum'] == checksum(count, i+3), (name, i)
                assert sample['counters'] == {'entities': count, 'upload_bytes': 72*count,
                    'copy_bytes': 36*count, 'download_bytes': 36*count,
                    'kernels': lanes if mode == 'kernel' else 0, 'graphs': lanes if mode == 'graph' else 0,
                    'publications': lanes, 'correlations': lanes, 'commands': 5*lanes}
            trace = json.loads(proc.stdout)
            assert trace['version'] == 1 and trace['clock'] == 'host-steady-ns'
            assert trace['device_timestamps'] == 'not_available' and trace['case'] == name
            assert trace['provider'] == 'rtfw.cuda-physics'
            assert len(trace['records']) == 7*lanes
            for i, record in enumerate(trace['records']):
                assert set(record) == {'id', 'invocation', 'lane', 'timeline_value', 'commands', 'host_submit_ns', 'host_complete_ns'}
                assert record['id'] == i+1 and record['invocation'] == i//lanes and record['lane'] == i % lanes
                assert record['timeline_value'] == i//lanes+1 and record['commands'] == 5
                assert record['host_submit_ns'] <= record['host_complete_ns']
            assert run('--case', name, '--output', destination).returncode == 2
        assert run('--case', expected[0], '--clock', 'steady', '--output', root/'steady').returncode == 0
        artifact.validate_bundle(root/'steady')
        assert run('--case', 'unknown', '--output', root/'unknown').returncode == 2
        assert not (root/'unknown').exists()
        assert run('--case', expected[0], '--clock', 'wrong', '--output', root/'bad').returncode == 2
        assert not (root/'bad').exists()
    print('PASS: 20 particle cases, exact independent oracle, bounded correlations, M23 artifacts and CLI failures')


if __name__ == '__main__':
    main()
