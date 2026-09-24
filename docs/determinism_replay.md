# Determinism, Checkpoints, and Replay

M22-03 preserves checkpoint codec, input-log, mixed-rate-action, and active-
replay schemas and bytes. Closure-disabled checkpoints retain the exact M22-02
path. Closure-enabled checkpoints append one ordinary fixed
`rtfw.live-control` state record that captures Runtime-owned active/staged
records, payloads, slot state, boundary cursors, mailbox/producer sequences,
and the next action position. Restore fully validates before mutation, binds
records to the current Runtime with a new configuration generation, and retires
old producer handles.

M22-04 typed payloads are application content inside those unchanged retained
payload sections. Their separate 32-byte `RTLC` envelope and fixed body use
canonical little-endian field encoding and the existing raw payload digest.
No checkpoint, input-log, action, replay, observability, profile, ABI, or HAL
schema is extended. Native object representation, padding, addresses, implicit
clocks, and host endianness are never replay identity.

The distinct live-control replay schema 1 embeds that unchanged checkpoint and
one unchanged input-log or active-replay artifact, plus a complete payload-free
action transcript and explicitly retained survivor records/payloads. Parsing is
allocation-free and bounded; exact section and whole checksums, identities,
ordering, generation/rollback chains, and payload digests are validated before
restore. Replay restores once, injects immutable generations at exact targets,
executes the unchanged nested path, and compares actions plus final registered
application state. Divergence reports the exact frame, boundary target, action
sequence, and generation identity. External producer timing, live clocks,
physical arrivals, application-side-effect rollback, and vendor-driver replay
are not deterministic inputs.

M21-05 preserves checkpoint and input-log schema 1 byte-for-byte and adds a
distinct additive active-replay schema 1 for mixed-rate execution. A closure-
enabled checkpoint conditionally carries one ordinary mixed-rate state record;
active replay binds that checkpoint to a complete logical-action transcript
and explicit caller-owned inputs for deterministic mock/loopback re-execution.

RTFW 1.2 retains milestone M7 for the target `rt::Runtime` path. It provides
an explicit D1 contract for registered state, a stable little-endian
checkpoint format, a stable input-log format, and bounded checkpoint replay.
It does not qualify arbitrary application code as deterministic, and it does
not establish D2 or D3.

## Supported tiers

| Tier | 1.2 behavior |
| --- | --- |
| D0 — unspecified | Supported. Checkpoints require an exact resolved configuration identity. No schedule-independent state claim is made. |
| D1 — schedule-independent | Supported as an explicit application contract. The same binary, ISA, semantic configuration, seed, input log, and registered-state schema must produce identical registered state across supported worker counts. |
| D2 — reproducible build profile | Reserved and rejected by configuration. The repository has no qualified compiler/dependency/hardware profile. |
| D3 — portable deterministic | Reserved and rejected by configuration. Arbitrary floating-point callbacks are outside this tier. |

Selecting D1 also requires the static deterministic executor and disables the
timing-driven frame watchdog. Throughput scheduling and watchdog degradation
are rejected because their scheduling or elapsed-time observations can change
callback-visible state.

## Registered-state boundary

The runtime snapshots only storage explicitly registered with
`Runtime::register_state()` or `rtfw_register_state()`. Each registration has:

- a unique 1–63 byte stable name from `A-Za-z0-9._:/@-`;
- a nonzero application schema version;
- a non-empty, fixed-size, caller-owned byte region.

The storage remains borrowed through runtime destruction. Registration freezes
at finalization, duplicate names and overlapping storage regions are rejected,
and artifact output buffers cannot alias registered state, input records, or
input payloads. Checkpoint and replay calls reject an active frame, periodic
loop, or replay operation.

Registered bytes are an interchange representation, not an invitation to
register native C++ object layouts. Applications must encode integers,
fixed-point values, and explicitly approved floating-point representations in
a canonical form. `rt::store_u32_le`, `rt::store_u64_le`,
`rt::load_u32_le`, and `rt::load_u64_le` are provided for fixed-width
little-endian fields. Compiler padding, pointers, container internals, mutexes,
atomics, and process-local handles must not be registered.

