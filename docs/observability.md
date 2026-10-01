# Versioned Observability Contract

Release 1.2 retains the M6 RT0 observability surface for `rt::Runtime` with M8
device records.
Emission is bounded and allocation-free on runtime lanes; inspection and
serialization are explicit non-RT host operations. This is a telemetry
contract, not a latency qualification or a native OpenTelemetry, ETW, or eBPF
integration.

The legacy `SimCore` binary trace and mutex-backed metrics registry remain
experimental and do not inherit this contract.

M16-04 adds a deliberately separate C++ rate-action telemetry schema version
1. Its fixed 160-byte records, 20 counters/gauges, metadata, and runtime-bound
cursors are specified in the [rate-action telemetry contract](rate_telemetry.md).
They do not append global schema-2 event or metric IDs, are not exported by the
C ABI, and are not checkpoint or replay history.

M22-03 leaves global schema 2, every trace/metric ID, rate-action schema 1, and
mixed-rate-action schema 1 unchanged. It adds a distinct C++ live-control
action schema 1: fixed 256-byte payload-free records, one finalized direct-
index ring, exact loss/overwrite counters, and runtime-bound gap-reporting
cursors. Actions carry only fixed identities, targets, digests, counts, and
closed outcomes. Payload bytes appear only in an explicit caller-requested
replay artifact and never in global trace, metrics, JSON, or background
transport.

M21-05 likewise leaves global schema 2 and every existing trace/metric ID
unchanged. Its additive C++ mixed-rate-action schema is a separate fixed ring
with its own runtime-bound cursors and loss counters. It carries content
identities and terminal logical results, never sampled payload bytes, raw
addresses, vendor handles, callback pointers, or thread identity. A complete
gap-free range may be embedded in the distinct active-replay artifact; global
trace and metric history is never treated as replay input.

## Schema and provenance

Observability schema version 2 has fixed numeric trace-event and metric IDs.
It preserves every schema-v1 ID and appends device definitions.
The version/build/config/workload identifiers make each exported stream
self-describing and correlatable.
Every metadata record, metric snapshot, and trace read reports:

- observability schema version;
- runtime semantic version;
- fixed trace-record and metric-sample sizes;
- metric count and trace capacity;
- build identifier;
- canonical configuration identifier;
- workload identifier;
- process-local runtime identifier.

The build identifier defaults to `rtfw-<version>` and can be set at build time
with `-DRTFW_BUILD_ID=<token>`. Build and workload identifiers are restricted
to 1–63 characters from `A-Za-z0-9._:/@-`; the runtime rejects malformed or
unterminated workload IDs.

`config_id` is a 64-bit FNV-1a fingerprint of configuration schema version 7,
every behavioral `RuntimeConfig` field, and `workload_id`. The identifier is
useful for correlation and exact configuration equality checks; it is not a
cryptographic digest. M7's separate `replay_id` applies its documented D0/D1
compatibility policy instead of reusing `config_id`.

## Trace emission

`trace_capacity` commits a global fixed slot count at finalization. Each
attempted event receives a monotonically increasing sequence number. Producers
make one nonblocking slot-claim attempt:

- a free slot is populated and atomically committed;
- reuse of a committed slot increments `trace.events_overwritten`;
- contention or zero configured capacity drops the new event and increments
  `trace.events_dropped`;
- producers never wait, allocate, invoke host code, or fall back to another
  queue.

Payload fields are atomic so a reader cannot observe a C++ data race. Public
records are fixed at 64 bytes and include schema/size, sequence, event type,
status, runtime timestamp, frame, producer, callback/worker indices, and an
event-specific value. Callback events carry registration index, worker index,
and task index. Periodic wake events carry the corresponding absolute release;
watchdog and degradation records carry their configured timeout or applied
level.

| ID | Event name |
| ---: | --- |
| 1 | `runtime.finalized` |
| 2 | `runtime.started` |
| 3 | `periodic.release` |
| 4 | `periodic.wake` |
| 5 | `frame.begin` |
| 6 | `callback.begin` |
| 7 | `callback.end` |
| 8 | `watchdog.fired` |
| 9 | `degradation.applied` |
| 10 | `frame.end` |
| 11 | `runtime.stopped` |
| 12 | `device.submitted` |
| 13 | `device.completed` |
| 14 | `device.reset` |

Device events carry the graph phase index in `callback_index`, the submission
ID in `value`, and the originating frame. Reset has no callback index and
carries the backend index in `value`. Submission records identify their CPU
worker, completions use producer `device_service`, and reset uses `host`.

