# HAL and device benchmarks

`rtfw.device` is an optional source-example provider. It exercises the public
CUDA/XDMA candidates and Runtime; it does not replace their schedulers or drivers.
The exact finite catalog is `bench/fixtures/device_cases.json`. Existing CPU,
Runtime and framework providers remain available unchanged.

Build and inspect:

```sh
cmake -S . -B build -DRTFW_BUILD_BENCHMARKS=ON -DENABLE_TESTS=ON
cmake --build build --parallel 2
build/bench/rtfw-bench list
build/bench/rtfw-bench describe --provider rtfw.device --case cuda-graph-4096
build/bench/rtfw-bench run --provider rtfw.device --case pipeline-kernel-4096-frames-4 --clock steady --output new-results
python3 tools/check_benchmark_artifact.py --artifact-root new-results
```

The ordinary CLI uses declared benchmark-owned protocol drivers and fixed local
storage. Fake-clock artifacts are `structural_fixture`; steady-clock artifacts
are `portable_characterization` of that protocol fixture on the named host.
Neither is physical CUDA/FPGA characterization. Driver identity explicitly says
`benchmark-owned-protocol-fixture`. Real descriptors without supplied sessions
produce `not_run`, exit 3, and zero warmup/measured samples. Invalid supplied
driver tables and failed initialization are errors, not missing prerequisites.

## Cases and timing

- HAL v1/v2 capability and native topology inspection performs no initialization
  or device I/O. It checks capacities, topology, malformed output structures and
  absence of driver activity.
- CUDA transactions upload/download, copy device storage, memset, launch an
  integer increment kernel, or dispatch a caller-instantiated increment graph.
  Graph commands retain stable ID 7 and named buffer bindings. Every transaction
  checks exact output at 64 or 4096 bytes after terminal completion.
- XDMA transfers use actual prestarted backend I/O workers and bytewise output
  checks. Control and event cases use only the declared local protocol map.
  Capacity cases hold work pending to prove full configured occupancy 1 or 4
  before reject-next, then prove useful recovery.
- Standalone staging copies disjoint host spans and checks bytes and untouched
  tail storage. Runtime pipelines execute CPU preparation, CUDA upload/transform/
  download, explicit host copy, XDMA upload/download and CPU validation. Kernel
  and graph variants use one/four frames and CPU workers. The two timelines stay
  backend-local. This is host staging, not peer DMA.
- Lifecycle cases include reconstruction, allocation, fault injection, recovery
  and checked cleanup inside the timer. Faults are injected only into benchmark
  protocol drivers. CUDA graph pipeline failure suppresses downstream output,
  resets through Runtime's public API and retains resources through checked stop.

`prepare` owns setup outside steady timing. `invoke` includes the entire
transaction, completion wait and output validation; no case claims submit-only
latency. `finish` checks shutdown before publication. A failed invoke poisons that
prepared run until finish; stale success observations cannot authorize a retry.
Descriptions and provider discovery never enumerate or probe physical devices.

Counters are per invocation. Accepted submissions, terminal completions, rejected
requests and failed operations are distinct. `copied_bytes` counts successful
transfers and explicit host staging; failed partial I/O is not counted as a
successful transfer. `kernels` counts verified increment transforms, including
one kernel inside a graph. `checked_elements` counts verified payload elements
(or declared capability fields in metadata cases). `peak_outstanding` is a
sampled backend health observation in direct transaction/capacity cases; pipeline
cases leave it zero because their internal submission lane is not sampled.
Polling counters likewise cover explicit fixture polling, not internal Runtime
polls. Reset, cleanup retry, event and cancellation counters report actual checked
operations. No durations or speed thresholds are substituted for correctness.

Steady portable cases are tested with allocations tracked across all threads.
Fixtures keep large storage on the heap; lifecycle and pipeline CLI checks also
run with a 512 KiB caller stack. Each pipeline declares a 240 MiB Runtime budget,
one CUDA and one XDMA candidate, at most four CPU workers and two I/O workers.
Independent concurrent pipelines keep separate drivers, state and lifetimes.

## Real host entry points

The optional native executables require the existing production adapters:

```sh
cmake -S . -B build-native -DRTFW_BUILD_BENCHMARKS=ON -DRTFW_ENABLE_CUDA=ON -DRTFW_ENABLE_XDMA=ON
cmake --build build-native --parallel 2
```

CUDA needs a working NVIDIA Driver API and toolkit at build time. The host owns
CUDA device 0's retained primary context, stream, module, function and, for graph
cases, the graph executable and bound device allocation until checked finish:

```sh
build-native/bench/rtfw-bench-cuda real-cuda-graph-4096 new-cuda-results
```

XDMA needs the supported Linux AXI-MM character driver. Supply both exact channel
endpoints and a scratch window confirmed for this bench. The decimal offset and
byte length are operator inputs; the executable does not invent a register map.
These commands overwrite only the declared transfer span at that offset:

```sh
build-native/bench/rtfw-bench-xdma real-xdma-roundtrip-4096 new-xdma-results H2C_PATH C2H_PATH CONFIRMED_OFFSET CONFIRMED_BYTES
build-native/bench/rtfw-bench-pipeline real-pipeline-graph-4096 new-pipeline-results H2C_PATH C2H_PATH CONFIRMED_OFFSET CONFIRMED_BYTES
```

Missing endpoints or CUDA devices produce NOT RUN. Existing endpoints that
cannot initialize, malformed resources and runtime errors remain failures. No
native host silently substitutes a fake driver. Destructive fault injection is
not exposed on real sessions. Caller-supplied integrations may instead retain
`CudaSession` and `XdmaSession` and pass them to the source-example provider.
Graph storage must match the selected payload exactly; combined XDMA sessions
use queue capacity four, one buffer, one/two workers and transfers up to 4096
bytes. Supplied-session tests use injected tables to prove this path works; they
are not hardware evidence.

No physical measurements or new support/real-time qualification are claimed by
this implementation. Hardware characterization requires execution on an accessible
named bench and the resulting raw artifacts.

## Optional source consumers

With benchmarks enabled, provider sources, bounded fixtures and native host
examples install under `share/rtfw/bench/examples`. They add no default SDK
headers, exported device-provider library or stable ABI. Consumers compile the
sources and link `rtfw::benchmark`, `rtfw::runtime`, `rtfw::cuda_backend` and
`rtfw::xdma_backend`; native hosts additionally link the selected production
adapter. The device package wrapper chains the unchanged Runtime, CPU and legacy
wrappers before testing installed, relocated and `add_subdirectory` consumers.
