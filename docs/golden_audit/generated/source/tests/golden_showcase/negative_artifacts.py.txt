"""Mutation tests over actual source-bound showcase execution, with restoration."""
import argparse
import json
from pathlib import Path
import struct
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'samples/golden_showcase'))
import artifacts as a
import report
import run


def main():
    p=argparse.ArgumentParser();p.add_argument('--evidence',type=Path,required=True);p.add_argument('--provenance',type=Path,required=True);p.add_argument('--build',type=Path,required=True);p.add_argument('--config');args=p.parse_args()
    root=args.evidence
    names=('golden_showcase','golden_experiment','golden_telemetry_loss','golden_system','golden_cuda','golden_xdma','golden_controller')
    binaries={name:run.executable(args.build,name,args.config) for name in names}
    validate=lambda:report.validate(root,args.provenance,binaries=binaries)
    validate();count=0
    def reject(paths,change,check=validate):
        nonlocal count
        saved={path:path.read_bytes() for path in paths}
        try:
            change()
            try:check()
            except (OSError,ValueError,KeyError,TypeError):count+=1
            else:raise AssertionError('mutation accepted: '+str(paths))
        finally:
            for path,content in saved.items():path.write_bytes(content)
    def edit(path,fn):
        obj=a.load(path);fn(obj);path.write_text(json.dumps(obj,sort_keys=True,separators=(',',':'))+'\n')
    bench=root/'benchmarks/golden-physics';state=bench/'invocations/golden-physics/2/state.bin';inv=state.with_name('invocation.json')
    # Recompute internal checksum after corrupting an inactive state lane.
    def inactive():
        data=bytearray(state.read_bytes());struct.pack_into('<i',data,512+255*4,1);data[480:488]=bytes(8);struct.pack_into('<Q',data,480,a.fnv(data));state.write_bytes(data)
        edit(inv,lambda d:d.update(checksum=a.fnv(data)))
    reject([state,inv],inactive,lambda:a.benchmark(bench))
    # Make the entire M23 bundle internally consistent with a false operation
    # count. The independent invocation workload bound must still reject it.
    raw=bench/'benchmark/raw.json';result=bench/'benchmark/result.json';tool,_=a.benchmark_tool()
    def false_counter():
        edit(inv,lambda d:d.update(operations=d['operations']+1))
        r=a.load(raw);r['samples'][0]['counters']['operations']+=1;raw.write_bytes(tool.canonical(r))
        r=a.load(result);r['raw_sha256']=a.sha(raw.read_bytes());r['statistics']['counter_totals']['operations']+=1
        r['result_sha256']=a.sha(tool.canonical({k:v for k,v in r.items() if k!='result_sha256'}));result.write_bytes(tool.canonical(r))
        tool.validate_bundle(bench/'benchmark',a.benchmark_tool()[1])
    reject([inv,raw,result],false_counter,lambda:a.benchmark(bench))
    # A self-consistent M23 identity must still match the executed variant.
    device=root/'benchmarks/sim_cuda-graph-native';result=device/'benchmark/result.json';device_raw=device/'benchmark/raw.json'
    def false_backend():
        r=a.load(result);r['identity']['backend']='cpu';r['identity']['driver']='none'
        context=dict(descriptor_sha256=r['descriptor_sha256'],identity=r['identity'],start_utc=r['start_utc'])
        r['run_context_sha256']=a.sha(tool.canonical(context)[:-1])
        samples=a.load(device_raw);samples['run_context_sha256']=r['run_context_sha256'];device_raw.write_bytes(tool.canonical(samples))
        r['raw_sha256']=a.sha(device_raw.read_bytes())
        r['result_sha256']=a.sha(tool.canonical({k:v for k,v in r.items() if k!='result_sha256'}));result.write_bytes(tool.canonical(r))
        tool.validate_bundle(device/'benchmark',a.benchmark_tool()[1])
    reject([result,device_raw],false_backend,lambda:a.benchmark(device))
    path=root/'benchmarks/golden-replay/invocations/golden-replay/2/trusted.bin'
    other=path.parent.parent/'3/trusted.bin'
    reject([path],lambda:path.write_bytes(other.read_bytes()))
    path=root/'faults/telemetry_loss/evidence.json'
    reject([path],lambda:edit(path,lambda d:d.update(lost=0)))
    path=root/'experiments/control_burst-17.json'
    reject([path],lambda:edit(path,lambda d:d.update(accepted=17,rejected=0)))
    path=root/'experiments/scratch_bytes-256.json'
    reject([path],lambda:edit(path,lambda d:d.update(effective=64)))
    path=root/'inputs.json'
    reject([path],lambda:edit(path,lambda d:d['executables'].update(golden_showcase='0'*64)))
    for name in ('report.json','faults.json','coverage.json'):
        path=root/name;reject([path],lambda path=path:path.write_text('{}'))
    path=root/'benchmarks/golden-loop/invocations/golden-loop/0/state.bin'
    reject([path],lambda:path.unlink())
    path=root/'faults/stop_failure/execution.json'
    reject([path],lambda:edit(path,lambda d:d.update(fault_safety=0)))
    copied=root/'negative-provenance.json';copied.write_bytes(args.provenance.read_bytes())
    try:
        edit(copied,lambda d:d['files'].update({'golden_showcase/provider.cpp':'0'*64}))
        try:a.provenance(copied)
        except ValueError:count+=1
        else:raise AssertionError('foreign source accepted')
    finally:copied.unlink()
    validate()
    print(f'PASS {count} source/state/replay/counter/gap/false-report mutation controls')
if __name__=='__main__':main()
