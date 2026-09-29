# M26-04R4 risk review

Review updated after a concrete cleanup finding: native terminal completions
can remain after Runtime service polling quiesces, keeping registrations busy
through repeated checked stops. Canonical amendment571 bounds its repair. The private
change resolves the safe-phase reference through the existing compiled map,
checks both bounds, and forwards only the already validated optional simulation
policy. Absent policy passes nullptr exactly as before. Backend timeout and
logical identity remain original safe duration; checked arithmetic already
rejects overflow. Existing DeviceManager enforces independent finite host and
logical deadlines and terminal acknowledgement. No persistent field, public
layout, artifact identity, native implementation or MemoryPlan changes.

The important risks are policy leakage to native/default work, lost logical
deadline, false safety on cancellation, and missing ownership after timeout.
Independent public native-XDMA fixture covers selected/unselected/native timing,
logical expiry, host expiry, missing ACK, checked cleanup, all three lifecycle
paths and concurrent owners. All previous tests/sources remain intact. Default
MemoryPlan, checkpoint/active bytes and startup allocation count match baseline.
Steady work/acknowledged stop allocate0 with positive allocation1.

Initial test construction incorrectly enabled replay on native and used a CPU
callback error to infer failure safety; corrected to replay-disabled native and
a real device timeout. ASan exposed intermittent destruction with cleanup pending; GDB retains the
Runtime destructor fail-closed stack. New fixture timeout paths now complete
bounded checked-stop retries while retaining each original timeout assertion.
The independent finite host negative uses100ms expiry and200ms hold, preserving
safe8ms logical/backend timing. ASan/UBSan/leaks and TSan each pass11 cases;
steady/stop allocation0 with positive1 and unchanged startup1. Original logs
remain retained. No prior fixture or assertion was weakened.

Remaining validation: complete final-source focused/sanitizer/static/package/full
and all32 exact-head hosted gates, then clean/head/base/review/merge-tree guards.
Agent review is not an independent human assessment. No physical/RT claim.

## Bounded completion collection

DeviceManager stop now polls each initialized command backend once, after
submission/service lane quiescence and before unregister. Existing poll capacity
and completion storage bound work; no new loop wait, allocation or state.
Quarantined late completion cannot replace its timeout status or count as safe
ACK. When a native completion remains pending, unregister still fails and
retains ownership. New held-poll cases10/11 force that condition on selected and
default mock paths: baseline checked stops collect0 and abort at destruction;
repair performs exactly1 bounded poll per retry, retains unknown/live before
visibility, and releases all ownership after actual terminal collection.

Initial checked-retry fixture change alone did not resolve the underlying
Runtime issue; those aborts remain retained. Original native sources stay intact.
Fresh complete gates are required for both private production changes.
