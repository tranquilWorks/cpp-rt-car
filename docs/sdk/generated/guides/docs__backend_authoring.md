[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)

> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.

# Author a bounded HAL-v2 backend

The installed `examples/backend_authoring` directory is a source-only teaching
kit for the **single-slot host-copy profile**. Copy it into your application,
change the command/profile deliberately, and retain its conformance checks. It
uses installed public APIs only. It adds no SDK header, exported target, compiled
ABI, driver support, or hardware/RT qualification. The portable product remains
1.2.1 at RT0, C ABI v8/SONAME8, device ABI v1 and Apache-2.0.

## Build and run

With a previously installed SDK at `$PWD/sdk` and the default data directory:

```sh
cmake -S sdk/share/rtfw/examples/backend_authoring -B build-backend \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/sdk"
cmake --build build-backend --config Release --parallel 2
ctest --test-dir build-backend -C Release --output-on-failure
```

For source embedding, select a source checkout explicitly:

```sh
cmake -S samples/backend_authoring -B build-backend-embedded \
  -DCMAKE_BUILD_TYPE=Release -DBACKEND_RTFW_SOURCE="$PWD"
cmake --build build-backend-embedded --config Release --parallel 2
ctest --test-dir build-backend-embedded -C Release --output-on-failure
```

The expected line is:

```text
backend_authoring: conformance=ok native=3 v1=3 verified=3 ownership=0
```

Use your configured install data directory instead of `share` when customized.
The kit consists of `CMakeLists.txt`, `check_output.cmake`, `main.cpp`,
`backend.hpp`, `profile.hpp`, `conformance.hpp`, and `runtime_example.hpp`.
It links only `rtfw::runtime`, which supplies C++20 and platform link requirements.
No private include path, vendor SDK or experimental target is needed. The
root example has the distinct target `sample_backend_authoring`, so explicit
examples-ON embedding is also supported.

## Capabilities to decide before writing a driver

| Decision | This profile's contract |
| --- | --- |
| Core table | All ten functions, non-null borrowed instance, version2, full known record sizes, zero reserved fields |
| Capacity | One accepted command, at most two registered buffers, at most256 bytes per buffer/copy |
| Negotiation | Exactly one requested command; one or two requested registrations, enforced for the current initialization |
| Command | Opcode1, zero payload, exactly two equal-length nonempty references: source READ and destination WRITE |
| Bounds | Each offset/length fits its registered extent using subtraction; no overflow-prone addition |
| Flags | Known buffer flags only; device-read/write permissions required for the corresponding reference |
| Identity | Nonzero instance-local buffer tokens never recycled; token exhaustion fails before publication |
| Cancellation | Successful cancel retires the accepted ID with no completion and no payload write; racing retirement returns non-success |
| Reset | Caller-serialized, no outstanding work; preserves registrations, increments generation, clears failure health |
| Discovery | Complete fixed records, printable identifier, truthful storage/capacity and mock/cancel/reset flags |
| Extensions | None: memory/topology and command/timeline pointers remain null |

Input sizes may be larger for compatible tail extension; all known input fields
are checked. Inactive payload/reference array elements have no semantics. Buffer
names must be nonempty, terminated ASCII identifiers. Tokens are scoped to one
backend instance; do not use them with a different instance. Runtime logical
buffer handles additionally bind the owning Runtime. Caller-provided memory must
be valid host storage for the complete declared extent throughout ownership.

`backend.hpp` is intentionally a small core example. It does not implement
production device discovery, interrupt handling, DMA, persistent device loss,
real-time clock qualification or vendor APIs. `deterministic_mock` identifies
its fixture nature; wall-clock timeout enforcement does not promise deterministic
wall-clock scheduling or latency.

## Fixed storage and publication

The backend holds two fixed registration records and one fixed command record.
The atomic slot moves through `free -> writing -> pending -> retiring -> free`.
Submit claims free once, copies the command and acceptance time, then publishes
pending with release ordering. Poll or cancel claims pending once with acquire
ordering. Poll writes at most256 bytes before emitting one exact-ID completion;
only then does it release the slot. A second submit sees queue_full before taking
ownership. Poll/cancel races have one winner. There is no retry/spin loop inside
the backend, no heap allocation, mutex, hidden thread or I/O. The source requires
lock-free atomics on supported64-bit platforms. `memmove` permits overlapping
host regions; the bounded byte count does not establish a worst-case timing bound.

Submit callers may race each other; the intended Runtime arrangement has a
single completion consumer. Direct poll and cancel use the same ownership CAS.
Lifecycle, registration and reset are caller-serialized **after all execution
callers have joined**. Health may be inspected during execution but its individual
atomic counters are an observation, not one transactional snapshot. Inspect after
quiescence for conservation assertions. The setup ownership and registration
inspection hooks are control-thread-only. Fault injection sets one atomic
one-shot fixture fault; setting another replaces an unconsumed fault.

A one-slot backend limits throughput. The sample serializes native copy before
legacy fill because Runtime's outstanding capacity is one. Expanding capacity
requires separate bounded publication/retirement for each slot and new capacity,
concurrency and cancellation tests; increasing the advertised number alone is
incorrect. Do not insert blocking native-driver calls into submit/poll to emulate
an asynchronous backend.

## Lifetime and ownership

