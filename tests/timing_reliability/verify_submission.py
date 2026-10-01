#!/usr/bin/env python3
"""Classify the retained benchmark's exact-submission assumption without edits.

Compile its real fixture unchanged, then a diagnostic copy with a scheduling gate
at entry to the public submit callback. Keep the original one-millisecond budget
and all assertions. Observe the original terminal/submission predicate before
releasing the held callback, then allow cleanup before reporting that predicate.
This models preemption; it is not a proposed blocking backend implementation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]

GATE = r'''
#include <atomic>
#include <cstdio>
#include <thread>
#include <rt/runtime.hpp>
namespace submission_gate {
inline rt::HalV2CommandTimelineExtension original{}, table{};
inline std::atomic<bool> released{false}, completed{false};
inline rt::HalV2BackendRegistration wrap(rt::HalV2BackendRegistration registration) {
  original=*registration.command_timeline; table=original;
  table.submit=[](void* p,const rt::DeviceCommandBatch* batch) {
    while(!released.load()) std::this_thread::yield();
    const auto result=original.submit(p,batch);
    completed=true;
    return result;
  };
  registration.command_timeline=&table; return registration;
}
inline void release() {
  released=true;
  while(!completed.load()) std::this_thread::yield();
}
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-library', type=Path, required=True)
    parser.add_argument('--output-directory', type=Path, required=True)
    parser.add_argument('--compiler', default='c++')
    args = parser.parse_args()
    output = args.output_directory.resolve()
    output.mkdir(parents=True, exist_ok=False)
    original = ROOT / 'bench/providers/runtime_cases/device.cpp'
    catalog = ROOT / 'bench/providers/runtime_cases/catalog.inc'
    library = args.runtime_library.resolve()
    inputs = {str(p.relative_to(ROOT)) if p.is_relative_to(ROOT) else str(p):
              hashlib.sha256(p.read_bytes()).hexdigest() for p in [original, catalog, library]}
    gate = output / 'submission_gate.hpp'
    gate.write_text(GATE)
    copy = '#include "' + str(gate) + '"\n' + original.read_text()
    old = 'okay(owner.rt.register_device_backend(backend.hal_v2_registration(),backend_handle));'
    assert copy.count(old) == 1
    copy = copy.replace(old, 'okay(owner.rt.register_device_backend(submission_gate::wrap(backend.hal_v2_registration()),backend_handle));')
    old = '        require(terminals==1 && fixture.backend.stats().submissions==1);'
    assert copy.count(old) == 1
    copy = copy.replace(old, r'''
        const auto submissions=fixture.backend.stats().submissions;
        const bool original_predicate=terminals==1 && submissions==1;
        std::fprintf(stderr,"terminal=%zu submissions=%llu providers=%llu copied=%llu\n",
          terminals,static_cast<unsigned long long>(submissions),
          static_cast<unsigned long long>(fixture.providers),
          static_cast<unsigned long long>(fixture.copied_count));
        submission_gate::release();
        require(original_predicate);''')
    diagnostic = output / 'device-diagnostic.cpp'
    diagnostic.write_text(copy)
    records = []
    for label, fixture, repetitions in [('unchanged', original, 1000), ('held-submit', diagnostic, 1)]:
        source = output / f'{label}.cpp'
        source.write_text(f'''#include <cstdio>
#include "{fixture}"
int main() {{
  namespace b=rtfw::benchmark::runtime;
  using b::Family;
  constexpr b::Case catalog[]{{
#include "{catalog}"
  }};
  for(const auto& c:catalog) if(std::string_view(c.id)=="device-timeout-nonpublication") {{
    for(unsigned i=0;i<{repetitions};++i) {{
      try {{b::detail::DeviceFailure f(c);if(!f.run(i).correct) return 2;}}
      catch(const std::exception& e) {{std::fprintf(stderr,"iteration=%u %s\\n",i,e.what());return 1;}}
    }}
    std::puts("PASS {repetitions} original fixture invocations");return 0;
  }}
  return 3;
}}
''')
        executable = output / label
        command = [args.compiler, '-std=c++20', '-O1', '-g']
        command += ['-I' + str(ROOT / p) for p in ['include', 'rt/include', 'core/include', 'bench/include', 'bench/providers/runtime_cases']]
        command += [str(source), str(library), '-pthread', '-o', str(executable)]
        (output / f'{label}-command.json').write_text(json.dumps(command, indent=2) + '\n')
        with (output / f'{label}-build.log').open('w') as log:
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=180, check=True)
        log_path = output / f'{label}-run.log'
        with log_path.open('w') as log:
            result = subprocess.run([str(executable)], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=60)
        expected = 0 if label == 'unchanged' else 1
        records.append(dict(label=label, exit=result.returncode, expected=expected))
        (output / 'results.json').write_text(json.dumps({'inputs': inputs, 'records': records}, indent=2) + '\n')
        assert result.returncode == expected, records[-1]
        if label == 'held-submit':
            assert 'terminal=1 submissions=0 providers=1 copied=0' in log_path.read_text()
        print(label, result.returncode, flush=True)
    print('PASS classification: preempted submit may time out before backend accounting; historical hosted cause remains unproven', flush=True)


if __name__ == '__main__':
    main()
