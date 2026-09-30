#!/usr/bin/env python3
"""Validate exact generated source/requirement coverage and offline links."""
import argparse
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'samples/golden_audit'))
import catalog


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path)
    args = parser.parse_args()
    bundle = args.bundle or ROOT / 'docs/golden_audit/generated'
    catalog.verify_bundle(bundle)
    if args.bundle is None:
        for path, data in catalog.render(ROOT).items():
            catalog.require((bundle / path).read_bytes() == data, 'stale generated audit: ' + path)
    print('PASS exhaustive golden source, capability and offline-link audit')


if __name__ == '__main__':
    main()
