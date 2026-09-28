[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)

> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.

# Host Runtime Lifecycle

M22-01 adds live-control configuration and staging, and M22-02 closes each host
frame or active compiled rate release before its callbacks. M22-03 optionally
wraps every step in a preallocated Runtime-generation transaction, settles
provisional records only after execution succeeds, and restores the step-entry
generation on failure. Checkpoint, restore, and live replay require one
fail-fast all-mailbox host claim. Stop closes new claims and future boundaries
before the existing quiescence and cleanup checks, then terminalizes staged
slots. See
[live_controls.md](docs__live_controls.md).

M22-04's typed helpers do not change lifecycle ownership. They build a raw
record in caller-owned storage and never call Runtime, retry admission, select
a later target, advance a sequence, or stop from a destructor. The caller must
inspect the unchanged raw admission result and perform checked stop. Typed
callback decode is bounded fixed-layout validation/copy only; semantic parsing
of variable-length configuration remains an off-lane operation.

M21-05 records M21-04 startup/failure/shutdown acknowledgements as closed
mixed-rate actions. Checkpoint export and active replay require a quiescent
release boundary; active replay prevalidates its full bounded artifact before
restore and accepts only explicitly deterministic mock/loopback backends.

`rt::Runtime` is the target-path embedding surface introduced in M1 and
extended with the M2 compiled graph, M3 unified executor, M4 finalized memory
plan, M5 time/platform controls, M6 versioned observability, M7
determinism/replay, the M8 bounded device ABI and deterministic mock, and the
M11 stable C ABI plus host job-system adapter, M12 portable distribution, and
the M13 complete runtime-profile loader, M15 CPU/memory closure, the M16
multi-rate work, the M17-01 HAL v2 core/device-ABI-v1 compatibility path, and
the M17-02 bounded heterogeneous-memory/topology extension. It
provides explicit host-driven and finite self-paced operation without adopting
the legacy `SimCore` scheduler or pacing loop.

The surface is RT0 functional behavior, not qualified latency.
`query_capabilities()` and `rt_query_capabilities()` report host-driven time,
compiled-graph validation, the unified CPU executor, the target-path bounded
memory plan, self-paced time, the frame watchdog, and strict platform preflight
as available. They also report versioned observability and deterministic replay
as available; the latter means the implemented D0/D1 surface, not D2 or D3.
They report `host_executor_adapter` for the borrowed engine job-system policy
and `bounded_device_backend` for the poll-only canonical HAL v2 runtime path.
That capability includes unchanged v1 backends through the adapter, the
M17-02 C++ memory/topology surface, and optional M17-03 C++ command/timeline
registration. The stable capability schema itself remains unchanged.
Those capabilities do not imply engine, CUDA, Vulkan, or XDMA qualification. The
optional M9 CUDA and M10 XDMA candidates are separately linked backends with
independent support matrices and evidence gates.

## Lifecycle

| Current state | Allowed operation | Resulting state |
| --- | --- | --- |
| `configuring` | typed configuration, optional host-executor or memory-provider attachment, graph/state/device/rate registration and declarations | `configuring` |
| `configuring` | `finalize()` | `finalized` |
| `finalized` | `start()`, preflight, and create configured runtime-owned lanes or bind the already-running host team | `running` |
| `running` | `step(frame)`, synchronous `replay(checkpoint, input_log)`, or closure-enabled `replay_live_control(artifact)` | `running` |
| `running` | `run_periodic(config)` | `running` |
| `finalized` or `running` | `stop()` and all device/lane/stack/memory cleanup succeeds | `stopped` |
| `finalized` or `running` | `stop()` and device/lane/stack/memory cleanup fails | public state unchanged; execution and mutation gated |
| cleanup pending | repeated `stop()` | retry unresolved cleanup; `stopped` only on success |
| `stopped` | repeated `stop()` | `stopped` |

Other transitions return `rt::Status::invalid_state`. A stopped runtime is
terminal. Destruction requires that no host call or callback is active; hosts
with device backends must call `stop()` and require success before releasing
borrowed resources. The C++ destructor cannot return cleanup status and fails
closed rather than discarding unresolved device, lane, stack, or provider
ownership. Checked `stop()` is the recoverable integration path.
Strict preflight failure leaves the runtime `finalized` without creating a
runtime thread, so the host can inspect the report or retry after external
setup.

