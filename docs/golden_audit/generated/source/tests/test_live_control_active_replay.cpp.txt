#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

#include <rt/runtime.hpp>

namespace {

struct ReplayClock final : rt::RuntimeClock {
    std::atomic<std::uint64_t> now{1'000};
    std::uint64_t now_ns() noexcept override { return now.load(); }
};

struct ReplayWork {
    std::array<std::byte, 8> state{};
    std::array<std::uint64_t, 8> generations{};
    std::size_t calls = 0;
    std::size_t mailboxes = 1;
    bool corrupt_state = false;

    static rt::CallbackResult callback(
        void* opaque, const rt::CallbackContext& context) {
        auto& work = *static_cast<ReplayWork*>(opaque);
        if (!context.live_control ||
            context.live_control->records.size() != work.mailboxes ||
            work.calls == work.generations.size()) {
            return rt::CallbackResult::error;
        }
        for (const auto& record : context.live_control->records) {
            if (record.payload.size() != 8 || record.payload[0] !=
                    static_cast<std::byte>(context.frame.frame_index)) {
                return rt::CallbackResult::error;
            }
        }
        work.state[0] = static_cast<std::byte>(
            std::to_integer<unsigned>(work.state[0]) +
            static_cast<unsigned>(context.frame.frame_index) +
            (work.corrupt_state ? 1u : 0u));
        work.generations[work.calls++] =
            context.live_control->generation_identity;
        return rt::CallbackResult::ok;
    }
};

rt::CallbackResult no_input(void*, const rt::ReplayInputView&) {
    return rt::CallbackResult::ok;
}

// Own callback storage and the clock until checked stop, including on a failed
// assertion. No worker can outlive either borrowed object.
struct Scenario {
    ReplayClock clock;
    ReplayWork work;
    rt::Runtime runtime{clock};
    ~Scenario() {
        if (runtime.state() == rt::RuntimeState::running &&
            runtime.stop() != rt::Status::ok) {
            std::terminate();
        }
    }
};

void check_round_trip(std::uint32_t frames, std::uint32_t capacity,
                      std::uint32_t mailboxes, bool rate_target,
                      bool corrupt_application = false, bool lossless = false,
                      bool replaced = false, bool rejected = false) {
    Scenario scenario;
    auto& runtime = scenario.runtime;
    auto& work = scenario.work;
    work.mailboxes = mailboxes;
    rt::RuntimeConfig config;
    config.callback_capacity = 2;
    config.worker_count = 1;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    config.trace_capacity = 0;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(runtime.set_rate_execution_policy({16, 1, 1, 1, 64}),
              rt::Status::ok);
    ASSERT_EQ(runtime.set_mixed_rate_closure_policy(
                  {1, 256, 256, 131072, 16,
                   rt::MixedRateOverflowPolicy::overwrite_committed,
                   true, true, {}}), rt::Status::ok);
    rt::LiveControlPolicy policy;
    policy.policy_identity = 1;
    policy.mailbox_capacity = mailboxes;
    policy.producer_capacity = mailboxes;
    policy.record_capacity = capacity * mailboxes;
    policy.payload_bytes_per_record = 8;
    policy.total_payload_storage_bytes = policy.record_capacity * 8;
    ASSERT_EQ(runtime.set_live_control_policy(policy), rt::Status::ok);
    for (std::uint32_t index = 0; index < mailboxes; ++index) {
        rt::LiveControlMailboxRegistration mailbox;
        mailbox.mailbox_identity = index + 1;
        mailbox.record_capacity = capacity;
        mailbox.payload_bytes_per_record = 8;
        ASSERT_EQ(runtime.register_live_control_mailbox(mailbox), rt::Status::ok);
        rt::LiveControlProducerRegistration producer;
        producer.mailbox_identity = index + 1;
        producer.producer_identity = index + 1;
        ASSERT_EQ(runtime.register_live_control_producer(producer), rt::Status::ok);
    }
    rt::LiveControlClosurePolicy closure;
    closure.policy_identity = 1;
    closure.action_capacity = 256;
    closure.retained_generation_capacity = 16;
    closure.retained_record_capacity = 32;
    closure.retained_payload_bytes = 256;
    closure.replay_record_capacity = 256;
    closure.replay_max_bytes = 131072;
    closure.replay_enabled = true;
    ASSERT_EQ(runtime.set_live_control_closure_policy(closure), rt::Status::ok);
    if (lossless) {
        rt::LiveControlReplayRetentionPolicy retention;
        retention.policy_identity = 77;
        retention.admission_capacity = 128;
        retention.payload_capacity_bytes = 1024;
        ASSERT_EQ(runtime.set_live_control_replay_retention_policy(retention),
                  rt::Status::ok);
    }
    rt::PhaseHandle phase;
    rt::RateDomainHandle domain;
    ASSERT_EQ(runtime.register_callback({"work", ReplayWork::callback, &work},
                                        phase), rt::Status::ok);
    ASSERT_EQ(runtime.register_rate_domain({"rate", 100, 1, 100, 1}, domain),
              rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(phase, domain), rt::Status::ok);
    // Extend the reference supercycle so each tested rate target is explicit.
    rt::RateDomainHandle anchor;
    ASSERT_EQ(runtime.register_rate_domain(
                  {"anchor", frames * 100u, 1, frames * 100u, 1}, anchor),
              rt::Status::ok);
    rt::PhaseHandle anchor_phase;
    ASSERT_EQ(runtime.register_callback(
                  {"anchor-work", [](void*, const rt::CallbackContext&) {
                      return rt::CallbackResult::ok;
                  }, nullptr}, anchor_phase), rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(anchor_phase, anchor), rt::Status::ok);
    ASSERT_EQ(runtime.register_state({"state", 1, work.state}), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    std::size_t size = 0;
    ASSERT_EQ(runtime.checkpoint_size(size), rt::Status::ok);
    std::vector<std::byte> checkpoint(size);
    rt::ArtifactWriteResult written;
    ASSERT_EQ(runtime.write_checkpoint(0, checkpoint, written), rt::Status::ok);
    std::array<rt::LiveControlProducerHandle, 2> handles{};
    for (std::uint32_t index = 0; index < mailboxes; ++index) {
        ASSERT_EQ(runtime.live_control_producer_handle(index + 1, index + 1,
                                                       handles[index]),
                  rt::Status::ok);
    }
    std::vector<rt::ReplayInputRecord> inputs(frames);
    for (std::uint32_t frame = 1; frame <= frames; ++frame) {
        std::array<std::byte, 8> payload{};
        payload[0] = static_cast<std::byte>(frame);
        for (std::uint32_t index = 0; index < mailboxes; ++index) {
            const auto& handle = handles[index];
            rt::LiveControlUpdateRecord update;
            update.runtime_id = handle.runtime_id;
            update.configuration_generation = handle.configuration_generation;
            update.mailbox_identity = handle.mailbox_identity;
            update.producer_identity = handle.producer_identity;
            update.producer_sequence = frame;
            update.target_frame_index = frame;
            update.payload_bytes = 8;
            update.payload_digest = rt::live_control_payload_digest(payload);
            if (rate_target) {
                rt::ReferenceRelease release;
                const auto reference = frame == 1 ? 0u : frame;
                ASSERT_TRUE(runtime.reference_release_at(reference, release));
                update.target_kind = rt::LiveControlTargetKind::rate_release;
                update.target_frame_index = std::numeric_limits<std::uint64_t>::max();
                update.reference_release_index = reference;
                update.rate_domain_registration_index =
                    static_cast<std::uint32_t>(release.domain_registration_index);
                update.phase_index = release.phase.index();
                update.rate_substep_ordinal = release.substep_ordinal;
                update.rate_release_sequence = release.domain_release_sequence;
            }
            rt::LiveControlAdmissionResult admission;
            if (replaced) {
                payload[0] = static_cast<std::byte>(frame + 100);
                update.producer_sequence = 2 * frame - 1;
                update.payload_digest = rt::live_control_payload_digest(payload);
                ASSERT_EQ(runtime.stage_live_control_update(handle, update, payload,
                                                            admission),
                          rt::Status::ok);
                ASSERT_EQ(admission, rt::LiveControlAdmissionResult::accepted);
                payload[0] = static_cast<std::byte>(frame);
                update.producer_sequence = 2 * frame;
                update.payload_digest = rt::live_control_payload_digest(payload);
            }
            if (rejected) {
                update.payload_digest ^= 1;
                ASSERT_EQ(runtime.stage_live_control_update(handle, update, payload,
                                                            admission),
                          rt::Status::ok);
                ASSERT_EQ(admission, rt::LiveControlAdmissionResult::invalid);
                update.payload_digest ^= 1;
            }
            ASSERT_EQ(runtime.stage_live_control_update(handle, update, payload,
                                                         admission), rt::Status::ok);
            ASSERT_EQ(admission, rt::LiveControlAdmissionResult::accepted);
        }
        inputs[frame - 1] = {{frame, std::chrono::nanoseconds{100},
                              std::nullopt, 1'000 + (frame - 1) * 100u}, 1, {}};
        ASSERT_EQ(runtime.step(inputs[frame - 1].frame), rt::Status::ok);
    }
    const auto expected_state = static_cast<std::byte>(frames * (frames + 1) / 2);
    ASSERT_EQ(work.state[0], expected_state);
    const auto expected_generations = work.generations;
    std::vector<std::byte> active(131072), artifact(131072);
    ASSERT_EQ(runtime.write_active_replay_artifact(checkpoint, inputs, active,
                                                   written), rt::Status::ok);
    active.resize(written.bytes_written);
    ASSERT_EQ(runtime.write_live_control_replay_artifact(
                  checkpoint, active, rt::LiveControlNestedArtifactKind::active_replay,
                  artifact, written), rt::Status::ok);
    artifact.resize(written.bytes_written);
    rt::LiveControlReplayMetadata metadata;
    ASSERT_EQ(rt::inspect_live_control_replay_artifact(artifact, metadata), rt::Status::ok);

    // Bad extents and checksums must be rejected before any callback or restore.
    auto corrupt = artifact;
    corrupt.back() ^= std::byte{1};
    for (auto bytes : {std::span<const std::byte>(corrupt),
                       std::span<const std::byte>(artifact).first(artifact.size() - 1)}) {
        EXPECT_NE(runtime.replay_live_control(bytes, no_input, nullptr), rt::Status::ok);
        EXPECT_EQ(work.state[0], expected_state);
        EXPECT_EQ(work.calls, frames);
    }
    if (!lossless && (replaced || rejected)) {
        EXPECT_EQ(runtime.replay_live_control(artifact, no_input, nullptr),
                  rt::Status::incompatible_artifact);
        EXPECT_EQ(work.state[0], expected_state);
        EXPECT_EQ(work.calls, frames);
        return;
    }
    EXPECT_EQ(metadata.schema_version, lossless ? 2u : 1u);
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        work.calls = 0;
        work.generations = {};
        work.corrupt_state = corrupt_application;
        rt::LiveControlReplayResult result;
        const auto status = runtime.replay_live_control(artifact, no_input, nullptr,
                                                        &result);
        if (corrupt_application) {
            EXPECT_NE(status, rt::Status::ok);
            break;
        }
        ASSERT_EQ(status, rt::Status::ok) << runtime.last_error();
        EXPECT_EQ(work.state[0], expected_state);
        EXPECT_EQ(work.calls, frames);
        EXPECT_EQ(work.generations, expected_generations);
        EXPECT_EQ(result.frames_replayed, frames);
        EXPECT_EQ(result.generations_compared, frames);
        EXPECT_EQ(result.actions_compared, metadata.action_record_count);
        rt::LiveControlCommitInfo commit;
        ASSERT_TRUE(runtime.live_control_commit_info(commit));
        EXPECT_EQ(commit.committed, frames * mailboxes);
        EXPECT_EQ(commit.staged_occupancy, 0u);
        for (std::uint32_t index = 0; index < mailboxes; ++index) {
            rt::LiveControlMailboxInfo info;
            ASSERT_TRUE(runtime.live_control_mailbox_info(index + 1, info));
            const auto admissions = frames * (replaced ? 2u : 1u);
            EXPECT_EQ(info.accepted, admissions);
            EXPECT_EQ(info.invalid, rejected ? frames : 0u);
            EXPECT_EQ(info.next_mailbox_sequence, admissions + 1);
            EXPECT_EQ(info.occupancy, 0u);
            // Latest records retain their status and copied bytes after replay.
            rt::LiveControlRecordStatusInfo record;
            ASSERT_TRUE(
                runtime.live_control_record_status(index + 1, admissions, record));
            EXPECT_EQ(record.status, rt::LiveControlRecordStatus::committed);
            std::array<std::byte, 8> payload{};
            ASSERT_EQ(runtime.copy_live_control_payload(index + 1, admissions, payload),
                      rt::Status::ok);
            EXPECT_EQ(payload[0], static_cast<std::byte>(frames));
        }
    }
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

} // namespace

TEST(LiveControlActiveReplay, HostTwoAndEightFramesPreserveCanonicalState) {
    check_round_trip(2, 8, 1, false);
    check_round_trip(8, 8, 1, false);
}

TEST(LiveControlActiveReplay, ReclaimsOnlyAfterFreeSlotsAreExhausted) {
    check_round_trip(8, 2, 1, false);
}

TEST(LiveControlActiveReplay, MultipleMailboxesAndCompiledRateTargets) {
    check_round_trip(2, 8, 2, true);
    check_round_trip(8, 8, 2, true);
}

TEST(LiveControlActiveReplay, FinalApplicationStateDivergenceStillFails) {
    check_round_trip(2, 8, 1, false, true);
}

TEST(LiveControlActiveReplay, ConcurrentRuntimeInstancesAreIsolated) {
    std::thread first([] { check_round_trip(8, 8, 1, false); });
    std::thread second([] { check_round_trip(8, 2, 2, true); });
    first.join();
    second.join();
}

TEST(LiveControlActiveReplay, LosslessReplacementsAndRejectionsPreserveAllState) {
    for (auto frames : {2u, 8u}) {
        for (bool rate : {false, true}) {
            check_round_trip(frames, 2, 2, rate, false, true, true, true);
        }
    }
}

TEST(LiveControlActiveReplay, LosslessApplicationDivergenceStillFails) {
    check_round_trip(2, 4, 2, false, true, true, true, true);
}

TEST(LiveControlActiveReplay, IncompleteV1HistoriesRejectBeforeRestore) {
    check_round_trip(2, 4, 1, false, false, false, true, false);
    check_round_trip(2, 4, 1, true, false, false, false, true);
}

TEST(LiveControlActiveReplay, ConcurrentLosslessInstancesRemainIsolated) {
    std::thread first(
        [] { check_round_trip(8, 2, 2, false, false, true, true, true); });
    std::thread second(
        [] { check_round_trip(8, 4, 1, true, false, true, true, true); });
    first.join();
    second.join();
}
