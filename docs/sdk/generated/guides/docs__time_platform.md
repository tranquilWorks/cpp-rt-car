[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)

> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.

# Time, Watchdog, and Platform Contract

Release 0.6 introduced, and release 1.2 retains, the M5 RT0 time/platform
surface in `rt::Runtime`.
It adds finite self-paced execution, a one-shot frame watchdog, frame-thread
degradation state, and a strict reportable Linux platform preflight. These are
functional contracts, not a hard-real-time or RT2 qualification.

The legacy `SimCore::run()`, `rt::Watchdog`, hardening scripts, and limp-mode
controller are separate experiments and do not inherit this contract.

## Two explicit time modes

Host-driven `Runtime::step()` remains the default embedding operation. The host
supplies a frame index, simulation delta, and optional absolute deadline;
`step()` never paces or sleeps.

`Runtime::run_periodic()` is the separate runtime-paced operation. It executes
a finite number of frames on the calling frame thread. For frame offset `i`,
the release and deadline are:

```text
release(i)  = first_release + i * period
deadline(i) = release(i) + relative_deadline
```

If `first_release` is omitted, the runtime samples its clock once and uses that
value as the epoch. Every wait is an absolute `sleep_until_ns(release(i))`.
A late frame does not move the epoch and no configured frame is skipped:
subsequent late releases execute immediately and may run back-to-back. The
runtime rejects zero counts, nonpositive durations, and any release, deadline,
next-release, or frame-index overflow before the first wait.

A custom `RuntimeClock` must return monotonic nanoseconds in one domain,
implement absolute waiting, and report `supports_absolute_sleep()`. Host-driven
steps remain usable with a clock that cannot wait; periodic execution returns
`clock_failure`.

The M16-01 reference supercycle has a different, relative epoch-zero purpose.
It does not pace the clock, and reference-only plans do not change
`run_periodic()` releases.
For an opt-in M16-03 plan, each periodic release is also the active nominal
timestamp and the positive period is the next half-open logical window; the
absolute cadence remains unchanged by late work.

M16-02 selection ages are exact differences within that relative reference
model, including checked prior-supercycle wrap. They use no runtime clock,
sleep, tolerance, floating point, callback completion order, or wall time.
Reference selection freshness remains immutable and does not change cadence.
Active callback reads report fresh/stale from exact committed generation age.

`PeriodicFrameResult` reports status, frame index, release, wake, start, finish,
signed deadline slack, miss state, watchdog state, and degradation level.
`PeriodicRunResult` reports executed frames, deadline misses, watchdog events,
the final degradation level, the epoch, next release, and last frame. An
optional observer runs synchronously on the frame thread after each attempted
step. Its execution time therefore affects following releases, and host
observer code is outside the runtime's allocation/blocking contract. An
observer failure stops the finite loop with `callback_failed`.

## Watchdog and degradation

`watchdog_timeout_ns == 0` disables the watchdog. A nonzero timeout creates one
runtime-owned service lane in `start()` and joins it in `stop()`. Each step
arms one deadline relative to that step's measured start.
Each arm produces at most one event. This remains true when both the service
clock and the runtime clock observe expiry.

The service lane can mark an overrun while a callback is still running, but it:

- never invokes host code;
- never mutates degradation state;
- cannot preempt, cancel, or terminate a stuck callback.

Once the graph quiesces, the calling frame thread consumes the one-shot result.
If the frame finished at or after the watchdog deadline, it records one event
and increments the runtime-owned degradation level up to
`watchdog_max_degradation_level`. Callbacks in the overrun frame observe the
previous level; callbacks in following frames observe the committed level
through `CallbackContext::degradation_level`.

The watchdog arm/disarm path uses atomics and a notification only. Its mutex
and condition-variable wait are confined to the non-RT service lane. The M4
allocation gate runs complete frames with the watchdog armed.

