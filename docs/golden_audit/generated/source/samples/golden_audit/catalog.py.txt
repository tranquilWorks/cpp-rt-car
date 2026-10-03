"""Source-linked audit inventory and offline bundle validation (standard library)."""
import hashlib
import html
import json
from pathlib import Path, PurePosixPath
import re

REQUIREMENTS_SHA = 'cf8054b0e990a836f21327f337156b786872fb47793af012cbe3f45ddf0a97a6'
YAML_SHA = '3b8a9aff7e370bac2ba24a4a24778cb4866fef2ce7d42b4d64c11a4c41e112a5'
BASELINE = '912ce878ad311d38e452a5d2916ee0daa6ee3f72'
FROZEN = '9c93b5d5970caa5dc4570a4d0c43c9ca21ab7aa5a7211c02ce946cf8ffa62771'
REQUIREMENT_BINDING = dict(repository='tranquilWorks/portfolio-control',
    revision='14d624e25e7038fae18d4ab0af5122d4577c4564',
    path='products/cpp-rt-car/acceptance.yaml', blob='cfe72dd40080626953df24355880703e437aa83a',
    yaml_sha256=YAML_SHA, json_sha256=REQUIREMENTS_SHA, target_baseline=BASELINE)
CAPABILITIES = tuple('CAP-M' + str(n) for n in range(15, 27))
STATUS = {'portable_implemented', 'remaining_engineering', 'gated_unperformed', 'claim_limit'}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2) + '\n').encode()


def load(path):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, 'duplicate JSON key: ' + key)
            result[key] = value
        return result
    return json.loads(Path(path).read_text(encoding='utf-8'), object_pairs_hook=pairs,
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))


def safe(path):
    require(isinstance(path, str) and path and '\\' not in path and ':' not in path,
            'invalid relative path')
    p = PurePosixPath(path)
    require(not p.is_absolute() and all(v not in ('', '.', '..') for v in path.split('/')),
            'unsafe relative path: ' + path)
    return p


def read(root, name):
    safe(name)
    path = root / name
    require(path.is_file() and not path.is_symlink() and
            root.resolve() in path.resolve().parents, 'missing/escaped file: ' + name)
    return path.read_bytes()


def requirement_rows(data):
    require(data['schema_version'] == 1, 'requirement schema')
    require(tuple(c['id'] for c in data['capabilities']) == CAPABILITIES,
            'complete CAP-M15 through CAP-M26 inventory')
    rows = {}
    for cap in data['capabilities']:
        for kind, values in [('outcome', [cap['outcome']]), ('scenario', cap['scenarios']),
                             ('automated', cap['evidence']['automated']),
                             ('manual', cap['evidence']['manual']),
                             ('limit', [cap['claim_limit']])]:
            for index, text in enumerate(values):
                rows[f'{cap["id"]}/{kind}/{index + 1}'] = text
    return rows


def disposition(key):
    cap, kind, number = key.split('/')
    n = int(number)
    if kind == 'limit':
        return 'claim_limit'
    if kind == 'manual' or (cap == 'CAP-M18' and kind in ('scenario', 'outcome')):
        return 'gated_unperformed'
    if cap == 'CAP-M19' and ((kind == 'scenario' and n in (1, 3)) or kind in ('automated', 'outcome')):
        return 'remaining_engineering'
    if cap == 'CAP-M20':
        return 'portable_implemented' if kind == 'scenario' and n == 3 else 'remaining_engineering'
    if cap == 'CAP-M26' and kind == 'scenario' and n == 2:
        return 'gated_unperformed'
    return 'portable_implemented'


def scenario_rows(c):
    """Derive coverage from frozen contract structure, independently of the map."""
    result = {}
    for category in ('rates', 'phases', 'channels', 'faults', 'levers', 'artifacts', 'variants'):
        for item in c[category]:
            result[category + '/' + item['id']] = item
    for name in c['resources']:
        result['resources/' + name] = name
    for item in c['state']['fields']:
        result['state/' + item['id']] = item
    for item in c['controls']['schemas']:
        result['controls/' + item['id']] = item
    for item in c['controls']['boundaries']:
        result['boundaries/' + item['kind']] = item
    for item in c['benchmarks']['cases']:
        result['benchmarks/' + item['id']] = item
    for i, edge in enumerate(c['edges']):
        result['edges/' + str(i)] = edge
    # Preserve the whole record as well as individually navigable members.
    for name in ('state', 'clock', 'execution', 'controls', 'telemetry', 'external_cil'):
        result['contracts/' + name] = c[name]
    return result