An enabled live-control surface is immutable after finalization. Producer
handles are valid only for that Runtime identity and configuration generation.
Admission is allowed from bounded external producer threads in `finalized` or
`running`; Runtime callbacks are rejected. The terminal `stopped` instance
retains published records for inspection but rejects every new claim. Its
storage is discarded with the Runtime and is never reused by another Runtime
identity.

When the M22-03 closure is enabled, nested/concurrent step, periodic, replay,
checkpoint, restore, and stop ownership cannot share the step transaction.
The transaction restores only Runtime-owned immutable generation/mailbox
state. User callbacks and backends may already have produced side effects;
applications remain responsible for canonical payload validation and their own
registered-state or external transaction strategy.

M15-03 finalization transactionally acquires active phase-scratch,
task-scratch, and trace-storage backing in that order. Startup applies and
independently observes their memory policy before M15-02 verifies the caller
frame or creates a runtime-owned lane. Strict memory failure reverses completed
operations and leaves the runtime finalized and retryable. The held thread
transaction then aggregates native apply/readback and commits `running` only
after every required row is acceptable. A later creation, apply, mismatch, or
device-start failure quiesces device service, executor workers in reverse index
order, and watchdog before reversing memory policy. No phase, device command
provider, or periodic observer can run before commit. M15-04 additionally
reconciles constructed fragmented controls with the finalized plan, aggregates
live runtime-owned stack/guard commitment, and applies supported stack policy
inside the same startup barrier. External host-adapter, XDMA, and vendor lanes
remain verify-only; copied accounting declarations describe logical facts only.

A failed backend initialization gets one checked shutdown attempt. Failed
buffer unregistration and backend shutdown preserve their ownership markers,
and later `stop()` calls retry only unresolved operations in reverse order.
The device service, executor workers in reverse instance order, and watchdog
are quiesced; each lane performs stack cleanup on its owner before join. A
cleanup failure retains the quiescent lane and blocks fragmented-control and
trace/task/phase rollback plus provider release. While cleanup is pending,
`start()`, `step()`, `run_periodic()`,
checkpoint restore, replay, device health, and device reset fail closed;
read-only lifecycle diagnostics remain available.
The successful checked retry republishes the resolved owning-lane cleanup
state before entering `stopped`, including when the original failure occurred
during startup while the public state remained `finalized`.

Control operations are single-host-thread operations in 1.2. `step()`,
`run_periodic()`, checkpoint/restore, input-log export, and replay are
non-reentrant with execution. A periodic observer cannot recursively step, and
`stop()` called from inside a callback, periodic loop, or replay is rejected.

An enabled M16-01 rate model is also part of the finalization transaction.
Domain and binding storage is copied during configuration; the timeline is
compiled before provider acquisition or any native, device, thread, or callback
action. Failure publishes no plan and leaves the runtime configuring. After
success, fixed-copy inspectors remain available before start, while running,
and after stop. They do not resize or mutate the plan.

M16-02 cross-rate declarations join that transaction after the reference
timeline is complete and before memory-provider acquisition. Successful
finalization publishes copied initial bytes, immutable channel/selection
inspectors, and one preallocated store per channel together. Failure leaves the
runtime configuring with no cross-rate inspectors, provider callback, native
policy action, worker, device ownership, or callback execution. A rejected
declaration may be replaced under its stable instance-owned handle and retried.
Selection inspection distinguishes the first supercycle from repeating steady
state, and each compiled channel owns one two-slot SPSC store. M16-03 uses it
only when the configuring-only active execution policy is present.

M17-01 adds a second `register_device_backend()` overload for copied native
`HalV2BackendRegistration` records beside the existing positional
`DeviceBackendRegistration` source surface. Both are configuring-only, enforce
the same duplicate, capacity, handle-owner, and freeze rules, and publish no
partial handle on failure. A v1 registration creates one runtime-owned,
address-stable adapter; its canonical HAL v2 table cannot be invalidated by
later configuring-vector growth.

