from pathlib import Path
import copy
import json
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/portable_closeout'))
import overlay


class OverlayTests(unittest.TestCase):
    def test_current_mapping(self):
        overlay.validate(json.loads((ROOT / 'docs/closeout/m29-final.json').read_text()))

    def test_omission_false_completion_and_transferred_exclusions_refused(self):
        original = json.loads((ROOT / 'docs/closeout/m29-final.json').read_text())
        mutations = [lambda x: x['requirements'].pop(), lambda x: x['findings'].pop(),
                     lambda x: x['existing_cards'].pop(),
                     lambda x: x['claims'].update(software_complete=True),
                     lambda x: x['claims'].update(CAP_M20_complete=True),
                     lambda x: x['findings'][0].update(current_disposition='external_acceptance'),
                     lambda x: x['requirements'][0].update(dispositions={}),
                     lambda x: x.update(owner_excluded_unsatisfied=[]),
                     lambda x: x.update(next_batch='final_owner_stage')]
        for identity in sorted(overlay.EXCLUDED):
            for disposition in ('passed', 'waived', 'external_acceptance', 'implemented_pending_final_receipts'):
                mutations.append(lambda x, identity=identity, disposition=disposition:
                                 next(f for f in x['findings'] if f['id'] == identity)
                                 .update(current_disposition=disposition))
            mutations.append(lambda x, identity=identity:
                             next(f for f in x['findings'] if f['id'] == identity).update(evidence=[]))
        for i, mutate in enumerate(mutations):
            data = copy.deepcopy(original)
            mutate(data)
            with self.subTest(case=i), self.assertRaises(ValueError):
                overlay.validate(data)


if __name__ == '__main__':
    unittest.main()
