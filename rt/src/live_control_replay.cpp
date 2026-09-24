#include "live_control_replay.hpp"

#include "live_control_actions.hpp"
#include "snapshot_codec.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace rt::detail {
namespace {

constexpr std::uint64_t kMagic = 0x3152434c57465452ull; // RTFWLCR1
constexpr std::uint64_t kMagicV2 = 0x3252434c57465452ull; // RTFWLCR2
constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

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
    if (left != 0 && right >
            std::numeric_limits<std::size_t>::max() / left) {
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

void hash_u64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= kFnvPrime;
        value >>= 8u;
    }
}

void hash_target(
    std::uint64_t& hash,
    const LiveControlBoundaryTarget& target) noexcept {
    hash_u64(hash, static_cast<std::uint8_t>(target.kind));
    hash_u64(hash, target.frame_index);
    hash_u64(hash, target.rate_release_sequence);
    hash_u64(hash, target.reference_release_index);
    hash_u64(hash, target.rate_domain_registration_index);
    hash_u64(hash, target.phase_index);
    hash_u64(hash, target.rate_substep_ordinal);
}

[[nodiscard]] std::uint64_t checksum(
    std::span<const std::byte> bytes) noexcept {
    auto hash = kFnvOffset;
    hash_bytes(hash, bytes);
    return hash;
}

[[nodiscard]] std::uint64_t replay_artifact_checksum(
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
            static_cast<std::uint8_t>(bytes[offset + index])) <<
            (8u * index);
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
            static_cast<std::uint8_t>(bytes[offset + index])) <<
            (8u * index);
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

template <std::size_t Size>
[[nodiscard]] bool identifier_valid(
    const std::array<char, Size>& identifier) noexcept {
    bool terminated = false;
    for (std::size_t index = 0; index < identifier.size(); ++index) {
        const auto value = identifier[index];
        if (terminated) {
            if (value != '\0') {
                return false;
            }
            continue;
        }
        if (value == '\0') {
            if (index == 0) {
                return false;
            }
            terminated = true;
            continue;
        }
        const bool valid =
            (value >= 'a' && value <= 'z') ||
            (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '.' || value == '_' ||
            value == ':' || value == '/' || value == '@' || value == '-';
        if (!valid) {
            return false;
        }
    }
    return terminated;
}

[[nodiscard]] bool target_equal(
    const LiveControlBoundaryTarget& left,
    const LiveControlBoundaryTarget& right) noexcept {
    return left.frame_index == right.frame_index &&
        left.rate_release_sequence == right.rate_release_sequence &&
        left.reference_release_index == right.reference_release_index &&
        left.rate_domain_registration_index ==
            right.rate_domain_registration_index &&
        left.phase_index == right.phase_index &&
        left.rate_substep_ordinal == right.rate_substep_ordinal &&
        left.kind == right.kind;
}

[[nodiscard]] bool nested_valid(
    LiveControlNestedArtifactKind kind,
    std::span<const std::byte> artifact) noexcept {
    if (kind == LiveControlNestedArtifactKind::input_log) {
        InputLogMetadata metadata;
        return inspect_input_log_artifact(artifact, metadata) == Status::ok;
    }
    if (kind == LiveControlNestedArtifactKind::active_replay) {
        ActiveReplayMetadata metadata;
        return inspect_active_replay_artifact(artifact, metadata) == Status::ok;
    }
    return false;
}

[[nodiscard]] bool descriptor_target(
    std::span<const std::byte> descriptor,
    LiveControlBoundaryTarget& target) noexcept {
    if (descriptor.size() != live_control_replay_generation_bytes) {
        return false;
    }
    std::memcpy(&target, descriptor.data(), sizeof(target));
    return (target.kind == LiveControlTargetKind::host_frame ||
            target.kind == LiveControlTargetKind::rate_release) &&
        zero_range(descriptor, 33, 40) &&
        zero_range(descriptor, 93, 120) &&
        zero_range(descriptor, 120, 128);
}

} // namespace

