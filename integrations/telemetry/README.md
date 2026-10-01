# Optional non-RT telemetry source kit

This kit captures the existing Runtime global observability schema 2 and produces
native Perfetto TrackEvent protobuf, OpenTelemetry OTLP/HTTP protobuf logs and
metrics, and Windows TraceLogging ETW events. It is installed as source under
`${RTFW_DATA_DIR}/integrations/telemetry`. It adds no default SDK header, exported
target, Runtime thread, Runtime policy, public ABI or schema change.

## Build and use

```sh
cmake -S /path/to/sdk/share/rtfw/integrations/telemetry -B telemetry-build \
  -DCMAKE_PREFIX_PATH=/path/to/sdk
cmake --build telemetry-build --config Release
./telemetry-build/rtfw_telemetry_example > snapshots.ndjson
python3 -m venv telemetry-env
telemetry-env/bin/pip install -r /path/to/kit/requirements.txt
telemetry-env/bin/python /path/to/kit/export.py snapshots.ndjson \
  --perfetto runtime.pftrace --otlp-directory native-otlp
telemetry-env/bin/python /path/to/kit/export.py snapshots.ndjson \
  --endpoint http://127.0.0.1:4318
```

On Windows use `telemetry-build/Release/rtfw_telemetry_example.exe` and the venv's
`Scripts/python.exe`. Python dependencies are optional and are not installed by
CMake or linked into Runtime. The example uses two actual Runtime instances with
**synthetic** clocks and an explicitly synthetic Unix anchor. Replace those
anchors with a measured/certified mapping for your Runtime clock before using
real timestamps. The NDJSON spool is an intermediate kit format, not Perfetto,
OTLP or a replacement for the existing Runtime JSON API.

Applications may add this source directory with `add_subdirectory` after finding
`rtfw::runtime`, and link the local `rtfw_telemetry_kit` target. Configure/start/
step/stop and `Queue::capture` must use the Runtime's single host owner at a
quiescent boundary. Never capture from a Runtime callback, worker, device lane,
or concurrently with Runtime control or execution. The queue mutex serializes
only kit operations; it cannot make concurrent Runtime inspection safe. A host
consumer thread may call `drain_one` while that owner continues execution.
The kit creates no threads. Capture, validation, encoding, stream writes and
network/ETW calls belong on non-RT host lanes and may allocate or block.

## Bounds, identity, clocks and loss

A queue accepts 1–64 batches, each with at most 256 copied trace events and all
32 fixed metrics. Partial trace reads retain the cursor for the next capture.
No callback data, addresses or payload handles are exported. Session and clock
labels are 1–63 token characters. Session labels must be unique for each process
incarnation/capture epoch; Runtime IDs alone are only process-local. Use a new
queue and session label when resetting a Runtime or changing clock mapping.
One queue is bound to one Runtime after its first successful capture.

`Context` explicitly states Runtime and Unix nanosecond anchors, clock domain,
and uncertainty. Export uses `unix_anchor + timestamp - runtime_anchor`, with
checked overflow/underflow. This is an offset-only mapping, not a drift estimator
or synchronization guarantee. The caller supplies meaningful uncertainty and a
mapping valid for the whole captured epoch. Original timestamps and anchors are
retained in native records. Invalid or unrepresentable timestamps reject export.

Full, disabled, closed and failed captures never advance the queue's cursors.
`full` counts rejected capture calls, **not lost Runtime events**. Established
Runtime trace cursors separately report skipped/overwritten sequence positions;
a fresh cursor starts at the oldest retained event and cannot establish earlier
history loss. Interval counters cover time since the previous accepted capture;
gauges are sampled at the interval end. Capture does not backpressure Runtime
producers. Failed inspections can increment Runtime snapshot sequence numbers;
queue batch sequence advances only on acceptance.

A sink acknowledges a whole snapshot. A false return or exception retains it.
A partial external write may already be visible; retrying may duplicate records.
Deduplicate by session, Runtime ID, batch sequence and event sequence. Sinks run
under the queue lock and must not reenter the queue. `close()` rejects new
captures and retains pending batches for drain; `close(true)` discards them and
counts discarded batches and events. Closing again is idempotent. Read and retain
`statistics()` after shutdown, since rejections after the final snapshot and
explicit discards cannot be reported by an earlier snapshot. Counters saturate
at UINT64_MAX; acceptance stops before batch-sequence overflow. Destruction does
not implicitly flush, persist or report anything; explicitly close/drain first.

The spool parser bounds input to 8 MiB, 64 batches and 128 KiB per line. It rejects
unknown/duplicate fields, unsupported schemas, malformed records, nonfinite or
out-of-range values, changed owner provenance, duplicate/missing interior
batches, overlapping trace sequences, unaccounted sequence holes, inconsistent loss totals,
changed trace capacity and discontinuous metric intervals. A file
may begin at any batch sequence; absent prior history is not inferred. Each file
is a bounded export unit; cross-file deduplication/continuity is the consumer's
responsibility. The caller retains the spool; exports never remove it.

## Native semantics

