# Proposed M26-04R: opt-in four-slot sampled-I/O storage

Status: proposal only; owner decision pending. No Runtime repair implemented or
activated. M26-04 remains incomplete; M26-05/M26-06 remain inactive.

## Reproduction and boundary

Frozen `samples/golden_system/contract.json` declares `ring_slots: 4` for all
five scenario channels. The current M21 sampled compiler rejects ring capacity
greater than `rt::cross_rate_snapshot_slot_count`, which is 2, and additionally
requires exact equality with the compiled cross-rate store. Cross-rate storage
is hard-coded to two slots at sizing, descriptor construction and allocation.

The preserved feature probe compiles and returns invalid_argument at finalize.
The retained same-graph diagnostic requests four slots (exit1, rejected) then
two (exit0, finalized), without starting Runtime: both report zero native driver
initializations/events. The two-slot diagnostic is not golden execution or
acceptance evidence. Both original failures and diagnostic source are retained
in `M26-04-ring-blocker/`. No prior production/source-kit/test was changed.

## Concrete proposed scope

Use the existing public `SampledIoChannelRegistration::ring_capacity` as the
explicit opt-in, supporting 4 in addition to the existing accepted 2. Preserve
the default two-slot constant, ordinary CPU channels, public structure layouts,
C ABI v8/70 exports/fingerprint, SONAME8, device ABIv1, artifact schemas, native
backends and all earlier source/tests/evidence. No generic dynamic capacity or
unbounded storage. No sample-only fake fourth slot or scenario downgrade.

Likely implementation is confined to private cross-rate compilation/storage and
sampled validation, plus Runtime finalization plumbing. Validate requested
channel handles, duplicates and supported bounds before allocating; pass bounded
per-channel slot counts into the existing variable-sized SnapshotStore. Use the
same count in aggregate capacity checks, compiled descriptors, actual allocation,
MemoryPlan and identity/accounting. The existing sampled identity already hashes
ring_capacity; verify it rather than adding an undocumented artifact rewrite.
No steady-state allocation. No default behavior, timeout or safe-ACK relaxation.
Public header comments/docs may clarify the existing field without changing its
layout. Default source identity manifests/generated docs may need separately
listed current-hash refreshes; historical evidence remains immutable.

## Required repair evidence

- Old-order negative control: the new four-slot public-API regression fails
  against the retained baseline and passes only with the repair.
- Existing two-slot/default and ordinary CPU behavior/identity/artifact bytes
  remain unchanged; invalid 0/1/3/5 and over-capacity requests reject before
  start, with retryable configuration and no leaked provider ownership.
- Genuine four-slot descriptor/store/MemoryPlan counts and exact payload bytes;
  repeated wraparound, producer/consumer selection, sampled underflow/overrun,
  safe transitions and lifetime behavior through actual device endpoints.
- Originating-owner replay, paired checkpoint recovery and foreign 2-versus-4
  topology rejection before effects; no hidden identity or sequence rewriting.
- No-allocation, isolation, saturation, lifecycle and sanitizer/TSan coverage;
  public installed SDK consumer and relocated package, full ABI/default-v1 gates.
- Contract/quick/full and all32 exact-head hosted records, risk review and guarded
  tested-tree merge. A separately merged canonical repair must precede any
  forbidden Runtime edit. Then canonically reactivate and finish M26-04.

## Feature work still required after repair

The current new source is only the first compile/finalize prototype. Native
safe transitions and nominal scenario execution have not run. Complete numerical
and metadata parity, CUDA kernel/Graph combined variants, independent host jobs,
external CIL, all frozen/fault/retention paths, originating-owner replay and
fresh-owner recovery, allocation/ownership checks, CLI/artifact validator,
installation/provenance/package/process tests, all local/hosted gates, target
integration and canonical closure. Physical/human/RT/Unreal/performance/release
gates remain separate.
