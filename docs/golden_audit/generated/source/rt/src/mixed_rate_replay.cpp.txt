#include "mixed_rate_replay.hpp"

#include "mixed_rate_actions.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace rt::detail {
namespace {

constexpr std::uint64_t kMagic = 0x3152414d57465452ull; // RTFWMAR1
constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::uint32_t kInputDeadline = 1u;
constexpr std::uint32_t kInputNominal = 2u;

[[nodiscard]] bool add_size(
    std::size_t left,
    std::size_t right,
    std::size_t& result) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] bool multiply_size(
    std::size_t left,
    std::size_t right,
    std::size_t& result) noexcept {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

void hash_bytes(
    std::uint64_t& hash,
    std::span<const std::byte> bytes) noexcept {
    for (const auto byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= kFnvPrime;
    }
}

[[nodiscard]] std::uint64_t checksum(
    std::span<const std::byte> bytes) noexcept {
    auto hash = kFnvOffset;
    hash_bytes(hash, bytes);
    return hash;
}

[[nodiscard]] std::uint64_t artifact_checksum(
    std::span<const std::byte> bytes) noexcept {
    auto hash = kFnvOffset;
    hash_bytes(hash, bytes.first(24));
    const std::array<std::byte, 8> zero{};
    hash_bytes(hash, zero);
    hash_bytes(hash, bytes.subspan(32));
    return hash;
}

[[nodiscard]] bool store_u32(
    std::span<std::byte> bytes,
    std::size_t offset,
    std::uint32_t value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        return false;
    }
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::byte>(value >> (8u * index));
    }
    return true;
}

[[nodiscard]] bool store_u64(
    std::span<std::byte> bytes,
    std::size_t offset,
    std::uint64_t value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 8) {
        return false;
    }
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<std::byte>(value >> (8u * index));
    }
    return true;
}

[[nodiscard]] bool load_u32(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::uint32_t& value) noexcept {
    value = 0;
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        return false;
    }
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(
            static_cast<std::uint8_t>(bytes[offset + index])) << (8u * index);
    }
    return true;
}

[[nodiscard]] bool load_u64(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::uint64_t& value) noexcept {
    value = 0;
    if (offset > bytes.size() || bytes.size() - offset < 8) {
        return false;
    }
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(
            static_cast<std::uint8_t>(bytes[offset + index])) << (8u * index);
    }
    return true;
}