def verify_inputs(requirements_bytes, yaml_bytes, contract, mapping, audit):
    require(sha(requirements_bytes) == REQUIREMENTS_SHA and sha(yaml_bytes) == YAML_SHA,
            'foreign or changed canonical requirements')
    require(sha(json.dumps(contract, sort_keys=True, separators=(',', ':')).encode()) == FROZEN,
            'changed frozen scenario contract')
    expected = requirement_rows(json.loads(requirements_bytes))
    require(audit['schema'] == 1 and audit['baseline'] == BASELINE, 'audit identity')
    require([r['id'] for r in audit['requirements']] == list(expected),
            'missing/duplicate/reordered requirement')
    for row in audit['requirements']:
        require(row['requirement'] == expected[row['id']], 'changed requirement text')
        require(row['status'] == disposition(row['id']), 'unsupported requirement disposition')
        require(isinstance(row['assessment'], str) and len(row['assessment']) >= 30,
                'missing evidence assessment')
        require(row['sources'] and row['tests'] and row['evidence'], 'missing traceability')
    scenarios = scenario_rows(contract)
    require(mapping['schema'] == 1 and [r['id'] for r in mapping['entries']] == list(scenarios),
            'missing/duplicate/reordered scenario map entry')
    for row in mapping['entries']:
        require(row['contract'] == scenarios[row['id']], 'changed frozen scenario mapping')
        require(row['sources'] and row['tests'] and len(row['explanation']) >= 30,
                'missing scenario explanation/source/test')
    require(audit['remaining_actions'] and len({r['id'] for r in audit['remaining_actions']}) ==
            len(audit['remaining_actions']), 'missing/duplicate remaining action')
    required = {'M18-physical-rt', 'M19-unreal', 'M20-release-engineering', 'M25-human-review',
                'protected-fixture-findings', 'foreign-replay-boundary', 'application-models'}
    require({r['id'] for r in audit['remaining_actions']} == required, 'remaining-action inventory')
    require(all(r['status'] in ('open', 'gated') and r['next_action'] and r['evidence']
                for r in audit['remaining_actions']), 'unjustified remaining-action closure')
    return expected, scenarios


def references(mapping, audit):
    refs = []
    for row in mapping['entries']:
        refs += row['sources'] + row['tests']
    for row in audit['requirements']:
        refs += row['sources'] + row['tests'] + row['evidence']
    for row in audit['remaining_actions']:
        refs += row['evidence']
    return refs


def reference_line(data, ref):
    require(set(ref) == {'path', 'anchor'}, 'source reference schema')
    safe(ref['path'])
    lines = data.decode('utf-8').splitlines()
    matches = [i for i, line in enumerate(lines, 1) if ref['anchor'] in line]
    require(bool(ref['anchor']) and len(matches) == 1,
            'missing/ambiguous source anchor: ' + ref['path'] + ': ' + ref['anchor'])
    return matches[0]


def source_page(data, path):
    body = '\n'.join(f'<span id="L{i}">{html.escape(line)}</span>'
                     for i, line in enumerate(data.decode('utf-8').splitlines(), 1))
    return ('<!doctype html><html lang="en"><meta charset="utf-8"><title>' + html.escape(path) +
            '</title><h1>' + html.escape(path) + '</h1><p>Exact source snapshot for documentation. '
            'Private implementation and test files are not installed SDK APIs.</p><pre>' + body +
            '</pre></html>\n').encode()


def coverage(mapping, audit):
    return {'schema': 1, 'batch': 'M26-06', 'baseline': BASELINE,
        'requirement_count': len(audit['requirements']), 'scenario_entry_count': len(mapping['entries']),
        'dispositions': {s: [r['id'] for r in audit['requirements'] if r['status'] == s] for s in sorted(STATUS)},
        'remaining_actions': [r['id'] for r in audit['remaining_actions']],
        'meaning': 'Source and retained-evidence audit; fresh execution results are separate.',
        'physical': 'NOT_RUN', 'rt': 'NOT_RUN', 'human_acceptance': 'NOT_RUN',
        'controlled_performance': 'NOT_RUN', 'unreal': 'NOT_RUN', 'release': 'NOT_RUN'}



