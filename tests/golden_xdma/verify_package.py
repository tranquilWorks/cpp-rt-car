#!/usr/bin/env python3
"""Real CPack extraction, relocated public source kit, full consumers and negatives."""
import argparse
import json
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('golden_package_helpers',ROOT/'tests/installed_docs/verify_package.py')
helpers=importlib.util.module_from_spec(spec);spec.loader.exec_module(helpers)
run=helpers.run

def main():
    p=argparse.ArgumentParser();p.add_argument('--work-directory',type=Path,required=True);a=p.parse_args()
    work=a.work_directory.resolve();work.mkdir(parents=True,exist_ok=True);sdk=work/'sdk build'
    with tempfile.TemporaryDirectory(prefix='golden XDMA original ',dir=work) as old,tempfile.TemporaryDirectory(prefix='golden XDMA relocated ') as new:
        original=Path(old)/'sdk';relocated=Path(new)
        run('cmake','-S',ROOT,'-B',sdk,'-DCMAKE_BUILD_TYPE=Release','-DENABLE_TESTS=OFF','-DRTFW_BUILD_EXAMPLES=OFF','-DRTFW_BUILD_RUNTIME_DEMO=OFF','-DRTFW_BUILD_EXPERIMENTAL=OFF','-DRTFW_BUILD_BENCHMARKS=OFF','-DRTFW_ENABLE_CUDA=OFF','-DSIM_SANITIZERS=','-DSIM_WERROR=ON','-DCMAKE_INSTALL_INCLUDEDIR=sdk/include','-DCMAKE_INSTALL_DATADIR=custom/data',f'-DCMAKE_INSTALL_PREFIX={original}')
        run('cmake','--build',sdk,'--config','Release','--parallel','2')
        run('cmake','--install',sdk,'--config','Release','--prefix',original)
        archives=relocated/'archive'
        run('cpack','--config',sdk/'CPackConfig.cmake','-C','Release','-B',archives,'-D','CPACK_PACKAGING_INSTALL_PREFIX=/','-D','CPACK_PACKAGE_FILE_NAME=rtfw-golden-xdma')
        shutil.rmtree(original)
        prefix=relocated/'SDK with spaces'
        run(sys.executable,ROOT/'tools/extract_release_archive.py','--artifact-dir',archives,'--destination',prefix)
        kit=prefix/'custom/data/rtfw/examples/golden_xdma';build=relocated/'kit build'
        expected={x.name for x in (ROOT/'samples/golden_xdma').iterdir() if x.is_file()}|{'provenance.json'}
        if {x.name for x in kit.iterdir()}!=expected:raise RuntimeError('source inventory mismatch')
        helpers.build_test(kit,build,f'-DCMAKE_PREFIX_PATH={prefix}','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON')
        helpers.check_no_private_paths(build,[ROOT,sdk,original])
        run(sys.executable,kit/'run.py','--prefix',prefix,'--build',build,'--output',relocated/'one command evidence','--mode','external')
        helpers.build_test(ROOT/'tests/package_consumer',relocated/'full SDK',f'-DCMAKE_PREFIX_PATH={prefix}','-DRTFW_TEST_BENCHMARK=OFF')
        inventory=json.loads(run('ctest','--test-dir',relocated/'full SDK','-C','Release','--show-only=json-v1'))
        names={test['name'] for test in inventory['tests']}
        expected={'golden_xdma_kit_'+mode+'_'+dispatch for mode in ('native','host','external') for dispatch in ('cpu','kernel','graph')}
        if len(inventory['tests'])!=64 or not expected.issubset(names):
            raise RuntimeError('full SDK must include all 55 prior consumers and nine XDMA variants')
        helpers.build_test(kit,relocated/'embedded',f'-DGOLDEN_RTFW_SOURCE={ROOT}','-DENABLE_TESTS=OFF','-DRTFW_BUILD_EXAMPLES=ON','-DSIM_SANITIZERS=','-DSIM_WERROR=ON')
        # Public SDK discovery and source embedding failures must be useful.
        run('cmake','-S',kit,'-B',relocated/'missing SDK','-DCMAKE_DISABLE_FIND_PACKAGE_rtfw=TRUE',failure='rtfw')
        run('cmake','-S',kit,'-B',relocated/'missing source',f'-DGOLDEN_RTFW_SOURCE={relocated}/absent',failure='must name an RTFW source checkout')
        variant=relocated/'missing source kit'/'golden_xdma';shutil.copytree(kit,variant);shutil.copytree(kit.parent/'golden_system',variant.parent/'golden_system');shutil.copytree(kit.parent/'golden_cuda',variant.parent/'golden_cuda');(variant/'io.hpp').unlink()
        run('cmake','-S',variant,'-B',relocated/'missing header',f'-DCMAKE_PREFIX_PATH={prefix}')
        run('cmake','--build',relocated/'missing header','--config','Release','--target','golden_xdma','--parallel','2',failure='io.hpp')
        # Missing binaries and false evidence must not become successful reports.
        run(sys.executable,kit/'run.py','--plant',relocated/'absent','--output',relocated/'absent evidence',failure='absent')
        state=build/'native-graph evidence/state.bin';saved=state.read_bytes();state.unlink()
        run(sys.executable,kit/'artifacts.py',build/'native-graph evidence',failure='state.bin');state.write_bytes(saved)
        provenance=kit/'provenance.json';saved=provenance.read_bytes();provenance.write_text('{}')
        run(sys.executable,kit/'artifacts.py',build/'native-graph evidence',failure='provenance');provenance.write_bytes(saved)
        print('PASS CPack relocation, one-command execution, full SDK, embedding and negative controls',flush=True)
if __name__=='__main__':main()
