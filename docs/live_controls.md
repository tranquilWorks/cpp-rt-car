# Transactional live-control contract

M22-01 provides the additive C++ staging substrate inside `rt::Runtime`.
That retained M22-01 surface is data-only: no staged update is scheduled by
admission itself.
M22-02 consumes those complete records at exact host-frame and active compiled
rate-release boundaries. M22-03 optionally settles those publications as one
step transaction, restores the step-entry Runtime generation after execution
failure, emits a distinct payload-free action stream, checkpoints Runtime-owned
mailbox/generation state, and replays explicitly retained generations. Payloads
remain opaque canonical bytes and are never parsed or implicitly transferred.
M22-04 adds the optional installed `<rt/live_control.hpp>` source layer for
fixed application-owned typed data/config payloads. It does not replace or
hide any raw operation. See [live_control_sdk.md](live_control_sdk.md).

## Typed source layer

The M22-04 typed payload begins with one exact 32-byte canonical little-endian
envelope: `RTLC` magic, envelope version 1, complete encoded byte count,
positive application type and schema identities, one closed
`LiveControlUpdateKind`, and two zero reserved words. A positive compile-time
fixed application body follows. It is ordinary application payload content,
not a new Runtime checkpoint, replay, action, telemetry, profile, ABI, device,
extension, or HAL schema.

Applications specialize `LiveControlTypeTraits<T>` and encode individual
fields; copying a native C++ object representation is outside the contract.
`LiveControlTypedPayload<T>` remains caller-owned. Host/rate builders fill one
ordinary raw record and never call Runtime, retry, retarget, advance a producer
sequence, or own teardown. Decode validates raw kind/extent/digest and the
complete envelope/body before assigning caller output. The raw empty clear-
fault form remains valid; a fixed typed clear-fault payload is additive.

Semantic validation and variable-length parsing belong off lane. Runtime
callbacks may perform only bounded fixed-layout validation/copy: no allocation,
exceptions, text parsing, authentication, filesystem/network I/O, logging,
vendor calls, or implicit side effects. Runtime rollback still covers only its
immutable generation and never application/backend/external/physical effects.

## Configuration and ownership

`LiveControlPolicy` is copied once while Runtime is configuring. Its schema and
structure size are exact, reserved bytes are zero, the admission rule is
`reject_new`, and reset behavior is `discard_with_runtime`. A policy whose
identity and every capacity are zero disables the complete surface. An enabled
policy has a positive identity and positive bounded capacities no greater than:

| Resource | Absolute limit |
| --- | ---: |
| Mailboxes | 64 |
| Producers | 256 |
| Total records | 65,536 |
| Payload bytes per record | 65,536 |
| Total payload storage | 1 GiB |

Mailbox declarations assign a positive identity, record capacity, and payload
stride. Producer declarations assign a globally unique positive producer
identity, exactly one mailbox, and a positive initial sequence below
`UINT64_MAX`. Declarations are copied, duplicates and checked-arithmetic
overflow are rejected, and finalization fails transactionally if the complete
set is absent, malformed, over capacity, or over the Runtime memory budget.

Successful finalization allocates every mailbox control, record slot, payload
slot, producer sequence, counter, and compiled rate-target copy. It then issues
producer handles bound to a non-reused 64-bit Runtime identity, configuration
generation, mailbox, producer, and producer index. A handle from another
Runtime or generation cannot address local storage. A stopped Runtime is
terminal; a new Runtime receives a different identity and fresh storage.

## Canonical record

`LiveControlUpdateRecord` is exactly 128 bytes and contains only fixed-width
values. An input record has mailbox sequence zero. Runtime assigns a positive
mailbox sequence only after a successful reservation and complete payload copy.
The record carries:

- exact schema and record size;
- Runtime/configuration generation and mailbox/producer identities;
- positive expected producer sequence;
- host-frame or compiled-rate-release target identity;
- one closed update kind;
- exact payload byte count, alignment, FNV-1a 64-bit digest, and canonical
  little-endian policy flag; and
- zero reserved bytes.