| Stage | Owner and permitted next action |
| --- | --- |
| Table registration | Runtime copies the table; application retains backend instance |
| Initialize attempted | Even an error can leave setup owned; checked shutdown must release it |
| Buffer registration success | Backend borrows the extent until successful unregister; failed registration here returns zero and transfers nothing |
| Submit success | Backend borrows both references until completion or successful cancellation |
| Submit rejection | No new ownership; caller retains input and may act on the explicit status |
| Pending command | Unregister, reset and shutdown reject; retain all borrowed memory |
| Completion/failure | Exact ID retires once; failures make no copy and require reset before another submission |
| Unregister failure | Token and storage remain owned; preserve them and retry explicitly |
| Shutdown failure | Setup remains owned; retry after the reported problem is resolved |
| Checked Runtime stop success | Runtime's borrowed backend/buffer/callback state may be destroyed |

Declare backend, buffers and callback state before Runtime and its checked-stop
guard. Construct `rt::sdk::CheckedStopGuard` after successful finalize. Check
`close()` explicitly when cleanup can fail; retain every borrowed object until
it succeeds. Its destructor terminates if its last cleanup attempt still fails.
Backend destruction is not an implicit cancellation mechanism. A health value
of shutdown alone is not proof that partial-start ownership has been released.

Core Runtime stop must not be assumed to cancel arbitrary provider-held pending
work. This example steps to a terminal completion before stopping. Direct
conformance demonstrates explicit cancel/drain before unregister. In a new
adapter, define and test how stop quiesces the provider; retain storage on an
unresolved result. Do not treat `invalid_state` as a universal cleanup-success
code, especially following partial initialization.

## Failure matrix

| Injected/observed condition | Result | Ownership and recovery |
| --- | --- | --- |
| Partial initialization | error | Setup marker retained; shutdown succeeds or reports its own retryable error |
| Registration fault | error, token0 | No buffer acquired; prior registrations unchanged |
| Registry capacity | resource_exhausted, token0 | No acquisition beyond negotiated limit |
| Queue saturation | queue_full | Existing command unchanged; rejected ID is not completed |
| Invalid token/access/size/reserved/range | invalid_argument | No accepted command or side effect |
| Completion error | exact-ID error, value0 | No write; reset_required health; quiescent reset then retry |
| Deadline/injected timeout | exact-ID timeout, value0 | No write; reset_required health; reset then retry |
| Injected loss | exact-ID lost, value0 | No write; lost health; fixture reset recovers (not physical recovery) |
| Wrong cancel ID | invalid_argument | Original pending command remains owned |
| Cancel loses retirement race | invalid_state | Keep storage until completion or an explicit later successful action |
| Unregister fault | error | Exact registration preserved for retry |
| Shutdown fault | error | Setup retained for retry; no success-shaped ownership shortcut |

Command outcome counters are lifetime unsigned64-bit observations (ordinary
unsigned wrap applies); `last_status` tracks completion/cancel/reset, not every
control-call return. Successful initialization and
reset advance generation. Successful reset requires quiescence and keeps valid
buffer registrations; shutdown followed by initialization allocates fresh token
values. Neither reset nor failed work can produce a delayed payload write.

## Reuse the conformance harness

`conformance.hpp` accepts a public `HalV2BackendApi`, a `Profile` with the copy
opcode plus explicit fault/ownership fixture hooks, and caller-owned `Storage`.
It has no dependency on `Backend`, a unit-test framework or private Runtime
headers. The fixed scenario list makes at most160 HAL calls and records at
most128 named results. `cleanup` makes at most five calls per explicit retry;
it does not wait for work or retry indefinitely.

Start with a fresh instance and storage. Keep both alive while
`report.cleanup_complete` is false. Inspect each failed named check. Repair or
quiesce the fixture and call `cleanup` again explicitly; never discard the
storage simply because `run` returned. If a malicious/defective backend reports
success with a zero token or conceals ownership, generic cleanup cannot invent
the lost identity. The negative-control tests retain independently saved real
tokens for fixture repair. Production adapters need their own truthful ownership
inspection and fail-closed policy; the sample executable terminates rather than
unwinding unresolved borrowed storage.

The profile tests discovery, malformed inputs, negotiated saturation, stale
handles, exact completion bytes/count/ID, no duplicate completion, cancellation,
health/counters/reset, partial start and cleanup retry. Repository tests add ten
independent defective wrappers, malformed boundaries, repeated generations,
10,000 concurrent submissions/completions, zero allocations in direct and
Runtime steady execution, two isolated Runtime instances, native failure status
propagation and failed-start/stop ownership. ASan/UBSan/leaks, TSan, strict builds
and installed consumers are complementary evidence. Passing this named profile
is not universal HAL conformance, independent novice usability review or vendor
qualification.

## Device ABI v1 and native extensions

`runtime_example.hpp` registers this C++ native HAL-v2 table alongside the public
`MockDeviceBackend::api()` C device-ABI-v1 table. Native copy precedes legacy fill;
a dependent CPU callback checks every byte and the frame sequence. Three steps
must verify before checked stop releases ownership. Runtime translates v1 core
records/statuses through its existing bounded compatibility adapter. The example
changes neither device ABI v1 nor the stable C Runtime ABI. Native HAL-v2 is a
C++ source contract, not a replacement binary ABI promise.

A v1 backend can continue using the existing core table without advertising
native-only extensions. Optional native memory/topology and command/timeline
extensions require their own complete tables, capability discovery, memory
domain/handle lifetimes and synchronization/stop semantics. They are absent here;
unsupported extension functionality is not silently emulated. For vendor work,
keep toolkit/driver versions, permitted control-thread operations, registered
memory lifetime, finite completion budgets and physical qualification separate.
Portable copy tests do not qualify CUDA, XDMA, peer DMA, electrical safe output,
RT1/RT2 or controlled performance.
