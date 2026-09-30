# M26-04 sampled-I/O lifecycle and recovery repair prerequisite

M26-04R2 target281 is merged at `242793be0cb1ff75b56c74e5b89d0f6e8a21fd76`.
Control566 reactivated the feature at `069139f9c2504349d71f280f6dbd16dbc1c92089`,
canonical blob `20d0d8b47161d3be5deef554e65ad38187e5bfca`. Local control validation
passed. Hosted run36627259575/job109607328702 failed with no steps; recorded
UNPERFORMED only under the standing control-only exception. Feature279 was
reconciled in `9788686`; no target or local functional gate is waived.

## Verified feature progress

The new combined Session explicitly selects native_per_backend; XDMA-only keeps
uniform defaults. Actual four-slot sampled storage, CUDA1/XDMA2 and global3 remain.
A sample-driver encoding defect was fixed: actual zero safe payloads now update
the returned output checksum and correlated timestamp before C2H completion.
All six CPU-physics/XDMA, CUDA-kernel/XDMA and CUDA-Graph/XDMA native/host modes
passed 9-entity/24-tick full canonical parity, originating-owner replay and zero
ordinary allocation with positive control1. R2's earlier independently retained
256-entity/1024-tick probe remains prerequisite evidence, not final feature gates.

New underflow control3 skips real CPU command publication at ticks6/9. Runtime
substitutes zero effort, actual event ACKs increment, tick12 clears the fault on
the same owner, and exact replay passes. Standalone overrun control4 triggers
real duplicate sampled publication in sensor phase3 tick6: first accepted,
second invalid_state, overrun1, callback_failed, no committed sensor overwrite.
Stop-failure control9 causes native actuator event timeout at tick6; checked stop
returns device_timeout with unknown safety and retains the complete owner and
three borrowed regions. Clearing the actual driver fault allows checked retry.
A new CLI and closed-form underflow oracle are in progress; they are not yet a
complete installed/validated source kit.

## Reproduced blockers in protected Runtime

1. A one-shot native XDMA driver shutdown failure returns device_error(-19)
   with ownership retained. The next two checked stops return invalid_state(-1):
   stop repeats sampled safe submission even though backend teardown already
   stopped workers. No device output work may resume after partial teardown.
   The diagnostic process explicitly exits without destructing its retained
   owner after demonstrating the bounded retries; it makes no leak-pass claim.
2. A compatible fresh owner accepts the pre-tick6 checkpoint, but input phase0
   cannot read actuator channel26004: CrossRateReadStatus::not_ready(4), no
   decoded frame and no generation. apply_active_checkpoint_state zeroes
   producer timestamp-domain metadata; copy_active_channel refuses produced
   device data with domain0. Originating-owner replay from the initial frame
   does not expose this intermediate-checkpoint defect.

Both failures are retained under M26-04-resumed-diagnostics, with source bindings,
commands/build errors, telemetry probes and successful nominal/underflow artifacts.
The earlier prototype and every repair test/source/evidence remain unchanged.
CLI overrun/stop recovery currently fails at the subsequent fresh-owner tick6.
Combined CUDA loss/reset cleanup retries also expose the first stop defect.

## Bounded next repair: M26-04R3

Complete this mechanical prerequisite separately under the owner's repair-and-
finish authorization. Canonically scope production changes to host_runtime.cpp:
retain a successful stop-time sampled safety acknowledgement across partial
teardown retries; retry a failed acknowledgement; clear private bookkeeping at
valid lifecycle boundaries. Do not submit new batches to stopped workers.
Restore produced sampled-device provenance from the existing validated frame,
compiled endpoint and saved logical release. Reject malformed/inconsistent
metadata before effects. Do not rewrite artifacts, synthesize device completion,
change schemas, add a public policy, or infer provenance for untyped ordinary
payloads. If existing bytes cannot support exact reconstruction, stop for a
separate explicit format/design decision rather than guessing.

Add independent new public-consumer/regression/allocation/lifecycle/recovery tests,
including old-order negative controls, native partial teardown, real missing ACK,
multi-backend retry, same/fresh owner continuation, underflow/held data, malformed
rejection and isolation. Preserve exact default MemoryPlan/checkpoint/active bytes,
ABI8/70 exports/fingerprint/SONAME8/deviceABI1 and both previous repairs. Require
local contract/quick/full, sanitizers/leaks/TSan, static, relocated full SDK,
exact32 hosted gates and guarded fetched-tree merge. Then canonically reactivate
and finish feature279 with all original acceptance and fresh feature gates.

M26-04 remains incomplete. Fault/process/matrix/foreign/malformed/native/retention
coverage, full SDK/provenance/independent artifact validator, package/install/docs,
full/sanitizer/static and exact32 feature gates remain. M26-05/M26-06 are inactive;
physical/human/HIL/RT/Unreal/performance/release claims remain unperformed.
