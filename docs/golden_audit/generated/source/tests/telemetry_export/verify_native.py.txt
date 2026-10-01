#!/usr/bin/env python3
"""Real Perfetto Trace Processor and OpenTelemetry Collector acceptance gate."""
import argparse
import csv
import importlib.util
import io
import json
from pathlib import Path
import signal
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def require(value, message):
    if not value: raise RuntimeError(message)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kit', type=Path, required=True)
    p.add_argument('--example', type=Path, required=True)
    p.add_argument('--trace-processor', type=Path, required=True)
    p.add_argument('--collector', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args(); a.output.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('export', a.kit / 'export.py')
    e = importlib.util.module_from_spec(spec); spec.loader.exec_module(e)
    raw = subprocess.check_output([str(a.example.resolve())], timeout=30)
    (a.output / 'snapshots.ndjson').write_bytes(raw)
    rows = e.load_spool(io.BytesIO(raw))
    events = sum(len(s['trace']['events']) for s in rows)
    trace = e.perfetto(rows); payload = e.otlp(rows)
    path = a.output / 'native.pftrace'; path.write_bytes(trace)
    for name, data in payload.items(): (a.output / (name + '.pb')).write_bytes(data)
    from perfetto.protos.perfetto.trace import perfetto_trace_pb2
    from opentelemetry.proto.collector.logs.v1.logs_service_pb2 import ExportLogsServiceRequest, ExportLogsServiceResponse
    from opentelemetry.proto.collector.metrics.v1.metrics_service_pb2 import ExportMetricsServiceRequest
    from google.protobuf.json_format import ParseDict
    decoded = perfetto_trace_pb2.Trace.FromString(trace)
    require(len(decoded.packet) > events, 'official Perfetto protobuf decoder')
    logs = ExportLogsServiceRequest.FromString(payload['logs'])
    metrics = ExportMetricsServiceRequest.FromString(payload['metrics'])
    require(sum(len(scope.log_records) for r in logs.resource_logs for scope in r.scope_logs) == events + len(rows),
            'official OTLP log decoder')
    points = [m for r in metrics.resource_metrics for scope in r.scope_metrics for m in scope.metrics]
    require(len(points) == 32 * len(rows), 'official OTLP metric decoder')
    require(sum(m.sum.data_points[0].as_int for m in points if m.name == 'runtime.frames_started') == 6,
            'delta counter conservation')
    require(all(m.sum.aggregation_temporality == 1 and m.sum.is_monotonic for m in points if m.HasField('sum')),
            'native delta metric semantics')
    def query(label, sql):
        result = subprocess.run([str(a.trace_processor.resolve()), str(path), '-Q', sql], text=True,
                                capture_output=True, timeout=30)
        (a.output / (label + '.stdout')).write_text(result.stdout)
        (a.output / (label + '.stderr')).write_text(result.stderr)
        require(result.returncode == 0, 'Trace Processor failed: ' + label)
        return list(csv.DictReader(io.StringIO(result.stdout)))
    q = query('native-events', 'select count(*) as n from slice')
    require(int(q[0]['n']) == events + len(rows), 'actual native trace event count')
    q = query('native-metrics', 'select count(*) as n from counter')
    require(int(q[0]['n']) == 32 * len(rows), 'actual native metric count')
    q = query('native-errors', "select name,value from stats where severity='error' and value>0")
    require(not q, 'native trace import errors')
    q = query('native-identities', "select distinct string_value from args where key='debug.rtfw_session' order by string_value")
    require([v['string_value'] for v in q] == ['example-owner-a', 'example-owner-b'], 'native owner identity')
    expected = sorted(e.timestamp(s, v['timestamp_ns']) for s in rows for v in s['trace']['events'])
    q = query('native-timestamps', "select ts from slice where name!='rtfw.snapshot' order by ts")
    require([int(v['ts']) for v in q] == expected, 'native exact mapped timestamp')
    q = query('native-configs', "select distinct string_value from args where key='debug.rtfw_config_id'")
    require({v['string_value'] for v in q} == {str(s['config_id']) for s in rows}, 'exact unsigned config identity')

    # Real collector, explicit loopback receiver, native OTLP file exporter.
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
    collected = a.output.resolve() / 'collector.json'
    config = a.output / 'collector.yaml'
    config.write_text('''receivers:
  otlp:
    protocols:
      http:
        endpoint: 127.0.0.1:PORT
exporters:
  file:
    path: FILE
    flush_interval: 100ms
service:
  telemetry:
    metrics:
      level: none
      readers: []
  pipelines:
    logs:
      receivers: [otlp]
      exporters: [file]
    metrics:
      receivers: [otlp]
      exporters: [file]
'''.replace('PORT', str(port)).replace('FILE', json.dumps(str(collected))))
    with (a.output / 'collector.log').open('w') as log:
        process = subprocess.Popen([str(a.collector.resolve()), '--config=' + str(config.resolve())], stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 20
            while True:
                require(process.poll() is None, 'collector exited during startup')
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=.2): break
                except OSError:
                    require(time.monotonic() < deadline, 'collector startup timeout'); time.sleep(.05)
            receipts = [e.send('http://127.0.0.1:' + str(port), name, data) for name, data in payload.items()]
            (a.output / 'receipts.json').write_text(json.dumps(receipts, indent=2))
        finally:
            process.send_signal(signal.SIGINT)
            try: code = process.wait(timeout=10)
            except subprocess.TimeoutExpired: process.kill(); process.wait(); raise
    require(code == 0, 'collector did not shut down cleanly')
    received_logs, received_metrics = [], []
    for line in collected.read_text().splitlines():
        value = json.loads(line)
        if 'resourceLogs' in value:
            request = ParseDict(value, ExportLogsServiceRequest())
            received_logs += [record for r in request.resource_logs for scope in r.scope_logs for record in scope.log_records]
        elif 'resourceMetrics' in value:
            request = ParseDict(value, ExportMetricsServiceRequest())
            received_metrics += [metric for r in request.resource_metrics for scope in r.scope_metrics for metric in scope.metrics]
        else: raise RuntimeError('unexpected native collector record')
    expected_logs = [record for r in logs.resource_logs for scope in r.scope_logs for record in scope.log_records]
    require(sorted(r.SerializeToString(deterministic=True) for r in received_logs) ==
            sorted(r.SerializeToString(deterministic=True) for r in expected_logs), 'collected exact log values/identity')
    require(sorted(r.SerializeToString(deterministic=True) for r in received_metrics) ==
            sorted(r.SerializeToString(deterministic=True) for r in points), 'collected exact metric values/temporality')

    # Deterministic transport negatives, separate from actual native collection.
    response = ExportLogsServiceResponse(); response.partial_success.rejected_log_records = 1
    cases = [(200, 'application/x-protobuf', response.SerializeToString()),
             (200, 'application/x-protobuf', b'\xff'), (200, 'text/plain', b''),
             (503, 'application/x-protobuf', b''), (302, 'application/x-protobuf', b''),
             (200, 'application/x-protobuf', b'x' * 65537),
             (-1, 'application/x-protobuf', b''), (-2, 'application/x-protobuf', b'')]
    calls = []
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def do_POST(self):
            body = self.rfile.read(int(self.headers['Content-Length']))
            calls.append((self.path, body))
            status, content, data = self.server.reply
            if status == -1:
                self.connection.close(); return
            if status == -2:
                time.sleep(.2); status = 200
            self.send_response(status); self.send_header('Content-Type', content)
            self.send_header('Location', '/redirect'); self.end_headers()
            try: self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError): pass
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever); thread.start()
    try:
        for reply in cases:
            server.reply = reply
            try: e.send('http://127.0.0.1:' + str(server.server_port), 'logs', payload['logs'], timeout=.05 if reply[0] == -2 else 10)
            except e.TransportError: pass
            else: raise RuntimeError('transport failure incorrectly acknowledged')
        require(len(calls) == len(cases) and all(path == '/v1/logs' for path, _ in calls), 'no retry/redirect')
    finally:
        server.shutdown(); thread.join(); server.server_close()
    # The old JSON/spool is not native protobuf; independent official decoders
    # must reject it rather than allowing a relabelled export to pass.
    from google.protobuf.message import DecodeError
    for message in (perfetto_trace_pb2.Trace, ExportLogsServiceRequest, ExportMetricsServiceRequest):
        try: message.FromString(raw)
        except DecodeError: pass
        else: raise RuntimeError('JSON accepted as native protobuf')
    result = dict(perfetto_events=events + len(rows), perfetto_metrics=len(points), native_clock_errors=0,
                  collector_logs=len(received_logs), collector_metrics=len(received_metrics),
                  transport_negatives=len(cases), windows_etw='separate real Windows gate')
    (a.output / 'result.json').write_text(json.dumps(result, indent=2))
    print('PASS real Trace Processor, official protobuf decoders, actual OTLP Collector, transport negatives', result)


if __name__ == '__main__': main()
