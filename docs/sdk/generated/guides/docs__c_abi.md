[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)

> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.

# Stable C ABI

Release 0.12 introduced ABI version 8, the first stable C ABI for RTFW;
release 1.2.1 retains it. The stability promise applies to the symbols and C
declarations represented by
`abi/rtfw_c_abi_v8.exports`, `rt/include/rt/c_api.h`, and
`rt/include/rt/device_abi.h`. It does not turn the C++ API into a binary ABI,
qualify a deployment for hard real time, or make hardware drivers portable.

M17-01 adds HAL API version 2 only to the C++ source surface in the already
installed `rt/device.hpp` header. It adds no C symbol, C structure, C status,
installed header, target, or SONAME change. Existing C hosts and every
device-ABI-v1 backend continue to use their frozen declarations; the runtime
copies each accepted v1 table into an internal compatibility adapter before
the canonical HAL v2 manager sees it. Native-v2 users must recompile and
receive no C++ binary ABI promise. See [the HAL v2 contract](https://github.com/tranquilWorks/cpp-rt-car/blob/98ecca5a6572dd83d59e26063716f6d76b7a5d4f/docs/hal_v2.md).

M17-02 likewise changes only C++ declarations in the existing installed
headers. Its optional memory/topology extension, six-domain taxonomy,
heterogeneous buffer overload, inspectors, and correlation query add no C
symbol, record, status, capability bit, export, target, header, fingerprint, or
SONAME change. C hosts and device-ABI-v1 backends continue through the exact
M17-01 adapter and implicit borrowed-host mapping. See the
[heterogeneous-memory contract](https://github.com/tranquilWorks/cpp-rt-car/blob/98ecca5a6572dd83d59e26063716f6d76b7a5d4f/docs/heterogeneous_memory.md).

## Compatibility handshake

An integration should compile against one installed header set and check the
loaded library before creating a runtime:

```c
if (rtfw_check_abi(
        RTFW_C_ABI_VERSION,
        RTFW_C_ABI_LAYOUT_FINGERPRINT) != RTFW_STATUS_OK) {
    /* Refuse to continue with mismatched headers/library. */
}
```

`rtfw_get_abi_info()` reports the library's current and minimum compatible ABI
versions, public-surface layout fingerprint, pointer size, and feature flags.
The fingerprint is a reviewed digest of both public C headers; CI rejects an
unreviewed declaration, constant, callback, structure, or device-ABI change.
Compatibility also requires the same process architecture and calling
convention. Loading a 32-bit library into a 64-bit process is outside the
platform loader contract and cannot be repaired by an ABI version check.

Version 8 currently accepts only version 8 and its exact fingerprint. A future
library may retain version-8 compatibility while adding a newer ABI, but it
must keep the complete v8 symbol/declaration behavior and explicitly advertise
the older minimum version. Incompatible changes require a new ABI version,
SONAME, allowlist, digest, and compatibility test; they must not silently
rewrite the v8 manifest.

## Export and package boundary

The shared library uses hidden visibility internally. ELF and Mach-O builds
add an explicit export allowlist; Windows uses `RTFW_API` declarations. Linux
CI compares the built dynamic symbol table with the checked-in list exactly.
The shared-object SONAME follows C ABI version 8 independently of the package
version.

The installed CMake package provides these components and imported targets:

| Component | Imported target | Contract |
| --- | --- | --- |
| `c_shared` | `rtfw::c_shared` | Stable C ABI in the shared library; `rtfw::rtfw` is the 1.x compatibility name |
| `c_static` | `rtfw::c_static` | Same C source API, statically linked; `rtfw::rtfw_static` is the compatibility name |
| `runtime` | `rtfw::runtime` | Preferred C++20 source API; no cross-version C++ ABI promise |
| `cpp_runtime` | `rtfw::simcore_rt` | 1.x compatibility component/target for the supported C++ API and deprecated legacy façade |
| `cuda_backend` | `rtfw::cuda_backend` | Portable CUDA state machine |
| `cuda_driver` | `rtfw::cuda_driver` | Optional toolkit-dependent adapter |
| `xdma_backend` | `rtfw::xdma_backend` | Portable XDMA state machine |
| `xdma_linux` | `rtfw::xdma_linux` | Optional Linux character-device adapter |

For 1.x, CMake version-file compatibility is same-major. The stable C ABI and
the C++ source package therefore have deliberately separate compatibility
policies: C ABI v8 is the binary boundary, while target C++ declarations are
source-compatible and require recompilation.
`tests/package_consumer` is configured outside the source build against a
relocated install tree on Linux and Windows. It runs preferred and compatibility
C/C++ targets, independently consumes portable backends, compiles every
installed header alone, rejects research headers/targets and producer policy,
and verifies the installed Apache-2.0 digest.

## Structure, ownership, and errors

- Initialize every public output/configuration structure with its matching
  `*_init` function.
- Set no reserved field. Inputs accept `struct_size >=` the version-8 size so a
  future compatible library may append fields without reading beyond an older
  caller's declared size.
- All discriminators and statuses use fixed-width integer typedefs. Unknown
  numeric values are rejected before conversion to C++ enums.
- The runtime copies configuration, names, and host-adapter callback tables.
  Callback user data, device instances, registered buffers, registered state,
  and host job-system user data remain caller-owned for their documented
  lifetime.
- Device-owning callers must require `rtfw_stop()` to return
  `RTFW_STATUS_OK` before releasing borrowed objects or calling
  `rtfw_destroy()`. Failed device cleanup is retryable and leaves the public
  runtime state unchanged while execution and state mutation are gated.
- ABI v8 retains its void `rtfw_destroy()`. The implementation does not delete
  a handle when its implicit stop reports a device-cleanup failure, but the
  void call cannot safely advertise that outcome. This fail-safe preserves a
  pointer already known to require cleanup; it is not a substitute for checked
  `rtfw_stop()`.
- Returned error strings are library-owned. A handle-specific error may change
  on the next call using that handle.
- `RTFW_STATUS_INCOMPATIBLE_ABI` is reserved for header/library ABI mismatch;
  checkpoint and replay mismatch continues to use
  `RTFW_STATUS_INCOMPATIBLE_ARTIFACT`.
- HAL v2 callback failures and unknown or malformed backend results map to the
  existing runtime statuses. They add no status value and never reinterpret
  device ABI v1 `UNSUPPORTED` as success.

## Host job system adapter

`RTFW_EXECUTOR_HOST_ADAPTER` and the equivalent C++ `host_adapter` policy run
the compiled graph and nested CPU work through a borrowed host job system. The
adapter is attached while configuring and must declare the same worker count
and total host-job reservation as the runtime configuration.

For each accepted job, the host copies `rtfw_host_job`, then invokes `execute`
exactly once with the unchanged execution context, completion context,
completion token, and a worker index below the declared count. The scratch
pointer is runtime-owned, aligned, exclusive to that invocation, and expires
when `execute` returns. Completion tokens prevent a stale duplicate callback
from completing a later job that reused the same scratch slot. Token and slot
state are one atomic generation word, and generation exhaustion fails closed
instead of recycling an opaque token.

`submit` is a bounded, nonblocking operation. It returns
`RTFW_STATUS_QUEUE_FULL` only when it did not accept or invoke the job.
`try_execute_one` executes at most one accepted job and is required: runtime
callbacks may synchronously submit nested ranges/reductions, so a worker waiting
for children must be able to help the host queue. The runtime creates zero CPU
worker threads for this policy (`worker_starts == 0`); the host owns worker
creation, affinity, priority, shutdown, and any memory used by its queues.

These are RT0 functional rules. An engine adapter inherits no RT1/RT2 claim
until the host job system, operating system, allocator behavior, and complete
deployment tuple are qualified together.

## Release evidence

- header/manifest/fingerprint check: `tools/check_c_abi.py`;
- exact shared-library export check: `abi/rtfw_c_abi_v8.exports`;
- dynamic legacy-surface and failed-teardown retry coverage:
  `tests/test_cabi_dlopen.c`;
- C and C++ host-adapter, prestarted host-team concurrency, saturation,
  stale-completion, and no-allocation coverage:
  `tests/host_adapter_tests.cpp`;
- relocated installed consumers: `tests/package_consumer`,
  `.github/workflows/ci.yml`.

The complete deprecation, package-version, and release rules are in the
[release policy](docs__release_policy.md).

M17-03 changes only additive C++ declarations in the already installed
`rt/device.hpp`, `rt/runtime.hpp`, and `rt/config.hpp`. It adds no C symbol,
installed header, target, or C representation and makes no C++ binary ABI
promise. Stable C ABI v8 remains exactly 70 exports with its frozen fingerprint
and SONAME 8; device ABI v1, its layouts/values, and Runtime status numeric
values remain unchanged.

M17-04 likewise changes only source-level C++ vendor records and methods in
the already installed CUDA and XDMA headers. The additive version-2 driver
tails and native HAL-v2 registration tables require recompilation and carry no
C++ binary ABI promise. Stable C ABI v8 remains exactly 70 exports with its
frozen fingerprint and SONAME 8; every device-ABI-v1 declaration, layout, and
value remains unchanged.
# Extension ABI boundary

`rt/extension_abi.h` version 1 is a separate registration ABI, not a revision
of this stable C ABI. It reuses the frozen v8 callback result and context types
but adds no `librtfw` export. Its documented entry symbol is resolved and owned
by the embedding host and passed directly to the additive C++ Runtime API.
Stable C ABI v8 remains exactly 70 exports, fingerprint
`0xd0e7a5a14bf35f97`, and SONAME 8. See
[extension registration](https://github.com/tranquilWorks/cpp-rt-car/blob/98ecca5a6572dd83d59e26063716f6d76b7a5d4f/docs/extension_registration.md).

## M17-06 ABI boundary

The Runtime discovery repair is internal and the combined executable uses only
existing additive C++ source contracts and targets. M17-06 adds no public
declaration, C or extension ABI symbol, status value, installed header, target,
or C representation. Stable C ABI v8 remains exactly 70 exports with
fingerprint `0xd0e7a5a14bf35f97` and SONAME 8; device ABI v1 and both candidate
`api()` behaviors remain unchanged.