D1 applies only to these registered bytes. Application callbacks remain
responsible for all of the following:

- declaring every shared resource access so unordered conflicting phases are
  rejected;
- avoiding races and execution-order-dependent writes;
- deriving random values from stable seeds and counters;
- avoiding wall clocks, process-global mutable state, device completion order,
  unspecified iteration order, and unapproved math behavior;
- treating queue or scratch exhaustion as a failed qualification run.

## Stable identities

Finalization computes four independent identities:

| Identity | Contents and use |
| --- | --- |
| `config_id` | Complete resolved configuration, including worker count and operational capacities; preserved as provenance |
| `graph_id` | Callback/resource names, dependency edges, resource-access declarations, and any explicit rate-domain/binding semantics in registration order |
| `state_schema_id` | Registered state names, schema versions, sizes, and order |
| `replay_id` | Compatibility identity used by restore and replay |

For D0, `replay_id` includes `config_id`, so a worker-count change is
incompatible. For D1, `replay_id` excludes worker count and operational
capacities but includes callback-visible numerical/scratch/overload choices,
the graph, state schema, and workload ID. This is what permits a valid D1
checkpoint to transfer between supported worker counts without discarding its
original `config_id`.

The build ID and complete runtime version are stored as provenance. D1 restore
requires the same runtime major version but does not treat build ID equality as
a D2 claim.

M16-01 conditionally appends copied domain name, period, substeps, deadline,
budget/WCET, criticality, optionality, and normalized phase/domain binding
indexes to `graph_id`. Runtime-owner tokens, pointers, storage capacities,
floating point, wall clocks, and scheduling are excluded. With no explicit
rate model, the pre-M16 graph/replay hash path is byte-for-byte unchanged.
Checkpoint and input-log schema 1 need no new fields because their existing
graph/replay identities already reject a semantically different plan before
state mutation.

M16-02 conditionally appends channel name, normalized producer/consumer phase
indexes, mode, payload size, maximum age, and every copied initial-sample byte
to `graph_id`. The marker is absent when no channel is declared, preserving the
exact M16-01 graph/replay identity. Selection records are a deterministic
derivative of already-hashed graph/rate/channel inputs and are not separately
hashed.

M16-03 conditionally appends the active execution bound and every late-action
and catch-up field to graph identity. The marker is absent for reference-only
plans, preserving their exact M16-01/M16-02 identity and artifact bytes. Active
execution is D0 only. It contributes one reserved canonical generic state
record containing the logical cursor, epoch mapping, degradation/fault state,
channel generations/aliases, and committed payload bytes. That record changes
state-schema/replay identity and participates in the existing state hash; the
checkpoint codec and schema number remain unchanged. Restore validates the
complete internal record before changing any application state, then rebuilds
the live stores without allocation.

Schema-1 input-log records have no nominal release or action decision fields.
`write_input_log()` and `replay()` therefore reject active execution explicitly;
reference-only input logs remain unchanged. M16-04 conditionally appends host
policy version, late/on-time thresholds, and deterministic optional shedding
order to active graph identity. Telemetry capacity is excluded because loss
cannot affect dispatch. Its canonical generic state tail transactionally
retains thresholds, streaks, optional order, shed bitset, and policy version;
mandatory-only state bytes remain exact. The separate rate-action telemetry
history, counters, and caller cursors are process-local and not checkpointed.
That legacy rate-action history remains observational and is not inferred as
D1 input. M21-05 active replay instead uses its separate complete mixed-rate
action transcript.

M21-01 conditionally appends the device-rate phase/domain indexes, completion
budget, per-phase in-flight bound, and ordered payload roles to `graph_id` only
when a device-rate binding exists. Backend, batch, buffer, and timeline
identity already come from the copied M17 graph semantics and remain the sole
source. No-device and CPU-only conditional hash paths are unchanged. A mixed
metadata change therefore changes graph/replay compatibility without changing
checkpoint or input-log schema 1. M21-02 alone remains D0 and does not encode
device terminal order, timeout decisions, or payload in existing schema-1
artifacts.

