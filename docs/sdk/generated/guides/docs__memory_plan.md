[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)

> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.

# Finalized Memory and Overload Contract

M22 adds no provider region. Copied live-control declarations, mailbox and
producer controls, atomics/counters, immutable record slots, payload strides,
and copied rate-target identities are counted exactly once in the existing
Runtime-control row and logical extent ledger. `MemoryPlan` appends mailbox,
producer, total-record, payload-storage, and complete live-control-control byte
fields. M22-02 also counts candidate indexes, terminal state/counters,
boundary cursors, and two complete immutable generation record/payload stores
in that subtotal. Disabled policies report zeros. Admission, boundary close,
callback views, and inspection use only finalization-time storage and
caller-owned output spans.

M22-03 closure accounting adds the copied semantic policy, action controls and
fixed slots, a third complete rollback generation, provisional source records,
retained generation/record/payload storage, replay controls, and fixed
conditional-checkpoint scratch exactly once to `runtime_control_bytes` and the
logical extent ledger. Observational capacities do not alter semantic identity
when replay is disabled. Caller checkpoint/replay buffers and registered
application state remain borrowed and excluded.

M22-04 adds no Runtime allocation, MemoryPlan field, provider region, lane,
registry, or hidden storage. `LiveControlTypedPayload<T>` is caller-owned
compile-time fixed storage and the header-only builders/decoder use only their
arguments and bounded automatic scalar state. The existing raw Runtime copies
the resulting bytes into the already-accounted M22 mailbox/generation storage.

M21-05 action slots/counters, replay controls, digest staging, and conditional
mixed-rate checkpoint state are included exactly once inside
`rate_plan_bytes` and `runtime_control_bytes`. The plan exposes their exact
subtotals without adding a seventh row. Caller artifacts, registered coherent
payloads, telemetry output spans, and loopback-owned storage are not counted.

Release 1.2 retains the M4 memory closure for the target-path runtime,
`rt::Runtime`. This is portable RT0 functionality: it defines and tests
bounded storage and nonblocking overload behavior, but it is not a latency or
hard-real-time qualification.

The legacy `SimCore`, frame arenas, `WorkerPool`, `rt::Scheduler`, `FiberPool`,
detached-thread GPU stub, plugins, and legacy snapshots are outside this
contract. M7 target-path checkpoint/replay registry metadata and M8
device-manager storage are included; caller-owned canonical state, artifacts,
device buffers, and backend-private storage are explicitly reported or bounded
but not allocated by the runtime.

## Finalization-time plan

`Runtime::finalize()` compiles the graph, calculates a `MemoryPlan`, compares
`planned_bytes` with `memory_budget_bytes`, and allocates the committed
storage. M15-03 obtains phase scratch, task scratch, and trace storage through
the default allocator or a configured provider before constructing their
owners. An overflow, invalid provider span/token/capability, allocation or
construction failure, or plan above budget returns an error while the runtime
remains configurable; provisional ownership is released in reverse order.

After successful finalization, `Runtime::memory_plan()` returns the immutable
plan. C hosts initialize `rtfw_memory_plan` with `rtfw_memory_plan_init()` and
read it with `rtfw_get_memory_plan()`.

The accounting equation is:

```text
planned_bytes =
    runtime_control_bytes +
    executor_control_bytes +
    device_control_bytes +
    phase_scratch_total_bytes +
    task_scratch_total_bytes +
    trace_storage_bytes
```

The plan reports requested payload/control bytes, not a process resident-set
size. It excludes allocator metadata and rounding internal to the C++ runtime,
OS thread stacks and implementation-owned thread state, executable and shared
library pages, host-owned callback/resource data and registered canonical state
bytes, caller-owned checkpoint/input-log buffers, registered device-buffer
payload bytes, backend-reported private storage, the small C ABI adapter
handle, and every legacy subsystem outside `rt::Runtime`.
Configuration/finalization temporaries are also excluded because they are
released before the running state.

`state_count` and `registered_state_bytes` report the frozen borrowed state.
`snapshot_max_bytes`, `replay_input_capacity`, and `input_log_max_bytes` expose
the enforced artifact bounds. State-registry entries themselves are included
in `runtime_control_bytes`.

