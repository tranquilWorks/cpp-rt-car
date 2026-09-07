# RTFW Completion Roadmap

M21 and M22 are portable-software complete through merged PR #250. M23-01 merged as PR #251 and M23-02 source merged as PR #252.
The M23-02 CLI discovery correction and CI closure are active. Software feature completion remains
M26-06, not M22 or the presence of a benchmark framework.

The roadmap converts the [product contract](product_contract.md) into
dependency-ordered milestones. A milestone is complete only when its exit gates
pass; file presence or a passing smoke test is not sufficient.

## Status legend

- **Complete** — deliverables and exit gates are represented in the repository.
- **Candidate** — implementation and portable tests exist, but a milestone
  requiring external hardware evidence has not passed its qualification gates.
- **Next** — next dependency-ordered implementation batch.
- **Planned** — blocked on earlier milestones.
- **Qualified separately** — not part of portable 1.0.

## Milestones

| ID | Status | Outcome |
| --- | --- | --- |
| M0 | Complete | Product contract, truthful documentation, versioning, and license |
| M1 | Complete | Runtime lifecycle, typed configuration, host-driven API, C ABI draft |
| M2 | Complete | Compiled graph, cycle rejection, and resource hazards |
| M3 | Complete | Unified CPU executor with honest selectable policies |
| M4 | Complete | Memory plan and zero-allocation RT-lane closure |
| M5 | Complete | Self-paced absolute cadence, watchdog, and platform preflight |
| M6 | Complete | RT-safe, versioned observability |
| M7 | Complete | Determinism tiers, safe snapshots, and replay |
| M8 | Complete | Device ABI and deterministic fault-injectable mock |
| M9 | Candidate | Real CUDA backend; hardware qualification pending |
| M10 | Candidate | Xilinx Linux XDMA AXI-MM backend; hardware qualification pending |
| M11 | Complete | Stable ABI, package/export cleanup, and engine adapters |
| M12 | Complete | Portable 1.0 release contract; deployment qualification remains separate |
| M13 | Complete | Strict runtime profiles and operational target-runtime autotune integration |
| M14 | Complete | Professional SDK target, package boundary, and consumer-isolation contract |
| M14.1 | Complete | Recoverable device initialization and teardown safety closure |
| M15 | Complete | M15-01 through M15-04 merged with mandatory CI; CPU/memory policy, accounting, and rollback closure retained |
| M16 | Complete | M16-01 through M16-04 are merged in target history; retained evidence preserves the original gate record without inventing absent review identifiers |
| M17 | Qualification incomplete | M17-01 through M17-04 and M17-06 are merged; the portable software path exists, while physical CUDA/XDMA/combined and RT qualification remain absent |
| M18 | Planned | M18-01 offline schemas/tools implemented; named NVIDIA, XDMA, combined, RT1, and optional RT2 campaigns remain |
| M19 | In progress | M19-01 size-versioned extension registration implemented; engine adapters and Unreal lifecycle remain |
| M20 | In progress | M20-PRE-01 is merged with deterministic host-independent fuzz, static, dependency, candidate-SBOM/provenance, strict-manifest, offline-fixture, and relocated-package assurance without signing or release; CAP-M20 remains open |
| M21 | Complete | M21-01 through M21-05 are merged with fixed logical actions, conditional checkpoint state, deterministic active replay, and reusable public-surface conformance; physical/RT qualification remains separate |
| M22 | Complete | M22-01 through M22-04 merged; typed controls, replay and stress closure at portable RT0 |
| M23 | In progress | M23-01 and M23-02 source merged; M23-02 discovery/CI closure active; remaining providers/comparison policy belong to M23-03 through M23-05 |
| M24 | Planned | Runtime-integrated CUDA physics, CPU oracle, bounded failure/lifetime and capability coverage |
| M25 | Planned | Consumer SDK, backend kit, external CIL, independent host adapter and installed docs |
| M26 | Planned | Golden full-system scenario, fault/benchmark/replay/documentation and final software audit |

## M0 — Product contract and truth reset

Deliverables:

- accepted product contract, RT/determinism tiers, lifecycle, support matrix,
  non-goals, and release gates;
- accepted ADRs for executor, time ownership, and device boundaries;
- README and supporting docs that distinguish implemented, experimental, and
  planned behavior;
- Apache-2.0 license;
- one repository version source;
- automated documentation/claim checks.

Exit gates:

- every runnable README command is exercised by CI;
- no present-tense README claim describes an absent CUDA/XDMA/config feature;
- no unqualified latency, determinism, or production-readiness guarantee;
- version and license checks pass.

## M1 — Lifecycle, configuration, and host API

Delivered in 0.2:

