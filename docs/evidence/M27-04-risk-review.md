# M27-04 risk review — candidate, final Windows/hosted gates pending

No unmitigated implementation finding has been identified in the reviewed queue,
portable encoder or HTTP path. Acceptance is not complete until native Windows
ETL/TDH collection and exact-final-head full/hosted gates pass. The new sample
clock race discovered by TSan was fixed with atomic reads/increments and fresh
initialized TSan runs of both queue tests and shipped sample; the original report
is retained. Earlier sanitizer startup failures are not functional passes.

| Boundary | Concrete failure considered | Disposition / evidence |
| --- | --- | --- |
| Runtime lanes | A exporter silently reads concurrently with active execution or adds Runtime I/O | Runtime source is unchanged. README requires serialized caller-owned quiescent capture. Consumers see copied bounded slots only. Existing25 observability/no-allocation tests pass. |
| Queue ownership | Full/disabled/failed capture consumes interval/trace cursor or crosses Runtime owners | Candidate cursors commit only after validation and bounded admission. Tests exercise full, foreign owner, retry, overwrite,256-event continuation and two concurrent owners. |
| Sink lifecycle | A partial write is mistaken for exactly-once success or pending data silently flushed | False/throw retains front; partial writes explicitly may duplicate. Close keeps or explicitly counts discarded batches/events. Destructor has no implicit flush guarantee. |
| Native clocks | Runtime monotonic ticks are mislabeled Unix/QPC or over/underflow | Explicit caller-certified anchors and checked arithmetic; original timestamps retained. Real Trace Processor checks exact mapped timestamps and zero import errors. ETW header delivery time is explicitly separate. |
| Integer identity | FNV config IDs or uint64 event fields lose bits through signed/double consumers | Native annotations/attributes use exact decimal strings; ETW uses uint64. Native metric/timestamp signed range rejects overflow; Perfetto exact metric string accompanies chart values. Actual config ID above INT64_MAX is decoded exactly. |
| Native formats | JSON is renamed as native, or encoding tests agree with their own bug | Official protobuf modules plus actual Trace Processor SQL and Collector HTTP/file decoding. Independent decoders reject raw JSON. Spool has an explicit distinct schema. |
| Transport | Redirect/retry duplicates batches, or partial rejection is acknowledged | No redirects/automatic retries. Bounded timeout/response and explicit HTTP, malformed, partial, disconnect and timeout negatives. Logs/metrics are separate requests; receiver ACK does not claim downstream delivery. |
| ETW provider | One instance unregisters another; emission is mistaken for collection | Shared registration mutex/refcount and disabled-provider retention. Actual Windows private ETL session/TDH decoding is a mandatory pending gate; collector loss counters and exact owner/event/metric data are checked. |
| Packaging | Optional dependencies or kit targets leak into default SDK | Source-only data installation. Existing exact header/target checks remain; explicit kit inventory. Default/optional relocated clients and all71 existing SDK consumers are required. Python packages are explicit optional tools. |
| Untrusted spool | Oversize, duplicate fields, malformed records or foreign clock/owner history accepted |8MiB/64batch/128KiB line limits, strict schema/value/order/identity checks and five parser test groups. Cross-file continuity is explicitly caller-owned. |

Residual limitations are deliberate: offset-only clock mapping and caller-supplied
uncertainty; finite queues/history; no cross-file deduplication or delivery ACK;
no new rate/live-control/sampled-I/O schema export; no Windows evidence from Linux;
no hardware/HIL/RT/human/Unreal/controlled-performance/signing/release/deployment
claim. Sanitizers instrument new kit/test/sample translation units; the unchanged
Runtime archive is not reinstrumented in these focused runs. Final full profiles
and unchanged hosted gates remain mandatory. No tests, Runtime deadlines, schema,
ABI, support claims or predecessor failures are waived.

The first Windows compiler rejected a metric string_view supplied to a
TraceLogging string field. The adapter now holds a terminated copy through the
emission call. A real relocated Windows ETW consumer is also wired into the
existing package job. Actual Windows compilation/collection remains mandatory.
