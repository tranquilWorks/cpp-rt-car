#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>

#include <rt/runtime.hpp>

#include "rt/src/thread_policy.hpp"

#if defined(__linux__)
#include <sys/uio.h>
#include <unistd.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define RTFW_TEST_ADDRESS_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define RTFW_TEST_ADDRESS_SANITIZER 1
#endif
#endif

#if defined(__SANITIZE_THREAD__)
#define RTFW_TEST_THREAD_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define RTFW_TEST_THREAD_SANITIZER 1
#endif
#endif

namespace {

constexpr std::size_t kPageBytes = 4096;
constexpr std::size_t kProviderBytes = 32 * 1024;

enum class EventKind : std::uint32_t {
    acquire = 100,
    apply = 200,
    observe = 300,
    rollback = 400,
    release = 500,
};

std::uint32_t event(EventKind kind, rt::MemoryRegionId region) {
    return static_cast<std::uint32_t>(kind) + region.value;
}

std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

const rt::MemoryPolicyReport* find_memory(
    const rt::CpuMemoryPolicyReport& report,
    rt::MemoryRegionId region) {
    for (std::size_t index = 0; index < report.memory_count; ++index) {
        if (report.memory[index].region == region) {
            return &report.memory[index];
        }
    }
    return nullptr;
}

rt::RuntimeConfig provider_config() {
    rt::RuntimeConfig config;
    config.callback_capacity = 1;
    config.scratch_bytes = 64;
    config.trace_capacity = 8;
    config.worker_count = 1;
    config.executor_queue_capacity = 4;
    config.scratch_alignment = 64;
    config.task_scratch_bytes = 64;
    config.task_scratch_slots = 4;
    config.memory_budget_bytes = 1024 * 1024;
    return config;
}

struct CallbackProbe {
    std::size_t calls = 0;
    std::byte* phase_scratch = nullptr;
};

rt::CallbackResult record_callback(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& probe = *static_cast<CallbackProbe*>(user_data);
    ++probe.calls;
    probe.phase_scratch = context.scratch.data();
    return rt::CallbackResult::ok;
}

enum class MalformedAllocation {
    none,
    undersized,
    misaligned,
    overflowed_extent,
    overlap,
    duplicate_token,
    guard_before_outside_extent,
    guard_after_outside_extent,
    invalid_page_rounding,
    invalid_page_size,
    contradictory_huge_outcome,
};

class FixedProvider final {
public:
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct alignas(kPageBytes) Slot {
        std::array<std::byte, kProviderBytes> bytes{};
        rt::MemoryRegionId region{};
        std::size_t committed = 0;
        std::byte* usable = nullptr;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    rt::MemoryProvider table() noexcept {
        rt::MemoryProvider provider;
        provider.capabilities =
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
        provider.user_data = this;
        provider.acquire = &acquire;
        provider.apply = &apply;
        provider.observe = &observe;
        provider.rollback = &rollback;
        provider.release = &release;
        return provider;
    }

    void clear_events() noexcept {
        event_count = 0;
        events.fill(0);
    }

    std::size_t count(EventKind kind) const noexcept {
        return static_cast<std::size_t>(std::count_if(
            events.begin(),
            events.begin() + static_cast<std::ptrdiff_t>(event_count),
            [&](std::uint32_t value) {
                return value >= static_cast<std::uint32_t>(kind) + 1 &&
                       value <= static_cast<std::uint32_t>(kind) + 12;
            }));
    }

    std::array<Slot, 3> slots{};
    std::array<std::uint32_t, 64> events{};
    std::array<rt::MemoryProviderAcquireRequest, 3> requests{};
    std::array<rt::RollbackIntent, 3> release_intents{};
    std::size_t event_count = 0;
    MalformedAllocation malformed = MalformedAllocation::none;
    rt::MemoryRegionId malformed_region = rt::memory_region_task_scratch;
    rt::MemoryRegionId acquire_failure_region{};
    bool acquire_failure_returns_live_token = false;
    rt::MemoryRegionId apply_failure_region{};
    rt::MemoryRegionId mismatch_region{};
    rt::Runtime* reentrant_runtime = nullptr;
    rt::Status reentrant_status = rt::Status::ok;
    bool reenter_on_release = false;
    std::size_t release_reentry_count = 0;
    std::size_t rollback_failures_remaining = 0;
    bool reentrant_report_available = true;
    std::size_t reentrant_callback_count = std::numeric_limits<std::size_t>::max();

private:
    static FixedProvider& self(void* user_data) noexcept {
        return *static_cast<FixedProvider*>(user_data);
    }

    static std::size_t slot_index(rt::MemoryRegionId region) noexcept {
        return static_cast<std::size_t>(
            region.value - rt::memory_region_phase_scratch.value);
    }

    void record(EventKind kind, rt::MemoryRegionId region) noexcept {
        if (event_count < events.size()) {
            events[event_count++] = event(kind, region);
        }
    }

    static rt::Status acquire(
        void* user_data,
        const rt::MemoryProviderAcquireRequest& request,
        rt::MemoryProviderAllocation& allocation) noexcept {
        auto& provider = self(user_data);
        provider.record(EventKind::acquire, request.region);
        if (provider.reentrant_runtime) {
            provider.reentrant_status =
                provider.reentrant_runtime->set_cpu_memory_policy({});
            rt::CpuMemoryPolicyReport report;
            provider.reentrant_report_available =
                provider.reentrant_runtime->cpu_memory_policy_report(report);
            provider.reentrant_callback_count =
                provider.reentrant_runtime->callback_count();
        }
        if (request.region == provider.acquire_failure_region) {
            if (provider.acquire_failure_returns_live_token) {
                allocation.token = &provider.slots[0];
                allocation.allocation_base = provider.slots[0].bytes.data();
                allocation.allocation_bytes = provider.slots[0].bytes.size();
            }
            allocation.provider_error = 71;
            return rt::Status::resource_exhausted;
        }

        const auto index = slot_index(request.region);
        if (index >= provider.slots.size()) {
            return rt::Status::invalid_argument;
        }
        auto& slot = provider.slots[index];
        provider.requests[index] = request;
        slot.region = request.region;
        const std::size_t committed =
            request.page_rounding == rt::PageRounding::base_page
            ? align_up(request.logical_bytes, kPageBytes)
            : request.logical_bytes;
        const std::size_t before = request.guard_bytes_before == 0
            ? 0
            : align_up(request.guard_bytes_before, kPageBytes);
        const std::size_t after = request.guard_bytes_after == 0
            ? 0
            : align_up(request.guard_bytes_after, kPageBytes);
        const std::size_t usable_offset = align_up(
            before,
            std::max(request.required_alignment, std::size_t{1}));
        if (usable_offset > slot.bytes.size() ||
            committed > slot.bytes.size() - usable_offset ||
            after > slot.bytes.size() - usable_offset - committed) {
            return rt::Status::resource_exhausted;
        }
        slot.committed = committed;
        slot.usable = slot.bytes.data() + usable_offset;

        Slot* token_slot = &slot;
        std::byte* allocation_base = slot.bytes.data();
        std::byte* usable = slot.usable;
        if (request.region == provider.malformed_region) {
            if (provider.malformed == MalformedAllocation::misaligned) {
                ++usable;
            } else if (
                provider.malformed == MalformedAllocation::overflowed_extent) {
                allocation_base = reinterpret_cast<std::byte*>(
                    std::numeric_limits<std::uintptr_t>::max() - 15u);
            } else if (provider.malformed == MalformedAllocation::overlap) {
                allocation_base = provider.slots[0].bytes.data();
                usable = provider.slots[0].bytes.data();
            } else if (
                provider.malformed == MalformedAllocation::duplicate_token) {
                token_slot = &provider.slots[0];
            }
        }

        allocation.token = token_slot;
        allocation.allocation_base = allocation_base;
        allocation.allocation_bytes =
            request.region == provider.malformed_region &&
                provider.malformed == MalformedAllocation::overflowed_extent
            ? 64u
            : usable_offset + committed + after;
        if (request.region == provider.malformed_region &&
            provider.malformed ==
                MalformedAllocation::guard_before_outside_extent) {
            allocation_base = usable;
            allocation.allocation_base = allocation_base;
            allocation.allocation_bytes = committed + after;
        } else if (
            request.region == provider.malformed_region &&
            provider.malformed ==
                MalformedAllocation::guard_after_outside_extent) {
            allocation.allocation_bytes = usable_offset + committed;
        }
        allocation.usable_data = usable;
        allocation.usable_bytes = committed;
        allocation.committed_bytes = committed;
        allocation.alignment = request.required_alignment;
        allocation.actual_page_bytes = kPageBytes;
        allocation.guard_bytes_before = before;
        allocation.guard_bytes_after = after;
        allocation.explicit_huge_pages = false;
        allocation.used_huge_page_fallback =
            request.huge_pages == rt::HugePagePreference::prefer;
        if (request.region == provider.malformed_region &&
            provider.malformed == MalformedAllocation::undersized) {
            allocation.usable_bytes = request.logical_bytes - 1;
        } else if (
            request.region == provider.malformed_region &&
            provider.malformed == MalformedAllocation::invalid_page_rounding) {
            allocation.actual_page_bytes = 0;
        } else if (
            request.region == provider.malformed_region &&
            provider.malformed == MalformedAllocation::invalid_page_size) {
            allocation.actual_page_bytes = 3;
        } else if (
            request.region == provider.malformed_region &&
            provider.malformed ==
                MalformedAllocation::contradictory_huge_outcome) {
            allocation.explicit_huge_pages = true;
        }
        return rt::Status::ok;
    }

    static rt::Status apply(
        void* user_data,
        void* token,
        const rt::MemoryPolicy& resolved,
        rt::MemoryProviderObservation& applied) noexcept {
        auto& provider = self(user_data);
        auto& slot = *static_cast<Slot*>(token);
        provider.record(EventKind::apply, slot.region);
        if (slot.region == provider.apply_failure_region) {
            applied.system_error = 72;
            return rt::Status::internal_error;
        }
        applied.resident_bytes = slot.committed;
        applied.locked_bytes = resolved.locking == rt::PolicyToggle::enabled
            ? slot.committed
            : 0;
        applied.pinned_bytes = resolved.pinning == rt::PolicyToggle::enabled
            ? slot.committed
            : 0;
        applied.numa_node = resolved.numa_node;
        applied.prefaulted = resolved.prefault == rt::PolicyToggle::enabled;
        applied.caller_first_touched =
            resolved.first_touch == rt::FirstTouchPolicy::caller;
        applied.independently_observed = true;
        return rt::Status::ok;
    }

    static rt::Status observe(
        void* user_data,
        void* token,
        const rt::MemoryPolicy& resolved,
        rt::MemoryProviderObservation& observed) noexcept {
        auto& provider = self(user_data);
        auto& slot = *static_cast<Slot*>(token);
        provider.record(EventKind::observe, slot.region);
        observed.resident_bytes = slot.committed;
        observed.locked_bytes = resolved.locking == rt::PolicyToggle::enabled
            ? slot.committed
            : 0;
        observed.pinned_bytes = resolved.pinning == rt::PolicyToggle::enabled
            ? slot.committed
            : 0;
        observed.numa_node = resolved.numa_node;
        observed.prefaulted = resolved.prefault == rt::PolicyToggle::enabled;
        observed.caller_first_touched =
            resolved.first_touch == rt::FirstTouchPolicy::caller;
        observed.independently_observed =
            slot.region != provider.mismatch_region;
        return rt::Status::ok;
    }

    static rt::Status rollback(
        void* user_data,
        void* token,
        const rt::MemoryPolicy&,
        const rt::MemoryProviderObservation&) noexcept {
        auto& provider = self(user_data);
        auto& slot = *static_cast<Slot*>(token);
        provider.record(EventKind::rollback, slot.region);
        if (provider.rollback_failures_remaining != 0) {
            --provider.rollback_failures_remaining;
            return rt::Status::internal_error;
        }
        return rt::Status::ok;
    }

    static void release(
        void* user_data,
        void* token,
        rt::RollbackIntent intent) noexcept {
        auto& provider = self(user_data);
        auto& slot = *static_cast<Slot*>(token);
        provider.record(EventKind::release, slot.region);
        provider.release_intents[slot_index(slot.region)] = intent;
        if (provider.reenter_on_release && provider.reentrant_runtime) {
            ++provider.release_reentry_count;
            provider.reentrant_status = provider.reentrant_runtime->step(
                {0, std::chrono::nanoseconds(1), std::nullopt});
        }
    }
};

struct NestedProbe {
    std::array<std::byte*, 3> scratch{};
    std::size_t calls = 0;
};

rt::TaskResult nested_grandchild(
    void* user_data,
    const rt::TaskContext& context,
    const rt::TaskRange&) {
    auto& probe = *static_cast<NestedProbe*>(user_data);
    probe.scratch[2] = context.scratch().data();
    ++probe.calls;
    return rt::TaskResult::ok;
}

rt::TaskResult nested_child(
    void* user_data,
    const rt::TaskContext& context,
    const rt::TaskRange&) {
    auto& probe = *static_cast<NestedProbe*>(user_data);
    probe.scratch[1] = context.scratch().data();
    ++probe.calls;
    return context.parallel_for(1, 1, &nested_grandchild, &probe) ==
            rt::Status::ok
        ? rt::TaskResult::ok
        : rt::TaskResult::error;
}

rt::CallbackResult nested_phase(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& probe = *static_cast<NestedProbe*>(user_data);
    probe.scratch[0] = context.tasks.scratch().data();
    ++probe.calls;
    return context.tasks.parallel_for(1, 1, &nested_child, &probe) ==
            rt::Status::ok
        ? rt::CallbackResult::ok
        : rt::CallbackResult::error;
}

struct InlineHost {
    std::array<rt::HostExecutorJob, 16> jobs{};
    std::size_t head = 0;
    std::size_t tail = 0;
    std::size_t size = 0;