- `rt::Runtime` with explicit configure/finalize/start/step/stop states;
- strict typed configuration with callback, scratch, trace, and numerical
  policy behavior;
- a synchronous host-driven callback path with runtime-local time, trace,
  numerical helper, and scratch ownership;
- a size/version-checked experimental C ABI;
- executable C and C++ callback samples and dynamic-library coverage.

Exit gates:

- explicit configure/finalize/start/run/stop state machine;
- unknown configuration and CLI keys fail;
- every schema key maps to runtime behavior;
- host-driven steps never sleep;
- two runtime instances do not share clock, trace, numerical, or scratch
  state;
- C and C++ samples register and execute real user callbacks.

## M2 — Compiled graph

Delivered in 0.3:

- instance-owned phase and logical-resource handles across the C++ and C APIs;
- explicit dependency and read/write resource declarations;
- deterministic finalization-time topological compilation;
- pre-start cycle, invalid/foreign-handle, and unordered resource-conflict
  diagnostics;
- immutable compiled topology consumed by the synchronous host executor;
- randomized DAG/reference-executor and first-frame allocation evidence.

Exit gates:

- cycles, invalid handles, and conflicting resource access are diagnosed before
  start;
- graph topology is immutable after finalization;
- randomized DAG tests agree with a reference executor;
- no topology allocation occurs on the first frame.

## M3 — Unified executor

Delivered in 0.4:

- one runtime-owned fixed worker team for graph phases and nested CPU work;
- `static_deterministic` precomputed phase assignment and
  `bounded_throughput` per-worker queues with real successful-steal counters;
- synchronous nested range work and fixed-tree deterministic reductions;
- bounded queue submission with an explicit `queue_full` result;
- phase-local scratch isolation for concurrent callbacks;
- fixed thread creation in `start()` and joining in `stop()`, with no
  emergency, detached, or inline-bypass execution path in the unified
  executor.

Exit gates:

- independent phases with nested range/reduction work pass stress and TSAN;
- queue-full submission returns within a tested bound;
- no emergency/detached execution path;
- static policy produces stable assignment metadata;
- throughput policy demonstrates real local execution and successful steals.

## M4 — Memory closure

Delivered in 0.5:

- a finalization-time budget and inspectable immutable memory plan covering
  target-runtime control structures, queues, aligned phase/task scratch, and
  the trace ring;
- exclusive scratch slots reserved before graph/range/reduction work is
  accepted and retained through callback completion and nested helping;
- explicit `reject_submission` and `fail_frame` overload policies for bounded
  queue and scratch reservation failure;
- matching aligned allocation/deallocation ownership;
- a multi-frame allocation gate covering concurrent phases, nested range work,
  fixed-tree reductions, and tracing after start.

Exit gates:

- a complete finalized CPU frame performs zero heap allocation on RT lanes;
- no RT-lane file I/O, hidden thread creation, condition-variable wait, or
  blocking mutex;
- overflow policy is explicit and identical in Debug and Release;
- every execution context has valid scratch storage;
- allocation origin and deallocation are paired.

## M5 — Time and platform

Delivered in 0.6:

- a finite self-paced C++/C loop using epoch-based absolute releases and
  explicit relative deadlines;
- per-frame release, wake, start, finish, slack, miss, watchdog, and
  degradation results;
- a one-shot watchdog service lane that never invokes host code, plus
  frame-thread application of capped runtime degradation state;
- disabled-by-default, read-only strict Linux preflight covering the runtime
  clock, PREEMPT_RT, memory lock limit/current locked memory, isolated
  affinity, and realtime scheduling;
- fixed-capacity inspectable failure reports, deterministic fake-clock/probe
  tests, C ABI v4 coverage, and an armed-watchdog allocation gate.

Exit gates:

- fake-clock tests prove exact release/deadline behavior;
- self-paced mode uses absolute releases;
- one watchdog arm produces at most one event;
- degradation is applied on the frame thread;
- strict platform preflight explains and rejects unmet prerequisites.

The precise surface and qualification boundary are in
[the time/platform contract](time_platform.md).

## M6 — Observability

Delivered in 0.7:

- a fixed-width schema-v1 runtime trace with monotonic sequence numbers,
  nonblocking slot claims, and explicit overwrite/drop accounting;
- caller-owned, runtime-bound trace cursors with exact retained-window loss
  reporting and bounded caller-provided output;
- 22 stable counter/gauge definitions with cumulative and independent
  cursor-based interval windows;
- build, runtime-version, configuration, workload, and runtime-instance
  provenance on every snapshot/read;
- experimental C ABI v5 access and a non-RT JSON export helper;
- isolation, window-partition, loss, contention, C ABI, no-allocation, and
  ThreadSanitizer evidence.

