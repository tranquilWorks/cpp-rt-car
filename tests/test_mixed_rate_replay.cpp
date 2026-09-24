#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <rt/runtime.hpp>

namespace {

bool load_u64(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::uint64_t& value) {
    value = 0;
    if (offset > bytes.size() || bytes.size() - offset < sizeof(value)) {
        return false;
    }
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(
            static_cast<std::uint8_t>(bytes[offset + index])) << (8u * index);
    }
    return true;
}

bool store_u64(
    std::span<std::byte> bytes,
    std::size_t offset,
    std::uint64_t value) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(value)) {
        return false;
    }
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] =
            static_cast<std::byte>(value >> (8u * index));
    }
    return true;
}

struct ManualClock final : rt::RuntimeClock {
    std::uint64_t now = 1'000;
    std::uint64_t now_ns() noexcept override { return now; }
    rt::Status sleep_until_ns(std::uint64_t release) noexcept override {
        now = release;
        return rt::Status::ok;
    }
    bool supports_absolute_sleep() const noexcept override { return true; }
};

rt::CallbackResult increment_state(
    void* data,
    const rt::CallbackContext& context) {
    if (!context.rate_release) {
        return rt::CallbackResult::error;
    }
    auto bytes = std::span<std::byte>(
        static_cast<std::byte*>(data), sizeof(std::uint64_t));
    std::uint64_t value = 0;
    if (!load_u64(bytes, 0, value) ||
        !store_u64(bytes, 0, value + 1)) {
        return rt::CallbackResult::error;
    }
    return rt::CallbackResult::ok;
}

struct InputProbe {
    std::size_t calls = 0;
};

rt::CallbackResult apply_input(
    void* data,
    const rt::ReplayInputView& input) {
    auto& probe = *static_cast<InputProbe*>(data);
    if (!input.frame.nominal_release_ns || !input.payload.empty()) {
        return rt::CallbackResult::error;
    }
    ++probe.calls;
    return rt::CallbackResult::ok;
}

rt::RuntimeConfig replay_config() {
    rt::RuntimeConfig config;
    config.callback_capacity = 4;
    config.executor_queue_capacity = 4;
    config.task_scratch_slots = 4;
    config.state_capacity = 4;
    config.snapshot_max_bytes = 64 * 1024;
    config.memory_budget_bytes = 4 * 1024 * 1024;
    return config;
}

} // namespace

namespace {
constexpr std::uint64_t watchdog_replay_budget = 60'000'000'000;
constexpr std::uint64_t watchdog_replay_period = 4 * watchdog_replay_budget;

struct WatchdogReplayProbe {
    struct Clock final : rt::RuntimeClock {
        std::atomic<std::uint64_t> now{1000};
        std::uint64_t now_ns() noexcept override { return now.load(); }
    } clock;
    std::array<std::byte, 8> state{};
    std::array<std::uint32_t, 8> observed_levels{};
    std::size_t callbacks = 0;
    std::size_t inputs = 0;
    bool expire = false;
    bool live = false;

    static rt::CallbackResult work(void* opaque, const rt::CallbackContext& ctx) {
        auto& p = *static_cast<WatchdogReplayProbe*>(opaque);
        if (!ctx.rate_release || ctx.frame.frame_index < 1 ||
            ctx.frame.frame_index > p.observed_levels.size()) {
            return rt::CallbackResult::error;
        }
        p.observed_levels[ctx.frame.frame_index - 1] = ctx.degradation_level;
        std::uint64_t value = 0;
        if (!load_u64(p.state, 0, value)) return rt::CallbackResult::error;
        if (p.live) {
            std::uint64_t control = 0;
            if (!ctx.live_control || ctx.live_control->records.size() != 1 ||
                !load_u64(ctx.live_control->records[0].payload, 0, control) ||
                control != ctx.frame.frame_index + 17) {
                return rt::CallbackResult::error;
            }
        }
        if (!store_u64(p.state, 0, value * 17 + ctx.frame.frame_index)) {
            return rt::CallbackResult::error;
        }
        ++p.callbacks;
        p.clock.now.fetch_add(p.expire ? watchdog_replay_budget + 1 : 5);
        return rt::CallbackResult::ok;
    }

