# Proposed M26-04R2: explicit per-backend command capacity

Status: proposal only; no Runtime, native backend, prior test or frozen contract
change is implemented or authorized by this document. M26-04R storage is already
merged as target280 at e8caf4396cd5987d9aa4a4bf3dad9a178cf2ded7 after all32 gates.
M26-04 is reactivated by control562 at 3af5a3db321a51fa61b7fb18f133ddbdd3eed689.

## Reproduced second prerequisite

The original four-slot XDMA probe now executes24 ticks, actual transfers and
startup safe acknowledgements, matching the unchanged golden oracle. New native
and independent host-job tests also match every canonical non-owner byte against
an actual CPU owner and pass originating-owner trusted replay with zero ordinary
heap allocations, including the native XDMA workers. Allocation positive control
is detected. These are count9/ticks24 partial results, not complete feature proof.

Combined CUDA/XDMA fails before execution. Its existing CUDA backend has one
slot, XDMA has two, and the three rate phases share nominal release0. Runtime's
conservative admission plan counts three reservations, even though graph/channel
ordering serializes the actual dependent work. The current API simultaneously
requires every backend's native command capacity to be at least the entire
Runtime device_outstanding_capacity, and later initializes every backend with
that same count.

Relevant existing mechanisms:

- rt/src/host_runtime.cpp: register_device_backend rejects command/timeline
  capacities below the global configured outstanding/completion bounds;
  finalize separately requires every backend core max_in_flight to cover it.
- rt/src/rate_dispatch.cpp: compile_device_rate_plan maintains separate global,
  backend and phase demands; at the common nominal release the global demand is3.
- rt/src/device_manager.cpp: constructor gives each command backend the global
  slot count; start forwards that count as requested_in_flight to every backend.
- Native CUDA/XDMA correctly enforce their own actual configured capacities.
  The preserved CUDA source kit owns its fixed event pool and one-slot backend.

Retained diagnostics vary only sample configuration in isolated copies. Capacity1
with native CUDA1/XDMA2 and capacity2 with a diagnostic CUDA2/XDMA2 both reject
at admission before driver initialization. Capacity3 with the real original
CUDA1/XDMA2 rejects at registration. A diagnostic-only all-three-slots variant
passes finalization but violates the frozen XDMA bound; attempting startup also
exhausts the unchanged CUDA simulator event pool. It is not a viable workaround
and is not installed or used by the feature. The finalization-only controls stop
before device initialization. Original failures and diagnostic sources remain
retained under M26-04-capacity-blocker.

## Recommended bounded repair

Add an explicit, additive C++ opt-in capacity policy for native HAL-v2
command/timeline backends. Preserve the default uniform policy exactly, including
existing rejection behavior, identities, artifact bytes and all prior tests.
Keep RuntimeConfig schema7 and its existing layout, stable C ABIv8/70exports/
fingerprint0xd0e7a5a14bf35f97, SONAME8 and deviceABIv1 unchanged.

The opt-in would distinguish aggregate admission capacity from each backend's
actual native capacity. Resolve bounded per-backend outstanding/completion counts
from validated capabilities; use those exact counts consistently for private slot
allocation, initialization requests, submission limits, polling bounds, retirement,
stop/reset/retry, replay/checkpoint reconstruction and MemoryPlan. Global admission
and memory caps still apply; do not weaken its global/backend/phase checks or
change deadline/rate/safe-ACK rules. No advertised capacity may exceed real storage.
Unsupported legacy or extension combinations should reject under the opt-in;
the default legacy/native paths remain unchanged.

For this graph, retain aggregate admission reservation3 with native CUDA1 and
XDMA2. Canonical reactivation must explicitly distinguish that aggregate reserve
from the frozen maximum2 XDMA slots; do not reinterpret the frozen device bound
as permission to create three native XDMA slots. Preserve the exact frozen logical
workload, phases/channels and8ms safe transition timeout. Only the new combined
sample explicitly selects the policy; old CPU/CUDA/XDMA samples keep their behavior.

Tentative production ownership: additive declaration/method in runtime.hpp,
private Runtime configuration/discovery/finalization/identity plumbing and
DeviceManager per-backend capacity planning/accounting. Native backend sources,
rate admission algorithm, C ABI, default configuration and earlier tests/evidence
remain protected. A canonical repair batch must settle the precise narrow path
list and policy contract before implementation.

## Required proof and integration

- Old-order negative control reproduces rejection; actual one/two-slot owned
  CUDA/XDMA combined graph then finalizes/executes without capability inflation.
- Real allocated slot and MemoryPlan deltas, global and per-backend saturation,
  malformed/foreign/frozen policy rejection and no partial ownership on failure.
- Multiple native worker lanes, completion capacities, failed initialize/stop/
  reset retention and bounded retry, safe acknowledgement, isolation and races.
- Originating-owner replay and compatible recovery; default checkpoint/active
  bytes unchanged; incompatible policy/capacity artifacts reject before effects.
- Positive-controlled zero steady/replay allocation including native workers;
  ASan/UBSan/leaks, TSan, production static, installed/relocated SDK, ABI/default
  golden, contract/quick/full and all32 exact-head hosted checks.
- Separate guarded repair merge, then canonical M26-04 reactivation and completion
  of every remaining feature acceptance. M26-05/M26-06 stay inactive. No physical,
  human, RT, Unreal, controlled-performance, release or deployment claim.

This is a new Runtime/C++ policy decision outside the approved ring-storage repair
and current M26-04 allowed paths. The repository assurance skill says to
"Stop and report rather than improvising" when "a forbidden path or protected
invariant must change"; the active batch independently requires a separately
bounded canonical amendment/repair for a Runtime/native/public API change.
Owner approval is required before implementing this new scope.
