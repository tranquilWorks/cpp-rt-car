#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include <rt/runtime.hpp>

#include "rt/src/rate_dispatch.hpp"
#include "rt/src/snapshot_codec.hpp"

namespace {

using namespace std::chrono_literals;

struct ManualClock final : rt::RuntimeClock {
    std::uint64_t now = 1'000;
    std::size_t sleeps = 0;

    std::uint64_t now_ns() noexcept override { return now; }
    rt::Status sleep_until_ns(std::uint64_t release) noexcept override {
        ++sleeps;
        now = release;
        return rt::Status::ok;
    }
    bool supports_absolute_sleep() const noexcept override { return true; }
};

struct ReleaseProbe {
    std::array<rt::RateReleaseView, 16> releases{};
    std::size_t count = 0;
    rt::CrossRateChannelHandle publish_channel{};
    rt::CrossRateChannelHandle copy_channel{};
    std::array<std::byte, 4> publish_payload{};
    std::array<std::byte, 4> copied_payload{};
    rt::Status publish_status = rt::Status::ok;
    rt::Status duplicate_status = rt::Status::ok;
    rt::CrossRateReadResult read{};
    std::size_t publish_payload_size = 4;
    std::size_t copy_payload_size = 4;
    bool publish = false;
    bool duplicate = false;
    bool copy = false;
    bool fail = false;
};

struct SubstepChannelProbe {
    rt::CrossRateChannelHandle channel{};
    std::array<rt::CrossRateReadResult, 4> reads{};
    std::array<std::byte, 4> values{};
    std::size_t count = 0;
    bool producer = false;
};

rt::CallbackResult substep_channel_callback(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& probe = *static_cast<SubstepChannelProbe*>(user_data);
    if (!context.rate_release || probe.count >= probe.values.size()) {
        return rt::CallbackResult::error;
    }
    if (probe.producer) {
        const std::array payload{
            static_cast<std::byte>(
                context.rate_release->substep_ordinal + 1)};
        ++probe.count;
        return context.rate_release->publish(probe.channel, payload) ==
                rt::Status::ok
            ? rt::CallbackResult::ok
            : rt::CallbackResult::error;
    }
    std::array<std::byte, 1> payload{};
    auto& read = probe.reads[probe.count];
    const auto status = context.rate_release->copy(
        probe.channel,
        payload,
        read);
    probe.values[probe.count] = payload[0];
    ++probe.count;
    return status == rt::CrossRateReadStatus::ok
        ? rt::CallbackResult::ok
        : rt::CallbackResult::error;
}

struct EncodedState {
    std::string_view name{};
    std::uint32_t schema_version = 0;
    std::span<const std::byte> payload{};
};

bool provide_encoded_state(
    void* context,
    std::size_t index,
    rt::detail::StateWriteView& output) noexcept {
    if (index != 0) {
        output = {};
        return false;
    }
    const auto& state = *static_cast<const EncodedState*>(context);
    output = {state.name, state.schema_version, state.payload};
    return true;
}

void store_u64_le(
    std::span<std::byte> bytes,
    std::size_t offset,
    std::uint64_t value) {
    ASSERT_LE(offset + sizeof(value), bytes.size());
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8)) & 0xffu);
    }
}

rt::CallbackResult release_callback(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& probe = *static_cast<ReleaseProbe*>(user_data);
    if (!context.rate_release || probe.count >= probe.releases.size()) {
        return rt::CallbackResult::error;
    }
    probe.releases[probe.count++] = *context.rate_release;
    if (probe.publish) {
        probe.publish_status = context.rate_release->publish(
            probe.publish_channel,
            std::span<const std::byte>(
                probe.publish_payload.data(),
                std::min(
                    probe.publish_payload_size,
                    probe.publish_payload.size())));
        if (probe.duplicate) {
            probe.duplicate_status = context.rate_release->publish(
                probe.publish_channel,
                std::span<const std::byte>(
                    probe.publish_payload.data(),
                    std::min(
                        probe.publish_payload_size,
                        probe.publish_payload.size())));
        }
    }
    if (probe.copy) {
        (void)context.rate_release->copy(
            probe.copy_channel,
            std::span<std::byte>(
                probe.copied_payload.data(),
                std::min(
                    probe.copy_payload_size,
                    probe.copied_payload.size())),
            probe.read);
    }
    return probe.fail ? rt::CallbackResult::error : rt::CallbackResult::ok;
}

rt::CallbackResult legacy_callback(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& calls = *static_cast<std::size_t*>(user_data);
    ++calls;
    EXPECT_EQ(context.rate_release, nullptr);
    return rt::CallbackResult::ok;
}

rt::RuntimeConfig test_config() {
    rt::RuntimeConfig config;
    config.callback_capacity = 8;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    config.snapshot_max_bytes = 64 * 1024;
    config.memory_budget_bytes = 2 * 1024 * 1024;
    config.watchdog_max_degradation_level = 3;
    return config;
}

struct DeviceAdmissionFixture {
    std::uint32_t owner = 77;
    rt::PhaseHandle phase{owner, 0};
    rt::RateDomainHandle domain{owner, 0};
    rt::DeviceBackendHandle backend{owner, 0};
    rt::DeviceBufferHandle buffer{owner, 0};
    rt::DeviceTimelineHandle timeline{owner, 0};
    rt::DeviceCommandBatch declaration{};
    rt::detail::CompiledRatePlan rate_plan{};
    std::vector<rt::detail::DeviceRateBindingSpec> bindings;
    std::vector<rt::detail::DeviceRateBackendSource> backends;
    std::vector<rt::detail::DeviceRateBufferSource> buffers;
    std::vector<rt::detail::DeviceRateTimelineSource> timelines;
    std::vector<rt::detail::DeviceRatePhaseSource> phases;

    DeviceAdmissionFixture() {
        declaration.command_count = 1;
        declaration.signal_count = 1;
        auto& command = declaration.commands[0];
        command.kind = static_cast<std::uint32_t>(
            rt::HalV2CommandKind::dispatch);
        command.opcode = 17;
        command.buffer_count = 1;
        command.buffers[0] = {
            buffer.value, RTFW_DEVICE_ACCESS_READ, 0, 0, 64};
        declaration.signals[0].timeline_handle = timeline.value;

        rate_plan.supercycle_ns = 10;
        rate_plan.domains.push_back({
            domain, {}, 0, 10, 1, 10, 0,
            rt::RateCriticality::critical, false, 1, 1, 1,
            rt::RateLateAction::fail, 0});
        rate_plan.bindings.push_back({
            phase, domain, rt::RatePhaseKind::device, 0});
        rate_plan.releases.push_back({
            0, domain, 0, 0, phase, rt::RatePhaseKind::device, 0,
            0, 1, 6, 6, 0, rt::RateCriticality::critical, false,
            rt::RateLateAction::fail, 0});
        rate_plan.releases.push_back({
            6, domain, 0, 1, phase, rt::RatePhaseKind::device, 0,
            0, 1, 6, 12, 0, rt::RateCriticality::critical, false,
            rt::RateLateAction::fail, 0});

        bindings.push_back({
            phase, domain, 6, 2, {rt::DeviceRatePayloadRole::input}});
        rt::HalV2CommandTimelineCapabilities capabilities;
        capabilities.max_in_flight_batches = 2;
        capabilities.max_commands_per_batch = 4;
        capabilities.max_wait_points = 4;
        capabilities.max_signal_points = 4;
        capabilities.max_timelines = 4;
        capabilities.completion_batch_capacity = 2;
        backends.push_back({backend, capabilities, 9, true});
        buffers.push_back({
            buffer, backend, 64,
            RTFW_DEVICE_BUFFER_DEVICE_READ |
                RTFW_DEVICE_BUFFER_DEVICE_WRITE,
            1, 1});
        timelines.push_back({timeline, backend, 0});
        phases.push_back({phase, backend, &declaration});
    }
};

