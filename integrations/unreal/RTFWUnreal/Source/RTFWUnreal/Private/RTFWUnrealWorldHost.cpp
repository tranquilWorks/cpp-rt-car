#include "RTFWUnrealWorldHost.h"
#include "RTFWUnrealAdapters.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include <rtfw_unreal/sample.hpp>
#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <limits>

namespace {
std::atomic<uint32> HostCount{0};
struct FBindings
{
    FRTFWUnrealClock Clock;
    FRTFWUnrealMemoryProvider Memory;
    FRTFWUnrealJobAdapter Jobs;
    explicit FBindings(const rt::RuntimeConfig& Config) noexcept
        : Jobs(Config.worker_count, Config.executor_queue_capacity) {}
    bool valid() const noexcept { return Jobs.IsValid(); }
    rt::RuntimeClock& clock() noexcept { return Clock; }
    rt::Status attach(rt::Runtime& Runtime, const rt::RuntimeConfig& Config) noexcept {
        auto Status = Jobs.Attach(Runtime, Config);
        if (Status == rt::Status::ok) Status = Memory.Attach(Runtime);
        if (Status == rt::Status::ok) Status = Runtime.set_cpu_memory_policy(rtfw_unreal::memory_policy());
        return Status;
    }
    void close_admission() noexcept { Jobs.CloseAdmission(); }
    bool quiescent() noexcept { return Jobs.IsQuiescent() && !Memory.HasLiveAllocations(); }
};
}

