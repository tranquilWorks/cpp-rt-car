# Unreal source integration before environment handover

M32-01 follows the owner's November 3 instruction to finish implementation before
providing an engine, GPU, FPGA, credentials or other environment. It reconciles
the adapter source from draft [238](https://github.com/tranquilWorks/cpp-rt-car/pull/238)
(`4714dc3fecabe15ee748e29aa949b3cc57eeb6ab`) onto main
`838dc8ffe3f4604dccdcd5545239c91a2cc7d1fa` and composes the merged M31 registry.

This is a **source candidate awaiting engine validation**. Earlier draft engine
results do not validate this tree. Actual Unreal compilation, commandlet tests,
PIE, cooking, packaged execution and engine performance are **NOT RUN** here.
The source does not establish M19/CAP-M19 acceptance or global software completion.

## Delivered paths

| Concern | Implementation | Verification available without an engine |
| --- | --- | --- |
| Jobs, allocator, clock, frame mapping | Existing `RTFWUnrealAdapters` from238; Shipping cleanup guards and bounded clock publication strengthened | Existing Runtime regressions; engine-specific code awaits its original ten engine tests |
| World/runtime ownership | `portable/rtfw_unreal/worlds.hpp` wraps existing `rtfw_host::Registry`; instance-owned bindings, cleanup retry, module leases through scheduler retirement | Real Runtime graphs, original host memory/job kits, isolated worlds, negative setup/lifecycle cases and zero-allocation frames |
| Unreal world events | `RTFWUnrealWorldHost`: game-thread init/tick/cleanup, end-frame cleanup retry, pre-exit shutdown | Shared orchestration runs here; five additional actual-engine automation sources are supplied, not executed |
| Extension reload | Selected absolute-path C extension via `FPlatformProcess`; unload only after checked owner release | Eight real native loader cycles and refusal during both service quiescence and host retirement; engine-loader automation awaits runner |
| Sample | Shared integer graph, disjoint per-world state, nested task range, opt-in game module and explicit success/cleanup markers | Identical model executes on the supported Runtime here; packaged engine run awaits runner |
| Real PIE | Separate latent editor test requests two clients in one process, checks independent valid frames, ends play and repeats | Source only; not represented by broadcasting delegates or by giving a UWorld the PIE enum |
| Build/package handoff | `prepare.py`, `run.py`, `scripts/verify-unreal.*` | Source staging, SDK linkage, report rejection and missing-environment tests; actual UBT/UAT remains pending |

## Starting later

No owner input is needed for this source delivery. Once an environment exists,
use the original first validation tuple: Unreal source tag `5.8.1-release`, commit
`71fe36aac5a8df5ccd66c763ffc902b29b6a9c43`, Debian12 Linux x86-64 and bundled
`v26_clang-20.1.8-rockylinux8` Clang/libc++/lld20.1.8. Native Windows and other
engine versions are not silently treated as that tuple. The PowerShell wrapper
runs on Linux PowerShell; it is not a Windows port.

First build/install the exact current RTFW revision with that engine's matching
compiler, standard library, linker and compatible C++ build flags. Follow the
original toolchain recipe in draft238's retained evidence; do not link a default
GCC/libstdc++ archive into the libc++ engine. The runtime static archive must be
`lib/librtfw_runtime.a`. The SDK data area includes the existing host_lifecycle
kit plus this optional Unreal source kit. No compiled Runtime or default SDK
header/target is added by this integration. `RTFW_UNREAL_DATA_ROOT` overrides
`SDK/share/rtfw/integrations` for a custom installed data directory.

Source staging alone can run now with an ordinary installed Linux SDK:

```sh
python3 integrations/unreal/prepare.py stage --sdk-root /absolute/sdk --output /new/stage
```

It records source hashes, runtime archive/header hashes and the exact fifteen
commandlet test names. It explicitly records `engine_validation: NOT_RUN`.
Existing output is refused, so prior logs and evidence cannot be erased.

On the exact engine host, the later validation entry points are:

```sh
bash scripts/verify-unreal.sh --engine-root /absolute/Unreal --sdk-root /absolute/sdk --output /new/dev-run
bash scripts/verify-unreal.sh --engine-root /absolute/Unreal --sdk-root /absolute/sdk --output /new/shipping-run --configuration Shipping --build-only
bash scripts/verify-unreal.sh --engine-root /absolute/Unreal --sdk-root /absolute/sdk --output /new/complete-run --pie --package --run-packaged
```

`--pie` uses the full editor and needs its display/windowing prerequisites. The
commandlet path uses NullRHI and does not claim a PIE session. Development
packaging cooks `/Engine/Maps/Entry`; the opt-in game sample runs120 frames,
checks checksum16320, and requires its checked-shutdown marker before success.
Shipping is compiled separately; no Shipping logging configuration is changed.

Every external command retains argv, full combined output, exit code and failure
or timeout. `run-result.json` distinguishes build-only from selected actual
passes; each fresh output includes a source manifest. Automation validation
requires every expected test exactly once, successful per-test states, consistent
counts and no failed, unfinished or warning result. A nonempty success count
cannot hide a partial run. Missing engine tools fail; they are never mocked or
reported as passed. Build/automation/package errors require correction and fresh
validation; this source delivery promises no zero-fix first build.

## Application integration and lifecycle

Construct `FRTFWUnrealWorldHost` on the game thread. `Bind(factory, context,
observer)` is opt-in and should occur before game worlds initialize. Existing
worlds can be opened explicitly. The factory returns a configuration callback
and context; optional `Release` transfers that context to the host until cleanup
completes. Ordinary recipe setup may allocate. The Runtime graph uses only
copied callback tables, never a borrowed UWorld in executor callbacks.

`PrepareWorld` attaches jobs, clock and the three-region FMemory provider, then
calls the recipe while configuring. It consumes a recipe on normal game-thread
entry, including early invalid/capacity failures. Rejected off-thread or reentrant
calls do not consume it. `OpenWorld` additionally finalizes/starts. An issued
handle may remain a cleanup token after a failed setup. Call `CloseWorld` or
`Stop` and retain every owner until success. Recipe context/release code must
not be shared with an already-live recipe if the new recipe transfers ownership.

Use `PrepareWorld`, `AttachModule`, then `StartWorld` for selected extensions.
Libraries and their host have explicit lifetime order: worlds close first,
libraries close second, host object destruction last. Library export pointers
are borrowed and must be discarded before successful close. The module lease
covers only this host's tracked callbacks/services, not arbitrary DLL threads or
another registry. Explicit extension close/reopen is supported by the source;
arbitrary Live Coding code patching during active callbacks is not.

Each host controls at most8 worlds and8 extension modules with8 attachments per
world. World/module identities are generation checked. Per-world clock, allocator
and task storage remain alive after Runtime stop until engine task nodes retire.
Failed cleanup closes admission and retains owners. Other worlds still close.
World identity pointers are not dereferenced after cleanup notification; recipe
state must itself be independent of UWorld destruction. End-frame retries are
bounded single attempts; pre-exit shutdown has an explicit five-second control
budget and fails closed rather than unloading live code. Destructors never hide
unresolved ownership; Shipping also enforces these conditions.

The plugin module is passive until a host is created. Dynamic module reload is
refused while any world-host object or independent job/memory/clock adapter exists, even an idle one. The sample game
module refuses dynamic reload while its host exists. Stop/destroy the host and
release all independent adapter owners before any explicit plugin reload. The
plugin does not promise safety against forced unload or engine teardown that
ignores its module-lifetime contract.

Only the three existing resident regions use FMemory; allocator extent is not
proof of physical commitment, residency, pinning, NUMA or whole-process memory.
Engine scheduler allocations remain host behavior. The D-009 exact-version
LowLevelTasks exception from238 is retained; no additional private engine API is
introduced. Runtime defaults, C ABI8/70 exports/SONAME8, device/extension ABI1,
release1.2.1 and qualification/support matrices remain unchanged.

## References and remaining acceptance

The new source uses Epic's documented
[FWorldDelegates](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FWorldDelegates),
[IModuleInterface](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/IModuleInterface),
[third-party library integration](https://dev.epicgames.com/documentation/unreal-engine/integrating-third-party-libraries-into-unreal-engine)
and [RequestPlaySession](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/UEditorEngine/RequestPlaySession)
contracts. Exact engine headers/source and builds still determine compatibility.
No Epic engine source is copied into this repository.

Original M19-02/03/04 remain acceptance gates for real engine builds, job/memory/
clock behavior, actual editor/PIE/world/module lifetimes and packaged execution.
Physical CUDA/XDMA/combined, controlled performance, RT/endurance, independent
human review and production signing/release/deployment remain separate. Excluded
cyber criteria remain unsatisfied. This batch requests no environment or keys.
