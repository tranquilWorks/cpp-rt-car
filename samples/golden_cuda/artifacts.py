"""Independent CUDA golden execution validation, including actual backend counts."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import sys
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('golden_cpu_artifacts',HERE.parent/'golden_system/artifacts.py')
cpu=importlib.util.module_from_spec(spec);spec.loader.exec_module(cpu)
require,load,sha,canonical,contract=cpu.require,cpu.load,cpu.sha,cpu.canonical,cpu.contract
FROZEN,FILES,CAMPAIGNS=cpu.FROZEN,cpu.FILES,cpu.CAMPAIGNS
CONFIG=cpu.CONFIG+('dispatch','fault')

def provenance(path):
    p=load(path)
    require(set(p)=={'schema','source_commit','source_tree','dirty','files'},'provenance fields')
    require(p['schema']==1 and type(p['dirty']) is bool,'provenance schema')
    for key in ('source_commit','source_tree'):
        require(isinstance(p[key],str) and re.fullmatch('[0-9a-f]{40}',p[key]),'source identity')
    require(isinstance(p['files'],dict),'source inventory')
    actual={str(x.relative_to(HERE.parent)).replace('\\','/'):sha(x.read_bytes())
            for folder in ('golden_cuda','golden_system') for x in (HERE.parent/folder).iterdir()
            if x.is_file() and x.name!='provenance.json'}
    require(p['files']==actual,'complete CUDA/shared source inventory and digests')
    return p

def inspect(directory):
    d = contract()
    e = load(directory/'execution.json')
    fault=e['fault']; device=e['dispatch']!='cpu'
    require(e['dispatch'] in ('cpu','kernel','graph') and fault in ('none','device_loss','reset_failure'), 'CUDA variant')
    require(e['variant']==('sim_cuda' if device else 'cpu'), 'backend evidence identity')
    require(fault=='none' or (device and e['campaign']=='nominal' and not e['external'] and e['ticks']>=19), 'fault configuration')
    recovered=e['campaign']=='overload' or fault!='none'
    for key, lo, hi in [('count',1,256),('ticks',1,1024),('workers',1,3),('grain',1,64)]:
        require(type(e[key]) is int and lo <= e[key] <= hi, 'configuration '+key)
    require(e['grain'] in (1,4,16,64) and e['mode'] in ('native','host') and type(e['external']) is bool and e['campaign'] in CAMPAIGNS, 'configuration enum')
    require(e['campaign'] == 'nominal' or e['ticks'] >= 19, 'fault horizon')
    numeric=('schema','runtime_id','runtime_planned_bytes','sample_owned_bytes','trace_events','trace_lost','control_accepted','control_invalid','control_replaced','control_committed','replay_frames','replay_actions','replay_generations','recoveries','jobs_accepted','jobs_completed','memory_acquired','memory_released','peer_responses','rate_actions','mixed_actions','control_actions','action_gaps','deadline_failures')
    for key in numeric:require(type(e[key]) is int and 0 <= e[key] < 2**64, 'execution integer '+key)
    require(isinstance(e['session_identity'],str) and re.fullmatch('[1-9][0-9]*-[1-9][0-9]*',e['session_identity']), 'session identity')
    require(isinstance(e['phase_calls'],list) and all(type(x) is int for x in e['phase_calls']), 'phase integer counts')
    require(e['schema'] == 1 and e['contract_sha256'] == FROZEN, 'execution identity')
    require(e['oracle'] is True and e['cleanup'] is True and e['trace_lost'] == 0 and e['trace_events'] > 0, 'execution/cleanup/telemetry')
    require(0 < e['runtime_planned_bytes'] <= d['execution']['runtime_budget_bytes'] and 0 < e['sample_owned_bytes'] <= d['execution']['sample_budget_bytes'], 'memory budgets')
    require(e['action_gaps']==0 and e['deadline_failures']==(1 if e['campaign']=='overload' else 0) and all(e[k]>0 for k in ('rate_actions','mixed_actions','control_actions')), 'action telemetry')
    require(e['memory_acquired'] == e['memory_released'] == (6 if recovered else 3), 'memory conservation')
    require(e['jobs_accepted'] == e['jobs_completed'] and (e['mode'] != 'host' or e['jobs_accepted'] > 0), 'job conservation')
    ticks=e['ticks'];t=ticks-1
    require(e['phase_calls'] == [t//p+1 for p in (1,1,1,2,3,3,6,6)], 'phase counts')
    require(e['recoveries'] == int(recovered) and e['replay_frames'] == ticks-(6 if recovered else 0), 'replay/recovery count')
    require(e['control_invalid'] == (1 if e['campaign']=='control_rejected' else 0) and e['control_replaced'] == (1 if e['campaign']=='control_replaced' else 0), 'admission outcomes')
    releases=sum(t//period+1 for period in (1,2,3,3,6))+e['recoveries']
    require(e['rate_actions']==releases and e['mixed_actions']==releases+(ticks+int(fault!='none') if device else 0), 'exact rate/mixed action counts')
    controls = sum(ticks>b for b in (1,2,3,12,18))+(e['campaign'] in ('stale_input','peer_missing','control_replaced'))
    require(e['control_committed'] == controls and e['control_accepted'] == controls+e['control_replaced'], 'control counts')
    require(e['replay_generations'] == (2 if recovered else controls) and e['replay_actions'] > 0, 'replay transcript')
    require(type(e['peer_responses']) is int and 0 <= e['peer_responses'] <= t//3+1, 'peer count')
    if not e['external']: require(e['peer_responses']==0 and e['peer_status']=='ok' and e['peer_cleanup']=='ok', 'nonexternal peer claims')
    elif e['peer_status']=='ok': require(e['peer_responses']==t//3+1, 'peer completeness')
    steps=(2*ticks-6*int(recovered)+int(fault!='none')) if device else 0
    publications=steps-int(fault!='none')
    owners=1+int(recovered)
    exact={'device_providers':steps,'device_publications':publications,'device_uploads':2*steps,
           'device_copies':steps,'device_downloads':steps,'device_events':steps,
           'device_kernels':steps if e['dispatch']=='kernel' else 0,
           'device_graphs':steps if e['dispatch']=='graph' else 0,
           'device_registrations':2*owners if device else 0,'device_unregistrations':2*owners if device else 0,
           'event_creates':owners if device else 0,'event_destroys':owners if device else 0,'device_allocations':0,'device_frees':0,
           'device_timeline':2*(ticks-6*int(recovered)) if device else 0,
           'device_faults':0 if fault=='none' else 2 if fault=='device_loss' else 3,
           'device_failures':int(fault!='none'),
           'fault_status':(-5 if recovered else 0) if fault=='none' else -20 if fault=='device_loss' else -22,
           'reset_status':0 if fault=='none' else -20 if fault=='device_loss' else -22,
           'reset_retry_status':0,'stop_status':-19 if fault!='none' else 0,
           'fault_health':0 if fault=='none' else 4 if fault=='device_loss' else 3,
           'fault_outstanding':int(fault!='none')}
    for key,value in exact.items():require(type(e[key]) is int and e[key]==value,'exact backend/fault count '+key)
    require(e['device_protocol'] is True, 'driver protocol/cleanup')
    cpu.inspect_state(directory,e)
    return e

def expected(directory,p):
    e=inspect(directory)
    configuration={key:e[key] for key in CONFIG}
    identity={'contract_sha256':FROZEN,'configuration_sha256':sha(canonical(configuration))}
    files={name:sha((directory/name).read_bytes()) for name in FILES}
    good=e['peer_status']=='ok' and e['peer_cleanup']=='ok'
    run=dict(identity,session_identity=e['session_identity'],runtime_id=e['runtime_id'],
             source_commit=p['source_commit'],source_tree=p['source_tree'],source_dirty=p['dirty'],source_files=p['files'],
             variant=e['variant'],dispatch=e['dispatch'],host=e['mode'],evidence='injected_driver_protocol' if e['dispatch']!='cpu' else 'portable_cpu',
             status='PASS' if good else 'FAIL',configuration=configuration,phase_calls=e['phase_calls'],
             state_digest=files['state.bin'],oracle='independent_every_field_exact',cleanup=e['cleanup'] and e['peer_cleanup']=='ok',files=files,
             backend={k:v for k,v in e.items() if k.startswith(('device_','event_','fault_','reset_','stop_'))})
    replay=dict(identity,sample_snapshot_digest=files['state.bin'],runtime_snapshot_digest=files['checkpoint.bin'],
                trusted_payload_digest=files['trusted.bin'],action_digest=files['active.bin'],gap_count=e['trace_lost'],
                replay_status='PASS',frames=e['replay_frames'],actions=e['replay_actions'],generations=e['replay_generations'],
                ownership='originating_runtime',recovery='fresh_owner_checkpoint_resume' if e['recoveries'] else 'not_required',
                backend='sample_owned_deterministic_simulator' if e['dispatch']!='cpu' else 'cpu')
    return run,replay

def validate(directory, provenance_path, publish=False):
    p=provenance(provenance_path);run,replay=expected(directory,p)
    if publish:
        for name,value in [('run.json',run),('replay.json',replay)]:
            (directory/name).write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8')
    require(load(directory/'run.json')==run, 'run artifact mismatch')
    require(load(directory/'replay.json')==replay, 'replay artifact mismatch')
    return run

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);p.add_argument('--provenance',type=Path,default=HERE/'provenance.json');p.add_argument('--publish',action='store_true');a=p.parse_args()
    try:
        result=validate(a.directory,a.provenance,a.publish)
        require(result['status']=='PASS','retained failed execution')
    except (ValueError,OSError,KeyError,TypeError) as e:p.exit(1,str(e)+'\n')
    print('PASS: independent CUDA golden state/backend/replay validation')
if __name__=='__main__':main()
