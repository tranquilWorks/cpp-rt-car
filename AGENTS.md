# RTFW repository instructions

RTFW is an assurance-profile C++20 bounded simulation runtime. The supported
portable product is release 1.2.1 at RT0 with stable C ABI v8, SONAME 8, device
ABI v1, and Apache-2.0. M14, M14.1, M15, M16, and the portable M17 software
path through M17-06 are complete, while hardware/RT qualification remains
incomplete. M20-PRE-01, M21-01 through M21-05, and M22-01 through M22-04 are
merged. M23-01 and M23-02 are merged. M23-03R2, M23-03R3 and M23-03R4 are merged;
M23-03 is the active approved benchmark batch, with implementation and local
verification complete. CI-QUEUE-01 is merged as PR #257. PR #254 records final
exact-head hosted verification and integration; M23-04/M23-05 remain separate.
M16-01 supplies the exact reference timeline, M16-02 adds deterministic
CPU-only cross-rate channel selection and bounded SPSC stores, and M16-03 adds
opt-in mandatory admission, dispatch, transfer, and late actions. M16-04 adds
optional CPU shedding/recovery and a separate versioned rate-action stream; it
does not add action replay, device-rate execution, or RT1/RT2 qualification.
M17-01 adds the additive C++ HAL v2 core contract and routes unchanged device
ABI v1 backends through one bounded compatibility adapter. M17-02 adds the
bounded C++ memory/topology extension, six explicit heterogeneous-memory
domains, copied topology/timestamp contracts, and canonical heterogeneous
registration while preserving the legacy core path. M17-03 adds the bounded
command/timeline extension, same-backend timeline completion, explicit ordered
memory synchronization, and one isolated Runtime submission lane per opted-in
backend. M17-04 adds native HAL-v2 registration paths to the preserved CUDA and
XDMA candidates, bounded caller-owned CUDA Graph launch, and bounded XDMA
control/event operations. M17-06 repairs Runtime-owned capability discovery
and adds a portable CPU-to-simulated-CUDA-to-host-stage-to-simulated-XDMA-to-
CPU sample with backend-local timelines. It is simulated-protocol evidence
only. M18-01 adds bounded offline qualification schemas and proposal tooling
only; it promotes no tuple. Physical combined execution and hardware or RT
qualification remain deferred.

M21-01 adds the bounded C++ device-rate model, immutable mixed reference plan,
deterministic cyclic admission/reporting, and exact identity/accounting
integration for opted-in HAL-v2 command-batch phases. M21-02 activates their
bounded release dispatch and completion on existing Runtime backend lanes,
with precomputed release/frontier/slot state, concurrent admitted work, exact
dependency barriers, and deterministic timeout/cancel/stop ownership. M21-03
adds bounded CPU-to-device and device-to-CPU cross-rate payload transport over
explicitly selected, pre-registered host-visible command references. M21-04
adds only generic fixed-capacity sampled-I/O descriptors, acknowledged safe-
output transitions, and a public package-consumable HAL-v2 loopback backend.
It does not add replay/telemetry closure, a vendor adapter, change versioned
schemas, promote support, or establish physical CUDA, XDMA, DAC/DAQ, CAN,
IIO, HIL, RT1, or RT2 evidence. M21-05 owns the distinct bounded mixed-rate
action and deterministic active-replay closure for declared mock/loopback
backends only.

M22-01 adds only an opt-in C++ live-control staging surface: copied bounded
policy, mailbox and producer declarations; Runtime-generation-bound producer
handles; canonical fixed records; Runtime-owned payload copies; bounded
nonblocking admission; deterministic inspection; and exact identity/accounting.
M22-02 adds exact host-frame and active rate-release boundary consumption,
canonical replacement/order, immutable generation publication, callback-local
views, terminal reclamation, and fixed inspection. It does not roll back,
checkpoint, replay, or emit action telemetry. M22-03 owns bounded Runtime-
generation rollback, payload-free action telemetry, conditional checkpoint
state, and explicit trusted-payload replay artifacts. M22-04 adds the optional
header-only fixed typed-payload SDK, compiling data/config examples, package
coverage, and deterministic capacity/concurrency/lifecycle stress. It changes
no compiled ABI or Runtime schema and is the portable CAP-M22 closure batch.

## Read before nontrivial work