[[nodiscard]] bool zero_range(
    std::span<const std::byte> bytes,
    std::size_t begin,
    std::size_t end) noexcept {
    if (begin > end || end > bytes.size()) {
        return false;
    }
    for (std::size_t index = begin; index < end; ++index) {
        if (bytes[index] != std::byte{0}) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool identifier_valid(
    const std::array<char, replay_identifier_capacity>& value) noexcept {
    const auto terminator = std::find(value.begin(), value.end(), '\0');
    return terminator != value.begin() && terminator != value.end();
}

[[nodiscard]] bool decode_input_at(
    const ActiveReplayArtifactView& view,
    std::size_t wanted,
    ReplayInputView* output,
    std::size_t* next_offset) noexcept {
    if (wanted >= view.metadata.input_record_count) {
        return false;
    }
    std::size_t descriptor_bytes = 0;
    std::size_t descriptor_offset = 0;
    std::size_t descriptor_end = 0;
    if (!multiply_size(
            view.metadata.input_record_count,
            active_replay_input_header_size,
            descriptor_bytes) ||
        !multiply_size(
            wanted, active_replay_input_header_size, descriptor_offset) ||
        !add_size(view.input_offset, descriptor_offset, descriptor_offset) ||
        !add_size(view.input_offset, descriptor_bytes, descriptor_end) ||
        descriptor_end > view.action_offset ||
        descriptor_offset > descriptor_end ||
        descriptor_end - descriptor_offset < active_replay_input_header_size) {
        return false;
    }
    const auto header = view.artifact.subspan(
        descriptor_offset, active_replay_input_header_size);
    std::uint64_t frame = 0;
    std::uint64_t delta = 0;
    std::uint64_t deadline = 0;
    std::uint64_t nominal = 0;
    std::uint32_t input_type = 0;
    std::uint32_t flags = 0;
    std::uint64_t payload_size_u64 = 0;
    std::uint64_t payload_checksum = 0;
    std::uint64_t record_checksum = 0;
    std::uint64_t payload_offset_u64 = 0;
    if (!load_u64(header, 0, frame) || !load_u64(header, 8, delta) ||
        !load_u64(header, 16, deadline) ||
        !load_u64(header, 24, nominal) ||
        !load_u32(header, 32, input_type) ||
        !load_u32(header, 36, flags) ||
        !load_u64(header, 40, payload_size_u64) ||
        !load_u64(header, 48, payload_checksum) ||
        !load_u64(header, 56, record_checksum) ||
        !load_u64(header, 64, payload_offset_u64) ||
        (flags & ~(kInputDeadline | kInputNominal)) != 0 ||
        (flags & kInputNominal) == 0 || delta == 0 ||
        payload_size_u64 > std::numeric_limits<std::size_t>::max() ||
        payload_offset_u64 > std::numeric_limits<std::size_t>::max() ||
        !zero_range(header, 72, active_replay_input_header_size)) {
        return false;
    }
    const auto payload_size = static_cast<std::size_t>(payload_size_u64);
    const auto payload_offset = static_cast<std::size_t>(payload_offset_u64);
    std::size_t end = 0;
    if (payload_offset < descriptor_end ||
        !add_size(payload_offset, payload_size, end) ||
        end > view.action_offset) {
        return false;
    }
    const auto payload = view.artifact.subspan(payload_offset, payload_size);
    auto hash = kFnvOffset;
    hash_bytes(hash, header.first(56));
    const std::array<std::byte, 8> zero{};
    hash_bytes(hash, zero);
    hash_bytes(hash, header.subspan(64));
    hash_bytes(hash, payload);
    if (checksum(payload) != payload_checksum || hash != record_checksum ||
        ((flags & kInputDeadline) == 0 && deadline != 0)) {
        return false;
    }
    if (output) {
        output->frame.frame_index = frame;
        output->frame.delta = std::chrono::nanoseconds(delta);
        output->frame.deadline_ns = (flags & kInputDeadline) != 0
            ? std::optional<std::uint64_t>{deadline}
            : std::nullopt;
        output->frame.nominal_release_ns = nominal;
        output->input_type = input_type;
        output->payload = payload;
    }
    if (next_offset) {
        *next_offset = end;
    }
    return true;
}

} // namespace

Status encode_active_replay_artifact(
    ActiveReplayMetadata metadata,
    std::span<const std::byte> checkpoint,
    std::span<const ReplayInputRecord> inputs,
    std::uint64_t first_action_sequence,
    std::size_t action_count,
    MixedRateActionReader reader,
    void* reader_context,
    std::size_t maximum_bytes,
    std::span<std::byte> output,
    ArtifactWriteResult& result) noexcept {
    result = {};
    if (maximum_bytes == 0 || maximum_bytes > active_replay_absolute_max_bytes ||
        inputs.empty() || inputs.size() > active_replay_record_capacity_limit ||
        action_count > active_replay_record_capacity_limit ||
        (action_count != 0 && !reader) || metadata.host_policy_version == 0 ||
        !identifier_valid(metadata.build_id) ||
        !identifier_valid(metadata.workload_id)) {
        return Status::invalid_argument;
    }
    CheckpointMetadata checkpoint_metadata;
    if (inspect_checkpoint_artifact(checkpoint, checkpoint_metadata) != Status::ok) {
        return Status::invalid_artifact;
    }
    std::size_t descriptor_bytes = 0;
    std::size_t input_bytes = 0;
    std::uint64_t input_payload_bytes = 0;
    std::uint64_t previous_frame = 0;
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const auto& input = inputs[index];
        if (input.frame.delta.count() <= 0 ||
            !input.frame.nominal_release_ns ||
            (index != 0 && input.frame.frame_index <= previous_frame) ||
            input.payload.size() >
                std::numeric_limits<std::uint64_t>::max() - input_payload_bytes ||
            !add_size(input_bytes, input.payload.size(), input_bytes)) {
            return Status::invalid_argument;
        }
        input_payload_bytes += input.payload.size();
        previous_frame = input.frame.frame_index;
    }
    if (!multiply_size(
            inputs.size(), active_replay_input_header_size, descriptor_bytes) ||
        !add_size(input_bytes, descriptor_bytes, input_bytes)) {
        return Status::capacity_exceeded;
    }
    std::size_t action_bytes = 0;
    std::size_t total = 0;
    if (!multiply_size(action_count, active_replay_action_bytes, action_bytes) ||
        !add_size(active_replay_header_size, checkpoint.size(), total) ||
        !add_size(total, input_bytes, total) ||
        !add_size(total, action_bytes, total) || total > maximum_bytes) {
        return Status::capacity_exceeded;
    }
    result.required_bytes = total;
    if (output.size() < total) {
        return Status::capacity_exceeded;
    }
    auto bytes = output.first(total);
    std::fill(bytes.begin(), bytes.end(), std::byte{0});
    const auto checkpoint_offset = active_replay_header_size;
    const auto input_offset = checkpoint_offset + checkpoint.size();
    const auto action_offset = input_offset + input_bytes;
    metadata.schema_version = active_replay_schema_version;
    metadata.header_size = active_replay_header_size;
    metadata.total_bytes = total;
    metadata.checkpoint_frame_index = checkpoint_metadata.checkpoint_frame_index;
    metadata.first_frame_index = inputs.front().frame.frame_index;
    metadata.last_frame_index = inputs.back().frame.frame_index;
    metadata.checkpoint_bytes = checkpoint.size();
    metadata.input_payload_bytes = input_payload_bytes;
    metadata.first_action_sequence = first_action_sequence;
    metadata.last_action_sequence = action_count == 0
        ? first_action_sequence
        : first_action_sequence + action_count - 1;
    metadata.input_record_count = static_cast<std::uint32_t>(inputs.size());
    metadata.action_record_count = static_cast<std::uint32_t>(action_count);
    if ((action_count != 0 &&
         metadata.last_action_sequence < first_action_sequence) ||
        !store_u64(bytes, 0, kMagic) ||
        !store_u32(bytes, 8, active_replay_schema_version) ||
        !store_u32(bytes, 12, active_replay_header_size) ||
        !store_u64(bytes, 16, total) || !store_u64(bytes, 32, metadata.runtime_id) ||
        !store_u64(bytes, 40, metadata.config_id) ||
        !store_u64(bytes, 48, metadata.replay_id) ||
        !store_u64(bytes, 56, metadata.graph_id) ||
        !store_u64(bytes, 64, metadata.state_schema_id) ||
        !store_u64(bytes, 72, metadata.host_policy_version) ||
        !store_u64(bytes, 80, metadata.checkpoint_frame_index) ||
        !store_u64(bytes, 88, metadata.first_frame_index) ||
        !store_u64(bytes, 96, metadata.last_frame_index) ||
        !store_u64(bytes, 104, metadata.nominal_epoch_ns) ||
        !store_u64(bytes, 112, metadata.final_state_hash) ||
        !store_u64(bytes, 120, checkpoint_offset) ||
        !store_u64(bytes, 128, checkpoint.size()) ||
        !store_u64(bytes, 136, checksum(checkpoint)) ||
        !store_u64(bytes, 144, input_offset) ||
        !store_u32(bytes, 152, metadata.input_record_count) ||
        !store_u32(bytes, 156, active_replay_input_header_size) ||
        !store_u64(bytes, 160, input_payload_bytes) ||
        !store_u64(bytes, 168, action_offset) ||
        !store_u32(bytes, 176, metadata.action_record_count) ||
        !store_u32(bytes, 180, active_replay_action_bytes) ||
        !store_u64(bytes, 184, metadata.first_action_sequence) ||
        !store_u64(bytes, 192, metadata.last_action_sequence) ||
        !store_u32(bytes, 200, static_cast<std::uint32_t>(metadata.determinism_tier))) {
        return Status::internal_error;
    }
    std::memcpy(bytes.data() + 224, metadata.build_id.data(), metadata.build_id.size());
    std::memcpy(bytes.data() + 288, metadata.workload_id.data(), metadata.workload_id.size());
    std::copy(checkpoint.begin(), checkpoint.end(), bytes.begin() + checkpoint_offset);
    auto payload_cursor = input_offset + descriptor_bytes;
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const auto& input = inputs[index];
        const auto descriptor_offset =
            input_offset + index * active_replay_input_header_size;
        auto header = bytes.subspan(
            descriptor_offset, active_replay_input_header_size);
        const auto flags = kInputNominal |
            (input.frame.deadline_ns ? kInputDeadline : 0u);
        (void)store_u64(header, 0, input.frame.frame_index);
        (void)store_u64(header, 8, static_cast<std::uint64_t>(input.frame.delta.count()));
        (void)store_u64(header, 16, input.frame.deadline_ns.value_or(0));
        (void)store_u64(header, 24, *input.frame.nominal_release_ns);
        (void)store_u32(header, 32, input.input_type);
        (void)store_u32(header, 36, flags);
        (void)store_u64(header, 40, input.payload.size());
        (void)store_u64(header, 48, checksum(input.payload));
        (void)store_u64(header, 64, payload_cursor);
        std::copy(
            input.payload.begin(), input.payload.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(payload_cursor));
        auto hash = kFnvOffset;
        hash_bytes(hash, std::span<const std::byte>(header).first(56));
        const std::array<std::byte, 8> zero{};
        hash_bytes(hash, zero);
        hash_bytes(hash, std::span<const std::byte>(header).subspan(64));
        hash_bytes(hash, input.payload);
        (void)store_u64(header, 56, hash);
        payload_cursor += input.payload.size();
    }
    for (std::size_t index = 0; index < action_count; ++index) {
        MixedRateActionRecord action;
        const auto sequence = first_action_sequence + index;
        if (!reader(reader_context, sequence, action) ||
            action.sequence != sequence || !mixed_rate_action_valid(action)) {
            std::fill(bytes.begin(), bytes.end(), std::byte{0});
            return Status::invalid_artifact;
        }
        const auto offset = action_offset + index * active_replay_action_bytes;
        const auto action_bytes_span = std::as_bytes(std::span(&action, 1));
        std::copy(
            action_bytes_span.begin(), action_bytes_span.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        (void)store_u64(bytes, offset + sizeof(action), checksum(action_bytes_span));
    }
    const auto whole = artifact_checksum(std::span<const std::byte>(bytes));
    (void)store_u64(bytes, 24, whole);
    result.bytes_written = total;
    result.checksum = whole;
    return Status::ok;
}

