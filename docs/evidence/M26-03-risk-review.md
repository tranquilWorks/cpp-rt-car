# M26-03 agent risk review

Reviewed the complete diff against repaired main
c78075e2b30efba48db4f0ca8136110b1399baf5 and canonical control555.
This is an agent code/evidence review, not independent human acceptance.

Resolved findings:

- **High: foreign trusted replay can diverge after execution.** Calling the
  broad Runtime API directly did not establish this kit's originating-owner
  restriction. `OwnedReplay::apply` now inspects the public envelope Runtime ID
  before entry. Malformed/foreign/cross-variant tests assert unchanged canonical
  state and no submissions. Fresh-owner paired checkpoints remain a distinct
  supported operation. No Runtime semantics changed.
- **High: borrowed resources can outlive failed reset or stop.** `Owner::close`
  retains Session/Physics on failed checked teardown; driver, staging, context,
  buffers and jobs outlive Runtime. Actual query/context-loss/synchronization/
  unregister/event-destroy failures prove no failed plant publication, retained
  resources, bounded retry and accepted-resource conservation. Fresh recovery
  destroys the old owner before constructing the replacement.
- **Medium: native executable loader failure precedes NOT_RUN.** The new
  driver-independent launcher handles help and absent driver libraries before
  executing the actual linked native host. Missing library and toolkit-stub
  paths return3; failed host execution stays failure. Only this optional sample
  launcher links the platform loader; Runtime/exported SDK targets stay unchanged.
- **Medium: new test stack and aggregate bounds.** The new3MiB test arena now
  resides on the heap; the entire matrix passes with512KiB process stack.
  Named configuration/owner fixtures preserve every original state/replay/count/
  allocation assertion, including complete1024-frame originating-owner replay.
  CPU reference bytes are unmodified and independently decoded; CTest fixtures
  prevent device comparisons when the CPU producer fails. Every case keeps120s.
  The fully instrumented new fixture uses-O1, with Debug/O0 Runtime/backends;
  prior tests, CLI flags, sanitizer diagnostics and timeouts remain unchanged.
  Native, host and external process groups preserve all original cases and40
  artifact negatives, each bounded120s. Aggregate timeout attempts remain failures.
- **Medium: compiler and platform portability.** GCC11 inlined the prior
  allocation replacement implementation into this new large fixture and reported
  mismatched-new-delete. The unchanged implementation now lives in a separate
  new translation unit; positive ordinary/aligned allocation controls prove
  interception, and step/replay checks retain zero-allocation assertions. No
  compiler warning is suppressed. Windows correctly reports its existing
  unsupported_best_effort executor policy fallback; the new fixture now checks
  that exact report instead of assuming Linux park support. Hosted compiler
  and Windows gates remain mandatory.
- **Medium: stale or false evidence.** Complete installed source inventories and
  hashes bind both sibling kits. Independent Python checks every state field,
  tails, channel/control metadata, nested artifact pairing, exact actual backend
  activity, fault/reset/stop status and resource conservation. Source changes
  correctly invalidate stale provenance. False physical claims and backend/
  cleanup/count/source/state mutations reject. Artifact hashes are corruption
  and provenance checks, not authenticated physical execution attestations.

No unresolved software finding identified in the reviewed implementation.
Final exact-head full/hosted verification and guarded integration remain required
and are recorded in PR277; this review does not waive those gates.

Preserved boundaries: frozen scenario/CPU model/oracles, all prior kits/tests and
repair evidence, Runtime/native backends, C ABI v8/70exports/SONAME8/deviceABIv1,
license, default public SDK inventories and protected CI workflow. The optional
Session policy argument defaults to the original policy and copies only during
configuration. The new CUDA sample requests parked workers where supported; unchanged Windows
policy reports unsupported_best_effort. This makes no RT claim. Simulator
capability/timing/replay cannot be selected through native resource construction.

Residual limits: actual CUDA/XDMA/HIL and physical timing remain NOT RUN; RT1/RT2,
controlled performance, independent unfamiliar-consumer/human review, Unreal,
signing/release and deployment remain separate. Native replay is unsupported.
MemoryPlan/sample-owned bounds do not establish whole-process RSS or OS bounds.