Status encode_live_control_replay_artifact(
    LiveControlReplayMetadata metadata, std::span<const std::byte> checkpoint,
    std::span<const std::byte> nested_artifact, std::uint64_t first_action_sequence,
    std::size_t action_count, LiveControlActionReader action_reader,
    std::size_t generation_count, LiveControlGenerationReader generation_reader,
    LiveControlRetainedRecordReader record_reader, void* reader_context,
    std::size_t maximum_bytes, std::span<std::byte> output, ArtifactWriteResult& result,
    std::uint64_t retention_policy_identity, std::size_t admission_count,
    LiveControlAdmissionReader admission_reader,
    std::uint64_t admission_close_sequence) noexcept {
    result = {};
    const bool lossless =
        metadata.schema_version == live_control_lossless_replay_schema_version;
    const auto header_size = lossless ? live_control_lossless_replay_header_size
                                      : live_control_replay_header_size;
    std::size_t admission_payload_bytes = 0;
    std::size_t admission_bytes = 0;
    if ((!lossless && metadata.schema_version != live_control_replay_schema_version) ||
        (lossless && (retention_policy_identity == 0 || !admission_reader ||
                      admission_count > live_control_action_capacity_limit)) ||
        (!lossless && (retention_policy_identity != 0 || admission_count != 0))) {
        return Status::invalid_argument;
    }
    if (lossless) {
        std::uint64_t previous = 0;
        for (std::size_t index = 0; index < admission_count; ++index) {
            LiveControlRetainedAdmission admission;
            std::span<const std::byte> payload;
            if (!admission_reader(reader_context, index, admission, payload) ||
                (index != 0 && admission.action_sequence <= previous) ||
                !add_size(admission_payload_bytes, payload.size(),
                          admission_payload_bytes)) {
                return Status::invalid_artifact;
            }
            previous = admission.action_sequence;
        }
        if (!multiply_size(admission_count, live_control_replay_admission_bytes,
                           admission_bytes)) {
            return Status::capacity_exceeded;
        }
    }
    CheckpointMetadata checkpoint_metadata;
    if (maximum_bytes == 0 ||
        maximum_bytes > live_control_replay_absolute_max_bytes ||
        metadata.runtime_id == 0 || metadata.configuration_generation == 0 ||
        metadata.policy_identity == 0 || action_count == 0 ||
        !identifier_valid(metadata.build_id) ||
        !identifier_valid(metadata.workload_id) ||
        !action_reader ||
        (generation_count != 0 && (!generation_reader || !record_reader)) ||
        inspect_checkpoint_artifact(checkpoint, checkpoint_metadata) != Status::ok ||
        !nested_valid(metadata.nested_kind, nested_artifact)) {
        return Status::invalid_argument;
    }
    std::size_t record_count = 0;
    std::size_t payload_bytes = 0;
    for (std::size_t generation_index = 0;
         generation_index < generation_count;
         ++generation_index) {
        LiveControlRetainedGenerationView generation;
        if (!generation_reader(reader_context, generation_index, generation) ||
            !generation.settled || generation.generation_identity == 0 ||
            generation.record_count == 0 ||
            !add_size(record_count, generation.record_count, record_count) ||
            !add_size(payload_bytes, generation.payload_bytes, payload_bytes)) {
            return Status::invalid_artifact;
        }
        std::size_t observed_payload = 0;
        for (std::size_t record_index = 0;
             record_index < generation.record_count;
             ++record_index) {
            LiveControlUpdateRecord record;
            std::span<const std::byte> payload;
            if (!record_reader(
                    reader_context,
                    generation_index,
                    record_index,
                    record,
                    payload) ||
                payload.size() != record.payload_bytes ||
                live_control_payload_digest(payload) != record.payload_digest ||
                !add_size(observed_payload, payload.size(), observed_payload)) {
                return Status::invalid_artifact;
            }
        }
        if (observed_payload != generation.payload_bytes) {
            return Status::invalid_artifact;
        }
    }
    if (record_count > std::numeric_limits<std::uint32_t>::max() ||
        payload_bytes > std::numeric_limits<std::uint32_t>::max() ||
        action_count > std::numeric_limits<std::uint32_t>::max() ||
        generation_count > std::numeric_limits<std::uint32_t>::max()) {
        return Status::capacity_exceeded;
    }
    std::size_t action_bytes = 0;
    std::size_t generation_bytes = 0;
    std::size_t record_bytes = 0;
    std::size_t total = header_size;
    if (!multiply_size(action_count, live_control_replay_action_bytes, action_bytes) ||
        !multiply_size(generation_count, live_control_replay_generation_bytes,
                       generation_bytes) ||
        !multiply_size(record_count, live_control_replay_record_bytes, record_bytes) ||
        !add_size(total, checkpoint.size(), total) ||
        !add_size(total, nested_artifact.size(), total) ||
        !add_size(total, action_bytes, total) ||
        !add_size(total, generation_bytes, total) ||
        !add_size(total, record_bytes, total) ||
        !add_size(total, payload_bytes, total) ||
        !add_size(total, admission_bytes, total) ||
        !add_size(total, admission_payload_bytes, total) || total > maximum_bytes) {
        return Status::capacity_exceeded;
    }
    result.required_bytes = total;
    if (output.size() < total) {
        return Status::capacity_exceeded;
    }
    auto bytes = output.first(total);
    std::fill(bytes.begin(), bytes.end(), std::byte{0});
    const auto checkpoint_offset = header_size;
    const auto nested_offset = checkpoint_offset + checkpoint.size();
    const auto action_offset = nested_offset + nested_artifact.size();
    const auto generation_offset = action_offset + action_bytes;
    const auto record_offset = generation_offset + generation_bytes;
    const auto payload_offset = record_offset + record_bytes;
    const auto admission_offset = payload_offset + payload_bytes;
    const auto admission_payload_offset = admission_offset + admission_bytes;
    const auto last_action_sequence = first_action_sequence + action_count - 1;
    if (last_action_sequence < first_action_sequence ||
        !store_u64(bytes, 0, lossless ? kMagicV2 : kMagic) ||
        !store_u32(bytes, 8, metadata.schema_version) ||
        !store_u32(bytes, 12, static_cast<std::uint32_t>(header_size)) ||
        !store_u64(bytes, 16, total) || !store_u64(bytes, 32, metadata.runtime_id) ||
        !store_u64(bytes, 40, metadata.configuration_generation) ||
        !store_u64(bytes, 48, metadata.config_id) ||
        !store_u64(bytes, 56, metadata.replay_id) ||
        !store_u64(bytes, 64, metadata.graph_id) ||
        !store_u64(bytes, 72, metadata.state_schema_id) ||
        !store_u64(bytes, 80, metadata.policy_identity) ||
        !store_u64(bytes, 88, metadata.final_state_hash) ||
        !store_u64(bytes, 96, checkpoint_offset) ||
        !store_u64(bytes, 104, checkpoint.size()) ||
        !store_u64(bytes, 112, checksum(checkpoint)) ||
        !store_u64(bytes, 120, nested_offset) ||
        !store_u64(bytes, 128, nested_artifact.size()) ||
        !store_u64(bytes, 136, checksum(nested_artifact)) ||
        !store_u64(bytes, 144, action_offset) ||
        !store_u64(bytes, 152, first_action_sequence) ||
        !store_u64(bytes, 160, last_action_sequence) ||
        !store_u32(bytes, 168, static_cast<std::uint32_t>(action_count)) ||
        !store_u32(bytes, 172, live_control_replay_action_bytes) ||
        !store_u64(bytes, 176, generation_offset) ||
        !store_u32(bytes, 184, static_cast<std::uint32_t>(generation_count)) ||
        !store_u32(bytes, 188, live_control_replay_generation_bytes) ||
        !store_u64(bytes, 192, record_offset) ||
        !store_u32(bytes, 200, static_cast<std::uint32_t>(record_count)) ||
        !store_u32(bytes, 204, live_control_replay_record_bytes) ||
        !store_u64(bytes, 208, payload_offset) ||
        !store_u64(bytes, 216, payload_bytes) ||
        !store_u32(bytes, 224, static_cast<std::uint32_t>(metadata.nested_kind)) ||
        !store_u32(bytes, 228, static_cast<std::uint32_t>(metadata.determinism_tier))) {
        return Status::internal_error;
    }
    std::memcpy(bytes.data() + 232, metadata.build_id.data(),
                metadata.build_id.size());
    std::memcpy(bytes.data() + 296, metadata.workload_id.data(),
                metadata.workload_id.size());
    std::copy(checkpoint.begin(), checkpoint.end(),
              bytes.data() + checkpoint_offset);
    std::copy(nested_artifact.begin(), nested_artifact.end(),
              bytes.data() + nested_offset);
    for (std::size_t index = 0; index < action_count; ++index) {
        LiveControlActionRecord action;
        const auto sequence = first_action_sequence + index;
        if (!action_reader(reader_context, sequence, action) ||
            action.sequence != sequence || !live_control_action_valid(action)) {
            std::fill(bytes.begin(), bytes.end(), std::byte{0});
            return Status::invalid_artifact;
        }
        action.runtime_id = 0;
        action.configuration_generation = 0;
        const auto offset = action_offset +
            index * live_control_replay_action_bytes;
        std::memcpy(bytes.data() + offset, &action, sizeof(action));
        (void)store_u64(
            bytes,
            offset + sizeof(action),
            checksum(bytes.subspan(offset, sizeof(action))));
    }
    std::size_t global_record = 0;
    std::size_t global_payload = 0;
    for (std::size_t generation_index = 0;
         generation_index < generation_count;
         ++generation_index) {
        LiveControlRetainedGenerationView generation;
        (void)generation_reader(reader_context, generation_index, generation);
        const auto offset = generation_offset +
            generation_index * live_control_replay_generation_bytes;
        std::memcpy(bytes.data() + offset, &generation.target,
                    sizeof(generation.target));
        (void)store_u64(bytes, offset + 40, generation.generation_identity);
        (void)store_u64(bytes, offset + 48,
                        generation.prior_generation_identity);
        (void)store_u64(bytes, offset + 56,
                        generation.first_action_sequence);
        (void)store_u64(bytes, offset + 64, global_record);
        (void)store_u64(bytes, offset + 72, global_payload);
        (void)store_u32(bytes, offset + 80,
                        static_cast<std::uint32_t>(generation.record_count));
        (void)store_u32(bytes, offset + 84,
                        static_cast<std::uint32_t>(generation.payload_bytes));
        (void)store_u32(bytes, offset + 88,
                        static_cast<std::uint32_t>(generation.terminal_status));
        bytes[offset + 92] = generation.settled
            ? std::byte{1} : std::byte{0};
        for (std::size_t record_index = 0;
             record_index < generation.record_count;
             ++record_index, ++global_record) {
            LiveControlUpdateRecord record;
            std::span<const std::byte> payload;
            (void)record_reader(
                reader_context,
                generation_index,
                record_index,
                record,
                payload);
            record.runtime_id = 0;
            record.configuration_generation = 0;
            const auto record_descriptor = record_offset +
                global_record * live_control_replay_record_bytes;
            std::memcpy(bytes.data() + record_descriptor, &record,
                        sizeof(record));
            (void)store_u64(bytes, record_descriptor + 128, global_payload);
            (void)store_u32(bytes, record_descriptor + 136,
                            static_cast<std::uint32_t>(payload.size()));
            std::copy(payload.begin(), payload.end(),
                      bytes.data() + payload_offset + global_payload);
            auto record_hash = kFnvOffset;
            hash_bytes(record_hash, bytes.subspan(record_descriptor, 144));
            hash_bytes(record_hash, payload);
            (void)store_u64(bytes, record_descriptor + 144, record_hash);
            global_payload += payload.size();
        }
    }
    (void)store_u64(
        bytes, 360, checksum(bytes.subspan(action_offset, action_bytes)));
    (void)store_u64(
        bytes, 368,
        checksum(bytes.subspan(generation_offset, generation_bytes)));
    (void)store_u64(
        bytes, 376,
        checksum(bytes.subspan(record_offset, record_bytes + payload_bytes)));
    if (lossless) {
        (void)store_u64(bytes, 384, retention_policy_identity);
        (void)store_u64(bytes, 392, admission_offset);
        (void)store_u32(bytes, 400, static_cast<std::uint32_t>(admission_count));
        (void)store_u32(bytes, 404, live_control_replay_admission_bytes);
        (void)store_u64(bytes, 408, admission_payload_offset);
        (void)store_u64(bytes, 416, admission_payload_bytes);
        (void)store_u64(bytes, 432, admission_close_sequence);
        std::size_t payload_cursor = 0;
        for (std::size_t index = 0; index < admission_count; ++index) {
            LiveControlRetainedAdmission admission;
            std::span<const std::byte> payload;
            if (!admission_reader(reader_context, index, admission, payload)) {
                return Status::invalid_artifact;
            }
            admission.record.runtime_id = 0;
            admission.record.configuration_generation = 0;
            const auto offset =
                admission_offset + index * live_control_replay_admission_bytes;
            std::memcpy(bytes.data() + offset, &admission.record,
                        sizeof(admission.record));
            (void)store_u64(bytes, offset + 128, admission.action_sequence);
            (void)store_u64(bytes, offset + 136, admission.mailbox_identity);
            (void)store_u64(bytes, offset + 144, admission.producer_identity);
            (void)store_u32(bytes, offset + 152, admission.slot_index);
            bytes[offset + 156] = static_cast<std::byte>(admission.outcome);
            bytes[offset + 157] = static_cast<std::byte>(admission.counter_changed);
            (void)store_u64(bytes, offset + 160, payload_cursor);
            (void)store_u32(bytes, offset + 168,
                            static_cast<std::uint32_t>(payload.size()));
            std::copy(payload.begin(), payload.end(),
                      bytes.data() + admission_payload_offset + payload_cursor);
            auto hash = kFnvOffset;
            hash_bytes(hash, bytes.subspan(offset, 176));
            hash_bytes(hash, payload);
            (void)store_u64(bytes, offset + 176, hash);
            payload_cursor += payload.size();
        }
        (void)store_u64(bytes, 424, checksum(bytes.subspan(admission_offset)));
    }
    const auto whole = replay_artifact_checksum(bytes);
    (void)store_u64(bytes, 24, whole);
    result.bytes_written = total;
    result.checksum = whole;
    return Status::ok;
}

