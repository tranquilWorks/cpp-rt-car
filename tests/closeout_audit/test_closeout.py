"""Reconciliation acceptance controls; no external service or device is used."""
import copy
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'samples/golden_audit'))
import catalog as c


class CloseoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.original = c.load(ROOT / 'docs/closeout/ledger.json')
        cls.audit = c.load(ROOT / 'docs/golden_audit/capabilities.json')
        cls.outputs = c.render(ROOT)

    def check(self, ledger):
        return c.verify_closeout(ledger, self.audit, lambda path: c.read(ROOT, path))

    def refusal(self, change, message):
        data = copy.deepcopy(self.original)
        change(data)
        with self.assertRaisesRegex(ValueError, message):
            self.check(data)

    def test_complete_original_inventory_and_obligations(self):
        ledger = self.check(self.original)
        self.assertEqual(len(ledger['requirements']), 112)
        self.assertEqual(len(ledger['findings']), 30)
        self.assertEqual(len(ledger['deliveries']), 12)
        self.assertEqual(len(ledger['existing_cards']), 13)
        self.assertEqual(len(ledger['remaining_actions']), 7)
        self.assertEqual({f['id'] for f in ledger['findings'] if f['status'] == 'unresolved_software'},
                         {'loopback-cancel-submit', 'optional-sdk-hal-header'})

    def test_missing_requirement(self):
        self.refusal(lambda x: x['requirements'].pop(), 'requirement IDs')

    def test_duplicate_requirement(self):
        self.refusal(lambda x: x['requirements'].append(x['requirements'][0]), 'requirement IDs')

    def test_changed_original_requirement(self):
        self.refusal(lambda x: x['requirements'][0].update(requirement='invented'), 'requirement drift')

    def test_changed_original_evidence(self):
        self.refusal(lambda x: x['requirements'][0].update(evidence=[]), 'original evidence drift')

    def test_invented_requirement_pass(self):
        self.refusal(lambda x: x['requirements'][0].update(status='PASS'), 'acceptance claim')

    def test_all_global_acceptance_claims_refuse(self):
        for name, value in self.original['claims'].items():
            with self.subTest(claim=name):
                self.refusal(lambda x: x['claims'].update({name: True if value is False else 'PASS'}),
                             'acceptance claim')
        self.refusal(lambda x: x['claims'].update(software_complete=0), 'acceptance claim')

    def test_each_finding_remains_present(self):
        for index in range(len(self.original['findings'])):
            with self.subTest(index=index):
                self.refusal(lambda x: x['findings'].pop(index), 'finding IDs')

    def test_duplicate_finding(self):
        self.refusal(lambda x: x['findings'].append(x['findings'][0]), 'finding IDs')

    def test_no_finding_can_be_promoted_by_retry(self):
        for index in range(len(self.original['findings'])):
            with self.subTest(index=index):
                self.refusal(lambda x: x['findings'][index].update(status='demonstrated_repair'
                             if x['findings'][index]['status'] != 'demonstrated_repair' else 'PASS'),
                             'finding disposition')

    def test_missing_finding_evidence_and_action(self):
        self.refusal(lambda x: x['findings'][0].update(evidence=[]), 'evidence/action')
        self.refusal(lambda x: x['findings'][0].update(next_action=''), 'evidence/action')
        self.refusal(lambda x: x['requirements'][0].update(next_action=''), 'assessment/action')

    def test_missing_local_evidence(self):
        self.refusal(lambda x: x['findings'][0]['evidence'][0].update(path='docs/absent-evidence.md'),
                     'missing|unsafe')

    def test_missing_local_anchor(self):
        self.refusal(lambda x: x['findings'][0]['evidence'][0].update(anchor='absent closeout anchor'),
                     'source anchor')

    def test_escaped_reference(self):
        self.refusal(lambda x: x['findings'][0]['evidence'][0].update(path='../outside.md'), 'path')

    def test_requirement_dependency_not_silently_dropped(self):
        row = next(i for i, x in enumerate(self.original['requirements']) if x['dependencies'])
        self.refusal(lambda x: x['requirements'][row].update(dependencies=[]), 'dependency mismatch')

    def test_broken_and_cyclic_finding_dependencies(self):
        self.refusal(lambda x: x['findings'][0].update(dependencies=['absent']), 'dependency links')
        self.refusal(lambda x: x['findings'][0].update(dependencies=[x['findings'][0]['id']]),
                     'dependency links')
        def cycle(x):
            x['findings'][0]['dependencies'] = [x['findings'][1]['id']]
            x['findings'][1]['dependencies'] = [x['findings'][0]['id']]
        self.refusal(cycle, 'cyclic')

    def test_existing_cards_cannot_be_replaced_or_omitted(self):
        self.refusal(lambda x: x['existing_cards'].pop(), 'owner-stage identities')
        self.refusal(lambda x: x['existing_cards'][0].update(revision='0' * 40), 'owner-stage identities')
        row = next(i for i, x in enumerate(self.original['findings']) if x['status'] == 'external_acceptance')
        self.refusal(lambda x: x['findings'][row].update(cards=[]), 'acceptance identity')

    def test_original_action_group_cannot_disappear(self):
        self.refusal(lambda x: x['remaining_actions'].pop('foreign-replay-boundary'), 'remaining-action')

    def test_predecessor_delivery_cannot_disappear(self):
        self.refusal(lambda x: x['deliveries'].pop(), 'predecessor delivery')

    def test_foreign_canonical_binding(self):
        self.refusal(lambda x: x['requirement_binding'].update(revision='0' * 40), 'canonical binding')

    def bundle(self, change):
        with tempfile.TemporaryDirectory(prefix='closeout consumer ') as tmp:
            root = Path(tmp)
            for name, data in self.outputs.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            change(root)
            c.verify_bundle(root)

    @staticmethod
    def rewrite(root, name, data):
        (root / name).write_bytes(data)
        inventory = c.load(root / 'inventory.json')
        inventory['outputs'][name] = c.sha(data)
        (root / 'inventory.json').write_bytes(c.encoded(inventory))

    def test_utf8_bundle_independent_of_windows_default_encoding(self):
        original = Path.read_text
        def windows_default(path, encoding=None, errors=None):
            return original(path, encoding=encoding or 'cp1252', errors=errors)
        with mock.patch.object(Path, 'read_text', windows_default):
            self.bundle(lambda root: None)

    def test_actual_offline_bundle(self):
        self.bundle(lambda root: None)

    def test_rehashed_acceptance_claim_refuses(self):
        def mutate(root):
            data = c.load(root / 'closeout.json')
            data['claims']['software_complete'] = True
            self.rewrite(root, 'closeout.json', c.encoded(data))
        with self.assertRaisesRegex(ValueError, 'acceptance claim'):
            self.bundle(mutate)

    def test_rehashed_rendered_claim_refuses(self):
        def mutate(root):
            data = (root / 'closeout.html').read_bytes().replace(b'NOT_RUN', b'PASS', 1)
            self.rewrite(root, 'closeout.html', data)
        with self.assertRaisesRegex(ValueError, 'fabricated closeout page'):
            self.bundle(mutate)

    def test_missing_bundle_evidence_refuses(self):
        with self.assertRaisesRegex(ValueError, 'file inventory'):
            self.bundle(lambda root: (root / 'source/docs/evidence/M28-05-predecessors.md.txt').unlink())


if __name__ == '__main__':
    unittest.main()
