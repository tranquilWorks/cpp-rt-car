# M27-05 source and compatibility review

Decision: ready for exact-head full and hosted verification; merge remains
conditional on every required gate. This is an agent source review, not an
independent human or qualification review.

- Only new fixed public-API tests, their CMake wiring, active contract and
  documentation/evidence change. All production/native/experimental sources,
  ABI/defaults, prior tests/workflows and earlier evidence remain unchanged.
- Registered state and callback counters are compared only after synchronous
  steps or checkpoint operations return. Two owners have disjoint storage and
  both end with checked stop. The ordinary destructor stop is an unwind fallback.
- Checkpoint and typed-payload refusals preserve application/output sentinels;
  active replay checks metadata from two actual frames without executing replay.
  These examples do not claim exhaustive parsing or concurrent API-call coverage.
- The full Runtime archive and test translation unit passed ASan/UBSan/leaks
  and initialized TSan without suppressions. An earlier partially instrumented
  TSan run reported a callback/checkpoint race; synchronization in that Runtime
  archive was invisible to TSan. Complete instrumentation did not reproduce it.
  The partial result is retained and is not used as acceptance evidence.
- Clang14 lacks the initial standard source-location diagnostic in this local
  toolchain. A portable file/line assertion helper replaced it. The final test
  passed in-tree, static analysis and both independently relocated SDKs.
- SDK packaging passed default/optional configurations and all71 preserved
  optional consumers. Final diagnostic-only test changes were rebuilt and run
  against both same relocated archives with fresh source hashes.
- Existing ABI/no-allocation/default-byte checks remain in full/hosted suites.
  No claim of new allocation instrumentation or product behavior repair is made;
  a failing pre-fix product control is inapplicable to regression-only additions.
- Resource failures during ASan archive creation are retained. Completed owned
  SDK builds were archived, every file hash/mode/link verified, then the original
  temporary tree removed. The same compile objects linked and passed afterward.
- Original campaign/continuous acceptance remains unperformed under control600.
  M27-06 is selected next;07/08 and external qualification stay separate.

Rollback: revert only M27-05 additions/wiring and activation documentation.
No unresolved production finding is silently closed by this review. Historical
startup timing, experimental initialization and qualification limitations remain.
