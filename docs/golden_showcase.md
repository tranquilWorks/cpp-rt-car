# Golden benchmark, fault and control showcase

M26-05 supplies an optional source kit around the immutable M26-01 scenario and
the existing CPU, CUDA and XDMA owners. Its scope is portable simulation and
injected-driver protocol evidence. The final CAP-M26 coverage audit belongs to
M26-06. Physical CUDA/XDMA/HIL, RT1/RT2, human acceptance, Unreal, controlled
performance and release/deployment are NOT_RUN.

Build an SDK with `-DRTFW_BUILD_BENCHMARKS=ON`, then run the installed kit:

```sh
python3 /sdk/share/rtfw/examples/golden_showcase/run.py --prefix /sdk --output /new/evidence
```

Custom install data/include directories and paths containing spaces are supported.
The kit requires public `runtime`, `cuda_backend`, `xdma_backend`, and `benchmark`
components. Its source includes immutable sibling kits; provenance hashes every
file. It adds no exported library or default Runtime dependency. Embedding uses
`-DGOLDEN_RTFW_SOURCE=/checkout`; default standalone use requires an installed SDK.
No physical device is automatically probed and no missing component falls back.

The default fake clock generates structural fixtures. `--clock steady` retains
portable host timings with the same work and validation. ProviderV1 metadata is
side-effect free: `golden_showcase --list` does not configure Runtime, start
threads, create files, or load a device. The provider has exactly fifteen frozen
case IDs, version1, two warmups and five retained raw samples. Observation vectors,
process control, artifact encoding and filesystem I/O are off the Runtime lanes.

Each timed invocation includes owner creation, setup, the named workload, its
oracle, checked cleanup and invocation evidence writes. There is no isolated
callback, kernel-only, device-time or controlled-performance claim. CPU phase
experiments use a distinct one-callback Runtime graph and explicit frozen
numerical/codec operations; they are not alternative full golden scenarios.

| Case | Actual timed workload |
| --- | --- |
| golden-input | Seeded full-capacity input SoA and initial command copy |
| golden-physics | Integer integration through public task dispatch |
| golden-stage | Full position/velocity host-frame encoding and sealing |
| golden-sensor | Checked application-frame decode and calibration |
| golden-controller | Saturating integer local controller |
| golden-actuator | Actuator application-frame encoding; no HAL safety claim |
| golden-aggregate | Six-field reduction with independent sums |
| golden-telemetry | Complete CPU loop and every trace/action drain |
| golden-host | Complete CPU loop on independent host jobs |
| golden-memory | Configure/finalize/start, MemoryPlan and provider release |
| golden-rates | Inspect all27 reference releases, then execute the loop |
| golden-controls | Actual tick6 replacement control campaign |
| golden-replay | Complete loop capture and originating-owner trusted replay |
| golden-external | Real shared-memory controller process for every invocation |
| golden-loop | Complete loop, telemetry, state oracle and trusted replay |

`golden-loop` additionally measures sim_cuda kernel/Graph, sim_xdma CPU physics,
and sim_combined kernel/Graph, each with native workers and independent host
jobs. These invoke the unchanged complete owners and independently validate their
actual backend counts, source states and original replay transcripts. Five
additional external-process variant runs cover those backends without claiming
that their process orchestration is an isolated benchmark.

The finite lever inventory is read from the frozen contract. Every requested
value executes an applicable configuration; no value is silently clamped.
Settings that change the canonical scenario's hardcoded assumptions are separate
bounded experiments. The full-loop CLI rejects incompatible variants and accepts
only its supported workers/grain settings, never arbitrary tuning flags.

| Lever | Values | Scope and outcome |
| --- | --- | --- |
| workers | 1,2,3 | Full golden loop, fresh owner |
| grain | 1,4,16,64 | Full golden loop, equal entity work |
| rate_multiplier | 1,2,4 | Public single-rate task probe,24 releases at changed periods |
| budget_percent | 25,50,100 | Actual registered budget and admission/reference inspection |
| queue_slots | 4,32,1024 | Same requested task work;4 retains bounded saturation failure |
| scratch_bytes | 0,64,256 | Actual task scratch spans checked by every task |
| device_depth | 1,2 | Public HAL-v2 loopback queue occupancy and excess rejection |
| cuda_graph | 0,1 | Full simulated CUDA golden loop, actual kernel/Graph counts |
| transfer_batch | 1,4,16 | Actual HAL batches, equal64 full256-lane payload copies |
| staging_slots | 2,4 | Actual registered/used host staging pairs, same payload work |
| telemetry_capacity | 0,64,16384 | Actual trace capacity/cursor accounting on the task probe |
| control_burst | 1,16,17 | Real mailbox acceptance, replacement, full rejection and sequence reuse |
| overload_policy | 0,1 | Actual mandatory fail or bounded catch-up limit1 at tick6 |

The HAL lever probes use the public deterministic loopback backend, explicitly
separate from native CUDA/XDMA and the full golden graph. They do not claim that
an unsupported depth or batch size tuned the canonical native device pipeline.
The Runtime probes preserve requested logical work; failed configurations report
the completed prefix and status and are excluded from successful comparisons.
Each record retains requested/effective values, actual counters and Runtime
configuration identity where a Runtime exists; the report binds each record to
source and executable bytes. Zero telemetry capacity is disabled capture. Its
unavailable sequence positions are not reported as measured retention pressure.

All eleven frozen fault IDs execute. The first ten use the preserved owner
campaigns, including real sampled underflow/overrun, unknown safety after missing
ACK, device loss, reset/stop retention and bounded retry. `telemetry_loss` pauses
the consumer at tick6 with a real64-slot trace store; draining at tick12 measures
exact loss and sequence conservation. The showcase replay adapter refuses a
measured gap before invoking Runtime replay. This is an adapter policy, not a
new Runtime promise about trace gaps. Recovery uses a fresh compatible owner,
its own paired checkpoint/replay identity, and exact final-state equality.
Native safe output remains8ms and the explicit simulator host watchdog5s.

The generated `report.json`, `faults.json` and `coverage.json` are recomputed from
all25 benchmark bundles, every warmup/measured invocation, all lever values,
eleven fault executions and five external variants. The unchanged M23 validator
checks bundle schemas/hashes/raw arithmetic. Independent scalar/state checks
reject mutated payloads and false counters; source, executable and configuration
bindings reject stale/foreign evidence. Hashes bind bytes and do not authenticate
who ran them. Verification requires the actual source and executable files:

```sh
python3 report.py /new/evidence --binaries-build build --provenance provenance.json
```

The ordinary and aligned allocation controls cover declared Runtime/task/host
steady execution and replay. Owner setup and report/benchmark orchestration may
allocate. Borrowed owners remain alive through a failed checked stop and explicit
retry. Prior Runtime/native source, schemas, ABIs, fixed budgets, all prior tests
and the 32 exact-head hosted gates remain mandatory and unchanged.
