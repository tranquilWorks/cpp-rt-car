#!/usr/bin/env python3
"""Extend the unchanged M23-01 package gate with CPU source-example consumers."""
import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
p = argparse.ArgumentParser()
p.add_argument('--work-directory', type=Path, required=True)
p.add_argument('--artifact-output', type=Path, required=True)
p.add_argument('--cpu-artifact-output', type=Path, required=True)
a = p.parse_args()
work = a.work_directory.resolve()

def run(*args):
    subprocess.run([str(x) for x in args], cwd=ROOT, check=True, timeout=1800)

run(sys.executable, ROOT/'tests/benchmark_fixtures/verify_package.py',
    '--work-directory', work, '--artifact-output', a.artifact_output.resolve())
suffix = '.exe' if sys.platform == 'win32' else ''
consumer = work/'consumer'/('rtfw_consumer_benchmark_cpu'+suffix)
if not consumer.is_file():
    consumer = work/'consumer'/'Release'/('rtfw_consumer_benchmark_cpu'+suffix)
output = a.cpu_artifact_output.resolve()
run(consumer, output)
run(sys.executable, ROOT/'tools/check_benchmark_artifact.py', '--artifact-root', output)
for mode in ('installed-cpu', 'embedded-cpu'):
    build = work/mode
    option = (f'-DCMAKE_PREFIX_PATH={work / "relocated/stage"}' if mode == 'installed-cpu'
              else f'-DRTFW_SOURCE_DIR={ROOT}')
    run('cmake', '-S', ROOT/'tests/benchmark_fixtures/cpu_consumer', '-B', build,
        '-DCMAKE_BUILD_TYPE=Release', option)
    run('cmake', '--build', build, '--config', 'Release', '--target', 'cpu_example', '--parallel', '2')
    run('ctest', '--test-dir', build, '-C', 'Release', '--output-on-failure',
        '-R', '^benchmark_cpu_package_consumer$')
print('M23 CPU installed-source, relocated and add-subdirectory consumers passed')
