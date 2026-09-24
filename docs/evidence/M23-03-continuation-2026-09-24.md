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

## Expanded fixture checkpoint

The original 78 cases now pass 77/78 after integrated v2 selection. Added six
cases retain every original geometry and bring the sorted catalog to 84:
two separately measured shedding action/counter reads; inclusive and overflowing
cross-rate selection compilation (first and repeating supercycles); and two
device error/timeout nonpublication lifecycles. Focused verification exercises
all 84 rows for two warm-ups plus five measurements: **83 pass, one fails**.

The remaining failure is `composition-fault-8`. Public trace inspection locates
frame 5's failed settlement; a CPU-only public reproducer independently shows
that watchdog events at cap 1 record 0→1 instead of 1→1, and at cap 3 record 2→3
instead of 3→3. Active replay then returns `incompatible_artifact`. Source
inspection identifies the inferred `after-1` prior level in `Runtime::step`.
Restoring the borrowed fixture clock did not fix it and was not adopted.

`tests/benchmark_fixtures/runtime_consumer/watchdog_replay_regression.cpp` uses
no benchmark framework, backend or private Runtime API. Compile with:

```sh
g++ -std=c++20 -O1 -pthread -Irt/include -Icore/include -Iinclude \
  tests/benchmark_fixtures/runtime_consumer/watchdog_replay_regression.cpp \
  build/m23-runtime/librtfw_runtime.a -o /tmp/watchdog-replay-regression
/tmp/watchdog-replay-regression
```

Baseline exit 1: cap 1 replays two frames before mismatch sequence 3; cap 3
replays four frames before mismatch sequence 7. Independent action checks also
fail every saturated frame. Control PR #481 defines separate M23-03R3 repair
scope, merged at `19eaf9eb45434584c3b5aecb1976a97d4e626dd8`. It preserves exact
transcript/full-state checks and public layouts. No Runtime source is changed
in this benchmark checkpoint.

Other additions now locally verified:

- Exact mailbox accepted/invalid/full/stale/sequence deltas, invalid host and
  compiled-rate targets, copied payloads and original replacement/rollback.
  A foreign-generation handle is not attributed to a mailbox; an out-of-order
  record with a valid handle separately checks its stale counter.
- Well-formed foreign identity rejection, corruption/truncation nonmutation
  across ordinary, active and live replay; configured 64-input reject-next;
  state registration overflow and domain/substep reject-next boundaries.
- Public mixed/live action and 1 GiB storage/artifact setters at inclusive and
  reject-next bounds; 262144 compiled selections without dispatch, and rejection
  at 262656. No large format cap is labeled executed occupancy.
- Device error and held-completion timeout leave sampled sequence and accepted
  frames unchanged, execute no consumer copies and retain ownership until
  checked stop returns with zero outstanding work. The timeout lifecycle uses
  the existing device-service host-monotonic 1 ms deadline, disclosed in metadata;
  it asserts terminal status, not elapsed duration or physical throughput.
- Fifty allocation-free invoke cases (including eight loopback frame cases and
  two inspection cases), seven invocations each, with global allocation tracking
  covering Runtime worker/service callbacks: exit 0.
- Compiler/FMA and GCC/Clang/MSVC package artifact upload/download/equality
  wiring now includes Runtime fixtures alongside both unchanged predecessors.
  The new hosted exchange is not yet verified.

CI run 36016148112 on checkpoint 0a8bbe9 reproduces composition-fault-8 on GCC
and MSVC. Clang 14 additionally lacks std::source_location with its libstdc++
version; the benchmark diagnostic helper now has a compiler-builtin fallback.
The original failure logs remain retained. Local strict rebuild passes; hosted
confirmation of the fallback remains pending.

Initial new-fixture checks exposed incorrect fixture assumptions, corrected
without Runtime changes: telemetry counters are cumulative across checkpoint
restore; selection storage includes first and repeating supercycles; excess
input records return invalid_argument; stale foreign handles have no mailbox
counter owner. The first expanded focused/allocation failures remain in logs;
the corrected all-case run fails only the unchanged composition case.

Full profile, expanded package chain, sanitizers, final hosted checks and the
acceptance audit remain pending after separate watchdog repair integration.
M23-04/M23-05 remain separate. No cybersecurity scope, Daybreak, Unreal,
hardware/RT qualification, support promotion, signing, release or deployment
work belongs to this PR.
