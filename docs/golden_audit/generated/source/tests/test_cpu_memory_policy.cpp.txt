#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include <rt/mock_device.hpp>
#include <rt/runtime.hpp>

namespace {

const rt::ThreadPolicyReport* find_thread(
    const rt::CpuMemoryPolicyReport& report,
    rt::ThreadRoleId role) {
    const auto end = report.threads.begin() +
        static_cast<std::ptrdiff_t>(report.thread_count);
    const auto found = std::find_if(
        report.threads.begin(),
        end,
        [&](const rt::ThreadPolicyReport& row) {
            return row.role == role;
        });
    return found == end ? nullptr : &*found;
}

const rt::MemoryPolicyReport* find_memory(
    const rt::CpuMemoryPolicyReport& report,
    rt::MemoryRegionId region) {
    const auto end = report.memory.begin() +
        static_cast<std::ptrdiff_t>(report.memory_count);
    const auto found = std::find_if(
        report.memory.begin(),
        end,
        [&](const rt::MemoryPolicyReport& row) {
            return row.region == region;
        });
    return found == end ? nullptr : &*found;
}

std::string_view stable_name(
    const std::array<char, rt::resource_accounting_name_capacity>& name) {
    const auto end = std::find(name.begin(), name.end(), '\0');
    return {name.data(), static_cast<std::size_t>(end - name.begin())};
}

rt::CallbackResult count_callback(
    void* user_data,
    const rt::CallbackContext&) {
    ++*static_cast<std::size_t*>(user_data);
    return rt::CallbackResult::ok;
}

rt::Status host_submit(
    void*,
    const rt::HostExecutorJob&) noexcept {
    return rt::Status::queue_full;
}

bool host_try_execute_one(void*) noexcept {
    return false;
}

void expect_invalid_policy(
    const rt::CpuMemoryPolicy& policy,
    std::string_view diagnostic) {
    rt::Runtime runtime;
    ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
    EXPECT_NE(runtime.last_error().find(diagnostic), std::string_view::npos);
    rt::CpuMemoryPolicyReport report;
    EXPECT_FALSE(runtime.cpu_memory_policy_report(report));
}

} // namespace

TEST(CpuMemoryPolicy, ReportRetainsPreM15_03AggregatePrefix) {
    rt::MemoryPolicy requested;
    requested.prefault = rt::PolicyToggle::enabled;
    rt::MemoryPolicy resolved;
    resolved.prefault = rt::PolicyToggle::enabled;
    const rt::MemoryPolicyReport row{
        rt::memory_region_phase_scratch,
        {17},
        {},
        rt::ResourceOwnership::runtime,
        rt::MemoryAccountingScope::planned,
        1,
        true,
        64,
        64,
        64,
        0,
        0,
        false,
        requested,
        resolved,
        rt::PolicyResolutionState::native_supported,
        rt::PolicyOperationState::succeeded,
        rt::PolicyOperationState::succeeded,
    };

    EXPECT_EQ(row.region, rt::memory_region_phase_scratch);
    EXPECT_EQ(row.accounting_key.value, 17u);
    EXPECT_EQ(row.requested.prefault, rt::PolicyToggle::enabled);
    EXPECT_EQ(row.resolved.prefault, rt::PolicyToggle::enabled);
    EXPECT_EQ(row.applied, rt::PolicyOperationState::succeeded);
    EXPECT_EQ(row.verified, rt::PolicyOperationState::succeeded);
    EXPECT_EQ(row.acquired, rt::PolicyOperationState::not_attempted);
    EXPECT_EQ(
        row.accounting_exactness,
        rt::ResourceAccountingExactness::unknown);

    const rt::CpuMemoryPolicy old_policy{1, {}, 1, {}};
    EXPECT_EQ(old_policy.thread_policy_count, 1u);
    EXPECT_EQ(old_policy.memory_policy_count, 1u);
    EXPECT_EQ(
        old_policy.accounting_requirement,
        rt::PolicyRequirement::best_effort);
    EXPECT_EQ(old_policy.accounting_declaration_count, 0u);

    const rt::CpuMemoryPolicyReport old_report{
        rt::cpu_memory_policy_schema_version, 0, {}, 0, {}};
    EXPECT_EQ(
        old_report.closed_total.exactness,
        rt::ResourceAccountingExactness::not_applicable);
    EXPECT_FALSE(old_report.accounting_complete);
}

