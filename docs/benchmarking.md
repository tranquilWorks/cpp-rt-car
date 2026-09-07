# Optional host-side benchmarks (M23-01 and M23-02)

The C++20 `<rtfw/benchmark.hpp>` API and `rtfw-bench` CLI are an optional,
instance-local host-control component. `RTFW_BUILD_BENCHMARKS=OFF` preserves
the default M22 SDK inventory. The component is not a Runtime dependency,
adds no Runtime lane, and introduces no C ABI or C++ binary-ABI promise.
The supported release remains 1.2.1 at RT0. M23/CAP-M23 remain incomplete.

## Build, run, validate

```sh
cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF -DRTFW_BUILD_BENCHMARKS=ON -DRTFW_BUILD_EXPERIMENTAL=OFF
cmake --build build/bench --parallel 2
build/bench/bench/rtfw-bench list
build/bench/bench/rtfw-bench describe --provider rtfw.self --case structural --clock fake
build/bench/bench/rtfw-bench run --provider rtfw.self --case structural --clock fake --output build/bench-evidence
python3 tools/check_benchmark_artifact.py --artifact-root build/bench-evidence
```

`list` prints the sorted 51 `rtfw.cpu` cases and `rtfw.self:structural`. The fake-clock self case performs two
warm-up and five measured calls, each with 16 operations and correctness token
120. Its 100 ns fake increments produce 100 ns per sample and 500 ns total.
These are **structural_fixture** values, not performance measurements. The
external consumer fixture instead uses 10 ns increments and one operation.
Replace `fake` with `steady` for **portable_characterization** on the calling
host. There are no elapsed-time pass/fail thresholds or speedup claims.
Use a new output directory for every run. Parents must already exist.
The install recipe deliberately uses `ENABLE_TESTS=OFF`: a test-enabled build
can also install GoogleTest and is not the supported clean SDK packaging path.

The CLI accepts only `list`, `describe`, and `run`, exact provider/case IDs,
`--clock fake|steady`, and (for run only) `--output`. Unknown/duplicate options,
unknown selections, and pre-existing destinations fail before invocation.
Exit 0 means a successful run or command, 3 means valid NOT RUN, 1 means a
case/publication failure, and 2 means invalid command input. The offline
validator returns 0 for a valid successful bundle, 3 for valid NOT RUN, and 1
for failed or invalid evidence. Inspect the retained status, not file presence.

## External providers and ownership

`ProviderV1` has an exact size/version prefix, positive implementation version,
bounded provider ID and case count, borrowed user pointer, description callback,
invocation callback, and zero reserved fields. Register explicitly on a `Runner`.
Registration copies the table and all validated descriptors transactionally.
Callable/user lifetimes remain borrowed until unregister or Runner destruction;
no callback is retained elsewhere. Handles must not outlive their Runner.
Unregister invalidates its generation; foreign and retired handles are rejected.

Discovery sorts provider then case identifiers, independently of registration
order. Parameters sort by name; counter declaration order is preserved because
it defines the observation vector. An invocation receives its copied case ID
and an ordinal (warm-ups first), and returns exactly the declared counters, a
bounded correctness token, and a correctness boolean. Descriptions are frozen
at registration. No case can alter selection or the runner's clock policy.

Each Runner is single-owner: do not call one instance concurrently. Recursive
registration, execution, and unregister fail busy during callbacks. Independent
Runners may run concurrently and have no shared registry. There is no static
constructor discovery, dynamic loader, environment plugin path, filesystem
provider scan, hidden thread, or Runtime-private header. Providers are trusted
native code, not a sandbox: callbacks must return and obey their declared
lifetimes. An exception becomes a bounded failure diagnostic; its potentially
sensitive exception text is never retained. Arbitrary native-code UB cannot
be made safe by a size/version prefix.

The library may allocate, hash, parse or write only on the invoking host-control
thread. It must not be called on an RT executor/device/backend lane. Timing
wraps invocation, not registration, descriptor parsing, hashing, artifact I/O,
or warm-up. Callback overhead and the observation writes themselves are part
of the measured region; no hidden subtraction or outlier removal is applied.

## Closed version-1 schemas and bounds

[Descriptor schema](../bench/schemas/descriptor.schema.json) binds provider and
case identity/version, subsystem, implementation/configuration, workload kind
and SHA-256, integer parameters/ranges, counters/units/ranges, exact warm-up and
repetition counts, clock/unit, all-measured raw retention, evidence class, and
comparison policy `none`. Correctness and counter ranges are structural
invariants, not performance thresholds.