rt::Status add_domain(
    rt::Runtime& runtime,
    std::string_view name,
    std::uint64_t period,
    std::uint64_t deadline,
    std::uint64_t budget,
    rt::RateLateAction action,
    std::uint32_t catch_up_limit,
    rt::RateDomainHandle& domain) {
    return runtime.register_rate_domain(
        {name,
         period,
         1,
         deadline,
         budget,
         rt::RateCriticality::normal,
         false,
         action,
         catch_up_limit},
        domain);
}

void expect_stopped(rt::Runtime& runtime) {
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

} // namespace

TEST(RateDispatch, DeviceRateCyclicAdmissionModelsWrapAndHalfOpenCapacity) {
    DeviceAdmissionFixture fixture;
    rt::detail::CompiledDeviceRatePlan plan;
    rt::detail::DeviceRateDiagnostic diagnostic;
    ASSERT_EQ(
        rt::detail::compile_device_rate_plan(
            fixture.owner, 2, 2, fixture.rate_plan, fixture.bindings,
            fixture.backends, fixture.buffers, fixture.timelines,
            fixture.phases, plan, diagnostic),
        rt::Status::ok) << (diagnostic.message ? diagnostic.message : "");
    ASSERT_EQ(plan.phases.size(), 1u);
    ASSERT_EQ(plan.commands.size(), 1u);
    ASSERT_EQ(plan.payload_references.size(), 1u);
    ASSERT_EQ(plan.timeline_references.size(), 1u);
    ASSERT_EQ(plan.admission_intervals.size(), 2u);
    EXPECT_EQ(plan.report.peak_global_in_flight, 2u);
    EXPECT_EQ(plan.admission_phases[0].peak_in_flight, 2u);
    EXPECT_TRUE(plan.admission_intervals[1].carries_across_supercycle);
    EXPECT_EQ(plan.payload_references[0].buffer, fixture.buffer);
    EXPECT_EQ(plan.payload_references[0].role,
              rt::DeviceRatePayloadRole::input);
    EXPECT_EQ(plan.timeline_references[0].timeline, fixture.timeline);

    fixture.bindings[0].maximum_in_flight = 1;
    EXPECT_EQ(
        rt::detail::compile_device_rate_plan(
            fixture.owner, 2, 2, fixture.rate_plan, fixture.bindings,
            fixture.backends, fixture.buffers, fixture.timelines,
            fixture.phases, plan, diagnostic),
        rt::Status::invalid_config);
    EXPECT_EQ(diagnostic.reference_index, 0u);
    EXPECT_STREQ(diagnostic.message,
                 "device-rate release exceeds an in-flight capacity");
}

TEST(RateDispatch, DeviceRateRejectsPayloadAndCompletionCapacityMismatch) {
    DeviceAdmissionFixture fixture;
    rt::detail::CompiledDeviceRatePlan plan;
    rt::detail::DeviceRateDiagnostic diagnostic;
    fixture.bindings[0].payload_roles[0] =
        rt::DeviceRatePayloadRole::output;
    EXPECT_EQ(
        rt::detail::compile_device_rate_plan(
            fixture.owner, 2, 2, fixture.rate_plan, fixture.bindings,
            fixture.backends, fixture.buffers, fixture.timelines,
            fixture.phases, plan, diagnostic),
        rt::Status::invalid_config);
    EXPECT_STREQ(diagnostic.message,
                 "device-rate payload roles do not exactly match the copied declaration");

    fixture.bindings[0].payload_roles[0] =
        rt::DeviceRatePayloadRole::input;
    fixture.rate_plan.releases[1].release_time_ns = 0;
    fixture.rate_plan.releases[1].deadline_time_ns = 6;
    fixture.rate_plan.releases[1].substep_ordinal = 1;
    fixture.backends[0].capabilities.completion_batch_capacity = 1;
    EXPECT_EQ(
        rt::detail::compile_device_rate_plan(
            fixture.owner, 2, 2, fixture.rate_plan, fixture.bindings,
            fixture.backends, fixture.buffers, fixture.timelines,
            fixture.phases, plan, diagnostic),
        rt::Status::invalid_config);
    EXPECT_STREQ(diagnostic.message,
                 "device-rate poll boundary exceeds completion-batch capacity");
}

TEST(RateDispatch, DeviceRateAllowsDistinctBackendOverlapAndRejectsWaitCycle) {
    DeviceAdmissionFixture fixture;
    const rt::PhaseHandle second_phase{fixture.owner, 1};
    const rt::DeviceBackendHandle second_backend{fixture.owner, 1};
    const rt::DeviceBufferHandle second_buffer{fixture.owner, 1};
    const rt::DeviceTimelineHandle second_timeline{fixture.owner, 1};
    auto second_declaration = fixture.declaration;
    second_declaration.commands[0].buffers[0].buffer_token =
        second_buffer.value;
    second_declaration.signals[0].timeline_handle = second_timeline.value;
    fixture.rate_plan.bindings.push_back({
        second_phase, fixture.domain, rt::RatePhaseKind::device, 1});
    fixture.rate_plan.releases.push_back({
        0, fixture.domain, 0, 2, second_phase,
        rt::RatePhaseKind::device, 1, 0, 1, 6, 6, 0,
        rt::RateCriticality::critical, false,
        rt::RateLateAction::fail, 0});
    fixture.bindings.push_back({
        second_phase, fixture.domain, 6, 1,
        {rt::DeviceRatePayloadRole::input}});
    auto second_capabilities = fixture.backends[0].capabilities;
    second_capabilities.max_in_flight_batches = 1;
    fixture.backends.push_back({
        second_backend, second_capabilities, 10, true});
    fixture.buffers.push_back({
        second_buffer, second_backend, 64,
        RTFW_DEVICE_BUFFER_DEVICE_READ |
            RTFW_DEVICE_BUFFER_DEVICE_WRITE,
        1, 1});
    fixture.timelines.push_back({second_timeline, second_backend, 0});
    fixture.phases.push_back({
        second_phase, second_backend, &second_declaration});

    rt::detail::CompiledDeviceRatePlan plan;
    rt::detail::DeviceRateDiagnostic diagnostic;
    ASSERT_EQ(
        rt::detail::compile_device_rate_plan(
            fixture.owner, 3, 2, fixture.rate_plan, fixture.bindings,
            fixture.backends, fixture.buffers, fixture.timelines,
            fixture.phases, plan, diagnostic),
        rt::Status::ok) << (diagnostic.message ? diagnostic.message : "");
    ASSERT_EQ(plan.admission_backends.size(), 2u);
    EXPECT_EQ(plan.report.peak_global_in_flight, 3u);
    EXPECT_EQ(plan.admission_backends[0].peak_in_flight, 2u);
    EXPECT_EQ(plan.admission_backends[1].peak_in_flight, 1u);

    fixture.backends[0].capabilities.max_in_flight_batches = 3;
    fixture.phases[1].backend = fixture.backend;
    fixture.buffers[1].backend = fixture.backend;
    fixture.timelines[1].backend = fixture.backend;
    fixture.declaration.wait_count = 1;
    fixture.declaration.waits[0].timeline_handle = second_timeline.value;
    second_declaration.wait_count = 1;
    second_declaration.waits[0].timeline_handle = fixture.timeline.value;
    EXPECT_EQ(
        rt::detail::compile_device_rate_plan(
            fixture.owner, 3, 2, fixture.rate_plan, fixture.bindings,
            fixture.backends, fixture.buffers, fixture.timelines,
            fixture.phases, plan, diagnostic),
        rt::Status::invalid_config);
    EXPECT_STREQ(diagnostic.message,
                 "device-rate wait graph contains a cycle");
}