Exit gates:

- production trace/counter emission preserves the M4 RT-lane gate;
- metric window semantics are invariant-tested;
- two runtime instances have isolated traces;
- output schemas include version/build/config/workload identifiers.

The exact schema, cursor behavior, and legacy boundary are in
[the observability contract](observability.md).

## M7 — Determinism and replay

Delivered in 0.8:

- explicit D0/D1 configuration with D2/D3 rejected until their evidence
  profiles exist;
- fixed-size caller-owned canonical state registration, frozen graph/state
  schema identities, and worker-count-independent D1 replay compatibility;
- versioned little-endian checkpoint and input-log formats with complete
  bounds, reserved-field, identity, and checksum validation;
- allocation-free inspectors and codecs that never allocate from encoded
  lengths, plus transactional registered-byte restore;
- checkpoint-plus-input replay through the existing synchronous `step()` path,
  with validation-before-restore and a final registered-state hash;
- C ABI v6, embedding samples, deterministic mutation tests, libFuzzer smoke,
  and compiler-exchanged artifact comparison.

Exit gates:

- D1 workloads match across supported worker counts;
- cross-compiler claims compare exchanged artifacts;
- snapshot fuzzing finds no crash or unbounded allocation;
- checkpoint plus input log reproduces registered state.

The supported tier boundary, exact artifact behavior, callback obligations, and
legacy exclusions are in
[the determinism/replay contract](determinism_replay.md).

## M8 — Device ABI and mock

Delivered in 0.9:

- C-compatible size/versioned backend ABI for capabilities, initialization,
  registered buffers, bounded submit/poll, cancel, health, reset, and shutdown;
- compiled device phases whose providers return without waiting while a
  runtime-owned completion lane retains and releases graph dependency tokens;
- preallocated outstanding/completion storage in the finalized memory plan;
- stable device statuses, schema-v2 trace/metrics, and experimental C ABI v7;
- deterministic fixed-capacity CPU mock with scripted delay, timeout, error,
  loss, saturation, reset, and shutdown behavior;
- C++ sample, dynamic C ABI backend test, steady-frame allocation gate, and
  sanitizer-focused device coverage.

Exit gates:

- bounded queue saturation, delay, timeout, error, loss, reset, and shutdown
  pass through the mock;
- CPU compute workers never block on a device;
- backend callbacks cannot outlive runtime/plugin ownership.

The exact ABI, ownership, scheduling, fault, and qualification boundaries are
in [the device backend contract](device_backend.md).

## M9 — CUDA backend candidate

Delivered in 0.10:

- an optional CUDA Driver API adapter around a caller-owned context and fixed
  caller-owned stream set;
- fixed-capacity event, submission, registered-buffer, external-device-buffer,
  and kernel registries;
- setup-time host pinning and device-mirror allocation with explicit external
  ownership alternatives;
- async host/device and device/device copies, byte fill, and fixed-payload
  kernel launch;
- nonblocking event-query completion, timeout quarantine until physical
  completion, enqueue-failure quarantine, soft reset, context-loss mapping,
  and drain-before-release shutdown;
- CPU-only fake-driver functional, concurrency, runtime-integration,
  no-allocation, sanitizer, and recovery tests;
- an opt-in real-GPU functional/latency evidence executable and self-hosted
  workflow;
- a schema-v1 support matrix with no qualified tuple.

Remaining exit gates:

- compile and run the adapter against a named CUDA toolkit/driver/GPU/OS
  tuple;
- publish repeated functional, steady-state resource, failure/recovery, and
  raw latency-decomposition evidence;
- select thresholds before the run and review the result;
- add the passing tuple to the versioned support matrix.

M9 remains Candidate until those hardware gates pass. The exact ownership,
timeout, evidence, and claim boundaries are in
[the CUDA contract](cuda_backend.md).

## M10 — XDMA backend candidate

Delivered in 0.11:

- a portable, injectable XDMA state machine implementing device ABI version 1;
- bounded registered-buffer and transfer-slot tables with fixed initialization-
  time I/O workers;
- nonblocking, allocation-free submit/poll paths that never invoke a character-
  device transfer on runtime CPU or completion lanes;
- validated H2C/C2H buffer access, channel, range, device offset, transfer
  limit, and power-of-two alignment;
- timeout quarantine until a running blocking driver transfer physically
  returns, plus stable health, soft reset, device-loss, and retryable shutdown
  behavior;
- an opt-in Linux adapter for official Xilinx XDMA AXI-MM character devices,
  anchored to one named upstream driver and example-design-compatible
  bitstream contract;
