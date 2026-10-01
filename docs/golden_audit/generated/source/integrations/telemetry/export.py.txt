#!/usr/bin/env python3
"""Bounded non-RT schema2 spool -> native Perfetto and OTLP/HTTP protobuf.

No implicit clock inference, background thread, redirect or automatic retry.
Optional protobuf imports occur only when a native encoder is selected.
"""
import argparse
import json
from pathlib import Path
import re
import urllib.error
import urllib.parse
import urllib.request

U64 = (1 << 64) - 1
I64 = (1 << 63) - 1
MAX_INPUT = 8 * 1024 * 1024
MAX_LINE = 128 * 1024
TOKEN = re.compile(r'[A-Za-z0-9._:/@-]{1,63}\Z')
EVENTS = ('runtime.finalized', 'runtime.started', 'periodic.release', 'periodic.wake',
          'frame.begin', 'callback.begin', 'callback.end', 'watchdog.fired',
          'degradation.applied', 'frame.end', 'runtime.stopped', 'device.submitted',
          'device.completed', 'device.reset')
METRICS = ('runtime.frames_started', 'runtime.frames_completed', 'runtime.frames_failed',
           'runtime.callbacks_started', 'runtime.callbacks_completed', 'runtime.callback_failures',
           'runtime.deadline_misses', 'runtime.watchdog_events', 'runtime.degradation_events',
           'runtime.periodic_releases', 'runtime.periodic_wakes', 'trace.events_emitted',
           'trace.events_overwritten', 'trace.events_dropped', 'executor.submitted_tasks',
           'executor.local_executions', 'executor.steal_attempts', 'executor.successful_steals',
           'executor.queue_rejections', 'executor.scratch_exhaustions', 'executor.worker_starts',
           'runtime.degradation_level', 'device.submissions', 'device.completions', 'device.failures',
           'device.queue_rejections', 'device.timeouts', 'device.losses', 'device.resets',
           'device.service_polls', 'device.outstanding', 'device.service_starts')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def keys(value, names):
    require(type(value) is dict and set(value) == set(names.split()), 'unexpected/missing fields')


def integer(value, lower=0, upper=U64):
    require(type(value) is int and lower <= value <= upper, 'integer out of range')
    return value


def token(value):
    require(type(value) is str and TOKEN.fullmatch(value), 'invalid identity')


def timestamp(s, value):
    integer(value)
    return integer(s['unix_anchor_ns'] + value - s['runtime_anchor_ns'], 1)


def validate(s):
    keys(s, 'spool_schema session clock_domain runtime_anchor_ns unix_anchor_ns uncertainty_ns '
         'batch_sequence queue_full schema_version runtime_id config_id build_id workload_id '
         'runtime_version trace_capacity metrics trace')
    require(type(s['spool_schema']) is int and s['spool_schema'] == 1, 'spool schema')
    require(type(s['schema_version']) is int and s['schema_version'] == 2, 'Runtime schema')
    for name in ('session', 'clock_domain', 'build_id', 'workload_id'):
        token(s[name])
    for name in ('runtime_anchor_ns', 'uncertainty_ns', 'queue_full', 'config_id', 'trace_capacity'):
        integer(s[name])
    for name in ('unix_anchor_ns', 'runtime_id', 'batch_sequence'):
        integer(s[name], 1)
    version = s['runtime_version']
    require(type(version) is list and len(version) == 3, 'runtime version')
    for v in version:
        integer(v, 0, (1 << 32) - 1)
    m = s['metrics']
    keys(m, 'sequence start_ns end_ns window samples')
    integer(m['sequence'], 1)
    integer(m['start_ns']); integer(m['end_ns'])
    require(m['start_ns'] <= m['end_ns'] and m['window'] == 'interval', 'metric window')
    timestamp(s, m['start_ns']); timestamp(s, m['end_ns'])
    require(type(m['samples']) is list and len(m['samples']) == 32, 'metric count')
    for i, v in enumerate(m['samples']):
        keys(v, 'id name kind value')
        integer(v['id'], 0, 31); integer(v['kind'], 0, 1); integer(v['value'])
        require(v['id'] == i and v['name'] == METRICS[i] and v['kind'] == int(i in (21, 30)),
                'metric identity/kind/order')
    t = s['trace']
    keys(t, 'first_sequence next_sequence lost_events remaining_sequence_count events')
    for k in ('first_sequence', 'next_sequence', 'lost_events', 'remaining_sequence_count'):
        integer(t[k])
    require(t['first_sequence'] <= t['next_sequence'], 'trace range')
    require(type(t['events']) is list and len(t['events']) <= min(256, s['trace_capacity']), 'trace count')
    holes = t['next_sequence'] - t['first_sequence'] - len(t['events'])
    require(holes >= 0 and t['lost_events'] >= holes, 'unreported trace loss')
    if s['batch_sequence'] == 1:
        require(t['lost_events'] == holes, 'fresh cursor cannot report prehistory loss')
    previous = -1
    for e in t['events']:
        keys(e, 'type name status producer sequence timestamp_ns frame callback worker value')
        integer(e['type'], 1, 14); integer(e['status'], -23, 0); integer(e['producer'], 0, 2)
        for k in ('sequence', 'timestamp_ns', 'frame', 'value'):
            integer(e[k])
        for k in ('callback', 'worker'):
            integer(e[k], 0, (1 << 32) - 1)
        require(e['name'] == EVENTS[e['type'] - 1] and previous < e['sequence'] and
                t['first_sequence'] <= e['sequence'] < t['next_sequence'], 'trace identity/order')
        previous = e['sequence']
        timestamp(s, e['timestamp_ns'])
    return s