TEST(RateDispatch, PolicyIsOptInCopiedFrozenAndLegacyDispatchIsExact) {
    rt::Runtime legacy;
    std::size_t calls = 0;
    rt::PhaseHandle phase;
    rt::RateDomainHandle domain;
    ASSERT_EQ(legacy.configure(test_config()), rt::Status::ok);
    ASSERT_EQ(
        legacy.register_callback({"legacy", &legacy_callback, &calls}, phase),
        rt::Status::ok);
    ASSERT_EQ(
        add_domain(
            legacy,
            "legacy-rate",
            100,
            100,
            10,
            rt::RateLateAction::fail,
            0,
            domain),
        rt::Status::ok);
    ASSERT_EQ(legacy.bind_phase_to_rate_domain(phase, domain), rt::Status::ok);
    ASSERT_EQ(legacy.finalize(), rt::Status::ok);
    EXPECT_FALSE(legacy.rate_execution_enabled());
    ASSERT_EQ(legacy.start(), rt::Status::ok);
    rt::StepResult result;
    ASSERT_EQ(
        legacy.step({1, 0ns, std::nullopt, std::nullopt}, &result),
        rt::Status::ok);
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(result.callbacks_executed, 1u);
    EXPECT_EQ(result.rate.due_reference_records, 0u);
    expect_stopped(legacy);

    rt::Runtime active;
    auto policy = rt::RateExecutionPolicy{4};
    ASSERT_EQ(active.set_rate_execution_policy(policy), rt::Status::ok);
    policy.maximum_dispatch_records_per_step = 0;
    rt::PhaseHandle active_phase;
    rt::RateDomainHandle active_domain;
    ReleaseProbe probe;
    ASSERT_EQ(
        active.register_callback(
            {"active", &release_callback, &probe},
            active_phase),
        rt::Status::ok);
    ASSERT_EQ(
        add_domain(
            active,
            "active-rate",
            100,
            100,
            10,
            rt::RateLateAction::fail,
            0,
            active_domain),
        rt::Status::ok);
    ASSERT_EQ(
        active.bind_phase_to_rate_domain(active_phase, active_domain),
        rt::Status::ok);
    ASSERT_EQ(active.finalize(), rt::Status::ok);
    rt::MemoryPlan active_plan;
    ASSERT_TRUE(active.memory_plan(active_plan));
    EXPECT_GT(active_plan.rate_dispatch_state_bytes, 0u);
    EXPECT_GT(active_plan.rate_checkpoint_state_bytes, 0u);
    EXPECT_EQ(
        active_plan.planned_bytes,
        active_plan.runtime_control_bytes +
            active_plan.executor_control_bytes +
            active_plan.device_control_bytes +
            active_plan.phase_scratch_total_bytes +
            active_plan.task_scratch_total_bytes +
            active_plan.trace_storage_bytes);
    EXPECT_TRUE(active.rate_execution_enabled());
    EXPECT_EQ(
        active.set_rate_execution_policy({1}),
        rt::Status::invalid_state);
}

TEST(RateDispatch, AdmissionRejectsMalformedAndInfeasibleActivePlans) {
    for (const auto malformed : std::array<rt::RateDomainRegistration, 4>{
             rt::RateDomainRegistration{
                 "zero-budget", 100, 1, 100, 0},
             rt::RateDomainRegistration{
                 "zero-deadline", 100, 1, 0, 10},
             rt::RateDomainRegistration{
                 "long-deadline", 100, 1, 101, 10},
             rt::RateDomainRegistration{
                 "bad-catch", 100, 1, 100, 10,
                 rt::RateCriticality::normal, false,
                 rt::RateLateAction::bounded_catch_up, 0},
         }) {
        SCOPED_TRACE(malformed.name);
        rt::Runtime runtime;
        std::size_t calls = 0;
        rt::PhaseHandle phase;
        rt::RateDomainHandle domain;
        ASSERT_EQ(runtime.set_rate_execution_policy({4}), rt::Status::ok);
        ASSERT_EQ(
            runtime.register_callback({"phase", &legacy_callback, &calls}, phase),
            rt::Status::ok);
        ASSERT_EQ(runtime.register_rate_domain(malformed, domain), rt::Status::ok);
        ASSERT_EQ(runtime.bind_phase_to_rate_domain(phase, domain), rt::Status::ok);
        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    }

    rt::Runtime infeasible;
    std::size_t calls = 0;
    rt::PhaseHandle first;
    rt::PhaseHandle second;
    rt::RateDomainHandle first_domain;
    rt::RateDomainHandle second_domain;
    ASSERT_EQ(infeasible.set_rate_execution_policy({4}), rt::Status::ok);
    ASSERT_EQ(
        infeasible.register_callback({"first", &legacy_callback, &calls}, first),
        rt::Status::ok);
    ASSERT_EQ(
        infeasible.register_callback({"second", &legacy_callback, &calls}, second),
        rt::Status::ok);
    ASSERT_EQ(
        add_domain(
            infeasible, "first-rate", 100, 60, 60,
            rt::RateLateAction::fail, 0, first_domain),
        rt::Status::ok);
    ASSERT_EQ(
        add_domain(
            infeasible, "second-rate", 100, 70, 20,
            rt::RateLateAction::fail, 0, second_domain),
        rt::Status::ok);
    ASSERT_EQ(infeasible.bind_phase_to_rate_domain(first, first_domain), rt::Status::ok);
    ASSERT_EQ(infeasible.bind_phase_to_rate_domain(second, second_domain), rt::Status::ok);
    EXPECT_EQ(infeasible.finalize(), rt::Status::invalid_config);
}