Status parse_active_replay_artifact(
    std::span<const std::byte> artifact,
    std::size_t maximum_bytes,
    ActiveReplayArtifactView& view) noexcept {
    view = {};
    if (maximum_bytes == 0 || maximum_bytes > active_replay_absolute_max_bytes ||
        artifact.size() < active_replay_header_size || artifact.size() > maximum_bytes) {
        return Status::invalid_artifact;
    }
    std::uint64_t magic = 0;
    std::uint32_t schema = 0;
    std::uint32_t header_size = 0;
    std::uint64_t total = 0;
    std::uint64_t whole = 0;
    std::uint64_t checkpoint_offset_u64 = 0;
    std::uint64_t checkpoint_bytes_u64 = 0;
    std::uint64_t checkpoint_checksum = 0;
    std::uint64_t input_offset_u64 = 0;
    std::uint32_t input_count = 0;
    std::uint32_t input_header_size = 0;
    std::uint64_t action_offset_u64 = 0;
    std::uint32_t action_count = 0;
    std::uint32_t action_record_size = 0;
    std::uint32_t determinism = 0;
    if (!load_u64(artifact, 0, magic) || !load_u32(artifact, 8, schema) ||
        !load_u32(artifact, 12, header_size) || !load_u64(artifact, 16, total) ||
        !load_u64(artifact, 24, whole) ||
        !load_u64(artifact, 120, checkpoint_offset_u64) ||
        !load_u64(artifact, 128, checkpoint_bytes_u64) ||
        !load_u64(artifact, 136, checkpoint_checksum) ||
        !load_u64(artifact, 144, input_offset_u64) ||
        !load_u32(artifact, 152, input_count) ||
        !load_u32(artifact, 156, input_header_size) ||
        !load_u64(artifact, 168, action_offset_u64) ||
        !load_u32(artifact, 176, action_count) ||
        !load_u32(artifact, 180, action_record_size) ||
        !load_u32(artifact, 200, determinism) || magic != kMagic ||
        schema != active_replay_schema_version ||
        header_size != active_replay_header_size || total != artifact.size() ||
        whole != artifact_checksum(artifact) || input_count == 0 ||
        input_count > active_replay_record_capacity_limit ||
        action_count > active_replay_record_capacity_limit ||
        input_header_size != active_replay_input_header_size ||
        action_record_size != active_replay_action_bytes ||
        checkpoint_offset_u64 != active_replay_header_size ||
        checkpoint_offset_u64 > std::numeric_limits<std::size_t>::max() ||
        checkpoint_bytes_u64 > std::numeric_limits<std::size_t>::max() ||
        input_offset_u64 > std::numeric_limits<std::size_t>::max() ||
        action_offset_u64 > std::numeric_limits<std::size_t>::max() ||
        !zero_range(artifact, 204, 224) || !zero_range(artifact, 352, 384)) {
        return Status::invalid_artifact;
    }
    const auto checkpoint_offset = static_cast<std::size_t>(checkpoint_offset_u64);
    const auto checkpoint_bytes = static_cast<std::size_t>(checkpoint_bytes_u64);
    const auto input_offset = static_cast<std::size_t>(input_offset_u64);
    const auto action_offset = static_cast<std::size_t>(action_offset_u64);
    std::size_t checkpoint_end = 0;
    std::size_t action_bytes = 0;
    std::size_t action_end = 0;
    if (!add_size(checkpoint_offset, checkpoint_bytes, checkpoint_end) ||
        checkpoint_end != input_offset || input_offset > action_offset ||
        !multiply_size(action_count, active_replay_action_bytes, action_bytes) ||
        !add_size(action_offset, action_bytes, action_end) ||
        action_end != artifact.size()) {
        return Status::invalid_artifact;
    }
    const auto checkpoint = artifact.subspan(checkpoint_offset, checkpoint_bytes);
    CheckpointMetadata checkpoint_metadata;
    if (checksum(checkpoint) != checkpoint_checksum ||
        inspect_checkpoint_artifact(checkpoint, checkpoint_metadata) != Status::ok) {
        return Status::invalid_artifact;
    }
    ActiveReplayMetadata metadata;
    metadata.schema_version = schema;
    metadata.header_size = header_size;
    metadata.total_bytes = total;
    metadata.artifact_checksum = whole;
    (void)load_u64(artifact, 32, metadata.runtime_id);
    (void)load_u64(artifact, 40, metadata.config_id);
    (void)load_u64(artifact, 48, metadata.replay_id);
    (void)load_u64(artifact, 56, metadata.graph_id);
    (void)load_u64(artifact, 64, metadata.state_schema_id);
    (void)load_u64(artifact, 72, metadata.host_policy_version);
    (void)load_u64(artifact, 80, metadata.checkpoint_frame_index);
    (void)load_u64(artifact, 88, metadata.first_frame_index);
    (void)load_u64(artifact, 96, metadata.last_frame_index);
    (void)load_u64(artifact, 104, metadata.nominal_epoch_ns);
    (void)load_u64(artifact, 112, metadata.final_state_hash);
    metadata.checkpoint_bytes = checkpoint_bytes;
    (void)load_u64(artifact, 160, metadata.input_payload_bytes);
    (void)load_u64(artifact, 184, metadata.first_action_sequence);
    (void)load_u64(artifact, 192, metadata.last_action_sequence);
    metadata.input_record_count = input_count;
    metadata.action_record_count = action_count;
    metadata.determinism_tier = static_cast<DeterminismTier>(determinism);
    std::memcpy(metadata.build_id.data(), artifact.data() + 224, metadata.build_id.size());
    std::memcpy(metadata.workload_id.data(), artifact.data() + 288, metadata.workload_id.size());
    if (metadata.host_policy_version == 0 ||
        metadata.checkpoint_frame_index != checkpoint_metadata.checkpoint_frame_index ||
        metadata.replay_id != checkpoint_metadata.replay_id ||
        metadata.graph_id != checkpoint_metadata.graph_id ||
        metadata.state_schema_id != checkpoint_metadata.state_schema_id ||
        metadata.determinism_tier != checkpoint_metadata.determinism_tier ||
        !identifier_valid(metadata.build_id) || !identifier_valid(metadata.workload_id) ||
        metadata.workload_id != checkpoint_metadata.workload_id ||
        (action_count != 0 &&
         (metadata.last_action_sequence < metadata.first_action_sequence ||
          metadata.last_action_sequence - metadata.first_action_sequence + 1 != action_count)) ||
        (action_count == 0 &&
         metadata.last_action_sequence != metadata.first_action_sequence)) {
        return Status::incompatible_artifact;
    }
    view.metadata = metadata;
    view.artifact = artifact;
    view.checkpoint = checkpoint;
    view.input_offset = input_offset;
    view.action_offset = action_offset;
    std::size_t descriptor_bytes = 0;
    std::size_t next = 0;
    if (!multiply_size(
            input_count, active_replay_input_header_size, descriptor_bytes) ||
        !add_size(input_offset, descriptor_bytes, next) ||
        next > action_offset) {
        view = {};
        return Status::invalid_artifact;
    }
    ReplayInputView input;
    std::uint64_t previous_frame = 0;
    std::uint64_t payload_total = 0;
    for (std::size_t index = 0; index < input_count; ++index) {
        std::size_t payload_end = 0;
        if (!decode_input_at(view, index, &input, &payload_end) ||
            input.payload.data() != artifact.data() + next ||
            (index != 0 && input.frame.frame_index <= previous_frame) ||
            input.payload.size() > std::numeric_limits<std::uint64_t>::max() - payload_total) {
            view = {};
            return Status::invalid_artifact;
        }
        payload_total += input.payload.size();
        previous_frame = input.frame.frame_index;
        next = payload_end;
    }
    if (next != action_offset || payload_total != metadata.input_payload_bytes ||
        input.frame.frame_index != metadata.last_frame_index) {
        view = {};
        return Status::invalid_artifact;
    }
    ReplayInputView first_input;
    if (!decode_input_at(view, 0, &first_input, nullptr) ||
        first_input.frame.frame_index != metadata.first_frame_index) {
        view = {};
        return Status::invalid_artifact;
    }
    for (std::size_t index = 0; index < action_count; ++index) {
        MixedRateActionRecord action;
        if (!active_replay_action_at(view, index, action) ||
            action.sequence != metadata.first_action_sequence + index ||
            action.host_policy_version != metadata.host_policy_version ||
            action.runtime_id != metadata.runtime_id ||
            action.frame_index < metadata.first_frame_index ||
            action.frame_index > metadata.last_frame_index) {
            view = {};
            return Status::invalid_artifact;
        }
    }
    return Status::ok;
}

