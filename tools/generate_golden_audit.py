#!/usr/bin/env python3
"""Generate or check the source-linked offline golden-system audit."""
import argparse
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'samples/golden_audit'))
import catalog


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    output = catalog.render(ROOT)
    destination = ROOT / 'docs/golden_audit/generated'
    existing = {p.relative_to(destination).as_posix() for p in destination.rglob('*') if p.is_file()}
    catalog.require(not (existing - set(output)), 'unexpected generated files; inspect before removal')
    for name, data in output.items():
        path = destination / name
        if args.check:
            catalog.require(path.is_file() and path.read_bytes() == data, 'stale generated audit: ' + name)
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    catalog.verify_bundle(destination)
    print(f'PASS generated golden audit: {len(output)} files; 100 source maps and 112 requirements')


if __name__ == '__main__':
    main()
