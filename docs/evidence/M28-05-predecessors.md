# M28-05 predecessor integration observations

This reconciliation retains current integration observations in addition to the
immutable original batch evidence. It does not amend or erase those records.

M28-04 target PR297 merged b37c124d6fd7c6ea7dd2b4dffcfc5531b0eb3c8e,
tested head007d1614562f6b313e953019c99ac7294a203414, tree
10247835e6f8f0f57a757587df01c0998cdaa960. Canonical closure PR616 merged
83f442269ea7a823097e171ab969264ca8d0c96c. Exact-head full181/169,
actual installed4/4 and original32 hosted names/multiplicity passed at closure.

Workflow37102403718 first GCC RelWithDebInfo ON job111144363684 failed
CommandBatch.SampledSimulationUnselectedMock: status-2 versus-18,8ms,
entered1,cleanup0/live0. Unchanged retry job111151770421 failed the native
counterpart in m17_command_batch_timeline; mock passed. Each attempt had186/187
passing CTests. Second unchanged retry job111154314407 passed187/187. No
source/assertion/deadline/workflow changed and no repair is inferred. Two GitHub
status-read connection failures were retried separately from functional outcomes.

The M28-04 offline generator macro-renamed original main. Its unused default
branch produced -Wreturn-type; the sole wrapper call uses argc2 and --frame,
whose paths explicitly return. No runtime fallthrough is reachable through that
wrapper and no shipped target is affected. Raw warning/call-path review remains
retained; Clang14 static analysis passed, without a warning-free compile claim.

The canonical M28-02 closure retains the Windows experimental GPUStub.Overlap
assertion with overlap=-43476200ns against a required positive value. Unchanged
retry success and experimental clock repairs do not prove this historical cause.
See the source-bound canonical predecessor snapshot in this batch checkpoint.

Canonical source: tranquilWorks/portfolio-control at
83f442269ea7a823097e171ab969264ca8d0c96c,
products/cpp-rt-car/CODEX_HANDOFF.md and roadmap.yaml. Target source is
tranquilWorks/cpp-rt-car at b37c124d6fd7c6ea7dd2b4dffcfc5531b0eb3c8e.
Actual raw closure/check/job receipts are retained with hashes in this batch
checkpoint; no historical failure is converted into acceptance.
