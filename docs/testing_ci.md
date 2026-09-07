# Testing and CI Evidence

M22-03 focused gates retain M22-01/M22-02 staging and boundary coverage and add
fixed action layouts/tables/ring loss, success settlement, callback-failure
Runtime-generation rollback, rolled-back slot ownership, conditional checkpoint
round trip and stale-handle retirement, bounded replay construction/inspection,
checksum mutation rejection, exact host-frame generation injection, short-
output behavior, package consumption, and strict source compilation. Hosted
GCC/Clang/MSVC, ASan/UBSan/TSan,
determinism, static, package, C ABI, and retained M16/M17/M21 gates remain
mandatory before merge.

M22-04 focused gates add exact envelope bytes and mutation rejection, caller-
output transactionality, compile-time codec rejection, transparent host/rate
builders, typed clear-fault/raw-result behavior, the 65,536-byte maximum typed
payload, absolute 64-mailbox/256-producer/65,536-record finalization, checked
1 GiB policy arithmetic without forced physical commitment, one-attempt
concurrent admission, exact full-occupancy/reclamation/action-loss counts,
rollback/watchdog orthogonality, 24 fresh lifecycles, eight concurrent isolated
Runtimes, no-allocation construction/decode, the domain sample, standalone
header compilation, and installed/relocated package consumption. Assertions
use finite operation counts, not sleeps or timing tolerances.

M21-05 focused gates cover action-ring bounds/loss, active-artifact parsing and
mutation rejection, deterministic device-active replay, public loopback logical
hooks, a multi-rate plant/sensor/controller/actuator fixture, exact accounting,
optional-device shed/recover, completion-error/timeout/loss fault replay,
two-instance isolation, and direct package consumption. Hosted GCC/Clang/MSVC,
TSan, determinism, packaging, and portable assurance passed for candidate run
33110390319 and remain required for any changed candidate head before merge.

CI is regression evidence for selected builds and behaviors. It is not a
latency qualification and does not prove the complete product contract.

M20-PRE-01 adds a required Ubuntu 22.04 Clang 14 `Portable assurance` job. It
runs exact dependency reconciliation, complete-manifest clang-tidy, three
fixed-seed ASan/UBSan fuzz smokes, deterministic candidate SPDX/provenance and
expected-source manifest verification, offline signature verification of a
public non-target fixture, safe archive extraction, relocated consumption, C
ABI v8, and SONAME 8. Machine reports are retained as candidate CI evidence.
The workflow remains `contents: read`; the job cannot sign, attest, publish,
release, deploy, or qualify hardware/RT behavior.

## Current coverage

- GoogleTest unit and integration tests cover phase execution, queues, arenas,
  numerics, snapshots, trace, metrics, mock devices, plugins, and utilities.
- Host-runtime tests cover lifecycle transitions, strict configuration,
  no-pacing host steps, callback failure containment, bounded traces, and
  isolation of two runtime instances.
- Runtime-profile tests cover complete schema-v7 application, allocation-free
  parsing, transactional failure, exact compatibility, bounded size/nesting,
  malformed UTF-8/JSON, wrong types, unknown/duplicate/missing keys, invalid
  cross-field values, and bounded error paths.
- Compiled-graph tests cover deterministic order, cycles, invalid and
  foreign-runtime handles, direct/transitive resource ordering, immutable
  topology, and randomized DAG agreement with an independent reference
  executor.
- M16-01 tests compare complete harmonic and non-harmonic `[0, lcm)` timelines
  field-by-field with an independent integer generator. They cover exact
  simultaneous-release ordering, reduced ratios, malformed/foreign/rebound/
  missing ownership, checked arithmetic and capacity boundaries,
  transactional retry, identity sensitivity, runtime isolation, unchanged
  host/periodic/mock-device dispatch, exact MemoryPlan reconciliation, and
  allocation-free inspection plus CPU/device frames.
- M16-02 tests compare every selection field against an independent integer
  selector across harmonic, co-prime, non-harmonic, simultaneous, reordered,
  wrap, and multi-substep cases. They cover copied/frozen API semantics,
  malformed/foreign/device/same-domain/duplicate/capacity failure, exact
  zero/boundary/unbounded freshness, first-cycle initial and prior-cycle
  provenance, held generations, transactional retry/provider ordering, exact
  accounting, identity, two-runtime isolation, unchanged dispatch, and
  allocation-free inspection.
