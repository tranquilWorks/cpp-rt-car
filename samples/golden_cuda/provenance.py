"""CMake configure-time source identity; dirty source is recorded explicitly."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

def generate(root, output):
    def git(*args):return subprocess.check_output(['git','-C',str(root),*args],text=True,stderr=subprocess.DEVNULL).strip()
    # SDK source archives can build without Git. They cannot invent a committed
    # identity: artifact publication will explicitly reject unavailable provenance.
    try:
        if Path(git('rev-parse','--show-toplevel')).resolve()!=root.resolve():raise ValueError('foreign parent repository')
        commit,tree,dirty=git('rev-parse','HEAD'),git('rev-parse','HEAD^{tree}'),bool(git('status','--porcelain'))
    except (OSError,ValueError,subprocess.CalledProcessError):
        commit,tree,dirty=None,None,True
    kit=Path(__file__).resolve().parent
    result={'schema':1,'source_commit':commit,'source_tree':tree,'dirty':dirty,'files':{str(p.relative_to(kit.parent)).replace('\\','/'):hashlib.sha256(p.read_bytes()).hexdigest() for folder in ('golden_cuda','golden_system') for p in sorted((kit.parent/folder).iterdir()) if p.is_file() and p.name!='provenance.json'}}
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('output',type=Path);a=p.parse_args();generate(a.root,a.output)
