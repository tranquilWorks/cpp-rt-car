#pragma once
#include <rt/sdk.hpp>
#include <array>
#include <cstdint>

namespace rtfw_unreal {
// Small integer model shared by native checks and the engine sample. One
// simulated tick per host frame; DeltaSeconds is carried by Runtime, not used
// to claim a physical integrator or a real-time rate.
struct Sample {
    std::array<std::int64_t, 16> values{};
    std::int64_t checksum = 0;
    std::uint64_t frames = 0;
    bool fail = false;
    static rt::TaskResult range(void* opaque, const rt::TaskContext&,
                                 const rt::TaskRange& r) noexcept {
        auto& self = *static_cast<Sample*>(opaque);
        for (auto i = r.begin; i < r.end; ++i) self.values[i] += static_cast<std::int64_t>(i + 1);
        return rt::TaskResult::ok;
    }
    rt::CallbackResult integrate(const rt::CallbackContext& context) noexcept {
        if (fail || frames >= 1000000) return rt::CallbackResult::error;
        return context.tasks.parallel_for(values.size(), 4, range, this) == rt::Status::ok
            ? rt::CallbackResult::ok : rt::CallbackResult::error;
    }
    rt::CallbackResult summarize(const rt::CallbackContext&) noexcept {
        checksum = 0; for (const auto value : values) checksum += value;
        ++frames;
        return matches() ? rt::CallbackResult::ok : rt::CallbackResult::error;
    }
    bool matches() const noexcept {
        for (std::size_t i = 0; i < values.size(); ++i)
            if (values[i] != static_cast<std::int64_t>(frames) * static_cast<std::int64_t>(i + 1)) return false;
        return checksum == static_cast<std::int64_t>(frames) * 136;
    }
    static rt::Status configure(void* opaque, rt::Runtime& runtime) noexcept {
        if (!opaque) return rt::Status::invalid_argument;
        auto& self = *static_cast<Sample*>(opaque);
        rt::sdk::GraphBuilder graph(runtime);
        rt::PhaseHandle integrate_phase, summary_phase;
        auto s = graph.phase<&Sample::integrate>("unreal.integrate", self, integrate_phase);
        if (s != rt::Status::ok) return s;
        s = graph.phase<&Sample::summarize>("unreal.summary", self, summary_phase);
        return s == rt::Status::ok ? graph.depends_on(summary_phase, integrate_phase) : s;
    }
};
inline rt::RuntimeConfig configuration() noexcept {
    rt::RuntimeConfig c;
    c.executor_policy = rt::ExecutorPolicy::host_adapter;
    c.worker_count = 2; c.executor_queue_capacity = 32;
    c.callback_capacity = 8; c.scratch_bytes = 64;
    c.task_scratch_slots = 32; c.task_scratch_bytes = 64; c.trace_capacity = 256;
    return c;
}
// Only the existing three resident regions are provided by FMemory. Explicitly
// refuse unsupported physical placement/locking policies instead of inventing
// residency observations from allocator extents.
inline rt::CpuMemoryPolicy memory_policy() noexcept {
    rt::CpuMemoryPolicy p;
    for (const auto region : {rt::memory_region_phase_scratch, rt::memory_region_task_scratch, rt::memory_region_trace_storage}) {
        auto& row = p.memory_policies[p.memory_policy_count++]; row.region = region;
        row.policy.provider = rt::MemoryProviderOwnership::host;
        row.policy.requirement = rt::PolicyRequirement::strict;
        row.policy.alignment = 64; row.policy.page_rounding = rt::PageRounding::none;
        row.policy.prefault = row.policy.locking = row.policy.pinning = row.policy.huge_page_fallback = row.policy.residency_verification = rt::PolicyToggle::disabled;
        row.policy.first_touch = rt::FirstTouchPolicy::none;
        row.policy.huge_pages = rt::HugePagePreference::disabled;
        row.policy.rollback = rt::RollbackIntent::release;
    }
    return p;
}
} // namespace rtfw_unreal