struct FRTFWUnrealWorldHost::FImpl
{
    struct FWorld
    {
        UWorld* Owner = nullptr; // Identity only after cleanup notification.
        rtfw_unreal::WorldHandle Handle{};
        FRTFWUnrealWorldRecipe Recipe{};
        uint64 Sequence = 0;
        bool bClosing = false, bFailed = false, bStarted = false;
    };
    rtfw_unreal::WorldsHost<FBindings> Host;
    FRTFWUnrealClock FrameClock;
    std::array<FWorld, 8> Worlds{};
    FFactory Factory = nullptr;
    FObservation Observation = nullptr;
    void* FactoryContext = nullptr;
    FDelegateHandle InitHandle, TickHandle, CleanupHandle, EndFrameHandle, ExitHandle;
    rt::Status Failure = rt::Status::ok;
    bool bBusy = false, bStopping = false;
    struct FGuard {
        bool& Busy;
        explicit FGuard(bool& InBusy) noexcept : Busy(InBusy) { Busy = true; }
        ~FGuard() { Busy = false; }
    };
    bool Available() const noexcept { return IsInGameThread() && !bBusy; }
    void Record(rt::Status Status) noexcept {
        if (Failure == rt::Status::ok && Status != rt::Status::ok) Failure = Status;
    }
    FWorld* Find(rtfw_unreal::WorldHandle Handle) noexcept {
        for (auto& World : Worlds) if (World.Owner && rtfw_unreal::same(World.Handle, Handle)) return &World;
        return nullptr;
    }
    void RemoveAdmission() noexcept {
        FWorldDelegates::OnPostWorldInitialization.Remove(InitHandle); InitHandle.Reset();
        FWorldDelegates::OnWorldTickStart.Remove(TickHandle); TickHandle.Reset();
        FWorldDelegates::OnWorldCleanup.Remove(CleanupHandle); CleanupHandle.Reset();
    }
    void RemoveAll() noexcept {
        RemoveAdmission();
        FCoreDelegates::OnEndFrame.Remove(EndFrameHandle); EndFrameHandle.Reset();
        FCoreDelegates::OnEnginePreExit.Remove(ExitHandle); ExitHandle.Reset();
        Factory = nullptr; Observation = nullptr; FactoryContext = nullptr;
    }
    rt::Status Close(FWorld& World) noexcept {
        World.bClosing = true;
        const auto Status = Host.close_world(World.Handle);
        if (Status != rt::Status::ok) return Status;
        const auto Recipe = World.Recipe;
        World = {};
        if (Recipe.Release) Recipe.Release(Recipe.Context);
        return rt::Status::ok;
    }
    rt::Status Retry(bool bAll) noexcept {
        auto First = rt::Status::ok;
        for (auto& World : Worlds) {
            if (!World.Owner || (!bAll && !World.bClosing)) continue;
            const auto Status = Close(World);
            if (First == rt::Status::ok && Status != rt::Status::ok) First = Status;
        }
        if (bStopping && Host.live_worlds() == 0) RemoveAll();
        return First;
    }
    rt::Status Prepare(UWorld* Owner, FRTFWUnrealWorldRecipe Recipe,
                        rtfw_unreal::WorldHandle& Output) noexcept {
        // Caller has transferred recipe ownership. Never release borrowed
        // callback state while a failed initialization still needs cleanup.
        const auto Refuse = [&](rt::Status Status) noexcept {
            if (Recipe.Release) Recipe.Release(Recipe.Context);
            return Status;
        };
        if (!Owner || !Recipe.Configure || bStopping) return Refuse(rt::Status::invalid_argument);
        for (const auto& World : Worlds)
            if (World.Owner == Owner) return Refuse(rt::Status::invalid_state);
        for (auto& World : Worlds) {
            if (World.Owner) continue;
            rtfw_unreal::WorldHandle Handle;
            auto Status = Host.create(Owner, Recipe.Configuration, Handle);
            if (!Handle.generation) return Refuse(Status);
            World.Owner = Owner; World.Handle = Handle; World.Recipe = Recipe;
            Output = Handle;
            if (Status == rt::Status::ok) Status = Host.configure(Handle, Recipe.Configure, Recipe.Context);
            if (Status != rt::Status::ok) { World.bFailed = true; (void)Close(World); }
            return Status;
        }
        return Refuse(rt::Status::resource_exhausted);
    }
    rt::Status Start(FWorld& World) noexcept {
        if (World.bClosing || World.bFailed || World.bStarted) return rt::Status::invalid_state;
        auto Status = Host.finalize(World.Handle);
        if (Status == rt::Status::ok) Status = Host.start(World.Handle);
        if (Status == rt::Status::ok) World.bStarted = true;
        else { World.bFailed = true; (void)Close(World); }
        return Status;
    }
    rt::Status Tick(FWorld& World, double DeltaSeconds, rt::StepResult* Output) noexcept {
        if (World.bClosing || World.bFailed || !World.bStarted ||
            World.Sequence == std::numeric_limits<uint64>::max()) return rt::Status::invalid_state;
        rt::HostFrameContext Frame;
        auto Status = FRTFWUnrealFrameContext::Make(FrameClock, World.Sequence, DeltaSeconds, {}, {}, Frame);
        if (Status != rt::Status::ok) {
            World.bFailed = true; Record(Status);
            const rt::StepResult Refused{};
            if (Observation) Observation(FactoryContext, World.Owner, Status, Refused);
            return Status;
        }
        rt::StepResult Result;
        Status = Host.step(World.Handle, Frame, &Result);
        if (Status == rt::Status::ok) ++World.Sequence;
        else World.bFailed = true;
        if (Output) *Output = Result;
        if (Observation) Observation(FactoryContext, World.Owner, Status, Result);
        Record(Status);
        return Status;
    }
    void OnInit(UWorld* Owner, const UWorld::InitializationValues) {
        if (!Available() || bStopping || !Owner || !Owner->IsGameWorld() || !Factory) return;
        FGuard Guard(bBusy);
        FRTFWUnrealWorldRecipe Recipe;
        auto Status = Factory(FactoryContext, Owner, Recipe);
        if (Status == rt::Status::ok) {
            rtfw_unreal::WorldHandle Handle;
            Status = Prepare(Owner, Recipe, Handle);
            if (Status == rt::Status::ok) Status = Start(*Find(Handle));
        } else if (Recipe.Release) Recipe.Release(Recipe.Context);
        Record(Status);
    }
    void OnTick(UWorld* Owner, ELevelTick, float DeltaSeconds) {
        if (!Available() || bStopping || DeltaSeconds <= 0.0f) return;
        FGuard Guard(bBusy);
        for (auto& World : Worlds) if (World.Owner == Owner && !World.bClosing && !World.bFailed)
            (void)Tick(World, static_cast<double>(DeltaSeconds), nullptr);
    }
    void OnCleanup(UWorld* Owner, bool, bool) {
        if (!Available()) std::terminate(); // No world teardown during a Runtime callback.
        FGuard Guard(bBusy);
        for (auto& World : Worlds) if (World.Owner == Owner) (void)Close(World);
    }
    void OnEndFrame() {
        if (Available()) { FGuard Guard(bBusy); (void)Retry(false); }
    }
    void OnExit() {
        if (!Available()) std::terminate();
        FGuard Guard(bBusy);
        bStopping = true; RemoveAdmission();
        // Shutdown control path only; no wait is added to an RT frame. Engine
        // task-node retirement can lag checked Runtime completion.
        const double Deadline = FPlatformTime::Seconds() + 5.0;
        auto Status = Retry(true);
        while (Status != rt::Status::ok && FPlatformTime::Seconds() < Deadline) {
            FPlatformProcess::SleepNoStats(0.0f); Status = Retry(true);
        }
        if (Status != rt::Status::ok) std::terminate();
    }
};

