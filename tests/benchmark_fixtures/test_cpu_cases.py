#!/usr/bin/env python3
"""Bounded CPU inventory, independent observations and unchanged artifact validation."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser()
p.add_argument('--cli',type=Path,required=True)
p.add_argument('--inventory',type=Path,required=True)
a=p.parse_args();cli=a.cli.resolve()
spec=importlib.util.spec_from_file_location('validator',ROOT/'tools/check_benchmark_artifact.py')
validator=importlib.util.module_from_spec(spec);spec.loader.exec_module(validator)
catalog=json.loads(a.inventory.read_text())
rows=catalog['cases'];assert 1<=len(rows)<=64
ids=[r['id'] for r in rows];assert ids==sorted(set(ids))
expected_families={'compile','empty_dispatch','graph_shape','granularity','workers','nested','entities',
                   'queue_pressure','host_adapter','trace','memory','residency','multiple','lifecycle'}
assert {r['family'] for r in rows}==expected_families
def command(*args):
    return subprocess.run([str(cli),*map(str,args)],capture_output=True,text=True,timeout=30)
listing=command('list');assert listing.returncode==0,listing.stderr
assert listing.stdout.splitlines()==sorted(['rtfw.cpu:'+i for i in ids]+['rtfw.self:structural'])
for row in rows:
    if 'native_counterpart' in row:
        native=next(c for c in rows if c['id']==row['native_counterpart'])
        for key in ('phases','entities','grain','workers','queue','trace','scratch','kind'):
            assert row[key]==native[key],(row['id'],key)
skipped=[]
with tempfile.TemporaryDirectory(prefix='m23-cpu-') as td:
    root=Path(td)
    for row in rows:
        id=row['id']
        described=command('describe','--provider','rtfw.cpu','--case',id,'--clock','fake')
        assert described.returncode==0,(id,described.stderr)
        d=json.loads(described.stdout)
        assert d['provider_id']=='rtfw.cpu' and d['case_id']==id
        assert d['configuration']==row['measurement_scope']
        assert [c['name'] for c in d['counters']]==row['counter_names']
        assert d['counters'][0]['unit']==row['operation_unit']
        assert d['warmup']==2 and d['repetitions']==5 and d['comparison_policy']=='none'
        params={x['name']:x['value'] for x in d['parameters']}
        for name in ('phases','entities','grain','workers','depth','queue','trace','scratch','cycles','chain','invalid_graph'):
            assert params[name]==row[name],(id,name)
        dest=root/id
        ran=command('run','--provider','rtfw.cpu','--case',id,'--clock','fake','--output',dest)
        assert ran.returncode in (0,3),(id,ran.stdout,ran.stderr)
        result=validator.validate_bundle(dest)
        if ran.returncode==3:
            assert row['memory']=='native' and result['status']=='not_run',id
            skipped.append(id);continue
        raw=json.loads((dest/'raw.json').read_text());samples=raw['samples']
        assert result['warmup_completed']==2 and result['measured_completed']==5
        assert result['statistics']['total_ns']==500 and result['statistics']['p99_ns']==100
        for sample in samples:
            counters=sample['counters'];assert sample['invariants_passed'],id
            assert counters['operations']==row['expected_operations'],id
            if row['kind']=='pressure':
                assert counters['operations']==row['queue'] and counters['rejected']==1
                assert counters['submitted']==row['queue']+2
            elif row['kind']=='compile':
                assert counters['operations']==(0 if row['invalid_graph'] else row['phases'])
            elif row['memory'] in ('acquire_failure','apply_failure'):
                assert counters['operations']==0
            else:
                expected=row['entities']*row['phases']*row['cycles']*(2 if row['kind']=='multiple' else 1)
                assert counters['operations']==expected,(id,counters,expected)
            if row['kind']=='memory':
                assert counters['acquired']==counters['released']
                assert counters['applied']==counters['rolled_back']
        original={x.name:x.read_bytes() for x in dest.iterdir()}
        collision=command('run','--provider','rtfw.cpu','--case',id,'--output',dest)
        assert collision.returncode==2 and original=={x.name:x.read_bytes() for x in dest.iterdir()}
    unknown=root/'missing'
    assert command('run','--provider','rtfw.cpu','--case','missing','--output',unknown).returncode==2
    assert not unknown.exists()
    steady=root/'steady'
    ran=command('run','--provider','rtfw.cpu','--case','host-adapter-64','--clock','steady','--output',steady)
    assert ran.returncode==0,ran.stderr
    result=validator.validate_bundle(steady)
    assert result['evidence_class']=='portable_characterization'
print(f'CPU inventory: {len(rows)} cases validated; native NOT RUN: {skipped}')