    static rt::Status submit(
        void* user_data,
        const rt::HostExecutorJob& job) noexcept {
        auto& host = *static_cast<InlineHost*>(user_data);
        if (host.size == host.jobs.size()) {
            return rt::Status::queue_full;
        }
        host.jobs[host.tail] = job;
        host.tail = (host.tail + 1) % host.jobs.size();
        ++host.size;
        return rt::Status::ok;
    }

    static bool try_execute_one(void* user_data) noexcept {
        auto& host = *static_cast<InlineHost*>(user_data);
        if (host.size == 0) {
            return false;
        }
        const auto job = host.jobs[host.head];
        host.head = (host.head + 1) % host.jobs.size();
        --host.size;
        job.execute(
            job.execution_context,
            job.completion_context,
            job.completion_token,
            0);
        return true;
    }
};

class FailingExecutorThreadProvider final
    : public rt::detail::ThreadPolicyProvider {
public:
    [[nodiscard]] rt::Status resolve(
        rt::ThreadRoleId,
        rt::PolicyApplicationMode,
        bool active,
        bool,
        bool,
        const rt::ThreadPolicy& requested,
        const rt::ThreadPolicy& role_default,
        rt::ThreadPolicy& resolved,
        rt::PolicyResolutionState& resolution,
        std::int32_t& system_error) noexcept override {
        resolved = role_default;
        resolved.requirement = requested.requirement;
        resolution = active
            ? rt::PolicyResolutionState::native_supported
            : rt::PolicyResolutionState::inactive;
        system_error = 0;
        return rt::Status::ok;
    }

    [[nodiscard]] rt::Status before_create(
        rt::ThreadRoleId role,
        std::size_t instance,
        const rt::detail::ThreadRolePlan&,
        std::int32_t& system_error) noexcept override {
        if (fail_before_create && role == fail_role &&
            instance == failure_instance) {
            system_error = ENOMEM;
            return rt::Status::resource_exhausted;
        }
        system_error = 0;
        return rt::Status::ok;
    }

    void apply_and_verify_current(
        rt::ThreadRoleId role,
        std::size_t,
        const rt::detail::ThreadRolePlan& plan,
        rt::detail::ThreadStartupResult& result) noexcept override {
        result.read_back = plan.resolved;
        result.applied = rt::PolicyOperationState::succeeded;
        result.verified = rt::PolicyOperationState::succeeded;
        if (role == fail_role && fail_executor) {
            result.verified = rt::PolicyOperationState::mismatched;
            result.verify_error = 73;
        }
    }

    void verify_current(
        rt::ThreadRoleId,
        const rt::detail::ThreadRolePlan& plan,
        rt::detail::ThreadStartupResult& result) noexcept override {
        result.read_back = plan.resolved;
        result.applied = rt::PolicyOperationState::not_attempted;
        result.verified = rt::PolicyOperationState::succeeded;
    }

    [[nodiscard]] rt::Status cleanup_stack_current(
        rt::ThreadRoleId role,
        std::size_t instance,
        const rt::detail::ThreadRolePlan& plan,
        rt::detail::ThreadStartupResult& result) noexcept override {
        if (role == rt::thread_role_executor_worker &&
            instance == cleanup_failure_instance) {
            ++cleanup_attempt_count;
            if (cleanup_failures_remaining != 0) {
                --cleanup_failures_remaining;
                result.stack_cleanup = rt::PolicyOperationState::failed;
                result.stack_cleanup_error = EIO;
                return rt::Status::internal_error;
            }
        }
        return ThreadPolicyProvider::cleanup_stack_current(
            role,
            instance,
            plan,
            result);
    }

    void after_join(rt::ThreadRoleId, std::size_t) noexcept override {
        if (memory_provider) {
            rollback_count_seen_at_join =
                memory_provider->count(EventKind::rollback);
        }
        ++join_count;
    }

    bool fail_executor = true;
    bool fail_before_create = false;
    rt::ThreadRoleId fail_role = rt::thread_role_executor_worker;
    std::size_t failure_instance = 0;
    std::size_t cleanup_failure_instance = 0;
    std::size_t cleanup_failures_remaining = 0;
    std::size_t cleanup_attempt_count = 0;
    FixedProvider* memory_provider = nullptr;
    std::size_t rollback_count_seen_at_join =
        std::numeric_limits<std::size_t>::max();
    std::size_t join_count = 0;
};

rt::CpuMemoryPolicy strict_resident_policy() {
    rt::CpuMemoryPolicy policy;
    const std::array regions{
        rt::memory_region_phase_scratch,
        rt::memory_region_task_scratch,
        rt::memory_region_trace_storage,
    };
    for (const auto region : regions) {
        auto& request =
            policy.memory_policies[policy.memory_policy_count++];
        request.region = region;
        request.policy.requirement = rt::PolicyRequirement::strict;
        request.policy.provider = rt::MemoryProviderOwnership::host;
        request.policy.page_rounding = rt::PageRounding::base_page;
        request.policy.prefault = rt::PolicyToggle::enabled;
        request.policy.locking = rt::PolicyToggle::enabled;
        request.policy.pinning = rt::PolicyToggle::enabled;
        request.policy.huge_pages = rt::HugePagePreference::prefer;
        request.policy.huge_page_fallback = rt::PolicyToggle::enabled;
        request.policy.numa_node = 2;
        request.policy.first_touch = rt::FirstTouchPolicy::caller;
        request.policy.residency_verification = rt::PolicyToggle::enabled;
        request.policy.rollback = rt::RollbackIntent::release;
    }
    policy.memory_policies[0].policy.guard_bytes_before = kPageBytes;
    policy.memory_policies[0].policy.guard_bytes_after = kPageBytes;
    return policy;
}

void configure_provider_runtime(
    rt::Runtime& runtime,
    FixedProvider& provider,
    CallbackProbe* callback = nullptr,
    const rt::CpuMemoryPolicy* policy = nullptr) {
    ASSERT_EQ(runtime.configure(provider_config()), rt::Status::ok);
    if (callback) {
        ASSERT_EQ(
            runtime.register_callback(
                {"memory.provider", &record_callback, callback}),
            rt::Status::ok);
    }
    auto table = provider.table();
    ASSERT_EQ(runtime.set_memory_provider(table), rt::Status::ok);
    if (policy) {
        ASSERT_EQ(runtime.set_cpu_memory_policy(*policy), rt::Status::ok);
    }
}

} // namespace

TEST(MemoryPolicy, ProviderTableValidatesCopiesAndRejectsReentrancy) {
    const auto expect_invalid = [](rt::MemoryProvider provider) {
        rt::Runtime runtime;
        ASSERT_EQ(runtime.set_memory_provider(provider), rt::Status::ok);
        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
        EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
    };

    FixedProvider invalid_fixture;
    auto invalid = invalid_fixture.table();
    invalid.struct_size = sizeof(invalid) - 1;
    expect_invalid(invalid);
    invalid = invalid_fixture.table();
    ++invalid.api_version;
    expect_invalid(invalid);
    invalid = invalid_fixture.table();
    invalid.reserved[0] = 1;
    expect_invalid(invalid);
    invalid = invalid_fixture.table();
    invalid.observe = nullptr;
    expect_invalid(invalid);
    invalid = invalid_fixture.table();
    invalid.capabilities &= ~rt::memory_provider_capability_bit(
        rt::MemoryProviderCapability::independent_observation);
    expect_invalid(invalid);

    FixedProvider provider;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(provider_config()), rt::Status::ok);
    CallbackProbe callback;
    ASSERT_EQ(
        runtime.register_callback(
            {"memory.copy", &record_callback, &callback}),
        rt::Status::ok);
    auto copied = provider.table();
    ASSERT_EQ(runtime.set_memory_provider(copied), rt::Status::ok);
    provider.reentrant_runtime = &runtime;
    copied.user_data = nullptr;
    copied.acquire = nullptr;
    copied.release = nullptr;
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    EXPECT_EQ(provider.reentrant_status, rt::Status::invalid_state);
    EXPECT_FALSE(provider.reentrant_report_available);
    EXPECT_EQ(provider.reentrant_callback_count, 0u);
    EXPECT_EQ(provider.count(EventKind::acquire), 3u);
    EXPECT_EQ(runtime.set_memory_provider(provider.table()), rt::Status::invalid_state);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(provider.count(EventKind::release), 3u);
}

