#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <rt/live_control.hpp>

namespace sdk_test {

struct ControllerGains {
    float proportional = 0.0F;
    float integral = 0.0F;
    float derivative = 0.0F;
    std::uint32_t revision = 0;

    friend bool operator==(ControllerGains, ControllerGains) noexcept = default;
};

struct ClearFault {
    std::uint32_t fault_identity = 0;
    std::uint32_t authorization = 0;

    friend bool operator==(ClearFault, ClearFault) noexcept = default;
};

struct ZeroIdentity {
    std::uint32_t value = 0;
};

struct ThrowingCodec {
    std::uint32_t value = 0;
};

struct MissingCodec {
    std::uint32_t value = 0;
};

} // namespace sdk_test

namespace rt {

template <>
struct LiveControlTypeTraits<sdk_test::ControllerGains> {
    static constexpr std::uint32_t application_type_identity = 0x4347'4149u;
    static constexpr std::uint32_t application_schema_version = 3;
    static constexpr LiveControlUpdateKind update_kind =
        LiveControlUpdateKind::controller_parameters;
    static constexpr std::size_t encoded_extent = 16;

    static bool validate(const sdk_test::ControllerGains& value) noexcept {
        return std::isfinite(value.proportional) &&
            std::isfinite(value.integral) &&
            std::isfinite(value.derivative) &&
            value.proportional >= 0.0F && value.proportional <= 100.0F &&
            value.integral >= 0.0F && value.integral <= 100.0F &&
            value.derivative >= 0.0F && value.derivative <= 100.0F &&
            value.revision != 0;
    }

    static bool encode(
        const sdk_test::ControllerGains& value,
        std::span<std::byte, encoded_extent> output) noexcept {
        return store_u32_le(
                   output, 0, std::bit_cast<std::uint32_t>(value.proportional)) &&
            store_u32_le(
                output, 4, std::bit_cast<std::uint32_t>(value.integral)) &&
            store_u32_le(
                output, 8, std::bit_cast<std::uint32_t>(value.derivative)) &&
            store_u32_le(output, 12, value.revision);
    }

    static bool decode(
        std::span<const std::byte, encoded_extent> input,
        sdk_test::ControllerGains& output) noexcept {
        std::uint32_t proportional = 0;
        std::uint32_t integral = 0;
        std::uint32_t derivative = 0;
        std::uint32_t revision = 0;
        if (!load_u32_le(input, 0, proportional) ||
            !load_u32_le(input, 4, integral) ||
            !load_u32_le(input, 8, derivative) ||
            !load_u32_le(input, 12, revision)) {
            return false;
        }
        output = {
            std::bit_cast<float>(proportional),
            std::bit_cast<float>(integral),
            std::bit_cast<float>(derivative),
            revision};
        return true;
    }
};

template <>
struct LiveControlTypeTraits<sdk_test::ClearFault> {
    static constexpr std::uint32_t application_type_identity = 0x434c'5246u;
    static constexpr std::uint32_t application_schema_version = 1;
    static constexpr LiveControlUpdateKind update_kind =
        LiveControlUpdateKind::clear_fault;
    static constexpr std::size_t encoded_extent = 8;

    static bool validate(const sdk_test::ClearFault& value) noexcept {
        return value.fault_identity != 0 && value.authorization != 0;
    }

    static bool encode(
        const sdk_test::ClearFault& value,
        std::span<std::byte, encoded_extent> output) noexcept {
        return store_u32_le(output, 0, value.fault_identity) &&
            store_u32_le(output, 4, value.authorization);
    }

