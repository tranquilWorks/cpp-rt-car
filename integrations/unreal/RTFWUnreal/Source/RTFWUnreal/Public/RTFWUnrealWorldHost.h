#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"
#include <rt/runtime.hpp>
#include <rtfw_unreal/worlds.hpp>

class UWorld;

struct FRTFWUnrealWorldRecipe
{
    rt::RuntimeConfig Configuration;
    rt::Status (*Configure)(void*, rt::Runtime&) noexcept = nullptr;
    void* Context = nullptr;
    // Optional ownership transfer. OpenWorld consumes the recipe, releasing it
    // only after successful cleanup (also on terminal setup failure).
    void (*Release)(void*) noexcept = nullptr;
};

class RTFWUNREAL_API FRTFWUnrealWorldHost final
{
public:
    using FFactory = rt::Status (*)(void*, UWorld*, FRTFWUnrealWorldRecipe&) noexcept;
    using FObservation = void (*)(void*, UWorld*, rt::Status, const rt::StepResult&) noexcept;
    FRTFWUnrealWorldHost();
    ~FRTFWUnrealWorldHost();
    FRTFWUnrealWorldHost(const FRTFWUnrealWorldHost&) = delete;
    FRTFWUnrealWorldHost& operator=(const FRTFWUnrealWorldHost&) = delete;

    // Game-thread only. Bind before creating worlds, or explicitly OpenWorld
    // for an existing world. Factory/observer code must outlive checked Stop.
    rt::Status Bind(FFactory Factory, void* Context,
                    FObservation Observation = nullptr) noexcept;
    rt::Status OpenWorld(UWorld* World, FRTFWUnrealWorldRecipe Recipe,
                         rtfw_unreal::WorldHandle& Output) noexcept;
    rt::Status TickWorld(rtfw_unreal::WorldHandle World, double DeltaSeconds,
                         rt::StepResult* Result = nullptr) noexcept;
    rt::Status CloseWorld(rtfw_unreal::WorldHandle World) noexcept;
    // Nonblocking checked cleanup. Repeated Stop retries pending owners.
    rt::Status Stop() noexcept;
    rt::Status RetryCleanup() noexcept;
    std::size_t LiveWorlds() const noexcept;
    rt::Status LastFailure() const noexcept;
    static uint32 ActiveHosts() noexcept;

    rt::Status RegisterModule(void* Owner, rtfw_extension_entry_fn_v1 Entry,
                              rtfw_unreal::ModuleHandle& Output) noexcept;
    rt::Status AttachModule(rtfw_unreal::WorldHandle World,
                            rtfw_unreal::ModuleHandle Module) noexcept;
    rt::Status ReleaseModule(rtfw_unreal::ModuleHandle Module) noexcept;
    // Split preparation is needed to attach extensions before finalization.
    rt::Status PrepareWorld(UWorld* World, FRTFWUnrealWorldRecipe Recipe,
                            rtfw_unreal::WorldHandle& Output) noexcept;
    rt::Status StartWorld(rtfw_unreal::WorldHandle World) noexcept;
private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};

// Selected trusted C extension libraries use the same checked ownership path.
// This is explicit close/reopen reload, not arbitrary UE Live Coding patching.
class RTFWUNREAL_API FRTFWUnrealExtensionLibrary final
{
public:
    explicit FRTFWUnrealExtensionLibrary(FRTFWUnrealWorldHost& InHost) noexcept;
    ~FRTFWUnrealExtensionLibrary();
    FRTFWUnrealExtensionLibrary(const FRTFWUnrealExtensionLibrary&) = delete;
    FRTFWUnrealExtensionLibrary& operator=(const FRTFWUnrealExtensionLibrary&) = delete;
    rt::Status Open(const FString& AbsolutePath) noexcept;
    rt::Status Close() noexcept;
    // Borrowed function address: discard before successful Close. All
    // callbacks/services must be tracked by this host, not another registry.
    void* FindExport(const TCHAR* Name) const noexcept;
    rtfw_unreal::ModuleHandle Module() const noexcept { return ModuleHandle; }
private:
    FRTFWUnrealWorldHost& Host;
    void* Handle = nullptr;
    rtfw_unreal::ModuleHandle ModuleHandle{};
};