TEST(MemoryPolicy, AcquiresStableOrderAndReleasesReverseExactlyOnce) {
    FixedProvider provider;
    rt::Runtime runtime;
    CallbackProbe callback;
    const auto policy = strict_resident_policy();
    configure_provider_runtime(runtime, provider, &callback, &policy);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    const std::array expected_acquire{
        event(EventKind::acquire, rt::memory_region_phase_scratch),
        event(EventKind::acquire, rt::memory_region_task_scratch),
        event(EventKind::acquire, rt::memory_region_trace_storage),
    };
    ASSERT_EQ(provider.event_count, expected_acquire.size());
    EXPECT_TRUE(std::equal(
        expected_acquire.begin(), expected_acquire.end(),
        provider.events.begin()));

    ASSERT_EQ(runtime.start(), rt::Status::ok);
    const auto events_before_step = provider.event_count;
    ASSERT_EQ(
        runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(callback.calls, 1u);
    EXPECT_EQ(provider.event_count, events_before_step);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);

    const std::array expected_tail{
        event(EventKind::rollback, rt::memory_region_trace_storage),
        event(EventKind::rollback, rt::memory_region_task_scratch),
        event(EventKind::rollback, rt::memory_region_phase_scratch),
        event(EventKind::release, rt::memory_region_trace_storage),
        event(EventKind::release, rt::memory_region_task_scratch),
        event(EventKind::release, rt::memory_region_phase_scratch),
    };
    ASSERT_GE(provider.event_count, expected_tail.size());
    EXPECT_TRUE(std::equal(
        expected_tail.begin(), expected_tail.end(),
        provider.events.begin() + static_cast<std::ptrdiff_t>(
            provider.event_count - expected_tail.size())));
    EXPECT_EQ(provider.count(EventKind::rollback), 3u);
    EXPECT_EQ(provider.count(EventKind::release), 3u);
}

TEST(MemoryPolicy, ReleaseCallbackCannotReenterRuntime) {
    FixedProvider provider;
    rt::Runtime runtime;
    CallbackProbe callback;
    configure_provider_runtime(runtime, provider, &callback);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    provider.reentrant_runtime = &runtime;
    provider.reenter_on_release = true;

    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(provider.release_reentry_count, 3u);
    EXPECT_EQ(provider.reentrant_status, rt::Status::invalid_state);
    EXPECT_EQ(callback.calls, 0u);
    EXPECT_EQ(provider.count(EventKind::release), 3u);
}

TEST(MemoryPolicy, RollbackFailureRetainsTokensUntilCheckedStopRetry) {
    FixedProvider provider;
    rt::Runtime runtime;
    CallbackProbe callback;
    configure_provider_runtime(runtime, provider, &callback);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    provider.rollback_failures_remaining = 3;

    EXPECT_EQ(runtime.stop(), rt::Status::internal_error);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::running);
    EXPECT_EQ(provider.count(EventKind::rollback), 3u);
    EXPECT_EQ(provider.count(EventKind::release), 0u);
    EXPECT_EQ(
        runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
        rt::Status::invalid_state);
    rt::CpuMemoryPolicyReport failed_report;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(failed_report));
    for (const auto region : {
             rt::memory_region_phase_scratch,
             rt::memory_region_task_scratch,
             rt::memory_region_trace_storage}) {
        const auto* row = find_memory(failed_report, region);
        ASSERT_NE(row, nullptr);
        EXPECT_NE(row->rollback_error, 0);
    }

    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::stopped);
    EXPECT_EQ(provider.count(EventKind::rollback), 6u);
    EXPECT_EQ(provider.count(EventKind::release), 3u);
}

