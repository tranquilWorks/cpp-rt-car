#!/usr/bin/env python3
"""Prepare a disposable Unreal project or validate a real automation report.

Staging never runs an engine and never produces engine acceptance evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expected_tests(plugin):
    names = []
    for source in sorted((plugin / 'Source/RTFWUnrealTests/Private').glob('*.cpp')):
        names.extend(re.findall(r'"(RTFW\.M19_\d+\.[A-Za-z0-9_.]+)"', source.read_text()))
    if not names or len(set(names)) != len(names):
        raise ValueError('empty or duplicate Unreal automation inventory')
    return sorted(names)


def stage(sdk, output, integration=ROOT, template=None):
    sdk, output, integration = sdk.resolve(), output.absolute(), integration.resolve()
    if output.exists():
        raise ValueError('output already exists; select a fresh disposable directory')
    if template is None:
        template = integration / 'project'
        if not template.is_dir():
            template = integration.parents[1] / 'tests/unreal'
    template = template.resolve()
    plugin = integration / 'RTFWUnreal'
    archives = [sdk / 'lib/librtfw_runtime.a', sdk / 'lib/rtfw_runtime.lib']
    archive = next((p for p in archives if p.is_file()), archives[0])
    required = [sdk / 'include/rt/runtime.hpp', archive,
                sdk / 'share/rtfw/integrations/host_lifecycle/lifecycle.hpp',
                sdk / 'share/rtfw/integrations/unreal/portable/rtfw_unreal/worlds.hpp',
                sdk / 'share/rtfw/integrations/unreal/portable/rtfw_unreal/sample.hpp',
                template / 'RTFWUnrealTestHost.uproject', plugin / 'RTFWUnreal.uplugin']
    if any(not p.is_file() or not p.stat().st_size for p in required):
        raise ValueError('incomplete template, integration, or installed SDK')
    for tree in (plugin, template):
        if any(p.is_symlink() for p in tree.rglob('*')):
            raise ValueError('source template must not contain symlinks')
    names = expected_tests(plugin)
    output.mkdir(parents=True)
    shutil.copytree(template, output / 'project')
    shutil.copytree(plugin, output / 'project/Plugins/RTFWUnreal')
    files = {p.relative_to(output / 'project').as_posix(): digest(p)
             for p in sorted((output / 'project').rglob('*')) if p.is_file()}
    manifest = {'schema_version': 1, 'engine_validation': 'NOT_RUN',
                'sdk_root': str(sdk), 'runtime_archive': archive.relative_to(sdk).as_posix(),
                'runtime_archive_sha256': digest(archive),
                'runtime_header_sha256': digest(required[0]),
                'files': files, 'expected_tests': names}
    manifest['sdk_source_sha256'] = {
        p.relative_to(sdk).as_posix(): digest(p)
        for directory in (sdk / 'include', sdk / 'share/rtfw/integrations/host_lifecycle',
                          sdk / 'share/rtfw/integrations/unreal/portable')
        for p in sorted(directory.rglob('*')) if p.is_file()}
    (output / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def validate_report(report, names):
    tests = report.get('tests')
    if not isinstance(tests, list) or not tests:
        raise ValueError('missing engine test records')
    actual = [test.get('fullTestPath') for test in tests]
    if len(set(actual)) != len(actual) or set(actual) != set(names):
        raise ValueError('automation inventory differs from staged source')
    if any(test.get('state') != 'Success' for test in tests):
        raise ValueError('an engine test did not succeed')
    for field in ('failed', 'notRun', 'inProcess'):
        if type(report.get(field)) is not int or report[field] != 0:
            raise ValueError(f'nonzero or missing {field} count')
    succeeded = report.get('succeeded')
    warnings = report.get('succeededWithWarnings', 0)
    if type(succeeded) is not int or type(warnings) is not int or warnings != 0 or succeeded != len(names):
        raise ValueError('inconsistent success/warning counts')
    return len(names)


def verify_stage(output, manifest):
    for relative, expected in manifest['files'].items():
        path = output / 'project' / relative
        if not path.is_file() or digest(path) != expected:
            raise ValueError(f'staged source changed during validation: {relative}')
    sdk = Path(manifest['sdk_root'])
    for relative, expected in manifest['sdk_source_sha256'].items():
        path = sdk / relative
        if not path.is_file() or digest(path) != expected:
            raise ValueError(f'installed source changed during validation: {relative}')
    if digest(sdk / manifest['runtime_archive']) != manifest['runtime_archive_sha256'] or digest(sdk / 'include/rt/runtime.hpp') != manifest['runtime_header_sha256']:
        raise ValueError('installed Runtime changed during validation')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest='action', required=True)
    staging = actions.add_parser('stage')
    staging.add_argument('--sdk-root', type=Path, required=True)
    staging.add_argument('--output', type=Path, required=True)
    validation = actions.add_parser('report')
    validation.add_argument('--manifest', type=Path, required=True)
    validation.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.action == 'stage':
            result = stage(args.sdk_root, args.output)
            print(json.dumps({'stage': str(args.output.absolute()), 'engine_validation': result['engine_validation'],
                              'expected_tests': len(result['expected_tests'])}))
        else:
            manifest = json.loads(args.manifest.read_text())
            count = validate_report(json.loads(args.report.read_text(encoding='utf-8-sig')), manifest['expected_tests'])
            print(f'PASS {count} actual engine test records')
    except (ValueError, OSError, KeyError, TypeError) as error:
        parser.exit(1, f'ERROR: {error}\n')


if __name__ == '__main__':
    main()
