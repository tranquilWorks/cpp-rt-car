# M26-03 CUDA golden physics

The [CUDA source kit](golden_cuda.md) implements CPU fallback and actual Runtime
CUDA kernel/Graph command batches through an explicitly owned injected Driver API.
It preserves the frozen golden model, rates, controls, channels and independent
state oracles. Native/host execution, actual CIL processes, originating-owner
trusted replay and fresh-owner checkpoint recovery after device faults are covered.
The optional native host compiles its actual integer PTX kernel, disables replay,
and reports missing driver/device as NOT_RUN/exit3 through a driver-independent
launcher. Physical CUDA remains unqualified and NOT RUN.

The simulator timing prerequisite [PR278](https://github.com/tranquilWorks/cpp-rt-car/pull/278)
merged at c78075e2b30efba48db4f0ca8136110b1399baf5. Canonical
[control555](https://github.com/tranquilWorks/portfolio-control/pull/555) is
7ce76ddfd4d3061b3b55bfffc88f6e8ca11e9548, blob ef229993dec359b6e35662100475473bb2760350.
Only the sample-owned deterministic adapter selects the five-second host watchdog;
the logical and command completion budgets stay one millisecond. Native capability
and replay semantics, Runtime, stable ABIs and protected prior tests remain intact.

[Feature PR277](https://github.com/tranquilWorks/cpp-rt-car/pull/277) records the
final exact-head full/hosted checks, reviewed merge guards and integration identity.
See [acceptance/evidence](evidence/M26-03-2026-09-29.md) and
[risk review](evidence/M26-03-risk-review.md). Local numerical, fault, ownership,
replay, allocation, process, sanitizer, static and relocated SDK checks pass;
the PR remains the authoritative record of the final integration gates. All
original failures, source corrections and intermediate runs are retained.

M26-04 XDMA, M26-05 showcase and M26-06 audit remain separate and inactive.
Independent human review, physical CUDA/XDMA/HIL, RT1/RT2, controlled performance,
Unreal, signing/release and deployment remain separate. This is portable protocol
software evidence and optional native-host compile evidence, not CAP-M26 closure.

## Historical M26-02 checkpoint

# M26-02 portable golden reference

M26-01 merged as PR275 at11fce253dc601824eeadbce519b8135bc778815c.
Canonical M26-02 plan PR546 and replay-ownership amendment PR548 merged;
active revision ab889ba70d768b73eb9c209d6aaa57f8febe92b0, blob
98cdb422eefd68a9560626c2506277eeb1c2ed74. The portable CPU source kit implements
native and independent host jobs, actual vector CIL processes, active rates,
controls, paired-checkpoint recovery, trusted replay and independent artifacts.
See the [guide](golden_system.md) and [evidence](evidence/M26-02-2026-09-29.md).
Local/hosted validation and exact integration status are retained there and in
the implementation PR and canonical control closure. Do not infer integration
from the existence of implementation files.

Four M26 batches remain: M26-03 CUDA, M26-04 XDMA, M26-05 benchmark/fault
showcase and M26-06 living documentation/audit. They remain inactive. M25
independent human acceptance is UNPERFORMED; physical/RT, controlled performance,
Unreal, signing/release/deployment remain separate.

## Historical M26-01 checkpoint

# M26-01 golden scenario specification

M25 software merged through target PR274 at62e61474c2d2efa6d673a9db31c45e2f35974ada.
Canonical plan PR544 merged at3850342958f8daa9607c9ba2e5f82e49efbe40d6, blobacd6b1b1589de95a638f91fe41866125243c4d40.
M26-01 freezes the [golden scenario/lever contract](golden_system.md), with offline
shape, numerical, graph/rate/channel/control/fault/identity checks and mutation
negatives. This is design validation, not execution of the future scenario.
Final local/hosted/integration results are retained in its PR and control closure.

M26-02 portable implementation, M26-03 CUDA, M26-04 XDMA, M26-05 benchmarks/faults
and M26-06 living docs/audit remain separate and unactivated. M25 independent
human acceptance remains UNPERFORMED; physical/RT, controlled performance,
Unreal and signing/release/deployment remain separately gated.

## Historical M25-06 checkpoint

# M25-06 installed documentation and recipes

M25-05 merged as PR273 at98ecca5a6572dd83d59e26063716f6d76b7a5d4f.
Canonical control PR541 merged atd385731eb44762e73596e830d4f99a8534bc5bf5,
blobac12e901ec4c12d945d83beda7535d5e84defc22. The active scope adds an offline manual,
reproducible source-linked API reference and executable public-package recipes
in actual CPack archives. See the [manual](sdk/generated/README.md) and
[M25-06 evidence](evidence/M25-06-2026-09-28.md); final local/hosted and integration
results are retained in the implementation PR and canonical closure.

After this batch all six M25 software batches are delivered. Independent
unfamiliar-consumer acceptance remains UNPERFORMED. Six M26 full-system batches,
physical CUDA/XDMA/HIL, RT1/RT2, controlled performance, Unreal and signing/
release/deployment remain separate. Do not activate M26 in this batch.

## Historical M25-05 checkpoint

# M25-05 independent host-adapter reference

M25-04 merged as PR272 at2c431a40f450aba06b721dd0929f467fd12652b4.
Canonical control PR535 merged at2808734c34b7189aef41630ab9fcf6f0b79d958a,
blob957f0e507493768ef17ab26d90bf48b5f13cd9ea. The active source-only kit
provides fixed host jobs and memory regions, independent residency observation,
a monotonic clock/frame loop, multiple Runtime instances, telemetry and checked
shutdown using public SDK artifacts. Native/adapter execution and lifecycle
verification are tracked in [M25-05 evidence](evidence/M25-05-2026-09-28.md).

After this batch, M25-06 installed API/docs/recipes is the one remaining M25
software batch. M26 has six full-system batches. Independent novice review,
hardware/RT, controlled performance, Unreal and signing/release/deployment remain
separate. Do not activate M25-06 here.

## Historical M25-04 checkpoint

# M25-04 external shared-memory CIL reference

M25-03 merged as PR271 at 315b04b25dd41acbd3a21313a093cf90b19c95ac.
Canonical control PR530 merged at 6927945598352edb25ec27759f6fd633a3b6f4de,
blob e0e7d67c81ca110f18762425402899273311403a. The active batch adds the
optional source-only external controller and Runtime plant, versioned bounded
channels, explicit failure/ownership/reconnect policy and relocated consumers.
Verification/integration are tracked in [M25-04 evidence](evidence/M25-04-2026-09-28.md).

After this batch, two M25 software batches remain: independent host adapter and
installed API/docs/recipes. M26 has six full-system batches. Independent novice
review, hardware/RT, controlled performance, Unreal and signing/release/deployment
remain separate. Do not activate M25-05 here.

## Historical M25-03 checkpoint

# M25-03 backend authoring kit

M25-02 merged as PR270 at e562ab164162b67d009171b5c8f53e9b04866285.
Canonical control PR527 merged at 7585342836adf7d5e90cd62acb8d4237dc802f22,
blob d65ec3071611b480748f3a92eca937160a5ea8d7. The active batch adds the
source-only backend, reusable copy-profile conformance, Runtime integration,
installed consumer and ownership guide. Verification/integration are in progress;
see [M25-03 evidence](evidence/M25-03-2026-09-28.md).

After this batch, three M25 software batches remain: external shared-memory CIL,
independent host adapter, and installed API/docs/recipes. M26 has six full-system
batches. Independent novice review, hardware/RT, controlled performance, Unreal
and signing/release/deployment remain separate. Do not activate M25-04 here.

## Historical M25-02 checkpoint

# M25-02 optional typed Runtime SDK

M25-01 merged as PR269 at d3052a0f7f17a0cd68334123aea3869fcae2094a.
Canonical control PR524 at ad159304c00151e248ffe2b7f5699c341a8b5361 activates
M25-02, blob d598ea817902036ebdf85cb1407e6cb78043ca08. Optional public
`rt/sdk.hpp` adds noexcept typed callbacks, raw-validated config/graph builders,
checked partial storage arithmetic, retryable checked-stop guards and fixed
caller-buffer diagnostics. The typed source kit and guide ship with the SDK.
Compiled Runtime/backend sources, stable ABIs and raw hello are unchanged.

Local verification passed portable49/strict53, SDK22, source1+1, seven negative
controls, ASan/UBSan3, TSan3, CUDA11+11, static, ABI and contract checks.
Exact-head hosted verification and integration are tracked in
[evidence](evidence/M25-02-2026-09-28.md). No later batch is activated.
After this batch, four M25 software batches remain: backend authoring/conformance,
external shared-memory CIL, independent host adapter and installed API/docs/recipes.
M26 has six full-system batches. Independent novice review, hardware/RT,
controlled performance, Unreal and signing/release/deployment remain separate.

## Historical M25-01 checkpoint

# M25-01 consumer installation and hello Runtime

M24 portable software merged as PR268 at facc42d4a09d1dab398d75dc92000a05fb08c396.
Canonical control PR520 at d1512a06be447fb275c625ca51b7d67a854b4bed activates
M25-01, blob462f1f1ab2915ff833905e45be317cb5a484c438. The 57-line public graph,
installed source kit, consumer-first README/shipped guide and legacy notices
are implemented. Local quick46/full50, SDK21, CUDA source11+11, ASan/UBSan3
and TSan3, exact README transcripts, static and ABI checks pass. Exact-head
hosted verification and integration are tracked in
[evidence](evidence/M25-01-2026-09-28.md); integration is pending.

The next software batch is M25-02 typed helpers/RAII/diagnostics, followed by
backend authoring, external CIL, independent host integration and full installed
API/recipes. M26 has six more software batches. No later batch is activated.
Independent novice review, physical/RT qualification, controlled performance,
Unreal and signing/release/deployment remain separate. Preserve every Runtime,
ABI, M23/M24 source, repair and evidence binding.

## Historical M24 completion checkpoint

# M24-04 benchmark and maturity closure

M24-03 merged as #265 at `74e59e8850ccb24d71a0fdbedfe4305971e773df` with all32
checks. Canonical M24-04 control #516 is integrated at
`ff8e989a5c9da0204082d9e03394d06ac900496d`, blob
`ee197e588a00c81d8bd6afab4085af9ec16b84e9`. The optional M23 particle provider
adds20 finite cases, fixed host timeline correlations and installed consumers.
The capability gate retains40 software and4 physical rows, with evidence/source
bindings and unchanged M23 workload crosswalk. Local verification is complete: quick43/full47, strict portable59,
ASan/UBSan11 and TSan11 plus final boundary checks, source consumers11+11 and
default SDK20. Coverage is40/44 (90.91%) and37/37 critical. The scoped target PR
records exact final hosted checks and integration; see
[evidence](evidence/M24-04-2026-09-27.md). Physical rows remain M18 NOT RUN.

M25 consumer/SDK work (six batches), M26 full-system reference (six batches),
physical CUDA/RT, controlled performance, independent review and signing/release
remain separate. Standing scoped publication/integration authorization persists.
Earlier checkpoints below are historical; do not reopen merged repairs.

## Historical M24-03 checkpoint

# M24-03 final conformance verification

M24-03 is active under merged control #514
`360bbc308989efe30bb2a6f51a455e7cf99aeeef`, blob
`c0e71268a7b8725342400fb7ae7b89cf2ffcdca8`, on Runtime repair #267
`27fcb90b49773196f8773c2edcf97ba746fa7f94`. Both prerequisite repairs
(#266 CUDA cleanup and #267 private ownership race) passed all 32 hosted checks
and are integrated unchanged. The installed four-mode failure/lifetime suite
is implemented and locally verified: quick42/42, experimental46/46, strict
portable42/42, ASan/UBSan/leak9/9, TSan9/9, source6+6, original SDK20/20,
compiled ABI, static analysis and actual root/installed CUDA compile/help/stub
checks pass. Final exact-head hosted verification and integration are recorded
in [feature PR #265](https://github.com/tranquilWorks/cpp-rt-car/pull/265).

See [usage](cuda_lifetime.md) and [evidence](evidence/M24-03-2026-09-27.md).
M24-04 owns benchmark/profiler integration and the 90% overall / 100% critical
capability coverage gate. M25 SDK/usability, M26 full-system reference, physical
CUDA, controlled performance, independent human review and RT qualification
remain separate. Earlier failure checkpoints below are historical.

## Historical pre-R2 feature checkpoint

# M24-03 conformance verification

M24-03 is active under canonical control #512, revision
`f168122ed236002fe54af2cea0515d24cb9c81c1`, blob
`531af791c31aa5ccb3088bbc2fba095b8d767802`, on CUDA repair #266
`ff47a64883e73cd9c2984616249b302e413566dc`. The finite installed conformance
suite exercises four schedule/launch modes, faults, cancellation, timeout,
quarantine/reset, context loss, retryable cleanup and resource conservation.
The repair and all original M24-01/02 tests remain unchanged.

Focused3/3, ASan/leak9/9, portable42/42 and source consumers6+6 pass.
TSan exposed a Runtime rate_owned retirement race in both lifetime executables;
the512KiB CLI also exits-11 without a diagnostic.
Preserve feature #265 while separately scoped M24-03R2 is planned and verified. See
[evidence](evidence/M24-03-2026-09-27.md). Integration is pending.
M24-04 owns benchmark/profiler integration and the 90% overall / 100% critical
capability gate. Physical CUDA, controlled performance, independent review,
RT qualification, M25 SDK/usability and M26 golden-reference work remain.
Earlier blocked checkpoints below are historical.

## Historical feature checkpoint before repair

# M24-03 continuation checkpoint

M24-02 merged as #264 at `5c82703d0c2840d13e18770d2d1b3083a96e29c0` with
all 32 exact-head checks passing. M24-03 is active under control #507;
its conformance draft exposed a production pending-CUDA cleanup ownership cycle.
See [retained evidence](evidence/M24-03-2026-09-27.md). Preserve the draft and
integrate a separately scoped repair before finishing M24-03. No passing full or
hosted result is claimed for this draft. M24-04 and physical/manual gates remain.

## Historical M24-02 checkpoint

# Current state

M24-02 is active under merged control #504 (`36b1b8e1976c6063b1ea20c1cda3afec762e735b`)
on merged M24-01 PR #263 (`c64d0672f1478acd863c68865663550d7fe22f8c`).
The bounded two-stream source pipeline adds caller-owned device buffers, D2D,
kernel/Graph parity and actual active-rate dispatch. Local verification passes: quick 39/39, full 43/43, ASan/UBSan and TSan
3/3 each, relocated/embedded source consumers and existing SDK 20/20. Hosted
verification and integration remain pending; see [pipeline usage](cuda_pipeline.md) and
[batch evidence](evidence/M24-02-2026-09-27.md). The original reference, Runtime,
backends, ABI and benchmarks remain protected. Physical CUDA and M24-03/04 maturity
closure remain separate. Earlier M24-01 pending text below is historical; it merged
after 32 passing hosted check records, with its local timeout retained separately.

## Historical M24-01 checkpoint

M24-01 is implemented under merged control #502
(`845e6180b9bf84dafb813ae023e19d4eedacb105`) on target baseline
`7e9fb883049df486f04a1d6730d36890cadc3853`. The public Runtime CUDA particle
reference has an independent CPU oracle, actual injected-driver transfers,
bounded failure/cleanup, optional compiled real CUDA kernel/host and installed
source consumers. Portable quick 36/36, focused ASan/UBSan and TSan, package
and ABI checks pass. The experimental suite finished 39/40 with the unchanged
300-second `simcore_all` timeout in its eight-thread determinism case; all new
M24 tests passed. The owner requested continuation around test timeouts.
[PR #263](https://github.com/tranquilWorks/cpp-rt-car/pull/263) retains the
checkpoint; hosted verification and integration remain pending.

See [model and usage](cuda_physics.md) and
[retained verification](evidence/M24-01-2026-09-27.md), including the separate
Debug/TSan 512 KiB embedded-thread limitation and the passing M23-style CLI
process-stack check. No physical CUDA run, controlled performance or CAP-M24
maturity closure is claimed. M24-02/03/04 remain separate. M23-05 is already
merged; earlier pending text below is historical.

## Historical M23 checkpoint

M23-05 is active on merged repair #262 at
`b39ac9f281862a5a0aff051880671218e117db47`, tree
`dbfc45df441bcad0696b34b7e92fc34827f60b34`, under canonical #500,
`76fb4a2261c4bdc401051172641f4148a11ec76d` (blob
`a6adf86ec1944c6d8126ded6d77aa8c402c231e2`). The repair passed all 32
hosted checks and its full local/sanitizer/relocated-SDK/ABI gates.

Feature PR #261 retains the complete offline analysis and third-party kit.
Synthetic manifests now avoid repeated production capture during test setup;
an explicit regression compares both manifests to actual capture. All original
trials, assertions, v1 validation and the 120-second CTest limit remain.
Fresh verification of this combined source is pending; final source/tree/CI
and merge outcomes are recorded in [PR #261](https://github.com/tranquilWorks/cpp-rt-car/pull/261)
and [evidence](evidence/M23-05-2026-09-25.md). Earlier Windows analysis,
completed-batch cancellation and failed repair attempts remain retained.

Physical characterization, controlled timing, threshold approval and profiler
capture remain NOT RUN. M24-M26 remain separate.

## Historical feature checkpoint before repair integration

M23-05 software implementation is complete in PR #261 under canonical control PR #494, merged at
`b61ab1396168647ab8309eb7015b25edfae9ecf1` (blob
`a043c31df4244e073a12229c9dcb907a22ba7d12`). The baseline is merged
M23-04 PR #260 at `37c3b155894133789621d41b4233c8edde79d023`, tree
`20435c387117ae06a971d974e7f7da03105c2447`; all 32 exact-head checks passed.

The new offline comparison and third-party source kit preserve every prior
provider, runner, v1 bundle/validator, Runtime, driver and ABI. Focused and normal software verification, ASan/UBSan and ABI checks pass.
Final full-profile/package, exact-head hosted checks and merge disposition are
retained in [PR #261](https://github.com/tranquilWorks/cpp-rt-car/pull/261). See [benchmark analysis](benchmark_analysis.md) and
[evidence](evidence/M23-05-2026-09-25.md). Actual controlled-host timing,
threshold approval, physical characterization and profiler capture are NOT RUN.
M24-M26 remain separate software scopes. Historical checkpoints below do not
reopen completed batches.

## Historical M23-04 implementation checkpoint

M23-04 software implementation is complete; final integration is tracked in PR #260 under merged
control #488 (`49b134215e74b2d6f27ae1cab3801ac9dc8c139d`, canonical blob
`479bbb95244b1ad7817158bf025dcc12568e8af3`). It preserves merged #254 at
`03e4c36b24a7a4a6a2a6083aef26b6f45b1dcd9e` and all integrated Runtime repairs.

The 68-case device provider includes actual HAL/CUDA/XDMA candidates, bounded
protocol faults, explicit host staging, complete Runtime kernel/graph pipelines,
functional supplied-session paths and optional native CUDA/XDMA/combined hosts.
Eleven focused tests, all CLI oracles and steady allocation checks pass locally.
The final installed/relocated/embedded package chain, ASan/UBSan device tests
and allocation guard pass. Full-profile and exact-head hosted/merge outcomes are
recorded in PR #260. See `docs/evidence/M23-04-acceptance-2026-09-24.md` and the retained
implementation evidence for coverage and failures. Physical characterization is
NOT RUN because no accessible bench is present. M23-05 remains separate.

## Historical M23-03 completion checkpoint

M23-03 implementation and portable local verification are complete: 87 public
Runtime benchmark cases, all 51 allocation-tracked cases, original 51 CPU cases,
legacy artifacts, independent owners, sanitizer suites and SDK integration pass.
The source includes verified replay, watchdog and device wake repairs. The Windows Debug crash was narrowed to a large automatic loopback fixture.
A bounded-stack negative control reproduces it; invocation-owned heap storage
passes the same 512 KiB caller-stack regression. Final hosted verification and
integration of that correction are tracked in PR #254. Final
exact-head hosted CI and merge identity are recorded in PR #254; the acceptance
map and retained failures are in the completion/integration evidence. M23 and
CAP-M23 remain open for separate M23-04/M23-05 scopes. Physical characterization
and controlled performance thresholds remain unperformed.

## Historical R4 integration checkpoint

M23-03 is active on integrated device wake repair #259, main
`49443ff1cee0966c228300530a93917cc95b81e0`. The repair passed all 32 hosted
checks, quick/full profiles, both sanitizer suites and relocated SDK/ABI checks.
The existing 87-case benchmark implementation is unchanged. PR #254 is being
freshly verified against this Runtime, including the previously timed-out CLI
and allocation gates. See `docs/evidence/M23-03-integration-2026-09-24.md`.
Earlier activation and verification checkpoints below remain historical.

## Historical benchmark checkpoint before R4

M23-03 continuation: replay/watchdog prerequisite PR #258 is merged at
`8ba51b3effa6174eaeb5382aef3404eeb5c160d1`, with all 32 hosted checks passed.
Its public regression, sanitizer, full-profile and relocated SDK results are
recorded in PR #258. M23-03 is active again on this integrated baseline.
Finish the remaining benchmark acceptance audit and verification in #254;
M23-04/M23-05 and physical characterization remain separate.

## Historical R4 activation

M23-03R4 is active under merged control #482. Target baseline is merged
watchdog #258 at `8ba51b3effa6174eaeb5382aef3404eeb5c160d1`. Benchmark #254
is retained at `e28a926aa115dcb19b46451f69bf16d806e91ac8`: all 87 in-process
cases, sanitizers and package chain pass, but an original loopback CLI case
timed out after 600 seconds. Repair the existing device-worker wake ordering
under this separate scope, verify and integrate, then resume unchanged M23-03.
Earlier dated checkpoints below remain historical.


## 2026-09-24: M23-03R3 watchdog saturation replay repair active

Integrated queue #257 and replay #256 main is
`52fbdd3b029f1531f8d5d40af85e3f8e11603571`. Separate control PR #481 merged at
`19eaf9eb45434584c3b5aecb1976a97d4e626dd8`; canonical blob is
`8d63ada5ed44e457440c6736bf65c7cd40d718fb`. Repair only the watchdog transition's
actual prior degradation level, with public active/nested replay regressions.
Benchmark draft #254 is retained at 0bd116eeb167d585108f4079d4f22a81ad9ff32f:
83/84 cases pass, with composition-fault-8 blocked by this independently
reproduced defect. All original 78 logical workloads and failures remain.
Validation and integration are pending; no Runtime repair pass is claimed yet.
After merging the verified repair, reactivate unchanged M23-03 and finish #254.
Earlier sections below are historical.


## 2026-09-24: benchmark acceptance completion

The catalog now contains 87 cases, retaining every original workload. Three final
additions cover explicit optional shedding during composition/replay and actual
four-batch loopback occupancy. Callbacks include replay, accepted admissions have
a separate column, and two-instance budgets total 512 MiB. The optional static
analysis source routing is corrected; existing TSan includes Runtime provider
and mixed-rate replay tests. Final all-case/artifact, allocation, sanitizer,
package, full-profile and exact-head hosted results are tracked in PR #254 and
`docs/evidence/M23-03-completion-2026-09-24.md`.

The following dated checkpoints retain the original failures and decisions;
the current source includes the separately merged repairs.

## 2026-09-24: 84-case checkpoint and watchdog repair prerequisite

PR #254 retains all 78 original workloads and adds six explicit inspection,
capacity and device-failure cases. Focused verification is 83/84; 50 declared
steady invoke cases pass global allocation checks. The remaining fault-8
composition failure is independently reproduced with only a public CPU Runtime:
saturated watchdog records contain an incorrect prior degradation level.
Separate canonical M23-03R3 control PR #481 merged at
`19eaf9eb45434584c3b5aecb1976a97d4e626dd8`; implement/verify/integrate that repair
before reactivating the unchanged M23-03 benchmark contract. No Runtime file is
edited in this checkpoint. See the continuation evidence for initial failures,
Clang diagnostic compatibility repair and exact remaining verification.

## 2026-09-24: M23-03 benchmark completion activated

Queue PR #257 and replay v2 PR #256 are integrated. Exact main is
`52fbdd3b029f1531f8d5d40af85e3f8e11603571`; replay final head
`c9a86254a25ffecbe97536a2f422bf470c083bec` passed all 32 hosted checks,
51 focused tests, quick 33/33/full 37/37 and 20 relocated consumers. Default-v1
bytes/public layouts/ABIs remain unchanged and all three public replay consumers
report zero measured run/replay allocations. Original failures remain retained.

M23-03 now binds merged control amendment #475 at
`b489a1a32e1ddd84f7213bae053114a2f17e79d7`, blob
`a6e1ea91b9e2b93833ea8992b46fdbe347797ecf`. Draft #254 is reconciled onto main.
Retain all 78 cases and original 74/78/intermediate 76/78 failures; integrate
explicit v2 retention into composition fixtures, then complete every partial
acceptance item before merging. M23/CAP-M23 remain open for M23-04/M23-05.
No Runtime or prior-provider/framework/validator changes are authorized here.
Unreal remains excluded. Hardware characterization is unperformed because this
host exposes no NVIDIA/XDMA device nodes. Earlier text below is historical.

## M23-03 Runtime benchmark work in progress

M23-02 discovery closure merged as PR #253 at
`795a56cabb78dd2c6ccf51796f54c4f6eff84f9f`. The active M23-03 contract
is bound to portfolio-control PR #367, revision
`38a7c91d8c3fe7a663a57d0353a3e60773ded20d`.

The draft adds 78 public Runtime benchmark descriptors, implementation, CLI
registration and source-example consumers. Standalone fixture checks cover
CPU/device rates, cross-rate payloads, controls, checkpoints, replay, watchdog
and telemetry. Four combined live-control/active-replay cases remain failing.
A small public-only reproducer returns `incompatible_artifact` with application
state, callback count, generation count and accepted-admission count all correct.
The complete Runtime replay-state comparison requires a separately scoped repair;
no Runtime implementation or schema has been changed in this benchmark batch.
The proposed repair scope is [portfolio-control PR #369](https://github.com/tranquilWorks/portfolio-control/pull/369); it remains a draft requiring approval.

Local package, ABI and contract checks pass. Quick CTest is 33/33; strict full
CTest is 36/37, with an additional unresolved failure in unchanged
`PredictiveAdaptive.PrestepsReduceReactiveCatchup`. No full-profile pass is claimed.

The owner temporarily waived unavailable hosted CI/CD on 2026-09-08 because of
utilization limits. This does not waive functional verification. M23-03 and
M23/CAP-M23 remain incomplete. Follow the exact failed/unperformed results and
remaining acceptance work in [M23-03 evidence](evidence/M23-03-2026-09-08.md).

Historical M23-02 audit: 2026-09-07
Planning baseline: `b52b42a6c64066553ce55fb08c874061d78e036e`
Continuation baseline: `543af2b4728a171243e6d756554efa188c2e7d2f` (merged PR #252)

## M23-02 CPU benchmarks and discovery closure

M23-01 merged as PR #251 after repaired head
`cb136d671088ed01bc75b5be0c67d991e7814cdf` passed CI run 34080242894.
Its initial Clang failure and local dependency blockage remain historical
facts in the unchanged M23-01 evidence. No separate human-review record is
inferred from merge. M22/CAP-M22 remain portable-software complete.

Merged PR #252 adds 51 CPU graph/executor/memory cases through the
unchanged optional runner, explicit CLI selection, installed source examples,
structural oracles and checked cleanup. See [benchmarking](benchmarking.md) and
[M23-02 evidence](evidence/M23-02-2026-09-07.md) for the original validation and failures.
Control PR #366 merged and authorizes the exact CLI inventory regression
correction. The [discovery closure](evidence/M23-02-discovery-closure-2026-09-07.md)
records the follow-up verification; merged source alone does not imply passing CI.
M23/CAP-M23, M24-M26, hardware/RT, Unreal, signing/release and deployment remain
incomplete. Correction PR #253 passed all 32 hosted checks before merge; M23-03 is active as described above.

## Product state

- Release 1.2.1 remains the supported portable RT0 product.
- Stable C ABI v8 remains exactly 70 exports with SONAME 8 and its frozen
  fingerprint. Device ABI v1, HAL core v2, memory/topology extension v1, and
  command/timeline extension v1 are unchanged.
- Runtime-profile schema 7 and its 25 keys, global observability schema 2,
  checkpoint/input-log schema 1, and rate-action schema 1 are unchanged.
  M21-05 uses separate additive C++ mixed-rate-action and active-replay
  schemas, both version 1.
- The installed target inventory, 1.x aliases, support matrices, and
  Apache-2.0 license are unchanged. The retained M19 addition is
  `rt/extension_abi.h`; M21-04 adds exactly the additive C++ header
  `rt/loopback_backend.hpp`. M22-04 adds exactly the optional header-only C++
  source API `rt/live_control.hpp`.
- M14, M14.1, M15, and M16 are complete. M17-01 through M17-04 and the
  M17-06 portable discovery/composition repair are merged. M17 and CAP-M17
  remain incomplete because named physical CUDA/XDMA, combined-hardware, RT,
  and manual qualification records do not exist. M18-01 offline qualification
  schemas and tools remain proposal-only and unpromoted. M19-01 adds extension
  ABI v1; M19 and CAP-M19 remain incomplete, and Unreal work is not part of the
  current Linux-host batch. M20-PRE-01, M21-01, and M21-02 are merged. M21-03
  and M21-04 are merged. M21-05 is merged at the audited baseline and closes
  the portable M21 software path. M22-01 through M22-03 are merged. M22-04 is merged as PR #250 and closes the portable M22 software path.
  M23-01 and M23-02 are merged; M23-03 is active with both replay repairs integrated.

## M22-04 typed live-control SDK and stress (merged foundation)

M22-01 added an opt-in additive C++ policy with positive bounded mailbox,
producer, record, per-record payload, and total copied-payload capacities.
Configuration copies fixed mailbox and producer declarations. Successful
finalization allocates all slots and payload storage, binds producer handles
to one Runtime identity and configuration generation, validates exact compiled
rate-release targets, and includes the frozen policy/declarations in graph,
configuration, replay, and exact Runtime-control accounting.

Admission remains an explicit non-RT producer operation. It performs one bounded
reservation attempt, copies canonical payload bytes before release
publication, and returns distinct accepted, invalid, full, busy, stale,
stopped, exhausted, or missed outcomes. M22-02 closes host-frame and exact
compiled rate-release targets before their callbacks, sorts complete records
by mailbox identity and mailbox sequence, applies same-mailbox/update-kind
replacement, and atomically publishes one copied immutable generation.
Read-only inspection exposes copied records, counters, occupancy, and
exact-size payload copying without an internal address. Stop closes admission
before existing cleanup ownership is processed.

Callbacks receive a nullable callback-lifetime view of fixed record metadata
and host payload spans. No payload is parsed or transferred to a backend
implicitly. M22-03 captures the step-entry immutable generation in a third
preallocated store. Boundary records remain provisional until the complete
step succeeds; any later step failure restores that Runtime-owned generation
and terminalizes the source slots as rolled back. This does not reverse
application, backend, external-process, or physical-device side effects.

M22-03 adds a separate fixed 256-byte payload-free action schema and
runtime-bound gap-reporting cursors. Closure-enabled ordinary checkpoints add
one `rtfw.live-control` schema-1 state record while closure-disabled checkpoint
bytes remain unchanged. A distinct bounded live-control replay artifact embeds
one unchanged checkpoint and one unchanged input-log or active-replay artifact,
plus correlated actions and explicitly retained generation payload bytes.
Replay fully validates before restore and injects immutable generations at
their exact boundaries; deterministic backend restrictions remain in force.
Every prior artifact, observability, rate-action, mixed-rate-action, and stable
ABI schema remains unchanged. M22-04 adds one installed header-only source API,
`rt/live_control.hpp`. Its 32-byte `RTLC` envelope has fixed little-endian
magic/version/extent/type/schema/kind/reserved fields followed by a positive
compile-time fixed body. Application specializations provide bounded
`noexcept` validate/encode/decode operations; builders write caller-owned
storage and one raw `LiveControlUpdateRecord` but do not stage, retry, retarget,
advance a sequence, or own checked teardown.

The supported sample provides fixed scenario, controller-gain, sensor-
calibration, and fault-configuration types, exact host and compiled-rate
targets, raw empty clear-fault compatibility, whole-value callback decode,
replacement/commit/rollback actions, and checked stop. Deterministic tests
cover absolute 64-mailbox/256-producer/65,536-record limits, the 65,536-byte
typed payload boundary, checked 1 GiB policy arithmetic without 1 GiB physical
commitment, one-attempt concurrent admission, representative full occupancy,
action loss/replay disqualification, watchdog/rollback interaction, repeated
lifecycle, and concurrent Runtime isolation. Portable tests are RT0 evidence
only; hosted CI and human SDK/format/callback/concurrency/compatibility/privacy/
claim review remain mandatory before M22/CAP-M22 can be called complete.

## M21-05 mixed-rate closure

M21-05 copies and freezes an optional C++ closure policy before finalization.
It preallocates a fixed 256-byte action-record ring with runtime-bound cursors,
monotonic sequence reservation, exact drop/overwrite/gap reporting, and replay
eligibility that fails closed after telemetry loss. Closed actions correlate
rate decisions, settled device outcomes, sampled publication/selection,
safe-output acknowledgement, watchdog/degradation changes, and checked stop
without exposing payload bytes, addresses, vendor handles, or wall-clock
thread identity.

Closure-enabled checkpoints append one ordinary schema-1 state record at a
quiescent release boundary; disabled and legacy checkpoint bytes remain
unchanged. A distinct bounded little-endian active-replay artifact binds that
checkpoint, explicit caller-owned inputs, the complete ordered action
transcript, semantic identities, per-record checksums, and a whole-artifact
checksum. Active replay accepts only frozen deterministic-mock backends,
prevalidates before restore, drives recorded logical decisions, re-executes the
mock/loopback provider path, and compares actions, payload/frame identities,
sampled metadata, terminal status, and final state.

The installed loopback adds bounded instance-local logical-action inspection.
A table-driven public-header conformance fixture runs CPU plant, device sensor,
CPU controller, device actuator, and observer work across three distinct rate
periods, sampled hold behavior, acknowledged safety, checkpoint, and exact
active replay. Local strict compilation, 9 focused tests, a 91-test impacted
suite, and direct package-consumer execution passed. All five hosted workflows,
the 24-job main CI matrix, and human review passed before merge. M21 and
CAP-M21 are portable-software complete in merged target history.
Physical/vendor I/O, HIL, RT1, and RT2 remain unclaimed.

## M21-04 sampled I/O and loopback

M21-04 adds copied fixed-capacity sampled input/output descriptors over one
exact admitted M21-03 payload endpoint. Finalization closes encoding and frame
geometry, rational scaling, units/calibration, sample interval, clock,
timestamp and trigger identities, sequence/generation, ring capacity,
stale/overrun/underrun policy, and exact initial/startup/failure/shutdown
frames. Device input publishes only after exact terminal completion and full
frame validation. Startup and checked stop submit safe outputs through the
existing backend lane and require terminal acknowledgement.

The installed `SampledIoLoopbackBackend` provides deterministic host-coherent
HAL-v2 frame transfer and bounded fault injection for portable tests. This is
RT0 software-loopback behavior only; vendor/physical I/O, electrical and timing
validation, and RT1/RT2 remain unclaimed.

## M21-03 CPU/device cross-rate payloads

M21-02's admitted dispatch/completion path remains unchanged for channel-free
plans. M21-03 appends explicit producer/consumer device selectors to the C++
cross-rate registration. Each selector names one ordered copied M21-01 payload
reference and a positive slot stride. Finalization accepts CPU→device or
device→CPU only, proves host-coherent access and role/direction agreement, and
derives one disjoint exact payload subrange for every admitted in-flight slot.

Before a device consumer provider runs, Runtime selects and copies the exact
CPU-produced generation into its derived input subrange. The provider must
still match the complete frozen declaration; only Runtime then substitutes the
selected offset/byte count in its owned materialization. Device output remains
unpublished until M21-02 correlates terminal success. Runtime retains terminal
slot ownership, copies the exact output subrange into the SPSC store, records
release/completion timestamp metadata, publishes one generation, and only then
releases dependent work and recycles the device slot.

This is portable RT0 fake-driver payload evidence. It adds no sampled-clock,
trigger, calibration, safe-output, overrun/underrun, replay, physical CUDA/
XDMA/DAC/DAQ, HIL, RT1, or RT2 claim. The later M21 batches do not promote
those claims.

## M21-02 device-rate dispatch and completion

M21-01's additive device-rate model and conservative cyclic admission are now
merged. M21-02 activates only admitted HAL-v2 command-batch references in an
M16 active plan. Finalization precomputes reference-to-device indexes and
same-group device dependency slices, and allocates one exact generation-tagged
completion ticket per reference record. The provider receives the appended
nullable `DeviceCallbackContext::rate_release` view and still materializes only
values permitted by the copied M17 declaration.

Each due provider runs exactly once through the existing executor, copies its
fixed-capacity batch and exact domain/phase/cycle/sequence/substep identity into
an existing backend slot, and returns without holding the executor phase open.
The existing per-backend submission lane performs vendor submit; the existing
service lane polls completion and owns submitted timeout cancellation.
Independent same-group device references can therefore remain outstanding at
the same time. Precomputed graph dependencies consume prerequisite tickets
before a dependent CPU/device callback, and the release-group barrier consumes
all remaining tickets before a successful active step advances.

Provider timeouts are finite and clamped to the earlier checked completion-
budget or release-deadline remainder. A timeout after vendor ownership enters a
non-reusable terminal quarantine, issues at most one cancellation request, and
cannot become success when a late completion arrives. Checked backend shutdown
is the reclamation boundary. Existing device metrics/traces and active-rate
failure/action accounting are reused without schema changes.

This remains the merged portable RT0 dispatch/completion foundation. Its
channel-free behavior, schemas, lane count, and qualification boundary are
unchanged by M21-03.

## M20-PRE-01 portable assurance

The active batch adds one host-independent `scripts/verify-portable-assurance.sh`
entry point with independently runnable `dependencies`, `static`, `fuzz`,
`artifacts`, and cumulative `all` modes. It reconciles exact build/action pins,
the complete default first-party Clang 14 compilation manifest, two supported
64 KiB parser fuzzers and one explicitly experimental 4 KiB queue fuzzer, a
canonical SPDX 2.3 candidate SBOM, an unsigned in-toto Statement v1 candidate,
an expected-source final manifest, offline RSA/DSSE verification of a retained
public non-target fixture, and extraction/relocated consumption of the default
package.

The lane has repository-read-only GitHub permissions and no private-key,
signing, OIDC, attestation-creation, publication, release, hardware, privileged
host, controlled-timing, or Unreal path. Its machine reports are candidate CI
evidence only. They do not establish continuous fuzzing, vulnerability or
license clearance, reproducible builds, authenticated RTFW provenance, a SLSA
level, support promotion, hardware/RT qualification, or production readiness.
See `docs/portable_assurance.md`.

## M19-01 extension registration

The installed C11 `rt/extension_abi.h` and additive C++ Runtime surface accept
an already-resolved entry function while configuring. One bounded transaction
copies names, descriptor/table prefixes, and local relationships and may
publish CPU phases, device-ABI-v1 backends through the canonical adapter, and
host-control services. Failed attempts publish nothing and retire provisional
generations.

Services initialize before Runtime lanes and backends. Checked stop closes
admission, retains the first error while continuing independent cleanup,
retries unresolved owners, and shuts services down in reverse order only after
related backend ownership is released. Checked detach clears borrowed callable
pointers, retires the handle generation, and reports host unload readiness; it
never unloads a module. See `docs/extension_registration.md`.

The change adds no C ABI export, loader dependency, target, Runtime lane,
MemoryPlan row, schema, or native HAL-v2 extension capability.
Extensions remain trusted in-process code, and no physical, HIL, field,
Unreal, RT1/RT2, signing, release, deployment, or production validation is
claimed.

## M17-04 implementation

The installed CUDA and XDMA candidate headers retain their exact device-ABI-v1
`api()` paths and add native `hal_v2_registration(name)` paths. Each native
registration exposes HAL core v2, memory/topology extension v1, and
command/timeline extension v1 tables. M17-06 makes those tables reachable
through canonical Runtime discovery without changing either vendor callback.
Each backend object admits only one chosen registration path until checked
shutdown succeeds.

Both injectable driver tables retain their complete version-1 positional
prefix and version-1 default. Exact version 2 tables add only the native
controls: CUDA Graph launch, or XDMA 32-bit control read/write, user-event wait,
and nonblocking stop request. A version-1 table exposes none of those
capabilities.

CUDA native-v2 describes one explicit staged host/device domain and
host-monotonic completion timestamps. Up to 16 caller-owned, already
instantiated graph executables may be registered before initialization under
unique identifiers 1 through 65535, with up to eight copied stable buffer
bindings. Graph dispatch uses `0x43470000 | graph_id`, zero inline payload, and
the exact declared bindings. One configured command stream executes explicit
copies, registered kernels, and graphs in order and records one precreated
completion event after the accepted batch is fully enqueued. RTFW never
captures, instantiates, updates, clones, uploads, destroys, or owns a graph.

XDMA native-v2 describes borrowed host staging, its configured AXI-MM endpoint,
and host-monotonic completion timestamps. Stable vendor commands encode
little-endian control read as `0x58480000 | offset/4`, control write as
`0x58490000 | offset/4`, and user-event wait as `0x584A0000 | event_index`.
The aperture is explicitly enabled, four-byte aligned, nonzero, and at most
262144 bytes; at most 16 events are configured. The Linux adapter copies
optional user-BAR and event paths, opens only configured endpoints, and uses a
nonblocking stop wakeup for finite event waits. All operations remain on the
existing fixed backend I/O team.

The new graph, control, event, registry, queue, and worker storage is fixed
backend-private storage reported through existing capability fields. Runtime
adds no lane, MemoryPlan row, provider region, schema, status, implicit command,
cross-backend timeline, spill, detached work, or executor-worker vendor call.
Malformed commands fail before vendor entry; uncertain CUDA or XDMA work
retains every referenced owner until physical readiness, successful drain, or
the documented final host reclamation boundary.

## M17-06 Runtime repair and portable combined graph

`discover_command_timeline_extension()` now passes a value-initialized
`HalV2CommandTimelineCapabilities` to the callback. Its default member
initializers supply the exact size/version input prefix while every semantic
and reserved field starts zero. Existing callback status mapping and complete
returned-record validation remain fail closed. Focused tests cover exact input,
malformed/partial/error/exception output, transactional rejection, corrected
retry, and simultaneous native CUDA/XDMA registration in one real Runtime.

The default, non-installed `sample_cpu_gpu_fpga_cpu` composes exactly CPU
prepare, simulated CUDA upload/Graph/download, a disjoint bounded host bridge,
simulated XDMA H2C/control/event/C2H, and CPU validation. Two separate
backend-local timelines, fixed payload/device/card/control/event storage,
finite timeouts, exact call/thread/cause assertions, post-start allocation
instrumentation, failure suppression, cancellation, recovery, isolation, and
checked cleanup are covered by its focused CTest executable. No timeline or
memory object crosses backends and no direct peer DMA is represented.

This is portable RT0 simulated-protocol evidence only. The portable M17-06
software path is merged, but physical CUDA/XDMA memory, Graph execution, MMIO,
interrupts, timing,
HIL, field, RT1/RT2, support promotion, signing, release, deployment, and
production qualification remain deferred. The failed M17-05 review remains
immutable in `docs/evidence/M17-05-2026-08-12.md`; current results are retained
in `docs/evidence/M17-06-2026-08-23.md`.

## M18-01 offline qualification plane

Four independent JSON Schema draft 2020-12 version-1 documents define a
separately retained campaign plan, immutable qualification record, human
promotion review, and generated promotion proposal. The Python 3.11
standard-library tool validates complete scope-specific tuple, policy,
workload, trial, threshold, resource, thermal, health, recovery, artifact, and
digest chains with bounded strict JSON and filesystem handling.

Synthetic fixtures cover NVIDIA, XDMA, combined, RT1, and RT2. They are labeled
`synthetic_fixture`, generate only `proposal_only` output, and are never
support-matrix eligible. M18-01's unchanged qualification tool still rejects a
non-synthetic combined proposal; revising that promotion policy is outside
M17-06. Reviewer names and timestamps are not authenticated, and external
pre-run plan provenance remains a human gate.

See `docs/qualification.md`. M18-01 changes no runtime, ABI, installed package,
version, support matrix, hardware workflow/sample, or prior evidence. It
performs no physical hardware, controlled timing, HIL, field, RT1/RT2, signing,
release, deployment, production, or support-promotion validation.

## Historical M23-05R activation checkpoint

M23-05R is active under merged control #496 and corrective amendment #498,
`09ed06bebf21ece833ffa1626826eaf99e042521`, canonical blob
`95def2eaf6bb45783bbc698ac195474ef0fe995a`. Baseline is merged M23-04
#260 at `37c3b155894133789621d41b4233c8edde79d023`. Feature PR #261
remains draft at `4f780cf23096155da50ff61a1c5fe4970c004a88`; its extra
normal CTest run observed one unexpected cancellation after completed batches.
Retire ordinary batch ownership before graph completion, preserve every test
assertion and prior failure, verify and integrate separately, then reconcile
M23-05 and freshly verify the combined feature tree.

See [repair evidence](evidence/M23-05R-2026-09-25.md). Current verification
and exact-head integration disposition are retained there and in the repair PR.
Physical characterization and controlled timing remain NOT RUN.
