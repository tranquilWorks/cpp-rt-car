[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)

> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.

# CUDA failure and lifetime conformance

The M24-03 portable runner exercises the [integer reference](docs__cuda_physics.md)
and [two-lane pipeline](docs__cuda_pipeline.md) through installed public Runtime and
CUDA backend APIs. It uses no GPU and establishes simulated software behavior.
All nine particle fields retain exact integer equality; nonzero floating-point
tolerances are inapplicable to this unchanged validation model.

Build `sample_cuda_lifetime` normally, then run it without arguments. `--help`
returns 0 without executing; invalid syntax returns 2, a conformance failure 1,
and a complete pass 0. The same finite suite is installed as source data:

```sh
cmake -S /path/to/prefix/share/rtfw/examples/cuda_physics -B consumer \
  -DCMAKE_PREFIX_PATH=/path/to/prefix
cmake --build consumer --parallel 2
ctest --test-dir consumer --output-on-failure
./consumer/lifetime_consumer
```

Custom data directories are supported. `PHYSICS_RTFW_SOURCE` selects the existing
`add_subdirectory` consumer path. The kit has exactly three additional files in
`lifetime/`: `protocol.hpp`, `conformance.hpp`, and `main.cpp`. Original reference
and pipeline inventories and the default SDK exports/headers are unchanged.

## Evidence boundaries

The actual CUDA backend runs every accepted command batch. The injected Driver
API moves actual bytes and computes the independent iterative kernel. Driver
faults cover launch/query failure, terminal timeout, context loss, registration,
synchronization and destruction. Its synthetic clock offset advances only the
injected driver's timeout clock; it does not change the Runtime or physics clock.

The separately labeled `Protocol` adapter wraps the public command extension to
inject queue rejection, malformed native descriptors, stale batch identity and mismatched
timeline signals. These are HAL protocol tests, not claims that CUDA emits those
protocol records. A separate provider mutation requires Runtime to reject a zero
timeout before any backend submission. Valid dispatch/poll/cancel/stop calls forward to the actual
backend. The stale-completion test replays the first completed batch during the second
step and requires terminal rejection with no second publication or timeline advance.

CUDA does not support canceling an accepted kernel. Concurrent checked stop
requests cancellation and returns invalid_state until the host joins the step.
Ordinary frames then report device_canceled; active noncancelable references
retain ownership until their unchanged five-second budget and report device_timeout;
borrowed storage stays live until a later checked stop succeeds. Failure tests
require suppression of CPU publication. Quarantine reset may synchronize on the
host control path and can fail/retry. Normal successful steps must perform no
synchronization. A lost context permanently rejects reset: checked teardown is
followed by a fresh caller session and Runtime, not in-place lost-device recovery.

The sample's optional copied `Instrumentation` callbacks are selected only before
prepare, default to null, and borrow their owner through checked stop. The
conformance kit uses them for bounded protocol injection and capacity validation.
The host must join active steps before using health, timeline, reset, ordinary
stop or non-atomic result access. `request_stop` is the narrow concurrent stop
request entry point. Failed sessions cannot resume particle stepping; reset
establishes safe retirement, and a fresh scenario restarts from its declared seed.

## Verification and remaining work

Every fault has a reachability counter or observed accepted-event condition.
Event create/destroy and host register/unregister totals must balance after
checked cleanup; backend allocation/free counters must remain zero. Repeated
lifetimes and simultaneous healthy/failing instances verify isolated ownership.
The test executable measures steady allocations separately from setup. Existing
M24-01/02 exact maximum parity and CLI tests remain mandatory, including the
Linux 512 KiB actual CLI process-stack method. This is not an embedded Debug/TSan
thread-stack guarantee.

Compiler/FMA/MSVC, sanitizers, static analysis, relocated/embedded consumers and
ABI checks are required. Retained commands, outcomes and failures are in
[batch evidence](https://github.com/tranquilWorks/cpp-rt-car/blob/912ce878ad311d38e452a5d2916ee0daa6ee3f72/docs/evidence/M24-03-2026-09-27.md); final hosted/merge identities are
recorded in its PR. Physical GPU execution, controlled timing, independent human
review and qualification remain separate. M24-04 still owns benchmark/profiler
integration and the capability matrix's 90% overall / 100% critical coverage gate.
