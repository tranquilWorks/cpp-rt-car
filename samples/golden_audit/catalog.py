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
    refs = references(mapping, audit)
    paths = {r['path'] for r in refs} | {'samples/golden_system/contract.json'}
    snapshots = {p: read(root, p) for p in sorted(paths)}
    for ref in refs:
        reference_line(snapshots[ref['path']], ref)
    output = {'requirements.json': requirement_bytes, 'requirements.yaml': yaml_bytes,
              'requirements-binding.json': read(inputs, 'requirements-binding.json'),
              'contract.json': contract_bytes, 'source-map.json': encoded(mapping),
              'capabilities.json': encoded(audit)}
    for path, data in snapshots.items():
        output['source/' + path + '.txt'] = data
        output['source/' + path + '.html'] = source_page(data, path)
    def links(items):
        return ' · '.join('<a href="source/' + r['path'] + '.html#L' +
                         str(reference_line(snapshots[r['path']], r)) + '">' +
                         html.escape(r['path']) + '</a>' for r in items)
    sections = ['<h2 id="scenario">Complete scenario map</h2>']
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
    refs = references(mapping, audit)
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
