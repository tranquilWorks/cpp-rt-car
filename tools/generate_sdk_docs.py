#!/usr/bin/env python3
"""Reproduce the offline SDK source reference; deliberately not a C++ parser."""
import argparse
import hashlib
import html
import json
import os
from pathlib import Path, PurePosixPath
import re
from urllib.parse import quote, unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
GENERATED = Path('docs/sdk/generated')
LINK = re.compile(r'(!?\[[^\]\n]*\]\()([^\s)]+)(\))')
REFERENCE = re.compile(r'^(\[[^\]\n]+\]:\s*)(\S+)(.*)$')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def inventory(root):
    data = json.loads((root / 'docs/sdk/inventory.json').read_text(encoding='utf-8'))
    if data['schema_version'] != 1:
        raise ValueError('unsupported SDK inventory schema')
    # Independent default-header contract already enforced by all package gates.
    contract = (root / 'tests/package_consumer/package_contract.cmake').read_text(encoding='utf-8')
    expected = re.search(r'set\(expected_headers\n(.*?)\)', contract, re.S).group(1).split()
    actual = [h['include'] for h in data['headers'] if h['component'] == 'default']
    if sorted(expected) != sorted(actual) or len(set(actual)) != len(actual):
        raise ValueError('default API header inventory differs from package contract')
    optional = {(h['include'], h['component']) for h in data['headers'] if h['component'] != 'default'}
    if optional != {('rt/cuda_driver.hpp', 'cuda_driver'), ('rt/xdma_linux.hpp', 'xdma_linux'), ('rtfw/benchmark.hpp', 'benchmark')}:
        raise ValueError('optional API header inventory differs')
    return data


def page_name(header):
    return 'api/' + header.replace('/', '__') + '.html'


def guide_name(source):
    return 'guides/' + source.replace('/', '__')


def recipe_files(root, data):
    result = {'examples/recipes/' + p.name: p for p in (root / 'samples/recipes').iterdir() if p.is_file()}
    for source in data['consumer_sources']:
        result['examples/recipes/package_consumer/' + Path(source).name] = root / source
    for source in data['support_sources']:
        result['examples/recipes/' + Path(source).name] = root / source
    return result


def shipped_files(root, data):
    """Virtual data-directory inventory, also used to verify a real installation."""
    result = recipe_files(root, data)
    # M26-06: default-installed offline composition/audit manual. This adds no
    # header, compiled target or documentation-generator dependency for users.
    audit = root / 'docs/golden_audit/generated'
    for path in audit.rglob('*'):
        if path.is_file():
            result['golden_audit/' + path.relative_to(audit).as_posix()] = path
    for kit in data['kits']:
        folder = root / 'samples' / kit
        for path in folder.rglob('*'):
            if path.is_file():
                result['examples/' + kit + '/' + path.relative_to(folder).as_posix()] = path
    return result


def entry_line(text, pattern):
    lines = [n for n, line in enumerate(text.splitlines(), 1) if re.search(pattern, line)]
    if len(lines) != 1:
        raise ValueError(f'API declaration must match exactly once: {pattern!r}: {lines}')
    return lines[0]


def relative_link(destination, current):
    return quote(os.path.relpath(destination, str(PurePosixPath(current).parent)).replace('\\', '/'), safe='/._-')


def rewrite_markdown(text, source, destination, mapping, root, revision):
    def target(raw):
        parsed = urlsplit(raw.strip('<>'))
        if parsed.scheme or parsed.netloc or not parsed.path:
            return raw
        path = (root / Path(source).parent / unquote(parsed.path)).resolve()
        try:
            key = path.relative_to(root.resolve()).as_posix()
        except ValueError as exc:
            raise ValueError(f'guide link escapes checkout: {source}: {raw}') from exc
        if not path.exists():
            raise ValueError(f'missing guide source link: {source}: {raw}')
        if key in mapping:
            link = relative_link(mapping[key], destination)
        else:
            kind = 'tree' if path.is_dir() else 'blob'
            link = f'https://github.com/tranquilWorks/cpp-rt-car/{kind}/{revision}/{quote(key)}'
        return link + ('#' + parsed.fragment if parsed.fragment else '')
    result, fenced = [], False
    for line in text.splitlines():
        if line.lstrip().startswith('```'):
            fenced = not fenced
        if not fenced and not line.lstrip().startswith('```'):
            line = LINK.sub(lambda m: m[1] + target(m[2]) + m[3], line)
            line = REFERENCE.sub(lambda m: m[1] + target(m[2]) + m[3], line)
        result.append(line)
    return '\n'.join(result) + '\n'