- Snapshot-store tests cover exact generation/size, retain/retire lifetime,
  capacity, not-ready and stale results, no substitution, and deterministic
  bytes under synthetic SPSC concurrency. A separate contended test removes
  the per-generation handoff, so payload visibility depends only on the store's
  publication atomics and each copied generation is checked byte-for-byte. The
  GCC ThreadSanitizer gate runs the complete `CrossRateData` suite.
- M16-03 tests cover copied/frozen opt-in policy, exact reference-only
  compatibility, malformed and infeasible admission, D1/cross-domain/skip-
  producer rejection, half-open contiguous windows, periodic nominal releases,
  global and catch-up caps, every late action, exact context/summary fields,
  produced/held reads, missing/duplicate publication, fault gating, active
  checkpoint round trip/corruption transactionality, and active input-log
  rejection. Allocation instrumentation covers on-time and late-degrade paths.
- M21-01 tests cover additive defaults and installed-source compatibility,
  copied-role validation, command/buffer/timeline/timestamp ownership,
  harmonic and wraparound half-open intervals, per-phase/backend/Runtime
  in-flight limits, exact completion-batch boundaries, deterministic
  inspection, transactional correction, identity/accounting, and pre-callback
  active-mixed pre-dispatch validation. The focused CTest target is
  `m21_device_rate_model`.
- M21-02 tests cover the accepted mixed start transition, appended exact
  provider context, direct dispatch/ticket accounting, more than one admitted
  independent release outstanding before completion, completion-budget
  timeout, one-shot cancellation, terminal quarantine, late-completion
  suppression, and unchanged ordinary command-batch behavior. These are
  portable fake-driver RT0 tests, not timing or hardware qualification.
- M21-03 tests cover invalid-stride correction before callbacks, exact
  CPU→device pre-provider copy, Runtime-only reference materialization,
  submit-without-publication blocking, terminal device→CPU payload capture,
  completion metadata, direct memory accounting, and unchanged M16/M21-02
  suites. They are portable fake-driver RT0 evidence only.
- M21-04 tests retain sampled admission, complete frame validation, terminal-
  success publication, stale/overrun/underrun policy, acknowledged startup and
  checked-stop safety, fixed loopback fault behavior, accounting, identity,
  relocation, and instance isolation.
- M21-05 tests cover configuring-policy bounds, fixed action records and
  capacity-zero/exact/overwrite/gap semantics, deterministic active-artifact
  bytes and checksum rejection, checkpoint-backed loopback replay with exact
  generated actions and final state, bounded loopback logical-action hooks,
  optional-device shed/recover without ownership, terminal sampled/stop
  actions, completion-error/timeout/loss replay with failure-safe output, and
  one reusable public-header conformance flow at 100/150/225 ms plus a 450 ms
  observer. The fixture is also compiled and run as a package consumer.
  Physical timing, vendor I/O, and RT qualification remain outside it.
- M22-01 tests cover disabled and exact-capacity policy paths, fixed descriptor
  layout, copied registrations, exact host/rate targets, canonical payload
  digest and copy isolation, short/long inspection rejection, full/no-
  overwrite behavior, stopped/stale/exhausted outcomes, callback rejection,
  Runtime isolation, frozen identity sensitivity, arrival exclusion, exact
  MemoryPlan/control-ledger accounting, and installed consumer use. Concurrent
  busy/stop stress and sanitizer behavior remain mandatory hosted gates; no
  test may describe staged bytes as committed state.
- Allocation instrumentation observes no heap allocation during the first
  frame of a representative compiled graph (the M2 topology gate) and during
  64 complete M4 target-path frames under each executor policy with independent
  phases, range work, fixed-tree reductions, aligned task scratch, tracing,
  and an armed M5 watchdog.
- Fake-clock tests prove exact absolute releases/deadlines, no epoch drift after
  late frames, clock-failure propagation, one-shot watchdog events, and
  frame-thread degradation.
- Injected platform probes prove strict pass/fail reporting, rejection before
  worker creation, duplicate-prerequisite rejection, and disabled-mode
  non-interference.
- Observability tests prove stable provenance, adjacent-interval partitioning
  of cumulative counters, gauge sampling, exact trace-cursor loss, runtime
  isolation, JSON schema fields, and drop-without-wait behavior under slot
  contention.
