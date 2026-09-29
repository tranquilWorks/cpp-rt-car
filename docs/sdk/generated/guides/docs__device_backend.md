[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)

> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.

# Bounded Device Backend Contract

M21-05 adds no HAL version. The installed `SampledIoLoopbackBackend` retains
the existing HAL-v2 core, memory/topology, and command/timeline tables and adds
only a bounded instance-local logical-action inspection/reset surface. It owns
no hidden thread. See [sampled_io.md](docs__sampled_io.md).

RTFW 1.2 retains the M8 target `rt::Runtime` device contract introduced in
0.9. The base contract adds a size/versioned C backend ABI, device phases in
the compiled graph, one runtime-owned completion-service lane, and a
deterministic fault-injectable CPU mock. M17-01 adds an additive C++ HAL v2
core and routes both native HAL v2 registrations and every unchanged
device-ABI-v1 registration through one canonical HAL v2 device manager. The
complete v1 translation contract is in [the HAL v2 contract](https://github.com/tranquilWorks/cpp-rt-car/blob/98ecca5a6572dd83d59e26063716f6d76b7a5d4f/docs/hal_v2.md).
M17-02 adds the optional memory/topology extension and Runtime registration,
inspection, and correlation surface described in the
[heterogeneous-memory contract](https://github.com/tranquilWorks/cpp-rt-car/blob/98ecca5a6572dd83d59e26063716f6d76b7a5d4f/docs/heterogeneous_memory.md).

This base is portable RT0 functional behavior. It is not itself
CUDA, Vulkan, XDMA, driver, latency, or RT2 qualification. The optional M9
CUDA implementation and its separate hardware-evidence boundary are in
[the CUDA backend contract](https://github.com/tranquilWorks/cpp-rt-car/blob/98ecca5a6572dd83d59e26063716f6d76b7a5d4f/docs/cuda_backend.md). The M10 Xilinx Linux XDMA AXI-MM
candidate uses the same device ABI while isolating blocking character-device
calls on fixed backend workers; its separate boundary is in
[the XDMA backend contract](docs__xdma_backend.md).

The legacy `hal/gpu_stub.hpp` detached-thread experiment is unchanged and
outside this contract. `rt::Runtime` does not invoke it.

## Ownership and lifecycle

A host registers either the existing `rtfw_device_backend_api` through
`DeviceBackendRegistration` or a native `HalV2BackendApi` through
`HalV2BackendRegistration` while the runtime is configuring. The function
table and its `instance` are borrowed through successful backend `shutdown()`.
Registered buffer storage and device-phase `user_data` are also borrowed until
runtime shutdown has completed. The runtime copies names, capabilities, graph
metadata, and the function table; it does not take ownership of backend or
application storage. The v1 table is additionally held by one runtime-owned,
address-stable compatibility adapter whose HAL v2 table is the only table seen
by the device manager.

The target lifecycle is:

1. The selected `register_device_backend()` overload validates device ABI
   version 1 or HAL API version 2, every required function, reserved field,
   identifier, and reported capacity. A v1 registration is wrapped exactly
   once before entering the canonical manager.
2. `register_device_buffer()` records either the legacy nonempty,
   nonoverlapping borrowed-host span or one explicit same-backend heterogeneous
   declaration. The backend does not see it yet.
3. `register_device_phase()` adds a command-provider phase to the same graph as
   CPU phases.
4. `finalize()` validates backend limits and commits canonical registrations,
   adapter/table/context storage, outstanding-slot, completion-batch, and graph
   storage to the memory plan.
5. `start()` starts the fixed CPU team, initializes each backend, registers
   buffers, then starts one runtime-owned device service lane. Failure performs
   a checked reverse rollback. An initialization failure is
   ownership-uncertain until one `shutdown()` attempt returns `OK` or
   `INVALID_STATE`; any other cleanup result is retained for `stop()` retry.
6. `step()` invokes a command provider on a CPU worker. The provider fills one
   fixed-size submission and returns. Accepted device work retains the phase's
   graph completion token; independent CPU work remains runnable.
7. The service lane polls completions and releases dependent graph phases.
8. `stop()` joins the service lane, cancels unresolved graph tokens,
   unregisters buffers in reverse order, and shuts backends down in reverse
   order before the CPU executor and watchdog finish stopping. Successful
   operations clear their ownership markers. Failed operations retain them;
   a backend is never shut down while one of its buffers remains registered,
   and independent cleanup continues while the first failure is preserved.
   Repeated `stop()` calls invoke only the unresolved operations. No backend
   callback exists in the ABI, so backend code cannot call into destroyed
   runtime or plugin ownership.

Runtime control methods are single-host-thread operations. The host must not
destroy a runtime, backend instance, buffer, plugin, or callback data while a
host call or callback is active.

If cleanup fails, `stop()` returns that status and leaves the public lifecycle
state at `running` or `finalized`; execution, restore/replay, health, reset, and
restart operations are gated until a later `stop()` succeeds. The service
lanes and executor are already quiesced. The host must retain every borrowed
backend table, instance, buffer, plugin, and callback object throughout the
retry. A C++ destructor cannot report a cleanup result and is only a
best-effort fallback, so device integrations must use the checked `stop()`
path.

## HAL and ABI shape

The supported manager invokes only HAL v2 core records. A native-v2 table and
an adapted-v1 table receive equivalent validation and lifecycle handling.
Direct calls to device ABI v1 are confined to the compatibility adapter; the
mock, CUDA candidate, and XDMA candidate continue to implement their frozen v1
surface unchanged.

The HAL v2 C++ records use API version 2, the existing 64-byte backend
identifier, 128-byte inline payload, and eight-reference limit. The required
table covers capability discovery, initialization, host-buffer registration
and unregistration, one core submission, bounded completion polling,
cancellation, health, reset, and shutdown. It has a non-null borrowed instance
and a zero reserved tail. See [the HAL v2 contract](https://github.com/tranquilWorks/cpp-rt-car/blob/98ecca5a6572dd83d59e26063716f6d76b7a5d4f/docs/hal_v2.md) for the complete
records, v1 field/status map, malformed-output rules, and deferred features.

`rt/include/rt/device_abi.h` is a C-compatible backend boundary with device ABI
version 1. All extensible records carry `struct_size`; ABI-bearing records also
carry `abi_version`. Reserved fields must be zero.

The backend supplies:

- capability discovery and initialization;
- host-buffer registration and unregistration;
- bounded, nonblocking submission;
- bounded, nonblocking completion polling;
- cancellation support or an explicit `unsupported` result;
- health, reset, and shutdown.

Every function is required in the table and must not throw across the ABI.
Optional behavior is represented by a capability byte and a stable status, not
by a null function. `submit()` and `poll()` must return without sleeping,
waiting for device completion, allocating an unbounded spill queue, invoking
host code, or creating a per-submission thread. `poll()` writes only to the
caller-provided completion array and never invokes a callback.

After an `initialize()` attempt fails, the runtime calls `shutdown()` once.
The backend returns `INVALID_STATE` only when that failed initialization owns
no resource requiring release. If partial initialization or rollback retained
anything, `shutdown()` returns the cleanup failure and remains retryable; it
must not report `INVALID_STATE`. The runtime then retains the backend table and
instance until a later shutdown succeeds. The same retry rule applies after a
failed buffer unregistration or ordinary shutdown.

`submit()` and `poll()` may run concurrently for one initialized backend
instance. A backend must synchronize its own fixed-capacity state without
blocking either caller. The manager uses an explicit submitting/early-ready
handshake: successful accounting and `device.submitted` are published before
an early completion is applied, so a completion cannot overtake its submission
record.

The adapter catches every exception before it crosses either table boundary,
validates every returned v1 status and output record, and preserves
`UNSUPPORTED` as a failure rather than promoting it. Native-v2 outputs receive
the same fail-closed validation. A malformed completion, health record, token,
output count, enum, boolean, identifier, or reserved field publishes no partial
result or ownership transition.

The current fixed submission contains:

- runtime-assigned submission and frame IDs;
- a positive backend-enforced `timeout_ns`;
- backend-defined opcode and up to 128 inline payload bytes;
- up to eight buffer references with token, access, offset, and length;
- zero flags and reserved fields in ABI version 1.

Application command providers use runtime-local logical buffer handles. The
manager bounds-checks each range and access, verifies backend ownership, and
translates it to the backend's private token before submission.

## Execution semantics

M21-01 does not alter ordinary frame semantics. It admits only phases
registered through `register_device_batch_phase()` on a backend with copied
command/timeline capabilities and a valid monotonic completion timestamp
domain. Legacy device-command and device-ABI-v1 phases remain usable in the
ordinary graph but cannot opt into active device-rate admission. This is an
additive support boundary, not a device-ABI-v1 incompatibility.

Declared device-rate completion budgets are planning inputs, not measured
latency or backend qualification. M21-02 clamps a materialized finite timeout
to the earlier checked completion-budget or release-deadline remainder, then
queues the copied batch and exact release identity. Existing backend lanes own
submit, poll, and timeout cancellation. A vendor-owned timeout becomes one
terminal quarantined slot; late completion cannot publish timeline progress or
convert it to success, and checked shutdown is the reuse boundary.

M26-03R adds an explicit `DeviceRatePhaseBinding::simulation` opt-in. Its
`DeviceRateSimulationPolicy::host_watchdog_ns` must be positive and at most
`device_rate_simulation_watchdog_limit_ns` (60 seconds). Only a backend whose
copied core capability declares `deterministic_mock` can use it. Binding and
replacement copy/validate the value; `CompiledDeviceRatePhase::simulation`
exposes the finalized value. Missing policy preserves the existing path;
explicit zero is invalid. Native CUDA does not become deterministic.

For this policy, the unchanged logical completion budget still drives admission
and clamps the unchanged backend command timeout. The device slot uses the
separate steady-clock watchdog for host liveness. Before successful timeline or
payload publication, the service lane checks the Runtime logical deadline as
well as the watchdog; a timestamp from an uncorrelated backend domain is never
used for this comparison. The borrowed Runtime clock must support bounded,
monotonic concurrent `now_ns()` reads. A pending backend with no completion is
bounded by the host watchdog even when the logical clock remains frozen.

The watchdog also covers queued work and the early-completion/submission-return
handshake. An unsent timeout has no vendor owner; accepted work remains
quarantined until checked cleanup. It does not preempt a vendor callback that
violates the existing bounded/nonblocking HAL contract. Stop/reset ownership,
ordinary command batches and default/native deadlines retain their contracts.
This is portable simulator behavior, not physical deadline qualification.

A device phase has two distinct moments in ordinary graph execution:

- the command provider is a normal bounded CPU phase and ends when submission
  is accepted or rejected;
- the graph phase completes only when the service lane publishes its device
completion.

M21-03 does not change backend tables. An opted-in cross-rate device endpoint
must select one copied payload-reference envelope on host-coherent registered
storage. Runtime validates the provider against that envelope before replacing
only its offset and byte count with a precompiled execution-slot subrange.
Input bytes are committed before submit; output bytes are captured only after
correlated terminal success and before the retained rate slot is recycled.
Borrowed buffer bytes remain application/backend-owned.

M21-05 active replay admits a backend only when its frozen existing
capabilities report deterministic-mock behavior. It still executes provider,
submit, poll, completion, frame validation, and publication, but compares the
result with the recorded logical transcript instead of deriving decisions from
completion arrival order. Physical or nondeterministic backends may produce
ordinary action evidence but cannot enter active replay.

CPU compute workers never wait for a device fence, future, callback, or
condition variable. `step()` remains synchronous to its host: it returns after
all CPU and device graph tokens have completed or one has failed. A dependent
phase is never released by provider return alone. Independent phases may run
while a device is outstanding.

The device service lane uses preallocated outstanding slots and a preallocated
completion batch. It sleeps on an atomic notification when there is no device
work and polls only while submissions are outstanding. Backend polling
frequency and hardware completion bounds remain backend/deployment concerns.

Active rate batches reuse that storage with one finite ownership extension:

| State | Atomic owner and permitted successor |
| --- | --- |
| reserved | provider-dispatch owner copies identity/batch, then `queued` |
| queued | submission lane claims `submitting`; an expired unsent wait claims ordinary terminal timeout |
| submitting | submission lane publishes `submitted` or existing early-ready state; service timeout may claim `rate-quarantine-owned` |
| submitted | service completion claims existing owned state; service timeout claims `rate-quarantine-owned` |
| early/completion ready | existing submission/service handshake claims owned completion |
| rate terminal | host consumes only the exact slot/batch/release ticket, then `free` |
| rate-quarantine-owned | service lane records one timeout/cancel outcome, then `rate quarantined` |
| rate quarantined | never reused during execution; a late exact completion is ignored without timeline publication |
| free | available only after normal ticket consumption or successful checked backend shutdown |

Every ticket contains slot index, nonzero batch generation, reference index,
domain, phase, supercycle cycle, domain release sequence, substep,
logical/nominal release, absolute deadline, and completion budget. Slot reuse
changes the batch generation, so a duplicate, unknown, stale, malformed,
wrong-domain, or wrong-signal completion cannot settle a newer release.

## Status and recovery

Backend statuses map to stable runtime statuses:

| Device ABI result | Runtime result |
| --- | --- |
| `QUEUE_FULL` | `device_queue_full` |
| `TIMEOUT` | `device_timeout` |
| `ERROR`, `UNSUPPORTED`, `INTERNAL_ERROR` | `device_error` |
| `LOST` | `device_lost` |
| `CANCELED` | `device_canceled` |
| `RESET_REQUIRED` | `device_reset_required` |
| `RESOURCE_EXHAUSTED` | `resource_exhausted` |

`device_health()` and `reset_device()` are non-RT host operations. They require
a running runtime, reject an active frame/periodic/replay call, validate the
instance-local backend handle, and never race an outstanding runtime
submission. Reset produces a trace event and counter only after the backend
reports success.

## Deterministic mock

`rt::MockDeviceBackend` owns fixed queue and buffer tables and creates no
thread. The runtime's service lane advances it by polling. Accepted
submissions have a one-based deterministic ordinal. A pre-start fault script
can attach one action to an ordinal:

- `delay`: add a fixed number of polls before completion;
- `timeout`: complete with timeout after deterministic timeout/poll-quantum
  conversion;
- `error`: complete with a stable backend error;
- `loss`: complete as lost and require reset.

The mock also supports deterministic queue saturation, inline writes, byte
fills, cancellation, health counters, reset generations, and shutdown. It
declares `deterministic_mock=1`; D1 finalization rejects any registered backend
that does not make that declaration. This declaration qualifies only the mock
model and application obligations in the D1 contract. It does not make real
hardware deterministic.

## Memory and observability

The finalized memory equation includes `device_control_bytes`. The plan reports
backend/buffer counts, outstanding capacity, completion batch, and
backend-reported private control bytes. Adapter context, copied v1 table, HAL
v2 table, and fixed translation scratch are runtime-owned device controls and
are counted exactly once in `device_control_bytes`. Backend-reported storage is
informational and excluded from `planned_bytes` because the backend owns it.
Registered buffer payload bytes are borrowed and excluded.

M17-02 extension tables, copied domain/topology/timestamp snapshots,
heterogeneous registration records, native tokens, and translation state are
also counted exactly once in `device_control_bytes`. Host spans and opaque or
device allocations remain excluded payloads; their declared logical byte
counts do not confer Runtime ownership.

M15-04 reconciles the runtime-owned device-manager object and constructed
registration, adapter, outstanding, completion, and service-control
allocations as non-overlapping logical extents against `device_control_bytes`.
Backend-private `control_storage_bytes` remains capability metadata: a matching
declaration is `declared_only`, and a contradiction fails finalization. Neither
mechanism authorizes access to backend-owned memory or changes device ABI v1.
The six-row `MemoryPlan` equation and three provider-capable regions are
unchanged.

Checked cleanup preserves M14.1 backend and buffer ownership before the
device-service lane performs stack cleanup on its owning quiescent thread and
joins. A failed backend or stack cleanup retains ownership and blocks lower
control/selected-region rollback and provider-token release until `stop()`
retry succeeds.

Observability schema version 2 retains IDs 0–21 and adds device submission,
completion, failure, saturation, timeout, loss, reset, poll, outstanding, and
service-start metrics. Trace IDs 12–14 report submission from its CPU worker,
completion from `device_service`, and reset from `host`. Telemetry is
diagnostic and does not establish a completion deadline.

Adapted v1 backends retain the exact pre-M17 graph/replay identity byte path.
Native-v2 backends conditionally contribute their backend kind and API version
2 to compatibility identity. Checkpoint/input-log schemas and codecs remain
version 1.

Native extension semantics and explicit heterogeneous registrations also enter
identity conditionally. Adapted-v1 and core-only v2 legacy registrations retain
their exact M17-01 path. Global observability schema 2 remains unchanged;
timestamp-domain inspectors and correlation results are control-plane data, not
trace timestamps or new metrics.

## Evidence and exclusions

Automated evidence covers native-v2 and adapted-v1 validation and equivalence,
queue saturation, early completion, cancellation, delayed dependency release,
timeout, error, loss, reset, reverse/retryable shutdown, failed-start rollback,
malformed tables and outputs, compatibility identity, C ABI dynamic loading,
memory-plan accounting, steady-state allocation, and sanitizer execution in
`tests/test_hal_v2.cpp`, `tests/test_device_runtime.cpp`,
`tests/test_determinism_replay.cpp`, `tests/test_trace_noalloc.cpp`, and
`tests/test_cabi_dlopen.c`. `samples/device_mock.cpp` remains the minimal v1
C++ flow and runs through the adapter.

M9 remains separately gated even though 1.2 contains a real CUDA Driver API
adapter candidate. It still needs a named driver, toolkit, hardware, OS, and
workload support tuple plus functional, recovery, resource, and
latency-decomposition evidence before its support matrix can contain a
qualified tuple. M10 likewise provides a bounded XDMA candidate but no
qualified hardware tuple. Neither backend inherits a portable completion-time
claim from M8.

M17-02 establishes bounded heterogeneous-memory, topology, coherency,
synchronization-declaration, and timestamp-correlation contracts. M17-03 adds
fixed command batches, same-backend timeline completion, ordered explicit
synchronization, one isolated Runtime submission lane per opted-in backend,
and whole-completion validation. The legacy single-submit and adapted-v1,
core-only-v2, memory-only-v2, mock, CUDA-v1, and XDMA-v1 behavior remains.

Synthetic tests do not establish physical CUDA/XDMA memory, peer DMA,
device-clock accuracy, native vendor-v2 behavior, CUDA Graph, XDMA controls,
physical accelerator behavior, HIL, field performance, worst-case latency,
RT1, RT2, signing, release, deployment, or production readiness. M17 and
CAP-M17 remain incomplete.

## M17-04 native CUDA and XDMA command backends

The CUDA and XDMA candidates offer native HAL-v2 registrations beside their
unchanged device-ABI-v1 APIs. The registration points at the same backend
object, which enforces exclusive path ownership through checked shutdown.
Runtime copies the three generic tables and continues to own all
submission/service lanes; the candidates add no Runtime thread or callback.

CUDA translates generic copy commands and vendor dispatch commands into one
declared-order stream. A pre-registered graph is selected by
`0x43470000 | graph_id`, and one completion event is recorded after the
complete accepted batch enqueue. XDMA translates its existing transfer opcodes
plus stable bounded control-read, control-write, and event-wait opcodes onto
its fixed backend I/O team. Every command is wholly validated before vendor
entry.

Partial enqueue, timeout, loss, reset, stop, and shutdown retain every buffer,
graph, stream, event, descriptor, batch, or driver owner that could still be
referenced. A backend completion alone authorizes the existing whole-timeline
publication; neither Runtime nor a backend fabricates completion. New storage
is fixed backend-private control reported through capability bytes and does
not change Runtime device-control accounting or the MemoryPlan equation.

M17-06 makes canonical Runtime registration of these command extensions
reachable. Discovery passes a value-initialized capability record whose exact
size/version prefix is present and whose semantic/reserved tail is zero, then
retains complete returned-record validation. Focused coverage registers the
actual CUDA and XDMA candidates together in one Runtime and reaches checked
stop; malformed, partial, error, and exceptional outputs remain transactional.
# Extension device-v1 records

An ABI-v1 extension may copy one or more complete
`rtfw_device_backend_api` version-1 tables. Runtime wraps each table with an
ownership gate and routes it through the same
`DeviceV1CompatibilityAdapter` and DeviceManager path as direct registration.
No HAL-v2 memory/topology or command/timeline table is accepted, so this path
cannot expose native CUDA/XDMA features. Stop retains related services until
reverse retryable backend shutdown has released ownership.

## M17-06 portable combined boundary

The combined sample uses two native registrations and two local timelines but
adds no backend kind, command, lane, queue, status, or completion rule. CUDA
and XDMA batches complete independently. A CPU dependency phase copies a fixed
extent between disjoint host regions only after the CUDA download completes;
no cross-backend timeline, fence, registration, coherency, or direct peer DMA
is created. Injected protocol behavior is not physical device evidence.

## Explicit four-slot sampled storage

The existing sampled-I/O `ring_capacity` accepts 2 or 4. Four selects a real
bounded four-slot cross-rate snapshot store for that channel. It does not
increase the device phase's declared maximum in flight, backend queue size or
device staging envelope. Ordinary channels and the default two-slot constant
remain unchanged. Invalid counts and inconsistent frame geometry fail before
four-slot storage allocation; existing global byte and Runtime budgets apply.

No native capability, timeout, sampled safe acknowledgement, public structure
layout or artifact schema changes. Existing ring capacity identity makes two-
and four-slot checkpoints/replay incompatible before state changes. See
[memory accounting](docs__memory_plan.md) for the exact reconciled counts.

## Opt-in native backend capacity

Select `Runtime::set_device_capacity_policy(DeviceCapacityPolicy::native_per_backend)` before registering any backend. Every backend must supply native HAL-v2 memory/topology and command/timeline extensions. Outstanding slots resolve to min(global outstanding, core max_in_flight, command max_in_flight_batches); polling resolves to min(global completion batch, command completion_batch_capacity). Initialization, actual slot storage, retirement and cleanup use those same bounds. Global rate admission remains enforced; native CUDA1/XDMA2 can reserve an aggregate3 without queue inflation. Uniform is the unchanged default. Malformed policies and unsupported mixtures reject before backend initialization; policy selection freezes on first successful backend registration.


## Sampled lifecycle and checkpoint recovery

Checked stop retains a completed sampled stop-time safe-output acknowledgement across partial backend teardown. Retry continues teardown without resubmitting to workers already stopped. A failed acknowledgement remains unknown and must execute successfully on retry before teardown may complete.
