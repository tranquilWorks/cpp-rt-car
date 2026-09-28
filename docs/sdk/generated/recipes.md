# Installed SDK recipes

This manual is the consumer entry point shipped with the SDK. The generated
API reference contains all 20 default installed headers and three clearly marked
optional-component headers. Its selected entry-point index links to exact
line-numbered declarations and comments, including complete header source.
It is a source reference, not a C++ semantic parser; private declarations and
`detail` helpers visible in headers do not become supported public interfaces.

## Build and installation

CMake 3.20+, a supported C++20 compiler and the installed package are sufficient
for compiling recipes. Python 3 is needed only for the optional transcript
runner. No documentation generator, Doxygen, network fetch or private header is
needed by a consumer. Developer generation uses Python's standard library.

The default installation puts this manual in `share/rtfw/manual` and the recipe
project in `share/rtfw/examples/recipes`. Use your configured `CMAKE_INSTALL_DATADIR`
and `CMAKE_INSTALL_INCLUDEDIR` when they differ; never add a checkout include path.
After `find_package(rtfw ...)`, `RTFW_DATA_DIR` and `RTFW_INCLUDE_DIR` identify the
actual relocated directories. Quote paths containing spaces.

Build the unchanged source checkout and install a tests-OFF SDK as described in
[getting started](guides/docs__getting_started.md). CPack uses those install rules to put
the same manual, reference and source files into release archives. The archive
verification removes the original prefix before compiling the extracted SDK.
An archive produced by a local test is not a signed or published release.

From the installed recipe directory, the portable transcript entry point is:

```text
python3 run_transcripts.py --prefix /absolute/path/to/sdk --build /absolute/path/to/recipe-build
```

Use `--source /absolute/path/to/rtfw` to exercise `add_subdirectory` instead of
`find_package`. The shipped recipe sources are still used. Add `--benchmark ON`
only with an SDK built with `RTFW_BUILD_BENCHMARKS=ON`; source embedding builds
that component when requested. The default is OFF, and importing the default
Runtime does not import the benchmark library. The exact argv vectors are
published below from the same JSON inventory that the runner executes. Each
command must return zero, and the final CTest output must confirm success.
CMake/CTest also work directly without Python.



Each line below is an argv array, not a shell string. Replace `{kit}`, `{build}`, `{prefix}`, `{source}` (empty for installed mode) and `{benchmark}` (`OFF` or `ON`) with explicit paths/options.

```json
[
  {
    "argv": [
      "cmake",
      "-S",
      "{kit}",
      "-B",
      "{build}",
      "-DCMAKE_BUILD_TYPE=Release",
      "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
      "-DCMAKE_PREFIX_PATH={prefix}",
      "-DRECIPE_RTFW_SOURCE={source}",
      "-DRECIPE_WITH_BENCHMARK={benchmark}",
      "-DRTFW_BUILD_BENCHMARKS={benchmark}",
      "-DENABLE_TESTS=OFF",
      "-DRTFW_BUILD_EXAMPLES=OFF",
      "-DRTFW_BUILD_EXPERIMENTAL=OFF",
      "-DRTFW_BUILD_RUNTIME_DEMO=OFF",
      "-DSIM_SANITIZERS=",
      "-DSIM_WERROR=ON"
    ],
    "contains": "Generating done"
  },
  {
    "argv": [
      "cmake",
      "--build",
      "{build}",
      "--config",
      "Release",
      "--parallel",
      "2"
    ],
    "contains": ""
  },
  {
    "argv": [
      "ctest",
      "--test-dir",
      "{build}",
      "-C",
      "Release",
      "--output-on-failure",
      "--no-tests=error",
      "-R",
      "^recipe_"
    ],
    "contains": "100% tests passed"
  }
]
```


## Recipe map and observable results

