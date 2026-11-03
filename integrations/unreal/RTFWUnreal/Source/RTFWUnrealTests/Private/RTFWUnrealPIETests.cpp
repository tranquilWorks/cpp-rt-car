#if WITH_EDITOR
#include "RTFWUnrealWorldHost.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UObject/StrongObjectPtr.h"
#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include <rtfw_unreal/sample.hpp>
#include <array>
#include <exception>

namespace {
// Requires the actual editor play loop, not UnrealEditor-Cmd. The disposable
// project's process owns the transient play settings; no user config is saved.
class FRTFWPIECycle final : public IAutomationLatentCommand {
    struct FWorldState { UWorld* Owner = nullptr; rtfw_unreal::Sample Model; };
    FAutomationTestBase* Test;
    std::array<FWorldState, 8> Worlds{};
    TUniquePtr<FRTFWUnrealWorldHost> Host;
    TStrongObjectPtr<ULevelEditorPlaySettings> Settings;
    unsigned Stage = 0, Cycle = 0, Released = 0;
    double Deadline = 0;
    bool Failed = false;
    static rt::Status Factory(void* Opaque, UWorld* World, FRTFWUnrealWorldRecipe& Recipe) noexcept {
        auto& Self = *static_cast<FRTFWPIECycle*>(Opaque);
        for (auto& State : Self.Worlds) {
            if (State.Owner) continue;
            State = {}; State.Owner = World;
            Recipe.Configuration = rtfw_unreal::configuration(); Recipe.Context = &State;
            Recipe.Configure = [](void* Data, rt::Runtime& Runtime) noexcept {
                return rtfw_unreal::Sample::configure(&static_cast<FWorldState*>(Data)->Model, Runtime);
            };
            // Released states remain until the next cycle so assertions can
            // inspect every world's final model after editor teardown.
            return rt::Status::ok;
        }
        return rt::Status::resource_exhausted;
    }
    void Fail(const TCHAR* Message) { Test->AddError(Message); Failed = true; }
public:
    explicit FRTFWPIECycle(FAutomationTestBase* InTest) : Test(InTest) {}
    ~FRTFWPIECycle() { if (Host && Host->Stop() != rt::Status::ok) std::terminate(); }
    bool Update() override {
        if (!GEditor) { Fail(TEXT("Actual editor is required for PIE automation")); return true; }
        if (Stage == 0) {
            if (GEditor->PlayWorld || GEditor->GetPlaySessionRequest().IsSet()) {
                Fail(TEXT("PIE automation requires an idle disposable editor")); return true;
            }
            Worlds = {}; Released = 0;
            Host = MakeUnique<FRTFWUnrealWorldHost>();
            if (Host->Bind(&Factory, this) != rt::Status::ok) { Fail(TEXT("Bind failed")); return true; }
            Settings.Reset(NewObject<ULevelEditorPlaySettings>(GetTransientPackage()));
            Settings->SetPlayNumberOfClients(2);
            Settings->SetRunUnderOneProcess(true);
            Settings->SetPlayNetMode(PIE_ListenServer);
            FRequestPlaySessionParams Params;
            Params.EditorPlaySettings = Settings.Get();
            Params.GlobalMapOverride = TEXT("/Engine/Maps/Entry");
            Params.bAllowOnlineSubsystem = false;
            GEditor->RequestPlaySession(Params);
            Deadline = FPlatformTime::Seconds() + 60.0; Stage = 1;
            return false;
        }
        if (Stage == 1) {
            unsigned Ready = 0;
            for (const auto& World : Worlds)
                if (World.Owner && World.Model.frames >= 8 && World.Model.matches()) ++Ready;
            if (Host->LastFailure() != rt::Status::ok) Fail(TEXT("Runtime failed during actual PIE"));
            if (Ready < 2 && !Failed && FPlatformTime::Seconds() < Deadline) return false;
            if (Ready < 2) Fail(TEXT("Two actual PIE worlds did not reach eight independent valid frames"));
            Released = Ready;
            GEditor->CancelRequestPlaySession();
            GEditor->RequestEndPlayMap();
            Deadline = FPlatformTime::Seconds() + 30.0; Stage = 2;
            return false;
        }
        if (Stage == 2) {
            (void)Host->RetryCleanup();
            if (GEditor->PlayWorld || Host->LiveWorlds()) {
                if (FPlatformTime::Seconds() < Deadline) return false;
                Fail(TEXT("PIE teardown retained editor worlds or Runtime owners"));
                // Retain owners rather than destroying borrowed callbacks.
                if (Host->Stop() != rt::Status::ok) std::terminate();
            }
            Test->TestTrue(TEXT("PIE worlds closed without model corruption"), Released >= 2);
            if (Host->Stop() != rt::Status::ok) { Fail(TEXT("Checked host stop failed")); return false; }
            Host.Reset(); Settings.Reset();
            if (++Cycle < 2 && !Failed) { Stage = 0; return false; }
            return true;
        }
        return true;
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRTFWUnrealActualPIE, "RTFW.PIE.WorldsAndCleanup",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRTFWUnrealActualPIE::RunTest(const FString&) {
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShareable(new FRTFWPIECycle(this)));
    return true;
}
#endif