TEST(CpuMemoryPolicy, DefaultsInventoryEveryStableRoleAndMemoryIdentity) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 1;
    config.worker_count = 3;
    config.executor_queue_capacity = 4;
    config.task_scratch_slots = 4;
    config.trace_capacity = 7;
    config.watchdog_timeout_ns = 1'000'000;
    config.device_backend_capacity = 1;
    config.device_buffer_capacity = 1;
    config.device_outstanding_capacity = 2;
    config.device_completion_batch = 2;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);

    std::size_t calls = 0;
    ASSERT_EQ(
        runtime.register_callback({"policy.phase", &count_callback, &calls}),
        rt::Status::ok);
    std::array<std::byte, 17> state{};
    ASSERT_EQ(
        runtime.register_state({"policy.state", 1, state}),
        rt::Status::ok);
    rt::MockDeviceBackend backend({2, 1, 1, 1'000});
    rt::DeviceBackendHandle backend_handle;
    ASSERT_EQ(
        runtime.register_device_backend(
            {"policy.mock", backend.api()},
            backend_handle),
        rt::Status::ok);
    std::array<std::byte, 23> buffer{};
    rt::DeviceBufferHandle buffer_handle;
    ASSERT_EQ(
        runtime.register_device_buffer(
            {"policy.buffer", backend_handle, buffer},
            buffer_handle),
        rt::Status::ok);

    rt::CpuMemoryPolicyReport unavailable;
    EXPECT_FALSE(runtime.cpu_memory_policy_report(unavailable));
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    EXPECT_EQ(report.schema_version, rt::cpu_memory_policy_schema_version);
    EXPECT_EQ(report.thread_count, 6u);
    EXPECT_EQ(report.memory_count, 12u);

    const auto* frame = find_thread(report, rt::thread_role_frame);
    const auto* executor =
        find_thread(report, rt::thread_role_executor_worker);
    const auto* watchdog = find_thread(report, rt::thread_role_watchdog);
    const auto* device =
        find_thread(report, rt::thread_role_device_service);
    const auto* xdma = find_thread(report, rt::thread_role_xdma_io);
    const auto* submission =
        find_thread(report, rt::thread_role_device_submission);
    ASSERT_NE(frame, nullptr);
    ASSERT_NE(executor, nullptr);
    ASSERT_NE(watchdog, nullptr);
    ASSERT_NE(device, nullptr);
    ASSERT_NE(xdma, nullptr);
    ASSERT_NE(submission, nullptr);
    EXPECT_EQ(stable_name(frame->stable_name), "thread.frame");
    EXPECT_EQ(frame->ownership, rt::ResourceOwnership::caller);
    EXPECT_EQ(frame->application_mode, rt::PolicyApplicationMode::verify_only);
    EXPECT_EQ(frame->logical_instance_count, 1u);
    EXPECT_TRUE(frame->cardinality_known);
    EXPECT_EQ(executor->ownership, rt::ResourceOwnership::runtime);
    EXPECT_EQ(executor->logical_instance_count, 3u);
    EXPECT_EQ(executor->resolved.wait_strategy, rt::WaitStrategy::yield);
    EXPECT_EQ(watchdog->logical_instance_count, 1u);
    EXPECT_EQ(watchdog->resolved.wait_strategy, rt::WaitStrategy::park);
    EXPECT_EQ(device->logical_instance_count, 1u);
    EXPECT_EQ(device->resolved.wait_strategy, rt::WaitStrategy::park);
    EXPECT_EQ(xdma->ownership, rt::ResourceOwnership::backend);
    EXPECT_EQ(xdma->application_mode, rt::PolicyApplicationMode::verify_only);
    EXPECT_FALSE(xdma->cardinality_known);
    EXPECT_EQ(submission->ownership, rt::ResourceOwnership::runtime);
    EXPECT_EQ(submission->application_mode,
              rt::PolicyApplicationMode::apply_and_verify);
    EXPECT_EQ(submission->logical_instance_count, 0u);
    EXPECT_TRUE(submission->cardinality_known);
    for (std::size_t index = 0; index < report.thread_count; ++index) {
        EXPECT_EQ(
            report.threads[index].applied,
            rt::PolicyOperationState::not_attempted);
        EXPECT_EQ(
            report.threads[index].verified,
            rt::PolicyOperationState::not_attempted);
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            EXPECT_NE(
                report.threads[index].accounting_key.value,
                report.threads[earlier].accounting_key.value);
        }
    }

    rt::MemoryPlan plan;
    ASSERT_TRUE(runtime.memory_plan(plan));
    std::size_t planned_sum = 0;
    for (std::size_t index = 0; index < report.memory_count; ++index) {
        const auto& row = report.memory[index];
        if (row.accounting_scope == rt::MemoryAccountingScope::planned) {
            planned_sum += row.accounted_bytes;
        }
        const bool resident_region =
            row.region == rt::memory_region_phase_scratch ||
            row.region == rt::memory_region_task_scratch ||
            row.region == rt::memory_region_trace_storage;
        EXPECT_EQ(
            row.committed_bytes,
            resident_region ? row.accounted_bytes : 0u);
        EXPECT_EQ(row.resident_bytes, 0u);
        EXPECT_EQ(row.locked_bytes, 0u);
        EXPECT_EQ(row.pinned_bytes, 0u);
        EXPECT_FALSE(row.used_huge_page_fallback);
        EXPECT_EQ(row.applied, rt::PolicyOperationState::not_attempted);
        EXPECT_EQ(row.verified, rt::PolicyOperationState::not_attempted);
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            EXPECT_NE(
                row.accounting_key.value,
                report.memory[earlier].accounting_key.value);
        }
    }
    EXPECT_EQ(planned_sum, plan.planned_bytes);
    EXPECT_EQ(report.planned_total.accounted_bytes, plan.planned_bytes);
    EXPECT_EQ(
        report.planned_total.exactness,
        rt::ResourceAccountingExactness::exact);
    EXPECT_FALSE(report.accounting_complete);

    const auto* runtime_control =
        find_memory(report, rt::memory_region_runtime_control);
    const auto* executor_control =
        find_memory(report, rt::memory_region_executor_control);
    const auto* device_control =
        find_memory(report, rt::memory_region_device_control);
    ASSERT_NE(runtime_control, nullptr);
    ASSERT_NE(executor_control, nullptr);
    ASSERT_NE(device_control, nullptr);
    EXPECT_EQ(runtime_control->accounted_bytes, plan.runtime_control_bytes);
    EXPECT_EQ(executor_control->accounted_bytes, plan.executor_control_bytes);
    EXPECT_EQ(device_control->accounted_bytes, plan.device_control_bytes);
    EXPECT_EQ(
        runtime_control->accounting_exactness,
        rt::ResourceAccountingExactness::exact);
    EXPECT_EQ(
        executor_control->accounting_exactness,
        rt::ResourceAccountingExactness::exact);
    EXPECT_EQ(
        device_control->accounting_exactness,
        rt::ResourceAccountingExactness::exact);

    const auto* state_row =
        find_memory(report, rt::memory_region_registered_state);
    const auto* buffer_row =
        find_memory(report, rt::memory_region_registered_device_buffer);
    const auto* runtime_stacks =
        find_memory(report, rt::memory_region_runtime_thread_stack);
    const auto* external_stacks =
        find_memory(report, rt::memory_region_external_thread_stack);
    ASSERT_NE(state_row, nullptr);
    ASSERT_NE(buffer_row, nullptr);
    ASSERT_NE(runtime_stacks, nullptr);
    ASSERT_NE(external_stacks, nullptr);
    EXPECT_EQ(state_row->logical_region_count, 1u);
    EXPECT_EQ(state_row->accounted_bytes, state.size());
    EXPECT_EQ(buffer_row->logical_region_count, 1u);
    EXPECT_EQ(buffer_row->accounted_bytes, buffer.size());
    EXPECT_EQ(runtime_stacks->logical_region_count, 5u);
    EXPECT_EQ(runtime_stacks->accounting_scope, rt::MemoryAccountingScope::excluded);
    EXPECT_EQ(
        runtime_stacks->resolution,
        rt::PolicyResolutionState::portable_default);
    EXPECT_EQ(external_stacks->logical_region_count, 1u);
    EXPECT_FALSE(external_stacks->cardinality_known);
    EXPECT_EQ(
        external_stacks->resolution,
        rt::PolicyResolutionState::portable_default);
}

