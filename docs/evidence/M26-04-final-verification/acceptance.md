# M26-04 local acceptance and remaining integration gates

All feature-local functional gates pass against integrated repair283. Evidence
is portable injected-driver protocol execution, not physical XDMA/CUDA or RT.

| Acceptance | Implementation and verification |
| --- | --- |
| Frozen scenario and prior sources | Golden contract/config/Python checks; scope guard preserves all Runtime/native/ABI and CPU/CUDA sources. |
| Actual native XDMA and combined execution | Owner/session/Io forwards real HAL-v2 batches; native/host/external CPU/kernel/Graph matrix and exact operation/resource counts. |
| Sampled channels and safe output | Four real ring4 sampled descriptors, distinct 120-byte codecs, terminal ACK counters; startup/failure/stop and underflow/overrun/missing ACK. |
| Full numerical parity | Every active/inactive state field against independent CPU bytes and C++/Python oracle; counts1/odd/default/256, workers1/2/3, grains1/4/16/64 and ticks through1024. |
| Honest simulator boundary | Complete owned-driver forwarding adapter; native capabilities/replay rejection; five-second mock watchdog with unchanged logical/backend safe8ms. |
| Replay and recovery | Complete originating-owner trusted replay, compatible fresh-owner paired checkpoint recovery; foreign/CPU/CUDA/cross-variant and checksum-repaired mutations rejected. |
| Faults and lifetime | 27 transport fault variants, missing/duplicate/stale frames, loss/reset, short/timeout/not-ready, two-slot queue/cancel, partial init/shutdown, retention/retry and concurrent owners. |
| Allocation and concurrency | Ordinary/aligned positive controls1; steady and supported replay0 including actual native workers; ASan/UBSan/leaks25 and TSan25. |
| External processes and truthful artifacts | Native43/host31/external11 process suites, independently checked state/actions/channels/operations/resource/provenance and malformed evidence negatives. |
| Complete public SDK source kit | Actual relocated CPack, custom paths/spaces/removed prefix, one-command run, nine variants plus all56 prior SDK consumers=65; embedding and missing/private/source/config/state/cleanup negatives. |
| Static/full compatibility | Clang analysis of new CLI and full fixture; contract/full148+152 and focused37; frozen ABI/export/schema/default and protected workflow checks. |
| Retained diagnostics and risk | Prior repairs/failures remain unchanged; new sanitizer harness note retains120s timeouts and unchanged121.09s full-run proof. No Runtime deadline relaxed. |
| Final integration | Exact final-head full/all32 names/multiplicity, clean/head/base/review and fetched-tested-tree guards still required before target merge; canonical closure afterward. |

The sanitizer-only300s harness applies only to new complete1024-tick cases9/18;
other new/earlier timeouts and all workloads/assertions remain intact. This is
functional instrumentation overhead, not a controlled-performance claim.
M26-05/M26-06 remain inactive. Human, physical/HIL/RT, Unreal, controlled
performance, signing/release and deployment remain separate.