- portable fake-driver functional, saturation, concurrency, timeout, recovery,
  no-allocation, sanitizer, and ThreadSanitizer evidence;
- an opt-in destructive H2C/C2H integrity and raw-latency evidence executable,
  self-hosted workflow, and schema-v1 empty support matrix.

Remaining exit gates:

- compile and run against a declared x86-64 Linux, XDMA driver, PCI function,
  FPGA part, XDMA IP configuration, and bitstream tuple;
- publish repeated integrity, steady-state resource, saturation, timeout,
  device-loss, reset/rebind, and shutdown evidence;
- select latency thresholds before measurement and retain raw submit, poll, and
  completion-wait samples;
- review the evidence and add only a passing tuple to the versioned support
  matrix.

M10 remains Candidate until those hardware gates pass. Character-device
blocking and kernel page-pin/allocation behavior prevent any inherited RT1 or
RT2 claim. The exact contract is in
[the XDMA backend contract](xdma_backend.md).

## M11 — Stable distribution and engine adapters

Delivered in 0.12:

- C ABI v8 as the first stable binary boundary, including current/minimum
  compatibility metadata, a reviewed public-surface fingerprint, and a typed
  incompatibility status;
- hidden internal visibility, an exact checked-in C export allowlist,
  ABI-numbered ELF SONAME, and Linux dynamic-symbol verification;
- CMake package components for shared C, static C, C++ runtime, and optional
  CUDA/XDMA targets with pre-1.0 same-minor package-version compatibility;
- clean shared/static/C++ consumers configured against a relocated install tree
  on Linux and Windows;
- a real `host_adapter` executor policy exposed in C++ and stable C, using a
  borrowed capacity-matched host job system, runtime-owned scratch,
  generation-tagged completion contexts, bounded overload, and nested-work
  helping;
- functional, saturation, stale-completion, no-allocation, dynamic ABI, and
  ThreadSanitizer gates.

Exit gates:

- header/library ABI mismatch fails before runtime creation;
- the built shared-library export set equals the reviewed allowlist;
- no runtime CPU worker is created for the host-adapter policy;
- accepted host jobs complete exactly once without stale-token reuse;
- shared C, static C, and C++ imported targets configure and run after install
  relocation.

The exact promise and change rules are in
[the stable C ABI contract](c_abi.md) and
[the executor contract](executor.md).

## M12 — Portable release and deployment-qualification boundary

Delivered in 1.0:

- an explicit 1.x SemVer, deprecation, stable-surface, support-level, security,
  and release policy;
- a machine-readable portable support matrix naming Ubuntu 22.04 GCC 11,
  Ubuntu 22.04 Clang 14, and Windows Server 2022 MSVC v143 as supported RT0
  tuples;
- same-major CMake package compatibility while preserving C ABI v8 as the
  independent stable binary boundary and explicitly declining a C++ binary
  ABI;
- a dedicated concurrent two-runtime gate proving independent device backends,
  borrowed buffers, metrics, health, and shutdown state;
- deterministic release-contract validation with reviewed SHA-256 identities
  for stable headers, ABI manifests, licensing, support policy, package
  configuration, release tools, and required workflows;
- full-commit pins for every third-party workflow action, including the
  release publisher's artifact download;
- CPack install archives consumed directly from fresh extracted prefixes,
  content-addressed artifact manifests, negative staging/extraction/manifest/
  contract tests, and tag/version validation on every supported tuple;
- all-tuple tag publication with unique assets and fail-if-existing release
  semantics; manual workflow runs do not publish;
- validated CUDA/XDMA `evidence_only` schemas and source-commit-bound raw
  evidence manifests that cannot promote a hardware support tuple;
- a changelog and supported-version security policy;
- final evidence review that leaves RT1, RT2, CUDA, and XDMA claims unqualified
  unless their separate tuple procedures pass.

Exit gates:

- every named portable tuple is represented by a pinned required CI runner and
  exact declared compiler;
- version, changelog, package metadata, support matrices, stable surface, and
  release tag agree;
- every package archive is relocatable and covered by a source-commit,
  byte-length, and SHA-256 manifest;
- corrupt, missing, unlisted, duplicate, or path-escaping release artifacts
  fail validation;
- concurrent runtimes cannot share device or observability state;
- release documentation never converts portable CI or raw hardware evidence
  into an RT1, RT2, CUDA, or XDMA qualification claim.

M9 and M10 remain Candidate until real hardware records pass their independent
gates. PREEMPT_RT deployment records are also reviewed separately from portable
1.0. The exact release and support promise is in
[the release policy](release_policy.md) and
[portable support matrix](portable_support_matrix.json).

## M13 — Strict runtime profiles and operational autotune

