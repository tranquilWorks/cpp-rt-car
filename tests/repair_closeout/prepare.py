#!/usr/bin/env python3
"""Instrument only a test-owned copy; never alter the shipped backend."""
import argparse
import hashlib
import json
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source-root', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
paths = ['rt/src/loopback_backend.cpp', 'rt/include/rt/loopback_backend.hpp']
source, header = [(a.source_root / name).read_text() for name in paths]
source = source.replace('SampledIoLoopbackBackend', 'ObservedLoopbackBackend')
header = '#pragma once\n#include <rt/loopback_backend.hpp>\nnamespace rt {\n' + header[header.index('class SampledIoLoopbackBackend final'):].replace('SampledIoLoopbackBackend', 'ObservedLoopbackBackend')
source = source.replace('#include <rt/loopback_backend.hpp>',
                        '#include "observed.hpp"\n#include "hooks.hpp"')
for anchor, point in [('        HalV2BatchCompletion completion{};\n        completion.batch_id', 1),
                      ('                output[*count] = backend->slots[index].completion;', 2)]:
    assert source.count(anchor) == 1, anchor
    source = source.replace(anchor, f'        repair_observer::pause({point});\n' + anchor)
(a.output / 'observed.cpp').write_text(source)
(a.output / 'observed.hpp').write_text(header)
(a.output / 'source-bindings.json').write_text(json.dumps(
    {name: hashlib.sha256((a.source_root / name).read_bytes()).hexdigest()
     for name in paths}, indent=2) + '\n')
