#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <thread>
#include <unordered_set>
#include <vector>

#include <rt/live_control.hpp>

namespace stress_test {

struct Value {
    std::uint32_t producer = 0;
    std::uint32_t operation = 0;
    std::uint32_t check = 0;
    std::uint32_t generation = 0;

    friend bool operator==(Value, Value) noexcept = default;
};

struct MaximumPayload {
    std::uint32_t seed = 0;

    friend bool operator==(MaximumPayload, MaximumPayload) noexcept = default;
};

} // namespace stress_test

namespace rt {

template <>
struct LiveControlTypeTraits<stress_test::Value> {
    static constexpr std::uint32_t application_type_identity = 0x5354'5253u;
    static constexpr std::uint32_t application_schema_version = 1;
    static constexpr LiveControlUpdateKind update_kind =
        LiveControlUpdateKind::scenario_parameters;
    static constexpr std::size_t encoded_extent = 16;

    static bool validate(const stress_test::Value& value) noexcept {
        return value.producer != 0 && value.operation != 0 &&
            value.check == (value.producer ^ value.operation ^ 0xa5a5'a5a5u) &&
            value.generation != 0;
    }

    static bool encode(
        const stress_test::Value& value,
        std::span<std::byte, encoded_extent> output) noexcept {
        return store_u32_le(output, 0, value.producer) &&
            store_u32_le(output, 4, value.operation) &&
            store_u32_le(output, 8, value.check) &&
            store_u32_le(output, 12, value.generation);
    }

    static bool decode(
        std::span<const std::byte, encoded_extent> input,
        stress_test::Value& output) noexcept {
        stress_test::Value candidate;
        if (!load_u32_le(input, 0, candidate.producer) ||
            !load_u32_le(input, 4, candidate.operation) ||
            !load_u32_le(input, 8, candidate.check) ||
            !load_u32_le(input, 12, candidate.generation)) {
            return false;
        }
        output = candidate;
        return true;
    }
};

template <>
struct LiveControlTypeTraits<stress_test::MaximumPayload> {
    static constexpr std::uint32_t application_type_identity = 0x4d41'5850u;
    static constexpr std::uint32_t application_schema_version = 1;
    static constexpr LiveControlUpdateKind update_kind =
        LiveControlUpdateKind::sensor_calibration;
    static constexpr std::size_t encoded_extent =
        live_control_payload_bytes_limit - live_control_typed_envelope_bytes;

    static bool validate(const stress_test::MaximumPayload& value) noexcept {
        return value.seed != 0;
    }

    static bool encode(
        const stress_test::MaximumPayload& value,
        std::span<std::byte, encoded_extent> output) noexcept {
        if (!store_u32_le(output, 0, value.seed)) {
            return false;
        }
        for (std::size_t index = sizeof(value.seed); index < output.size();
             ++index) {
            output[index] = static_cast<std::byte>(
                (value.seed + static_cast<std::uint32_t>(index)) & 0xffu);
        }
        return true;
    }