Status parse_live_control_replay_artifact(
    std::span<const std::byte> artifact,
    std::size_t maximum_bytes,
    LiveControlReplayArtifactView& view) noexcept {
    view = {};
    if (maximum_bytes == 0 ||
        maximum_bytes > live_control_replay_absolute_max_bytes ||
        artifact.size() < live_control_replay_header_size ||
        artifact.size() > maximum_bytes) {
        return Status::invalid_artifact;
    }
    std::uint64_t magic = 0;
    std::uint32_t schema = 0;
    std::uint32_t header = 0;
    std::uint64_t total = 0;
    std::uint64_t whole = 0;
    std::array<std::uint64_t, 6> offsets{};
    std::uint64_t checkpoint_bytes = 0;
    std::uint64_t checkpoint_hash = 0;
    std::uint64_t nested_bytes = 0;
    std::uint64_t nested_hash = 0;
    std::uint64_t first_action = 0;
    std::uint64_t last_action = 0;
    std::uint32_t action_count = 0;
    std::uint32_t action_stride = 0;
    std::uint32_t generation_count = 0;
    std::uint32_t generation_stride = 0;
    std::uint32_t record_count = 0;
    std::uint32_t record_stride = 0;
    std::uint64_t payload_bytes = 0;
    std::uint32_t nested_kind = 0;
    std::uint32_t determinism = 0;
    std::uint64_t action_hash = 0;
    std::uint64_t generation_hash = 0;
    std::uint64_t record_payload_hash = 0;
    if (!load_u64(artifact, 0, magic) || !load_u32(artifact, 8, schema) ||
        !load_u32(artifact, 12, header) || !load_u64(artifact, 16, total) ||
        !load_u64(artifact, 24, whole) || !load_u64(artifact, 96, offsets[0]) ||
        !load_u64(artifact, 104, checkpoint_bytes) ||
        !load_u64(artifact, 112, checkpoint_hash) ||
        !load_u64(artifact, 120, offsets[1]) ||
        !load_u64(artifact, 128, nested_bytes) ||
        !load_u64(artifact, 136, nested_hash) || !load_u64(artifact, 144, offsets[2]) ||
        !load_u64(artifact, 152, first_action) ||
        !load_u64(artifact, 160, last_action) ||
        !load_u32(artifact, 168, action_count) ||
        !load_u32(artifact, 172, action_stride) ||
        !load_u64(artifact, 176, offsets[3]) ||
        !load_u32(artifact, 184, generation_count) ||
        !load_u32(artifact, 188, generation_stride) ||
        !load_u64(artifact, 192, offsets[4]) ||
        !load_u32(artifact, 200, record_count) ||
        !load_u32(artifact, 204, record_stride) ||
        !load_u64(artifact, 208, offsets[5]) ||
        !load_u64(artifact, 216, payload_bytes) ||
        !load_u32(artifact, 224, nested_kind) ||
        !load_u32(artifact, 228, determinism) ||
        !load_u64(artifact, 360, action_hash) ||
        !load_u64(artifact, 368, generation_hash) ||
        !load_u64(artifact, 376, record_payload_hash) ||
        !((magic == kMagic && schema == live_control_replay_schema_version &&
           header == live_control_replay_header_size) ||
          (magic == kMagicV2 && schema == live_control_lossless_replay_schema_version &&
           header == live_control_lossless_replay_header_size &&
           artifact.size() >= header)) ||
        total != artifact.size() || whole != replay_artifact_checksum(artifact) ||
        action_count == 0 || action_stride != live_control_replay_action_bytes ||
        generation_stride != live_control_replay_generation_bytes ||
        record_stride != live_control_replay_record_bytes ||
        last_action < first_action ||
        last_action == std::numeric_limits<std::uint64_t>::max() ||
        last_action - first_action + 1 != action_count ||
        determinism >
            static_cast<std::uint32_t>(DeterminismTier::portable_deterministic)) {
        return Status::invalid_artifact;
    }
    std::size_t checkpoint_end = 0;
    std::size_t nested_end = 0;
    std::size_t action_bytes = 0;
    std::size_t action_end = 0;
    std::size_t generation_bytes = 0;
    std::size_t generation_end = 0;
    std::size_t record_bytes = 0;
    std::size_t record_end = 0;
    std::size_t payload_end = 0;
    const bool lossless = schema == live_control_lossless_replay_schema_version;
    if (offsets[0] != header ||
        !add_size(offsets[0], checkpoint_bytes, checkpoint_end) ||
        checkpoint_end != offsets[1] ||
        !add_size(offsets[1], nested_bytes, nested_end) || nested_end != offsets[2] ||
        !multiply_size(action_count, action_stride, action_bytes) ||
        !add_size(offsets[2], action_bytes, action_end) || action_end != offsets[3] ||
        !multiply_size(generation_count, generation_stride, generation_bytes) ||
        !add_size(offsets[3], generation_bytes, generation_end) ||
        generation_end != offsets[4] ||
        !multiply_size(record_count, record_stride, record_bytes) ||
        !add_size(offsets[4], record_bytes, record_end) || record_end != offsets[5] ||
        !add_size(offsets[5], payload_bytes, payload_end) ||
        payload_end > artifact.size() ||
        (!lossless && payload_end != artifact.size())) {
        return Status::invalid_artifact;
    }
    const auto checkpoint = artifact.subspan(offsets[0], checkpoint_bytes);
    const auto nested = artifact.subspan(offsets[1], nested_bytes);
    CheckpointMetadata checkpoint_metadata;
    const auto kind = static_cast<LiveControlNestedArtifactKind>(nested_kind);
    if (checksum(checkpoint) != checkpoint_hash ||
        checksum(nested) != nested_hash ||
        checksum(artifact.subspan(offsets[2], action_bytes)) != action_hash ||
        checksum(artifact.subspan(offsets[3], generation_bytes)) !=
            generation_hash ||
        checksum(artifact.subspan(offsets[4], record_bytes + payload_bytes)) !=
            record_payload_hash ||
        inspect_checkpoint_artifact(checkpoint, checkpoint_metadata) != Status::ok ||
        !nested_valid(kind, nested)) {
        return Status::invalid_artifact;
    }
    std::uint64_t checkpoint_generation = 0;
    bool found_live_control = false;
    CheckpointRecordCursor checkpoint_cursor;
    for (std::size_t index = 0;
         index < checkpoint_metadata.state_count;
         ++index) {
        CheckpointRecordView record;
        if (!next_checkpoint_record(
                checkpoint,
                checkpoint_metadata,
                checkpoint_cursor,
                record)) {
            return Status::invalid_artifact;
        }
        if (record.name != "rtfw.live-control") {
            continue;
        }
        std::uint32_t has_generation = 0;
        if (found_live_control ||
            record.schema_version != live_control_action_schema_version ||
            !load_u32(record.payload, 144, has_generation) ||
            !load_u64(record.payload, 152, checkpoint_generation) ||
            has_generation > 1 ||
            (has_generation == 0 && checkpoint_generation != 0) ||
            (has_generation != 0 && checkpoint_generation == 0)) {
            return Status::invalid_artifact;
        }
        found_live_control = true;
    }
    if (!found_live_control) {
        return Status::invalid_artifact;
    }
    LiveControlReplayMetadata metadata;
    metadata.header_size = header;
    metadata.total_bytes = total;
    metadata.artifact_checksum = whole;
    (void)load_u64(artifact, 32, metadata.runtime_id);
    (void)load_u64(artifact, 40, metadata.configuration_generation);
    (void)load_u64(artifact, 48, metadata.config_id);
    (void)load_u64(artifact, 56, metadata.replay_id);
    (void)load_u64(artifact, 64, metadata.graph_id);
    (void)load_u64(artifact, 72, metadata.state_schema_id);
    (void)load_u64(artifact, 80, metadata.policy_identity);
    (void)load_u64(artifact, 88, metadata.final_state_hash);
    metadata.checkpoint_bytes = checkpoint_bytes;
    metadata.nested_artifact_bytes = nested_bytes;
    metadata.first_action_sequence = first_action;
    metadata.last_action_sequence = last_action;
    metadata.action_record_count = action_count;
    metadata.retained_generation_count = generation_count;
    metadata.retained_record_count = record_count;
    metadata.nested_kind = kind;
    metadata.determinism_tier = static_cast<DeterminismTier>(determinism);
    std::memcpy(metadata.build_id.data(), artifact.data() + 232,
                metadata.build_id.size());
    std::memcpy(metadata.workload_id.data(), artifact.data() + 296,
                metadata.workload_id.size());
    if (metadata.runtime_id == 0 || metadata.configuration_generation == 0 ||
        metadata.policy_identity == 0 || payload_bytes >
            std::numeric_limits<std::uint32_t>::max() ||
        !identifier_valid(metadata.build_id) ||
        !identifier_valid(metadata.workload_id)) {
        return Status::invalid_artifact;
    }
    metadata.schema_version = schema;
    metadata.retained_payload_bytes = static_cast<std::uint32_t>(payload_bytes);
    view = {
        metadata,
        artifact,
        checkpoint,
        nested,
        static_cast<std::size_t>(offsets[2]),
        static_cast<std::size_t>(offsets[3]),
        static_cast<std::size_t>(offsets[4]),
        static_cast<std::size_t>(offsets[5]),
    };
    if (lossless) {
        std::uint64_t identity = 0, admission_offset = 0, payload_offset = 0;
        std::uint64_t bytes = 0, journal_hash = 0, closed = 0;
        std::uint32_t count = 0, stride = 0;
        std::size_t descriptors = 0, journal_end = 0, end = 0;
        if (!load_u64(artifact, 384, identity) || identity == 0 ||
            !load_u64(artifact, 392, admission_offset) ||
            admission_offset != payload_end || !load_u32(artifact, 400, count) ||
            count > action_count || !load_u32(artifact, 404, stride) ||
            stride != live_control_replay_admission_bytes ||
            !load_u64(artifact, 408, payload_offset) ||
            !load_u64(artifact, 416, bytes) || !load_u64(artifact, 424, journal_hash) ||
            !load_u64(artifact, 432, closed) || !zero_range(artifact, 440, 448) ||
            (closed != std::numeric_limits<std::uint64_t>::max() &&
             closed > last_action + 1) ||
            !multiply_size(count, stride, descriptors) ||
            !add_size(admission_offset, descriptors, journal_end) ||
            journal_end != payload_offset || !add_size(payload_offset, bytes, end) ||
            end != artifact.size() ||
            checksum(artifact.subspan(admission_offset)) != journal_hash) {
            view = {};
            return Status::invalid_artifact;
        }
        view.retention_policy_identity = identity;
        view.admission_offset = static_cast<std::size_t>(admission_offset);
        view.admission_count = count;
        view.admission_payload_offset = static_cast<std::size_t>(payload_offset);
        view.admission_payload_bytes = static_cast<std::size_t>(bytes);
        view.admission_close_sequence = closed;
        std::size_t next_admission = 0, next_payload_offset = 0;
        for (std::size_t index = 0; index < action_count; ++index) {
            LiveControlActionRecord action;
            if (!live_control_replay_action_at(view, index, action)) {
                view = {};
                return Status::invalid_artifact;
            }
            if (action.action != LiveControlActionId::admission) {
                continue;
            }
            LiveControlRetainedAdmission admission;
            std::span<const std::byte> payload;
            if (!live_control_replay_admission_at(view, next_admission++, admission,
                                                  payload) ||
                admission.action_sequence != action.sequence ||
                admission.outcome != action.admission_result ||
                admission.payload_offset != next_payload_offset ||
                ((admission.outcome == LiveControlAdmissionResult::accepted ||
                  admission.outcome == LiveControlAdmissionResult::missed) &&
                 (admission.record.mailbox_identity != action.mailbox_identity ||
                  admission.record.producer_identity != action.producer_identity ||
                  admission.record.mailbox_sequence != action.mailbox_sequence ||
                  admission.record.producer_sequence != action.producer_sequence ||
                  admission.record.payload_digest != action.payload_digest ||
                  admission.record.payload_bytes != action.payload_bytes))) {
                view = {};
                return Status::invalid_artifact;
            }
            next_payload_offset += payload.size();
        }
        if (next_admission != count || next_payload_offset != bytes) {
            view = {};
            return Status::invalid_artifact;
        }
    }
    std::uint64_t current_generation = checkpoint_generation;
    std::size_t next_record = 0;
    std::size_t next_payload = 0;
    std::size_t publication_count = 0;
    LiveControlActionRecord first_range_action;
    for (std::size_t index = 0; index < action_count; ++index) {
        LiveControlActionRecord action;
        if (!live_control_replay_action_at(view, index, action) ||
            action.sequence != first_action + index ||
            action.policy_identity != metadata.policy_identity) {
            view = {};
            return Status::invalid_artifact;
        }
        if (index == 0) {
            first_range_action = action;
        }
        publication_count += action.action ==
            LiveControlActionId::provisional_publication ? 1u : 0u;
    }
    if (publication_count != generation_count ||
        first_range_action.action != LiveControlActionId::checkpointed ||
        first_range_action.checkpoint_correlation !=
            checkpoint_metadata.artifact_checksum) {
        view = {};
        return Status::invalid_artifact;
    }
    for (std::size_t generation_index = 0;
         generation_index < generation_count;
         ++generation_index) {
        LiveControlRetainedGenerationView generation;
        const auto generation_descriptor = view.artifact.subspan(
            view.generation_offset +
                generation_index * live_control_replay_generation_bytes,
            live_control_replay_generation_bytes);
        std::uint64_t descriptor_first_record = 0;
        std::uint64_t descriptor_first_payload = 0;
        if (!live_control_replay_generation_at(view, generation_index, generation) ||
            !load_u64(generation_descriptor, 64, descriptor_first_record) ||
            !load_u64(generation_descriptor, 72, descriptor_first_payload) ||
            !generation.settled || generation.record_count == 0 ||
            generation.prior_generation_identity != current_generation ||
            descriptor_first_record != next_record ||
            descriptor_first_payload != next_payload ||
            generation.first_action_sequence < first_action ||
            generation.first_action_sequence > last_action) {
            view = {};
            return Status::invalid_artifact;
        }
        const auto action_index = static_cast<std::size_t>(
            generation.first_action_sequence - first_action);
        LiveControlActionRecord publication;
        if (!live_control_replay_action_at(view, action_index, publication) ||
            publication.action !=
                LiveControlActionId::provisional_publication ||
            publication.generation_identity != generation.generation_identity ||
            publication.prior_generation_identity !=
                generation.prior_generation_identity ||
            publication.survivor_count != generation.record_count ||
            !target_equal(publication.target, generation.target)) {
            view = {};
            return Status::invalid_artifact;
        }
        std::size_t committed_actions = 0;
        std::size_t replaced_actions = 0;
        std::size_t rolled_back_actions = 0;
        std::uint64_t previous_terminal_mailbox = 0;
        std::uint64_t previous_terminal_sequence = 0;
        bool has_terminal = false;
        for (std::size_t index = 0; index < action_count; ++index) {
            LiveControlActionRecord terminal;
            if (!live_control_replay_action_at(view, index, terminal)) {
                view = {};
                return Status::invalid_artifact;
            }
            if (terminal.generation_identity !=
                    generation.generation_identity ||
                terminal.stage != LiveControlActionStage::terminal) {
                continue;
            }
            if (terminal.action != LiveControlActionId::committed &&
                terminal.action != LiveControlActionId::replaced &&
                terminal.action != LiveControlActionId::rolled_back) {
                continue;
            }
            if (!target_equal(terminal.target, generation.target) ||
                terminal.terminal_status != static_cast<std::int32_t>(
                    generation.terminal_status) ||
                terminal.mailbox_sequence ==
                    std::numeric_limits<std::uint64_t>::max() ||
                terminal.producer_sequence ==
                    std::numeric_limits<std::uint64_t>::max() ||
                (has_terminal &&
                 (terminal.mailbox_identity < previous_terminal_mailbox ||
                  (terminal.mailbox_identity == previous_terminal_mailbox &&
                   terminal.mailbox_sequence <=
                       previous_terminal_sequence)))) {
                view = {};
                return Status::invalid_artifact;
            }
            previous_terminal_mailbox = terminal.mailbox_identity;
            previous_terminal_sequence = terminal.mailbox_sequence;
            has_terminal = true;
            committed_actions += terminal.action ==
                LiveControlActionId::committed ? 1u : 0u;
            replaced_actions += terminal.action ==
                LiveControlActionId::replaced ? 1u : 0u;
            rolled_back_actions += terminal.action ==
                LiveControlActionId::rolled_back ? 1u : 0u;
        }
        std::uint64_t identity = kFnvOffset;
        hash_target(identity, generation.target);
        std::uint64_t previous_mailbox = 0;
        std::uint64_t previous_sequence = 0;
        std::size_t observed_payload = 0;
        for (std::size_t record_index = 0;
             record_index < generation.record_count;
             ++record_index, ++next_record) {
            LiveControlUpdateRecord record;
            std::span<const std::byte> payload;
            if (!live_control_replay_record_at(view, generation_index, record_index,
                                               record, payload) ||
                record.runtime_id != 0 || record.configuration_generation != 0 ||
                record.schema_version != live_control_schema_version ||
                record.record_size != sizeof(LiveControlUpdateRecord) ||
                record.mailbox_sequence == 0 || record.mailbox_identity == 0 ||
                record.producer_identity == 0 || record.producer_sequence == 0 ||
                record.payload_bytes != payload.size() ||
                record.payload_alignment == 0 ||
                record.payload_alignment > live_control_payload_alignment_limit ||
                (record.payload_alignment & (record.payload_alignment - 1u)) != 0 ||
                payload.size() % record.payload_alignment != 0 ||
                record.policy_flags != live_control_payload_canonical_little_endian ||
                record.update_kind < LiveControlUpdateKind::scenario_parameters ||
                record.update_kind > LiveControlUpdateKind::clear_fault ||
                (record.payload_bytes == 0 &&
                 record.update_kind != LiveControlUpdateKind::clear_fault) ||
                !std::all_of(record.reserved.begin(), record.reserved.end(),
                             [](std::byte value) { return value == std::byte{0}; }) ||
                live_control_payload_digest(payload) != record.payload_digest ||
                (record.target_kind != generation.target.kind) ||
                (record.target_kind == LiveControlTargetKind::host_frame
                     ? record.target_frame_index != generation.target.frame_index
                     : record.reference_release_index !=
                               generation.target.reference_release_index ||
                           record.rate_release_sequence !=
                               generation.target.rate_release_sequence) ||
                (record_index != 0 &&
                 (record.mailbox_identity < previous_mailbox ||
                  (record.mailbox_identity == previous_mailbox &&
                   record.mailbox_sequence <= previous_sequence)))) {
                view = {};
                return Status::invalid_artifact;
            }
            std::size_t matching_terminal = 0;
            for (std::size_t action = 0; action < action_count; ++action) {
                LiveControlActionRecord terminal;
                if (!live_control_replay_action_at(view, action, terminal)) {
                    view = {};
                    return Status::invalid_artifact;
                }
                const auto expected_action = generation.terminal_status ==
                        Status::ok
                    ? LiveControlActionId::committed
                    : LiveControlActionId::rolled_back;
                if (terminal.action == expected_action &&
                    terminal.generation_identity ==
                        generation.generation_identity &&
                    terminal.mailbox_identity == record.mailbox_identity &&
                    terminal.mailbox_sequence == record.mailbox_sequence &&
                    terminal.producer_identity == record.producer_identity &&
                    terminal.producer_sequence == record.producer_sequence &&
                    terminal.update_kind == record.update_kind &&
                    terminal.payload_bytes == record.payload_bytes &&
                    terminal.payload_digest == record.payload_digest) {
                    ++matching_terminal;
                }
            }
            if (matching_terminal != 1) {
                view = {};
                return Status::invalid_artifact;
            }
            hash_u64(identity, record.mailbox_identity);
            hash_u64(identity, record.mailbox_sequence);
            hash_u64(identity, record.producer_identity);
            hash_u64(identity, record.producer_sequence);
            hash_u64(identity, static_cast<std::uint8_t>(record.update_kind));
            hash_u64(identity, record.payload_digest);
            previous_mailbox = record.mailbox_identity;
            previous_sequence = record.mailbox_sequence;
            observed_payload += payload.size();
            next_payload += payload.size();
        }
        if (identity == 0) {
            identity = 1;
        }
        if (identity != generation.generation_identity ||
            observed_payload != generation.payload_bytes ||
            (generation.terminal_status == Status::ok
                ? committed_actions != generation.record_count ||
                    replaced_actions != publication.replaced_count ||
                    rolled_back_actions != 0
                : committed_actions != 0 || replaced_actions != 0 ||
                    rolled_back_actions !=
                        static_cast<std::size_t>(generation.record_count) +
                            publication.replaced_count)) {
            view = {};
            return Status::invalid_artifact;
        }
        if (generation.terminal_status == Status::ok) {
            current_generation = generation.generation_identity;
        }
    }
    for (std::size_t index = 0; index < action_count; ++index) {
        LiveControlActionRecord admission;
        if (!live_control_replay_action_at(view, index, admission)) {
            view = {};
            return Status::invalid_artifact;
        }
        if (!lossless && admission.action == LiveControlActionId::admission &&
            admission.admission_result == LiveControlAdmissionResult::accepted) {
            std::size_t matching_terminal = 0;
            for (std::size_t terminal_index = 0;
                 terminal_index < action_count;
                 ++terminal_index) {
                LiveControlActionRecord terminal;
                if (!live_control_replay_action_at(
                        view, terminal_index, terminal)) {
                    view = {};
                    return Status::invalid_artifact;
                }
                if ((terminal.action == LiveControlActionId::committed ||
                     terminal.action == LiveControlActionId::replaced ||
                     terminal.action == LiveControlActionId::rolled_back) &&
                    target_equal(terminal.target, admission.target) &&
                    terminal.mailbox_identity == admission.mailbox_identity &&
                    terminal.producer_identity == admission.producer_identity &&
                    terminal.mailbox_sequence == admission.mailbox_sequence &&
                    terminal.producer_sequence == admission.producer_sequence &&
                    terminal.update_kind == admission.update_kind &&
                    terminal.payload_bytes == admission.payload_bytes &&
                    terminal.payload_digest == admission.payload_digest) {
                    ++matching_terminal;
                }
            }
            if (matching_terminal != 1) {
                view = {};
                return Status::invalid_artifact;
            }
        }
        if (!lossless && admission.action == LiveControlActionId::admission &&
            admission.admission_result == LiveControlAdmissionResult::missed) {
            LiveControlActionRecord missed;
            if (index + 1 >= action_count ||
                !live_control_replay_action_at(view, index + 1, missed) ||
                missed.action != LiveControlActionId::missed ||
                !target_equal(missed.target, admission.target) ||
                missed.mailbox_identity != admission.mailbox_identity ||
                missed.producer_identity != admission.producer_identity ||
                missed.mailbox_sequence != admission.mailbox_sequence ||
                missed.producer_sequence != admission.producer_sequence ||
                missed.update_kind != admission.update_kind ||
                missed.payload_bytes != admission.payload_bytes ||
                missed.payload_digest != admission.payload_digest) {
                view = {};
                return Status::invalid_artifact;
            }
        }
    }
    if (next_record != record_count || next_payload != payload_bytes) {
        view = {};
        return Status::invalid_artifact;
    }
    return Status::ok;
}

