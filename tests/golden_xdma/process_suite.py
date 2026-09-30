#!/usr/bin/env python3
"""Real CLI/child-process matrix and independent false-evidence negatives."""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import struct
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'samples/golden_xdma'))
import artifacts
import run

def rejected(call):
    try:call()
    except (ValueError,OSError,KeyError,TypeError):return
    raise AssertionError('false evidence accepted')

def main():
    p=argparse.ArgumentParser();p.add_argument('--plant',type=Path,required=True);p.add_argument('--controller',type=Path,required=True);p.add_argument('--provenance',type=Path,required=True);p.add_argument('--group',choices=['all','native','host','external'],default='all');a=p.parse_args()
    cases=negatives=0
    with tempfile.TemporaryDirectory(prefix='golden XDMA processes ') as folder:
        root=Path(folder)
        for mode in (() if a.group=='external' else ('native','host') if a.group=='all' else (a.group,)):
            for dispatch in ('cpu','kernel','graph'):
                campaigns=list(artifacts.CAMPAIGNS)+['underflow','overrun','stop_failure']+([] if dispatch=='cpu' else ['device_loss','reset_failure'])
                for case in campaigns:
                    out=root/f'{mode}-{dispatch}-{case}'
                    args=[a.plant,'--mode',mode,'--dispatch',dispatch,'--output',out]
                    args+=['--fault' if case in ('underflow','overrun','stop_failure','device_loss','reset_failure') else '--campaign',case]
                    run.command(args)
                    artifacts.validate(out,a.provenance,True);cases+=1
        if a.group=='native':
            # Different boundaries prevent count formulas fitting only 24 ticks.
            for ticks in (1,2,3,4,7,12,13,19,25):
                out=root/f'horizon-{ticks}'
                run.command([a.plant,'--dispatch','cpu','--ticks',ticks,'--output',out])
                artifacts.validate(out,a.provenance,True);cases+=1
            for dispatch in ('cpu','kernel','graph'):
                out=root/f'capacity-{dispatch}'
                run.command([a.plant,'--dispatch',dispatch,'--count',256,'--ticks',1024,'--workers',3,'--grain',64,'--output',out])
                artifacts.validate(out,a.provenance,True);cases+=1
        if a.group in ('native','host'):
            print(f'PASS {cases} {a.group} CLI cases',flush=True);return
        for mode in ('native','host'):
            for dispatch in ('cpu','kernel','graph'):
                out=root/f'external-{mode}-{dispatch}'
                h,c,ho,co=run.pair(a.plant,a.controller,out,mode=mode,extra=['--dispatch',dispatch])
                print(ho+co,end='');assert h==0 and c==0
                artifacts.validate(out,a.provenance,True);cases+=1
        for fixture in ('expired','schema','crash','hold'):
            out=root/f'external-{fixture}'
            h,c,ho,co=run.pair(a.plant,a.controller,out,fixture=fixture,timeout=1000,extra=['--dispatch','graph'])
            print(ho+co,end='');assert h==2
            assert artifacts.validate(out,a.provenance,True)['status']=='FAIL';cases+=1
        out=root/'native-graph-nominal'
        if a.group=='external':
            run.command([a.plant,'--dispatch','graph','--output',out]);artifacts.validate(out,a.provenance,True);cases+=1
        path=out/'execution.json';original=path.read_bytes();execution=json.loads(original)
        # Every claimed backend/fault counter is independently constrained.
        for key,value in execution.items():
            if key.startswith(('xdma_','cuda_','sensor_','actuator_','fault_','reset_','stop_','physical','sampled')) or key in ('cleanup','oracle','phase_calls','mixed_actions','rate_actions','action_gaps','memory_released','replay_frames','control_accepted','count'):
                bad=dict(execution)
                bad[key]=not value if type(value) is bool else value+1 if type(value) is int else [0]*8 if isinstance(value,list) else 'false'
                path.write_text(json.dumps(bad));rejected(lambda:artifacts.inspect(out));negatives+=1
        path.write_bytes(original)
        for name in ('state.bin','checkpoint.bin','active.bin','trusted.bin'):
            path=out/name;saved=path.read_bytes();bad=bytearray(saved);bad[-1]^=1;path.write_bytes(bad)
            rejected(lambda:artifacts.inspect(out));path.write_bytes(saved);negatives+=1
        # Repair the checksum after mutation so the scalar oracle, rather
        # than checksum validation alone, must reject changed active/inactive
        # vectors and channel/control metadata.
        path=out/'state.bin';saved=path.read_bytes()
        offsets=[40,44,48,52]+list(range(72,448,8))
        offsets += [512+4*(axis*256+lane) for axis in range(18) for lane in (0,255)]
        for offset in offsets:
            bad=bytearray(saved);bad[offset]^=1;bad[480:488]=bytes(8)
            struct.pack_into('<Q',bad,480,artifacts.cpu.fnv(bad));path.write_bytes(bad)
            rejected(lambda:artifacts.inspect(out));negatives+=1
        path.write_bytes(saved)
        path=out/'run.json';saved=path.read_bytes();bad=json.loads(saved);bad['evidence']='physical_cuda';path.write_text(json.dumps(bad))
        rejected(lambda:artifacts.validate(out,a.provenance));path.write_bytes(saved);negatives+=1
        bad=artifacts.load(a.provenance);bad['files']['golden_xdma/io.hpp']='0'*64
        path=root/'bad-provenance.json';path.write_text(json.dumps(bad));rejected(lambda:artifacts.validate(out,path));negatives+=1
        # Invalid combinations and absent child executables cannot produce PASS.
        run.command([a.plant,'--dispatch','cpu','--fault','device_loss'],expected=2)
        run.command([sys.executable,ROOT/'samples/golden_xdma/run.py','--plant',root/'absent','--output',root/'absent-output'],expected=1)
        print(f'PASS {cases} CLI/process cases and {negatives} false-evidence negatives')
if __name__=='__main__':main()