def pairs(items):
    result = {}
    for k, v in items:
        require(k not in result, 'duplicate JSON key')
        result[k] = v
    return result


def load_spool(stream):
    raw = stream.read(MAX_INPUT + 1)
    require(type(raw) is bytes and 0 < len(raw) <= MAX_INPUT and raw.endswith(b'\n'),
            'empty/oversize/truncated spool')
    lines = raw.splitlines()
    require(0 < len(lines) <= 64, 'batch count')
    snapshots, owners = [], {}
    for line in lines:
        require(0 < len(line) <= MAX_LINE, 'line size')
        s = validate(json.loads(line, object_pairs_hook=pairs,
                     parse_constant=lambda v: require(False, 'nonfinite JSON')))
        owner = (s['session'], s['runtime_id'])
        identity = tuple(str(s[k]) for k in ('clock_domain', 'runtime_anchor_ns', 'unix_anchor_ns',
                         'uncertainty_ns', 'config_id', 'build_id', 'workload_id', 'runtime_version', 'trace_capacity'))
        if owner in owners:
            old, old_identity = owners[owner]
            require(identity == old_identity, 'owner provenance changed')
            require(s['batch_sequence'] == old['batch_sequence'] + 1, 'duplicate/missing batch')
            require(s['metrics']['sequence'] > old['metrics']['sequence'] and
                    s['metrics']['start_ns'] == old['metrics']['end_ns'], 'metric discontinuity')
            require(s['trace']['next_sequence'] >= old['trace']['next_sequence'], 'trace regression')
            require(s['trace']['first_sequence'] >= old['trace']['next_sequence'], 'trace range overlap')
            consumed = s['trace']['next_sequence'] - old['trace']['next_sequence']
            require(s['trace']['lost_events'] == consumed - len(s['trace']['events']),
                    'trace records and reported losses do not conserve consumed sequences')
            require(not s['trace']['events'] or s['trace']['events'][0]['sequence'] >= old['trace']['next_sequence'],
                    'duplicate trace event')
            require(s['queue_full'] >= old['queue_full'], 'queue accounting regression')
        owners[owner] = (s, identity)
        snapshots.append(s)
    return snapshots


def attributes(s):
    # Exact decimal strings preserve unsigned schema values in signed OTLP /
    # Perfetto SQL stores, including config hashes above INT64_MAX.
    names = ('session', 'runtime_id', 'config_id', 'build_id', 'workload_id', 'clock_domain',
             'runtime_anchor_ns', 'unix_anchor_ns', 'uncertainty_ns', 'batch_sequence', 'queue_full')
    result = {'rtfw.' + k: str(s[k]) for k in names}
    result['rtfw.runtime_version'] = '.'.join(map(str, s['runtime_version']))
    result['rtfw.trace.lost_events'] = str(s['trace']['lost_events'])
    result['rtfw.trace.remaining_sequence_count'] = str(s['trace']['remaining_sequence_count'])
    result['rtfw.metrics.sequence'] = str(s['metrics']['sequence'])
    return result


