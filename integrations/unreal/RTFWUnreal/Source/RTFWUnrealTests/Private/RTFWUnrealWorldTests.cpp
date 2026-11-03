#include "RTFWUnrealWorldHost.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include <cstring>
#include <rtfw_unreal/sample.hpp>
#include <limits>

namespace {
constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
FRTFWUnrealWorldRecipe Recipe(rtfw_unreal::Sample& Model) {
    FRTFWUnrealWorldRecipe Out;
    Out.Configuration = rtfw_unreal::configuration();
    Out.Configure = &rtfw_unreal::Sample::configure; Out.Context = &Model;
    return Out;
}
bool Close(FRTFWUnrealWorldHost& Host) {
    const double Deadline = FPlatformTime::Seconds() + 5.0;
    auto Status = Host.Stop();
    while (Status != rt::Status::ok && FPlatformTime::Seconds() < Deadline) {
        FPlatformProcess::SleepNoStats(0.0f); Status = Host.Stop();
    }
    return Status == rt::Status::ok;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRTFWUnrealWorldIsolation, "RTFW.M19_03.WorldIsolation",
    Flags)
bool FRTFWUnrealWorldIsolation::RunTest(const FString&) {
    rtfw_unreal::Sample First, Second;
    FRTFWUnrealWorldHost Host;
    UWorld* A = UWorld::CreateWorld(EWorldType::Game, false);
    UWorld* B = UWorld::CreateWorld(EWorldType::PIE, false);
    if (!A || !B) {
        if (A) A->DestroyWorld(false);
        if (B) B->DestroyWorld(false);
        AddError(TEXT("Could not create the isolated UWorld objects")); return false;
    }
    rtfw_unreal::WorldHandle One, Two;
    bool Good = Host.OpenWorld(A, Recipe(First), One) == rt::Status::ok &&
        Host.OpenWorld(B, Recipe(Second), Two) == rt::Status::ok;
    for (uint64 Frame = 0; Good && Frame < 32; ++Frame) {
        Good = Host.TickWorld(One, 0.001) == rt::Status::ok;
        if ((Frame & 1u) == 0) Good = Good && Host.TickWorld(Two, 0.002) == rt::Status::ok;
    }
    TestTrue(TEXT("Actual engine adapters execute disjoint 32/16-frame models"),
        Good && First.frames == 32 && Second.frames == 16 && First.matches() && Second.matches());
    rtfw_unreal::WorldHandle Duplicate;
    TestTrue(TEXT("Duplicate world rejected"), Host.OpenWorld(A, Recipe(First), Duplicate) == rt::Status::invalid_state);
    const auto Cleanup = Close(Host);
    TestTrue(TEXT("Checked complete cleanup"), Cleanup && Host.LiveWorlds() == 0);
    TestTrue(TEXT("Stale world refused"), Host.TickWorld(One, 0.001) == rt::Status::invalid_handle);
    A->DestroyWorld(false); B->DestroyWorld(false);
    return Good && Cleanup;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRTFWUnrealWorldFailure, "RTFW.M19_03.CallbackFailureCleanup", Flags)
bool FRTFWUnrealWorldFailure::RunTest(const FString&) {
    rtfw_unreal::Sample Model;
    FRTFWUnrealWorldHost Host;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!World) return false;
    rtfw_unreal::WorldHandle Handle;
    const bool Opened = Host.OpenWorld(World, Recipe(Model), Handle) == rt::Status::ok;
    Model.fail = true;
    TestTrue(TEXT("Callback failure retained"), Opened &&
        Host.TickWorld(Handle, 0.001) == rt::Status::callback_failed && Host.LastFailure() == rt::Status::callback_failed);
    TestTrue(TEXT("No further frame admission after callback failure"), Host.TickWorld(Handle, 0.001) == rt::Status::invalid_state);
    const bool Clean = Close(Host);
    TestTrue(TEXT("Failure cleanup succeeds"), Clean);
    World->DestroyWorld(false); return Opened && Clean;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRTFWUnrealDelegateLifecycle, "RTFW.M19_03.DelegateLifecycle", Flags)
