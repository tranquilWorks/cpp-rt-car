#!/usr/bin/env python3
"""Bounded fixed public-API repetition; no daemon, scheduling or hardware access."""
import argparse
import hashlib
import json
import subprocess
import time
from pathlib import Path

FIELDS = dict(frames=193, logical_ns=19300, owners=2, acquired=6, released=6,
              live=0, checkpoint_reexecutions=1, expected_refusals=2)

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def run(command, cycles, seconds, output):
    if not 1 <= cycles <= 10000 or not 0 < seconds <= 3600:
        raise ValueError('cycles must be 1..10000; seconds must be >0 and <=3600')
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    args = [*map(str, command), '--cycles', str(cycles)]
    report = dict(schema=1, command=args, requested_cycles=cycles, maximum_wall_seconds=seconds,
                  executable_sha256=digest(command[0]), runner_sha256=digest(__file__),
                  started_unix_ns=time.time_ns(), status='started',
                  resource_scope='three provider regions per owner; not total process memory',
                  claim='bounded portable fixed-workload observation; no endurance or timing qualification')
    journal = output / 'journal.jsonl'
    def record():
        with journal.open('a', encoding='utf-8') as stream:
            stream.write(json.dumps(report, sort_keys=True) + '\n')
            stream.flush()
    record()
    start = time.monotonic()
    child = None
    reason = None
    try:
        with (output / 'stdout.log').open('wb') as stdout, (output / 'stderr.log').open('wb') as stderr:
            child = subprocess.Popen(args, stdout=stdout, stderr=stderr)
            while child.poll() is None:
                if time.monotonic() - start >= seconds:
                    reason = 'timeout'; break
                if stdout.tell() + stderr.tell() > 8 * 1024 * 1024:
                    reason = 'log_limit'; break
                time.sleep(0.02)
    except KeyboardInterrupt:
        reason = 'interrupted'
    except OSError as error:
        reason = 'launch_failed'
        report['error'] = str(error)
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill(); child.wait()
    report['elapsed_wall_seconds'] = time.monotonic() - start
    report['returncode'] = child.returncode if child is not None else None
    report['status'] = reason or ('passed' if report['returncode'] == 0 else 'failed')
    report['completed_cycles'] = 0
    try:
        lines = (output / 'stdout.log').read_text(encoding='utf-8').splitlines()
        for index, line in enumerate(lines, 1):
            value = json.loads(line)
            expected = dict(FIELDS, cycle=index)
            if value != expected or any(type(value[k]) is not int for k in expected):
                raise ValueError('cycle record does not match fixed workload/resource contract')
            report['completed_cycles'] += 1
        if report['status'] == 'passed' and len(lines) != cycles:
            raise ValueError('successful process did not finish the requested cycles')
        if len(lines) > cycles:
            raise ValueError('extra cycle records')
        if digest(command[0]) != report['executable_sha256']:
            raise ValueError('executable changed during observation')
    except (ValueError, OSError) as error:
        report['validation_error'] = str(error)
        if report['status'] == 'passed':
            report['status'] = 'invalid_output'
    report['totals'] = {key: value * report['completed_cycles'] for key, value in FIELDS.items()}
    report['logs'] = {name: dict(bytes=(output / name).stat().st_size, sha256=digest(output / name))
                      for name in ('stdout.log', 'stderr.log') if (output / name).exists()}
    record()
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--cycles', type=int, default=1024)
    parser.add_argument('--maximum-seconds', type=float, default=300)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = run([args.executable.resolve()], args.cycles, args.maximum_seconds, args.output)
    except (OSError, ValueError) as error:
        parser.exit(2, str(error) + '\n')
    print(json.dumps(report, sort_keys=True))
    return {'passed': 0, 'timeout': 124, 'interrupted': 130}.get(report['status'], 1)

if __name__ == '__main__':
    raise SystemExit(main())