# Reviewed M28-05 reconciliation policy. Updating an obligation is an explicit
# evidence review, never a result inferred from a successful retry or bundle hash.
CLOSEOUT_FINDINGS = {'replay-owner-refusal': 'demonstrated_repair',
 'poll-error-conservation': 'demonstrated_repair',
 'experimental-worker-lifetime': 'demonstrated_repair',
 'experimental-watchdog': 'demonstrated_repair',
 'experimental-clock-sampling': 'demonstrated_repair',
 'experimental-clock-initialization': 'demonstrated_repair',
 'benchmark-acceptance-accounting': 'demonstrated_repair',
 'loopback-cancel-submit': 'unresolved_software',
 'optional-sdk-hal-header': 'unresolved_software',
 'historical-benchmark-predicate': 'unresolved_observation',
 'sampled-startup-pending-cleanup': 'unresolved_observation',
 'windows-startup-2024ms': 'unresolved_observation',
 'windows-negative-overlap': 'unresolved_observation',
 'native-frame-tsan-deadline': 'instrumentation_limitation',
 'partial-runtime-tsan': 'instrumentation_limitation',
 'offline-diagnostic-warning': 'instrumentation_limitation',
 'replay-transactionality': 'unproven_boundary',
 'experimental-global-policy': 'unproven_boundary',
 'application-models': 'claim_boundary',
 'excluded-generated-campaigns': 'owner_excluded',
 'excluded-signature-fixtures': 'owner_excluded',
 'physical-cuda': 'external_acceptance',
 'physical-xdma': 'external_acceptance',
 'physical-combined': 'external_acceptance',
 'rt-policy-endurance': 'external_acceptance',
 'real-unreal': 'external_acceptance',
 'controlled-performance': 'external_acceptance',
 'unfamiliar-consumer': 'external_acceptance',
 'platform-release-review': 'external_acceptance',
 'production-signing-release': 'external_acceptance'}
