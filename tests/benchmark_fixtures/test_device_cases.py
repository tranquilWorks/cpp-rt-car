#!/usr/bin/env python3
"""Exercise the actual device CLI and independent artifact/counter oracles."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('validator', ROOT/'tools/check_benchmark_artifact.py')
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)
p = argparse.ArgumentParser()
p.add_argument('--cli', type=Path, required=True)
p.add_argument('--inventory', type=Path, required=True)
a = p.parse_args()
cli = a.cli.resolve()
rows = json.loads(a.inventory.read_text())['cases']
assert 0 < len(rows) <= 96
assert len({row['id'] for row in rows}) == len(rows)

def run(*args):
    return subprocess.run([str(cli), *map(str,args)], capture_output=True, text=True, timeout=60)

expected = ['rtfw.self:structural']
for family in ('cpu','runtime','device'):
    expected += ['rtfw.'+family+':'+row['id'] for row in
                 json.loads((ROOT/'bench/fixtures'/f'{family}_cases.json').read_text())['cases']]
listing = run('list')
assert listing.returncode == 0 and listing.stdout.splitlines() == sorted(expected)

with tempfile.TemporaryDirectory(prefix='rtfw-device-') as directory:
    root = Path(directory)
    for row in rows:
        output = root/row['id']
        args = ('run','--provider','rtfw.device','--case',row['id'],'--clock','fake','--output',output)
        proc = run(*args)
        assert proc.returncode == (3 if row['real'] else 0), (row['id'],proc.returncode,proc.stderr)
        result = validator.validate_bundle(output)
        samples = json.loads((output/'raw.json').read_text())['samples']
        assert result['status'] == ('not_run' if row['real'] else 'ok')
        assert result['warmup_completed'] == (0 if row['real'] else 2)
        assert result['measured_completed'] == (0 if row['real'] else 5)
        if row['real']:
            assert samples == []
        for index, sample in enumerate(samples):
            c = sample['counters']; mode = row['operation']; size = row['bytes']
            assert sample['invariants_passed']
            assert c['submissions'] == c['completions'] + int(mode=='fault-cleanup')
            if mode in ('roundtrip','kernel','graph','device-copy','memset'):
                assert c['submissions'] == (1 if mode=='graph' else 2 if mode=='roundtrip' else 3)
                assert c['copied_bytes'] == size*(3 if mode=='device-copy' else 2)
                assert c['kernels'] == int(mode in ('kernel','graph'))
                assert c['checked_elements'] == size//4
                assert c['peak_outstanding'] == 1 and c['rejected'] == 0
                n = size//4
                expected_checksum = n*0x2a2a2a2a if mode=='memset' else n*((index+2)*7+int(mode in ('kernel','graph')))+3*n*(n-1)//2
                assert sample['checksum'] == expected_checksum
            elif mode in ('depth','not-ready-poll','timeout'):
                assert c['peak_outstanding'] == row['depth']
                assert c['submissions'] == row['depth']+1
                assert c['rejected'] == int(mode=='depth')
                assert c['timeouts'] == int(mode=='timeout')
            elif mode == 'empty-poll':
                assert c['polls'] == 1 and c['completions'] == c['submissions'] == 0
            elif mode in ('fault-submit','fault-context','fault-cleanup'):
                assert c['failed'] == c['submissions'] == c['peak_outstanding'] == 1
                assert c['resets'] == int(mode=='fault-submit')
                assert c['cleanup_retries'] == c['rejected'] == int(mode=='fault-cleanup')
                assert c['copied_bytes'] == c['checked_elements'] == c['kernels'] == 0
            elif mode in ('xdma-roundtrip','xdma-fault-cleanup','xdma-fault-reset'):
                assert c['submissions'] == c['completions'] == 2+int(mode=='xdma-fault-reset')
                assert c['copied_bytes'] == size*2 and c['checked_elements'] == size
                assert sample['checksum'] == sum(((index+2)*7+i*3)%251 for i in range(size))
                assert c['failed'] == int(mode!='xdma-roundtrip')
                assert c['cleanup_retries'] == int(mode=='xdma-fault-cleanup')
                assert c['resets'] == int(mode=='xdma-fault-reset')
            elif mode.startswith('pipeline-'):
                if not row['allocation_free']:
                    assert c['failed'] == c['resets'] == 1
                    assert c['checked_elements'] == c['copied_bytes'] == sample['checksum'] == 0
                else:
                    frames = row['depth']; n = size//4
                    assert c['submissions'] == c['completions'] == frames*2
                    assert c['copied_bytes'] == size*frames*5
                    assert c['kernels'] == frames and c['checked_elements'] == n*frames
                    assert sample['checksum'] == sum(n*(frame*7+1)+3*n*(n-1)//2 for frame in range((index+2)*frames+1,(index+3)*frames+1))
            elif mode == 'xdma-depth':
                assert c['peak_outstanding'] == row['depth'] and c['rejected'] == 1
                assert c['submissions'] == row['depth']+1 and c['copied_bytes'] == size*(row['depth']+1)
            elif mode in ('xdma-control','xdma-event','xdma-fault-event','xdma-fault-cancel'):
                assert c['submissions'] == c['completions'] == 1
                assert c['control_reads'] == c['control_writes'] == int(mode=='xdma-control')
                assert c['events'] == int(mode!='xdma-control')
                assert c['failed'] == int(mode.startswith('xdma-fault-'))
                assert c['timeouts'] == int(mode=='xdma-fault-event')
                assert c['canceled'] == int(mode=='xdma-fault-cancel')
                assert c['checked_elements'] == int(not mode.startswith('xdma-fault-'))
                assert sample['checksum'] == (0 if mode.startswith('xdma-fault-') else 0x12340000+index+2)
            elif mode in ('xdma-fault-io','xdma-fault-loss','xdma-fault-short'):
                assert c['submissions'] == c['completions'] == c['failed'] == 1
                assert c['copied_bytes'] == c['checked_elements'] == 0
            else:
                raise AssertionError(('missing independent oracle',mode))
        before = {f.name:f.read_bytes() for f in output.iterdir()}
        assert run(*args).returncode == 2
        assert before == {f.name:f.read_bytes() for f in output.iterdir()}
    missing = root/'unknown'
    assert run('run','--provider','rtfw.device','--case','unknown','--output',missing).returncode == 2
    assert not missing.exists()
print(f'Device inventory: {len(rows)} cases validated; real sessions absent: {sum(r["real"] for r in rows)}')