**Perfetto:** `export.py` generates official `Trace`/`TrackEvent` protobuf. Every
Runtime event is an instant with its original type/name, status, producer,
sequence, frame, callback, worker and value; it does not invent matched slices
across missing events. There is a snapshot accounting instant and 32 metric
counter tracks per owner. Counter tracks display interval counter values or
sampled gauge values, not newly accumulated totals. An explicit file-clock
snapshot relates that timeline to supplied Unix time; no boot/QPC time is
fabricated. UUIDs are collision-free within a file. Unsigned provenance values
are decimal-string annotations, since native SQL stores use signed integers.
Trace Processor normalizes annotation dots to underscores (for example
`debug.rtfw_session`). Counter visualization may use floating point; the exact
integer also remains in `rtfw.metric.value`. The encoder rejects metric values
or mapped timestamps above INT64_MAX instead of silently converting to float.
The spool and ETW keep the full Runtime uint64 range.

```sh
trace_processor_shell runtime.pftrace -Q 'select ts,name from slice'
trace_processor_shell runtime.pftrace -Q 'select * from counter limit 10'
```

**OpenTelemetry:** actual `ExportLogsServiceRequest` and
`ExportMetricsServiceRequest` protobuf are sent to `/v1/logs` and `/v1/metrics`
with `application/x-protobuf`. Events and snapshot/loss summaries are logs.
Counters are monotonic DELTA sums with exact interval start/end; gauges remain
gauges. Stable session/Runtime resource identity and metric attributes avoid
creating a new series per batch. Original unsigned identity/event fields use
exact decimal-string log attributes; metric points use signed int64, with
out-of-range values rejected before either request. Logs and metrics are two
separate requests: a later failure cannot roll back earlier acceptance.

Only explicit HTTP(S) base URLs are accepted. HTTPS uses the Python platform
trust store. Redirects and automatic retries are disabled. Requests use a bounded
positive timeout of at most60 seconds and responses are limited to 64 KiB. Non-200, malformed or
wrong-content-type responses, transport failure and partial rejection are errors.
An error may be ambiguous after receiver ingestion. Keep the spool and assess
receipts before retrying; no exactly-once or downstream-delivery claim follows
from a receiver ACK. This small exporter is not a complete OpenTelemetry SDK and
does not implement SDK environment-variable configuration, authentication policy,
batching/retries or a production collector service.

**Windows ETW:** construct `Etw` on a host lane and drain with
`queue.drain_one([&](const Snapshot& s) { return exporter.write(s); })`.
Provider `RTFW.Telemetry` has GUID
`7744f8d4-bb73-4f12-9be6-49e159811370`, level4 and keyword1. Registration lifetime
is shared under a mutex across kit instances within the linked module; the final
instance unregisters. An initially disabled provider returns false and retains
the batch. Provider enablement can change during emission; `TraceLoggingWrite`
has no delivery ACK. A true result means attempted emission only. ETW session
loss counts and decoded sequences must establish actual delivery. Each bounded
batch emits a `Snapshot`, `RuntimeEvent` records and `RuntimeMetric` records with
native typed fields. Join them by Session/RuntimeId/BatchSequence. ETW event-header
QPC timestamps are host emission times; payload RuntimeTimestampNs and
UnixTimestampNs describe the original Runtime event. Do not conflate them.

The real Windows test starts a private in-process ETL session, enables this
provider, emits from two owners, unregisters one exporter, and uses
`OpenTrace`/`ProcessTrace`/TDH to decode the surviving export. It checks exact
record counts, identity, timestamps and zero collector loss. It is never replaced
by a Linux stub or a skipped test. ETW requires a Windows SDK and runner.

## Validation and limits

Always-on CTests cover the C++ queue and standard-library spool parser. The
separate `tests/telemetry_export/verify_native.py` gate requires the optional
Python environment, actual Trace Processor and actual OpenTelemetry Collector.
It checks native SQL, clock health, official protobuf decoding, real HTTP
collection/file decoding and transport rejection. Pinned verification tools are
Perfetto v58.2 and OpenTelemetry Collector v0.162.0; dependency versions are in
`requirements.txt`. Hashes and run results belong in M27-04 evidence.

This kit exports global observability schema2 only. Separate rate-action,
mixed-rate, live-control and sampled-I/O formats are not silently combined with
it. It does not add replay, physical/HIL/RT qualification, controlled-performance
acceptance, Unreal validation, production signing or release approval.

Specifications: [Perfetto native generation](https://perfetto.dev/docs/reference/synthetic-track-event),
[Perfetto file clocks](https://github.com/google/perfetto/blob/v58.2/protos/perfetto/common/builtin_clock.proto),
[OTLP](https://opentelemetry.io/docs/specs/otlp/),
[TraceLogging](https://learn.microsoft.com/en-us/windows/win32/api/traceloggingprovider/nf-traceloggingprovider-traceloggingwrite),
[private ETW sessions](https://learn.microsoft.com/en-us/windows/win32/etw/configuring-and-starting-a-private-logger-session).

The Windows relocated-package job also compiles this installed source kit and
runs the same actual ETL/TDH collection test. This is an explicit optional consumer
target, not a new default SDK export.