CLOSEOUT_CARDS = [{'id': 'CAP-M23/manual/1',
  'repository': 'tranquilWorks/portfolio-control',
  'revision': '7cae639252f16bb921a22839f738fb72dc91ae3b',
  'path': 'products/cpp-rt-car/acceptance.yaml',
  'blob': 'cfe72dd40080626953df24355880703e437aa83a',
  'url': 'https://github.com/tranquilWorks/portfolio-control/blob/7cae639252f16bb921a22839f738fb72dc91ae3b/products/cpp-rt-car/acceptance.yaml'},
 {'id': 'CAP-M25/manual/1',
  'repository': 'tranquilWorks/portfolio-control',
  'revision': '7cae639252f16bb921a22839f738fb72dc91ae3b',
  'path': 'products/cpp-rt-car/acceptance.yaml',
  'blob': 'cfe72dd40080626953df24355880703e437aa83a',
  'url': 'https://github.com/tranquilWorks/portfolio-control/blob/7cae639252f16bb921a22839f738fb72dc91ae3b/products/cpp-rt-car/acceptance.yaml'},
 {'id': 'M18-02-nvidia-tuple',
  'repository': 'tranquilWorks/portfolio-control',
  'revision': '7cae639252f16bb921a22839f738fb72dc91ae3b',
  'path': 'products/cpp-rt-car/roadmap.yaml',
  'blob': '3c163b8a4c563e9fe71c52d7f769ccb9d2b93578',
  'url': 'https://github.com/tranquilWorks/portfolio-control/blob/7cae639252f16bb921a22839f738fb72dc91ae3b/products/cpp-rt-car/roadmap.yaml'},
 {'id': 'M18-03-xdma-tuple',
  'repository': 'tranquilWorks/portfolio-control',
  'revision': '7cae639252f16bb921a22839f738fb72dc91ae3b',
  'path': 'products/cpp-rt-car/roadmap.yaml',
  'blob': '3c163b8a4c563e9fe71c52d7f769ccb9d2b93578',
  'url': 'https://github.com/tranquilWorks/portfolio-control/blob/7cae639252f16bb921a22839f738fb72dc91ae3b/products/cpp-rt-car/roadmap.yaml'},
 {'id': 'M18-04-combined-deployment',
  'repository': 'tranquilWorks/portfolio-control',
  'revision': '7cae639252f16bb921a22839f738fb72dc91ae3b',
  'path': 'products/cpp-rt-car/roadmap.yaml',
  'blob': '3c163b8a4c563e9fe71c52d7f769ccb9d2b93578',
  'url': 'https://github.com/tranquilWorks/portfolio-control/blob/7cae639252f16bb921a22839f738fb72dc91ae3b/products/cpp-rt-car/roadmap.yaml'},
 {'id': 'M18-05-rt1-rt2-and-endurance',
  'repository': 'tranquilWorks/portfolio-control',
  'revision': '7cae639252f16bb921a22839f738fb72dc91ae3b',
  'path': 'products/cpp-rt-car/roadmap.yaml',
  'blob': '3c163b8a4c563e9fe71c52d7f769ccb9d2b93578',
  'url': 'https://github.com/tranquilWorks/portfolio-control/blob/7cae639252f16bb921a22839f738fb72dc91ae3b/products/cpp-rt-car/roadmap.yaml'},
 {'id': 'M19-02-unreal-jobs-allocator-clock',
  'repository': 'tranquilWorks/portfolio-control',
  'revision': '7cae639252f16bb921a22839f738fb72dc91ae3b',
  'path': 'products/cpp-rt-car/roadmap.yaml',
  'blob': '3c163b8a4c563e9fe71c52d7f769ccb9d2b93578',
  'url': 'https://github.com/tranquilWorks/portfolio-control/blob/7cae639252f16bb921a22839f738fb72dc91ae3b/products/cpp-rt-car/roadmap.yaml'},
 {'id': 'M19-03-world-lifecycle-multiple-instances-unload',
  'repository': 'tranquilWorks/portfolio-control',
  'revision': '7cae639252f16bb921a22839f738fb72dc91ae3b',
  'path': 'products/cpp-rt-car/roadmap.yaml',
  'blob': '3c163b8a4c563e9fe71c52d7f769ccb9d2b93578',
  'url': 'https://github.com/tranquilWorks/portfolio-control/blob/7cae639252f16bb921a22839f738fb72dc91ae3b/products/cpp-rt-car/roadmap.yaml'},
 {'id': 'M19-04-complete-engine-sample',
  'repository': 'tranquilWorks/portfolio-control',
  'revision': '7cae639252f16bb921a22839f738fb72dc91ae3b',
  'path': 'products/cpp-rt-car/roadmap.yaml',
  'blob': '3c163b8a4c563e9fe71c52d7f769ccb9d2b93578',
  'url': 'https://github.com/tranquilWorks/portfolio-control/blob/7cae639252f16bb921a22839f738fb72dc91ae3b/products/cpp-rt-car/roadmap.yaml'},
 {'id': 'M20-01-controlled-performance-gates',
  'repository': 'tranquilWorks/portfolio-control',
  'revision': '7cae639252f16bb921a22839f738fb72dc91ae3b',
  'path': 'products/cpp-rt-car/roadmap.yaml',
  'blob': '3c163b8a4c563e9fe71c52d7f769ccb9d2b93578',
  'url': 'https://github.com/tranquilWorks/portfolio-control/blob/7cae639252f16bb921a22839f738fb72dc91ae3b/products/cpp-rt-car/roadmap.yaml'},
 {'id': 'M20-02-fuzz-static-security-signing',
  'repository': 'tranquilWorks/portfolio-control',
  'revision': '7cae639252f16bb921a22839f738fb72dc91ae3b',
  'path': 'products/cpp-rt-car/roadmap.yaml',
  'blob': '3c163b8a4c563e9fe71c52d7f769ccb9d2b93578',
  'url': 'https://github.com/tranquilWorks/portfolio-control/blob/7cae639252f16bb921a22839f738fb72dc91ae3b/products/cpp-rt-car/roadmap.yaml'},
 {'id': 'M20-04-platform-migration-soak-and-release',
  'repository': 'tranquilWorks/portfolio-control',
  'revision': '7cae639252f16bb921a22839f738fb72dc91ae3b',
  'path': 'products/cpp-rt-car/roadmap.yaml',
  'blob': '3c163b8a4c563e9fe71c52d7f769ccb9d2b93578',
  'url': 'https://github.com/tranquilWorks/portfolio-control/blob/7cae639252f16bb921a22839f738fb72dc91ae3b/products/cpp-rt-car/roadmap.yaml'},
 {'id': 'M25-06',
  'repository': 'tranquilWorks/portfolio-control',
  'revision': '7cae639252f16bb921a22839f738fb72dc91ae3b',
  'path': 'products/cpp-rt-car/batches/M25-06.yaml',
  'blob': 'ac12e901ec4c12d945d83beda7535d5e84defc22',
  'url': 'https://github.com/tranquilWorks/portfolio-control/blob/7cae639252f16bb921a22839f738fb72dc91ae3b/products/cpp-rt-car/batches/M25-06.yaml'}]