    static bool decode(
        std::span<const std::byte, encoded_extent> input,
        sdk_test::ClearFault& output) noexcept {
        sdk_test::ClearFault candidate;
        if (!load_u32_le(input, 0, candidate.fault_identity) ||
            !load_u32_le(input, 4, candidate.authorization)) {
            return false;
        }
        output = candidate;
        return true;
    }
};

template <>
struct LiveControlTypeTraits<sdk_test::ZeroIdentity> {
    static constexpr std::uint32_t application_type_identity = 0;
    static constexpr std::uint32_t application_schema_version = 1;
    static constexpr LiveControlUpdateKind update_kind =
        LiveControlUpdateKind::scenario_parameters;
    static constexpr std::size_t encoded_extent = 4;
    static bool validate(const sdk_test::ZeroIdentity&) noexcept { return true; }
    static bool encode(
        const sdk_test::ZeroIdentity&,
        std::span<std::byte, encoded_extent>) noexcept { return true; }
    static bool decode(
        std::span<const std::byte, encoded_extent>,
        sdk_test::ZeroIdentity&) noexcept { return true; }
};

template <>
struct LiveControlTypeTraits<sdk_test::ThrowingCodec> {
    static constexpr std::uint32_t application_type_identity = 1;
    static constexpr std::uint32_t application_schema_version = 1;
    static constexpr LiveControlUpdateKind update_kind =
        LiveControlUpdateKind::scenario_parameters;
    static constexpr std::size_t encoded_extent = 4;
    static bool validate(const sdk_test::ThrowingCodec&) { return true; }
    static bool encode(
        const sdk_test::ThrowingCodec&,
        std::span<std::byte, encoded_extent>) { return true; }
    static bool decode(
        std::span<const std::byte, encoded_extent>,
        sdk_test::ThrowingCodec&) { return true; }
};

} // namespace rt

static_assert(rt::LiveControlFixedType<sdk_test::ControllerGains>);
static_assert(rt::LiveControlFixedType<sdk_test::ClearFault>);
static_assert(!rt::LiveControlFixedType<sdk_test::ZeroIdentity>);
static_assert(!rt::LiveControlFixedType<sdk_test::ThrowingCodec>);
static_assert(!rt::LiveControlFixedType<sdk_test::MissingCodec>);
static_assert(!rt::LiveControlFixedType<std::string>);
static_assert(!rt::LiveControlFixedType<std::uint32_t*>);

namespace {

constexpr std::uint64_t kMailbox = 0x4d323204u;
constexpr std::uint64_t kProducer = 0x53444b31u;

rt::LiveControlProducerHandle handle() {
    rt::LiveControlProducerHandle value;
    value.runtime_id = 11;
    value.configuration_generation = 22;
    value.mailbox_identity = kMailbox;
    value.producer_identity = kProducer;
    value.producer_index = 0;
    return value;
}

rt::LiveControlUpdateRecord record_for(
    const rt::LiveControlProducerHandle& producer,
    std::span<const std::byte> payload) {
    rt::LiveControlUpdateRecord record;
    record.runtime_id = producer.runtime_id;
    record.configuration_generation = producer.configuration_generation;
    record.mailbox_identity = producer.mailbox_identity;
    record.producer_identity = producer.producer_identity;
    record.producer_sequence = 1;
    record.target_frame_index = 9;
    record.update_kind = rt::LiveControlUpdateKind::controller_parameters;
    record.payload_bytes = static_cast<std::uint32_t>(payload.size());
    record.payload_digest = rt::live_control_payload_digest(payload);
    return record;
}

void refresh_digest(
    rt::LiveControlUpdateRecord& record,
    std::span<const std::byte> payload) {
    record.payload_digest = rt::live_control_payload_digest(payload);
}

} // namespace