M21-03 additionally hashes each explicit endpoint selector and derived
direction, backend/buffer/reference identity, envelope, stride, slot count,
and completion timestamp domain. Raw host addresses, provider pointers,
payload contents after configuration, completion arrival order, and wall-clock
measurements remain excluded. Device payload bytes and completion metadata are
not added to checkpoint/input-log schema 1. M21-05 hashes its semantic policy
into compatibility identity, while excluding observational action capacity,
payload content, addresses, caller cursor position, arrival order, and wall-
clock observations.

## Mixed-rate checkpoint and active replay schema 1

When `MixedRateClosurePolicy` is enabled, checkpoint export appends one
`rtfw.mixed-rate` ordinary state record. At a quiescent release boundary it
captures the canonical active cursor, next logical generation/action sequence,
shedding and degradation state, sampled counters/status/safety/freshness, and
the replay-action position. It contains no in-flight vendor owner, action
history, caller cursor, backend token, address, or wall-clock value. Disabled
checkpoints remain byte-identical to their pre-M21-05 form.

The active artifact is fixed-width little-endian with a 384-byte header,
direct-index input descriptors, fixed 256-byte action records, per-record
content checksums, and one whole-artifact checksum. Its copied policy bounds
record count and byte count below the existing 1 GiB absolute ceiling. It
binds runtime/build/workload, determinism tier, graph/replay/state schema,
policy, checkpoint frame, first/last frame, nominal epoch, ordered explicit
inputs, and the complete gap-free action transcript. Inspection validates all
encoded lengths, identity, checksums, reserved fields, sequence/order, and
trailing extent without allocating from encoded values.

`Runtime::replay_active()` requires a compatible quiescent checkpoint, no
telemetry loss, and only backends whose frozen capabilities report
deterministic-mock behavior. Complete artifact and checkpoint validation occurs
before restore. Replay invokes the input callback once per recorded frame,
uses recorded nominal time and legal rate/watchdog decisions, re-executes the
provider and mock/loopback completion path, and compares each generated action,
payload/frame digest, sampled metadata, safety state, terminal status, and the
final registered-state hash. The first mismatch returns exact progress;
already completed frames are not rolled back. Nondeterministic or physical
backends cannot enter this mode.

Existing `Runtime::replay()` retains its non-active behavior and active-plan
rejection. Existing checkpoint/input-log magic, versions, layouts, and parser
behavior are unchanged.

M17-01 preserves the exact pre-M17 device identity path for every adapted
device-ABI-v1 registration. Its backend name and copied backend identifier are
hashed at the same positions as before; the adapter kind, HAL table, context,
addresses, and control-storage capacities append no bytes. An otherwise
identical v1 configuration therefore retains its exact `graph_id`, `replay_id`,
checkpoint bytes, input-log bytes, and artifact compatibility after it begins
traversing the adapter.

A native HAL v2 registration cannot impersonate that legacy path. For native
v2 only, graph identity conditionally appends an M17 backend-kind marker and
HAL API version 2 in addition to the existing name/backend identifier
semantics. Backend order remains registration order. This separation changes
no checkpoint or input-log field: the existing schema-1 identities reject a
native/adapted mismatch before state mutation. Equivalent native-v2 and
adapted-v1 execution may produce the same application state while remaining
intentionally artifact-incompatible.

M17-02 keeps that permanent distinction. An adapted-v1 or core-only native-v2
backend using only the legacy buffer registration contributes no new memory,
topology, timestamp, or extension marker beyond its M17-01 path. A native
memory/topology extension conditionally hashes the semantic snapshot in stable
domain, node, link, and timestamp order, including the completion-domain
selection. Each explicit heterogeneous-memory declaration hashes its backend,
domain identity, logical bytes, ownership, access, coherency,
synchronization, and declared opaque-handle bytes. Instance pointers, callback
addresses, host addresses, backend-private tokens, and unused opaque tail bytes
never enter compatibility identity.

