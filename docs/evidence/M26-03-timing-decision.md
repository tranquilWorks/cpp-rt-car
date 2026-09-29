# M26-03 timing decision — proposed, not approved

The M26-03 prototype cannot yet meet its portable completion/replay acceptance.
Repeated ordinary runs return `device_timeout` although the application's logical
clock remains at the current release. Parking executor workers also reproduces
the failure. The successful runs do not cancel this finding.

## Cause and protected requirements

The frozen M26-01 scenario has a 10 ms logical tick, a 3 ms plant envelope and
explicitly no wall-clock correctness oracle. M26-02 divides the plant envelope
over its three phases. M26-03 consequently binds CUDA physics with a 1 ms
completion budget. `Runtime::Impl::invoke_phase` clamps the materialized batch
timeout to this budget, including active replay. `DeviceManager` then converts
that duration to a `steady_clock` deadline across its submission, backend and
service lanes. An ordinary host scheduling delay can exceed it.

This does not establish a defect in existing physical deadline enforcement.
It establishes that the current public device-rate path couples the two budgets
and does not meet this sample's intended portable logical-time contract.
Privileged real-time scheduling is not an acceptable prerequisite for portable CI.

## Recommended decision

Authorize a separately bounded **M26-03R simulator timing policy** before resuming
this feature. The repair must distinguish a simulator's logical completion budget
from a finite host-liveness watchdog. It must be an explicit opt-in, restricted to
backends declaring deterministic mock semantics; native CUDA must reject it.

The proposed semantics are:

- Preserve the frozen 10 ms logical tick, 1 ms physics budget, all rate envelopes,
  channels, state/control oracles, numerical operations and workload identity.
- Preserve existing device-rate wall-time semantics by default, including all
  native CUDA, XDMA, ABI-v1 compatibility and previously configured callers.
- For the opt-in simulator only, retain logical deadline checks and the backend
  command timeout in the simulator clock domain. Independently bound host lane
  liveness by a copied, finite watchdog policy; the sample would request 5 seconds,
  matching its existing lifecycle bound. A frozen simulator clock must never
  turn a hung driver into an unbounded wait or free uncertain ownership.
- Copy and validate the policy at configuration time, expose its resolved value,
  and bind it to configuration/replay identity. Reject zero, excessive or
  overflowing explicit bounds, unsupported backends and cross-policy artifacts.
  Do not silently reinterpret an existing field or reserved slot.
- Keep C ABI v8, SONAME 8, device ABI v1, default configuration/artifact bytes and
  native `deterministic_mock=0`. Any necessary public C++ addition is additive
  and explicitly reviewed in the separate canonical plan.

The exact API and replay-schema compatibility must be settled in that plan before
implementation. Likely affected components are the public device-rate policy,
rate compiler/identity, host dispatch, private DeviceManager, focused rate/replay
tests and the corresponding package and architecture contracts. These paths are
currently forbidden in M26-03; this document does not authorize their modification.

## Required repair evidence

Retain this old-path failure as a negative control. Add deterministic tests with
controlled driver gates: host delay without logical advance succeeds only under
the explicit simulator policy; logical budget expiry fails; a stalled driver with
a frozen clock reaches the finite host watchdog; no failed output is published;
cancel/quarantine/stop/reset preserve ownership until actual retirement. Test
native-policy rejection, default-path timeout behavior, replay identity, independent
owners, no allocation and resolved accounting. Run full local/sanitizer/package/ABI
checks and all 32 exact-head hosted checks before repair integration. Then
canonically reactivate M26-03 and complete every still-open acceptance item.

Keeping the present policy is also valid, but leaves portable M26-03 acceptance
blocked. Increasing the sample's frozen budget, masking the failing status,
retrying until green, weakening a gate, or claiming hardware timing is not a fix.