| Recipe CTest | Shipped source | What actually runs |
| --- | --- | --- |
| `recipe_raw_c` | [C consumer](../examples/recipes/package_consumer/c_consumer.c) | Stable C ABI lifecycle, version and fingerprint checks through `c_static` |
| `recipe_raw_cpp` | [C++ consumer](../examples/recipes/package_consumer/cpp_consumer.cpp) | Public Runtime, cooperative host adapter and HAL-v2 integration checks |
| `recipe_profile` | [Profile consumer](../examples/recipes/package_consumer/profile_consumer.cpp) | Parse an explicit JSON profile and verify schema, executor and worker settings |
| `recipe_mixed_rate` | [Driver](../examples/recipes/package_consumer/mixed_rate_consumer.cpp), [scenario](../examples/recipes/mixed_rate_conformance.hpp) | Actual CPU/device rates, sampled I/O, safe acknowledgements, active replay and exact accounting |
| `recipe_live_control` | [Control consumer](../examples/recipes/package_consumer/live_control_consumer.cpp) | Stage bounded data, inspect publication and callback-local state |
| `recipe_replay`, `_replaced`, `_rejected` | [Replay consumer](../examples/recipes/package_consumer/live_control_replay_consumer.cpp) | Checkpoint, active/live replay, exact application state and admission counters, zero steady allocation |
| `recipe_cuda_boundary` | [CUDA surface](../examples/recipes/package_consumer/cuda_consumer.cpp) | Version constants at runtime; API construction/encoding compiles but the guarded driver branch does not execute |
| `recipe_xdma_boundary` | [XDMA surface](../examples/recipes/package_consumer/xdma_consumer.cpp) | Version/capacity constants at runtime; guarded driver construction does not execute |
| `recipe_benchmark` (optional) | [Provider](../examples/recipes/package_consumer/benchmark_consumer.cpp) | Fake-clock provider registration, seven invocations, five retained samples, encoding and unregister |

These source files are reused unchanged from the public-package conformance
suite. The advanced mixed-rate scenario is deliberately larger than the
[57-line hello](../examples/hello_runtime/main.cpp); start with hello, then use
these recipes to study checked integration. The scenario's companion header is
shipped with its original relative layout. None requires test-framework or
repository-private SDK headers. A successful process exit means its explicit
state/status assertions passed; the boundary recipes do not claim driver work.

The mixed-rate oracle requires CPU/device callback counts `9,6,4,4,2`, ten
terminal device actions, 23 sampled publications and 16 selections. Startup and
shutdown safe states must be acknowledged; active replay and memory accounting
must match. These are software loopback results, not physical HIL.

Replay ends at application state 3 with two callbacks and zero tracked steady
allocations. The default exercises format v1; replacement/rejection cases opt
into v2 so admission history is retained. Payload-bearing artifacts are trusted
inputs, not arbitrary process snapshots or a security sandbox. See
[determinism/replay](guides/docs__determinism_replay.md) and [live control](guides/docs__live_controls.md).

## Lifecycle, ownership and capacity

Create clocks, callbacks, state buffers and providers before the Runtime that
borrows them. Configure explicit capacities, register resources/graph/policies,
then check `finalize()` and `start()`. Run finite frames and inspect every status.
Stop new frame admission, quiesce external work, check `stop()`, and only then
release borrowed owners. A failed stop keeps ownership alive for an explicit
retry. Callback failure can leave application effects; Runtime does not promise
application rollback. Do not use destruction as a substitute for checked stop.

Use [typed builders and checked-stop guards](guides/docs__typed_runtime.md) when useful;
they preserve raw validation and explicit budgets. A guard terminates on
unresolved destructor cleanup. Account separately for Runtime-resident memory,
host arenas/stacks and control-plane allocations. The [memory plan](guides/docs__memory_plan.md)
and [host reference](guides/docs__host_adapter.md) describe those boundaries.

For multiple rates, separate logical release periods from physical clocks,
freeze release/queue/sample capacities before execution, and declare overload,
freshness and underrun policy. Timestamp domains and terminal device completion
must agree before publishing sampled data. Enqueue alone is not an acknowledged
safe state. See [host Runtime](guides/docs__host_runtime.md), [sampled I/O](guides/docs__sampled_io.md)
and [rate telemetry](guides/docs__rate_telemetry.md).

