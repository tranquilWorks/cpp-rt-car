"""CLI boundary matrix plus full independently validated artifact mutation suite."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'samples/golden_showcase'))
import artifacts as a
import run
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--provenance',type=Path,required=True);args=p.parse_args()
parent=Path(tempfile.mkdtemp(prefix='showcase acceptance ',dir=args.build));exe=run.executable(args.build,'golden_showcase')
for i,options in enumerate((['--grain','2'],['--workers','0'],['--count','257'],['--variant','sim_cuda','--dispatch','cpu'],['--rate_multiplier','4'],['--case','golden-input','--mode','host'],['--case','foreign'])):
    path=parent/f'negative-{i}'
    result=subprocess.run([str(exe),'--case','golden-loop','--output',str(path),*options],capture_output=True,text=True,timeout=20)
    a.require(result.returncode!=0 and not path.exists(),'invalid full-loop configuration had effects')
for count,ticks in ((1,1),(17,7),(256,24)):
    path=parent/f'boundary-{count}-{ticks}'
    run.command([exe,'--case','golden-loop','--count',count,'--ticks',ticks,'--clock','fake','--output',path])
    a.benchmark(path)
output=parent/'complete'
run.execute(args.build.resolve(),output,args.provenance.resolve(),'fake')
run.command([sys.executable,ROOT/'tests/golden_showcase/negative_artifacts.py','--evidence',output,'--provenance',args.provenance,'--build',args.build],timeout=180)
print('PASS CLI boundaries, complete execution, and independent evidence rejection')