[Result schema](../bench/schemas/result.schema.json) includes the raw schema in
`$defs.raw`. It binds the descriptor and raw filenames/digests, run-context
identity, runner/schema versions, source/build/host/policy/backend/workload
identity, UTC metadata, clock/evidence class, complete/partial counts, status,
bounded closed diagnostic, summary, and a result self-digest. All fields are
closed and required; unavailable optional facts are explicit strings.

Bounds are 32 providers, 256 cases total, 16 parameters and 16 counters per
case, 1,000 warm-ups, 1–10,000 measured repetitions, 64-byte ASCII identifiers,
128-byte sanitized labels, 32 MiB per artifact, depth 32, and integers from zero
to 2^63−1. Provider versions fit uint32. Version 1 deliberately has no floating
point, negative values, NaN, infinity, or arbitrary string payload. Units with
fractional meaning must use a declared integer scale encoded in configuration.
Unknown versions/fields/enums, duplicate keys/identities, invalid ranges and
unsafe labels fail closed. Counter/time aggregate overflow is a failed run.

All artifacts are ASCII (a canonical subset of UTF-8), object keys sorted,
compact separators, standard JSON escaping, exact integers, and one final LF.
Array order is semantic. Percentiles use nearest rank `ceil(p*N/100)-1` in
sorted durations; no interpolation. Statistics include min/max/p50/p95/p99,
total measured nanoseconds, and exact counter totals. Failed runs retain only
already validated measured samples, never a success summary. A callback may
return NOT RUN only before any warm-up or sample has succeeded.

File SHA-256 values cover the complete file including LF. Run-context SHA-256
covers compact canonical `{descriptor_sha256,identity,start_utc}` without LF.
`result_sha256` covers canonical result JSON **excluding** that field, with LF.
These detect accidental/malformed substitution; they are not signatures,
authentication, qualification, or proof against someone rewriting all hashes.
The offline validator recomputes schemas, context, hashes, counts, timelines,
invariants and summary arithmetic. It rejects trailing bytes, noncanonical
JSON, missing/unlisted files, mutation, symlinks and cross-run substitution.

## Clock and privacy boundary

`ClockV1` supplies the runner's monotonic nanosecond reads. Reads must fit the
integer bound and never go backward across warm-up or measured calls. Fake
clock UTC is the explicit fixed fixture epoch; captured real identity still
varies between hosts, so CLI fake artifacts are not claimed identical across
hosts. Determinism tests use the external consumer with an explicit constant
fixture identity; measured real timestamps never enter byte-equality gates.

Automatic identity capture is an allowlist: compiler/version/configuration,
SHA-256 of build flags, source commit/tree/dirty state at configure time,
OS/kernel/architecture, logical CPU count, CPU model, memory/page size when
available. Linux reads only the CPU model-name field, never CPU serials.
Windows unsupported kernel/model observations remain unavailable. Frequency,
temperature, policy and backend/driver observations are never guessed.
Reconfigure after editing sources before collecting evidence. A dirty flag
means the commit alone cannot reproduce that measurement; source archives
without Git metadata say unknown. Build flags and their potentially private
paths are hashed, not emitted. The digest includes configuration flags and
current directory/target compile definitions/options and link options, excluding
the self-referential identity macros. Add-subdirectory builds report unknown
flags because a parent may change the target after configuration. The caller is responsible for ensuring declared
host/policy/backend labels and correctness tokens are nonsensitive.

No hostname, username, home/checkout/temporary paths, environment values,
command lines, network/MAC addresses, serial/machine IDs, cloud metadata,
credentials, raw process listings, or proprietary payloads are collected or
emitted. Sanitized labels are data, not authentication. No driver, device,
network probe, privilege, scheduling/affinity/NUMA/locking/governor/power change,
or automatic hardware execution is performed.

## Failure-atomic publication

A completed in-memory Result publishes `descriptor.json`, `raw.json`, and
`result.json` as one new directory. Linux pins ancestors with directory handles,
uses no-follow/exclusive file opens and file/directory flushes, then
`renameat2(RENAME_NOREPLACE)`. Windows pins checked non-reparse ancestors and
renames the completed directory by handle with replacement disabled. Unsupported
filesystems/platforms fail; there is no racy check-then-replace fallback. A
concurrent winner or a pre-existing empty directory is never overwritten.

Failed writes/flushes/rename remove private staging files. Final-name readers
see either the complete set or no set. A process crash may leave a private
`.rtfw-bench-*` staging directory, never accepted as a final bundle. This is
visibility atomicity, not a claim of crash/power-loss durability or protection
against an administrator or the same account deliberately rewriting evidence.
No symlink/junction is followed in the output path or offline artifact read.
Windows UNC/device/alternate-stream paths and parent traversal are rejected.

