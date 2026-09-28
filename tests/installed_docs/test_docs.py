import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import generate_sdk_docs as gen
import check_sdk_docs as check
spec = importlib.util.spec_from_file_location('transcripts', ROOT / 'samples/recipes/run_transcripts.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class DocumentationNegatives(unittest.TestCase):
    def test_generated_source_drift_and_missing_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            expected = {'api.html': '<span id="L1">declaration</span>\n'}
            (path / 'api.html').write_text(expected['api.html'])
            gen.verify_outputs(path, expected)
            (path / 'api.html').write_text('stale declaration')
            with self.assertRaisesRegex(ValueError, 'stale'):
                gen.verify_outputs(path, expected)
            (path / 'api.html').unlink()
            with self.assertRaisesRegex(ValueError, 'inventory'):
                gen.verify_outputs(path, expected)

    def test_missing_and_ambiguous_declaration(self):
        self.assertEqual(gen.entry_line('comment\nclass Runtime {\n', '^class Runtime'), 2)
        for source in ['class Other {', 'class Runtime {\nclass Runtime {']:
            with self.assertRaisesRegex(ValueError, 'exactly once'):
                gen.entry_line(source, '^class Runtime')

    def test_header_inventory_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'docs/sdk').mkdir(parents=True)
            (root / 'tests/package_consumer').mkdir(parents=True)
            (root / 'docs/sdk/inventory.json').write_text(json.dumps({'schema_version': 1, 'headers': []}))
            (root / 'tests/package_consumer/package_contract.cmake').write_text('set(expected_headers\nrt/runtime.hpp)')
            with self.assertRaisesRegex(ValueError, 'header inventory'):
                gen.inventory(root)

    def test_local_file_fragment_and_escape(self):
        files = {'manual/README.md': b'[API](api.html#L2)\n', 'manual/api.html': b'<span id="L2">source</span>'}
        check.check_links(files)
        for link in ['missing.html', 'api.html#L3', '../../outside', '/absolute']:
            files['manual/README.md'] = f'[bad]({link})\n'.encode()
            with self.assertRaises(ValueError):
                check.check_links(files)

    def test_markdown_fragments_and_duplicate_html_ids(self):
        check.check_links({'manual/README.md': b'# Hello world\n[here](#hello-world)\n'})
        with self.assertRaisesRegex(ValueError, 'fragment'):
            check.check_links({'manual/README.md': b'# Hello\n[bad](#absent)\n'})
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            check.check_links({'manual/api.html': b'<b id="L1"></b><b id="L1"></b>'})

    def test_transcript_nonzero_and_wrong_output(self):
        with self.assertRaisesRegex(RuntimeError, 'failed'):
            runner.execute([{'argv': [sys.executable, '-c', 'raise SystemExit(3)'], 'contains': ''}], {})
        with self.assertRaisesRegex(RuntimeError, 'output mismatch'):
            runner.execute([{'argv': [sys.executable, '-c', 'print("actual")'], 'contains': 'false success'}], {})

    def test_transcript_literal_arguments_and_missing_value(self):
        # Shell metacharacters and spaces remain one literal argv element.
        value = 'path with spaces $(never-execute) `literal`'
        runner.execute([{'argv': [sys.executable, '-c', 'import sys;print(sys.argv[1])', '{path}'], 'contains': value}], {'path': value})
        with self.assertRaises(KeyError):
            runner.execute([{'argv': ['{missing}'], 'contains': ''}], {})


if __name__ == '__main__':
    unittest.main()
