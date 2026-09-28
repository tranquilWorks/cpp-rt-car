# First bounded Runtime graph

RTFW 1.2.1 is a C++20 portable RT0 runtime. This guide and the
`examples/hello_runtime/` source kit ship beside each other under
`<prefix>/<datadir>/rtfw/`. The kit contains `main.cpp`, `CMakeLists.txt` and
`check_output.cmake`; it needs only the public SDK, CMake 3.20+ and a C++20
compiler with platform build tools. Tests, GoogleTest, Python, SimCore and CUDA
are not consumer prerequisites. This is functional software evidence, with no
hardware, hard-real-time latency or C++ binary ABI claim.

## Install from a source checkout

Run in Bash from the RTFW source root. Use a fresh build directory when changing
compiler or generator. All paths are quoted where they may contain spaces.

<!-- transcript: install -->
```bash
cmake -S . -B build-sdk -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF -DRTFW_BUILD_EXAMPLES=OFF -DRTFW_BUILD_RUNTIME_DEMO=OFF -DRTFW_BUILD_EXPERIMENTAL=OFF
cmake --build build-sdk --config Release --parallel 2
cmake --install build-sdk --config Release --prefix "$PWD/rtfw-sdk"
cmake -S rtfw-sdk/share/rtfw/examples/hello_runtime -B build-hello -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/rtfw-sdk"
cmake --build build-hello --config Release --parallel 2
ctest --test-dir build-hello -C Release --output-on-failure
```

CTest prints `100% tests passed`. Add `-V` to display the exact program line:

```text
hello_runtime: frames=3 produced=3 consumed=3 stopped=ok
```

This minimal SDK build disables repository tests, example executables and the
profile demo. The source kit is still installed. No submodule initialization is
needed for this consumer build. A contributor who enables tests must initialize
the pinned GoogleTest submodule separately.

## Use an extracted SDK archive

Extract the whole archive and identify the directory containing its `include`,
library and data directories (an archive may contain one enclosing directory).
Keep them together when moving the prefix. From any working directory, use:

```bash
cmake -S "/absolute/path/to/sdk/share/rtfw/examples/hello_runtime" -B hello-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="/absolute/path/to/sdk"
cmake --build hello-build --config Release --parallel 2
ctest --test-dir hello-build -C Release --output-on-failure
```

Replace `share` with the package's configured `CMAKE_INSTALL_DATADIR`.
The package also exposes `RTFW_DATA_DIR` after discovery, so another CMake
project can locate `${RTFW_DATA_DIR}/examples/hello_runtime` without assuming
the data layout. Do not copy only the libraries or add repository include paths.

The kit's essential consumer wiring is:

```cmake
find_package(rtfw 1.2 CONFIG REQUIRED COMPONENTS runtime)
add_executable(hello_runtime main.cpp)
target_link_libraries(hello_runtime PRIVATE rtfw::runtime)
```

The target propagates C++20, include paths and platform link dependencies. Copy
`main.cpp` into your application and use those three lines after your CMake
`project(... LANGUAGES CXX)` declaration.

On Windows, use a Visual Studio developer PowerShell. The same CMake build and
CTest commands work, including `--config Release` / `-C Release`. Use absolute
paths for `--prefix`, `-S` and `-DCMAKE_PREFIX_PATH` in place of Bash's
`$PWD`. With a multi-configuration generator the executable is
`hello-build/Release/hello_runtime.exe`; CTest finds it automatically.

## Embed a source checkout

From the source root in Bash:

<!-- transcript: embed -->
```bash
cmake -S samples/hello_runtime -B build-hello-embedded -DCMAKE_BUILD_TYPE=Release -DHELLO_RTFW_SOURCE="$PWD"
cmake --build build-hello-embedded --config Release --parallel 2
ctest --test-dir build-hello-embedded -C Release --output-on-failure
```

