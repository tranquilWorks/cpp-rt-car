#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include <rt/runtime.hpp>

#include "rt/src/rate_telemetry.hpp"
#include "rt/src/snapshot_codec.hpp"

namespace {

using namespace std::chrono_literals;

struct ManualClock final : rt::RuntimeClock {
    std::uint64_t now = 1'000;
    std::uint64_t now_ns() noexcept override { return now; }
    rt::Status sleep_until_ns(std::uint64_t release) noexcept override {
        now = release;
        return rt::Status::ok;
    }
    bool supports_absolute_sleep() const noexcept override { return true; }
};

rt::CallbackResult count_callback(
    void* data,
    const rt::CallbackContext& context) {
    if (!context.rate_release) {
        return rt::CallbackResult::error;
    }
    ++*static_cast<std::size_t*>(data);
    return rt::CallbackResult::ok;
}

struct BlockingProbe {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
};

struct PeriodicInspectionProbe {
    rt::Runtime* runtime = nullptr;
    std::size_t calls = 0;
};

rt::CallbackResult inspect_during_periodic(
    void* data,
    const rt::PeriodicFrameResult&) {
    auto& probe = *static_cast<PeriodicInspectionProbe*>(data);
    rt::RateTelemetryMetadata metadata;
    if (!probe.runtime ||
        probe.runtime->rate_telemetry_metadata(metadata) !=
            rt::Status::invalid_state) {
        return rt::CallbackResult::error;
    }
    ++probe.calls;
    return rt::CallbackResult::ok;
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

rt::CallbackResult blocking_callback(
    void* data,
    const rt::CallbackContext& context) {
    auto& probe = *static_cast<BlockingProbe*>(data);
    if (!context.rate_release) {
        return rt::CallbackResult::error;
    }
    probe.entered.store(true, std::memory_order_release);
    while (!probe.release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    return rt::CallbackResult::ok;
}

rt::RuntimeConfig test_config() {
    rt::RuntimeConfig config;
    config.callback_capacity = 8;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    config.snapshot_max_bytes = 64 * 1024;
    config.memory_budget_bytes = 4 * 1024 * 1024;
    return config;
}

void add_rate(
    rt::Runtime& runtime,
    const char* name,
    bool optional,
    rt::RateCriticality criticality,
    rt::RateLateAction late_action,
    std::size_t& calls,
    rt::PhaseHandle& phase,
    rt::RateDomainHandle& domain) {
    ASSERT_EQ(
        runtime.register_callback({name, &count_callback, &calls}, phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_rate_domain(
            {name, 100, 1, 50, 10, criticality, optional, late_action, 0},
            domain),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.bind_phase_to_rate_domain(phase, domain),
        rt::Status::ok);
}

void configure_pair(
    rt::Runtime& runtime,
    std::size_t telemetry_capacity,
    std::size_t& mandatory_calls,
    std::size_t& optional_calls) {
    ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
    ASSERT_EQ(
        runtime.set_rate_execution_policy({16, 7, 1, 1, telemetry_capacity}),
        rt::Status::ok);
    rt::PhaseHandle mandatory_phase;
    rt::PhaseHandle optional_phase;
    rt::RateDomainHandle mandatory_domain;
    rt::RateDomainHandle optional_domain;
    add_rate(
        runtime, "mandatory", false, rt::RateCriticality::critical,
        rt::RateLateAction::skip, mandatory_calls, mandatory_phase,
        mandatory_domain);
    add_rate(
        runtime, "optional", true, rt::RateCriticality::background,
        rt::RateLateAction::fail, optional_calls, optional_phase,
        optional_domain);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
}

} // namespace

TEST(RateTelemetry, NumericSchemaAndCounterTablesAreExact) {
    EXPECT_EQ(rt::rate_action_schema_version, 1u);
    EXPECT_EQ(sizeof(rt::RateActionRecord), 160u);
    EXPECT_EQ(static_cast<unsigned>(rt::RateActionId::execute_on_time), 0u);
    EXPECT_EQ(static_cast<unsigned>(rt::RateActionId::optional_shed), 6u);
    EXPECT_EQ(static_cast<unsigned>(rt::RateTransitionId::recover), 2u);
    EXPECT_EQ(static_cast<unsigned>(rt::RateActionReason::arithmetic_failure), 7u);
    EXPECT_EQ(static_cast<unsigned>(rt::RateCounterId::stale_reads), 19u);
    for (std::size_t index = 0; index < rt::rate_action_counter_count; ++index) {
        rt::RateCounterDefinition definition;
        ASSERT_TRUE(rt::rate_counter_definition(index, definition));
        EXPECT_EQ(static_cast<std::size_t>(definition.id), index);
        EXPECT_EQ(
            definition.kind,
            index == 16 || index == 17
                ? rt::RateCounterKind::gauge
                : rt::RateCounterKind::counter);
        EXPECT_FALSE(definition.name.empty());
    }
    rt::RateCounterDefinition invalid;
    EXPECT_FALSE(rt::rate_counter_definition(20, invalid));
}

TEST(RateTelemetry, ConcurrentPublishAndReadRemainBoundedAndRaceFree) {
    rt::detail::RateTelemetryRing ring(8);
    std::atomic<bool> start{false};
    constexpr std::size_t attempts = 20'000;
    const auto producer = [&](std::uint64_t frame_base) {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (std::size_t index = 0; index < attempts / 2; ++index) {
            rt::RateActionRecord record;
            record.frame_index = frame_base + index;
            record.release_count = 1;
            record.reference_record_count = 1;
            (void)ring.emit(record);
        }
    };
    std::thread first(producer, 0);
    std::thread second(producer, attempts);
    start.store(true, std::memory_order_release);
    while (ring.next_sequence() < attempts) {
        const auto end = ring.next_sequence();
        if (end != 0) {
            rt::RateActionRecord record;
            (void)ring.read_sequence(end - 1, record);
        }
        std::this_thread::yield();
    }
    first.join();
    second.join();
    EXPECT_EQ(ring.next_sequence(), attempts);
    EXPECT_EQ(ring.emitted() + ring.dropped(), attempts);
    const auto end = ring.next_sequence();
    const auto oldest = ring.oldest_sequence(end);
    std::size_t retained = 0;
    for (auto sequence = oldest; sequence < end; ++sequence) {
        rt::RateActionRecord record;
        if (ring.read_sequence(sequence, record)) {
            ++retained;
            EXPECT_EQ(record.sequence, sequence);
            EXPECT_EQ(record.release_count, 1u);
        }
    }
    EXPECT_LE(retained, 8u);
}

TEST(RateTelemetry, InspectionRejectsAnActiveStep) {
    ManualClock clock;
    rt::Runtime runtime(clock);
    ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
    ASSERT_EQ(runtime.set_rate_execution_policy({2, 1, 1, 1, 2}), rt::Status::ok);
    BlockingProbe probe;
    rt::PhaseHandle phase;
    rt::RateDomainHandle domain;
    ASSERT_EQ(
        runtime.register_callback({"blocking", &blocking_callback, &probe}, phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_rate_domain(
            {"rate", 100, 1, 100, 10, rt::RateCriticality::critical,
             false, rt::RateLateAction::fail, 0}, domain),
        rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(phase, domain), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    rt::Status step_status = rt::Status::internal_error;
    std::thread step_thread([&] {
        step_status = runtime.step({0, 100ns, std::nullopt, 1'000});
    });
    while (!probe.entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    rt::RateTelemetryMetadata metadata;
    EXPECT_EQ(runtime.rate_telemetry_metadata(metadata), rt::Status::invalid_state);
    rt::RateCounterSnapshot counters;
    EXPECT_EQ(runtime.rate_counters_snapshot(counters), rt::Status::invalid_state);
    probe.release.store(true, std::memory_order_release);
    step_thread.join();
    EXPECT_EQ(step_status, rt::Status::ok);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(RateTelemetry, RuntimeBoundCursorsRejectForeignInstancesTransactionally) {
    ManualClock first_clock;
    ManualClock second_clock;
    rt::Runtime first(first_clock);
    rt::Runtime second(second_clock);
    std::array<std::size_t, 4> calls{};
    configure_pair(first, 2, calls[0], calls[1]);
    configure_pair(second, 2, calls[2], calls[3]);
    rt::RateTelemetryCursor cursor;
    rt::RateTelemetryReadResult read;
    std::array<rt::RateActionRecord, 1> records{};
    ASSERT_EQ(first.read_rate_actions(cursor, records, read), rt::Status::ok);
    ASSERT_NE(cursor.runtime_id, 0u);
    const auto committed = cursor;
    EXPECT_EQ(
        second.read_rate_actions(cursor, records, read),
        rt::Status::invalid_argument);
    EXPECT_EQ(cursor.runtime_id, committed.runtime_id);
    EXPECT_EQ(cursor.next_sequence, committed.next_sequence);
}

TEST(PeriodicRuntime, RateTelemetrySummaryAndInspectionBoundaryAreExact) {
    ManualClock clock;
    rt::Runtime runtime(clock);
    std::size_t mandatory_calls = 0;
    std::size_t optional_calls = 0;
    configure_pair(runtime, 8, mandatory_calls, optional_calls);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    PeriodicInspectionProbe probe{&runtime};
    rt::PeriodicRunConfig config;
    config.frame_count = 2;
    config.period = 100ns;
    config.first_release_ns = 1'000;
    config.relative_deadline = 100ns;
    rt::PeriodicRunResult result;
    ASSERT_EQ(
        runtime.run_periodic(
            config, &inspect_during_periodic, &probe, &result),
        rt::Status::ok);
    EXPECT_EQ(probe.calls, 2u);
    EXPECT_EQ(mandatory_calls, 2u);
    EXPECT_EQ(optional_calls, 2u);
    EXPECT_EQ(result.rate.due_domain_releases, 4u);
    EXPECT_EQ(result.rate.optional_due_domain_releases, 2u);
    EXPECT_EQ(result.rate.optional_executed_domain_releases, 2u);
    EXPECT_EQ(result.rate.shed_domain_releases, 0u);
    EXPECT_EQ(result.rate.currently_shed_domains, 0u);
    EXPECT_EQ(result.rate.rate_policy_version, 7u);
    rt::RateTelemetryMetadata metadata;
    ASSERT_EQ(runtime.rate_telemetry_metadata(metadata), rt::Status::ok);
    EXPECT_EQ(metadata.next_sequence, 4u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(RateShedding, ThresholdTransitionIsImmediateAndRecoveryIsReverse) {
    ManualClock clock;
    clock.now = 2'000;
    rt::Runtime runtime(clock);
    std::size_t mandatory_calls = 0;
    std::size_t optional_calls = 0;
    configure_pair(runtime, 16, mandatory_calls, optional_calls);
    rt::ObservabilityMetadata global_metadata;
    ASSERT_EQ(runtime.observability_metadata(global_metadata), rt::Status::ok);
    EXPECT_EQ(global_metadata.schema_version, 2u);
    EXPECT_EQ(global_metadata.metric_count, 32u);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    rt::StepResult late;
    ASSERT_EQ(
        runtime.step({0, 100ns, std::nullopt, 1'000}, &late),
        rt::Status::ok);
    EXPECT_EQ(mandatory_calls, 0u);
    EXPECT_EQ(optional_calls, 0u);
    EXPECT_EQ(late.rate.optional_due_domain_releases, 1u);
    EXPECT_EQ(late.rate.shed_domain_releases, 1u);
    EXPECT_EQ(late.rate.shed_transitions, 1u);
    EXPECT_EQ(late.rate.currently_shed_domains, 1u);
    EXPECT_EQ(late.rate.rate_policy_version, 7u);

    clock.now = 1'100;
    rt::StepResult recovered;
    ASSERT_EQ(
        runtime.step({1, 100ns, std::nullopt, 1'100}, &recovered),
        rt::Status::ok);
    EXPECT_EQ(mandatory_calls, 1u);
    EXPECT_EQ(optional_calls, 1u);
    EXPECT_EQ(recovered.rate.optional_executed_domain_releases, 1u);
    EXPECT_EQ(recovered.rate.recovery_transitions, 1u);
    EXPECT_EQ(recovered.rate.currently_shed_domains, 0u);

    std::array<rt::RateActionRecord, 8> records{};
    rt::RateTelemetryCursor cursor;
    rt::RateTelemetryReadResult read;
    ASSERT_EQ(runtime.read_rate_actions(cursor, records, read), rt::Status::ok);
    ASSERT_EQ(read.records_read, 4u);
    EXPECT_EQ(records[0].action, rt::RateActionId::skip);
    EXPECT_EQ(records[0].transition, rt::RateTransitionId::shed);
    EXPECT_EQ(records[0].reason, rt::RateActionReason::late_threshold);
    EXPECT_EQ(records[0].shed_state_before, 0u);
    EXPECT_NE(records[0].shed_state_after, 0u);
    EXPECT_EQ(records[1].action, rt::RateActionId::optional_shed);
    EXPECT_EQ(records[1].reason, rt::RateActionReason::already_shed);
    EXPECT_EQ(records[2].transition, rt::RateTransitionId::recover);
    EXPECT_EQ(records[3].action, rt::RateActionId::execute_on_time);

    rt::RateCounterSnapshot counters;
    ASSERT_EQ(runtime.rate_counters_snapshot(counters), rt::Status::ok);
    EXPECT_EQ(counters.values[8], 2u);
    EXPECT_EQ(counters.values[9], 1u);
    EXPECT_EQ(counters.values[10], 1u);
    EXPECT_EQ(counters.values[11], 1u);
    EXPECT_EQ(counters.values[12], 1u);
    EXPECT_EQ(counters.values[16], 0u);
    EXPECT_EQ(counters.values[17], 7u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(RateTelemetry, ZeroCapacityAndOverwriteReportExactCursorGaps) {
    for (const auto capacity : std::array<std::size_t, 2>{0, 1}) {
        ManualClock clock;
        rt::Runtime runtime(clock);
        std::size_t mandatory_calls = 0;
        std::size_t optional_calls = 0;
        configure_pair(runtime, capacity, mandatory_calls, optional_calls);
        ASSERT_EQ(runtime.start(), rt::Status::ok);
        clock.now = 1'000;
        ASSERT_EQ(runtime.step({0, 100ns, std::nullopt, 1'000}), rt::Status::ok);
        clock.now = 1'100;
        ASSERT_EQ(runtime.step({1, 100ns, std::nullopt, 1'100}), rt::Status::ok);

        rt::RateTelemetryMetadata metadata;
        ASSERT_EQ(runtime.rate_telemetry_metadata(metadata), rt::Status::ok);
        EXPECT_EQ(metadata.capacity, capacity);
        EXPECT_EQ(metadata.next_sequence, 4u);
        if (capacity == 0) {
            EXPECT_EQ(metadata.records_emitted, 0u);
            EXPECT_EQ(metadata.records_dropped, 4u);
        } else {
            EXPECT_EQ(metadata.records_emitted, 4u);
            EXPECT_EQ(metadata.records_overwritten, 3u);
        }

        std::array<rt::RateActionRecord, 4> records{};
        rt::RateTelemetryCursor cursor;
        rt::RateTelemetryReadResult read;
        ASSERT_EQ(runtime.read_rate_actions(cursor, records, read), rt::Status::ok);
        EXPECT_EQ(read.lost_records, capacity == 0 ? 4u : 3u);
        EXPECT_EQ(read.records_read, capacity);
        if (capacity == 1) {
            EXPECT_EQ(records[0].sequence, 3u);
        }
        const auto committed = cursor;
        auto malformed = cursor;
        malformed.schema_version = 2;
        EXPECT_EQ(
            runtime.read_rate_actions(malformed, records, read),
            rt::Status::invalid_argument);
        EXPECT_EQ(malformed.runtime_id, committed.runtime_id);
        EXPECT_EQ(malformed.next_sequence, committed.next_sequence);
        EXPECT_EQ(runtime.stop(), rt::Status::ok);
    }
}

TEST(RateShedding, OptionalOrderIsCriticalityThenReverseRegistration) {
    ManualClock clock;
    clock.now = 10'000;
    rt::Runtime runtime(clock);
    ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
    ASSERT_EQ(runtime.set_rate_execution_policy({64, 9, 1, 1, 32}), rt::Status::ok);
    std::array<std::size_t, 4> calls{};
    std::array<rt::PhaseHandle, 4> phases{};
    std::array<rt::RateDomainHandle, 4> domains{};
    add_rate(runtime, "mandatory", false, rt::RateCriticality::critical,
             rt::RateLateAction::skip, calls[0], phases[0], domains[0]);
    add_rate(runtime, "optional-a", true, rt::RateCriticality::background,
             rt::RateLateAction::skip, calls[1], phases[1], domains[1]);
    add_rate(runtime, "optional-b", true, rt::RateCriticality::background,
             rt::RateLateAction::skip, calls[2], phases[2], domains[2]);
    add_rate(runtime, "optional-c", true, rt::RateCriticality::critical,
             rt::RateLateAction::skip, calls[3], phases[3], domains[3]);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    for (std::uint64_t step = 0; step < 3; ++step) {
        ASSERT_EQ(
            runtime.step({step, 100ns, std::nullopt, 1'000 + step * 100}),
            rt::Status::ok);
    }
    std::array<rt::RateActionRecord, 32> records{};
    rt::RateTelemetryCursor cursor;
    rt::RateTelemetryReadResult read;
    ASSERT_EQ(runtime.read_rate_actions(cursor, records, read), rt::Status::ok);
    std::vector<std::uint32_t> transitioned;
    for (std::size_t index = 0; index < read.records_read; ++index) {
        if (records[index].transition == rt::RateTransitionId::shed) {
            transitioned.push_back(records[index].transition_domain_registration_index);
        }
    }
    ASSERT_EQ(transitioned.size(), 3u);
    EXPECT_EQ(transitioned[0], 2u);
    EXPECT_EQ(transitioned[1], 1u);
    EXPECT_EQ(transitioned[2], 3u);

    clock.now = 1'300;
    for (std::uint64_t step = 3; step < 6; ++step) {
        ASSERT_EQ(
            runtime.step({step, 100ns, std::nullopt, 1'000 + step * 100}),
            rt::Status::ok);
    }
    ASSERT_EQ(runtime.read_rate_actions(cursor, records, read), rt::Status::ok);
    std::vector<std::uint32_t> recovered;
    for (std::size_t index = 0; index < read.records_read; ++index) {
        if (records[index].transition == rt::RateTransitionId::recover) {
            recovered.push_back(records[index].transition_domain_registration_index);
        }
    }
    ASSERT_EQ(recovered.size(), 3u);
    EXPECT_EQ(recovered[0], 3u);
    EXPECT_EQ(recovered[1], 1u);
    EXPECT_EQ(recovered[2], 2u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(RateShedding, StreakResetAndOptionalLatenessNeverDriveTransitions) {
    ManualClock clock;
    rt::Runtime runtime(clock);
    ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
    ASSERT_EQ(runtime.set_rate_execution_policy({16, 4, 2, 2, 16}), rt::Status::ok);
    std::array<std::size_t, 2> calls{};
    std::array<rt::PhaseHandle, 2> phases{};
    std::array<rt::RateDomainHandle, 2> domains{};
    ASSERT_EQ(runtime.register_callback(
                  {"mandatory", &count_callback, &calls[0]}, phases[0]),
              rt::Status::ok);
    ASSERT_EQ(runtime.register_rate_domain(
                  {"mandatory", 100, 1, 100, 10,
                   rt::RateCriticality::critical, false,
                   rt::RateLateAction::skip, 0}, domains[0]),
              rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(phases[0], domains[0]), rt::Status::ok);
    ASSERT_EQ(runtime.register_callback(
                  {"optional", &count_callback, &calls[1]}, phases[1]),
              rt::Status::ok);
    ASSERT_EQ(runtime.register_rate_domain(
                  {"optional", 100, 1, 1, 10,
                   rt::RateCriticality::background, true,
                   rt::RateLateAction::skip, 0}, domains[1]),
              rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(phases[1], domains[1]), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    rt::StepResult result;
    clock.now = 2'000;
    ASSERT_EQ(runtime.step({0, 100ns, std::nullopt, 1'000}, &result), rt::Status::ok);
    EXPECT_EQ(result.rate.shed_transitions, 0u);
    clock.now = 1'150;
    ASSERT_EQ(runtime.step({1, 100ns, std::nullopt, 1'100}, &result), rt::Status::ok);
    EXPECT_EQ(result.rate.skipped_domain_releases, 1u);
    EXPECT_EQ(result.rate.shed_transitions, 0u);
    clock.now = 2'000;
    ASSERT_EQ(runtime.step({2, 100ns, std::nullopt, 1'200}, &result), rt::Status::ok);
    EXPECT_EQ(result.rate.shed_transitions, 0u);
    ASSERT_EQ(runtime.step({3, 100ns, std::nullopt, 1'300}, &result), rt::Status::ok);
    EXPECT_EQ(result.rate.shed_transitions, 1u);
    EXPECT_EQ(result.rate.currently_shed_domains, 1u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(RateShedding, TerminalFailPreventsPolicyTransition) {
    ManualClock clock;
    clock.now = 2'000;
    rt::Runtime runtime(clock);
    ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
    ASSERT_EQ(runtime.set_rate_execution_policy({8, 3, 1, 1, 8}), rt::Status::ok);
    std::array<std::size_t, 2> calls{};
    std::array<rt::PhaseHandle, 2> phases{};
    std::array<rt::RateDomainHandle, 2> domains{};
    add_rate(runtime, "mandatory", false, rt::RateCriticality::critical,
             rt::RateLateAction::fail, calls[0], phases[0], domains[0]);
    add_rate(runtime, "optional", true, rt::RateCriticality::background,
             rt::RateLateAction::skip, calls[1], phases[1], domains[1]);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    rt::StepResult result;
    EXPECT_EQ(
        runtime.step({0, 100ns, std::nullopt, 1'000}, &result),
        rt::Status::callback_failed);
    EXPECT_EQ(result.rate.failed_domain_releases, 1u);
    EXPECT_EQ(result.rate.shed_transitions, 0u);
    EXPECT_EQ(result.rate.currently_shed_domains, 0u);
    std::array<rt::RateActionRecord, 2> records{};
    rt::RateTelemetryCursor cursor;
    rt::RateTelemetryReadResult read;
    ASSERT_EQ(runtime.read_rate_actions(cursor, records, read), rt::Status::ok);
    ASSERT_EQ(read.records_read, 1u);
    EXPECT_EQ(records[0].action, rt::RateActionId::fail);
    EXPECT_EQ(records[0].transition, rt::RateTransitionId::none);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(RateShedding, LargeLateWindowAggregatesAfterBoundedTransitions) {
    ManualClock clock;
    clock.now = 10'000'000;
    rt::Runtime runtime(clock);
    std::size_t mandatory_calls = 0;
    std::size_t optional_calls = 0;
    configure_pair(runtime, 16, mandatory_calls, optional_calls);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    rt::StepResult result;
    ASSERT_EQ(
        runtime.step({0, 1'000'000ns, std::nullopt, 1'000}, &result),
        rt::Status::ok);
    EXPECT_EQ(result.rate.due_domain_releases, 20'000u);
    EXPECT_EQ(result.rate.optional_due_domain_releases, 10'000u);
    EXPECT_EQ(result.rate.skipped_domain_releases, 10'000u);
    EXPECT_EQ(result.rate.shed_domain_releases, 10'000u);
    EXPECT_EQ(result.rate.shed_transitions, 1u);
    EXPECT_EQ(result.rate.currently_shed_domains, 1u);
    EXPECT_EQ(mandatory_calls, 0u);
    EXPECT_EQ(optional_calls, 0u);

    std::array<rt::RateActionRecord, 8> records{};
    rt::RateTelemetryCursor cursor;
    rt::RateTelemetryReadResult read;
    ASSERT_EQ(runtime.read_rate_actions(cursor, records, read), rt::Status::ok);
    ASSERT_EQ(read.records_read, 4u);
    EXPECT_EQ(records[2].release_count, 9'999u);
    EXPECT_EQ(records[2].reference_record_count, 9'999u);
    EXPECT_EQ(records[2].release_period_ns, 100u);
    EXPECT_EQ(records[3].release_count, 9'999u);
    EXPECT_EQ(records[3].action, rt::RateActionId::optional_shed);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(RateShedding, LargeOptionalLatePrefixAggregatesBeforeShedThreshold) {
    ManualClock clock;
    clock.now = 10'000'000;
    rt::Runtime runtime(clock);
    ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
    ASSERT_EQ(
        runtime.set_rate_execution_policy({16, 11, 2, 1, 16}),
        rt::Status::ok);
    std::array<std::size_t, 2> calls{};
    std::array<rt::PhaseHandle, 2> phases{};
    std::array<rt::RateDomainHandle, 2> domains{};
    ASSERT_EQ(
        runtime.register_callback(
            {"mandatory", &count_callback, &calls[0]}, phases[0]),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_rate_domain(
            {"mandatory", 1'000'000, 1, 50, 10,
             rt::RateCriticality::critical, false,
             rt::RateLateAction::skip, 0},
            domains[0]),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.bind_phase_to_rate_domain(phases[0], domains[0]),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback(
            {"optional", &count_callback, &calls[1]}, phases[1]),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_rate_domain(
            {"optional", 100, 1, 50, 10,
             rt::RateCriticality::background, true,
             rt::RateLateAction::skip, 0},
            domains[1]),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.bind_phase_to_rate_domain(phases[1], domains[1]),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    rt::StepResult result;
    ASSERT_EQ(
        runtime.step({0, 1'000'000ns, std::nullopt, 1'000}, &result),
        rt::Status::ok);
    EXPECT_EQ(result.rate.due_domain_releases, 10'001u);
    EXPECT_EQ(result.rate.optional_due_domain_releases, 10'000u);
    EXPECT_EQ(result.rate.skipped_domain_releases, 10'001u);
    EXPECT_EQ(result.rate.shed_transitions, 0u);
    EXPECT_EQ(result.rate.shed_domain_releases, 0u);
    EXPECT_EQ(calls[0], 0u);
    EXPECT_EQ(calls[1], 0u);

    std::array<rt::RateActionRecord, 4> records{};
    rt::RateTelemetryCursor cursor;
    rt::RateTelemetryReadResult read;
    ASSERT_EQ(runtime.read_rate_actions(cursor, records, read), rt::Status::ok);
    ASSERT_EQ(read.records_read, 3u);
    EXPECT_EQ(read.lost_records, 0u);
    EXPECT_EQ(records[0].domain_registration_index, 0u);
    EXPECT_EQ(records[0].release_count, 1u);
    EXPECT_EQ(records[1].domain_registration_index, 1u);
    EXPECT_EQ(records[1].release_count, 1u);
    EXPECT_EQ(records[2].domain_registration_index, 1u);
    EXPECT_EQ(records[2].first_domain_release_sequence, 1u);
    EXPECT_EQ(records[2].release_count, 9'999u);
    EXPECT_EQ(records[2].reference_record_count, 9'999u);
    EXPECT_EQ(records[2].action, rt::RateActionId::skip);
    EXPECT_EQ(records[2].reason, rt::RateActionReason::deadline_late);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(RateShedding, PolicyAndOptionalChannelValidationAreTransactional) {
    rt::Runtime malformed;
    EXPECT_EQ(
        malformed.set_rate_execution_policy({4, 0, 1, 1, 0}),
        rt::Status::invalid_argument);
    EXPECT_EQ(
        malformed.set_rate_execution_policy(
            {4, 1, rt::rate_policy_threshold_limit + 1u, 1, 0}),
        rt::Status::invalid_argument);
    EXPECT_EQ(
        malformed.set_rate_execution_policy(
            {4, 1, 1, 1, rt::rate_telemetry_capacity_limit + 1}),
        rt::Status::invalid_argument);

    ManualClock clock;
    rt::Runtime runtime(clock);
    ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
    ASSERT_EQ(runtime.set_rate_execution_policy({8, 1, 1, 1, 0}), rt::Status::ok);
    std::size_t producer_calls = 0;
    std::size_t consumer_calls = 0;
    rt::PhaseHandle producer;
    rt::PhaseHandle consumer;
    rt::RateDomainHandle optional_domain;
    rt::RateDomainHandle mandatory_domain;
    add_rate(runtime, "optional-producer", true, rt::RateCriticality::background,
             rt::RateLateAction::hold, producer_calls, producer, optional_domain);
    add_rate(runtime, "mandatory-consumer", false, rt::RateCriticality::critical,
             rt::RateLateAction::fail, consumer_calls, consumer, mandatory_domain);
    const std::array initial{std::byte{1}};
    rt::CrossRateChannelHandle channel;
    ASSERT_EQ(
        runtime.register_cross_rate_channel(
            {"forbidden-optional", producer, consumer, 1, initial}, channel),
        rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    EXPECT_FALSE(runtime.cross_rate_model_enabled());
    EXPECT_EQ(producer_calls, 0u);
    EXPECT_EQ(consumer_calls, 0u);
}

TEST(RateShedding, CheckpointRoundTripRestoresStreakAndShedState) {
    ManualClock source_clock;
    source_clock.now = 2'000;
    rt::Runtime source(source_clock);
    std::size_t source_mandatory = 0;
    std::size_t source_optional = 0;
    configure_pair(source, 4, source_mandatory, source_optional);
    ASSERT_EQ(source.start(), rt::Status::ok);
    ASSERT_EQ(source.step({0, 100ns, std::nullopt, 1'000}), rt::Status::ok);
    ASSERT_EQ(source.stop(), rt::Status::ok);
    std::size_t required = 0;
    ASSERT_EQ(source.checkpoint_size(required), rt::Status::ok);
    std::vector<std::byte> checkpoint(required);
    rt::ArtifactWriteResult written;
    ASSERT_EQ(source.write_checkpoint(0, checkpoint, written), rt::Status::ok);
    checkpoint.resize(written.bytes_written);

    ManualClock restored_clock;
    restored_clock.now = 1'100;
    rt::Runtime restored(restored_clock);
    std::size_t restored_mandatory = 0;
    std::size_t restored_optional = 0;
    configure_pair(restored, 4, restored_mandatory, restored_optional);

    rt::CheckpointMetadata metadata;
    ASSERT_EQ(
        rt::inspect_checkpoint_artifact(checkpoint, metadata),
        rt::Status::ok);
    rt::detail::CheckpointRecordCursor record_cursor;
    rt::detail::CheckpointRecordView active_record;
    ASSERT_TRUE(rt::detail::next_checkpoint_record(
        checkpoint, metadata, record_cursor, active_record));
    ASSERT_EQ(active_record.name, "rtfw.rate-dispatch");
    std::vector<std::byte> malformed_payload(
        active_record.payload.begin(), active_record.payload.end());
    ASSERT_EQ(malformed_payload.size(), 145u);
    malformed_payload[144] = std::byte{0};
    EncodedState malformed_state{
        active_record.name, active_record.schema_version, malformed_payload};
    std::vector<std::byte> semantic_corruption(checkpoint.size());
    rt::ArtifactWriteResult malformed_write;
    ASSERT_EQ(
        rt::detail::encode_checkpoint_artifact(
            metadata, 1, &provide_encoded_state, &malformed_state,
            test_config().snapshot_max_bytes, semantic_corruption,
            malformed_write),
        rt::Status::ok);
    semantic_corruption.resize(malformed_write.bytes_written);
    ASSERT_EQ(
        rt::inspect_checkpoint_artifact(semantic_corruption, metadata),
        rt::Status::ok);
    EXPECT_EQ(
        restored.restore_checkpoint(semantic_corruption),
        rt::Status::incompatible_artifact);
    rt::RateCounterSnapshot unchanged;
    ASSERT_EQ(restored.rate_counters_snapshot(unchanged), rt::Status::ok);
    EXPECT_EQ(unchanged.values[16], 0u);
    ASSERT_EQ(restored.restore_checkpoint(checkpoint), rt::Status::ok);
    rt::RateCounterSnapshot before;
    ASSERT_EQ(restored.rate_counters_snapshot(before), rt::Status::ok);
    EXPECT_EQ(before.values[16], 1u);
    EXPECT_EQ(before.values[11], 0u);
    ASSERT_EQ(restored.start(), rt::Status::ok);
    rt::StepResult result;
    ASSERT_EQ(
        restored.step({1, 100ns, std::nullopt, 1'100}, &result),
        rt::Status::ok);
    EXPECT_EQ(result.rate.recovery_transitions, 1u);
    EXPECT_EQ(result.rate.currently_shed_domains, 0u);
    EXPECT_EQ(restored_optional, 1u);
    EXPECT_EQ(restored.stop(), rt::Status::ok);
}

TEST(MemoryPlan, RateTelemetryIsInsideRuntimeControlExactlyOnce) {
    ManualClock clock;
    rt::Runtime runtime(clock);
    std::size_t mandatory_calls = 0;
    std::size_t optional_calls = 0;
    configure_pair(runtime, 8, mandatory_calls, optional_calls);
    rt::MemoryPlan plan;
    ASSERT_TRUE(runtime.memory_plan(plan));
    EXPECT_EQ(plan.optional_rate_domain_count, 1u);
    EXPECT_EQ(plan.rate_telemetry_capacity, 8u);
    EXPECT_GT(plan.rate_telemetry_slot_bytes, sizeof(rt::RateActionRecord));
    EXPECT_EQ(
        plan.rate_telemetry_storage_bytes,
        plan.rate_telemetry_capacity * plan.rate_telemetry_slot_bytes);
    EXPECT_GT(plan.rate_shedding_state_bytes, 0u);
    EXPECT_GT(plan.rate_telemetry_counter_bytes, 0u);
    EXPECT_LE(plan.rate_plan_bytes, plan.runtime_control_bytes);
    EXPECT_EQ(
        plan.planned_bytes,
        plan.runtime_control_bytes + plan.executor_control_bytes +
            plan.device_control_bytes + plan.phase_scratch_total_bytes +
            plan.task_scratch_total_bytes + plan.trace_storage_bytes);
}
