# RTFW — Bounded Simulation Runtime

Portable hosts can use the additive `<rt/runtime.hpp>` live-control policy and
bounded mailboxes to publish copied canonical update bytes at exact frame/rate
boundaries through callback-local immutable views. M22-03 adds Runtime-
generation rollback, payload-free action records, conditional checkpoint
state, and explicit bounded replay artifacts. The M22-04 candidate adds the
optional header-only `<rt/live_control.hpp>` fixed typed-payload layer and
portable stress closure; see
[`docs/live_controls.md`](docs/live_controls.md). Portable mixed-rate
users can combine `<rt/runtime.hpp>` with the installed
`<rt/loopback_backend.hpp>` for fixed-capacity sampled I/O, closed logical
actions, quiescent checkpoints, and deterministic active replay. See
[`docs/sampled_io.md`](docs/sampled_io.md) and
[`docs/determinism_replay.md`](docs/determinism_replay.md). This is software
RT0 evidence, not physical HIL.

[![CI](https://github.com/tranquilWorks/cpp-rt-car/actions/workflows/ci.yml/badge.svg)](https://github.com/tranquilWorks/cpp-rt-car/actions/workflows/ci.yml)
[![Documentation contract](https://github.com/tranquilWorks/cpp-rt-car/actions/workflows/docs-contract.yml/badge.svg)](https://github.com/tranquilWorks/cpp-rt-car/actions/workflows/docs-contract.yml)

RTFW is a C++20 bounded-resource simulation runtime for hosts that need an
explicit phase/resource graph, fixed-capacity CPU execution, controlled memory,
versioned observability/replay, and asynchronous device integration.

> **Status: 1.2.1 portable RT0 release.** Named GCC, Clang, and MSVC tuples
> support the target `rt::Runtime` path and stable C ABI v8. Portable support
> makes no hard-real-time, worst-case-latency, cross-platform bitwise-
> determinism, CUDA-hardware, XDMA, or C++ binary ABI claim. See the
> [product contract](docs/product_contract.md)
> and [release policy](docs/release_policy.md) before integrating it.

## Current implementation

| Area | Status | What exists today |
| --- | --- | --- |
| Host runtime lifecycle | Implemented RT0 surface | `rt::Runtime` enforces configure/finalize/start/step/stop, freezes graph topology at finalization, and retains unresolved device ownership for checked teardown retry |
| Compiled graph | Implemented RT0 surface | C and C++ hosts declare phase dependencies and logical resource access; finalization rejects invalid handles, cycles, and unordered conflicts |
| Host-driven callbacks | Implemented RT0 surface | `step()` waits synchronously without pacing; dependency-ready callbacks may run concurrently on the fixed team |
| Unified CPU executor | Implemented RT0 surface | Static deterministic assignment and bounded local-queue throughput policies run graph, range, and fixed-tree reduction work |
| Host job-system adapter | Implemented RT0 surface | A third executor policy submits the same immutable graph/range/reduction jobs to a borrowed fixed-capacity engine queue with explicit runtime scratch and generation-tagged completion context |
| Nested CPU work | Implemented RT0 surface | C and C++ callbacks can submit synchronous range/reduction work through their callback-local task context |
| Target-path memory plan | Implemented RT0 surface | Finalization budgets aligned phase/task scratch, CPU/device queue/control storage, and the trace ring; post-start CPU/device-frame tests observe zero runtime heap allocation |
| CPU/memory policy model | M15 complete | Additive bounded C++ reports retain twelve stable memory identities, reconcile exact logical control extents, observe live runtime-owned stacks, accept declared-only external/backend facts, and preserve retryable reverse cleanup; the provider still backs only phase scratch, task scratch, and trace storage |
| Multi-rate simulation | M16 complete in merged history | Bounded domains/reference order, exact cross-rate selection/storage, opt-in mandatory admission, optional CPU dispatch and hysteretic recovery, canonical policy state, and separate rate-action telemetry |
| Mixed-rate device and sampled-I/O closure | M21 complete in merged history | M21-01 through M21-05 join admission, concurrent dispatch, cross-rate payloads, sampled frames, safe outputs, public HAL-v2 loopback, fixed mixed-rate actions, conditional checkpoint state, and bounded active replay for explicitly deterministic backends. Hardware, HIL, and RT qualification remain separate. |
| Transactional live controls | M22-04 implementation candidate | M22-01 through M22-03 raw staging/publication/rollback/action/checkpoint/replay plus an optional header-only canonical typed-payload SDK, domain sample, package coverage, and deterministic portable stress. Executable/shared-library/Unreal reload, application/backend/physical side-effect rollback, physical control, HIL, and RT qualification remain excluded. |
| HAL v2 command/timeline, memory/topology, and device ABI v1 compatibility | M17-06 portable path merged; qualification incomplete | Runtime seeds the exact native command-capability input header and retains whole-record validation. Actual CUDA/XDMA candidates register together through canonical Runtime, and one portable CPU-to-simulated-CUDA-to-host-stage-to-simulated-XDMA-to-CPU sample uses separate backend-local timelines and fixed storage. This is simulated protocol, not hardware qualification. |
| Qualification schemas and proposals | M18-01 offline tooling; no tuple qualified | Version-1 plan, record, review, and proposal schemas with bounded artifact/digest/threshold/trial validation and deterministic proposal-only output; synthetic fixtures cannot promote support |
| Extension registration | M19-01 implemented; M19 incomplete | Installed size-versioned C ABI v1 plus transactional C++ Runtime registration, device-v1 compatibility, host-control services, checked stop and detach; direct entry pointers only, with no loader or Unreal lifecycle |
| Self-paced time | Implemented RT0 surface | A finite caller-thread loop uses absolute epoch-based releases and reports release/wake/start/finish/slack without drifting after late frames |
| Frame watchdog/degradation | Implemented RT0 surface | One arm produces at most one event; the service lane never invokes host code and the frame thread commits capped degradation for following frames |
| Strict platform preflight | Implemented RT0 surface | Disabled by default; read-only Linux prerequisite checks fail closed with a fixed-capacity report before runtime threads start |
| Target-path observability | Implemented RT0 surface | Schema-v2 fixed records and 32 metrics, bounded nonblocking emission, runtime-bound trace/metric cursors, explicit loss, provenance metadata, and non-RT JSON export |
| Target-path checkpoint/replay | Implemented D0/D1 surface | Canonical state registration, stable little-endian checkpoints/input logs, transactional restore, worker-count-independent D1 identity, and synchronous input replay |
| Target-path device ABI/mock | Implemented RT0 surface | Size/versioned poll-only backend ABI, registered buffers, nonblocking device phases, a runtime-owned completion lane, and deterministic fault-injectable CPU mock |
| CUDA Driver API backend | Candidate; not hardware-qualified | Optional caller-owned context/stream adapter with fixed event/buffer/kernel/Graph registries, native HAL-v2 registration, staged copies, ordered kernel/Graph launch, timeout quarantine, fake-driver tests, and a raw-evidence tool |
| Legacy phase/range execution | Experimental | `SimCore` retains its separate phase, range, reduction, and pacing path for compatibility |
| Legacy `SimCore` graph | Experimental | Topological levels exist, but this path does not inherit the target runtime's cycle/resource validation |
| Legacy memory utilities | Experimental | Per-thread frame arenas and NUMA helpers remain outside the target plan; the Release arena overflow path can fall back to heap allocation |
| Numerics | Experimental | FTZ/DAZ setup, explicit FMA gating, fixed reductions, fixed point, and counter PRNG utilities |
| Legacy trace | Experimental | `SimCore` retains process-global per-thread binary rings and offline adapters outside the M6 contract |
| Legacy metrics | Experimental | Demo JSON uses mutex-backed counters and rolling phase histograms with a default 120-sample capacity |
| Legacy snapshots | Experimental compatibility surface | Demo/native-layout helpers are bounds checked but remain outside target checkpoint schema v1 |
| C ABI | Stable ABI v8 | Version/fingerprint handshake, exact shared-library export allowlist, ABI-numbered SONAME, fixed-width contracts, and relocated shared/static consumers |
| Portable distribution | Supported RT0 on named tuples | Same-major CMake package discovery, relocated C/C++ consumers, checked release contract, CPack archives, and content-addressed artifact manifests |
| Portable assurance | M20-PRE-01 merged | One noninteractive lane verifies immutable dependency identity, exact Clang 14 static-analysis coverage, bounded deterministic libFuzzer smoke, a canonical SPDX 2.3 candidate SBOM, an unsigned in-toto candidate statement, expected-source artifact manifests, offline cryptographic verification of a public non-target fixture, and relocated package consumption. It signs and publishes nothing. |
| Product SDK boundary | Supported M14 surface | `rtfw::runtime` is the preferred C++20 target; the default package installs an exact public-header allowlist with no broad SimCore headers/targets/dependencies and no scheduler, plugin, fiber, HAL-stub, warning-policy, or feature-macro surface |
| Runtime configuration | Implemented schema 7 | Twenty-five strict typed keys include bounded execution/device capacities, time/platform, provenance, determinism, artifacts, and the host-adapter policy; unknown keys fail |
| Runtime profiles/autotune | Implemented RT0 host tooling | Allocation-free transactional profile parser, exact version/schema compatibility, complete resolved configs, profile-driven target-runtime demo, generated-profile round trip, and direct frame metrics |
| Legacy GPU stub | Experimental compatibility path | Detached CPU-thread stub outside `rt::Runtime`; superseded for new CUDA work by the separate M9 candidate |
| Xilinx XDMA AXI-MM backend | Candidate; not hardware-qualified | Portable fixed-capacity state machine with native HAL-v2 transfer/control/event commands plus an opt-in Linux character-device adapter, stop-aware timeout quarantine, fake-driver tests, and raw-evidence tooling for one named stack |

`rt::Runtime` does not use the legacy `WorkerPool`, `rt::Scheduler`, or
`FiberPool`. Those compatibility experiments retain different lifetime and
task rules. The M11 host adapter is the only supported path for an external
engine job system.

## Support and compatibility

RTFW 1.x supports only the exact RT0 build/test tuples in the
[portable support matrix](docs/portable_support_matrix.json). Other C++20
platforms are best effort unless added through a reviewed matrix change. C ABI
v8 is the stable binary boundary. The target C++ declarations reachable from
`<rt/runtime.hpp>` are source-compatible within 1.x, require recompilation, and
have no C++ binary ABI promise. CUDA, XDMA, RT1, and RT2 evidence is reviewed
and promoted separately.

Release archives include an immutable source commit and SHA-256 for every
artifact in a generated manifest. The complete versioning, deprecation,
support, and release checklist is in the
[release policy](docs/release_policy.md); changes are recorded in the
[changelog](CHANGELOG.md).

The M20-PRE-01 candidate assurance lane and its exact limits are documented in
[portable assurance](docs/portable_assurance.md). Its SBOM and provenance
outputs are verification candidates, not authenticated release provenance.

## Quick start

Prerequisites are CMake 3.20+, a C++20 compiler, and a recursive checkout of
the GoogleTest submodule when tests are enabled.

The commands below are executed verbatim by the documentation-contract CI job.

<!-- ci-verified: .github/workflows/docs-contract.yml -->
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DRTFW_BUILD_EXPERIMENTAL=OFF -DSIM_WERROR=OFF
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/rtfw_runtime_demo --config configs/default_safe.json --run 20ms --rt --metrics-json-interval
```

The command loads a strict profile and drives the supported `rt::Runtime`
graph. Successful
execution is a functional smoke test, not an RT qualification or tuning
recommendation.

The CUDA candidate is opt-in and uses the Driver API, so the host owns CUDA
initialization, context, stream, module, and kernel lifetimes:

```bash
cmake -S . -B build-cuda -DRTFW_ENABLE_CUDA=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-cuda --target sample_cuda_qualification --parallel 2
./build-cuda/samples/sample_cuda_qualification \
  --warmup 1000 --iterations 10000 > cuda-evidence.json
```

That command produces raw functional and latency-decomposition evidence. It
does not by itself add a tuple to the versioned support matrix or establish
real-time qualification.

The M10 XDMA candidate is separately opt-in on Linux and targets the official
Xilinx XDMA AXI-MM character devices:

```bash
cmake -S . -B build-xdma -DRTFW_ENABLE_XDMA=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-xdma --target sample_xdma_qualification --parallel 2
./build-xdma/samples/sample_xdma_qualification \
  --pci-bdf 0000:01:00.0 \
  --driver-id <driver-revision-and-build> \
  --bitstream-id <bitstream-sha256> \
  --warmup 1000 --iterations 10000 > xdma-evidence.json
```

The transfer is destructive to the selected AXI-MM range and must run only
against the declared qualification bitstream. It does not qualify hardware by
itself; see the [XDMA contract](docs/xdma_backend.md).

## Demo command line

These are the options implemented by `src/main.cpp`. Unknown options are
rejected.

<!-- cli-options:start -->
| Option | Behavior |
| --- | --- |
| `--threads <n>` | Set a positive internal worker count |
| `--pin` | Request worker affinity using the current Linux implementation |
| `--metrics-json` | Emit counters and phase percentiles at exit without resetting |
| `--metrics-json-interval` | Emit metrics, then reset rolling histograms and resettable counter baselines |
| `--snapshot-out <file>` | Write the final demo snapshot |
| `--snapshot-in <file>` | Load a demo snapshot before running |
| `--version` | Print the repository/runtime version |
| `--help`, `-h` | Print command help |
<!-- cli-options:end -->

Phase percentiles are **not cumulative for the whole process**. They cover the
latest samples retained by a rolling histogram, whose default capacity is 120.
Several worker telemetry names are also provisional; see
[observability](docs/observability.md).

The legacy `rtfw_demo` intentionally does not implement `--config`, `--rt`,
`--run`, `--duration`, or `RTFW_PROFILE`. The separate
`rtfw_runtime_demo` exercises those target-runtime controls without changing
legacy snapshot or CLI behavior. See the
[runtime profile contract](docs/runtime_profiles.md).

## Embedding lifecycle

Release 1.2 provides the M1 lifecycle, M2 compiled graph, M3 executor, M4
memory closure, M5 time/platform controls, M6 observability, and M7
checkpoint/replay, the M8 device ABI/mock, M11 stable ABI/host adapter, M12
portable distribution, the M13 profile loader, and the M14 product SDK
boundary in `<rt/runtime.hpp>`,
`<rt/profile.hpp>`, and `<rt/c_api.h>`:

1. configure a typed runtime and optionally attach a borrowed host job system
   and/or copied C++ memory-provider table;
2. register callback/device phases, optional copied rate domains and phase
   ownership, logical resources, canonical replay state,
   optional device backends, and borrowed device buffers while configuring;
3. declare phase dependencies and read/write resource access;
4. finalize to validate and compile the graph, optional epoch-zero rate
   reference plan, and opt-in active CPU admission plan; reject an over-budget memory
   plan, acquire phase/task/trace backing in stable order, construct fixed
   storage, and freeze topology and static assignments;
5. optionally require strict read-only platform preflight, apply/observe
   selected resident-memory policy, then start the fixed runtime worker team or
   bind the already-running host team, plus configured watchdog/device service
   lanes;
6. submit host-owned frame index, simulation delta, optional deadline, and—only
   for active rates—a contiguous nominal release to `step()`, or run a finite
   absolute-cadence loop with `run_periodic()`;
7. inspect per-frame timing, watchdog/degradation, active-rate summaries, and
   preflight results;
8. export versioned counters and trace records from a non-RT host lane;
9. write/restore bounded checkpoints or replay a validated input log from a
   non-RT host lane;
10. inspect/reset device health from a non-RT host lane when applicable;
11. call `stop()` and require success before releasing borrowed provider user
    data, backend, buffer, callback, state, clock, or host-executor storage.

The M15 memory provider is an exact size/versioned table with acquire,
apply, observe, rollback, and nonthrowing release callbacks. It is used only on
configure/finalize/start/stop control paths for active phase scratch, task
scratch, and trace storage. The default path retains aligned-new behavior.
Linux page policy uses process-local `mmap`, base-page-rounded inaccessible
guards and aligned usable bases, explicit `MAP_HUGETLB` with requested
fallback, caller/prefault page touch, `mlock`, and `mincore` residency
observation. Live token and allocation extents cannot alias across runtime
instances, and locking-only native storage uses independent page-backed
mappings. `mlock` is not independent lock readback or device/DMA pinning.
Native physical NUMA placement is not independently observed, so a
strict runtime-provider NUMA request is rejected and best effort falls back;
an injected provider must advertise and independently observe NUMA binding.
Checked provider-backed stop destroys trace/executor owners and releases tokens
in reverse order; trace is unavailable afterward. M15-04 separately validates
a non-overlapping logical extent ledger for runtime, executor, and device
controls against the immutable `MemoryPlan`, and reports exact, declared-only,
partial, unknown, or not-applicable accounting without treating logical bytes
as allocator commitment or physical residency. Bounded configuring-time
declarations are copied metadata for opaque external/backend resources; they
authorize no mutation and establish no qualification.

Runtime-owned executor, watchdog, and device-service lanes publish live native
stack and guard commitment. Supported Linux stack locking/residency requests
participate in the startup barrier. Stack cleanup runs on the owning quiescent
lane before join and is status-bearing and retryable. A failed device, lane,
stack, or selected-region cleanup retains ownership, prevents provider-token
release, and requires checked `stop()` retry; destruction with unresolved
ownership fails closed.

M16-01 rate metadata is a separate additive C++ model, not runtime-profile
schema 7 or C ABI v8. Up to 64 copied domains use positive integral-nanosecond
periods and 1–64 same-time substeps; finalization rejects malformed ownership,
checked arithmetic overflow, or more than 65,536 reference releases before
start. Fixed-copy inspectors remain immutable and allocation-free after start.
M16-02 additionally freezes CPU-only cross-rate channels, exact initial/wrap/
held/fresh/stale selection metadata, and a two-slot exact-generation SPSC store
per channel. M16-03 optionally activates D0 mandatory-CPU serialized admission
and exact reference dispatch, with checked logical/nominal windows, staged
cross-rate transfer, bounded skip/catch-up/hold/degrade/fail actions, functional
summaries, and canonical checkpoint state. Reference-only behavior is unchanged.
M16-04 admits bounded optional CPU/device domains beside the mandatory-only
admission result, applies mandatory-release hysteresis with deterministic
shed/recovery order, and exposes its unchanged fixed-capacity versioned
rate-action stream. M21-05 adds a distinct mixed-rate action schema and active-
replay artifact; legacy `Runtime::replay()` and every existing artifact remain
byte-compatible.

M16 is complete in merged target history. M17-01 adds a distinct C++ HAL API
version 2 core table to the existing installed device header. Native v2 tables
and exactly-once, address-stable adapters around device-ABI-v1 tables enter one
canonical manager path. The adapter preserves all shared capability,
submission, buffer, completion, health, status, and cleanup fields; malformed
outputs and exceptions fail closed. Adapted v1 retains legacy graph/replay
identity, while native v2 includes a kind/API marker. Adapter storage is
counted once in device control without changing the six-row MemoryPlan. See the
[HAL v2 contract](docs/hal_v2.md). M17-02 appends a separately versioned,
fixed-capacity extension for exactly six memory-domain kinds, topology and
timestamp domains, explicit host-span/opaque registrations, inspection, and
declared running-state timestamp correlation. Core-only v2 and adapted-v1
backends keep one implicit borrowed-host domain and the exact M17-01 path. See
the [heterogeneous-memory contract](docs/heterogeneous_memory.md). M17-03 adds
fixed command batches, same-backend timelines, explicit synchronization, and
isolated per-backend submission lanes. M17-04 adds native registrations for
caller-owned CUDA Graphs and bounded XDMA control/event commands. M17 remains
incomplete; combined execution and physical qualification belong to later
batches.

`step()` remains synchronous to the host, but dependency-ready phases may run
concurrently. The static policy freezes worker placement; the throughput policy
uses local queues and bounded steals; `host_adapter` borrows a declared engine
queue/team. All three use the same task representation for nested
`parallel_for()` and deterministic-tree `parallel_reduce()` calls. See the
[host runtime contract](docs/host_runtime.md), the
[compiled graph contract](docs/compiled_graph.md), the
[executor contract](docs/executor.md), the
[memory-plan contract](docs/memory_plan.md), the
[time/platform contract](docs/time_platform.md), the
[observability contract](docs/observability.md), the
[determinism/replay contract](docs/determinism_replay.md), the
[device backend contract](docs/device_backend.md), and the
[stable C ABI contract](docs/c_abi.md), plus the working
[C](samples/embed_c/mini_app.c) and
[C++](samples/embed_cpp/mini_app.cpp) examples plus the
[mock-device sample](samples/device_mock.cpp). The optional M9 CUDA and M10 XDMA candidates have separate
[CUDA](docs/cuda_backend.md) and [XDMA](docs/xdma_backend.md) backend and
qualification contracts.

## Architecture direction

The target is a domain-neutral runtime rather than a car or physics engine:

1. A host configures phases, resources, callbacks, executor policy, and optional
   device backends.
2. Finalization validates and freezes the graph and creates a bounded memory
   and queue plan.
3. A unified executor runs the compiled graph.
4. Host-driven steps never sleep; finite self-paced execution uses a separate
   absolute-release mode.
5. Devices use a versioned backend interface and release dependent CPU work
   through completion events.

Accepted architecture decisions:

- [ADR-0001: one CPU executor boundary](docs/adr/0001-one-executor-boundary.md)
- [ADR-0002: host-driven time by default](docs/adr/0002-host-driven-time.md)
- [ADR-0003: bounded device backend ABI](docs/adr/0003-device-backend-boundary.md)

The M1–M14 portable runtime plus the 1.2.1 lifecycle-safety closure implements
lifecycle, both explicit time modes,
compiled graph validation, all three CPU policies, the bounded target-path
memory plan, watchdog/degradation, platform preflight, versioned
observability, bounded registered-state replay, and the bounded poll-only
device path. M11 adds the host job-system policy, stable C ABI, controlled
symbols, and installed-package gates. M9 adds an optional CUDA Driver API
backend candidate and M10 adds the bounded
Xilinx Linux XDMA AXI-MM candidate without changing the core device ABI or
claiming a qualified deployment. The existing
`SimCore` demo and legacy scheduler components remain outside that target
path. M12 adds the named support matrix, cross-instance device-isolation gate,
1.x compatibility policy, and checked package-manifest workflow. The
[M15 CPU/memory policy model](docs/cpu_memory_policy.md) adds strict
finalization validation, Linux runtime-owned thread apply/readback, stable
role/category identities, external verify-only ownership, and the bounded
three-region provider/resident transaction without changing steady-state
callbacks. M15-04 adds exact logical control extents, live runtime-stack
accounting and observation, declared-only opaque accounting, and retryable
cross-category cleanup. M15 is complete at the audited baseline. M16-01 adds
the bounded rate-domain/reference-plan boundary and M16-02 adds deterministic
cross-rate selection/storage. M16-03 adds opt-in admission, CPU dispatch,
transfer, and late-frame policy. M16-04 adds optional shedding/recovery and
versioned telemetry; M16 is complete in merged target history. M17-01 adds the
HAL v2 core and complete device-ABI-v1 compatibility path without promoting
later M17 or M18 capabilities. M17-04 adds bounded native vendor commands to
the M17-03 isolated-lane C++ contract without promoting physical memory, DMA,
coherency, clock, hardware, or qualification claims. The
[architecture guide](docs/architecture.md) distinguishes supported and
experimental paths.

## Real-time and determinism language

RTFW separates portable functionality from deployment qualification:

- **RT0:** portable functional behavior; no latency claim.
- **RT1:** best-effort low latency with measured distributions.
- **RT2:** a named Linux PREEMPT_RT hardware/kernel/driver/workload tuple that
  passes strict preflight and published deadline tests.

No RT2 record exists yet.

The offline [qualification contract](docs/qualification.md) defines exact
schema versions, bounds, digest rules, review limits, and proposal commands.
Passing those synthetic/tooling gates is not hardware or RT qualification and
does not edit a support matrix.

Determinism is tiered from D0 (unspecified) through D3 (portable approved
fixed-point/specified math). Release 1.2 supports D0 and an explicit D1
contract for registered canonical state. Its worker-count and compiler-artifact
fixtures do not prove D2, arbitrary floating-point identity, or cross-machine
D3 behavior. Definitions and evidence requirements are in the
[product contract](docs/product_contract.md) and
[determinism/replay contract](docs/determinism_replay.md).

## Repository map

| Path | Purpose |
| --- | --- |
| `include/simcore/` | Current phase runtime, queues, memory, trace, metrics, data-layout, and physics utilities |
| `rt/include/rt/`, `rt/src/` | M1–M16 portable runtime, the 1.2.1 lifecycle-safety closure, M15 CPU/thread policy and selected resident regions, M17-01 HAL v2 core/v1 compatibility, M17-02 heterogeneous-memory/topology controls, M17-03 command/timeline lanes, and the M9 CUDA/M10 XDMA candidates, graph compiler, unified/host executors, strict profiles, memory/time/platform/observability/replay/device controls, and source-only experimental scheduler/fiber/plugin components |
| `hal/`, `gpu/` | HAL and CPU-only device/frame-graph experiments |
| `api/` | Compatibility include for the pre-M1 C header path |
| `src/` | Demo, C shim, platform setup, metrics, and trace utility |
| `tests/` | GoogleTest, integration, and optional fuzz coverage |
| `tools/` | Trace, characterization, scaling, SBOM, and autotune tooling |
| `docs/` | Product contract, ADRs, roadmap, and component notes |

## Build options

The following CMake options are implemented; subproject-only behavior is
called out explicitly:

| Option | Default | Notes |
| --- | --- | --- |
| `ENABLE_LOG` | `ON` | Compile logging support |
| `ENABLE_PROF` | `ON` | Compile profiler support |
| `ENABLE_TESTS` | `ON` (top level only) | Authoritative test switch for a standalone RTFW build |
| `RTFW_BUILD_TESTS` | `OFF` (subproject only) | Explicitly opt a parent `add_subdirectory` build into RTFW tests; the parent's generic `ENABLE_TESTS` is ignored |
| `ENABLE_RAPIDCHECK` | `OFF` | Add RapidCheck property tests when available |
| `SIM_WERROR` | `OFF` | Treat project warnings as errors; CI enables it explicitly |
| `SIM_ENABLE_AVX2` | `OFF` | Experimental global AVX2/FMA compilation; runtime dispatch is planned |
| `SIM_SANITIZERS` | empty | Semicolon-separated compatible sanitizer list |
| `SIM_ENABLE_LTO` | `OFF` | Enable interprocedural optimization |
| `SIM_LTO_THIN` | `OFF` | Request ThinLTO where supported |
| `SIM_PGO` | empty | Experimental `gen`/`use` PGO mode |
| `SIM_BUILD_FUZZERS` | `OFF` | Build the Clang job-queue and snapshot-parser libFuzzer harnesses |
| `RTFW_ENABLE_CUDA` | `OFF` | Build the CUDA Driver API adapter and hardware qualification executable; requires CUDAToolkit |
| `RTFW_ENABLE_XDMA` | `OFF` | Linux only: build the official Xilinx XDMA character-device adapter and destructive AXI-MM qualification executable |
| `RTFW_BUILD_ID` | `rtfw-<version>` | Stable 1–63 character build identifier included in observability and checkpoint provenance |

Platform and optional dependency behavior is described in the
[build guide](docs/build_tooling.md).

## Tests and evidence

CI currently provides:

- Linux GCC/Clang Debug and RelWithDebInfo build/test matrices;
- selected FMA/AVX2 build variants;
- Windows portability builds;
- randomized graph/reference-order, cycle/resource validation, and first-frame
  topology-allocation tests;
- static-assignment, queue-saturation, nested range/reduction, throughput-steal,
  stress, and ThreadSanitizer executor gates;
- aligned phase/task-scratch, memory-budget, overload-policy, and multi-frame
  zero-allocation target-path gates;
- fake-clock absolute-release/deadline/no-drift tests, one-shot watchdog and
  frame-thread degradation tests, and fail-closed preflight tests;
- schema/provenance, interval-partition, trace-loss, instance-isolation,
  contended-emission, stable C ABI v8, JSON-export, and ThreadSanitizer observability
  gates;
- D1 equality across 1/2/4 workers, cross-worker checkpoint transfer,
  transactional corruption rejection, checkpoint/input replay, bounded
  mutation and libFuzzer parser gates, plus exchanged GCC/Clang/FMA artifacts;
- deterministic device saturation/delay/timeout/error/loss/reset/shutdown,
  dependency-release, no-allocation, dynamic C ABI, and sanitizer gates;
- CPU-only fake-driver CUDA queue, transfer, kernel, timeout-quarantine,
  recovery, shutdown, runtime-integration, no-allocation, and TSAN gates;
- an opt-in self-hosted NVIDIA workflow that emits raw per-stage CUDA
  submission/poll/completion samples without automatically creating a support
  claim;
- portable XDMA validation, saturation, timeout-quarantine, reset, retryable
  shutdown, no-allocation, ASan/UBSan, and ThreadSanitizer gates;
- a default portable two-frame simulated CUDA/host-stage/XDMA graph with exact
  operation, timeline, allocation, thread-ownership, failure, recovery,
  isolation, malformed-input, and checked-cleanup gates;
- an opt-in self-hosted XDMA workflow that records deployment identity,
  validates H2C/C2H AXI-MM integrity, and uploads raw per-direction timing
  without automatically creating a support claim;
- shared/static C and C++ compiled-graph samples plus dynamic C ABI loading;
- exact shared-library symbol checks, host-adapter C/C++ saturation,
  stale-completion and no-allocation tests, plus relocated Linux/Windows
  `find_package()` consumers;
- strict allocation-free profile parsing, negative compatibility/config
  cases, generated mapping, real runtime round-trip, and synthetic autotune
  smoke;
- scaling artifact smoke;
- documentation/version/claim checks.

These jobs provide regression evidence, not real-time qualification. Missing
high-risk gates are tracked in [the roadmap](docs/roadmap.md).

## Autotune tooling

The analysis pipeline retains its synthetic CI target, while the default spec
now drives `rtfw_runtime_demo` through the strict profile contract. Its
measurements describe only the built binary, declared workload, and observed
host; they are not portable latency bounds, hardware qualification, or an
automatic deployment recommendation. See
[DOE/autotune status](docs/DOE_AUTOTUNE.md).

The platform hardening scripts make privileged, best-effort host changes. They
are deployment experiments, not library behavior and not evidence of RT2.
Review [real-time hardening](docs/real_time_hardening.md) before running them.

## Contributing

Read the [contributor guide](docs/contributor_guide.md), the
[product contract](docs/product_contract.md), and relevant ADRs before changing
public semantics. New present-tense feature claims need an implementation test;
latency or qualification claims need a named evidence procedure.

## License

Licensed under the [Apache License 2.0](LICENSE).

## Optional host-side benchmarks (M23-01)

The optional M23-01 host-side C++20 benchmark framework provides explicit external providers, a library/CLI, fake/steady clocks, canonical raw/result artifacts and offline validation. It does not require experimental SimCore and does not complete the M23-M26 software program.

See [benchmarking](docs/benchmarking.md) for build, install, provider and artifact contracts.


## CPU benchmarks (M23-02)

M23-02 adds the optional `rtfw.cpu` source provider with 51 graph, executor, memory, queue-pressure and lifecycle cases. Each executes public Runtime operations with independent correctness checks; real timings remain portable characterization. The default build and SDK remain unchanged.