TEST(MemoryPolicy, PartialRollbackRetriesOnlyTheUnresolvedRegion) {
    FixedProvider provider;
    rt::Runtime runtime;
    CallbackProbe callback;
    configure_provider_runtime(runtime, provider, &callback);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    provider.clear_events();
    provider.rollback_failures_remaining = 1;

    EXPECT_EQ(runtime.stop(), rt::Status::internal_error);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::running);
    const std::array first_attempt{
        event(EventKind::rollback, rt::memory_region_trace_storage),
        event(EventKind::rollback, rt::memory_region_task_scratch),
        event(EventKind::rollback, rt::memory_region_phase_scratch),
    };
    ASSERT_EQ(provider.event_count, first_attempt.size());
    EXPECT_TRUE(std::equal(
        first_attempt.begin(),
        first_attempt.end(),
        provider.events.begin()));
    EXPECT_EQ(provider.count(EventKind::release), 0u);

    rt::RuntimeTraceCursor cursor;
    std::array<rt::RuntimeTraceEvent, 64> trace{};
    rt::RuntimeTraceReadResult trace_result;
    ASSERT_EQ(
        runtime.read_trace(cursor, trace, trace_result),
        rt::Status::ok);
    EXPECT_EQ(
        std::count_if(
            trace.begin(),
            trace.begin() + static_cast<std::ptrdiff_t>(
                trace_result.events_read),
            [](const rt::RuntimeTraceEvent& entry) {
                return entry.type == rt::RuntimeTraceEventType::stopped;
            }),
        0);

    provider.clear_events();
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::stopped);
    const std::array retry_and_release{
        event(EventKind::rollback, rt::memory_region_trace_storage),
        event(EventKind::release, rt::memory_region_trace_storage),
        event(EventKind::release, rt::memory_region_task_scratch),
        event(EventKind::release, rt::memory_region_phase_scratch),
    };
    ASSERT_EQ(provider.event_count, retry_and_release.size());
    EXPECT_TRUE(std::equal(
        retry_and_release.begin(),
        retry_and_release.end(),
        provider.events.begin()));

    provider.clear_events();
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(provider.event_count, 0u);
}

TEST(MemoryPolicy, AcquisitionFailureReleasesCompletedTokensInReverse) {
    const std::array failure_regions{
        rt::memory_region_phase_scratch,
        rt::memory_region_task_scratch,
        rt::memory_region_trace_storage,
    };
    for (std::size_t failed_index = 0;
         failed_index < failure_regions.size();
         ++failed_index) {
        SCOPED_TRACE(failed_index);
        FixedProvider provider;
        provider.acquire_failure_region = failure_regions[failed_index];
        rt::Runtime runtime;
        CallbackProbe callback;
        configure_provider_runtime(runtime, provider, &callback);
        EXPECT_EQ(runtime.finalize(), rt::Status::resource_exhausted);
        EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
        rt::CpuMemoryPolicyReport report;
        EXPECT_FALSE(runtime.cpu_memory_policy_report(report));
        EXPECT_EQ(callback.calls, 0u);

        std::array<std::uint32_t, 6> expected{};
        std::size_t expected_count = 0;
        for (std::size_t index = 0; index <= failed_index; ++index) {
            expected[expected_count++] =
                event(EventKind::acquire, failure_regions[index]);
        }
        for (std::size_t index = failed_index; index != 0; --index) {
            expected[expected_count++] =
                event(EventKind::release, failure_regions[index - 1]);
        }
        ASSERT_EQ(provider.event_count, expected_count);
        EXPECT_TRUE(std::equal(
            expected.begin(),
            expected.begin() + static_cast<std::ptrdiff_t>(expected_count),
            provider.events.begin()));

        provider.acquire_failure_region = {};
        provider.clear_events();
        ASSERT_EQ(runtime.finalize(), rt::Status::ok);
        EXPECT_EQ(runtime.stop(), rt::Status::ok);
        EXPECT_EQ(provider.count(EventKind::release), 3u);
    }
}

