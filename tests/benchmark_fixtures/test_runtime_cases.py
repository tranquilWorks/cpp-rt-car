#!/usr/bin/env python3
"""Runtime metadata, every fixture, truthful artifacts and unchanged validator."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--cli',type=Path,required=True);p.add_argument('--inventory',type=Path,required=True)
a=p.parse_args();cli=a.cli.resolve()
spec=importlib.util.spec_from_file_location('validator',ROOT/'tools/check_benchmark_artifact.py')
validator=importlib.util.module_from_spec(spec);spec.loader.exec_module(validator)
rows=json.loads(a.inventory.read_text())['cases']
ids=[r['id'] for r in rows];assert 1<=len(ids)<=96 and ids==sorted(set(ids))
assert {r['family'] for r in rows}=={'rates','channels','shedding','controls','checkpoint','replay','watchdog','telemetry','capacity','device','composition'}
cpu=json.loads((ROOT/'bench/fixtures/cpu_cases.json').read_text())['cases']
def command(*args):
    return subprocess.run([str(cli),*map(str,args)],capture_output=True,text=True,timeout=600)
listing=command('list');assert listing.returncode==0
assert listing.stdout.splitlines()==sorted(['rtfw.runtime:'+i for i in ids]+['rtfw.cpu:'+r['id'] for r in cpu]+['rtfw.self:structural'])
with tempfile.TemporaryDirectory(prefix='m23-runtime-') as tmp:
    root=Path(tmp)
    for row in rows:
        id=row['id'];description=command('describe','--provider','rtfw.runtime','--case',id,'--clock','fake')
        assert description.returncode==0,(id,description.stderr)
        d=json.loads(description.stdout)
        assert d['configuration']==row['measurement_scope'] and d['warmup']==2 and d['repetitions']==5
        assert [x['name'] for x in d['counters']]==row['counter_names'] and d['counters'][0]['unit']==row['operation_unit']
        assert {x['name']:x['value'] for x in d['parameters']}=={k:row[k] for k in ('count','width','bytes','capacity','variant')}
        assert all(row[k] for k in ('oracle','clock_semantics','counter_semantics','evidence_boundary','configured_limits'))
        output=root/id;ran=command('run','--provider','rtfw.runtime','--case',id,'--clock','fake','--output',output)
        assert ran.returncode==0,(id,ran.stdout,ran.stderr)
        result=validator.validate_bundle(output)
        assert result['status']=='ok' and result['warmup_completed']==2 and result['measured_completed']==5
        assert result['statistics']['total_ns']==500 and result['evidence_class']=='structural_fixture'
        samples=json.loads((output/'raw.json').read_text())['samples']
        for sample in samples:
            assert sample['invariants_passed'];c=sample['counters']
            if row['family']=='rates' and row['mode']=='dispatch':
                assert c['operations']==row['count']*row['width']*row['variant']==c['callbacks']==c['records']
            if row['family']=='controls':
                assert c['operations']==row['count'] and c['callbacks']==1 and c['records']==row['width']
            if row['family']=='watchdog':
                assert c['operations']==4 and c['callbacks']==4 and c['records']==(4 if row['variant']==2 else 0)
        before={p.name:p.read_bytes() for p in output.iterdir()}
        assert command('run','--provider','rtfw.runtime','--case',id,'--output',output).returncode==2
        assert before=={p.name:p.read_bytes() for p in output.iterdir()}
    missing=root/'missing';assert command('run','--provider','rtfw.runtime','--case','missing','--output',missing).returncode==2
    assert not missing.exists()
    steady=root/'steady';ran=command('run','--provider','rtfw.runtime','--case','rate-dispatch-8-d1-s1','--clock','steady','--output',steady)
    assert ran.returncode==0 and validator.validate_bundle(steady)['evidence_class']=='portable_characterization'
print(f'Runtime inventory: {len(rows)} cases validated')