TEST(CpuMemoryPolicy, BoundedDeclarationsCloseExternalFactsWithoutQualification) {
    rt::Runtime runtime;
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
    const auto* frame = find_thread(report, rt::thread_role_frame);
    const auto* external =
        find_memory(report, rt::memory_region_external_thread_stack);
    ASSERT_NE(frame, nullptr);
    ASSERT_NE(external, nullptr);
    EXPECT_EQ(frame->declared_accounted_bytes, 8u * 1024u * 1024u);
    EXPECT_EQ(
        frame->accounting_exactness,
        rt::ResourceAccountingExactness::declared_only);
    EXPECT_EQ(external->logical_region_count, 1u);
    EXPECT_EQ(external->accounted_bytes, 8u * 1024u * 1024u);
    EXPECT_EQ(
        external->accounting_exactness,
        rt::ResourceAccountingExactness::declared_only);
    EXPECT_FALSE(report.accounting_complete);

#if defined(__linux__)
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    EXPECT_TRUE(report.accounting_complete);
    EXPECT_EQ(
        report.closed_total.exactness,
        rt::ResourceAccountingExactness::declared_only);
#else
    EXPECT_EQ(runtime.start(), rt::Status::invalid_config);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    EXPECT_FALSE(report.accounting_complete);
    EXPECT_EQ(
        report.closed_total.exactness,
        rt::ResourceAccountingExactness::partial);
    EXPECT_NE(
        runtime.last_error().find("live runtime stack facts"),
        std::string_view::npos);
#endif
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(CpuMemoryPolicy, StrictClosureRejectsMissingFactsBeforeNativeMutation) {
    rt::Runtime runtime;
    rt::CpuMemoryPolicy policy;
    policy.accounting_requirement = rt::PolicyRequirement::strict;
    ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
    EXPECT_NE(
        runtime.last_error().find("external declarations"),
        std::string_view::npos);
}

TEST(CpuMemoryPolicy, FullyDeclaredHostAdapterClosesWithoutNativeOwnership) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.executor_policy = rt::ExecutorPolicy::host_adapter;
    config.worker_count = 2;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(
        runtime.set_host_executor({
            nullptr,
            2,
            8,
            &host_submit,
            &host_try_execute_one,
        }),
        rt::Status::ok);
    rt::CpuMemoryPolicy policy;
    policy.accounting_requirement = rt::PolicyRequirement::strict;
    policy.accounting_declaration_count = 2;
    policy.accounting_declarations[0] = {
        rt::thread_resource_accounting_key(rt::thread_role_frame),
        1,
        8u * 1024u * 1024u,
    };
    policy.accounting_declarations[1] = {
        rt::thread_resource_accounting_key(
            rt::thread_role_executor_worker),
        2,
        16u * 1024u * 1024u,
    };
    ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    const auto* executor = find_thread(
        report,
        rt::thread_role_executor_worker);
    const auto* external = find_memory(
        report,
        rt::memory_region_external_thread_stack);
    const auto* runtime_stack = find_memory(
        report,
        rt::memory_region_runtime_thread_stack);
    ASSERT_NE(executor, nullptr);
    ASSERT_NE(external, nullptr);
    ASSERT_NE(runtime_stack, nullptr);
    EXPECT_EQ(executor->ownership, rt::ResourceOwnership::host_executor);
    EXPECT_EQ(
        executor->accounting_exactness,
        rt::ResourceAccountingExactness::declared_only);
    EXPECT_EQ(external->logical_region_count, 3u);
    EXPECT_EQ(external->accounted_bytes, 24u * 1024u * 1024u);
    EXPECT_EQ(
        external->accounting_exactness,
        rt::ResourceAccountingExactness::declared_only);
    EXPECT_EQ(runtime_stack->logical_region_count, 0u);
    EXPECT_EQ(
        runtime_stack->accounting_exactness,
        rt::ResourceAccountingExactness::exact);
    EXPECT_TRUE(report.accounting_complete);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(CpuMemoryPolicy, RejectsDuplicateOwnedAndContradictoryDeclarations) {
    rt::CpuMemoryPolicy capacity;
    capacity.accounting_declaration_count =
        rt::resource_accounting_declaration_capacity + 1;
    expect_invalid_policy(capacity, "capacity");

    rt::CpuMemoryPolicy malformed;
    malformed.accounting_declaration_count = 1;
    malformed.accounting_declarations[0] = {
        rt::thread_resource_accounting_key(rt::thread_role_frame),
        1,
        std::numeric_limits<std::size_t>::max(),
    };
    expect_invalid_policy(malformed, "malformed or exceeds bounds");

    rt::CpuMemoryPolicy duplicate;
    duplicate.accounting_declaration_count = 2;
    duplicate.accounting_declarations[0].accounting_key =
        rt::thread_resource_accounting_key(rt::thread_role_frame);
    duplicate.accounting_declarations[0].logical_region_count = 1;
    duplicate.accounting_declarations[1] =
        duplicate.accounting_declarations[0];
    expect_invalid_policy(duplicate, "duplicate key");

    rt::CpuMemoryPolicy owned;
    owned.accounting_declaration_count = 1;
    owned.accounting_declarations[0] = {
        rt::memory_resource_accounting_key(
            rt::memory_region_runtime_control),
        1,
        1,
    };
    expect_invalid_policy(owned, "owned identity");

    rt::CpuMemoryPolicy aggregate_cardinality;
    aggregate_cardinality.accounting_declaration_count = 1;
    aggregate_cardinality.accounting_declarations[0] = {
        rt::memory_resource_accounting_key(
            rt::memory_region_external_thread_stack),
        2,
        16u * 1024u * 1024u,
    };
    expect_invalid_policy(aggregate_cardinality, "known cardinality");

    rt::Runtime host_runtime;
    rt::RuntimeConfig host_config;
    host_config.executor_policy = rt::ExecutorPolicy::host_adapter;
    host_config.worker_count = 2;
    host_config.executor_queue_capacity = 8;
    host_config.task_scratch_slots = 8;
    ASSERT_EQ(host_runtime.configure(host_config), rt::Status::ok);
    ASSERT_EQ(
        host_runtime.set_host_executor({
            nullptr,
            2,
            8,
            &host_submit,
            &host_try_execute_one,
        }),
        rt::Status::ok);
    rt::CpuMemoryPolicy host_cardinality;
    host_cardinality.accounting_declaration_count = 1;
    host_cardinality.accounting_declarations[0] = {
        rt::thread_resource_accounting_key(
            rt::thread_role_executor_worker),
        3,
        24u * 1024u * 1024u,
    };
    ASSERT_EQ(
        host_runtime.set_cpu_memory_policy(host_cardinality),
        rt::Status::ok);
    EXPECT_EQ(host_runtime.finalize(), rt::Status::invalid_config);
    EXPECT_NE(
        host_runtime.last_error().find("known cardinality"),
        std::string_view::npos);

    rt::Runtime runtime;
    rt::MockDeviceBackend backend({2, 1, 1, 1'000});
    rt::RuntimeConfig config;
    config.device_backend_capacity = 1;
    config.device_buffer_capacity = 1;
    config.device_outstanding_capacity = 2;
    config.device_completion_batch = 2;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    rt::DeviceBackendHandle handle;
    ASSERT_EQ(
        runtime.register_device_backend({"decl.mock", backend.api()}, handle),
        rt::Status::ok);
    rt::CpuMemoryPolicy contradiction;
    contradiction.accounting_declaration_count = 1;
    contradiction.accounting_declarations[0] = {
        rt::memory_resource_accounting_key(rt::memory_region_backend_control),
        1,
        1,
    };
    ASSERT_EQ(runtime.set_cpu_memory_policy(contradiction), rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    EXPECT_NE(runtime.last_error().find("device ABI v1"), std::string_view::npos);
}

TEST(CpuMemoryPolicy, BestEffortRequestsResolveToExplicitPortableNoops) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 1;
    config.worker_count = 1;
    config.executor_queue_capacity = 4;
    config.task_scratch_slots = 4;
    config.scratch_alignment = 64;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);

    rt::CpuMemoryPolicy policy;
    policy.thread_policy_count = 1;
    auto& thread = policy.thread_policies[0];
    thread.role = {rt::thread_role_custom_first};
    thread.policy.wait_strategy = rt::WaitStrategy::spin;
    constexpr std::string_view requested_name = "rtfw.worker";
    std::copy(
        requested_name.begin(),
        requested_name.end(),
        thread.policy.name.begin());

    policy.memory_policy_count = 1;
    auto& memory = policy.memory_policies[0];
    memory.region = rt::memory_region_phase_scratch;
    memory.policy.provider = rt::MemoryProviderOwnership::host;
    memory.policy.alignment = 4096;
    memory.policy.page_rounding = rt::PageRounding::base_page;
    memory.policy.guard_bytes_before = 4096;
    memory.policy.guard_bytes_after = 4096;
    memory.policy.prefault = rt::PolicyToggle::enabled;
    memory.policy.locking = rt::PolicyToggle::enabled;
    memory.policy.pinning = rt::PolicyToggle::enabled;
    memory.policy.huge_pages = rt::HugePagePreference::prefer;
    memory.policy.huge_page_fallback = rt::PolicyToggle::enabled;
    memory.policy.numa_node = 2;
    memory.policy.first_touch = rt::FirstTouchPolicy::owner_thread;
    memory.policy.residency_verification = rt::PolicyToggle::enabled;
    memory.policy.rollback = rt::RollbackIntent::release;
    ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);

    std::size_t calls = 0;
    ASSERT_EQ(
        runtime.register_callback({"policy.noop", &count_callback, &calls}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    const auto* thread_row =
        find_thread(report, rt::ThreadRoleId{rt::thread_role_custom_first});
    const auto* memory_row =
        find_memory(report, rt::memory_region_phase_scratch);
    ASSERT_NE(thread_row, nullptr);
    ASSERT_NE(memory_row, nullptr);
    EXPECT_EQ(
        thread_row->resolution,
        rt::PolicyResolutionState::external_verify_only);
    EXPECT_EQ(thread_row->requested.wait_strategy, rt::WaitStrategy::spin);
    EXPECT_EQ(thread_row->resolved.cpu_set.count, 0u);
    EXPECT_EQ(thread_row->resolved.scheduling_class, rt::SchedulingClass::inherit);
    EXPECT_EQ(thread_row->application_mode, rt::PolicyApplicationMode::verify_only);
    EXPECT_FALSE(thread_row->cardinality_known);
    EXPECT_EQ(
        memory_row->resolution,
        rt::PolicyResolutionState::native_best_effort_fallback);
    EXPECT_EQ(memory_row->requested.huge_pages, rt::HugePagePreference::prefer);
    EXPECT_EQ(memory_row->resolved.provider, rt::MemoryProviderOwnership::runtime);
    EXPECT_EQ(memory_row->resolved.alignment, 4096u);
    EXPECT_EQ(memory_row->resolved.locking, rt::PolicyToggle::disabled);

    ASSERT_EQ(runtime.start(), rt::Status::ok);
    EXPECT_EQ(
        runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(CpuMemoryPolicy, RejectsDuplicateMalformedAndContradictoryRequests) {
    rt::CpuMemoryPolicy capacity;
    capacity.thread_policy_count = rt::thread_policy_request_capacity + 1;
    expect_invalid_policy(capacity, "capacity");

    rt::CpuMemoryPolicy cpu_capacity;
    cpu_capacity.thread_policy_count = 1;
    cpu_capacity.thread_policies[0].role = rt::thread_role_frame;
    cpu_capacity.thread_policies[0].policy.cpu_set.count =
        rt::cpu_set_capacity + 1;
    expect_invalid_policy(cpu_capacity, "malformed or contradictory");

    rt::CpuMemoryPolicy invalid_role;
    invalid_role.thread_policy_count = 1;
    invalid_role.thread_policies[0].role = {7};
    expect_invalid_policy(invalid_role, "malformed or contradictory");

    rt::CpuMemoryPolicy invalid_region;
    invalid_region.memory_policy_count = 1;
    invalid_region.memory_policies[0].region = {13};
    expect_invalid_policy(invalid_region, "malformed or contradictory");

    rt::CpuMemoryPolicy invalid_enum;
    invalid_enum.thread_policy_count = 1;
    invalid_enum.thread_policies[0].role = rt::thread_role_frame;
    invalid_enum.thread_policies[0].policy.wait_strategy =
        static_cast<rt::WaitStrategy>(255);
    expect_invalid_policy(invalid_enum, "malformed or contradictory");

    rt::CpuMemoryPolicy duplicate_thread;
    duplicate_thread.thread_policy_count = 2;
    duplicate_thread.thread_policies[0].role = rt::thread_role_frame;
    duplicate_thread.thread_policies[1].role = rt::thread_role_frame;
    expect_invalid_policy(duplicate_thread, "duplicate role");

    rt::CpuMemoryPolicy duplicate_memory;
    duplicate_memory.memory_policy_count = 2;
    duplicate_memory.memory_policies[0].region =
        rt::memory_region_trace_storage;
    duplicate_memory.memory_policies[1].region =
        rt::memory_region_trace_storage;
    expect_invalid_policy(duplicate_memory, "duplicate region");

    rt::CpuMemoryPolicy duplicate_cpu;
    duplicate_cpu.thread_policy_count = 1;
    duplicate_cpu.thread_policies[0].role =
        rt::thread_role_executor_worker;
    duplicate_cpu.thread_policies[0].policy.cpu_set.count = 2;
    duplicate_cpu.thread_policies[0].policy.cpu_set.cpu_ids[0] = 4;
    duplicate_cpu.thread_policies[0].policy.cpu_set.cpu_ids[1] = 4;
    expect_invalid_policy(duplicate_cpu, "malformed or contradictory");

    rt::CpuMemoryPolicy scheduling;
    scheduling.thread_policy_count = 1;
    scheduling.thread_policies[0].role = rt::thread_role_watchdog;
    scheduling.thread_policies[0].policy.scheduling_class =
        rt::SchedulingClass::normal;
    scheduling.thread_policies[0].policy.scheduling_priority = 1;
    expect_invalid_policy(scheduling, "malformed or contradictory");

    rt::CpuMemoryPolicy name;
    name.thread_policy_count = 1;
    name.thread_policies[0].role = rt::thread_role_frame;
    name.thread_policies[0].policy.name.fill('a');
    expect_invalid_policy(name, "malformed or contradictory");

    rt::CpuMemoryPolicy memory;
    memory.memory_policy_count = 1;
    memory.memory_policies[0].region = rt::memory_region_runtime_control;
    memory.memory_policies[0].policy.huge_page_fallback =
        rt::PolicyToggle::enabled;
    memory.memory_policies[0].policy.huge_pages =
        rt::HugePagePreference::disabled;
    expect_invalid_policy(memory, "malformed or contradictory");
}

TEST(CpuMemoryPolicy, RejectsUnsupportedStrictAndCheckedArithmeticOverflow) {
    rt::CpuMemoryPolicy strict_thread;
    strict_thread.thread_policy_count = 1;
    strict_thread.thread_policies[0].role = {
        rt::thread_role_custom_first};
    strict_thread.thread_policies[0].policy.requirement =
        rt::PolicyRequirement::strict;
    expect_invalid_policy(strict_thread, "strict external custom thread policy");

    rt::CpuMemoryPolicy strict_memory;
    strict_memory.memory_policy_count = 1;
    strict_memory.memory_policies[0].region =
        rt::memory_region_runtime_control;
    strict_memory.memory_policies[0].policy.requirement =
        rt::PolicyRequirement::strict;
    strict_memory.memory_policies[0].policy.prefault =
        rt::PolicyToggle::enabled;
    expect_invalid_policy(strict_memory, "deferred or borrowed");

    rt::Runtime stack_runtime;
    rt::RuntimeConfig stack_config;
    stack_config.worker_count = 2;
    ASSERT_EQ(stack_runtime.configure(stack_config), rt::Status::ok);
    rt::CpuMemoryPolicy stack;
    stack.thread_policy_count = 1;
    stack.thread_policies[0].role = rt::thread_role_executor_worker;
    stack.thread_policies[0].policy.stack_bytes =
        static_cast<std::size_t>(std::uint64_t{1} << 40u);
    ASSERT_EQ(stack_runtime.set_cpu_memory_policy(stack), rt::Status::ok);
    EXPECT_EQ(stack_runtime.finalize(), rt::Status::invalid_config);
    EXPECT_NE(
        stack_runtime.last_error().find("stack policy overflows"),
        std::string_view::npos);

    rt::CpuMemoryPolicy region;
    region.memory_policy_count = 1;
    region.memory_policies[0].region = rt::memory_region_runtime_control;
    region.memory_policies[0].policy.guard_bytes_before =
        static_cast<std::size_t>(std::uint64_t{1} << 40u);
    expect_invalid_policy(region, "bounded accounting");
}

TEST(CpuMemoryPolicy, FailedFinalizationCanReplacePolicyAndRecover) {
    rt::Runtime runtime;
    rt::CpuMemoryPolicy rejected;
    rejected.thread_policy_count = 1;
    rejected.thread_policies[0].role = {
        rt::thread_role_custom_first};
    rejected.thread_policies[0].policy.requirement =
        rt::PolicyRequirement::strict;
    ASSERT_EQ(runtime.set_cpu_memory_policy(rejected), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::invalid_config);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);

    ASSERT_EQ(
        runtime.set_cpu_memory_policy(rt::CpuMemoryPolicy{}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    EXPECT_EQ(report.thread_count, 6u);
    EXPECT_EQ(runtime.set_cpu_memory_policy(rejected), rt::Status::invalid_state);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
}

TEST(CpuMemoryPolicy, ExternalHostAndCustomRolesRemainVerifyOnly) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.executor_policy = rt::ExecutorPolicy::host_adapter;
    config.worker_count = 2;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(
        runtime.set_host_executor({
            nullptr,
            2,
            8,
            &host_submit,
            &host_try_execute_one,
        }),
        rt::Status::ok);
    rt::CpuMemoryPolicy policy;
    policy.thread_policy_count = 1;
    policy.thread_policies[0].role = {
        rt::thread_role_custom_first + 7};
    ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    EXPECT_EQ(report.thread_count, 7u);
    const auto* executor =
        find_thread(report, rt::thread_role_executor_worker);
    const auto* custom = find_thread(
        report,
        rt::ThreadRoleId{rt::thread_role_custom_first + 7});
    const auto* executor_memory =
        find_memory(report, rt::memory_region_executor_control);
    const auto* external_stacks =
        find_memory(report, rt::memory_region_external_thread_stack);
    ASSERT_NE(executor, nullptr);
    ASSERT_NE(custom, nullptr);
    ASSERT_NE(executor_memory, nullptr);
    ASSERT_NE(external_stacks, nullptr);
    EXPECT_EQ(executor->ownership, rt::ResourceOwnership::host_executor);
    EXPECT_EQ(executor->application_mode, rt::PolicyApplicationMode::verify_only);
    EXPECT_EQ(executor->logical_instance_count, 2u);
    EXPECT_EQ(custom->ownership, rt::ResourceOwnership::vendor);
    EXPECT_EQ(custom->application_mode, rt::PolicyApplicationMode::verify_only);
    EXPECT_FALSE(custom->cardinality_known);
    EXPECT_EQ(stable_name(custom->stable_name), "thread.custom.65543");
    EXPECT_EQ(executor_memory->ownership, rt::ResourceOwnership::runtime);
    EXPECT_EQ(external_stacks->logical_region_count, 3u);
    EXPECT_FALSE(external_stacks->cardinality_known);
}

TEST(CpuMemoryPolicy, CustomUnknownCardinalityPropagatesToExternalStacks) {
    rt::Runtime runtime;
    rt::CpuMemoryPolicy policy;
    policy.thread_policy_count = 1;
    policy.thread_policies[0].role = {
        rt::thread_role_custom_first};
    ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(runtime.cpu_memory_policy_report(report));
    const auto* custom = find_thread(
        report,
        rt::ThreadRoleId{rt::thread_role_custom_first});
    const auto* external_stacks =
        find_memory(report, rt::memory_region_external_thread_stack);
    ASSERT_NE(custom, nullptr);
    ASSERT_NE(external_stacks, nullptr);
    EXPECT_FALSE(custom->cardinality_known);
    EXPECT_EQ(external_stacks->logical_region_count, 1u);
    EXPECT_FALSE(external_stacks->cardinality_known);
}

TEST(MemoryPlan, CpuMemoryPolicyTwoRuntimeReportsAreIsolated) {
    rt::Runtime first;
    rt::Runtime second;
    rt::RuntimeConfig first_config;
    first_config.worker_count = 1;
    rt::RuntimeConfig second_config = first_config;
    second_config.worker_count = 4;
    second_config.watchdog_timeout_ns = 1'000;
    ASSERT_EQ(first.configure(first_config), rt::Status::ok);
    ASSERT_EQ(second.configure(second_config), rt::Status::ok);

    rt::CpuMemoryPolicy second_policy;
    second_policy.thread_policy_count = 1;
    second_policy.thread_policies[0].role =
        rt::thread_role_executor_worker;
    second_policy.thread_policies[0].policy.wait_strategy =
        rt::WaitStrategy::spin;
    ASSERT_EQ(second.set_cpu_memory_policy(second_policy), rt::Status::ok);
    ASSERT_EQ(first.finalize(), rt::Status::ok);
    ASSERT_EQ(second.finalize(), rt::Status::ok);

    rt::CpuMemoryPolicyReport first_report;
    rt::CpuMemoryPolicyReport second_report;
    ASSERT_TRUE(first.cpu_memory_policy_report(first_report));
    ASSERT_TRUE(second.cpu_memory_policy_report(second_report));
    const auto* first_executor =
        find_thread(first_report, rt::thread_role_executor_worker);
    const auto* second_executor =
        find_thread(second_report, rt::thread_role_executor_worker);
    const auto* first_watchdog =
        find_thread(first_report, rt::thread_role_watchdog);
    const auto* second_watchdog =
        find_thread(second_report, rt::thread_role_watchdog);
    ASSERT_NE(first_executor, nullptr);
    ASSERT_NE(second_executor, nullptr);
    ASSERT_NE(first_watchdog, nullptr);
    ASSERT_NE(second_watchdog, nullptr);
    EXPECT_EQ(first_executor->logical_instance_count, 1u);
    EXPECT_EQ(second_executor->logical_instance_count, 4u);
#if defined(__linux__)
    EXPECT_EQ(
        first_executor->resolution,
        rt::PolicyResolutionState::native_supported);
#else
    EXPECT_EQ(
        first_executor->resolution,
        rt::PolicyResolutionState::portable_default);
#endif
    EXPECT_EQ(
        second_executor->requested.wait_strategy,
        rt::WaitStrategy::spin);
#if defined(__linux__)
    EXPECT_EQ(second_executor->resolved.wait_strategy, rt::WaitStrategy::spin);
#else
    EXPECT_EQ(second_executor->resolved.wait_strategy, rt::WaitStrategy::yield);
#endif
    EXPECT_EQ(first_watchdog->logical_instance_count, 0u);
    EXPECT_EQ(second_watchdog->logical_instance_count, 1u);

    first_report.threads[0].logical_instance_count = 99;
    rt::CpuMemoryPolicyReport reread;
    ASSERT_TRUE(first.cpu_memory_policy_report(reread));
    EXPECT_EQ(reread.threads[0].logical_instance_count, 1u);
}