These additions change no artifact field or schema. A schema-1 checkpoint or
input log carries the resulting existing graph/replay identity and therefore
rejects a heterogeneous semantic mismatch before mutation. Timestamp samples
and correlations are observations and are not checkpointed or replayed.

## Checkpoint format v1

Checkpoint artifacts use fixed-width little-endian fields:

- an 8-byte magic and schema-v1 256-byte header;
- runtime version, determinism tier, build/workload IDs, `config_id`,
  `replay_id`, `graph_id`, and `state_schema_id`;
- checkpoint frame index, state count, exact payload/total sizes, registered
  state hash, and whole-artifact checksum;
- one 88-byte record header per state containing the fixed-capacity name,
  schema version, payload size, and payload checksum;
- the canonical state payload immediately after each record header.

FNV-1a-64 checksums detect accidental corruption and stable-artifact drift.
They are not authentication or a defense against a malicious party capable of
recomputing checksums. Untrusted artifacts still require the application's
normal trust boundary.

`inspect_checkpoint_artifact()` validates the complete format without
allocating. Runtime restore additionally validates the frozen runtime
identities, registration order, every name/version/size, and input/output
non-overlap. It performs a full validation pass before copying any byte, so a
malformed, corrupt, truncated, oversized, foreign, or incompatible checkpoint
cannot partially mutate registered state.

`snapshot_max_bytes` is frozen at finalization and cannot exceed the 1 GiB
format safety limit. The runtime never allocates from an encoded length.
Checkpoint output is caller-owned; a short buffer returns
`capacity_exceeded` with the exact required size and writes nothing.

## Input-log format v1

An input log is produced from caller-owned `ReplayInputRecord` entries. Each
entry contains:

- a strictly increasing frame index;
- a nonnegative simulation delta and optional absolute deadline;
- a fixed-width application input type;
- a bounded caller-owned canonical payload.

The 192-byte log header carries runtime version, tier, workload,
`replay_id`, `state_schema_id`, record count, first/last frame, exact sizes,
and a whole-artifact checksum. Each 48-byte record header carries frame fields,
payload size, flags, and a payload checksum.

`replay_input_capacity` and `input_log_max_bytes` bound construction and
parsing. Input-log writers validate every record and compute the complete size
before writing. The C++ codec, inspectors, and replay parsing do not allocate.
The C ABI writer first stages fixed record descriptors in a
`replay_input_capacity`-bounded vector so it can validate C structures before
entering the codec; that non-RT adapter step may return
`resource_exhausted`, but it never allocates from an encoded artifact length.

## Replay sequence and failure semantics

`Runtime::replay()` and `rtfw_replay()` require a running runtime and perform
the following sequence:

1. validate the complete checkpoint;
2. validate the complete input log;
3. verify both artifacts against each other and the finalized runtime;
4. require the first logged frame to follow the checkpoint frame;
5. transactionally restore registered state;
6. invoke the host input callback once per record;
7. synchronously call `step()` with that record's frame context;
8. report processed records, replayed frames, and the final registered-state
   hash.

Malformed or incompatible input is rejected before checkpoint restore. Once a
valid replay begins, an application input callback or frame callback can still
fail; successful earlier replay frames are not rolled back. Applications that
need retry semantics should retain the original checkpoint and invoke replay
again after correcting the callback failure.

All checkpoint, inspection, input-log, hashing, and replay-control calls are
non-RT host operations. Runtime frame callbacks remain allocation-free under
the M4 gate; replay does not turn artifact parsing or host input application
into an RT-lane operation.

## Evidence

The M7 gates include:

- target-runtime state equality across 1, 2, and 4 workers;
- D1 checkpoint transfer between different worker counts;
- checkpoint plus input-log reproduction of the final registered-state hash;
- corrupt/foreign schema and graph rejection with unchanged destination state;
- validation of the input log before restore;
- 5,000 deterministic malformed-parser mutations in the normal test suite;
- a Clang libFuzzer/ASan/UBSan parser target with a bounded CI run;
- allocation instrumentation around checkpoint write, inspect, restore, and
  input-log write;
