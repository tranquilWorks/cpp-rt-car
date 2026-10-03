# Final portable artifact and owner handoff (M28-06)

This is a bounded portable verification delivery for release 1.2.1 at RT0.
It is not unqualified repository/software completion: `software_complete=false`
and `cap_m20_complete=false` remain in the [M28-05 ledger](ledger.json).
Its 112 requirements, 30 findings, 13 existing acceptance identities and seven
action groups remain the authoritative remaining-work inventory. No finding is
closed by a successful retry or by packaging unchanged code.

## Artifact and evidence index

| Item | Retained entry and interpretation |
| --- | --- |
| Exact source | M28-06 target PR and canonical closure bind clean tested commit/tree, activation revision/blob, guarded merged tree and changed-source hashes. |
| Default package | `rebuild/report.json`: two fresh builds, archive hashes/member inventories, copied relocated public consumers, input/tool hashes and command journals. |
| Optional package | `package/relocation-directory.txt` identifies actual default/benchmark-enabled CPack archives, removed original prefixes, custom include/data paths, installed audit and all 71 optional SDK consumers. Optional here excludes the known experimental full-SimCore header defect. |
| Modern/legacy migration | `migration/commands.json`, source bindings and installed discovery records bind both public targets to the new extracted default SDK, outside private checkout/build paths. |
| Fixed portable soak | `soak-current/report.json` and `soak-legacy/report.json`, each with start/terminal journal, stdout/stderr hashes, executable/runner hashes, measured wall duration and resource observations. Each requests 4096 unchanged lifecycle cycles. |
| Full local validation | Exact-head complete portable182/strict170 logs; retain failed attempts and their dispositions before any unchanged rerun. |
| Supported platforms | Original32 named hosted checks, including duplicate scaling-smoke records, supported GCC11/Clang14/MSVC-v143 tuples, sanitizer/static/no-allocation/ABI/default and actual installed-package gates. Job metadata/logs and source identity must agree. |
| Remaining findings | [Ledger](README.md), unchanged historical evidence and the M28-06 canonical acceptance/risk/closure records. |

Paths in this table are receipt labels within the canonical M28-06 evidence
closure, not a claim that a report already exists in every source checkout.
Final receipts are published after the last commit to avoid a self-referential
source hash. The canonical closure records archive locations, SHA-256 digests,
commands, results, PR/run/job identifiers and any failures. Missing, failed or
pending evidence is not PASS. Refer to [batch evidence](../evidence/M28-06-2026-10-03.md)
for the validation contract and receipt locator.

The existing `tests/golden_audit/verify_package.py --work-directory <new-dir>`
verifies both actual CPack variants and the installed audit/SDK. The existing
`tools/release_rebuild.py build --expected-commit <full-head> --work-directory
<new-dir> --cc gcc --cxx g++` performs the independent default builds. Use the
existing `tools/portable_soak/run.py --executable <installed-migration-binary>
--cycles 4096 --maximum-seconds 300 --output <new-dir>` for each public target.
Retain complete logs and terminal failures; do not relax the workload or limit.
These are ordinary offline public consumers and fixed repetitions. They do not
establish real-time deadlines, physical endurance, universal resource accounting,
authenticated provenance or a reproducible release across hosts.

## Work remaining before unqualified software closeout

1. Repair the broader loopback backend cancel/submit completion-storage race in
   a separately scoped batch, retaining the initialized TSan failure and causal
   controls. M28-02's provider-local observer fixes only those benchmark uses.
2. Repair the optional full-SimCore SDK's missing `hal.hpp` in a separately scoped
   batch. Passing supported Runtime and standalone clock consumers is insufficient.
3. Keep the sampled startup (-2 versus -18 at unchanged8ms), Windows2024ms startup,
   historical benchmark and negative Windows GPU overlap observations unresolved.
   M28-04 native-frame TSan still fails its original500us budget; the offline trace
   supports an instrumentation timing limitation, not a runtime repair or pass.
   Broader replay transactionality, experimental global FMA-policy concurrency and
   application-owned production models/calibration/safety remain bounded claims.

These are specific residual software/claim blockers, not missing bench results.
This batch verifies and hands off the current artifact; it does not authorize
repairs, waive findings or mark the whole repository complete.

## Ordered final owner stage

Each step reuses the original identities pinned by `existing_cards` in the ledger.
No new card, support tuple, approval, campaign or threshold is created here.

1. **Review artifact and scope.** Check final source/tree and receipt hashes,
   remaining software findings and [migration inventory](../migration.md).
   Choose a deployment workload and decide the effect of each open finding before
   dependent acceptance. Application models and safety integration remain owned by
   the application; the portable examples are not production vehicle validation.
2. **NVIDIA and XDMA, then combined.** Use M18-02-nvidia-tuple and
   M18-03-xdma-tuple before M18-04-combined-deployment, following
   [qualification](../qualification.md) and the backend procedures it links.
   Record exact device/driver/bitstream/topology, pre-run plan and thresholds,
   complete raw artifacts and independent review. M27-07 preparation is not a
   physical golden run. The existing combined proposal tool still refuses
   non-synthetic proposals under its original M17-05 prerequisite restriction;
   retain that gate for explicit resolution rather than bypassing it.
3. **RT and endurance.** M18-05-rt1-rt2-and-endurance uses the
   [readiness checklist](../real_time_readiness_checklist.md) on the complete named
   deployment with predeclared thresholds. Portable4096-cycle repeats are not this
   evidence and do not promote RT1/RT2 or a support matrix.
4. **Licensed Unreal integration.** Follow M19-02-unreal-jobs-allocator-clock,
   M19-03-world-lifecycle-multiple-instances-unload, then M19-04-complete-engine-sample.
   Reuse canonical M19-02 and exact-version decision D-009. Retain actual editor and
   packaged engine/toolchain evidence, allocator/jobs/clock behavior, multiple-world
   ownership and checked unload. Portable host adapters do not supply this evidence.
5. **Controlled performance.** CAP-M23/manual/1 and M20-01-controlled-performance-gates
   require owner-reviewed baseline/thresholds and controlled measurements using
   [benchmark analysis](../benchmark_analysis.md). Hosted structural passes do not
   approve timing budgets or performance regressions.
6. **Independent consumer review.** CAP-M25/manual/1 and M25-06 use the installed
   [SDK recipes](../sdk/recipes.md) with an unfamiliar consumer, retained findings
   and resolution. Automated relocation does not replace human usability review.
7. **Production provenance and release decision.** Reuse M20-02-fuzz-static-security-signing
   and M20-04-platform-migration-soak-and-release with [release policy](../release_policy.md).
   Original M27-05 generated-input/continuous/scanner and M27-06 signature-fixture
   expansion stay excluded/unperformed. Resolve their original acceptance explicitly;
   no exclusion is a pass. Key custody, authenticated target signing, independent
   soak review, release approval, tag/publication and deployment remain separate.

Bench, licensed engine, independent human, controlled performance, authenticated
production signing, release and deployment are NOT_RUN in this batch. Existing
mandatory CI gates remain unchanged. No repository-complete or CAP-M20 claim is
permitted while required software or final-stage acceptance remains open.

## Rollback

The pre-batch reference is target PR298 merge
`232df8fe2b56ffbdf174fc2bcf24a99995062d09`. Revert only the M28-06 documentation,
active-contract and digest/generated-document changes through the same gates.
Preserve prior repairs, all M28-05 ledger findings and historical evidence. There
is no runtime, ABI, default policy, test, package version or support-matrix change
to roll back. Retain closure/failure receipts even if this handoff is superseded.