`device_backend_count`, `device_buffer_count`,
`device_outstanding_capacity`, and `device_completion_batch` report the frozen
device plan. `device_control_bytes` includes copied canonical HAL v2
registrations, device-ABI-v1 adapter contexts/tables and fixed translation
scratch, the outstanding/early-completion table, completion batch, counters,
and service-lane object. Each adapter byte is represented once.
`device_backend_reported_bytes` is informational and excluded because each
backend owns that storage.

With the M11 `host_adapter` executor policy, `queue_slots` equals the declared
total host reservation rather than `worker_count * executor_queue_capacity`,
but `executor_control_bytes` includes only runtime-owned
completion/scratch-slot records and graph state. Host queue storage, worker
objects, stacks, affinity state, and job-system telemetry remain borrowed
host-owned memory and are excluded for the same reason as backend-owned
storage. The adapter must reserve the declared capacities before runtime
start; the runtime does not allocate or resize a host queue.

## M15 policy and resident-region projection

M15 leaves `MemoryPlan` and its equation source-compatible and adds a separate
`CpuMemoryPolicyReport`. Its six planned accounting rows map one-for-one to
`runtime_control_bytes`, `executor_control_bytes`, `device_control_bytes`,
`phase_scratch_total_bytes`, `task_scratch_total_bytes`, and
`trace_storage_bytes`; their `accounted_bytes` sum must equal `planned_bytes`.
Separate informational rows report registered state, backend-reported control,
and the checked sum of registered device-buffer spans. Stack and reserved
provider rows remain excluded from `planned_bytes`.

M15-03 makes exactly the active phase-scratch, task-scratch, and trace-storage
rows provider-capable. They are acquired in that order with the logical plan
bytes and existing alignment, then used as backing for the same scratch slices
and telemetry slots. A zero-byte selected row remains observable and does not
invoke a provider. The default path preserves aligned-new allocation unless
requested Linux page rounding, guards, or explicit huge pages require a
process-local mapping.

`accounted_bytes` remains the requested plan identity. `committed_bytes`
reports validated usable commitment after page rounding and therefore may
exceed the logical payload without changing `planned_bytes`. Resident,
verified-locked, and provider-confirmed pinned bytes are separate observations;
actual guard spans, actual page bytes, explicit huge-page outcome, fallback,
and provider/native errors are separate too. No physical field is added to the
plan equation or counted again in `memory.host-provider`. That reserved row is
always zero/not-applicable and rejects declarations; provider commitment and
observation stay on the selected planned rows.

M15-04 builds a checked logical extent ledger after persistent controls are
constructed. Every runtime, executor, and device extent has a nonzero stable
ledger identity, a validated address range, and exactly one owner. Missing,
duplicate, overlapping, overflowing, or estimate-mismatched inventories fail
finalization before report publication. The three ledger totals equal the
existing control terms; the six planned rows still sum to `planned_bytes`.