- stable C ABI v8 dynamic-loader coverage and C/C++ embedding samples;
- exact adapted-v1 pre-M17 identity/artifact compatibility and native-v2
  kind/version separation in `tests/test_determinism_replay.cpp`;
- active canonical-state round trip, corruption transactionality, and explicit
  active input-log/replay rejection in `tests/test_rate_dispatch.cpp`;
- CI artifact exchange that compares checkpoint bytes produced by GCC/Clang
  and FMA-on/FMA-off builds.

The exchanged artifact is a narrow integer-only compatibility fixture. Its
equality demonstrates stable encoding for that workload; it does not establish
D2 for arbitrary callbacks or floating-point kernels.

## Legacy boundary

`SimCore::saveFrame()`, the demo snapshot wrapper, and
`SnapshotWriter`/`SnapshotReader` remain compatibility surfaces outside the
target M7 format. The legacy reader now bounds every access and refuses an
encoded vector length that does not fit the supplied input, and the demo
enforces a 64 MiB file policy. Those changes prevent the prior out-of-bounds
and attacker-sized allocation paths, but legacy native-layout artifacts are
not portable checkpoint-v1 artifacts and do not inherit the M7 D1 claim.

## M17-03 conditional compatibility identity

An opted-in command extension contributes fixed capabilities. Timeline name,
backend ordinal, and initial value contribute conditionally. Each batch-phase
declaration contributes ordered command kind, operation, opcode, flags,
logical buffer indices/ranges/access, and wait/signal timeline indices.

Runtime addresses, callback pointers, mutable queue indices, generated batch
IDs, dynamic payload/timeout/point values, timeline progress, completion
status/timestamp, and health do not contribute. Configurations without the
extension retain the M17-02 identity path. Checkpoint/input-log schema 1 and
rate-action schema 1 remain unchanged; action replay remains unsupported.

## M17-04 vendor command identity

Native vendor capability and copied memory/topology declarations enter through
the existing conditional M17-02 identity path. The M17-03 batch declaration
then records the stable CUDA Graph opcode and ordered logical bindings, or the
stable XDMA control offset/event opcode and logical result reference. Native
handles, driver pointers, file descriptors, host addresses, timestamps,
health, and mutable queue state remain excluded. Selecting the unchanged
device-ABI-v1 paths therefore retains the byte-exact M17-03 identity path.
# Extension semantic identity

Configuration, graph, and replay identity conditionally include copied
extension name/version, negotiated ABI, phase/backend/resource semantic names
and capabilities, service names/interface versions, and local relationships.
They exclude callback and instance addresses, module handles, provisional
handles, owner/generation values, mutable lifecycle state, and observed status
counters. With no extension registered, all existing identity values are
unchanged.

## M17-06 deterministic composition evidence

Two consecutive sample frames derive fixed integer inputs from the frame index,
apply fixed CUDA and XDMA transforms, and verify every final element. Exact
operation order, call counts, Graph ID/bindings, control/event values, accepted-
before-completed timeline values, and trace causality are asserted without
pointer-derived identity or elapsed-time thresholds. Both timelines remain
backend-local and monotonically increasing.

M17-06 adds no checkpoint, input-log, rate-action, or compatibility schema and
no replay action. Runtime-generated batch IDs, native handles, host pointers,
thread IDs, failure switches, timeline progress, and simulated device state do
not enter compatibility identity.

### Explicit lossless live-control retention

The optional `LiveControlReplayRetentionPolicy` selects the separate trusted v2
format described in [live controls](live_controls.md#opt-in-lossless-trusted-replay-format-v2).
It retains original non-survivor payloads and admission ownership within finalized
bounds. The default v1 artifact and existing public structure layouts remain
unchanged. V2 validates ownership before restore and compares complete canonical
state plus transcripts; malformed, foreign or incomplete histories fail explicitly.
The format does not roll back application/backend side effects or qualify hardware.
