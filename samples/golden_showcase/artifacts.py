"""Independently bind showcase observations to source, state and raw M23 bundles."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import struct
import sys

HERE=Path(__file__).resolve().parent

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value)
    return value

cpu=module('showcase_cpu_artifacts',HERE.parent/'golden_system/artifacts.py')
require,load,sha,canonical,fnv=cpu.require,cpu.load,cpu.sha,cpu.canonical,cpu.fnv
FROZEN=cpu.FROZEN
IDS=tuple('golden-'+x for x in ('input','physics','stage','sensor','controller','actuator','aggregate','telemetry','host','memory','rates','controls','replay','external','loop'))
SUBSYSTEMS=('scenario','physics','host_staging','sampled_io','controller','hal','aggregation','observability','executor','memory','multi_rate','live_control','checkpoint_replay','external_cil','whole_loop')
COUNTERS=('operations','phase_calls','entity_checks','faults','gaps','cleanup_failures')
FOLDERS=('golden_showcase','golden_xdma','golden_cuda','golden_system')

def provenance(path):
    p=load(path)
    require(set(p)=={'schema','source_commit','source_tree','dirty','files'} and p['schema']==1 and type(p['dirty']) is bool,'provenance schema')
    for key in ('source_commit','source_tree'):require(isinstance(p[key],str) and re.fullmatch('[0-9a-f]{40}',p[key]),'source identity')
    actual={str(x.relative_to(HERE.parent)).replace('\\','/'):sha(x.read_bytes()) for folder in FOLDERS for x in (HERE.parent/folder).iterdir() if x.is_file() and x.name!='provenance.json'}
    require(p['files']==actual,'complete showcase/shared source inventory and digests')
    cpu.contract()
    return p

def benchmark_tool():
    root=HERE.parents[1]
    if (root/'tools/check_benchmark_artifact.py').is_file():
        return module('showcase_benchmark_validator',root/'tools/check_benchmark_artifact.py'),root/'bench/schemas'
    root=HERE.parents[1] # installed .../rtfw/examples/golden_showcase -> .../rtfw
    return module('showcase_benchmark_validator',root/'tools/check_benchmark_artifact.py'),root/'bench/schemas'

def variant_inspect(directory,variant):
    folder='golden_cuda' if variant=='sim_cuda' else 'golden_xdma'
    validator=module('showcase_'+folder,HERE.parent/folder/'artifacts.py')
    old=sys.modules.get('state')
    try:
        if folder=='golden_xdma':sys.modules['state']=module('showcase_xdma_state',HERE.parent/folder/'state.py')
        return validator.inspect(directory)
    finally:
        if old is None:sys.modules.pop('state',None)
        else:sys.modules['state']=old

def state_header(data,e):
    require(len(data)==18944,'state size')
    require(struct.unpack_from('<10I',data)==(0x5336324d,1,e['count'],e['ticks'],e['workers'],e['grain'],int(e['mode']=='host'),int(e['external']),cpu.CAMPAIGNS.index(e['campaign']),0),'state configuration')
    require(data[448:480].hex()==FROZEN and not any(data[488:512]),'state contract/reserved')
    checked=bytearray(data);checked[480:488]=bytes(8)
    require(fnv(checked)==struct.unpack_from('<Q',data,480)[0],'state checksum')

def probe_state(directory,e,kind):
    data=(directory/'state.bin').read_bytes();state_header(data,e)
    n,ticks=e['count'],e['ticks'];seed=1
    fields=[[[0]*256 for _ in range(3)] for _ in range(6)]
    def draw(mod,offset):
        nonlocal seed
        seed=(seed*1664525+1013904223)%2**32
        return seed%mod-offset
    for i in range(n):
        for a in range(3):
            x,v,f=draw(2049,1024),draw(129,64),draw(9,4)
            fields[0][a][i]=x+ticks*v+f*ticks*(ticks+1)//2 if kind==1 else x
            fields[1][a][i]=v+ticks*f if kind==1 else v
            fields[2][a][i]=f
            fields[3][a][i]=max(-4,min(4,2*(16-v))) if kind==4 else f
            if kind==3:fields[4][a][i],fields[5][a][i]=x+2,v+2
    require(list(struct.unpack_from('<4608i',data,512))==[v for field in fields for axis in field for v in axis],'independent complete phase state')
    calls=[0]*8;calls[kind]=ticks
    require(list(struct.unpack_from('<8Q',data,72))==calls,'probe calls')
    require(struct.unpack_from('<4i',data,40)==(8,1,0,0) and struct.unpack_from('<QQ',data,56)==(ticks,0),'probe controls')
    require(not any(data[136:400]),'probe channel/fault metadata')
    sums=[sum(a) for field in fields[:2] for a in field] if kind==6 else [0]*6
    require(list(struct.unpack_from('<6q',data,400))==sums,'probe sums')

def initial_state(directory,e):
    data=(directory/'state.bin').read_bytes();state_header(data,e)
    zero=dict(e);zero['ticks']=0
    # Initial memory-only owner: all dynamic state is zero except seeded plant,
    # command and the default control configuration.
    seed=1;fields=[[[0]*256 for _ in range(3)] for _ in range(6)]
    for i in range(e['count']):
        for a in range(3):
            for field,mod,offset in ((0,2049,1024),(1,129,64),(2,9,4)):
                seed=(seed*1664525+1013904223)%2**32;fields[field][a][i]=seed%mod-offset
            fields[3][a][i]=fields[2][a][i]
    require(list(struct.unpack_from('<4608i',data,512))==[v for f in fields for a in f for v in a],'memory initial state')
    require(struct.unpack_from('<4i',data,40)==(8,1,0,0) and not any(data[56:448]),'memory initial metadata')

def replay_pair(directory,v):
    trusted=(directory/'trusted.bin').read_bytes()
    require(trusted[:8]==b'RTFWLCR2' and len(trusted)>=400,'trusted schema')
    word=lambda n:struct.unpack_from('<Q',trusted,n)[0]
    require(word(16)==len(trusted) and word(32)==v['runtime_id'],'origin owner identity')
    checked=bytearray(trusted);checked[24:32]=bytes(8)
    require(fnv(checked)==word(24),'trusted checksum')
    for name,off,size in [('checkpoint.bin',96,104),('active.bin',120,128)]:
        require(trusted[word(off):word(off)+word(size)]==(directory/name).read_bytes(),'nested replay pairing')

def benchmark(directory):
    tool,schemas=benchmark_tool()
    result=tool.validate_bundle(directory/'benchmark',schemas)
    require(result['status']=='ok','benchmark did not pass')
    d=load(directory/'benchmark/descriptor.json');raw=load(directory/'benchmark/raw.json')
    require(d['case_id'] in IDS,'foreign case')
    i=IDS.index(d['case_id'])
    require(d['provider_id']=='rtfw.golden' and d['provider_version']==1 and d['warmup']==2 and d['repetitions']==5 and d['raw_policy']=='all_measured','frozen provider protocol')
    require(d['subsystem']==SUBSYSTEMS[i] and tuple(c['name'] for c in d['counters'])==COUNTERS,'case subsystem/counters')
    require(len(d['parameters'])<=16 and len(d['counters'])<=16,'descriptor capacity')
    p={x['name']:x['value'] for x in d['parameters']}
    require(set(p)=={'entities','ticks','workers','grain','host'} and p['grain'] in (1,4,16,64),'parameters')
    e=dict(count=p['entities'],ticks=p['ticks'],workers=p['workers'],grain=p['grain'],mode='host' if p['host'] else 'native',external=i==13,campaign='control_replaced' if i==11 else 'nominal',peer_responses=(p['ticks']+2)//3 if i==13 else 0)
    e['phase_calls']=[(p['ticks']-1)//period+1 for period in (1,1,1,2,3,3,6,6)]
    paths=directory/'invocations'/d['case_id']
    require({x.name for x in paths.iterdir()}==set(map(str,range(7))),'all seven invocations')
    state_module=module('showcase_state',HERE/'state.py')
    workload=('seed-and-command','integer-task-integrator','host-frame-staging','sample-decode-calibration','saturating-controller','actuator-frame-encoding','six-field-aggregation','loop-and-all-telemetry-drains','independent-host-loop','memory-plan-acquire-observe-release','reference-schedule-and-loop','replacement-control-loop','loop-capture-and-origin-owner-replay','external-process-cil-loop','full-loop-telemetry-and-replay')[i]+'.lifecycle-oracle-io'
    require(d['workload_kind']==workload and d['workload_sha256']==sha((FROZEN+':'+workload).encode()),'named workload identity')
    configurations=('cpu-cpu','sim_cuda-kernel','sim_cuda-graph','sim_xdma-cpu','sim_combined-kernel','sim_combined-graph') if i==14 else ('bounded-cpu-phase-experiment',) if i<7 else ('frozen-golden-cpu-scenario',)
    require(d['configuration'] in configurations,'configuration scope')
    source_digests={}
    for ordinal in range(7):
        path=paths/str(ordinal);v=load(path/'invocation.json')
        require(v['schema']==1 and v['case_id']==d['case_id'] and v['ordinal']==ordinal and v['contract_sha256']==FROZEN,'invocation binding')
        require(v['correct'] is True and not v['faults'] and not v['gaps'] and not v['cleanup_failures'],'invocation failure')
        for key,value in v.items():
            if key not in ('case_id','contract_sha256','correct'):require(type(value) is int and 0<=value<2**64,'invocation integer '+key)
        variant=d['configuration'].split('-')[0] if i==14 else 'cpu'
        if variant in ('sim_cuda','sim_xdma','sim_combined'):
            run=variant_inspect(path,variant)
            require(run['dispatch']==d['configuration'].split('-')[1] and all(run[k]==e[k] for k in e if k not in ('peer_responses','phase_calls')),'variant configuration')
            require(run['runtime_id']==v['runtime_id'] and run['replay_frames']==v['replay_frames'],'variant observation binding')
        elif i<7:probe_state(path,e,i)
        elif i==9:initial_state(path,e)
        else:state_module.inspect_state(path,e)
        require(v['checksum']==fnv((path/'state.bin').read_bytes()),'state observation checksum')
        calls=p['ticks'] if i<7 else 0 if i==9 else sum(e['phase_calls'])
        require(v['phase_calls']==calls and v['entity_checks']==(0 if i==9 else p['ticks']*768),'actual callback/oracle counts')
        operations=6 if i==9 else p['ticks']+(27 if i==10 else (p['ticks']+2)//3 if i==13 else 0)
        require(v['operations']==operations and v['planned_bytes']>0,'operation/memory counts')
        require(v['memory_acquired']==v['memory_released']==(0 if i<7 else 3),'memory conservation')
        require((v['jobs']>0)==(bool(p['host']) and i>=7 and i!=9),'actual host jobs')
        require(v['replay_frames']==(p['ticks'] if i in (12,14) else 0),'replay frames')
        if i in (12,14):replay_pair(path,v)
        if ordinal>=2:
            sample=raw['samples'][ordinal-2]
            require(sample['checksum']==v['checksum']&((1<<63)-1) and sample['counters']=={k:v[k] for k in COUNTERS},'raw invocation linkage')
        for source in path.iterdir():
            require(source.is_file() and not source.is_symlink(),'invocation source type')
            source_digests[str(source.relative_to(directory))]=sha(source.read_bytes())
    return dict(case=d['case_id'],clock=d['clock'],configuration=d['configuration'],parameters=p,evidence=result['evidence_class'],identity=result['identity'],files=source_digests,benchmark={x.name:sha(x.read_bytes()) for x in (directory/'benchmark').iterdir()})


def main():
    p=argparse.ArgumentParser();p.add_argument('directory',type=Path);a=p.parse_args()
    try:benchmark(a.directory)
    except (OSError,ValueError,KeyError,TypeError,ImportError) as e:p.exit(1,str(e)+'\n')
    print('PASS source observations, every state field and unchanged M23 bundle validator')
if __name__=='__main__':main()