Delivered in 1.1:

- installed `<rt/profile.hpp>` with a 64 KiB, allocation-free,
  transactional UTF-8/JSON parser and bounded diagnostics;
- complete resolved profiles containing all 25 runtime-config schema-v7
  fields, explicit envelope/runtime compatibility, and optional opaque
  experiment provenance;
- fail-closed rejection of malformed input, unknown/duplicate contract keys,
  missing fields, invalid types/cross-field values, incompatible versions, and
  trailing bytes;
- `rtfw_runtime_demo`, which loads a profile or `RTFW_PROFILE`, builds four
  independent physics systems plus a dependency barrier on `rt::Runtime`, and
  reports direct frame/deadline/executor metrics;
- an operational production autotune spec restricted to supported
  `worker_count`, `executor_policy`, and `executor_queue_capacity` factors;
- generated default/example profiles, content-derived profile IDs, schema and
  mapping checks, strict parser tests, and a mandatory generated-profile C++
  round trip in CI;
- unchanged stable C ABI v8, device ABI, and Apache-2.0 license.

Exit gates:

- parsing succeeds without allocation and never partially updates outputs;
- every runtime field is resolved and validated by the authoritative typed
  runtime contract;
- incompatible schema/runtime versions and negative parser mutations fail
  closed with bounded diagnostics;
- generated factor changes reach the effective `RuntimeConfig`;
- the real target-runtime demo accepts a generated profile and emits the
  metrics consumed by `run_one.py`;
- documentation does not convert autotune output into an RT1, RT2, CUDA, XDMA,
  or portable latency claim.

The profile format and precedence rules are in
[the runtime profile contract](runtime_profiles.md).

## M14 — Professional SDK and package boundary

Delivered in 1.2:

- canonical `runtime` component and `rtfw::runtime` C++20 target, with the
  existing `cpp_runtime` component and `rtfw::simcore_rt` target retained as
  1.x compatibility paths;
- a supported runtime archive containing only graph, executor, memory,
  time/platform, observability, replay/profile, and device-runtime code;
- default-off broad research targets for SimCore, plugins, scheduler,
  crashdump, and utilities, with a narrow separate 1.x compatibility archive
  retaining the accidental demo/fiber link path outside the supported runtime;
- an exact default installed-header allowlist plus focused status,
  configuration, and canonical-byte headers;
- isolated optional CUDA/XDMA adapter exports and build-local project warning,
  Werror, logging, and profiling policy;
- relocated consumers that validate every supported header alone, exact SDK
  inventory, automatic C++20 propagation, independent components,
  compatibility aliases, policy isolation, and the installed license digest;
- release-contract schema 2, with explicit preferred/compatibility targets,
  default/conditional headers, forbidden default surfaces, and experiment
  defaults.

Exit gates:

- stable C ABI v8, SONAME 8, device ABI v1, and existing schemas are unchanged;
- `rtfw::runtime` has no plugin, scheduler, fiber, crashdump, SimCore, HAL
  stub, GPU stub, `dl`, warning-policy, or feature-macro dependency;
- the default package exports only contracted targets and headers;
- optional vendor dependencies are resolved only for requested adapter
  components;
- old target names still compile and run supported runtime consumers;
- source and installed Apache-2.0 digests remain canonical.

## M14.1 — Recoverable device lifecycle safety

Delivered in 1.2.1:

- status-bearing, deterministic reverse buffer/backend cleanup that retains
  failed ownership markers and retries only unresolved operations;
- ownership-uncertain handling for failed initialization, including a checked
  shutdown attempt and recoverable failed rollback;
- runtime quiescence and fail-closed execution/state-mutation gates while
  cleanup is pending, without falsely reporting the public `stopped` state;
- ABI-v8-compatible C destruction that preserves a handle when implicit device
  cleanup fails, plus an explicit checked-stop integration contract;
- injected multi-backend, failed-start, CUDA/XDMA partial-initialization,
  dynamic-C, warnings-as-errors, sanitizer, ABI, and release-contract
  evidence.

Exit gates:

- no registered buffer or initialized backend marker is cleared after its
  release callback fails;
- no backend shutdown runs while one of its registered buffers remains;
- first cleanup failure is preserved while independent reverse cleanup
  continues;
- repeated stop touches only unresolved ownership and becomes idempotent after
  success;
- execution, restore, and replay cannot mutate state during cleanup-pending;
- C ABI v8, 70 exports, SONAME 8, device ABI v1, schemas, package inventory,
  qualification claims, and Apache-2.0 remain unchanged.

## M15 — CPU and memory policy

Delivered in M15-01:

- additive bounded C++ thread and memory policy request types in the existing
  installed SDK headers;
