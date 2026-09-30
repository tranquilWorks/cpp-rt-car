#!/usr/bin/env python3
"""Build/run or independently revalidate the installed golden-system audit."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time
import traceback
import catalog

HERE = Path(__file__).resolve().parent
SHOWCASE = HERE.parent / 'golden_showcase'
sys.path.insert(0, str(SHOWCASE))
import artifacts as a
import run as showcase
import report

NAMES = ('golden_showcase', 'golden_experiment', 'golden_telemetry_loss',
         'golden_system', 'golden_cuda', 'golden_xdma', 'golden_controller')


def matrix(cycles):
    catalog.require(type(cycles) is int and 2 <= cycles <= 32, 'cycles must be 2..32')
    result = []
    variants = [('cpu', 'golden_system', None), ('cuda-kernel', 'golden_cuda', 'kernel'),
                ('cuda-graph', 'golden_cuda', 'graph'), ('xdma', 'golden_xdma', 'cpu'),
                ('combined-kernel', 'golden_xdma', 'kernel'), ('combined-graph', 'golden_xdma', 'graph')]
    for cycle in range(cycles):
        for variant, executable, dispatch in variants:
            for mode in ('native', 'host', 'external'):
                result.append(dict(id=f'{cycle:02d}-{variant}-{mode}', cycle=cycle,
                    variant=variant, executable=executable, dispatch=dispatch, mode=mode,
                    campaign='nominal', fault='none', workers=1 + cycle % 3,
                    grain=(1, 4, 16, 64)[cycle % 4], count=16, ticks=24))
        for fault in ('underflow', 'overrun', 'device_loss', 'reset_failure', 'stop_failure'):
            result.append(dict(id=f'{cycle:02d}-fault-{fault}', cycle=cycle,
                variant='combined-graph', executable='golden_xdma', dispatch='graph',
                mode='host' if cycle % 2 else 'native', campaign='nominal', fault=fault,
                workers=2, grain=4, count=16, ticks=24))
        result.append(dict(id=f'{cycle:02d}-fault-overload', cycle=cycle, variant='cpu',
            executable='golden_system', dispatch=None, mode='native', campaign='overload',
            fault='none', workers=2, grain=4, count=16, ticks=24))
    return result


def file_hashes(directory):
    paths = list(directory.rglob('*'))
    catalog.require(not directory.is_symlink() and not any(p.is_symlink() for p in paths),
                    'symlink evidence is not admitted')
    return {p.relative_to(directory).as_posix(): catalog.sha(p.read_bytes())
            for p in sorted(paths) if p.is_file()}


def source_binding(provenance, manual):
    audit = catalog.load(provenance)
    catalog.require(set(audit) == {'schema', 'source_commit', 'source_tree', 'dirty', 'files'}
                    and audit['schema'] == 1 and type(audit['dirty']) is bool, 'audit provenance schema')
    actual = {p.name: catalog.sha(p.read_bytes()) for p in HERE.iterdir()
              if p.is_file() and p.name != 'provenance.json'}
    catalog.require(audit['files'] == actual, 'audit runner source inventory/digests')
    shared = a.provenance(SHOWCASE / 'provenance.json')
    catalog.require(all(audit[k] == shared[k] for k in ('source_commit', 'source_tree', 'dirty')),
                    'audit/showcase source identity mismatch')
    inventory = catalog.verify_bundle(manual)
    for path, digest in inventory['sources'].items():
        parts = Path(path).parts
        if len(parts) >= 3 and parts[0] == 'samples' and parts[1] in a.FOLDERS:
            catalog.require(catalog.sha((HERE.parent / Path(*parts[1:])).read_bytes()) == digest,
                            'manual and installed sample source differ: ' + path)
    return dict(audit=audit, shared=shared,
                manual_sha256=catalog.sha((manual / 'inventory.json').read_bytes()))


def inspect(path, row):
    if row['executable'] == 'golden_system':
        value = a.cpu.inspect(path)
    else:
        variant = 'sim_cuda' if row['executable'] == 'golden_cuda' else 'sim_xdma' if row['dispatch'] == 'cpu' else 'sim_combined'
        value = a.variant_inspect(path, variant)
    for key in ('count', 'ticks', 'workers', 'grain', 'campaign'):
        catalog.require(value[key] == row[key], 'soak configuration differs: ' + key)
    catalog.require(value['mode'] == ('native' if row['mode'] == 'external' else row['mode'])
                    and value['external'] == (row['mode'] == 'external'), 'soak executor/peer selection')
    catalog.require(value['peer_status'] == value['peer_cleanup'] == 'ok', 'failed peer lifecycle')
    if row['mode'] == 'external':
        catalog.require(value['peer_responses'] == 8, 'actual controller response count')
    if row['executable'] != 'golden_system':
        catalog.require(value['dispatch'] == row['dispatch'] and value['fault'] == row['fault'],
                        'soak dispatch/fault differs')
    return value


def verify(directory, build, configuration, provenance, manual, publish=False):
    source = source_binding(provenance, manual)
    execution = catalog.load(directory / 'execution.json')
    expected = matrix(execution['cycles'])
    catalog.require(execution['schema'] == 1 and execution['source'] == source and
                    execution['plan'] == expected, 'execution source/plan identity')
    binaries = {n: showcase.executable(build, n, configuration) for n in NAMES}
    binary_hashes = {n: catalog.sha(p.read_bytes()) for n, p in binaries.items()}
    catalog.require(execution['binaries'] == binary_hashes, 'changed executable bytes')
    catalog.require(execution['clock'] in ('fake', 'steady') and execution['completed'] is True,
                    'incomplete execution')
    records = execution['records']
    catalog.require([r['id'] for r in records] == ['showcase'] + [r['id'] for r in expected],
                    'missing/duplicate/reordered execution record')
    catalog.require(all(r['status'] == 'completed' and type(r['elapsed_ns']) is int and r['elapsed_ns'] > 0
                        for r in records), 'failed/incomplete execution record')
    top = {'execution.json', 'showcase', 'runs', 'logs'} | ({'soak.json'} if not publish else set())
    catalog.require({p.name for p in directory.iterdir()} == top, 'unexpected or missing audit artifact')
    catalog.require({p.name for p in (directory / 'runs').iterdir()} == {r['id'] for r in expected},
                    'omitted/extra soak run')
    catalog.require({p.name for p in (directory / 'logs').iterdir()} == {r['id'] + '.log' for r in records},
                    'missing/extra execution log')
    coverage = report.validate(directory / 'showcase', SHOWCASE / 'provenance.json', binaries=binaries)
    catalog.require(catalog.load(directory / 'showcase/inputs.json')['clock'] == execution['clock'],
                    'showcase clock mismatch')
    observations = []
    for row in expected:
        path = directory / 'runs' / row['id']
        value = inspect(path, row)
        observations.append(dict(id=row['id'], observation=value, files=file_hashes(path)))
    raw = {k: v for k, v in file_hashes(directory).items() if k != 'soak.json'}
    result = dict(schema=1, batch='M26-06', status='PASS', source=source, binaries=binary_hashes,
                  cycles=execution['cycles'], nominal_runs=18 * execution['cycles'],
                  fault_runs=6 * execution['cycles'], nominal_logical_ticks=18 * 24 * execution['cycles'],
                  wall_elapsed_ns=execution['wall_elapsed_ns'], showcase=coverage,
                  observations=observations, files=raw,
                  claim='bounded portable regression soak; not physical or long-duration endurance',
                  physical='NOT_RUN', rt='NOT_RUN', human_acceptance='NOT_RUN',
                  controlled_performance='NOT_RUN', unreal='NOT_RUN', release='NOT_RUN')
    catalog.require(type(result['wall_elapsed_ns']) is int and result['wall_elapsed_ns'] > 0 and
                    result['wall_elapsed_ns'] >= sum(r['elapsed_ns'] for r in records), 'wall duration accounting')
    if publish:
        catalog.require(not (directory / 'soak.json').exists(), 'soak result already exists')
        (directory / 'soak.json').write_bytes(catalog.encoded(result))
    else:
        catalog.require(catalog.load(directory / 'soak.json') == result, 'fabricated or stale soak result')
    return result


def execute(output, build, configuration, provenance, manual, cycles, clock):
    plan = matrix(cycles)
    source = source_binding(provenance, manual)
    binaries = {n: showcase.executable(build, n, configuration) for n in NAMES}
    catalog.require(not output.exists(), 'output already exists')
    output.mkdir(parents=True)
    (output / 'logs').mkdir()
    (output / 'runs').mkdir()
    execution = dict(schema=1, cycles=cycles, clock=clock, source=source, plan=plan,
        binaries={n: catalog.sha(p.read_bytes()) for n, p in binaries.items()},
        records=[], completed=False, wall_elapsed_ns=0)
    start = time.monotonic_ns()
    def save():
        execution['wall_elapsed_ns'] = time.monotonic_ns() - start
        (output / 'execution.json').write_bytes(catalog.encoded(execution))
    def perform(name, action):
        record = dict(id=name, status='running', elapsed_ns=0)
        execution['records'].append(record); save()
        began = time.monotonic_ns()
        with (output / 'logs' / (name + '.log')).open('w', encoding='utf-8') as log:
            try:
                action(log)
                record['status'] = 'completed'
            except BaseException:
                record['status'] = 'failed'
                traceback.print_exc(file=log)
                raise
            finally:
                record['elapsed_ns'] = time.monotonic_ns() - began; save()
        print('completed', name, flush=True)
    def command(argv, log, timeout):
        log.write(json.dumps(list(map(str, argv))) + '\n'); log.flush()
        subprocess.run(list(map(str, argv)), stdout=log, stderr=subprocess.STDOUT,
                       timeout=timeout, check=True)
    perform('showcase', lambda log: command([sys.executable, SHOWCASE / 'run.py',
        '--binaries-build', build, '--provenance', SHOWCASE / 'provenance.json',
        '--output', output / 'showcase', '--clock', clock,
        *(['--config', configuration] if configuration else [])], log, 900))
    pair = a.module('golden_audit_pair', HERE.parent / 'golden_system/run.py').pair
    for row in plan:
        path = output / 'runs' / row['id']
        extra = ['--count', row['count'], '--ticks', row['ticks'], '--workers', row['workers'],
                 '--grain', row['grain'], '--campaign', row['campaign']]
        if row['dispatch']:
            extra += ['--dispatch', row['dispatch'], '--fault', row['fault']]
        def action(log):
            if row['mode'] == 'external':
                h, peer, ho, po = pair(binaries[row['executable']], binaries['golden_controller'],
                                     path, extra=extra)
                log.write(ho + po)
                catalog.require(h == peer == 0, 'external child failure')
            else:
                command([binaries[row['executable']], '--mode', row['mode'], '--output', path, *extra], log, 120)
            inspect(path, row)
        perform(row['id'], action)
    catalog.require(source_binding(provenance, manual) == source and
                    execution['binaries'] == {n: catalog.sha(p.read_bytes()) for n, p in binaries.items()},
                    'source or executable changed during execution')
    execution['completed'] = True; save()
    return verify(output, build, configuration, provenance, manual, publish=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', type=Path)
    parser.add_argument('--build', type=Path, default=HERE / 'build')
    parser.add_argument('--binaries-build', type=Path)
    parser.add_argument('--config')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--verify', type=Path)
    parser.add_argument('--cycles', type=int, default=16)
    parser.add_argument('--clock', choices=('fake', 'steady'), default='steady')
    parser.add_argument('--provenance', type=Path, default=HERE / 'provenance.json')
    parser.add_argument('--manual', type=Path, default=HERE.parents[1] / 'golden_audit')
    args = parser.parse_args()
    try:
        catalog.require(bool(args.output) != bool(args.verify), 'select exactly one of --output or --verify')
        matrix(args.cycles)
        build = args.binaries_build
        if args.verify:
            catalog.require(build is not None, '--verify requires --binaries-build')
        elif build is None:
            catalog.require(args.prefix is not None, '--prefix is required to build the installed source kit')
            args.config = args.config or 'Release'
            showcase.command(['cmake', '-S', SHOWCASE, '-B', args.build,
                              '-DCMAKE_BUILD_TYPE=' + args.config,
                              '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
                              '-DCMAKE_PREFIX_PATH=' + str(args.prefix.resolve())])
            showcase.command(['cmake', '--build', args.build, '--config', args.config, '--parallel', '2'])
            build = args.build
        if args.verify:
            value = verify(args.verify.resolve(), build.resolve(), args.config,
                           args.provenance.resolve(), args.manual.resolve())
        else:
            value = execute(args.output.resolve(), build.resolve(), args.config,
                            args.provenance.resolve(), args.manual.resolve(), args.cycles, args.clock)
        print('PASS M26-06 installed audit and bounded soak:', value['nominal_runs'],
              'nominal runs,', value['fault_runs'], 'fault runs;',
              value['wall_elapsed_ns'] / 1e9, 'wall seconds')
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