bool active_replay_input_at(
    const ActiveReplayArtifactView& view,
    std::size_t index,
    ReplayInputView& input) noexcept {
    input = {};
    return index < view.metadata.input_record_count &&
        decode_input_at(view, index, &input, nullptr);
}

bool active_replay_action_at(
    const ActiveReplayArtifactView& view,
    std::size_t index,
    MixedRateActionRecord& action) noexcept {
    action = {};
    if (index >= view.metadata.action_record_count) {
        return false;
    }
    const auto offset = view.action_offset + index * active_replay_action_bytes;
    if (offset > view.artifact.size() ||
        view.artifact.size() - offset < active_replay_action_bytes) {
        return false;
    }
    std::memcpy(&action, view.artifact.data() + offset, sizeof(action));
    std::uint64_t expected = 0;
    const auto record_bytes = view.artifact.subspan(offset, sizeof(action));
    return load_u64(view.artifact, offset + sizeof(action), expected) &&
        checksum(record_bytes) == expected && mixed_rate_action_valid(action);
}

} // namespace rt::detail

namespace rt {

Status inspect_active_replay_artifact(
    std::span<const std::byte> artifact,
    ActiveReplayMetadata& metadata) noexcept {
    metadata = {};
    detail::ActiveReplayArtifactView view;
    const auto status = detail::parse_active_replay_artifact(
        artifact, active_replay_absolute_max_bytes, view);
    if (status == Status::ok) {
        metadata = view.metadata;
    }
    return status;
}

} // namespace rt
