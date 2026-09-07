# Build and Tooling Guide

## Direct build

The supported baseline is CMake 3.20+ and C++20:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Tests use the checked-in GoogleTest submodule by default. Clone submodules
recursively or initialize them before configuring with tests.

## Embedding with add_subdirectory

An in-tree host can add RTFW without inheriting repository tooling:

```cmake
add_subdirectory(third_party/rtfw)
target_link_libraries(my_engine PRIVATE rtfw::runtime)
```

Subproject mode builds library targets only by default. A parent's generic
`ENABLE_TESTS` value does not enable RTFW tests, and RTFW does not create CPack
or package targets in the parent. Use the namespaced
`RTFW_BUILD_TESTS=ON` switch for an intentional subproject test build.
Repository examples, benchmarks, demos, experiments, and installation remain
separate explicit options.

## Optional CUDA Driver API adapter

The bounded CUDA state machine (`rtfw::cuda_backend`) is built without a
toolkit dependency so CPU-only CI can test it. Enable the production adapter
only when CUDAToolkit and a compatible driver environment are available:

```bash
cmake -S . -B build-cuda \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_TESTS=ON \
  -DRTFW_ENABLE_CUDA=ON
cmake --build build-cuda \
  --target simcore_tests sample_cuda_qualification \
  --parallel 2
GTEST_FILTER='CudaBackend.*' \
  ctest --test-dir build-cuda --output-on-failure -R simcore_all
```

Installed CUDA-enabled packages export `rtfw::cuda_driver`, propagate the
`RTFW_CUDA_DRIVER_AVAILABLE` definition, and resolve `CUDA::cuda_driver`.
Packages built with the default `RTFW_ENABLE_CUDA=OFF` still export
`rtfw::cuda_backend` for injection/testing but do not expose the production
adapter symbol. See [the CUDA contract](cuda_backend.md).

## Optional Xilinx XDMA Linux adapter

The portable fixed-capacity state machine is always available as
`rtfw::xdma_backend`. On Linux, enable the production adapter and destructive
AXI-MM qualification tool explicitly:

```bash
cmake -S . -B build-xdma \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_TESTS=ON \
  -DRTFW_ENABLE_XDMA=ON
cmake --build build-xdma \
  --target rtfw_xdma_backend_tests sample_xdma_qualification \
  --parallel 2
ctest --test-dir build-xdma --output-on-failure -R xdma_backend
```

The host must separately install/load the official Xilinx XDMA Linux driver
and program the declared AXI-MM-compatible bitstream. The library does not
install a kernel module, mutate driver policy, or discover a safe device range.
See [the XDMA contract](xdma_backend.md) before running the qualification tool.

## Presets

The Ninja presets write under `build/`:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure

cmake --preset release
cmake --build --preset release
```

The Release preset requests LTO/ThinLTO. Compiler and generator support is
checked during configuration.

## Experimental PGO

`pgo-generate` and `pgo-use` expose compiler profile flags, but the repository
does not orchestrate a representative training workload, merge profiles across
processes, or validate that a profile matches the binary. Treat these presets
as scaffolding:

```bash
cmake --preset pgo-generate
cmake --build --preset pgo-generate
# Run a deliberately selected training workload here.
cmake --preset pgo-use
cmake --build --preset pgo-use
```

## Optional analysis

- `SIM_ENABLE_IWYU=ON` requests include-what-you-use when the executable is
  installed.
- `SIM_WARN_ODR=ON` adds supported ODR warnings.
- a `bloaty` target is created only when Bloaty is found.
- `SIM_ENABLE_AVX2=ON` applies global x86 AVX2/FMA compile flags and is
  experimental; it is not runtime ISA dispatch.
- sanitizer combinations are described in [sanitizers](sanitizers.md).

## Installation/versioning

`VERSION.txt` is the release source of truth. CMake installs shared/static
libraries, public headers, package config/version files, the license, and the
version, changelog, security, support-matrix, and release-contract files.
Release 1.2 exports component-checked targets for shared C, static C, the C++
runtime, and portable/optional CUDA/XDMA adapters. `find_package()`
compatibility is same-major for 1.x. Stable C ABI compatibility is checked
independently at runtime; target C++ compatibility is source-only and requires
recompilation. Install layouts honor `CMAKE_INSTALL_INCLUDEDIR` and
`CMAKE_INSTALL_DATADIR`; neither consumer include paths nor license discovery
assume the default `include`/`share` layout.

CI builds the CPack archive, stages and verifies it, extracts that exact
downloadable artifact to a fresh prefix, then configures and runs
`tests/package_consumer` as an external project on Linux and Windows. The
consumer requests the stable C/C++ surfaces plus the always-built portable
CUDA/XDMA state-machine components:

```cmake
find_package(
  rtfw 1.2 CONFIG REQUIRED
  COMPONENTS
    c_shared c_static runtime cpp_runtime
    cuda_backend xdma_backend)