def render(root=ROOT):
    data = inventory(root)
    version = (root / 'VERSION.txt').read_text(encoding='utf-8').strip()
    mapping = {h['source']: 'manual/' + page_name(h['include']) for h in data['headers']}
    mapping.update({s: 'manual/' + guide_name(s) for s in data['guides']})
    mapping.update({p.relative_to(root).as_posix(): dst for dst, p in shipped_files(root, data).items()})
    mapping['docs/golden_audit/requirements.yaml'] = 'golden_audit/requirements.yaml'
    mapping['docs/sdk/recipes.md'] = 'manual/recipes.md'
    mapping['docs/sdk/generated/README.md'] = 'manual/README.md'
    outputs, header_records = {}, []
    for header in data['headers']:
        source = root / header['source']
        raw = source.read_bytes()
        text = raw.decode('utf-8')
        component = header['component']
        flag = 'Default installed header' if component == 'default' else f'Optional {component} component; this reference does not imply the header/library is installed or qualified'
        body = '\n'.join(f'<span id="L{i}"><a href="#L{i}">{i:5}</a> {html.escape(line)}</span>' for i, line in enumerate(text.splitlines(), 1))
        outputs[page_name(header['include'])] = (f'<!doctype html>\n<html lang="en"><meta charset="utf-8"><title>{html.escape(header["include"])} — RTFW {version}</title>\n'
            '<style>body{font:16px system-ui;margin:2rem}pre{font:13px monospace;overflow:auto}pre a{color:#555}a{color:#0758a5}</style>\n'
            f'<h1>{html.escape(header["include"])}</h1><p>{flag}. Version {version}.</p>\n'
            f'<p>Source: {html.escape(header["source"])}; SHA-256: {digest(raw)}. Exact declaration/comment source; private/detail content is not a supported API promise.</p>\n'
            f'<p><a href="index.html">API index</a> · <a href="../README.md">Manual</a></p><pre>{body}</pre></html>\n')
        header_records.append(dict(header, sha256=digest(raw), lines=len(text.splitlines())))
    rows = []
    labels = set()
    for entry in data['entries']:
        if entry['label'] in labels:
            raise ValueError('duplicate API entry label')
        labels.add(entry['label'])
        header = next(h for h in data['headers'] if h['include'] == entry['header'])
        line = entry_line((root / header['source']).read_text(encoding='utf-8'), entry['pattern'])
        link = Path(page_name(entry['header'])).name + '#L' + str(line)
        rows.append(f'<li><a href="{link}">{html.escape(entry["label"])}</a> — {html.escape(entry["note"])}</li>')
    header_links = ''.join(f'<li><a href="{Path(page_name(h["include"])).name}">{h["include"]}</a> ({h["component"]})</li>' for h in data['headers'])
    outputs['api/index.html'] = ('<!doctype html>\n<html lang="en"><meta charset="utf-8"><title>RTFW API reference</title>\n'
        f'<h1>RTFW {version} public SDK source reference</h1><p><a href="../README.md">Manual</a> · <a href="../recipes.md">Recipes</a></p>\n'
        '<p>Selected entry points link to exact checked declarations. All default and named optional headers are reproduced with comments and line anchors. This is not a semantic C++ index; private/detail declarations remain unsupported internals.</p>\n'
        '<h2>Lifecycle, ownership and integration</h2><ul>' + '\n'.join(rows) + '</ul>\n<h2>Complete header inventory</h2><ul>' + header_links + '</ul></html>\n')
    for source in data['guides']:
        name = guide_name(source)
        outputs[name] = ('[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)\n\n'
            '> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.\n\n' + rewrite_markdown((root / source).read_text(encoding='utf-8'), source, 'manual/' + name, mapping, root, data['repository_revision']))
    transcript = json.loads((root / 'samples/recipes/transcripts.json').read_text(encoding='utf-8'))
    block = '\n\nEach line below is an argv array, not a shell string. Replace `{kit}`, `{build}`, `{prefix}`, `{source}` (empty for installed mode) and `{benchmark}` (`OFF` or `ON`) with explicit paths/options.\n\n```json\n' + json.dumps(transcript['commands'], indent=2) + '\n```\n'
    recipes = (root / 'docs/sdk/recipes.md').read_text(encoding='utf-8')
    if recipes.count('<!-- generated-transcript -->') != 1:
        raise ValueError('recipe transcript marker missing or duplicated')
    outputs['recipes.md'] = rewrite_markdown(recipes.replace('<!-- generated-transcript -->', block), 'docs/sdk/recipes.md', 'manual/recipes.md', mapping, root, data['repository_revision'])
    outputs['README.md'] = f'''# RTFW {version} installed SDK manual

Start with [recipes, build commands, ownership, errors and migration](recipes.md).
Open the [generated source-linked API reference](api/index.html) in a browser.
All 20 default and three optional public headers include exact source/comments
and checked line anchors. Optional reference pages do not imply installed vendor
components or physical qualification. Default SDK builds need no documentation tools.

The adjacent `examples` directory contains the prior source kits and the recipe
project. Sources and guides are included in ordinary CPack archives; this bundle
works after relocation. Repository-only/external links are explicitly HTTPS and
require a connection. The runtime version and exact input hashes are recorded in
[the inventory](inventory.json). Generated content is reproducible and checked
against source during development and against archive bytes during package tests.

The [golden system and capability audit](../golden_audit/index.html) maps the
portable composition to exact source and retained evidence. M25/M26 software
delivery does not establish an independent novice walkthrough, physical
CUDA/XDMA/HIL, RT1/RT2, controlled performance, Unreal, signing or release approval.

## Guides

''' + '\n'.join(f'- [{s}](<'+guide_name(s)+'>)' for s in data['guides']) + '\n'
    inputs = set(data['guides'] + data['consumer_sources'] + data['support_sources'] + ['docs/sdk/recipes.md', 'docs/sdk/inventory.json', 'samples/recipes/transcripts.json'])
    inputs.update(h['source'] for h in data['headers'])
    outputs['inventory.json'] = json.dumps({'schema_version': 1, 'version': version, 'headers': header_records,
        'inputs': {p: digest((root / p).read_bytes()) for p in sorted(inputs)},
        'outputs': {p: digest(v.encode()) for p, v in sorted(outputs.items())}}, indent=2) + '\n'
    return outputs


def verify_outputs(directory, outputs):
    actual = {p.relative_to(directory).as_posix() for p in directory.rglob('*') if p.is_file()}
    if actual != set(outputs):
        raise ValueError('generated documentation file inventory differs')
    for name, expected in outputs.items():
        if (directory / name).read_bytes() != expected.encode():
            raise ValueError('stale generated documentation: ' + name)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    output = render()
    directory = ROOT / GENERATED
    if args.check:
        verify_outputs(directory, output)
    else:
        # Refuse stale files rather than deleting unknown developer work.
        extras = {p.relative_to(directory).as_posix() for p in directory.rglob('*') if p.is_file()} - set(output)
        if extras:
            raise ValueError(f'unexpected generated files: {sorted(extras)}')
        for name, text in output.items():
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(text.encode())
    print(f'PASS: reproducible SDK reference/manual ({len(output)} files)')


if __name__ == '__main__':
    main()
