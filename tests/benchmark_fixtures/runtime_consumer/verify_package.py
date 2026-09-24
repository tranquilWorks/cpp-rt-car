#!/usr/bin/env python3
"""Append Runtime coverage after both unchanged predecessor package wrappers."""
import argparse
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser()
for name in ('work-directory','artifact-output','cpu-artifact-output','runtime-artifact-output'):
    p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();work=a.work_directory.resolve()
def run(*args):subprocess.run([str(x) for x in args],cwd=ROOT,check=True,timeout=1800)
run(sys.executable,ROOT/'tests/benchmark_fixtures/cpu_consumer/verify_package.py','--work-directory',work,
    '--artifact-output',a.artifact_output.resolve(),'--cpu-artifact-output',a.cpu_artifact_output.resolve())
suffix='.exe' if sys.platform=='win32' else ''
consumer=work/'consumer'/('rtfw_consumer_benchmark_runtime'+suffix)
if not consumer.is_file():consumer=work/'consumer'/'Release'/('rtfw_consumer_benchmark_runtime'+suffix)
run(consumer,a.runtime_artifact_output.resolve())
run(sys.executable,ROOT/'tools/check_benchmark_artifact.py','--artifact-root',a.runtime_artifact_output.resolve())
for mode in ('installed-runtime','embedded-runtime'):
    option=f'-DCMAKE_PREFIX_PATH={(work/"relocated/stage").as_posix()}' if mode=='installed-runtime' else f'-DRTFW_SOURCE_DIR={ROOT.as_posix()}'
    build=work/mode
    run('cmake','-S',ROOT/'tests/benchmark_fixtures/runtime_consumer','-B',build,'-DCMAKE_BUILD_TYPE=Release',option)
    run('cmake','--build',build,'--config','Release','--target','runtime_example','--parallel','2')
    run('ctest','--test-dir',build,'-C','Release','--output-on-failure','-R','^benchmark_runtime_package_consumer$')
print('Runtime installed-source, relocated and add-subdirectory consumers passed')