CLOSEOUT_ACTIONS = {'M18-physical-rt': ['physical-cuda', 'physical-xdma', 'physical-combined', 'rt-policy-endurance'],
 'M19-unreal': ['real-unreal'],
 'M20-release-engineering': ['excluded-generated-campaigns',
                             'excluded-signature-fixtures',
                             'controlled-performance',
                             'platform-release-review',
                             'production-signing-release'],
 'M25-human-review': ['unfamiliar-consumer', 'optional-sdk-hal-header'],
 'protected-fixture-findings': ['poll-error-conservation',
                                'experimental-worker-lifetime',
                                'experimental-watchdog',
                                'experimental-clock-sampling',
                                'experimental-clock-initialization',
                                'benchmark-acceptance-accounting',
                                'loopback-cancel-submit',
                                'historical-benchmark-predicate',
                                'sampled-startup-pending-cleanup',
                                'windows-startup-2024ms',
                                'windows-negative-overlap',
                                'native-frame-tsan-deadline',
                                'partial-runtime-tsan',
                                'offline-diagnostic-warning',
                                'experimental-global-policy'],
 'foreign-replay-boundary': ['replay-owner-refusal', 'replay-transactionality'],
 'application-models': ['application-models']}


def closeout_references(ledger):
    refs = []
    for row in ledger['requirements']:
        for kind in ('sources', 'tests', 'evidence'):
            refs.extend(row[kind])
    for row in ledger['findings'] + ledger['deliveries']:
        refs.extend(row['evidence'])
    return refs