Runtime-owned stack commitment and guard bytes are aggregated exactly once
from live executor, watchdog, and device-service lanes and remain excluded from
the plan equation. On Linux NPTL the guard is a subspan of the native stack
extent returned by `pthread_getattr_np`, not additional commitment.
Cross-category totals classify facts as exact,
declared-only, partial, unknown, or not applicable. Bounded declarations may
close logical byte/cardinality facts that device ABI v1 or an external host
does not expose, but never authorize mutation or establish residency, locking,
pinning, or qualification. Borrowed registered state/device buffers and
backend-owned storage are not mutated. See
[the CPU/memory policy contract](https://github.com/tranquilWorks/cpp-rt-car/blob/912ce878ad311d38e452a5d2916ee0daa6ee3f72/docs/cpu_memory_policy.md).

## M16-01 rate-plan accounting

Copied rate-domain definitions, phase bindings, compiled domain/binding
records, and the reference-release vector are configuration/finalization-only
dynamic storage. `MemoryPlan::rate_plan_bytes` reports that exact dynamic
subcomponent, while the same bytes are included once in
`runtime_control_bytes` and its validated logical extent. Domain, binding, and
release counts are frozen inspectors.

No seventh planned row is added. The six-row equation and the provider's exact
phase-scratch/task-scratch/trace acquisition boundary are unchanged. Plan
inspection and complete CPU/mock-device frames allocate no ordinary heap after
start; two runtime instances own separate handles and release vectors.

## M16-02 cross-rate accounting

Copied channel specifications and initial bytes, compiled descriptors and two
selection records per consumer reference release, store objects, packed atomic
slot controls, and fixed payload slots are included exactly once in
`MemoryPlan::rate_plan_bytes` and the same M15 runtime-control logical extent.
The plan separately reports channel/selection counts, copied initial bytes,
total slot count, and total snapshot payload bytes for reconciliation.

Limits are explicit: 256 channels, names shorter than 64 bytes, 64 KiB per
payload, 1 MiB aggregate initial bytes, 262,144 selections, two slots per
ordinary/default channel, and 2 MiB aggregate slot payload. An explicit
`SampledIoChannelRegistration::ring_capacity = 4` opts that sampled channel into
four actual slots; supported sampled counts are exactly 2 and 4. The same count
drives payload/control allocation, compiled descriptors and MemoryPlan.
The aggregate byte caps still apply across mixed two/four-slot channels. Checked arithmetic and construction
complete before publication. No seventh planned row is added and cross-rate
storage never enters the phase/task/trace `MemoryProvider` acquisition boundary.

## Configuration

| Key | Default | Contract |
| --- | ---: | --- |
| `scratch_alignment` | 64 | Power-of-two alignment for phase and task scratch; accepted range is `alignof(max_align_t)` through 4096 |
| `task_scratch_bytes` | 4096 | Bytes exposed to each accepted execution context; zero creates a valid empty span and the maximum is 1,048,576 |
| `task_scratch_slots` | 1024 | Maximum simultaneously accepted execution contexts; must be at least the compiled phase count |
| `memory_budget_bytes` | 268435456 | Upper bound applied to `planned_bytes` at finalization; accepted ceiling is 1 TiB on 64-bit hosts and addressable `size_t` on 32-bit hosts |
| `overload_policy` | `reject_submission` | Selects `reject_submission` or `fail_frame` |
| `state_capacity` | 64 | Maximum registered canonical state regions; registry metadata contributes to runtime control bytes |
| `snapshot_max_bytes` | 1048576 | Maximum encoded checkpoint size; output storage is caller-owned |
| `replay_input_capacity` | 4096 | Maximum input records accepted by input-log write/replay |
| `input_log_max_bytes` | 1048576 | Maximum encoded input-log size; output storage is caller-owned |
| `device_backend_capacity` | 1 | Maximum backend registrations; copied table/control metadata is budgeted |
| `device_buffer_capacity` | 64 | Maximum borrowed buffer registrations; payload bytes are excluded |
| `device_outstanding_capacity` | 64 | Fixed runtime-wide outstanding-slot count and backend minimum |
| `device_completion_batch` | 16 | Fixed poll-result batch; cannot exceed outstanding capacity |

`scratch_bytes`, `trace_capacity`, `worker_count`, and
`executor_queue_capacity` also contribute directly to the plan. Queue slots
equal `worker_count * executor_queue_capacity`. M6 trace storage is
`trace_capacity * trace_slot_bytes`; an internal slot is larger than the
64-byte exported record because it contains atomic commit and producer-claim
state.

## Scratch ownership

Phase scratch has one aligned stride per registered phase. A phase receives
exactly `scratch_bytes`, its address is stable across frames, and no other
phase receives the same block. Contents are initialized during finalization
but are not cleared between invocations.

Every accepted graph, range, or reduction work item reserves one task-scratch
slot before it enters a worker queue. The slot:

- is aligned to `scratch_alignment`;
- exposes exactly `task_scratch_bytes` through `TaskContext::scratch()` or
  `rtfw_task_scratch()`;
- is exclusive to that callback, including while the callback synchronously
  submits and helps nested work;
- is released only after the callback returns.

This ownership rule prevents active parent, child, and grandchild contexts
from aliasing even when one worker helps all three. Task-scratch contents are
unspecified on entry and must not be retained after the callback returns.

Provider-backed storage preserves these payload sizes, strides, alignment,
lifetime, and non-aliasing rules. The runtime validates each provider usable
span before object construction. Checked stop first quiesces all execution
lanes, then destroys executor/trace objects and releases trace, task, and phase
tokens in reverse acquisition order. If rollback fails, checked stop retains
the owners and tokens and fails until a retry completes rollback. A
provider-backed trace ring is unavailable after its token is released.

## Bounded overload behavior

Queue reservation and scratch-slot reservation use bounded compare/exchange
attempts. Submission can therefore reject either physical exhaustion or
persistent contention; it never waits for capacity, allocates a spill record,
executes rejected work inline, or creates a helper thread.

| Condition | Status |
| --- | --- |
| Worker queue cannot accept the item | `queue_full` / `RTFW_STATUS_QUEUE_FULL` |
| No task-scratch slot can be reserved within the attempt bound | `scratch_exhausted` / `RTFW_STATUS_SCRATCH_EXHAUSTED` |
| No outstanding device slot or backend queue space | `device_queue_full` / `RTFW_STATUS_DEVICE_QUEUE_FULL` |

With `reject_submission`, a nested call returns the status to its caller. Any
accepted prefix completes before that call returns; the caller decides whether
to fail its phase. If root graph submission itself is rejected, the step
fails.

With `fail_frame`, the same status is returned and the active graph is also
marked failed. The step returns the overload status even if a user callback
ignores a failed nested submission. Already-running or accepted child work is
allowed to quiesce before `step()` returns.

`ExecutorStats::queue_full_rejections` and
`ExecutorStats::scratch_exhaustions` remain direct functional counters. M6 also
publishes them through the versioned metric schema without adding an RT-lane
allocation or output sink.

## Checkpoint and replay storage

Canonical state registrations are borrowed fixed byte spans. The runtime copies
only each stable identifier, schema version, pointer, and length. Registrations
with overlapping storage are rejected so restore destinations are independent.

The core C++ checkpoint and input-log codecs accept caller-provided buffers and
perform no heap allocation. The non-RT C input-log adapter may allocate a
capacity-bounded descriptor vector while validating C records. Output buffers
may not overlap registered state, input descriptors, or encoded input
payloads. A short buffer returns `capacity_exceeded` with
`required_bytes` and writes no partial artifact. Restore validates the entire
source before copying any state, so malformed or incompatible checkpoints
leave all registered bytes unchanged.

## Running-state boundary

M21-01 counts the copied device-rate binding/role storage and every compiled
phase, command, payload reference, timeline reference, backend/phase admission
row, and interval exactly once in `device_rate_plan_bytes`. That value is a
subcomponent of `rate_plan_bytes` and the existing `runtime_control_bytes`
logical extent. The six-row equation and provider regions do not change.
Temporary compiler inventories are discarded before publication; successful
finalization freezes all retained vectors. M21-02 additionally counts immutable
reference-to-device indexes, dependency offsets/entries, and preallocated
generation-tagged completion tickets once in `device_rate_execution_bytes`,
`rate_dispatch_state_bytes`, `rate_plan_bytes`, and the existing Runtime
control extent. The six-row equation and provider regions remain unchanged;
active provider, dispatch, completion, timeout, quarantine, and ticket reuse
perform no ordinary allocation.

M21-03 counts compiled device-endpoint descriptors, channel-to-endpoint
indexes, consumer slices, and the enlarged fixed channel/ticket state exactly
once in `rate_plan_bytes` and `runtime_control_bytes`. The
`cross_rate_device_endpoint_count` and `cross_rate_device_staging_bytes` fields
reconcile endpoint metadata and the existing Runtime-owned staging subset.
Pre-registered device-buffer envelope bytes are borrowed and are never added
to `planned_bytes`.

M21-04 counts sampled descriptor copies, initial/safe frames, direct maps,
safe-phase slices, and fixed status state once in the same categories. M21-05
adds `mixed_rate_action_capacity`, exact slot/storage bytes, and
`mixed_rate_replay_control_bytes`. The latter includes copied policy,
checkpoint state, replay decision/index state, and fixed digest/comparison
staging retained by Runtime. Observational capacity zero commits no action
slots; enabled active replay requires positive copied record and byte limits.
Caller-owned encoded artifacts and their explicit input payloads remain outside
`planned_bytes`.

M16-03 active state is allocated only during finalization. Admission records,
domain-release groups, channel aliases/generations, staged and committed
payload bytes, publication claims, and the canonical checkpoint-state buffer
are counted once in `rate_dispatch_state_bytes`, within `rate_plan_bytes` and
the existing runtime-control logical extent. `rate_checkpoint_state_bytes`
identifies the canonical record buffer without adding a planned row or provider
region. The six-row equation remains unchanged. Active dispatch, publish,
copy, skip, catch-up, hold, degrade, fail, summary inspection, and checked stop
use only that storage; allocation instrumentation covers on-time and late
degrade frames.

M16-04 adds the copied optional order, shed bitset/streak state, conditional
checkpoint tail, fixed rate-action ring/slots, counter bank, and policy
metadata to the same accounting. `rate_telemetry_bytes` and
`rate_policy_state_bytes` reconcile exactly once within `rate_plan_bytes` and
the runtime-control logical extent. The six-row equation and provider regions
remain unchanged. Zero telemetry capacity owns no slots but still records
publication drops in the fixed counter bank. Execute, shed, recover, loss,
inspection, and stop remain allocation-free after successful start.

M17-01 routes native HAL v2 and adapted device-ABI-v1 registrations into one
canonical device manager. A v1 adapter owns its copied v1 table, HAL v2 table,
context, and completion-translation scratch in address-stable storage. These
controls are included exactly once in `device_control_bytes` and enumerated as
device-owned logical extents. The estimator and constructed inventory must
agree on every byte; a missing, duplicate, overlapping, overflowing, or
estimate-mismatched adapter extent fails finalization before report
publication.

Configuring metadata remains in the existing runtime-control inventory, while
the finalized manager and adapter controls remain device-control. A borrowed
native instance, borrowed registered-buffer payload, and backend-reported
`control_storage_bytes` are not adopted as runtime-owned controls. The latter
remains the informational backend-control fact and is excluded from
`planned_bytes`.

M17-02 adds copied extension tables, the fixed 16-domain/32-node/64-link/
8-timestamp snapshot envelope, heterogeneous registration records, native
tokens, and translation/cleanup state to the same `device_control_bytes` term.
The estimator and constructed logical extents must again agree exactly. Host
spans and opaque/device allocations are not adopted; their checked declared
bytes are the informational registered-device-buffer fact. Domain maximums and
granularities bound registration but do not become payload allocation rows.
Core-only and adapted-v1 implicit-domain state is also fixed device control and
does not create a seventh plan row or fourth provider region.

No seventh plan row or fourth provider region is added. The six-row equation,
the exact M15 runtime/executor/device ledger, and the phase-scratch,
task-scratch, and trace-only provider boundary remain unchanged. Checked
adapter arithmetic and capacity rejection occur before provider acquisition,
native policy, thread creation, backend initialization, or callback execution.
After start, submission, early or polled completion, health, reset, cleanup
retry, and complete CPU-plus-device frames allocate no ordinary heap.

Before `start()` creates a thread, M15-03 applies and observes resident-memory
policy for phase scratch, task scratch, and trace storage. It then creates the
configured fixed worker team, an optional M5 watchdog lane, and one M8 device
service lane when backends exist. M17-03 additionally creates one submission
lane per opted-in command backend. After it returns, the
target CPU/device frame path uses preallocated graph, queue, scratch, trace,
outstanding, and completion storage. It contains no file I/O, blocking mutex,
hidden per-frame thread creation, heap fallback, or intentional heap
allocation. M6 trace producers make one atomic slot-claim attempt and drop on
contention rather than waiting. Watchdog waiting and device polling are
confined to runtime-owned service lanes. CPU device submission and completion
publication use fixed-capacity canonical HAL v2 manager state without waiting.
Workers use bounded queue operations and yield when idle or waiting for nested
work. M15-04 holds the startup barrier until requested runtime-stack
application and live observation are acceptable. On stop, stack cleanup
completes on each owning quiescent lane before join; unresolved cleanup blocks
control/selected-region rollback and provider-token release until checked
retry succeeds.

This boundary covers runtime and C-adapter code. User callbacks can still
allocate, block, perform I/O, submit to unrelated systems, or retain invalid
pointers; the runtime cannot make arbitrary host code real-time safe.

## Evidence

- plan, alignment, nested ownership, budget, and overload tests:
  `tests/test_memory_plan.cpp`;
- 64 complete frames under each executor policy with concurrent phases, range
  work, fixed-tree reductions, tracing, an armed watchdog, plus 64
  mock-device frames, under allocation instrumentation:
  `tests/test_trace_noalloc.cpp`;
- active admission/storage accounting and allocation-free dispatch:
  `tests/test_memory_plan.cpp`, `tests/test_rate_dispatch.cpp`,
  `tests/test_rate_telemetry.cpp`,
  `tests/test_trace_noalloc.cpp`;
- HAL v2 adapter/table/context accounting, overflow rejection, address
  stability, and allocation-free native/adapted device frames:
  `tests/test_hal_v2.cpp`, `tests/test_memory_plan.cpp`, and
  `tests/test_trace_noalloc.cpp`;
- heterogeneous snapshot/specification/token accounting, opaque logical-byte
  reporting, overflow/capacity rejection, and unchanged six-row reconciliation:
  `tests/test_heterogeneous_memory.cpp`, `tests/test_memory_plan.cpp`, and
  `tests/test_trace_noalloc.cpp`;
- C ABI plan, scratch, malformed discriminator, and reserved-field checks:
  `tests/test_cabi_dlopen.c`;
- canonical-state accounting, overlap rejection, allocation-free artifact
  writing/restoring, corrupt-input transactionality, and parser mutation tests:
  `tests/test_memory_plan.cpp`, `tests/test_trace_noalloc.cpp`,
  `tests/test_determinism_replay.cpp`;
- allocation/deallocation pairing:
  `rt/src/aligned_storage.hpp`;
- provider/native resident-region acquisition, observation, rollback, and
  release: `rt/src/memory_policy.cpp`, `tests/test_memory_policy.cpp`;
- implementation:
  `rt/src/host_runtime.cpp`, `rt/src/executor.cpp`,
  `rt/src/device_manager.cpp`.

## M17-03 fixed device storage

The device row counts copied command-extension state, timeline
descriptors/atomics, `device_outstanding_capacity` batch slots per opted-in
backend, batch completion scratch, submission-lane controls, and validation
state. `device_batch_backend_count`, `device_timeline_count`, and
`device_batch_queue_slots` expose exact cardinalities. Declared backend-private
command control bytes remain backend-reported bytes.

Submission stacks are reconciled once through the M15 runtime-stack row. The
six-row equation, twelve region identities, and three provider-backed regions
do not change. Checked multiplication rejects an unrepresentable slot plan,
and the logical device extent must equal its estimate before commit.

## M17-04 vendor-private storage

The bounded CUDA Graph registry, copied binding metadata, stream events, and
native batch slots, and the XDMA control/event queue, worker state, descriptors,
and stop-wakeup resources remain backend-private. Their fixed capacities are
reported through vendor capabilities where applicable and are not added to
Runtime-owned `device_control_bytes`. The six-row equation, twelve region
identities, three provider-backed regions, and runtime-stack accounting remain
unchanged.
# Extension control accounting

M19-01 fixed extension descriptors, handle maps, owner gates, relationships,
and lifecycle bookkeeping are counted exactly once in
`runtime_control_bytes`. DeviceManager storage created for extension
device-v1 backends remains in `device_control_bytes`. No seventh MemoryPlan
row or provider region exists. Registration may allocate only while
configuring; start, phase dispatch, service status, checked stop/retry,
inspection, detach, and stale-handle rejection do not allocate ordinary heap
memory.

## M17-06 plan preservation

The portable combined fixture finalizes with two device backends, two
heterogeneous buffers, two command-batch backends, and two timelines using the
existing six-row equation. Fixed simulated CUDA device storage, XDMA card and
control/event state, Graph bindings, call traces, and failure switches are
backend/sample-owned and create no Runtime storage row or provider region.
`device_control_bytes`, runtime-control accounting, twelve memory identities,
three provider-capable regions, and runtime-stack reconciliation retain their
existing formulas.

M26-03R simulator timing metadata is included in the existing size-based device
slot and compiled device-rate phase extents. The optional public policy adds no
steady-state allocation, runtime thread, implicit buffer or separate memory
region. Its actual control-storage increase is reported even when the policy is
absent; admission budgets and default configuration/artifact identities do not
encode this implementation layout.

## Per-backend command storage

Under the explicit native capacity policy, device_batch_queue_slots is the sum of resolved native command slots. device_control_bytes includes exactly that many actual BatchSlots; shared completion scratch remains bounded by the global completion batch. Existing global fields retain aggregate meaning. Default uniform allocation, object sizes and MemoryPlan remain unchanged.


## Sampled lifecycle and checkpoint recovery

Sampled stop retry bookkeeping occupies existing private Runtime padding; exact baseline MemoryPlan and checkpoint/active artifact bytes are regression-checked. Sampled provenance reconstruction uses bounded stack metadata and existing frame storage, without extra allocations.
