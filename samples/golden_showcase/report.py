"""Recompute showcase coverage; reports never certify their own source inputs."""
import argparse
import json
from pathlib import Path
import artifacts as a


def experiment(path,lever,value):
    e=a.load(path)
    a.require(e['schema']==1 and e['lever']==lever and e['requested']==e['effective']==value,'requested/effective lever')
    a.require(e['correct'] is True and e['cleanup'] is True,'experiment failed/unclean')
    for key,item in e.items():
        if key not in ('lever','scope','correct','cleanup','status','has_state'):a.require(type(item) is int and 0<=item<2**64,'experiment integer')
    a.require(type(e['status']) is int,'experiment status')
    if lever in ('workers','grain','cuda_graph'):
        a.require(e['scope']=='frozen-golden-loop' and e['operations']==24 and e['calls']==108 and e['checks']==18432 and e['status']==0,'golden experiment workload')
        a.require(e['runtime_id']>0 and e['config_id']>0 and e['bytes']>0,'golden experiment identities')
        if lever=='cuda_graph':a.require(e['accepted']==24,'actual CUDA dispatches')
    elif lever=='control_burst':
        a.require(e['scope']=='public-control-boundary' and e['operations']==value and e['accepted']==min(16,value) and e['rejected']==max(0,value-16) and e['replaced']==min(16,value)-1,'control sequence conservation')
        a.require(e['calls']==8 and e['checks']==768 and e['status']==0,'control boundary')
    elif lever in ('device_depth','transfer_batch','staging_slots'):
        depth=value if lever=='device_depth' else 2
        batch=value if lever=='transfer_batch' else 4
        slots=value if lever=='staging_slots' else 2
        a.require(e['scope']=='public-hal-v2-loopback-batch-copy' and e['operations']==64 and e['bytes']==65536,'equal HAL payload work')
        a.require(e['accepted']==e['calls']==64//batch and e['rejected']==64//batch//depth and e['capacity']==depth and e['scratch']==slots and e['checks']==slots*1024 and e['status']==0,'actual HAL capacities/copies')
    else:
        a.require(e['scope']=='public-runtime-task-rate-telemetry' and e['checks']==64 and e['runtime_id']>0 and e['config_id']>0,'public Runtime experiment')
        rejected=lever=='queue_slots' and value==4
        failed=lever=='overload_policy' and value==0
        a.require(e['operations']==(1 if rejected else 7 if failed else 24) and e['calls']==(1 if rejected else 6 if failed else 24),'bounded work/failure count')
        a.require(e['status']==(-5 if rejected or failed else 0) and e['rejected']==int(rejected or failed),'failure outcome retained')
        if rejected:a.require(0<=e['checksum']<=2080,'bounded partial task execution')
        else:a.require(e['checksum']==e['calls']*2080,'task scalar oracle')
        a.require(e['capacity']==(value if lever=='queue_slots' else 32) and e['scratch']==(value if lever=='scratch_bytes' else 64),'effective queue/scratch')
        if lever=='telemetry_capacity':
            # Zero capacity is disabled capture; the raw sequence gap is
            # retained as unavailable events, never classified as pressure.
            a.require(e['gaps']==(96 if value==0 else 32 if value==64 else 0),'trace cursor accounting')
    has_state=lever in ('workers','grain','cuda_graph','control_burst')
    a.require(e['has_state'] is has_state,'experiment state inventory')
    if has_state:
        config=dict(count=16,ticks=24,workers=value if lever=='workers' else 2,grain=value if lever=='grain' else 4,mode='native',external=False,campaign='nominal',peer_responses=0,phase_calls=[24,24,24,12,8,8,4,4])
        data=Path(str(path)+'.state.bin').read_bytes()
        a.require(a.fnv(data)==e['checksum'],'experiment actual state binding')
        if lever!='control_burst':
            state=a.module('showcase_experiment_state',a.HERE/'state.py')
            state.inspect_state(path.parent,config,path.name+'.state.bin')
        else:
            import struct
            a.state_header(data,config)
            body,sums,configuration,stale,missing=a.cpu.reference(dict(config,ticks=1))
            a.require(list(struct.unpack_from('<4608i',data,512))==body,'control burst complete state oracle')
            a.require(struct.unpack_from('<4i',data,40)==configuration and struct.unpack_from('<Q',data,56)[0]==1 and struct.unpack_from('<Q',data,64)[0]>0,'control burst state boundary')
            a.require(struct.unpack_from('<8Q',data,72)==(1,)*8 and struct.unpack_from('<5Q',data,136)==(1,)*5 and struct.unpack_from('<5Q',data,176)==(1,)*5,'control burst phase/channel counts')
            a.require(not any(data[216:256]) and struct.unpack_from('<5Q',data,256)==(2,2,2,1,2) and not any(data[296:392]),'control burst selection metadata')
            a.require(struct.unpack_from('<Q',data,392)[0]==1 and struct.unpack_from('<6q',data,400)==tuple(sums),'control burst survivor state')
        e=dict(e,state_sha256=a.sha(data))
    e=dict(e,record_sha256=a.sha(path.read_bytes()),outcome='bounded_rejection' if e['status'] else 'completed')
    e['measurement']='disabled_capture' if lever=='telemetry_capacity' and value==0 else 'actual_public_api_execution'
    return e

def telemetry(directory):
    e=a.load(directory/'evidence.json')
    a.require(e['schema']==1 and e['fault']=='telemetry_loss' and e['injected_tick']==6 and e['detected_tick']==12 and e['capacity']==64,'telemetry fault boundary')
    a.require(e['lost']==20 and e['first_sequence']-e['consumer_before']==e['lost'] and e['next_sequence']-e['first_sequence']==64 and e['emitted']==266,'exact real retention gap')
    a.require(e['refusal_status']==-6 and e['state_unchanged'] is True and e['replay_calls_before_refusal']==0,'pre-effect replay refusal')
    a.require(e['original_runtime']>0 and e['recovery_runtime']>0 and e['original_runtime']!=e['recovery_runtime'] and e['recovery_frames']==18,'fresh-owner recovery')
    a.require(e['cleanup'] is True and e['correct'] is True and e['memory_acquired']==e['memory_released']==6,'fault cleanup')
    state=a.module('showcase_loss_state',a.HERE/'state.py')
    config=dict(count=16,ticks=24,workers=2,grain=4,mode='native',external=False,campaign='nominal',peer_responses=0,phase_calls=[24,24,24,12,8,8,4,4])
    state.inspect_state(directory,config)
    a.replay_pair(directory,dict(runtime_id=e['original_runtime']))
    trusted=(directory/'recovery_trusted.bin').read_bytes()
    a.require(trusted[:8]==b'RTFWLCR2' and len(trusted)>=400,'recovery schema')
    import struct
    word=lambda n:struct.unpack_from('<Q',trusted,n)[0]
    a.require(word(32)==e['recovery_runtime'] and word(16)==len(trusted),'fresh replay pairing')
    checked=bytearray(trusted);checked[24:32]=bytes(8)
    a.require(a.fnv(checked)==word(24),'recovery checksum')
    for name,off,size in [('recovery_checkpoint.bin',96,104),('recovery_active.bin',120,128)]:
        a.require(trusted[word(off):word(off)+word(size)]==(directory/name).read_bytes(),'fresh nested pairing')
    return e

def validate(directory,provenance,publish=False,binaries=None):
    a.require(not directory.is_symlink() and all(not path.is_symlink() for path in directory.rglob('*')),'symlink evidence is not admitted')
    source=a.provenance(provenance);inputs=a.load(directory/'inputs.json')
    a.require(inputs['schema']==1 and inputs['source']==source and inputs['clock'] in ('fake','steady'),'run source/clock binding')
    names={'golden_showcase','golden_experiment','golden_telemetry_loss','golden_system','golden_cuda','golden_xdma','golden_controller'}
    a.require(set(inputs['executables'])==names and binaries is not None and set(binaries)==names,'provide all seven actual executables for byte binding')
    a.require(inputs['executables']=={name:a.sha(path.read_bytes()) for name,path in binaries.items()},'executable byte binding')
    expected=set(a.IDS)|{f'{variant}-{dispatch}-{mode}' for variant,dispatch in [('sim_cuda','kernel'),('sim_cuda','graph'),('sim_xdma','cpu'),('sim_combined','kernel'),('sim_combined','graph')] for mode in ('native','host')}
    a.require({x.name for x in (directory/'benchmarks').iterdir()}==expected,'benchmark matrix coverage')
    benchmarks={name:a.benchmark(directory/'benchmarks'/name) for name in sorted(expected)}
    a.require(all(v['clock']==inputs['clock'] for v in benchmarks.values()),'mixed clock evidence')
    a.require(all(v['identity']['source_commit']==source['source_commit'] and v['identity']['source_tree']==source['source_tree'] for v in benchmarks.values()),'benchmark/source commit and tree binding')
    contract=a.cpu.contract();experiments=[]
    expected_experiments={f'{lever["id"]}-{value}.json' for lever in contract['levers'] for value in lever['values']}
    expected_experiments|={name+'.state.bin' for name in list(expected_experiments) if name.split('-')[0] in ('workers','grain','cuda_graph','control_burst')}
    a.require({x.name for x in (directory/'experiments').iterdir()}==expected_experiments,'every frozen lever/value')
    for lever in contract['levers']:
        for value in lever['values']:experiments.append(experiment(directory/'experiments'/f'{lever["id"]}-{value}.json',lever['id'],value))
    faults={};expected_faults={v['id'] for v in contract['faults']}
    a.require({x.name for x in (directory/'faults').iterdir()}==expected_faults,'every frozen fault')
    for name in sorted(expected_faults):
        path=directory/'faults'/name
        if name=='telemetry_loss':e=telemetry(path)
        elif name in ('underflow','overrun','stop_failure','device_loss','reset_failure'):e=a.variant_inspect(path,'sim_combined')
        else:e=a.cpu.inspect(path)
        faults[name]=dict(execution=e,files={p.name:a.sha(p.read_bytes()) for p in path.iterdir() if p.is_file()})
    external={}
    expected_external={f'{kit}-{dispatch}' for kit,ds in [('golden_cuda',('kernel','graph')),('golden_xdma',('cpu','kernel','graph'))] for dispatch in ds}
    a.require({x.name for x in (directory/'external').iterdir()}==expected_external,'external variant matrix')
    for name in sorted(expected_external):
        path=directory/'external'/name
        e=a.variant_inspect(path,'sim_cuda' if name.startswith('golden_cuda') else 'sim_xdma' if name.endswith('cpu') else 'sim_combined')
        a.require(e['external'] is True and e['peer_responses']==8,'actual external responses')
        external[name]=dict(execution=e,files={p.name:a.sha(p.read_bytes()) for p in path.iterdir() if p.is_file()})
    coverage=dict(schema=1,contract_sha256=a.FROZEN,batch='M26-05',status='PASS',source=source,executables=inputs['executables'],benchmark_cases=list(a.IDS),benchmark_runs=25,raw_measured_samples=125,warmup_invocations=50,levers={v['id']:v['values'] for v in contract['levers']},faults=sorted(expected_faults),external_variants=sorted(external),final_cap_m26_audit='M26-06_NOT_RUN',physical='NOT_RUN',controlled_performance='NOT_RUN',rt_qualification='NOT_RUN')
    report=dict(coverage=coverage,benchmarks=benchmarks,experiments=experiments,external=external)
    for name,value in [('report.json',report),('faults.json',faults),('coverage.json',coverage)]:
        path=directory/name
        if publish:a.require(not path.exists(),'report exists');path.write_text(json.dumps(value,sort_keys=True,indent=2)+'\n')
        else:a.require(a.load(path)==value,'fabricated or stale '+name)
    return coverage

def main():
    p=argparse.ArgumentParser();p.add_argument('directory',type=Path);p.add_argument('--provenance',type=Path,default=a.HERE/'provenance.json');p.add_argument('--binaries-build',type=Path,required=True);args=p.parse_args()
    import run
    names=('golden_showcase','golden_experiment','golden_telemetry_loss','golden_system','golden_cuda','golden_xdma','golden_controller')
    try:validate(args.directory,args.provenance,binaries={name:run.executable(args.binaries_build,name) for name in names})
    except (OSError,ValueError,KeyError,TypeError) as e:p.exit(1,str(e)+'\n')
    print('PASS independently recomputed report, faults, coverage and executable/source binding')
if __name__=='__main__':main()
