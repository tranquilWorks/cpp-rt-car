# Runtime CUDA particle reference

This is a bounded validation model, version `constant-acceleration-v1`. It
exercises the supported `rt::Runtime` and actual `CudaDeviceBackend` through
public installed interfaces. It is not a general physics engine. The default
host uses an injected software driver; no NVIDIA hardware is accessed.

## Model and correctness

Each particle stores nine signed 32-bit integers: position x/y/z, velocity
x/y/z, and acceleration x/y/z, in that order. One logical step is 1/256 second.
Position units are 1/65536 metre; velocity units are position units per step;
acceleration units are position units per step squared. The update is
`v := v + a; x := x + v`, independently for each axis. There are no collisions,
constraints, floating-point tolerances or claims of physical fidelity.

The uint32 seed evolves as `seed = seed*1664525 + 1013904223` modulo 2^32.
Each axis consumes three successive values: `seed%2049-1024` for position,
`seed%129-64` for velocity and `seed%9-4` for acceleration (subtract in signed
arithmetic after the modulus). Particles consume axes x/y/z in order. Seed zero
starts with positions `(627,694,448)`, velocities `(43,57,-6)` and accelerations
`(-3,-1,-2)`. At step seven its first particle is `(844,1065,350)` with velocity
`(22,50,-20)`. These retained vectors are checked independently of the kernel.

The CPU oracle evaluates `v(n)=v0+n*a` and
`x(n)=x0+n*v0+a*n*(n+1)/2` using int64 intermediates from the original state.
It does not call the iterative kernel. Every axis/field of every particle is
checked after every successful frame. Intentional output corruption must fail
validation and publish no successful frame. Device results feed the next frame;
the CPU does not replace them with precomputed answers.

Counts are 1..4096, steps 1..1024, seeds 0..4294967295, and CPU workers 1..2.
Velocity magnitude is bounded by 4160 and position by 2165760 throughout the
allowed horizon, so kernel int32 arithmetic cannot overflow. Zero, oversized,
negative, trailing, duplicate and overflowing CLI arguments are rejected.

## Runtime graph and ownership

CPU preparation -> Runtime CUDA command batch (H2D, kernel, D2H) -> CPU oracle
and publication. Fixed buffer references and kernel arguments are declared
before finalization. One registered resource and explicit phase dependencies
order accesses. Each frame signals and checks its backend-local timeline value.
There is one context, stream, backend and in-flight slot per instance. Driver
calls use Runtime's existing submission/service lanes; executor callbacks do
not call the CUDA driver. Completion must be observed before CPU validation.

