#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

#include <rt/canonical_bytes.hpp>
#include <rt/runtime.hpp>

namespace rt {

inline constexpr std::uint32_t live_control_typed_envelope_magic =
    0x434c'5452u; // "RTLC" in canonical little-endian byte order.
inline constexpr std::uint32_t live_control_typed_envelope_version = 1;
inline constexpr std::size_t live_control_typed_envelope_bytes = 32;

enum class LiveControlTypedStatus : std::uint8_t {
    ok = 0,
    invalid_argument = 1,
    invalid_handle = 2,
    invalid_target = 3,
    invalid_options = 4,
    codec_rejected = 5,
    malformed_envelope = 6,
    type_mismatch = 7,
    schema_mismatch = 8,
    kind_mismatch = 9,
    digest_mismatch = 10,
};

struct LiveControlTypedEnvelopeMetadata {
    std::uint32_t magic = 0;
    std::uint32_t envelope_version = 0;
    std::uint32_t total_encoded_bytes = 0;
    std::uint32_t application_type_identity = 0;
    std::uint32_t application_schema_version = 0;
    LiveControlUpdateKind update_kind =
        LiveControlUpdateKind::scenario_parameters;
};

struct LiveControlTypedRecordOptions {
    std::uint32_t payload_alignment = 1;
    std::uint32_t policy_flags =
        live_control_payload_canonical_little_endian;
};

// Applications specialize this class for each fixed payload type. The codec
// must write fields individually in canonical order; copying a native object
// representation is outside this contract.
template <typename T>
struct LiveControlTypeTraits;

namespace detail {

[[nodiscard]] constexpr bool live_control_update_kind_valid(
    LiveControlUpdateKind kind) noexcept {
    return kind >= LiveControlUpdateKind::scenario_parameters &&
        kind <= LiveControlUpdateKind::clear_fault;
}

template <typename T>
concept LiveControlCodecShape = requires(
    const T& input,
    T& output,
    std::span<
        std::byte,
        static_cast<std::size_t>(LiveControlTypeTraits<T>::encoded_extent)>
        encoded,
    std::span<
        const std::byte,
        static_cast<std::size_t>(LiveControlTypeTraits<T>::encoded_extent)>
        canonical) {
    { LiveControlTypeTraits<T>::application_type_identity } ->
        std::convertible_to<std::uint32_t>;
    { LiveControlTypeTraits<T>::application_schema_version } ->
        std::convertible_to<std::uint32_t>;
    { LiveControlTypeTraits<T>::update_kind } ->
        std::convertible_to<LiveControlUpdateKind>;
    { LiveControlTypeTraits<T>::encoded_extent } ->
        std::convertible_to<std::size_t>;
    { LiveControlTypeTraits<T>::validate(input) } noexcept ->
        std::same_as<bool>;
    { LiveControlTypeTraits<T>::encode(input, encoded) } noexcept ->
        std::same_as<bool>;
    { LiveControlTypeTraits<T>::decode(canonical, output) } noexcept ->
        std::same_as<bool>;
};

[[nodiscard]] inline bool live_control_typed_options_valid(
    std::size_t payload_bytes,
    LiveControlTypedRecordOptions options) noexcept {
    const auto alignment = options.payload_alignment;
    return alignment != 0 &&
        alignment <= live_control_payload_alignment_limit &&
        (alignment & (alignment - 1u)) == 0 &&
        (payload_bytes % alignment) == 0 &&
        options.policy_flags ==
            live_control_payload_canonical_little_endian;
}

[[nodiscard]] inline bool live_control_target_reserved_zero(
    const LiveControlBoundaryTarget& target) noexcept {
    return std::all_of(
        target.reserved.begin(),
        target.reserved.end(),
        [](std::byte value) noexcept { return value == std::byte{0}; });
}

} // namespace detail

template <typename T>
concept LiveControlFixedType =
    detail::LiveControlCodecShape<T> &&
    !std::is_pointer_v<T> &&
    !std::is_array_v<T> &&
    std::is_standard_layout_v<T> &&
    std::is_trivially_copyable_v<T> &&
    std::is_nothrow_default_constructible_v<T> &&
    std::is_nothrow_copy_assignable_v<T> &&
    static_cast<std::uint32_t>(
        LiveControlTypeTraits<T>::application_type_identity) != 0 &&
    static_cast<std::uint32_t>(
        LiveControlTypeTraits<T>::application_schema_version) != 0 &&
    detail::live_control_update_kind_valid(
        static_cast<LiveControlUpdateKind>(
            LiveControlTypeTraits<T>::update_kind)) &&
    static_cast<std::size_t>(LiveControlTypeTraits<T>::encoded_extent) != 0 &&
    static_cast<std::size_t>(LiveControlTypeTraits<T>::encoded_extent) <=
        live_control_payload_bytes_limit - live_control_typed_envelope_bytes;

template <LiveControlFixedType T>
inline constexpr std::size_t live_control_typed_payload_extent =
    live_control_typed_envelope_bytes +
    static_cast<std::size_t>(LiveControlTypeTraits<T>::encoded_extent);

template <LiveControlFixedType T>
using LiveControlTypedPayload =
    std::array<std::byte, live_control_typed_payload_extent<T>>;

[[nodiscard]] inline LiveControlTypedStatus inspect_live_control_typed_envelope(
    std::span<const std::byte> payload,
    LiveControlTypedEnvelopeMetadata& output) noexcept {
    if (payload.size() < live_control_typed_envelope_bytes ||
        payload.size() > live_control_payload_bytes_limit ||
        payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return LiveControlTypedStatus::malformed_envelope;
    }

    LiveControlTypedEnvelopeMetadata candidate;
    std::uint32_t kind = 0;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    if (!load_u32_le(payload, 0, candidate.magic) ||
        !load_u32_le(payload, 4, candidate.envelope_version) ||
        !load_u32_le(payload, 8, candidate.total_encoded_bytes) ||
        !load_u32_le(payload, 12, candidate.application_type_identity) ||
        !load_u32_le(payload, 16, candidate.application_schema_version) ||
        !load_u32_le(payload, 20, kind) ||
        !load_u32_le(payload, 24, reserved0) ||
        !load_u32_le(payload, 28, reserved1) ||
        candidate.magic != live_control_typed_envelope_magic ||
        candidate.envelope_version != live_control_typed_envelope_version ||
        candidate.total_encoded_bytes != payload.size() ||
        candidate.application_type_identity == 0 ||
        candidate.application_schema_version == 0 ||
        kind < static_cast<std::uint32_t>(
            LiveControlUpdateKind::scenario_parameters) ||
        kind > static_cast<std::uint32_t>(LiveControlUpdateKind::clear_fault) ||
        reserved0 != 0 || reserved1 != 0) {
        return LiveControlTypedStatus::malformed_envelope;
    }
    candidate.update_kind = static_cast<LiveControlUpdateKind>(kind);
    output = candidate;
    return LiveControlTypedStatus::ok;
}

template <LiveControlFixedType T>
[[nodiscard]] LiveControlTypedStatus encode_live_control_typed_payload(
    const T& value,
    LiveControlTypedPayload<T>& output) noexcept {
    if (!LiveControlTypeTraits<T>::validate(value)) {
        return LiveControlTypedStatus::codec_rejected;
    }

    std::fill(output.begin(), output.end(), std::byte{0});
    const auto type_identity = static_cast<std::uint32_t>(
        LiveControlTypeTraits<T>::application_type_identity);
    const auto schema_version = static_cast<std::uint32_t>(
        LiveControlTypeTraits<T>::application_schema_version);
    const auto kind = static_cast<std::uint32_t>(
        LiveControlTypeTraits<T>::update_kind);
    const auto total = static_cast<std::uint32_t>(output.size());
    if (!store_u32_le(output, 0, live_control_typed_envelope_magic) ||
        !store_u32_le(output, 4, live_control_typed_envelope_version) ||
        !store_u32_le(output, 8, total) ||
        !store_u32_le(output, 12, type_identity) ||
        !store_u32_le(output, 16, schema_version) ||
        !store_u32_le(output, 20, kind) ||
        !store_u32_le(output, 24, 0) ||
        !store_u32_le(output, 28, 0)) {
        return LiveControlTypedStatus::malformed_envelope;
    }
    std::span<
        std::byte,
        static_cast<std::size_t>(LiveControlTypeTraits<T>::encoded_extent)>
        body{output.data() + live_control_typed_envelope_bytes,
             static_cast<std::size_t>(
                 LiveControlTypeTraits<T>::encoded_extent)};
    return LiveControlTypeTraits<T>::encode(value, body)
        ? LiveControlTypedStatus::ok
        : LiveControlTypedStatus::codec_rejected;
}

template <LiveControlFixedType T>
[[nodiscard]] LiveControlTypedStatus decode_live_control_typed_payload(
    const LiveControlUpdateRecord& record,
    std::span<const std::byte> payload,
    T& output) noexcept {
    constexpr auto expected_extent = live_control_typed_payload_extent<T>;
    constexpr auto expected_kind = static_cast<LiveControlUpdateKind>(
        LiveControlTypeTraits<T>::update_kind);
    if (record.schema_version != live_control_schema_version ||
        record.record_size != sizeof(LiveControlUpdateRecord) ||
        record.payload_bytes != expected_extent ||
        payload.size() != expected_extent ||
        record.policy_flags !=
            live_control_payload_canonical_little_endian) {
        return LiveControlTypedStatus::malformed_envelope;
    }
    if (record.update_kind != expected_kind) {
        return LiveControlTypedStatus::kind_mismatch;
    }
    if (record.payload_digest != live_control_payload_digest(payload)) {
        return LiveControlTypedStatus::digest_mismatch;
    }

    LiveControlTypedEnvelopeMetadata envelope;
    const auto inspection = inspect_live_control_typed_envelope(
        payload, envelope);
    if (inspection != LiveControlTypedStatus::ok) {
        return inspection;
    }
    if (envelope.application_type_identity != static_cast<std::uint32_t>(
            LiveControlTypeTraits<T>::application_type_identity)) {
        return LiveControlTypedStatus::type_mismatch;
    }
    if (envelope.application_schema_version != static_cast<std::uint32_t>(
            LiveControlTypeTraits<T>::application_schema_version)) {
        return LiveControlTypedStatus::schema_mismatch;
    }
    if (envelope.update_kind != expected_kind) {
        return LiveControlTypedStatus::kind_mismatch;
    }

    std::span<
        const std::byte,
        static_cast<std::size_t>(LiveControlTypeTraits<T>::encoded_extent)>
        body{payload.data() + live_control_typed_envelope_bytes,
             static_cast<std::size_t>(
                 LiveControlTypeTraits<T>::encoded_extent)};
    T candidate{};
    if (!LiveControlTypeTraits<T>::decode(body, candidate) ||
        !LiveControlTypeTraits<T>::validate(candidate)) {
        return LiveControlTypedStatus::codec_rejected;
    }
    output = candidate;
    return LiveControlTypedStatus::ok;
}

namespace detail {

template <LiveControlFixedType T>
[[nodiscard]] LiveControlTypedStatus make_live_control_update_base(
    const LiveControlProducerHandle& handle,
    std::uint64_t producer_sequence,
    const T& value,
    LiveControlTypedRecordOptions options,
    LiveControlTypedPayload<T>& payload,
    LiveControlUpdateRecord& output) noexcept {
    if (!handle.valid()) {
        return LiveControlTypedStatus::invalid_handle;
    }
    if (producer_sequence == 0 ||
        producer_sequence == std::numeric_limits<std::uint64_t>::max()) {
        return LiveControlTypedStatus::invalid_argument;
    }
    if (payload.size() != live_control_typed_payload_extent<T>) {
        return LiveControlTypedStatus::invalid_argument;
    }
    if (!live_control_typed_options_valid(payload.size(), options)) {
        return LiveControlTypedStatus::invalid_options;
    }

    const auto encoded = encode_live_control_typed_payload(value, payload);
    if (encoded != LiveControlTypedStatus::ok) {
        return encoded;
    }

    LiveControlUpdateRecord candidate;
    candidate.runtime_id = handle.runtime_id;
    candidate.configuration_generation = handle.configuration_generation;
    candidate.mailbox_identity = handle.mailbox_identity;
    candidate.producer_identity = handle.producer_identity;
    candidate.producer_sequence = producer_sequence;
    candidate.payload_digest = live_control_payload_digest(payload);
    candidate.payload_bytes = static_cast<std::uint32_t>(payload.size());
    candidate.payload_alignment = options.payload_alignment;
    candidate.policy_flags = options.policy_flags;
    candidate.update_kind = static_cast<LiveControlUpdateKind>(
        LiveControlTypeTraits<T>::update_kind);
    output = candidate;
    return LiveControlTypedStatus::ok;
}

} // namespace detail

template <LiveControlFixedType T>
[[nodiscard]] LiveControlTypedStatus make_live_control_host_update(
    const LiveControlProducerHandle& handle,
    std::uint64_t producer_sequence,
    std::uint64_t target_frame_index,
    const T& value,
    LiveControlTypedPayload<T>& payload,
    LiveControlUpdateRecord& output,
    LiveControlTypedRecordOptions options = {}) noexcept {
    if (target_frame_index == std::numeric_limits<std::uint64_t>::max()) {
        return LiveControlTypedStatus::invalid_target;
    }
    LiveControlUpdateRecord candidate;
    const auto result = detail::make_live_control_update_base(
        handle,
        producer_sequence,
        value,
        options,
        payload,
        candidate);
    if (result != LiveControlTypedStatus::ok) {
        return result;
    }
    candidate.target_frame_index = target_frame_index;
    candidate.target_kind = LiveControlTargetKind::host_frame;
    output = candidate;
    return LiveControlTypedStatus::ok;
}

template <LiveControlFixedType T>
[[nodiscard]] LiveControlTypedStatus make_live_control_rate_update(
    const LiveControlProducerHandle& handle,
    std::uint64_t producer_sequence,
    const LiveControlBoundaryTarget& target,
    const T& value,
    LiveControlTypedPayload<T>& payload,
    LiveControlUpdateRecord& output,
    LiveControlTypedRecordOptions options = {}) noexcept {
    constexpr auto invalid64 = std::numeric_limits<std::uint64_t>::max();
    constexpr auto invalid32 = std::numeric_limits<std::uint32_t>::max();
    if (target.kind != LiveControlTargetKind::rate_release ||
        target.frame_index != invalid64 ||
        target.rate_release_sequence == invalid64 ||
        target.reference_release_index == invalid32 ||
        target.rate_domain_registration_index == invalid32 ||
        target.phase_index == invalid32 ||
        target.rate_substep_ordinal == invalid32 ||
        !detail::live_control_target_reserved_zero(target)) {
        return LiveControlTypedStatus::invalid_target;
    }
    LiveControlUpdateRecord candidate;
    const auto result = detail::make_live_control_update_base(
        handle,
        producer_sequence,
        value,
        options,
        payload,
        candidate);
    if (result != LiveControlTypedStatus::ok) {
        return result;
    }
    candidate.target_frame_index = target.frame_index;
    candidate.rate_release_sequence = target.rate_release_sequence;
    candidate.reference_release_index = target.reference_release_index;
    candidate.rate_domain_registration_index =
        target.rate_domain_registration_index;
    candidate.phase_index = target.phase_index;
    candidate.rate_substep_ordinal = target.rate_substep_ordinal;
    candidate.target_kind = LiveControlTargetKind::rate_release;
    output = candidate;
    return LiveControlTypedStatus::ok;
}

} // namespace rt
