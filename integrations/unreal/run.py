#!/usr/bin/env python3
"""Build/test/package the source candidate on the existing exact Unreal tuple."""
import argparse
import datetime
import json
import os
import signal
from pathlib import Path
import subprocess
import sys
import prepare

ENGINE_COMMIT = '71fe36aac5a8df5ccd66c763ffc902b29b6a9c43'
TOOLCHAIN = 'v26_clang-20.1.8-rockylinux8'


def command(args, log, env, records, timeout=7200):
    args = list(map(str, args))
    record = {'argv': args, 'log': str(log), 'started': datetime.datetime.now(datetime.timezone.utc).isoformat()}
    records.append(record)
    try:
        with log.open('wb') as stream:
            process = subprocess.Popen(args, stdout=stream, stderr=subprocess.STDOUT,
                                       env=env, start_new_session=True)
            try:
                process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                # Target runner is Linux. Reap the complete build/editor group,
                # not just its shell launcher, before returning a failed gate.
                os.killpg(process.pid, signal.SIGTERM)
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL); process.wait()
                raise
        record['exit_code'] = process.returncode
        if process.returncode:
            raise ValueError(f'command failed ({process.returncode}): {log}')
    except subprocess.TimeoutExpired:
        record['timed_out'] = True
        raise ValueError(f'command timed out: {log}') from None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine-root', required=True, type=Path)
    parser.add_argument('--sdk-root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--configuration', choices=['Development', 'Shipping'], default='Development')
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--package', action='store_true')
    parser.add_argument('--run-packaged', action='store_true')
    parser.add_argument('--pie', action='store_true', help='also run real multi-instance PIE in the full editor')
    args = parser.parse_args()
    records, result = [], {'engine_validation': 'FAIL', 'commands': []}
    output = args.output.absolute()
    try:
        if args.configuration == 'Shipping' and (not args.build_only or args.package):
            raise ValueError('Shipping is compile-only; automation/package validation uses Development')
        if args.run_packaged and not args.package:
            raise ValueError('--run-packaged requires --package')
        if args.pie and args.build_only:
            raise ValueError('--pie cannot be combined with --build-only')
        engine = args.engine_root.resolve(strict=True)
        sdk = args.sdk_root.resolve(strict=True)
        if subprocess.check_output(['git', '-C', str(engine), 'rev-parse', 'HEAD'], text=True).strip() != ENGINE_COMMIT:
            raise ValueError('engine commit differs from the existing approved tuple')
        version = json.loads((engine / 'Engine/Build/Build.version').read_text())
        if [version[k] for k in ('MajorVersion', 'MinorVersion', 'PatchVersion')] != [5, 8, 1]:
            raise ValueError('expected the existing Unreal 5.8.1 source tuple')
        tools = engine / f'Engine/Extras/ThirdPartyNotUE/SDKs/HostLinux/Linux_x64/{TOOLCHAIN}/x86_64-unknown-linux-gnu'
        compiler = tools / 'bin/clang++'
        if 'clang version 20.1.8' not in subprocess.check_output([str(compiler), '--version'], text=True):
            raise ValueError('wrong engine compiler')
        archive_comment = subprocess.check_output(['readelf', '-p', '.comment', str(sdk / 'lib/librtfw_runtime.a')], text=True)
        if 'clang version 20.1.8' not in archive_comment:
            raise ValueError('SDK was not built with the engine compiler')
        manifest = prepare.stage(sdk, output)
        result.update({'engine_commit': ENGINE_COMMIT, 'version': version,
                       'runtime_archive_sha256': manifest['runtime_archive_sha256'],
                       'source_manifest_sha256': prepare.digest(output / 'source-manifest.json')})
        env = dict(os.environ, RTFW_UNREAL_SDK_ROOT=str(sdk))
        project = output / 'project/RTFWUnrealTestHost.uproject'
        reports = output / 'reports'; reports.mkdir()
        build = engine / 'Engine/Build/BatchFiles/Linux/Build.sh'
        common = [f'-Project={project}', '-WaitMutex', '-NoHotReloadFromIDE', '-DisableAdaptiveUnity', '-MaxParallelActions=4']
        if args.configuration == 'Development':
            command([build, 'RTFWUnrealTestHostEditor', 'Linux', 'Development', *common], reports / 'editor-build.log', env, records)
        command([build, 'RTFWUnrealTestHost', 'Linux', args.configuration, *common], reports / 'game-build.log', env, records)
        if not args.build_only:
            demo_source = sdk / 'share/rtfw/integrations/host_lifecycle/plugin.c'
            demo = output / 'host_lifecycle_module.so'
            command([tools / 'bin/clang', '-shared', '-fPIC', '-std=c11', '-I', sdk / 'include', demo_source, '-o', demo],
                    reports / 'extension-build.log', env, records)
            editor = engine / 'Engine/Binaries/Linux/UnrealEditor-Cmd'
            command([editor, project, '-unattended', '-nop4', '-nullrhi', '-nosound', '-nosplash',
                     '-DDC=NoZenLocalFallback', '-ExecCmds=Automation RunTests RTFW.M19_',
                     f'-RTFWTestExtension={demo}', f'-ReportExportPath={reports}',
                     '-TestExit=Automation Test Queue Empty', f'-log={reports / "automation.log"}'],
                    reports / 'automation-commandlet.log', env, records, timeout=1800)
            report = json.loads((reports / 'index.json').read_text(encoding='utf-8-sig'))
            result['automation_tests'] = prepare.validate_report(report, manifest['expected_tests'])
        if args.pie:
            pie_reports = reports / 'pie'; pie_reports.mkdir()
            command([engine / 'Engine/Binaries/Linux/UnrealEditor', project, '/Engine/Maps/Entry',
                     '-unattended', '-nop4', '-nosound', '-nosplash',
                     '-ExecCmds=Automation RunTests RTFW.PIE.WorldsAndCleanup',
                     f'-ReportExportPath={pie_reports}', '-TestExit=Automation Test Queue Empty'],
                    reports / 'pie-editor.log', env, records, timeout=600)
            pie_report = json.loads((pie_reports / 'index.json').read_text(encoding='utf-8-sig'))
            result['pie_tests'] = prepare.validate_report(pie_report, ['RTFW.PIE.WorldsAndCleanup'])
        if args.package:
            uat = engine / 'Engine/Build/BatchFiles/RunUAT.sh'
            archive = output / 'archive'
            command([uat, 'BuildCookRun', f'-project={project}', '-noP4', '-unattended', '-utf8output',
                     '-platform=Linux', '-clientconfig=Development', '-build', '-cook', '-stage', '-pak',
                     '-archive', f'-archivedirectory={archive}', '-map=/Engine/Maps/Entry'],
                    reports / 'package.log', env, records)
            if args.run_packaged:
                launchers = list(archive.rglob('RTFWUnrealTestHost.sh'))
                if len(launchers) != 1:
                    raise ValueError('expected exactly one packaged sample launcher')
                command([launchers[0], '-unattended', '-nullrhi', '-nosound', '-RTFWSample',
                         '-RTFWSampleExit', '-RTFWSampleFrames=120', '-stdout', '-FullStdOutLogOutput'],
                        reports / 'packaged-run.log', env, records, timeout=120)
                log = (reports / 'packaged-run.log').read_text(errors='replace')
                if 'RTFW_SAMPLE PASS frames=120 checksum=16320' not in log or 'RTFW_SAMPLE shutdown failed=0' not in log or 'RTFW_SAMPLE FAIL' in log:
                    raise ValueError('packaged sample did not report the expected model AND checked shutdown')
                result['packaged_sample'] = 'PASS'
        prepare.verify_stage(output, manifest)
        result['engine_validation'] = 'BUILD_ONLY' if args.build_only else 'PASS_SELECTED_GATES'
        print(json.dumps(result, indent=2))
        return 0
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        result['error'] = str(error); print(f'ERROR: {error}', file=sys.stderr)
        return 1
    finally:
        # Staging refuses an existing output. Do not overwrite anyone's prior
        # evidence if preflight/staging failed before this invocation owned it.
        if 'source_manifest_sha256' in result:
            result['commands'] = records
            (output / 'run-result.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    sys.exit(main())