TEST(RateDispatch, AdmissionMatchesIndependentCompleteSupercycleSimulation) {
    constexpr std::uint32_t owner = 71;
    const std::array phases{
        rt::PhaseHandle{owner, 0},
        rt::PhaseHandle{owner, 1},
    };
    const std::array domains{
        rt::detail::RateDomainSpec{
            "seven", 7, 2, 5, 1,
            rt::RateCriticality::normal, false,
            rt::RateLateAction::fail, 0},
        rt::detail::RateDomainSpec{
            "ten", 10, 1, 8, 1,
            rt::RateCriticality::critical, false,
            rt::RateLateAction::degrade, 0},
    };
    auto bindings = std::array{
        rt::detail::RateBindingSpec{
            phases[0], rt::RateDomainHandle{owner, 0},
            rt::RatePhaseKind::cpu},
        rt::detail::RateBindingSpec{
            phases[1], rt::RateDomainHandle{owner, 1},
            rt::RatePhaseKind::cpu},
    };
    rt::detail::CompiledRatePlan rate_plan;
    rt::detail::RateCompileDiagnostic rate_diagnostic;
    ASSERT_EQ(
        rt::detail::compile_rate_timeline(
            owner,
            phases.size(),
            phases,
            domains,
            bindings,
            rate_plan,
            rate_diagnostic),
        rt::Status::ok);
    ASSERT_EQ(rate_plan.supercycle_ns, 70u);

    rt::detail::CompiledRateDispatchPlan dispatch_plan;
    rt::detail::RateDispatchDiagnostic dispatch_diagnostic;
    ASSERT_EQ(
        rt::detail::compile_rate_dispatch(
            owner,
            rt::DeterminismTier::unspecified,
            {rate_plan.releases.size()},
            {},
            rate_plan,
            {},
            nullptr,
            dispatch_plan,
            dispatch_diagnostic),
        rt::Status::ok);
    ASSERT_EQ(dispatch_plan.admission.size(), rate_plan.releases.size());
    std::uint64_t independent_finish = 0;
    for (std::size_t index = 0; index < rate_plan.releases.size(); ++index) {
        const auto& release = rate_plan.releases[index];
        const auto independent_start =
            std::max(independent_finish, release.release_time_ns);
        independent_finish = independent_start + release.budget_wcet_ns;
        ASSERT_LE(independent_finish, release.deadline_time_ns);
        EXPECT_EQ(
            dispatch_plan.admission[index].reference_index,
            index);
        EXPECT_EQ(
            dispatch_plan.admission[index].declared_start_ns,
            independent_start);
        EXPECT_EQ(
            dispatch_plan.admission[index].declared_finish_ns,
            independent_finish);
        EXPECT_EQ(
            dispatch_plan.admission[index].deadline_ns,
            release.deadline_time_ns);
    }
    EXPECT_LE(independent_finish, rate_plan.supercycle_ns);

    bindings[1].phase_kind = rt::RatePhaseKind::device;
    ASSERT_EQ(
        rt::detail::compile_rate_timeline(
            owner,
            phases.size(),
            phases,
            domains,
            bindings,
            rate_plan,
            rate_diagnostic),
        rt::Status::ok);
    EXPECT_EQ(
        rt::detail::compile_rate_dispatch(
            owner,
            rt::DeterminismTier::unspecified,
            {rate_plan.releases.size()},
            {},
            rate_plan,
            {},
            nullptr,
            dispatch_plan,
            dispatch_diagnostic),
        rt::Status::invalid_config);
}

TEST(RateDispatch, RejectsD1CrossDomainDependenciesAndSkipProducers) {
    {
        rt::Runtime runtime;
        auto config = test_config();
        config.determinism_tier = rt::DeterminismTier::schedule_independent;
        std::size_t calls = 0;
        rt::PhaseHandle phase;
        rt::RateDomainHandle domain;
        ASSERT_EQ(runtime.configure(config), rt::Status::ok);
        ASSERT_EQ(runtime.set_rate_execution_policy({4}), rt::Status::ok);
        ASSERT_EQ(runtime.register_callback(
                      {"phase", &legacy_callback, &calls}, phase),
                  rt::Status::ok);
        ASSERT_EQ(add_domain(
                      runtime, "rate", 100, 100, 10,
                      rt::RateLateAction::fail, 0, domain),
                  rt::Status::ok);
        ASSERT_EQ(runtime.bind_phase_to_rate_domain(phase, domain), rt::Status::ok);
        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    }

    {
        rt::Runtime runtime;
        std::size_t calls = 0;
        rt::PhaseHandle first;
        rt::PhaseHandle second;
        rt::RateDomainHandle first_domain;
        rt::RateDomainHandle second_domain;
        ASSERT_EQ(runtime.set_rate_execution_policy({4}), rt::Status::ok);
        ASSERT_EQ(runtime.register_callback(
                      {"first", &legacy_callback, &calls}, first),
                  rt::Status::ok);
        ASSERT_EQ(runtime.register_callback(
                      {"second", &legacy_callback, &calls}, second),
                  rt::Status::ok);
        ASSERT_EQ(add_domain(
                      runtime, "first-rate", 100, 100, 10,
                      rt::RateLateAction::fail, 0, first_domain),
                  rt::Status::ok);
        ASSERT_EQ(add_domain(
                      runtime, "second-rate", 100, 100, 10,
                      rt::RateLateAction::fail, 0, second_domain),
                  rt::Status::ok);
        ASSERT_EQ(runtime.bind_phase_to_rate_domain(first, first_domain), rt::Status::ok);
        ASSERT_EQ(runtime.bind_phase_to_rate_domain(second, second_domain), rt::Status::ok);
        ASSERT_EQ(runtime.add_dependency(first, second), rt::Status::ok);
        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    }

    {
        rt::Runtime runtime;
        ReleaseProbe producer;
        ReleaseProbe consumer;
        rt::PhaseHandle producer_phase;
        rt::PhaseHandle consumer_phase;
        rt::RateDomainHandle producer_domain;
        rt::RateDomainHandle consumer_domain;
        rt::CrossRateChannelHandle channel;
        ASSERT_EQ(runtime.set_rate_execution_policy({4}), rt::Status::ok);
        ASSERT_EQ(runtime.register_callback(
                      {"producer", &release_callback, &producer},
                      producer_phase),
                  rt::Status::ok);
        ASSERT_EQ(runtime.register_callback(
                      {"consumer", &release_callback, &consumer},
                      consumer_phase),
                  rt::Status::ok);
        ASSERT_EQ(add_domain(
                      runtime, "producer-rate", 100, 100, 10,
                      rt::RateLateAction::skip, 0, producer_domain),
                  rt::Status::ok);
        ASSERT_EQ(add_domain(
                      runtime, "consumer-rate", 100, 100, 10,
                      rt::RateLateAction::fail, 0, consumer_domain),
                  rt::Status::ok);
        ASSERT_EQ(runtime.bind_phase_to_rate_domain(
                      producer_phase, producer_domain),
                  rt::Status::ok);
        ASSERT_EQ(runtime.bind_phase_to_rate_domain(
                      consumer_phase, consumer_domain),
                  rt::Status::ok);
        const std::array initial{std::byte{0}};
        ASSERT_EQ(runtime.register_cross_rate_channel(
                      {"channel", producer_phase, consumer_phase,
                       initial.size(), initial},
                      channel),
                  rt::Status::ok);
        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    }

    {
        rt::Runtime runtime;
        std::size_t calls = 0;
        rt::PhaseHandle phase;
        rt::RateDomainHandle domain;
        ASSERT_EQ(runtime.set_rate_execution_policy({4}), rt::Status::ok);
        ASSERT_EQ(runtime.register_callback(
                      {"phase", &legacy_callback, &calls}, phase),
                  rt::Status::ok);
        ASSERT_EQ(add_domain(
                      runtime, "rate", 100, 100, 10,
                      static_cast<rt::RateLateAction>(255), 0, domain),
                  rt::Status::ok);
        ASSERT_EQ(runtime.bind_phase_to_rate_domain(phase, domain), rt::Status::ok);
        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    }
}

