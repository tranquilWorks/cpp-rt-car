#!/usr/bin/env python3
"""Verify relocated optional Unreal source orchestration with the real SDK.

This does not compile any Unreal-dependent translation unit.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--config', default='Release')
    parser.add_argument('--c-compiler', default='cc')
    parser.add_argument('--cxx-compiler', default='c++')
    parser.add_argument('--sanitizers', default='')
    args = parser.parse_args()
    build = args.build.resolve(strict=True)
    source = Path(__file__).resolve().parents[2]
    work = Path(tempfile.mkdtemp(prefix='unreal-source-package-', dir=build))
    records = []

    def run(argv):
        argv = list(map(str, argv)); log = work / f'command-{len(records)}.log'
        with log.open('wb') as output:
            result = subprocess.run(argv, stdout=output, stderr=subprocess.STDOUT, timeout=180, check=False)
        records.append({'argv': argv, 'exit_code': result.returncode,
                        'log': str(log), 'sha256': hashlib.sha256(log.read_bytes()).hexdigest()})
        (work / 'commands.json').write_text(json.dumps(records, indent=2) + '\n')
        if result.returncode:
            print(log.read_text(errors='replace'))
            raise RuntimeError(f'failed: {argv}; retained {work}')

    original, relocated = work / 'original', work / 'relocated sdk'
    run(['cmake', '--install', build, '--prefix', original, '--config', args.config])
    shutil.move(str(original), str(relocated))
    assert not original.exists()
    data = relocated / 'share/rtfw/integrations'
    for root in ['RTFWUnreal', 'portable']:
        for path in (source / 'integrations/unreal' / root).rglob('*'):
            if path.is_file() and '__pycache__' not in path.parts:
                assert (data / 'unreal' / path.relative_to(source / 'integrations/unreal')).read_bytes() == path.read_bytes()
    run([sys.executable, data / 'unreal/prepare.py', 'stage', '--sdk-root', relocated, '--output', work / 'staged'])
    manifest = json.loads((work / 'staged/source-manifest.json').read_text())
    assert manifest['engine_validation'] == 'NOT_RUN' and len(manifest['expected_tests']) == 15
    consumer = work / 'consumer'; consumer.mkdir()
    body = (source / 'tests/unreal_source/test_worlds.cpp').read_text()
    body = body.replace('../cuda_physics/allocation_guard.hpp', 'allocation_guard.hpp')
    body = body.replace('../../samples/host_adapter/jobs.hpp', 'jobs.hpp')
    body = body.replace('../../samples/host_adapter/memory.hpp', 'memory.hpp')
    (consumer / 'test_worlds.cpp').write_text(body)
    shutil.copy2(source / 'tests/cuda_physics/allocation_guard.hpp', consumer)
    (consumer / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(unreal_installed_worlds LANGUAGES C CXX)
find_package(rtfw 1.2 CONFIG REQUIRED COMPONENTS runtime)
add_executable(worlds test_worlds.cpp)
target_compile_features(worlds PRIVATE cxx_std_20)
target_include_directories(worlds PRIVATE
    "${DATA}/integrations/unreal/portable" "${DATA}/integrations/host_lifecycle"
    "${DATA}/examples/host_adapter")
target_link_libraries(worlds PRIVATE rtfw::runtime ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(worlds PRIVATE Psapi)
endif()
add_library(demo SHARED "${DATA}/integrations/host_lifecycle/plugin.c")
target_include_directories(demo PRIVATE $<TARGET_PROPERTY:rtfw::runtime,INTERFACE_INCLUDE_DIRECTORIES>)
enable_testing()
add_test(NAME actual_installed_worlds COMMAND worlds $<TARGET_FILE:demo>)
''')
    flags = []
    if args.sanitizers:
        value = '-fsanitize=' + args.sanitizers + ' -fno-omit-frame-pointer'
        flags = ['-D' + key + '=' + value for key in ['CMAKE_C_FLAGS', 'CMAKE_CXX_FLAGS', 'CMAKE_EXE_LINKER_FLAGS', 'CMAKE_SHARED_LINKER_FLAGS']]
    consumer_build = work / 'consumer-build'
    run(['cmake', '-S', consumer, '-B', consumer_build, '-DCMAKE_BUILD_TYPE=' + args.config,
         '-DCMAKE_PREFIX_PATH=' + str(relocated), '-DDATA=' + str(relocated / 'share/rtfw'),
         '-DCMAKE_C_COMPILER=' + args.c_compiler, '-DCMAKE_CXX_COMPILER=' + args.cxx_compiler, *flags])
    run(['cmake', '--build', consumer_build, '--config', args.config, '--parallel', '2'])
    run(['ctest', '--test-dir', consumer_build, '-C', args.config, '--output-on-failure'])
    cache = (consumer_build / 'CMakeCache.txt').read_text()
    assert str(source) not in cache.replace(str(work), '')
    (work / 'result.json').write_text(json.dumps({'portable_installed_status': 'PASS',
        'engine_validation': 'NOT_RUN', 'original_prefix_removed': True, 'commands': records}, indent=2) + '\n')
    print('PASS actual relocated SDK/source consumer; Unreal NOT RUN; retained', work)


if __name__ == '__main__': main()