- Determinism/replay tests compare D1 canonical state across one, two, and four
  workers; transfer checkpoints across worker counts; reproduce final state
  from checkpoint plus input log; require transactional rejection of corrupt
  and foreign artifacts; and exercise 5,000 deterministic parser mutations.
- Allocation instrumentation also covers checkpoint sizing/writing,
  inspection, restore, and input-log writing against caller-owned buffers.
- Device-runtime tests cover bounded queue/outstanding saturation,
  cancellation, delayed dependency release while independent CPU work
  proceeds, timeout, error, loss,
  health/reset, shutdown ownership, malformed backend tables, memory
  accounting, trace ordering, all 32 metrics, and concurrent state isolation
  between independent runtime/backend/buffer instances. A named isolation gate
  also runs explicitly in every supported Linux build tuple.
- M17-01 HAL tests cover exact API version/default records and core limits;
  complete native-v2 and device-ABI-v1 table/capability/status/operation
  translation; malformed size, version, enum, boolean, identifier, reserved,
  output-count, completion, and health data; callback exceptions; duplicate,
  capacity, freeze, rollback, cleanup retry, address stability, and two-runtime
  isolation. One direct native/adapted scenario compares submission, polled
  completion, health/reset, trace/metrics, borrowed-buffer observations, and
  checked stop. Permanent adapted-v1 manager tests separately retain early
  completion, fault, mutation, dependency-release, recovery, and isolation
  coverage on the same canonical manager path.
- M17-02 tests cover the exact six-domain taxonomy and fixed snapshot limits;
  positive discovery, registration, inspection, correlation, and submission;
  invalid size/version/reserved/enumeration/reference/count/alignment/
  granularity/capacity/ownership/access/coherency/sync/opaque-handle records;
  foreign handles, overlap, unsupported calls, callback exceptions, timeout,
  cancellation, failed-start rollback, reverse cleanup retry/recovery, and
  two-runtime isolation. Legacy adapted-v1 and core-only-v2 cases prove the
  implicit borrowed-host mapping and permanent M17-01 equivalence.
- Identity tests retain exact adapted-v1 pre-M17 graph/replay and artifact
  bytes while requiring native-v2 kind/API separation. Memory tests reconcile
  adapter/table/context storage exactly once in `device_control_bytes`, the
  M15 logical ledger, and the unchanged six-row plan. Installed preferred and
  compatibility consumers retain old v1 aggregate prefixes and compile the
  additive native-v2 registration.
- Allocation instrumentation covers 64 post-warmup mock-device frames, and the
  dynamic C ABI test defines and drives a backend solely through loaded v7
  symbols.
- CUDA candidate tests use an injected CPU-only driver to cover fixed queue
  saturation, concurrent submitters, host/device and device/device copies,
  byte fills and range rejection, external allocation ownership, failed
  registration cleanup ownership, fixed kernel payloads, timeout quarantine,
  enqueue/query-failure drain/reset, context loss, retryable drain-before-free
  shutdown, runtime graph integration, and steady-state submit/poll
  allocation.
- XDMA candidate tests use an injected CPU-only blocking driver to cover H2C
  and C2H integrity, malformed registration/submission rejection, bounded
  saturation, timeout quarantine until physical worker return, reset-required
  health and soft recovery, retryable shutdown, concurrent submit/poll, and
  steady-state no-allocation behavior.
- Unified-executor tests stress independent phases with nested ranges and
  fixed-tree reductions under both policies, verify stable static assignment
  metadata, force deterministic queue saturation, and require real local
  execution and successful cross-worker steals.
- A focused GCC ThreadSanitizer job runs the unified-executor, M4 memory-plan,
  M5 time/platform, M6 observability, M7 determinism/replay, and M8 device
  suites plus the CPU-only M9 CUDA state machine.
  It also runs the M16 reference-plan, cross-rate selection/store, and complete
  `RateDispatch` functional suite, including two-runtime, inspection,
  host/periodic, failure, and no-allocation regressions.
  M17-01 adds complete `HalV2`, adapted `DeviceRuntime`/`DeviceMock`,
  early-ready, concurrent submit/poll, cleanup/retry, two-runtime, CUDA
  fake-driver, and portable XDMA processes without weakening permanent M15 or
  M16 gates.
  M17-02 adds the heterogeneous-memory suite and its registration/correlation,
  cleanup-retry, submission-translation, and cross-instance processes without
  weakening those permanent gates.
