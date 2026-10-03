#!/usr/bin/env python3
"""Add scheduling/observation seams to a test-only copy of the real provider."""
import argparse
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--provider-directory', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
source = (a.provider_directory / 'device.cpp').read_text()

def replace(old, new):
    global source
    assert source.count(old) == 1, old
    source = source.replace(old, new)

if 'okay(owner.rt.register_device_backend(failure_registration,backend_handle));' in source:
    replace('okay(owner.rt.register_device_backend(failure_registration,backend_handle));',
            'okay(owner.rt.register_device_backend(closeout::wrap(failure_registration),backend_handle));')
else:
    replace('okay(owner.rt.register_device_backend(backend.hal_v2_registration(),backend_handle));',
            'okay(owner.rt.register_device_backend(closeout::wrap(backend.hal_v2_registration()),backend_handle));')
replace('require(fixture.owner.rt.step(frame(1,2*period,1000),&step)==expected);',
        'require(fixture.owner.rt.step(frame(1,2*period,1000),&step)==expected);\n'
        '        closeout::terminal(fixture);')
replace('okay(fixture.finish());',
        'closeout::before_stop();\n        okay(fixture.finish());\n'
        '        closeout::stopped(fixture);')
# Corrupt only observed test data, after real checked shutdown. This exercises
# the actual fixture oracles without changing a backend or Runtime contract.
start = source.index('struct DeviceFailure final:Fixture')
source = source[:start] + source[start:].replace(
    'fixture.backend.stats()', 'closeout::stats(fixture.backend)')
replace('require(terminals==1', 'closeout::terminals(terminals);\n        require(terminals==1')
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(source)
