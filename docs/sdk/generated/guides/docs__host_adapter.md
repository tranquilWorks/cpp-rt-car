[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)

> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.

# Independent headless host

This optional source kit demonstrates a host with its own jobs, fixed memory
arenas, clock, frame loop and telemetry consumer. It runs two public RTFW Runtime
instances, using either native RTFW workers or host-adapter workers. It requires
C++20, CMake 3.20+, a 64-bit Linux or Windows toolchain, and 4096-byte OS pages.
No Unreal or repository-private header is needed. The kit adds no public SDK
header, exported target or mandatory Runtime dependency. It is a small reference,
not a general-purpose game engine or a hardware/RT/performance qualification.

## Installed and embedded entry points

Install RTFW, then use its data directory (normally `share/rtfw`; custom
`CMAKE_INSTALL_DATADIR` is supported):

```sh
cmake -S "$PREFIX/share/rtfw/examples/host_adapter" -B host-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX"
cmake --build host-build --config Release --parallel 2
./host-build/host_adapter native
./host-build/host_adapter host
ctest --test-dir host-build -C Release --output-on-failure
```

Visual Studio executables are `host-build/Release/host_adapter.exe`. Source
embedding uses the same entry point:

```sh
cmake -S samples/host_adapter -B host-build -DCMAKE_BUILD_TYPE=Release -DHOST_RTFW_SOURCE="$PWD"
```

`find_package(rtfw CONFIG REQUIRED COMPONENTS runtime)` and explicit
`add_subdirectory` embedding both use only `rtfw::runtime`. Windows links the
reference's residency observer to `psapi`, without adding that dependency to RTFW.
Root examples remain opt-in when embedding; their executable has a separate name.
Expected complete outputs:

```text
host_adapter: mode=native frames=16 instances=2 checksums=2176,6528 telemetry=32/32 lost=0 jobs=balanced memory=6/6 stopped=ok
host_adapter: mode=host frames=16 instances=2 checksums=2176,6528 telemetry=32/32 lost=0 jobs=balanced memory=6/6 stopped=ok
```

The host integrates 16 integer elements in each world over 16 frames. For world
multipliers 1 and 3, each element gains `multiplier * (index + 1)` per frame.
A nested four-row/four-column task graph updates disjoint elements, followed by
a dependent checksum phase. The frame owner checks every element after every
step against an independent formula. Both instances remain alive throughout the
loop. Native and adapter modes must produce identical states; execution order
and observed timestamps need not match.

## Jobs and nested work

`Jobs` owns 32 fixed slots and two worker threads, plus a registered frame-thread
helper. The adapter declares three execution indices: helper0 and workers1/2.
All Runtime control sharing a pool belongs to the single registered frame thread;
foreign threads cannot help. Independent hosts can run on distinct control threads.
Submission can occur concurrently from Runtime callbacks and the frame thread.

Each slot moves empty → copying → ready → executing → empty using lock-free
atomic state. Submit performs one bounded scan, claims an empty slot, copies the
entire public `HostExecutorJob`, increments ownership counters and release-publishes
ready. Capacity failure accepts nothing and returns `queue_full`; no callback is
invoked for rejected work. Execution acquire-claims a ready slot, preserves the
opaque completion identity and invokes the copied callback exactly once. The slot
remains reserved until that callback returns. Scratch belongs to Runtime and is
never stored beyond execution. Accepted throwing jobs terminate: replaying them
would violate exactly-once ownership.

`try_execute_one` executes at most one job per call, without a queue lock. Nested
Runtime work can cooperatively help a saturated team, including a deterministic
fixture with no background workers. The Runtime's synchronous nested call still
waits until accepted children finish; the helper does not make a bounded-duration
promise for arbitrary caller callbacks. This reference graph has fixed nesting,
64 task-scratch slots and 64 bytes of exclusive scratch per task. Sentinel tests
check that nested child scratch does not overwrite its parent.

There is no allocation, mutex, I/O, sleeping or thread creation in queue callbacks
or the graph. A scan may return `queue_full` under contention even if a slot becomes
free immediately afterward. Worker idle loops yield to the OS. Thread creation,
join and a five-second quiescence wait occur only on the control path. These are
ordinary OS threads, not hard-real-time execution guarantees.

## Host memory and observation

Each Runtime has its own three-region fixed arena: phase scratch, task scratch
and trace storage, each with capacity65536 bytes and alignment4096. The containing
Host is allocated once before execution to avoid placing large arenas on a small
thread stack. Acquire validates category, size, power-of-two alignment, policy
and duplicate ownership. It reports actual usable/committed logical bytes within
its fixed backing span. Release does not free the Host's arena; it returns that
region to the host for a later checked lifecycle.

