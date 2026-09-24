# M23-03 continuation after integrated replay v2

Status: active implementation; PR #254 is not merge-ready.

Target main: `52fbdd3b029f1531f8d5d40af85e3f8e11603571` (replay PR #256).
Queue PR #257 and replay PR #256 each passed all 32 final-head hosted checks.
Canonical amended M23-03: control PR #475,
`b489a1a32e1ddd84f7213bae053114a2f17e79d7`, blob
`a6e1ea91b9e2b93833ea8992b46fdbe347797ecf`. Original planning baseline,
78-case draft, 74/78 and intermediate 76/78 failures remain unchanged in history.

The merge retains integrated Runtime/queue source and combines benchmark and
replay package/test ledgers. Active-contract schema, exact 39-file scope and
repository contract pass. Composition fixtures explicitly select public v2
retention policy 230302 with 4*count+8 admission descriptors and 16*count+64
payload bytes; default-v1 fixtures remain unchanged. Catalog rows disclose these
bounds. No original workload or oracle is removed.

Next verify all 78 cases, then complete late/failed device nonpublication,
separate shedding inspection, invalid-target/counter coverage, replay identity,
configured/absolute capacity boundaries, device callback allocation checks,
compiler/FMA artifact exchange and complete package/CI wiring. Preserve all
functional failures; no cybersecurity scope, Daybreak, Unreal, hardware/RT,
support promotion, signing, release or deployment work belongs to this PR.