- stable role/category identifiers and exact-once accounting keys;
- strict finalization validation, portable no-op resolution, transactional
  requested/resolved reports, external verify-only ownership, and two-runtime
  isolation;
- unchanged schema 7, C ABI v8, device ABI v1, package inventory, callbacks,
  and runtime steady state.

Delivered in M15-02:

- Linux native resolution, apply, and readback for runtime-owned executor,
  watchdog, and device-service lanes, with caller/external verify-only ownership;
- one startup barrier, strict fail-closed reverse rollback, truthful
  best-effort aggregation, and retryable M14.1 device-cleanup preservation;
- role-local spin, yield, and park idle behavior with bounded work/stop wakeups;
- unchanged portable RT0, ABI, package, schema, release, and qualification claims.

Implemented in M15-03, subject to the batch verification and review gates:

- one copied size/versioned C++ provider table with five required callbacks,
  borrowed user data, reentrancy rejection, and reverse exactly-once cleanup;
- exact phase-scratch, task-scratch, and trace-storage acquisition in stable
  order, pre-thread apply/observation, reverse rollback, and checked-stop token
  release;
- default aligned-new compatibility plus Linux process-local base-page/guard
  mappings, explicit `MAP_HUGETLB` attempt and requested fallback,
  caller/prefault touch, `mlock` application, and independent `mincore`
  residency sampling;
- requested/resolved/acquired/applied/verified region reporting without
  double-counting the existing memory-plan equation;
- truthful boundary that `mlock` is not independent lock readback or
  device/DMA pinning, and provider-backed trace is unavailable after its token
  is released at checked stop.

Implemented in M15-04, subject to the batch verification and review gates:

- exact non-overlapping logical extent reconciliation for constructed
  runtime/executor/device controls against the existing `MemoryPlan` terms;
- exact, declared-only, partial, unknown, and not-applicable row/aggregate
  accounting with bounded copied declarations for opaque external/backend facts;
- live runtime-owned stack commitment/observation and status-bearing owning-lane
  cleanup before native stack lifetime ends;
- retryable reverse device/thread/stack/control/selected-region cleanup with
  first-error retention and no early provider-token release;
- unchanged provider acquisition boundary, borrowed-resource ownership, stable
  C ABI v8, device ABI v1, schemas, package inventory, and RT0 claim boundary.

Exit gates:

- strict policy fails closed before callbacks run and rolls back every partial
  thread or memory commitment;
- all runtime-owned threads and regions are represented exactly once;
- portable defaults retain current behavior and all zero-allocation,
  determinism, sanitizer, ABI, and package gates pass;
- no RT1 or RT2 claim is made from policy configuration alone.

## M16 — Multi-rate simulation

M16-01 through M16-04 are complete in merged target history:

- bounded copied rate domains and exactly-one phase ownership;
- exact checked epoch-zero `[0, lcm)` reference timeline with deterministic
  same-time ordering and fixed-copy inspection;
- graph/replay identity and runtime-control accounting integration;
- unchanged complete-graph host and periodic dispatch.
- bounded copied CPU-only channels with exact first-cycle initial and
  repeating-cycle wrap selections;
- distinct held/provenance/fresh/stale metadata and a preallocated two-slot
  exact-generation SPSC store per channel;
- conditional cross-rate graph/replay identity and exact runtime-control
  accounting without a seventh plan row or provider-region expansion.
- opt-in D0 mandatory-CPU admission on one conservative serialized lane;
- checked logical/nominal active windows, exact reference-order dispatch, and
  bounded skip, catch-up, hold, degrade, and fail actions;
- staged exact-generation channel transfer, functional step/periodic summaries,
  and one canonical active checkpoint state record without schema evolution.

- mandatory-only admission with bounded optional CPU execution, deterministic
  criticality/registration shedding order, reverse recovery, and hysteresis;
- fixed-capacity versioned rate-action records, exact loss/cursors/counters,
  conditional policy checkpoint state, and unchanged schema-2 observability;
- action-aware replay remains outside M16 and would require separate approval
  beyond schema-1 input logs.

Exit gates:

- harmonic and non-harmonic schedules match a reference timeline;
- cross-rate snapshots are race-free and deterministic at their declared tier;
- overload never creates unbounded catch-up work or silently skips mandatory
  releases.

The retained M16 evidence contains its original pre-merge gate language. The
merged commits establish completion; no absent final CI run identifier, check
count, or separate human-review object is inferred.

## M17 — HAL v2 and heterogeneous memory

M17's portable software path is merged and its qualification remains
incomplete. M17-01 permanently implements:

