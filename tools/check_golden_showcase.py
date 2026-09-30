#!/usr/bin/env python3
"""Cross-check the optional showcase inventory against the frozen M26 contract."""
import importlib.util
from pathlib import Path
import sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'samples/golden_showcase'))
import artifacts

def main():
    contract=artifacts.cpu.contract()
    cases=contract['benchmarks']['cases']
    if list(artifacts.IDS)!=[row['id'] for row in cases]:raise ValueError('frozen case inventory differs')
    if list(artifacts.SUBSYSTEMS)!=[row['subsystem'] for row in cases]:raise ValueError('frozen subsystem inventory differs')
    cpp=(ROOT/'samples/golden_showcase/provider.hpp').read_text()
    for name in artifacts.IDS:
        if cpp.count('"'+name+'"')!=1:raise ValueError('C++ provider case differs: '+name)
    guide=(ROOT/'docs/golden_showcase.md').read_text()
    for row in contract['levers']:
        if '| '+row['id']+' |' not in guide:raise ValueError('undocumented lever')
    cmake=(ROOT/'CMakeLists.txt').read_text()
    for path in (ROOT/'samples/golden_showcase').iterdir():
        if path.is_file() and path.name!='provenance.json' and str(path.relative_to(ROOT)) not in cmake:
            raise ValueError('uninstalled showcase source: '+path.name)
    for text in ('M26-06','NOT_RUN','kernel-only','disabled capture','two warmups','five retained','originating-owner'):
        if text not in guide:raise ValueError('missing claim/timing boundary: '+text)
    print('PASS frozen showcase case/subsystem/lever and installed source inventory')
if __name__=='__main__':
    try:main()
    except (OSError,ValueError,KeyError,TypeError) as e:sys.exit(str(e))