## Integration recipes

The earlier installed source kits remain independent buildable projects:
[hello](../examples/hello_runtime/CMakeLists.txt),
[typed Runtime](../examples/typed_runtime/CMakeLists.txt),
[backend authoring](../examples/backend_authoring/CMakeLists.txt),
[external CIL](../examples/cil_shared_memory/CMakeLists.txt),
[independent host](../examples/host_adapter/CMakeLists.txt), and
[CUDA physics](../examples/cuda_physics/CMakeLists.txt).
Their installed guides describe their commands and exact output/failure oracles.

The backend kit exercises fixed copy-profile registration, completion, reset,
faults and checked cleanup. The CIL kit starts real controller/plant processes
with bounded shared memory, generation/sequence/timeout and reconnect rules.
The independent host owns jobs, memory, clock and telemetry for two Runtime
instances in native and host-adapter modes. These are portable integration
references, not universal transports, schedulers or an Unreal implementation.

Use the actual [CUDA physics/pipeline/lifetime guides](guides/docs__cuda_physics.md) for
portable simulated workloads and optional real sessions; the boundary recipe
above alone does not exercise a device. [XDMA](guides/docs__xdma_backend.md) requires the
named Linux driver/device setup for real access. Physical commands in archived
guides are conditional instructions, not commands exercised by this batch.
No portable check establishes peer DMA, electrical behavior, CUDA/XDMA/HIL,
RT1/RT2 or controlled latency. Keep software loopback and real-device evidence
separate.

## Measurement and diagnostics

The optional benchmark recipe uses a fake clock to prove provider lifecycle and
result encoding. It does not measure throughput or latency. For real measurement,
follow [benchmarking](guides/docs__benchmarking.md) and [offline analysis](guides/docs__benchmark_analysis.md),
retain raw samples/identity and report portable characterization separately from
controlled performance. No benchmark result overrides a functional failure.

| Symptom/status | Next action |
| --- | --- |
| CMake cannot find `rtfw` | Point `CMAKE_PREFIX_PATH` to the extracted installation root, not a library or source directory |
| Optional component unavailable | Build/install the requested component; do not silently substitute simulated hardware |
| `invalid_config`, `capacity_exceeded`, `scratch_exhausted` | Inspect the rejected config/plan and size fixed resources before finalization |
| `queue_full`, `device_queue_full` | Handle bounded backpressure; rejected work has not transferred ownership |
| `invalid_state`, `invalid_handle` | Check lifecycle, Runtime identity and configuration generation |
| `callback_failed`, `clock_failure` | Stop admission and inspect application/clock state; do not claim automatic rollback |
| `device_timeout`, `device_lost`, `device_reset_required` | Retain unresolved resources and follow backend recovery/checked-stop rules |
| `invalid_artifact`, `incompatible_artifact` | Check trusted format/version/identity and compatibility; never force acceptance |

The generated [Status source](api/rt__status.hpp.html) and
[typed diagnostics](api/rt__sdk.hpp.html) are the authoritative names and
helpers. A bounded diagnostic buffer may truncate; failure is not converted to
success by logging it.

## Migration and claims

The raw API and compatibility aliases remain supported within the documented
1.x policy. Prefer `rtfw::runtime` for new C++ consumers and explicit `c_static`
or `c_shared` for C. C ABI v8/SONAME 8 and device ABI v1 remain stable; a stable
C++ binary ABI is not promised. Rebuild C++ consumers with a compatible toolchain
and use the [C ABI guide](guides/docs__c_abi.md) for language/plugin boundaries. Optional
helpers and source kits add no default exported target or private include path.
Legacy experimental SimCore examples are not the supported SDK entry path.

M25 software delivery does not establish the independent unfamiliar-consumer
walkthrough, M26 full-system composition/audit, physical qualification, Unreal,
signing or release approval. Those gates remain separate and unperformed here.
See the [release policy](guides/docs__release_policy.md). Archived guides retain historical
milestone/evidence statements; this recipe index states the current bundle scope.
