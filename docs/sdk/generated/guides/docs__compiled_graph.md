[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)

> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.

# Compiled Graph Contract

M22-01 leaves the compiled graph and release order unchanged. After the rate
plan is available, finalization validates copied live-control declarations and
copies the exact legal rate-release target table into optional mailbox control
storage. Frozen schema/legal tables, policy, capacities, and sorted declarations
participate in compatibility identity; staged payloads, arrivals, occupancy,
counters, Runtime identity, and inspector calls do not. M22-02 additionally
hashes its frozen exact-close, order, replacement, missed, reclamation, and view
semantics. It closes immediately before existing dispatch and adds no node,
edge, release, callback, dependency, or dispatch action.

M21-05 finalization retains M21-04's direct sampled-channel maps and adds a
copied closure policy, fixed action/replay controls, and conditional checkpoint
state. Graph/replay identity hashes semantic policy and action/replay rules but
excludes observational ring capacity, caller cursors, addresses, payload bytes,
completion arrival order, and live timing.

Release 0.3 added the M2 compiled graph to the target-path `rt::Runtime`.
The graph is an RT0 functional surface: it validates dependency and logical
resource ordering before start, supplies an immutable deterministic phase order
for introspection, and provides dependency tables to the M3 executor.

Release 0.4 added parallel execution without changing the M2 validation rules.
Release 0.5 includes the committed graph and executor storage in the M4 memory
plan. Release 1.2 runs the same frozen graph from host-driven or finite
self-paced M5 calls. Latency qualification remains deployment-specific work.
M16-01 adds a separate C++ rate-domain kind and reference-plan compiler. M16-02
adds a distinct instance-owned cross-rate channel kind and immutable selection
records. M16-03 can opt into selected CPU record execution without changing the
stable C ABI or reference-only graph execution.

M21-01 lets a HAL-v2 command-batch phase use a separate device-rate binding.
The generic reference order still sorts equal releases by domain registration,
compiled phase, and substep. Device inspection and admission derive from that
order and the copied M17 declaration; no second graph, command list, buffer
list, or timeline list is introduced. Device completion intervals are
half-open, remove completions before equal-time releases, and include prior-
cycle carry. M21-02 maps each device reference directly to that admitted record
and precomputes same-group device-prerequisite slices. Independent admitted
records may remain outstanding together, but a dependent CPU/device record
consumes the exact prerequisite ticket first and a release group cannot settle
successfully until every remaining device ticket is terminal.
M21-03 appends one explicit device selector to the device side of a cross-rate
edge. It names a phase-local copied payload-reference ordinal and stride;
finalization derives and validates every subrange and publishes direct phase/
channel slices. Device-to-device edges and implicit reference matching remain
invalid.
M21-04 adds direct sampled and safe-output slices without changing graph
dependency order. M21-05 attaches closed actions to already compiled direct
indexes; running execution never rescans the configuring registries. Active
replay consumes the recorded release/action order only after complete identity,
extent, checksum, and ordering validation.
Failed admission publishes no partial plan; a fixed-size diagnostic retains the
status, rejected phase, and first reference index for configuring-time repair.

## Graph vocabulary

Each registered callback is one **phase**. Registration can return a
`rt::PhaseHandle`, which is used to add directed dependencies and resource
access declarations. `rt::ResourceHandle` identifies a logical application
resource; the runtime does not own or inspect the application memory described
by it. `write` means the phase may mutate the resource and may also read it;
there is no separate read/write declaration.

Handles carry a runtime-owner token, a phase/resource kind tag, and an object
index. They remain stable for the lifetime of that runtime. Default, forged,
cross-kind, out-of-range, and foreign-runtime handles return
`rt::Status::invalid_handle`.

The C ABI exposes the same encoded values as `rtfw_phase_id` and
`rtfw_resource_id`. Handle encodings are opaque and process-local; they are not
snapshot, replay, or interchange identifiers.

## Construction and finalization

While the runtime is `configuring`, a host may:

1. register callbacks and receive phase handles;
2. register uniquely named logical resources;
3. add a directed `prerequisite -> dependent` edge;
4. declare one `read` or `write` access per phase/resource pair.

The additive C++ configuration path may also register a copied cross-rate
channel between distinct rate-bound phases after registering explicit domain
ownership. CPU/CPU retains M16 behavior. M21-03 permits exactly one device side
when its explicit selector resolves to an admitted host-coherent M21 payload
envelope. Finalization rejects foreign/stale endpoints, same-phase or same-
domain edges, device/device edges, missing/partial selectors, duplicate or
overlapping envelopes, malformed payload/initial bytes, and bounded-capacity/
arithmetic errors before publishing any descriptor or store.

Duplicate names, edges, and phase/resource declarations are rejected. A
self-dependency is rejected immediately. Longer cycles are diagnosed by
`finalize()`.

The 0.3 safety ceiling is 65,536 registered resources; callback phases are
bounded by `callback_capacity`, whose accepted maximum is also 65,536.
Dependency and access-declaration storage can grow during configuration.
Finalization includes their committed capacities in the M4 runtime-control
accounting; configure-time temporary allocation remains permitted.

Finalization is the only graph compile point. It:

1. validates every stored handle and access mode;
2. computes a topological order with registration index as the ready-phase
   tie-break;
