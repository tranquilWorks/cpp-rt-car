#include "RTFWUnrealAdapters.h"

#include "Async/Fundamental/Scheduler.h"
#include "Async/Fundamental/Task.h"
#include "HAL/PlatformTime.h"
#include "HAL/UnrealMemory.h"

#include <cmath>
#include <new>
#include <exception>

namespace
{
constexpr uint64 StateBits = 3;
constexpr uint64 StateMask = (uint64{1} << StateBits) - 1;
constexpr uint64 MaxGeneration = std::numeric_limits<uint64>::max() >> StateBits;
std::atomic<uint32> AdapterOwners{0};

enum class ESlotState : uint64
{
    Free = 0,
    Reserving = 1,
    Accepted = 2,
    Executing = 3,
    AwaitingRetirement = 4,
};

[[nodiscard]] uint64 Pack(uint64 Generation, ESlotState State) noexcept
{
    return (Generation << StateBits) | static_cast<uint64>(State);
}

[[nodiscard]] uint64 GenerationOf(uint64 Control) noexcept
{
    return Control >> StateBits;
}

[[nodiscard]] ESlotState StateOf(uint64 Control) noexcept
{
    return static_cast<ESlotState>(Control & StateMask);
}

[[nodiscard]] bool IsPowerOfTwo(uint32 Value) noexcept
{
    return Value >= 2 && (Value & (Value - 1)) == 0;
}
}

struct FRTFWUnrealJobAdapter::FImpl
{
    struct FSlot
    {
        LowLevelTasks::FTask Task;
        // One-attempt control lease protects Init/help/reuse from each other.
        // Scheduler execution uses its own task-node lifetime protocol.
        std::atomic<bool> NodeAccess{false};
        std::atomic<uint64> Control{Pack(0, ESlotState::Free)};
        rt::HostExecutorJob Job{};
        uint32 Index = 0;
    };

    uint32 WorkerCount = 0;
    uint32 Capacity = 0;
    TUniquePtr<FSlot[]> Slots;
    std::atomic<bool> bValid{false};
    std::atomic<bool> bAdmissionOpen{false};
    std::atomic<bool> bAttached{false};
    std::atomic<uint64> SearchCursor{0};
    std::atomic<uint64> WorkerCursor{0};
    std::atomic<uint64> NextGeneration{1};
    std::atomic<uint64> Accepted{0};
    std::atomic<uint64> Executed{0};
    std::atomic<uint64> Helped{0};
    std::atomic<uint64> QueueFull{0};
    std::atomic<uint64> LaunchRejected{0};
    std::atomic<uint64> StaleOrDuplicate{0};
    std::atomic<uint64> Retired{0};
    std::atomic<uint64> GenerationExhausted{0};
#if WITH_DEV_AUTOMATION_TESTS
    std::atomic<bool> bRejectNextLaunch{false};
#endif

    FImpl(uint32 InWorkerCount, uint32 InCapacity) noexcept
        : WorkerCount(InWorkerCount), Capacity(InCapacity)
    {
        if (WorkerCount == 0 || WorkerCount > 256 ||
            !IsPowerOfTwo(Capacity) || Capacity > 1'048'576)
        {
            return;
        }
        Slots.Reset(new (std::nothrow) FSlot[Capacity]);
        if (!Slots)
        {
            return;
        }
        for (uint32 Index = 0; Index < Capacity; ++Index)
        {
            Slots[Index].Index = Index;
        }
        bAdmissionOpen.store(true, std::memory_order_release);
        bValid.store(true, std::memory_order_release);
    }

    [[nodiscard]] uint64 AllocateGeneration() noexcept
    {
        const uint64 Generation = NextGeneration.fetch_add(
            1,
            std::memory_order_relaxed);
        if (Generation == 0 || Generation > MaxGeneration)
        {
            GenerationExhausted.fetch_add(1, std::memory_order_relaxed);
            bAdmissionOpen.store(false, std::memory_order_release);
            return 0;
        }
        return Generation;
    }

