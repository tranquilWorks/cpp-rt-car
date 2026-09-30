#pragma once
#include <rt/runtime.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace replay_rejection {
inline void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
struct Clock final : rt::RuntimeClock {
    std::atomic<std::uint64_t> now{1000};
    std::uint64_t now_ns() noexcept override { return now.load(); }
};
struct Scenario {
    Clock clock;
    std::array<std::byte, 8> state{};
    std::size_t calls = 0, input_calls = 0;
    bool active;
    rt::Runtime runtime{clock};
    explicit Scenario(bool active_replay, bool lossless) : active(active_replay) {
        rt::RuntimeConfig config;
        config.callback_capacity = 2;
        config.worker_count = 1;
        config.executor_queue_capacity = 8;
        config.task_scratch_slots = 8;
        config.trace_capacity = 0;
        check(runtime.configure(config), "configure");
        if (active) {
            check(runtime.set_rate_execution_policy({16, 1, 1, 1, 64}), "rate policy");
            check(runtime.set_mixed_rate_closure_policy(
                {1, 256, 256, 131072, 16,
                 rt::MixedRateOverflowPolicy::overwrite_committed, true, true, {}}),
                "mixed closure");
        }
        rt::LiveControlPolicy policy;
        policy.policy_identity = 1;
        policy.mailbox_capacity = 1;
        policy.producer_capacity = 1;
        policy.record_capacity = 8;
        policy.payload_bytes_per_record = 8;
        policy.total_payload_storage_bytes = 64;
        check(runtime.set_live_control_policy(policy), "mailbox policy");
        rt::LiveControlMailboxRegistration mailbox;
        mailbox.mailbox_identity = 1;
        mailbox.record_capacity = 8;
        mailbox.payload_bytes_per_record = 8;
        check(runtime.register_live_control_mailbox(mailbox), "mailbox");
        rt::LiveControlProducerRegistration producer;
        producer.mailbox_identity = 1;
        producer.producer_identity = 1;
        check(runtime.register_live_control_producer(producer), "producer");
        rt::LiveControlClosurePolicy closure;
        closure.policy_identity = 1;
        closure.action_capacity = 256;
        closure.retained_generation_capacity = 16;
        closure.retained_record_capacity = 32;
        closure.retained_payload_bytes = 256;
        closure.replay_record_capacity = 256;
        closure.replay_max_bytes = 131072;
        closure.replay_enabled = true;
        check(runtime.set_live_control_closure_policy(closure), "live closure");
        if (lossless) {
            rt::LiveControlReplayRetentionPolicy retention;
            retention.policy_identity = 77;
            retention.admission_capacity = 128;
            retention.payload_capacity_bytes = 1024;
            check(runtime.set_live_control_replay_retention_policy(retention), "v2 retention");
        }
        rt::PhaseHandle phase;
        check(runtime.register_callback({"work", callback, this}, phase), "callback");
        if (active) {
            rt::RateDomainHandle domain, anchor;
            check(runtime.register_rate_domain({"rate", 100, 1, 100, 1}, domain), "rate");
            check(runtime.bind_phase_to_rate_domain(phase, domain), "binding");
            check(runtime.register_rate_domain({"anchor", 400, 1, 400, 1}, anchor), "anchor");
            rt::PhaseHandle anchor_phase;
            check(runtime.register_callback({"anchor-work", [](void*, const rt::CallbackContext&) {
                return rt::CallbackResult::ok;
            }, nullptr}, anchor_phase), "anchor callback");
            check(runtime.bind_phase_to_rate_domain(anchor_phase, anchor), "anchor binding");
        }
        check(runtime.register_state({"state", 1, state}), "registered state");
        check(runtime.finalize(), "finalize");
        check(runtime.start(), "start");
        rt::ObservabilityMetadata graph_owner;
        rt::LiveControlActionMetadata control_owner;
        check(runtime.observability_metadata(graph_owner), "graph owner");
        check(runtime.live_control_action_metadata(control_owner), "control owner");
        require(graph_owner.runtime_id != control_owner.runtime_id,
                "fixture must exercise independent owner namespaces");
    }
    ~Scenario() {
        if (runtime.state() == rt::RuntimeState::running && runtime.stop() != rt::Status::ok)
            std::terminate();
    }
    void check(rt::Status status, const char* operation) {
        if (status != rt::Status::ok)
            throw std::runtime_error((std::string(operation) + ": ").append(runtime.last_error()));
    }
    static rt::CallbackResult callback(void* opaque, const rt::CallbackContext& ctx) {
        auto& self = *static_cast<Scenario*>(opaque);
        if (!ctx.live_control || ctx.live_control->records.size() != 1 ||
            ctx.live_control->records[0].payload.size() != 8 ||
            ctx.live_control->records[0].payload[0] != static_cast<std::byte>(ctx.frame.frame_index))
            return rt::CallbackResult::error;
        ++self.calls;
        self.state[0] = static_cast<std::byte>(std::to_integer<unsigned>(self.state[0]) + ctx.frame.frame_index);
        return rt::CallbackResult::ok;
    }
    static rt::CallbackResult input(void* opaque, const rt::ReplayInputView&) {
        ++static_cast<Scenario*>(opaque)->input_calls;
        return rt::CallbackResult::ok;
    }
    static rt::ReplayInputRecord input_record(std::uint64_t frame) {
        return {{frame, std::chrono::nanoseconds{100}, std::nullopt, 1000 + (frame - 1) * 100}, 1, {}};
    }
    void step(std::uint64_t frame) {
        rt::LiveControlProducerHandle handle;
        check(runtime.live_control_producer_handle(1, 1, handle), "producer handle");
        std::array<std::byte, 8> payload{};
        payload[0] = static_cast<std::byte>(frame);
        rt::LiveControlUpdateRecord update;
        update.runtime_id = handle.runtime_id;
        update.configuration_generation = handle.configuration_generation;
        update.mailbox_identity = handle.mailbox_identity;
        update.producer_identity = handle.producer_identity;
        update.producer_sequence = frame;
        update.target_frame_index = frame;
        update.payload_bytes = 8;
        update.payload_digest = rt::live_control_payload_digest(payload);
        rt::LiveControlAdmissionResult admitted;
        check(runtime.stage_live_control_update(handle, update, payload, admitted), "stage");
        require(admitted == rt::LiveControlAdmissionResult::accepted, "admission not accepted");
        check(runtime.step(input_record(frame).frame), "step");
    }
    std::vector<std::byte> checkpoint(std::uint64_t frame) {
        std::size_t bytes = 0;
        check(runtime.checkpoint_size(bytes), "checkpoint size");
        std::vector<std::byte> result(bytes);
        rt::ArtifactWriteResult written;
        check(runtime.write_checkpoint(frame, result, written), "checkpoint");
        require(written.bytes_written == result.size(), "checkpoint size mismatch");
        return result;
    }
    std::vector<std::byte> capture(std::span<const std::byte> initial, std::uint64_t first, std::uint64_t last) {
        std::vector<rt::ReplayInputRecord> inputs;
        for (auto frame = first; frame <= last; ++frame) inputs.push_back(input_record(frame));
        std::vector<std::byte> nested(131072), result(131072);
        rt::ArtifactWriteResult written;
        if (active) check(runtime.write_active_replay_artifact(initial, inputs, nested, written), "active capture");
        else check(runtime.write_input_log(inputs, nested, written), "input capture");
        nested.resize(written.bytes_written);
        check(runtime.write_live_control_replay_artifact(initial, nested,
            active ? rt::LiveControlNestedArtifactKind::active_replay : rt::LiveControlNestedArtifactKind::input_log,
            result, written), "trusted capture");
        result.resize(written.bytes_written);
        return result;
    }
};
// Deliberately adversarial test mutation: preserve valid bytes/checksums while
// changing only the outer owner. Production code never rewrites identities.
inline void forge_outer_owner(std::vector<std::byte>& artifact, std::uint64_t owner) {
    require(artifact.size() >= 400, "trusted header extent");
    for (std::size_t i = 0; i < 8; ++i) artifact[32+i] = static_cast<std::byte>(owner >> (8*i));
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < artifact.size(); ++i) {
        hash ^= (i >= 24 && i < 32) ? 0 : std::to_integer<std::uint8_t>(artifact[i]);
        hash *= 1099511628211ull;
    }
    for (std::size_t i = 0; i < 8; ++i) artifact[24+i] = static_cast<std::byte>(hash >> (8*i));
    rt::LiveControlReplayMetadata metadata;
    require(rt::inspect_live_control_replay_artifact(artifact, metadata) == rt::Status::ok,
            "adversarial owner mutation must remain structurally valid");
}
inline auto mailbox_fields(const rt::LiveControlMailboxInfo& v) {
    return std::tie(v.schema_version,v.struct_size,v.runtime_id,v.configuration_generation,v.policy_identity,
        v.mailbox_identity,v.next_mailbox_sequence,v.accepted,v.invalid,v.full,v.busy,v.stale,v.stopped,
        v.exhausted,v.record_capacity,v.payload_bytes_per_record,v.occupancy,v.producer_count,v.admission_open,v.reserved);
}
inline auto action_fields(const rt::LiveControlActionMetadata& v) {
    return std::tie(v.schema_version,v.record_size,v.runtime_id,v.configuration_generation,v.policy_identity,
        v.capacity,v.next_sequence,v.records_emitted,v.records_overwritten,v.records_dropped,v.retained_generation_count,
        v.replay_eligible,v.reserved);
}
}