A host-frame target supplies a finite frame index and leaves all rate fields at
their invalid sentinels. A rate target leaves the frame field at its sentinel
and exactly repeats one finalization-time reference release: reference index,
rate-domain registration index, phase index, domain release sequence, and
substep ordinal. This is one exact occurrence in the compiled reference
supercycle, not a recurring template. M22-02 marks a record missed after that
occurrence closes; it never retargets the record to a later supercycle.

The closed update kinds are scenario parameters, controller parameters, sensor
calibration, fault configuration, and clear fault. Only clear fault permits an
empty payload. The digest covers exactly the supplied bytes; callers compute
it with `live_control_payload_digest()`. Payload contents remain opaque. The
runtime performs no application parsing, authorization callback, signature
verification, file/network access, executable loading, or vendor call.

## Admission and publication

`stage_live_control_update()` is an explicit non-RT producer API. Runtime
callbacks cannot call it. It validates the handle, producer sequence, complete
record structure, target, and payload before attempting the mailbox claim.
The mailbox uses one atomic claim attempt; contention returns `busy` without a
mutex, wait, retry loop, allocation, alternate queue, or overwrite.

After a successful claim, Runtime rechecks admission and producer sequence,
then claims either an unused slot or one terminal slot that is not under
inspection. It copies the payload into the selected Runtime-owned stride,
zeroes unused bytes, writes the immutable record, and publishes the slot with
release ordering. Only then does it advance mailbox and producer sequences.
The boundary close and final publish use atomic state transitions: a producer
that wins publication before close is eligible, while a producer still writing
at close records `missed` after its complete copy. Neither side waits for the
other and no partial slot becomes visible.

Outcomes are distinct:

| Outcome | Meaning |
| --- | --- |
| `accepted` | One complete immutable slot was published. |
| `invalid` | Record, target, flags, reserved bytes, or payload structure failed. |
| `full` | Capacity was already committed; no existing slot changed. |
| `busy` | The single bounded claim attempt observed another producer. |
| `stale` | Handle ownership/generation or expected producer sequence disagreed. |
| `stopped` | Stop had closed new admission. |
| `exhausted` | A producer or mailbox sequence reached its no-wrap sentinel. |
| `missed` | The complete record's exact target had already closed. |

Foreign Runtime handles fail before touching a local mailbox. Structural
rejection and full/busy outcomes do not consume a record or advance either
sequence. A complete post-copy `missed` or `stopped` record does consume its
assigned monotonic sequences and remains inspectable until bounded reuse.
Full remains reject-new while no unused or safely reclaimable terminal slot is
available.

## Exact boundary close and immutable generations

Immediately before any callback for a host frame, Runtime atomically advances
that instance's monotonic frame cursor and scans every fixed slot once. Active
rate execution similarly closes one exact finalization-time reference release
immediately before its first CPU or device callback. Earlier closed targets are
terminally missed, future targets remain staged, and an empty boundary leaves
the prior data generation unchanged.

Current-boundary candidates are sorted by target identity, mailbox identity,
then mailbox sequence. Since one close has one exact target, the effective
cross-mailbox order is mailbox identity ascending and mailbox sequence
ascending. Within that order a later record replaces an earlier record only
for the same mailbox identity and update kind. The boundary copies survivors
and payloads into the inactive preallocated generation, hashes the exact target
plus ordered record identities and payload digests to a nonzero identity, and
release-publishes the complete generation. With closure disabled it then
terminalizes source slots as `committed` or `replaced`, preserving the exact
M22-02 path. With closure enabled the slots remain `boundary_owned` and
nonreclaimable until the owning step settles; no published generation
references a mailbox slot.

`CallbackContext::live_control` and `DeviceCallbackContext::live_control` are
nullable callback-lifetime views. They contain copied fixed record metadata and
read-only payload spans. They must not be retained after callback return.
Device callbacks receive host spans only; Runtime performs no implicit backend
transfer or vendor operation.

## Step transaction and rollback

`LiveControlClosurePolicy` is copied once while configuring. An all-zero policy
disables M22-03 without changing M22-02 execution, identity, memory, checkpoint,
telemetry, or package behavior. An enabled policy has one positive semantic
identity, the single `restore_step_entry_generation` rollback rule, a fixed
action capacity, fixed generation-journal record/payload capacities, fixed
artifact record/byte limits, and explicit replay enable.

