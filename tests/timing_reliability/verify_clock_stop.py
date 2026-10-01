#!/usr/bin/env python3
"""Actual baseline/current clock and watchdog stop causal controls.

Only temporary copies receive scheduling hooks or private-member visibility.
Clock outlier uses the unmodified public entry point. Exit 49 means the exact
lost-wake diagnostic fired; a generic timeout is never an expected negative.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BASELINE = "b60a73b1e9bacd6ace250dfa1d5d51fab862eaa7"

CLOCK = '#include <simcore/highres_clock.hpp>\n#include <atomic>\n#include <cstdio>\n#include <chrono>\nstatic std::atomic<unsigned> reads{0};\nstatic std::uint64_t reader() {\n  const auto n=reads.fetch_add(1);\n  return n<2 ? static_cast<std::uint64_t>(n+1)*1000 : 1\'000\'000\'000ULL;\n}\nint main() {\n HighResClock::init(reader);\n const auto first=HighResClock::now();\n for(unsigned i=0;i<5000;++i) (void)HighResClock::now();\n const auto after=HighResClock::now();\n const auto reference=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());\n std::printf("first=%llu after=%llu reference=%llu ahead=%llu tsc=%d\\n",static_cast<unsigned long long>(first),static_cast<unsigned long long>(after),static_cast<unsigned long long>(reference),static_cast<unsigned long long>(after>reference?after-reference:0),HighResClock::using_tsc());\n return !HighResClock::using_tsc() && after<=reference+1\'000\'000 ? 0:1;\n}\n'

FLOOR = '#include <simcore/highres_clock.hpp>\n#include <cstdio>\nint main() {\n HighResClock::last_ns_=100;\n unsigned long long delayed=0;\n std::thread older([&] {delayed=HighResClock::ensure_monotonic(200);});\n while(!floor_probe::paused.load()) std::this_thread::yield();\n const auto newer=HighResClock::ensure_monotonic(300);\n floor_probe::release=true;older.join();\n const auto following=HighResClock::ensure_monotonic(250);\n std::printf("delayed=%llu completed=%llu following=%llu\\n",delayed,static_cast<unsigned long long>(newer),static_cast<unsigned long long>(following));\n return following>newer && following>delayed?0:1;\n}\n'

FLOOR_HOOKS = '#pragma once\n#include <atomic>\n#include <thread>\nnamespace floor_probe {\ninline std::atomic<bool> paused{false}, release{false};\ninline void after_read(unsigned long long ns) {\n if(ns==200) {paused=true;while(!release.load()) std::this_thread::yield();}\n}\n}\n'

STOP = '#include <rt/watchdog.hpp>\n#include <memory>\nint main() {\n auto watchdog=std::make_unique<rt::Watchdog>(std::chrono::hours(24),std::chrono::hours(24),[]{});\n while(!stop_probe::checked.load()) std::this_thread::yield();\n std::thread monitor([] {\n  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);\n  while(!stop_probe::finished.load()) {\n   if(std::chrono::steady_clock::now()>deadline) {std::fprintf(stderr,"REPRO: watchdog stop notification lost before condition-variable wait; join cannot complete\\n");std::_Exit(49);}\n   std::this_thread::yield();\n  }\n });\n watchdog.reset();stop_probe::finished=true;monitor.join();std::puts("watchdog stop joined");\n}\n'

STOP_HOOKS = '#pragma once\n#include <atomic>\n#include <chrono>\n#include <cstdio>\n#include <cstdlib>\n#include <thread>\nnamespace stop_probe {\ninline std::atomic<bool> checked{false}, release{false}, finished{false};\ninline void predicate(bool value) {\n if(!value && !checked.exchange(true)) while(!release.load()) std::this_thread::yield();\n}\ninline void notified() {std::fprintf(stderr,"stop notification while wait predicate remains held\\n");release=true;}\n}\n'


def replace_once(source, old, new):
    assert source.count(old) == 1, old
    return source.replace(old, new)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-directory', type=Path, required=True)
    parser.add_argument('--compiler', default='c++')
    args = parser.parse_args()
    output = args.output_directory.resolve()
    output.mkdir(parents=True, exist_ok=False)
    records = []
    for kind, source, hooks in [('clock', CLOCK, ''), ('floor', FLOOR, FLOOR_HOOKS), ('stop', STOP, STOP_HOOKS)]:
        path = 'rt/include/rt/watchdog.hpp' if kind == 'stop' else 'include/simcore/highres_clock.hpp'
        baseline = subprocess.check_output(['git', 'show', BASELINE + ':' + path], cwd=ROOT)
        for label, raw in [('baseline', baseline), ('repaired', (ROOT / path).read_bytes())]:
            directory = output / (kind + '-' + label)
            relative = 'rt/watchdog.hpp' if kind == 'stop' else 'simcore/highres_clock.hpp'
            header = directory / relative
            header.parent.mkdir(parents=True)
            text = raw.decode().replace('\r\n', '\n')
            if hooks:
                hook = directory / 'hooks.hpp'
                hook.write_text(hooks)
                text = '#include "' + str(hook) + '"\n' + text
            if kind == 'floor':
                text = replace_once(text, 'private:', 'public:')
                needle = '        uint64_t last = last_ns_.load();'
                text = replace_once(text, needle, needle + '\n        floor_probe::after_read(ns);')
            if kind == 'stop':
                text = replace_once(text, 'cv_.wait(lk, [this] { return stop_.load() || armed_; });',
                    'cv_.wait(lk, [this] { const bool value=stop_.load() || armed_; stop_probe::predicate(value); return value; });')
                if label == 'baseline':
                    needle = '        stop_ = true;\n        cv_.notify_all();'
                    text = replace_once(text, needle, needle + '\n        stop_probe::notified();')
                else:
                    # Release before acquiring the same predicate mutex. Its
                    # ownership orders stop publication after CV wait registration.
                    needle = '    ~Watchdog() {'
                    text = replace_once(text, needle, needle + '\n        stop_probe::release=true;')
            header.write_text(text)
            test = directory / 'probe.cpp'
            test.write_text(source)
            exe = directory / 'probe'
            command = [args.compiler, '-std=c++20', '-O1', '-g', '-pthread', '-I' + str(directory), str(test), '-o', str(exe)]
            (directory / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
            with (directory / 'build.log').open('w') as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
            with (directory / 'run.log').open('w') as log:
                result = subprocess.run([str(exe)], stdout=log, stderr=subprocess.STDOUT, timeout=10)
            expected = (49 if kind == 'stop' else 1) if label == 'baseline' else 0
            records.append(dict(kind=kind, label=label, source_path=path, baseline_revision=BASELINE,
                source_sha256=hashlib.sha256(raw).hexdigest(), instrumented_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),
                exit=result.returncode, expected=expected))
            (output / 'results.json').write_text(json.dumps(records, indent=2) + '\n')
            assert result.returncode == expected, records[-1]
            if kind == 'stop' and label == 'baseline':
                assert 'REPRO: watchdog stop notification lost' in (directory / 'run.log').read_text()
            print(kind, label, result.returncode, flush=True)
    print('PASS clock outlier/publication and watchdog stop causal controls', flush=True)


if __name__ == '__main__':
    main()