Only ordinary host storage is supported. Guard pages, page rounding, huge pages,
NUMA placement, prefault/first-touch policy, locking, pinning and strict residency
requirements are rejected. No such capability is inferred from successful
allocation. The host selects these policies explicitly disabled. Startup's
independent observer uses [Linux mincore](https://man7.org/linux/man-pages/man2/mincore.2.html)
or [Windows QueryWorkingSetEx](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-queryworkingsetex)
to sample residency of at most16 pages per region. Reported bytes are clipped to
the logical span. This is a momentary OS observation, not a guarantee that pages
remain resident. It does not claim memory locking, pinning or NUMA observation.
Unsupported page size or failed observation produces an explicit startup failure.

The provider advertises policy-operation and independent-observation capabilities
because both callbacks are implemented; its no-op policy application grants no
privileged capability. Each live token has explicit acquired/applied state.
Failed rollback retains the token and backing storage for checked retry. Invalid,
double or premature release records a violation and retains live storage. Provider
callbacks run serially under one Runtime's lifecycle; never share one provider
between concurrently controlled Runtime instances or retain tokens outside it.
After a successful close, the Runtime object is destroyed before host detachment.

The public memory plan is checked against these three capacities. This provider
**does not replace every Runtime allocation**: Runtime control structures, native
thread stacks and host queue/thread/arena ownership remain distinct. The output's
`memory=6/6` counts acquired/released regions across two instances, not all bytes
or OS allocations. No complete memory-accounting or RSS claim is made.

## Clock, frame loop and telemetry

The injected host clock uses `steady_clock` monotonic nanoseconds and outlives the
Runtime objects. The finite host loop supplies logical one-millisecond frame
increments; `step` does not pace the host or sleep. A zero or regressing host frame
timestamp rejects admission with `clock_failure`. No cross-clock conversion or
periodic absolute-sleep implementation is claimed.

After each step, the frame thread drains trace into a fixed32-record buffer, with
at most nine reads for the configured capacity256. It checks schema, record size,
runtime identity, sequence and loss count. Cumulative metrics must report exact
completed-frame/callback counts and zero failed frames. Each instance has separate
cursors; cross-instance reuse is rejected. Tests deliberately overflow capacity8
and verify the exact16 lost events after four six-event frames. Normal runs lose
none. Printing occurs after checked shutdown, outside callbacks. Telemetry drain
covers startup and frame execution; no post-release trace access is required.

## Checked shutdown and failures

The frame owner stops admitting frames and joins any external frame/submit callers
before close. It then waits for host jobs to finish their callback-return epilogues,
checks Runtime stop, verifies that provider tokens are released, destroys the
Runtime object and detaches the pool owner. Only after every Runtime owner detaches
may the pool stop and join its workers. Never close the pool underneath nested
work or discard a queued accepted job. `close` rejects attached/nonquiescent pools.

Partial worker creation joins already-created workers. Failed configuration or
finalization destroys the configuring Runtime only after checking that no provider
token remains. Failed startup and failed stop retain borrowed storage/host ownership
until checked cleanup succeeds. The example makes one explicit cleanup retry;
unresolved destruction terminates instead of silently freeing live owners.
A callback failure ends frame admission; partial world updates are not rolled back.
It is an error to free the clock, provider, callbacks or job owner before this
sequence completes. Repeated lifecycles create new Runtime objects and reacquire
only fully released regions.

## Evidence and package boundary

Tests exercise 4096 exact-once jobs from concurrent producers, copied records,
capacity rejection, nested helper progress, parent/child scratch isolation,
zero-allocation steady frames, actual OS observation, bad requests, partial start,
clock/callback failure, stop retry, telemetry loss, repeated ownership and
simultaneously live independent hosts. ASan/UBSan/leak, TSan, Clang analysis and
Linux/Windows hosted checks cover the reference. These establish software behavior,
not worst-case latency or physical qualification. Independent unfamiliar-human
first-use review remains unperformed.

Installed source inventory: `CMakeLists.txt`, `check_output.cmake`, `main.cpp`,
`host.hpp`, `jobs.hpp`, `memory.hpp`, plus this guide in the data directory.
Package verification removes the original prefix, relocates a tests-disabled SDK
with custom paths and spaces, checks exact files/bytes, runs both modes and the
complete SDK suite, embeds the source and checks missing package/source/header and
incorrect-output negatives. Earlier raw, typed, backend, CIL and CUDA kits remain
covered. M25-06 documentation closure and M26 remain separate later work.
