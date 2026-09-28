# Legacy research experiments — not the Runtime SDK

These archived research programs are not the supported `rt::Runtime` entry
path, installed SDK examples, or a tested standalone build recipe. New users
should start with [hello Runtime](../samples/hello_runtime/main.cpp) and the
[consumer guide](../docs/getting_started.md).

- `particles_on_plane.cpp` uses legacy SimCore/WorkerPool internals.
- `tyre_belt_on_drum.cpp` is a standalone numerical experiment.
- `gpu_offload_demo.cpp` is an OpenMP offload experiment, not the CUDA backend.
- `perf_experiments.cpp` uses experimental logging, profiling and platform APIs.

The former generic one-file compiler command omitted required implementation
and platform dependencies and has been withdrawn. These sources are retained
for research history; no current standalone compilation or execution claim is
made. Repository-built experimental samples are listed separately in
[samples](../samples/README.md) and require `RTFW_BUILD_EXPERIMENTAL=ON`.