M17-01 preserves these exact meanings for native HAL v2 and adapted
device-ABI-v1 backends. Both registration kinds traverse the same manager event
path; adapter entry, translation, or backend kind does not add an event, metric,
producer, or schema field. An early completion is published only after its
accepted `device.submitted` record, and malformed output publishes neither a
completion event nor a successful reset/health result.

M17-02 adds no global event, metric, producer, or schema field. Memory-domain,
topology, timestamp-domain, completion-domain, and memory-object facts are
available through bounded C++ Runtime inspectors. Timestamp correlations are
explicit running-state control results; they are not trace timestamps, are not
emitted automatically, and are not retained in checkpoint, replay, or rate
telemetry. Synthetic device timestamps do not establish correlation accuracy
or physical clock behavior.

Trace slots are cache-line aligned. The implementation fails compilation on a
target whose 16-, 32-, or 64-bit standard atomics are not always lock-free,
rather than silently routing an RT-lane operation through a library lock.

`RuntimeTraceCursor` and `rtfw_trace_cursor` are caller-owned. A fresh cursor
must be default initialized (or initialized by the matching C function),
starts at the oldest event currently retained, and does not call earlier
history “lost.” If an established cursor falls behind, the next read advances
to the oldest retained sequence and reports the exact skipped sequence count.
Dropped sequence holes encountered inside a batch are also included in
`lost_events`. Cursors are bound to one runtime and are rejected by another.

The older `trace_event_count()` and `trace_event()` methods remain compatibility
views over the latest retained window. Cursor reads are the production
interface.

## Metric schema

Schema version 2 publishes these ordered samples:

| ID | Name | Kind |
| ---: | --- | --- |
| 0 | `runtime.frames_started` | Counter |
| 1 | `runtime.frames_completed` | Counter |
| 2 | `runtime.frames_failed` | Counter |
| 3 | `runtime.callbacks_started` | Counter |
| 4 | `runtime.callbacks_completed` | Counter |
| 5 | `runtime.callback_failures` | Counter |
| 6 | `runtime.deadline_misses` | Counter |
| 7 | `runtime.watchdog_events` | Counter |
| 8 | `runtime.degradation_events` | Counter |
| 9 | `runtime.periodic_releases` | Counter |
| 10 | `runtime.periodic_wakes` | Counter |
| 11 | `trace.events_emitted` | Counter |
| 12 | `trace.events_overwritten` | Counter |
| 13 | `trace.events_dropped` | Counter |
| 14 | `executor.submitted_tasks` | Counter |
| 15 | `executor.local_executions` | Counter |
| 16 | `executor.steal_attempts` | Counter |
| 17 | `executor.successful_steals` | Counter |
| 18 | `executor.queue_rejections` | Counter |
| 19 | `executor.scratch_exhaustions` | Counter |
| 20 | `executor.worker_starts` | Counter |
| 21 | `runtime.degradation_level` | Gauge |
| 22 | `device.submissions` | Counter |
| 23 | `device.completions` | Counter |
| 24 | `device.failures` | Counter |
| 25 | `device.queue_rejections` | Counter |
| 26 | `device.timeouts` | Counter |
| 27 | `device.losses` | Counter |
| 28 | `device.resets` | Counter |
| 29 | `device.service_polls` | Counter |
| 30 | `device.outstanding` | Gauge |
| 31 | `device.service_starts` | Counter |

Counter values are monotonic from finalization. A completed frame includes a
failed frame; `frames_failed` identifies the failed subset. Callback and
executor counts describe accepted/executed runtime work, not application-level
entities.

## Window semantics

`cumulative` snapshots report current values since finalization and do not
require or mutate a cursor.

`interval` snapshots require a caller-owned metric cursor:

1. A default-initialized fresh cursor covers finalization through the first
   snapshot.
2. Each later snapshot starts at the previous successful window end.
3. Counters are current minus that cursor's prior values.
4. Gauges are sampled at the window end and are never differenced.
5. A failed snapshot does not advance the cursor.

Therefore, adjacent intervals from one cursor partition each cumulative
counter. Multiple exporters do not reset global state or affect each other.
Metric cursors are runtime-bound and cross-instance use is rejected.

## Host export APIs

C++:

- `Runtime::observability_metadata()`;
- `Runtime::metrics_snapshot()`;
- `Runtime::read_trace()`;
- `write_observability_json()` in
  `<rt/observability_export.hpp>`.

Stable C ABI v8:

