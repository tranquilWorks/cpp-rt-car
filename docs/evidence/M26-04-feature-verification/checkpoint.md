# M26-04 implementation checkpoint

Feature branch reconciles verified repairs280/281/282; main baseline is
f5402b9a646a6d7adfbb54f5eca2170a69a6e292. Active canonical control568 revision
1a2c1ee989319ffcd6c2d1874f13e1ed0f82bccb/blob e45d19649894bbc4f4abb92645730b812afc986f.

Complete new public kit, independent scalar/state/artifact validator, source
provenance across all three kits, installed/embedded build/run entry point,
additive CMake/CI integration and guide are implemented. New C++ matrix covers
18 configurations, split1024-tick CPU references and all3 device variants,
ordinary/aligned allocation controls, steady/replay zero-allocation, underflow,
27 actual transport-fault variants, native capability rejection, partial acquire/
rollback and shutdown retry, two-slot saturation/not-ready/queued cancellation,
independent concurrent owners, retention gap and foreign/malformed artifacts.
Protected Runtime/native/CPU/CUDA/old tests and evidence remain unchanged.

Contract gate passed. All25 C++ matrix/contract tests and native/host/external
entry points pass individually; expanded sweep tests04 passes36/37. Native
process sweep intermittently fails startup safe acknowledgement at unchanged8ms
(three retained attempts). No timeout, gate, logical model or assertion is waived.
The initial24-tick-only control-action formula was corrected against other
horizons: host boundaries continue every tick while the immutable reference
boundaries occupy the first six ticks. Every-state scalar oracle and exact
operation/action/sampled counters validate retained runs and1..25 horizons.

CPack01 passes standalone9, existing55 SDK, embedding9 and negative controls,
but audit found its new full-SDK include condition incorrectly replaced Linux
with the inventory string. That defect is corrected; package verification now
requires64 tests including all55 prior consumers and all9 XDMA cases. The full
package must rerun. An attempted reuse after temporary-package deletion failed
as expected; its log is retained. Early compile/fixture expected-status/name
errors and a diagnostic provenance rejection from concurrent source editing are
retained; they do not count as successful final verification.

Remaining: stable complete feature run, fresh package64, ASan/UBSan/leaks/TSan,
static, contract/quick/full, complete agent risk review, exact final-head all32
hosted records, guarded target integration and control closure. This checkpoint
is not feature completion. M26-05/06 remain inactive; no physical/HIL/RT/human/
Unreal/controlled-performance/release claim. Standing scoped authority continues.
