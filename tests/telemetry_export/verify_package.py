#!/usr/bin/env python3
"""Run optional native telemetry clients against real default and optional relocated CPack archives."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('package_helpers', ROOT / 'tests/installed_docs/verify_package.py')
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)
run = h.run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-directory', type=Path, required=True)
    parser.add_argument('--native-python', type=Path)
    parser.add_argument('--trace-processor', type=Path)
    parser.add_argument('--collector', type=Path)
    args = parser.parse_args()
    if args.native_python and not (args.trace_processor and args.collector):
        parser.error('native validation requires both real tools')
    record = args.work_directory.resolve()
    if record.exists():
        raise RuntimeError('record directory must be new')
    record.mkdir(parents=True)
    work = Path(tempfile.mkdtemp(prefix='telemetry relocated ')).resolve()
    if work == ROOT or ROOT in work.parents:
        raise RuntimeError('consumer must be outside checkout')
    (record / 'relocation-directory.txt').write_text(str(work) + '\n')
    sdk = work / 'SDK build'
    source = work / 'independent tests'
    inputs = [ROOT / 'tests/telemetry_export' / n for n in
              ('CMakeLists.txt', 'main.cpp', 'etw_native.cpp', 'test_spool.py')]
    bindings = {}
    for path in inputs:
        relative = path.relative_to(ROOT / 'tests')
        target = source / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        bindings[relative.as_posix()] = hashlib.sha256(target.read_bytes()).hexdigest()
    (record / 'consumer-sources.json').write_text(json.dumps(bindings, indent=2) + '\n')
    for optional in (False, True):
        label = 'optional' if optional else 'default'
        original = work / (label + ' original prefix')
        prefix = work / (label + ' relocated SDK with spaces')
        archives = work / (label + ' archives')
        run('cmake', '-S', ROOT, '-B', sdk, '-DCMAKE_BUILD_TYPE=Release', '-DENABLE_TESTS=OFF',
            '-DRTFW_BUILD_EXAMPLES=OFF', '-DRTFW_BUILD_RUNTIME_DEMO=OFF', '-DRTFW_BUILD_EXPERIMENTAL=OFF',
            '-DRTFW_BUILD_BENCHMARKS=' + ('ON' if optional else 'OFF'), '-DRTFW_ENABLE_CUDA=OFF',
            '-DSIM_SANITIZERS=', '-DSIM_WERROR=ON', '-DCMAKE_INSTALL_INCLUDEDIR=sdk include',
            '-DCMAKE_INSTALL_DATADIR=custom data', '-DCMAKE_INSTALL_PREFIX=' + str(original))
        run('cmake', '--build', sdk, '--config', 'Release', '--parallel', '2')
        run('cmake', '--install', sdk, '--config', 'Release', '--prefix', original)
        run('cpack', '--config', sdk / 'CPackConfig.cmake', '-C', 'Release', '-B', archives,
            '-D', 'CPACK_PACKAGING_INSTALL_PREFIX=/', '-D', 'CPACK_PACKAGE_FILE_NAME=rtfw-telemetry-' + label)
        shutil.rmtree(original)
        run(sys.executable, ROOT / 'tools/extract_release_archive.py', '--artifact-dir', archives,
            '--destination', prefix)
        data = prefix / 'custom data/rtfw'
        h.docs.verify(data_root=data)
        build = work / (label + ' independent consumer')
        flags = ['-DCMAKE_PREFIX_PATH=' + str(prefix), '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
                 '-DTELEMETRY_KIT=' + str(data / 'integrations/telemetry')]
        h.build_test(source / 'telemetry_export', build, *flags)
        if not (build / 'compile_commands.json').is_file():
            raise RuntimeError('compile commands are required')
        h.check_no_private_paths(build, [ROOT, sdk, original])
        inventory = json.loads(run('ctest', '--test-dir', build, '--show-only=json-v1'))
        if len(inventory['tests']) != (4 if sys.platform == 'win32' else 3):
            raise RuntimeError('independent consumer inventory differs')
        kit = data / 'integrations/telemetry'
        for name in ('telemetry.hpp', 'telemetry.cpp', 'etw.hpp', 'etw.cpp', 'export.py', 'requirements.txt'):
            if (kit / name).read_bytes() != (ROOT / 'integrations/telemetry' / name).read_bytes():
                raise RuntimeError('installed source differs: ' + name)
        # Exercise the actual relocated encoder entrypoint and native decoders
        # when an explicitly selected optional tool environment is supplied.
        if args.native_python:
            example = build / 'telemetry-kit/rtfw_telemetry_example'
            run(args.native_python, ROOT / 'tests/telemetry_export/verify_native.py',
                '--kit', kit, '--example', example, '--trace-processor', args.trace_processor,
                '--collector', args.collector, '--output', record / (label + '-native'))
        if optional:
            full = work / 'all prior SDK consumers'
            h.build_test(ROOT / 'tests/package_consumer', full,
                         '-DCMAKE_PREFIX_PATH=' + str(prefix), '-DRTFW_TEST_BENCHMARK=ON')
            inventory = json.loads(run('ctest', '--test-dir', full, '--show-only=json-v1'))
            if len(inventory['tests']) != 71:
                raise RuntimeError('all 71 preserved optional consumers are required')
    run('cmake', '-S', source / 'telemetry_export', '-B', work / 'missing SDK',
        '-DCMAKE_DISABLE_FIND_PACKAGE_rtfw=ON', failure='REQUIRED')
    run('cmake', '-S', source / 'telemetry_export', '-B', work / 'missing kit',
        '-DCMAKE_PREFIX_PATH=' + str(prefix), '-DTELEMETRY_KIT=' + str(work / 'absent-kit'),
        failure='not an existing directory')
    print('PASS default/optional actual CPack, removed prefixes, independent telemetry clients plus real native consumers, all 71 preserved SDK consumers and missing-input negatives')


if __name__ == '__main__':
    main()
