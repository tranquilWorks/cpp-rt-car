# M26-04 aggregate CLI sanitizer budget correction

Candidate `73a3c3e9a8de1e7c01d9d3f70a8460326abdb398` passed exact-head local quick148/strict152 and29 of32
hosted records. Windows RelWithDebInfo passed167/168 CTests but the unchanged
experimental simcore_all determinism test hit300s; its Debug sibling was
canceled. The hosted TSan job passed121/122 tests, including all25 new C++
XDMA cases and all prior tests. Its sole failure was the new native CLI
aggregate at120.04s after40 short runs, before reporting the three1024-tick
capacity runs. No race diagnostic was reported. Complete failed logs remain.

The unchanged Debug/O0 CLI and Debug/O0 Runtime/native libraries, all fully
TSan-instrumented, completed all43 native CLI cases locally in
785.59s with exact artifact/state/replay/provenance checks.
A temporary entry-marker main calls the unchanged portable_main; its wrapper
retries only diagnosed before-main mapping failures or empty pre-entry SIGSEGV.
Any failure after entry stops. The probe uses non-PIE locally to work around
loader address-layout collisions, not reduced instrumentation/optimization.
All attempts, original logs and wrapper source are retained here.

Only the new native process aggregate under SIM_SANITIZERS receives1020s:
the original120s short-case allowance plus three existing300s child limits.
The process suite, all43 cases, each300s child bound, ordinary120s aggregate,
other process suites, previous tests/timeouts, assertions, workloads, actual
safe8ms and simulator5s watchdog are unchanged. No Runtime/sample/native source
changes. CTest JSON confirms native1020, host120 and external120 under TSan.
This is a functional aggregate harness budget, not a performance claim.

All original functional/source/package/static/sanitizer gates remain applicable
because only this new CTest property changes. A fresh exact-head full148/152
and all32 hosted names/multiplicity must pass before guarded merge. The complete
hosted TSan native CLI test must pass; the failure is not waived. New full
Windows jobs must pass as well; the prior experimental timeout remains a
separate retained finding. M26-05/M26-06 remain inactive and no physical,
RT, human, Unreal, controlled-performance or release qualification is claimed.

## Previous agent risk review

# M26-04 final agent risk review

Local disposition: complete; final-head hosted and guarded integration pending.
This is an agent review, not independent human qualification. Full148+152,
focused37, sanitizer25+25/allocation, static and actual CPack65 SDK/embedding/
negative gates pass. Reviewed source hashes and acceptance map are in
M26-04-final-verification. Native drivers, Runtime, public formats/ABI/defaults,
prior kits/tests and protected CI equal the integrated repair baseline.

Remaining risks: owned simulated drivers do not establish physical safe-state,
latency or RT qualification; a host watchdog bounds simulator scheduling only.
Borrowed resources require successful checked cleanup; failed stops retain them.
Trusted replay is original-owner bound; compatible recovery uses paired artifacts.
Artifact hashes establish consistency, not authenticated physical execution.
Prior scheduler-sensitive baseline Windows/benchmark findings remain separate.

No unresolved scoped source finding remains. Require exact final full/all32 and
clean/head/base/no-unresolved-review/fetched-tree equality before integration,
then record canonical closure. M26-05/M26-06 remain separate inactive scopes.

## Historical draft review

# M26-04 draft checkpoint risk review

Disposition: do not merge. High-risk feature acceptance remains blocked at
finalization. This is an agent review of an incomplete draft, not independent
human acceptance.

The source-scope audit passes: every changed path is authorized; Runtime,
backends, ABI, prior CPU/CUDA kits/tests, benchmark sources and protected CI are
unchanged. The canonical header-description correction is factual and merged
under exact head/base/review/tree guards. Contract verification passes.

The new card simulator and full forwarding adapter compile against the actual
public SDK/native libraries. The decisive finalize-only comparison fails for
four slots and passes for two before any driver initialization or event. No
physical/numerical/replay/safe-state/allocation/runtime-success claim follows.
The temporary two-slot diagnostic is retained solely to localize the failure;
shipping a two-slot golden descriptor would violate the preserved scenario.

The sample remains a prototype: missing variant owners, artifact/replay/restore
plumbing, fault injection/lifecycle coverage, CLI/process packaging and test
integration. Device operation ordering, resource lifetimes, zero allocations,
backend-local timeline behavior and full-state parity require execution after
the bounded repair. Safe transition8ms remains unchanged and untested. All
failures are retained. Existing hosted gates cannot establish feature completion
while the new tests are not integrated. No target merge is authorized by this
review. See M26-04-repair-proposal.md for the proposed prerequisite repair and
remaining work; no production edit is made before a separately approved batch.