- `test_differential_output.cpp` compares a sample numerical kernel with a
  checked-in golden result under an absolute drift threshold.
- fault-injection tests exercise selected allocator, delay, and transient-error
  paths.
- the determinism matrix runs a filtered integration test and emits the same
  integer-only D1 checkpoint artifact under GCC/Clang and FMA on/off. A
  separate exchange job byte-compares all four artifacts.
- a Clang/libFuzzer smoke job runs the allocation-free checkpoint and input-log
  inspectors for 20,000 generated inputs.
- Linux and Windows jobs exercise selected Debug/RelWithDebInfo builds.
- Executable shared/static C and C++ compiled-graph samples, dynamic C ABI
  loading, real generated-profile runtime round trips, autotune-tooling,
  scaling-artifact, and documentation-contract jobs
  provide focused smoke coverage.
- The named Ubuntu GCC/Clang and Windows MSVC support tuples build and run
  relocated consumers from the exact extracted CPack archives. Pull-request
  CI also checks the 1.2 support/compatibility contract and creates then
  verifies each archive manifest.
- A tag release waits for all three supported-tuple archive consumers, requires
  exactly nine uniquely named assets, verifies the existing tag, and creates
  the GitHub Release once. Manual release-workflow runs do not publish.
- Release-tool unit tests cover manifest round trips, corruption, unlisted
  files, unsafe paths, incomplete commit identities, empty artifact sets, and
  negative mutations of the portable support and contract digests. CPack
  staging tests reject corrupt sidecars, unexpected/stale outputs, and
  accidental publication of its internal staging tree. Extraction tests cover
  flat TGZ/ZIP layouts, safe SONAME links, traversal rejection, and cleanup of
  a rejected partial destination. The same suite
  validates CUDA/XDMA `evidence_only` schemas and rejects promoted, duplicate,
  or unhealthy raw records.
- M18-01 release-tool tests validate all four independent draft-2020-12
  qualification schemas and synthetic NVIDIA, XDMA, combined, RT1, and RT2
  plan/record/review/proposal chains. Negative coverage includes duplicate and
  non-finite JSON, depth/size bounds, scope/identity/digest substitution,
  threshold/trial drift, unstable resources, device health/recovery, unsafe or
  unlisted artifacts, symlinks, noncanonical proposals, output collision and
  cancellation cleanup, recovery, isolation, and matrix/input nonmutation.

## What CI does not currently establish

- No job measures a statistically controlled worst-case or confidence-bound
  latency gate on dedicated hardware.
- Sanitizer jobs do not cover every sanitizer, platform, or code path.
- The libFuzzer smoke target is not a continuous fuzzing service and cannot
  establish parser safety for every input.
- The profile parser has deterministic negative/mutation-style cases but is
  not yet attached to a continuous libFuzzer service.
- M7 artifact checksums detect accidental corruption; they are not
  authentication and do not protect against maliciously rewritten artifacts.
- Cross-compiler artifact equality currently covers one canonical integer
  workload on Linux, not arbitrary callback code, floating-point behavior,
  architectures, standard libraries, or D2/D3 determinism.
- Passing deterministic mock-device tests do not exercise a hardware device,
  driver, DMA path, or completion-latency bound.
- Passing HAL v2, heterogeneous-memory, compatibility-adapter, package,
  sanitizer, hosted CI, or deterministic artifact tests establishes only the
  portable record/lifecycle behavior exercised. It does not establish physical
  allocation, pinning, DMA, coherency, synchronization, topology, peer access,
  timestamp accuracy, command batches, timelines, isolated blocking-vendor
  execution, physical hardware, HIL, field behavior, latency, RT1, or RT2.
- Passing fake-driver CUDA tests establish the backend state machine, not a
  CUDA toolkit build, physical GPU behavior, resource stability, recovery
  behavior, or completion-latency bound. The opt-in self-hosted CUDA workflow
  emits raw evidence but does not automatically qualify a tuple.
- Passing fake-driver XDMA tests establish queue and lifetime behavior, not an
  XDMA kernel-module build, PCIe/FPGA integrity, kernel page-pin/resource
  stability, failure recovery, or completion-latency bound. The destructive
  opt-in self-hosted XDMA workflow emits raw evidence but does not
  automatically qualify a tuple.