Capability discovery and all malformed-table/output validation complete while
configuring. Finalization accounts fixed adapter/table/context and translation
storage before provider, native-policy, thread, backend-init, or callback side
effects. Start and stop retain the existing startup barrier, one device-service
lane, reverse buffer/backend cleanup, first-error retention, and unresolved-only
retry. The supported manager invokes only HAL v2 operations; direct v1 calls
occur only inside the adapter. See [the HAL v2 contract](https://github.com/tranquilWorks/cpp-rt-car/blob/98ecca5a6572dd83d59e26063716f6d76b7a5d4f/docs/hal_v2.md).

M17-02 optionally discovers one bounded native memory/topology snapshot while
configuring. Discovery and validation are transactional. Core-only native v2
and adapted-v1 backends receive one implicit borrowed-host, host-coherent,
no-sync domain. Explicit heterogeneous buffer registration accepts one
same-instance domain plus a host span or opaque handle and validates the
declaration before publication. Start registers native memory after backend
initialization; checked stop unregisters it in reverse order before backend
shutdown and retains unresolved ownership for retry. Instance-local inspectors
expose copied domain, node, link, timestamp, completion-domain, and memory
object facts. Timestamp correlation is a bounded running-state control call.
See the [heterogeneous-memory contract](https://github.com/tranquilWorks/cpp-rt-car/blob/98ecca5a6572dd83d59e26063716f6d76b7a5d4f/docs/heterogeneous_memory.md).

## Typed configuration

`rt::RuntimeConfig` has twenty-five schema keys:

| Key | Type/default | Runtime behavior |
| --- | --- | --- |
| `callback_capacity` | positive integer, `64` | Maximum registrations accepted before finalization |
| `scratch_bytes` | nonnegative integer, `65536` | Bytes of distinct phase-local scratch exposed to each callback |
| `trace_capacity` | nonnegative integer, `1024` | Capacity of the instance-local lifecycle/callback trace ring; zero disables it |
| `numerical_mode` | `precise` | Selects `precise` or `fused_multiply_add` behavior for the callback numerical helper |
| `executor_policy` | `static_deterministic` | Selects `static_deterministic`, `bounded_throughput`, or `host_adapter` |
| `worker_count` | positive integer, `1` | Fixed runtime workers or exact borrowed host-worker count; accepted range is 1–256 |
| `executor_queue_capacity` | power-of-two integer, `1024` | Fixed capacity of each runtime-local queue, or total accepted-job reservation for `host_adapter`; accepted range is 2–1,048,576 |
| `scratch_alignment` | power-of-two integer, `64` | Alignment shared by phase/task scratch; accepted range is `alignof(max_align_t)`–4096 |
| `task_scratch_bytes` | nonnegative integer, `4096` | Callback-local bytes owned by each accepted graph/range/reduction context; accepted maximum is 1,048,576 |
| `task_scratch_slots` | positive integer, `1024` | Fixed simultaneous task-context reservations; accepted maximum is 1,048,576 |
| `memory_budget_bytes` | positive integer, `268435456` | Maximum reported target-runtime plan accepted by finalization; ceiling is 1 TiB on 64-bit hosts and addressable `size_t` on 32-bit hosts |
| `overload_policy` | `reject_submission` | Selects `reject_submission` or `fail_frame` for queue/scratch rejection |
| `watchdog_timeout_ns` | nonnegative integer, `0` | Nanoseconds from measured step start to watchdog expiry; zero disables the watchdog and the maximum is 24 hours |
| `watchdog_max_degradation_level` | nonnegative integer, `0` | Caps frame-thread degradation increments; accepted range is 0–255 |
| `platform_preflight_mode` | `disabled` | Selects `disabled` or fail-closed, read-only `strict` prerequisite checks |
| `workload_id` | identifier, `unspecified` | Labels observability and replay artifacts with 1–63 characters from `A-Za-z0-9._:/@-`; it affects configuration and replay identity |
| `determinism_tier` | `d0_unspecified` | Selects D0 unspecified behavior or D1 schedule-independent replay; D2 and D3 are rejected in 1.2 |
| `state_capacity` | nonnegative integer, `64` | Maximum canonical state regions accepted before finalization; zero disables registration |
| `snapshot_max_bytes` | positive integer, `1048576` | Per-runtime upper bound for encoded checkpoint bytes |
| `replay_input_capacity` | nonnegative integer, `4096` | Maximum records accepted in one encoded or replayed input log |
| `input_log_max_bytes` | positive integer, `1048576` | Per-runtime upper bound for encoded input-log bytes |
| `device_backend_capacity` | positive integer, `1` | Maximum combined native-v2 and adapted-v1 backend tables accepted before finalization; accepted range is 1–256 |
| `device_buffer_capacity` | nonnegative integer, `64` | Maximum borrowed device buffers accepted before finalization; accepted maximum is 65,536 |
| `device_outstanding_capacity` | positive integer, `64` | Runtime-wide preallocated outstanding submission slots; every backend must report at least this capacity |
| `device_completion_batch` | positive integer, `16` | Preallocated completion records supplied to each bounded poll; it cannot exceed outstanding capacity |

The typed structure can be supplied with `configure()`. Dynamic callers can use
`configure_key()` or the C `rtfw_config_set()` equivalent. Unknown keys,
partially parsed values, invalid enum values, and out-of-range capacities fail;
there is no ignored-key path.

`host_adapter` additionally requires `Runtime::set_host_executor()` or
`rtfw_set_host_executor()` before finalization. Its copied callback table must
declare capacities exactly equal to the typed configuration. The detailed
submission, scratch, completion, helping, and ownership rules are in the
[executor contract](docs__executor.md).

`Runtime::set_memory_provider()` is another configuring-only C++ attachment.
The runtime copies its exact size/versioned table and borrows `user_data`. All
five callbacks (`acquire`, `apply`, `observe`, `rollback`, and nonthrowing
`release`) are required, must not throw, and must not reenter the runtime. The
provider is consulted only for active phase scratch, task scratch, and trace
storage. Reentrant status calls fail closed and observation accessors return
safe empty/default values while any provider callback is active. Live token and
allocation-extent uniqueness is enforced across runtime instances. See the
[CPU/memory policy contract](https://github.com/tranquilWorks/cpp-rt-car/blob/98ecca5a6572dd83d59e26063716f6d76b7a5d4f/docs/cpu_memory_policy.md).

`<rt/profile.hpp>` maps the complete schema into `RuntimeConfig` through a
bounded, allocation-free, transactional JSON parser. It rejects missing,
unknown, duplicate, incompatible, and invalid contract fields before
`Runtime::configure()`. Profile file I/O remains a non-RT host responsibility.
See [runtime profiles](docs__runtime_profiles.md).

## Host-driven steps

The host supplies:

- a frame index;
- a simulation delta;
- an optional absolute deadline in that runtime clock's nanosecond domain.

`Runtime::step()` submits dependency-ready callbacks to the already-started
executor and waits for the graph to quiesce. It never sleeps, advances no
hidden frame counter, or creates a thread. The canonical registration-index
topological order remains available for introspection, but independent
callback completion is not a total order. `StepResult` reports callback count,
start/finish timestamps, deadline miss, watchdog event, and committed
degradation level.

A device command provider is scheduled like a CPU callback but does not finish
its graph phase when it returns. The worker validates and translates its
runtime-local buffer handles, then performs one bounded canonical HAL v2 core
submission. An accepted submission retains the phase token until the existing
runtime-owned service lane polls its completion. Independent CPU phases remain
runnable; dependent phases are released only by completion. Submission
saturation or a device failure becomes the typed step result. See the
[device backend contract](docs__device_backend.md).

Use `Runtime::now_ns()` or `rtfw_now_ns()` to form a deadline in the correct
clock domain. The default clock epoch is local to the runtime instance. C++ tests
can inject a `RuntimeClock`, including a fake clock.

If a callback returns an error or throws through the C++ surface, the step
returns `callback_failed`. Dependent and not-yet-invoked work is skipped;
already-running independent callbacks may finish. The runtime remains running
so the host can inspect the error and decide whether to retry or stop. A C
callback must not throw across the language boundary.

## Canonical state and replay

State registration is allowed only while configuring. Each registration
supplies a unique stable name, a nonzero schema version, and one non-empty,
non-overlapping caller-owned byte region. Its address and size are frozen by
finalization and must remain valid until runtime destruction. Those bytes must
already be a canonical representation; native object layout, padding,
pointers, and platform-endian fields are not portable state formats.

Checkpoint and input-log APIs are non-RT control operations. Callers provide
the output storage, and short buffers report the exact required size without a
partial artifact. Restore fully validates format, bounds, checksums, runtime
identity, registered schema, and every destination size before changing any
registered byte. Replay validates both artifacts before restore, applies each
input callback immediately before its frame, and invokes the ordinary
synchronous `step()` path. See the
[determinism/replay contract](docs__determinism_replay.md).

## Self-paced frames

`Runtime::run_periodic()` runs a finite frame count on the calling frame
thread. It waits for `first_release + i * period`, never shifts the release
epoch after a late frame, and supplies `release + relative_deadline` to each
step. Results expose release, wake, start, finish, signed slack, miss,
watchdog, and degradation fields. The C equivalent is `rtfw_run_periodic()`.

The injected clock must implement absolute waiting; otherwise the call returns
`clock_failure` without inventing a fallback cadence. Complete arithmetic is
validated before the first frame. The exact late-frame, observer, watchdog,
and overflow semantics are in the
[time/platform contract](docs__time_platform.md).

## Watchdog, degradation, and preflight

A nonzero watchdog timeout creates one service lane during `start()`. One arm
can produce at most one event. The service never invokes host code or mutates
degradation; the frame thread consumes the event after graph quiescence and
increments the capped level for following frames. The watchdog cannot abort a
stuck callback.

Strict platform preflight is disabled by default. When enabled, it inspects all
six Linux prerequisites before any runtime thread starts and fails closed with
`platform_preflight_failed`. A complete report remains inspectable while the
runtime stays finalized. The native probe reads host state but does not change
memory locking, affinity, scheduling, limits, or system policy. Passing these
checks is not RT2 qualification.

## Ownership and isolation

Each runtime owns:

- its callback registry and copied callback names;
- its logical resources, dependency declarations, and compiled phase order;
- one fixed scratch block per registered phase;
- a fixed pool of aligned task-scratch blocks reserved per accepted work item;
- its trace ring and write cursor;
- its numerical-helper policy;
- its clock object or explicitly borrowed C++ clock;
- its watchdog state and degradation level;
- its fixed-capacity platform-preflight report;
- its copied canonical-state registry metadata and replay identities.
- its copied memory-provider table, live region tokens, backing spans, policy
  observations, and rollback state, while provider `user_data` remains
  borrowed;
- its copied canonical HAL v2 backend tables, address-stable v1 adapter
  contexts and translation scratch, device registration metadata, outstanding
  slots, completion batch, service lane, and device telemetry counters.
- its copied memory/topology extension tables and snapshots, heterogeneous
  declarations, native memory tokens, and unresolved cleanup state.

Callback user data remains host-owned and must outlive every step that can use
it. A phase's scratch contents may persist across frames, but no phase sees
another phase's block. Cross-phase state belongs in host-owned memory covered
by resource declarations. Callers must not treat scratch as durable
application state.
Registered canonical state storage also remains host-owned and must remain
stable for the runtime lifetime. Checkpoint and input-log buffers are
caller-owned and need exist only for the duration of the relevant call.
Native HAL v2 and device-ABI-v1 backend instances, borrowed host spans, opaque
handles, and device command `user_data` remain externally valid through
successful checked cleanup according to their declared ownership. Adapter and
native-token state remains runtime-owned throughout unresolved cleanup.
The service lane is joined before buffers are unregistered and backend shutdown
returns.

The memory provider and its `user_data` must remain valid until checked
`stop()` succeeds. Stop quiesces device, executor, and watchdog lanes before
resident-memory rollback. For provider-backed storage it then destroys the
trace/executor owners and releases trace, task, and phase tokens in reverse
acquisition order. A rollback failure leaves the operation and tokens pending,
makes stop fail, and requires a checked stop retry before provider state may be
released. Because the trace backing has been released after successful stop,
trace access is unavailable after such a stop; the memory plan and CPU/memory
policy report remain inspectable. C++ destruction attempts the same cleanup
only as a best-effort fallback.

The new surface does not modify the legacy process-global `HighResClock`,
`bintrace` registration, or `rt::set_use_fma` flag used by `SimCore`. This is
what allows multiple `rt::Runtime` instances to remain isolated. Arbitrary
floating-point expressions inside user callbacks are still outside the
numerical helper's control.

## C ABI

Stable C ABI v8 mirrors the lifecycle:

- `rtfw_config_init` / `rtfw_config_set`;
- `rtfw_create`;
- `rtfw_register_phase` / `rtfw_register_callback`;
- `rtfw_register_resource`;
- `rtfw_add_dependency` / `rtfw_declare_resource_access`;
- `rtfw_parallel_for` / `rtfw_parallel_reduce` from a callback-local task
  context;
- `rtfw_task_scratch` for callback-local task storage;
- `rtfw_finalize`;
- `rtfw_get_memory_plan`;
- `rtfw_start`;
- `rtfw_step`;
- `rtfw_run_periodic`;
- `rtfw_get_platform_preflight_report`;
- `rtfw_get_degradation_level`;
- `rtfw_get_observability_metadata`;
- `rtfw_get_metrics`;
- `rtfw_read_trace`;
- `rtfw_register_state`;
- `rtfw_checkpoint_size` / `rtfw_checkpoint_write` /
  `rtfw_checkpoint_inspect` / `rtfw_checkpoint_restore`;
- `rtfw_input_log_write` / `rtfw_input_log_inspect`;
- `rtfw_replay` / `rtfw_registered_state_hash`;
- `rtfw_register_device_backend`;
- `rtfw_register_device_buffer`;
- `rtfw_register_device_phase`;
- `rtfw_get_device_health` / `rtfw_reset_device`;
- `rtfw_get_abi_info` / `rtfw_check_abi`;
- `rtfw_set_host_executor`;
- `rtfw_stop`;
- `rtfw_destroy`.

Device-owning C integrations call `rtfw_stop()` until it returns
`RTFW_STATUS_OK` before `rtfw_destroy()`. ABI v8 predates a status-bearing
destroy function. As a compatible fail-safe, `rtfw_destroy()` attempts stop and
does not delete the handle when device cleanup fails; the retained pointer can
be passed to `rtfw_last_error()`, `rtfw_stop()`, and finally `rtfw_destroy()`
after recovery. Because the void return cannot tell an arbitrary caller whether
the handle was consumed, this behavior does not replace the checked-stop
sequence.

Public configuration, frame, callback, result, and memory-plan structures carry
sizes, and configuration carries stable `RTFW_C_ABI_VERSION` 8 in release
1.2. Periodic, preflight, observability, checkpoint, input-log,
replay, and device structures follow the same initialized-output rule. Call
the supplied structure initializers and leave reserved fields zero.
`rtfw_status_message()` provides status text even when no runtime handle was
created; `rtfw_last_error()` adds handle-specific context. Export, compatibility,
ownership, SONAME, and change-policy details are in the
[stable C ABI contract](docs__c_abi.md).

## Compiled graph

Graph construction, resource-hazard rules, deterministic tie-breaking,
diagnostics, and allocation boundaries are specified in the
[compiled graph contract](docs__compiled_graph.md).

Finalization rejects cycles and unordered read/write or write/write access to a
shared logical resource. Once finalization succeeds, every topology mutation
is frozen. Independent phases may run concurrently, which is why resource
conflicts must be ordered explicitly instead of relying on an incidental
traversal.

## Current boundary

M21-01 adds configuring-only `bind_device_phase_to_rate_domain()` and
`replace_device_rate_binding()`. The copied role span must exactly cover the
flattened M17 declaration references and match their read/write access. After
finalization, bounded queries expose device phases, command skeletons, payload
slices/roles, timeline references, admission capacities/peaks, and exact
reference intervals. Query output contains no provider pointer, vendor object,
payload bytes, address, or wall-clock measurement.
After a device-rate admission failure,
`device_rate_admission_diagnostic()` exposes the fixed-size status, rejected
phase, and first reference index while `last_error()` retains the deterministic
text. A corrected successful finalization clears that diagnostic and publishes
only the complete immutable plan.

Reference-only mixed plans retain ordinary graph execution. If the M16 active
policy is selected, M21-02 accepts the mixed plan only when every device
reference maps directly to an admitted M21-01 command-batch record. The
provider receives an appended nullable release view and returns after copying
one validated batch into an existing backend queue slot; it invokes no vendor
entry. Existing submission/service lanes own submit, poll, and submitted
timeout cancellation. Exact tickets preserve device prerequisites and the
release-group terminal barrier while independent records may overlap.

M21-03 keeps channel-free execution exact and adds direct producer/consumer
channel slices for opted-in device endpoints. CPU→device selection copies one
fresh exact generation into the deterministic execution-slot subrange before
the provider runs. The provider must match the frozen envelope; Runtime alone
substitutes the compiled offset and payload byte count in its owned batch.
Device→CPU completion retains the terminal slot until the output subrange is
copied into the channel store and one generation with fixed release/status/
timestamp metadata is published. Failed terminal states publish nothing.

M21-04 validates sampled frames and makes acknowledged safe output part of
start and checked stop. M21-05 adds `set_mixed_rate_closure_policy()`, direct
action inspection, active-artifact writing/inspection, and `replay_active()`.
The policy is copied only while configuring. Action records become visible
only after their logical or terminal result settles; completion arrival cannot
rewrite an emitted terminal record. Active replay uses recorded nominal time
and decisions, re-executes the deterministic provider/backend path, and fails
at the first action, status, sampled metadata, content digest, or final-state
mismatch. Completed replay frames are not rolled back.

M4 finalizes aligned phase/task scratch, queue/control, and trace storage under
a configured memory budget. M15 supplies exactly the phase/task/trace backing
through a bounded provider or Linux resident mapping transaction. M15-04 adds
logical fragmented-control closure, live runtime-stack accounting, declared
external/backend facts, and retryable cross-category cleanup without expanding
that provider boundary. The target CPU frame path has a multi-frame
zero-allocation gate and explicit queue/scratch overload behavior. Plan scope
and exclusions are specified in the
[memory-plan contract](docs__memory_plan.md); policy, queue, and nesting details are
in the [executor contract](docs__executor.md). M5 adds absolute cadence, one-shot
watchdog/degradation, and fail-closed prerequisite reporting. It does not
preempt callbacks or qualify a deployment; see the
[time/platform contract](docs__time_platform.md). M6 adds bounded versioned
trace/counter emission and non-RT cursor/export APIs; see the
[observability contract](https://github.com/tranquilWorks/cpp-rt-car/blob/98ecca5a6572dd83d59e26063716f6d76b7a5d4f/docs/observability.md). M7 adds bounded canonical-state
checkpoint and replay control operations; see the
[determinism/replay contract](docs__determinism_replay.md). M8 adds bounded
submission/poll, graph-held completions, health/reset/shutdown, and a
fault-injectable mock; see the
[device backend contract](docs__device_backend.md).
M17-01 makes that path canonical HAL v2 for both registration kinds while
preserving the v1 mock/CUDA/XDMA implementations, lifecycle, identity, and
observable results. M17-02 adds bounded memory/topology discovery,
heterogeneous registration, inspection, and timestamp correlation while
preserving that core path. It adds no device-rate execution, command batch,
timeline, vendor lane, or qualification claim; see the
[HAL v2 contract](https://github.com/tranquilWorks/cpp-rt-car/blob/98ecca5a6572dd83d59e26063716f6d76b7a5d4f/docs/hal_v2.md) and
[heterogeneous-memory contract](https://github.com/tranquilWorks/cpp-rt-car/blob/98ecca5a6572dd83d59e26063716f6d76b7a5d4f/docs/heterogeneous_memory.md).
M16-01 adds rate-model/reference-plan inspection. M16-02 adds CPU-only
cross-rate declarations, immutable first/repeating selection inspection,
copied initial-sample inspection, and preallocated snapshot stores. M16-03 adds
a configuring-only opt-in execution policy, conservative serialized admission,
checked logical/nominal active windows, serial selected-CPU dispatch, exact-generation
publish/copy, late actions, result summaries, and one canonical active
checkpoint record. Reference-only behavior remains exact. Optional
CPU domains now use mandatory-release hysteresis, deterministic shedding and
reverse recovery, with policy state retained in the same generic record. A
separate fixed-capacity schema-1 rate-action ring and 20 counters/gauges expose
bounded loss-aware inspection without changing global observability schema 2.

## Code and evidence

- C++ API: `rt/include/rt/runtime.hpp`
- Implementation: `rt/src/host_runtime.cpp`
- Graph compiler: `rt/src/compiled_graph.cpp`
- Rate compiler: `rt/src/rate_timeline.cpp`
- Cross-rate compiler/store: `rt/src/cross_rate_data.cpp`
- Active admission/dispatch compiler: `rt/src/rate_dispatch.cpp`
- Rate-action telemetry: `rt/src/rate_telemetry.cpp`
- Unified executor: `rt/src/executor.cpp`
- Resident-region policy: `rt/src/memory_policy.cpp`
- C ABI: `rt/include/rt/c_api.h`, `src/c_abi.cpp`
- C++ lifecycle tests: `tests/test_host_runtime.cpp`
- C++ graph tests: `tests/test_compiled_graph.cpp`
- Rate/reference tests: `tests/test_rate_timeline.cpp`
- Active admission/dispatch tests: `tests/test_rate_dispatch.cpp`
- Shedding/telemetry tests: `tests/test_rate_telemetry.cpp`
- Executor tests: `tests/test_executor.cpp`
- Memory-plan tests: `tests/test_memory_plan.cpp`
- Memory-provider/policy tests: `tests/test_memory_policy.cpp`
- Time/watchdog tests: `tests/test_periodic_runtime.cpp`
- Platform-preflight tests: `tests/test_platform_preflight.cpp`
- Observability tests: `tests/test_observability.cpp`
- Determinism/replay tests: `tests/test_determinism_replay.cpp`
- Device ABI/runtime tests: `tests/test_device_runtime.cpp`
- HAL v2/compatibility tests: `tests/test_hal_v2.cpp`
- Artifact parser fuzz target: `tests/snapshot_fuzz.cpp`
- Dynamic C ABI test: `tests/test_cabi_dlopen.c`
- C sample: `samples/embed_c/mini_app.c`
- C++ sample: `samples/embed_cpp/mini_app.cpp`
- Device sample: `samples/device_mock.cpp`

## M17-03 host surface and lifecycle

`register_device_timeline()` and `register_device_batch_phase()` are
configuring-only. Handles are runtime/backend bound, and copied declarations
fix command order, operations, logical references, and wait/signal handles.
Providers run as ordinary graph CPU phases and fill one borrowed fixed batch;
they invoke no backend function.

Admission requires a finite timeout, one to 16 commands, zero to eight
prior-accepted waits, and one to eight increasing signals. Runtime performs one
bounded copy attempt. Concurrent host stop during a blocked submit issues the
nonblocking stop request and returns `invalid_state`; the host retries checked
stop after the active step quiesces. Callback-reentrant stop remains
side-effect-free. No new status or schema is introduced.

## M17-06 host graph and discovery

Command-capability discovery now preserves the default size/version input
prefix and zero semantic/reserved state; it still publishes only a complete
valid record. The portable combined sample registers both actual native
candidates while configuring, then freezes exactly five ordinary dependency
phases. The CPU bridge runs only after whole CUDA completion, performs one
fixed disjoint host copy, and precedes XDMA admission. Separate backend-local
timelines carry no cross-backend dependency.

Start, complete `step()` calls, injected failure, correction, and checked stop
use existing lifecycle/status behavior. No Runtime configuration key, lane,
status, schema, target, or post-start allocation path is added.

# M19-01 extension lifecycle

While configuring, `register_extension` stages and commits one complete ABI-v1
extension. Services initialize in registration order before Runtime lanes and
backends. `stop` closes extension admission first, continues independent
cleanup while retaining the first error, and cleans services in reverse only
after related backend ownership resolves. `detach_extension` is checked,
clears borrowed callables, retires the generation, and reports readiness
without unloading. Details are in
[extension registration](https://github.com/tranquilWorks/cpp-rt-car/blob/98ecca5a6572dd83d59e26063716f6d76b7a5d4f/docs/extension_registration.md).
