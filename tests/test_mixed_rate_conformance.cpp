#include <gtest/gtest.h>

#include "mixed_rate_conformance.hpp"

TEST(MixedRateConformance, ThreeRatePublicFixtureIsOrderedAndExact) {
    const auto result = rtfw_test::run_mixed_rate_conformance();
    EXPECT_EQ(result.status, rt::Status::ok)
        << result.failure_stage << ": " << result.diagnostic.data()
        << ", actions=" << result.action_count
        << ", loopback=" << result.loopback_logical_actions
        << ", terminal_phase=" << result.last_terminal_phase
        << ", terminal_status=" << result.last_terminal_status
        << ", backend_status=" << result.last_backend_status
        << ", mismatch=" << result.replay_mismatch_sequence
        << ", compared=" << result.replay_actions_compared
        << ", action=" << static_cast<unsigned>(result.replay_expected_action)
        << "/" << static_cast<unsigned>(result.replay_actual_action)
        << ", batch=" << result.replay_expected_batch
        << "/" << result.replay_actual_batch
        << ", timestamp=" << result.replay_expected_timestamp
        << "/" << result.replay_actual_timestamp
        << ", payload=" << result.replay_expected_payload
        << "/" << result.replay_actual_payload;
    EXPECT_EQ(result.callback_counts[0], 9u);
    EXPECT_EQ(result.callback_counts[1], 6u);
    EXPECT_EQ(result.callback_counts[2], 4u);
    EXPECT_EQ(result.callback_counts[3], 4u);
    EXPECT_EQ(result.callback_counts[4], 2u);
    EXPECT_GT(result.action_count, 23u);
    EXPECT_EQ(result.device_terminal_actions, 10u);
    EXPECT_EQ(result.sampled_publish_actions, 23u);
    EXPECT_EQ(result.sampled_select_actions, 16u);
    EXPECT_EQ(result.safe_transition_actions, 2u);
    EXPECT_EQ(result.first_logical_release_ns, 0u);
    EXPECT_EQ(result.last_logical_release_ns, 800'000'000u);
    EXPECT_TRUE(result.startup_safe_acknowledged);
    EXPECT_TRUE(result.shutdown_safe_acknowledged);
    EXPECT_TRUE(result.active_replay_exact);
    EXPECT_GT(result.replay_actions_compared, 0u);
    EXPECT_GE(result.loopback_logical_actions, 12u);
    EXPECT_TRUE(result.memory_accounting_exact);
}

TEST(MixedRateConformance, TwoInstancesShareNoMutableState) {
    const auto first = rtfw_test::run_mixed_rate_conformance();
    const auto second = rtfw_test::run_mixed_rate_conformance();
    EXPECT_EQ(first.status, rt::Status::ok);
    EXPECT_EQ(second.status, rt::Status::ok);
    EXPECT_EQ(first.callback_counts, second.callback_counts);
    EXPECT_EQ(first.action_count, second.action_count);
}

TEST(MixedRateConformance, FaultMatrixReplaysTerminalClosureAndSafeFailure) {
    struct FaultCase {
        rt::SampledIoLoopbackFault fault;
        rt::Status status;
        rt::MixedRateActionReason reason;
        rt::MixedRateActionStage stage;
    };
    const std::array cases{
        FaultCase{
            rt::SampledIoLoopbackFault::completion_error,
            rt::Status::device_error,
            rt::MixedRateActionReason::completion_error,
            rt::MixedRateActionStage::terminal,
        },
        FaultCase{
            rt::SampledIoLoopbackFault::completion_timeout,
            rt::Status::device_timeout,
            rt::MixedRateActionReason::timeout,
            rt::MixedRateActionStage::quarantined,
        },
        FaultCase{
            rt::SampledIoLoopbackFault::completion_lost,
            rt::Status::device_lost,
            rt::MixedRateActionReason::lost,
            rt::MixedRateActionStage::quarantined,
        },
    };
    for (const auto& test : cases) {
        const auto result =
            rtfw_test::run_mixed_rate_conformance(test.fault);
        EXPECT_EQ(result.status, rt::Status::ok)
            << "fault=" << static_cast<unsigned>(test.fault)
            << ", stage=" << result.failure_stage
            << ", diagnostic=" << result.diagnostic.data()
            << ", terminal=" << result.last_terminal_status
            << ", compared=" << result.replay_actions_compared
            << ", mismatch=" << result.replay_mismatch_sequence
            << ", action="
            << static_cast<unsigned>(result.replay_expected_action)
            << "/"
            << static_cast<unsigned>(result.replay_actual_action)
            << ", batch=" << result.replay_expected_batch
            << "/" << result.replay_actual_batch
            << ", timestamp=" << result.replay_expected_timestamp
            << "/" << result.replay_actual_timestamp
            << ", payload=" << result.replay_expected_payload
            << "/" << result.replay_actual_payload
            << ", byte=" << result.replay_first_different_byte;
        EXPECT_EQ(
            result.last_terminal_status,
            static_cast<std::int32_t>(test.status));
        EXPECT_EQ(result.last_terminal_reason, test.reason);
        EXPECT_EQ(result.last_terminal_stage, test.stage);
        EXPECT_EQ(result.device_terminal_actions, 1u);
        EXPECT_TRUE(result.failure_safe_acknowledged);
        EXPECT_TRUE(result.active_replay_exact);
        EXPECT_GT(result.replay_actions_compared, 0u);
        EXPECT_GE(result.loopback_logical_actions, 6u);
    }
}

TEST(MixedRateConformance, MissingStartupAcknowledgementHonorsOriginalTimeout) {
    const auto result = rtfw_test::run_mixed_rate_conformance(
        rt::SampledIoLoopbackFault::none,
        80'000'000,
        rt::SampledIoLoopbackFault::completion_timeout);
    EXPECT_EQ(result.status, rt::Status::device_timeout);
    EXPECT_EQ(result.failure_stage, 4u);
    for (const auto count : result.callback_counts) EXPECT_EQ(count, 0u);
    EXPECT_EQ(result.action_count, 0u);
    EXPECT_EQ(result.loopback_logical_actions, 1u);
    EXPECT_FALSE(result.startup_safe_acknowledged);
    EXPECT_EQ(result.startup_cleanup_status, rt::Status::ok);
}