def verify_closeout(ledger, audit, reader):
    require(set(ledger) == {'schema', 'batch', 'baseline', 'requirement_binding',
            'claims', 'requirements', 'findings', 'existing_cards', 'remaining_actions',
            'deliveries'}, 'closeout fields')
    require(type(ledger['schema']) is int and ledger['schema'] == 1 and
            ledger['batch'] == 'M28-05' and
            ledger['baseline'] == 'b37c124d6fd7c6ea7dd2b4dffcfc5531b0eb3c8e', 'closeout identity')
    require(ledger['requirement_binding'] == REQUIREMENT_BINDING, 'closeout canonical binding')
    require(ledger['claims'] == dict(software_complete=False, cap_m20_complete=False,
            physical='NOT_RUN', rt='NOT_RUN', unreal='NOT_RUN', human_acceptance='NOT_RUN',
            controlled_performance='NOT_RUN', authenticated_provenance='NOT_RUN',
            release='NOT_RUN', deployment='NOT_RUN') and
            type(ledger['claims']['software_complete']) is bool and
            type(ledger['claims']['cap_m20_complete']) is bool, 'invalid closeout acceptance claim')
    require(ledger['existing_cards'] == CLOSEOUT_CARDS, 'existing owner-stage identities')
    require(ledger['remaining_actions'] == CLOSEOUT_ACTIONS and
            set(ledger['remaining_actions']) == {r['id'] for r in audit['remaining_actions']},
            'closeout remaining-action inventory')
    rows, findings = ledger['requirements'], ledger['findings']
    require([r['id'] for r in rows] == [r['id'] for r in audit['requirements']],
            'closeout missing/duplicate requirement IDs')
    require([f['id'] for f in findings] == list(CLOSEOUT_FINDINGS),
            'closeout missing/duplicate finding IDs')
    ids = {r['id'] for r in rows}
    for f in findings:
        require(set(f) == {'id', 'status', 'requirements', 'evidence', 'assessment',
                'dependencies', 'next_action', 'cards'}, 'closeout finding fields')
        require(f['status'] == CLOSEOUT_FINDINGS[f['id']], 'invalid finding disposition')
        require(f['requirements'] and len(f['requirements']) == len(set(f['requirements'])) and
                set(f['requirements']) <= ids, 'finding requirement links')
        require(len(f['dependencies']) == len(set(f['dependencies'])) and
                set(f['dependencies']) <= set(CLOSEOUT_FINDINGS) and
                f['id'] not in f['dependencies'], 'finding dependency links')
        require(len(f['cards']) == len(set(f['cards'])) and
                set(f['cards']) <= {c['id'] for c in CLOSEOUT_CARDS}, 'finding card links')
        require(f['evidence'] and isinstance(f['assessment'], str) and len(f['assessment']) >= 40
                and isinstance(f['next_action'], str) and len(f['next_action']) >= 30,
                'finding evidence/action missing')
        if f['status'] in ('external_acceptance', 'owner_excluded'):
            require(f['cards'], 'missing existing acceptance identity')
    visiting, visited = set(), set()
    by_id = {f['id']: f for f in findings}
    def visit(key):
        require(key not in visiting, 'cyclic closeout dependencies')
        if key in visited:
            return
        visiting.add(key)
        for dependency in by_id[key]['dependencies']:
            visit(dependency)
        visiting.remove(key)
        visited.add(key)
    for key in by_id:
        visit(key)
    for row, original in zip(rows, audit['requirements']):
        require(set(row) == {'id', 'requirement', 'original_status', 'status', 'evidence',
                'sources', 'tests', 'dependencies', 'assessment', 'next_action'},
                'closeout requirement fields')
        require(row['requirement'] == original['requirement'] and
                row['original_status'] == original['status'], 'closeout requirement drift')
        require(all(row[k] == original[k] for k in ('evidence', 'sources', 'tests')),
                'closeout original evidence drift')
        deps = [f['id'] for f in findings if row['id'] in f['requirements']]
        require(row['dependencies'] == deps, 'closeout requirement dependency mismatch')
        status = ('claim_limit' if '/limit/' in row['id'] else
                  'retained_evidence_with_obligations' if deps else 'retained_portable_evidence')
        require(row['status'] == status, 'invalid requirement acceptance claim')
        require(isinstance(row['assessment'], str) and len(row['assessment']) >= 30 and
                isinstance(row['next_action'], str) and len(row['next_action']) >= 30,
                'requirement assessment/action missing')
    require([d['id'] for d in ledger['deliveries']] ==
            ['M27-%02d' % i for i in range(1, 9)] + ['M28-%02d' % i for i in range(1, 5)],
            'missing predecessor delivery')
    for d in ledger['deliveries']:
        require(set(d) == {'id', 'evidence'} and d['evidence'], 'delivery evidence')
    for ref in closeout_references(ledger):
        reference_line(reader(ref['path']), ref)
    return ledger


