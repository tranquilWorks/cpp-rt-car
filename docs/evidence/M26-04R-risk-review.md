# M26-04R agent risk review

No unresolved implementation finding after the bounded storage, identity and
ownership review. Integration remains gated on final local and all32 hosted
checks; the original Windows C4324 failure is retained and its scoped fixture
annotation requires fresh Windows verification.

Runtime::finalize resolves a bounded transactional slot map before compilation.
Ordinary entries remain2; only validated sampled descriptors may select4.
compile_cross_rate_data uses the same count for checked aggregate byte limits,
compiled descriptors and SnapshotStore::create. compile_sampled_io still checks
exact agreement with actual storage. The public change is comments only; there
is no new struct layout, artifact marker, execution branch or native capability.
The existing capacity limit bounds the preflight array and quadratic duplicate
scan; all work occurs at finalize. Allocation and lifecycle ownership remain on
the existing paths. Rejected configuration remains correctable before start.

Reviewed tests exercise real4-generation retention/saturation/retirement,
wrapped payloads and selections, mixed ordinary2/sampled2/4 exact memory deltas,
global cap rejection with correction, malformed/foreign/frozen descriptors,
missing/duplicate publication, failed safe ACK retaining borrowed resources and
successful retry, origin replay, fresh paired recovery and independent owners.
Old Runtime fails the unchanged four-slot consumer at finalize; repaired Runtime
passes. Both libraries produce byte-identical default2 checkpoint and active
replay artifacts. Protected prior sources, assertions and deadlines are unchanged.
ASan/UBSan/leaks and TSan52tests each, zero-allocation guards with positive
controls, production36TU static analysis and relocated full SDK/package checks
pass. TSan mapping failure and process-local ASLR workaround are retained.

Residual limits: these are portable storage/simulated protocol results, with no
physical timing or hardware safety claim. The native XDMA golden feature remains
separately incomplete in draft279 until canonical reactivation, implementation
and fresh validation. All32 final-head target checks and clean/head/base/review/
fetched-tree guards remain mandatory before merge; no functional gate waiver.
