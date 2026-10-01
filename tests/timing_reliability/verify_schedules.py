#!/usr/bin/env python3
"""Linux diagnostic: force legal preemption points in copies of real headers.

No product hooks or replacement algorithm. Build the unchanged determinism test
against the retained Git baseline and current source. The baseline must expose
missing startup synchronization (44) and lost dispatch progress (42); the current
source must complete the exact original 1500-frame/5000-element hash assertion.
Exit 43 is a broken diagnostic gate, never a successful negative control.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BASELINE = 'b60a73b1e9bacd6ace250dfa1d5d51fab862eaa7'


def instrument(source, kind):
    hooks = ROOT / 'tests/timing_reliability' / f'{kind}_schedule.hpp'
    source = f'#include "{hooks}"\n' + source
    source = re.sub(r'#include "([^"]+)"', lambda m: m[0] if m[1].startswith('/')
                    else f'#include "{ROOT / "include/simcore" / m[1]}"', source)

    def replace(old, new, count=1):
        nonlocal source
        assert source.count(old) == count, (old, source.count(old), count)
        source = source.replace(old, new)

    if kind == 'startup':
        replace('        frameArenas_->bindCurrentThread(i);',
                '        m27_startup::before_bind(i,workerCount_);\n'
                '        frameArenas_->bindCurrentThread(i);')
        replace('      frameArenas_->beginFrame();',
                '      (m27_startup::before_frame(workerCount_), frameArenas_->beginFrame());')
        if '  void waitForWorkersReady() {' in source:
            replace('  void waitForWorkersReady() {',
                    '  void waitForWorkersReady() {\n    m27_startup::before_wait(workerCount_);')
    else:
        replace('    std::uint64_t localToken = dispatchToken_.load(std::memory_order_acquire);',
                '    std::uint64_t localToken = dispatchToken_.load(std::memory_order_acquire);\n'
                '    m27_schedule::worker_ready(workerCount_);')
        replace('      processActiveRange();',
                '      m27_schedule::worker_enter(localToken,workerCount_);\n'
                '      processActiveRange();\n'
                '      m27_schedule::worker_exit(localToken,workerCount_);')
        replace('        nextChunk_.store(0, std::memory_order_relaxed);',
                '        m27_schedule::before_publish(frame_,workerCount_);\n'
                '        nextChunk_.store(0, std::memory_order_relaxed);\n'
                '        m27_schedule::reset(frame_,workerCount_,remaining_,nextChunk_);', 2)
        replace('        dispatchToken_.fetch_add(1, std::memory_order_acq_rel);',
                '        dispatchToken_.fetch_add(1, std::memory_order_acq_rel);\n'
                '        m27_schedule::published(frame_,workerCount_);', 2)
        if '  void waitForRangeWorkers() {' in source:
            replace('  void waitForRangeWorkers() {',
                    '  void waitForRangeWorkers() {\n'
                    '    m27_schedule::quiescence(frame_,workerCount_);')
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-directory', type=Path, required=True)
    parser.add_argument('--output-directory', type=Path, required=True)
    parser.add_argument('--compiler', default='c++')
    args = parser.parse_args()
    build, output = args.build_directory.resolve(), args.output_directory.resolve()
    output.mkdir(parents=True, exist_ok=False)
    records = []
    baseline = subprocess.check_output(['git', 'show', f'{BASELINE}:include/simcore/SimCore.hpp'], cwd=ROOT)
    test = ROOT / 'tests/test_simcore_determinism.cpp'
    assert test.read_bytes() == subprocess.check_output(
        ['git', 'show', f'{BASELINE}:tests/test_simcore_determinism.cpp'], cwd=ROOT)
    inputs = {'baseline_revision': BASELINE, 'test_sha256': hashlib.sha256(test.read_bytes()).hexdigest()}
    for label, raw in [('baseline', baseline), ('repaired', (ROOT / 'include/simcore/SimCore.hpp').read_bytes())]:
        inputs[label + '_sha256'] = hashlib.sha256(raw).hexdigest()
        for kind in ['startup', 'dispatch']:
            directory = output / f'{label}-{kind}'
            (directory / 'simcore').mkdir(parents=True)
            source = raw.decode().replace('\r\n', '\n')
            (directory / 'simcore/SimCore.hpp').write_text(instrument(source, kind))
            exe = directory / 'probe'
            command = [args.compiler, '-std=c++20', '-O1', '-g', '-pthread', '-I' + str(directory)]
            command += ['-I' + str(ROOT / p) for p in ['include', 'rt/include', 'core/include', 'hal', 'gpu', 'external/googletest/googletest/include']]
            command += [str(test)] + [str(build / p) for p in ['lib/libgtest_main.a', 'lib/libgtest.a', 'libsimcore_platform.a', 'libsimcore_rt.a', 'librtfw_experimental.a', 'librtfw_runtime.a']]
            command += ['-ldl', '-o', str(exe)]
            (directory / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
            with (directory / 'build.log').open('w') as log:
                subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
            for threads in [2, 4, 8]:
                command = [str(exe), f'--gtest_filter=CrossMatrixDeterminism/*{threads}ThreadsFmaOff']
                with (directory / f'{threads}-threads.log').open('w') as log:
                    result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=15)
                expected = (44 if kind == 'startup' else 42) if label == 'baseline' else 0
                records.append(dict(label=label, kind=kind, threads=threads, command=command,
                                    exit=result.returncode, expected=expected))
                (output / 'results.json').write_text(json.dumps({'inputs': inputs, 'records': records}, indent=2) + '\n')
                assert result.returncode == expected, records[-1]
                print(label, kind, threads, result.returncode, flush=True)
    print('PASS: six baseline negatives and six unchanged-workload positives', flush=True)


if __name__ == '__main__':
    main()