Degradation is a signal, not automatic workload shedding. The host callback
must select cheaper work when it sees a higher level. There is no recovery or
level-decrease policy in 1.2.

## Strict platform preflight

`platform_preflight_mode` defaults to `disabled`. Disabled mode performs no
probe and does not mutate the host.

In `strict` mode, `start()` runs preflight after finalization and before any
executor or watchdog thread is created. Success requires exactly one passing
result for every prerequisite:

| Check | Linux condition |
| --- | --- |
| Absolute monotonic clock | The runtime clock supports absolute waits and the standard steady clock is steady |
| Realtime kernel | `/sys/kernel/realtime` reports enabled, or `uname` identifies `PREEMPT_RT` |
| Memory-lock limit | `RLIMIT_MEMLOCK` covers the finalized runtime memory plan |
| Locked memory | `VmLck` in `/proc/self/status` covers the finalized runtime memory plan |
| Isolated CPU affinity | The calling thread's affinity mask is nonempty and entirely contained in the kernel isolated-CPU list |
| Realtime scheduler | The calling thread uses `SCHED_FIFO` or `SCHED_RR` with positive priority |

The native probe is read-only: it does not call `mlockall`, change affinity,
raise priority, edit limits, or alter system policy. On unsupported platforms,
Linux-specific checks report `unsupported`, so strict mode fails closed.

A failure returns `platform_preflight_failed`, leaves the runtime finalized,
and starts no runtime threads. The complete fixed-capacity report remains
available through `platform_preflight_report()` or
`rtfw_get_platform_preflight_report()`, including per-check status, system
error, and explanation. C++ tests can inject a `PlatformPreflightProbe` for
deterministic validation.