bool FRTFWUnrealDelegateLifecycle::RunTest(const FString&) {
    struct FContext { rtfw_unreal::Sample Model; unsigned Released = 0; } Context;
    FRTFWUnrealWorldHost Host;
    TestTrue(TEXT("Bind world delegates"), Host.Bind([](void* Opaque, UWorld*, FRTFWUnrealWorldRecipe& Out) noexcept {
        auto& State = *static_cast<FContext*>(Opaque);
        Out = Recipe(State.Model); Out.Context = &State;
        Out.Configure = [](void* Value, rt::Runtime& Runtime) noexcept {
            return rtfw_unreal::Sample::configure(&static_cast<FContext*>(Value)->Model, Runtime);
        };
        Out.Release = [](void* Value) noexcept { ++static_cast<FContext*>(Value)->Released; };
        return rt::Status::ok;
    }, &Context) == rt::Status::ok);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!World) return false;
    // Deliberately exercise the delegate routing itself. This is not a PIE
    // session: actual editor PIE/start/end remains a separate runner gate.
    FWorldDelegates::OnPostWorldInitialization.Broadcast(World, UWorld::InitializationValues());
    FWorldDelegates::OnWorldTickStart.Broadcast(World, LEVELTICK_All, 0.001f);
    TestTrue(TEXT("Delegate reached the actual Runtime graph"), Context.Model.frames == 1 && Context.Model.matches());
    FWorldDelegates::OnWorldCleanup.Broadcast(World, true, true);
    const bool Clean = Close(Host);
    TestTrue(TEXT("Recipe released exactly once after checked cleanup"), Clean && Context.Released == 1);
    // Removed handlers must not access the old context or create another owner.
    FWorldDelegates::OnPostWorldInitialization.Broadcast(World, UWorld::InitializationValues());
    TestTrue(TEXT("Stop removes admission delegates"), Host.LiveWorlds() == 0);
    World->DestroyWorld(false); return Clean;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRTFWUnrealRecipeFailure, "RTFW.M19_03.RecipeFailure", Flags)
bool FRTFWUnrealRecipeFailure::RunTest(const FString&) {
    unsigned Released = 0;
    FRTFWUnrealWorldHost Host;
    FRTFWUnrealWorldRecipe Invalid;
    Invalid.Context = &Released;
    Invalid.Release = [](void* Opaque) noexcept { ++*static_cast<unsigned*>(Opaque); };
    rtfw_unreal::WorldHandle Output{99,88,77};
    TestTrue(TEXT("Invalid recipe refused and released"), Host.OpenWorld(nullptr, Invalid, Output) == rt::Status::invalid_argument && Released == 1);
    TestTrue(TEXT("No issued world token on early failure"), Output.registry == 99 && Output.generation == 88);
    rtfw_unreal::Sample Model;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!World) return false;
    const bool Opened = Host.OpenWorld(World, Recipe(Model), Output) == rt::Status::ok;
    rt::StepResult Preserved; Preserved.callbacks_executed = 99;
    TestTrue(TEXT("Invalid engine frame fails closed and preserves output"), Opened &&
        Host.TickWorld(Output, std::numeric_limits<double>::quiet_NaN(), &Preserved) == rt::Status::invalid_argument &&
        Preserved.callbacks_executed == 99 && Host.LastFailure() == rt::Status::invalid_argument &&
        Host.TickWorld(Output, 0.001) == rt::Status::invalid_state);
    const bool Clean = Close(Host);
    World->DestroyWorld(false); return Opened && Clean;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRTFWUnrealExtensionReload, "RTFW.M19_03.ExtensionReload", Flags)
bool FRTFWUnrealExtensionReload::RunTest(const FString&) {
    FString Path;
    if (!FParse::Value(FCommandLine::Get(), TEXT("RTFWTestExtension="), Path)) {
        AddError(TEXT("RTFWTestExtension must name the trusted lifecycle demonstration library")); return false;
    }
    rtfw_unreal::Sample Model;
    FRTFWUnrealWorldHost Host;
    FRTFWUnrealExtensionLibrary Library(Host);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!World) return false;
    bool Good = true;
    for (unsigned Cycle = 0; Good && Cycle < 8; ++Cycle) {
        Model = {};
        Good = Library.Open(Path) == rt::Status::ok;
        if (!Good) break;
        rtfw_unreal::WorldHandle Handle;
        Good = Host.PrepareWorld(World, Recipe(Model), Handle) == rt::Status::ok &&
            Host.AttachModule(Handle, Library.Module()) == rt::Status::ok &&
            Host.StartWorld(Handle) == rt::Status::ok &&
            Host.TickWorld(Handle, 0.001) == rt::Status::ok;
        TestTrue(TEXT("Loaded module cannot close with a live world"), Library.Close() == rt::Status::invalid_state);
        TestTrue(TEXT("Quiescence failure retains world"), Host.CloseWorld(Handle) == rt::Status::invalid_state);
        using Release = void (RTFW_EXTENSION_CALL*)();
        auto* Address = Library.FindExport(TEXT("rtfw_host_demo_release"));
        Release Gate = nullptr;
        static_assert(sizeof(Gate) == sizeof(Address));
        std::memcpy(&Gate, &Address, sizeof(Gate));
        if (!Gate) { AddError(TEXT("Missing demonstration gate export")); std::terminate(); }
        Gate(); Gate = nullptr; Address = nullptr;
        const double Deadline = FPlatformTime::Seconds() + 5.0;
        auto Status = Host.CloseWorld(Handle);
        while (Status != rt::Status::ok && FPlatformTime::Seconds() < Deadline) {
            FPlatformProcess::SleepNoStats(0.0f); Status = Host.CloseWorld(Handle);
        }
        Good = Good && Status == rt::Status::ok && Library.Close() == rt::Status::ok;
    }
    const bool Clean = Close(Host);
    World->DestroyWorld(false);
    TestTrue(TEXT("Eight real engine loader close/reopen cycles"), Good && Clean);
    return Good && Clean;
}
