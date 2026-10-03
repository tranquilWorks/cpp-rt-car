# M28-02 risk review

- Timeout is not proof of backend acceptance. The lower-bound terminal sample
  cannot decrease and is reconciled with exactly one submit attempt, returned
  status and stable backend counters only after successful checked shutdown.
- The failure observer is instance-local and lives longer than Runtime. It is
  selected only for single-invocation failure/timeout fixtures. Release publication
  of submit's result precedes acquired cancellation access to completion storage.
  Cancellation before acceptance fails promptly; storage remains owned until stop.
- Submit/cancel/poll/stop forwarding adds no callback mutex, allocation or retry.
  The original1ms service deadline, fault selection, terminal/output checks and
  one-command workload remain. Runtime/backend source and public semantics do not
  change. The original steady benchmark allocation checks still pass.
- Zero acceptance cannot conceal rejection or missing work: one callback attempt,
  invalid_state return after admission closure and all-zero device-work counters
  are required. Accepted work must have exactly one copy/action, a valid terminal
  disposition, no application publication and zero post-stop Runtime outstanding.
- Actual-provider negative controls reject erroneous accounting on both branches;
  baseline causal failure, genuine TSan findings and pre-main failures are retained.
- The broader loopback backend race remains open. This scoped provider-local
  correction does not certify direct backend concurrency or close all software
  findings. The SDK HAL-header and startup/native timing findings remain explicit.
- Old source tests, catalog, schemas, ABI/defaults, other benchmark sources and
  historical evidence are byte-preserved. New CMake wiring is append-only.
- Installed validation uses an actual CPack archive, removed original prefix,
  independent source copies and the unchanged installed CLI validator. Final
  committed-source bindings/full/all32/review/head/base/tree guards remain required.

No unperformed human, hardware, RT, controlled-performance or release qualification
is claimed. See the batch evidence and final PR/canonical closure for actual gates.
