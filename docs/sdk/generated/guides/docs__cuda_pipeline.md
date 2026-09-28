[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)

> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.

# Runtime CUDA particle pipeline

M24-02 adds a bounded two-lane source example to the [M24-01 reference](docs__cuda_physics.md).
It uses the unchanged seeded integer particle model, kernel and independent
closed-form CPU oracle. This is validation content, not a general physics
engine, a hardware support claim, or CAP-M24 maturity closure.

## Composition and ownership

A global sequence of 1..4096 particles is split into contiguous shards of
ceil(count/2) and floor(count/2). Count one uses only the first lane. Each lane
advances the same 1..1024 logical model steps. Positions, velocities and
accelerations retain M24-01's units, seed wrapping and int64 oracle bounds.
The combined final state is identical across kernel/Graph and frame/active modes.

Each lane declares preparation, a device command batch, and dependent CPU
validation. Preparation copies the previous validated output to staging and
clears the output span. The ordered batch is:

1. H2D staging upload.
2. H2D cleared work-span upload.
3. D2D staging-to-work copy through the public CUDA dispatch opcode.
4. Kernel dispatch or the pre-instantiated kernel Graph.
5. D2H work download, followed by one terminal event and timeline signal.

The second upload is intentional: Runtime requires explicit synchronization of
staged references before dispatch reads; it cannot infer initialization from a
vendor-specific D2D opcode. The cleared work span also makes omitted/corrupt D2D
behavior observable to the oracle. Every particle field is checked after terminal
completion. Failed work cannot publish a successful lane step.

The current native CUDA batch API selects the first stream of each backend.
Consequently the example uses **two backend instances**, each configured with
one distinct caller-owned stream and sharing one caller-owned context/module/
function. There is no private backend submission or polling. Runtime owns both
submission/service paths. Each backend has two bounded event slots, each phase
admits one in-flight batch, and the workload accepts at most two batches at once.
Each backend/lane has its own timeline, starting at zero and advancing exactly
once per successful model step. There is no cross-backend wait or peer DMA.

Four caller-owned device allocations map to four registered host spans. The
backend's mirror allocation is disabled; named addresses are bound before start.
Each lane's Graph contains the unchanged kernel with its exact work address and
count, created and instantiated before execution. The Graph registry names that
same buffer with read/write access. The kernel path uses a fixed copied argument
table. No argument rebinding, graph construction or ordinary allocation occurs
in steady steps. Scenario plus simulated-driver inline storage is statically
bounded below 32 MiB; Runtime separately declares at most 128 MiB, two CPU
workers, two backends and four buffers. Storage is heap-owned before start.

Checked Runtime stop precedes destroying Graph executables/graphs, allocations,
module, streams and primary context. Failed stop or caller-resource cleanup keeps
unreleased ownership for retry. An unresolved cleanup cannot silently destruct
borrowed resources. Fresh runs construct fresh instances; terminal Runtime restart
is not assumed. This reference does not establish arbitrary device-loss recovery.

## Frame and active scheduling

`--schedule frame` uses normal Runtime frame dependencies. `--schedule active`
enables the public rate execution policy and binds all CPU/device phases to one
common domain. It inspects the compiled device plan and verifies every callback's
release sequence, logical release time and substep, plus terminal timeline values.
The phase declarations place both preparations and submissions before dependent
validation barriers so the active dispatcher can retain both independent batches.

The active validation envelope is a **10-second nominal period**, with a declared
1-second CPU budget and 5-second device completion budget. These generous bounds
are scheduling test inputs, distinct from the unchanged **1/256-second model
step**. Host-driven calls supply contiguous nominal windows mapped from a steady
clock epoch and do not sleep to pace them. This proves active dispatch and bounded
ownership, not a real-time workload, heterogeneous rates, physical overlap or
speedup. Heterogeneous cross-rate transport is not implemented by this example.

## Build and run