    static rt::CallbackResult apply(void* opaque, const rt::ReplayInputView& input) {
        auto& p = *static_cast<WatchdogReplayProbe*>(opaque);
        if (!input.frame.nominal_release_ns || !input.payload.empty() ||
            input.input_type != 23 || input.frame.frame_index != p.inputs + 1) {
            return rt::CallbackResult::error;
        }
        p.clock.now = *input.frame.nominal_release_ns;
        ++p.inputs;
        return rt::CallbackResult::ok;
    }
};

void verify_watchdog_replay(std::uint32_t cap, unsigned mode, bool live) {
    SCOPED_TRACE(::testing::Message() << "cap=" << cap << " mode=" << mode << " live=" << live);
    WatchdogReplayProbe probe;
    probe.expire = mode == 2;
    probe.live = live;
    rt::Runtime runtime(probe.clock);
    auto config = replay_config();
    config.worker_count = 1;
    config.watchdog_timeout_ns = mode == 0 ? 0 : watchdog_replay_budget;
    config.watchdog_max_degradation_level = cap;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(runtime.set_rate_execution_policy({8, 23, 1, 1, 64}), rt::Status::ok);
    ASSERT_EQ(runtime.set_mixed_rate_closure_policy({23, 128, 64, 1024 * 1024, 16,
        rt::MixedRateOverflowPolicy::overwrite_committed, true, true, {}}), rt::Status::ok);
    if (live) {
        rt::LiveControlPolicy policy;
        policy.policy_identity = 23;
        policy.mailbox_capacity = 1;policy.producer_capacity = 1;
        policy.record_capacity = 4;policy.payload_bytes_per_record = 8;
        policy.total_payload_storage_bytes = 32;
        ASSERT_EQ(runtime.set_live_control_policy(policy), rt::Status::ok);
        rt::LiveControlMailboxRegistration mailbox;
        mailbox.mailbox_identity = 101;mailbox.record_capacity = 4;
        mailbox.payload_bytes_per_record = 8;
        ASSERT_EQ(runtime.register_live_control_mailbox(mailbox), rt::Status::ok);
        rt::LiveControlProducerRegistration producer;
        producer.mailbox_identity = 101;producer.producer_identity = 1001;
        ASSERT_EQ(runtime.register_live_control_producer(producer), rt::Status::ok);
        rt::LiveControlClosurePolicy closure;
        closure.policy_identity = 23;closure.action_capacity = 256;
        closure.retained_generation_capacity = 16;closure.retained_record_capacity = 32;
        closure.retained_payload_bytes = 256;closure.replay_record_capacity = 256;
        closure.replay_max_bytes = 1024 * 1024;closure.replay_enabled = true;
        ASSERT_EQ(runtime.set_live_control_closure_policy(closure), rt::Status::ok);
        rt::LiveControlReplayRetentionPolicy retention;
        retention.policy_identity = 23;retention.admission_capacity = 32;
        retention.payload_capacity_bytes = 128;
        ASSERT_EQ(runtime.set_live_control_replay_retention_policy(retention), rt::Status::ok);
    }
    rt::PhaseHandle phase;rt::RateDomainHandle domain;
    ASSERT_EQ(runtime.register_callback({"watchdog-replay", WatchdogReplayProbe::work, &probe}, phase), rt::Status::ok);
    ASSERT_EQ(runtime.register_rate_domain({"watchdog-rate", watchdog_replay_period, 1,
        watchdog_replay_period, 1}, domain), rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(phase, domain), rt::Status::ok);
    ASSERT_EQ(runtime.register_state({"watchdog-state", 1, probe.state}), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    std::size_t bytes = 0;
    ASSERT_EQ(runtime.checkpoint_size(bytes), rt::Status::ok);
    std::vector<std::byte> checkpoint(bytes);
    rt::ArtifactWriteResult write;
    ASSERT_EQ(runtime.write_checkpoint(0, checkpoint, write), rt::Status::ok);
    rt::LiveControlProducerHandle producer;
    if (live) {
        ASSERT_EQ(runtime.live_control_producer_handle(101, 1001, producer), rt::Status::ok);
    }
    std::array<rt::ReplayInputRecord, 8> inputs{};
    std::uint64_t expected_state = 0;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto release = 1000 + i * watchdog_replay_period;
        inputs[i] = {{i + 1, std::chrono::nanoseconds{watchdog_replay_period}, std::nullopt, release}, 23, {}};
        if (live) {
            std::array<std::byte, 8> payload{};
            ASSERT_TRUE(store_u64(payload, 0, i + 18));
            rt::LiveControlUpdateRecord update;
            update.runtime_id = producer.runtime_id;
            update.configuration_generation = producer.configuration_generation;
            update.mailbox_identity = 101;update.producer_identity = 1001;
            update.producer_sequence = i + 1;update.target_frame_index = i + 1;
            update.payload_bytes = 8;update.payload_digest = rt::live_control_payload_digest(payload);
            rt::LiveControlAdmissionResult result;
            ASSERT_EQ(runtime.stage_live_control_update(producer, update, payload, result), rt::Status::ok);
            ASSERT_EQ(result, rt::LiveControlAdmissionResult::accepted);
            ++update.producer_sequence;update.payload_digest ^= 1;
            ASSERT_EQ(runtime.stage_live_control_update(producer, update, payload, result), rt::Status::ok);
            ASSERT_EQ(result, rt::LiveControlAdmissionResult::invalid);
        }
        probe.clock.now = release;
        rt::StepResult result;
        ASSERT_EQ(runtime.step(inputs[i].frame, &result), rt::Status::ok);
        EXPECT_EQ(result.watchdog_fired, mode == 2);
        EXPECT_EQ(result.degradation_level, mode == 2 ? std::min<std::uint32_t>(static_cast<std::uint32_t>(i + 1), cap) : 0u);
        EXPECT_EQ(probe.observed_levels[i], mode == 2 ? std::min<std::uint32_t>(static_cast<std::uint32_t>(i), cap) : 0u);
        expected_state = expected_state * 17 + i + 1;
    }
    std::array<rt::MixedRateActionRecord, 128> actions{};
    rt::MixedRateActionCursor cursor;rt::MixedRateActionReadResult read;
    ASSERT_EQ(runtime.read_mixed_rate_actions(cursor, actions, read), rt::Status::ok);
    EXPECT_EQ(read.lost_records, 0u);
    std::size_t events = 0;
    for (std::size_t i = 0; i < read.records_read; ++i) {
        const auto& action = actions[i];
        if (action.action != rt::MixedRateActionId::watchdog_transition) continue;
        EXPECT_EQ(action.frame_index, events + 1);
        EXPECT_EQ(action.degradation_before, std::min<std::uint32_t>(static_cast<std::uint32_t>(events), cap));
        EXPECT_EQ(action.degradation_after, std::min<std::uint32_t>(static_cast<std::uint32_t>(events + 1), cap));
        ++events;
    }
    EXPECT_EQ(events, mode == 2 ? 8u : 0u);
    ASSERT_EQ(runtime.write_active_replay_artifact(checkpoint, inputs, {}, write), rt::Status::capacity_exceeded);
    std::vector<std::byte> artifact(write.required_bytes);
    ASSERT_EQ(runtime.write_active_replay_artifact(checkpoint, inputs, artifact, write), rt::Status::ok);
    rt::ActiveReplayMetadata metadata;
    ASSERT_EQ(rt::inspect_active_replay_artifact(artifact, metadata), rt::Status::ok);
    if (live) {
        auto nested = artifact;
        ASSERT_EQ(runtime.write_live_control_replay_artifact(checkpoint, nested,
            rt::LiveControlNestedArtifactKind::active_replay, {}, write), rt::Status::capacity_exceeded);
        artifact.resize(write.required_bytes);
        ASSERT_EQ(runtime.write_live_control_replay_artifact(checkpoint, nested,
            rt::LiveControlNestedArtifactKind::active_replay, artifact, write), rt::Status::ok);
    }
    probe.callbacks = 0;probe.inputs = 0;probe.observed_levels = {};
    auto corrupt = artifact;corrupt.back() ^= std::byte{1};
    const auto invalid = live ? runtime.replay_live_control(corrupt, WatchdogReplayProbe::apply, &probe)
                              : runtime.replay_active(corrupt, WatchdogReplayProbe::apply, &probe);
    EXPECT_NE(invalid, rt::Status::ok);
    std::uint64_t actual = 0;ASSERT_TRUE(load_u64(probe.state, 0, actual));
    EXPECT_EQ(actual, expected_state);EXPECT_EQ(probe.inputs, 0u);EXPECT_EQ(probe.callbacks, 0u);
    if (live) {
        rt::LiveControlReplayResult result;
        EXPECT_EQ(runtime.replay_live_control(artifact, WatchdogReplayProbe::apply, &probe, &result), rt::Status::ok) << runtime.last_error();
        EXPECT_EQ(result.frames_replayed, 8u);EXPECT_EQ(result.generations_compared, 8u);
        EXPECT_EQ(result.mismatch_status, rt::Status::ok);
        rt::LiveControlMailboxInfo info;ASSERT_TRUE(runtime.live_control_mailbox_info(101, info));
        EXPECT_EQ(info.accepted, 8u);EXPECT_EQ(info.invalid, 8u);EXPECT_EQ(info.occupancy, 0u);
    } else {
        rt::ActiveReplayResult result;
        EXPECT_EQ(runtime.replay_active(artifact, WatchdogReplayProbe::apply, &probe, &result), rt::Status::ok) << runtime.last_error();
        EXPECT_EQ(result.replay.frames_replayed, 8u);
        EXPECT_EQ(result.actions_compared, metadata.action_record_count);
        EXPECT_EQ(result.mismatch_status, rt::Status::ok);
    }
    ASSERT_TRUE(load_u64(probe.state, 0, actual));
    EXPECT_EQ(actual, expected_state);EXPECT_EQ(probe.inputs, 8u);EXPECT_EQ(probe.callbacks, 8u);
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        EXPECT_EQ(probe.observed_levels[i], mode == 2 ? std::min<std::uint32_t>(static_cast<std::uint32_t>(i), cap) : 0u);
    }
    EXPECT_EQ(runtime.degradation_level(), mode == 2 ? cap : 0u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}
} // namespace

