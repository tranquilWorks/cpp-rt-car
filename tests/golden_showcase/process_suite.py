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
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--provenance',type=Path,required=True);p.add_argument('--config');args=p.parse_args()
# Model the actual Windows multi-config layout before launching real binaries.
with tempfile.TemporaryDirectory(prefix='showcase config ') as temporary:
    root=Path(temporary)
    for folder,name in [('Debug','golden_showcase'),('bench/Debug','sample_golden_showcase'),('samples/RelWithDebInfo','sample_golden_controller')]:
        path=root/folder/(name+'.exe');path.parent.mkdir(parents=True,exist_ok=True);path.touch()
        logical='golden_controller' if 'controller' in name else 'golden_showcase'
        a.require(run.executable(root,logical)==path.resolve(),'multi-config executable discovery')
        path.unlink()
    debug=root/'bench/Debug/sample_golden_showcase.exe';debug.touch()
    release=root/'bench/Release/sample_golden_showcase.exe';release.parent.mkdir();release.touch()
    a.require(run.executable(root,'golden_showcase','Debug')==debug.resolve(),'explicit build configuration')
    for selected in (None,'RelWithDebInfo','../Debug'):
        try:run.executable(root,'golden_showcase',selected)
        except ValueError:pass
        else:raise AssertionError('ambiguous/missing/invalid configuration accepted')
parent=Path(tempfile.mkdtemp(prefix='showcase acceptance ',dir=args.build));exe=run.executable(args.build,'golden_showcase',args.config)
for i,options in enumerate((['--grain','2'],['--workers','0'],['--count','257'],['--variant','sim_cuda','--dispatch','cpu'],['--rate_multiplier','4'],['--case','golden-input','--mode','host'],['--case','foreign'])):
    path=parent/f'negative-{i}'
    result=subprocess.run([str(exe),'--case','golden-loop','--output',str(path),*options],capture_output=True,text=True,timeout=20)
    a.require(result.returncode!=0 and not path.exists(),'invalid full-loop configuration had effects')
for count,ticks in ((1,1),(17,7),(256,24)):
    path=parent/f'boundary-{count}-{ticks}'
    run.command([exe,'--case','golden-loop','--count',count,'--ticks',ticks,'--clock','fake','--output',path])
    a.benchmark(path)
output=parent/'complete'
run.execute(args.build.resolve(),output,args.provenance.resolve(),'fake',args.config)
run.command([sys.executable,ROOT/'tests/golden_showcase/negative_artifacts.py','--evidence',output,'--provenance',args.provenance,'--build',args.build]+(['--config',args.config] if args.config else []),timeout=180)
print('PASS CLI boundaries, complete execution, and independent evidence rejection')