Passing this preflight does not establish RT2. It does not validate BIOS and
power settings, IRQ placement, driver/device behavior, PCIe topology, thermal
behavior, workload bounds, or measured deadline distributions. Those remain
deployment qualification requirements in the
[product contract](https://github.com/tranquilWorks/cpp-rt-car/blob/98ecca5a6572dd83d59e26063716f6d76b7a5d4f/docs/product_contract.md).

## M15 policy application

The additive C++ CPU/memory policy model inventories the caller frame lane,
runtime-owned executor/watchdog/device-service lanes, and external XDMA/vendor
roles. Linux finalization resolves only process-available affinity, scheduler,
NUMA CPU selection, stack/guard, bounded name, and wait behavior without
mutation. Startup applies and reads back runtime-owned lanes before a shared
gate commits. The caller frame is read back only; host-adapter and vendor lanes
remain external and verify-only. Strict unsupported or mismatched policy fails
closed before callbacks. See [the policy contract](https://github.com/tranquilWorks/cpp-rt-car/blob/98ecca5a6572dd83d59e26063716f6d76b7a5d4f/docs/cpu_memory_policy.md).

M15-03 adds a process-local resident-memory transaction after preflight and
before the thread gate. Exactly phase scratch, task scratch, and trace storage
are applied and observed. Linux can base-page-round mappings and guards,
attempt explicit `MAP_HUGETLB` with only the requested fallback, touch pages on
the caller for prefault/first-touch requests, apply `mlock`, and sample
residency using `mincore`. The existing preflight remains read-only; these
operations occur only after it passes or when it is disabled.

An `mmap`, `mprotect`, touch, `mlock`, or `mincore` result describes only this
process and named host. `mlock` success is not independent lock readback and
never proves CUDA, device, or DMA pinning. Transparent huge-page behavior is
not explicit huge-page allocation. Unsupported owner-thread first touch or
native physical NUMA observation is reported or rejected under strict policy
rather than inferred. In M15-03, a strict runtime-provider NUMA request is
rejected and best effort retains default placement; provider results require
advertised capabilities and independent observation.

M15-04 observes each runtime-owned executor, watchdog, and device-service stack
only while its native mapping is live. Supported Linux stack locking and
`mincore` residency observation participate in the held startup gate. Cleanup
is requested on the owning quiescent lane before join and remains status-
bearing and retryable; unresolved stack ownership blocks later region rollback
and provider release. No custom stack substitution, fixed CPU/NUMA assumption,
privileged host mutation, or timing-only verification is added.

## M16-03 logical and nominal rate time

Active rate execution keeps logical simulation time distinct from the runtime
clock. The first positive host step maps logical epoch zero to its appended
nominal release timestamp. Later windows must be contiguous and equal
`epoch + cursor`; each covers the exact half-open interval
`[cursor, cursor + delta)`. Overflow, zero delta, a missing nominal release, or
a noncontiguous mapping fails before callback dispatch. Host-driven steps never
sleep. `run_periodic()` passes its existing absolute release as the nominal
timestamp, so late work never shifts the periodic epoch.

Immediately before one atomic domain release, the runtime compares the injected
or monotonic clock with the checked absolute deadline. The configured skip,
bounded-catch-up, hold, degrade, or fail action is then applied once to the
whole release. Large late skip/hold prefixes use checked aggregate counts; the
global callback cap and catch-up cap prevent backlog or spill. Fake-clock tests
establish these integer decisions only. They are not measured WCET, latency,
deadline, HIL, field, RT1, or RT2 evidence.

Only a settled mandatory domain release updates the M16-04 consecutive-late or
consecutive-on-time streak. A threshold changes exactly one optional domain;
the change is effective for the next total-order release, even at the same
logical timestamp. Optional lateness keeps its configured late action but does
not drive those streaks. Large omitted windows use checked bounded aggregate
updates and do not replay every omitted supercycle.

Neither requested/resolved/acquired/applied/verified reports, preflight, nor
named-host functional tests establish hardware/HIL behavior, latency, field
results, RT1, RT2, signing, release, deployment, or production validation.
Logical extents and declarations do not establish allocator commitment or
physical residency; `mlock` remains application rather than independent lock
readback.

## C ABI

Stable ABI version 8 retains the M5 surface:

- watchdog and preflight fields in `rtfw_config`;
- `rtfw_run_periodic()` and its initialized config/result records;
- periodic observer results with release/wake/start/finish/slack;
- watchdog and degradation fields in callback and step results;
- initialized fixed-capacity preflight reports;
- `RTFW_STATUS_PLATFORM_PREFLIGHT_FAILED` and
  `RTFW_STATUS_CLOCK_FAILURE`.

All public structures retain size and reserved-field validation. The ABI remains
covered by the M11 compatibility and export policy.

## Evidence and boundaries

- implementation: `rt/src/host_runtime.cpp`,
  `rt/src/memory_policy.cpp`,
  `rt/src/watchdog_monitor.cpp`,
  `rt/src/native_platform_preflight.cpp`;
- fake-clock, no-drift, deadline, watchdog, and degradation tests:
  `tests/test_periodic_runtime.cpp`, `tests/test_rate_dispatch.cpp`;
- fail-closed preflight tests: `tests/test_platform_preflight.cpp`;
- armed-watchdog allocation gate: `tests/test_trace_noalloc.cpp`;
- dynamic C ABI coverage: `tests/test_cabi_dlopen.c`;
- finite C and C++ periodic samples:
  `samples/embed_c/mini_app.c`,
  `samples/embed_cpp/mini_app.cpp`.

## Explicit simulator device deadlines

`DeviceRatePhaseBinding::simulation` is an optional deterministic-mock policy,
not a clock-rate conversion. Its positive host watchdog is independently bounded
to60 seconds; logical periods, phase budgets and backend command timeouts remain
unchanged. The borrowed RuntimeClock must allow concurrent bounded monotonic
reads when this policy is used. Logical deadline expiry is checked before
successful terminal publication. Pending work still expires at the finite host
watchdog if logical time is frozen. Active replay uses recorded decisions and
backend simulator time while retaining that host watchdog. The default and
native backend paths do not select this policy. No RT timing claim follows.