FRTFWUnrealWorldHost::FRTFWUnrealWorldHost() : Impl(MakeUnique<FImpl>()) {
    if (!IsInGameThread()) std::terminate();
    HostCount.fetch_add(1, std::memory_order_relaxed);
}
FRTFWUnrealWorldHost::~FRTFWUnrealWorldHost() {
    if (Stop() != rt::Status::ok) std::terminate();
    HostCount.fetch_sub(1, std::memory_order_relaxed);
}
uint32 FRTFWUnrealWorldHost::ActiveHosts() noexcept { return HostCount.load(std::memory_order_relaxed); }
rt::Status FRTFWUnrealWorldHost::Bind(FFactory Factory, void* Context, FObservation Observation) noexcept {
    if (!Impl->Available() || Impl->Factory || Impl->bStopping || !Factory) return rt::Status::invalid_state;
    FImpl::FGuard Guard(Impl->bBusy);
    Impl->Factory = Factory; Impl->FactoryContext = Context; Impl->Observation = Observation;
    Impl->InitHandle = FWorldDelegates::OnPostWorldInitialization.AddRaw(Impl.Get(), &FImpl::OnInit);
    Impl->TickHandle = FWorldDelegates::OnWorldTickStart.AddRaw(Impl.Get(), &FImpl::OnTick);
    Impl->CleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(Impl.Get(), &FImpl::OnCleanup);
    Impl->EndFrameHandle = FCoreDelegates::OnEndFrame.AddRaw(Impl.Get(), &FImpl::OnEndFrame);
    Impl->ExitHandle = FCoreDelegates::OnEnginePreExit.AddRaw(Impl.Get(), &FImpl::OnExit);
    return rt::Status::ok;
}
rt::Status FRTFWUnrealWorldHost::PrepareWorld(UWorld* Owner, FRTFWUnrealWorldRecipe Recipe,
                                            rtfw_unreal::WorldHandle& Output) noexcept {
    if (!Impl->Available()) {
        // No transfer on rejected thread/reentry. Caller still owns the recipe.
        return rt::Status::invalid_state;
    }
    FImpl::FGuard Guard(Impl->bBusy); return Impl->Prepare(Owner, Recipe, Output);
}
rt::Status FRTFWUnrealWorldHost::StartWorld(rtfw_unreal::WorldHandle Handle) noexcept {
    if (!Impl->Available()) return rt::Status::invalid_state;
    FImpl::FGuard Guard(Impl->bBusy);
    auto* World = Impl->Find(Handle);
    return World ? Impl->Start(*World) : rt::Status::invalid_handle;
}
rt::Status FRTFWUnrealWorldHost::OpenWorld(UWorld* Owner, FRTFWUnrealWorldRecipe Recipe,
                                         rtfw_unreal::WorldHandle& Output) noexcept {
    const auto Status = PrepareWorld(Owner, Recipe, Output);
    return Status == rt::Status::ok ? StartWorld(Output) : Status;
}
rt::Status FRTFWUnrealWorldHost::TickWorld(rtfw_unreal::WorldHandle Handle, double DeltaSeconds,
                                         rt::StepResult* Result) noexcept {
    if (!Impl->Available()) return rt::Status::invalid_state;
    FImpl::FGuard Guard(Impl->bBusy);
    auto* World = Impl->Find(Handle);
    return World ? Impl->Tick(*World, DeltaSeconds, Result) : rt::Status::invalid_handle;
}
rt::Status FRTFWUnrealWorldHost::CloseWorld(rtfw_unreal::WorldHandle Handle) noexcept {
    if (!Impl->Available()) return rt::Status::invalid_state;
    FImpl::FGuard Guard(Impl->bBusy);
    auto* World = Impl->Find(Handle);
    return World ? Impl->Close(*World) : rt::Status::invalid_handle;
}
rt::Status FRTFWUnrealWorldHost::Stop() noexcept {
    if (!Impl->Available()) return rt::Status::invalid_state;
    FImpl::FGuard Guard(Impl->bBusy);
    Impl->bStopping = true; Impl->RemoveAdmission(); return Impl->Retry(true);
}
rt::Status FRTFWUnrealWorldHost::RetryCleanup() noexcept {
    if (!Impl->Available()) return rt::Status::invalid_state;
    FImpl::FGuard Guard(Impl->bBusy); return Impl->Retry(false);
}
std::size_t FRTFWUnrealWorldHost::LiveWorlds() const noexcept {
    return Impl->Available() ? Impl->Host.live_worlds() : std::numeric_limits<std::size_t>::max();
}
rt::Status FRTFWUnrealWorldHost::LastFailure() const noexcept { return Impl->Failure; }
rt::Status FRTFWUnrealWorldHost::RegisterModule(void* Owner, rtfw_extension_entry_fn_v1 Entry,
                                              rtfw_unreal::ModuleHandle& Output) noexcept {
    if (!Impl->Available() || Impl->bStopping) return rt::Status::invalid_state;
    FImpl::FGuard Guard(Impl->bBusy); return Impl->Host.add_module(Owner, Entry, Output);
}
rt::Status FRTFWUnrealWorldHost::AttachModule(rtfw_unreal::WorldHandle World,
                                            rtfw_unreal::ModuleHandle Module) noexcept {
    if (!Impl->Available() || Impl->bStopping) return rt::Status::invalid_state;
    FImpl::FGuard Guard(Impl->bBusy); rt::ExtensionHandle Extension;
    return Impl->Host.attach(World, Module, Extension);
}
rt::Status FRTFWUnrealWorldHost::ReleaseModule(rtfw_unreal::ModuleHandle Module) noexcept {
    if (!Impl->Available()) return rt::Status::invalid_state;
    FImpl::FGuard Guard(Impl->bBusy); return Impl->Host.release_module(Module);
}