bool live_control_replay_admission_at(const LiveControlReplayArtifactView& view,
                                      std::size_t index,
                                      LiveControlRetainedAdmission& admission,
                                      std::span<const std::byte>& payload) noexcept {
    admission = {};
    payload = {};
    if (view.metadata.schema_version != live_control_lossless_replay_schema_version ||
        index >= view.admission_count) {
        return false;
    }
    const auto offset =
        view.admission_offset + index * live_control_replay_admission_bytes;
    std::uint32_t bytes = 0;
    std::uint64_t stored = 0;
    std::memcpy(&admission.record, view.artifact.data() + offset,
                sizeof(admission.record));
    if (!load_u64(view.artifact, offset + 128, admission.action_sequence) ||
        !load_u64(view.artifact, offset + 136, admission.mailbox_identity) ||
        !load_u64(view.artifact, offset + 144, admission.producer_identity) ||
        !load_u32(view.artifact, offset + 152, admission.slot_index) ||
        !load_u64(view.artifact, offset + 160, admission.payload_offset) ||
        !load_u32(view.artifact, offset + 168, bytes) ||
        !load_u64(view.artifact, offset + 176, stored) ||
        !zero_range(view.artifact, offset + 158, offset + 160) ||
        !zero_range(view.artifact, offset + 172, offset + 176) ||
        admission.record.runtime_id != 0 ||
        admission.record.configuration_generation != 0 ||
        admission.payload_offset > view.admission_payload_bytes ||
        bytes > view.admission_payload_bytes - admission.payload_offset) {
        return false;
    }
    const auto outcome = std::to_integer<unsigned>(view.artifact[offset + 156]);
    const auto changed = std::to_integer<unsigned>(view.artifact[offset + 157]);
    if (outcome < 1 || outcome > 8 || changed > 1) {
        return false;
    }
    admission.outcome = static_cast<LiveControlAdmissionResult>(outcome);
    admission.counter_changed = changed != 0;
    const bool assigned =
        admission.slot_index != std::numeric_limits<std::uint32_t>::max();
    if (admission.counter_changed
            ? (admission.mailbox_identity == 0 || admission.producer_identity == 0)
            : (admission.mailbox_identity != 0 || admission.producer_identity != 0 ||
               assigned)) {
        return false;
    }
    payload = view.artifact.subspan(
        view.admission_payload_offset + admission.payload_offset, bytes);
    if (assigned
            ? (bytes != admission.record.payload_bytes ||
               live_control_payload_digest(payload) != admission.record.payload_digest)
            : bytes != 0) {
        return false;
    }
    auto hash = kFnvOffset;
    hash_bytes(hash, view.artifact.subspan(offset, 176));
    hash_bytes(hash, payload);
    return hash == stored;
}