3. rejects a cycle;
4. validates every conflicting resource-access pair;
5. commits the compiled order only after all validation and finalization-time
   allocations succeed.

A failed finalization leaves the runtime in `configuring`. A host can add
missing dependency paths and retry. There is no removal API in 0.3, so a host
that submitted a longer cycle must recreate its runtime.

After successful finalization, callback/resource registration, dependency
addition, and access declaration all return `invalid_state`. The compiled
order remains available through `compiled_phase_at()` in finalized, running,
and stopped states.

## Rate ownership and reference order

An explicit rate model copies up to 64 named domains while configuring. Each
CPU or device phase must bind to exactly one instance-owned `RateDomainHandle`,
and every domain must own a phase. Finalization rejects malformed or duplicate
domains, missing/rebound/foreign/stale handles, empty ownership, arithmetic
overflow, or more than 65,536 reference releases without committing a partial
plan. A rejected plan leaves the runtime configuring and correctable.

The immutable reference interval is `[0, lcm(periods))` relative to epoch zero.
All domain releases are integral nanoseconds; substeps share their domain
release timestamp. Equal timestamps order by domain registration, this
contract's compiled phase order, then substep ordinal. Reduced period ratios
are exact against registration-order domain zero. Without the execution policy,
`step()` and `run_periodic()` still submit the complete compiled graph once per
frame. Active plans repeat this exact order by checked supercycle
arithmetic. They permit ordinary dependencies only within one domain and run
each selected CPU phase serially through the existing executor without
releasing graph successors. Late actions operate on a whole domain release;
mandatory-only admission excludes optional budgets. Optional CPU domains start
active and shed by increasing criticality then reverse registration, with
recovery reversing that order. A transition after a settled mandatory release
affects the next record in this same total order. Optional cross-rate endpoints
are rejected so omission cannot change generation selection.

## Resource ordering

Resource declarations prevent an incidental traversal from becoming an unsafe
parallel graph. For every pair of phases that names the same resource:

| First access | Second access | Required ordering |
| --- | --- | --- |
| Read | Read | None |
| Read | Write | A dependency path in either direction |
| Write | Read | A dependency path in either direction |
| Write | Write | A dependency path in either direction |

The path may be direct or transitive. A deterministic topological tie-break is
not an ordering declaration: if neither phase reaches the other, `finalize()`
returns `resource_conflict` and identifies the resource and both phases.

This is a validation contract, not automatic synchronization. Hosts must
declare every logical resource that can race, and callbacks must honor those
declarations when touching application memory.

## Execution and allocation boundary

`step()` submits dependency-ready phases to the M3 worker team and waits
synchronously from the host. Trace callback indices remain phase registration
indices. `compiled_phase_at()` still returns the registration-index
tie-broken canonical topological order, but independent callbacks may overlap
and their completion order is not that canonical total order.

Graph compilation, adjacency construction, cycle detection, and resource-path
analysis may allocate and may have data-dependent cost during `finalize()`.
The committed step path does not build, sort, or mutate topology. An allocation
instrumentation test covers the first frame of a representative compiled graph
and observes no heap allocation.

The M4 gate extends that evidence across multiple complete frames with
independent phases, range work, fixed-tree reductions, task scratch, and
tracing. `bounded_memory_plan` is therefore true for the target CPU runtime.
Arbitrary callbacks can still allocate or block. Executor semantics are
specified in the [M3 executor contract](docs__executor.md), and accounting scope is
specified in the [memory-plan contract](docs__memory_plan.md).

## C and C++ entry points

The preferred C++ graph calls are:

- `Runtime::register_callback(registration, out_phase)`;
- `Runtime::register_resource(name, out_resource)`;
- `Runtime::add_dependency(prerequisite, dependent)`;
- `Runtime::declare_resource_access(phase, resource, access)`;
- `Runtime::finalize()`.

The C equivalents are:

- `rtfw_register_phase`;
- `rtfw_register_resource`;
- `rtfw_add_dependency`;
- `rtfw_declare_resource_access`;
- `rtfw_finalize`.

The M1 `register_callback` / `rtfw_register_callback` forms remain available
for independent phases that do not need a returned handle. Stable C ABI v8
freezes this graph surface under the M11 compatibility policy.

## Evidence

- Compiler: `rt/src/compiled_graph.cpp`
- Rate compiler: `rt/src/rate_timeline.cpp`
- Cross-rate compiler/store: `rt/src/cross_rate_data.cpp`
- Active admission/dispatch compiler: `rt/src/rate_dispatch.cpp`
- Rate-action telemetry: `rt/src/rate_telemetry.cpp`
- Runtime integration: `rt/src/host_runtime.cpp`
- C++ graph tests: `tests/test_compiled_graph.cpp`
- Rate/reference tests: `tests/test_rate_timeline.cpp`
- Cross-rate selection/storage tests: `tests/test_cross_rate_data.cpp`
- Active admission/dispatch tests: `tests/test_rate_dispatch.cpp`
- Shedding/telemetry tests: `tests/test_rate_telemetry.cpp`
- First-frame allocation instrumentation: `tests/test_trace_noalloc.cpp`
- Dynamic C ABI coverage: `tests/test_cabi_dlopen.c`
- C and C++ embedding samples: `samples/embed_c/mini_app.c`,
  `samples/embed_cpp/mini_app.cpp`
