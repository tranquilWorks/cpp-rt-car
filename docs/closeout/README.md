# Acceptance and remaining-work reconciliation

[M28-05 ledger](../golden_audit/generated/closeout.html) covers all 112 canonical
requirements, all 100 original scenario-map entries through the original audit,
12 predecessor deliveries, 30 retained findings/obligations and seven original
remaining-action groups. [JSON source](ledger.json) is the editable inventory.
The installed entry is `share/rtfw/golden_audit/closeout.html` (also linked from
its original audit index). Every requirement keeps its original text, source,
test and evidence links plus current disposition, dependencies and next action.

Software-complete and CAP-M20-complete claims are false. Two demonstrated software
findings remain open: broader direct loopback cancel/submit storage ownership and
the optional full-SimCore SDK missing HAL header. Startup and historical benchmark/
Windows overlap observations, broader replay transactionality and experimental
policy limits remain explicit. Native full-frame TSan still fails its500us budget;
M28-04 classifies traced instrumentation timing without claiming a repair or pass.

The finding statuses distinguish demonstrated repairs, unresolved software,
unresolved observations, instrumentation limitations, unproven boundaries, claim
limits, owner exclusions and external acceptance. Dependencies include conservative
capability-level context, not a claim that each implemented subcriterion failed.
Original audit implementation labels remain distinct from final acceptance.

## Owner final-stage handoff

Reuse the pinned original identities in `existing_cards`, with procedures linked
from each obligation. This creates no new qualification cards or thresholds.

- M18-02 NVIDIA,03 XDMA,04 combined and05 RT/endurance: predeclare exact tuples,
  workload, policy and thresholds using the existing qualification plan/record/
  review process. M27-07 prepares the native host; it does not complete a physical
  golden run. The existing combined promotion-tool restriction remains explicit.
- M19-02 engine adapter,03 world/multiple-instance/unload and04 complete sample:
  use the existing approved M19-02 batch and roadmap identities with exact licensed
  engine/UBT/UAT/toolchain and actual editor plus packaged evidence.
- CAP-M23/manual/1 and M20-01: retain controlled baseline/threshold review before
  performance gates. Portable CI remains characterization only.
- CAP-M25/manual/1 and M25-06: an unfamiliar consumer follows the existing installed
  recipes and records findings; automated consumption does not replace that review.
- M20-02 and04: production identity/key custody, authenticated signing/provenance,
  supported-tuple/soak review, release publication and deployment decisions remain
  separate. Original M27-05 campaign/scanner and M27-06 signature-fixture expansion
  are excluded and unperformed; unsigned matching builds do not close them.

None of these actions is executed or approved by this ledger. The separately
selected [M28-06 final artifact and ordered handoff](final-handoff.md) repeats
portable verification; it cannot erase unresolved defects.

## Reproduce and validate

From the checkout run `python3 tools/generate_golden_audit.py --check`,
`python3 tools/check_golden_audit.py`, and
`python3 tests/closeout_audit/test_closeout.py`. The same catalog validates the
actual installed offline bundle using `verify_bundle(Path(bundle_directory))`.
It rejects omitted/duplicate IDs, missing evidence/anchors, broken dependencies,
removed owner/excluded obligations, invented completion and altered rendered
claims even when output hashes are recomputed. Existing audit tests are preserved.

This is an offline source/evidence reconciliation. It starts no device, endpoint,
new campaign, signing operation or workflow. Snapshot hashes bind bundled bytes;
they do not authenticate an external author, reviewer or production identity.
