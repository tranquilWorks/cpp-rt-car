# M27-08 scoped risk review

High-risk batch, reviewed against activated canonical605 and baseline target292.

- Scope/compatibility: no production/runtime/native/default/ABI/workflow change.
  Prior evidence and assertions are preserved. New CMake lines are strictly additive;
  release-contract hashes add only new coverage/doc sources and refresh changed files.
- Entry points: actual extracted CPack with deleted original prefix and independently
  copied consumer passes both targets, exact-output equivalence and eight discovery
  outcomes. NO_DEFAULT_PATH prevents silently selecting another SDK for version tests.
- Observation validity: executable and runner hashes, fixed integer row contracts,
  complete log hashes, request counts and measured monotonic time are retained.
  Exit0 alone does not pass; missing/extra rows and resource imbalance fail. Start-only
  journals remain incomplete. Existing output cannot be reused. Source/build bindings
  supplement the unsigned runner observation; this is not authenticated provenance.
- Lifetime: two isolated owners, checkpoint reexecution, compatible cross-owner
  restore, checked/idempotent stop and continued peer progression are asserted.
  All six reference-provider acquisitions/releases balance each cycle. This does not
  claim universal heap conservation or physical endurance. ASan/leaks and initialized
  TSan exercise the complete unchanged Runtime separately; all startup attempts remain.
- Cancellation: parent handles interruption/timeout, terminates then waits, with a
  bounded graceful allowance and forced cleanup fallback. Abrupt parent death may
  leave incomplete evidence. No automatic resume, child daemon or schedule exists.
- Baseline controls: no production defect is claimed. Fixed nonzero/missing/unbalanced
  journal fixtures and actual timeout/interruption prove refusal semantics. New tests
  exercise compatible behavior already present in baseline; no old test is weakened.
- Limits/open findings: native timing, experimental clock, benchmark assumptions,
  original05/06 exclusions, actual Unreal/human/hardware/RT/performance/signing/release
  obligations remain open. Existing32 exact-head records, full160/165, clean/head/base/
  no-unresolved-review and fetched tested merge-tree equality are integration blockers.

Disposition: implementation passes focused/package/instrumented/static/contract
review. Eligible for final full+hosted verification, then guarded scoped integration;
not global qualification or release acceptance. No high-risk finding is waived.
