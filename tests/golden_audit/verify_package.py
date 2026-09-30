"""Actual default/optional CPack relocation, full SDK and installed audit soak."""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'samples/golden_audit'))
import catalog
spec = importlib.util.spec_from_file_location('audit_package_helpers', ROOT / 'tests/installed_docs/verify_package.py')
h = importlib.util.module_from_spec(spec); spec.loader.exec_module(h)
run = h.run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-directory', type=Path, required=True)
    args = parser.parse_args()
    record = args.work_directory.resolve()
    catalog.require(not record.exists(), 'package work directory must be new')
    record.mkdir(parents=True)
    # Retain actual archives, source, binaries and raw evidence outside the checkout.
    work = Path(tempfile.mkdtemp(prefix='golden audit relocated ')).resolve()
    catalog.require(ROOT not in work.parents and work != ROOT, 'relocation must be outside checkout')
    (record / 'relocation-directory.txt').write_text(str(work) + '\n')
    sdk = work / 'SDK build'
    for optional in (False, True):
        label = 'optional' if optional else 'default'
        original = work / (label + ' original prefix')
        prefix = work / (label + ' relocated SDK with spaces')
        archives = work / (label + ' archives')
        run('cmake', '-S', ROOT, '-B', sdk, '-DCMAKE_BUILD_TYPE=Release', '-DENABLE_TESTS=OFF',
            '-DRTFW_BUILD_EXAMPLES=OFF', '-DRTFW_BUILD_RUNTIME_DEMO=OFF', '-DRTFW_BUILD_EXPERIMENTAL=OFF',
            '-DRTFW_BUILD_BENCHMARKS=' + ('ON' if optional else 'OFF'), '-DRTFW_ENABLE_CUDA=OFF',
            '-DSIM_SANITIZERS=', '-DSIM_WERROR=ON', '-DCMAKE_INSTALL_INCLUDEDIR=sdk include',
            '-DCMAKE_INSTALL_DATADIR=custom data', '-DCMAKE_INSTALL_PREFIX=' + str(original))
        run('cmake', '--build', sdk, '--config', 'Release', '--parallel', '2')
        run('cmake', '--install', sdk, '--config', 'Release', '--prefix', original)
        run('cpack', '--config', sdk / 'CPackConfig.cmake', '-C', 'Release', '-B', archives,
            '-D', 'CPACK_PACKAGING_INSTALL_PREFIX=/', '-D', 'CPACK_PACKAGE_FILE_NAME=rtfw-golden-audit-' + label)
        shutil.rmtree(original)
        run(sys.executable, ROOT / 'tools/extract_release_archive.py', '--artifact-dir', archives,
            '--destination', prefix)
        data = prefix / 'custom data/rtfw'
        manual = data / 'golden_audit'
        catalog.verify_bundle(manual)
        for name, expected in catalog.render(ROOT).items():
            catalog.require((manual / name).read_bytes() == expected, 'actual archive manual differs from source')
        if not optional:
            catalog.require(not (data / 'examples/golden_audit').exists(), 'optional runner leaked into default SDK')
            continue
        kit = data / 'examples/golden_audit'
        expected = {p.name for p in (ROOT / 'samples/golden_audit').iterdir() if p.is_file()} | {'provenance.json'}
        catalog.require({p.name for p in kit.iterdir()} == expected, 'complete audit source kit inventory')
        build = work / 'consumer build'
        evidence = work / 'installed evidence'
        run(sys.executable, kit / 'run.py', '--prefix', prefix, '--build', build,
            '--output', evidence, '--cycles', '16', '--clock', 'steady', '--config', 'Release')
        catalog.require((build / 'compile_commands.json').is_file(), 'compile command evidence is required')
        h.check_no_private_paths(build, [ROOT, sdk, original])
        run(sys.executable, kit / 'run.py', '--verify', evidence, '--binaries-build', build, '--config', 'Release')
        run(sys.executable, ROOT / 'tests/golden_audit/negative_evidence.py', '--kit', kit,
            '--evidence', evidence, '--build', build, '--manual', manual, '--config', 'Release')
        h.build_test(ROOT / 'tests/package_consumer', work / 'complete SDK consumers',
                     '-DCMAKE_PREFIX_PATH=' + str(prefix), '-DRTFW_TEST_BENCHMARK=ON')
        inventory = json.loads(run('ctest', '--test-dir', work / 'complete SDK consumers', '-C', 'Release', '--show-only=json-v1'))
        catalog.require(len(inventory['tests']) == 71, 'all prior 71 optional SDK consumers required')
        default = work / 'default relocated SDK with spaces'
        run('cmake', '-S', data / 'examples/golden_showcase', '-B', work / 'missing component',
            '-DCMAKE_PREFIX_PATH=' + str(default), failure='set rtfw_FOUND to FALSE')
        run(sys.executable, kit / 'run.py', '--binaries-build', work / 'absent', '--output', work / 'bad binaries',
            failure='missing executable')
        run(sys.executable, kit / 'run.py', '--binaries-build', build, '--output', work / 'bad cycles',
            '--cycles', '1', failure='cycles must be 2..32')
        run(sys.executable, kit / 'run.py', '--binaries-build', build, '--output', work / 'missing provenance',
            '--provenance', work / 'absent.json', failure='absent.json')
        missing = kit / 'README.md'; saved = missing.read_bytes()
        try:
            missing.unlink()
            run(sys.executable, kit / 'run.py', '--binaries-build', build, '--output', work / 'missing source',
                failure='source inventory/digests')
        finally: missing.write_bytes(saved)
        corrupt = manual / 'index.html'; saved = corrupt.read_bytes()
        try:
            corrupt.write_bytes(saved + b'altered')
            run(sys.executable, kit / 'run.py', '--binaries-build', build, '--output', work / 'stale manual',
                failure='stale bundle file')
        finally: corrupt.write_bytes(saved)
        catalog.require(not any((work / name).exists() for name in ('bad binaries', 'bad cycles',
            'missing provenance', 'missing source', 'stale manual')), 'negative run created evidence')
    print('PASS default/optional actual CPack, removed prefixes, relocated offline manual, installed soak, all 71 SDK consumers and negatives')


if __name__ == '__main__':
    main()