- M17-04 CUDA/XDMA command-capability tests invoke each candidate callback with
  a default-initialized output record. They do not exercise canonical Runtime
  discovery, which clears that header and is currently rejected by both
  callbacks. No successful native command/timeline Runtime integration is
  established until a positive behavioral registration test passes.
- Best-effort host-hardening steps on shared runners are not RT evidence.
- SHA-256 artifact manifests are integrity metadata, not signatures,
  provenance attestations, or proof that two builds are reproducible.
- M18-01 synthetic fixtures, parser success, reviewer strings, timestamps, and
  generated proposals do not prove pre-run chronology, authenticated approval,
  physical behavior, latency, RT1/RT2, or support promotion.

## Adding evidence

A present-tense feature claim should point to a test that fails when the
behavior disappears. A determinism claim must name its tier and comparison
matrix. A performance or RT claim must retain raw data, environment identity,
predeclared thresholds, and the measurement procedure.

## Code anchors

- Main workflows: `.github/workflows/ci.yml`
- Opt-in CUDA hardware evidence:
  `.github/workflows/cuda-qualification.yml`
- Opt-in XDMA hardware evidence:
  `.github/workflows/xdma-qualification.yml`
- Documentation contract: `.github/workflows/docs-contract.yml`
- Portable release workflow: `.github/workflows/release.yml`
- Release contract and manifest tests: `tools/check_release_contract.py`,
  `tools/stage_release_artifacts.py`, `tools/release_manifest.py`,
  `tools/extract_release_archive.py`, `tools/check_hardware_evidence.py`,
  `tests/test_release_tools.py`
- Qualification schemas and tool: `qualification/schemas/`,
  `tools/qualification.py`, `docs/qualification.md`, and the permanent
  `QualificationToolTests` cases in `tests/test_release_tools.py`
- Differential test: `tests/test_differential_output.cpp`
- Fault injection: `tests/test_fault_injection.cpp`
- Determinism integration: `tests/integration/test_determinism.cpp`
- M20-PRE-01 fuzz harnesses: `tests/snapshot_fuzz.cpp` and
  `tests/runtime_profile_fuzz.cpp` are supported-parser smoke surfaces;
  `tests/jobqueue_fuzz.cpp` remains explicitly experimental
- Portable assurance policies and tools: `.clang-tidy`,
  `tools/static_analysis_sources.txt`, `tools/check_static_analysis.py`,
  `tools/run_fuzz_smoke.py`, `tools/sbom_expected.json`, `tools/sbom.py`,
  `tools/provenance.py`, `release/portable-assurance-policy.json`, and
  `scripts/verify-portable-assurance.sh`
- Cross-build artifact producer: `tests/determinism_artifact.cpp`
- M1–M12 lifecycle, graph, executor/host adapter, memory, time, platform,
  observability, replay, and device
  tests:
  `tests/test_host_runtime.cpp`,
  `tests/test_compiled_graph.cpp`, `tests/test_executor.cpp`,
  `tests/test_memory_plan.cpp`,
  `tests/test_periodic_runtime.cpp`,
  `tests/test_platform_preflight.cpp`,
  `tests/test_observability.cpp`,
  `tests/test_determinism_replay.cpp`,
  `tests/test_device_runtime.cpp`,
  `tests/test_cuda_backend.cpp`,
  `tests/xdma_backend_tests.cpp`,
  `tests/host_adapter_tests.cpp`,
  `tests/test_trace_noalloc.cpp`,
  `tests/test_cabi_dlopen.c`
- M16-01 rate model and reference compiler:
  `rt/src/rate_timeline.cpp`, `tests/test_rate_timeline.cpp`
- M16-02 cross-rate compiler and bounded store:
  `rt/src/cross_rate_data.cpp`, `tests/test_cross_rate_data.cpp`
- M16-03 admission and active dispatcher:
  `rt/src/rate_dispatch.cpp`, `tests/test_rate_dispatch.cpp`
- M16-04 shedding recovery and versioned rate-action telemetry:
  `rt/src/rate_telemetry.cpp`, `tests/test_rate_telemetry.cpp`
