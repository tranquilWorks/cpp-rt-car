#!/usr/bin/env python3
"""Compare two independent local C++ package builds; never sign or publish."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]
MAX_MEMBERS = 10000
MAX_BYTES = 512 * 1024 * 1024
MAX_LOG_BYTES = 16 * 1024 * 1024


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def query(*args: str, cwd: Path | None = None) -> str:
    return subprocess.check_output(args, cwd=cwd, text=True,
                                   stderr=subprocess.STDOUT, timeout=30).strip()


def source_identity(root: Path, expected: str) -> dict:
    if re.fullmatch(r'[0-9a-f]{40}', expected) is None:
        raise ValueError('expected commit must be a complete lowercase Git SHA')
    if Path(query('git', 'rev-parse', '--show-toplevel', cwd=root)).resolve() != root.resolve():
        raise ValueError('source must be the Git worktree root')
    commit = query('git', 'rev-parse', 'HEAD', cwd=root)
    if commit != expected:
        raise ValueError('source commit differs from expected commit')
    if query('git', 'status', '--porcelain', '--untracked-files=all', cwd=root):
        raise ValueError('source worktree must be clean')
    return {'commit': commit, 'tree': query('git', 'rev-parse', 'HEAD^{tree}', cwd=root),
            'epoch': int(query('git', 'show', '-s', '--format=%ct', 'HEAD', cwd=root)),
            'state': 'clean'}


def archive_inventory(path: Path) -> list[dict]:
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_BYTES:
        raise ValueError('archive must be a regular file within the byte limit')
    rows, seen, total = [], set(), 0
    with tarfile.open(path, 'r:gz') as archive:
        for entry in archive:
            name = PurePosixPath(entry.name)
            if name.is_absolute() or '..' in name.parts or not name.parts:
                raise ValueError('archive member name must be relative')
            key = name.as_posix()
            if key in seen or len(rows) >= MAX_MEMBERS:
                raise ValueError('duplicate member or member count limit')
            seen.add(key)
            if entry.size < 0:
                raise ValueError('negative member extent')
            total += entry.size
            if total > MAX_BYTES:
                raise ValueError('archive member byte limit')
            row = {'name': key, 'mode': entry.mode, 'size': entry.size,
                   'mtime': entry.mtime, 'uid': entry.uid, 'gid': entry.gid,
                   'uname': entry.uname, 'gname': entry.gname,
                   'pax_headers': dict(sorted(entry.pax_headers.items()))}
            if entry.isfile():
                row['kind'] = 'file'
                hasher = hashlib.sha256()
                count = 0
                with archive.extractfile(entry) as stream:
                    for block in iter(lambda: stream.read(1024 * 1024), b''):
                        count += len(block)
                        hasher.update(block)
                if count != entry.size:
                    raise ValueError('incomplete archive member')
                row['sha256'] = hasher.hexdigest()
            elif entry.isdir():
                row['kind'] = 'directory'
            elif entry.issym() or entry.islnk():
                row.update(kind='symlink' if entry.issym() else 'hardlink', target=entry.linkname)
            else:
                raise ValueError('unsupported archive member type')
            rows.append(row)
    if not rows:
        raise ValueError('archive inventory is empty')
    return sorted(rows, key=lambda row: row['name'])


def compare_archives(first: Path, second: Path) -> dict:
    inventories = [archive_inventory(path) for path in (first, second)]
    hashes = [digest(path) for path in (first, second)]
    mappings = [{row['name']: row for row in inventory} for inventory in inventories]
    changed = [name for name in sorted(set(mappings[0]) | set(mappings[1]))
               if mappings[0].get(name) != mappings[1].get(name)]
    return {'archives': [{'path': str(path), 'size': path.stat().st_size,
                           'sha256': sha, 'members': members}
                          for path, sha, members in zip((first, second), hashes, inventories)],
            'archive_bytes_equal': hashes[0] == hashes[1],
            'members_equal': not changed, 'different_members': changed,
            'reproducible_for_recorded_inputs': hashes[0] == hashes[1] and not changed}


def tool_identity(name: str) -> dict:
    found = shutil.which(name)
    if not found:
        raise ValueError('required tool missing: ' + name)
    path = Path(found).resolve()
    return {'path': str(path), 'sha256': digest(path),
            'version': query(str(path), '--version')}


class Runner:
    def __init__(self, work: Path, environment: dict, timeout: int):
        self.work, self.environment, self.timeout = work, environment, timeout
        self.commands = []

    def run(self, *args) -> None:
        command = [str(arg) for arg in args]
        log = self.work / ('command-%02d.log' % (len(self.commands) + 1))
        record = {'command': command, 'cwd': str(self.work), 'log': str(log)}
        self.commands.append(record)
        start = time.monotonic()
        reason = None
        with log.open('xb') as output:
            process = subprocess.Popen(command, cwd=self.work, env=self.environment,
                                       stdout=output, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            try:
                while process.poll() is None:
                    if time.monotonic() - start > self.timeout:
                        reason = 'command timeout'
                    elif log.stat().st_size > MAX_LOG_BYTES:
                        reason = 'command log limit exceeded'
                    if reason:
                        break
                    time.sleep(0.1)
            finally:
                if process.poll() is None:
                    try:
                        os.killpg(process.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        process.wait()
        record.update(exit_code=process.returncode, seconds=time.monotonic() - start,
                      log_sha256=digest(log), log_bytes=log.stat().st_size)
        if reason or process.returncode:
            raise ValueError(reason or 'command failed; see ' + str(log))


def rebuild(root: Path, expected: str, work: Path, cc: str, cxx: str, timeout: int) -> dict:
    if platform.system() != 'Linux':
        raise ValueError('the build command currently supports Linux; archive comparison is portable')
    root, work = root.resolve(), work.resolve()
    if work == root or root in work.parents or work in root.parents:
        raise ValueError('work directory must be outside the source tree')
    if work.exists():
        raise ValueError('work directory must be new')
    source = source_identity(root, expected)
    identities = {name: tool_identity(command) for name, command in
                  [('cc', cc), ('cxx', cxx), ('cmake', 'cmake'), ('cpack', 'cpack'),
                   ('ctest', 'ctest'), ('python', sys.executable), ('git', 'git'),
                   ('make', 'make'), ('ar', 'ar'), ('ld', 'ld')]}
    spec = importlib.util.spec_from_file_location('rebuild_dependencies', root / 'tools/sbom.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    policy = root / 'tools/sbom_expected.json'
    dependencies = module.verify_dependencies(root, policy)
    work.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, SOURCE_DATE_EPOCH=str(source['epoch']), LC_ALL='C', TZ='UTC')
    for key in ('CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'CPATH',
                'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH', 'CMAKE_GENERATOR',
                'CMAKE_TOOLCHAIN_FILE', 'CMAKE_PREFIX_PATH'):
        environment.pop(key, None)
    runner = Runner(work, environment, timeout)
    report = {'schema_version': 1, 'authentication': False, 'publication': False,
              'source': source, 'source_root': str(root), 'tools': identities,
              'dependency_policy_sha256': digest(policy), 'dependencies': dependencies,
              'platform': platform.platform(), 'commands': runner.commands,
              'environment': {'SOURCE_DATE_EPOCH': str(source['epoch']), 'LC_ALL': 'C', 'TZ': 'UTC'},
              'scope': 'Two local builds on this host; not a hermetic build or authenticated provenance.',
              'build_records': [], 'status': 'incomplete'}
    archives = []
    try:
        for label in ('first', 'second'):
            if source_identity(root, expected) != source:
                raise ValueError('source identity changed')
            build, output = work / label / 'build', work / label / 'archives'
            cmake = identities['cmake']['path']
            runner.run(cmake, '-S', root, '-B', build, '-G', 'Unix Makefiles',
                       '-DCMAKE_BUILD_TYPE=Release', '-DENABLE_TESTS=OFF',
                       '-DRTFW_BUILD_EXPERIMENTAL=OFF', '-DRTFW_BUILD_EXAMPLES=OFF',
                       '-DRTFW_BUILD_RUNTIME_DEMO=OFF', '-DRTFW_BUILD_BENCHMARKS=OFF',
                       '-DSIM_SANITIZERS=', '-DSIM_WERROR=ON', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
                       '-DCMAKE_C_COMPILER=' + identities['cc']['path'],
                       '-DCMAKE_CXX_COMPILER=' + identities['cxx']['path'],
                       '-DCMAKE_C_FLAGS=', '-DCMAKE_CXX_FLAGS=',
                       '-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG', '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG',
                       '-DCMAKE_C_COMPILER_LAUNCHER=', '-DCMAKE_CXX_COMPILER_LAUNCHER=',
                       '-DCMAKE_INSTALL_PREFIX=/usr/local', '-DCMAKE_INSTALL_LIBDIR=lib')
            runner.run(cmake, '--build', build, '--parallel', '2')
            runner.run(identities['cpack']['path'], '--config', build / 'CPackConfig.cmake',
                       '-C', 'Release', '-G', 'TGZ', '-B', output,
                       '-D', 'CPACK_PACKAGING_INSTALL_PREFIX=/')
            packages = list(output.glob('*.tar.gz'))
            if len(packages) != 1:
                raise ValueError('expected exactly one CPack archive per build')
            archives.append(packages[0])
            prefix = work / label / 'relocated'
            runner.run(identities['python']['path'], root / 'tools/extract_release_archive.py',
                       '--artifact-dir', output, '--destination', prefix)
            consumer = work / label / 'consumer-source'
            consumer.mkdir()
            for name in ('CMakeLists.txt', 'main.cpp', 'lifecycle.hpp'):
                shutil.copy2(root / 'tests/runtime_robustness' / name, consumer / name)
            runner.run(cmake, '-S', consumer, '-B', work / label / 'consumer-build',
                       '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_PREFIX_PATH=' + str(prefix),
                       '-DCMAKE_CXX_COMPILER=' + identities['cxx']['path'])
            runner.run(cmake, '--build', work / label / 'consumer-build', '--parallel', '2')
            runner.run(identities['ctest']['path'], '--test-dir', work / label / 'consumer-build',
                       '--output-on-failure', '--no-tests=error')
            report['build_records'].append({'directory': str(build),
                'cache_sha256': digest(build / 'CMakeCache.txt'),
                'compile_commands_sha256': digest(build / 'compile_commands.json'),
                'cpack_config_sha256': digest(build / 'CPackConfig.cmake')})
        report['comparison'] = compare_archives(*archives)
        if source_identity(root, expected) != source or module.verify_dependencies(root, policy) != dependencies:
            raise ValueError('source or dependency inputs changed during builds')
        for name, identity in identities.items():
            if tool_identity(identity['path']) != identity:
                raise ValueError('tool changed during builds: ' + name)
        report['status'] = 'pass' if report['comparison']['reproducible_for_recorded_inputs'] else 'mismatch'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        (work / 'report.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    compare = sub.add_parser('compare')
    compare.add_argument('first', type=Path)
    compare.add_argument('second', type=Path)
    build = sub.add_parser('build')
    build.add_argument('--repo-root', type=Path, default=ROOT)
    build.add_argument('--expected-commit', required=True)
    build.add_argument('--work-directory', type=Path, required=True)
    build.add_argument('--cc', default='cc')
    build.add_argument('--cxx', default='c++')
    build.add_argument('--command-timeout', type=int, default=900)
    args = parser.parse_args()
    try:
        if args.command == 'compare':
            result = compare_archives(args.first, args.second)
            print(json.dumps(result, indent=2, sort_keys=True))
            return 0 if result['reproducible_for_recorded_inputs'] else 1
        if not 1 <= args.command_timeout <= 3600:
            raise ValueError('command timeout must be between 1 and 3600 seconds')
        report = rebuild(args.repo_root, args.expected_commit, args.work_directory,
                         args.cc, args.cxx, args.command_timeout)
        print(json.dumps({'status': report['status'], 'report': str(args.work_directory / 'report.json')}))
        return 0 if report['status'] == 'pass' else 1
    except (OSError, EOFError, ValueError, tarfile.TarError, subprocess.SubprocessError) as error:
        print('Build comparison failed: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