TEST(RateDispatch, ExecutesExactHalfOpenWindowsAndReportsContextAndCaps) {
    ManualClock clock;
    rt::Runtime runtime(clock);
    ReleaseProbe probe;
    rt::PhaseHandle phase;
    rt::RateDomainHandle domain;
    ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
    ASSERT_EQ(runtime.set_rate_execution_policy({2}), rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback({"phase", &release_callback, &probe}, phase),
        rt::Status::ok);
    ASSERT_EQ(
        add_domain(
            runtime, "rate", 100, 100, 10,
            rt::RateLateAction::fail, 0, domain),
        rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(phase, domain), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    rt::StepResult result;
    EXPECT_EQ(runtime.step({0, 0ns, std::nullopt, 1'000}, &result),
              rt::Status::invalid_argument);
    EXPECT_EQ(runtime.step({0, 100ns, std::nullopt, std::nullopt}, &result),
              rt::Status::invalid_argument);
    ASSERT_EQ(runtime.step({0, 100ns, std::nullopt, 1'000}, &result),
              rt::Status::ok);
    ASSERT_EQ(probe.count, 1u);
    EXPECT_EQ(result.rate.due_domain_releases, 1u);
    EXPECT_EQ(result.rate.executed_reference_records, 1u);
    EXPECT_EQ(probe.releases[0].domain, domain);
    EXPECT_EQ(probe.releases[0].phase, phase);
    EXPECT_EQ(probe.releases[0].domain_release_sequence, 0u);
    EXPECT_EQ(probe.releases[0].logical_release_ns, 0u);
    EXPECT_EQ(probe.releases[0].nominal_release_ns, 1'000u);
    EXPECT_EQ(probe.releases[0].absolute_deadline_ns, 1'100u);
    EXPECT_EQ(probe.releases[0].declared_budget_ns, 10u);

    EXPECT_EQ(runtime.step({1, 100ns, std::nullopt, 1'099}, &result),
              rt::Status::invalid_argument);
    ASSERT_EQ(runtime.step({1, 200ns, std::nullopt, 1'100}, &result),
              rt::Status::ok);
    EXPECT_EQ(result.rate.due_reference_records, 2u);
    EXPECT_EQ(result.rate.executed_reference_records, 2u);
    EXPECT_EQ(probe.releases[1].domain_release_sequence, 1u);
    EXPECT_EQ(probe.releases[2].domain_release_sequence, 2u);

    EXPECT_EQ(runtime.step({2, 300ns, std::nullopt, 1'300}, &result),
              rt::Status::capacity_exceeded);
    EXPECT_EQ(result.rate.due_reference_records, 3u);
    EXPECT_EQ(result.rate.executed_reference_records, 2u);
    EXPECT_EQ(result.rate.rejected_reference_records, 1u);
    expect_stopped(runtime);
}

TEST(RateDispatch, CallbackFailureRejectsEveryUnattemptedSubstep) {
    ManualClock clock;
    rt::Runtime runtime(clock);
    ReleaseProbe probe;
    probe.fail = true;
    rt::PhaseHandle phase;
    rt::RateDomainHandle domain;
    ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
    ASSERT_EQ(runtime.set_rate_execution_policy({4}), rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback(
            {"failing-substeps", &release_callback, &probe},
            phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_rate_domain(
            {"rate", 100, 3, 100, 10,
             rt::RateCriticality::normal, false,
             rt::RateLateAction::fail, 0},
            domain),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.bind_phase_to_rate_domain(phase, domain),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    rt::StepResult result;
    EXPECT_EQ(
        runtime.step({0, 100ns, std::nullopt, 1'000}, &result),
        rt::Status::callback_failed);
    EXPECT_EQ(probe.count, 1u);
    EXPECT_EQ(result.rate.due_domain_releases, 1u);
    EXPECT_EQ(result.rate.due_reference_records, 3u);
    EXPECT_EQ(result.rate.executed_reference_records, 1u);
    EXPECT_EQ(result.rate.rejected_reference_records, 2u);
    EXPECT_EQ(result.rate.failed_domain_releases, 1u);
    ASSERT_TRUE(result.rate.has_first_failure);
    EXPECT_EQ(result.rate.first_failing_domain, domain);
    EXPECT_EQ(result.rate.first_failing_sequence, 0u);
    EXPECT_EQ(result.rate.first_failing_substep, 0u);
    expect_stopped(runtime);
}

TEST(RateDispatch, AppliesSkipCatchUpDegradeAndFailPolicies) {
    const auto run_late = [](
                              rt::RateLateAction action,
                              std::uint32_t catch_up_limit,
                              std::uint64_t delta,
                              rt::Status expected_status) {
        ManualClock clock;
        clock.now = 2'000 + delta;
        rt::Runtime runtime(clock);
        ReleaseProbe probe;
        rt::PhaseHandle phase;
        rt::RateDomainHandle domain;
        EXPECT_EQ(runtime.configure(test_config()), rt::Status::ok);
        EXPECT_EQ(runtime.set_rate_execution_policy({8}), rt::Status::ok);
        EXPECT_EQ(
            runtime.register_callback({"phase", &release_callback, &probe}, phase),
            rt::Status::ok);
        EXPECT_EQ(
            add_domain(
                runtime, "rate", 100, 100, 10,
                action, catch_up_limit, domain),
            rt::Status::ok);
        EXPECT_EQ(runtime.bind_phase_to_rate_domain(phase, domain), rt::Status::ok);
        EXPECT_EQ(runtime.finalize(), rt::Status::ok);
        EXPECT_EQ(runtime.start(), rt::Status::ok);
        rt::StepResult result;
        EXPECT_EQ(
            runtime.step(
                {0,
                 std::chrono::nanoseconds(delta),
                 std::nullopt,
                 1'000},
                &result),
            expected_status);
        EXPECT_EQ(runtime.stop(), rt::Status::ok);
        return std::pair{probe.count, result};
    };

    const auto skipped = run_late(
        rt::RateLateAction::skip, 0, 1'000'000, rt::Status::ok);
    EXPECT_EQ(skipped.first, 0u);
    EXPECT_EQ(skipped.second.rate.skipped_domain_releases, 10'000u);
    EXPECT_EQ(skipped.second.rate.late_domain_releases, 10'000u);

    const auto caught = run_late(
        rt::RateLateAction::bounded_catch_up,
        1,
        300,
        rt::Status::capacity_exceeded);
    EXPECT_EQ(caught.first, 1u);
    EXPECT_EQ(caught.second.rate.caught_up_domain_releases, 1u);
    EXPECT_EQ(caught.second.rate.rejected_reference_records, 2u);

    const auto degraded = run_late(
        rt::RateLateAction::degrade, 0, 100, rt::Status::ok);
    EXPECT_EQ(degraded.first, 1u);
    EXPECT_EQ(degraded.second.rate.degraded_domain_releases, 1u);
    EXPECT_EQ(degraded.second.degradation_level, 1u);
    EXPECT_EQ(degraded.second.rate.executed_reference_records, 1u);

    ManualClock fail_clock;
    fail_clock.now = 10'000;
    rt::Runtime failed(fail_clock);
    ReleaseProbe fail_probe;
    rt::PhaseHandle phase;
    rt::RateDomainHandle domain;
    ASSERT_EQ(failed.set_rate_execution_policy({4}), rt::Status::ok);
    ASSERT_EQ(
        failed.register_callback({"phase", &release_callback, &fail_probe}, phase),
        rt::Status::ok);
    ASSERT_EQ(
        add_domain(
            failed, "rate", 100, 100, 10,
            rt::RateLateAction::fail, 0, domain),
        rt::Status::ok);
    ASSERT_EQ(failed.bind_phase_to_rate_domain(phase, domain), rt::Status::ok);
    ASSERT_EQ(failed.finalize(), rt::Status::ok);
    std::size_t pre_failure_checkpoint_bytes = 0;
    ASSERT_EQ(
        failed.checkpoint_size(pre_failure_checkpoint_bytes),
        rt::Status::ok);
    std::vector<std::byte> pre_failure_checkpoint(
        pre_failure_checkpoint_bytes);
    rt::ArtifactWriteResult pre_failure_write;
    ASSERT_EQ(
        failed.write_checkpoint(
            0,
            pre_failure_checkpoint,
            pre_failure_write),
        rt::Status::ok);
    ASSERT_EQ(failed.start(), rt::Status::ok);
    rt::StepResult fail_result;
    EXPECT_EQ(failed.step({0, 100ns, std::nullopt, 1'000}, &fail_result),
              rt::Status::callback_failed);
    EXPECT_EQ(fail_probe.count, 0u);
    EXPECT_EQ(fail_result.rate.failed_domain_releases, 1u);
    EXPECT_TRUE(fail_result.rate.has_first_failure);
    EXPECT_EQ(fail_result.rate.first_failing_domain, domain);
    EXPECT_EQ(
        failed.restore_checkpoint(pre_failure_checkpoint),
        rt::Status::invalid_state);
    EXPECT_EQ(failed.step({1, 100ns, std::nullopt, 1'000}),
              rt::Status::invalid_state);
    expect_stopped(failed);
}

TEST(RateDispatch, PublishesCopiesHoldsAndRejectsDuplicateOrMissingPayloads) {
    const auto configure_channel_runtime = [](
                                               ManualClock& clock,
                                               rt::Runtime& runtime,
                                               ReleaseProbe& producer,
                                               ReleaseProbe& consumer,
                                               rt::RateLateAction producer_action,
                                               rt::CrossRateChannelHandle& channel) {
        rt::PhaseHandle producer_phase;
        rt::PhaseHandle consumer_phase;
        rt::RateDomainHandle producer_domain;
        rt::RateDomainHandle consumer_domain;
        EXPECT_EQ(runtime.configure(test_config()), rt::Status::ok);
        EXPECT_EQ(runtime.set_rate_execution_policy({4}), rt::Status::ok);
        EXPECT_EQ(runtime.register_callback(
                      {"producer", &release_callback, &producer},
                      producer_phase),
                  rt::Status::ok);
        EXPECT_EQ(runtime.register_callback(
                      {"consumer", &release_callback, &consumer},
                      consumer_phase),
                  rt::Status::ok);
        EXPECT_EQ(add_domain(
                      runtime, "producer-rate", 100, 10, 4,
                      producer_action, 0, producer_domain),
                  rt::Status::ok);
        EXPECT_EQ(add_domain(
                      runtime, "consumer-rate", 100, 100, 4,
                      rt::RateLateAction::fail, 0, consumer_domain),
                  rt::Status::ok);
        EXPECT_EQ(runtime.bind_phase_to_rate_domain(
                      producer_phase, producer_domain),
                  rt::Status::ok);
        EXPECT_EQ(runtime.bind_phase_to_rate_domain(
                      consumer_phase, consumer_domain),
                  rt::Status::ok);
        const std::array initial{
            std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        EXPECT_EQ(runtime.register_cross_rate_channel(
                      {"samples", producer_phase, consumer_phase,
                       initial.size(), initial,
                       rt::CrossRateMode::sample_and_hold, 0},
                      channel),
                  rt::Status::ok);
        producer.publish_channel = channel;
        consumer.copy_channel = channel;
        (void)clock;
    };

    ManualClock clock;
    rt::Runtime runtime(clock);
    ReleaseProbe producer;
    ReleaseProbe consumer;
    rt::CrossRateChannelHandle channel;
    configure_channel_runtime(
        clock,
        runtime,
        producer,
        consumer,
        rt::RateLateAction::hold,
        channel);
    producer.publish = true;
    producer.publish_payload = {
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    consumer.copy = true;
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    rt::StepResult result;
    ASSERT_EQ(runtime.step({0, 100ns, std::nullopt, 1'000}, &result),
              rt::Status::ok);
    EXPECT_EQ(producer.publish_status, rt::Status::ok);
    EXPECT_EQ(consumer.read.status, rt::CrossRateReadStatus::ok);
    EXPECT_EQ(consumer.read.provenance,
              rt::CrossRateSampleProvenance::produced);
    EXPECT_FALSE(consumer.read.held);
    EXPECT_EQ(consumer.copied_payload, producer.publish_payload);
    expect_stopped(runtime);

    ManualClock hold_clock;
    hold_clock.now = 1'050;
    rt::Runtime held(hold_clock);
    ReleaseProbe held_producer;
    ReleaseProbe held_consumer;
    rt::CrossRateChannelHandle held_channel;
    configure_channel_runtime(
        hold_clock,
        held,
        held_producer,
        held_consumer,
        rt::RateLateAction::hold,
        held_channel);
    held_consumer.copy = true;
    ASSERT_EQ(held.finalize(), rt::Status::ok);
    ASSERT_EQ(held.start(), rt::Status::ok);
    ASSERT_EQ(held.step({0, 100ns, std::nullopt, 1'000}, &result),
              rt::Status::ok);
    EXPECT_EQ(held_producer.count, 0u);
    EXPECT_EQ(held_consumer.count, 1u);
    EXPECT_TRUE(held_consumer.read.held);
    EXPECT_EQ(held_consumer.read.provenance,
              rt::CrossRateSampleProvenance::initial_sample);
    EXPECT_EQ(result.rate.held_domain_releases, 1u);
    expect_stopped(held);

    ManualClock duplicate_clock;
    rt::Runtime duplicate_runtime(duplicate_clock);
    ReleaseProbe duplicate_producer;
    ReleaseProbe duplicate_consumer;
    rt::CrossRateChannelHandle duplicate_channel;
    configure_channel_runtime(
        duplicate_clock,
        duplicate_runtime,
        duplicate_producer,
        duplicate_consumer,
        rt::RateLateAction::hold,
        duplicate_channel);
    duplicate_producer.publish = true;
    duplicate_producer.duplicate = true;
    ASSERT_EQ(duplicate_runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(duplicate_runtime.start(), rt::Status::ok);
    EXPECT_EQ(
        duplicate_runtime.step({0, 100ns, std::nullopt, 1'000}, &result),
        rt::Status::callback_failed);
    EXPECT_EQ(duplicate_producer.duplicate_status, rt::Status::invalid_state);
    EXPECT_EQ(duplicate_consumer.count, 0u);
    expect_stopped(duplicate_runtime);

    ManualClock wrong_publish_clock;
    rt::Runtime wrong_publish_runtime(wrong_publish_clock);
    ReleaseProbe wrong_publish_producer;
    ReleaseProbe wrong_publish_consumer;
    rt::CrossRateChannelHandle wrong_publish_channel;
    configure_channel_runtime(
        wrong_publish_clock,
        wrong_publish_runtime,
        wrong_publish_producer,
        wrong_publish_consumer,
        rt::RateLateAction::hold,
        wrong_publish_channel);
    wrong_publish_producer.publish = true;
    wrong_publish_producer.publish_payload_size = 3;
    ASSERT_EQ(wrong_publish_runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(wrong_publish_runtime.start(), rt::Status::ok);
    EXPECT_EQ(
        wrong_publish_runtime.step(
            {0, 100ns, std::nullopt, 1'000},
            &result),
        rt::Status::callback_failed);
    EXPECT_EQ(
        wrong_publish_producer.publish_status,
        rt::Status::invalid_argument);
    EXPECT_EQ(wrong_publish_consumer.count, 0u);
    expect_stopped(wrong_publish_runtime);

    ManualClock wrong_copy_clock;
    rt::Runtime wrong_copy_runtime(wrong_copy_clock);
    ReleaseProbe wrong_copy_producer;
    ReleaseProbe wrong_copy_consumer;
    rt::CrossRateChannelHandle wrong_copy_channel;
    configure_channel_runtime(
        wrong_copy_clock,
        wrong_copy_runtime,
        wrong_copy_producer,
        wrong_copy_consumer,
        rt::RateLateAction::hold,
        wrong_copy_channel);
    wrong_copy_producer.publish = true;
    wrong_copy_consumer.copy = true;
    wrong_copy_consumer.copy_payload_size = 3;
    ASSERT_EQ(wrong_copy_runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(wrong_copy_runtime.start(), rt::Status::ok);
    EXPECT_EQ(
        wrong_copy_runtime.step(
            {0, 100ns, std::nullopt, 1'000},
            &result),
        rt::Status::ok);
    EXPECT_EQ(
        wrong_copy_consumer.read.status,
        rt::CrossRateReadStatus::size_mismatch);
    expect_stopped(wrong_copy_runtime);

    ManualClock wrong_owner_clock;
    rt::Runtime wrong_owner_runtime(wrong_owner_clock);
    ReleaseProbe wrong_owner_producer;
    ReleaseProbe wrong_owner_consumer;
    rt::CrossRateChannelHandle wrong_owner_channel;
    configure_channel_runtime(
        wrong_owner_clock,
        wrong_owner_runtime,
        wrong_owner_producer,
        wrong_owner_consumer,
        rt::RateLateAction::hold,
        wrong_owner_channel);
    wrong_owner_producer.publish = true;
    wrong_owner_consumer.copy = true;
    wrong_owner_consumer.copy_channel = channel;
    ASSERT_EQ(wrong_owner_runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(wrong_owner_runtime.start(), rt::Status::ok);
    EXPECT_EQ(
        wrong_owner_runtime.step(
            {0, 100ns, std::nullopt, 1'000},
            &result),
        rt::Status::ok);
    EXPECT_EQ(
        wrong_owner_consumer.read.status,
        rt::CrossRateReadStatus::wrong_owner);
    expect_stopped(wrong_owner_runtime);

    ManualClock missing_clock;
    rt::Runtime missing_runtime(missing_clock);
    ReleaseProbe missing_producer;
    ReleaseProbe missing_consumer;
    rt::CrossRateChannelHandle missing_channel;
    configure_channel_runtime(
        missing_clock,
        missing_runtime,
        missing_producer,
        missing_consumer,
        rt::RateLateAction::hold,
        missing_channel);
    ASSERT_EQ(missing_runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(missing_runtime.start(), rt::Status::ok);
    EXPECT_EQ(
        missing_runtime.step({0, 100ns, std::nullopt, 1'000}, &result),
        rt::Status::callback_failed);
    EXPECT_EQ(missing_producer.count, 1u);
    EXPECT_EQ(missing_consumer.count, 0u);
    missing_producer.publish = true;
    missing_producer.publish_payload = {
        std::byte{9}, std::byte{8}, std::byte{7}, std::byte{6}};
    missing_consumer.copy = true;
    EXPECT_EQ(
        missing_runtime.step({1, 100ns, std::nullopt, 1'100}, &result),
        rt::Status::ok);
    EXPECT_EQ(missing_producer.count, 2u);
    EXPECT_EQ(missing_consumer.count, 1u);
    EXPECT_EQ(
        missing_consumer.copied_payload,
        missing_producer.publish_payload);
    expect_stopped(missing_runtime);
}

TEST(RateDispatch, PublishesEverySubstepAndUsesExactHeldSelection) {
    ManualClock clock;
    rt::Runtime runtime(clock);
    SubstepChannelProbe producer;
    SubstepChannelProbe consumer;
    producer.producer = true;
    rt::PhaseHandle producer_phase;
    rt::PhaseHandle consumer_phase;
    rt::RateDomainHandle producer_domain;
    rt::RateDomainHandle consumer_domain;
    ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
    ASSERT_EQ(runtime.set_rate_execution_policy({8}), rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback(
            {"producer", &substep_channel_callback, &producer},
            producer_phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback(
            {"consumer", &substep_channel_callback, &consumer},
            consumer_phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_rate_domain(
            {"producer-rate", 200, 2, 40, 4,
             rt::RateCriticality::normal, false,
             rt::RateLateAction::fail, 0},
            producer_domain),
        rt::Status::ok);
    ASSERT_EQ(
        add_domain(
            runtime,
            "consumer-rate",
            100,
            100,
            4,
            rt::RateLateAction::fail,
            0,
            consumer_domain),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.bind_phase_to_rate_domain(producer_phase, producer_domain),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.bind_phase_to_rate_domain(consumer_phase, consumer_domain),
        rt::Status::ok);
    const std::array initial{std::byte{0}};
    rt::CrossRateChannelHandle channel;
    ASSERT_EQ(
        runtime.register_cross_rate_channel(
            {"substeps", producer_phase, consumer_phase,
             initial.size(), initial,
             rt::CrossRateMode::sample_and_hold, 0},
            channel),
        rt::Status::ok);
    producer.channel = channel;
    consumer.channel = channel;
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    rt::StepResult result;
    ASSERT_EQ(
        runtime.step({0, 200ns, std::nullopt, 1'000}, &result),
        rt::Status::ok);
    EXPECT_EQ(producer.count, 2u);
    ASSERT_EQ(consumer.count, 2u);
    EXPECT_EQ(consumer.values[0], std::byte{2});
    EXPECT_EQ(consumer.values[1], std::byte{2});
    EXPECT_FALSE(consumer.reads[0].held);
    EXPECT_TRUE(consumer.reads[1].held);
    EXPECT_EQ(
        consumer.reads[0].freshness,
        rt::CrossRateFreshness::fresh);
    EXPECT_EQ(
        consumer.reads[1].freshness,
        rt::CrossRateFreshness::stale);
    EXPECT_EQ(consumer.reads[0].generation, consumer.reads[1].generation);
    EXPECT_EQ(result.rate.executed_reference_records, 4u);
    expect_stopped(runtime);
}

TEST(RateDispatch, PeriodicUsesAbsoluteReleaseAndAggregatesRateSummaries) {
    ManualClock clock;
    rt::Runtime runtime(clock);
    ReleaseProbe probe;
    rt::PhaseHandle phase;
    rt::RateDomainHandle domain;
    ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
    ASSERT_EQ(runtime.set_rate_execution_policy({2}), rt::Status::ok);
    ASSERT_EQ(runtime.register_callback(
                  {"phase", &release_callback, &probe}, phase),
              rt::Status::ok);
    ASSERT_EQ(add_domain(
                  runtime, "rate", 100, 100, 10,
                  rt::RateLateAction::fail, 0, domain),
              rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(phase, domain), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    rt::PeriodicRunResult result;
    ASSERT_EQ(runtime.run_periodic(
                  {0, 3, 100ns, 1'000, 100ns},
                  nullptr,
                  nullptr,
                  &result),
              rt::Status::ok);
    EXPECT_EQ(clock.sleeps, 3u);
    EXPECT_EQ(probe.count, 3u);
    EXPECT_EQ(probe.releases[0].nominal_release_ns, 1'000u);
    EXPECT_EQ(probe.releases[1].nominal_release_ns, 1'100u);
    EXPECT_EQ(probe.releases[2].nominal_release_ns, 1'200u);
    EXPECT_EQ(result.rate.due_domain_releases, 3u);
    EXPECT_EQ(result.rate.executed_reference_records, 3u);
    EXPECT_EQ(result.last_frame.rate.executed_reference_records, 1u);
    expect_stopped(runtime);
}

TEST(RateDispatch, TwoActiveRuntimeEpochsAndCursorsRemainIsolated) {
    const auto configure_runtime = [](
                                       rt::Runtime& runtime,
                                       ReleaseProbe& probe) {
        rt::PhaseHandle phase;
        rt::RateDomainHandle domain;
        ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
        ASSERT_EQ(runtime.set_rate_execution_policy({4}), rt::Status::ok);
        ASSERT_EQ(
            runtime.register_callback(
                {"phase", &release_callback, &probe},
                phase),
            rt::Status::ok);
        ASSERT_EQ(
            add_domain(
                runtime,
                "rate",
                100,
                100,
                10,
                rt::RateLateAction::fail,
                0,
                domain),
            rt::Status::ok);
        ASSERT_EQ(
            runtime.bind_phase_to_rate_domain(phase, domain),
            rt::Status::ok);
        ASSERT_EQ(runtime.finalize(), rt::Status::ok);
        ASSERT_EQ(runtime.start(), rt::Status::ok);
    };

    ManualClock first_clock;
    ManualClock second_clock;
    rt::Runtime first(first_clock);
    rt::Runtime second(second_clock);
    ReleaseProbe first_probe;
    ReleaseProbe second_probe;
    configure_runtime(first, first_probe);
    configure_runtime(second, second_probe);

    ASSERT_EQ(
        first.step({0, 100ns, std::nullopt, 1'000}),
        rt::Status::ok);
    EXPECT_EQ(first_probe.count, 1u);
    EXPECT_EQ(second_probe.count, 0u);
    ASSERT_EQ(
        second.step({0, 200ns, std::nullopt, 5'000}),
        rt::Status::ok);
    EXPECT_EQ(second_probe.count, 2u);
    EXPECT_EQ(second_probe.releases[0].nominal_release_ns, 5'000u);
    EXPECT_EQ(second_probe.releases[1].nominal_release_ns, 5'100u);
    ASSERT_EQ(
        first.step({1, 100ns, std::nullopt, 1'100}),
        rt::Status::ok);
    EXPECT_EQ(first_probe.count, 2u);
    EXPECT_EQ(first_probe.releases[1].nominal_release_ns, 1'100u);

    expect_stopped(first);
    expect_stopped(second);
}

TEST(RateDispatch, ActiveCheckpointRoundTripsAndReplayIsExplicitlyRejected) {
    ManualClock clock;
    rt::Runtime source(clock);
    ReleaseProbe source_probe;
    rt::PhaseHandle phase;
    rt::RateDomainHandle domain;
    ASSERT_EQ(source.configure(test_config()), rt::Status::ok);
    ASSERT_EQ(source.set_rate_execution_policy({4}), rt::Status::ok);
    ASSERT_EQ(source.register_callback(
                  {"phase", &release_callback, &source_probe}, phase),
              rt::Status::ok);
    ASSERT_EQ(add_domain(
                  source, "rate", 100, 100, 10,
                  rt::RateLateAction::fail, 0, domain),
              rt::Status::ok);
    ASSERT_EQ(source.bind_phase_to_rate_domain(phase, domain), rt::Status::ok);
    ASSERT_EQ(source.finalize(), rt::Status::ok);
    ASSERT_EQ(source.start(), rt::Status::ok);
    ASSERT_EQ(source.step({0, 100ns, std::nullopt, 1'000}), rt::Status::ok);
    ASSERT_EQ(source.stop(), rt::Status::ok);

    std::size_t checkpoint_bytes = 0;
    ASSERT_EQ(source.checkpoint_size(checkpoint_bytes), rt::Status::ok);
    std::vector<std::byte> checkpoint(checkpoint_bytes);
    rt::ArtifactWriteResult write_result;
    ASSERT_EQ(source.write_checkpoint(0, checkpoint, write_result), rt::Status::ok);
    rt::CheckpointMetadata metadata;
    ASSERT_EQ(rt::inspect_checkpoint_artifact(checkpoint, metadata), rt::Status::ok);
    EXPECT_EQ(metadata.state_count, 1u);

    ManualClock restored_clock;
    rt::Runtime restored(restored_clock);
    ReleaseProbe restored_probe;
    rt::PhaseHandle restored_phase;
    rt::RateDomainHandle restored_domain;
    ASSERT_EQ(restored.configure(test_config()), rt::Status::ok);
    ASSERT_EQ(restored.set_rate_execution_policy({4}), rt::Status::ok);
    ASSERT_EQ(restored.register_callback(
                  {"phase", &release_callback, &restored_probe}, restored_phase),
              rt::Status::ok);
    ASSERT_EQ(add_domain(
                  restored, "rate", 100, 100, 10,
                  rt::RateLateAction::fail, 0, restored_domain),
              rt::Status::ok);
    ASSERT_EQ(restored.bind_phase_to_rate_domain(
                  restored_phase, restored_domain),
              rt::Status::ok);
    ASSERT_EQ(restored.finalize(), rt::Status::ok);

    rt::detail::CheckpointRecordCursor cursor;
    rt::detail::CheckpointRecordView active_record;
    ASSERT_TRUE(rt::detail::next_checkpoint_record(
        checkpoint,
        metadata,
        cursor,
        active_record));
    ASSERT_EQ(active_record.name, "rtfw.rate-dispatch");
    std::vector<std::byte> malformed_payload(
        active_record.payload.begin(),
        active_record.payload.end());
    store_u64_le(
        malformed_payload,
        24,
        std::numeric_limits<std::uint64_t>::max());
    EncodedState malformed_state{
        active_record.name,
        active_record.schema_version,
        malformed_payload};
    std::vector<std::byte> semantic_corruption(checkpoint.size());
    rt::ArtifactWriteResult malformed_write;
    ASSERT_EQ(
        rt::detail::encode_checkpoint_artifact(
            metadata,
            1,
            &provide_encoded_state,
            &malformed_state,
            test_config().snapshot_max_bytes,
            semantic_corruption,
            malformed_write),
        rt::Status::ok);
    semantic_corruption.resize(malformed_write.bytes_written);
    EXPECT_EQ(
        rt::inspect_checkpoint_artifact(semantic_corruption, metadata),
        rt::Status::ok);
    EXPECT_EQ(
        restored.restore_checkpoint(semantic_corruption),
        rt::Status::incompatible_artifact);
    ASSERT_EQ(restored.restore_checkpoint(checkpoint), rt::Status::ok);
    ASSERT_EQ(restored.start(), rt::Status::ok);
    auto corrupted = checkpoint;
    corrupted.back() ^= std::byte{1};
    EXPECT_EQ(
        restored.restore_checkpoint(corrupted),
        rt::Status::invalid_artifact);
    ASSERT_EQ(restored.step({1, 100ns, std::nullopt, 1'100}), rt::Status::ok);
    ASSERT_EQ(restored.stop(), rt::Status::ok);

    std::array<std::byte, 256> input_log{};
    rt::ArtifactWriteResult input_result;
    EXPECT_EQ(
        restored.write_input_log({}, input_log, input_result),
        rt::Status::invalid_state);
}
