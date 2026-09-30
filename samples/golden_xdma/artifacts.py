"""Independent XDMA golden execution validation, including actual backend counts."""
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
            for folder in ('golden_xdma','golden_cuda','golden_system') for x in (HERE.parent/folder).iterdir()
            if x.is_file() and x.name!='provenance.json'}
    require(p['files']==actual,'complete XDMA/CUDA/shared source inventory and digests')
    return p

def inspect(directory):
    import state
    d=contract();e=load(directory/'execution.json')
    fault=e['fault'];device=e['dispatch']!='cpu'
    require(e['dispatch'] in ('cpu','kernel','graph') and fault in ('none','underflow','overrun','stop_failure','device_loss','reset_failure'),'XDMA variant')
    require(e['variant']==('sim_combined' if device else 'sim_xdma') and e['physical']=='NOT_RUN','backend evidence identity')
    require(fault not in ('device_loss','reset_failure') or device,'CUDA fault variant')
    require(fault=='none' or (e['campaign']=='nominal' and not e['external'] and e['ticks']>=19),'fault configuration')
    recovered=e['campaign']=='overload' or fault in ('overrun','stop_failure','device_loss','reset_failure')
    for key,lo,hi in [('count',1,256),('ticks',1,1024),('workers',1,3),('grain',1,64)]:
        require(type(e[key]) is int and lo<=e[key]<=hi,'configuration '+key)
    require(e['grain'] in (1,4,16,64) and e['mode'] in ('native','host') and type(e['external']) is bool and e['campaign'] in CAMPAIGNS,'configuration enum')
    require(e['campaign']=='nominal' or e['ticks']>=19,'fault horizon')
    for key,value in e.items():
        if key not in ('contract_sha256','variant','dispatch','physical','fault','mode','campaign','external','session_identity','phase_calls','oracle','cleanup','sampled','peer_status','peer_cleanup'):
            require(type(value) is int and -(2**31)<=value<2**64,'execution integer '+key)
    require(isinstance(e['session_identity'],str) and re.fullmatch('[1-9][0-9]*-[1-9][0-9]*',e['session_identity']),'session identity')
    require(e['schema']==1 and e['contract_sha256']==FROZEN and e['runtime_id']>0,'execution identity')
    require(e['oracle'] is True and e['cleanup'] is True and e['trace_lost']==0 and e['trace_events']>0 and e['action_gaps']==0,'execution/cleanup/telemetry')
    require(0<e['runtime_planned_bytes']<=d['execution']['runtime_budget_bytes'] and 0<e['sample_owned_bytes']<=d['execution']['sample_budget_bytes'],'memory budgets')
    ticks=e['ticks'];t=ticks-1;sensor=t//2+1;actuator=t//3+1
    phase=[t//p+1 for p in (1,1,1,2,3,3,6,6)]
    require(type(e['phase_calls']) is list and all(type(x) is int for x in e['phase_calls']) and e['phase_calls']==phase,'logical phase counts')
    require(e['physical_phases']==10 and e['reference_releases']==32 and e['physical_callbacks']==sum(phase)+sensor+actuator,'physical graph counts')
    owners=1+int(recovered)
    require(e['memory_acquired']==e['memory_released']==3*owners and e['xdma_initializes']==e['xdma_shutdowns']==owners,'memory/driver conservation')
    require(e['jobs_accepted']==e['jobs_completed'] and (e['jobs_accepted']>0 if e['mode']=='host' else e['jobs_accepted']==0),'job conservation')
    controls=sum(ticks>b for b in (1,2,3,12,18))+int(e['campaign'] in ('stale_input','peer_missing','control_replaced'))+int(fault=='underflow')
    accepted=controls+int(e['campaign']=='control_replaced')
    require(e['control_committed']==controls and e['control_accepted']==accepted and e['control_invalid']==int(e['campaign']=='control_rejected') and e['control_replaced']==int(e['campaign']=='control_replaced'),'control outcomes')
    releases=sum(t//p+1 for p in (1,2,3,3,6))
    # Every release, actual terminal, sampled publication/selection and startup ACK.
    # A native provider also inspects its selected input via the public copy API.
    mixed=releases+2*ticks+4*sensor+6*actuator+2+device*ticks
    rate_extra=4 if fault=='stop_failure' else 2 if fault=='overrun' else int(recovered)
    mixed_extra=15 if fault=='stop_failure' else 8 if fault=='overrun' else 3 if fault in ('device_loss','reset_failure') else int(recovered)
    # The failed prefix includes one CUDA terminal when XDMA subsequently fails.
    if device and fault in ('overrun','stop_failure'):mixed_extra+=1
    control_extra=5 if fault in ('overrun','stop_failure') else 3*int(recovered)
    require(e['rate_actions']==releases+rate_extra and e['mixed_actions']==mixed+mixed_extra,'exact action counts')
    # Rate control boundaries bind the 32 immutable reference releases in
    # the first six ticks; host-frame boundaries continue for every tick.
    reference_boundaries=sum((min(ticks,6)-1)//p+1 for p in (1,1,1,2,3,3,6,6,2,3))
    control_actions=ticks+reference_boundaries+2*accepted+1+int(e['campaign']=='control_rejected')
    require(e['control_actions']==control_actions+control_extra,'exact control actions')
    require(e['deadline_failures']==int(e['campaign']=='overload') and e['device_failures']==int(fault in ('stop_failure','device_loss','reset_failure')),'failure counts')
    require(e['recoveries']==int(recovered) and e['replay_frames']==ticks-6*int(recovered) and e['replay_generations']==(2 if recovered else controls) and e['replay_actions']==(ticks-3 if recovered else control_actions),'replay counts')
    sp=2*sensor-3*int(recovered)+int(fault=='stop_failure')
    ap=2*actuator-2*int(recovered)+int(fault=='stop_failure')
    transfers=sp+ap+4*owners
    if fault=='stop_failure':transfers+=2
    exact={'sensor_providers':sp,'sensor_publications':sp,'actuator_providers':ap,'actuator_publications':ap-int(fault=='stop_failure'),
           'xdma_uploads':2*transfers,'xdma_controls':transfers,'xdma_downloads':transfers-2*int(fault=='stop_failure'),'xdma_events':transfers-2*int(fault=='stop_failure'),
           'xdma_safe_acks':4*owners+4*int(fault=='underflow')+int(fault=='stop_failure'),
           'fault_status':-18 if fault=='stop_failure' else -20 if fault=='device_loss' else -22 if fault=='reset_failure' else -5 if recovered else 0,
           'stop_status':-18 if fault=='stop_failure' else -19 if fault in ('device_loss','reset_failure') else 0,
           'reset_status':-20 if fault=='device_loss' else -22 if fault=='reset_failure' else 0,'reset_retry_status':0,
           'fault_safety':int(fault=='stop_failure'),'fault_overruns':int(fault=='overrun'),'retained_regions':3*int(fault in ('stop_failure','device_loss','reset_failure'))}
    cuda_steps=(2*ticks-6*int(recovered)+int(fault in ('overrun','stop_failure','device_loss','reset_failure'))) if device else 0
    exact.update(cuda_providers=cuda_steps,cuda_publications=cuda_steps-int(fault in ('device_loss','reset_failure')),cuda_uploads=2*cuda_steps,cuda_downloads=cuda_steps,cuda_events=cuda_steps,cuda_kernels=cuda_steps if e['dispatch']=='kernel' else 0,cuda_graphs=cuda_steps if e['dispatch']=='graph' else 0,cuda_faults=2 if fault=='device_loss' else 3 if fault=='reset_failure' else 0)
    for key,value in exact.items():require(e[key]==value,'exact backend/fault count '+key)
    require(type(e['sampled']) is list and len(e['sampled'])==4,'sampled inventory')
    for i,c in enumerate(e['sampled']):
        under=2 if i==2 and fault=='underflow' else 0
        expected=dict(identity=26001+i,ring=4,header_bytes=120,safe_timeout_ns=8000000 if i%2==0 else 0,accepted=[ticks,sensor,actuator,actuator][i]+1-under,sequence=[ticks,sensor,actuator,actuator][i]+1,stale=0,overruns=0,underruns=under,substituted=under,safety=3 if i%2==0 else 0,status=0)
        require(all(type(v) is int for v in c.values()) and c==expected,'sampled metadata/safety '+str(i))
    require(0<=e['peer_responses']<=actuator,'peer count')
    if not e['external']:require(e['peer_responses']==0 and e['peer_status']==e['peer_cleanup']=='ok','nonexternal claims')
    elif e['peer_status']=='ok':require(e['peer_responses']==actuator,'peer completeness')
    state.inspect_state(directory,e)
    return e

def expected(directory,p):
    e=inspect(directory)
    configuration={key:e[key] for key in CONFIG}
    identity={'contract_sha256':FROZEN,'configuration_sha256':sha(canonical(configuration))}
    files={name:sha((directory/name).read_bytes()) for name in FILES}
    good=e['peer_status']=='ok' and e['peer_cleanup']=='ok'
    run=dict(identity,session_identity=e['session_identity'],runtime_id=e['runtime_id'],
             source_commit=p['source_commit'],source_tree=p['source_tree'],source_dirty=p['dirty'],source_files=p['files'],
             variant=e['variant'],dispatch=e['dispatch'],host=e['mode'],evidence='injected_driver_protocol',physical='NOT_RUN',
             status='PASS' if good else 'FAIL',configuration=configuration,phase_calls=e['phase_calls'],
             state_digest=files['state.bin'],oracle='independent_every_field_exact',cleanup=e['cleanup'] and e['peer_cleanup']=='ok',files=files,
             backend={k:v for k,v in e.items() if k.startswith(('xdma_','cuda_','sensor_','actuator_','fault_','reset_','stop_','sampled','physical_'))})
    replay=dict(identity,sample_snapshot_digest=files['state.bin'],runtime_snapshot_digest=files['checkpoint.bin'],
                trusted_payload_digest=files['trusted.bin'],action_digest=files['active.bin'],gap_count=e['trace_lost'],
                replay_status='PASS',frames=e['replay_frames'],actions=e['replay_actions'],generations=e['replay_generations'],
                ownership='originating_runtime',recovery='fresh_owner_checkpoint_resume' if e['recoveries'] else 'not_required',
                backend='sample_owned_deterministic_simulator')
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
    print('PASS: independent XDMA golden state/backend/replay validation')
if __name__=='__main__':main()
