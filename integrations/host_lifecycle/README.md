# Portable host lifecycle integration

This source kit uses the installed RTFW Runtime and extension ABI v1. It owns a
fixed number of Runtime instances, retains borrowed module/clock/provider/job
owners through checked cleanup, and makes a managed library's unload depend on
all of its tracked Runtime bindings being released. Linux and Windows hosts can
build it without an engine, GPU, FPGA, repository-private header or loader.

```sh
cmake -S /path/to/prefix/share/rtfw/integrations/host_lifecycle -B lifecycle-build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/prefix
cmake --build lifecycle-build --config Release
ctest --test-dir lifecycle-build --build-config Release --output-on-failure
```

CTest selects the exact built module path. The executable also accepts one
absolute path to the explicitly selected trusted demonstration module. It runs
eight load/run/close/unload cycles with two worlds and sixteen frames each. A
controlled first-world quiescence failure retains that world and the library;
the other world closes independently. Releasing the demonstration gate permits
checked retry and actual operating-system unload. Stale handles are rejected
after subsequent reloads. This is real Runtime/module behavior, not Unreal or
physical-device evidence.

For an application, keep a `rtfw_host::Registry<>` alive before constructing
`rtfw_host::Library library(registry)`. Open the selected trusted module, create
worlds with stable non-null owner identities, attach `library.module()`, finalize
and start, then drive `registry.step()` from the serialized host frame thread.
Use `configure_world()` before finalization to attach existing memory/job APIs
and register the application's graph. Its callback may configure the Runtime
but must not retain the Runtime reference or finalize/start/step/stop it.

`close_world()` and `close_all()` close frame admission and retain unresolved
ownership on failure. Retry after the external owner actually becomes quiescent.
Call `library.close()` after world cleanup; it enforces module release before
unload. Never release a managed library's module record manually, keep a symbol
pointer past unload, or unload through a separate platform API. Registry/Library
destructors require explicit completed cleanup and terminate on live ownership.

Every world, module and symbol use is serialized. A module must report all of
its outstanding owners through its extension services; this registry cannot
govern another registry, an untracked Runtime, arbitrary module threads or a
platform handle used outside this host. Borrowed clock, provider/job callback
data and module storage must remain alive through successful world close. A
configuring Runtime has never started; closing it discards its callable tables
before module references are released. Finalized/started Runtime instances
require checked stop/detach, including retry after failed initialization.

`ticks_to_ns()` computes an exact integer floor; supported frequency is 1 through
`UINT64_MAX / 1,000,000,000` ticks per second. Overflow and unsupported frequency
return `invalid_argument` and preserve output. `frame_from_ticks()` also checks
signed nanosecond duration and converts optional absolute deadline and nominal
release ticks. All absolute ticks must already use the injected Runtime clock's
domain. This supplies no pacing or periodic-clock implementation.

Capacities and generation limits are compile-time parameters. Exhausted world
or module slots retire permanently; no generation wraps. Registry identities
are process-local and require a successful `ready()` check. Configuration and
Runtime construction/finalization are control operations with the existing
Runtime allocation behavior. Registry bookkeeping adds no allocation, worker,
mutex or wait; extension/Runtime/OS calls retain their own documented contracts.

The actual Unreal scheduler, FMemory, engine clock, world/PIE/module bindings and
packaged engine sample remain separate work requiring the approved engine
toolchain. Hardware, RT/endurance/performance qualification, independent review,
signing and release remain open. Cyber criteria remain unsatisfied.