def native_bounds(snapshots):
    # Native signed integer metrics and Trace Processor timestamps must be exact.
    for s in snapshots:
        validate(s)
        integer(timestamp(s, s['metrics']['start_ns']), 1, I64)
        integer(timestamp(s, s['metrics']['end_ns']), 1, I64)
        for e in s['trace']['events']:
            integer(timestamp(s, e['timestamp_ns']), 1, I64)
        for m in s['metrics']['samples']:
            integer(m['value'], 0, I64)


def perfetto(snapshots):
    from perfetto.protos.perfetto.trace import perfetto_trace_pb2 as pb
    native_bounds(snapshots)
    trace = pb.Trace()
    tracks = {}
    def packet(t):
        return trace.packet.add(timestamp=t, timestamp_clock_id=11, trusted_packet_sequence_id=1)
    anchor = timestamp(snapshots[0], snapshots[0]['metrics']['start_ns'])
    clock = packet(anchor).clock_snapshot
    clock.primary_trace_clock = 11  # explicit file timeline; no fabricated boot clock
    clock.clocks.add(clock_id=11, timestamp=anchor)
    clock.clocks.add(clock_id=1, timestamp=anchor)
    def track(s, metric=None):
        key = (s['session'], s['runtime_id'], metric)
        if key not in tracks:
            uid = len(tracks) + 1  # collision-free within this bounded trace file
            tracks[key] = uid
            d = packet(anchor).track_descriptor
            d.uuid = uid
            d.name = s['session'] + '/' + str(s['runtime_id']) + ('/' + metric if metric else '/events')
            if metric:
                d.counter.unit_name = 'count'
        return tracks[key]
    def annotate(event, values):
        for k, v in values.items():
            event.debug_annotations.add(name=k, string_value=str(v))
    for s in snapshots:
        common = attributes(s)
        uid = track(s)
        summary = packet(timestamp(s, s['metrics']['end_ns'])).track_event
        summary.type = 3; summary.track_uuid = uid; summary.name = 'rtfw.snapshot'
        annotate(summary, common)
        for e in s['trace']['events']:
            event = packet(timestamp(s, e['timestamp_ns'])).track_event
            event.type = 3; event.track_uuid = uid; event.name = e['name']
            annotate(event, common)
            annotate(event, {'rtfw.event.' + k: v for k, v in e.items()})
        for m in s['metrics']['samples']:
            uid = track(s, m['name'])
            event = packet(timestamp(s, s['metrics']['end_ns'])).track_event
            event.type = 4; event.track_uuid = uid; event.counter_value = m['value']
            annotate(event, common)
            annotate(event, {'rtfw.metric.kind': m['kind'], 'rtfw.metric.window': 'interval',
                            'rtfw.metric.value': m['value']})
    return trace.SerializeToString()


def otlp(snapshots):
    from opentelemetry.proto.collector.logs.v1.logs_service_pb2 import ExportLogsServiceRequest
    from opentelemetry.proto.collector.metrics.v1.metrics_service_pb2 import ExportMetricsServiceRequest
    native_bounds(snapshots)
    logs, metrics = ExportLogsServiceRequest(), ExportMetricsServiceRequest()
    def attrs(target, values):
        for k, v in values.items():
            target.add(key=k).value.string_value = str(v)
    for s in snapshots:
        common = attributes(s)
        lr, mr = logs.resource_logs.add(), metrics.resource_metrics.add()
        resource = {'service.name': 'rtfw', 'service.instance.id': s['session'] + '/' + str(s['runtime_id'])}
        # Batch-specific metadata belongs on records, not on metric resource identity.
        for target in (lr, mr):
            attrs(target.resource.attributes, resource)
        sl, sm = lr.scope_logs.add(), mr.scope_metrics.add()
        sl.scope.name = sm.scope.name = 'rtfw.telemetry.schema2'
        for e in [None, *s['trace']['events']]:
            t = s['metrics']['end_ns'] if e is None else e['timestamp_ns']
            record = sl.log_records.add(time_unix_nano=timestamp(s, t), severity_number=9,
                                       severity_text='INFO')
            record.body.string_value = 'rtfw.snapshot' if e is None else e['name']
            attrs(record.attributes, common)
            if e is not None:
                attrs(record.attributes, {'rtfw.event.' + k: v for k, v in e.items()})
        for m in s['metrics']['samples']:
            metric = sm.metrics.add(name=m['name'], unit='1')
            if m['kind'] == 0:
                metric.sum.aggregation_temporality = 1  # DELTA, matching Runtime interval cursor
                metric.sum.is_monotonic = True
                point = metric.sum.data_points.add()
            else:
                point = metric.gauge.data_points.add()
            point.start_time_unix_nano = timestamp(s, s['metrics']['start_ns'])
            point.time_unix_nano = timestamp(s, s['metrics']['end_ns'])
            point.as_int = m['value']
            # Stable attributes prevent a new time series for every snapshot.
            attrs(point.attributes, {'rtfw.clock_domain': s['clock_domain'],
                                    'rtfw.config_id': s['config_id'], 'rtfw.build_id': s['build_id'],
                                    'rtfw.workload_id': s['workload_id']})
    return {'logs': logs.SerializeToString(), 'metrics': metrics.SerializeToString()}