## Installed and add-subdirectory consumption

```sh
cmake --install build/bench --prefix build/bench-install
cmake -S tests/package_consumer -B build/bench-consumer -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/build/bench-install" -DRTFW_TEST_BENCHMARK=ON
cmake --build build/bench-consumer --parallel 2
ctest --test-dir build/bench-consumer --output-on-failure
```

Request `find_package(rtfw CONFIG REQUIRED COMPONENTS benchmark)` and link only
`rtfw::benchmark`. Unrequested components do not import the optional target.
The optional install includes the public header, static library, CLI, schemas,
offline validator, this guide, and the external provider source under
`share/rtfw/bench/examples`. Default builds install none of these additions.
Only `cxx_std_20` and the include path propagate: project warnings, internal
feature macros, Runtime and experimental dependencies do not.

For embedding, enable `RTFW_BUILD_BENCHMARKS`, then add the RTFW source directory
with `add_subdirectory` and link `rtfw::benchmark`. The test project in
`tests/benchmark_fixtures/consumer` exercises this without private headers.
External providers need no runner fork. The historical AoSoA/SoA benchmark is
still available only with experimental builds and remains **unpromoted**.

## Verification and remaining work

`m23_benchmark` runs schema/table/runner/artifact C++ tests;
`m23_benchmark_artifact_validator` runs offline adversarial/CLI tests;
`benchmark_package_consumer` runs a separate explicitly registered provider.
Canonical external-provider artifacts are exchanged across GCC, Clang, FMA
variants and MSVC. Existing sanitizers, ABI, package, release, no-allocation and
portable-assurance gates remain required. Public API/privacy/portability and
claim review remain explicit human gates; CI does not fabricate those reviews.

M23-02 adds the CPU/graph/executor/memory cases described below; M23-03 owns multi-rate,
control/replay/observability cases; M23-04 owns HAL/CUDA/XDMA/pipeline providers;
M23-05 owns baseline comparison/noise/confidence/threshold policy. M24 CUDA
physics, M25 general SDK experience, and M26 golden-system composition remain
separate. No controlled-host performance, optimization/near-optimality,
hardware support, HIL, RT1/RT2, Unreal, signing, release, deployment, or
production evidence is established by this component.


## CPU provider (M23-02)

`rtfw.cpu` is an explicit instance-local ProviderV1, implementation version 1.
It uses only installed Runtime APIs. The runner still has no Runtime link edge;
the CPU source example and CLI explicitly link both components. Discovery and
describe only copy metadata. The CLI validates input and output destination
before preparing the selected case. Preparation may allocate and create the
configured workers on the host control thread. It never prepares other cases.

```sh
build/bench/bench/rtfw-bench describe --provider rtfw.cpu --case host-adapter-64 --clock fake
build/bench/bench/rtfw-bench run --provider rtfw.cpu --case host-adapter-64 --clock steady --output build/cpu-evidence
python3 tools/check_benchmark_artifact.py --artifact-root build/cpu-evidence
```

The finite [inventory](../bench/fixtures/cpu_cases.json) has 51 cases, two warm-ups
and five retained samples each. It covers the thirteen contracted areas; memory
and native residency use separate catalog family labels. Bounds are four
workers, 64 phases, 4096 entities, 256 nested tasks and 16 parameters/counters.
Cases are representative workload points, not maximum-capacity certification.

| Area | Cases / geometry | Independent checks |
| --- | --- | --- |
| Compile/finalize | Chain, wide DAG and invalid cycle, 4/32 phases | Compiled order/count/dependencies; exact cycle status; no callbacks/workers |
| Empty dispatch | 1/32 callbacks | Exactly one visit per phase, zero entity work |
| Width/depth | 4/32 phases, 64 entities each | Dependency order and per-phase integer sums |
| Grain | 1/16/64 over 128 entities | Exact accepted ranges without overlap or gaps |
| Workers/policy | 1/2/4, static and throughput, 1024 entities | Completed work includes local executions and successful steals |
| Nested for/reduce | Two depths, 64 entities | Public nested operations, fixed partials, integer oracle |
| Entity scaling | 64/1024/4096 | Per-index outputs and exact completed counts |
| Queue pressure | Host queue capacity 2/8 | Accepted prefix, one real rejection, all accepted work settled, recovery submission |
| Host adapter | 64/1024, caller-thread FIFO | Exact native counterparts: entities-64 / workers-static_workers-1; explicit bounded fixture |
| Trace | Capacity 0/256, four phases | Identical output; observed emitted/overwritten/dropped deltas |
| Memory/residency | Scratch 64/256; inactive zero; native 4096/8192 | Six-row MemoryPlan equation; ownership, policy results and cleanup |
| Multiple Runtime | Two live owners, 64/1024 | Independent counters/work; one stops while the other continues |
| Lifecycle | 1/4 fresh objects per invocation | Configure/finalize/start/step/checked-stop on each fresh instance |

