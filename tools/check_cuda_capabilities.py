#!/usr/bin/env python3
"""Version1 CUDA evidence coverage; physical rows remain in the denominator."""
import argparse
import hashlib
import importlib.util
import statistics
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Normative v1 inventory. Removing/weakening a row requires a reviewed version change.
CRITICAL = ('configuration module function context streams host_registration borrowed_memory h2d d2h d2d '
            'kernel graph timelines cross_rate saturation invalid_descriptor timeout cancellation late_completion '
            'reset context_loss pending_stop startup_rollback cleanup_retry repeated_lifetime isolation allocation '
            'hidden_synchronization stack_bound asan_ubsan tsan abi relocated_package embedded_package '
            'real_compile oracle replay_boundary').split()
NONCRITICAL = ['observability', 'benchmarks', 'optimization']
PHYSICAL = ['physical_correctness_recovery', 'physical_profiling_performance', 'physical_thermal', 'physical_endurance']
GATES = {'reference', 'pipeline', 'lifetime', 'cross_rate', 'sanitizers', 'abi', 'package', 'real_compile',
         'benchmark', 'optimization'}
CROSSWALK = {
    'transfer': ['rtfw.device:cuda-roundtrip-64', 'rtfw.device:cuda-device-copy-4096'],
    'launch': ['rtfw.device:cuda-kernel-64'],
    'graph': ['rtfw.device:cuda-graph-4096'],
    'overlap': ['rtfw.device:cuda-depth-4'],
    'multi_rate': ['rtfw.runtime:channel-fast-slow-256-c8', 'rtfw.runtime:channel-slow-fast-256-c8'],
    'recovery': ['rtfw.device:cuda-submit-failure-recovery', 'rtfw.device:cuda-cleanup-retry'],
    'end_to_end': ['rtfw.device:pipeline-kernel-4096-frames-4', 'rtfw.device:pipeline-graph-4096-frames-4'],
}

ROW_GATES = {name: ['lifetime'] for name in CRITICAL}
for name in ('configuration', 'module', 'function', 'context', 'streams', 'host_registration',
             'borrowed_memory', 'h2d', 'd2h', 'd2d', 'kernel', 'graph', 'timelines', 'oracle'):
    ROW_GATES[name] = ['pipeline']
ROW_GATES['oracle'] = ['reference', 'pipeline']
ROW_GATES['module'] = ['pipeline', 'real_compile']
ROW_GATES.update(cross_rate=['cross_rate'], stack_bound=['lifetime'], asan_ubsan=['sanitizers'],
                 tsan=['sanitizers'], abi=['abi'], relocated_package=['package'], embedded_package=['package'],
                 real_compile=['real_compile'], replay_boundary=['benchmark'], observability=['benchmark'],
                 benchmarks=['benchmark'], optimization=['optimization'])
GATE_SOURCES = {
    'reference': ['samples/cuda_physics/model.hpp', 'samples/cuda_physics/scenario.hpp', 'tests/test_cuda_physics.cpp'],
    'pipeline': ['samples/cuda_physics/pipeline/scenario.hpp', 'samples/cuda_physics/pipeline/simulated_driver.hpp', 'tests/test_cuda_pipeline.cpp'],
    'lifetime': ['samples/cuda_physics/lifetime/conformance.hpp', 'samples/cuda_physics/lifetime/protocol.hpp',
                 'tests/test_cuda_lifetime.cpp', 'tests/cuda_physics/test_lifetime_cli.py'],
    'cross_rate': ['tests/test_cross_rate_data.cpp', 'tests/test_mixed_rate_replay.cpp'],
    'sanitizers': ['.github/workflows/ci.yml', 'tests/test_cuda_benchmark.cpp'],
    'abi': ['abi/rtfw_c_abi_v8.sha256', 'abi/rtfw_c_abi_v8.exports'],
    'package': ['tests/cuda_physics/verify_package.py', 'samples/cuda_physics/CMakeLists.txt',
                'tests/package_consumer/package_contract.cmake'],
    'real_compile': ['samples/cuda_physics/real.cpp', 'samples/cuda_physics/pipeline/real.cpp', 'samples/cuda_physics/particle.cu'],
    'benchmark': ['samples/cuda_physics/benchmark/provider.cpp', 'samples/cuda_physics/benchmark/provider.hpp',
                  'samples/cuda_physics/benchmark/main.cpp', 'tests/test_cuda_benchmark.cpp', 'tests/cuda_physics/test_benchmark.py'],
    'optimization': ['tests/cuda_physics/test_benchmark.py', 'docs/evidence/M24-04-measurements/comparison.json'],
}