For your own application, replace package discovery with
`add_subdirectory("/absolute/path/to/rtfw" rtfw-build)` and link the same public
target. The hello kit's `HELLO_RTFW_SOURCE` selects this path. An installed SDK
cannot serve as that source checkout. RTFW subproject tests, examples and tools
default off; a parent's generic `ENABLE_TESTS` does not enable them. Explicit
subproject tests use `RTFW_BUILD_TESTS=ON`.

## What the graph does and owns

`State` belongs to the host and is constructed before `Runtime`, so it remains
alive through shutdown and Runtime destruction. Callback registration borrows
its address. `produce` increments the counter; `consume` checks that the next
value arrived and copies it. Both mutate the same logical resource, so both
declare write access and the producer-to-consumer dependency orders them.
Resource declarations describe access; they do not transfer memory ownership.

The source explicitly selects two callback slots, two workers, a queue capacity
of two, two task scratch slots, zero phase/task scratch bytes and 32 trace slots.
Remaining settings use `RuntimeConfig` defaults, including its 256 MiB runtime
memory budget. That budget is a ceiling, not a claim about measured resident
memory. This example submits no nested tasks or devices. Configuration and
finalization may allocate; start creates the fixed worker team. Callbacks do
only bounded integer work, with no allocation, I/O or exceptions.

Every status-bearing operation is checked. Finalization validates and freezes
the graph; start prepares execution. Three synchronous `step` calls supply
logical 1 ms deltas, without pacing or promising 1 ms wall-clock deadlines.
Output occurs on the host after checked `stop`, never inside a callback.
A failed start or step still reaches checked stop. Configuration/finalization
failure exits before execution. The dependency and consumer check enforce the
sequence; the final counters and external output check make silent failure
visible.

This CPU-only example owns no external backend resources. When adapting it to
borrowed devices, providers or code modules, retain all borrowed resources until
`stop()` succeeds and retry unresolved cleanup as directed by that backend;
a destructor cannot report cleanup status. Keep lifecycle calls on one host
thread and do not invoke them recursively from callbacks.

## Troubleshooting

| Symptom | Action |
| --- | --- |
| CMake cannot find `rtfwConfig.cmake` | Install/extract the entire SDK; pass its absolute prefix via `CMAKE_PREFIX_PATH`, or the directory containing the config file via `rtfw_DIR`. Use a fresh consumer build after moving the SDK. |
| `HELLO_RTFW_SOURCE must name an RTFW source checkout` | Set it to the source root containing the RTFW CMakeLists.txt; leave it unset for installed consumption. |
| Missing `rt/runtime.hpp` or C++20 errors | Link `rtfw::runtime`, enable CXX in the consumer project and use a supported C++20 compiler; avoid manual include flags. |
| GoogleTest missing during SDK configuration | Use a clean build with `ENABLE_TESTS=OFF`; consumer installation requires no tests. |
| `register consume failed (-4): configured capacity exceeded` | The graph has two callbacks; retain `callback_capacity = 2` or raise it deliberately for a larger graph. |
| `finalize` reports unordered conflicting access | Restore the producer-to-consumer dependency; registration order is not a substitute for a dependency. |
| `step` fails or counters disagree | Keep callback state alive and obey resource ordering; inspect the printed operation, numeric status and `last_error()` before cleanup clears it. |
| `stop` fails after adding external resources | Preserve borrowed ownership, resolve the reported cleanup failure and retry; never treat a destructor as a successful checked stop. |
| CTest cannot find the executable | Build first and use the same Release configuration for build, install and CTest. |

The repository's `samples/` contains supported Runtime consumers and explicitly
marked experimental samples. The separate `examples/` directory contains
legacy research experiments, not the installed SDK entry path. Larger raw C
and C++ embedding, live-control, sampled-I/O and CUDA references remain
available; helper builders, backend authoring, external CIL, a full host adapter
and the complete installed API/recipe catalog are separate M25 batches.