bool live_control_replay_action_at(
    const LiveControlReplayArtifactView& view,
    std::size_t index,
    LiveControlActionRecord& action) noexcept {
    action = {};
    if (index >= view.metadata.action_record_count) {
        return false;
    }
    const auto offset = view.action_offset +
        index * live_control_replay_action_bytes;
    std::uint64_t stored_checksum = 0;
    std::memcpy(&action, view.artifact.data() + offset, sizeof(action));
    return load_u64(view.artifact, offset + sizeof(action), stored_checksum) &&
        checksum(view.artifact.subspan(offset, sizeof(action))) ==
            stored_checksum &&
        action.runtime_id == 0 && action.configuration_generation == 0 &&
        [&]() {
            auto candidate = action;
            candidate.runtime_id = view.metadata.runtime_id;
            candidate.configuration_generation =
                view.metadata.configuration_generation;
            return live_control_action_valid(candidate);
        }();
}

bool live_control_replay_generation_at(
    const LiveControlReplayArtifactView& view,
    std::size_t index,
    LiveControlRetainedGenerationView& generation) noexcept {
    generation = {};
    if (index >= view.metadata.retained_generation_count) {
        return false;
    }
    const auto offset = view.generation_offset +
        index * live_control_replay_generation_bytes;
    const auto descriptor = view.artifact.subspan(
        offset, live_control_replay_generation_bytes);
    std::uint64_t first_record = 0;
    std::uint64_t first_payload = 0;
    std::uint32_t record_count = 0;
    std::uint32_t payload_bytes = 0;
    std::uint32_t terminal = 0;
    if (!descriptor_target(descriptor, generation.target) ||
        !load_u64(descriptor, 40, generation.generation_identity) ||
        !load_u64(descriptor, 48, generation.prior_generation_identity) ||
        !load_u64(descriptor, 56, generation.first_action_sequence) ||
        !load_u64(descriptor, 64, first_record) ||
        !load_u64(descriptor, 72, first_payload) ||
        !load_u32(descriptor, 80, record_count) ||
        !load_u32(descriptor, 84, payload_bytes) ||
        !load_u32(descriptor, 88, terminal) ||
        first_record > view.metadata.retained_record_count ||
        record_count > view.metadata.retained_record_count - first_record ||
        first_payload > view.metadata.retained_payload_bytes ||
        payload_bytes > view.metadata.retained_payload_bytes - first_payload ||
        static_cast<std::int32_t>(terminal) >
            static_cast<std::int32_t>(Status::ok) ||
        static_cast<std::int32_t>(terminal) <
            static_cast<std::int32_t>(Status::incompatible_abi) ||
        descriptor[92] > std::byte{1}) {
        return false;
    }
    generation.record_count = record_count;
    generation.payload_bytes = payload_bytes;
    generation.terminal_status = static_cast<Status>(
        static_cast<std::int32_t>(terminal));
    generation.settled = descriptor[92] == std::byte{1};
    return true;
}

