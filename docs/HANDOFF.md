# Handoff

## M23-03 Runtime benchmark work in progress

M23-02 discovery closure merged as PR #253 at
`795a56cabb78dd2c6ccf51796f54c4f6eff84f9f`. The active M23-03 contract
is bound to portfolio-control PR #367, revision
`38a7c91d8c3fe7a663a57d0353a3e60773ded20d`.

The draft adds 78 public Runtime benchmark descriptors, implementation, CLI
registration and source-example consumers. Standalone fixture checks cover
CPU/device rates, cross-rate payloads, controls, checkpoints, replay, watchdog
and telemetry. Four combined live-control/active-replay cases remain failing.
A small public-only reproducer returns `incompatible_artifact` with application
state, callback count, generation count and accepted-admission count all correct.
The complete Runtime replay-state comparison requires a separately scoped repair;
no Runtime implementation or schema has been changed in this benchmark batch.
The proposed repair scope is [portfolio-control PR #369](https://github.com/tranquilWorks/portfolio-control/pull/369); it remains a draft requiring approval.

Local package, ABI and contract checks pass. Quick CTest is 33/33; strict full
CTest is 36/37, with an additional unresolved failure in unchanged
`PredictiveAdaptive.PrestepsReduceReactiveCatchup`. No full-profile pass is claimed.

The owner temporarily waived unavailable hosted CI/CD on 2026-09-08 because of
utilization limits. This does not waive functional verification. M23-03 and
M23/CAP-M23 remain incomplete. Follow the exact failed/unperformed results and
remaining acceptance work in [M23-03 evidence](evidence/M23-03-2026-09-08.md).

## Historical M23-02 closure

M23-02 source merged as PR #252 at
`543af2b4728a171243e6d756554efa188c2e7d2f`. The original planning baseline
remains merged PR #251 at `b52b42a6c64066553ce55fb08c874061d78e036e`.
Control PR #366 merged at `7549d5d647fada8fad7637779301226c3ad87296`, adding
`tests/benchmark_fixtures/test_artifacts.py` to the approved scope. The local
then-active contract bound that canonical revision/blob without rewriting its
original planning baseline.

Read [benchmarking](benchmarking.md), the preserved
[M23-02 evidence](evidence/M23-02-2026-09-07.md), and the new
[discovery closure](evidence/M23-02-discovery-closure-2026-09-07.md).
The CLI regression now expects the exact sorted self-plus-51-CPU inventory;
malformed input, destination preservation, privacy, and self-provider checks
remain enforced. PR #252's final CI run passed the Windows optional SDK and
cross-platform artifact exchange but failed the obsolete list assertion in
nine matrix jobs. Keep those historical failures; follow-up CI is authoritative
for the correction.

PR #253 closed that correction after all 32 hosted checks passed. M23-03 is now
activated and bound to the integrated canonical contract described above. M23-04/M23-05, M24 CUDA maturity, M25 SDK and
M26 golden system remain future work. M18 hardware/RT, M19 Unreal and
signing/release/deployment remain separate gates.

## Restart context

RTFW remains 1.2.1 at RT0 with C ABI v8, 70 exports and SONAME 8. The following
M22 notes describe the merged foundation, not the active candidate.

## M22-04 implementation handoff

Include `<rt/live_control.hpp>` only when the typed helper is wanted. Specialize
`rt::LiveControlTypeTraits<T>` with positive application type/schema identities,
one closed update kind, a positive fixed encoded extent, and fixed-span
`noexcept` validate/encode/decode functions. The SDK requires a standard-layout,
trivially-copyable, nothrow output type; codecs still must explicitly encode
fields and must not copy native object representation, padding, pointers,
dynamic containers, borrowed views, paths, handles, callbacks, or clocks.

`LiveControlTypedPayload<T>` is caller-owned. The host/rate builders validate
before assigning the raw output record and preserve the exact producer handle,
sequence, target fields, alignment, canonical flag, extent, and digest. They do
not call Runtime. The caller invokes `stage_live_control_update` directly and
owns every returned `Status`, `LiveControlAdmissionResult`, retry/correction,
sequence, lifetime, and checked-stop decision. Decode validates raw kind and
extent, digest, the complete 32-byte envelope, type/schema identity, and the
fixed body before assigning output.

Semantic validation and all variable-length text/config parsing remain off
Runtime lanes. A callback may only perform bounded fixed-layout decode/copy;
it must not allocate, throw, log, authenticate, access files/networks, call a
vendor, or attribute application/backend/physical side-effect rollback to
Runtime. The raw M22-01 through M22-03 API remains directly usable, including
the empty clear-fault form; a typed clear-fault envelope is additive.

The sample and stress suite cover the approved domain types, exact host/rate
targets, commit/replacement/rollback/action inspection, absolute limits,
maximum payload, concurrency/loss, repeated lifecycle, multiple instances,
package relocation, and checked teardown. See `docs/live_control_sdk.md` and
`docs/live_controls.md`. This is portable RT0 data/config behavior only, not
executable/shared-library/Unreal reload, physical control, HIL, controlled
latency, RT1/RT2, release, deployment, or production evidence.

## M22-03 implementation handoff

`LiveControlPolicy` is copied once while configuring. A completely zero-capacity
policy disables the feature without changing compatibility identity; an
enabled policy has positive bounded mailbox, producer, record, per-record
payload, and total storage capacities. Mailboxes and producers are copied
before finalization. Each producer belongs to one mailbox and receives a
Runtime-identity/configuration-generation handle only after finalization.

`LiveControlUpdateRecord` is a fixed 128-byte envelope. Host-frame targets use
only `target_frame_index`; rate targets must repeat one exact compiled
reference-release index, domain-registration index, phase index, release
sequence, and substep. Payload size, FNV-1a digest, alignment, canonical
little-endian flag, enum values, reserved bytes, producer sequence, ownership,
and generation are structurally validated before a reservation attempt.
Accepted payloads are copied and unused slot bytes zeroed before release
publication. Full mailboxes reject new records without overwrite. Contention
returns `busy`; stale, stopped, exhausted, invalid, full, and accepted remain
distinct inspectable outcomes.

The admission API is for non-RT producer threads and does not allocate, invoke
callbacks, call vendors, perform I/O, parse application payloads, or spin for a
slot. Inspection returns copied records and exact-size payload copies, never an
internal pointer. `stop()` closes new admission first and requires a checked
retry if one bounded claim is still active; Runtime remains terminal after
successful stop. Configuration and finalized mailbox controls are included in
the existing Runtime-control MemoryPlan row and logical extent ledger. Frozen
policy/declarations affect graph/config/replay identity; arrivals and payloads
do not.

The frame owner closes one monotonically increasing frame target before graph
dispatch. Active rate dispatch closes one exact finalization-time reference
release before its callback. It never waits on a producer: atomic slot state
decides eligible versus missed. Candidates sort by mailbox identity then
mailbox sequence; later same-mailbox/update-kind records replace earlier ones.
Survivors are copied to the inactive generation, hashed with their target and
identities, and release-published before callbacks receive a read-only view.

Terminal committed, replaced, missed, and stopped slots are reusable only
outside bounded inspectors; generations own independent record/payload copies,
so callbacks never reference reclaimable slots.

`LiveControlClosurePolicy` is copied once while configuring. The all-zero
policy preserves the exact M22-02 path. An enabled policy freezes a semantic
identity, action capacity, rollback rule, retention limits, artifact bounds,
and optional replay. `step()` snapshots the current immutable generation into
a third preallocated store. Exact-boundary publications remain provisional;
success commits/replaces source slots, while any non-success step result
restores the complete prior Runtime generation and marks the provisional source
slots rolled back without allocation, waiting, callback, or fallible cleanup.

The fixed action ring carries identities, targets, digests, counts, and closed
outcomes but never payload bytes. Loss and cursor gaps are explicit and make a
range replay-ineligible. Checkpoints conditionally append one fixed
`rtfw.live-control` record and restore current Runtime identities while retiring
old producer handles. The separate replay artifact contains the unchanged
checkpoint and nested execution artifact plus a complete correlated action
range and explicitly trusted retained survivor payloads. Inspection allocates
nothing, complete validation precedes restore, and replay injects only exact
immutable generations. Runtime rollback does not undo application, backend,
external-process, or physical-device effects. M22-04 owns typed SDK examples
and stress closure. See `docs/live_controls.md` for the complete contract.

## M21-05 implementation handoff

`MixedRateClosurePolicy` is configuring-only and freezes bounded action and
replay capacities plus its semantic policy identity. Action capacity is
observational and may be zero; active replay requires positive transcript and
byte limits and an explicitly deterministic backend. The 256-byte action
records and runtime-bound cursors are distinct from rate-action schema 1 and
global observability schema 2. Any action gap, overwrite, or drop makes the
captured range replay-ineligible.

Closure-enabled checkpoints append `rtfw.mixed-rate` as a normal schema-1
state record. Export and active replay require quiescence. The active artifact
is its own fixed little-endian schema and must be fully validated before
restore; legacy `Runtime::replay()` keeps its active-plan rejection. Replay
uses recorded logical decisions, suppresses live watchdog timing, permits only
deterministic mock/loopback backends, re-executes providers, and compares the
complete generated action and content-identity sequence.

The public loopback logical-action log is fixed capacity and instance-local;
reset is allowed only after shutdown. The conformance fixture includes only
installed headers and is shared by repository and relocated-package tests.
Keep caller replay payloads borrowed/explicit, never add sampled payload bytes
to default telemetry, and retain exact MemoryPlan accounting. M21/CAP-M21 may
be called software-complete only after this candidate merges; physical and RT
qualification remain separate.

## M21-03 implementation handoff

Use `CrossRateDeviceEndpointSelector` only for the device side of a
CPU→device or device→CPU channel. Its phase-local payload-reference ordinal and
positive stride are copied and frozen. Finalization derives the backend,
buffer, envelope, role, host access, timestamp domain, and in-flight slot count
from M21-01; it rejects device→device, implicit matching, incoherent/opaque
memory, overlapping envelopes, and partial selectors transactionally.

CPU input is copied before the provider runs. The provider still returns the
exact frozen declaration; Runtime alone materializes the selected slot offset
and payload byte count. Device output is copied and published only after exact
terminal success while the M21-02 slot remains host-owned. Failure, timeout,
cancel, malformed completion, loss, skip, and shed paths do not call the output
publisher. Channel-free M21-02 and ordinary M17 batches retain exact-reference
behavior.

M21-04 owns sampled-I/O descriptors, clocks/triggers, safe outputs, and
overrun/underrun policy. M21-05 consumes those semantics without changing
their HAL or artifact versions. Do not describe this portable fake-driver path
as physical device, DAC/DAQ, HIL, RT1, or RT2 evidence.

## M21-02 implementation handoff

M21-01's admitted model remains the only source of backend, command, buffer,
timeline, completion-budget, and capacity identity. M21-02 adds immutable
reference-to-device and dependency slices plus preallocated completion tickets.
Do not derive execution identity from provider output or rescan configuring
registries after start.

An active device provider receives `DeviceCallbackContext::rate_release`, runs
once on the selected executor path, and enqueues a complete copied batch into
the existing per-backend submission lane. It does not call a vendor entry or
wait for completion. Independent records in one release group may overlap;
precomputed device dependencies and the group terminal barrier prevent a
dependent CPU/device record or successful step from advancing early.

Submitted timeouts are settled once by the existing service lane and retain
their slot in quarantine through checked backend shutdown. The exact batch and
release identity prevents a late completion from settling a reused slot.
Ordinary M17 batches retain executor-token completion behavior. M21-02 added no
new telemetry schema, replay encoding, lane, or hardware/RT qualification and
remains the ownership foundation consumed by M21-03.

## M20-PRE-01 implementation handoff

Use `scripts/verify-portable-assurance.sh` with an explicit build directory and
complete source commit. The five modes are `dependencies`, `static`, `fuzz`,
`artifacts`, and cumulative `all`. Source manifests, seed manifests,
dictionaries, dependency/action pins, the Clang 14 policy, SPDX schema, public
fixture trust snapshot, and artifact policy are checked-in inputs. Generated
reports and candidates must stay below the explicit build/CI evidence root.

The package candidate is staged twice to prove byte-stable SBOM generation,
then bound to an unsigned in-toto statement and a strict expected-source final
manifest before extraction and relocated consumption. The signed fixture is
fictional non-target material used only to prove offline cryptographic positive
and mutation rejection. No RTFW candidate is signed or authenticated.

Do not broaden this batch into production source, ABI or schema changes,
support/qualification promotion, a release workflow, target signing,
continuous-service fuzzing, controlled performance, soak, hardware, RT, or
Unreal work. Missing Clang 14/CMake/libFuzzer tooling is an unperformed gate,
not a pass. Hosted CI and separate human supply-chain, fuzz/analyzer,
compatibility, and claim review remain mandatory before merge.

## M19-01 implementation handoff

`rt/extension_abi.h` is the independently versioned C11 ABI v1. Runtime takes
only an already-resolved entry pointer and transactionally stages fixed CPU
phases, device-v1 backends, services, resources, and local relationships.
Generational owner/kind/slot handles reject failed, foreign, wrong-kind, stale,
and detached uses. The full ABI layout, copied/borrowed matrix, lifecycle,
control-thread rule, retry order, accounting, identity, evolution, and trust
boundary are in `docs/extension_registration.md`.

Do not add a loader or release extension code until `stop()` and
`detach_extension()` both succeed. M19-03 owns Unreal and host module unload
orchestration. M19 and CAP-M19 remain incomplete. M18 remains unpromoted.

## Implemented boundary

M17-04 keeps the CUDA and XDMA `api()` device-ABI-v1 candidate paths exact and
adds `hal_v2_registration(std::string_view) noexcept` accessors backed by the
existing HAL core v2, memory/topology v1, and command/timeline v1 contracts.
Each candidate object is borrowed through one selected path until checked
shutdown succeeds. Driver tables preserve their version-1 aggregate prefixes
and defaults; exact version 2 adds CUDA Graph launch or XDMA control/event/stop
callbacks only when the complete required configuration is present.

CUDA native-v2 uses an explicit staged domain, one configured command stream,
and fixed completion events. It accepts at most 16 caller-owned pre-instantiated
graph handles, unique 16-bit nonzero IDs, and eight copied bindings per graph.
`0x43470000 | graph_id` is the stable dispatch identity. Batch copies, kernels,
and graphs execute in order; partial enqueue, timeout, event failure, loss,
reset, and stop retain every possibly referenced owner until safe drain.

XDMA native-v2 retains H2C/C2H and adds bounded 32-bit little-endian control
read/write plus one finite user-event wait. Stable opcodes encode the checked
word offset or event index. A configured aperture is at most 262144 bytes and
there are at most 16 events. The Linux adapter copies the optional paths,
opens them close-on-exec, wakes event waits through an idempotent stop request,
and never closes a descriptor while a fixed I/O worker may reference it.

Backend-private storage remains fixed and reported through existing capability
bytes. Runtime-owned command storage, submission/service lanes, six MemoryPlan
rows, three provider regions, schemas, installed inventory, release, support,
and license remain unchanged.

## Protected decisions

- Preserve C ABI v8, 70 exports/fingerprint, SONAME 8, device ABI v1, HAL core
  v2, memory extension v1, command extension v1, Runtime statuses, and schemas.
- Preserve exact device-ABI-v1 CUDA/XDMA behavior and conditional identity when
  native registration is unused.
- Keep vendor work off executor workers and within the existing Runtime
  submission/service lanes or fixed XDMA I/O team.
- Never infer graph ownership, CUDA pinning/device memory/coherency, a safe FPGA
  register map, interrupt latency, driver cancellation, or direct peer DMA.
- Keep cross-backend timelines, direct peer paths, device-rate payloads and
  sampled I/O, physical combined qualification, support promotion, release,
  and deployment deferred.

## M17-06 implementation handoff

The sole production repair is in `rt/src/command_batch.cpp`: capability
discovery retains the value-initialized size/version prefix instead of
overwriting it with zero. Complete output validation and existing statuses are
unchanged. `tests/test_command_batch.cpp` observes the exact incoming header
and zero tail and proves every malformed/error/exception attempt publishes
nothing before a corrected retry. `tests/test_vendor_hal_v2.cpp` registers the
actual CUDA and XDMA native candidates together through canonical Runtime and
reaches checked stop.

`samples/cpu_gpu_fpga_cpu.cpp` and its standalone focused test use only the
existing public Runtime/CUDA/XDMA targets. The five-phase graph is CPU prepare,
simulated CUDA upload/Graph/download, disjoint host-stage bridge, simulated
XDMA H2C/control/event/C2H, and CPU validate. Storage is fixed, timelines are
backend-local, complete steps are measured allocation-free, and exact
operation/thread/causality counts cover two frames plus CUDA failure, XDMA
event timeout/cancellation, malformed declarations, correction, isolation,
and unresolved-only cleanup retry.

Retain the M17-05 failed review unchanged; M17-06 is the approved repair, not a
rewrite of that evidence. The M17-06 retained evidence must distinguish local
results from mandatory hosted CI and human review. No physical Graph, device
memory, pinning, coherency, DMA, MMIO, event interrupt, timing, HIL, field,
RT1/RT2, Unreal, signing, release, deployment, or production claim follows.
Exact acceptance and validation results are in
`docs/evidence/M17-06-2026-08-23.md`.

## M18-01 implementation handoff

`qualification/schemas/` contains independent version-1 plan, record, review,
and proposal schemas. `tools/qualification.py` uses only Python 3.11 standard
library facilities and performs strict bounded parsing, exact-byte plan/record/
review binding, canonical embedded-manifest hashing, complete trial and
threshold comparison, artifact-tree verification, and atomic non-overwriting
proposal output. `tests/qualification_fixtures/` contains only synthetic data.

The precise bounds, canonicalization, evolution, review, commands, rollback,
and claim boundary are in `docs/qualification.md`. Existing M12 CUDA/XDMA
`evidence_only` records remain raw inputs only. The tool does not mutate a
support matrix and does not authenticate reviewer attribution or prove plan
chronology. The unchanged M18-01 tool still rejects non-synthetic combined
promotion; a later approved qualification-policy batch owns any revision.

After local verification, mandatory GitHub CI and human schema, security,
qualification, compatibility, and claim-boundary review remain external merge
gates. No physical campaign or support promotion belongs to M18-01.
