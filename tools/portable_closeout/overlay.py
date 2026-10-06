#!/usr/bin/env python3
"""Reconcile the narrowed closeout without discharging excluded acceptance."""
from pathlib import Path
import argparse
import hashlib
import json

ROOT = Path(__file__).resolve().parents[2]
EXCLUDED = {'excluded-generated-campaigns', 'excluded-signature-fixtures'}


def validate(value: dict, root: Path = ROOT) -> dict:
    raw = (root / 'docs/closeout/ledger.json').read_bytes()
    old = json.loads(raw)
    previous = json.loads((root / 'docs/closeout/m29-current.json').read_text())
    if value.get('schema') != 1 or value.get('batch') != 'M29-03':
        raise ValueError('overlay version/batch')
    if value.get('historical_ledger_sha256') != hashlib.sha256(raw).hexdigest():
        raise ValueError('historical binding')
    if value.get('claims') != previous['claims']:
        raise ValueError('unsatisfied acceptance prohibits completion claims')
    if value.get('existing_cards') != old['existing_cards'] or len(value['existing_cards']) != 13:
        raise ValueError('original card identities')
    if value.get('owner_excluded_unsatisfied') != sorted(EXCLUDED):
        raise ValueError('explicit owner exclusions')
    for kind, count in [('requirements', 112), ('findings', 30)]:
        rows = value.get(kind, [])
        if (len(rows) != count or len({x['id'] for x in rows}) != count or
                {x['id'] for x in rows} != {x['id'] for x in previous[kind]}):
            raise ValueError(kind + ' identities')
    index = {x['id']: x for x in value['findings']}
    for before in previous['findings']:
        after = index[before['id']]
        if before['id'] not in EXCLUDED:
            if after != before:
                raise ValueError('unselected finding drift')
            continue
        for key in ('id', 'historical_status', 'requirements', 'cards'):
            if after[key] != before[key]:
                raise ValueError('historical finding identity drift')
        if after['current_disposition'] != 'excluded_by_owner_unsatisfied':
            raise ValueError('excluded acceptance must remain unsatisfied')
        if not after['assessment'] or after['evidence'] != [{
                'path': 'docs/evidence/M29-03-2026-10-06.md',
                'anchor': '## Owner-excluded unsatisfied acceptance'}]:
            raise ValueError('owner exclusion evidence')
        evidence = root / after['evidence'][0]['path']
        if after['evidence'][0]['anchor'] not in evidence.read_text(encoding='utf-8'):
            raise ValueError('missing exclusion evidence')
    old_requirements = {x['id']: x for x in previous['requirements']}
    for row in value['requirements']:
        before = old_requirements[row['id']]
        if any(row[k] != before[k] for k in before if k != 'dispositions'):
            raise ValueError('requirement/evidence mapping drift')
        ids = [x['id'] for x in value['findings'] if row['id'] in x['requirements']]
        if row['findings'] != ids or row['dispositions'] != {
                i: index[i]['current_disposition'] for i in ids}:
            raise ValueError('requirement disposition mapping')
    if value.get('additional_software_prerequisite') != previous['additional_software_prerequisite']:
        raise ValueError('prerequisite drift')
    if value.get('next_batch') != 'owner_decision_with_cyber_acceptance_unsatisfied':
        raise ValueError('remaining acceptance boundary')
    return value


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--overlay', type=Path, default=ROOT / 'docs/closeout/m29-final.json')
    args = parser.parse_args()
    validate(json.loads(args.overlay.read_text(encoding='utf-8')))
    print('PASS 112 requirements, 30 findings, 13 cards; cyber criteria unsatisfied')