Scenario and driver storage are allocated by the host before start, including
fixed 4096-particle arrays. Combined retained storage is below 1 MiB (and the
contract's 32 MiB limit); Runtime has a separate declared 128 MiB budget. No
steady step allocates. A 512 KiB process-stack regression of the real CLI entry point keeps large state on
owned heap storage. The injected driver copies real bytes, decodes arguments,
executes an independent iterative update, checks stream/function identities,
and tracks copies, bytes, launches and resource ownership.

Context, stream, module/function, driver table state and registered buffers
outlive accepted work and checked Runtime cleanup. A rejected copy/launch may
leave CUDA work quarantined: the host uses `Runtime::reset_device` before
checked stop to settle that work. Failed reset or stop retains ownership for
retry. Cleanup failure still makes the invocation fail, even when a later
cleanup retry succeeds. Unrecoverable cleanup terminates the process rather
than destroying possibly live borrowed resources. A failed frame cannot be
continued; use a fresh Scenario. Runtime instances are not restarted after
terminal stop. No device loss recovery or arbitrary application rollback is
claimed by this first reference batch.

## Build and run

```sh
cmake -S . -B build/physics -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_TESTS=ON -DRTFW_BUILD_EXPERIMENTAL=OFF -DSIM_SANITIZERS= -DSIM_WERROR=ON
cmake --build build/physics --parallel 2
build/physics/samples/sample_cuda_physics --count 4096 --steps 1024 --seed 4294967295
ctest --test-dir build/physics -R 'm24_cuda_physics|sample_cuda_physics' --output-on-failure
```

The deterministic summary states the model, seed, dimensions, completed steps,
validation result and `simulated-driver-protocol` evidence. Exit 0 means checked
execution and cleanup succeeded, 1 means execution/oracle/cleanup failure, and
2 means invalid arguments. `--help` succeeds without creating a device session.
Timing is not reported and the test timeout is not a latency qualification.

Source examples are installed under `${RTFW_DATA_DIR}/examples/cuda_physics`
and this document under `${RTFW_DATA_DIR}`. They add no SDK exports/headers.
After installation (use the configured data directory for a custom layout):

```sh
cmake -S "$prefix/share/rtfw/examples/cuda_physics" -B consumer \
  -DCMAKE_PREFIX_PATH="$prefix"
cmake --build consumer --parallel 2
ctest --test-dir consumer --output-on-failure
python3 tests/cuda_physics/verify_package.py --work-directory build/physics-package
```

`PHYSICS_RTFW_SOURCE=/path/to/checkout` selects the explicit add_subdirectory
consumer instead of find_package. The verifier installs a custom data layout,
archives it, removes the original prefix, builds the shipped sources outside
the repository against the relocated package, and checks compiler commands for
source-tree/original-prefix dependencies. It then tests source embedding.

## Optional real CUDA

Explicitly configure `RTFW_ENABLE_CUDA=ON` to build `sample_cuda_physics_real`.
The shipped standalone consumer uses `PHYSICS_REAL_CUDA=ON` and builds
`physics_real_consumer`. Both require the CUDA toolkit's Driver API and nvcc.
`real.cmake` compiles the checked-in `particle.cu` into build-local PTX with
`nvcc --ptx -arch=compute_75`; no opaque PTX blob is shipped. Record the actual
nvcc/driver/device versions for a real run. The kernel target requires a
compatible device (compute capability 7.5 or newer); it establishes no supported
or qualified tuple. Toolkit CI compiles the kernel and host without a GPU.

The explicit real executable acquires device zero's primary context, a
nonblocking stream and the compiled module/function on the host. It passes
those resources into exactly the same Scenario graph and oracle. A missing
device or a link-time stub driver reports `NOT_RUN` with exit 3; other
driver/configuration/oracle/cleanup errors remain failures. A build without the
toolkit has no real executable: real execution is NOT RUN, not a fake fallback.
If the dynamic loader cannot find the CUDA driver library, the real host cannot
start; retain that unavailable run rather than recording a successful sample.
List/help and portable execution never probe CUDA. Do not run this optional
host without ownership of the selected device.

## Coverage and remaining maturity

`test_cuda_physics.cpp` covers known vectors and the maximum arithmetic bound;
counts 1/17/256/1024/4096; steps 1/7/64/1024; seeds 0/1/UINT32_MAX; one/two
workers; simultaneous maximum count/steps; repeated and concurrent independent
owners; every-field oracle; corrupted output; copy/launch rejection; delayed
completion/nonpublication; checked cleanup retry; no-allocation; and allocation conservation. The CLI test retains M23's Linux 512 KiB process-stack method. A separate diagnostic found that an embedded 512 KiB pthread running an
unoptimized TSan-instrumented Runtime finalization can exhaust its stack; no
guarantee of that embedded-thread stack configuration is made. Runtime source
is unchanged. This is a declared finite matrix, not a full Cartesian sweep.
The actual CLI has malformed-input, maximum-run and repeat-output tests.

M24-02 adds the separate [Graph/pipeline/active-rate example](cuda_pipeline.md); M24-03 retains the broad
pressure, cancellation, loss/reset and determinism campaign; M24-04 retains
benchmark/profiler and capability-matrix closure. This batch cannot claim
CAP-M24's 90% overall/100% critical-row maturity, real GPU correctness, physical
support, RT1/RT2, controlled performance, signing, release or deployment.
