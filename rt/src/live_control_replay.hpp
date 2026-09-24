#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include <rt/runtime.hpp>

namespace rt::detail {

inline constexpr std::size_t live_control_replay_header_size = 384;
inline constexpr std::size_t live_control_lossless_replay_header_size = 448;
inline constexpr std::size_t live_control_replay_admission_bytes = 184;
inline constexpr std::size_t live_control_replay_action_bytes =
    sizeof(LiveControlActionRecord) + sizeof(std::uint64_t);
inline constexpr std::size_t live_control_replay_generation_bytes = 128;
inline constexpr std::size_t live_control_replay_record_bytes = 152;

struct LiveControlRetainedGenerationView {
    LiveControlBoundaryTarget target{};
    std::uint64_t generation_identity = 0;
    std::uint64_t prior_generation_identity = 0;
    std::uint64_t first_action_sequence = 0;
    std::size_t record_count = 0;
    std::size_t payload_bytes = 0;
    Status terminal_status = Status::ok;
    bool settled = false;
};

struct LiveControlRetainedAdmission {
    LiveControlUpdateRecord record{};
    std::uint64_t action_sequence = 0;
    std::uint64_t mailbox_identity = 0;
    std::uint64_t producer_identity = 0;
    std::uint64_t payload_offset = 0;
    std::uint32_t slot_index = std::numeric_limits<std::uint32_t>::max();
    LiveControlAdmissionResult outcome = LiveControlAdmissionResult::invalid;
    bool counter_changed = false;
};

using LiveControlActionReader = bool (*)(void* context, std::uint64_t sequence,
                                         LiveControlActionRecord& record) noexcept;
using LiveControlGenerationReader =
    bool (*)(void* context, std::size_t index,
             LiveControlRetainedGenerationView& generation) noexcept;
using LiveControlRetainedRecordReader = bool (*)(
    void* context, std::size_t generation_index, std::size_t record_index,
    LiveControlUpdateRecord& record, std::span<const std::byte>& payload) noexcept;
using LiveControlAdmissionReader =
    bool (*)(void* context, std::size_t index, LiveControlRetainedAdmission& admission,
             std::span<const std::byte>& payload) noexcept;

struct LiveControlReplayArtifactView {
    LiveControlReplayMetadata metadata{};
    std::span<const std::byte> artifact{};
    std::span<const std::byte> checkpoint{};
    std::span<const std::byte> nested_artifact{};
    std::size_t action_offset = 0;
    std::size_t generation_offset = 0;
    std::size_t record_offset = 0;
    std::size_t payload_offset = 0;
    std::uint64_t retention_policy_identity = 0;
    std::size_t admission_offset = 0;
    std::size_t admission_count = 0;
    std::size_t admission_payload_offset = 0;
    std::size_t admission_payload_bytes = 0;
    std::uint64_t admission_close_sequence = std::numeric_limits<std::uint64_t>::max();
};

[[nodiscard]] Status encode_live_control_replay_artifact(
    LiveControlReplayMetadata metadata, std::span<const std::byte> checkpoint,
    std::span<const std::byte> nested_artifact, std::uint64_t first_action_sequence,
    std::size_t action_count, LiveControlActionReader action_reader,
    std::size_t generation_count, LiveControlGenerationReader generation_reader,
    LiveControlRetainedRecordReader record_reader, void* reader_context,
    std::size_t maximum_bytes, std::span<std::byte> output, ArtifactWriteResult& result,
    std::uint64_t retention_policy_identity = 0, std::size_t admission_count = 0,
    LiveControlAdmissionReader admission_reader = nullptr,
    std::uint64_t admission_close_sequence =
        std::numeric_limits<std::uint64_t>::max()) noexcept;

[[nodiscard]] Status parse_live_control_replay_artifact(
    std::span<const std::byte> artifact,
    std::size_t maximum_bytes,
    LiveControlReplayArtifactView& view) noexcept;

[[nodiscard]] bool live_control_replay_action_at(
    const LiveControlReplayArtifactView& view,
    std::size_t index,
    LiveControlActionRecord& action) noexcept;
[[nodiscard]] bool
live_control_replay_admission_at(const LiveControlReplayArtifactView& view,
                                 std::size_t index,
                                 LiveControlRetainedAdmission& admission,
                                 std::span<const std::byte>& payload) noexcept;
[[nodiscard]] bool live_control_replay_generation_at(
    const LiveControlReplayArtifactView& view,
    std::size_t index,
    LiveControlRetainedGenerationView& generation) noexcept;
[[nodiscard]] bool live_control_replay_record_at(
    const LiveControlReplayArtifactView& view,
    std::size_t generation_index,
    std::size_t record_index,
    LiveControlUpdateRecord& record,
    std::span<const std::byte>& payload) noexcept;

} // namespace rt::detail
