#!/usr/bin/env python3
"""Summarize offline observations without converting a failed probe to a pass."""
import argparse
import json
import re
from pathlib import Path


def summarize(path: Path) -> dict:
    text = path.read_text()
    if 'WARNING: ThreadSanitizer' in text or 'ERROR: AddressSanitizer' in text or 'runtime error:' in text:
        raise ValueError('Sanitizer finding requires separate disposition')
    header = re.search(r'^TRACE count=(\d+) capacity=(\d+) overflow=0$', text, re.M)
    result = re.search(r'^ORIGINAL_EXIT ([01])$', text, re.M)
    if not header or not result:
        raise ValueError('Missing complete initialized diagnostic result')
    events = []
    generation = 0
    for line in text.splitlines():
        if not line.startswith('EVENT '):
            continue
        parts = line.split()
        if int(parts[1]) != len(events):
            raise ValueError('Noncontiguous trace')
        event = {'kind': parts[2], **{k: int(v) for k, v in (p.split('=') for p in parts[3:])}}
        if event['kind'] == 'queued' and event['batch'] == 1:
            generation += 1
        event['generation'] = generation
        events.append(event)
    if len(events) != int(header[1]) or not events or len(events) > int(header[2]):
        raise ValueError('Incomplete trace')
    frames = []
    for start in (e for e in events if e['kind'] == 'frame_queued'):
        group = [e for e in events if (e['generation'], e['batch']) == (start['generation'], start['batch'])]
        by_kind = {e['kind']: e for e in group}
        deadline = start['deadline']
        row = {'generation': start['generation'], 'batch': start['batch'],
               'budget_ns': 500_000, 'queued_after_deadline_start_ns': start['wall'] - (deadline - 500_000),
               'events_relative_to_deadline_ns': {e['kind']: e['wall'] - deadline for e in group},
               'expired_state': by_kind.get('expired', {}).get('state'),
               'quarantined': 'quarantine' in by_kind,
               'shutdown_released': 'shutdown_release' in by_kind}
        if 'submit_enter' in by_kind and 'submit_exit' in by_kind:
            a, b = by_kind['submit_enter'], by_kind['submit_exit']
            if a['thread'] != b['thread']:
                raise ValueError('Cross-thread callback duration')
            row.update(submit_wall_ns=b['wall']-a['wall'], submit_thread_cpu_ns=b['cpu']-a['cpu'])
        if 'delay_begin' in by_kind:
            a, b = by_kind['delay_begin'], by_kind['delay_end']
            row.update(delay_wall_ns=b['wall']-a['wall'], delay_thread_cpu_ns=b['cpu']-a['cpu'])
            # Positive causal control, NOT success of the original full frame.
            if not (int(result[1]) == 1 and 'step status=-18 allocations=0' in text
                    and a['wall'] < deadline <= by_kind['expired']['wall'] < b['wall']
                    and by_kind['expired']['state'] == 3
                    and by_kind['shutdown_release']['state'] == 10
                    and not any(e['kind'] == 'finish' and e['status'] == 0 for e in group)):
                raise ValueError('Delay/timeout/quarantine/retirement control not established')
            row['delay_control_established'] = True
        frames.append(row)
    if not frames:
        raise ValueError('No original 500us frame observed')
    return {'log': str(path), 'original_exit': int(result[1]), 'events': len(events),
            'frames': frames, 'claim': 'Offline scheduling observation; no repair or timing qualification'}

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('logs', nargs='+', type=Path)
    args = p.parse_args()
    print(json.dumps([summarize(path) for path in args.logs], indent=2))