- `rtfw_get_observability_metadata()`;
- `rtfw_get_metrics()`;
- `rtfw_read_trace()`;
- matching cursor/result initialization functions.

The JSON helper gathers a complete bounded snapshot before writing, may
allocate staging memory, and emits schema/version/build/config/workload/runtime
identifiers, fixed record/sample sizes and capacities, metric definitions and
values, and retained trace records. Interval JSON export requires a persistent
metric cursor. It is not an RT callback facility.

All inspection/export calls reject an active step or periodic loop. Control
operations and export remain single-host-thread operations. The runtime does
not create a background exporter, perform file or network I/O, or invoke a
host sink.

## Evidence

- fixed trace schema, loss accounting, metric windows, provenance, runtime
  isolation, JSON, and contended nonblocking emission:
  `tests/test_observability.cpp`;
- C ABI v8 symbol, structure, cursor, metric, trace, and device coverage:
  `tests/test_cabi_dlopen.c`;
- complete post-start CPU and mock-device frames with tracing and counters
  under allocation instrumentation: `tests/test_trace_noalloc.cpp`;
- device event ordering and counters: `tests/test_device_runtime.cpp`;
- native-v2/adapted-v1 successful event and metric equivalence:
  `tests/test_hal_v2.cpp`;
- ThreadSanitizer coverage includes `Observability.*`;
- implementation: `rt/src/telemetry.cpp`,
  `rt/src/host_runtime.cpp`, and
  `rt/src/observability_export.cpp`.
- separate rate-action implementation and tests: `rt/src/rate_telemetry.cpp`,
  `tests/test_rate_telemetry.cpp`.

## Explicit boundaries

- Schema version 2 applies only to the target `rt::Runtime` path.
- The legacy per-thread binary trace, rolling histograms, and demo JSON retain
  their existing experimental semantics.
- A trace capacity can overwrite unread events; the cursor reports loss rather
  than applying backpressure to runtime lanes.
- Export can allocate, block in the destination stream, or fail with
  `resource_exhausted`; it belongs on a non-RT host lane.
- Observability correctness does not establish RT1/RT2 timing behavior.

## M17-03 observability boundary

M17-03 adds no global event, metric, producer, cursor, or schema field. Batch
outcomes reuse existing device categories, and submission-lane policy/readback
uses the M15 report. Timeline inspection is a control query, not a trace event.
Batch IDs, device timestamps, and mutable progress are excluded from
compatibility identity. Global observability schema 2 and every ID remain
unchanged.

## M17-04 observability boundary

CUDA Graph launch and XDMA transfer/control/event outcomes reuse the existing
device completion and health surfaces. M17-04 adds no event, metric, producer,
cursor, schema field, or ID. Graph identifiers, control offsets, and event
indexes participate in compatibility through command declarations, while
native handles, observed values, timestamps, counters, and health do not.
Portable fake-driver results do not establish physical Graph, MMIO, interrupt,
latency, HIL, RT1, or RT2 behavior.

## M17-06 trace and claim boundary

The combined test consumes existing device submitted/completed trace records
to prove accepted-before-complete causality and expected submission/service
producers for four successful batches. Exact sample-local call and timeline
records cover failures and isolation. No global event, metric, producer,
cursor, schema field, or ID is added, and no simulated timestamp is a latency
claim. The executable's stable line explicitly reports
`evidence=simulated_protocol`, `physical_hardware=false`, and
`direct_peer_dma=false`.

## M27-04 optional native telemetry source kit

The optional [telemetry source kit](../integrations/telemetry/README.md) adds a
bounded non-RT host snapshot queue and native Perfetto TrackEvent protobuf,
OTLP/HTTP protobuf logs/metrics and Windows TraceLogging adapters. The existing
Runtime inspectors, JSON API, schema2 record bytes and default SDK headers and
targets remain unchanged. The installed kit lives under
`${RTFW_DATA_DIR}/integrations/telemetry`; its README gives build, clock mapping,
identity, loss, retry, shutdown and native collection instructions.

Capture must be serialized with Runtime control/execution at a quiescent host
boundary. A consumer may drain copied snapshots on another host thread. No new
Runtime thread, queue, allocation, mutex or I/O is introduced into RT lanes.
Native output carries explicit session/Runtime identity, caller-supplied clock
anchors and separate Runtime loss versus exporter backpressure accounting.
Receiver acceptance and ETW emission attempts are not downstream delivery ACKs.
See [M27-04 evidence](evidence/M27-04-2026-10-01.md) for tested consumers and limits.
