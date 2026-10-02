"""Ordinary archive comparison and source-identity regression cases."""
import gzip
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('local_rebuild', ROOT / 'tools/release_rebuild.py')
rebuild = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rebuild)


class BuildComparisonTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def archive(self, name, data=b'library bytes', mode=0o644, target='lib.so.8', stamp=0, duplicate=False):
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode='w') as archive:
            entry = tarfile.TarInfo('lib/lib.so.8')
            entry.size, entry.mode = len(data), mode
            archive.addfile(entry, io.BytesIO(data))
            link = tarfile.TarInfo('lib/lib.so')
            link.type, link.linkname = tarfile.SYMTYPE, target
            archive.addfile(link)
            if duplicate:
                archive.addfile(entry, io.BytesIO(data))
        path = self.root / name
        path.write_bytes(gzip.compress(buffer.getvalue(), mtime=stamp))
        return path

    def cli(self, *args):
        return subprocess.run([sys.executable, str(ROOT / 'tools/release_rebuild.py'), *map(str, args)],
                              text=True, capture_output=True, timeout=30)

    def test_equal_archives_and_real_cli(self):
        first, second = self.archive('first.tar.gz'), self.archive('second.tar.gz')
        result = self.cli('compare', first, second)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertTrue(report['archive_bytes_equal'])
        self.assertTrue(report['reproducible_for_recorded_inputs'])
        row = next(row for row in report['archives'][0]['members'] if row['kind'] == 'file')
        self.assertEqual(row['sha256'], hashlib.sha256(b'library bytes').hexdigest())

    def test_content_mode_link_and_missing_member_differences(self):
        first = self.archive('first.tar.gz')
        for label, options in [('content', {'data': b'different bytes'}),
                               ('mode', {'mode': 0o755}), ('link', {'target': 'lib.so.9'})]:
            with self.subTest(label=label):
                second = self.archive(label + '.tar.gz', **options)
                result = self.cli('compare', first, second)
                self.assertEqual(result.returncode, 1)
                report = json.loads(result.stdout)
                self.assertFalse(report['members_equal'])
                self.assertTrue(report['different_members'])
        missing = self.root / 'missing-member.tar.gz'
        with tarfile.open(missing, 'w:gz') as archive:
            archive.addfile(tarfile.TarInfo('different-name'))
        self.assertFalse(rebuild.compare_archives(first, missing)['members_equal'])

    def test_container_difference_is_not_complete_reproducibility(self):
        first = self.archive('first.tar.gz', stamp=1)
        second = self.archive('second.tar.gz', stamp=2)
        report = rebuild.compare_archives(first, second)
        self.assertTrue(report['members_equal'])
        self.assertFalse(report['archive_bytes_equal'])
        self.assertFalse(report['reproducible_for_recorded_inputs'])

    def test_missing_truncated_and_duplicate_inputs(self):
        first = self.archive('first.tar.gz')
        broken = self.root / 'truncated.tar.gz'
        broken.write_bytes(first.read_bytes()[:8])
        duplicate = self.archive('duplicate.tar.gz', duplicate=True)
        for path in (self.root / 'missing.tar.gz', broken, duplicate):
            with self.subTest(path=path):
                result = self.cli('compare', first, path)
                self.assertNotEqual(result.returncode, 0)

    def test_actual_clean_wrong_and_dirty_source_identities(self):
        repo = self.root / 'repo'
        repo.mkdir()
        def git(*args):
            return subprocess.check_output(['git', *args], cwd=repo, text=True, stderr=subprocess.STDOUT).strip()
        git('init')
        (repo / 'source.cpp').write_text('int main() { return 0; }\n')
        git('add', 'source.cpp')
        git('-c', 'user.name=Build fixture', '-c', 'user.email=fixture@example.invalid',
            '-c', 'commit.gpgsign=false', 'commit', '-m', 'fixture')
        head = git('rev-parse', 'HEAD')
        self.assertEqual(rebuild.source_identity(repo, head)['tree'], git('rev-parse', 'HEAD^{tree}'))
        if sys.platform.startswith('linux'):
            result = self.cli('build', '--repo-root', repo, '--expected-commit', head,
                              '--work-directory', self.root / 'new-output')
            self.assertEqual(result.returncode, 1)
            self.assertIn('driver must belong', result.stderr)
        with self.assertRaisesRegex(ValueError, 'differs'):
            rebuild.source_identity(repo, '0' * 40)
        (repo / 'extra.txt').write_text('untracked input')
        with self.assertRaisesRegex(ValueError, 'clean'):
            rebuild.source_identity(repo, head)
        (repo / 'extra.txt').unlink()
        (repo / 'source.cpp').write_text('changed source')
        with self.assertRaisesRegex(ValueError, 'clean'):
            rebuild.source_identity(repo, head)

    @unittest.skipUnless(sys.platform.startswith('linux'), 'runner is Linux-only')
    def test_normal_command_failure_preserves_output_and_exit(self):
        runner = rebuild.Runner(self.root, dict(os.environ), 30)
        with self.assertRaisesRegex(ValueError, 'command failed'):
            runner.run(sys.executable, '-c', 'print("ordinary failed check"); raise SystemExit(3)')
        self.assertEqual(runner.commands[0]['exit_code'], 3)
        self.assertIn('ordinary failed check', Path(runner.commands[0]['log']).read_text())

    @unittest.skipUnless(sys.platform.startswith('linux'), 'runner is Linux-only')
    def test_finished_command_over_log_limit_is_not_accepted(self):
        runner = rebuild.Runner(self.root, dict(os.environ), 30)
        with mock.patch.object(rebuild, 'MAX_LOG_BYTES', 8):
            with self.assertRaisesRegex(ValueError, 'log limit'):
                runner.run(sys.executable, '-c', 'print("ordinary output beyond configured limit")')
        self.assertGreater(runner.commands[0]['log_bytes'], 8)

    @unittest.skipUnless(sys.platform.startswith('linux'), 'build command is Linux-only')
    def test_existing_output_refused_without_changes(self):
        marker = self.root / 'keep.txt'
        marker.write_text('preserve')
        result = self.cli('build', '--repo-root', ROOT, '--expected-commit', '0' * 40,
                          '--work-directory', self.root)
        self.assertEqual(result.returncode, 1)
        self.assertIn('must be new', result.stderr)
        self.assertEqual(marker.read_text(), 'preserve')


if __name__ == '__main__':
    unittest.main()