FRTFWUnrealExtensionLibrary::FRTFWUnrealExtensionLibrary(FRTFWUnrealWorldHost& InHost) noexcept : Host(InHost) {}
FRTFWUnrealExtensionLibrary::~FRTFWUnrealExtensionLibrary() { if (Handle) std::terminate(); }
rt::Status FRTFWUnrealExtensionLibrary::Open(const FString& AbsolutePath) noexcept {
    if (!IsInGameThread() || Handle || AbsolutePath.IsEmpty() || FPaths::IsRelative(AbsolutePath)) return rt::Status::invalid_argument;
    Handle = FPlatformProcess::GetDllHandle(*AbsolutePath);
    if (!Handle) return rt::Status::invalid_argument;
    void* Address = FPlatformProcess::GetDllExport(Handle, TEXT("rtfw_extension_entry_v1"));
    rtfw_extension_entry_fn_v1 Entry = nullptr;
    static_assert(sizeof(Entry) == sizeof(Address));
    std::memcpy(&Entry, &Address, sizeof(Entry));
    const auto Status = Entry ? Host.RegisterModule(this, Entry, ModuleHandle) : rt::Status::invalid_argument;
    if (Status != rt::Status::ok) { FPlatformProcess::FreeDllHandle(Handle); Handle = nullptr; }
    return Status;
}
rt::Status FRTFWUnrealExtensionLibrary::Close() noexcept {
    if (!IsInGameThread() || !Handle) return rt::Status::invalid_state;
    const auto Status = Host.ReleaseModule(ModuleHandle);
    if (Status != rt::Status::ok) return Status;
    FPlatformProcess::FreeDllHandle(Handle); Handle = nullptr; ModuleHandle = {};
    return rt::Status::ok;
}
void* FRTFWUnrealExtensionLibrary::FindExport(const TCHAR* Name) const noexcept {
    return IsInGameThread() && Handle && Name ? FPlatformProcess::GetDllExport(Handle, Name) : nullptr;
}
