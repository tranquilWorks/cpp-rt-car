# Runtime samples

M25-04 adds the optional source-only [external shared-memory CIL reference](../docs/cil_shared_memory.md):
separate controller and public Runtime plant, bounded versioned channels, explicit
hold/reconnect/ownership policy and installed process tests on Linux/Windows x86-64.
It adds no mandatory SDK target or compiled ABI and makes no physical/RT claim.

Start with [hello_runtime](hello_runtime/main.cpp), the small raw public-API graph.
The [first-use guide](../docs/getting_started.md) gives tested install/find_package
and add_subdirectory commands. The source kit is also installed under
`<datadir>/rtfw/examples/hello_runtime`.

Larger supported Runtime source examples include [C embedding](embed_c/mini_app.c),
[C++ embedding](embed_cpp/mini_app.cpp), [device mock](device_mock.cpp),
[typed live controls](live_control_typed.cpp), and the
[CUDA particle reference](cuda_physics/main.cpp). Portable CUDA examples use
simulated drivers; optional physical execution needs separate hardware evidence.

`particles.cpp`, `tyre_belt_kernel.cpp` and `async_gpu_stub.cpp` are legacy
SimCore experiments, built only with `RTFW_BUILD_EXPERIMENTAL=ON`. They do not
use the supported Runtime graph or establish SDK, CUDA or RT qualification.
The separate [examples directory](../examples/README.md) holds more legacy
research code and is not a consumer quick start.

The optional [typed Runtime kit](typed_runtime/) adds noexcept callbacks, explicit
builders, checked-stop guards and fixed diagnostics. See [the guide](../docs/typed_runtime.md).

## Backend authors

The installed source-only `backend_authoring` kit includes a minimal HAL-v2 copy
backend, reusable conformance and native/v1 Runtime example. See [the guide](../docs/backend_authoring.md) for build commands,
fixed storage, lifetime rules and failure/cleanup retry.