```

On Linux, that gate also configures a nested project with only the C language
enabled and links both C components. This verifies that the static C target
exports the implementation toolchain's required C++ runtime libraries instead
of relying on the consumer to enable C++.

New C++ integrations link `rtfw::runtime`; `rtfw::simcore_rt` remains a 1.x
compatibility name. The preferred C targets are `rtfw::c_shared` and
`rtfw::c_static`, while `rtfw::rtfw` and `rtfw::rtfw_static` remain
compatible. Project warning/Werror and logging/profiler macros are never
consumer usage requirements. Optional CUDA/XDMA adapter export files are
loaded only when those components are requested. Their opt-in qualification
workflows also install to a relocated prefix and compile/link consumers that
request `cuda_driver` or `xdma_linux` explicitly.

The default package contains only the contract-listed runtime/C/backend
headers. Broad source-tree SimCore, scheduler, plugin, crashdump, NUMA,
numerics, and snapshot experiments require `RTFW_BUILD_EXPERIMENTAL=ON`;
installation additionally requires `RTFW_INSTALL_EXPERIMENTAL=ON`. The narrow
`rtfw::simcore_rt` compatibility archive remains available through 1.x for the
accidental demo/fiber link path, but the corresponding SimCore, HAL/GPU, and
fiber headers are not part of the default SDK.

Linux CI also compares the
built shared-library symbols with the v8 allowlist. Run the header/manifest
half locally with:

```bash
python3 tools/check_c_abi.py
```

See [the stable C ABI contract](c_abi.md) for SONAME and compatibility rules.

## Portable assurance tooling

M20-PRE-01 provides one noninteractive host-independent entry point:

```bash
scripts/verify-portable-assurance.sh all \
  --build-dir build/m20-pre-01 \
  --source-commit "$(git rev-parse HEAD)"
```

Its `dependencies`, `static`, `fuzz`, `artifacts`, and cumulative `all` modes
write only below the explicit build directory. Static and fuzz modes require
Clang/clang-tidy 14; missing or mismatched tools fail. Artifact mode requires a
default non-experimental Release package and performs deterministic candidate
SBOM/provenance/manifest verification, safe extraction, relocated consumption,
C ABI v8 verification, and SONAME 8 verification. It performs no network
lookup during candidate/fixture verification, signing, tag, publication,
release, system-wide install, device access, privileged operation, or Unreal
build. See [portable assurance](portable_assurance.md).

## Portable release archive

CPack creates a `.tar.gz` archive on non-Windows hosts and a `.zip` archive on
Windows. It also creates a SHA-256 sidecar:

```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_TESTS=OFF \
  -DRTFW_BUILD_EXPERIMENTAL=OFF \
  -DSIM_WERROR=ON
cmake --build build-release --config Release --parallel 2
cpack --config build-release/CPackConfig.cmake -C Release -B cpack-output
python3 tools/stage_release_artifacts.py \
  --cpack-dir cpack-output \
  --artifact-dir release-artifacts \
  --generator TGZ \
  --version-file VERSION.txt
```

The staging check admits exactly one archive and its matching CPack SHA-256
sidecar, leaving CPack's internal `_CPack_Packages` tree unpublished. The
release workflow then records every staged file, its byte length, its SHA-256,
and the complete source commit in a manifest and verifies the directory
contains neither missing nor unlisted files:

```bash
python3 tools/release_manifest.py create \
  --artifact-dir release-artifacts \
  --output release-artifacts/rtfw-release-manifest.json \
  --version-file VERSION.txt \
  --source-commit <full-40-character-git-sha>
python3 tools/release_manifest.py verify \
  --artifact-dir release-artifacts \
  --manifest release-artifacts/rtfw-release-manifest.json \
  --version-file VERSION.txt
python3 tools/extract_release_archive.py \
  --artifact-dir release-artifacts \
  --destination extracted-prefix
```

These digests detect substitution or corruption after creation; they are not
a signature, provenance attestation, or proof of reproducible builds. The
extractor rejects traversal, duplicate members, unsafe links, unsupported
member types, and bounded-size violations before the packaged-consumer test.
For an exact `v<version>` tag, the release workflow publishes the three
archive/checksum/tuple-manifest sets only after all three packaged consumers
pass. A manual dispatch performs the builds without publishing.
The normative compatibility and release rules are in
[the release policy](release_policy.md).

## Code anchors

- Build configuration and installation: `CMakeLists.txt`
- Presets: `CMakePresets.json`
- Clang toolchain: `cmake/toolchains/clang.cmake`
- Version source: `VERSION.txt`
- Release contract check: `tools/check_release_contract.py`
- Archive staging check: `tools/stage_release_artifacts.py`
- Safe archive extraction: `tools/extract_release_archive.py`
- Artifact manifest: `tools/release_manifest.py`
- Archive workflow: `.github/workflows/release.yml`
# M19-01 package delta

The supported default package installs exactly one additional header,
`rt/extension_abi.h`. It adds no CMake component or target and no transitive
loader dependency. Both C11 and C++20 installed consumers compile the header;
the C++ consumer registers a direct/static entry function. Experimental plugin
headers and targets remain excluded from the default install.

## Optional host-side benchmarks (M23-01)

`RTFW_BUILD_BENCHMARKS=ON` now enables the optional supported host-side library/CLI without requiring experiments. With it OFF, the default target/header inventory is unchanged. The old AoSoA/SoA executable still requires experiments and remains unpromoted.

See [benchmarking](benchmarking.md) for build, install, provider and artifact contracts.


## CPU benchmarks (M23-02)

With benchmarks enabled, the optional install also contains CPU provider/example sources under `share/rtfw/bench/examples`. Consumers compile them against explicitly requested `rtfw::runtime` and `rtfw::benchmark`; no CPU target is added to the exported SDK. `tests/benchmark_fixtures/cpu_consumer/verify_package.py` extends the original clean relocation and embedding gate.
