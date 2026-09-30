#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

#include <rt/mock_device.hpp>
#include <rt/runtime.hpp>

#include "rt/src/thread_policy.hpp"

namespace {

#if defined(ENOTSUP)
constexpr std::int32_t unsupported_error = ENOTSUP;
#else
constexpr std::int32_t unsupported_error = EINVAL;
#endif

enum class InjectedOutcome : std::uint8_t {
    success,
    create_failure,
    apply_failure,
    unsupported,
    mismatch,
    stack_success,
    stack_failure,
    stack_mismatch,
};

constexpr std::size_t event_capacity = 128;
constexpr std::uint32_t event_resolve = 1'000;
constexpr std::uint32_t event_create = 2'000;
constexpr std::uint32_t event_apply = 3'000;
constexpr std::uint32_t event_verify = 4'000;
constexpr std::uint32_t event_join = 5'000;

std::uint32_t event(
    std::uint32_t kind,
    rt::ThreadRoleId role,
    std::size_t instance = 0) {
    return kind + (role.value * 32u) +
        static_cast<std::uint32_t>(instance);
}

class InjectedPolicyProvider final : public rt::detail::ThreadPolicyProvider {
public:
    rt::ThreadRoleId failure_role{};
    std::size_t failure_instance = 0;
    InjectedOutcome outcome = InjectedOutcome::success;
    rt::ThreadRoleId cleanup_failure_role =
        rt::thread_role_executor_worker;
    std::size_t cleanup_failure_instance = 0;
    std::size_t cleanup_failures_remaining = 0;
    std::size_t cleanup_attempt_count = 0;

    void clear_events() noexcept {
        event_count_.store(0, std::memory_order_relaxed);
    }

    void clear_failure() noexcept {
        failure_role = {};
        failure_instance = 0;
        outcome = InjectedOutcome::success;
    }