At `step()` entry Runtime copies the currently active immutable generation into
a third preallocated generation store and opens one transaction. Every host or
active-rate publication in that step remains provisional. A successful step
terminalizes source slots as committed/replaced in canonical order and retains
the latest generation. Any non-success result after publication atomically
release-restores the step-entry generation, marks every provisional source slot
rolled back, and completes the transaction without allocation, waiting,
callback, vendor operation, or fallible copy. Nested step, periodic, replay,
checkpoint, restore, stop, or a second transaction cannot share this owner.

Rollback covers only Runtime-owned immutable generation, mailbox, action, and
checkpoint state. It cannot reverse state already mutated by an application
callback, backend, external process, or physical device. Applications remain
responsible for payload validation and their own registered-state/external
transaction boundary.

## Actions and retained generations

`LiveControlActionRecord` schema 1 is fixed at 256 bytes/alignment 8 and uses
closed action, stage, reason, and result tables. Its monotonic sequence binds
Runtime/configuration/policy identity, exact target, applicable mailbox and
producer sequences, update kind, payload digest/byte count, generation chain,
survivor/replacement counts, admission/record/terminal result, and exact
checkpoint/replay correlations. It contains no payload bytes, address,
callback, vendor handle, path, clock sample, or producer-thread identity.

The finalized direct-index ring records admission outcomes, empty/provisional
boundaries, committed, replaced, missed, stopped, rolled-back, checkpointed,
and replay results. A zero-capacity or contended slot causes an explicit drop;
reuse reports overwrite; runtime-bound cursors report exact gaps. Loss,
sequence exhaustion, or incomplete retention makes the affected transcript
replay-ineligible but does not block admission or execution.

Replay retention is a separate fixed journal. It copies only exact ordered
survivor records and payload bytes already published by M22-02. Default action,
trace, metric, and JSON paths remain payload-free. Retained payload bytes cross
the existing artifact trust boundary only when the caller explicitly requests
a live-control replay artifact.

## Checkpoint and replay

Closure-enabled ordinary schema-1 checkpoints append exactly one fixed
`rtfw.live-control` state record. It captures active/staged generation records
and payloads, source-slot terminal state, mailbox/producer sequences and
counters, boundary/rate cursors, and the next action position. Closure-disabled
checkpoints remain byte-identical. Export and restore make one bounded
all-mailbox/control claim, never wait for a producer, fully validate before
mutation, and restore artifact-local records under the current Runtime identity
and a new configuration generation. Existing producer handles become stale;
the current handle inspector resumes the restored producer sequence.

The distinct little-endian live-control replay artifact schema 1 embeds one
unchanged compatible checkpoint and exactly one unchanged input-log schema-1 or
active-replay schema-1 artifact. It also carries a complete gap-free action
range, retained generation descriptors, explicit ordered records/payloads,
section checksums, and a whole checksum under copied record/byte bounds and the
1 GiB absolute ceiling. A short output reports the exact required size and
writes nothing.

Inspection and Runtime replay validate complete extents, identities, reserved
fields, ordering, generation/rollback chain, record replacement, payload
digests, checksums, and nested artifacts before restore. Replay restores the
checkpoint once, bypasses external producer timing, injects only validated
immutable generations at their exact frame/rate boundaries, executes the
unchanged nested path, and compares terminal status, progress, and final
registered application state. On divergence the result reports the exact
frame, boundary target, action sequence, and generation identity reached. It
preserves the existing deterministic-mock backend restriction and never claims
to reproduce physical arrivals, arbitrary application side effects, or a
vendor driver.

## Inspection, identity, and accounting

Runtime exposes mailbox counts, one fixed mailbox-info snapshot, retained
record lookup by mailbox sequence, and exact-size copying into caller-owned
payload storage. `LiveControlCommitInfo` reports the latest published identity
and target, survivor count, terminal counters, and staged occupancy.
`LiveControlRecordStatusInfo` reports staged, committed, replaced, missed,
stopped, or rolled back for a retained record. Reclaimed record history is
unavailable; the separate action cursor owns bounded outcome history.
Inspection does not advance execution and leaves caller output unchanged when
the requested record is unavailable.

