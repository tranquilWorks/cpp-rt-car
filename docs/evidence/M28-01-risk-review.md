# M28-01 risk review

No unresolved finding in the bounded clock correction after reviewed focused gates.
Final full/hosted/package/source-tree guards remain mandatory before integration.

- Atomic snapshot: every concurrently accessed calibration field is atomic. All
  field and generation accesses are SC; equal even generations cannot bracket a
  different publication. A snapshot is attempted once and never waits for a writer.
- Writers serialize calibration and replacement off the read path. Callback
  reentry into a writer is unsupported and documented; callbacks remain borrowed.
  Holding calibration cannot block ordinary reads, proven by the held-reader test.
- Failure is generation-specific. A stale callback's result neither reads a mixed
  calibration nor disables a replacement, proven by held stale-sample ordering.
- init cannot lower the monotonic floor; invalid samples still receive the existing
  pre-publication finite/range/drift validation. Original causal controls are kept.
- Zero/allocation control and lock-free atomic assertions protect the read path.
  The existing monotonic CAS loop is retained, not newly claimed wait-free or RT.
- The correction remains experimental. Supported Runtime/default ABI/support tuples
  are unchanged. FMA-off owner tests preserve caller-thread arenas; no universal
  SimCore global-policy concurrency guarantee is inferred.
- Installed full-SimCore missing HAL header is a real pre-existing limitation;
  the failed attempt is retained and scoped installed clock plus supported Runtime
  verification cannot close that separate software packaging finding.
- Old source tests/assertions/deadlines and evidence are byte-preserved. The new
  standalone clock test does not change old installed consumer inventories.

Validation evidence and unperformed gates are in M28-01-2026-10-02.md and the
retained manifest. Only the exact final-head full/all32/package and guarded merge
receipts may establish final software integration. No independent human review or
external qualification is fabricated by this risk review.
