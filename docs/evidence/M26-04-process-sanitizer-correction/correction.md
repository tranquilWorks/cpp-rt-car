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
