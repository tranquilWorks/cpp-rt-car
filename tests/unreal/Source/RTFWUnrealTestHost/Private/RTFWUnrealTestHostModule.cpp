#include "Modules/ModuleManager.h"
#include "RTFWUnrealWorldHost.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include <rtfw_unreal/sample.hpp>
#include <array>
#include <exception>

class FRTFWUnrealTestHostModule final : public IModuleInterface
{
    struct FState {
        UWorld* Owner = nullptr;
        rtfw_unreal::Sample Model;
        bool bFinished = false;
    };
    // Recipe state precedes and outlives the host. No UObject is dereferenced
    // from an executor callback; each world has disjoint integer state.
    std::array<FState, 8> States{};
    TUniquePtr<FRTFWUnrealWorldHost> Host;
    bool bFailed = false;
    uint64 ExpectedFrames = 120;
    static rt::Status MakeRecipe(void* Context, UWorld* World,
                                  FRTFWUnrealWorldRecipe& Recipe) noexcept {
        auto& Self = *static_cast<FRTFWUnrealTestHostModule*>(Context);
        for (auto& State : Self.States) {
            if (State.Owner) continue;
            State = {}; State.Owner = World;
            Recipe.Configuration = rtfw_unreal::configuration();
            Recipe.Context = &State;
            Recipe.Configure = [](void* Opaque, rt::Runtime& Runtime) noexcept {
                return rtfw_unreal::Sample::configure(&static_cast<FState*>(Opaque)->Model, Runtime);
            };
            Recipe.Release = [](void* Opaque) noexcept {
                auto& Released = *static_cast<FState*>(Opaque);
                UE_LOG(LogTemp, Display, TEXT("RTFW_SAMPLE world closed frames=%llu checksum=%lld valid=%d"),
                    static_cast<unsigned long long>(Released.Model.frames),
                    static_cast<long long>(Released.Model.checksum), Released.Model.matches() ? 1 : 0);
                Released.Owner = nullptr;
            };
            return rt::Status::ok;
        }
        return rt::Status::resource_exhausted;
    }
    static void Observe(void* Context, UWorld* World, rt::Status Status,
                         const rt::StepResult&) noexcept {
        auto& Self = *static_cast<FRTFWUnrealTestHostModule*>(Context);
        for (auto& State : Self.States) {
            if (State.Owner != World) continue;
            if (Status != rt::Status::ok || !State.Model.matches()) {
                Self.bFailed = true;
                UE_LOG(LogTemp, Error, TEXT("RTFW_SAMPLE FAIL status=%d"), static_cast<int>(Status));
                FPlatformMisc::RequestExitWithStatus(false, 1); return;
            }
            if (!State.bFinished && State.Model.frames >= Self.ExpectedFrames) {
                State.bFinished = true;
                UE_LOG(LogTemp, Display, TEXT("RTFW_SAMPLE PASS frames=%llu checksum=%lld"),
                    static_cast<unsigned long long>(State.Model.frames),
                    static_cast<long long>(State.Model.checksum));
                // Automatic exit is opt-in, so editor PIE worlds continue.
                if (FParse::Param(FCommandLine::Get(), TEXT("RTFWSampleExit")))
                    FPlatformMisc::RequestExitWithStatus(false, 0);
            }
        }
    }
public:
    void StartupModule() override {
        if (!FParse::Param(FCommandLine::Get(), TEXT("RTFWSample"))) return;
        FParse::Value(FCommandLine::Get(), TEXT("RTFWSampleFrames="), ExpectedFrames);
        if (!ExpectedFrames || ExpectedFrames > 1000000) {
            UE_LOG(LogTemp, Error, TEXT("RTFW_SAMPLE invalid frame count"));
            FPlatformMisc::RequestExitWithStatus(false, 2); return;
        }
        FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("RTFWUnreal"));
        Host = MakeUnique<FRTFWUnrealWorldHost>();
        if (Host->Bind(&MakeRecipe, this, &Observe) != rt::Status::ok) std::terminate();
    }
    void ShutdownModule() override {
        if (!Host) return;
        const double Deadline = FPlatformTime::Seconds() + 5.0;
        auto Status = Host->Stop();
        while (Status != rt::Status::ok && FPlatformTime::Seconds() < Deadline) {
            FPlatformProcess::SleepNoStats(0.0f); Status = Host->Stop();
        }
        if (Status != rt::Status::ok) std::terminate();
        Host.Reset();
        UE_LOG(LogTemp, Display, TEXT("RTFW_SAMPLE shutdown failed=%d"), bFailed ? 1 : 0);
    }
    bool SupportsDynamicReloading() override { return !Host; }
};

IMPLEMENT_PRIMARY_GAME_MODULE(
    FRTFWUnrealTestHostModule,
    RTFWUnrealTestHost,
    "RTFWUnrealTestHost")
