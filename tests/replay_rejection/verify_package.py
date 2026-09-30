#!/usr/bin/env python3
"""Run public replay regressions against real default and optional relocated CPack archives."""
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
    args = parser.parse_args()
    record = args.work_directory.resolve()
    if record.exists():
        raise RuntimeError('record directory must be new')
    record.mkdir(parents=True)
    work = Path(tempfile.mkdtemp(prefix='replay rejection relocated ')).resolve()
    if work == ROOT or ROOT in work.parents:
        raise RuntimeError('consumer must be outside checkout')
    (record / 'relocation-directory.txt').write_text(str(work) + '\n')
    sdk = work / 'SDK build'
    source = work / 'independent tests'
    inputs = [ROOT / 'tests/replay_rejection' / n for n in
              ('CMakeLists.txt', 'main.cpp', 'scenario.hpp', 'golden_probe.cpp')]
    inputs += [ROOT / 'tests' / n for n in
               ('golden_cuda/allocation.cpp', 'golden_cuda/allocation.hpp', 'cuda_physics/allocation_guard.hpp')]
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
            '-D', 'CPACK_PACKAGING_INSTALL_PREFIX=/', '-D', 'CPACK_PACKAGE_FILE_NAME=rtfw-replay-' + label)
        shutil.rmtree(original)
        run(sys.executable, ROOT / 'tools/extract_release_archive.py', '--artifact-dir', archives,
            '--destination', prefix)
        data = prefix / 'custom data/rtfw'
        h.docs.verify(data_root=data)
        build = work / (label + ' independent consumer')
        flags = ['-DCMAKE_PREFIX_PATH=' + str(prefix), '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON']
        if optional:
            flags += ['-DRTFW_REPLAY_GOLDEN_KIT=' + str(data / 'examples/golden_showcase')]
        h.build_test(source / 'replay_rejection', build, *flags)
        if not (build / 'compile_commands.json').is_file():
            raise RuntimeError('compile commands are required')
        h.check_no_private_paths(build, [ROOT, sdk, original])
        inventory = json.loads(run('ctest', '--test-dir', build, '--show-only=json-v1'))
        if len(inventory['tests']) != (2 if optional else 1):
            raise RuntimeError('independent consumer inventory differs')
        if optional:
            full = work / 'all prior SDK consumers'
            h.build_test(ROOT / 'tests/package_consumer', full,
                         '-DCMAKE_PREFIX_PATH=' + str(prefix), '-DRTFW_TEST_BENCHMARK=ON')
            inventory = json.loads(run('ctest', '--test-dir', full, '--show-only=json-v1'))
            if len(inventory['tests']) != 71:
                raise RuntimeError('all 71 preserved optional consumers are required')
            run('cmake', '-S', source / 'replay_rejection', '-B', work / 'missing kit',
                '-DCMAKE_PREFIX_PATH=' + str(prefix), '-DRTFW_REPLAY_GOLDEN_KIT=' + str(work / 'absent'),
                failure='Missing golden showcase source kit')
    run('cmake', '-S', source / 'replay_rejection', '-B', work / 'missing SDK',
        '-DCMAKE_DISABLE_FIND_PACKAGE_rtfw=ON', failure='REQUIRED')
    print('PASS default/optional actual CPack, removed prefixes, independent public replay consumers, all 71 preserved SDK consumers and missing-input negatives')


if __name__ == '__main__':
    main()