    [[nodiscard]] std::size_t event_count() const noexcept {
        return event_count_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint32_t event_at(std::size_t index) const noexcept {
        return events_[index];
    }

    [[nodiscard]] std::size_t count_kind(std::uint32_t kind) const noexcept {
        std::size_t count = 0;
        for (std::size_t index = 0; index < event_count(); ++index) {
            const auto value = events_[index];
            count += value >= kind && value < kind + 1'000 ? 1u : 0u;
        }
        return count;
    }

    [[nodiscard]] rt::Status resolve(
        rt::ThreadRoleId role,
        rt::PolicyApplicationMode mode,
        bool active,
        bool observable,
        bool has_request,
        const rt::ThreadPolicy& requested,
        const rt::ThreadPolicy& role_default,
        rt::ThreadPolicy& resolved,
        rt::PolicyResolutionState& resolution,
        std::int32_t& system_error) noexcept override {
        record(event(event_resolve, role));
        resolved = role_default;
        resolved.requirement = requested.requirement;
        system_error = 0;
        if (!active) {
            resolution = rt::PolicyResolutionState::inactive;
            if (has_request &&
                requested.requirement == rt::PolicyRequirement::strict) {
                system_error = ENODEV;
                return rt::Status::invalid_config;
            }
            return rt::Status::ok;
        }
        if (mode == rt::PolicyApplicationMode::verify_only && !observable) {
            resolution = rt::PolicyResolutionState::external_verify_only;
            if (has_request) {
                system_error = unsupported_error;
            }
            return has_request &&
                    requested.requirement == rt::PolicyRequirement::strict
                ? rt::Status::invalid_config
                : rt::Status::ok;
        }
        if (requested.cpu_set.count != 0) {
            resolved.cpu_set = requested.cpu_set;
        }
        if (requested.scheduling_class != rt::SchedulingClass::inherit) {
            resolved.scheduling_class = requested.scheduling_class;
            resolved.scheduling_priority = requested.scheduling_priority;
        }
        resolved.numa_node = requested.numa_node;
        if (requested.wait_strategy != rt::WaitStrategy::inherit) {
            resolved.wait_strategy = requested.wait_strategy;
        }
        if (requested.stack_bytes != 0) {
            resolved.stack_bytes = requested.stack_bytes;
        }
        if (requested.guard_bytes != 0) {
            resolved.guard_bytes = requested.guard_bytes;
        }
        if (requested.name.front() != '\0') {
            resolved.name = requested.name;
        }
        resolution = rt::PolicyResolutionState::native_supported;
        return rt::Status::ok;
    }

    [[nodiscard]] rt::Status before_create(
        rt::ThreadRoleId role,
        std::size_t instance,
        const rt::detail::ThreadRolePlan&,
        std::int32_t& system_error) noexcept override {
        record(event(event_create, role, instance));
        if (matches_failure(role, instance, InjectedOutcome::create_failure)) {
            system_error = EAGAIN;
            return rt::Status::resource_exhausted;
        }
        system_error = 0;
        return rt::Status::ok;
    }

    void apply_and_verify_current(
        rt::ThreadRoleId role,
        std::size_t instance,
        const rt::detail::ThreadRolePlan& plan,
        rt::detail::ThreadStartupResult& result) noexcept override {
        record(event(event_apply, role, instance));
        result.read_back = plan.resolved;
        if (matches_failure(role, instance, InjectedOutcome::apply_failure)) {
            result.applied = rt::PolicyOperationState::failed;
            result.verified = rt::PolicyOperationState::failed;
            result.apply_error = EPERM;
            result.used_default_fallback = true;
            return;
        }
        if (matches_failure(role, instance, InjectedOutcome::unsupported)) {
            result.applied = rt::PolicyOperationState::unsupported;
            result.verified = rt::PolicyOperationState::unsupported;
            result.apply_error = unsupported_error;
            result.verify_error = unsupported_error;
            result.used_default_fallback = true;
            return;
        }
        result.applied = rt::PolicyOperationState::succeeded;
        if (matches_failure(role, instance, InjectedOutcome::mismatch)) {
            result.verified = rt::PolicyOperationState::mismatched;
            result.verify_error = EPROTO;
            result.read_back.wait_strategy =
                plan.resolved.wait_strategy == rt::WaitStrategy::spin
                ? rt::WaitStrategy::yield
                : rt::WaitStrategy::spin;
            return;
        }
        result.verified = rt::PolicyOperationState::succeeded;
    }

    void verify_current(
        rt::ThreadRoleId role,
        const rt::detail::ThreadRolePlan& plan,
        rt::detail::ThreadStartupResult& result) noexcept override {
        record(event(event_verify, role));
        result.applied = rt::PolicyOperationState::not_attempted;
        result.verified = rt::PolicyOperationState::succeeded;
        result.read_back = plan.resolved;
    }

    void apply_and_verify_stack_current(
        rt::ThreadRoleId role,
        std::size_t instance,
        const rt::detail::ThreadRolePlan& plan,
        rt::detail::ThreadStartupResult& result) noexcept override {
        if (!matches_failure(role, instance, InjectedOutcome::stack_success) &&
            !matches_failure(role, instance, InjectedOutcome::stack_failure) &&
            !matches_failure(role, instance, InjectedOutcome::stack_mismatch)) {
            ThreadPolicyProvider::apply_and_verify_stack_current(
                role,
                instance,
                plan,
                result);
            return;
        }
        const bool unavailable = matches_failure(
            role,
            instance,
            InjectedOutcome::stack_failure);
        if (!unavailable) {
            result.stack_mapping_base = reinterpret_cast<std::byte*>(0x10000u);
            result.stack_mapping_bytes = 8192;
            result.stack_usable_bytes = 4096;
            result.stack_guard_bytes = 4096;
            result.stack_resident_bytes = 4096;
        }
        result.stack_applied = unavailable
            ? rt::PolicyOperationState::failed
            : rt::PolicyOperationState::succeeded;
        result.stack_verified =
            unavailable
                ? rt::PolicyOperationState::failed
                : matches_failure(
                      role,
                      instance,
                      InjectedOutcome::stack_mismatch)
                    ? rt::PolicyOperationState::mismatched
                    : rt::PolicyOperationState::succeeded;
        result.stack_apply_error = result.stack_applied ==
                rt::PolicyOperationState::failed
            ? EPERM
            : 0;
        result.stack_verify_error = result.stack_verified ==
                rt::PolicyOperationState::succeeded
            ? 0
            : EPROTO;
    }

    [[nodiscard]] rt::Status cleanup_stack_current(
        rt::ThreadRoleId role,
        std::size_t instance,
        const rt::detail::ThreadRolePlan& plan,
        rt::detail::ThreadStartupResult& result) noexcept override {
        if (role == cleanup_failure_role &&
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

    void after_join(
        rt::ThreadRoleId role,
        std::size_t instance) noexcept override {
        record(event(event_join, role, instance));
    }

private:
    [[nodiscard]] bool matches_failure(
        rt::ThreadRoleId role,
        std::size_t instance,
        InjectedOutcome expected) const noexcept {
        return outcome == expected && role == failure_role &&
               instance == failure_instance;
    }

    void record(std::uint32_t value) noexcept {
        const auto index = event_count_.fetch_add(1, std::memory_order_relaxed);
        if (index < events_.size()) {
            events_[index] = value;
        }
    }

    std::array<std::uint32_t, event_capacity> events_{};
    std::atomic<std::size_t> event_count_{0};
};

const rt::ThreadPolicyReport* find_thread(
    const rt::CpuMemoryPolicyReport& report,
    rt::ThreadRoleId role) {
    for (std::size_t index = 0; index < report.thread_count; ++index) {
        if (report.threads[index].role == role) {
            return &report.threads[index];
        }
    }
    return nullptr;
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

rt::CallbackResult count_callback(
    void* data,
    const rt::CallbackContext&) {
    ++*static_cast<std::size_t*>(data);
    return rt::CallbackResult::ok;
}

rt::RuntimeConfig threaded_config(bool device = false) {
    rt::RuntimeConfig config;
    config.callback_capacity = 1;
    config.worker_count = 2;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    config.watchdog_timeout_ns = 1'000'000;
    if (device) {
        config.device_backend_capacity = 1;
        config.device_buffer_capacity = 1;
        config.device_outstanding_capacity = 2;
        config.device_completion_batch = 2;
    }
    return config;
}

void add_strict_role(rt::CpuMemoryPolicy& policy, rt::ThreadRoleId role) {
    auto& request = policy.thread_policies[policy.thread_policy_count++];
    request.role = role;
    request.policy.requirement = rt::PolicyRequirement::strict;
}

void add_runtime_stack_policy(
    rt::CpuMemoryPolicy& policy,
    rt::PolicyRequirement requirement) {
    auto& request = policy.memory_policies[policy.memory_policy_count++];
    request.region = rt::memory_region_runtime_thread_stack;
    request.policy.requirement = requirement;
}

} // namespace

TEST(ThreadPolicy, AppliesEveryRuntimeOwnedInstanceBeforeCommitAndJoinsReverse) {
    InjectedPolicyProvider provider;
    rt::Runtime runtime;
    rt::detail::RuntimeThreadPolicyTestAccess::set_provider(runtime, provider);
    ASSERT_EQ(runtime.configure(threaded_config(true)), rt::Status::ok);
    std::size_t callbacks = 0;
    ASSERT_EQ(
        runtime.register_callback({"thread.sequence", &count_callback, &callbacks}),
        rt::Status::ok);
    rt::MockDeviceBackend backend({2, 1, 1, 1'000});
    rt::DeviceBackendHandle backend_handle;
    ASSERT_EQ(
        runtime.register_device_backend(
            {"thread.mock", backend.api()},
            backend_handle),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    EXPECT_EQ(provider.count_kind(event_create), 0u);
    EXPECT_EQ(provider.count_kind(event_apply), 0u);

    ASSERT_EQ(runtime.start(), rt::Status::ok);
    EXPECT_EQ(callbacks, 0u);
    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    const auto* frame = find_thread(report, rt::thread_role_frame);
    const auto* executor = find_thread(report, rt::thread_role_executor_worker);
    const auto* watchdog = find_thread(report, rt::thread_role_watchdog);
    const auto* device = find_thread(report, rt::thread_role_device_service);
    ASSERT_NE(frame, nullptr);
    ASSERT_NE(executor, nullptr);
    ASSERT_NE(watchdog, nullptr);
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(frame->applied, rt::PolicyOperationState::not_attempted);
    EXPECT_EQ(frame->verified, rt::PolicyOperationState::succeeded);
    EXPECT_EQ(executor->attempted_instance_count, 2u);
    EXPECT_EQ(executor->applied_instance_count, 2u);
    EXPECT_EQ(executor->verified_instance_count, 2u);
    EXPECT_EQ(executor->applied, rt::PolicyOperationState::succeeded);
    EXPECT_EQ(executor->verified, rt::PolicyOperationState::succeeded);
    EXPECT_EQ(watchdog->verified, rt::PolicyOperationState::succeeded);
    EXPECT_EQ(device->verified, rt::PolicyOperationState::succeeded);

    ASSERT_EQ(
        runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(callbacks, 1u);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);

    const std::array<std::uint32_t, 4> expected_joins{
        event(event_join, rt::thread_role_device_service, 0),
        event(event_join, rt::thread_role_executor_worker, 1),
        event(event_join, rt::thread_role_executor_worker, 0),
        event(event_join, rt::thread_role_watchdog, 0),
    };
    std::array<std::uint32_t, 4> actual_joins{};
    std::size_t join_count = 0;
    for (std::size_t index = 0; index < provider.event_count(); ++index) {
        const auto value = provider.event_at(index);
        if (value >= event_join && value < event_join + 1'000) {
            ASSERT_LT(join_count, actual_joins.size());
            actual_joins[join_count++] = value;
        }
    }
    EXPECT_EQ(join_count, expected_joins.size());
    EXPECT_EQ(actual_joins, expected_joins);
}

TEST(ThreadPolicy, StrictFailuresRollbackWithoutCallbacksAndCanRecover) {
    const std::array<std::pair<rt::ThreadRoleId, InjectedOutcome>, 4> cases{{
        {rt::thread_role_watchdog, InjectedOutcome::apply_failure},
        {rt::thread_role_executor_worker, InjectedOutcome::create_failure},
        {rt::thread_role_executor_worker, InjectedOutcome::mismatch},
        {rt::thread_role_device_service, InjectedOutcome::mismatch},
    }};
    for (const auto& [failed_role, outcome] : cases) {
        SCOPED_TRACE(failed_role.value);
        InjectedPolicyProvider provider;
        provider.failure_role = failed_role;
        provider.failure_instance =
            failed_role == rt::thread_role_executor_worker ? 1u : 0u;
        provider.outcome = outcome;
        rt::Runtime runtime;
        rt::detail::RuntimeThreadPolicyTestAccess::set_provider(runtime, provider);
        ASSERT_EQ(runtime.configure(threaded_config(true)), rt::Status::ok);
        std::size_t callbacks = 0;
        ASSERT_EQ(
            runtime.register_callback(
                {"thread.rollback", &count_callback, &callbacks}),
            rt::Status::ok);
        rt::MockDeviceBackend backend({2, 1, 1, 1'000});
        rt::DeviceBackendHandle backend_handle;
        ASSERT_EQ(
            runtime.register_device_backend(
                {"rollback.mock", backend.api()},
                backend_handle),
            rt::Status::ok);
        rt::CpuMemoryPolicy policy;
        add_strict_role(policy, failed_role);
        ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
        ASSERT_EQ(runtime.finalize(), rt::Status::ok);

        EXPECT_NE(runtime.start(), rt::Status::ok);
        EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
        EXPECT_EQ(callbacks, 0u);
        rt::CpuMemoryPolicyReport report;
        ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
        const auto* failed = find_thread(report, failed_role);
        ASSERT_NE(failed, nullptr);
        EXPECT_NE(failed->verified, rt::PolicyOperationState::succeeded);
        provider.clear_failure();
        provider.clear_events();
        ASSERT_EQ(runtime.start(), rt::Status::ok);
        ASSERT_EQ(
            runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
            rt::Status::ok);
        EXPECT_EQ(callbacks, 1u);
        EXPECT_EQ(runtime.stop(), rt::Status::ok);
    }
}

TEST(ThreadPolicy, BestEffortFailureRemainsObservableAndContinues) {
    InjectedPolicyProvider provider;
    provider.failure_role = rt::thread_role_executor_worker;
    provider.failure_instance = 1;
    provider.outcome = InjectedOutcome::unsupported;
    rt::Runtime runtime;
    rt::detail::RuntimeThreadPolicyTestAccess::set_provider(runtime, provider);
    ASSERT_EQ(runtime.configure(threaded_config()), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    const auto* executor = find_thread(report, rt::thread_role_executor_worker);
    ASSERT_NE(executor, nullptr);
    EXPECT_EQ(executor->attempted_instance_count, 2u);
    EXPECT_EQ(executor->applied_instance_count, 1u);
    EXPECT_EQ(executor->verified_instance_count, 1u);
    EXPECT_EQ(executor->fallback_instance_count, 1u);
    EXPECT_EQ(executor->applied, rt::PolicyOperationState::unsupported);
    EXPECT_EQ(executor->verified, rt::PolicyOperationState::unsupported);
    EXPECT_EQ(executor->apply_error, unsupported_error);
    EXPECT_EQ(executor->verify_error, unsupported_error);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(ThreadPolicy, LiveRuntimeStackAccountingIsExactBoundedAndIsolated) {
    rt::Runtime first;
    auto first_config = threaded_config();
    first_config.worker_count = 1;
    first_config.watchdog_timeout_ns = 0;
    ASSERT_EQ(first.configure(first_config), rt::Status::ok);
    ASSERT_EQ(first.finalize(), rt::Status::ok);

    rt::Runtime second;
    auto second_config = threaded_config();
    second_config.worker_count = 2;
    ASSERT_EQ(second.configure(second_config), rt::Status::ok);
    ASSERT_EQ(second.finalize(), rt::Status::ok);

    ASSERT_EQ(first.start(), rt::Status::ok);
    ASSERT_EQ(second.start(), rt::Status::ok);
    rt::CpuMemoryPolicyReport first_report;
    rt::CpuMemoryPolicyReport second_report;
    ASSERT_TRUE(first.cpu_memory_policy_report(first_report));
    ASSERT_TRUE(second.cpu_memory_policy_report(second_report));
    const auto* first_stack = find_memory(
        first_report,
        rt::memory_region_runtime_thread_stack);
    const auto* second_stack = find_memory(
        second_report,
        rt::memory_region_runtime_thread_stack);
    ASSERT_NE(first_stack, nullptr);
    ASSERT_NE(second_stack, nullptr);
    EXPECT_EQ(first_stack->logical_region_count, 1u);
    EXPECT_EQ(second_stack->logical_region_count, 3u);
#if defined(__linux__)
    EXPECT_EQ(
        first_stack->accounting_exactness,
        rt::ResourceAccountingExactness::exact);
    EXPECT_EQ(
        second_stack->accounting_exactness,
        rt::ResourceAccountingExactness::exact);
    EXPECT_GT(first_stack->committed_bytes, 0u);
    EXPECT_GT(second_stack->committed_bytes, first_stack->committed_bytes);
    EXPECT_EQ(first_stack->accounted_bytes, first_stack->committed_bytes);
    EXPECT_EQ(second_stack->accounted_bytes, second_stack->committed_bytes);
    EXPECT_LE(
        first_stack->resident_bytes,
        first_stack->committed_bytes - first_stack->actual_guard_bytes_before);
    EXPECT_LE(
        second_stack->resident_bytes,
        second_stack->committed_bytes - second_stack->actual_guard_bytes_before);
#else
    EXPECT_EQ(
        first_stack->accounting_exactness,
        rt::ResourceAccountingExactness::unknown);
    EXPECT_EQ(
        second_stack->accounting_exactness,
        rt::ResourceAccountingExactness::unknown);
#endif

    const auto second_bytes = second_stack->accounted_bytes;
    ASSERT_EQ(first.stop(), rt::Status::ok);
    ASSERT_TRUE(second.cpu_memory_policy_report(second_report));
    second_stack = find_memory(
        second_report,
        rt::memory_region_runtime_thread_stack);
    ASSERT_NE(second_stack, nullptr);
    EXPECT_EQ(second_stack->accounted_bytes, second_bytes);
    ASSERT_EQ(second.stop(), rt::Status::ok);
}

TEST(ThreadPolicy, StrictRuntimeStackMismatchRollsBackAndRecovers) {
    InjectedPolicyProvider provider;
    provider.failure_role = rt::thread_role_executor_worker;
    provider.outcome = InjectedOutcome::stack_mismatch;
    rt::Runtime runtime;
    rt::detail::RuntimeThreadPolicyTestAccess::set_provider(runtime, provider);
    auto config = threaded_config();
    config.worker_count = 1;
    config.watchdog_timeout_ns = 0;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    std::size_t callbacks = 0;
    ASSERT_EQ(
        runtime.register_callback(
            {"stack.strict", &count_callback, &callbacks}),
        rt::Status::ok);
    rt::CpuMemoryPolicy policy;
    add_runtime_stack_policy(policy, rt::PolicyRequirement::strict);
    policy.memory_policies[0].policy.residency_verification =
        rt::PolicyToggle::enabled;
    ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
#if defined(__linux__)
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    EXPECT_EQ(runtime.start(), rt::Status::internal_error);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    EXPECT_EQ(callbacks, 0u);
    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    const auto* stack = find_memory(
        report,
        rt::memory_region_runtime_thread_stack);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->verified, rt::PolicyOperationState::mismatched);
    EXPECT_EQ(stack->verify_error, EPROTO);

    provider.outcome = InjectedOutcome::stack_success;
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(
        runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(callbacks, 1u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
#else
    EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
    EXPECT_EQ(callbacks, 0u);
    EXPECT_EQ(provider.count_kind(event_create), 0u);
    EXPECT_EQ(provider.count_kind(event_apply), 0u);
    EXPECT_EQ(provider.count_kind(event_verify), 0u);
    EXPECT_EQ(provider.count_kind(event_join), 0u);
#endif
}

TEST(ThreadPolicy, FailedStartCleanupRetryClearsRetainedStackError) {
    InjectedPolicyProvider provider;
    provider.failure_role = rt::thread_role_executor_worker;
    provider.outcome = InjectedOutcome::stack_mismatch;
    provider.cleanup_failures_remaining = 1;
    rt::Runtime runtime;
    rt::detail::RuntimeThreadPolicyTestAccess::set_provider(runtime, provider);
    auto config = threaded_config();
    config.worker_count = 1;
    config.watchdog_timeout_ns = 0;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    rt::CpuMemoryPolicy policy;
    add_runtime_stack_policy(policy, rt::PolicyRequirement::strict);
    policy.memory_policies[0].policy.residency_verification =
        rt::PolicyToggle::enabled;
    ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
#if defined(__linux__)
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    EXPECT_EQ(runtime.start(), rt::Status::internal_error);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    EXPECT_EQ(provider.cleanup_attempt_count, 1u);
    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    const auto* stack = find_memory(
        report,
        rt::memory_region_runtime_thread_stack);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->rollback_error, EIO);

    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::stopped);
    EXPECT_EQ(provider.cleanup_attempt_count, 2u);
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    stack = find_memory(report, rt::memory_region_runtime_thread_stack);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->rollback_error, 0);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
#else
    EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
    EXPECT_EQ(provider.cleanup_attempt_count, 0u);
    EXPECT_EQ(provider.count_kind(event_create), 0u);
    EXPECT_EQ(provider.count_kind(event_apply), 0u);
    EXPECT_EQ(provider.count_kind(event_verify), 0u);
    EXPECT_EQ(provider.count_kind(event_join), 0u);
#endif
}

TEST(ThreadPolicy, BestEffortRuntimeStackFailureIsReportedWithoutCommitBlock) {
    InjectedPolicyProvider provider;
    provider.failure_role = rt::thread_role_executor_worker;
    provider.outcome = InjectedOutcome::stack_failure;
    rt::Runtime runtime;
    rt::detail::RuntimeThreadPolicyTestAccess::set_provider(runtime, provider);
    auto config = threaded_config();
    config.worker_count = 1;
    config.watchdog_timeout_ns = 0;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    rt::CpuMemoryPolicy policy;
    add_runtime_stack_policy(policy, rt::PolicyRequirement::best_effort);
    policy.memory_policies[0].policy.locking = rt::PolicyToggle::enabled;
    ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    const auto* stack = find_memory(
        report,
        rt::memory_region_runtime_thread_stack);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->applied, rt::PolicyOperationState::failed);
    EXPECT_EQ(stack->verified, rt::PolicyOperationState::failed);
    EXPECT_EQ(stack->apply_error, EPERM);
    EXPECT_EQ(stack->verify_error, EPROTO);
    EXPECT_EQ(stack->accounted_bytes, 0u);
    EXPECT_EQ(
        stack->accounting_exactness,
        rt::ResourceAccountingExactness::unknown);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

#if defined(__linux__)
TEST(ThreadPolicy, StrictStackLockingCannotClaimIndependentReadback) {
    rt::Runtime runtime;
    auto config = threaded_config();
    config.worker_count = 1;
    config.watchdog_timeout_ns = 0;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    std::size_t callbacks = 0;
    ASSERT_EQ(
        runtime.register_callback(
            {"stack.lock-readback", &count_callback, &callbacks}),
        rt::Status::ok);
    rt::CpuMemoryPolicy policy;
    add_runtime_stack_policy(policy, rt::PolicyRequirement::strict);
    policy.memory_policies[0].policy.locking = rt::PolicyToggle::enabled;
    ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    EXPECT_EQ(runtime.start(), rt::Status::internal_error);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    EXPECT_EQ(callbacks, 0u);
    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    const auto* stack = find_memory(
        report,
        rt::memory_region_runtime_thread_stack);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->locked_bytes, 0u);
    EXPECT_NE(stack->verified, rt::PolicyOperationState::succeeded);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}
#endif

TEST(ThreadPolicy, SpinYieldAndParkWakeForWorkAndStop) {
    for (const auto wait : {
             rt::WaitStrategy::spin,
             rt::WaitStrategy::yield,
             rt::WaitStrategy::park}) {
        SCOPED_TRACE(static_cast<unsigned>(wait));
        InjectedPolicyProvider provider;
        rt::Runtime runtime;
        rt::detail::RuntimeThreadPolicyTestAccess::set_provider(runtime, provider);
        auto config = threaded_config();
        config.watchdog_timeout_ns = 0;
        ASSERT_EQ(runtime.configure(config), rt::Status::ok);
        std::size_t callbacks = 0;
        ASSERT_EQ(
            runtime.register_callback(
                {"thread.wait", &count_callback, &callbacks}),
            rt::Status::ok);
        rt::CpuMemoryPolicy policy;
        policy.thread_policy_count = 1;
        policy.thread_policies[0].role = rt::thread_role_executor_worker;
        policy.thread_policies[0].policy.wait_strategy = wait;
        ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
        ASSERT_EQ(runtime.finalize(), rt::Status::ok);
        ASSERT_EQ(runtime.start(), rt::Status::ok);
        ASSERT_EQ(
            runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
            rt::Status::ok);
        EXPECT_EQ(callbacks, 1u);
        EXPECT_EQ(runtime.stop(), rt::Status::ok);
    }
}

TEST(ThreadPolicy, StrictExternalAndUnavailableRequestsFailDuringFinalize) {
    {
        rt::Runtime runtime;
        rt::CpuMemoryPolicy policy;
        add_strict_role(
            policy,
            rt::ThreadRoleId{rt::thread_role_custom_first + 11});
        ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
        EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
    }
#if defined(__linux__)
    {
        rt::Runtime runtime;
        rt::CpuMemoryPolicy policy;
        add_strict_role(policy, rt::thread_role_executor_worker);
        auto& request = policy.thread_policies[0].policy;
        request.cpu_set.count = 1;
        request.cpu_set.cpu_ids[0] = CPU_SETSIZE;
        ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
        EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
    }
    {
        rt::Runtime runtime;
        rt::CpuMemoryPolicy policy;
        add_strict_role(policy, rt::thread_role_executor_worker);
        policy.thread_policies[0].policy.numa_node = 1'000'000;
        ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
        EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
    }
#endif

}

#if defined(__linux__)
TEST(ThreadPolicy, LinuxAppliesAndReadsBackAvailableNativeFields) {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        GTEST_SKIP() << "sched_getaffinity unavailable";
    }
    int selected_cpu = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &allowed)) {
            selected_cpu = cpu;
            break;
        }
    }
    if (selected_cpu < 0) {
        GTEST_SKIP() << "no allowed CPU";
    }
    const long page = sysconf(_SC_PAGESIZE);
    ASSERT_GT(page, 0);

    rt::Runtime runtime;
    auto config = threaded_config();
    config.worker_count = 1;
    config.watchdog_timeout_ns = 0;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    rt::CpuMemoryPolicy policy;
    add_strict_role(policy, rt::thread_role_executor_worker);
    auto& requested = policy.thread_policies[0].policy;
    requested.cpu_set.count = 1;
    requested.cpu_set.cpu_ids[0] = static_cast<std::uint32_t>(selected_cpu);
    requested.scheduling_class = rt::SchedulingClass::normal;
    requested.scheduling_priority = 0;
    requested.wait_strategy = rt::WaitStrategy::yield;
    requested.stack_bytes = static_cast<std::size_t>(PTHREAD_STACK_MIN) * 4u;
    requested.guard_bytes = static_cast<std::size_t>(page);
    constexpr std::string_view name = "rtfw-m15";
    std::copy(name.begin(), name.end(), requested.name.begin());
    ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    const auto* executor = find_thread(report, rt::thread_role_executor_worker);
    ASSERT_NE(executor, nullptr);
    EXPECT_EQ(executor->resolution, rt::PolicyResolutionState::native_supported);
    EXPECT_EQ(executor->applied, rt::PolicyOperationState::succeeded);
    EXPECT_EQ(executor->verified, rt::PolicyOperationState::succeeded);
    EXPECT_EQ(executor->read_back.cpu_set.count, 1u);
    EXPECT_EQ(
        executor->read_back.cpu_set.cpu_ids[0],
        static_cast<std::uint32_t>(selected_cpu));
    EXPECT_EQ(executor->read_back.scheduling_class, rt::SchedulingClass::normal);
    EXPECT_EQ(executor->read_back.scheduling_priority, 0);
    EXPECT_EQ(executor->read_back.wait_strategy, rt::WaitStrategy::yield);
    EXPECT_EQ(executor->read_back.stack_bytes, requested.stack_bytes);
    EXPECT_EQ(executor->read_back.guard_bytes, requested.guard_bytes);
    EXPECT_EQ(executor->read_back.name, requested.name);
    const auto* stack = find_memory(
        report,
        rt::memory_region_runtime_thread_stack);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(
        stack->committed_bytes,
        requested.stack_bytes);
    EXPECT_EQ(stack->actual_guard_bytes_before, requested.guard_bytes);
    EXPECT_EQ(stack->logical_region_count, 1u);
    EXPECT_EQ(
        stack->accounting_exactness,
        rt::ResourceAccountingExactness::exact);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}
#endif

#if defined(RTFW_STANDALONE_ROLLBACK_TEST)
TEST(ThreadPolicy, StrictFailureJoinsCreatedLanesInReverseBeforeReturning) {
    InjectedPolicyProvider provider;
    provider.failure_role = rt::thread_role_device_service;
    provider.outcome = InjectedOutcome::mismatch;
    rt::Runtime runtime;
    rt::detail::RuntimeThreadPolicyTestAccess::set_provider(runtime, provider);
    ASSERT_EQ(runtime.configure(threaded_config(true)), rt::Status::ok);
    std::size_t callbacks = 0;
    ASSERT_EQ(
        runtime.register_callback(
            {"thread.abort-order", &count_callback, &callbacks}),
        rt::Status::ok);
    rt::MockDeviceBackend backend({2, 1, 1, 1'000});
    rt::DeviceBackendHandle backend_handle;
    ASSERT_EQ(
        runtime.register_device_backend(
            {"abort-order.mock", backend.api()},
            backend_handle),
        rt::Status::ok);
    rt::CpuMemoryPolicy policy;
    add_strict_role(policy, rt::thread_role_device_service);
    ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    EXPECT_NE(runtime.start(), rt::Status::ok);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    EXPECT_EQ(callbacks, 0u);

    const std::array<std::uint32_t, 4> expected_joins{
        event(event_join, rt::thread_role_device_service, 0),
        event(event_join, rt::thread_role_executor_worker, 1),
        event(event_join, rt::thread_role_executor_worker, 0),
        event(event_join, rt::thread_role_watchdog, 0),
    };
    std::array<std::uint32_t, 4> actual_joins{};
    std::size_t join_count = 0;
    for (std::size_t index = 0; index < provider.event_count(); ++index) {
        const auto value = provider.event_at(index);
        if (value >= event_join && value < event_join + 1'000) {
            ASSERT_LT(join_count, actual_joins.size());
            actual_joins[join_count++] = value;
        }
    }
    EXPECT_EQ(join_count, expected_joins.size());
    EXPECT_EQ(actual_joins, expected_joins);
}
#endif