TEST(MixedRateReplay, WatchdogTransitionsPreserveActualPriorLevelAtSaturation) {
    for (const auto cap : {1u, 3u}) {
        for (unsigned mode = 0; mode < 3; ++mode) {
            verify_watchdog_replay(cap, mode, false);
        }
    }
}

TEST(MixedRateReplay, LiveControlWatchdogReplayClosesSaturatedFrames) {
    for (const auto cap : {1u, 3u}) {
        for (unsigned mode = 0; mode < 3; ++mode) {
            verify_watchdog_replay(cap, mode, true);
        }
    }
}

TEST(MixedRateReplay, ActiveArtifactRoundTripsAndDrivesTranscriptDecisions) {
    using namespace std::chrono_literals;

    ManualClock clock;
    rt::Runtime runtime(clock);
    ASSERT_EQ(runtime.configure(replay_config()), rt::Status::ok);
    ASSERT_EQ(
        runtime.set_rate_execution_policy({8, 3, 1, 1, 8}),
        rt::Status::ok);
    const rt::MixedRateClosurePolicy closure{
        11,
        16,
        16,
        64 * 1024,
        8,
        rt::MixedRateOverflowPolicy::overwrite_committed,
        true,
        true,
        {},
    };
    ASSERT_EQ(
        runtime.set_mixed_rate_closure_policy(closure),
        rt::Status::ok);

    std::array<std::byte, 8> state{};
    rt::PhaseHandle phase;
    rt::RateDomainHandle domain;
    ASSERT_EQ(
        runtime.register_callback(
            {"counter", &increment_state, state.data()},
            phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_rate_domain(
            {"rate", 100, 1, 100, 10},
            domain),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.bind_phase_to_rate_domain(phase, domain),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_state({"state.counter", 1, state}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    std::size_t checkpoint_bytes = 0;
    ASSERT_EQ(runtime.checkpoint_size(checkpoint_bytes), rt::Status::ok);
    std::vector<std::byte> checkpoint(checkpoint_bytes);
    rt::ArtifactWriteResult checkpoint_result;
    ASSERT_EQ(
        runtime.write_checkpoint(0, checkpoint, checkpoint_result),
        rt::Status::ok);
    checkpoint.resize(checkpoint_result.bytes_written);

    const std::array frames{
        rt::HostFrameContext{1, 100ns, std::nullopt, std::uint64_t{1'000}},
        rt::HostFrameContext{2, 100ns, std::nullopt, std::uint64_t{1'100}},
    };
    for (const auto& frame : frames) {
        ASSERT_EQ(runtime.step(frame), rt::Status::ok);
    }
    std::uint64_t state_value = 0;
    ASSERT_TRUE(load_u64(state, 0, state_value));
    ASSERT_EQ(state_value, 2u);

    const std::array inputs{
        rt::ReplayInputRecord{frames[0], 1, {}},
        rt::ReplayInputRecord{frames[1], 1, {}},
    };
    std::vector<std::byte> artifact(64 * 1024);
    rt::ArtifactWriteResult artifact_result;
    ASSERT_EQ(
        runtime.write_active_replay_artifact(
            checkpoint, inputs, artifact, artifact_result),
        rt::Status::ok);
    artifact.resize(artifact_result.bytes_written);

    rt::ActiveReplayMetadata metadata;
    ASSERT_EQ(
        rt::inspect_active_replay_artifact(artifact, metadata),
        rt::Status::ok);
    EXPECT_EQ(metadata.schema_version, 1u);
    EXPECT_EQ(metadata.input_record_count, inputs.size());
    EXPECT_EQ(metadata.action_record_count, 2u);
    EXPECT_EQ(metadata.first_action_sequence, 0u);
    EXPECT_EQ(metadata.last_action_sequence, 1u);
    EXPECT_EQ(metadata.first_frame_index, 1u);
    EXPECT_EQ(metadata.last_frame_index, 2u);

    InputProbe input_probe;
    rt::ActiveReplayResult replay_result;
    ASSERT_EQ(
        runtime.replay_active(
            artifact, &apply_input, &input_probe, &replay_result),
        rt::Status::ok);
    EXPECT_EQ(input_probe.calls, 2u);
    EXPECT_EQ(replay_result.replay.records_processed, 2u);
    EXPECT_EQ(replay_result.replay.frames_replayed, 2u);
    EXPECT_EQ(replay_result.actions_compared, 2u);
    EXPECT_EQ(replay_result.mismatch_status, rt::Status::ok);
    ASSERT_TRUE(load_u64(state, 0, state_value));
    EXPECT_EQ(state_value, 2u);

    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(MixedRateReplay, ParserRejectsMutationTrailingBytesAndShortOutput) {
    using namespace std::chrono_literals;

    ManualClock clock;
    rt::Runtime runtime(clock);
    ASSERT_EQ(runtime.configure(replay_config()), rt::Status::ok);
    ASSERT_EQ(runtime.set_rate_execution_policy({4}), rt::Status::ok);
    ASSERT_EQ(
        runtime.set_mixed_rate_closure_policy({
            5,
            8,
            8,
            32 * 1024,
            4,
            rt::MixedRateOverflowPolicy::overwrite_committed,
            true,
            true,
            {},
        }),
        rt::Status::ok);
    std::array<std::byte, 8> state{};
    rt::PhaseHandle phase;
    rt::RateDomainHandle domain;
    ASSERT_EQ(
        runtime.register_callback(
            {"counter", &increment_state, state.data()}, phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_rate_domain(
            {"rate", 100, 1, 100, 10}, domain),
        rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(phase, domain), rt::Status::ok);
    ASSERT_EQ(runtime.register_state({"counter", 1, state}), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    std::size_t checkpoint_bytes = 0;
    ASSERT_EQ(runtime.checkpoint_size(checkpoint_bytes), rt::Status::ok);
    std::vector<std::byte> checkpoint(checkpoint_bytes);
    rt::ArtifactWriteResult checkpoint_result;
    ASSERT_EQ(
        runtime.write_checkpoint(0, checkpoint, checkpoint_result),
        rt::Status::ok);
    checkpoint.resize(checkpoint_result.bytes_written);
    const rt::HostFrameContext frame{
        1, 100ns, std::nullopt, std::uint64_t{1'000}};
    ASSERT_EQ(runtime.step(frame), rt::Status::ok);
    const std::array inputs{rt::ReplayInputRecord{frame, 1, {}}};

    std::array<std::byte, 1> short_output{};
    rt::ArtifactWriteResult short_result;
    EXPECT_EQ(
        runtime.write_active_replay_artifact(
            checkpoint, inputs, short_output, short_result),
        rt::Status::capacity_exceeded);
    EXPECT_GT(short_result.required_bytes, short_output.size());
    EXPECT_EQ(short_result.bytes_written, 0u);

    std::vector<std::byte> artifact(short_result.required_bytes);
    rt::ArtifactWriteResult result;
    ASSERT_EQ(
        runtime.write_active_replay_artifact(
            checkpoint, inputs, artifact, result),
        rt::Status::ok);
    artifact.resize(result.bytes_written);

    auto mutated = artifact;
    mutated[mutated.size() / 2] ^= std::byte{1};
    rt::ActiveReplayMetadata metadata;
    EXPECT_EQ(
        rt::inspect_active_replay_artifact(mutated, metadata),
        rt::Status::invalid_artifact);
    std::uint64_t state_before_rejection = 0;
    ASSERT_TRUE(load_u64(state, 0, state_before_rejection));
    InputProbe rejection_probe;
    rt::ActiveReplayResult rejection_result;
    EXPECT_EQ(
        runtime.replay_active(
            mutated,
            &apply_input,
            &rejection_probe,
            &rejection_result),
        rt::Status::invalid_artifact);
    std::uint64_t state_after_rejection = 0;
    ASSERT_TRUE(load_u64(state, 0, state_after_rejection));
    EXPECT_EQ(state_after_rejection, state_before_rejection);
    EXPECT_EQ(rejection_probe.calls, 0u);

    auto trailing = artifact;
    trailing.push_back(std::byte{0});
    EXPECT_EQ(
        rt::inspect_active_replay_artifact(trailing, metadata),
        rt::Status::invalid_artifact);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}