Frozen schema/legal tables, policy, capacities, admission/reset rules, sorted
declarations, exact-boundary ordering, replacement, missed, reclamation, and
view-layout semantics participate in graph, configuration, and replay
compatibility identity. Runtime identity, caller addresses, payload contents,
arrivals, current generation, occupancy, counters, and inspection calls do not.

`MemoryPlan` reports mailbox/producer counts, total record capacity, exact
payload bytes, and the complete live-control heap contribution. Policy and
declaration storage, mailbox controls, atomics/counters, immutable record
slots, payload slots, producer state, copied compiled-rate targets, candidate
indexes, terminal state, boundary cursors, inspection state, two publication
generations, the third rollback generation, provisional records, action slots/
counters, retention storage, and fixed checkpoint scratch are included once in
`runtime_control_bytes`, `planned_bytes`, and the exact logical control-extent
ledger. Caller-owned replay artifacts are excluded. No
provider-backed memory region or execution lane is added.

## Lifecycle and deferred behavior

Admission may occur after finalization, before or during running execution,
from bounded external producer threads. It is not signal-handler or interrupt
safe. `step()` and active mixed-rate dispatch close boundaries and expose only
the immutable generation view. Existing sampled-I/O, device completion,
checkpoint codec, input-log, active-replay, rate/mixed-rate action, global
telemetry, watchdog, and stable state registry formats remain unchanged.

`stop()` closes admission before checking active execution and existing cleanup
ownership. A producer that already won the claim may finish its bounded copy;
the first stop attempt reports `invalid_state` and must be retried after that
claim quiesces. A new claim observes stopped. Once execution is quiescent,
remaining staged records become terminal `stopped`. Retained records never
carry into another Runtime generation.

This establishes portable RT0 Runtime-generation rollback, payload-free action
telemetry, conditional Runtime-owned checkpoint state, and explicit trusted-
artifact generation replay in addition to staging and exact publication. It is
not evidence for arbitrary application/backend/physical side-effect rollback,
physical control, HIL, controlled latency, RT1/RT2, executable or Unreal hot
reload, support promotion, release, deployment, or production readiness.


## Nested active replay continuation limitation (2026-09-23)

M23-03R repairs free-before-terminal slot selection and distinguishes retained
history capacity from simultaneous mailbox capacity. Canonical state and action
comparison remain enabled. Version-1 replay history does not retain replaced
payload bytes, and rejected-admission actions omit the mailbox association needed
to restore its counters. Nested active replay can therefore still reject these
histories with `incompatible_artifact` even when application bytes and surviving
generations match. Do not treat matching application bytes as replay acceptance.
See `tests/package_consumer/live_control_replay_consumer.cpp` and the retained
M23-03R evidence. Format evolution is a separate scope; this repair changes no
public header, ABI, serializer or artifact version.

## Opt-in lossless trusted replay (format v2)

`LiveControlReplayRetentionPolicy` is a distinct additive 48-byte C++ policy.
Call `set_live_control_replay_retention_policy()` after enabling the closure's
replay policy and before finalization. Set a nonzero `policy_identity`,
`admission_capacity`, and `payload_capacity_bytes`; all reserved bytes must be
zero. The setter copies the policy, rejects repeated/late configuration, and
freezes identity and capacities into compatibility IDs. Existing policy layouts,
format-v1 constants, C/device/extension ABIs and default v1 writing are unchanged.

```cpp
rt::LiveControlReplayRetentionPolicy retention;
retention.policy_identity = 0x1002;
retention.admission_capacity = 128;
retention.payload_capacity_bytes = 4096;
const auto status = runtime.set_live_control_replay_retention_policy(retention);
```

V2 retains each admission's canonical original record, action sequence, owner
and producer identities, counter attribution, assigned slot when present, and
outcome. It copies the original payload for every admission that acquired a slot,
including records later replaced, rolled back, missed or stopped, and records
still pending at export. Rejected input payloads are not copied. A foreign stale
handle has no owner-counter effect. An attempt during an exclusive checkpoint,
export or restore returns effect-free `busy`; it creates neither a mailbox change
nor a transcript entry. Ordinary per-mailbox contention retains its owner and
counter effect. Existing telemetry remains payload-free.