All integer leaf work computes `3*i+1`; the independent sum is
`n*(3*n-1)/2`. Parallel range cases verify every output and visit, deterministic
reduce verifies its fixed partial reduction, and graphs check each phase's sum.
No counter substitutes declared entity size for completed work. The queue case
uses actual Runtime admission with a host-owned FIFO; capacities 2/8 accept that
many children, reject the next, drain the prefix, then accept a recovery task.
The rejected child never runs. Native throughput scheduling is not assumed to
have deterministic ordering or steal counts.

### Timing, counters and ownership

`step-and-observation` includes reset, the full public Runtime step, oracle and
counter inspection, and observation writes. Configuration/start are prepared
before the runner, and checked stop happens before publication.
`lifecycle-setup-inspect-cleanup` includes construction, configuration,
registration, finalize, inspection and cleanup; memory/lifecycle cases also
include start and step when their expected status permits. Compile cases are
**not finalize-only timings**. The unchanged ProviderV1 cannot subtract those
costs. Multiple-owner continuation after stopping the second owner is a checked
post-run assertion outside measured samples.

`operations`, `phase_calls`, `range_calls`, `submitted`, and `rejected` are
per-invocation counts. Non-host cases additionally expose observed worker-start
and MemoryPlan gauges, provider acquire/apply/observe/rollback/release counts,
residency gauges, and trace event deltas. The inventory declares aggregation and
availability. Runner totals sum all five samples: summed gauges are neither
peaks, RSS, leaks nor a count of workers created during a steady step. Worker
start counters advance at loop entry, which can be observed after start returns. Default
or native memory has no host-provider callbacks, so its host callback counts
are zero; this does not mean Runtime acquired no memory. Host FIFO storage and
fixture arrays are not Runtime-owned MemoryPlan bytes.

The bounded simulated provider reports explicitly simulated policy/residency
facts. Acquire failure, apply failure and one failed rollback followed by a
checked retry exercise actual Runtime ownership. Inactive regions acquire
nothing. Native residency is opt-in Linux process-local base-page/prefault
behavior, with locking, pinning and huge pages disabled. It changes no affinity,
NUMA, governor or system configuration. An unavailable native probe yields
NOT RUN before successful warm-up; it never becomes measured zero residency.
Successful stop is terminal; repeated lifecycle always constructs a fresh
Runtime. Unexpected unresolved teardown preserves ownership until retry; an
unresolved destructor terminates instead of freeing borrowed live resources.
The CLI publishes no success bundle if post-run cleanup fails.

Fake clocks exercise structural outcomes only. The fixed-identity installed
CPU consumer uses the deterministic host-adapter case and just its five
portable operation counters for byte equality across compilers, FMA settings
and Windows. Native MemoryPlan sizes, residency and scheduling observations
are excluded from that cross-platform fixture. Real-clock results remain
portable_characterization with comparison policy `none`, no timing threshold,
noise filter, confidence claim, or optimality conclusion.

### CPU source example and tests

The optional clean install includes `cpu_provider.hpp`, `cpu_provider.cpp`,
`cpu_cases.json`, and `benchmark_cpu_consumer.cpp` under
`share/rtfw/bench/examples`. The provider is source, not another exported target
or C++ binary ABI. Request `find_package(rtfw CONFIG REQUIRED COMPONENTS runtime
benchmark)`, compile those example sources, include their directory, and link
`rtfw::runtime` plus `rtfw::benchmark`. The standalone project in
`tests/benchmark_fixtures/cpu_consumer` supports either `CMAKE_PREFIX_PATH` for
the relocated installation or `RTFW_SOURCE_DIR` for embedding.

`BenchmarkCpu.*` covers metadata, every case, oracles, queue recovery, memory
faults, repeated runs, concurrent independent owners and clock-failure cleanup.
`m23_benchmark_cpu_inventory` checks exact discovery/descriptors, all artifacts
with the unchanged validator, collisions, invalid selections and a real clock.
`m23_benchmark_cpu_noalloc` guards full steady invocations on prestarted workers
with a pre-reserved output vector; host registration, runner serialization and
lifecycle allocation are deliberately outside that claim.
`benchmark_cpu_package_consumer` verifies public source consumption. The CPU
package wrapper runs every unchanged M23-01 packaging check before the additional
installed-source, relocated and add-subdirectory CPU consumers.