bool live_control_replay_record_at(
    const LiveControlReplayArtifactView& view,
    std::size_t generation_index,
    std::size_t record_index,
    LiveControlUpdateRecord& record,
    std::span<const std::byte>& payload) noexcept {
    record = {};
    payload = {};
    LiveControlRetainedGenerationView generation;
    if (!live_control_replay_generation_at(
            view, generation_index, generation) ||
        record_index >= generation.record_count) {
        return false;
    }
    const auto generation_descriptor = view.artifact.subspan(
        view.generation_offset +
            generation_index * live_control_replay_generation_bytes,
        live_control_replay_generation_bytes);
    std::uint64_t first_record = 0;
    (void)load_u64(generation_descriptor, 64, first_record);
    const auto global_index = first_record + record_index;
    const auto offset = view.record_offset +
        static_cast<std::size_t>(global_index) *
            live_control_replay_record_bytes;
    std::uint64_t payload_offset = 0;
    std::uint32_t payload_bytes = 0;
    std::uint64_t stored_checksum = 0;
    std::memcpy(&record, view.artifact.data() + offset, sizeof(record));
    if (!load_u64(view.artifact, offset + 128, payload_offset) ||
        !load_u32(view.artifact, offset + 136, payload_bytes) ||
        !load_u64(view.artifact, offset + 144, stored_checksum) ||
        !zero_range(view.artifact, offset + 140, offset + 144) ||
        payload_offset > view.metadata.retained_payload_bytes ||
        payload_bytes > view.metadata.retained_payload_bytes - payload_offset) {
        return false;
    }
    payload = view.artifact.subspan(
        view.payload_offset + payload_offset, payload_bytes);
    auto hash = kFnvOffset;
    hash_bytes(hash, view.artifact.subspan(offset, 144));
    hash_bytes(hash, payload);
    return hash == stored_checksum;
}

} // namespace rt::detail

namespace rt {

Status inspect_live_control_replay_artifact(
    std::span<const std::byte> artifact,
    LiveControlReplayMetadata& metadata) noexcept {
    detail::LiveControlReplayArtifactView view;
    const auto status = detail::parse_live_control_replay_artifact(
        artifact, live_control_replay_absolute_max_bytes, view);
    metadata = status == Status::ok ? view.metadata : LiveControlReplayMetadata{};
    return status;
}

} // namespace rt