This opt-in changes data retention: **non-survivor payloads are retained** in the
trusted artifact. The application controls which canonical synthetic or sensitive
values it supplies and who can receive exported artifacts. Default telemetry is
not a substitute for this explicit trusted export. Redact at the application
boundary by selecting safe input values; altering journal sections requires a
new internally consistent artifact and is not a supported partial-history replay.
Keep borrowed callback/backend state alive through checked stop and all accepted
work. Retained storage lives with the Runtime and is released at teardown; replay
restore resets journal cursors without freeing live borrowed state.

All storage is preallocated at finalization and included in MemoryPlan and storage
extents: admission descriptors, copied payloads, export order and checkpoint-sized
validation scratch. No callback allocation, new thread or blocking wait is added.
The ceilings are 262144 admissions and 1 GiB of admission payloads, further bounded
by the Runtime memory budget and the existing 1 GiB complete-artifact limit.
Exhaustion or a lost action makes the history explicitly replay-ineligible;
normal execution continues, but export cannot report a successful partial bundle.

### Binary layout

All new numeric fields use little-endian encoding. V2 has magic `RTFWLCR2`, schema
2 and a 448-byte header. Existing checkpoint, nested-artifact, 264-byte action,
128-byte generation, 152-byte retained-record and survivor-payload sections keep
their encodings; the header grows and appends admission descriptors and payloads.
V1 remains `RTFWLCR1`, schema 1, with its exact 384-byte header.

| Header byte | Width | V2 field |
| --- | --- | --- |
| 384 | 8 | Retention policy identity |
| 392 | 8 | Admission descriptor offset |
| 400 | 4 | Admission count |
| 404 | 4 | Descriptor stride, 184 |
| 408 | 8 | Admission payload offset |
| 416 | 8 | Admission payload byte count |
| 424 | 8 | FNV-1a checksum of both admission sections |
| 432 | 8 | Admission-close action cursor, or UINT64_MAX if still open |
| 440 | 8 | Reserved zero |

| Admission byte | Width | Field |
| --- | --- | --- |
| 0 | 128 | Original record, with instance runtime/generation fields zeroed |
| 128 | 8 | Corresponding admission action sequence |
| 136 / 144 | 8 each | Counter-owner mailbox / producer identity |
| 152 | 4 | Original slot, or UINT32_MAX for no acquired slot |
| 156 / 157 | 1 each | Outcome / whether an owner counter changed |
| 158 | 2 | Reserved zero |
| 160 | 8 | Relative admission payload offset |
| 168 | 4 | Copied payload bytes |
| 172 | 4 | Reserved zero |
| 176 | 8 | FNV-1a over descriptor bytes 0–175 and its copied payload |

Sections are contiguous, extents and arithmetic are checked, and descriptors are
ordered by action sequence. Zero padding, checksums, payload digests, action
association, capacities, policy/topology identity and checkpoint binding are
validated before restore. The existing whole-artifact checksum includes the new
header and sections. Checksums detect corruption; they are not authentication.

Ownership validation uses finalized scratch under the exclusive host claim. It
rejects slot collisions, foreign owners, duplicate order and impossible terminal
transitions before canonical state or callbacks can change. Replay reconstructs
original slots, bytes and counters at action positions and uses normal boundary
settlement. The existing `final_state_hash` field means the complete canonical
Runtime state for v2, including mailbox/producer/generation state; v1 retains its
legacy application-state meaning. V2 always checks that hash, even when it is zero,
and preserves transcript and nested active-replay comparisons. Application or
backend side effects are not rolled back by the control journal.

Complete v1 histories remain readable. Missing replacement payloads or rejection
ownership cannot be recovered from old v1 artifacts; unsupported incomplete
histories are rejected before restore. V2 does not infer payload bytes from hashes
or repair counters to match an expected final hash. No hardware or RT qualification
follows from replay success.