- an additive C++ HAL API version 2 core table in the existing installed
  header, with capability, lifecycle, borrowed-buffer, one-submission, bounded
  poll, cancellation, health, reset, and shutdown operations;
- one canonical HAL v2 manager path for native tables and exactly-once,
  address-stable adapters around unchanged device-ABI-v1 backends;
- complete shared-field/status translation, exception containment,
  fail-closed output validation, legacy adapted-v1 identity, conditional
  native-v2 kind/version identity, and exact device-control accounting;
- unchanged service-lane, early-ready, reverse cleanup/retry, mock, CUDA
  candidate, XDMA candidate, C ABI, package, schema, release, and support
  boundaries.

M17-02 implements:

- an optional, separately versioned and copied HAL v2 extension with bounded
  discovery, memory registration, cleanup, and timestamp correlation;
- exactly six domain kinds: host, pinned host, CUDA device, imported, DMA
  mapped, and peer, with explicit ownership, access, coherency,
  synchronization, alignment, granularity, capacity, and opaque-handle rules;
- bounded topology nodes/links and timestamp domains, instance-local Runtime
  inspection, and one declared running-state correlation operation;
- an additive heterogeneous-buffer registration overload with foreign/stale,
  malformed, overlap, capacity, alignment, cleanup-retry, and isolation rules;
- an implicit borrowed-host, host-coherent, no-sync domain for adapted-v1 and
  core-only v2 backends, preserving the exact M17-01 legacy identity path;
- conditional semantic identity and exact `device_control_bytes` accounting
  without changing the MemoryPlan equation, provider regions, package, C ABI,
  device ABI, schemas, release, support, or qualification boundaries.

M17-03 implements fixed command batches, same-backend timeline waits/signals,
explicit ordered synchronization, one precreated Runtime submission lane per
opted-in backend using role value 6, whole-completion validation, deterministic
timeout/cancel/stop, and exact device-control/runtime-stack accounting.

M17-04 implements additive native-v2 registrations on the existing CUDA and
XDMA candidates, caller-owned pre-instantiated CUDA Graph dispatch through
stable identifiers and copied bindings, and bounded XDMA 32-bit control and
user-event operations. It preserves both device-ABI-v1 paths and routes work
only through the existing isolated submission/service and backend-worker
model.

M17-05 retained the exact discovery blocker: canonical Runtime cleared the
command-capability input header while both native callbacks required its size.
M17-06 supplies the approved Runtime-owned repair, preserves full returned-
record validation, and proves both actual native candidates register in one
Runtime. It also implements the portable CPU-to-simulated-CUDA-to-disjoint-
host-stage-to-simulated-XDMA-to-CPU graph with two backend-local timelines,
fixed storage, bounded failure/recovery, isolation, and sanitizer gates. No
plugin/factory, cross-backend timeline, direct-peer, combined-hardware, or
qualification capability is present. M17 and CAP-M17 remain incomplete until
mandatory hosted CI and human gates pass.

Exit gates:

- no vendor callback executes on an executor worker;
- submit, worker, and poll steady state is allocation-free and bounded;
- blocked-driver, queue failure, quarantine, coherency, cross-instance,
  sanitizer, and v1-compatibility tests pass.

## M18 — Hardware and real-time qualification

M18-01 implements the offline foundation:

- independent version-1 plan, record, review, and generated proposal schemas;
- bounded strict parsing, exact-byte provenance, artifact-tree integrity,
  predeclared threshold evaluation, mandatory trial coverage, and deterministic
  atomic proposal-only output;
- synthetic NVIDIA, XDMA, combined, RT1, and RT2 fixtures that are never
  support-matrix eligible;
- a hard boundary that continues to reject non-synthetic combined promotion
  until a separately approved qualification-policy update, and leaves human
  chronology, identity, review, and matrix changes outside the tool.

Remaining outcome:

- retained evidence for named CPU/kernel/BIOS/compiler/runtime/workload tuples;
- CUDA driver/GPU and XDMA driver/firmware/bitstream/BDF tuples with functional,
  recovery, resource-stability, and latency-decomposition evidence;
- RT1/RT2 promotion only against thresholds selected before measurement.

Exit gates:

- qualification tools reject incomplete provenance, post-selected thresholds,
  unstable resources, or missing failure/recovery trials;
- only reviewed tuples enter support matrices; portable builds retain no
  implied latency or hardware claim.

## M19 — Engine and extension integration

M19 is active and incomplete. M19-01 implements the installed, independently
versioned C extension ABI v1 and configuring-only C++ registration operation.
It provides transactional fixed-capacity phase, device-v1 backend, service,
resource, and relationship staging; generation-bearing instance-local handles;
host-control service lifecycle; checked stop/retry; and checked detach
readiness without an OS loader. It adds exactly one default SDK header and no
target or C ABI export. M19-03 retains Unreal world/module integration and
actual host unload orchestration. CAP-M19 remains incomplete.

