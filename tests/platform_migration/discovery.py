#!/usr/bin/env python3
"""Check current installed package against documented version/component requests."""
import argparse
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    root=Path(tempfile.mkdtemp(prefix='attempt-',dir=args.output))
    cases=[('v10','1.0','runtime',True),('v11','1.1','cpp_runtime',True),
           ('v12','1.2','runtime',True),('exact','1.2.1 EXACT','runtime',True),
           ('future_patch','1.2.2','runtime',False),('major','2.0','runtime',False),
           ('old_exact','1.1 EXACT','runtime',False),('unknown','1.2','missing_component',False)]
    records=[]
    for name,version,component,success in cases:
        source=root/name;source.mkdir()
        cmake='cmake_minimum_required(VERSION 3.20)\nproject(migration LANGUAGES CXX)\n'
        cmake+=f'find_package(rtfw {version} CONFIG REQUIRED COMPONENTS {component} PATHS "${{MIGRATION_PREFIX}}" NO_DEFAULT_PATH)\n'
        cmake+='if(NOT RTFW_VERSION STREQUAL "1.2.1" OR NOT TARGET rtfw::runtime OR NOT TARGET rtfw::simcore_rt)\nmessage(FATAL_ERROR "migration target/version mismatch")\nendif()\n'
        cmake+='find_package(rtfw 1.2 CONFIG REQUIRED COMPONENTS runtime OPTIONAL_COMPONENTS missing_component PATHS "${MIGRATION_PREFIX}" NO_DEFAULT_PATH)\nif(rtfw_missing_component_FOUND)\nmessage(FATAL_ERROR "unknown optional component admitted")\nendif()\n'
        (source/'CMakeLists.txt').write_text(cmake,encoding='utf-8')
        command=['cmake','-S',str(source),'-B',str(source/'build'),'-DMIGRATION_PREFIX='+str(args.prefix.resolve()),'-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF','-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF']
        with (source/'configure.log').open('w',encoding='utf-8') as log:
            result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,timeout=60)
        output=(source/'configure.log').read_text(encoding='utf-8')
        okay=(result.returncode==0)==success
        if not success:
            okay=okay and ('Could not find a configuration file for package "rtfw"' in output or 'rtfw_FOUND to FALSE' in output)
        records.append(dict(name=name,request=version,component=component,expected_success=success,
                            command=command,returncode=result.returncode,passed=okay,
                            input_sha256=hashlib.sha256(cmake.encode()).hexdigest(),
                            log_sha256=hashlib.sha256((source/'configure.log').read_bytes()).hexdigest()))
        (root/'report.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')
        if not okay:raise SystemExit('Unexpected discovery result: '+str(source/'configure.log'))
    print('PASS eight current-package version/component requests; retained '+str(root))
if __name__=='__main__':main()