    void ReclaimCompleted(FSlot& Slot) noexcept
    {
        bool Access = false;
        if (!Slot.NodeAccess.compare_exchange_strong(Access, true, std::memory_order_acquire)) return;
        uint64 Control = Slot.Control.load(std::memory_order_acquire);
        if (StateOf(Control) != ESlotState::AwaitingRetirement ||
            !Slot.Task.IsCompleted(std::memory_order_acquire))
        {
            Slot.NodeAccess.store(false, std::memory_order_release);
            return;
        }
        if (Slot.Control.compare_exchange_strong(
                Control,
                Pack(GenerationOf(Control), ESlotState::Reserving),
                std::memory_order_acq_rel,
                std::memory_order_relaxed))
        {
            Slot.Job = {};
            Slot.Control.store(
                Pack(GenerationOf(Control), ESlotState::Free),
                std::memory_order_release);
            Retired.fetch_add(1, std::memory_order_relaxed);
        }
        Slot.NodeAccess.store(false, std::memory_order_release);
    }

    [[nodiscard]] bool Dispatch(FSlot& Slot, uint64 Generation) noexcept
    {
        uint64 Expected = Pack(Generation, ESlotState::Accepted);
        if (!Slot.Control.compare_exchange_strong(
                Expected,
                Pack(Generation, ESlotState::Executing),
                std::memory_order_acq_rel,
                std::memory_order_relaxed))
        {
            StaleOrDuplicate.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const rt::HostExecutorJob Job = Slot.Job;
        const uint32 LogicalWorker = static_cast<uint32>(
            WorkerCursor.fetch_add(1, std::memory_order_relaxed) % WorkerCount);
        if (Job.execute != nullptr)
        {
            Job.execute(
                Job.execution_context,
                Job.completion_context,
                Job.completion_token,
                LogicalWorker);
        }
        Executed.fetch_add(1, std::memory_order_relaxed);
        Slot.Control.store(
            Pack(Generation, ESlotState::AwaitingRetirement),
            std::memory_order_release);
        return true;
    }

    [[nodiscard]] rt::Status Submit(const rt::HostExecutorJob& Job) noexcept
    {
        if (!bValid.load(std::memory_order_acquire) ||
            !bAdmissionOpen.load(std::memory_order_acquire) ||
            Job.execute == nullptr)
        {
            return rt::Status::invalid_state;
        }

        const uint64 Start = SearchCursor.fetch_add(1, std::memory_order_relaxed);
        for (uint32 Offset = 0; Offset < Capacity; ++Offset)
        {
            FSlot& Slot = Slots[(Start + Offset) & (Capacity - 1)];
            ReclaimCompleted(Slot);
            uint64 Expected = Slot.Control.load(std::memory_order_acquire);
            if (StateOf(Expected) != ESlotState::Free ||
                !Slot.Control.compare_exchange_strong(
                    Expected,
                    Pack(GenerationOf(Expected), ESlotState::Reserving),
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                continue;
            }

            const uint64 Generation = AllocateGeneration();
            if (Generation == 0)
            {
                Slot.Control.store(
                    Pack(GenerationOf(Expected), ESlotState::Free),
                    std::memory_order_release);
                return rt::Status::resource_exhausted;
            }

            bool Access = false;
            if (!Slot.NodeAccess.compare_exchange_strong(Access, true, std::memory_order_acquire))
            {
                Slot.Control.store(Pack(GenerationOf(Expected), ESlotState::Free), std::memory_order_release);
                continue;
            }

            Slot.Job = Job;
            Slot.Task.Init(
                TEXT("RTFW accepted host job"),
                LowLevelTasks::ETaskPriority::Normal,
                [this, &Slot, Generation]() noexcept
                {
                    (void)Dispatch(Slot, Generation);
                },
                LowLevelTasks::ETaskFlags::AllowCancellation);
            Slot.Control.store(
                Pack(Generation, ESlotState::Accepted),
                std::memory_order_release);

#if WITH_DEV_AUTOMATION_TESTS
            if (bRejectNextLaunch.exchange(false, std::memory_order_acq_rel))
            {
                const bool bCanceled = Slot.Task.TryCancel(
                    LowLevelTasks::ECancellationFlags::PrelaunchCancellation |
                    LowLevelTasks::ECancellationFlags::TryLaunchOnSuccess);
                check(bCanceled && Slot.Task.IsCompleted());
                Slot.Job = {};
                Slot.Control.store(
                    Pack(Generation, ESlotState::Free),
                    std::memory_order_release);
                Slot.NodeAccess.store(false, std::memory_order_release);
                LaunchRejected.fetch_add(1, std::memory_order_relaxed);
                QueueFull.fetch_add(1, std::memory_order_relaxed);
                return rt::Status::queue_full;
            }
#endif

            if (!LowLevelTasks::TryLaunch(
                    Slot.Task,
                    LowLevelTasks::EQueuePreference::GlobalQueuePreference))
            {
                // A freshly initialized private node has no competing launcher.
                // If the pinned API ever violates that invariant, fail closed.
                bAdmissionOpen.store(false, std::memory_order_release);
                Slot.NodeAccess.store(false, std::memory_order_release);
                return rt::Status::internal_error;
            }
            // No helper or new submission may touch this node before launch
            // returns, even when the scheduler finishes the callback inline.
            Slot.NodeAccess.store(false, std::memory_order_release);
            Accepted.fetch_add(1, std::memory_order_relaxed);
            return rt::Status::ok;
        }

        QueueFull.fetch_add(1, std::memory_order_relaxed);
        return rt::Status::queue_full;
    }

    [[nodiscard]] bool TryExecuteOne() noexcept
    {
        if (!bValid.load(std::memory_order_acquire))
        {
            return false;
        }
        const uint64 Start = SearchCursor.fetch_add(1, std::memory_order_relaxed);
        for (uint32 Offset = 0; Offset < Capacity; ++Offset)
        {
            FSlot& Slot = Slots[(Start + Offset) & (Capacity - 1)];
            const uint64 Control = Slot.Control.load(std::memory_order_acquire);
            if (StateOf(Control) != ESlotState::Accepted)
            {
                ReclaimCompleted(Slot);
                continue;
            }
            bool Access = false;
            if (!Slot.NodeAccess.compare_exchange_strong(Access, true, std::memory_order_acquire)) continue;
            // Recheck after taking the node lease; a prior completion/reuse
            // cannot race TryExpedite against Init on another generation.
            if (Slot.Control.load(std::memory_order_acquire) != Control)
            {
                Slot.NodeAccess.store(false, std::memory_order_release);
                continue;
            }
            const bool bExecuted = Slot.Task.TryExpedite();
            Slot.NodeAccess.store(false, std::memory_order_release);
            if (bExecuted)
            {
                Helped.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            return false;
        }
        return false;
    }

    static rt::Status SubmitThunk(
        void* Context,
        const rt::HostExecutorJob& Job) noexcept
    {
        return static_cast<FImpl*>(Context)->Submit(Job);
    }

    static bool HelpThunk(void* Context) noexcept
    {
        return static_cast<FImpl*>(Context)->TryExecuteOne();
    }
};

FRTFWUnrealJobAdapter::FRTFWUnrealJobAdapter(
    uint32 WorkerCount,
    uint32 Capacity) noexcept
    : Impl(MakeUnique<FImpl>(WorkerCount, Capacity))
{
    AdapterOwners.fetch_add(1, std::memory_order_relaxed);
}

FRTFWUnrealJobAdapter::~FRTFWUnrealJobAdapter()
{
    if (Impl)
    {
        CloseAdmission();
        if (!IsQuiescent()) std::terminate(); // Also enforced in Shipping.
    }
    AdapterOwners.fetch_sub(1, std::memory_order_relaxed);
}

bool FRTFWUnrealJobAdapter::IsValid() const noexcept
{
    return Impl && Impl->bValid.load(std::memory_order_acquire);
}

rt::Status FRTFWUnrealJobAdapter::Attach(
    rt::Runtime& Runtime,
    const rt::RuntimeConfig& Configuration) noexcept
{
    if (!IsValid() ||
        Configuration.executor_policy != rt::ExecutorPolicy::host_adapter ||
        Configuration.worker_count != Impl->WorkerCount ||
        Configuration.executor_queue_capacity != Impl->Capacity)
    {
        return rt::Status::invalid_config;
    }
    bool Expected = false;
    if (!Impl->bAttached.compare_exchange_strong(
            Expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed))
    {
        return rt::Status::invalid_state;
    }
    const rt::Status Status = Runtime.set_host_executor(GetAdapter());
    if (Status != rt::Status::ok)
    {
        Impl->bAttached.store(false, std::memory_order_release);
    }
    return Status;
}

void FRTFWUnrealJobAdapter::CloseAdmission() noexcept
{
    if (Impl)
    {
        Impl->bAdmissionOpen.store(false, std::memory_order_release);
    }
}

bool FRTFWUnrealJobAdapter::IsQuiescent() noexcept
{
    if (!Impl || !Impl->Slots)
    {
        return true;
    }
    bool bQuiescent = true;
    for (uint32 Index = 0; Index < Impl->Capacity; ++Index)
    {
        Impl->ReclaimCompleted(Impl->Slots[Index]);
        auto& Slot = Impl->Slots[Index];
        bool Access = false;
        if (!Slot.NodeAccess.compare_exchange_strong(Access, true, std::memory_order_acquire))
        {
            bQuiescent = false;
            continue;
        }
        bQuiescent = bQuiescent && StateOf(Slot.Control.load(std::memory_order_acquire)) == ESlotState::Free &&
            Slot.Task.IsCompleted(std::memory_order_acquire);
        Slot.NodeAccess.store(false, std::memory_order_release);
    }
    return bQuiescent;
}

rt::HostExecutorAdapter FRTFWUnrealJobAdapter::GetAdapter() noexcept
{
    return IsValid()
        ? rt::HostExecutorAdapter{
              Impl.Get(),
              Impl->WorkerCount,
              Impl->Capacity,
              &FImpl::SubmitThunk,
              &FImpl::HelpThunk}
        : rt::HostExecutorAdapter{};
}

FRTFWUnrealJobAdapterStats FRTFWUnrealJobAdapter::GetStats() const noexcept
{
    if (!Impl)
    {
        return {};
    }
    return {
        Impl->Accepted.load(std::memory_order_relaxed),
        Impl->Executed.load(std::memory_order_relaxed),
        Impl->Helped.load(std::memory_order_relaxed),
        Impl->QueueFull.load(std::memory_order_relaxed),
        Impl->LaunchRejected.load(std::memory_order_relaxed),
        Impl->StaleOrDuplicate.load(std::memory_order_relaxed),
        Impl->Retired.load(std::memory_order_relaxed),
        Impl->GenerationExhausted.load(std::memory_order_relaxed),
        static_cast<uint64>(Impl->Capacity) * sizeof(FImpl::FSlot),
        Impl->WorkerCount,
        Impl->Capacity,
        Impl->bAdmissionOpen.load(std::memory_order_relaxed),
    };
}

#if WITH_DEV_AUTOMATION_TESTS
void FRTFWUnrealJobAdapter::RejectNextLaunchForTesting() noexcept
{
    if (Impl)
    {
        Impl->bRejectNextLaunch.store(true, std::memory_order_release);
    }
}

void FRTFWUnrealJobAdapter::SetNextGenerationForTesting(uint64 Generation) noexcept
{
    if (Impl)
    {
        Impl->NextGeneration.store(Generation, std::memory_order_release);
    }
}

bool FRTFWUnrealJobAdapter::DispatchForTesting(
    uint32 SlotIndex,
    uint64 Generation) noexcept
{
    return Impl && SlotIndex < Impl->Capacity &&
        Impl->Dispatch(Impl->Slots[SlotIndex], Generation);
}
#endif

uint32 RTFWUnrealActiveAdapters() noexcept { return AdapterOwners.load(std::memory_order_relaxed); }

FRTFWUnrealMemoryProvider::FRTFWUnrealMemoryProvider() noexcept {
    AdapterOwners.fetch_add(1, std::memory_order_relaxed);
}
FRTFWUnrealMemoryProvider::~FRTFWUnrealMemoryProvider()
{
    if (HasLiveAllocations()) std::terminate(); // Also enforced in Shipping.
    AdapterOwners.fetch_sub(1, std::memory_order_relaxed);
}

rt::Status FRTFWUnrealMemoryProvider::Attach(rt::Runtime& Runtime) noexcept
{
    bool Expected = false;
    if (!bAttached.compare_exchange_strong(
            Expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed))
    {
        return rt::Status::invalid_state;
    }
    const rt::Status Status = Runtime.set_memory_provider(GetProvider());
    if (Status != rt::Status::ok)
    {
        bAttached.store(false, std::memory_order_release);
    }
    return Status;
}

rt::MemoryProvider FRTFWUnrealMemoryProvider::GetProvider() noexcept
{
    return {
        sizeof(rt::MemoryProvider),
        rt::memory_provider_api_version,
        rt::memory_provider_capability_bit(
            rt::MemoryProviderCapability::policy_operations) |
            rt::memory_provider_capability_bit(
                rt::MemoryProviderCapability::independent_observation),
        this,
        &Acquire,
        &Apply,
        &Observe,
        &Rollback,
        &Release,
        {},
    };
}

bool FRTFWUnrealMemoryProvider::IsActiveRegion(
    rt::MemoryRegionId Region) noexcept
{
    return Region == rt::memory_region_phase_scratch ||
        Region == rt::memory_region_task_scratch ||
        Region == rt::memory_region_trace_storage;
}

bool FRTFWUnrealMemoryProvider::IsSupportedPolicy(
    const rt::MemoryPolicy& Policy) noexcept
{
    return Policy.provider == rt::MemoryProviderOwnership::host &&
        Policy.guard_bytes_before == 0 &&
        Policy.guard_bytes_after == 0 &&
        Policy.page_rounding != rt::PageRounding::base_page &&
        Policy.prefault != rt::PolicyToggle::enabled &&
        Policy.locking != rt::PolicyToggle::enabled &&
        Policy.pinning != rt::PolicyToggle::enabled &&
        Policy.huge_pages != rt::HugePagePreference::prefer &&
        Policy.numa_node < 0 &&
        Policy.first_touch != rt::FirstTouchPolicy::caller &&
        Policy.first_touch != rt::FirstTouchPolicy::owner_thread &&
        Policy.residency_verification != rt::PolicyToggle::enabled;
}

FRTFWUnrealMemoryProvider::FAllocation*
FRTFWUnrealMemoryProvider::FindFree() noexcept
{
    for (FAllocation& Allocation : Allocations)
    {
        if (!Allocation.bOwned)
        {
            return &Allocation;
        }
    }
    return nullptr;
}

FRTFWUnrealMemoryProvider::FAllocation*
FRTFWUnrealMemoryProvider::FindToken(void* Token) noexcept
{
    for (FAllocation& Allocation : Allocations)
    {
        if (&Allocation == Token && Allocation.bOwned)
        {
            return &Allocation;
        }
    }
    return nullptr;
}

const FRTFWUnrealMemoryProvider::FAllocation*
FRTFWUnrealMemoryProvider::FindToken(void* Token) const noexcept
{
    for (const FAllocation& Allocation : Allocations)
    {
        if (&Allocation == Token && Allocation.bOwned)
        {
            return &Allocation;
        }
    }
    return nullptr;
}

rt::Status FRTFWUnrealMemoryProvider::Acquire(
    void* Context,
    const rt::MemoryProviderAcquireRequest& Request,
    rt::MemoryProviderAllocation& Allocation) noexcept
{
    auto& Self = *static_cast<FRTFWUnrealMemoryProvider*>(Context);
    Allocation = {};
    if (!IsActiveRegion(Request.region) || Request.logical_bytes == 0 ||
        Request.required_alignment == 0 ||
        (Request.required_alignment & (Request.required_alignment - 1)) != 0 ||
        Request.required_alignment > std::numeric_limits<uint32>::max() ||
        Request.page_rounding == rt::PageRounding::base_page ||
        Request.guard_bytes_before != 0 || Request.guard_bytes_after != 0 ||
        Request.huge_pages == rt::HugePagePreference::prefer ||
        Request.numa_node >= 0)
    {
        return rt::Status::invalid_config;
    }
#if WITH_DEV_AUTOMATION_TESTS
    const int32 Index = static_cast<int32>(Self.Acquired.load(std::memory_order_relaxed));
    if (Self.FailAcquireAt.load(std::memory_order_relaxed) == Index)
    {
        return rt::Status::resource_exhausted;
    }
#endif
    FAllocation* Record = Self.FindFree();
    if (Record == nullptr)
    {
        return rt::Status::capacity_exceeded;
    }
    void* Address = FMemory::Malloc(
        static_cast<SIZE_T>(Request.logical_bytes),
        static_cast<uint32>(Request.required_alignment));
    if (Address == nullptr)
    {
        return rt::Status::resource_exhausted;
    }
    const SIZE_T Extent = FMemory::GetAllocSize(Address);
    if (Extent < Request.logical_bytes ||
        reinterpret_cast<UPTRINT>(Address) % Request.required_alignment != 0)
    {
        FMemory::Free(Address);
        return rt::Status::internal_error;
    }
    *Record = {
        Address,
        Extent,
        static_cast<SIZE_T>(Request.logical_bytes),
        static_cast<uint32>(Request.required_alignment),
        Request.region,
        true,
        false,
    };
    Allocation.token = Record;
    Allocation.allocation_base = static_cast<std::byte*>(Address);
    Allocation.allocation_bytes = static_cast<std::size_t>(Extent);
    Allocation.usable_data = static_cast<std::byte*>(Address);
    Allocation.usable_bytes = static_cast<std::size_t>(Extent);
    Allocation.committed_bytes = static_cast<std::size_t>(Extent);
    Allocation.alignment = Request.required_alignment;
    Self.Acquired.fetch_add(1, std::memory_order_relaxed);
    return rt::Status::ok;
}

rt::Status FRTFWUnrealMemoryProvider::Apply(
    void* Context,
    void* Token,
    const rt::MemoryPolicy& Resolved,
    rt::MemoryProviderObservation& Output) noexcept
{
    auto& Self = *static_cast<FRTFWUnrealMemoryProvider*>(Context);
    Output = {};
    FAllocation* Record = Self.FindToken(Token);
    if (Record == nullptr)
    {
        return rt::Status::invalid_handle;
    }
    Record->bApplied = true;
    if (!IsSupportedPolicy(Resolved))
    {
        Self.UnsupportedPolicies.fetch_add(1, std::memory_order_relaxed);
        return rt::Status::invalid_config;
    }
#if WITH_DEV_AUTOMATION_TESTS
    const int32 Index = static_cast<int32>(Self.AppliedCount.load(std::memory_order_relaxed));
    if (Self.FailApplyAt.load(std::memory_order_relaxed) == Index)
    {
        return rt::Status::internal_error;
    }
#endif
    Self.AppliedCount.fetch_add(1, std::memory_order_relaxed);
    return rt::Status::ok;
}

rt::Status FRTFWUnrealMemoryProvider::Observe(
    void* Context,
    void* Token,
    const rt::MemoryPolicy& Resolved,
    rt::MemoryProviderObservation& Output) noexcept
{
    auto& Self = *static_cast<FRTFWUnrealMemoryProvider*>(Context);
    Output = {};
    FAllocation* Record = Self.FindToken(Token);
    if (Record == nullptr || !Record->bApplied)
    {
        return rt::Status::invalid_handle;
    }
    if (!IsSupportedPolicy(Resolved))
    {
        Self.UnsupportedPolicies.fetch_add(1, std::memory_order_relaxed);
        return rt::Status::invalid_config;
    }
#if WITH_DEV_AUTOMATION_TESTS
    const int32 Index = static_cast<int32>(Self.ObservedCount.load(std::memory_order_relaxed));
    if (Self.FailObserveAt.load(std::memory_order_relaxed) == Index)
    {
        return rt::Status::internal_error;
    }
#endif
    const SIZE_T ObservedExtent = FMemory::GetAllocSize(Record->Address);
    if (ObservedExtent != Record->ExtentBytes ||
        ObservedExtent < Record->LogicalBytes)
    {
        Self.ExtentMismatches.fetch_add(1, std::memory_order_relaxed);
        return rt::Status::internal_error;
    }
    Output.independently_observed = true;
    Output.numa_node = -1;
    Self.ObservedCount.fetch_add(1, std::memory_order_relaxed);
    return rt::Status::ok;
}

rt::Status FRTFWUnrealMemoryProvider::Rollback(
    void* Context,
    void* Token,
    const rt::MemoryPolicy&,
    const rt::MemoryProviderObservation&) noexcept
{
    auto& Self = *static_cast<FRTFWUnrealMemoryProvider*>(Context);
    FAllocation* Record = Self.FindToken(Token);
    if (Record == nullptr)
    {
        return rt::Status::invalid_handle;
    }
#if WITH_DEV_AUTOMATION_TESTS
    uint32 Remaining = Self.FailRollbackCount.load(std::memory_order_relaxed);
    if (Remaining != 0 && Self.FailRollbackCount.compare_exchange_strong(
            Remaining,
            Remaining - 1,
            std::memory_order_acq_rel,
            std::memory_order_relaxed))
    {
        return rt::Status::internal_error;
    }
#endif
    Record->bApplied = false;
    Self.RolledBack.fetch_add(1, std::memory_order_relaxed);
    return rt::Status::ok;
}

void FRTFWUnrealMemoryProvider::Release(
    void* Context,
    void* Token,
    rt::RollbackIntent) noexcept
{
    auto& Self = *static_cast<FRTFWUnrealMemoryProvider*>(Context);
    FAllocation* Record = Self.FindToken(Token);
    if (Record == nullptr)
    {
        return;
    }
    FMemory::Free(Record->Address);
    *Record = {};
    Self.Released.fetch_add(1, std::memory_order_relaxed);
}

FRTFWUnrealAllocatorStats FRTFWUnrealMemoryProvider::GetStats() const noexcept
{
    uint64 LiveBytes = 0;
    uint32 LiveCount = 0;
    for (const FAllocation& Allocation : Allocations)
    {
        if (Allocation.bOwned)
        {
            LiveBytes += static_cast<uint64>(Allocation.ExtentBytes);
            ++LiveCount;
        }
    }
    return {
        Acquired.load(std::memory_order_relaxed),
        AppliedCount.load(std::memory_order_relaxed),
        ObservedCount.load(std::memory_order_relaxed),
        RolledBack.load(std::memory_order_relaxed),
        Released.load(std::memory_order_relaxed),
        UnsupportedPolicies.load(std::memory_order_relaxed),
        ExtentMismatches.load(std::memory_order_relaxed),
        LiveBytes,
        LiveCount,
    };
}

bool FRTFWUnrealMemoryProvider::HasLiveAllocations() const noexcept
{
    for (const FAllocation& Allocation : Allocations)
    {
        if (Allocation.bOwned)
        {
            return true;
        }
    }
    return false;
}

#if WITH_DEV_AUTOMATION_TESTS
void FRTFWUnrealMemoryProvider::FailAcquireAtForTesting(int32 Index) noexcept
{
    FailAcquireAt.store(Index, std::memory_order_release);
}

void FRTFWUnrealMemoryProvider::FailApplyAtForTesting(int32 Index) noexcept
{
    FailApplyAt.store(Index, std::memory_order_release);
}

void FRTFWUnrealMemoryProvider::FailObserveAtForTesting(int32 Index) noexcept
{
    FailObserveAt.store(Index, std::memory_order_release);
}

void FRTFWUnrealMemoryProvider::FailRollbackCountForTesting(uint32 Count) noexcept
{
    FailRollbackCount.store(Count, std::memory_order_release);
}
#endif

FRTFWUnrealClock::FRTFWUnrealClock() noexcept
    : SecondsPerCycle(FPlatformTime::GetSecondsPerCycle64())
{
    AdapterOwners.fetch_add(1, std::memory_order_relaxed);
}
FRTFWUnrealClock::~FRTFWUnrealClock() { AdapterOwners.fetch_sub(1, std::memory_order_relaxed); }

bool FRTFWUnrealClock::TrySecondsToNanoseconds(
    long double Seconds,
    bool bRequirePositive,
    uint64& Nanoseconds) noexcept
{
    if (!std::isfinite(Seconds) || Seconds < 0.0L ||
        (bRequirePositive && Seconds <= 0.0L))
    {
        return false;
    }
    const long double Scaled = Seconds * 1'000'000'000.0L;
    if (!std::isfinite(Scaled) ||
        Scaled >= std::ldexp(1.0L, 64))
    {
        return false;
    }
    const long double Rounded = std::floor(Scaled + 0.5L);
    if ((bRequirePositive && Rounded < 1.0L) || Rounded < 0.0L ||
        Rounded >= std::ldexp(1.0L, 64))
    {
        return false;
    }
    Nanoseconds = static_cast<uint64>(Rounded);
    return true;
}

bool FRTFWUnrealClock::TryCyclesToNanoseconds(
    uint64 Cycles,
    uint64& Nanoseconds) const noexcept
{
    if (!std::isfinite(SecondsPerCycle) || SecondsPerCycle <= 0.0)
    {
        return false;
    }
    return TrySecondsToNanoseconds(
        static_cast<long double>(Cycles) *
            static_cast<long double>(SecondsPerCycle),
        false,
        Nanoseconds);
}

std::uint64_t FRTFWUnrealClock::now_ns() noexcept
{
    uint64 Candidate = 0;
    if (!TryCyclesToNanoseconds(FPlatformTime::Cycles64(), Candidate))
    {
        FailureCount.fetch_add(1, std::memory_order_relaxed);
        return LastNanoseconds.load(std::memory_order_acquire);
    }
    uint64 Previous = LastNanoseconds.load(std::memory_order_relaxed);
    if (Candidate <= Previous) return Previous;
    // One publication attempt. On contention return the observed publication;
    // never return an unpublished candidate or spin on an engine frame lane.
    return LastNanoseconds.compare_exchange_strong(Previous, Candidate,
        std::memory_order_acq_rel, std::memory_order_acquire) ? Candidate : Previous;
}

rt::Status FRTFWUnrealClock::sleep_until_ns(std::uint64_t) noexcept
{
    return rt::Status::clock_failure;
}

bool FRTFWUnrealClock::supports_absolute_sleep() const noexcept
{
    return false;
}

uint64 FRTFWUnrealClock::ConversionFailures() const noexcept
{
    return FailureCount.load(std::memory_order_relaxed);
}

rt::Status FRTFWUnrealFrameContext::Make(
    const FRTFWUnrealClock& Clock,
    uint64 FrameSequence,
    double DeltaSeconds,
    TOptional<uint64> DeadlineCycles,
    TOptional<uint64> NominalReleaseCycles,
    rt::HostFrameContext& Output) noexcept
{
    uint64 DeltaNanoseconds = 0;
    if (!FRTFWUnrealClock::TrySecondsToNanoseconds(
            static_cast<long double>(DeltaSeconds),
            true,
            DeltaNanoseconds) ||
        DeltaNanoseconds > static_cast<uint64>(
            std::numeric_limits<std::chrono::nanoseconds::rep>::max()))
    {
        return rt::Status::invalid_argument;
    }

    std::optional<uint64> Deadline;
    if (DeadlineCycles.IsSet())
    {
        uint64 Value = 0;
        if (!Clock.TryCyclesToNanoseconds(DeadlineCycles.GetValue(), Value))
        {
            return rt::Status::clock_failure;
        }
        Deadline = Value;
    }
    std::optional<uint64> NominalRelease;
    if (NominalReleaseCycles.IsSet())
    {
        uint64 Value = 0;
        if (!Clock.TryCyclesToNanoseconds(
                NominalReleaseCycles.GetValue(),
                Value))
        {
            return rt::Status::clock_failure;
        }
        NominalRelease = Value;
    }

    Output = {
        FrameSequence,
        std::chrono::nanoseconds(
            static_cast<std::chrono::nanoseconds::rep>(DeltaNanoseconds)),
        Deadline,
        NominalRelease,
    };
    return rt::Status::ok;
}