- M17-01 HAL v2 core and v1 compatibility path:
  `rt/src/hal_v2.cpp`, `rt/src/device_manager.cpp`,
  `tests/test_hal_v2.cpp`, `tests/test_device_runtime.cpp`,
  `tests/test_determinism_replay.cpp`, `tests/test_memory_plan.cpp`,
  `tests/test_trace_noalloc.cpp`
- Stable ABI/export and installed-package gates:
  `tools/check_c_abi.py`, `abi/rtfw_c_abi_v8.exports`,
  `tests/package_consumer`
- M17-03 command/timeline and isolated-lane path:
  `rt/src/command_batch.cpp`, `rt/src/device_manager.cpp`,
  `tests/test_command_batch.cpp`, CTest `m17_command_batch_timeline`, and the
  ThreadSanitizer filter `CommandBatch.*`. Coverage includes exact public
  layouts/defaults, malformed extension/provider/completion output,
  same-backend timeline monotonicity, ordered synchronization, submit
  exception/error, timeout/cancel, blocked-stop recovery, isolation,
  accounting, source prefixes, and installed consumers.
- M17-04 native vendor paths: `tests/test_vendor_hal_v2.cpp`,
  `tests/test_cuda_backend.cpp`, and `tests/xdma_backend_tests.cpp`, with
  permanent CTest gates `m17_cuda_graph` and `m17_xdma_control`. Coverage
  includes exact table versions and stable opcodes, malformed driver tables and
  commands, copied Graph bindings, control apertures, event waits, timeout and
  stop recovery, lifecycle isolation, resource bounds, and preservation of the
  device-ABI-v1 candidates and installed consumers.
- M17-06 discovery and portable composition:
  `tests/test_command_batch.cpp`, `tests/test_vendor_hal_v2.cpp`,
  `tests/test_cpu_gpu_fpga_cpu_sample.cpp`, and
  `samples/cpu_gpu_fpga_cpu.cpp`, with CTest gates
  `m17_cpu_gpu_fpga_cpu_sample` and `sample_cpu_gpu_fpga_cpu`. Coverage includes
  exact capability input/whole-output rejection and retry, actual dual native
  registration, two deterministic frames, exact CUDA/bridge/XDMA ordering,
  separate timelines, zero measured step allocations, thread ownership,
  malformed declarations, CUDA and XDMA failure suppression, cancellation,
  recovery, isolation, checked-stop retry, default-off ASan/UBSan and
  ThreadSanitizer builds, package preservation, and repeat execution.

These gates authorize only static, unit, synthetic, failure-injected,
fake-driver native-v2 protocol, sanitizer, package, and RT0 functional
conclusions. They are not physical Graph, MMIO, interrupt, hardware, HIL,
field, bounded-latency, RT1/RT2, signing, release, deployment, or production
evidence. Mandatory GitHub CI and human API,
compatibility, concurrency, lifetime, memory-order, accounting, security, and
claim review remain external merge gates.
# M19-01 permanent gates

`m19_extension_registration` retains C/C++ layout, negotiation, malformed
input, transactional publication, phase, device-v1 adapter, service lifecycle,
retry, stale handle, two-Runtime, identity, accounting, and no-allocation
coverage. Linux GCC/Clang, Windows v143, sanitizer, deterministic, installed
package, archive, C ABI, dependency, vendor compile, and documentation/release
contract jobs remain mandatory. These are portable RT0/static/simulated gates,
not physical, Unreal, HIL, field, RT1/RT2, or production evidence.

## Optional host-side benchmarks (M23-01)

M23-01 adds the `m23_benchmark`, `m23_benchmark_artifact_validator`, and `benchmark_package_consumer` gates. Existing compiler/sanitizer/ABI/release checks are retained. Constant-identity fake-clock artifacts are compared across compilers; real timing is characterization, never a CI latency threshold.

See [benchmarking](benchmarking.md) for build, install, provider and artifact contracts.


## CPU benchmarks (M23-02)

M23-02 adds `BenchmarkCpu.*`, `m23_benchmark_cpu_inventory`, `m23_benchmark_cpu_noalloc`, and `benchmark_cpu_package_consumer`. The canonical CPU host-adapter fixture is compared separately across seven compiler/FMA/SDK configurations; platform-dependent memory and native scheduling observations are not byte-equality inputs. The CPU suite is included in existing sanitizer matrices and optional static analysis. Check retained evidence for actual results, including any unresolved scope or hosted gate.
