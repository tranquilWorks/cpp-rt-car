# Migrating a portable RTFW 1.x application

RTFW 1.2.1 remains portable RT0, C ABI v8/SONAME8 and device ABI1.
Recompile C++ consumers; there is no C++ binary ABI promise. The public umbrella
is `<rt/runtime.hpp>`. Prefer `find_package(rtfw 1.2 CONFIG REQUIRED COMPONENTS
runtime)` and link `rtfw::runtime`. Existing `cpp_runtime` and `rtfw::simcore_rt`
remain compatibility names throughout 1.x. The compatibility target additionally
carries experimental legacy pipeline implementation; moving to `runtime` requires
removing reliance on that implementation. Existing deprecated declarations and
`tick_duration` retention are described in [release policy](release_policy.md).

## What is already covered

| Surface | Existing executable evidence | Boundary |
| --- | --- | --- |
| GCC11/Clang14 Ubuntu22.04 and MSVCv143 Windows2022 | required CI matrix and relocated consumers | exact tuples in portable_support_matrix.json only |
| C shared/static and old target aliases | package_consumer C tests, pure-C project, ABI export/fingerprint checks | C ABI8, no older binary archive migration assertion |
| C++ additive aggregate defaults and old pipeline/tick symbols | package_consumer/compat_consumer.cpp and cpp_consumer.cpp | current headers recompiled; broader SimCore remains experimental |
| Default target/header/features and warnings | package_contract.cmake, warning_consumer.cpp | optional CUDA/XDMA/benchmark remain explicitly selected |
| Profiles, active/live-control replay and sampled I/O | existing relocated consumers and schema/refusal tests | schemas and opt-in defaults remain unchanged |
| Native XDMA preparation | M27-07 installed host and absent-device test | full hardware execution/qualification unperformed |

M27-08 adds current installed-package discovery requests for minimum1.0,1.1,1.2
and exact1.2.1, with refusal of minimum1.2.2, major2.0, exact1.1 and an unknown
required component. An unknown optional component does not prevent runtime use.
These test the documented version-selection promise against the actual current
archive. They do not substitute fabricated metadata for historical SDK builds.

The same public consumer is compiled against current and legacy CMake targets.
It uses two isolated Runtime owners, actual checkpoint capture/restore and fixed
reexecution, stops one owner while the other progresses, and checks expected
post-stop refusals and idempotent stop. Each cycle verifies all six provider
allocations are released and zero provider violations remain. This accounts for
three explicit resident regions per owner, not all process heap or OS resources.
The existing sanitizer and no-allocation suites retain their separate coverage.

## Repeat the fixed workload

Build `tests/platform_migration` as a standalone copied consumer against an
extracted SDK, using `-DCMAKE_PREFIX_PATH=<relocated SDK>`. It needs the installed
`examples/golden_system/memory.hpp` reference provider and current public headers.
No source-tree include path or live device is needed. The two executables are
`rtfw_migration_current` and `rtfw_migration_legacy` (`.exe` on Windows).
Run either directly for two cycles, or retain a bounded larger observation:

```sh
python3 tools/portable_soak/run.py --executable /path/to/rtfw_migration_current --cycles 1024 --maximum-seconds 300 --output /tmp/rtfw-soak-new
python3 tests/platform_migration/discovery.py --prefix '/path/to/relocated SDK' --output /tmp/rtfw-discovery
```

The soak output directory must be new. A single process repeats a fixed sequence;
there are no generated inputs, daemon, schedule or automatic retries. Limits are
1..10000 cycles and at most3600 seconds. Each completed cycle reports193 callbacks,
19300 logical nanoseconds, two owners, six acquired/released regions, one local
checkpoint reexecution and two expected state refusals. Monotonic elapsed wall
seconds are measured separately; logical nanoseconds are not endurance duration.

`journal.jsonl` records start and terminal dispositions, `stdout.log` retains every
cycle, and `stderr.log` retains diagnostics. `report.json` binds executable/runner
and log hashes, exact arguments, completed cycles and measured time. Timeout,
interruption, launch error, nonzero return, missing/extra/malformed rows and
unbalanced resources cannot pass. Child termination is followed by wait, with a
five-second graceful-stop allowance before forced cleanup. Log size is observed
every20ms with an8MiB threshold; an observed failure log may exceed that threshold.
A hard-killed parent can leave only a start record; that is incomplete evidence.
Interrupted work is never silently resumed or combined: keep the old directory
and start a new observation. Reports are unsigned observations, not attestations.

## Remaining gates after M27 software delivery

1. Resolve/review the retained experimental concurrent-clock initialization and
   benchmark submission-count assumptions, sampled native startup timing result,
   and M27-07 full-native-frame TSan deadline observation before claims that depend
   on those outcomes. Existing assertions and deadlines remain intact.
2. Original M27-05 generated-input/continuous coverage and M27-06 signature-fixture
   expansion remain unperformed under the owner's narrowed scope. CAP-M20 is not
   promoted by ordinary regression or unsigned rebuild observations.
3. Licensed real Unreal integration and unfamiliar-consumer/human acceptance need
   the actual environment and independent review.
4. Physical CUDA/XDMA/FPGA driver/bitstream behavior, safe outputs, device loss,
   saturation, thermal/endurance and real-time qualification need named approved
   hardware deployments and their original acceptance procedures.
5. Controlled performance measurements, authenticated provenance/production signing,
   release approval, publication and deployment remain separate gates.

No additional M27 software card follows08. This finite list is a readiness handoff,
not global feature/qualification acceptance or authorization to release.
