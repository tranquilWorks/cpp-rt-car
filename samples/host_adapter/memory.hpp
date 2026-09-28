#pragma once
#include <rt/runtime.hpp>
#include <algorithm>
#include <array>
#include <exception>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#else
#error "The host memory reference supports Linux and Windows only"
#endif

namespace host_example {
// Fixed host arena; acquire/release and OS observation are control-plane only.
// It replaces three supported resident regions, not every Runtime allocation.
class Memory {
    struct alignas(4096) Slot {
        std::array<std::byte, 65536> bytes{};
        std::size_t used = 0;
        bool live = false, applied = false;
        // Keep the page-aligned stride explicit: MSVC /W4 diagnoses implicit
        // alignment padding. No warning suppression or storage-layout change.
        std::array<std::byte, 4096 - sizeof(std::size_t) - 2 * sizeof(bool)> padding{};
    };
    static_assert(sizeof(Slot) == 65536 + 4096 && alignof(Slot) == 4096);
    std::array<Slot, 3> slots_{};
    Slot* find(void* token) noexcept {
        for (auto& slot : slots_) if (&slot == token && slot.live) return &slot;
        return nullptr;
    }
    static bool basic_policy(const rt::MemoryPolicy& p) noexcept {
        return p.page_rounding == rt::PageRounding::none && p.guard_bytes_before == 0 && p.guard_bytes_after == 0 &&
            p.prefault == rt::PolicyToggle::disabled && p.first_touch == rt::FirstTouchPolicy::none &&
            p.locking == rt::PolicyToggle::disabled && p.pinning == rt::PolicyToggle::disabled &&
            p.huge_pages == rt::HugePagePreference::disabled && p.huge_page_fallback == rt::PolicyToggle::disabled &&
            p.rollback == rt::RollbackIntent::release && p.numa_node == -1 &&
            p.residency_verification == rt::PolicyToggle::disabled;
    }
public:
    static constexpr std::size_t region_capacity = 65536;
    std::size_t acquisitions = 0, releases = 0, observations = 0, violations = 0;
    unsigned fail_acquire = 0, fail_apply = 0, fail_observe = 0, fail_rollback = 0;
    Memory() = default;
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    ~Memory() { if (live_count()) std::terminate(); }
    std::size_t live_count() const noexcept {
        std::size_t n = 0; for (const auto& slot : slots_) n += slot.live ? 1u : 0u; return n;
    }
    rt::MemoryProvider table() noexcept {
        rt::MemoryProvider p; p.user_data = this;
        p.capabilities = rt::memory_provider_capability_bit(rt::MemoryProviderCapability::policy_operations) |
                         rt::memory_provider_capability_bit(rt::MemoryProviderCapability::independent_observation);
        p.acquire = &acquire; p.apply = &apply; p.observe = &observe; p.rollback = &rollback; p.release = &release;
        return p;
    }
    static rt::Status acquire(void* opaque, const rt::MemoryProviderAcquireRequest& r, rt::MemoryProviderAllocation& out) noexcept {
        out = {}; if (!opaque) return rt::Status::invalid_argument;
        auto& self = *static_cast<Memory*>(opaque);
        const std::array ids{rt::memory_region_phase_scratch, rt::memory_region_task_scratch, rt::memory_region_trace_storage};
        std::size_t index = ids.size();
        for (std::size_t i = 0; i < ids.size(); ++i) if (r.region == ids[i]) index = i;
        if (index == ids.size() || !r.logical_bytes || !r.required_alignment ||
            (r.required_alignment & (r.required_alignment - 1)) || r.required_alignment > 4096 ||
            r.page_rounding != rt::PageRounding::none || r.guard_bytes_before || r.guard_bytes_after ||
            r.huge_pages != rt::HugePagePreference::disabled || r.huge_page_fallback == rt::PolicyToggle::enabled || r.numa_node != -1 ||
            r.rollback != rt::RollbackIntent::release) return rt::Status::invalid_config;
        if (r.logical_bytes > region_capacity) return rt::Status::resource_exhausted;
        auto& slot = self.slots_[index]; if (slot.live) return rt::Status::invalid_state;
        if (self.fail_acquire && --self.fail_acquire == 0) return rt::Status::resource_exhausted;
        slot.used = r.logical_bytes; slot.live = true; slot.applied = false; ++self.acquisitions;
        out.token = &slot; out.allocation_base = slot.bytes.data(); out.allocation_bytes = slot.bytes.size();
        out.usable_data = slot.bytes.data(); out.usable_bytes = slot.used; out.committed_bytes = slot.used;
        out.alignment = 4096;
        return rt::Status::ok;
    }
    static rt::Status apply(void* opaque, void* token, const rt::MemoryPolicy& p, rt::MemoryProviderObservation& out) noexcept {
        out = {}; if (!opaque) return rt::Status::invalid_argument;
        auto& self = *static_cast<Memory*>(opaque); auto* slot = self.find(token);
        if (!slot || !basic_policy(p)) return rt::Status::invalid_config;
        slot->applied = true; // Retain even a failed apply until rollback.
        if (self.fail_apply && --self.fail_apply == 0) return rt::Status::internal_error;
        return rt::Status::ok;
    }
    static rt::Status observe(void* opaque, void* token, const rt::MemoryPolicy& p, rt::MemoryProviderObservation& out) noexcept {
        out = {}; if (!opaque) return rt::Status::invalid_argument;
        auto& self = *static_cast<Memory*>(opaque); auto* slot = self.find(token);
        if (!slot || !slot->applied || !basic_policy(p)) return rt::Status::invalid_state;
        if (self.fail_observe && --self.fail_observe == 0) return rt::Status::internal_error;
        constexpr std::size_t page = 4096;
        const auto count = (slot->used + page - 1) / page;
        std::array<bool, region_capacity / page> resident{};
#if defined(_WIN32)
        SYSTEM_INFO info{}; GetSystemInfo(&info);
        if (info.dwPageSize != page) return rt::Status::invalid_config;
        std::array<PSAPI_WORKING_SET_EX_INFORMATION, region_capacity / page> pages{};
        for (std::size_t i = 0; i < count; ++i) pages[i].VirtualAddress = slot->bytes.data() + i * page;
        if (!QueryWorkingSetEx(GetCurrentProcess(), pages.data(), static_cast<DWORD>(count * sizeof(pages[0])))) return rt::Status::internal_error;
        for (std::size_t i = 0; i < count; ++i) resident[i] = pages[i].VirtualAttributes.Valid != 0;
#else
        if (sysconf(_SC_PAGESIZE) != static_cast<long>(page)) return rt::Status::invalid_config;
        std::array<unsigned char, region_capacity / page> pages{};
        if (mincore(slot->bytes.data(), slot->used, pages.data()) != 0) return rt::Status::internal_error;
        for (std::size_t i = 0; i < count; ++i) resident[i] = (pages[i] & 1u) != 0;
#endif
        for (std::size_t i = 0; i < count; ++i)
            if (resident[i]) out.resident_bytes += std::min(page, slot->used - i * page);
        out.independently_observed = true; ++self.observations;
        return rt::Status::ok;
    }
    static rt::Status rollback(void* opaque, void* token, const rt::MemoryPolicy&, const rt::MemoryProviderObservation&) noexcept {
        if (!opaque) return rt::Status::invalid_argument;
        auto& self = *static_cast<Memory*>(opaque); auto* slot = self.find(token);
        if (!slot) return rt::Status::invalid_state;
        if (self.fail_rollback && --self.fail_rollback == 0) return rt::Status::internal_error;
        slot->applied = false; return rt::Status::ok;
    }
    static void release(void* opaque, void* token, rt::RollbackIntent intent) noexcept {
        if (!opaque) return;
        auto& self = *static_cast<Memory*>(opaque); auto* slot = self.find(token);
        if (!slot || slot->applied || intent != rt::RollbackIntent::release) { ++self.violations; return; }
        slot->live = false; slot->used = 0; ++self.releases;
    }
    static rt::CpuMemoryPolicy policy() noexcept {
        rt::CpuMemoryPolicy p;
        for (auto id : {rt::memory_region_phase_scratch, rt::memory_region_task_scratch, rt::memory_region_trace_storage}) {
            auto& r = p.memory_policies[p.memory_policy_count++]; r.region = id;
            r.policy.requirement = rt::PolicyRequirement::strict; r.policy.provider = rt::MemoryProviderOwnership::host;
            r.policy.alignment = 64; r.policy.page_rounding = rt::PageRounding::none;
            r.policy.prefault = r.policy.locking = r.policy.pinning = r.policy.huge_page_fallback = r.policy.residency_verification = rt::PolicyToggle::disabled;
            r.policy.first_touch = rt::FirstTouchPolicy::none; r.policy.huge_pages = rt::HugePagePreference::disabled;
            r.policy.rollback = rt::RollbackIntent::release;
        }
        return p;
    }
};
} // namespace host_example
