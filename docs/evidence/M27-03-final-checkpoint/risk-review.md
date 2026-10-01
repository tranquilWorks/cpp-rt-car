# M27-03 risk review

No unresolved change-introduced finding identified in the current source review.
Final acceptance remains conditional on every required local and hosted gate.

- Startup readiness publishes initialized per-thread arenas and captured dispatch
  generation before coordinator frame reset. Workers publish with acq_rel RMW;
  coordinator acquire observes the release sequence for all workers. Startup,
  zero/one-worker configurations and repeated owner teardown are covered.
- Chunk completion alone is not worker quiescence. Each dispatched worker now
  acknowledges exactly once after returning from processActiveRange, even if no
  chunk was available. Dispatch generation cannot advance and metadata/counters/
  temporary reduction storage cannot be reused until all acknowledgements arrive.
  The retained old-order schedule demonstrates unpublished chunk consumption and
  lost accounting, not merely an elapsed-time difference. Scope is the existing
  coordinator dispatch path, not a universal experimental scheduler qualification.
- Watchdog callbacks access only atomics. The local trip count is published last.
  rt::Watchdog counts a scheduled callback before invoking it; disarm acquires its
  mutex and clears armed before returning, so its count is stable until next arm.
  Waiting for matching local count establishes callback publication before pending
  events are drained. Final-frame logging/degradation is conserved. Destructor
  joins watchdog before atomics, trace and other dependent members are destroyed.
  Original thresholds and degradation ladder remain unchanged. Applying settings
  at a quiescent boundary prevents mid-phase mutation; immediate atomic limp/trip
  visibility remains available. No supported Runtime watchdog changes.
- Experimental watchdog stop uses its existing predicate mutex before notify,
  so the worker cannot miss the transition between checking and sleeping. The
  destructor releases that mutex before join. Callback lifetime still ends before
  dependent SimCore members are destroyed; supported Runtime is unchanged.
- Clock rejects backward, nonfinite and unrepresentable deltas before unsigned
  conversion/addition, then applies the existing1ms drift bound before publishing
  every sample. Conservative floating bounds reject the rounded limit itself.
  Atomic compare/exchange publishes one monotonic maximum without a stale store.
  No cross-thread initialization guarantee or physical timing claim is added.
- New range tests use independent exact closed-form integer-valued doubles, every
  element/frame sum/leaf hash, multiple chunk sizes and separate owners. Test-only
  initialization mutex/barriers honor global-clock and caller-thread arena limits;
  they are not product locks or production hooks. Failed earlier fixtures retained.
- Instrumented source copies for causal controls retain the original workload;
  baseline is fetched from its exact local Git object. Negative exit42/44 and
  watchdog assertion failures and shutdown diagnostic49 are distinct from a broken gate(exit43) or compile/
  timeout failure. Current positives must pass original assertions. Hosted CI
  separately runs unchanged original tests plus new uninstrumented regressions.
- M23 benchmark classification preserves original sources/assertions. A public
  submit-entry scheduling gate proves deadline expiry can precede backend submit
  accounting with a valid timeout terminal and no publication. Original historical
  provider_error lacks predicate detail, so its exact cause remains unresolved.
  A1000-pass current sample does not erase it or establish a Runtime repair.
- Scope/byte comparison confirms protected Runtime/native/benchmark/original test/
  ABI/workflow sources unchanged. No new public ABI/schema/default/support claim,
  no released package target addition and no new RT-lane allocation/mutex.

Residual limits: unmodified benchmark timing assumption; process-global
experimental clock initialization cannot be concurrent; other uncharacterized
experimental combinations are not promoted to supported Runtime. CPU software,
TSan/ASan and hosted OS/compiler runs do not establish hardware, HIL, RT,
controlled performance, Unreal, independent human, signing, release or deployment
acceptance. All prior failures remain retained. M27-04..08 stay inactive.