TEST(MemoryPolicy, FailedAcquireCannotReleaseAnotherRuntimesLiveToken) {
    FixedProvider provider;
    rt::Runtime first;
    rt::Runtime second;
    CallbackProbe first_callback;
    CallbackProbe second_callback;
    configure_provider_runtime(first, provider, &first_callback);
    configure_provider_runtime(second, provider, &second_callback);
    ASSERT_EQ(first.finalize(), rt::Status::ok);
    const auto releases_before = provider.count(EventKind::release);

    provider.acquire_failure_region = rt::memory_region_phase_scratch;
    provider.acquire_failure_returns_live_token = true;
    EXPECT_EQ(second.finalize(), rt::Status::resource_exhausted);
    EXPECT_EQ(second.state(), rt::RuntimeState::configuring);
    EXPECT_EQ(provider.count(EventKind::release), releases_before);

    ASSERT_EQ(first.start(), rt::Status::ok);
    ASSERT_EQ(
        first.step({0, std::chrono::nanoseconds(1), std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(first_callback.calls, 1u);
    EXPECT_EQ(first.stop(), rt::Status::ok);
    EXPECT_EQ(provider.count(EventKind::release), releases_before + 3u);
}

TEST(MemoryPolicy, RejectsMalformedProviderAllocationsWithoutPublishing) {
    for (const auto malformed : {
             MalformedAllocation::undersized,
             MalformedAllocation::misaligned,
             MalformedAllocation::overflowed_extent,
             MalformedAllocation::overlap,
             MalformedAllocation::duplicate_token,
             MalformedAllocation::contradictory_huge_outcome}) {
        SCOPED_TRACE(static_cast<unsigned>(malformed));
        FixedProvider provider;
        provider.malformed = malformed;
        rt::Runtime runtime;
        CallbackProbe callback;
        configure_provider_runtime(runtime, provider, &callback);
        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
        EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
        EXPECT_EQ(callback.calls, 0u);
        rt::CpuMemoryPolicyReport report;
        EXPECT_FALSE(runtime.cpu_memory_policy_report(report));
        EXPECT_EQ(provider.count(EventKind::acquire), 2u);
        EXPECT_EQ(
            provider.count(EventKind::release),
            malformed == MalformedAllocation::duplicate_token ? 1u : 2u);
    }
}

TEST(MemoryPolicy, RejectsMalformedPageAndHugeOutcomes) {
    for (const auto malformed : {
             MalformedAllocation::invalid_page_rounding,
             MalformedAllocation::invalid_page_size,
             MalformedAllocation::contradictory_huge_outcome}) {
        SCOPED_TRACE(static_cast<unsigned>(malformed));
        FixedProvider provider;
        provider.malformed = malformed;
        rt::Runtime runtime;
        CallbackProbe callback;
        const auto policy = strict_resident_policy();
        configure_provider_runtime(runtime, provider, &callback, &policy);

        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
        EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
        EXPECT_EQ(callback.calls, 0u);
        rt::CpuMemoryPolicyReport report;
        EXPECT_FALSE(runtime.cpu_memory_policy_report(report));
        EXPECT_EQ(provider.count(EventKind::acquire), 2u);
        EXPECT_EQ(provider.count(EventKind::release), 2u);
    }
}

TEST(MemoryPolicy, RejectsGuardClaimsOutsideAllocationExtent) {
    for (const auto malformed : {
             MalformedAllocation::guard_before_outside_extent,
             MalformedAllocation::guard_after_outside_extent}) {
        SCOPED_TRACE(static_cast<unsigned>(malformed));
        FixedProvider provider;
        provider.malformed = malformed;
        provider.malformed_region = rt::memory_region_phase_scratch;
        rt::Runtime runtime;
        CallbackProbe callback;
        const auto policy = strict_resident_policy();
        configure_provider_runtime(runtime, provider, &callback, &policy);

        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
        EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
        EXPECT_EQ(callback.calls, 0u);
        rt::CpuMemoryPolicyReport report;
        EXPECT_FALSE(runtime.cpu_memory_policy_report(report));
        EXPECT_EQ(provider.count(EventKind::acquire), 1u);
        EXPECT_EQ(provider.count(EventKind::release), 1u);
    }
}

TEST(MemoryPolicy, RejectsCapabilityMismatchBeforePublishing) {
    FixedProvider provider;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(provider_config()), rt::Status::ok);
    auto table = provider.table();
    table.capabilities &= ~rt::memory_provider_capability_bit(
        rt::MemoryProviderCapability::guard_pages);
    ASSERT_EQ(runtime.set_memory_provider(table), rt::Status::ok);
    const auto policy = strict_resident_policy();
    ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
    rt::CpuMemoryPolicyReport report;
    EXPECT_FALSE(runtime.cpu_memory_policy_report(report));
    EXPECT_EQ(provider.event_count, 0u);
}

TEST(MemoryPolicy, InactiveRowsDoNotInvokeProvider) {
    FixedProvider provider;
    rt::Runtime runtime;
    auto config = provider_config();
    config.trace_capacity = 0;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(runtime.set_memory_provider(provider.table()), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    EXPECT_EQ(provider.event_count, 1u);
    EXPECT_EQ(
        provider.events[0],
        event(EventKind::acquire, rt::memory_region_task_scratch));
    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    const auto* phase = find_memory(report, rt::memory_region_phase_scratch);
    const auto* trace = find_memory(report, rt::memory_region_trace_storage);
    ASSERT_NE(phase, nullptr);
    ASSERT_NE(trace, nullptr);
    EXPECT_EQ(phase->resolution, rt::PolicyResolutionState::inactive);
    EXPECT_EQ(trace->resolution, rt::PolicyResolutionState::inactive);
    EXPECT_EQ(phase->acquired, rt::PolicyOperationState::not_attempted);
    EXPECT_EQ(trace->acquired, rt::PolicyOperationState::not_attempted);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(provider.count(EventKind::release), 1u);
}

TEST(MemoryPolicy, StrictClosureDoesNotDoubleCountProviderStorage) {
    FixedProvider provider;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(provider_config()), rt::Status::ok);
    ASSERT_EQ(runtime.set_memory_provider(provider.table()), rt::Status::ok);
    rt::CpuMemoryPolicy policy;
    policy.accounting_requirement = rt::PolicyRequirement::strict;
    policy.accounting_declaration_count = 1;
    policy.accounting_declarations[0] = {
        rt::thread_resource_accounting_key(rt::thread_role_frame),
        1,
        8u * 1024u * 1024u,
    };
    ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    const auto* host_provider = find_memory(
        report,
        rt::memory_region_host_provider);
    ASSERT_NE(host_provider, nullptr);
    EXPECT_EQ(host_provider->logical_region_count, 0u);
    EXPECT_EQ(host_provider->accounted_bytes, 0u);
    EXPECT_EQ(
        host_provider->accounting_exactness,
        rt::ResourceAccountingExactness::not_applicable);

    const auto start_status = runtime.start();
#if defined(__linux__)
    ASSERT_EQ(start_status, rt::Status::ok);
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    EXPECT_TRUE(report.accounting_complete);
#else
    EXPECT_EQ(start_status, rt::Status::invalid_config);
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    EXPECT_FALSE(report.accounting_complete);
#endif
    EXPECT_EQ(runtime.stop(), rt::Status::ok);

    FixedProvider rejected_provider;
    rt::Runtime rejected;
    ASSERT_EQ(rejected.configure(provider_config()), rt::Status::ok);
    ASSERT_EQ(
        rejected.set_memory_provider(rejected_provider.table()),
        rt::Status::ok);
    rt::CpuMemoryPolicy invalid;
    invalid.accounting_declaration_count = 1;
    invalid.accounting_declarations[0] = {
        rt::memory_resource_accounting_key(
            rt::memory_region_host_provider),
        1,
        kProviderBytes,
    };
    ASSERT_EQ(rejected.set_cpu_memory_policy(invalid), rt::Status::ok);
    EXPECT_EQ(rejected.finalize(), rt::Status::invalid_config);
    EXPECT_EQ(rejected_provider.event_count, 0u);
}

TEST(MemoryPolicy, StrictDeferredRegionFailsBeforeProviderCallbacks) {
    const std::array deferred_regions{
        rt::memory_region_runtime_control,
        rt::memory_region_executor_control,
        rt::memory_region_device_control,
        rt::memory_region_registered_state,
        rt::memory_region_backend_control,
        rt::memory_region_registered_device_buffer,
        rt::memory_region_runtime_thread_stack,
        rt::memory_region_external_thread_stack,
        rt::memory_region_host_provider,
    };
    for (const auto region : deferred_regions) {
        SCOPED_TRACE(region.value);
        FixedProvider provider;
        rt::Runtime runtime;
        ASSERT_EQ(runtime.configure(provider_config()), rt::Status::ok);
        ASSERT_EQ(runtime.set_memory_provider(provider.table()), rt::Status::ok);
        rt::CpuMemoryPolicy policy;
        policy.memory_policy_count = 1;
        policy.memory_policies[0].region = region;
        policy.memory_policies[0].policy.requirement =
            rt::PolicyRequirement::strict;
        policy.memory_policies[0].policy.prefault = rt::PolicyToggle::enabled;
        ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
        EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
        EXPECT_EQ(provider.event_count, 0u);
    }
}

TEST(MemoryPolicy, NativeNumaRequiresIndependentProviderObservation) {
    rt::CpuMemoryPolicy policy;
    policy.memory_policy_count = 1;
    policy.memory_policies[0].region = rt::memory_region_phase_scratch;
    policy.memory_policies[0].policy.requirement =
        rt::PolicyRequirement::strict;
    policy.memory_policies[0].policy.numa_node = 0;

    rt::Runtime strict_runtime;
    ASSERT_EQ(strict_runtime.configure(provider_config()), rt::Status::ok);
    CallbackProbe strict_callback;
    ASSERT_EQ(
        strict_runtime.register_callback(
            {"memory.strict-numa", &record_callback, &strict_callback}),
        rt::Status::ok);
    ASSERT_EQ(
        strict_runtime.set_cpu_memory_policy(policy),
        rt::Status::ok);
    EXPECT_EQ(strict_runtime.finalize(), rt::Status::invalid_config);
    EXPECT_EQ(strict_runtime.state(), rt::RuntimeState::configuring);
    EXPECT_EQ(strict_callback.calls, 0u);

    policy.memory_policies[0].policy.requirement =
        rt::PolicyRequirement::best_effort;
    rt::Runtime best_effort_runtime;
    ASSERT_EQ(
        best_effort_runtime.configure(provider_config()),
        rt::Status::ok);
    CallbackProbe best_effort_callback;
    ASSERT_EQ(
        best_effort_runtime.register_callback(
            {"memory.best-effort-numa",
             &record_callback,
             &best_effort_callback}),
        rt::Status::ok);
    ASSERT_EQ(
        best_effort_runtime.set_cpu_memory_policy(policy),
        rt::Status::ok);
    ASSERT_EQ(best_effort_runtime.finalize(), rt::Status::ok);
    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(best_effort_runtime.cpu_memory_policy_report(report));
    const auto* row = find_memory(
        report,
        rt::memory_region_phase_scratch);
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(
        row->resolution,
        rt::PolicyResolutionState::native_best_effort_fallback);
    EXPECT_EQ(row->resolved.numa_node, -1);
    EXPECT_EQ(best_effort_runtime.stop(), rt::Status::ok);
}

TEST(MemoryPolicy, ReportsRequestedCommittedAndVerifiedProviderState) {
    FixedProvider provider;
    rt::Runtime runtime;
    CallbackProbe callback;
    const auto policy = strict_resident_policy();
    configure_provider_runtime(runtime, provider, &callback, &policy);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    const auto* phase = find_memory(report, rt::memory_region_phase_scratch);
    const auto* task = find_memory(report, rt::memory_region_task_scratch);
    const auto* trace = find_memory(report, rt::memory_region_trace_storage);
    ASSERT_NE(phase, nullptr);
    ASSERT_NE(task, nullptr);
    ASSERT_NE(trace, nullptr);
    for (const auto* row : {phase, task, trace}) {
        EXPECT_EQ(row->resolution, rt::PolicyResolutionState::native_supported);
        EXPECT_EQ(row->resolved.provider, rt::MemoryProviderOwnership::host);
        EXPECT_EQ(row->acquired, rt::PolicyOperationState::succeeded);
        EXPECT_EQ(row->applied, rt::PolicyOperationState::succeeded);
        EXPECT_EQ(row->verified, rt::PolicyOperationState::succeeded);
        EXPECT_EQ(row->committed_bytes, kPageBytes);
        EXPECT_EQ(row->resident_bytes, row->committed_bytes);
        EXPECT_EQ(row->locked_bytes, row->committed_bytes);
        EXPECT_EQ(row->pinned_bytes, row->committed_bytes);
        EXPECT_EQ(row->actual_page_bytes, kPageBytes);
        EXPECT_FALSE(row->used_explicit_huge_pages);
        EXPECT_TRUE(row->used_huge_page_fallback);
        EXPECT_EQ(row->provider_error, 0);
        EXPECT_EQ(row->apply_error, 0);
        EXPECT_EQ(row->verify_error, 0);
        EXPECT_EQ(row->resolved.alignment, 64u);
    }
    EXPECT_EQ(phase->actual_guard_bytes_before, kPageBytes);
    EXPECT_EQ(phase->actual_guard_bytes_after, kPageBytes);
    const std::array rows{phase, task, trace};
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& request = provider.requests[index];
        EXPECT_EQ(request.region, rows[index]->region);
        EXPECT_EQ(request.logical_bytes, rows[index]->accounted_bytes);
        EXPECT_EQ(request.required_alignment, 64u);
        EXPECT_EQ(request.page_rounding, rt::PageRounding::base_page);
        EXPECT_EQ(request.huge_pages, rt::HugePagePreference::prefer);
        EXPECT_EQ(request.huge_page_fallback, rt::PolicyToggle::enabled);
        EXPECT_EQ(request.numa_node, 2);
        EXPECT_EQ(request.rollback, rt::RollbackIntent::release);
    }

    rt::MemoryPlan plan;
    ASSERT_TRUE(runtime.memory_plan(plan));
    std::size_t planned_sum = 0;
    for (std::size_t index = 0; index < report.memory_count; ++index) {
        if (report.memory[index].accounting_scope ==
            rt::MemoryAccountingScope::planned) {
            planned_sum += report.memory[index].accounted_bytes;
        }
    }
    EXPECT_EQ(planned_sum, plan.planned_bytes);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(MemoryPolicy, RejectsHugeFallbackWhenRequestForbidsIt) {
    FixedProvider provider;
    rt::Runtime runtime;
    CallbackProbe callback;
    auto policy = strict_resident_policy();
    for (std::size_t index = 0; index < policy.memory_policy_count; ++index) {
        policy.memory_policies[index].policy.huge_page_fallback =
            rt::PolicyToggle::disabled;
    }
    configure_provider_runtime(runtime, provider, &callback, &policy);
    EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
    EXPECT_EQ(provider.count(EventKind::acquire), 1u);
    EXPECT_EQ(provider.count(EventKind::release), 1u);
}

TEST(MemoryPolicy, SharedProviderCannotAliasTwoRuntimes) {
    FixedProvider provider;
    rt::Runtime first;
    rt::Runtime second;
    CallbackProbe first_callback;
    CallbackProbe second_callback;
    configure_provider_runtime(first, provider, &first_callback);
    configure_provider_runtime(second, provider, &second_callback);
    ASSERT_EQ(first.finalize(), rt::Status::ok);
    const auto releases_before = provider.count(EventKind::release);
    EXPECT_EQ(second.finalize(), rt::Status::invalid_config);
    EXPECT_EQ(second.state(), rt::RuntimeState::configuring);
    EXPECT_EQ(provider.count(EventKind::release), releases_before);
    ASSERT_EQ(first.start(), rt::Status::ok);
    EXPECT_EQ(first.stop(), rt::Status::ok);
    EXPECT_EQ(provider.count(EventKind::release), 3u);
}

TEST(MemoryPolicy, ProviderBackedNestedAndHostAdapterScratchRemainDistinct) {
    for (const auto policy : {
             rt::ExecutorPolicy::static_deterministic,
             rt::ExecutorPolicy::host_adapter}) {
        SCOPED_TRACE(static_cast<unsigned>(policy));
        FixedProvider provider;
        InlineHost host;
        rt::Runtime runtime;
        auto config = provider_config();
        config.executor_policy = policy;
        config.task_scratch_slots = 8;
        ASSERT_EQ(runtime.configure(config), rt::Status::ok);
        if (policy == rt::ExecutorPolicy::host_adapter) {
            ASSERT_EQ(
                runtime.set_host_executor({
                    &host,
                    1,
                    config.executor_queue_capacity,
                    &InlineHost::submit,
                    &InlineHost::try_execute_one,
                }),
                rt::Status::ok);
        }
        ASSERT_EQ(runtime.set_memory_provider(provider.table()), rt::Status::ok);
        NestedProbe probe;
        ASSERT_EQ(
            runtime.register_callback({"memory.nested", &nested_phase, &probe}),
            rt::Status::ok);
        ASSERT_EQ(runtime.finalize(), rt::Status::ok);
        ASSERT_EQ(runtime.start(), rt::Status::ok);
        ASSERT_EQ(
            runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
            rt::Status::ok);
        EXPECT_EQ(probe.calls, 3u);
        EXPECT_NE(probe.scratch[0], probe.scratch[1]);
        EXPECT_NE(probe.scratch[0], probe.scratch[2]);
        EXPECT_NE(probe.scratch[1], probe.scratch[2]);
        for (auto* pointer : probe.scratch) {
            ASSERT_NE(pointer, nullptr);
            EXPECT_EQ(reinterpret_cast<std::uintptr_t>(pointer) % 64u, 0u);
            const auto address = reinterpret_cast<std::uintptr_t>(pointer);
            const auto begin = reinterpret_cast<std::uintptr_t>(
                provider.slots[1].usable);
            EXPECT_GE(address, begin);
            EXPECT_LT(address, begin + provider.slots[1].committed);
        }
        EXPECT_EQ(runtime.stop(), rt::Status::ok);
    }
}

TEST(MemoryPolicy, ProviderBackedPeriodicWatchdogPreservesTraceLoss) {
    FixedProvider provider;
    rt::Runtime runtime;
    auto config = provider_config();
    config.trace_capacity = 4;
    config.watchdog_timeout_ns = 60'000'000'000ull;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(runtime.set_memory_provider(provider.table()), rt::Status::ok);
    CallbackProbe callback;
    ASSERT_EQ(
        runtime.register_callback({"memory.periodic", &record_callback, &callback}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    rt::RuntimeTraceCursor cursor;
    std::array<rt::RuntimeTraceEvent, 4> initial_events{};
    rt::RuntimeTraceReadResult initial_trace;
    ASSERT_EQ(
        runtime.read_trace(cursor, initial_events, initial_trace),
        rt::Status::ok);
    rt::PeriodicRunConfig periodic;
    periodic.frame_count = 3;
    periodic.period = std::chrono::nanoseconds(1);
    periodic.first_release_ns = runtime.now_ns();
    periodic.relative_deadline = std::chrono::seconds(1);
    rt::PeriodicRunResult periodic_result;
    ASSERT_EQ(
        runtime.run_periodic(periodic, nullptr, nullptr, &periodic_result),
        rt::Status::ok);
    EXPECT_EQ(callback.calls, 3u);
    EXPECT_EQ(periodic_result.frames_executed, 3u);
    std::array<rt::RuntimeTraceEvent, 4> events{};
    rt::RuntimeTraceReadResult trace_result;
    ASSERT_EQ(runtime.read_trace(cursor, events, trace_result), rt::Status::ok);
    EXPECT_GT(trace_result.lost_events, 0u);
    EXPECT_GT(trace_result.events_read, 0u);
    EXPECT_LE(trace_result.events_read, events.size());
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(MemoryPolicy, FailedStartDestructionRollsBackBeforeRelease) {
    FixedProvider provider;
    provider.apply_failure_region = rt::memory_region_task_scratch;
    provider.rollback_failures_remaining = 1;
    {
        rt::Runtime runtime;
        CallbackProbe callback;
        const auto policy = strict_resident_policy();
        configure_provider_runtime(runtime, provider, &callback, &policy);
        ASSERT_EQ(runtime.finalize(), rt::Status::ok);
        EXPECT_EQ(runtime.start(), rt::Status::internal_error);
        EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
        EXPECT_EQ(provider.count(EventKind::release), 0u);
    }
    EXPECT_GE(provider.count(EventKind::rollback), 3u);
    EXPECT_EQ(provider.count(EventKind::release), 3u);
}

TEST(MemoryPolicy, StrictApplyAndReadbackFailuresRollbackAndRetry) {
    for (const bool mismatch : {false, true}) {
        SCOPED_TRACE(mismatch);
        FixedProvider provider;
        if (mismatch) {
            provider.mismatch_region = rt::memory_region_task_scratch;
        } else {
            provider.apply_failure_region = rt::memory_region_task_scratch;
        }
        rt::Runtime runtime;
        CallbackProbe callback;
        const auto policy = strict_resident_policy();
        configure_provider_runtime(runtime, provider, &callback, &policy);
        ASSERT_EQ(runtime.finalize(), rt::Status::ok);
        provider.clear_events();

        EXPECT_EQ(runtime.start(), rt::Status::internal_error);
        EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
        EXPECT_EQ(callback.calls, 0u);
        rt::CpuMemoryPolicyReport failed_report;
        ASSERT_TRUE(runtime.cpu_memory_policy_report(failed_report));
        const auto* failed = find_memory(
            failed_report,
            rt::memory_region_task_scratch);
        ASSERT_NE(failed, nullptr);
        EXPECT_EQ(
            failed->verified,
            mismatch
                ? rt::PolicyOperationState::mismatched
                : rt::PolicyOperationState::not_attempted);
        EXPECT_EQ(provider.count(EventKind::rollback), 2u);

        provider.apply_failure_region = {};
        provider.mismatch_region = {};
        provider.clear_events();
        ASSERT_EQ(runtime.start(), rt::Status::ok);
        ASSERT_EQ(
            runtime.step({1, std::chrono::nanoseconds(1), std::nullopt}),
            rt::Status::ok);
        EXPECT_EQ(callback.calls, 1u);
        EXPECT_EQ(runtime.stop(), rt::Status::ok);
        EXPECT_EQ(provider.count(EventKind::release), 3u);
    }
}

TEST(MemoryPolicy, LaterThreadFailureRollsBackThreadsThenMemoryAndRetries) {
    FixedProvider provider;
    FailingExecutorThreadProvider thread_provider;
    thread_provider.memory_provider = &provider;
    rt::Runtime runtime;
    rt::detail::RuntimeThreadPolicyTestAccess::set_provider(
        runtime,
        thread_provider);
    CallbackProbe callback;
    auto policy = strict_resident_policy();
    auto& executor_request =
        policy.thread_policies[policy.thread_policy_count++];
    executor_request.role = rt::thread_role_executor_worker;
    executor_request.policy.requirement = rt::PolicyRequirement::strict;
    configure_provider_runtime(runtime, provider, &callback, &policy);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    provider.clear_events();

    EXPECT_EQ(runtime.start(), rt::Status::internal_error);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    EXPECT_EQ(callback.calls, 0u);
    EXPECT_GE(thread_provider.join_count, 1u);
    EXPECT_EQ(thread_provider.rollback_count_seen_at_join, 0u);
    EXPECT_EQ(provider.count(EventKind::rollback), 3u);
    EXPECT_EQ(provider.count(EventKind::release), 0u);

    thread_provider.fail_executor = false;
    provider.clear_events();
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(
        runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(callback.calls, 1u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(provider.count(EventKind::release), 3u);
}

TEST(MemoryPolicy, WatchdogFailureJoinsBeforeMemoryRollbackAndRetries) {
    FixedProvider provider;
    FailingExecutorThreadProvider thread_provider;
    thread_provider.fail_role = rt::thread_role_watchdog;
    thread_provider.memory_provider = &provider;
    rt::Runtime runtime;
    rt::detail::RuntimeThreadPolicyTestAccess::set_provider(
        runtime,
        thread_provider);
    auto config = provider_config();
    config.watchdog_timeout_ns = 60'000'000'000ull;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    CallbackProbe callback;
    ASSERT_EQ(
        runtime.register_callback({"memory.watchdog", &record_callback, &callback}),
        rt::Status::ok);
    ASSERT_EQ(runtime.set_memory_provider(provider.table()), rt::Status::ok);
    auto policy = strict_resident_policy();
    auto& watchdog = policy.thread_policies[policy.thread_policy_count++];
    watchdog.role = rt::thread_role_watchdog;
    watchdog.policy.requirement = rt::PolicyRequirement::strict;
    ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    provider.clear_events();

    EXPECT_EQ(runtime.start(), rt::Status::internal_error);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    EXPECT_EQ(callback.calls, 0u);
    EXPECT_GE(thread_provider.join_count, 1u);
    EXPECT_EQ(thread_provider.rollback_count_seen_at_join, 0u);
    EXPECT_EQ(provider.count(EventKind::rollback), 3u);

    thread_provider.fail_executor = false;
    provider.clear_events();
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(
        runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(callback.calls, 1u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(provider.count(EventKind::release), 3u);
}

TEST(MemoryPolicy, ExecutorStackCleanupRetriesWithWatchdogStillAccounted) {
    FixedProvider provider;
    FailingExecutorThreadProvider thread_provider;
    thread_provider.fail_executor = false;
    thread_provider.cleanup_failure_instance = 1;
    thread_provider.cleanup_failures_remaining = 1;
    thread_provider.memory_provider = &provider;
    rt::Runtime runtime;
    rt::detail::RuntimeThreadPolicyTestAccess::set_provider(
        runtime,
        thread_provider);
    auto config = provider_config();
    config.worker_count = 2;
    config.watchdog_timeout_ns = 60'000'000'000ull;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(runtime.set_memory_provider(provider.table()), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    provider.clear_events();

    EXPECT_EQ(runtime.stop(), rt::Status::internal_error);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::running);
    EXPECT_EQ(thread_provider.cleanup_attempt_count, 1u);
    EXPECT_EQ(thread_provider.join_count, 2u);
    EXPECT_EQ(provider.count(EventKind::rollback), 0u);
    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    const auto* stack = find_memory(
        report,
        rt::memory_region_runtime_thread_stack);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->logical_region_count, 3u);
    EXPECT_EQ(stack->rollback_error, EIO);

    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::stopped);
    EXPECT_EQ(thread_provider.cleanup_attempt_count, 2u);
    EXPECT_EQ(thread_provider.join_count, 3u);
    EXPECT_EQ(provider.count(EventKind::rollback), 2u);
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    stack = find_memory(report, rt::memory_region_runtime_thread_stack);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->rollback_error, 0);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(MemoryPolicy, StartupRetainsFirstErrorWhenRollbackAlsoFails) {
    FixedProvider provider;
    provider.rollback_failures_remaining = 1;
    FailingExecutorThreadProvider thread_provider;
    thread_provider.fail_executor = false;
    thread_provider.fail_before_create = true;
    rt::Runtime runtime;
    rt::detail::RuntimeThreadPolicyTestAccess::set_provider(
        runtime,
        thread_provider);
    CallbackProbe callback;
    auto policy = strict_resident_policy();
    auto& executor_request =
        policy.thread_policies[policy.thread_policy_count++];
    executor_request.role = rt::thread_role_executor_worker;
    executor_request.policy.requirement = rt::PolicyRequirement::strict;
    configure_provider_runtime(runtime, provider, &callback, &policy);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    provider.clear_events();

    EXPECT_EQ(runtime.start(), rt::Status::resource_exhausted);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    EXPECT_EQ(callback.calls, 0u);
    EXPECT_EQ(provider.count(EventKind::rollback), 3u);
    EXPECT_EQ(provider.count(EventKind::release), 0u);

    thread_provider.fail_before_create = false;
    provider.clear_events();
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(
        runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(callback.calls, 1u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(provider.count(EventKind::release), 3u);
}

TEST(MemoryPolicy, BestEffortApplyFailureIsReportedAndRuntimeContinues) {
    FixedProvider provider;
    provider.apply_failure_region = rt::memory_region_task_scratch;
    rt::Runtime runtime;
    CallbackProbe callback;
    configure_provider_runtime(runtime, provider, &callback);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    const auto* task = find_memory(report, rt::memory_region_task_scratch);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->applied, rt::PolicyOperationState::failed);
    EXPECT_EQ(task->verified, rt::PolicyOperationState::not_attempted);
    EXPECT_EQ(task->apply_error, 72);
    EXPECT_EQ(task->resident_bytes, 0u);
    EXPECT_EQ(task->locked_bytes, 0u);
    EXPECT_EQ(task->pinned_bytes, 0u);
    ASSERT_EQ(
        runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(callback.calls, 1u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(MemoryPolicy, RuntimeInstancesIsolateTokensBackingReportsAndRollback) {
    FixedProvider first_provider;
    FixedProvider second_provider;
    rt::Runtime first;
    rt::Runtime second;
    CallbackProbe first_callback;
    CallbackProbe second_callback;
    const auto policy = strict_resident_policy();
    configure_provider_runtime(first, first_provider, &first_callback, &policy);
    configure_provider_runtime(second, second_provider, &second_callback, &policy);
    ASSERT_EQ(first.finalize(), rt::Status::ok);
    ASSERT_EQ(second.finalize(), rt::Status::ok);
    ASSERT_EQ(first.start(), rt::Status::ok);
    ASSERT_EQ(second.start(), rt::Status::ok);
    ASSERT_EQ(
        first.step({0, std::chrono::nanoseconds(1), std::nullopt}),
        rt::Status::ok);
    ASSERT_EQ(
        second.step({0, std::chrono::nanoseconds(1), std::nullopt}),
        rt::Status::ok);
    EXPECT_NE(first_callback.phase_scratch, second_callback.phase_scratch);
    EXPECT_EQ(first_callback.phase_scratch, first_provider.slots[0].usable);
    EXPECT_EQ(second_callback.phase_scratch, second_provider.slots[0].usable);

    rt::CpuMemoryPolicyReport first_report;
    rt::CpuMemoryPolicyReport second_report;
    ASSERT_TRUE(first.cpu_memory_policy_report(first_report));
    ASSERT_TRUE(second.cpu_memory_policy_report(second_report));
    first_report.memory[0].committed_bytes =
        std::numeric_limits<std::size_t>::max();
    rt::CpuMemoryPolicyReport fresh_first_report;
    ASSERT_TRUE(first.cpu_memory_policy_report(fresh_first_report));
    EXPECT_NE(
        fresh_first_report.memory[0].committed_bytes,
        first_report.memory[0].committed_bytes);

    ASSERT_EQ(first.stop(), rt::Status::ok);
    EXPECT_EQ(first_provider.count(EventKind::release), 3u);
    EXPECT_EQ(second_provider.count(EventKind::release), 0u);
    ASSERT_EQ(
        second.step({1, std::chrono::nanoseconds(1), std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(second_callback.calls, 2u);
    EXPECT_EQ(second.stop(), rt::Status::ok);
    EXPECT_EQ(second_provider.count(EventKind::release), 3u);
}

TEST(MemoryPolicy, NativeLinuxPageBackingIsRoundedGuardedAndObserved) {
    rt::Runtime runtime;
    auto config = provider_config();
    config.scratch_bytes = 73;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    CallbackProbe callback;
    ASSERT_EQ(
        runtime.register_callback(
            {"memory.native", &record_callback, &callback}),
        rt::Status::ok);
    rt::CpuMemoryPolicy policy;
    policy.memory_policy_count = 1;
    auto& request = policy.memory_policies[0];
    request.region = rt::memory_region_phase_scratch;
    request.policy.requirement = rt::PolicyRequirement::strict;
    request.policy.provider = rt::MemoryProviderOwnership::runtime;
    request.policy.page_rounding = rt::PageRounding::base_page;
    request.policy.guard_bytes_before = kPageBytes;
    request.policy.guard_bytes_after = kPageBytes;
    request.policy.prefault = rt::PolicyToggle::enabled;
    request.policy.huge_pages = rt::HugePagePreference::prefer;
    request.policy.huge_page_fallback = rt::PolicyToggle::enabled;
    request.policy.first_touch = rt::FirstTouchPolicy::caller;
    request.policy.residency_verification = rt::PolicyToggle::enabled;
    request.policy.rollback = rt::RollbackIntent::release;
    ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);

#if defined(__linux__)
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    rt::CpuMemoryPolicyReport finalized;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(finalized));
    const auto* acquired = find_memory(
        finalized,
        rt::memory_region_phase_scratch);
    ASSERT_NE(acquired, nullptr);
    const long native_page = ::sysconf(_SC_PAGESIZE);
    ASSERT_GT(native_page, 0);
    EXPECT_EQ(
        acquired->committed_bytes,
        align_up(acquired->accounted_bytes, static_cast<std::size_t>(native_page)));
    EXPECT_GE(acquired->actual_guard_bytes_before, kPageBytes);
    EXPECT_GE(acquired->actual_guard_bytes_after, kPageBytes);
    EXPECT_NE(
        acquired->used_explicit_huge_pages,
        acquired->used_huge_page_fallback);

    ASSERT_EQ(runtime.start(), rt::Status::ok);
    rt::CpuMemoryPolicyReport started;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(started));
    const auto* observed = find_memory(
        started,
        rt::memory_region_phase_scratch);
    ASSERT_NE(observed, nullptr);
    EXPECT_EQ(observed->applied, rt::PolicyOperationState::succeeded);
    EXPECT_EQ(observed->verified, rt::PolicyOperationState::succeeded);
    EXPECT_EQ(observed->resident_bytes, observed->committed_bytes);
    EXPECT_EQ(observed->locked_bytes, 0u);
    EXPECT_EQ(observed->pinned_bytes, 0u);
    ASSERT_EQ(
        runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
        rt::Status::ok);
    ASSERT_EQ(callback.calls, 1u);
    ASSERT_NE(callback.phase_scratch, nullptr);
    auto* usable = callback.phase_scratch;
    const auto committed = observed->committed_bytes;
    const auto usable_address = reinterpret_cast<std::uintptr_t>(usable);
#if defined(RTFW_TEST_THREAD_SANITIZER) || \
    defined(RTFW_TEST_ADDRESS_SANITIZER)
    std::byte readable{};
    ::iovec local{&readable, sizeof(readable)};
    ::iovec usable_remote{usable, sizeof(readable)};
    errno = 0;
    ASSERT_EQ(
        ::process_vm_readv(
            ::getpid(), &local, 1, &usable_remote, 1, 0),
        1);

    ::iovec before_guard_remote{
        reinterpret_cast<void*>(usable_address - 1),
        sizeof(readable)};
    errno = 0;
    EXPECT_EQ(
        ::process_vm_readv(
            ::getpid(), &local, 1, &before_guard_remote, 1, 0),
        -1);
    EXPECT_EQ(errno, EFAULT);

    ::iovec after_guard_remote{
        reinterpret_cast<void*>(usable_address + committed),
        sizeof(readable)};
    errno = 0;
    EXPECT_EQ(
        ::process_vm_readv(
            ::getpid(), &local, 1, &after_guard_remote, 1, 0),
        -1);
    EXPECT_EQ(errno, EFAULT);
#elif GTEST_HAS_DEATH_TEST
    const auto previous_death_test_style =
        ::testing::GTEST_FLAG(death_test_style);
    ::testing::GTEST_FLAG(death_test_style) = "threadsafe";
    auto* before_guard = reinterpret_cast<volatile std::byte*>(
        usable_address - 1);
    auto* after_guard = reinterpret_cast<volatile std::byte*>(
        usable_address + committed);
    EXPECT_EXIT(
        {
            volatile auto value = *before_guard;
            (void)value;
            std::_Exit(0);
        },
        ::testing::KilledBySignal(SIGSEGV),
        "");
    EXPECT_EXIT(
        {
            volatile auto value = *after_guard;
            (void)value;
            std::_Exit(0);
        },
        ::testing::KilledBySignal(SIGSEGV),
        "");
    ::testing::GTEST_FLAG(death_test_style) = previous_death_test_style;
#endif
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
#else
    EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
#endif
}

TEST(MemoryPolicy, NativeLockingUsesIsolatedPagesAndNeverReportsPinning) {
#if defined(__linux__)
    std::array<rt::Runtime, 2> runtimes;
    std::array<CallbackProbe, 2> callbacks{};
    for (std::size_t index = 0; index < runtimes.size(); ++index) {
        auto config = provider_config();
        ASSERT_EQ(runtimes[index].configure(config), rt::Status::ok);
        ASSERT_EQ(
            runtimes[index].register_callback(
                {"memory.lock", &record_callback, &callbacks[index]}),
            rt::Status::ok);
        rt::CpuMemoryPolicy policy;
        policy.memory_policy_count = 1;
        policy.memory_policies[0].region = rt::memory_region_phase_scratch;
        policy.memory_policies[0].policy.locking = rt::PolicyToggle::enabled;
        ASSERT_EQ(runtimes[index].set_cpu_memory_policy(policy), rt::Status::ok);
        ASSERT_EQ(runtimes[index].finalize(), rt::Status::ok);
        ASSERT_EQ(runtimes[index].start(), rt::Status::ok);
        ASSERT_EQ(
            runtimes[index].step(
                {index, std::chrono::nanoseconds(1), std::nullopt}),
            rt::Status::ok);
        rt::CpuMemoryPolicyReport report;
        ASSERT_TRUE(runtimes[index].cpu_memory_policy_report(report));
        const auto* row = find_memory(report, rt::memory_region_phase_scratch);
        ASSERT_NE(row, nullptr);
        EXPECT_EQ(row->pinned_bytes, 0u);
        EXPECT_EQ(row->locked_bytes, 0u);
        EXPECT_EQ(row->actual_page_bytes, static_cast<std::size_t>(::sysconf(_SC_PAGESIZE)));
        if (row->applied == rt::PolicyOperationState::succeeded) {
            EXPECT_EQ(row->verified, rt::PolicyOperationState::mismatched);
        } else {
            EXPECT_EQ(row->applied, rt::PolicyOperationState::failed);
            EXPECT_NE(row->apply_error, 0);
        }
    }
    const auto page = static_cast<std::uintptr_t>(::sysconf(_SC_PAGESIZE));
    ASSERT_NE(callbacks[0].phase_scratch, nullptr);
    ASSERT_NE(callbacks[1].phase_scratch, nullptr);
    EXPECT_NE(
        reinterpret_cast<std::uintptr_t>(callbacks[0].phase_scratch) / page,
        reinterpret_cast<std::uintptr_t>(callbacks[1].phase_scratch) / page);
    EXPECT_EQ(runtimes[0].stop(), rt::Status::ok);
    EXPECT_EQ(runtimes[1].stop(), rt::Status::ok);
#endif
}
