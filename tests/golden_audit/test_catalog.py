"""Meaningful omissions, forged completion, stale source and offline-link negatives."""
import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'samples/golden_audit'))
import catalog as c


class CatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = ROOT / 'docs/golden_audit'
        cls.req = (cls.folder / 'requirements.json').read_bytes()
        cls.yaml = (cls.folder / 'requirements.yaml').read_bytes()
        cls.contract = c.load(ROOT / 'samples/golden_system/contract.json')
        cls.mapping = c.load(cls.folder / 'source-map.json')
        cls.audit = c.load(cls.folder / 'capabilities.json')
        cls.outputs = c.render(ROOT)

    def validate(self, mapping=None, audit=None, requirements=None):
        return c.verify_inputs(requirements or self.req, self.yaml, self.contract,
                               mapping or self.mapping, audit or self.audit)

    def test_complete_canonical_inventory(self):
        requirements, scenarios = self.validate()
        self.assertEqual(len(requirements), 112)
        self.assertEqual(len(scenarios), 100)
        self.assertEqual(set(x.split('/')[0] for x in requirements), set(c.CAPABILITIES))
        self.assertEqual(sum('/outcome/' in x for x in requirements), 12)

    def test_missing_parent_outcome(self):
        data = copy.deepcopy(self.audit)
        data['requirements'] = [r for r in data['requirements'] if r['id'] != 'CAP-M20/outcome/1']
        with self.assertRaisesRegex(ValueError, 'missing/duplicate'): self.validate(audit=data)

    def test_omitted_requirement(self):
        data = copy.deepcopy(self.audit); data['requirements'].pop(12)
        with self.assertRaisesRegex(ValueError, 'missing/duplicate'): self.validate(audit=data)

    def test_duplicate_requirement(self):
        data = copy.deepcopy(self.audit); data['requirements'][5] = data['requirements'][4]
        with self.assertRaisesRegex(ValueError, 'missing/duplicate'): self.validate(audit=data)

    def test_changed_canonical_text(self):
        data = copy.deepcopy(self.audit); data['requirements'][0]['requirement'] += ' relaxed'
        with self.assertRaisesRegex(ValueError, 'changed requirement'): self.validate(audit=data)

    def test_foreign_requirement_snapshot(self):
        with self.assertRaisesRegex(ValueError, 'canonical requirements'):
            self.validate(requirements=self.req.replace(b'CAP-M15', b'CAP-M99'))

    def test_missing_evidence(self):
        data = copy.deepcopy(self.audit); data['requirements'][0]['evidence'] = []
        with self.assertRaisesRegex(ValueError, 'traceability'): self.validate(audit=data)

    def test_manual_and_engineering_cannot_be_promoted(self):
        for key in ('CAP-M18/scenario/1', 'CAP-M19/scenario/1', 'CAP-M20/scenario/1', 'CAP-M25/manual/1'):
            with self.subTest(key=key):
                data = copy.deepcopy(self.audit)
                next(r for r in data['requirements'] if r['id'] == key)['status'] = 'portable_implemented'
                with self.assertRaisesRegex(ValueError, 'disposition'): self.validate(audit=data)

    def test_native_exporters_do_not_promote_release_acceptance(self):
        rows = {r['id']: r for r in self.audit['requirements']}
        self.assertEqual(rows['CAP-M20/scenario/3']['status'], 'portable_implemented')
        for key in ('CAP-M20/outcome/1', 'CAP-M20/scenario/1', 'CAP-M20/scenario/2',
                    'CAP-M20/scenario/4', 'CAP-M20/automated/1'):
            self.assertEqual(rows[key]['status'], 'remaining_engineering')
        self.assertEqual(rows['CAP-M20/manual/1']['status'], 'gated_unperformed')

    def test_missing_phase_and_lever_are_rejected(self):
        for key in ('phases/physics', 'levers/staging_slots', 'contracts/external_cil'):
            with self.subTest(key=key):
                data = copy.deepcopy(self.mapping)
                data['entries'] = [r for r in data['entries'] if r['id'] != key]
                with self.assertRaisesRegex(ValueError, 'scenario map'): self.validate(mapping=data)

    def test_frozen_capacity_cannot_be_reinterpreted(self):
        data = copy.deepcopy(self.mapping)
        next(r for r in data['entries'] if r['id'] == 'channels/plant_sensor')['contract']['ring_slots'] = 2
        with self.assertRaisesRegex(ValueError, 'frozen scenario'): self.validate(mapping=data)

    def test_remaining_actions_cannot_disappear(self):
        data = copy.deepcopy(self.audit); data['remaining_actions'].pop()
        with self.assertRaisesRegex(ValueError, 'remaining-action'): self.validate(audit=data)

    def test_remaining_action_cannot_claim_closure(self):
        data = copy.deepcopy(self.audit); data['remaining_actions'][0]['status'] = 'complete'
        with self.assertRaisesRegex(ValueError, 'closure'): self.validate(audit=data)

    def bundle(self, change):
        with tempfile.TemporaryDirectory(prefix='golden audit docs ') as temporary:
            root = Path(temporary)
            for name, content in self.outputs.items():
                path = root / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(content)
            change(root)
            return c.verify_bundle(root)

    @staticmethod
    def rewrite(root, name, data):
        (root / name).write_bytes(data)
        inventory = c.load(root / 'inventory.json')
        inventory['outputs'][name] = c.sha(data)
        (root / 'inventory.json').write_bytes(c.encoded(inventory))

    def test_complete_offline_bundle(self):
        self.assertGreater(len(self.bundle(lambda root: None)['sources']), 50)

    def test_missing_snapshot(self):
        with self.assertRaisesRegex(ValueError, 'file inventory'):
            self.bundle(lambda root: (root / 'source/samples/golden_system/world.hpp.txt').unlink())

    def test_rehashed_false_coverage(self):
        def change(root):
            data = c.load(root / 'coverage.json'); data['physical'] = 'PASS'
            self.rewrite(root, 'coverage.json', c.encoded(data))
        with self.assertRaisesRegex(ValueError, 'fabricated capability'): self.bundle(change)

    def test_rehashed_foreign_requirement_revision(self):
        def change(root):
            data = c.load(root / 'requirements-binding.json'); data['revision'] = '0' * 40
            self.rewrite(root, 'requirements-binding.json', c.encoded(data))
        with self.assertRaisesRegex(ValueError, 'requirement binding'): self.bundle(change)

    def test_consistently_forged_contract_and_map(self):
        contract = copy.deepcopy(self.contract)
        mapping = copy.deepcopy(self.mapping)
        contract['channels'][0]['ring_slots'] = 2
        next(r for r in mapping['entries'] if r['id'] == 'channels/plant_sensor')['contract']['ring_slots'] = 2
        with self.assertRaisesRegex(ValueError, 'frozen scenario contract'):
            c.verify_inputs(self.req, self.yaml, contract, mapping, self.audit)

    def test_rehashed_bad_offline_fragment(self):
        def change(root):
            data = (root / 'index.html').read_bytes().replace(b'href="#run"', b'href="#nonexistent"', 1)
            self.rewrite(root, 'index.html', data)
        with self.assertRaisesRegex(ValueError, 'offline fragment'): self.bundle(change)

    def test_rehashed_network_dependency(self):
        def change(root):
            data = (root / 'index.html').read_bytes().replace(b'href="#run"', b'href="https://example.invalid"', 1)
            self.rewrite(root, 'index.html', data)
        with self.assertRaisesRegex(ValueError, 'external link'): self.bundle(change)

    def test_rehashed_stale_source_snapshot(self):
        def change(root):
            path = 'source/samples/golden_system/world.hpp.txt'
            self.rewrite(root, path, (root / path).read_bytes().replace(b'struct World {', b'struct Foreign {'))
        with self.assertRaisesRegex(ValueError, 'source snapshot'): self.bundle(change)

    def test_missing_or_ambiguous_anchor(self):
        for data in (b'absent\n', b'match\nmatch\n'):
            with self.assertRaisesRegex(ValueError, 'source anchor'):
                c.reference_line(data, dict(path='source.cpp', anchor='match'))

    def test_escaped_source_path(self):
        for path in ('../outside', '/absolute', 'a/../../x', 'a\\b', 'C:/x', 'a/./b'):
            with self.subTest(path=path), self.assertRaises(ValueError): c.safe(path)


if __name__ == '__main__':
    unittest.main()
