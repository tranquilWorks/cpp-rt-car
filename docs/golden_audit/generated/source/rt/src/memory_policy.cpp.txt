#include "memory_policy.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

constexpr std::uint64_t kKnownCapabilities =
    rt::memory_provider_capability_bit(
        rt::MemoryProviderCapability::guard_pages) |
    rt::memory_provider_capability_bit(
        rt::MemoryProviderCapability::explicit_huge_pages) |
    rt::memory_provider_capability_bit(
        rt::MemoryProviderCapability::policy_operations) |
    rt::memory_provider_capability_bit(
        rt::MemoryProviderCapability::independent_observation) |
    rt::memory_provider_capability_bit(
        rt::MemoryProviderCapability::pinning) |
    rt::memory_provider_capability_bit(
        rt::MemoryProviderCapability::numa_binding);

struct LiveAllocationRegistry {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    rt::detail::LiveResidentAllocation* head = nullptr;
};

constinit LiveAllocationRegistry g_live_allocations{};

class RegistryGuard final {
public:
    RegistryGuard() noexcept {
        while (g_live_allocations.lock.test_and_set(
            std::memory_order_acquire)) {
        }
    }
    ~RegistryGuard() {
        g_live_allocations.lock.clear(std::memory_order_release);
    }
};

[[nodiscard]] bool has_capability(
    const rt::MemoryProvider& provider,
    rt::MemoryProviderCapability capability) noexcept {
    return (provider.capabilities &
            rt::memory_provider_capability_bit(capability)) != 0;
}

[[nodiscard]] bool pointer_range(
    const std::byte* pointer,
    std::size_t bytes,
    std::uintptr_t& begin,
    std::uintptr_t& end) noexcept {
    begin = reinterpret_cast<std::uintptr_t>(pointer);
    if (bytes > std::numeric_limits<std::uintptr_t>::max() - begin) {
        end = 0;
        return false;
    }
    end = begin + bytes;
    return true;
}

class CallbackGuard final {
public:
    explicit CallbackGuard(std::atomic<bool>& active) noexcept
        : active_(active) {
        active_.store(true, std::memory_order_release);
    }
    ~CallbackGuard() {
        active_.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool>& active_;
};

[[nodiscard]] std::size_t native_page_size() noexcept {
#if defined(__linux__)
    const long value = ::sysconf(_SC_PAGESIZE);
    return value > 0 ? static_cast<std::size_t>(value) : 4096u;
#else
    return 4096u;
#endif
}

[[nodiscard]] bool strict_requested(
    const rt::MemoryPolicyReport& row) noexcept {
    return row.requested.requirement == rt::PolicyRequirement::strict;
}

} // namespace

