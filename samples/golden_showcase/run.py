#!/usr/bin/env python3
"""Build, run and validate the optional golden showcase source kit."""
import argparse
import json
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time
import uuid
import artifacts as a
HERE=Path(__file__).resolve().parent


def command(args,timeout=300,quiet=False):
    r=subprocess.run(list(map(str,args)),stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=timeout)
    if not quiet:print(r.stdout,end='',flush=True)
    if r.returncode:raise RuntimeError(f'exit {r.returncode}: {args}')
    return r.stdout

def executable(build,name):
    for directory,base in [(build,name),(build/'Release',name),(build/'bench','sample_'+name),(build/'bench/Release','sample_'+name),(build/'samples','sample_'+name),(build/'samples/Release','sample_'+name)]:
        for suffix in ('','.exe'):
            candidate=directory/(base+suffix)
            if candidate.is_file():return candidate.resolve()
    raise ValueError('missing executable '+name)

def peer_benchmark(args,controller):
    # Read stdout in a thread so a broken child cannot make readline unbounded.
    child=subprocess.Popen(list(map(str,args)),stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    lines=queue.Queue();peers=[]
    def read():
        try:
            for line in child.stdout:lines.put(line)
        finally:lines.put(None)
    reader=threading.Thread(target=read,daemon=True);reader.start()
    deadline=time.monotonic()+120
    try:
        while True:
            line=lines.get(timeout=max(.01,deadline-time.monotonic()))
            if line is None:break
            print(line,end='',flush=True)
            if line.startswith('showcase_peer '):
                key=line.split()[1]
                a.require(key.startswith('golden_') and key.replace('_','').isalnum(),'peer key')
                peers.append(subprocess.Popen([str(controller),key,'26','5000'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True))
        a.require(child.wait(timeout=5)==0,'external benchmark child failure')
        a.require(len(peers)==7,'seven actual controller processes')
        for peer in peers:
            output=peer.communicate(timeout=5)[0];print(output,end='',flush=True)
            a.require(peer.returncode==0,'controller process failure')
    finally:
        for process in [child,*peers]:
            if process.poll() is None:process.kill();process.communicate(timeout=5)
        reader.join(timeout=5)

def execute(build,output,provenance,clock):
    a.require(not output.exists(),'output already exists')
    source=a.provenance(provenance)
    output.mkdir(parents=True)
    names=('golden_showcase','golden_experiment','golden_telemetry_loss','golden_system','golden_cuda','golden_xdma','golden_controller')
    binaries={name:executable(build,name) for name in names}
    inputs={name:a.sha(path.read_bytes()) for name,path in binaries.items()}
    descriptors=[json.loads(line) for line in command([binaries['golden_showcase'],'--list'],quiet=True).splitlines() if line.strip()]
    a.require([d['case_id'] for d in descriptors]==list(a.IDS),'exact case inventory')
    for case in a.IDS:
        dest=output/'benchmarks'/case
        args=[binaries['golden_showcase'],'--case',case,'--clock',clock,'--output',dest]
        if case=='golden-external':peer_benchmark(args+['--peer-prefix','golden_'+uuid.uuid4().hex],binaries['golden_controller'])
        else:command(args)
        a.benchmark(dest)
    for variant,dispatch in [('sim_cuda','kernel'),('sim_cuda','graph'),('sim_xdma','cpu'),('sim_combined','kernel'),('sim_combined','graph')]:
        for mode in ('native','host'):
            dest=output/'benchmarks'/f'{variant}-{dispatch}-{mode}'
            command([binaries['golden_showcase'],'--case','golden-loop','--clock',clock,'--variant',variant,'--dispatch',dispatch,'--mode',mode,'--output',dest])
            a.benchmark(dest)
    for lever in a.cpu.contract()['levers']:
        for value in lever['values']:
            dest=output/'experiments'/f'{lever["id"]}-{value}.json';dest.parent.mkdir(exist_ok=True)
            command([binaries['golden_experiment'],lever['id'],value,dest])
    # Every fault is executed afresh through its existing actual owner path.
    for fault in a.cpu.contract()['faults']:
        name=fault['id'];dest=output/'faults'/name;dest.mkdir(parents=True)
        if name=='telemetry_loss':command([binaries['golden_telemetry_loss'],dest/'evidence.json']);continue
        if name in ('underflow','overrun','stop_failure','device_loss','reset_failure'):
            command([binaries['golden_xdma'],'--dispatch','graph','--fault',name,'--output',dest])
            a.variant_inspect(dest,'sim_combined')
        else:
            command([binaries['golden_system'],'--campaign',name,'--output',dest]);a.cpu.inspect(dest)
    # Explicit real external CIL on the CUDA and combined backends supplements
    # the timed CPU external case; no child process is replaced by a callback.
    pair=a.module('showcase_pair',HERE.parent/'golden_system/run.py').pair
    for name,dispatch in [('golden_cuda','kernel'),('golden_cuda','graph'),('golden_xdma','cpu'),('golden_xdma','kernel'),('golden_xdma','graph')]:
        dest=output/'external'/f'{name}-{dispatch}'
        h,c,ho,co=pair(binaries[name],binaries['golden_controller'],dest,extra=['--dispatch',dispatch])
        print(ho+co,end='',flush=True);a.require(h==c==0,'external variant failure')
        a.variant_inspect(dest,'sim_cuda' if name=='golden_cuda' else 'sim_xdma' if dispatch=='cpu' else 'sim_combined')
    # Source and executable bytes must still be the ones admitted at entry.
    a.require(a.provenance(provenance)==source,'source changed during execution')
    a.require(inputs=={name:a.sha(path.read_bytes()) for name,path in binaries.items()},'executable changed during execution')
    (output/'inputs.json').write_text(json.dumps(dict(schema=1,source=source,executables=inputs,clock=clock),sort_keys=True,indent=2)+'\n')
    import report
    report.validate(output,provenance,publish=True,binaries=binaries)
    print('PASS golden showcase; physical, RT, controlled performance and final CAP-M26 audit remain separate')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,default=HERE/'build');p.add_argument('--binaries-build',type=Path);p.add_argument('--prefix',type=Path);p.add_argument('--output',type=Path,default=HERE/'output');p.add_argument('--provenance',type=Path,default=HERE/'provenance.json');p.add_argument('--clock',choices=('fake','steady'),default='fake');args=p.parse_args()
    try:
        build=args.binaries_build
        if build is None:
            config=['cmake','-S',HERE,'-B',args.build,'-DCMAKE_BUILD_TYPE=Release']
            if args.prefix:config.append('-DCMAKE_PREFIX_PATH='+str(args.prefix.resolve()))
            command(config);command(['cmake','--build',args.build,'--config','Release','--parallel','2']);build=args.build
        execute(build.resolve(),args.output.resolve(),args.provenance.resolve(),args.clock)
    except (OSError,ValueError,RuntimeError,KeyError,subprocess.SubprocessError,queue.Empty) as e:p.exit(1,str(e)+'\n')
if __name__=='__main__':main()
