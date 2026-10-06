# Engine-independent host lifecycle integration

The [installed source kit](../integrations/host_lifecycle/README.md) completes the
portable ownership and time-conversion layer needed by an embedding host. It
uses the existing public Runtime and extension ABI; the Runtime implementation,
stable C ABI v8, device/extension ABI v1 and default installed SDK targets/headers
are unchanged. Only source/data files are added under
`share/rtfw/integrations/host_lifecycle`.

## Entry points

`Registry<Worlds, Modules, Bindings>` owns fixed world slots containing Runtime
objects. `create_world()` configures a Runtime with either its ordinary clock or
an explicitly borrowed `RuntimeClock`. `configure_world()` supplies a bounded
control callback for existing job/provider/graph APIs. `attach()` registers an
extension from an explicitly registered module, commits one module reference
only after successful Runtime registration, and preserves caller output on
rejection. `finalize()`, `start()` and `step()` forward the real public API.

World and module handles contain process-local registry identity, slot and
generation. Duplicate host-world identities, duplicate module owner/entry,
foreign/stale handles, reentry and capacity exhaustion fail before publication.
No generation wraps. The test's shortened generation limit proves retirement;
normal limits are uint64 maximum. All calls belong to one serialized host control
and frame thread. Symbols and handles are not serialized artifact identities.

`close_world()` first closes admission. For a configuring/unstarted world it
destroys the Runtime before retiring its module references. For finalized or
started worlds, it requires successful checked stop and each actual extension
detach, retaining unresolved bindings after an error. `close_all()` continues
independent cleanup and returns the first failure. It never turns a failed
cleanup into readiness. Reopening creates a new world generation.

`Library<Registry>` opens an absolute, explicitly selected trusted module using
the host platform loader, resolves the extension-v1 entry and registers its owner
with that Registry. Its `close()` refuses to unload while any world binding is
live. After successful release, it checks the real operating-system unload result.
There is no experimental plugin loader or implicit destructor unload. Application
code must route the module's complete lifetime through this wrapper and report
all outstanding owners through extension services. It cannot establish a global
lease against code operating outside this registry.

`ticks_to_ns()` and `frame_from_ticks()` provide checked integer conversion with
explicit rounding, frequency/overflow/signed-duration limits and preserved output
on rejection. Optional deadline and nominal release share the injected clock's
domain. Host-driven frames do not pace, and this kit implements no periodic wait.

## Distribution and verification

Use the standalone install/`find_package` transcript in the kit README. The
package test installs the complete current build, moves the prefix, copies the
installed kit to an independent consumer directory, verifies every source digest,
and builds/runs against the relocated SDK. Neither source-private include paths
nor an in-tree target are available to that consumer.

The real C shared-module example exercises eight cycles, two isolated worlds,
sixteen frames, a deliberate first-world quiescence failure, independent second-
world cleanup, refused unload, closed frame admission, successful retry, retired
handles and actual unload/reload. Contract tests additionally cover output
preservation, capacities, duplicates, foreign handles, generation retirement,
failed registration, graph finalization, service initialization, frame callbacks,
request-stop/shutdown failures, first-error preservation, reentry, arithmetic
boundaries and allocation controls. The existing host-adapter sanitizer build
targets depend on the new binaries; original tests and timeouts remain unchanged.

Exact source-bound full local, all original hosted checks, installed consumer,
compatibility/risk and guarded integration results are retained in
[M31 evidence](evidence/M31-01-2026-10-06.md) and the canonical closing records.

## Remaining integration

Existing [host job/provider sample](host_adapter.md), native
[telemetry kit](../integrations/telemetry/README.md), HAL authoring, shared-memory
CIL, migration and portable golden samples remain delivered. This kit adds the
missing reusable world/module ownership layer and checked engine-time mapping
without duplicating them.

Original M19-02/03/04 remain open: actual Unreal jobs/FMemory/clock bindings,
world/PIE/multiple-instance/unload automation and the complete editor/packaged
sample require the approved licensed engine environment. Portable ownership
tests cannot close those original engine cards. The
[remaining-batch register](remaining_batches.md) retains actual hardware/RT,
controlled performance/endurance, independent human and production obligations.
Cyber remains `excluded_by_owner_unsatisfied`; global software-complete and
CAP-M20-complete claims remain false.
