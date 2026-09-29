# M26-04R preliminary agent review

Status: local focused checkpoint; not ready to merge.

The opt-in is the existing sampled ring_capacity field. The default2 constant,
public layouts, native backends and steady-state Runtime execution are untouched.
A bounded transactional stack map feeds actual allocation/descriptor/cap checks;
MemoryPlan uses existing actual-store accounting. Existing sampled identity
already includes ring capacity. Default and two-slot checkpoint bytes match the
old library. Four-slot old-order failure and positive wrap/replay are retained.

Higher-risk acceptance remaining: complete local profiles, sanitizer/leak/TSan
and static analysis, relocated installed consumers and full exact-head hosted
gates. Final scope/compatibility/lifetime/ownership review must reconcile those
results before guarded merge. Preserve draft279 and every failed diagnostic;
repair completion must be followed by canonical M26-04 reactivation and full
feature implementation/verification, not a premature feature closure.
