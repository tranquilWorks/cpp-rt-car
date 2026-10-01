#!/usr/bin/env python3
"""Compare the final-boundary watchdog oracle against actual baseline/current headers.

The only header instrumentation publishes callback completion, so baseline
settings reads are ordered after its asynchronous writes. Product code has no
hooks. The baseline must fail the named boundary test; current source must pass.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BASELINE = 'b60a73b1e9bacd6ace250dfa1d5d51fab862eaa7'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-directory', type=Path, required=True)
    parser.add_argument('--output-directory', type=Path, required=True)
    parser.add_argument('--compiler', default='c++')
    args = parser.parse_args()
    output, build = args.output_directory.resolve(), args.build_directory.resolve()
    output.mkdir(parents=True, exist_ok=False)
    hooks = output / 'callback-complete.hpp'
    hooks.write_text('''#pragma once
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
namespace m27_watchdog {
inline std::atomic<bool> completed{false};
inline void await_callback() {
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!completed.load()) {
    if(std::chrono::steady_clock::now()>deadline) std::_Exit(43);
    std::this_thread::yield();
  }
}
}
''')
    baseline = subprocess.check_output(['git', 'show', f'{BASELINE}:include/simcore/SimCore.hpp'], cwd=ROOT)
    test_path = ROOT / 'tests/test_timing_reliability.cpp'
    test = test_path.read_text()
    needle = '    EXPECT_GT(sim.watchdogTrips(), 0);'
    assert test.count(needle) == 1
    test = test.replace(needle, '    m27_watchdog::await_callback();\n' + needle)
    source = output / 'test.cpp'
    source.write_text(test)
    inputs = {'baseline_revision': BASELINE, 'test_sha256': hashlib.sha256(test_path.read_bytes()).hexdigest()}
    records = []
    for label, raw in [('baseline', baseline), ('repaired', (ROOT / 'include/simcore/SimCore.hpp').read_bytes())]:
        directory = output / label
        (directory / 'simcore').mkdir(parents=True)
        inputs[label + '_sha256'] = hashlib.sha256(raw).hexdigest()
        header = '#include "' + str(hooks) + '"\n' + raw.decode().replace('\r\n', '\n')
        header = re.sub(r'#include "([^"]+)"', lambda m: m[0] if m[1].startswith('/')
                        else f'#include "{ROOT / "include/simcore" / m[1]}"', header)
        needle = '        });\n    initThreads();'
        assert header.count(needle) == 1
        header = header.replace(needle, '          m27_watchdog::completed=true;\n' + needle)
        (directory / 'simcore/SimCore.hpp').write_text(header)
        exe = directory / 'probe'
        command = [args.compiler, '-std=c++20', '-O1', '-g', '-pthread', '-I' + str(directory)]
        command += ['-I' + str(ROOT / p) for p in ['include', 'rt/include', 'core/include', 'hal', 'gpu', 'external/googletest/googletest/include']]
        command += [str(source)] + [str(build / p) for p in ['lib/libgtest_main.a', 'lib/libgtest.a', 'libsimcore_platform.a', 'libsimcore_rt.a', 'librtfw_experimental.a', 'librtfw_runtime.a']]
        command += ['-ldl', '-o', str(exe)]
        (directory / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
        with (directory / 'build.log').open('w') as log:
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
        with (directory / 'run.log').open('w') as log:
            result = subprocess.run([str(exe), '--gtest_filter=SimCoreTimingReliability.WatchdogActionsReachTheFinalQuiescentBoundary'], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=15)
        expected = 1 if label == 'baseline' else 0
        records.append(dict(label=label, exit=result.returncode, expected=expected))
        (output / 'results.json').write_text(json.dumps({'inputs': inputs, 'records': records}, indent=2) + '\n')
        assert result.returncode == expected, records[-1]
        if label == 'baseline':
            log = (directory / 'run.log').read_text()
            assert all(text in log for text in ['sim.visualizersEnabled()', 'sim.broadphaseCoarse()', 'sim.rungActivations(4)', 'events'])
        print(label, result.returncode, flush=True)
    print('PASS watchdog baseline negative and final-boundary handoff', flush=True)


if __name__ == '__main__':
    main()
