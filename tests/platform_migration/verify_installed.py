#!/usr/bin/env python3
"""Mandatory installed-consumer build checks; retain the frozen CTest inventory."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('current', 'legacy', 'prefix', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='attempt-', dir=args.output))
    records = []
    outputs = []
    commands = [('current', [str(args.current)], 60),
                ('legacy', [str(args.legacy)], 60),
                ('discovery', [sys.executable, str(Path(__file__).with_name('discovery.py')),
                 '--prefix', str(args.prefix), '--output', str(root / 'discovery')], 180)]
    try:
        for name, command, timeout in commands:
            record = dict(name=name, command=command, timeout_seconds=timeout, status='started')
            records.append(record)
            (root / 'report.json').write_text(json.dumps(records, indent=2) + '\n', encoding='utf-8')
            with (root / (name + '.log')).open('w', encoding='utf-8') as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
            data = (root / (name + '.log')).read_bytes()
            print(data.decode('utf-8'), end='', flush=True)
            record.update(returncode=result.returncode, log_sha256=hashlib.sha256(data).hexdigest())
            if result.returncode:
                raise RuntimeError(name + ' returned nonzero')
            record['status'] = 'passed'
            if name != 'discovery':
                rows = [json.loads(line) for line in data.decode('utf-8').splitlines()]
                if len(rows) != 2 or [row['cycle'] for row in rows] != [1, 2]:
                    raise ValueError('missing/extra lifecycle rows')
                for row in rows:
                    if row['acquired'] != 6 or row['released'] != 6 or row['live'] != 0:
                        raise ValueError('provider conservation mismatch')
                outputs.append(data)
        if outputs[0] != outputs[1]:
            raise ValueError('current and legacy consumer observations differ')
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired) as error:
        if records:
            records[-1].update(status='failed', error=str(error))
        print('FAIL installed migration verification: ' + str(error), file=sys.stderr)
        return 1
    finally:
        (root / 'report.json').write_text(json.dumps(records, indent=2) + '\n', encoding='utf-8')
    print('PASS installed migration build verification: two equivalent consumers and eight discovery cases; ' + str(root))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
