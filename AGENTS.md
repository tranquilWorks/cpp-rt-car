# RTFW repository instructions

RTFW is an assurance-profile C++20 bounded simulation runtime. The supported
portable product is release 1.2.1 at RT0 with stable C ABI v8, SONAME 8, device
ABI v1, and Apache-2.0. M14, M14.1, M15, M16, and the portable M17 software
path through M17-06 are complete, while hardware/RT qualification remains
incomplete. M20-PRE-01, M21-01 through M21-05, and M22-01 through M22-04 are
merged. M23-01 and M23-02 are merged. M23-03R2 is the active approved repair batch;
CI-QUEUE-01 is merged as PR #257. M23-03 remains an unmerged benchmark draft.
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