```sh
cmake -S . -B build/pipeline -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DSIM_SANITIZERS= -DSIM_WERROR=ON
cmake --build build/pipeline --target sample_cuda_pipeline rtfw_cuda_pipeline_tests --parallel 2
build/pipeline/samples/sample_cuda_pipeline --dispatch graph --schedule active --count 4096 --steps 1024 --seed 4294967295 --workers 2
ctest --test-dir build/pipeline -R 'm24_cuda_pipeline|sample_cuda_pipeline' --output-on-failure
```

Defaults are Graph dispatch, ordinary frame scheduling, 256 particles, 64 steps,
seed one and two workers. `--dispatch kernel|graph` and `--schedule frame|active`
select all four combinations. The remaining options retain the reference bounds.
Help exits zero; malformed/duplicate/out-of-range options exit two; failed work,
oracle or cleanup exits one. A success summary is printed only after checked stop
and caller-resource cleanup. Portable summaries say `simulated-driver-protocol`.

The installed kit remains under `${datadir}/rtfw/examples/cuda_physics`, with the
original nine files plus exactly five files in `pipeline/`. Documentation installs
beside the model document. No SDK header or target is added. The kit's existing
CMake project now builds `pipeline_consumer` and tests all four modes in addition
to `physics_consumer`. Its `PHYSICS_RTFW_SOURCE` option also supports embedding.

```sh
cmake -S /relocated/sdk/share/rtfw/examples/cuda_physics -B consumer -DCMAKE_PREFIX_PATH=/relocated/sdk
cmake --build consumer --parallel 2
ctest --test-dir consumer --output-on-failure
python3 tests/cuda_physics/verify_package.py --work-directory build/pipeline-package
```

## Optional actual CUDA host

Enable `RTFW_ENABLE_CUDA=ON` with a CUDA toolkit providing nvcc. Build
`sample_cuda_pipeline_real`; the existing kernel is compiled to compute_75 PTX
using the same build function as M24-01. The real host accepts identical mode
options. The installed kit's `PHYSICS_REAL_CUDA=ON` builds
`pipeline_real_consumer` and `physics_real_consumer`. Both support `--help`
without accessing a device. Real mode never substitutes the simulated driver.

The host creates a primary context, module/function, nonblocking streams,
allocations and optional kernel Graphs, then supplies the same Scenario. Graph
parameters are copied at creation as documented by the
[CUDA Driver Graph API](https://docs.nvidia.com/cuda/archive/12.9.0/cuda-driver-api/group__CUDA__GRAPH.html).
Missing device or the official driver stub reports `status=NOT_RUN`, exit three.
Other driver/configuration/oracle/cleanup errors fail. A loader unable to start
without libcuda is an unperformed invocation, not a simulated fallback. Actual
physical correctness requires a named real run; compile/link and stub checks
cannot supply it.

## Coverage and boundaries

`test_cuda_pipeline.cpp` compares every final byte across all four modes for
counts 1/17/256/1024/4095/4096, steps 1/7/64/1024, seeds 0/1/UINT32_MAX and
one/two workers, including simultaneous maxima. Each internal step independently
checks every field against the closed-form oracle. A known seed-zero vector,
corrupted D2D/output and rejected Graph exercise negative controls. Tests observe
both held terminal events on distinct streams before allowing publication, then
reuse both lanes. They also verify exact copy/dispatch/byte counts, no backend
allocation/free of borrowed addresses, checked cleanup retention and retry,
malformed sessions, independent concurrent instances and zero steady allocations.

The actual CLI exercises four modes, numeric rejection, repeatable maximum output
and the existing Linux 512 KiB process-stack method. M24-01's separate embedded
512 KiB Debug/TSan thread limitation remains unqualified. Existing ABI, default SDK,
prior source ledger, benchmark inventory and test oracles are preserved. M24-03's
full fault/determinism campaign and M24-04's benchmarks and versioned maturity
matrix remain separate, as do physical CUDA, controlled performance, independent
human review, RT1/RT2, signing and release.
