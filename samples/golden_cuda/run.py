#!/usr/bin/env python3
"""Build, run and independently validate the installed CUDA golden kit."""
import argparse
from pathlib import Path
import subprocess
import sys
import uuid
import artifacts

HERE=Path(__file__).resolve().parent

def command(args, expected=0):
    r=subprocess.run(list(map(str,args)),text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=300)
    print(r.stdout,end='',flush=True)
    if r.returncode!=expected:raise RuntimeError(f'exit {r.returncode}, expected {expected}: {args}')
    return r.stdout

def executable(build,name):
    return next(p for p in (build/name,build/(name+'.exe'),build/'Release'/(name+'.exe')) if p.is_file())

def pair(plant,controller,output,mode='native',fixture='normal',timeout=5000,extra=()):
    key='golden_'+uuid.uuid4().hex
    args=[str(plant),'--mode',mode,'--output',str(output),'--peer',key,'--generation','26','--timeout-ms',str(timeout),*map(str,extra)]
    host=subprocess.Popen(args,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    peer=None
    try:
        # Startup line is flushed after exclusive creation; process lifetime is
        # bounded below. Controller can safely start before publication as well.
        peer=subprocess.Popen([str(controller),key,'26',str(timeout),fixture],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        h=host.communicate(timeout=30)[0];c=peer.communicate(timeout=30)[0]
        return host.returncode,peer.returncode,h,c
    finally:
        for p in (host,peer):
            if p is not None and p.poll() is None:p.kill();p.communicate(timeout=5)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,default=HERE/'build');p.add_argument('--prefix',type=Path);p.add_argument('--output',type=Path,default=HERE/'output');p.add_argument('--mode',choices=['native','host','external'],default='native');p.add_argument('--plant',type=Path);p.add_argument('--controller',type=Path);p.add_argument('--provenance',type=Path,default=HERE/'provenance.json');p.add_argument('--dispatch',choices=['cpu','kernel','graph'],default='graph');p.add_argument('--fault',choices=['none','device_loss','reset_failure'],default='none');p.add_argument('--campaign',choices=artifacts.CAMPAIGNS,default='nominal');a=p.parse_args()
    try:
        if not a.plant:
            args=['cmake','-S',HERE,'-B',a.build,'-DCMAKE_BUILD_TYPE=Release']
            if a.prefix:args.append('-DCMAKE_PREFIX_PATH='+str(a.prefix.resolve()))
            command(args);command(['cmake','--build',a.build,'--config','Release','--parallel','2'])
            a.plant=executable(a.build,'golden_cuda');a.controller=executable(a.build,'golden_controller')
        if a.mode=='external':
            if not a.controller:raise ValueError('--controller required for external execution')
            h,c,ho,co=pair(a.plant,a.controller,a.output,extra=['--dispatch',a.dispatch,'--fault',a.fault,'--campaign',a.campaign])
            print(ho+co,end='');artifacts.require(h==0 and c==0,'external process failure')
        else:command([a.plant,'--mode',a.mode,'--output',a.output,'--dispatch',a.dispatch,'--fault',a.fault,'--campaign',a.campaign])
        result=artifacts.validate(a.output,a.provenance,publish=True)
        artifacts.require(result['status']=='PASS','run failed')
    except (OSError,ValueError,RuntimeError,subprocess.SubprocessError,StopIteration) as e:p.exit(1,str(e)+'\n')
    print('PASS: portable golden run/state/replay; hardware and RT qualification not performed')
if __name__=='__main__':main()