    static bool decode(
        std::span<const std::byte, encoded_extent> input,
        stress_test::MaximumPayload& output) noexcept {
        stress_test::MaximumPayload candidate;
        if (!load_u32_le(input, 0, candidate.seed)) {
            return false;
        }
        for (std::size_t index = sizeof(candidate.seed); index < input.size();
             ++index) {
            const auto expected = static_cast<std::byte>(
                (candidate.seed + static_cast<std::uint32_t>(index)) & 0xffu);
            if (input[index] != expected) {
                return false;
            }
        }
        output = candidate;
        return true;
    }
};

} // namespace rt

static_assert(rt::LiveControlFixedType<stress_test::Value>);
static_assert(rt::LiveControlFixedType<stress_test::MaximumPayload>);
static_assert(
    rt::live_control_typed_payload_extent<stress_test::MaximumPayload> ==
    rt::live_control_payload_bytes_limit);

namespace {

constexpr std::uint64_t kMailbox = 0x5354'5245u;
constexpr std::uint64_t kProducerBase = 0x5052'0000u;

stress_test::Value value(
    std::uint32_t producer,
    std::uint32_t operation,
    std::uint32_t generation = 1) {
    return {
        producer,
        operation,
        producer ^ operation ^ 0xa5a5'a5a5u,
        generation};
}

struct DecodeProbe {
    std::size_t callbacks = 0;
    std::size_t records = 0;
    std::size_t decode_failures = 0;
    stress_test::Value last{};
};

rt::CallbackResult decode_callback(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& probe = *static_cast<DecodeProbe*>(user_data);
    ++probe.callbacks;
    if (!context.live_control) {
        return rt::CallbackResult::ok;
    }
    for (const auto& view : context.live_control->records) {
        stress_test::Value decoded;
        if (rt::decode_live_control_typed_payload(
                view.record, view.payload, decoded) !=
            rt::LiveControlTypedStatus::ok) {
            ++probe.decode_failures;
            return rt::CallbackResult::error;
        }
        ++probe.records;
        probe.last = decoded;
    }
    return rt::CallbackResult::ok;
}

rt::CallbackResult decode_then_fail_callback(
    void* user_data,
    const rt::CallbackContext& context) {
    if (decode_callback(user_data, context) != rt::CallbackResult::ok) {
        return rt::CallbackResult::error;
    }
    return rt::CallbackResult::error;
}

rt::LiveControlPolicy policy(
    std::uint32_t producers,
    std::uint32_t records,
    std::uint32_t payload_bytes) {
    rt::LiveControlPolicy result;
    result.policy_identity = 0x4d32'3204u;
    result.mailbox_capacity = 1;
    result.producer_capacity = producers;
    result.record_capacity = records;
    result.payload_bytes_per_record = payload_bytes;
    result.total_payload_storage_bytes =
        static_cast<std::uint64_t>(records) * payload_bytes;
    return result;
}

void configure_one_mailbox(
    rt::Runtime& runtime,
    std::uint32_t producer_count,
    std::uint32_t record_count,
    std::uint32_t payload_bytes,
    bool replay = false,
    std::size_t action_capacity = 0) {
    ASSERT_EQ(
        runtime.set_live_control_policy(
            policy(producer_count, record_count, payload_bytes)),
        rt::Status::ok);
    if (action_capacity != 0 || replay) {
        rt::LiveControlClosurePolicy closure;
        closure.policy_identity = 0x434c'4f53u;
        closure.action_capacity = action_capacity;
        if (replay) {
            closure.retained_generation_capacity = 4;
            closure.retained_record_capacity = 4;
            closure.retained_payload_bytes = 4 * payload_bytes;
            closure.replay_record_capacity = 2'048;
            closure.replay_max_bytes = 1024 * 1024;
            closure.replay_enabled = true;
        }
        ASSERT_EQ(
            runtime.set_live_control_closure_policy(closure),
            rt::Status::ok);
    }
    rt::LiveControlMailboxRegistration mailbox;
    mailbox.mailbox_identity = kMailbox;
    mailbox.record_capacity = record_count;
    mailbox.payload_bytes_per_record = payload_bytes;
    ASSERT_EQ(
        runtime.register_live_control_mailbox(mailbox),
        rt::Status::ok);
    for (std::uint32_t index = 0; index < producer_count; ++index) {
        rt::LiveControlProducerRegistration producer;
        producer.mailbox_identity = kMailbox;
        producer.producer_identity = kProducerBase + index + 1;
        ASSERT_EQ(
            runtime.register_live_control_producer(producer),
            rt::Status::ok);
    }
}

} // namespace

TEST(LiveControlStress, AbsoluteConfigurationAndPayloadBoundariesAreExact) {
    rt::Runtime maximum;
    rt::RuntimeConfig config;
    config.memory_budget_bytes = 512 * 1024 * 1024;
    ASSERT_EQ(maximum.configure(config), rt::Status::ok);
    rt::LiveControlPolicy maximum_policy;
    maximum_policy.policy_identity = 0x4d41'5843u;
    maximum_policy.mailbox_capacity = rt::live_control_mailbox_capacity_limit;
    maximum_policy.producer_capacity =
        rt::live_control_producer_capacity_limit;
    maximum_policy.record_capacity = rt::live_control_record_capacity_limit;
    maximum_policy.payload_bytes_per_record = 1;
    maximum_policy.total_payload_storage_bytes =
        rt::live_control_record_capacity_limit;
    ASSERT_EQ(
        maximum.set_live_control_policy(maximum_policy),
        rt::Status::ok);
    for (std::uint32_t mailbox_index = 0;
         mailbox_index < rt::live_control_mailbox_capacity_limit;
         ++mailbox_index) {
        rt::LiveControlMailboxRegistration mailbox;
        mailbox.mailbox_identity = 1'000 + mailbox_index;
        mailbox.record_capacity =
            rt::live_control_record_capacity_limit /
            rt::live_control_mailbox_capacity_limit;
        mailbox.payload_bytes_per_record = 1;
        ASSERT_EQ(
            maximum.register_live_control_mailbox(mailbox),
            rt::Status::ok);
    }
    for (std::uint32_t producer_index = 0;
         producer_index < rt::live_control_producer_capacity_limit;
         ++producer_index) {
        rt::LiveControlProducerRegistration producer;
        producer.mailbox_identity =
            1'000 + producer_index % rt::live_control_mailbox_capacity_limit;
        producer.producer_identity = 10'000 + producer_index;
        ASSERT_EQ(
            maximum.register_live_control_producer(producer),
            rt::Status::ok);
    }
    rt::LiveControlMailboxRegistration extra_mailbox;
    extra_mailbox.mailbox_identity = 99'999;
    extra_mailbox.record_capacity = 1;
    extra_mailbox.payload_bytes_per_record = 1;
    EXPECT_EQ(
        maximum.register_live_control_mailbox(extra_mailbox),
        rt::Status::capacity_exceeded);
    rt::LiveControlProducerRegistration extra_producer;
    extra_producer.mailbox_identity = 1'000;
    extra_producer.producer_identity = 99'999;
    EXPECT_EQ(
        maximum.register_live_control_producer(extra_producer),
        rt::Status::capacity_exceeded);
    ASSERT_EQ(maximum.finalize(), rt::Status::ok);
    rt::MemoryPlan plan;
    ASSERT_TRUE(maximum.memory_plan(plan));
    EXPECT_EQ(
        plan.live_control_mailbox_count,
        rt::live_control_mailbox_capacity_limit);
    EXPECT_EQ(
        plan.live_control_producer_count,
        rt::live_control_producer_capacity_limit);
    EXPECT_EQ(
        plan.live_control_record_capacity,
        rt::live_control_record_capacity_limit);
    EXPECT_EQ(
        plan.live_control_payload_storage_bytes,
        rt::live_control_record_capacity_limit);

    rt::Runtime checked_gib;
    rt::LiveControlPolicy checked_policy;
    checked_policy.policy_identity = 0x4749'4231u;
    checked_policy.mailbox_capacity = 1;
    checked_policy.producer_capacity = 1;
    checked_policy.record_capacity = 1;
    checked_policy.payload_bytes_per_record = 1;
    checked_policy.total_payload_storage_bytes =
        rt::live_control_total_storage_limit;
    ASSERT_EQ(
        checked_gib.set_live_control_policy(checked_policy),
        rt::Status::ok);
    rt::LiveControlMailboxRegistration one_mailbox;
    one_mailbox.mailbox_identity = kMailbox;
    one_mailbox.record_capacity = 1;
    one_mailbox.payload_bytes_per_record = 1;
    ASSERT_EQ(
        checked_gib.register_live_control_mailbox(one_mailbox),
        rt::Status::ok);
    rt::LiveControlProducerRegistration one_producer;
    one_producer.mailbox_identity = kMailbox;
    one_producer.producer_identity = kProducerBase;
    ASSERT_EQ(
        checked_gib.register_live_control_producer(one_producer),
        rt::Status::ok);
    ASSERT_EQ(checked_gib.finalize(), rt::Status::ok);
    ASSERT_TRUE(checked_gib.memory_plan(plan));
    EXPECT_EQ(plan.live_control_payload_storage_bytes, 1u);
    EXPECT_LT(
        plan.live_control_control_bytes,
        rt::live_control_total_storage_limit);

    rt::Runtime one_over;
    auto invalid = checked_policy;
    invalid.total_payload_storage_bytes =
        rt::live_control_total_storage_limit + 1;
    EXPECT_EQ(
        one_over.set_live_control_policy(invalid),
        rt::Status::invalid_argument);
    EXPECT_EQ(one_over.state(), rt::RuntimeState::configuring);

    rt::Runtime maximum_payload;
    constexpr auto payload_extent = static_cast<std::uint32_t>(
        rt::live_control_typed_payload_extent<stress_test::MaximumPayload>);
    configure_one_mailbox(maximum_payload, 1, 1, payload_extent);
    ASSERT_EQ(maximum_payload.finalize(), rt::Status::ok);
    rt::LiveControlProducerHandle handle;
    ASSERT_EQ(
        maximum_payload.live_control_producer_handle(
            kMailbox, kProducerBase + 1, handle),
        rt::Status::ok);
    const stress_test::MaximumPayload source{0x1020'3040u};
    rt::LiveControlTypedPayload<stress_test::MaximumPayload> payload{};
    rt::LiveControlUpdateRecord update;
    ASSERT_EQ(
        rt::make_live_control_host_update(
            handle, 1, 5, source, payload, update),
        rt::LiveControlTypedStatus::ok);
    EXPECT_EQ(update.payload_bytes, rt::live_control_payload_bytes_limit);
    rt::LiveControlAdmissionResult admission;
    ASSERT_EQ(
        maximum_payload.stage_live_control_update(
            handle, update, payload, admission),
        rt::Status::ok);
    EXPECT_EQ(admission, rt::LiveControlAdmissionResult::accepted);
    stress_test::MaximumPayload decoded;
    EXPECT_EQ(
        rt::decode_live_control_typed_payload(update, payload, decoded),
        rt::LiveControlTypedStatus::ok);
    EXPECT_EQ(decoded, source);
}

TEST(LiveControlStress, ConcurrentFullOccupancyReclaimsWithExactLossCounts) {
    constexpr std::uint32_t producer_count = 32;
    constexpr std::uint32_t operations_per_producer = 8;
    constexpr std::uint32_t operation_count =
        producer_count * operations_per_producer;
    constexpr auto payload_bytes = static_cast<std::uint32_t>(
        rt::live_control_typed_payload_extent<stress_test::Value>);

    rt::Runtime runtime;
    DecodeProbe probe;
    ASSERT_EQ(
        runtime.register_callback({"typed", &decode_callback, &probe}),
        rt::Status::ok);
    configure_one_mailbox(
        runtime, producer_count, producer_count, payload_bytes, false, 128);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    std::array<rt::LiveControlProducerHandle, producer_count> handles{};
    for (std::uint32_t index = 0; index < producer_count; ++index) {
        ASSERT_EQ(
            runtime.live_control_producer_handle(
                kMailbox, kProducerBase + index + 1, handles[index]),
            rt::Status::ok);
    }

    std::barrier gate(static_cast<std::ptrdiff_t>(producer_count));
    std::array<std::thread, producer_count> threads;
    std::array<rt::Status, producer_count> statuses{};
    std::array<rt::LiveControlAdmissionResult, producer_count> admissions{};
    for (std::uint32_t producer_index = 0;
         producer_index < producer_count;
         ++producer_index) {
        threads[producer_index] = std::thread([&, producer_index] {
            gate.arrive_and_wait();
            const auto source = value(producer_index + 1, 1, 1);
            rt::LiveControlTypedPayload<stress_test::Value> payload{};
            rt::LiveControlUpdateRecord update;
            if (rt::make_live_control_host_update(
                    handles[producer_index],
                    1,
                    10,
                    source,
                    payload,
                    update) != rt::LiveControlTypedStatus::ok) {
                statuses[producer_index] = rt::Status::internal_error;
                return;
            }
            statuses[producer_index] = runtime.stage_live_control_update(
                handles[producer_index],
                update,
                payload,
                admissions[producer_index]);
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const auto accepted = static_cast<std::uint32_t>(std::count(
        admissions.begin(),
        admissions.end(),
        rt::LiveControlAdmissionResult::accepted));
    const auto busy = static_cast<std::uint32_t>(std::count(
        admissions.begin(),
        admissions.end(),
        rt::LiveControlAdmissionResult::busy));
    EXPECT_TRUE(std::all_of(
        statuses.begin(),
        statuses.end(),
        [](rt::Status status) { return status == rt::Status::ok; }));
    EXPECT_EQ(accepted + busy, producer_count);
    ASSERT_GT(accepted, 0u);
    rt::LiveControlMailboxInfo info;
    ASSERT_TRUE(runtime.live_control_mailbox_info(kMailbox, info));
    EXPECT_EQ(info.occupancy, accepted);
    EXPECT_EQ(info.accepted, accepted);
    EXPECT_EQ(info.busy, busy);

    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(
        runtime.step({10, std::chrono::nanoseconds{1}}),
        rt::Status::ok);
    rt::LiveControlCommitInfo commit;
    ASSERT_TRUE(runtime.live_control_commit_info(commit));
    EXPECT_EQ(commit.survivor_count, 1u);
    EXPECT_EQ(commit.committed, 1u);
    EXPECT_EQ(commit.replaced, accepted - 1);
    EXPECT_EQ(commit.staged_occupancy, 0u);
    EXPECT_EQ(probe.records, 1u);
    EXPECT_EQ(probe.decode_failures, 0u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);

    rt::Runtime full_runtime;
    DecodeProbe full_probe;
    ASSERT_EQ(
        full_runtime.register_callback(
            {"typed-full", &decode_callback, &full_probe}),
        rt::Status::ok);
    configure_one_mailbox(
        full_runtime,
        producer_count,
        operation_count,
        payload_bytes,
        true,
        512);
    ASSERT_EQ(full_runtime.finalize(), rt::Status::ok);
    std::array<rt::LiveControlProducerHandle, producer_count> full_handles{};
    for (std::uint32_t index = 0; index < producer_count; ++index) {
        ASSERT_EQ(
            full_runtime.live_control_producer_handle(
                kMailbox, kProducerBase + index + 1, full_handles[index]),
            rt::Status::ok);
    }

    for (std::uint32_t wave = 0; wave < 2; ++wave) {
        for (std::uint32_t producer_index = 0;
             producer_index < producer_count;
             ++producer_index) {
            for (std::uint32_t operation = 1;
                 operation <= operations_per_producer;
                 ++operation) {
                const auto sequence = wave * operations_per_producer + operation;
                const auto source = value(
                    producer_index + 1,
                    sequence,
                    wave + 1);
                rt::LiveControlTypedPayload<stress_test::Value> payload{};
                rt::LiveControlUpdateRecord update;
                ASSERT_EQ(
                    rt::make_live_control_host_update(
                        full_handles[producer_index],
                        sequence,
                        20 + wave,
                        source,
                        payload,
                        update),
                    rt::LiveControlTypedStatus::ok);
                rt::LiveControlAdmissionResult admission;
                ASSERT_EQ(
                    full_runtime.stage_live_control_update(
                        full_handles[producer_index],
                        update,
                        payload,
                        admission),
                    rt::Status::ok);
                ASSERT_EQ(
                    admission,
                    rt::LiveControlAdmissionResult::accepted);
            }
        }
        ASSERT_TRUE(full_runtime.live_control_mailbox_info(kMailbox, info));
        EXPECT_EQ(info.occupancy, operation_count);
        if (wave == 0) {
            ASSERT_EQ(full_runtime.start(), rt::Status::ok);
        }
        ASSERT_EQ(
            full_runtime.step({20 + wave, std::chrono::nanoseconds{1}}),
            rt::Status::ok);
        ASSERT_TRUE(full_runtime.live_control_commit_info(commit));
        EXPECT_EQ(commit.committed, wave + 1);
        EXPECT_EQ(commit.replaced, (wave + 1) * (operation_count - 1));
        EXPECT_EQ(full_probe.records, wave + 1);
        EXPECT_EQ(full_probe.decode_failures, 0u);
    }

    rt::LiveControlActionMetadata metadata;
    ASSERT_EQ(
        full_runtime.live_control_action_metadata(metadata),
        rt::Status::ok);
    EXPECT_EQ(metadata.records_emitted, 2u * (2u * operation_count + 1u));
    EXPECT_EQ(
        metadata.records_overwritten,
        metadata.records_emitted - metadata.capacity);
    EXPECT_FALSE(metadata.replay_eligible);
    std::array<rt::LiveControlActionRecord, 512> actions{};
    rt::LiveControlActionCursor cursor;
    cursor.runtime_id = full_handles[0].runtime_id;
    cursor.configuration_generation =
        full_handles[0].configuration_generation;
    rt::LiveControlActionReadResult read;
    ASSERT_EQ(
        full_runtime.read_live_control_actions(cursor, actions, read),
        rt::Status::ok);
    EXPECT_EQ(read.lost_records, metadata.records_overwritten);
    EXPECT_EQ(read.records_read, actions.size());
    EXPECT_EQ(full_runtime.stop(), rt::Status::ok);
}

TEST(LiveControlStress, CallbackRollbackRemainsOrthogonalToWatchdogPolicy) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.watchdog_timeout_ns = 1'000'000'000;
    config.watchdog_max_degradation_level = 2;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    DecodeProbe probe;
    ASSERT_EQ(
        runtime.register_callback(
            {"typed-failure", &decode_then_fail_callback, &probe}),
        rt::Status::ok);
    constexpr auto payload_bytes = static_cast<std::uint32_t>(
        rt::live_control_typed_payload_extent<stress_test::Value>);
    configure_one_mailbox(runtime, 1, 1, payload_bytes, false, 8);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    rt::LiveControlProducerHandle handle;
    ASSERT_EQ(
        runtime.live_control_producer_handle(
            kMailbox, kProducerBase + 1, handle),
        rt::Status::ok);
    const auto source = value(1, 1);
    rt::LiveControlTypedPayload<stress_test::Value> payload{};
    rt::LiveControlUpdateRecord update;
    ASSERT_EQ(
        rt::make_live_control_host_update(
            handle, 1, 7, source, payload, update),
        rt::LiveControlTypedStatus::ok);
    rt::LiveControlAdmissionResult admission;
    ASSERT_EQ(
        runtime.stage_live_control_update(handle, update, payload, admission),
        rt::Status::ok);
    ASSERT_EQ(admission, rt::LiveControlAdmissionResult::accepted);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    EXPECT_EQ(
        runtime.step({7, std::chrono::nanoseconds{1}}),
        rt::Status::callback_failed);
    EXPECT_EQ(probe.records, 1u);
    EXPECT_EQ(probe.decode_failures, 0u);
    EXPECT_EQ(runtime.degradation_level(), 0u);
    rt::LiveControlCommitInfo commit;
    ASSERT_TRUE(runtime.live_control_commit_info(commit));
    EXPECT_EQ(commit.generation_identity, 0u);
    EXPECT_EQ(commit.committed, 0u);
    rt::LiveControlRecordStatusInfo status;
    ASSERT_TRUE(runtime.live_control_record_status(kMailbox, 1, status));
    EXPECT_EQ(status.status, rt::LiveControlRecordStatus::rolled_back);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(LiveControlStress, RepeatedLifecycleAndConcurrentRuntimesStayIsolated) {
    constexpr std::size_t lifecycle_count = 24;
    constexpr auto payload_bytes = static_cast<std::uint32_t>(
        rt::live_control_typed_payload_extent<stress_test::Value>);
    rt::LiveControlProducerHandle stale;
    rt::LiveControlUpdateRecord stale_update;
    rt::LiveControlTypedPayload<stress_test::Value> stale_payload{};
    for (std::size_t iteration = 0; iteration < lifecycle_count; ++iteration) {
        rt::Runtime runtime;
        DecodeProbe probe;
        ASSERT_EQ(
            runtime.register_callback({"typed", &decode_callback, &probe}),
            rt::Status::ok);
        configure_one_mailbox(runtime, 1, 1, payload_bytes, false, 8);
        ASSERT_EQ(runtime.finalize(), rt::Status::ok);
        rt::LiveControlProducerHandle handle;
        ASSERT_EQ(
            runtime.live_control_producer_handle(
                kMailbox, kProducerBase + 1, handle),
            rt::Status::ok);
        const auto source = value(
            1,
            static_cast<std::uint32_t>(iteration + 1),
            static_cast<std::uint32_t>(iteration + 1));
        rt::LiveControlTypedPayload<stress_test::Value> payload{};
        rt::LiveControlUpdateRecord update;
        ASSERT_EQ(
            rt::make_live_control_host_update(
                handle, 1, 3, source, payload, update),
            rt::LiveControlTypedStatus::ok);
        rt::LiveControlAdmissionResult admission;
        ASSERT_EQ(
            runtime.stage_live_control_update(
                handle, update, payload, admission),
            rt::Status::ok);
        ASSERT_EQ(admission, rt::LiveControlAdmissionResult::accepted);
        ASSERT_EQ(runtime.start(), rt::Status::ok);
        ASSERT_EQ(
            runtime.step({3, std::chrono::nanoseconds{1}}),
            rt::Status::ok);
        EXPECT_EQ(probe.last, source);
        ASSERT_EQ(runtime.stop(), rt::Status::ok);
        update.producer_sequence = 2;
        admission = rt::LiveControlAdmissionResult::invalid;
        EXPECT_EQ(
            runtime.stage_live_control_update(
                handle, update, payload, admission),
            rt::Status::ok);
        EXPECT_EQ(admission, rt::LiveControlAdmissionResult::stopped);
        if (iteration == 0) {
            stale = handle;
            stale_update = update;
            stale_payload = payload;
        }
    }

    struct Instance {
        std::unique_ptr<rt::Runtime> runtime = std::make_unique<rt::Runtime>();
        DecodeProbe probe{};
        rt::LiveControlProducerHandle handle{};
        rt::LiveControlTypedPayload<stress_test::Value> payload{};
        rt::LiveControlUpdateRecord update{};
        std::vector<std::byte> checkpoint{};
        std::vector<std::byte> input_log{};
        std::vector<std::byte> replay_artifact{};
        rt::Status status = rt::Status::internal_error;
        rt::LiveControlAdmissionResult admission =
            rt::LiveControlAdmissionResult::invalid;
    };
    constexpr std::size_t instance_count = 8;
    std::array<Instance, instance_count> instances;
    for (std::size_t index = 0; index < instances.size(); ++index) {
        auto& instance = instances[index];
        ASSERT_EQ(
            instance.runtime->register_callback(
                {"typed", &decode_callback, &instance.probe}),
            rt::Status::ok);
        configure_one_mailbox(
            *instance.runtime, 1, 1, payload_bytes, true, 8);
        ASSERT_EQ(instance.runtime->finalize(), rt::Status::ok);
        ASSERT_EQ(
            instance.runtime->live_control_producer_handle(
                kMailbox, kProducerBase + 1, instance.handle),
            rt::Status::ok);
        const auto source = value(
            static_cast<std::uint32_t>(index + 1), 1, 99);
        ASSERT_EQ(
            rt::make_live_control_host_update(
                instance.handle,
                1,
                6,
                source,
                instance.payload,
                instance.update),
            rt::LiveControlTypedStatus::ok);
        ASSERT_EQ(instance.runtime->start(), rt::Status::ok);
        std::size_t checkpoint_bytes = 0;
        ASSERT_EQ(
            instance.runtime->checkpoint_size(checkpoint_bytes),
            rt::Status::ok);
        instance.checkpoint.resize(checkpoint_bytes);
        rt::ArtifactWriteResult checkpoint_write;
        ASSERT_EQ(
            instance.runtime->write_checkpoint(
                0, instance.checkpoint, checkpoint_write),
            rt::Status::ok);
        instance.checkpoint.resize(checkpoint_write.bytes_written);
        const std::array<rt::ReplayInputRecord, 1> inputs{
            rt::ReplayInputRecord{
                {6, std::chrono::nanoseconds{1}}, 1, {}}};
        instance.input_log.resize(4'096);
        rt::ArtifactWriteResult input_write;
        ASSERT_EQ(
            instance.runtime->write_input_log(
                inputs, instance.input_log, input_write),
            rt::Status::ok);
        instance.input_log.resize(input_write.bytes_written);
    }
    std::barrier gate(static_cast<std::ptrdiff_t>(instance_count));
    std::array<std::thread, instance_count> threads;
    for (std::size_t index = 0; index < instances.size(); ++index) {
        threads[index] = std::thread([&, index] {
            auto& instance = instances[index];
            gate.arrive_and_wait();
            instance.status = instance.runtime->stage_live_control_update(
                instance.handle,
                instance.update,
                instance.payload,
                instance.admission);
            if (instance.status == rt::Status::ok) {
                instance.status = instance.runtime->step(
                    {6, std::chrono::nanoseconds{1}});
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    std::unordered_set<std::uint64_t> runtime_ids;
    std::unordered_set<std::uint64_t> generation_ids;
    std::unordered_set<const std::byte*> checkpoint_buffers;
    std::unordered_set<std::uint64_t> artifact_checksums;
    for (std::size_t index = 0; index < instances.size(); ++index) {
        auto& instance = instances[index];
        EXPECT_EQ(instance.status, rt::Status::ok);
        EXPECT_EQ(
            instance.admission,
            rt::LiveControlAdmissionResult::accepted);
        EXPECT_EQ(instance.probe.records, 1u);
        EXPECT_EQ(instance.probe.decode_failures, 0u);
        EXPECT_EQ(
            instance.probe.last.producer,
            static_cast<std::uint32_t>(index + 1));
        runtime_ids.insert(instance.handle.runtime_id);
        rt::LiveControlCommitInfo commit;
        ASSERT_TRUE(instance.runtime->live_control_commit_info(commit));
        EXPECT_EQ(commit.committed, 1u);
        EXPECT_EQ(commit.replaced, 0u);
        EXPECT_NE(commit.generation_identity, 0u);
        generation_ids.insert(commit.generation_identity);

        std::array<rt::LiveControlActionRecord, 8> actions{};
        rt::LiveControlActionCursor cursor;
        rt::LiveControlActionReadResult read;
        ASSERT_EQ(
            instance.runtime->read_live_control_actions(
                cursor, actions, read),
            rt::Status::ok);
        constexpr std::size_t accepted_action_index = 1;
        ASSERT_EQ(read.records_read, 4u);
        EXPECT_EQ(read.lost_records, 0u);
        EXPECT_EQ(actions[0].runtime_id, instance.handle.runtime_id);
        EXPECT_EQ(
            actions[0].action,
            rt::LiveControlActionId::checkpointed);
        EXPECT_EQ(
            actions[accepted_action_index].action,
            rt::LiveControlActionId::admission);
        EXPECT_EQ(
            actions[accepted_action_index + 1].action,
            rt::LiveControlActionId::provisional_publication);
        EXPECT_EQ(
            actions[accepted_action_index + 2].action,
            rt::LiveControlActionId::committed);

        checkpoint_buffers.insert(instance.checkpoint.data());
        instance.replay_artifact.resize(64 * 1'024);
        rt::ArtifactWriteResult replay_write;
        ASSERT_EQ(
            instance.runtime->write_live_control_replay_artifact(
                instance.checkpoint,
                instance.input_log,
                rt::LiveControlNestedArtifactKind::input_log,
                instance.replay_artifact,
                replay_write),
            rt::Status::ok);
        instance.replay_artifact.resize(replay_write.bytes_written);
        rt::LiveControlReplayMetadata metadata;
        ASSERT_EQ(
            rt::inspect_live_control_replay_artifact(
                instance.replay_artifact, metadata),
            rt::Status::ok);
        EXPECT_EQ(metadata.runtime_id, instance.handle.runtime_id);
        EXPECT_EQ(
            metadata.configuration_generation,
            instance.handle.configuration_generation);
        EXPECT_EQ(metadata.retained_generation_count, 1u);
        EXPECT_EQ(metadata.retained_record_count, 1u);
        artifact_checksums.insert(metadata.artifact_checksum);
    }
    EXPECT_EQ(runtime_ids.size(), instance_count);
    EXPECT_EQ(generation_ids.size(), instance_count);
    EXPECT_EQ(checkpoint_buffers.size(), instance_count);
    EXPECT_EQ(artifact_checksums.size(), instance_count);

    auto stale_result = rt::LiveControlAdmissionResult::invalid;
    EXPECT_EQ(
        instances[0].runtime->stage_live_control_update(
            stale, stale_update, stale_payload, stale_result),
        rt::Status::ok);
    EXPECT_EQ(stale_result, rt::LiveControlAdmissionResult::stale);

    std::barrier stop_gate(static_cast<std::ptrdiff_t>(instance_count));
    for (std::size_t index = 0; index < instances.size(); ++index) {
        threads[index] = std::thread([&, index] {
            stop_gate.arrive_and_wait();
            instances[index].status = instances[index].runtime->stop();
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (const auto& instance : instances) {
        EXPECT_EQ(instance.status, rt::Status::ok);
    }
}