def closeout_page(ledger, reader):
    def links(refs):
        return ' · '.join('<a href="source/' + r['path'] + '.html#L' +
                         str(reference_line(reader(r['path']), r)) + '">' +
                         html.escape(r['path']) + '</a>' for r in refs)
    parts = ['<!doctype html><html lang="en"><meta charset="utf-8">'
             '<title>M28-05 acceptance reconciliation</title>'
             '<style>body{max-width:1100px;margin:2rem auto;font:17px/1.5 system-ui}'
             'section{border-top:1px solid #bbb;padding:1rem}table{border-collapse:collapse}'
             'td,th{border:1px solid #bbb;padding:.5rem;vertical-align:top}</style>'
             '<h1>M28-05 acceptance and remaining-work ledger</h1>'
             '<p><a href="index.html">Original golden audit</a> · '
             '<a href="closeout.json">Machine-readable ledger</a></p>'
             '<p>All 112 original requirements are retained. Software-complete and CAP-M20 '
             'completion are not claimed. Physical, RT, Unreal, independent human, controlled '
             'performance, authenticated provenance, release and deployment acceptance remain '
             'NOT_RUN. Dependencies include capability-level context; they do not mean every '
             'implemented subcriterion has failed. Original audit statuses describe implementation, '
             'not current global acceptance. No waiver or human risk acceptance is inferred.</p>']
    parts += ['<h2>Retained findings and obligations</h2>']
    for f in ledger['findings']:
        deps = ' · '.join('<a href="#' + d + '">' + d + '</a>' for d in f['dependencies']) or 'None'
        parts += ['<section id="' + f['id'] + '"><h3>' + f['id'] + ' — ' + f['status'] +
                  '</h3><p>' + html.escape(f['assessment']) + '</p><p>Next: ' +
                  html.escape(f['next_action']) + '</p><p>Dependencies: ' + deps +
                  '</p><p>Existing acceptance identities: ' + html.escape(', '.join(f['cards']) or 'None') +
                  '</p><p>Evidence: ' + links(f['evidence']) + '</p></section>']
    parts += ['<h2>Complete original requirement inventory</h2>']
    for row in ledger['requirements']:
        deps = ' · '.join('<a href="#' + d + '">' + d + '</a>' for d in row['dependencies']) or 'None'
        parts += ['<section id="' + row['id'].replace('/', '-') + '"><h3>' + row['id'] + '</h3><p>' +
                  html.escape(row['requirement']) + '</p><p>Original implementation disposition: ' +
                  row['original_status'] + '; reconciliation: ' + row['status'] + '</p><p>' +
                  html.escape(row['assessment']) + '</p><p>Dependencies: ' + deps + '</p><p>Next: ' +
                  html.escape(row['next_action']) + '</p><p>Source: ' + links(row['sources']) +
                  '</p><p>Checks: ' + links(row['tests']) + '</p><p>Evidence: ' + links(row['evidence']) +
                  '</p></section>']
    parts += ['<h2>Existing final-stage identities</h2><p>These are pointers to existing '
              'canonical cards or criteria, not newly created qualification cards. Pinned '
              'repository paths and URLs below are provenance text; this bundle stays offline.</p><ul>']
    for card in ledger['existing_cards']:
        parts += ['<li>' + html.escape(card['id'] + ' — ' + card['url'] + ' (blob ' + card['blob'] + ')') + '</li>']
    parts += ['</ul><h2>Delivered batch evidence</h2>']
    for d in ledger['deliveries']:
        parts += ['<p>' + d['id'] + ': ' + links(d['evidence']) + '</p>']
    return ('\n'.join(parts) + '</html>\n').encode()

def render(root):
    inputs = root / 'docs/golden_audit'
    requirement_bytes = read(inputs, 'requirements.json')
    yaml_bytes = read(inputs, 'requirements.yaml')
    binding = load(inputs / 'requirements-binding.json')
    require(binding == REQUIREMENT_BINDING, 'canonical requirement binding')
    contract_bytes = read(root, 'samples/golden_system/contract.json')
    contract = json.loads(contract_bytes)
    mapping, audit = load(inputs / 'source-map.json'), load(inputs / 'capabilities.json')
    verify_inputs(requirement_bytes, yaml_bytes, contract, mapping, audit)
    ledger = verify_closeout(load(root / 'docs/closeout/ledger.json'), audit, lambda p: read(root, p))
    refs = references(mapping, audit) + closeout_references(ledger)
    paths = {r['path'] for r in refs} | {'samples/golden_system/contract.json'}
    snapshots = {p: read(root, p) for p in sorted(paths)}
    for ref in refs:
        reference_line(snapshots[ref['path']], ref)
    output = {'requirements.json': requirement_bytes, 'requirements.yaml': yaml_bytes,
              'requirements-binding.json': read(inputs, 'requirements-binding.json'),
              'contract.json': contract_bytes, 'source-map.json': encoded(mapping),
              'capabilities.json': encoded(audit), 'closeout.json': encoded(ledger),
              'closeout.html': closeout_page(ledger, lambda p: read(root, p))}
    for path, data in snapshots.items():
        output['source/' + path + '.txt'] = data
        output['source/' + path + '.html'] = source_page(data, path)
    def links(items):
        return ' · '.join('<a href="source/' + r['path'] + '.html#L' +
                         str(reference_line(snapshots[r['path']], r)) + '">' +
                         html.escape(r['path']) + '</a>' for r in items)
    sections = ['<p><a href="closeout.html">Current M28-05 acceptance and remaining-work ledger</a></p>', '<h2 id="scenario">Complete scenario map</h2>']
    for row in mapping['entries']:
        sections += ['<section id="' + row['id'].replace('/', '-') + '"><h3>' +
                     html.escape(row['id']) + '</h3><p>' + html.escape(row['explanation']) +
                     '</p><p>Source: ' + links(row['sources']) + '</p><p>Checks: ' +
                     links(row['tests']) + '</p><details><summary>Frozen requirement record</summary><pre>' +
                     html.escape(json.dumps(row['contract'], indent=2)) + '</pre></details></section>']
    sections += ['<h2 id="capabilities">CAP-M15 through CAP-M26 audit</h2>']
    for row in audit['requirements']:
        sections += ['<section id="' + row['id'].replace('/', '-') + '"><h3>' +
                     html.escape(row['id']) + ' — ' + row['status'] + '</h3><p>' +
                     html.escape(row['requirement']) + '</p><p>' + html.escape(row['assessment']) +
                     '</p><p>Source: ' + links(row['sources']) + '</p><p>Checks: ' +
                     links(row['tests']) + '</p><p>Retained evidence: ' + links(row['evidence']) +
                     '</p></section>']
    sections += ['<h2 id="remaining">Remaining actions</h2>']
    for row in audit['remaining_actions']:
        sections += ['<section id="' + row['id'] + '"><h3>' + row['id'] + '</h3><p>' +
                     html.escape(row['next_action']) + '</p><p>' + links(row['evidence']) + '</p></section>']
    narrative = read(inputs, 'guide.html').decode()
    output['index.html'] = ('<!doctype html><html lang="en"><meta charset="utf-8">'
                           '<title>RTFW golden system and capability audit</title>'
                           '<style>body{max-width:1000px;margin:2rem auto;padding:0 1rem;'
                           'font:17px/1.5 system-ui}pre{overflow:auto;background:#f2f4f6;padding:1rem}'
                           'section{border-top:1px solid #ddd;padding:1rem 0}</style>' +
                           narrative + '\n'.join(sections) + '</html>\n').encode()
    output['coverage.json'] = encoded(coverage(mapping, audit))
    output['inventory.json'] = encoded({'schema': 1, 'baseline': BASELINE,
        'sources': {p: sha(v) for p, v in snapshots.items()},
        'outputs': {p: sha(v) for p, v in sorted(output.items())}})
    return output