1. `contracts/repo-profile.yaml`
2. `contracts/active-batch.yaml`
3. `docs/CURRENT_STATE.md`
4. `docs/HANDOFF.md`
5. `docs/product_contract.md`
6. `docs/architecture.md`
7. `docs/roadmap.md`
8. Relevant component contracts and ADRs
9. `.agents/skills/rtfw-assurance/SKILL.md`

The control-plane product source is
`tranquilWorks/portfolio-control/products/cpp-rt-car/`. Repository contracts and the
actual code remain authoritative when a stale control-plane copy conflicts.

## Protected invariants

- Preserve stable C ABI v8, SONAME 8, the exact export allowlist, and ABI
  fingerprint unless an explicitly approved ABI-version batch says otherwise.
- Preserve device ABI v1 compatibility; HAL v2 work is additive.
- Keep `rtfw::runtime` free of experimental SimCore, scheduler, fiber, plugin,
  crashdump, HAL/GPU stubs, `dl`, project warning policy, and leaked feature
  macros.
- Default installation exposes only the contracted SDK targets and headers;
  compatibility aliases remain usable within 1.x.
- Declared RT lanes do not gain ordinary heap allocation, hidden thread
  creation, file I/O, blocking mutexes, unbounded waits, or spill execution.
- Multiple runtime instances remain isolated.
- CUDA, XDMA, combined, RT1, and RT2 support require named retained evidence.
  Portable CI, preflight, or a benchmark alone is not qualification.
- Do not assume direct GPU-to-FPGA peer DMA.
- Preserve Apache-2.0 and the release/support claim boundary.

## Working protocol

- Implement only the active approved batch. Treat allowed and forbidden paths,
  acceptance, validation, rollback, and stop conditions as binding.
- Inspect before editing; do not infer repository state from a prompt.
- Use an isolated branch/worktree and preserve unrelated user work.
- Make the smallest coherent change and add tests with behavior.
- Run focused checks, then `./scripts/agent-verify.sh full`.
- Record commands, results, acceptance status, changed invariants, residual
  risks, and unperformed validation under `docs/evidence/`.
- A mocked backend, hosted runner, or portable build is not physical hardware
  evidence.
- Stop rather than silently broaden scope when a product decision, forbidden
  path, compatibility change, or credible validation gap appears.

## Repository actions

Do not commit, push, open or merge a PR, release, deploy, change secrets, or
modify repository settings unless explicitly instructed. When publication is
authorized, use a scoped branch and draft PR; deterministic CI remains
authoritative and no agent may waive a failing gate.

## Governed continuous delivery

Invoking `./scripts/product-autopilot.sh` is the explicit instruction to plan,
implement, verify, publish, and merge only the bounded control/target PRs created
by that run, then continue through dependency-ready software batches. The
wrapper still cannot release, deploy, change secrets/settings, waive a gate, or
manufacture hardware/RT qualification. It stops at a genuine product decision,
mandatory hardware/RT evidence, signing, release, or deployment gate. See
`docs/runbooks/continuous-autopilot.md`.

<!-- BEGIN PORTFOLIO-CONTROL MANAGED -->
## Governed agentic delivery

- Read `.agents/skills/engineering-execution/SKILL.md` for nontrivial work: complete the requested outcome, verify its entry point, and preserve context.
- Product: `cpp-rt-car`; delivery profile: `assurance`.
- Control revision: `aeebab5f0140c2ac9161ff88f8fe42749ce42ebc`; harness version: `2`.
- Read `contracts/profile-requirements.yaml` and the approved
  `contracts/active-batch.yaml` before implementation.
- Stay inside active-batch allowed paths and preserve every forbidden path.
- Run the repository-local verification contract before claiming completion.
- Record exact evidence and distinguish static, simulated, protocol, bench,
  field, playtest, staging, and production validation.
- Do not claim physical, release, deployment, or production evidence that was
  not actually produced.
<!-- END PORTFOLIO-CONTROL MANAGED -->

## M23-03R2 continuation (2026-09-24)

The owner approved opt-in trusted replay v2 and confirmed merged control PR #463
at `092bab84f4228acc35a2eacf024ba0a61d1fe2e7`. Its canonical blob and reconciled
main/repair baselines are bound in `contracts/active-batch.yaml`.
Finish the lossless repair in draft PR #256, verify and integrate it, then amend
M23-03 separately before its benchmark fixtures select v2. Preserve default v1
bytes, existing public layouts, stable ABIs, complete state/transcript checks and
all earlier failed evidence. M23-03 and dependent scopes remain open.
Continue the requested non-Unreal work under standing scoped publication/merge
authorization. The hosted-utilization waiver never waives local functional failures.
Physical characterization requires an actual accessible bench. Workspace cleanup
removed the unpublished earlier v2 candidate; reconstructed source requires fresh
verification. Preserve durable draft checkpoints throughout this continuation.

