#!/usr/bin/env python3
"""Actual optional CPack extraction, removed prefix, public kit and full SDK."""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('showcase_package_helpers',ROOT/'tests/installed_docs/verify_package.py')
h=importlib.util.module_from_spec(spec);spec.loader.exec_module(h)
run=h.run

def main():
    p=argparse.ArgumentParser();p.add_argument('--work-directory',type=Path,required=True);a=p.parse_args()
    work=a.work_directory.resolve();work.mkdir(parents=True,exist_ok=True);sdk=work/'sdk build'
    with tempfile.TemporaryDirectory(prefix='showcase original ',dir=work) as old,tempfile.TemporaryDirectory(prefix='showcase relocated ') as new:
        original=Path(old)/'sdk';relocated=Path(new)
        run('cmake','-S',ROOT,'-B',sdk,'-DCMAKE_BUILD_TYPE=Release','-DENABLE_TESTS=OFF','-DRTFW_BUILD_EXAMPLES=OFF','-DRTFW_BUILD_RUNTIME_DEMO=OFF','-DRTFW_BUILD_EXPERIMENTAL=OFF','-DRTFW_BUILD_BENCHMARKS=ON','-DRTFW_ENABLE_CUDA=OFF','-DSIM_SANITIZERS=','-DSIM_WERROR=ON','-DCMAKE_INSTALL_INCLUDEDIR=sdk include','-DCMAKE_INSTALL_DATADIR=custom data',f'-DCMAKE_INSTALL_PREFIX={original}')
        run('cmake','--build',sdk,'--config','Release','--parallel','2')
        run('cmake','--install',sdk,'--config','Release','--prefix',original)
        archives=relocated/'archive'
        run('cpack','--config',sdk/'CPackConfig.cmake','-C','Release','-B',archives,'-D','CPACK_PACKAGING_INSTALL_PREFIX=/','-D','CPACK_PACKAGE_FILE_NAME=rtfw-golden-showcase')
        shutil.rmtree(original)
        prefix=relocated/'SDK with spaces'
        run(sys.executable,ROOT/'tools/extract_release_archive.py','--artifact-dir',archives,'--destination',prefix)
        kit=prefix/'custom data/rtfw/examples/golden_showcase';build=relocated/'kit build'
        expected={x.name for x in (ROOT/'samples/golden_showcase').iterdir() if x.is_file()}|{'provenance.json'}
        if {x.name for x in kit.iterdir()}!=expected:raise RuntimeError('complete optional source inventory')
        h.build_test(kit,build,f'-DCMAKE_PREFIX_PATH={prefix}','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON')
        h.check_no_private_paths(build,[ROOT,sdk,original])
        run(sys.executable,kit/'run.py','--prefix',prefix,'--build',build,'--output',relocated/'one command evidence','--clock','steady')
        h.build_test(ROOT/'tests/package_consumer',relocated/'full SDK',f'-DCMAKE_PREFIX_PATH={prefix}','-DRTFW_TEST_BENCHMARK=ON')
        inventory=json.loads(run('ctest','--test-dir',relocated/'full SDK','-C','Release','--show-only=json-v1'))
        names={v['name'] for v in inventory['tests']}
        if len(inventory['tests'])!=71 or 'golden_showcase_kit' not in names:
            raise RuntimeError('all70 prior optional SDK consumers plus showcase required')
        h.build_test(kit,relocated/'embedded',f'-DGOLDEN_RTFW_SOURCE={ROOT}','-DENABLE_TESTS=OFF','-DRTFW_BUILD_EXAMPLES=OFF','-DRTFW_BUILD_RUNTIME_DEMO=OFF','-DSIM_SANITIZERS=','-DSIM_WERROR=ON')
        run('cmake','-S',kit,'-B',relocated/'missing SDK','-DCMAKE_DISABLE_FIND_PACKAGE_rtfw=TRUE',failure='rtfw')
        run('cmake','-S',kit,'-B',relocated/'missing source',f'-DGOLDEN_RTFW_SOURCE={relocated}/absent',failure='must name an RTFW source checkout')
        run('cmake','-S',kit,'-B',relocated/'missing component',f'-DGOLDEN_RTFW_SOURCE={ROOT}','-DRTFW_BUILD_BENCHMARKS=OFF','-DENABLE_TESTS=OFF','-DRTFW_BUILD_EXAMPLES=OFF',failure='optional benchmark component')
        run(sys.executable,kit/'run.py','--binaries-build',relocated/'absent','--output',relocated/'absent evidence',failure='missing executable')
        variant=relocated/'missing header source'/'golden_showcase';shutil.copytree(kit,variant)
        for sibling in ('golden_system','golden_cuda','golden_xdma'):shutil.copytree(kit.parent/sibling,variant.parent/sibling)
        (variant/'probes.hpp').unlink()
        run('cmake','-S',variant,'-B',relocated/'missing header',f'-DCMAKE_PREFIX_PATH={prefix}')
        run('cmake','--build',relocated/'missing header','--config','Release','--target','golden_showcase','--parallel','2',failure='probes.hpp')
        print('PASS actual CPack removal/relocation, complete optional SDK, source embedding, one command and negatives',flush=True)
if __name__=='__main__':main()