Planned outcome:

- maintained adapters and examples for engine job systems, allocators,
  clocks, telemetry, state serialization, and external GPU/FPGA resources;
- lifecycle-safe extension registration, version negotiation, and unload
  rules with no dependency on the experimental plugin path.

Exit gates:

- at least one representative game-engine-style host and one independent
  simulation host pass relocated integration, hot-reload/unload, saturation,
  and long-run tests;
- adapters do not bypass runtime ownership, capacity, determinism, or
  qualification boundaries.

## M20 — Operational and release hardening

M20 is active through preparatory batch M20-PRE-01. That batch adds a bounded,
deterministic portable assurance lane for exact dependency identity, complete
default first-party Clang 14 static-analysis coverage, supported snapshot and
runtime-profile fuzz smoke, explicitly experimental job-queue fuzz smoke,
canonical SPDX 2.3 candidate SBOMs, strict expected-source manifests, unsigned
in-toto candidate statements, offline cryptographic verification of retained
public fixture material, and relocated default-package consumption.

M20-PRE-01 does not sign or publish RTFW artifacts and does not complete
CAP-M20. Continuous fuzzing, authenticated target provenance, reproducible
signed releases, controlled performance, long-duration soak, vulnerability
response operations, and supported-version maintenance remain later batches.

Planned outcome:

- fuzzing and sanitizer matrix expansion, static analysis, SBOM/provenance,
  signed reproducible releases, vulnerability response, deprecation tooling,
  long-duration soak/fault campaigns, and supported-version maintenance rules.

Exit gates:

- every shipped artifact is reproducible, attributable, install-tested, and
  covered by the declared support/security policy;
- release promotion is blocked by ABI, docs, package, license, evidence,
  sanitizer, soak, or unresolved critical-security failures.

## M21 — Active device rates and sampled I/O

M21-01 is merged. It models and conservatively admits
HAL-v2 command-batch device releases using copied completion budgets, in-flight
bounds, payload-reference roles, backend-local timelines, Runtime/backend
capacities, and exact cyclic completion intervals. It is deliberately static:
ordinary device-frame execution remains unchanged.

M21-02 is merged with bounded dispatch/completion on existing backend lanes,
direct dependency slices, independent in-flight overlap, exact release
tickets, deadline/budget timeout clamping, and late-completion quarantine.
M21-03 is merged and adds explicit CPU/device cross-rate payload selectors,
disjoint slot subranges, Runtime-only materialization, and terminal-success
publication. M21-04 is merged with generic sampled I/O, safe states, and the
installed loopback. M21-05 is merged with fixed logical actions, conditional
checkpoint state, deterministic active replay, loopback action hooks, and
reusable installed-surface conformance.
Portable evidence does not qualify physical CUDA, XDMA, DAC/DAQ, HIL, RT1, or
RT2 behavior.

## M22 — Transactional live controls

M22-01 is merged. It adds an opt-in copied
policy, fixed mailbox and producer declarations, generation-safe handles,
canonical frame/rate-target records, bounded copied payload slots, one-attempt
reject-new admission, deterministic inspection, and exact identity/accounting.
M22-02 is merged. It adds exact frame/rate close, canonical
ordering and replacement, atomic immutable generations, terminal slot reuse,
callback-local views, and fixed commit/status inspection without changing an
artifact, telemetry, stable ABI, support, or qualification contract.
M22-03 is merged. It adds Runtime-generation rollback, a distinct payload-free
action stream, conditional checkpoint state, and a separate bounded live-
control replay artifact without changing prior schemas.

Planned dependent batches:

- M22-02 defines deterministic exact-boundary consumption, ordering, and
  missed/late/replacement semantics (merged).
- M22-03 adds bounded Runtime-generation rollback, a distinct payload-free
  action stream, conditional checkpoint state, and a separate live-control
  replay artifact without weakening existing artifact compatibility (merged).
- M22-04 adds typed SDK helpers, examples, stress closure, and milestone
  evidence (active candidate).

Exit gates require bounded nonblocking ownership, no partial publication,
complete instance/generation isolation, exact memory and compatibility
identity, unchanged disabled-path behavior, hosted sanitizer/concurrency and
package compatibility, and human API/claim review. Portable mailbox evidence
does not establish physical control, HIL, RT1/RT2, Unreal hot reload, release,
deployment, or production readiness.
M22-04 closes only CAP-M22 portable software after every hosted and human gate
passes and the candidate merges.
