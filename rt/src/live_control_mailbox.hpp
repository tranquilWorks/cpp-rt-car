#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <rt/runtime.hpp>

namespace rt::detail {

struct LiveControlReplayArtifactView;

struct LiveControlStorageExtent {
    const void* data = nullptr;
    std::size_t bytes = 0;
};

class LiveControlMailboxSet final {
public:
    struct Impl;

    static Status create(const LiveControlPolicy& policy,
                         const LiveControlClosurePolicy& closure_policy,
                         bool closure_enabled,
                         const LiveControlReplayRetentionPolicy& retention_policy,
                         std::uint64_t runtime_id,
                         std::uint64_t configuration_generation,
                         std::size_t memory_budget_bytes,
                         std::span<const LiveControlMailboxRegistration> mailboxes,
                         std::span<const LiveControlProducerRegistration> producers,
                         std::span<const ReferenceRelease> rate_releases,
                         std::unique_ptr<LiveControlMailboxSet>& output,
                         const char*& diagnostic) noexcept;

    ~LiveControlMailboxSet();
    LiveControlMailboxSet(const LiveControlMailboxSet&) = delete;
    LiveControlMailboxSet& operator=(const LiveControlMailboxSet&) = delete;

    [[nodiscard]] Status producer_handle(
        std::uint64_t mailbox_identity,
        std::uint64_t producer_identity,
        LiveControlProducerHandle& handle) const noexcept;
    [[nodiscard]] LiveControlAdmissionResult stage(
        LiveControlProducerHandle producer,
        const LiveControlUpdateRecord& update,
        std::span<const std::byte> payload) noexcept;
    void record_admission(
        LiveControlProducerHandle producer,
        const LiveControlUpdateRecord& update,
        LiveControlAdmissionResult result) noexcept;
    [[nodiscard]] bool mailbox_info(
        std::uint64_t mailbox_identity,
        LiveControlMailboxInfo& info) const noexcept;
    [[nodiscard]] bool record_at(
        std::uint64_t mailbox_identity,
        std::uint64_t mailbox_sequence,
        LiveControlUpdateRecord& record) const noexcept;
    [[nodiscard]] Status copy_payload(
        std::uint64_t mailbox_identity,
        std::uint64_t mailbox_sequence,
        std::span<std::byte> output) const noexcept;
    [[nodiscard]] const LiveControlGenerationView* close_host_frame(
        std::uint64_t frame_index) noexcept;
    [[nodiscard]] const LiveControlGenerationView* close_rate_release(
        std::size_t reference_release_index,
        std::uint64_t release_sequence) noexcept;
    [[nodiscard]] const LiveControlGenerationView*
    active_generation_view() const noexcept;
    [[nodiscard]] bool begin_step_transaction() noexcept;
    void settle_step_transaction(Status status) noexcept;
    [[nodiscard]] bool transaction_active() const noexcept;
    void expire_rate_releases_before(std::uint64_t logical_time_ns) noexcept;
    [[nodiscard]] bool commit_info(
        LiveControlCommitInfo& info) const noexcept;
    [[nodiscard]] bool record_status(
        std::uint64_t mailbox_identity,
        std::uint64_t mailbox_sequence,
        LiveControlRecordStatusInfo& info) const noexcept;
    [[nodiscard]] bool action_metadata(
        LiveControlActionMetadata& metadata) const noexcept;
    [[nodiscard]] Status read_actions(
        LiveControlActionCursor& cursor,
        std::span<LiveControlActionRecord> output,
        LiveControlActionReadResult& result) const noexcept;
    [[nodiscard]] std::size_t checkpoint_state_size() const noexcept;
    [[nodiscard]] bool write_checkpoint_state(
        std::span<std::byte> output) const noexcept;
    [[nodiscard]] bool sync_checkpoint_state(
        std::span<const std::byte>& output) noexcept;
    [[nodiscard]] bool validate_checkpoint_state(
        std::span<const std::byte> input) const noexcept;
    [[nodiscard]] bool restore_checkpoint_state(
        std::span<const std::byte> input) noexcept;
    void record_checkpoint(std::uint64_t correlation) noexcept;
    void record_replay_verified(std::uint64_t correlation) noexcept;
    [[nodiscard]] bool checkpoint_action_sequence(
        std::span<const std::byte> state,
        std::uint64_t& sequence) const noexcept;
    [[nodiscard]] Status write_replay_artifact(
        LiveControlReplayMetadata metadata,
        std::span<const std::byte> checkpoint,
        std::span<const std::byte> nested_artifact,
        std::uint64_t first_action_sequence,
        std::span<std::byte> output,
        ArtifactWriteResult& result) noexcept;
    [[nodiscard]] bool validate_replay_artifact(
        const LiveControlReplayArtifactView& view) const noexcept;
    [[nodiscard]] bool begin_replay(
        const LiveControlReplayArtifactView& view) noexcept;
    void apply_replay_history() noexcept;
    void end_replay() noexcept;
    [[nodiscard]] std::size_t replay_generations_compared() const noexcept;
    [[nodiscard]] Status replay_mismatch_status() const noexcept;
    [[nodiscard]] std::uint64_t replay_mismatch_action_sequence()
        const noexcept;
    [[nodiscard]] bool claim_all() noexcept;
    void release_all() noexcept;
    [[nodiscard]] bool host_claimed() const noexcept;

    void close_admission() noexcept;
    void terminalize_staged_on_stop() noexcept;
    [[nodiscard]] bool has_active_reservation() const noexcept;

    [[nodiscard]] std::size_t mailbox_count() const noexcept;
    [[nodiscard]] std::size_t producer_count() const noexcept;
    [[nodiscard]] std::size_t record_capacity() const noexcept;
    [[nodiscard]] std::size_t payload_storage_bytes() const noexcept;
    [[nodiscard]] std::size_t control_bytes() const noexcept;
    [[nodiscard]] std::size_t action_capacity() const noexcept;
    [[nodiscard]] std::size_t action_storage_bytes() const noexcept;
    [[nodiscard]] std::size_t retained_generation_capacity() const noexcept;
    [[nodiscard]] std::size_t retained_record_capacity() const noexcept;
    [[nodiscard]] std::size_t retained_payload_storage_bytes() const noexcept;
    [[nodiscard]] std::size_t closure_control_bytes() const noexcept;
    [[nodiscard]] std::size_t extent_count() const noexcept;
    [[nodiscard]] bool extent_at(
        std::size_t index,
        LiveControlStorageExtent& extent) const noexcept;
    [[nodiscard]] std::uint64_t runtime_id() const noexcept;
    [[nodiscard]] std::uint64_t configuration_generation() const noexcept;
    [[nodiscard]] bool claim_for_test(
        std::uint64_t mailbox_identity) noexcept;
    void release_for_test(std::uint64_t mailbox_identity) noexcept;

private:
  void retain_admission(LiveControlProducerHandle handle,
                        const LiveControlUpdateRecord& update,
                        std::span<const std::byte> payload,
                        LiveControlAdmissionResult result, std::size_t mailbox_index,
                        std::uint32_t slot_index,
                        std::uint64_t action_sequence) noexcept;
  explicit LiveControlMailboxSet(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

struct RuntimeLiveControlTestAccess {
    [[nodiscard]] static bool claim(
        Runtime& runtime,
        std::uint64_t mailbox_identity) noexcept;
    static void release(
        Runtime& runtime,
        std::uint64_t mailbox_identity) noexcept;
};

} // namespace rt::detail
