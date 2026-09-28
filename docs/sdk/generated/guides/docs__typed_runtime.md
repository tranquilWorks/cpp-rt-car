[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)

> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.

# Optional typed Runtime SDK

Include `<rt/sdk.hpp>` and link only `rtfw::runtime` (C++20). These header-only
helpers add no compiled ABI, registry, hidden thread or steady-state allocation.
The raw API and the raw hello example remain supported. Configuration and graph
finalization retain the raw Runtime's setup-time allocation behavior.

From an installed SDK (adjust the prefix and configurable data directory):

```bash
cmake -S rtfw-sdk/share/rtfw/examples/typed_runtime -B build-typed -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/rtfw-sdk"
cmake --build build-typed --config Release --parallel 2
ctest --test-dir build-typed -C Release --output-on-failure
```

For source embedding, configure `samples/typed_runtime` with
`-DTYPED_RTFW_SOURCE=/absolute/path/to/cpp-rt-car` instead of the prefix option.
The shipped kit also accepts this option. It requires no private headers.
Expected program output:

```text
typed_runtime: frames=3 produced=3 consumed=3 stopped=ok
```

## State and callbacks

`rt::sdk::callback<&function>(name, state)` returns an ordinary
`CallbackRegistration`. `GraphBuilder::phase<&function>(name, state, handle)`
registers it immediately. Free functions take `(State&, const CallbackContext&)`;
member functions take `(const CallbackContext&)`. Both must be `noexcept` and
return exactly `CallbackResult`. Throwing signatures, wrong result types, null
functions, temporary state and const state are rejected during compilation.
No callable captures are stored. A `noexcept` annotation does not make allocating,
blocking or I/O-performing application code safe: callbacks must obey the raw
Runtime contract and express application failure with `CallbackResult::error`.

Declare callback state before Runtime, and the guard after successful finalization.
Keep each at the same address through successful checked stop. Helpers do not own
or extend state lifetime. Do not move Runtime or its borrowed state while a helper
refers to it. A callback context and its scratch spans expire on callback return.
Use ordinary resource declarations and dependencies to order shared state access.

## Configuration and graph validation

`ConfigBuilder(config)` copies the explicit `RuntimeConfig` unchanged. `value()`
exposes it for inspection; `set(key, value)` uses the same strict public config
parser, and `apply(runtime)` returns the exact `Runtime::configure` status.
No failed call is remembered or silently ignored: check every result. Subsequent
successful calls can repair the configuration. Default construction retains every
raw default; it does not tune or infer a capacity.

`GraphBuilder` borrows Runtime. `resource`, `access` and `finalize` directly return
raw validation results. `depends_on(dependent, prerequisite)` adds the raw edge
from prerequisite to dependent. Phase/resource handles are ordinary instance-local
handles. No hidden nodes, dependency inference, automatic finalization or start.
An error does not undo earlier successful declarations. A missing ordering edge
can be added after `resource_conflict`; a cyclic graph must be rebuilt because the
raw API has no edge removal. Mixing raw declarations with helpers is supported.
Setup and lifecycle operations are control-thread operations, not concurrent APIs.

## Capacity calculations

`aligned_capacity(bytes, alignment, count, out)` computes rounded stride and total
bytes with checked arithmetic. A non-power-of-two/zero alignment returns
`invalid_argument`; addition/multiplication overflow returns `capacity_exceeded`.
Zero bytes or count is valid. Outputs are unchanged on failure.

`estimate_native_storage(config, phase_count, out)` calculates **only** phase
scratch, task scratch and native executor queue slots. It checks phase capacity,
scratch arithmetic and queue multiplication; it is not full configuration
validation. It rejects host-adapter and unknown executor policies with
`invalid_config`. It never adjusts capacities. Even a successful estimate can
be followed by a raw configuration/finalization rejection.

Always call `configure`/`finalize` and read `Runtime::memory_plan()` for the exact
committed plan and memory-budget decision. Estimates exclude trace storage and
all Runtime/executor/device controls, borrowed payloads and OS storage; adding
scratch totals is not a complete budget estimate. The example compares native
queue slots to the actual plan before starting.

## Checked shutdown

Construct `CheckedStopGuard(runtime)` **after successful finalize and before
start**. Call and check `close()` explicitly. A successful close disarms it;
repeated close returns `ok` without another stop. A failed close returns the raw
status and leaves it armed: keep backend/provider/state owners alive, inspect
the failure, and retry when the backend permits. Do not assume any failed cleanup
released ownership. Start/step failures still require this cleanup path.

On scope exit an armed guard attempts `stop()` once. If it fails, the guard calls
`std::terminate()` to prevent unwinding through borrowed owners with unresolved
ownership. This is a fail-closed fallback, not recoverable error reporting.
Handle recoverable failure with explicit `close()` before leaving the scope.
There is no detach/release escape hatch, silent failure, or indefinite retry loop.
The guard is noncopyable/nonmovable and must be destroyed before Runtime and all
borrowed owners. Raw Runtime remains available when an application needs a
different shutdown policy. Runtime stop has its existing backend timeout bounds;
the helper promises no shorter latency or RT qualification.

## Diagnostics

`render_error(span<char>, operation, status, detail)` renders the operation, raw
status name and numeric value, optional detail and recovery hint without heap or
I/O. Pass `runtime.last_error()` immediately after failure; that borrowed view can
change on the next Runtime operation. Inputs must not overlap the output buffer.
The result reports `required_bytes` including NUL, `written_bytes` excluding NUL,
and `truncated`. Every nonempty output span is NUL terminated; an empty span can
query the required extent. Fixed storage is suitable for bounded capture; write
logs later on the host, outside callbacks. Preserve the returned status separately.
Hints suggest investigation, never automatic retry or restoration of application
side effects. Unknown status values retain their numeric code.

This is portable source-level usability evidence. Independent novice review,
physical CUDA/XDMA, RT qualification, controlled performance and release remain
separate. Backend authoring, shared-memory CIL, host adapters and full API recipes
belong to later M25 batches.
