#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <rt/runtime.hpp>

RTFWUNREAL_API uint32 RTFWUnrealActiveAdapters() noexcept;

struct FRTFWUnrealJobAdapterStats
{
    uint64 Accepted = 0;
    uint64 Executed = 0;
    uint64 Helped = 0;
    uint64 QueueFull = 0;
    uint64 LaunchRejected = 0;
    uint64 StaleOrDuplicate = 0;
    uint64 Retired = 0;
    uint64 GenerationExhausted = 0;
    uint64 SlotStorageBytes = 0;
    uint32 WorkerCount = 0;
    uint32 Capacity = 0;
    bool bAdmissionOpen = false;
};

class RTFWUNREAL_API FRTFWUnrealJobAdapter final
{
public:
    FRTFWUnrealJobAdapter(uint32 WorkerCount, uint32 Capacity) noexcept;
    ~FRTFWUnrealJobAdapter();

    FRTFWUnrealJobAdapter(const FRTFWUnrealJobAdapter&) = delete;
    FRTFWUnrealJobAdapter& operator=(const FRTFWUnrealJobAdapter&) = delete;

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] rt::Status Attach(
        rt::Runtime& Runtime,
        const rt::RuntimeConfig& Configuration) noexcept;
    void CloseAdmission() noexcept;
    [[nodiscard]] bool IsQuiescent() noexcept;
    [[nodiscard]] rt::HostExecutorAdapter GetAdapter() noexcept;
    [[nodiscard]] FRTFWUnrealJobAdapterStats GetStats() const noexcept;

#if WITH_DEV_AUTOMATION_TESTS
    void RejectNextLaunchForTesting() noexcept;
    void SetNextGenerationForTesting(uint64 Generation) noexcept;
    [[nodiscard]] bool DispatchForTesting(
        uint32 SlotIndex,
        uint64 Generation) noexcept;
#endif

private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};

struct FRTFWUnrealAllocatorStats
{
    uint64 Acquired = 0;
    uint64 Applied = 0;
    uint64 Observed = 0;
    uint64 RolledBack = 0;
    uint64 Released = 0;
    uint64 UnsupportedPolicies = 0;
    uint64 ExtentMismatches = 0;
    uint64 LiveBytes = 0;
    uint32 LiveAllocations = 0;
};

class RTFWUNREAL_API FRTFWUnrealMemoryProvider final
{
public:
    FRTFWUnrealMemoryProvider() noexcept;
    ~FRTFWUnrealMemoryProvider();

    FRTFWUnrealMemoryProvider(const FRTFWUnrealMemoryProvider&) = delete;
    FRTFWUnrealMemoryProvider& operator=(
        const FRTFWUnrealMemoryProvider&) = delete;

    [[nodiscard]] rt::Status Attach(rt::Runtime& Runtime) noexcept;
    [[nodiscard]] rt::MemoryProvider GetProvider() noexcept;
    [[nodiscard]] FRTFWUnrealAllocatorStats GetStats() const noexcept;
    [[nodiscard]] bool HasLiveAllocations() const noexcept;

#if WITH_DEV_AUTOMATION_TESTS
    void FailAcquireAtForTesting(int32 AcquisitionIndex) noexcept;
    void FailApplyAtForTesting(int32 ApplicationIndex) noexcept;
    void FailObserveAtForTesting(int32 ObservationIndex) noexcept;
    void FailRollbackCountForTesting(uint32 Count) noexcept;
#endif

private:
    struct FAllocation
    {
        void* Address = nullptr;
        SIZE_T ExtentBytes = 0;
        SIZE_T LogicalBytes = 0;
        uint32 Alignment = 0;
        rt::MemoryRegionId Region{};
        bool bOwned = false;
        bool bApplied = false;
    };

    static rt::Status Acquire(
        void* Context,
        const rt::MemoryProviderAcquireRequest& Request,
        rt::MemoryProviderAllocation& Allocation) noexcept;
    static rt::Status Apply(
        void* Context,
        void* Token,
        const rt::MemoryPolicy& Resolved,
        rt::MemoryProviderObservation& Applied) noexcept;
    static rt::Status Observe(
        void* Context,
        void* Token,
        const rt::MemoryPolicy& Resolved,
        rt::MemoryProviderObservation& Observed) noexcept;
    static rt::Status Rollback(
        void* Context,
        void* Token,
        const rt::MemoryPolicy& Resolved,
        const rt::MemoryProviderObservation& Applied) noexcept;
    static void Release(
        void* Context,
        void* Token,
        rt::RollbackIntent Intent) noexcept;

    [[nodiscard]] static bool IsActiveRegion(
        rt::MemoryRegionId Region) noexcept;
    [[nodiscard]] static bool IsSupportedPolicy(
        const rt::MemoryPolicy& Policy) noexcept;
    [[nodiscard]] FAllocation* FindFree() noexcept;
    [[nodiscard]] FAllocation* FindToken(void* Token) noexcept;
    [[nodiscard]] const FAllocation* FindToken(void* Token) const noexcept;

    std::array<FAllocation, 3> Allocations{};
    std::atomic<bool> bAttached{false};
    std::atomic<uint64> Acquired{0};
    std::atomic<uint64> AppliedCount{0};
    std::atomic<uint64> ObservedCount{0};
    std::atomic<uint64> RolledBack{0};
    std::atomic<uint64> Released{0};
    std::atomic<uint64> UnsupportedPolicies{0};
    std::atomic<uint64> ExtentMismatches{0};
#if WITH_DEV_AUTOMATION_TESTS
    std::atomic<int32> FailAcquireAt{-1};
    std::atomic<int32> FailApplyAt{-1};
    std::atomic<int32> FailObserveAt{-1};
    std::atomic<uint32> FailRollbackCount{0};
#endif
};

class RTFWUNREAL_API FRTFWUnrealClock final : public rt::RuntimeClock
{
public:
    FRTFWUnrealClock() noexcept;
    ~FRTFWUnrealClock() override;

    [[nodiscard]] std::uint64_t now_ns() noexcept override;
    [[nodiscard]] rt::Status sleep_until_ns(
        std::uint64_t AbsoluteNs) noexcept override;
    [[nodiscard]] bool supports_absolute_sleep() const noexcept override;

    [[nodiscard]] bool TryCyclesToNanoseconds(
        uint64 Cycles,
        uint64& Nanoseconds) const noexcept;
    [[nodiscard]] static bool TrySecondsToNanoseconds(
        long double Seconds,
        bool bRequirePositive,
        uint64& Nanoseconds) noexcept;
    [[nodiscard]] uint64 ConversionFailures() const noexcept;

private:
    double SecondsPerCycle = 0.0;
    std::atomic<uint64> LastNanoseconds{0};
    std::atomic<uint64> FailureCount{0};
};

class RTFWUNREAL_API FRTFWUnrealFrameContext final
{
public:
    [[nodiscard]] static rt::Status Make(
        const FRTFWUnrealClock& Clock,
        uint64 FrameSequence,
        double DeltaSeconds,
        TOptional<uint64> DeadlineCycles,
        TOptional<uint64> NominalReleaseCycles,
        rt::HostFrameContext& Output) noexcept;
};