## Replay integration after CI-QUEUE-01

Queue repair PR #257 merged at `da22fdaff9e7e5d3fbffa8013c0db7bd81f3697c`
after all 32 exact-head checks passed. Preserve its sources/evidence unchanged.
Reactivate M23-03R2 relative to this integrated main, reconcile draft #256,
verify the combined source and integrate before editing benchmark draft #254.
M23-03's v2 fixture amendment is already merged in control PR #475 at
`b489a1a32e1ddd84f7213bae053114a2f17e79d7`; activate it only after R2 integration.

## M23-03 completion after replay integration

PR #256 merged at `52fbdd3b029f1531f8d5d40af85e3f8e11603571` after all 32
exact-head hosted checks, focused/full/default-v1/ABI/no-allocation and relocated
consumer checks passed. Activate merged control amendment #475 and complete
the retained Runtime benchmark draft #254. Preserve all 78 original cases and
logical workloads, explicitly opt in to v2 only in fixtures needing admission
history, and close all partial acceptance without changing Runtime or prior
providers/framework/validators. M23-04 and M23-05 remain separate later scopes.
Standing scoped publication/merge authorization continues; no functional gate
waiver, Unreal work or invented physical qualification is permitted.

## Benchmark continuation after M23-03R3

Watchdog repair PR #258 merged at `8ba51b3effa6174eaeb5382aef3404eeb5c160d1`
after all 32 exact-head hosted gates passed at `a520626bd0fa024afb54f3fe2270669acf1db562`.
Its full local profiles, ASan/UBSan, TSan, 20 relocated package consumers, ABI,
default-v1 golden and unchanged 84 benchmark cases passed. The original timeout
and all earlier failures remain retained in PR #258 and its evidence. Preserve
this integrated Runtime source and reactivate the existing M23-03 amendment
(control #475). Finish benchmark PR #254 under standing scoped publication/merge
authorization. No cybersecurity, Daybreak or Unreal scope is authorized.

## Benchmark continuation after M23-03R4

Device wake ordering repair #259 merged at
`49443ff1cee0966c228300530a93917cc95b81e0` after all 32 exact-head checks
passed at `ea97f91968b9661d83c8b10ba733ef036fb22e8b`, workflow 36030838286.
Local quick 33/33, full 37/37, ASan/UBSan 41/41, TSan 41/41, relocated SDK
20/20 and compiled ABI checks passed. Retained execution-permission failures
and their ordinary relinks are in #259. Preserve all integrated Runtime and
repair evidence unchanged. Reactivate canonical M23-03 control #475; benchmark
implementation remains the same 87 cases. Freshly run every benchmark gate,
including the previously timed-out loopback CLI and no-allocation checks.
Keep the separate prior Windows experimental determinism timeout explicit.
Finish #254 under standing scoped publication/merge authorization. No
cybersecurity, Daybreak, Unreal or physical qualification scope is added.

## M23-04 activation after PR #254

PR #254 merged at `03e4c36b24a7a4a6a2a6083aef26b6f45b1dcd9e`; all 32
exact-head checks passed at `f9796335a7e706bb76f97a79dbe76939a86b8247`.
M23-04 is now active under canonical control PR #488, merge
`49b134215e74b2d6f27ae1cab3801ac9dc8c139d`. Implement the separately bounded
HAL/CUDA/XDMA/pipeline benchmark and optional real-session paths. Preserve
Runtime, drivers, framework/schema/validator and prior providers/repair evidence.
The dependency queue already authorizes scoped software publication/integration.
No cybersecurity, Daybreak or Unreal work. Missing real devices report NOT RUN.
M23-05 and physical characterization remain separate; no completed work is
reopened by the historical checkpoint text above.

## M23-05R completion ordering repair (2026-09-25)

Canonical control #496 merged at `7890ad076ce0a53a624bcf02e4c937ea9768aa3b`.
M23-05R is active from target main `37c3b155894133789621d41b4233c8edde79d023`.
Feature draft #261 is preserved at `4f780cf23096155da50ff61a1c5fe4970c004a88`.
An extra full local suite found one unexpected cancellation after completed
ordinary command batches; a later 50-repeat pass does not erase the failure.
Repair retirement before graph publication separately, retain old-order
negative control and all existing assertions, then verify and integrate under
standing scoped publication/merge authorization. Reconcile and reactivate
M23-05 afterward. No cybersecurity, Daybreak, Unreal or physical qualification.

Canonical amendment #497 (`5b258a4dc302a2ccf2c92162fe550144c8c694dc`) also
allows the two private mixed-rate conformance fixture/test files to park idle
CPU workers and inspect that policy. Retain the Windows startup failure; all
80-ms deadlines, logical periods, fault injections and existing oracles remain
unchanged. No Runtime default or rate-owned production path is modified.

Canonical correction #498 (`09ed06bebf21ece833ffa1626826eaf99e042521`)
withdraws the unsupported Windows park-policy attempt. Only the private
conformance fixture's safe-transition setup guard becomes five seconds;
active 80-ms provider/completion budgets and all logical/fault/output oracles
remain unchanged. A new startup missing-ack regression explicitly uses the
original 80-ms setting and verifies device_timeout, no scenario work and
checked cleanup. Runtime deadline enforcement/defaults are unchanged.

## M23-05 activation after PR #260

M23-04 is merged at `37c3b155894133789621d41b4233c8edde79d023` after all
32 exact-head checks passed. Canonical control PR #494, merge
`b61ab1396168647ab8309eb7015b25edfae9ecf1`, activates the already-authorized
M23-05 offline comparison/reporting and third-party source kit. Preserve every
prior benchmark/provider/validator, Runtime, driver, ABI and qualification boundary.
Standing scoped publication/merge authorization continues; no cybersecurity,
Daybreak, Unreal or fabricated physical/controlled evidence is authorized.
Historical checkpoint text above does not reopen completed batches.

## M23-05 reactivation after PR #262

Repair #262 merged at `b39ac9f281862a5a0aff051880671218e117db47`, tree
`dbfc45df441bcad0696b34b7e92fc34827f60b34`, after all 32 exact-head checks
passed. Canonical reactivation #500 merged at
`76fb4a2261c4bdc401051172641f4148a11ec76d`. Reconcile feature #261 with
this Runtime, construct synthetic analysis manifests independently and compare
them with actual capture. Keep all original samples/assertions, v1 validation
and the 120-second CTest bound. Freshly verify combined local and hosted gates
before scoped integration. Preserve all failed evidence. Earlier activation
paragraphs are historical; standing scope and publication authorization remain.

## M24-01 activation (2026-09-27)

M23-05 is merged at `7e9fb883049df486f04a1d6730d36890cadc3853`.
Control PR #502 merged the separately bounded M24-01 contract at
`845e6180b9bf84dafb813ae023e19d4eedacb105`. Implement the Runtime CUDA particle reference,
independent closed-form CPU oracle, shared portable/real graph and installed
source consumers. Preserve Runtime, backends, benchmarks, ABI and support claims.
Standing scoped publication/integration authorization continues. The owner
explicitly said to continue around API failures; retain those as unperformed
external evidence, without waiving local functional checks or claiming hardware.
Historical checkpoints above do not reopen completed M23 work.

## M24-02 activation (2026-09-27)

M24-01 merged as PR #263 at `c64d0672f1478acd863c68865663550d7fe22f8c`
after all 32 hosted checks passed. Canonical M24-02 control #504 merged at
`36b1b8e1976c6063b1ea20c1cda3afec762e735b`. Implement only its bounded
two-stream, borrowed-memory, kernel/Graph and active-rate pipeline scope.
Preserve the original reference and tests, Runtime, backends, ABI and benchmarks.
Retain the prior local timeout and stack limitation. Standing scoped
publication/integration authorization continues; physical CUDA stays NOT RUN
without a named actual device. M24-03/04 and maturity closure remain separate.

## M24-03R stopped CUDA retirement repair

Control #510 merged at `97c8db65f265b64c8d10d15bdcb5c3a02c66631a`.
M24-03 draft #265 at `dede9ea` retains a pending CUDA cleanup ownership cycle.
Repair only the allowed CUDA implementation and regressions against M24-02
main, preserving all earlier tests/evidence and ABI/Runtime boundaries. Verify
old-order negative control, full/sanitizer/package/ABI and exact-head hosted gates,
then integrate separately and reactivate M24-03 under a canonical amendment.
Standing scoped publication/merge authorization persists; no physical claims.

Control amendment #511 at `273c042c04cda85c542a55b6e76ad4421f2c27ce`
also covers the reproduced failed native-memory registration zero-token rollback
cycle, within the same CUDA implementation/tests. Validate complete descriptors,
retire only exact uncertain residual ownership, preserve successful registrations,
and retain all failures. Reverify the combined repair before integrating #266.

## M24-03 reactivation after repair #266

M24-03R merged at `ff47a64883e73cd9c2984616249b302e413566dc` after all32
exact-head checks passed at b290fa5. Canonical reactivation #512 merged at
`f168122ed236002fe54af2cea0515d24cb9c81c1`. Finish preserved feature #265 on
this repaired source; Runtime/backends and repair regressions/evidence remain
protected. Ordinary stop cancels immediately; active noncancelable references
retain ownership to their existing finite budget and return timeout. Keep all
publication/resource/timeout oracles. Complete local and exact-head hosted gates,
then integrate and report remaining M24-04 and wider/manual work as requested.
Standing scoped publication/integration authority continues. No physical claims.

## M24-03 reactivation after Runtime repair #267

R2 merged at `27fcb90b49773196f8773c2edcf97ba746fa7f94`, exact tested tree
`59442fa32a4b76ad403f6d5d9937b5947278fcf1`, after all 32 checks passed at
c14a742 in CI36351132581. Canonical reactivation #514 merged at
`360bbc308989efe30bb2a6f51a455e7cf99aeeef`. Preserve the private atomic ownership
flag, its independent command-batch regression and all earlier CUDA repairs.
Finish feature #265 with fresh combined local and exact-head hosted gates.
Concurrent fixture setup stays on the control thread; accepted execution/fault
isolation and the actual 512 KiB CLI gate remain. Retain all earlier TSan,
stack and timeout failures. Then report remaining M24-04 and broader/manual
work. Standing scoped publication/integration authority continues.

## M24-04 activation

M24-03 merged as #265 at74e59e8850ccb24d71a0fdbedfe4305971e773df with all32
checks passing. Canonical M24-04 control #516 merged at
`ff8e989a5c9da0204082d9e03394d06ac900496d`. Implement only the optional benchmark,
host profiler-correlation and capability closure kit. Preserve all prior Runtime,
backend, CUDA model/test and benchmark source and repair evidence. Standing scoped
publication/integration authority persists; all target gates remain required.
Physical, controlled-performance, independent review, M25/M26 and release gates
remain separate. Earlier checkpoint paragraphs are historical.

## M25-01 activation (2026-09-28)

M24-04 merged as PR268 at facc42d4a09d1dab398d75dc92000a05fb08c396 after
all32 exact-head checks. Canonical M25-01 control PR520 merged at
d1512a06be447fb275c625ca51b7d67a854b4bed, blob462f1f1ab2915ff833905e45be317cb5a484c438.
Implement only the bounded hello/install/embedding/legacy separation contract.
Preserve Runtime, ABI, all M23/M24 implementation and evidence bindings.
Standing scoped publication/integration authorization applies. M25-02 and later
batches, independent novice review, hardware/RT, signing/release remain separate.

## M25-02 activation (2026-09-28)

M25-01 merged as PR269 at d3052a0f7f17a0cd68334123aea3869fcae2094a.
Canonical control PR524 merged at ad159304c00151e248ffe2b7f5699c341a8b5361,
blob d598ea817902036ebdf85cb1407e6cb78043ca08. Implement only its optional typed SDK, checked builders,
capacity arithmetic, guard, diagnostics and shipped consumer scope. Preserve
compiled production and historical evidence. All32 target checks remain required.
Standing scoped publication/integration authorization persists. Do not activate
M25-03; hardware/RT, independent review, performance and release remain separate.

## M25-03 activation (2026-09-28)

M25-02 merged as PR270 at e562ab164162b67d009171b5c8f53e9b04866285.
Canonical control PR527 merged at 7585342836adf7d5e90cd62acb8d4237dc802f22,
blob d65ec3071611b480748f3a92eca937160a5ea8d7. Implement only the backend
authoring source kit, bounded conformance, integration and package guide.
Preserve production, SDK headers, ABI and prior evidence. All32 target checks
remain required. Standing scoped publication/integration authority persists.
Do not activate M25-04; hardware/RT and release remain separate.

## M25-04 activation (2026-09-28)

M25-03 merged as PR271 at315b04b25dd41acbd3a21313a093cf90b19c95ac.
Canonical control PR530 merged at6927945598352edb25ec27759f6fd633a3b6f4de,
blob e0e7d67c81ca110f18762425402899273311403a. Implement only the optional
shared-memory source kit, actual external controller/Runtime plant, protocol,
reconnect/ownership tests and installed guide. Preserve compiled production,
SDK headers/targets, ABIs and prior source/evidence bindings. All32 target
checks remain required. Standing scoped publication/integration authority
persists. Do not activate M25-05; no hardware/RT or release promotion.

## M25-05 activation (2026-09-28)

M25-04 merged as PR272 at2c431a40f450aba06b721dd0929f467fd12652b4.
Canonical control PR535 merged at2808734c34b7189aef41630ab9fcf6f0b79d958a,
blob957f0e507493768ef17ab26d90bf48b5f13cd9ea. Implement only the optional
independent headless host kit, actual native/host execution, jobs/provider/clock/
telemetry/lifecycle tests and installed guide. Preserve production, SDK inventory,
ABIs and all prior source/evidence bindings. All32 target checks remain required.
Standing scoped publication/integration authority persists. Do not activate
M25-06; Unreal, hardware/RT and release remain separate.

## M25-06 activation (2026-09-28)

M25-05 merged as PR273 at98ecca5a6572dd83d59e26063716f6d76b7a5d4f.
Canonical control PR541 merged atd385731eb44762e73596e830d4f99a8534bc5bf5,
blobac12e901ec4c12d945d83beda7535d5e84defc22. Implement only installed docs, reproducible
source-linked API reference, public-SDK recipes and archive/transcript/link checks.
Preserve production, SDK/ABI and prior sources/evidence. All32 target checks remain
required. Standing scoped publication/integration authority persists. Close M25
software only; human first-use, physical/RT/performance/Unreal/release stay separate.
Do not activate M26. Earlier activation paragraphs are historical.

## M26-01 scenario contract activation

The owner requested M26-01. Canonical control PR544 merged at 3850342958f8daa9607c9ba2e5f82e49efbe40d6.
Freeze and validate the sample-owned golden scenario/lever specification only.
Preserve all production, ABI, SDK, earlier source kits and evidence, and require
all32 exact-head target gates. Standing scoped publication/integration authority
persists. M26-02 through M26-06 remain inactive; M25 human acceptance is
UNPERFORMED. Do not claim golden execution, hardware/RT, Unreal or release.

## M26-02 portable scenario activation

The owner requested M26-02. Canonical control PR546 merged at e7b8b39e7979b4e555567eccd169f5cf05690ee2,
batch blob 327c5c46f47d6749977ffabc3c2837f6afd70fac. Implement only the portable golden CPU/native/host/external
CIL source kit, controls/replay/faults and validation under the active contract.
Preserve immutable M26-01 JSON/hash/checker, production/SDK/ABI and earlier kits.
All32 exact-head target gates and scoped integration authority persist.
M26-03 through M26-06 remain inactive; M25 human/physical/RT/Unreal/release gates
remain separate. Earlier activation sections are historical.

Canonical amendment PR548 merged at ab889ba70d768b73eb9c209d6aaa57f8febe92b0, blob 98cdb422eefd68a9560626c2506277eeb1c2ed74.
Active replay is originating-owner bound; verify same-owner trusted replay,
explicit cross-owner rejection and fresh-owner paired-checkpoint recovery.
Preserve Runtime and the frozen M26-01 scenario without artifact identity rewriting.

## M26-03 CUDA physics activation

The owner requested the next batch after M26-02 merged target PR276 at
dbbab7a433a01459ed737cbb19d3b1a64a3ea1bf with all32 checks. Canonical control
PR550 merged at 0823458c29939d50efb58b16cf3c984ff82e47e8,
blob 773c288349c57a47860481edd5c78f1bd9f68b59. Implement only the bounded CUDA golden
physics variant, simulator-only replay adapter, faults/recovery, installed kit
and optional real-host compile scope. Preserve production/SDK/ABI, immutable
scenario and all prior tests/kits/evidence. Native CUDA remains nondeterministic;
no physical replay claim. All32 target gates and scoped integration authority
persist; M26-04 through M26-06 and human/physical/RT/Unreal/release stay separate.
