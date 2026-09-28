#!/usr/bin/env python3
"""Verify actual CPack archives, offline documentation and executable recipes."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import check_sdk_docs as docs


def run(*command, failure=None):
    argv = list(map(str, command))
    print('+', json.dumps(argv), flush=True)
    result = subprocess.run(argv, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=900)
    print(result.stdout, end='', flush=True)
    if failure is None:
        if result.returncode:
            raise RuntimeError(f'command failed ({result.returncode}): {argv}')
    elif not result.returncode or failure not in result.stdout:
        raise RuntimeError(f'expected failure {failure!r}: {argv}')
    return result.stdout


def build_test(source, build, *flags):
    run('cmake', '-S', source, '-B', build, '-DCMAKE_BUILD_TYPE=Release', *flags)
    run('cmake', '--build', build, '--config', 'Release', '--parallel', '2')
    run('ctest', '--test-dir', build, '-C', 'Release', '--output-on-failure', '--no-tests=error')


def recipe_inventory(build, benchmark):
    data = json.loads(run('ctest', '--test-dir', build, '-C', 'Release', '--show-only=json-v1'))
    names = {t['name'] for t in data['tests'] if t['name'].startswith('recipe_')}
    expected = {'recipe_' + name for name in ('raw_c', 'raw_cpp', 'profile', 'mixed_rate',
                'live_control', 'replay', 'replay_replaced', 'replay_rejected', 'cuda_boundary', 'xdma_boundary')}
    if benchmark:
        expected.add('recipe_benchmark')
    if names != expected:
        raise RuntimeError(f'recipe test inventory differs: {names}')


def check_no_private_paths(build, forbidden):
    commands = build / 'compile_commands.json'
    if commands.exists():
        for entry in json.loads(commands.read_text()):
            command = entry.get('command', ' '.join(entry.get('arguments', []))).replace('\\', '/')
            for path in forbidden:
                if path.as_posix() in command:
                    raise RuntimeError('private/original compile path leaked: ' + str(path))


def must_reject_bundle(data, relative):
    path = data / relative
    saved = path.read_bytes()
    try:
        path.unlink()
        try:
            docs.verify(data_root=data)
        except ValueError:
            print('PASS: missing installed artifact rejected:', relative, flush=True)
        else:
            raise RuntimeError('missing installed artifact was accepted')
    finally:
        path.write_bytes(saved)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--work-directory', type=Path, required=True)
    args = parser.parse_args()
    work = args.work_directory.resolve()
    work.mkdir(parents=True, exist_ok=True)
    sdk = work / 'sdk build'
    with tempfile.TemporaryDirectory(prefix='manual original ', dir=work) as original_name, tempfile.TemporaryDirectory(prefix='manual relocated ') as relocated_name:
        original, relocated = Path(original_name) / 'sdk', Path(relocated_name)
        for benchmark in (False, True):
            mode = 'benchmark' if benchmark else 'default'
            option = 'ON' if benchmark else 'OFF'
            run('cmake', '-S', ROOT, '-B', sdk, '-DCMAKE_BUILD_TYPE=Release',
                '-DENABLE_TESTS=OFF', '-DRTFW_BUILD_EXAMPLES=OFF', '-DRTFW_BUILD_RUNTIME_DEMO=OFF',
                '-DRTFW_BUILD_EXPERIMENTAL=OFF', f'-DRTFW_BUILD_BENCHMARKS={option}',
                '-DSIM_SANITIZERS=', '-DSIM_WERROR=ON', '-DCMAKE_INSTALL_INCLUDEDIR=sdk/include',
                '-DCMAKE_INSTALL_DATADIR=custom/data', f'-DCMAKE_INSTALL_PREFIX={original}')
            run('cmake', '--build', sdk, '--config', 'Release', '--parallel', '2')
            run('cmake', '--install', sdk, '--config', 'Release', '--prefix', original)
            archive_dir = relocated / ('archive ' + mode)
            run('cpack', '--config', sdk / 'CPackConfig.cmake', '-C', 'Release',
                '-B', archive_dir, '-D', 'CPACK_PACKAGING_INSTALL_PREFIX=/',
                '-D', 'CPACK_PACKAGE_FILE_NAME=rtfw-manual-' + mode)
            archive = archive_dir / ('rtfw-manual-' + mode + ('.zip' if sys.platform == 'win32' else '.tar.gz'))
            if not archive.is_file():
                raise RuntimeError('CPack archive missing')
            shutil.rmtree(original)
            prefix = relocated / ('sdk ' + mode)
            run(sys.executable, ROOT / 'tools/extract_release_archive.py', '--artifact-dir', archive_dir, '--destination', prefix)
            data = prefix / 'custom/data/rtfw'
            docs.verify(data_root=data)
            kit = data / 'examples/recipes'
            consumer = relocated / ('consumer ' + mode)
            run(sys.executable, kit / 'run_transcripts.py', '--prefix', prefix, '--build', consumer, '--benchmark', option)
            recipe_inventory(consumer, benchmark)
            check_no_private_paths(consumer, [ROOT, sdk, original])
            build_test(ROOT / 'tests/package_consumer', relocated / ('full SDK ' + mode),
                       f'-DCMAKE_PREFIX_PATH={prefix}', f'-DRTFW_TEST_BENCHMARK={option}')
            recipe_inventory(relocated / ('full SDK ' + mode), benchmark)
            embedded = relocated / 'embedded'
            run(sys.executable, kit / 'run_transcripts.py', '--prefix', prefix, '--build', embedded,
                '--source', ROOT, '--benchmark', option)
            recipe_inventory(embedded, benchmark)
            if not benchmark:
                must_reject_bundle(data, 'manual/api/rt__runtime.hpp.html')
                must_reject_bundle(data, 'examples/recipes/mixed_rate_conformance.hpp')
                # An altered independent expected count must fail actual execution.
                variant = relocated / 'false oracle source'
                shutil.copytree(kit, variant)
                driver = variant / 'package_consumer/mixed_rate_consumer.cpp'
                text = driver.read_text()
                before = 'result.callback_counts[0] == 9'
                if text.count(before) != 1:
                    raise RuntimeError('ambiguous recipe oracle mutation')
                driver.write_text(text.replace(before, 'result.callback_counts[0] == 8'))
                build = relocated / 'false oracle build'
                run('cmake', '-S', variant, '-B', build, '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_PREFIX_PATH={prefix}')
                run('cmake', '--build', build, '--config', 'Release', '--target', 'recipe_mixed_rate', '--parallel', '2')
                run('ctest', '--test-dir', build, '-C', 'Release', '--output-on-failure',
                    '--no-tests=error', '-R', '^recipe_mixed_rate$', failure='Failed')
                # Missing support source must also fail a real clean consumer build.
                (variant / 'mixed_rate_conformance.hpp').unlink()
                missing = relocated / 'missing support build'
                run('cmake', '-S', variant, '-B', missing, '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_PREFIX_PATH={prefix}')
                run('cmake', '--build', missing, '--config', 'Release', '--target', 'recipe_mixed_rate',
                    '--parallel', '2', failure='mixed_rate_conformance.hpp')
        print('PASS: default/optional CPack manuals, recipes, full SDK, embedding and four archive negatives', flush=True)


if __name__ == '__main__':
    main()