TEST(LiveControlSdk, CanonicalEnvelopeHasExactLittleEndianLayout) {
    const sdk_test::ControllerGains gains{1.0F, 0.5F, 0.25F, 7};
    rt::LiveControlTypedPayload<sdk_test::ControllerGains> payload{};
    ASSERT_EQ(
        rt::encode_live_control_typed_payload(gains, payload),
        rt::LiveControlTypedStatus::ok);
    ASSERT_EQ(payload.size(), 48u);

    const std::array<std::byte, 32> expected_header{
        std::byte{0x52}, std::byte{0x54}, std::byte{0x4c}, std::byte{0x43},
        std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x30}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x49}, std::byte{0x41}, std::byte{0x47}, std::byte{0x43},
        std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    EXPECT_TRUE(std::equal(
        expected_header.begin(), expected_header.end(), payload.begin()));
    EXPECT_EQ(payload[32], std::byte{0x00});
    EXPECT_EQ(payload[34], std::byte{0x80});
    EXPECT_EQ(payload[35], std::byte{0x3f});

    rt::LiveControlTypedEnvelopeMetadata metadata;
    ASSERT_EQ(
        rt::inspect_live_control_typed_envelope(payload, metadata),
        rt::LiveControlTypedStatus::ok);
    EXPECT_EQ(metadata.magic, rt::live_control_typed_envelope_magic);
    EXPECT_EQ(metadata.envelope_version, 1u);
    EXPECT_EQ(metadata.total_encoded_bytes, payload.size());
    EXPECT_EQ(metadata.application_type_identity, 0x4347'4149u);
    EXPECT_EQ(metadata.application_schema_version, 3u);
    EXPECT_EQ(
        metadata.update_kind,
        rt::LiveControlUpdateKind::controller_parameters);

    auto record = record_for(handle(), payload);
    sdk_test::ControllerGains decoded;
    EXPECT_EQ(
        rt::decode_live_control_typed_payload(record, payload, decoded),
        rt::LiveControlTypedStatus::ok);
    EXPECT_EQ(decoded, gains);
}

TEST(LiveControlSdk, DecodeRejectsMutationsWithoutPartiallyChangingOutput) {
    const sdk_test::ControllerGains gains{2.0F, 1.0F, 0.5F, 9};
    rt::LiveControlTypedPayload<sdk_test::ControllerGains> canonical{};
    ASSERT_EQ(
        rt::encode_live_control_typed_payload(gains, canonical),
        rt::LiveControlTypedStatus::ok);
    auto record = record_for(handle(), canonical);
    const sdk_test::ControllerGains sentinel{7.0F, 8.0F, 9.0F, 99};
    sdk_test::ControllerGains output = sentinel;

    EXPECT_EQ(
        rt::decode_live_control_typed_payload(
            record,
            std::span<const std::byte>{canonical}.first(canonical.size() - 1),
            output),
        rt::LiveControlTypedStatus::malformed_envelope);
    EXPECT_EQ(output, sentinel);

    auto mutated = canonical;
    mutated[0] ^= std::byte{0x01};
    refresh_digest(record, mutated);
    EXPECT_EQ(
        rt::decode_live_control_typed_payload(record, mutated, output),
        rt::LiveControlTypedStatus::malformed_envelope);
    EXPECT_EQ(output, sentinel);

    mutated = canonical;
    mutated[24] = std::byte{1};
    refresh_digest(record, mutated);
    EXPECT_EQ(
        rt::decode_live_control_typed_payload(record, mutated, output),
        rt::LiveControlTypedStatus::malformed_envelope);
    EXPECT_EQ(output, sentinel);

    mutated = canonical;
    ASSERT_TRUE(rt::store_u32_le(mutated, 12, 0x1111u));
    refresh_digest(record, mutated);
    EXPECT_EQ(
        rt::decode_live_control_typed_payload(record, mutated, output),
        rt::LiveControlTypedStatus::type_mismatch);
    EXPECT_EQ(output, sentinel);

    mutated = canonical;
    ASSERT_TRUE(rt::store_u32_le(mutated, 16, 4u));
    refresh_digest(record, mutated);
    EXPECT_EQ(
        rt::decode_live_control_typed_payload(record, mutated, output),
        rt::LiveControlTypedStatus::schema_mismatch);
    EXPECT_EQ(output, sentinel);

    mutated = canonical;
    mutated[32] ^= std::byte{0x01};
    record = record_for(handle(), canonical);
    EXPECT_EQ(
        rt::decode_live_control_typed_payload(record, mutated, output),
        rt::LiveControlTypedStatus::digest_mismatch);
    EXPECT_EQ(output, sentinel);

    mutated = canonical;
    ASSERT_TRUE(rt::store_u32_le(
        mutated,
        32,
        std::bit_cast<std::uint32_t>(
            std::numeric_limits<float>::quiet_NaN())));
    refresh_digest(record, mutated);
    EXPECT_EQ(
        rt::decode_live_control_typed_payload(record, mutated, output),
        rt::LiveControlTypedStatus::codec_rejected);
    EXPECT_EQ(output, sentinel);

    std::vector<std::byte> trailing(canonical.begin(), canonical.end());
    trailing.push_back(std::byte{0});
    rt::LiveControlTypedEnvelopeMetadata metadata;
    EXPECT_EQ(
        rt::inspect_live_control_typed_envelope(trailing, metadata),
        rt::LiveControlTypedStatus::malformed_envelope);
}

