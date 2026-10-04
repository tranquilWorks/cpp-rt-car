# Retained software findings after M29-01

M29-02 keeps the [immutable M28 ledger](closeout/ledger.json) and adds a
[current disposition overlay](closeout/m29-current.json). It covers all 112
requirements, 30 original finding IDs and 13 existing acceptance/card identities.
Final integration receipts belong to [M29-02 evidence](evidence/M29-02-2026-10-04.md).
M29-03 remains planned. No global software-complete or CAP-M20 claim is made.

## Concrete repairs

On FMA-enabled builds `rt::set_use_fma`, `rt::use_fma` and `rt::fma` previously
accessed a shared plain bool. Concurrent setting/reading reproduces an initialized
TSan data race. The existing flag now uses lock-free atomic references for those
operations, with its required alignment and relaxed ordering. No arithmetic-data
publication is implied by a policy access. Defaults and the legacy raw-reference
signature are preserved; that reference requires exclusive access. Same-policy
concurrent users are exercised, with zero allocations after worker construction.
The policy remains process-global. Configure it while arithmetic is quiescent;
concurrent different policies do not provide per-owner numerical determinism.
The forced-FMA test uses portable `std::fma`, not a requirement for hardware FMA.

M17-06 repaired native command capability header initialization and tested actual
CUDA/XDMA candidate registration in one Runtime. The unconditional M17-05 refusal
in the proposal tool was stale. A fully rebound local parser fixture previously
failed solely at that refusal. It now passes complete validation and produces only
`proposal_only` / `human_matrix_change_required` output. Tampered plan/record and
artifacts, rejected review, missing external chronology verification and overwrite
still fail. Qualification schemas/matrices remain unchanged. The locally fabricated
`qualification_campaign` label is only a parser control, never physical evidence
or authenticated human attribution. The original invalid fixture still fails its
actual plan/record digest checks; only its obsolete M17-05-message assertion and
two substring validators are superseded by this explicitly selected scope.

## Scheduling observations and provenance limits

The original sampled fixture has an 8ms native argument, a 30ms host gate delay,
a two-second native event wait and, when selected, a separate simulator watchdog.
Timeout cause and cleanup ownership are distinct. A generated test-only copy parks
the original native event tail until `start()` returns. Both mock and native
unselected cases then fail their unchanged -18 assertion with -2, release the tail,
and finish original checked cleanup at0/live0. This demonstrates how a descheduled
worker can leave cleanup pending even when stop requests release. The production
contract correctly retains ownership; changing -2 to -18 would hide it. It does
not prove the exact schedule of an untraced historic run. Original files, assertions,
8ms deadlines and failed receipts remain unchanged and mandatory in their suites.

A second generated control delays only the fixture gate worker until startup
returns. The original native two-second wait expires despite frozen logical time;
case0 fails its unchanged success assertion with -18 after at least two seconds.
This is a causal platform scheduling path, not proof of the historic Windows2024ms
schedule. Actual hosted Windows executes these same controls before completion.

The original GPUStub overlap estimator subtracts coordinator wall time from the
sum of separately measured CPU/GPU durations. A generated copy retains its exact
`EXPECT_GT(overlap.count(), 0)` oracle and 50ms workloads, but delays the coordinator
until the fence signals and for another150ms before drain. The original oracle
fails even with a working monotonic clock. Parent verification requires that exact
GTest failure, rather than calling the original test a pass. RT0 hosted scheduling
cannot guarantee positive measured overlap; this is not a GPU or clock defect.
The old Windows -43476200ns event remains untraced. Neither clock repairs nor
successful retries establish its exact historical cause.

The historical benchmark failure did not retain its failed predicate. M28-02's
held-submit control establishes and repairs the unsound exact-one-acceptance
assumption; it cannot recover the missing historic predicate. This is a permanent
provenance limit, not a remaining implementable repair or a reconstructed cause.

## Instrumentation boundaries

The original native500us `--frame` probe remains a failed initialized TSan timing
result, distinct from passing ordinary/ASan frames and mandatory protocol tests.
M28-04 source-bound traces and the2ms delay control show logical expiry, quarantine,
late native completion and checked release, without output publication. Instrumented
execution on an uncontrolled host has no500us timing guarantee. The deadline is
unchanged; a later pass would not erase the failure or provide RT qualification.

The earlier partially instrumented Runtime race observation is excluded from
concurrency acceptance. Complete Runtime instrumentation and correctly bound
libraries are required. Pre-main mapping/signal failures never count as passes.
The M29 controls use matching complete Runtime instrumentation; all changed code
is freshly compiled in each sanitizer mode.

The old offline diagnostic macro-renamed `main`, losing C++ main's implicit return0
on its unused default branch and emitting `-Wreturn-type`. Its sole wrapper call
selects `--frame`, whose paths explicitly return; the diagnostic never entered a
shipped target. Preserve that warning and restricted call-path review. New controls
use explicit-return entry points or an original executable, and compile with strict
warnings; no warning-free claim is made for the old diagnostic artifact.

## Replay and application ownership

The existing owner-refusal repair and sample pre-effect guards remain. New fixed
truncation/header/checksum/body controls cover both ordinary/active nesting and
trusted v1/v2. They require invalid-artifact refusal before callbacks, registered
state changes, mailbox/capture changes or allocation, followed by valid recovery.
This finite set is not a proof over every malformed byte string; generated campaigns
are separately planned in M29-03.

After a valid replay has restored its checkpoint and invoked an input callback,
a callback can change application state and then fail. Tests verify that effect
remains, no frame is counted complete, a non-success mismatch is reported, and
explicit replay recovery restores the expected final state. Replay is not an
all-effects transaction. Runtime generation rollback does not undo arbitrary
application, backend or physical side effects. Applications must validate their
inputs before effects and own recovery; no new rollback guarantee is introduced.

The golden integer models demonstrate framework composition. Production vehicle or
plant physics, calibration, vendor safety integration and peer DMA are not delivered
models. Applications supply and validate those under their own requirements. This
existing product boundary does not hide a portable framework defect.

## Remaining work

M29-03 owns the reopened generated-input/automation and throwaway signature-fixture
software obligations and final exact-source package/soak/unsigned rebuild checks.
The existing final owner cards retain physical CUDA/XDMA/combined, RT/endurance,
licensed Unreal, controlled measurements, unfamiliar-consumer and platform review,
production identities/signing/release/deployment. No such owner action occurs here.