def verify_bundle(directory):
    inventory = load(directory / 'inventory.json')
    require(inventory['schema'] == 1 and inventory['baseline'] == BASELINE, 'bundle identity')
    actual = {p.relative_to(directory).as_posix() for p in directory.rglob('*') if p.is_file()}
    require(actual == set(inventory['outputs']) | {'inventory.json'}, 'bundle file inventory')
    for path, digest in inventory['outputs'].items():
        require(sha(read(directory, path)) == digest, 'stale bundle file: ' + path)
    mapping, audit = load(directory / 'source-map.json'), load(directory / 'capabilities.json')
    require(load(directory / 'requirements-binding.json') == REQUIREMENT_BINDING,
            'canonical requirement binding')
    verify_inputs(read(directory, 'requirements.json'), read(directory, 'requirements.yaml'),
                  load(directory / 'contract.json'), mapping, audit)
    require(load(directory / 'coverage.json') == coverage(mapping, audit), 'fabricated capability coverage')
    ledger = verify_closeout(load(directory / 'closeout.json'), audit,
                             lambda p: read(directory, 'source/' + p + '.txt'))
    require(read(directory, 'closeout.html') == closeout_page(ledger,
            lambda p: read(directory, 'source/' + p + '.txt')), 'fabricated closeout page')
    refs = references(mapping, audit) + closeout_references(ledger)
    require(set(inventory['sources']) == {r['path'] for r in refs} | {'samples/golden_system/contract.json'},
            'source snapshot inventory')
    for path, digest in inventory['sources'].items():
        data = read(directory, 'source/' + path + '.txt')
        require(sha(data) == digest and read(directory, 'source/' + path + '.html') == source_page(data, path),
                'source snapshot/line anchor mismatch')
    for ref in refs:
        reference_line(read(directory, 'source/' + ref['path'] + '.txt'), ref)
    for path in actual:
        if not path.endswith('.html'):
            continue
        content = read(directory, path).decode()
        for target in re.findall(r'href="([^"]+)"', content):
            require(not re.match(r'\w+:|//', target), 'offline manual contains external link')
            location, _, fragment = target.partition('#')
            candidate = (directory / path).parent / location if location else directory / path
            require(candidate.is_file() and directory.resolve() in candidate.resolve().parents,
                    'broken/escaped offline link')
            if fragment:
                require('id="' + fragment + '"' in candidate.read_text(), 'broken offline fragment')
    return inventory