TEST(LiveControlSdk, BuildersPreserveExactHandleSequenceTargetAndRawStatus) {
    const auto producer = handle();
    const sdk_test::ControllerGains gains{3.0F, 2.0F, 1.0F, 5};
    rt::LiveControlTypedPayload<sdk_test::ControllerGains> payload{};
    rt::LiveControlUpdateRecord host;
    ASSERT_EQ(
        rt::make_live_control_host_update(
            producer, 41, 72, gains, payload, host),
        rt::LiveControlTypedStatus::ok);
    EXPECT_EQ(host.runtime_id, producer.runtime_id);
    EXPECT_EQ(
        host.configuration_generation,
        producer.configuration_generation);
    EXPECT_EQ(host.mailbox_identity, producer.mailbox_identity);
    EXPECT_EQ(host.producer_identity, producer.producer_identity);
    EXPECT_EQ(host.producer_sequence, 41u);
    EXPECT_EQ(host.target_frame_index, 72u);
    EXPECT_EQ(
        host.rate_release_sequence,
        std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(host.target_kind, rt::LiveControlTargetKind::host_frame);
    EXPECT_EQ(
        host.update_kind,
        rt::LiveControlUpdateKind::controller_parameters);
    EXPECT_EQ(host.payload_bytes, payload.size());
    EXPECT_EQ(host.payload_alignment, 1u);
    EXPECT_EQ(
        host.policy_flags,
        rt::live_control_payload_canonical_little_endian);
    EXPECT_EQ(host.payload_digest, rt::live_control_payload_digest(payload));

    rt::LiveControlBoundaryTarget rate;
    rate.kind = rt::LiveControlTargetKind::rate_release;
    rate.rate_release_sequence = 18;
    rate.reference_release_index = 2;
    rate.rate_domain_registration_index = 3;
    rate.phase_index = 4;
    rate.rate_substep_ordinal = 5;
    rt::LiveControlUpdateRecord rate_record;
    ASSERT_EQ(
        rt::make_live_control_rate_update(
            producer, 42, rate, gains, payload, rate_record),
        rt::LiveControlTypedStatus::ok);
    EXPECT_EQ(rate_record.producer_sequence, 42u);
    EXPECT_EQ(rate_record.target_frame_index,
              std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(rate_record.rate_release_sequence, 18u);
    EXPECT_EQ(rate_record.reference_release_index, 2u);
    EXPECT_EQ(rate_record.rate_domain_registration_index, 3u);
    EXPECT_EQ(rate_record.phase_index, 4u);
    EXPECT_EQ(rate_record.rate_substep_ordinal, 5u);
    EXPECT_EQ(rate_record.target_kind, rt::LiveControlTargetKind::rate_release);

    const auto preserved = host;
    auto invalid = producer;
    invalid.runtime_id = 0;
    EXPECT_EQ(
        rt::make_live_control_host_update(
            invalid, 43, 73, gains, payload, host),
        rt::LiveControlTypedStatus::invalid_handle);
    EXPECT_EQ(host.producer_sequence, preserved.producer_sequence);
    EXPECT_EQ(
        rt::make_live_control_host_update(
            producer, 0, 73, gains, payload, host),
        rt::LiveControlTypedStatus::invalid_argument);
    EXPECT_EQ(host.producer_sequence, preserved.producer_sequence);
    EXPECT_EQ(
        rt::make_live_control_host_update(
            producer,
            43,
            std::numeric_limits<std::uint64_t>::max(),
            gains,
            payload,
            host),
        rt::LiveControlTypedStatus::invalid_target);
    EXPECT_EQ(host.producer_sequence, preserved.producer_sequence);
    auto bad_options = rt::LiveControlTypedRecordOptions{};
    bad_options.policy_flags = 0;
    EXPECT_EQ(
        rt::make_live_control_host_update(
            producer, 43, 73, gains, payload, host, bad_options),
        rt::LiveControlTypedStatus::invalid_options);
    EXPECT_EQ(host.producer_sequence, preserved.producer_sequence);

    auto rejected = gains;
    rejected.proportional = -1.0F;
    EXPECT_EQ(
        rt::make_live_control_host_update(
            producer, 43, 73, rejected, payload, host),
        rt::LiveControlTypedStatus::codec_rejected);
    EXPECT_EQ(host.producer_sequence, preserved.producer_sequence);
}

TEST(LiveControlSdk, TypedClearFaultKeepsRawAdmissionResultVisible) {
    rt::Runtime runtime;
    rt::LiveControlPolicy policy;
    policy.policy_identity = 0x4d323204;
    policy.mailbox_capacity = 1;
    policy.producer_capacity = 1;
    policy.record_capacity = 1;
    policy.payload_bytes_per_record = static_cast<std::uint32_t>(
        rt::live_control_typed_payload_extent<sdk_test::ClearFault>);
    policy.total_payload_storage_bytes = policy.payload_bytes_per_record;
    ASSERT_EQ(runtime.set_live_control_policy(policy), rt::Status::ok);
    rt::LiveControlMailboxRegistration mailbox;
    mailbox.mailbox_identity = kMailbox;
    mailbox.record_capacity = 1;
    mailbox.payload_bytes_per_record = policy.payload_bytes_per_record;
    ASSERT_EQ(
        runtime.register_live_control_mailbox(mailbox),
        rt::Status::ok);
    rt::LiveControlProducerRegistration registration;
    registration.mailbox_identity = kMailbox;
    registration.producer_identity = kProducer;
    ASSERT_EQ(
        runtime.register_live_control_producer(registration),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    rt::LiveControlProducerHandle producer;
    ASSERT_EQ(
        runtime.live_control_producer_handle(kMailbox, kProducer, producer),
        rt::Status::ok);
    const sdk_test::ClearFault clear{77, 0x434c'4541u};
    rt::LiveControlTypedPayload<sdk_test::ClearFault> payload{};
    rt::LiveControlUpdateRecord update;
    ASSERT_EQ(
        rt::make_live_control_host_update(
            producer, 1, 4, clear, payload, update),
        rt::LiveControlTypedStatus::ok);

    auto admission = rt::LiveControlAdmissionResult::busy;
    EXPECT_EQ(
        runtime.stage_live_control_update(
            producer, update, payload, admission),
        rt::Status::ok);
    EXPECT_EQ(admission, rt::LiveControlAdmissionResult::accepted);

    update.producer_sequence = 2;
    update.payload_digest = rt::live_control_payload_digest(payload);
    admission = rt::LiveControlAdmissionResult::invalid;
    EXPECT_EQ(
        runtime.stage_live_control_update(
            producer, update, payload, admission),
        rt::Status::ok);
    EXPECT_EQ(admission, rt::LiveControlAdmissionResult::full);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}