namespace rt::detail {

ResidentRegionSet::ResidentRegionSet(
    const MemoryProvider* provider,
    std::atomic<bool>& callback_active) noexcept
    : regions_{{
          Region{memory_region_phase_scratch},
          Region{memory_region_task_scratch},
          Region{memory_region_trace_storage},
      }},
      callback_active_(&callback_active) {
    if (provider) {
        provider_copy_ = *provider;
        provider_ = &provider_copy_;
    }
}

ResidentRegionSet::~ResidentRegionSet() {
    release();
}

Status ResidentRegionSet::validate_provider(
    const MemoryProvider* provider,
    const char*& diagnostic) noexcept {
    diagnostic = nullptr;
    if (!provider) {
        return Status::ok;
    }
    if (provider->struct_size != sizeof(MemoryProvider) ||
        provider->api_version != memory_provider_api_version) {
        diagnostic = "memory provider size or version is incompatible";
        return Status::invalid_config;
    }
    if ((provider->capabilities & ~kKnownCapabilities) != 0 ||
        std::any_of(
            provider->reserved.begin(),
            provider->reserved.end(),
            [](std::uint64_t value) { return value != 0; })) {
        diagnostic = "memory provider capabilities or reserved fields are invalid";
        return Status::invalid_config;
    }
    if (!provider->acquire || !provider->release ||
        !provider->apply || !provider->observe ||
        !provider->rollback ||
        !has_capability(
            *provider,
            MemoryProviderCapability::policy_operations) ||
        !has_capability(
            *provider,
            MemoryProviderCapability::independent_observation)) {
        diagnostic = "memory provider callback table is incomplete";
        return Status::invalid_config;
    }
    return Status::ok;
}

MemoryPolicyReport* ResidentRegionSet::find_report(
    CpuMemoryPolicyReport& report,
    MemoryRegionId region) noexcept {
    for (std::size_t index = 0; index < report.memory_count; ++index) {
        if (report.memory[index].region == region) {
            return &report.memory[index];
        }
    }
    return nullptr;
}

ResidentRegionSet::Region* ResidentRegionSet::find_region(
    MemoryRegionId region) noexcept {
    for (auto& candidate : regions_) {
        if (candidate.id == region) {
            return &candidate;
        }
    }
    return nullptr;
}

Status ResidentRegionSet::acquire(
    CpuMemoryPolicyReport& report,
    const char*& diagnostic) noexcept {
    diagnostic = nullptr;
    for (auto& region : regions_) {
        auto* row = find_report(report, region.id);
        if (!row) {
            diagnostic = "memory policy inventory is missing a resident region";
            release();
            return Status::internal_error;
        }
        const std::size_t alignment =
            region.id == memory_region_trace_storage
            ? std::max(row->resolved.alignment, std::size_t{64})
            : row->resolved.alignment;
        const auto status = acquire_one(
            region,
            *row,
            alignment == 0 ? alignof(std::max_align_t) : alignment,
            diagnostic);
        if (status != Status::ok) {
            release();
            return status;
        }
    }
    return Status::ok;
}

Status ResidentRegionSet::acquire_one(
    Region& region,
    MemoryPolicyReport& row,
    std::size_t required_alignment,
    const char*& diagnostic) noexcept {
    row.acquired = PolicyOperationState::not_attempted;
    row.provider_error = 0;
    if (row.accounted_bytes == 0) {
        return Status::ok;
    }

    const MemoryProviderAcquireRequest request{
        region.id,
        row.accounted_bytes,
        required_alignment,
        row.resolved.page_rounding,
        row.resolved.guard_bytes_before,
        row.resolved.guard_bytes_after,
        row.resolved.huge_pages,
        row.resolved.huge_page_fallback,
        row.resolved.numa_node,
        row.resolved.rollback,
    };

    Status status = Status::ok;
    if (provider_) {
        MemoryProviderAllocation allocation{};
        {
            CallbackGuard guard(*callback_active_);
            status = provider_->acquire(
                provider_->user_data,
                request,
                allocation);
        }
        region.allocation = allocation;
        if (status != Status::ok) {
            // A failed acquire may still return a provisional token requiring
            // cleanup. Claim it atomically before adopting it so a malformed
            // provider cannot make this runtime release a token or extent
            // already owned by another live runtime.
            if (allocation.token &&
                register_provisional(region) ==
                    RegistrationResult::registered) {
                region.acquired = true;
            }
            row.provider_error = allocation.provider_error;
            row.acquired = PolicyOperationState::failed;
            diagnostic = "memory provider acquisition failed";
            return status;
        }
        if (allocation.token) {
            const bool duplicate_live_token = std::any_of(
                regions_.begin(),
                regions_.end(),
                [&](const Region& other) {
                    return &other != &region && other.acquired &&
                           other.allocation.token == allocation.token;
                });
            if (duplicate_live_token) {
                row.provider_error = allocation.provider_error;
                row.acquired = PolicyOperationState::failed;
                diagnostic = "memory provider returned a duplicate live token";
                return Status::invalid_config;
            }
            region.acquired = true;
        }
    } else {
        status = acquire_native(
            region,
            request,
            row.resolved.locking == PolicyToggle::enabled,
            diagnostic);
        if (status != Status::ok) {
            row.acquired = PolicyOperationState::failed;
            return status;
        }
        region.acquired = true;
    }

    status = validate_allocation(region, request, diagnostic);
    if (status != Status::ok) {
        row.provider_error = region.allocation.provider_error;
        row.acquired = PolicyOperationState::failed;
        return status;
    }
    const auto registration = register_live(region);
    if (registration != RegistrationResult::registered) {
        row.provider_error = region.allocation.provider_error;
        row.acquired = PolicyOperationState::failed;
        if (registration == RegistrationResult::duplicate_token) {
            // A provider did not create a new ownership token. Releasing it
            // here could invalidate the already-live runtime that owns it.
            region.acquired = false;
            diagnostic = "memory provider returned a token already owned by another runtime";
        } else {
            diagnostic = "memory provider returned storage overlapping another runtime";
        }
        return Status::invalid_config;
    }
    row.committed_bytes = region.allocation.committed_bytes;
    row.actual_guard_bytes_before =
        region.allocation.guard_bytes_before;
    row.actual_guard_bytes_after =
        region.allocation.guard_bytes_after;
    row.actual_page_bytes = region.allocation.actual_page_bytes;
    row.used_explicit_huge_pages =
        region.allocation.explicit_huge_pages;
    row.used_huge_page_fallback =
        region.allocation.used_huge_page_fallback;
    row.provider_error = region.allocation.provider_error;
    row.acquired = PolicyOperationState::succeeded;
    return Status::ok;
}

Status ResidentRegionSet::acquire_native(
    Region& region,
    const MemoryProviderAcquireRequest& request,
    bool isolate_for_locking,
    const char*& diagnostic) noexcept {
    const bool page_policy =
        request.page_rounding == PageRounding::base_page ||
        request.guard_bytes_before != 0 ||
        request.guard_bytes_after != 0 ||
        request.huge_pages == HugePagePreference::prefer ||
        request.numa_node >= 0 || isolate_for_locking;
    if (!page_policy) {
        try {
            region.owned.allocate(
                request.logical_bytes,
                request.required_alignment);
        } catch (const std::bad_alloc&) {
            diagnostic = "default resident-region allocation failed";
            return Status::resource_exhausted;
        }
        region.allocation = {
            region.owned.data(),
            region.owned.data(),
            region.owned.size(),
            region.owned.data(),
            region.owned.size(),
            region.owned.size(),
            region.owned.alignment(),
            0,
            0,
            0,
            false,
            false,
            0,
        };
        return Status::ok;
    }

#if !defined(__linux__)
    diagnostic = "native page-backed memory policy is unsupported";
    return Status::invalid_config;
#else
    const auto page = native_page_size();
    std::size_t committed = 0;
    std::size_t guard_before = 0;
    std::size_t guard_after = 0;
    if (!checked_align_up(request.logical_bytes, page, committed) ||
        (request.guard_bytes_before != 0 &&
         !checked_align_up(request.guard_bytes_before, page, guard_before)) ||
        (request.guard_bytes_after != 0 &&
         !checked_align_up(request.guard_bytes_after, page, guard_after))) {
        diagnostic = "resident-region page rounding overflows";
        return Status::invalid_config;
    }
    const auto alignment = std::max(request.required_alignment, page);
    std::size_t mapping_bytes = 0;
    std::size_t extra = 0;
    if (!checked_add(guard_before, guard_after, extra) ||
        !checked_add(extra, alignment, extra) ||
        !checked_add(committed, extra, mapping_bytes)) {
        diagnostic = "resident-region mapping size overflows";
        return Status::invalid_config;
    }

    void* mapping = MAP_FAILED;
    bool explicit_huge = false;
#if defined(MAP_HUGETLB)
    if (request.huge_pages == HugePagePreference::prefer &&
        guard_before == 0 && guard_after == 0) {
        constexpr std::size_t kExplicitHugePageBytes = 2u * 1024u * 1024u;
        std::size_t huge_committed = 0;
        std::size_t huge_mapping_bytes = 0;
        if (!checked_align_up(
                request.logical_bytes,
                kExplicitHugePageBytes,
                huge_committed) ||
            !checked_add(huge_committed, alignment, huge_mapping_bytes) ||
            !checked_align_up(
                huge_mapping_bytes,
                kExplicitHugePageBytes,
                huge_mapping_bytes)) {
            diagnostic = "explicit huge-page mapping size overflows";
            return Status::invalid_config;
        }
#if defined(MAP_HUGE_SHIFT)
        constexpr int kHuge2MiB = 21 << MAP_HUGE_SHIFT;
#else
        // Linux UAPI fixes MAP_HUGE_SHIFT at bit 26 even when older libc
        // headers omit the convenience macro.
        constexpr int kHuge2MiB = 21 << 26;
#endif
        mapping = ::mmap(
            nullptr,
            huge_mapping_bytes,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | kHuge2MiB,
            -1,
            0);
        explicit_huge = mapping != MAP_FAILED;
        if (explicit_huge) {
            committed = huge_committed;
            mapping_bytes = huge_mapping_bytes;
        }
    }
#endif
    bool fallback = false;
    if (mapping == MAP_FAILED) {
        if (request.huge_pages == HugePagePreference::prefer &&
            request.huge_page_fallback != PolicyToggle::enabled) {
            diagnostic = "explicit huge-page allocation failed without fallback";
            return Status::resource_exhausted;
        }
        fallback = request.huge_pages == HugePagePreference::prefer;
        mapping = ::mmap(
            nullptr,
            mapping_bytes,
            PROT_NONE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0);
        if (mapping == MAP_FAILED) {
            diagnostic = "resident-region mapping failed";
            return errno == ENOMEM
                ? Status::resource_exhausted
                : Status::internal_error;
        }
    }

    const auto base = reinterpret_cast<std::uintptr_t>(mapping);
    std::size_t offset_value = 0;
    if (!checked_add(
            static_cast<std::size_t>(base),
            guard_before,
            offset_value) ||
        !checked_align_up(offset_value, alignment, offset_value)) {
        (void)::munmap(mapping, mapping_bytes);
        diagnostic = "resident-region usable address overflows";
        return Status::invalid_config;
    }
    auto* usable = reinterpret_cast<std::byte*>(
        static_cast<std::uintptr_t>(offset_value));
    if (!explicit_huge &&
        ::mprotect(usable, committed, PROT_READ | PROT_WRITE) != 0) {
        const int error = errno;
        (void)::munmap(mapping, mapping_bytes);
        diagnostic = "resident-region guard protection failed";
        return error == ENOMEM
            ? Status::resource_exhausted
            : Status::internal_error;
    }
    std::memset(usable, 0, request.logical_bytes);
    const auto actual_guard_before =
        static_cast<std::size_t>(
            reinterpret_cast<std::uintptr_t>(usable) - base);
    const auto actual_guard_after = mapping_bytes -
        actual_guard_before - committed;
    region.native_mapping = mapping;
    region.native_mapping_bytes = mapping_bytes;
    region.allocation = {
        mapping,
        static_cast<std::byte*>(mapping),
        mapping_bytes,
        usable,
        committed,
        committed,
        alignment,
        explicit_huge ? 2u * 1024u * 1024u : page,
        explicit_huge ? 0 : actual_guard_before,
        explicit_huge ? 0 : actual_guard_after,
        explicit_huge,
        fallback,
        0,
    };
    return Status::ok;
#endif
}

Status ResidentRegionSet::validate_allocation(
    const Region& region,
    const MemoryProviderAcquireRequest& request,
    const char*& diagnostic) const noexcept {
    const auto& allocation = region.allocation;
    if (!allocation.token || !allocation.allocation_base ||
        !allocation.usable_data || allocation.allocation_bytes == 0 ||
        allocation.usable_bytes < request.logical_bytes ||
        allocation.committed_bytes < request.logical_bytes ||
        allocation.committed_bytes > allocation.usable_bytes ||
        allocation.alignment < request.required_alignment ||
        (allocation.alignment & (allocation.alignment - 1)) != 0 ||
        (reinterpret_cast<std::uintptr_t>(allocation.usable_data) &
         (request.required_alignment - 1)) != 0) {
        diagnostic = "memory provider returned an undersized or misaligned span";
        return Status::invalid_config;
    }
    std::uintptr_t allocation_begin = 0;
    std::uintptr_t allocation_end = 0;
    std::uintptr_t usable_begin = 0;
    std::uintptr_t usable_end = 0;
    if (!pointer_range(
            allocation.allocation_base,
            allocation.allocation_bytes,
            allocation_begin,
            allocation_end) ||
        !pointer_range(
            allocation.usable_data,
            allocation.usable_bytes,
            usable_begin,
            usable_end) ||
        usable_begin < allocation_begin || usable_end > allocation_end) {
        diagnostic = "memory provider returned an overflowed allocation extent";
        return Status::invalid_config;
    }
    if (allocation.guard_bytes_before < request.guard_bytes_before ||
        allocation.guard_bytes_after < request.guard_bytes_after) {
        diagnostic = "memory provider did not establish requested guards";
        return Status::invalid_config;
    }
    const auto contained_before = usable_begin - allocation_begin;
    const auto contained_after = allocation_end - usable_end;
    if (allocation.guard_bytes_before > contained_before ||
        allocation.guard_bytes_after > contained_after) {
        diagnostic = "memory provider guards lie outside the allocation extent";
        return Status::invalid_config;
    }
    if (allocation.explicit_huge_pages &&
        allocation.used_huge_page_fallback) {
        diagnostic = "memory provider reported contradictory huge-page outcomes";
        return Status::invalid_config;
    }
    if (request.huge_pages != HugePagePreference::prefer &&
        (allocation.explicit_huge_pages ||
         allocation.used_huge_page_fallback)) {
        diagnostic = "memory provider reported an unrequested huge-page outcome";
        return Status::invalid_config;
    }
    if (request.page_rounding == PageRounding::base_page &&
        !allocation.explicit_huge_pages &&
        (allocation.actual_page_bytes == 0 ||
         (allocation.actual_page_bytes &
          (allocation.actual_page_bytes - 1)) != 0 ||
         allocation.committed_bytes % allocation.actual_page_bytes != 0 ||
         reinterpret_cast<std::uintptr_t>(allocation.usable_data) %
                 allocation.actual_page_bytes !=
             0)) {
        diagnostic = "memory provider did not return a page-rounded usable span";
        return Status::invalid_config;
    }
    if (request.huge_pages == HugePagePreference::prefer &&
        !allocation.explicit_huge_pages &&
        !allocation.used_huge_page_fallback) {
        diagnostic = "memory provider did not report a huge-page outcome";
        return Status::invalid_config;
    }
    if (allocation.used_huge_page_fallback &&
        request.huge_page_fallback != PolicyToggle::enabled) {
        diagnostic = "memory provider used a huge-page fallback that was not allowed";
        return Status::invalid_config;
    }
    if (provider_) {
        if ((allocation.guard_bytes_before != 0 ||
             allocation.guard_bytes_after != 0) &&
            !has_capability(
                *provider_,
                MemoryProviderCapability::guard_pages)) {
            diagnostic = "memory provider lacks guard capability";
            return Status::invalid_config;
        }
        if (allocation.explicit_huge_pages &&
            !has_capability(
                *provider_,
                MemoryProviderCapability::explicit_huge_pages)) {
            diagnostic = "memory provider overclaimed explicit huge pages";
            return Status::invalid_config;
        }
    }
    for (const auto& other : regions_) {
        if (&other == &region || !other.acquired) {
            continue;
        }
        std::uintptr_t other_begin = 0;
        std::uintptr_t other_end = 0;
        if (!pointer_range(
                other.allocation.allocation_base,
                other.allocation.allocation_bytes,
                other_begin,
                other_end) ||
            (allocation_begin < other_end && other_begin < allocation_end)) {
            diagnostic = "memory provider returned overlapping live regions";
            return Status::invalid_config;
        }
    }
    return Status::ok;
}

ResidentRegionSet::RegistrationResult ResidentRegionSet::register_live(
    Region& region) noexcept {
    auto& live = region.live;
    live.token = region.allocation.token;
    live.allocation_begin = reinterpret_cast<std::uintptr_t>(
        region.allocation.allocation_base);
    live.allocation_end = live.allocation_begin +
        region.allocation.allocation_bytes;
    RegistryGuard guard;
    for (auto* other = g_live_allocations.head;
         other != nullptr;
         other = other->next) {
        if (other->token == live.token) {
            live = {};
            return RegistrationResult::duplicate_token;
        }
        if (live.allocation_begin < other->allocation_end &&
            other->allocation_begin < live.allocation_end) {
            live = {};
            return RegistrationResult::overlapping_span;
        }
    }
    live.next = g_live_allocations.head;
    live.registered = true;
    g_live_allocations.head = &live;
    return RegistrationResult::registered;
}

ResidentRegionSet::RegistrationResult
ResidentRegionSet::register_provisional(Region& region) noexcept {
    auto& live = region.live;
    live.token = region.allocation.token;
    if (region.allocation.allocation_base &&
        region.allocation.allocation_bytes != 0 &&
        !pointer_range(
            region.allocation.allocation_base,
            region.allocation.allocation_bytes,
            live.allocation_begin,
            live.allocation_end)) {
        live.allocation_begin = 0;
        live.allocation_end = 0;
    }
    RegistryGuard guard;
    for (auto* other = g_live_allocations.head;
         other != nullptr;
         other = other->next) {
        if (other->token == live.token) {
            live = {};
            return RegistrationResult::duplicate_token;
        }
        if (live.allocation_begin < live.allocation_end &&
            other->allocation_begin < other->allocation_end &&
            live.allocation_begin < other->allocation_end &&
            other->allocation_begin < live.allocation_end) {
            live = {};
            return RegistrationResult::overlapping_span;
        }
    }
    live.next = g_live_allocations.head;
    live.registered = true;
    g_live_allocations.head = &live;
    return RegistrationResult::registered;
}

void ResidentRegionSet::unregister_live(Region& region) noexcept {
    auto& live = region.live;
    if (!live.registered) {
        return;
    }
    RegistryGuard guard;
    auto** link = &g_live_allocations.head;
    while (*link && *link != &live) {
        link = &(*link)->next;
    }
    if (*link == &live) {
        *link = live.next;
    }
    live = {};
}

Status ResidentRegionSet::apply_and_verify(
    CpuMemoryPolicyReport& report,
    const char*& diagnostic) noexcept {
    diagnostic = nullptr;
    for (auto& region : regions_) {
        auto* row = find_report(report, region.id);
        if (!row) {
            diagnostic = "memory policy inventory is missing a resident region";
            (void)rollback(report);
            return Status::internal_error;
        }
        const auto status = apply_one(region, *row, diagnostic);
        if (status != Status::ok) {
            (void)rollback(report);
            return status;
        }
    }
    return Status::ok;
}

Status ResidentRegionSet::apply_one(
    Region& region,
    MemoryPolicyReport& row,
    const char*& diagnostic) noexcept {
    row.applied = PolicyOperationState::not_attempted;
    row.verified = PolicyOperationState::not_attempted;
    row.resident_bytes = 0;
    row.locked_bytes = 0;
    row.pinned_bytes = 0;
    row.apply_error = 0;
    row.verify_error = 0;
    row.rollback_error = 0;
    region.applied = {};
    region.operation_applied = false;
    if (!region.acquired) {
        return Status::ok;
    }

    if (provider_) {
        Status apply_status = Status::ok;
        // A failing provider may already have changed native state. Treat every
        // apply invocation as rollback-eligible so retry cannot inherit an
        // unknown partial operation.
        region.operation_applied = true;
        {
            CallbackGuard guard(*callback_active_);
            apply_status = provider_->apply(
                provider_->user_data,
                region.allocation.token,
                row.resolved,
                region.applied);
        }
        row.apply_error = region.applied.system_error;
        if (apply_status != Status::ok) {
            row.applied = PolicyOperationState::failed;
            row.verified = PolicyOperationState::not_attempted;
            if (strict_requested(row)) {
                diagnostic = "strict memory provider apply failed";
                return apply_status;
            }
            return Status::ok;
        }
        row.applied = PolicyOperationState::succeeded;
        MemoryProviderObservation observed{};
        Status observe_status = Status::ok;
        {
            CallbackGuard guard(*callback_active_);
            observe_status = provider_->observe(
                provider_->user_data,
                region.allocation.token,
                row.resolved,
                observed);
        }
        row.verify_error = observed.system_error;
        if (observe_status != Status::ok) {
            row.verified = PolicyOperationState::failed;
            if (strict_requested(row)) {
                diagnostic = "strict memory provider observation failed";
                return observe_status;
            }
            return Status::ok;
        }
        row.resident_bytes = observed.resident_bytes;
        row.locked_bytes = observed.locked_bytes;
        row.pinned_bytes = observed.pinned_bytes;
        const bool bounded =
            observed.resident_bytes <= row.committed_bytes &&
            observed.locked_bytes <= row.committed_bytes &&
            observed.pinned_bytes <= row.committed_bytes;
        const bool pin_capability_matches =
            observed.pinned_bytes == 0 ||
            has_capability(
                *provider_,
                MemoryProviderCapability::pinning);
        bool matches = bounded && pin_capability_matches &&
            observed.independently_observed;
        if (row.resolved.prefault == PolicyToggle::enabled) {
            matches = matches && observed.prefaulted;
        }
        if (row.resolved.first_touch == FirstTouchPolicy::caller) {
            matches = matches && observed.caller_first_touched;
        }
        if (row.resolved.locking == PolicyToggle::enabled) {
            matches = matches &&
                observed.locked_bytes == row.committed_bytes;
        }
        if (row.resolved.pinning == PolicyToggle::enabled) {
            matches = matches &&
                observed.pinned_bytes == row.committed_bytes;
        }
        if (row.resolved.residency_verification == PolicyToggle::enabled) {
            matches = matches &&
                observed.resident_bytes == row.committed_bytes;
        }
        if (row.resolved.numa_node >= 0) {
            matches = matches &&
                observed.numa_node == row.resolved.numa_node;
        }
        row.verified = matches
            ? PolicyOperationState::succeeded
            : PolicyOperationState::mismatched;
        if (!matches && strict_requested(row)) {
            diagnostic = "strict memory provider readback mismatched";
            return Status::internal_error;
        }
        return Status::ok;
    }

    auto* bytes = region.allocation.usable_data;
    const auto committed = region.allocation.committed_bytes;
    if (row.resolved.first_touch == FirstTouchPolicy::caller ||
        row.resolved.prefault == PolicyToggle::enabled) {
        const auto page = native_page_size();
        for (std::size_t offset = 0; offset < committed; offset += page) {
            volatile unsigned char* location =
                reinterpret_cast<volatile unsigned char*>(bytes + offset);
            const auto value = *location;
            *location = value;
        }
        if (committed != 0) {
            volatile unsigned char* location =
                reinterpret_cast<volatile unsigned char*>(
                    bytes + committed - 1);
            const auto value = *location;
            *location = value;
        }
        region.applied.prefaulted =
            row.resolved.prefault == PolicyToggle::enabled;
        region.applied.caller_first_touched =
            row.resolved.first_touch == FirstTouchPolicy::caller;
    }
#if defined(__linux__)
    if (row.resolved.locking == PolicyToggle::enabled) {
        if (::mlock(bytes, committed) == 0) {
            region.native_locked = true;
        } else {
            row.apply_error = errno;
            row.applied = PolicyOperationState::failed;
            if (strict_requested(row)) {
                diagnostic = "strict native memory locking failed";
                return Status::internal_error;
            }
        }
    }
#endif
    region.operation_applied = true;
    if (row.applied != PolicyOperationState::failed) {
        row.applied = PolicyOperationState::succeeded;
    }

#if defined(__linux__)
    if (row.resolved.residency_verification != PolicyToggle::enabled &&
        row.resolved.locking != PolicyToggle::enabled) {
        row.verified = PolicyOperationState::succeeded;
        return Status::ok;
    }
    const auto page = native_page_size();
    const auto usable_begin = reinterpret_cast<std::uintptr_t>(bytes);
    const auto observed_begin = usable_begin & ~(page - 1);
    std::size_t usable_end = 0;
    std::size_t observed_end = 0;
    if (!checked_add(
            static_cast<std::size_t>(usable_begin),
            committed,
            usable_end) ||
        !checked_align_up(usable_end, page, observed_end)) {
        row.verify_error = EOVERFLOW;
        row.verified = PolicyOperationState::failed;
        return strict_requested(row) ? Status::internal_error : Status::ok;
    }
    const auto observed_bytes = observed_end - observed_begin;
    const auto pages = observed_bytes / page;
    try {
        std::unique_ptr<unsigned char[]> residency =
            pages == 0 ? nullptr : std::make_unique<unsigned char[]>(pages);
        if (pages != 0 &&
            ::mincore(
                reinterpret_cast<void*>(observed_begin),
                observed_bytes,
                residency.get()) != 0) {
            row.verify_error = errno;
            row.verified = PolicyOperationState::failed;
            if (strict_requested(row)) {
                diagnostic = "strict native residency observation failed";
                return Status::internal_error;
            }
            return Status::ok;
        }
        std::size_t resident = 0;
        for (std::size_t index = 0; index < pages; ++index) {
            if ((residency[index] & 1u) != 0) {
                const auto page_begin = observed_begin + (index * page);
                const auto page_end = page_begin + page;
                const auto overlap_begin = std::max(page_begin, usable_begin);
                const auto overlap_end = std::min(
                    page_end,
                    static_cast<std::uintptr_t>(usable_end));
                resident += overlap_end > overlap_begin
                    ? overlap_end - overlap_begin
                    : 0;
            }
        }
        row.resident_bytes = resident;
        bool matches = true;
        if (row.resolved.residency_verification == PolicyToggle::enabled) {
            matches = resident == committed;
        }
        // Linux exposes mincore residency independently, but mlock success is
        // not independent lock readback and never proves provider/device pin.
        if (row.resolved.locking == PolicyToggle::enabled) {
            matches = false;
        }
        row.verified = matches
            ? PolicyOperationState::succeeded
            : PolicyOperationState::mismatched;
        if (!matches && strict_requested(row)) {
            diagnostic = "strict native memory observation mismatched";
            return Status::internal_error;
        }
    } catch (const std::bad_alloc&) {
        row.verify_error = ENOMEM;
        row.verified = PolicyOperationState::failed;
        if (strict_requested(row)) {
            diagnostic = "native residency observation allocation failed";
            return Status::resource_exhausted;
        }
    }
#else
    row.verified = PolicyOperationState::succeeded;
#endif
    return Status::ok;
}

bool ResidentRegionSet::rollback(CpuMemoryPolicyReport& report) noexcept {
    bool complete = true;
    for (std::size_t index = regions_.size(); index != 0; --index) {
        auto& region = regions_[index - 1];
        if (!region.operation_applied) {
            continue;
        }
        auto* row = find_report(report, region.id);
        if (provider_) {
            Status status = Status::ok;
            {
                CallbackGuard guard(*callback_active_);
                status = provider_->rollback(
                    provider_->user_data,
                    region.allocation.token,
                    row ? row->resolved : MemoryPolicy{},
                    region.applied);
            }
            if (row && status != Status::ok) {
                row->rollback_error = static_cast<std::int32_t>(status);
            }
            if (status != Status::ok) {
                complete = false;
                continue;
            }
        }
#if defined(__linux__)
        if (region.native_locked) {
            if (::munlock(
                    region.allocation.usable_data,
                    region.allocation.committed_bytes) != 0) {
                if (row) {
                    row->rollback_error = errno;
                }
                complete = false;
            } else {
                region.native_locked = false;
            }
        }
#endif
        region.operation_applied = region.native_locked;
    }
    return complete;
}

void ResidentRegionSet::release_one(Region& region) noexcept {
    if (!region.acquired) {
        return;
    }
    if (provider_ && region.allocation.token) {
        CallbackGuard guard(*callback_active_);
        provider_->release(
            provider_->user_data,
            region.allocation.token,
            RollbackIntent::release);
    }
#if defined(__linux__)
    if (region.native_mapping) {
        (void)::munmap(
            region.native_mapping,
            region.native_mapping_bytes);
    }
#endif
    unregister_live(region);
    region.owned.reset();
    region.native_mapping = nullptr;
    region.native_mapping_bytes = 0;
    region.allocation = {};
    region.applied = {};
    region.acquired = false;
    region.operation_applied = false;
    region.native_locked = false;
}

void ResidentRegionSet::release() noexcept {
    for (std::size_t index = regions_.size(); index != 0; --index) {
        release_one(regions_[index - 1]);
    }
}

std::span<std::byte> ResidentRegionSet::span(
    MemoryRegionId region) noexcept {
    auto* record = find_region(region);
    if (!record || !record->acquired) {
        return {};
    }
    return {
        record->allocation.usable_data,
        record->allocation.usable_bytes,
    };
}

bool ResidentRegionSet::has_live_tokens() const noexcept {
    return std::any_of(
        regions_.begin(),
        regions_.end(),
        [](const Region& region) { return region.acquired; });
}

bool ResidentRegionSet::has_pending_operations() const noexcept {
    return std::any_of(
        regions_.begin(),
        regions_.end(),
        [](const Region& region) { return region.operation_applied; });
}

} // namespace rt::detail
