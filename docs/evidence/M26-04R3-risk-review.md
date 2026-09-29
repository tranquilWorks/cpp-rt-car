# M26-04R3 risk review

No unresolved correctness finding in the reviewed candidate. All local gates pass. Exact-final-head hosted gates remain required before integration. This is a private
Runtime lifecycle and checkpoint repair, not feature or qualification closure.

`Runtime::stop` previously repeated sampled safety after native workers had
already stopped during partial teardown. The private stage flag advances only
after all actual stop-time safe operations acknowledge. Startup/ordinary ACKs
cannot set it. Missing ACK remains unknown and executes again on retry. Existing
stop_pending blocks step/reset/replay/checkpoint until checked cleanup succeeds.
The flag resets at successful start and fits existing private padding. New tests
exercise one and two actual native backends, partial shutdown, missing ACK,
startup failure, finalized/idempotent stop and concurrent independent owners.
The retained combined CUDA/XDMA probe also covers failed CUDA unregister retry.

`sampled_checkpoint_provenance` reconstructs only produced sampled device
channels. An existing validated header supplies generation, sequence, actual
device timestamp and domain; the compiled epoch-zero producer rate supplies
release/substep. Checked arithmetic ties this to the saved source logical
release. The complete validation pass rejects malformed data before state or
device effects. The apply pass repeats the same pure reconstruction. Initial
and ordinary untyped channels retain their prior behavior. No schema, artifact,
identity or public layout changes are needed. Tests include different Runtime
and device clocks, 3 substeps, prior-cycle reads, underflow bytes, intermediate
owner replay, fresh continuation and 12 reencoded malformed headers.

Baseline and repaired final public consumers use identical source: both old
failures reproduce, both repaired paths pass, and default MemoryPlan and exact
checkpoint/active bytes match. Ordinary steady/replay allocation control is
zero with a positive control. The fixture transports data through actual native
XDMA H2C, control, event and C2H operations; only its owned-card registration is
marked replay-capable. Native backend extensions and public sources are intact.

Local full110+114/package55/sanitizer11+11/allocation/static36 gates pass.
Remaining integration requirements: exact-final-head full rerun, all32 hosted
checks, then clean/head/base/review/fetched-tree guards.
Retain every failed fixture/build/startup attempt. No functional gate waiver.
