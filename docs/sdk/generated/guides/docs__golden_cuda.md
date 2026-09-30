[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)

> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.

# Golden CUDA physics

M26-03 replaces only the frozen golden scenario's physics phase with a Runtime
HAL-v2 command-batch phase. CPU fallback, kernel and Graph paths retain all
integer state, tails, controls, channel identities, rates and the independent
C++ and Python oracles. The sibling CPU kit remains usable with its original
commands. Final exact-head validation and integration identities are recorded in
[PR277](https://github.com/tranquilWorks/cpp-rt-car/pull/277).

The installed `examples/golden_cuda` source kit uses public Runtime and CUDA
backend APIs and the installed sibling `golden_system` source kit. Run
`python3 run.py --prefix /path/to/sdk --dispatch graph --mode host`; use
`--mode external` to start the actual CIL controller process. `--dispatch cpu`
selects fallback; `kernel` and `graph` execute the actual CUDA backend through
an explicitly owned injected Driver API. Host/native modes select executor
ownership, not physical-device support.

The bounded command graph uploads both host staging buffers, copies device
input to output, launches integer physics, downloads output and completes one
same-backend timeline signal. Publication follows terminal success and validates
all output before changing the canonical world. Runtime has one device in-flight
slot; device storage, stream and executable remain caller owned. Steps and
supported replay allocate no ordinary heap memory.

Only the owned deterministic simulator adapter advertises mock semantics. It
explicitly selects a five-second host scheduling watchdog while preserving the
one-millisecond logical completion budget and backend command timeout. The new
CUDA sample requests parked idle executor workers where supported; Windows
reports its existing unsupported-policy fallback. The CPU fallback retains
its existing default policy. Native
construction has no simulation policy and disables replay. Logical simulation
time and this host guard establish no physical deadline or RT qualification.

Active/trusted replay belongs to the originating owner. The kit checks the public
trusted artifact envelope's Runtime identity before entering replay; foreign
artifacts are rejected without backend activity. Runtime's broader replay API is
unchanged. Compatible paired checkpoint recovery instead constructs a fresh
owner, restores canonical host state and uploads it on the next release. Opaque
control generations/checksums are validated per owner, never rewritten to fake
cross-variant byte identity.

`--fault device_loss` and `--fault reset_failure` inject actual Driver API faults
at tick6. The first yields permanent lost health and rejects reset. The second
requires reset, fails synchronization once, retains borrowed resources and then
resets successfully. Both inject a checked-stop unregister failure, verify
retention and retry teardown before fresh-owner checkpoint recovery. Overload
also recovers with the selected physics implementation. Artifacts retain exact
status, health, operations, publications, fault counts and per-owner timeline.

Optional `-DGOLDEN_CUDA_REAL=ON` builds the native host plus PTX integer kernel
using a toolkit and Driver-enabled SDK. `golden_cuda_real --help` does not open
a device or require a driver library. A small control-plane launcher checks driver
library availability before starting the linked native host. Missing device/driver
reports NOT_RUN with exit3. The host owns its
actual context, stream, two device buffers and optional Graph. It uses the same
scenario/physics graph, disables replay and only writes native state/execution
evidence into a fresh output directory after checked cleanup. It cannot emit
simulated replay PASS. Physical execution remains NOT RUN until performed on a
named accessible tuple.

M26-04 and M26-05 deliver XDMA/combined execution and showcase benchmarks.
The [M26-06 audit](docs__golden_audit.md) records the current source and remaining
engineering/review/qualification work. Independent human review, physical/RT/
Unreal qualification and release remain separate.

The [XDMA sampled-I/O reference](docs__golden_xdma.md) adds an installed simulated
XDMA/combined kit using the same frozen logical scenario.