class TransportError(RuntimeError):
    """Retain the spool. An ambiguous failure may already have been ingested."""


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def send(endpoint, signal, payload, timeout=10):
    require(signal in ('logs', 'metrics') and type(payload) is bytes and len(payload) <= 16 * 1024 * 1024,
            'signal/payload limit')
    require(type(timeout) in (int, float) and 0 < timeout <= 60, 'timeout range')
    url = urllib.parse.urlsplit(endpoint)
    require(url.scheme in ('http', 'https') and url.hostname and not url.username and not url.password
            and not url.query and not url.fragment, 'endpoint must be an HTTP(S) base URL')
    request = urllib.request.Request(endpoint.rstrip('/') + '/v1/' + signal, payload,
                                    {'Content-Type': 'application/x-protobuf', 'Accept-Encoding': 'identity'})
    try:
        with urllib.request.build_opener(NoRedirect()).open(request, timeout=timeout) as response:
            body = response.read(65537)
            if response.status != 200 or len(body) > 65536 or response.headers.get_content_type() != 'application/x-protobuf':
                raise TransportError('invalid OTLP response; ingestion may be ambiguous')
            if response.headers.get('Content-Encoding', 'identity') != 'identity':
                raise TransportError('unsupported response encoding; ingestion may be ambiguous')
    except (OSError, urllib.error.URLError) as e:
        raise TransportError('OTLP transport failure; ingestion may be ambiguous; no retry') from e
    if signal == 'logs':
        from opentelemetry.proto.collector.logs.v1.logs_service_pb2 import ExportLogsServiceResponse as Response
        field = 'rejected_log_records'
    else:
        from opentelemetry.proto.collector.metrics.v1.metrics_service_pb2 import ExportMetricsServiceResponse as Response
        field = 'rejected_data_points'
    try:
        result = Response.FromString(body)
    except Exception as e:
        raise TransportError('malformed OTLP response; ingestion may be ambiguous') from e
    partial = result.partial_success
    rejected = getattr(partial, field)
    if rejected or partial.error_message:
        raise TransportError(f'OTLP partial success: rejected={rejected}; {partial.error_message}; no retry')
    return {'signal': signal, 'receiver_accepted': True, 'downstream_delivery': 'unacknowledged'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('spool', type=Path)
    p.add_argument('--perfetto', type=Path)
    p.add_argument('--otlp-directory', type=Path)
    p.add_argument('--endpoint', help='explicit HTTP(S) base URL; sends logs then metrics once')
    p.add_argument('--timeout', type=float, default=10)
    a = p.parse_args()
    require(a.perfetto or a.otlp_directory or a.endpoint, 'select at least one native exporter')
    with a.spool.open('rb') as f:
        snapshots = load_spool(f)
    # Complete validation/encoding before the first external side effect.
    trace = perfetto(snapshots) if a.perfetto else None
    payloads = otlp(snapshots) if a.otlp_directory or a.endpoint else None
    if trace is not None:
        a.perfetto.write_bytes(trace)
    if a.otlp_directory:
        a.otlp_directory.mkdir(parents=True, exist_ok=True)
        for signal, data in payloads.items():
            (a.otlp_directory / (signal + '.pb')).write_bytes(data)
    if a.endpoint:
        for signal, data in payloads.items():
            print(json.dumps(send(a.endpoint, signal, data, a.timeout)), flush=True)


if __name__ == '__main__':
    main()
