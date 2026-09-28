[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)

> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.

# CUDA particle benchmark and host correlation

The optional `rtfw-bench-cuda-physics` executable runs the unchanged M24 pipeline
through public Runtime and CudaDeviceBackend APIs. Its 20 M23 ProviderV1 cases
cover kernel/Graph, ordinary/active dispatch, 1/17/4096 entities and one/two workers.
The original M23 CLI and all 207 descriptors remain unchanged. The capability
matrix crosswalk identifies its transfer, launch, queue-overlap, recovery,
heterogeneous-rate and composed-pipeline cases; those cases retain their original
models and timing scopes. The particle active mode uses one common rate domain.

Configure with `-DRTFW_BUILD_BENCHMARKS=ON`, build `rtfw-bench-cuda-physics`, then:

```sh
build/m24-benchmark/bench/rtfw-bench-cuda-physics list
build/m24-benchmark/bench/rtfw-bench-cuda-physics --case graph-active-4096-w2 --clock steady --output particle-run > particle-trace.json
python3 tools/check_benchmark_artifact.py --artifact-root particle-run
```

Use a new output directory with existing parents. Help/list exit zero, invalid
arguments exit two, and execution/oracle/cleanup/publication failures exit one.
There is no hardware fallback: this provider is explicitly simulated protocol.
The original real reference/pipeline hosts remain the real CUDA entry points;
missing devices report NOT RUN, exit three. See [pipeline](docs__cuda_pipeline.md).

Each run prepares one fixed owner, performs two warmup plus five measured steps,
and checks stop/resource conservation before publishing the unchanged three-file
M23 bundle. Every measured duration includes the complete Runtime step, staging,
H2D/D2D/kernel-or-Graph/D2H, independent CPU oracle, host correlation and observed
counter collection. Preparation and teardown are excluded. No unmeasured cost is
subtracted. Counts are per invocation; M23 sums only the five measured samples.
Entity updates per second can be derived from entity count and complete-step
nanoseconds; these are host simulated-protocol rates, not GPU throughput.

The kernel and Graph execute the same nine-field integer model, with exact output
parity. One/two lanes retain the existing admission and completion budgets. No
normal vendor synchronization, backend device allocation or steady new allocation
is allowed. The M23 runner preallocates each observation's counter vector; direct
provider callers must reserve at least nine counter entries before timed invoke.

The separate stdout JSON correlation stream has version 1 and at most 14 records.
A record contains an invocation-local ID, ordinal (0..6, including warmup), lane,
actual timeline signal value, command count, host callback timestamp and host
checked-completion timestamp. Private Runtime timeline handles are checked against
completion and never exported. Case ID and the bundle identify the run; numeric
IDs are local to that run, not globally unique. Each lane writes a distinct fixed
slot; records are exposed only after checked stop. Capacity below seven records
per lane rejects before setup, and capacity above 14 is invalid. No callback does
allocation, logging, thread creation or filesystem I/O. Host timestamps include
scheduler effects and cannot establish GPU overlap, GPU elapsed time or an Nsight
capture. Device timestamps explicitly remain `not_available`.

Install the SDK with benchmarks enabled and configure the shipped
`share/rtfw/examples/cuda_physics` project with `-DPHYSICS_BENCHMARKS=ON` and
`-DCMAKE_PREFIX_PATH=/relocated/sdk`. For embedding, additionally set
`-DPHYSICS_RTFW_SOURCE=/checkout`. The `physics_benchmark_consumer` executable
uses those same three installed source files and the optional `rtfw::benchmark`
component. Default SDK targets/headers and all prior sample files remain intact.

[Generated capabilities](https://github.com/tranquilWorks/cpp-rt-car/blob/98ecca5a6572dd83d59e26063716f6d76b7a5d4f/docs/cuda_capabilities.md) binds each row to named automated
checks and retained source/evidence digests. Physical rows remain in the overall
denominator and explicit M18 manual gates. CUDA does not claim deterministic
checkpoint/replay of external device state; the actual CUDA registration rejects
D1 finalization, covered by the benchmark conformance test. Generic cross-rate
transport and trusted mock replay retain their existing M21/M23 boundaries.

The retained M24-04 optimization record compares equal-workload kernel and Graph
runs with raw M23 samples. Different dispatch case identities intentionally cannot
be treated as same-case baselines by M23's regression analyzer. Timing differences
on this host are portable characterization with no approved latency threshold,
physical performance or near-optimal claim. A comparison need not show a speedup.
