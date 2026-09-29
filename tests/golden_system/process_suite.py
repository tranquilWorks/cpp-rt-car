#!/usr/bin/env python3
"""Actual process failures, independent artifact mutations, session ownership."""
import argparse
import concurrent.futures
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import uuid

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'samples/golden_system'))
import artifacts
import run

class Process:
    def __init__(self,args):
        self.p=subprocess.Popen(list(map(str,args)),stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        self.ready=threading.Event();self.stopped=threading.Event();self.lines=[]
        def read():
            for line in self.p.stdout:
                if len(self.lines)>=100:raise RuntimeError('output bound')
                self.lines.append(line)
                if line.startswith('ready '):self.ready.set()
                if line.startswith('stopped old'):self.stopped.set()
            self.p.stdout.close()
        self.reader=threading.Thread(target=read,daemon=True);self.reader.start()
    def finish(self,code):
        actual=self.p.wait(timeout=20);self.reader.join(timeout=2)
        artifacts.require(actual==code and not self.reader.is_alive(),f'process exit {actual} expected{code}: {self.lines}')
    def close(self):
        if self.p.poll() is None:self.p.kill();self.p.wait(timeout=5)
        self.reader.join(timeout=2)

def mutations(directory,provenance):
    originals={p:p.read_bytes() for p in directory.iterdir() if p.is_file()}
    def reject(change):
        try:
            change()
            try:artifacts.validate(directory,provenance)
            except (ValueError,OSError,KeyError,TypeError):pass
            else:raise AssertionError('artifact mutation accepted')
        finally:
            for p,b in originals.items():p.write_bytes(b)
    def edit_json(name,key,value):
        p=directory/name;d=json.loads(p.read_text());d[key]=value;p.write_text(json.dumps(d))
    for name,key,value in [('run.json','cleanup',False),('run.json','source_commit','0'*40),('run.json','phase_calls',[0]*8),('run.json','state_digest','0'*64),('replay.json','gap_count',1),('replay.json','replay_status','NOT_RUN'),('execution.json','control_accepted',99),('execution.json','oracle',False),('execution.json','replay_frames',23),('execution.json','peer_responses',7),('execution.json','action_gaps',1),('execution.json','deadline_failures',1)]:
        reject(lambda n=name,k=key,v=value:edit_json(n,k,v))
    for name in artifacts.FILES:
        reject(lambda n=name:(directory/n).unlink())
    for name in ('state.bin','checkpoint.bin','active.bin','trusted.bin'):
        def corrupt(n=name):
            p=directory/n;b=bytearray(p.read_bytes());b[-1]^=1;p.write_bytes(b)
        reject(corrupt)
    # Recompute the state checksum: the independent numerical oracle must still fail.
    def false_state():
        import struct
        p=directory/'state.bin';b=bytearray(p.read_bytes());b[512]^=1;b[480:488]=bytes(8);struct.pack_into('<Q',b,480,artifacts.fnv(b));p.write_bytes(b)
    reject(false_state)

def suite(a):
    with tempfile.TemporaryDirectory(prefix='golden processes ') as work:
        work=Path(work)
        for mode in ('native','host'):
            output=work/mode;h,c,ho,co=run.pair(a.plant,a.controller,output,mode)
            artifacts.require(h==c==0,ho+co);artifacts.validate(output,a.provenance,True);mutations(output,a.provenance)
        for mode in ('native','host'):
            for campaign in artifacts.CAMPAIGNS[1:]:
                output=work/(mode+'-'+campaign)
                run.command([a.plant,'--mode',mode,'--campaign',campaign,'--output',output])
                artifacts.validate(output,a.provenance,True)
        for campaign in ('stale_input','control_rejected','control_replaced','peer_missing'):
            output=work/('external-'+campaign)
            h,c,ho,co=run.pair(a.plant,a.controller,output,'host',extra=('--campaign',campaign))
            artifacts.require(h==c==0,ho+co);artifacts.validate(output,a.provenance,True)
        expected={'schema':('schema',0,0),'generation':('generation',0,0),'sequence':('sequence',0,0),'future':('future',0,0),'expired':('expired',1,0),'correlation':('correlation',0,0),'replay':('sequence',1,0),'crash':('timeout',0,77),'partial':('timeout',0,78),'no_ack':('ok',8,0),'hold':('timeout',0,0)}
        for fixture,(status,responses,exitcode) in expected.items():
            output=work/fixture;h,c,ho,co=run.pair(a.plant,a.controller,output,fixture=fixture,timeout=1000)
            artifacts.require(h==2 and c==exitcode,f'{fixture}: {h}/{c} {ho} {co}')
            e=artifacts.inspect(output);artifacts.require(e['peer_status']==status and e['peer_responses']==responses,fixture+' outcome')
            result=artifacts.validate(output,a.provenance,True);artifacts.require(result['status']=='FAIL',fixture+' false success')
        # Simultaneous mappings and native/host sessions must remain isolated.
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            futures=[pool.submit(run.pair,a.plant,a.controller,work/('parallel'+str(i)),mode) for i,mode in enumerate(('native','host'))]
            for i,f in enumerate(futures):
                h,c,ho,co=f.result();artifacts.require(h==c==0,ho+co);artifacts.validate(work/('parallel'+str(i)),a.provenance,True)
        peers=[]
        try:
            key='owner_'+uuid.uuid4().hex
            host=Process([a.plant,'--peer',key,'--generation','31','--timeout-ms','1500','--output',work/'old']);peers.append(host)
            artifacts.require(host.ready.wait(10),'plant startup')
            duplicate=Process([a.plant,'--peer',key,'--generation','31','--timeout-ms','1500']);peers.append(duplicate);duplicate.finish(2)
            old=Process([a.controller,key,'31','1500','linger']);peers.append(old);artifacts.require(old.ready.wait(10),'old peer startup')
            duplicate=Process([a.controller,key,'31','1500']);peers.append(duplicate);duplicate.finish(2)
            host.finish(2);artifacts.require(old.stopped.wait(5),'old stop signal')
            # Keep the old mapped writer alive during a new-name/new-generation run.
            freshkey='fresh_'+uuid.uuid4().hex
            fresh=Process([a.plant,'--peer',freshkey,'--generation','32','--timeout-ms','1500','--output',work/'fresh']);peers.append(fresh)
            controller=Process([a.controller,freshkey,'32','1500']);peers.append(controller)
            fresh.finish(0);controller.finish(0);artifacts.validate(work/'fresh',a.provenance,True);old.finish(0)
            missing=Process([a.plant,'--peer','missing_'+uuid.uuid4().hex,'--generation','33','--timeout-ms','100','--output',work/'missing']);peers.append(missing);missing.finish(2)
            e=artifacts.inspect(work/'missing');artifacts.require(e['peer_status']=='timeout' and e['peer_responses']==0,'missing peer')
        finally:
            for peer in peers:peer.close()
    print('PASS vector processes, faults, artifact mutations, ownership and fresh-session isolation')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--plant',type=Path,required=True);p.add_argument('--controller',type=Path,required=True);p.add_argument('--provenance',type=Path,required=True);suite(p.parse_args())