def product_sources(root):
    # Bind the implementation behind the sample, not just its tests/wrappers.
    return {str(path.relative_to(root)) for directory in ('rt', 'core/include')
            for path in (root/directory).rglob('*') if path.suffix in ('.cpp', '.hpp', '.h')}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_measurements(root):
    directory = root/'docs/evidence/M24-04-measurements'
    comparison = json.loads((directory/'comparison.json').read_text())
    require(comparison['evidence_class'] == 'portable_characterization' and comparison['pairs'] == 3,
            'measurement evidence class/count')
    for path, sha in comparison['source_sha256'].items():
        require(digest(root/path) == sha, 'measurement source drift')
    spec = importlib.util.spec_from_file_location('m24_artifact', root/'tools/check_benchmark_artifact.py')
    artifact = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(artifact)
    durations = {'kernel': [], 'graph': []}
    for pair in range(3):
        sums = {}
        for mode in durations:
            bundle = directory/f'{mode}-{pair}'
            summary = artifact.validate_bundle(bundle)
            require(summary['status'] == 'ok' and summary['measured_completed'] == 5, 'measurement not successful')
            samples = json.loads((bundle/'raw.json').read_text())['samples']
            durations[mode].extend(s['end_ns']-s['start_ns'] for s in samples)
            sums[mode] = [s['checksum'] for s in samples]
        require(sums['kernel'] == sums['graph'], 'measurement correctness parity')
    require(comparison['raw_ns'] == durations, 'measurement samples changed')
    require(comparison['median_ns'] == {mode: int(statistics.median(v)) for mode, v in durations.items()},
            'measurement median changed')
    require(comparison['median_entity_updates_per_second'] == {mode: 4096*1_000_000_000//ns for mode, ns in comparison['median_ns'].items()},
            'measurement throughput changed')


def validate(matrix, root=ROOT):
    require(set(matrix) == {'version', 'rows', 'gates', 'm23_crosswalk', 'product_sources'}, 'unknown/missing top-level field')
    require(type(matrix['version']) is int and matrix['version'] == 1, 'version')
    require(type(matrix['rows']) is list, 'rows')
    require(isinstance(matrix['product_sources'], dict) and set(matrix['product_sources']) == product_sources(root), 'product source inventory')
    for path, sha in matrix['product_sources'].items():
        require(digest(root/path) == sha, f'stale product implementation: {path}')
    ids = [r['id'] for r in matrix['rows']]
    require(len(ids) == len(set(ids)) and set(ids) == set(CRITICAL+NONCRITICAL+PHYSICAL), 'fixed v1 row inventory')
    require(set(matrix['gates']) == GATES, 'gate inventory')
    for name, gate in matrix['gates'].items():
        require(set(gate) == {'command', 'evidence', 'sources', 'status'}, 'gate fields')
        require(gate['status'] == 'passed' and isinstance(gate['command'], str) and gate['command'], 'unperformed/failed gate')
        require(isinstance(gate['sources'], dict) and set(GATE_SOURCES[name]) <= set(gate['sources']), 'missing required source binding')
        require(isinstance(gate['evidence'], dict) and gate['evidence'] and all(p.startswith('docs/evidence/') for p in gate['evidence']), 'missing retained evidence')
        for path, sha in {**gate['sources'], **gate['evidence']}.items():
            item = Path(path)
            require(not item.is_absolute() and '..' not in item.parts and not (root/item).is_symlink(), 'unsafe evidence path')
            require((root/item).is_file() and digest(root/item) == sha, f'stale/missing {name} binding: {path}')
    automated = critical = 0
    for row in matrix['rows']:
        require(set(row) == {'id', 'title', 'critical', 'state', 'gates'}, 'row fields')
        require(isinstance(row['title'], str) and row['title'], 'row title')
        require(type(row['critical']) is bool and row['critical'] == (row['id'] in CRITICAL), 'critical flag changed')
        require(isinstance(row['gates'], list) and len(row['gates']) == len(set(row['gates'])), 'duplicate gates')
        if row['id'] in PHYSICAL:
            require(row['state'] == 'M18_NOT_RUN' and row['gates'] == [], 'physical row cannot be automated')
        else:
            require(row['state'] == 'automated' and row['gates'] == ROW_GATES[row['id']], 'missing/unknown or substituted automated gate')
            automated += 1
            critical += row['critical']
    require(automated*100 >= len(ids)*90 and critical == len(CRITICAL), 'coverage threshold')
    require(matrix['m23_crosswalk'] == CROSSWALK, 'M23 crosswalk changed')
    inventory = {}
    for family in ('cpu', 'runtime', 'device'):
        data = json.loads((root/f'bench/fixtures/{family}_cases.json').read_text())
        inventory.update({f"{data['provider']}:{row['id']}": row for row in data['cases']})
    require(len(inventory)+1 == 207, 'original M23 inventory changed')
    require(all(case in inventory for cases in CROSSWALK.values() for case in cases), 'missing M23 case')
    validate_measurements(root)
    return {'version': 1, 'total_rows': len(ids), 'automated_rows': automated,
            'critical_rows': len(CRITICAL), 'automated_critical_rows': critical,
            'physical_not_run_rows': len(PHYSICAL), 'overall_percent': round(100*automated/len(ids), 2),
            'critical_percent': 100, 'evidence_class': 'portable-software-only'}


def render(matrix):
    report = validate(matrix)
    lines = ['# CUDA capability coverage (generated)', '',
             'Generated by `python3 tools/check_cuda_capabilities.py --write-doc`.', '',
             f"Automated: {report['automated_rows']}/{report['total_rows']} ({report['overall_percent']}%). "
             f"Critical: {report['automated_critical_rows']}/{report['critical_rows']} (100%).", '',
             'All four physical rows remain in the denominator and are M18 NOT RUN.',
             'Critical flags describe portable software correctness/ownership; physical qualification remains mandatory separately.',
             'Evidence bindings retain exact source and evidence SHA-256 values in `cuda_capabilities.json`.',
             'This report checks retained automated evidence; it does not itself execute the bound suites.', '',
             '| Capability | Critical | State | Gates |', '| --- | --- | --- | --- |']
    for row in matrix['rows']:
        lines.append(f"| {row['title']} | {'yes' if row['critical'] else 'no'} | {row['state']} | {', '.join(row['gates']) or 'M18 manual'} |")
    lines += ['', '## Existing M23 workload integration', '', '| Purpose | Preserved cases |', '| --- | --- |']
    for category, cases in CROSSWALK.items():
        lines.append(f"| {category} | {', '.join(cases)} |")
    lines += ['', 'Entity scaling and full particle latency are supplied by the 20 new source-kit descriptors.',
              'Active particle cases share one rate domain; heterogeneous rate cases above are the existing generic Runtime workloads.',
              'Overlap is observed in portable admitted queues; physical GPU overlap is unperformed.', '',
              'Physical correctness/recovery, profiling/performance, thermal and endurance require named M18 tuples.',
              'No production physics, RT qualification, controlled regression threshold or release promotion is implied.', '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--matrix', type=Path, default=ROOT/'docs/cuda_capabilities.json')
    parser.add_argument('--write-doc', action='store_true')
    args = parser.parse_args()
    try:
        matrix = json.loads(args.matrix.read_text())
        report = validate(matrix)
        document = ROOT/'docs/cuda_capabilities.md'
        expected = render(matrix)
        if args.write_doc:
            document.write_text(expected)
        else:
            require(document.read_text() == expected, 'generated documentation drift')
        print(json.dumps(report, sort_keys=True))
        return 0
    except (ValueError, KeyError, TypeError, OSError) as error:
        print(f'FAIL: {error}')
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
