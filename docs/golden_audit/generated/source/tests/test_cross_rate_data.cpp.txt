#include <gtest/gtest.h>

#include <rt/mock_device.hpp>
#include <rt/runtime.hpp>

#include "rt/src/cross_rate_data.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using namespace std::chrono_literals;

rt::CallbackResult count_callback(
    void* user_data,
    const rt::CallbackContext&) {
    if (user_data) {
        ++*static_cast<std::size_t*>(user_data);
    }
    return rt::CallbackResult::ok;
}

rt::CallbackResult submit_noop(
    void* user_data,
    const rt::DeviceCallbackContext&,
    rt::DeviceSubmission& submission) {
    if (user_data) {
        ++*static_cast<std::size_t*>(user_data);
    }
    submission.timeout_ns = 10'000;
    submission.opcode = rt::mock_device_opcode_noop;
    return rt::CallbackResult::ok;
}

struct ManualClock final : rt::RuntimeClock {
    std::uint64_t now = 100;
    std::uint64_t now_ns() noexcept override { return now; }
    rt::Status sleep_until_ns(std::uint64_t release) noexcept override {
        now = release;
        return rt::Status::ok;
    }
    bool supports_absolute_sleep() const noexcept override { return true; }
};

struct RuntimeFixture {
    rt::Runtime runtime;
    rt::PhaseHandle producer;
    rt::PhaseHandle consumer;
    rt::RateDomainHandle producer_domain;
    rt::RateDomainHandle consumer_domain;
    rt::CrossRateChannelHandle channel;
};

struct CountingProvider {
    std::size_t calls = 0;

    static rt::Status acquire(
        void* opaque,
        const rt::MemoryProviderAcquireRequest&,
        rt::MemoryProviderAllocation&) noexcept {
        ++static_cast<CountingProvider*>(opaque)->calls;
        return rt::Status::resource_exhausted;
    }
    static rt::Status apply(
        void* opaque,
        void*,
        const rt::MemoryPolicy&,
        rt::MemoryProviderObservation&) noexcept {
        ++static_cast<CountingProvider*>(opaque)->calls;
        return rt::Status::internal_error;
    }
    static rt::Status observe(
        void* opaque,
        void*,
        const rt::MemoryPolicy&,
        rt::MemoryProviderObservation&) noexcept {
        ++static_cast<CountingProvider*>(opaque)->calls;
        return rt::Status::internal_error;
    }
    static rt::Status rollback(
        void* opaque,
        void*,
        const rt::MemoryPolicy&,
        const rt::MemoryProviderObservation&) noexcept {
        ++static_cast<CountingProvider*>(opaque)->calls;
        return rt::Status::internal_error;
    }
    static void release(
        void* opaque,
        void*,
        rt::RollbackIntent) noexcept {
        ++static_cast<CountingProvider*>(opaque)->calls;
    }

    rt::MemoryProvider table() noexcept {
        rt::MemoryProvider provider;
        provider.capabilities =
            rt::memory_provider_capability_bit(
                rt::MemoryProviderCapability::policy_operations) |
            rt::memory_provider_capability_bit(
                rt::MemoryProviderCapability::independent_observation);
        provider.user_data = this;
        provider.acquire = &acquire;
        provider.apply = &apply;
        provider.observe = &observe;
        provider.rollback = &rollback;
        provider.release = &release;
        return provider;
    }
};

RuntimeFixture make_fixture(
    std::uint64_t producer_period = 4,
    std::uint64_t consumer_period = 3,
    std::uint32_t producer_substeps = 2,
    std::uint32_t consumer_substeps = 2,
    std::uint64_t maximum_age =
        std::numeric_limits<std::uint64_t>::max()) {
    RuntimeFixture fixture;
    EXPECT_EQ(
        fixture.runtime.register_callback(
            {"producer", &count_callback, nullptr},
            fixture.producer),
        rt::Status::ok);
    EXPECT_EQ(
        fixture.runtime.register_callback(
            {"consumer", &count_callback, nullptr},
            fixture.consumer),
        rt::Status::ok);
    // Consumer-first domain registration exercises prior-cycle wrap and the
    // rule that a later producer at the same timestamp is not yet visible.
    EXPECT_EQ(
        fixture.runtime.register_rate_domain(
            {"consumer.rate", consumer_period, consumer_substeps},
            fixture.consumer_domain),
        rt::Status::ok);
    EXPECT_EQ(
        fixture.runtime.register_rate_domain(
            {"producer.rate", producer_period, producer_substeps},
            fixture.producer_domain),
        rt::Status::ok);
    EXPECT_EQ(
        fixture.runtime.bind_phase_to_rate_domain(
            fixture.consumer,
            fixture.consumer_domain),
        rt::Status::ok);
    EXPECT_EQ(
        fixture.runtime.bind_phase_to_rate_domain(
            fixture.producer,
            fixture.producer_domain),
        rt::Status::ok);
    const std::array initial{
        std::byte{0x10},
        std::byte{0x20},
        std::byte{0x30},
        std::byte{0x40},
    };
    EXPECT_EQ(
        fixture.runtime.register_cross_rate_channel(
            {
                "producer.to.consumer",
                fixture.producer,
                fixture.consumer,
                initial.size(),
                initial,
                rt::CrossRateMode::sample_and_hold,
                maximum_age,
            },
            fixture.channel),
        rt::Status::ok);
    return fixture;
}

struct ExpectedSelection {
    rt::CrossRateSelectionHorizon horizon;
    std::size_t consumer_reference_index;
    std::size_t producer_reference_index;
    std::int32_t source_cycle_offset;
    std::uint64_t age;
    rt::CrossRateSampleProvenance provenance;
    bool held;
    rt::CrossRateFreshness freshness;
};

std::vector<ExpectedSelection> independent_selections(
    const std::vector<rt::ReferenceRelease>& releases,
    std::uint64_t supercycle,
    rt::PhaseHandle producer,
    rt::PhaseHandle consumer,
    std::uint64_t maximum_age) {
    std::vector<std::size_t> producer_indexes;
    std::vector<std::size_t> consumer_indexes;
    for (std::size_t index = 0; index < releases.size(); ++index) {
        if (releases[index].phase == producer) {
            producer_indexes.push_back(index);
        }
        if (releases[index].phase == consumer) {
            consumer_indexes.push_back(index);
        }
    }
    struct Source {
        rt::CrossRateSampleProvenance provenance;
        std::size_t reference;
        std::int32_t cycle;
        std::uint64_t age;
    };
    std::vector<Source> first_sources;
    std::vector<Source> repeating_sources;
    for (const auto consumer_index : consumer_indexes) {
        std::size_t latest = rt::invalid_reference_release_index;
        for (const auto producer_index : producer_indexes) {
            if (producer_index >= consumer_index) {
                break;
            }
            latest = producer_index;
        }
        const auto consumer_time = releases[consumer_index].release_time_ns;
        if (latest == rt::invalid_reference_release_index) {
            first_sources.push_back({
                rt::CrossRateSampleProvenance::initial_sample,
                rt::invalid_reference_release_index,
                0,
                consumer_time,
            });
            const auto wrap = producer_indexes.back();
            repeating_sources.push_back({
                rt::CrossRateSampleProvenance::produced,
                wrap,
                -1,
                (supercycle - releases[wrap].release_time_ns) + consumer_time,
            });
        } else {
            const auto age = consumer_time - releases[latest].release_time_ns;
            first_sources.push_back({
                rt::CrossRateSampleProvenance::produced,
                latest,
                0,
                age,
            });
            repeating_sources.push_back(first_sources.back());
        }
    }
    std::vector<ExpectedSelection> expected;
    for (std::size_t index = 0; index < consumer_indexes.size(); ++index) {
        const auto fresh = [&](std::uint64_t age) {
            return maximum_age == std::numeric_limits<std::uint64_t>::max() ||
                    age <= maximum_age
                ? rt::CrossRateFreshness::fresh
                : rt::CrossRateFreshness::stale;
        };
        const auto first_held = index != 0 &&
            std::tie(
                first_sources[index].provenance,
                first_sources[index].reference,
                first_sources[index].cycle) ==
            std::tie(
                first_sources[index - 1].provenance,
                first_sources[index - 1].reference,
                first_sources[index - 1].cycle);
        const auto previous_repeating = index == 0
            ? Source{
                  repeating_sources.back().provenance,
                  repeating_sources.back().reference,
                  repeating_sources.back().cycle - 1,
                  repeating_sources.back().age}
            : repeating_sources[index - 1];
        const auto repeating_held =
            std::tie(
                repeating_sources[index].provenance,
                repeating_sources[index].reference,
                repeating_sources[index].cycle) ==
            std::tie(
                previous_repeating.provenance,
                previous_repeating.reference,
                previous_repeating.cycle);
        expected.push_back({
            rt::CrossRateSelectionHorizon::first_supercycle,
            consumer_indexes[index],
            first_sources[index].reference,
            first_sources[index].cycle,
            first_sources[index].age,
            first_sources[index].provenance,
            first_held,
            fresh(first_sources[index].age),
        });
        expected.push_back({
            rt::CrossRateSelectionHorizon::repeating_supercycle,
            consumer_indexes[index],
            repeating_sources[index].reference,
            repeating_sources[index].cycle,
            repeating_sources[index].age,
            repeating_sources[index].provenance,
            repeating_held,
            fresh(repeating_sources[index].age),
        });
    }
    return expected;
}

void digest_u64(std::uint64_t& digest, std::uint64_t value) {
    constexpr std::uint64_t prime = 1099511628211ull;
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        digest ^= (value >> (byte * 8u)) & 0xffu;
        digest *= prime;
    }
}

std::uint64_t normalized_selection_digest(rt::Runtime& runtime) {
    std::uint64_t digest = 14695981039346656037ull;
    digest_u64(digest, runtime.reference_supercycle_ns());
    digest_u64(digest, runtime.cross_rate_selection_count());
    for (std::size_t index = 0;
         index < runtime.cross_rate_selection_count();
         ++index) {
        rt::CompiledCrossRateSelection selection;
        EXPECT_TRUE(runtime.compiled_cross_rate_selection_at(index, selection));
        digest_u64(digest, selection.channel_registration_index);
        digest_u64(digest, static_cast<std::uint64_t>(selection.horizon));
        digest_u64(digest, selection.consumer_reference_index);
        digest_u64(digest, selection.consumer_release_sequence);
        digest_u64(digest, selection.consumer_substep_ordinal);
        digest_u64(digest, selection.producer_reference_index);
        digest_u64(digest, selection.producer_release_sequence);
        digest_u64(digest, selection.producer_substep_ordinal);
        digest_u64(
            digest,
            static_cast<std::uint32_t>(selection.source_cycle_offset));
        digest_u64(digest, selection.age_ns);
        digest_u64(digest, static_cast<std::uint64_t>(selection.provenance));
        digest_u64(digest, selection.held ? 1u : 0u);
        digest_u64(digest, static_cast<std::uint64_t>(selection.freshness));
    }
    return digest;
}

rt::CheckpointMetadata metadata(rt::Runtime& runtime) {
    std::size_t required = 0;
    EXPECT_EQ(runtime.checkpoint_size(required), rt::Status::ok);
    std::vector<std::byte> checkpoint(required);
    rt::ArtifactWriteResult result;
    EXPECT_EQ(runtime.write_checkpoint(0, checkpoint, result), rt::Status::ok);
    rt::CheckpointMetadata output;
    EXPECT_EQ(
        rt::inspect_checkpoint_artifact(checkpoint, output),
        rt::Status::ok);
    return output;
}

std::uint64_t channel_graph_id(
    std::string_view name,
    bool reverse_endpoints,
    std::size_t payload_size,
    std::uint64_t maximum_age,
    std::byte last_initial) {
    rt::Runtime runtime;
    rt::PhaseHandle first;
    rt::PhaseHandle second;
    EXPECT_EQ(
        runtime.register_callback({"first", &count_callback, nullptr}, first),
        rt::Status::ok);
    EXPECT_EQ(
        runtime.register_callback({"second", &count_callback, nullptr}, second),
        rt::Status::ok);
    rt::RateDomainHandle fast;
    rt::RateDomainHandle slow;
    EXPECT_EQ(runtime.register_rate_domain({"fast", 2}, fast), rt::Status::ok);
    EXPECT_EQ(runtime.register_rate_domain({"slow", 3}, slow), rt::Status::ok);
    EXPECT_EQ(runtime.bind_phase_to_rate_domain(first, fast), rt::Status::ok);
    EXPECT_EQ(runtime.bind_phase_to_rate_domain(second, slow), rt::Status::ok);
    std::vector<std::byte> initial(payload_size, std::byte{0x2a});
    initial.back() = last_initial;
    rt::CrossRateChannelHandle channel;
    EXPECT_EQ(
        runtime.register_cross_rate_channel(
            {
                name,
                reverse_endpoints ? second : first,
                reverse_endpoints ? first : second,
                payload_size,
                initial,
                rt::CrossRateMode::sample_and_hold,
                maximum_age,
            },
            channel),
        rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();
    return metadata(runtime).graph_id;
}

} // namespace

TEST(CrossRateData, PublicModelCopiesAndFreezesWithLifecycleInspectors) {
    auto fixture = make_fixture();
    EXPECT_TRUE(fixture.channel.valid());
    EXPECT_FALSE(fixture.runtime.cross_rate_model_enabled());
    EXPECT_EQ(fixture.runtime.cross_rate_channel_count(), 0u);

    ASSERT_EQ(fixture.runtime.finalize(), rt::Status::ok)
        << fixture.runtime.last_error();
    EXPECT_TRUE(fixture.runtime.cross_rate_model_enabled());
    EXPECT_EQ(fixture.runtime.cross_rate_channel_count(), 1u);
    rt::CompiledCrossRateChannel descriptor;
    ASSERT_TRUE(fixture.runtime.compiled_cross_rate_channel_at(0, descriptor));
    EXPECT_EQ(descriptor.channel, fixture.channel);
    EXPECT_STREQ(descriptor.name.data(), "producer.to.consumer");
    EXPECT_EQ(descriptor.producer, fixture.producer);
    EXPECT_EQ(descriptor.consumer, fixture.consumer);
    EXPECT_NE(descriptor.producer_domain, descriptor.consumer_domain);
    EXPECT_EQ(descriptor.payload_size, 4u);
    EXPECT_EQ(descriptor.snapshot_slot_count, 2u);
    EXPECT_EQ(descriptor.snapshot_bytes, 8u);

    std::array<std::byte, 4> copied{};
    ASSERT_EQ(
        fixture.runtime.copy_cross_rate_initial_sample(0, copied),
        rt::Status::ok);
    EXPECT_EQ(
        copied,
        (std::array{
            std::byte{0x10},
            std::byte{0x20},
            std::byte{0x30},
            std::byte{0x40}}));
    std::array<std::byte, 3> short_output{};
    EXPECT_EQ(
        fixture.runtime.copy_cross_rate_initial_sample(0, short_output),
        rt::Status::capacity_exceeded);
    std::array<std::byte, 5> long_output{};
    EXPECT_EQ(
        fixture.runtime.copy_cross_rate_initial_sample(0, long_output),
        rt::Status::invalid_argument);
    rt::CompiledCrossRateChannel invalid_descriptor;
    invalid_descriptor.payload_size = 99;
    EXPECT_FALSE(
        fixture.runtime.compiled_cross_rate_channel_at(1, invalid_descriptor));
    EXPECT_EQ(invalid_descriptor.payload_size, 0u);
    EXPECT_EQ(
        fixture.runtime.register_cross_rate_channel({}, fixture.channel),
        rt::Status::invalid_state);
    rt::ObservabilityMetadata observability;
    ASSERT_EQ(
        fixture.runtime.observability_metadata(observability),
        rt::Status::ok);
    EXPECT_EQ(observability.schema_version, rt::observability_schema_version);
    const std::array input_payload{std::byte{0x77}};
    const std::array input_records{
        rt::ReplayInputRecord{
            {0, 1ns, std::nullopt},
            1,
            input_payload,
        },
    };
    std::array<std::byte, 1024> input_log{};
    rt::ArtifactWriteResult input_result;
    ASSERT_EQ(
        fixture.runtime.write_input_log(
            input_records,
            input_log,
            input_result),
        rt::Status::ok);
    rt::InputLogMetadata input_metadata;
    ASSERT_EQ(
        rt::inspect_input_log_artifact(
            std::span<const std::byte>(input_log).first(
                input_result.bytes_written),
            input_metadata),
        rt::Status::ok);
    EXPECT_EQ(input_metadata.schema_version, rt::input_log_schema_version);
    ASSERT_EQ(fixture.runtime.start(), rt::Status::ok);
    EXPECT_TRUE(fixture.runtime.compiled_cross_rate_channel_at(0, descriptor));
    ASSERT_EQ(fixture.runtime.stop(), rt::Status::ok);
    EXPECT_TRUE(fixture.runtime.compiled_cross_rate_channel_at(0, descriptor));
}

TEST(CrossRateData, SelectionMatchesIndependentCompleteSupercycle) {
    constexpr std::uint64_t maximum_age = 4;
    auto fixture = make_fixture(4, 3, 2, 2, maximum_age);
    ASSERT_EQ(fixture.runtime.finalize(), rt::Status::ok)
        << fixture.runtime.last_error();
    std::vector<rt::ReferenceRelease> releases(
        fixture.runtime.reference_release_count());
    for (std::size_t index = 0; index < releases.size(); ++index) {
        ASSERT_TRUE(fixture.runtime.reference_release_at(index, releases[index]));
    }
    const auto expected = independent_selections(
        releases,
        fixture.runtime.reference_supercycle_ns(),
        fixture.producer,
        fixture.consumer,
        maximum_age);
    ASSERT_EQ(fixture.runtime.cross_rate_selection_count(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        rt::CompiledCrossRateSelection actual;
        ASSERT_TRUE(
            fixture.runtime.compiled_cross_rate_selection_at(index, actual));
        EXPECT_EQ(actual.channel, fixture.channel);
        EXPECT_EQ(actual.producer, fixture.producer);
        EXPECT_EQ(actual.consumer, fixture.consumer);
        EXPECT_EQ(actual.horizon, expected[index].horizon);
        EXPECT_EQ(
            actual.consumer_reference_index,
            expected[index].consumer_reference_index);
        EXPECT_EQ(
            actual.consumer_release_sequence,
            releases[expected[index].consumer_reference_index]
                .domain_release_sequence);
        EXPECT_EQ(
            actual.consumer_substep_ordinal,
            releases[expected[index].consumer_reference_index]
                .substep_ordinal);
        EXPECT_EQ(
            actual.producer_reference_index,
            expected[index].producer_reference_index);
        EXPECT_EQ(
            actual.source_cycle_offset,
            expected[index].source_cycle_offset);
        EXPECT_EQ(actual.age_ns, expected[index].age);
        EXPECT_EQ(actual.provenance, expected[index].provenance);
        EXPECT_EQ(actual.held, expected[index].held);
        EXPECT_EQ(actual.freshness, expected[index].freshness);
        if (actual.provenance == rt::CrossRateSampleProvenance::produced) {
            const auto& source = releases[actual.producer_reference_index];
            EXPECT_EQ(
                actual.producer_release_sequence,
                source.domain_release_sequence);
            EXPECT_EQ(
                actual.producer_substep_ordinal,
                source.substep_ordinal);
        } else {
            EXPECT_EQ(
                actual.producer_reference_index,
                rt::invalid_reference_release_index);
            EXPECT_EQ(
                actual.producer_release_sequence,
                rt::invalid_cross_rate_sequence);
            EXPECT_EQ(
                actual.producer_substep_ordinal,
                rt::invalid_cross_rate_substep);
        }
    }
    rt::CompiledCrossRateSelection first;
    rt::CompiledCrossRateSelection repeating;
    ASSERT_TRUE(fixture.runtime.compiled_cross_rate_selection_at(0, first));
    ASSERT_TRUE(fixture.runtime.compiled_cross_rate_selection_at(1, repeating));
    EXPECT_EQ(
        first.provenance,
        rt::CrossRateSampleProvenance::initial_sample);
    EXPECT_FALSE(first.held);
    EXPECT_EQ(
        repeating.provenance,
        rt::CrossRateSampleProvenance::produced);
    EXPECT_EQ(repeating.source_cycle_offset, -1);
    EXPECT_EQ(repeating.age_ns, 4u);
    EXPECT_TRUE(repeating.held);
    EXPECT_EQ(repeating.freshness, rt::CrossRateFreshness::fresh);
    EXPECT_EQ(
        normalized_selection_digest(fixture.runtime),
        4'944'802'289'737'772'168ull);
}

TEST(CrossRateData, SelectionAgreementCoversRateAndRegistrationPermutations) {
    struct Scenario {
        std::uint64_t producer_period;
        std::uint64_t consumer_period;
        std::uint32_t producer_substeps;
        std::uint32_t consumer_substeps;
        bool producer_callback_first;
        bool producer_domain_first;
    };
    const std::array scenarios{
        Scenario{2, 4, 1, 1, true, true},
        Scenario{5, 7, 2, 3, true, false},
        Scenario{5, 7, 3, 2, false, true},
        Scenario{6, 4, 2, 3, false, false},
    };
    for (const auto& scenario : scenarios) {
        rt::Runtime runtime;
        rt::PhaseHandle producer;
        rt::PhaseHandle consumer;
        const auto register_producer = [&] {
            return runtime.register_callback(
                {"producer", &count_callback, nullptr}, producer);
        };
        const auto register_consumer = [&] {
            return runtime.register_callback(
                {"consumer", &count_callback, nullptr}, consumer);
        };
        ASSERT_EQ(
            scenario.producer_callback_first
                ? register_producer()
                : register_consumer(),
            rt::Status::ok);
        ASSERT_EQ(
            scenario.producer_callback_first
                ? register_consumer()
                : register_producer(),
            rt::Status::ok);
        rt::RateDomainHandle producer_domain;
        rt::RateDomainHandle consumer_domain;
        const auto register_producer_domain = [&] {
            return runtime.register_rate_domain(
                {"producer.rate", scenario.producer_period,
                 scenario.producer_substeps},
                producer_domain);
        };
        const auto register_consumer_domain = [&] {
            return runtime.register_rate_domain(
                {"consumer.rate", scenario.consumer_period,
                 scenario.consumer_substeps},
                consumer_domain);
        };
        ASSERT_EQ(
            scenario.producer_domain_first
                ? register_producer_domain()
                : register_consumer_domain(),
            rt::Status::ok);
        ASSERT_EQ(
            scenario.producer_domain_first
                ? register_consumer_domain()
                : register_producer_domain(),
            rt::Status::ok);
        ASSERT_EQ(
            runtime.bind_phase_to_rate_domain(producer, producer_domain),
            rt::Status::ok);
        ASSERT_EQ(
            runtime.bind_phase_to_rate_domain(consumer, consumer_domain),
            rt::Status::ok);
        const std::array initial{std::byte{0x42}};
        rt::CrossRateChannelHandle channel;
        ASSERT_EQ(
            runtime.register_cross_rate_channel(
                {"edge", producer, consumer, 1, initial,
                 rt::CrossRateMode::sample_and_hold, 3},
                channel),
            rt::Status::ok);
        ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();
        std::vector<rt::ReferenceRelease> releases(
            runtime.reference_release_count());
        for (std::size_t index = 0; index < releases.size(); ++index) {
            ASSERT_TRUE(runtime.reference_release_at(index, releases[index]));
        }
        const auto expected = independent_selections(
            releases,
            runtime.reference_supercycle_ns(),
            producer,
            consumer,
            3);
        ASSERT_EQ(runtime.cross_rate_selection_count(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            rt::CompiledCrossRateSelection actual;
            ASSERT_TRUE(runtime.compiled_cross_rate_selection_at(index, actual));
            EXPECT_EQ(actual.channel, channel);
            EXPECT_EQ(actual.horizon, expected[index].horizon);
            EXPECT_EQ(
                actual.consumer_reference_index,
                expected[index].consumer_reference_index);
            EXPECT_EQ(
                actual.producer_reference_index,
                expected[index].producer_reference_index);
            EXPECT_EQ(
                actual.source_cycle_offset,
                expected[index].source_cycle_offset);
            EXPECT_EQ(actual.age_ns, expected[index].age);
            EXPECT_EQ(actual.provenance, expected[index].provenance);
            EXPECT_EQ(actual.held, expected[index].held);
            EXPECT_EQ(actual.freshness, expected[index].freshness);
        }
    }
}

TEST(CrossRateData, SameTimestampVisibilityAndFreshnessBoundariesAreExact) {
    auto consumer_first = make_fixture(4, 3, 1, 1, 0);
    ASSERT_EQ(consumer_first.runtime.finalize(), rt::Status::ok);
    rt::CompiledCrossRateSelection first;
    rt::CompiledCrossRateSelection steady;
    ASSERT_TRUE(
        consumer_first.runtime.compiled_cross_rate_selection_at(0, first));
    ASSERT_TRUE(
        consumer_first.runtime.compiled_cross_rate_selection_at(1, steady));
    EXPECT_EQ(
        first.provenance,
        rt::CrossRateSampleProvenance::initial_sample);
    EXPECT_EQ(first.age_ns, 0u);
    EXPECT_EQ(first.freshness, rt::CrossRateFreshness::fresh);
    EXPECT_EQ(steady.source_cycle_offset, -1);
    EXPECT_EQ(steady.freshness, rt::CrossRateFreshness::stale);

    rt::Runtime producer_first;
    rt::PhaseHandle producer;
    rt::PhaseHandle consumer;
    ASSERT_EQ(
        producer_first.register_callback(
            {"producer", &count_callback, nullptr}, producer),
        rt::Status::ok);
    ASSERT_EQ(
        producer_first.register_callback(
            {"consumer", &count_callback, nullptr}, consumer),
        rt::Status::ok);
    rt::RateDomainHandle producer_domain;
    rt::RateDomainHandle consumer_domain;
    ASSERT_EQ(
        producer_first.register_rate_domain(
            {"producer.rate", 4, 2}, producer_domain),
        rt::Status::ok);
    ASSERT_EQ(
        producer_first.register_rate_domain(
            {"consumer.rate", 3, 1}, consumer_domain),
        rt::Status::ok);
    ASSERT_EQ(
        producer_first.bind_phase_to_rate_domain(producer, producer_domain),
        rt::Status::ok);
    ASSERT_EQ(
        producer_first.bind_phase_to_rate_domain(consumer, consumer_domain),
        rt::Status::ok);
    const std::array initial{std::byte{1}};
    rt::CrossRateChannelHandle channel;
    ASSERT_EQ(
        producer_first.register_cross_rate_channel(
            {"edge", producer, consumer, 1, initial, {}, 0},
            channel),
        rt::Status::ok);
    ASSERT_EQ(producer_first.finalize(), rt::Status::ok);
    ASSERT_TRUE(producer_first.compiled_cross_rate_selection_at(0, first));
    EXPECT_EQ(first.provenance, rt::CrossRateSampleProvenance::produced);
    EXPECT_EQ(first.age_ns, 0u);
    EXPECT_EQ(first.producer_substep_ordinal, 1u);
    EXPECT_EQ(first.freshness, rt::CrossRateFreshness::fresh);

    auto unbounded = make_fixture(
        4,
        3,
        1,
        1,
        std::numeric_limits<std::uint64_t>::max());
    ASSERT_EQ(unbounded.runtime.finalize(), rt::Status::ok);
    ASSERT_TRUE(
        unbounded.runtime.compiled_cross_rate_selection_at(1, steady));
    EXPECT_EQ(steady.freshness, rt::CrossRateFreshness::fresh);
}

TEST(CrossRateData, ValidationIsOwnedBoundedTransactionalAndCorrectable) {
    rt::Runtime first;
    rt::Runtime second;
    rt::PhaseHandle producer;
    rt::PhaseHandle consumer;
    rt::PhaseHandle foreign;
    ASSERT_EQ(
        first.register_callback({"producer", &count_callback, nullptr}, producer),
        rt::Status::ok);
    ASSERT_EQ(
        first.register_callback({"consumer", &count_callback, nullptr}, consumer),
        rt::Status::ok);
    ASSERT_EQ(
        second.register_callback({"foreign", &count_callback, nullptr}, foreign),
        rt::Status::ok);
    const std::array initial{std::byte{1}, std::byte{2}};
    rt::CrossRateChannelHandle channel;
    rt::CrossRateChannelHandle ignored;
    EXPECT_EQ(
        first.register_cross_rate_channel(
            {"bad name", producer, consumer, 2, initial}, ignored),
        rt::Status::invalid_argument);
    EXPECT_EQ(
        first.register_cross_rate_channel(
            {"foreign", producer, foreign, 2, initial}, ignored),
        rt::Status::invalid_handle);
    EXPECT_EQ(
        first.register_cross_rate_channel(
            {"self", producer, producer, 2, initial}, ignored),
        rt::Status::invalid_argument);
    EXPECT_EQ(
        first.register_cross_rate_channel(
            {"zero", producer, consumer, 0, {}}, ignored),
        rt::Status::invalid_argument);
    EXPECT_EQ(
        first.register_cross_rate_channel(
            {"mismatch", producer, consumer, 1, initial}, ignored),
        rt::Status::invalid_argument);
    EXPECT_EQ(
        first.register_cross_rate_channel(
            {
                "mode",
                producer,
                consumer,
                2,
                initial,
                static_cast<rt::CrossRateMode>(255),
            },
            ignored),
        rt::Status::invalid_argument);
    ASSERT_EQ(
        first.register_cross_rate_channel(
            {"edge", producer, consumer, 2, initial}, channel),
        rt::Status::ok);
    EXPECT_EQ(
        first.register_cross_rate_channel(
            {"edge", consumer, producer, 2, initial}, ignored),
        rt::Status::invalid_argument);
    EXPECT_EQ(
        first.register_cross_rate_channel(
            {"other", producer, consumer, 2, initial}, ignored),
        rt::Status::invalid_argument);
    EXPECT_EQ(first.finalize(), rt::Status::invalid_config);
    EXPECT_EQ(first.state(), rt::RuntimeState::configuring);
    EXPECT_FALSE(first.cross_rate_model_enabled());
    EXPECT_EQ(first.cross_rate_selection_count(), 0u);

    rt::RateDomainHandle first_domain;
    ASSERT_EQ(
        first.register_rate_domain({"shared", 2}, first_domain),
        rt::Status::ok);
    ASSERT_EQ(
        first.bind_phase_to_rate_domain(producer, first_domain),
        rt::Status::ok);
    ASSERT_EQ(
        first.bind_phase_to_rate_domain(consumer, first_domain),
        rt::Status::ok);
    EXPECT_EQ(first.finalize(), rt::Status::invalid_config);
    rt::PhaseHandle corrected_consumer;
    ASSERT_EQ(
        first.register_callback(
            {"corrected.consumer", &count_callback, nullptr},
            corrected_consumer),
        rt::Status::ok);
    rt::RateDomainHandle second_domain;
    ASSERT_EQ(
        first.register_rate_domain({"other.rate", 3}, second_domain),
        rt::Status::ok);
    ASSERT_EQ(
        first.bind_phase_to_rate_domain(corrected_consumer, second_domain),
        rt::Status::ok);
    ASSERT_EQ(
        first.replace_cross_rate_channel(
            channel,
            {"edge", producer, corrected_consumer, 2, initial}),
        rt::Status::ok);
    ASSERT_EQ(first.finalize(), rt::Status::ok) << first.last_error();
    EXPECT_TRUE(first.cross_rate_model_enabled());
    EXPECT_EQ(
        first.replace_cross_rate_channel(channel, {}),
        rt::Status::invalid_state);
    EXPECT_EQ(
        second.replace_cross_rate_channel(channel, {}),
        rt::Status::invalid_handle);
}

TEST(CrossRateData, AggregateCapacityFailsBeforeProviderAndCanBeCorrected) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 34;
    config.executor_queue_capacity = 64;
    config.task_scratch_slots = 64;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    CountingProvider provider;
    ASSERT_EQ(runtime.set_memory_provider(provider.table()), rt::Status::ok);
    rt::RateDomainHandle producer_domain;
    rt::RateDomainHandle consumer_domain;
    ASSERT_EQ(
        runtime.register_rate_domain({"producer.rate", 2}, producer_domain),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_rate_domain({"consumer.rate", 3}, consumer_domain),
        rt::Status::ok);
    std::vector<std::byte> initial(
        rt::cross_rate_payload_capacity,
        std::byte{0x5a});
    std::vector<rt::CrossRateChannelHandle> channels;
    for (std::size_t index = 0; index < 17; ++index) {
        rt::PhaseHandle producer;
        rt::PhaseHandle consumer;
        ASSERT_EQ(
            runtime.register_callback(
                {"producer." + std::to_string(index), &count_callback, nullptr},
                producer),
            rt::Status::ok);
        ASSERT_EQ(
            runtime.register_callback(
                {"consumer." + std::to_string(index), &count_callback, nullptr},
                consumer),
            rt::Status::ok);
        ASSERT_EQ(
            runtime.bind_phase_to_rate_domain(producer, producer_domain),
            rt::Status::ok);
        ASSERT_EQ(
            runtime.bind_phase_to_rate_domain(consumer, consumer_domain),
            rt::Status::ok);
        rt::CrossRateChannelHandle channel;
        ASSERT_EQ(
            runtime.register_cross_rate_channel(
                {"edge." + std::to_string(index), producer, consumer,
                 initial.size(), initial},
                channel),
            rt::Status::ok);
        channels.push_back(channel);
    }
    EXPECT_EQ(runtime.finalize(), rt::Status::capacity_exceeded);
    EXPECT_EQ(provider.calls, 0u);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
    EXPECT_EQ(runtime.cross_rate_channel_count(), 0u);
    const std::array corrected{std::byte{0x01}};
    for (std::size_t index = 0; index < channels.size(); ++index) {
        const auto producer_index = index * 2;
        const auto consumer_index = producer_index + 1;
        ASSERT_EQ(
            runtime.replace_cross_rate_channel(
                channels[index],
                {"edge." + std::to_string(index),
                 rt::PhaseHandle{channels[index].owner(),
                                 static_cast<std::uint32_t>(producer_index)},
                 rt::PhaseHandle{channels[index].owner(),
                                 static_cast<std::uint32_t>(consumer_index)},
                 corrected.size(), corrected}),
            rt::Status::ok);
    }
    // The rejecting provider is deliberately still untouched; replacing it
    // is outside the channel transaction, so a successful retry would invoke
    // it and fail at its configured acquisition boundary.
    EXPECT_EQ(runtime.finalize(), rt::Status::resource_exhausted)
        << runtime.last_error();
    EXPECT_GT(provider.calls, 0u);
}

TEST(CrossRateData, DeviceEndpointIsRejectedBeforeOwnershipOrExecution) {
    rt::MockDeviceBackend backend({4, 1, 1, 1'000});
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 2;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    config.device_outstanding_capacity = 4;
    config.device_completion_batch = 1;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    rt::DeviceBackendHandle backend_handle;
    ASSERT_EQ(
        runtime.register_device_backend({"mock", backend.api()}, backend_handle),
        rt::Status::ok);
    rt::PhaseHandle cpu;
    rt::PhaseHandle device;
    ASSERT_EQ(
        runtime.register_callback({"cpu", &count_callback, nullptr}, cpu),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_device_phase(
            {"device", backend_handle, &submit_noop, nullptr}, device),
        rt::Status::ok);
    rt::RateDomainHandle cpu_domain;
    rt::RateDomainHandle device_domain;
    ASSERT_EQ(runtime.register_rate_domain({"cpu.rate", 2}, cpu_domain), rt::Status::ok);
    ASSERT_EQ(runtime.register_rate_domain({"device.rate", 3}, device_domain), rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(cpu, cpu_domain), rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(device, device_domain), rt::Status::ok);
    const std::array initial{std::byte{1}};
    rt::CrossRateChannelHandle channel;
    ASSERT_EQ(
        runtime.register_cross_rate_channel(
            {"device.edge", device, cpu, 1, initial}, channel),
        rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
    EXPECT_FALSE(runtime.cross_rate_model_enabled());
}

TEST(CrossRateData, SnapshotStoreIsExactBoundedAndNeverSubstitutes) {
    rt::detail::SnapshotStore store;
    ASSERT_EQ(
        rt::detail::SnapshotStore::create(4, 2, store),
        rt::Status::ok);
    const std::array first{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const std::array second{
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    const std::array third{
        std::byte{9}, std::byte{10}, std::byte{11}, std::byte{12}};
    std::array<std::byte, 4> output{};
    output.fill(std::byte{0x7f});
    EXPECT_EQ(
        store.copy(1, output, rt::detail::SnapshotRetention::retain),
        rt::detail::SnapshotStoreResult::not_ready);
    EXPECT_EQ(output[0], std::byte{0x7f});
    EXPECT_EQ(store.publish(1, first), rt::detail::SnapshotStoreResult::ok);
    EXPECT_EQ(
        store.copy(1, output, rt::detail::SnapshotRetention::retain),
        rt::detail::SnapshotStoreResult::ok);
    EXPECT_EQ(output, first);
    output.fill(std::byte{0});
    EXPECT_EQ(
        store.copy(1, output, rt::detail::SnapshotRetention::retain),
        rt::detail::SnapshotStoreResult::ok);
    EXPECT_EQ(output, first);
    EXPECT_EQ(store.publish(2, second), rt::detail::SnapshotStoreResult::ok);
    EXPECT_EQ(
        store.publish(3, third),
        rt::detail::SnapshotStoreResult::capacity_exceeded);
    EXPECT_EQ(
        store.copy(1, output, rt::detail::SnapshotRetention::retire),
        rt::detail::SnapshotStoreResult::ok);
    EXPECT_EQ(store.publish(3, third), rt::detail::SnapshotStoreResult::ok);
    output.fill(std::byte{0x55});
    EXPECT_EQ(
        store.copy(1, output, rt::detail::SnapshotRetention::retain),
        rt::detail::SnapshotStoreResult::stale_generation);
    EXPECT_EQ(output[0], std::byte{0x55});
    EXPECT_EQ(
        store.copy(2, output, rt::detail::SnapshotRetention::retire),
        rt::detail::SnapshotStoreResult::ok);
    EXPECT_EQ(output, second);
    std::array<std::byte, 3> malformed{};
    EXPECT_EQ(
        store.publish(4, malformed),
        rt::detail::SnapshotStoreResult::invalid_argument);
    EXPECT_EQ(
        store.copy(0, output, rt::detail::SnapshotRetention::retain),
        rt::detail::SnapshotStoreResult::invalid_argument);
}

TEST(CrossRateData, SnapshotStoreSpscConcurrencyHasDeterministicBytes) {
    constexpr std::size_t payload_size = 4096;
    constexpr std::uint64_t generations = 2'000;
    rt::detail::SnapshotStore store;
    ASSERT_EQ(
        rt::detail::SnapshotStore::create(payload_size, 2, store),
        rt::Status::ok);
    std::atomic<std::uint64_t> ready{0};
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<std::size_t> errors{0};
    std::thread producer([&] {
        std::vector<std::byte> bytes(payload_size);
        for (std::uint64_t generation = 1;
             generation <= generations;
             ++generation) {
            std::fill(
                bytes.begin(),
                bytes.end(),
                static_cast<std::byte>(generation & 0xffu));
            if (store.publish(generation, bytes) !=
                rt::detail::SnapshotStoreResult::ok) {
                errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            ready.store(generation, std::memory_order_release);
            while (consumed.load(std::memory_order_acquire) != generation) {
                std::this_thread::yield();
            }
        }
    });
    std::thread consumer_thread([&] {
        std::vector<std::byte> bytes(payload_size);
        for (std::uint64_t generation = 1;
             generation <= generations;
             ++generation) {
            while (ready.load(std::memory_order_acquire) != generation) {
                std::this_thread::yield();
            }
            const auto result = store.copy(
                generation,
                bytes,
                rt::detail::SnapshotRetention::retire);
            if (result != rt::detail::SnapshotStoreResult::ok ||
                !std::all_of(bytes.begin(), bytes.end(), [&](std::byte value) {
                    return value ==
                        static_cast<std::byte>(generation & 0xffu);
                })) {
                errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            consumed.store(generation, std::memory_order_release);
        }
    });
    producer.join();
    consumer_thread.join();
    EXPECT_EQ(errors.load(std::memory_order_acquire), 0u);
    EXPECT_EQ(consumed.load(std::memory_order_acquire), generations);
}

TEST(CrossRateData, SnapshotStoreSpscContentionPublishesCompleteGenerations) {
    constexpr std::size_t payload_size = 4096;
    constexpr std::uint64_t generations = 5'000;
    constexpr std::size_t retry_limit = 1'000'000;
    rt::detail::SnapshotStore store;
    ASSERT_EQ(
        rt::detail::SnapshotStore::create(payload_size, 2, store),
        rt::Status::ok);

    std::atomic<bool> failed{false};
    std::thread producer([&] {
        std::vector<std::byte> bytes(payload_size);
        for (std::uint64_t generation = 1;
             generation <= generations &&
             !failed.load(std::memory_order_acquire);
             ++generation) {
            for (std::size_t index = 0; index < bytes.size(); ++index) {
                bytes[index] = static_cast<std::byte>(
                    (generation * 17u + index * 29u) & 0xffu);
            }
            bool published = false;
            for (std::size_t retry = 0; retry < retry_limit; ++retry) {
                const auto result = store.publish(generation, bytes);
                if (result == rt::detail::SnapshotStoreResult::ok) {
                    published = true;
                    break;
                }
                if (result !=
                    rt::detail::SnapshotStoreResult::capacity_exceeded) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
                if (failed.load(std::memory_order_acquire)) {
                    return;
                }
                std::this_thread::yield();
            }
            if (!published) {
                failed.store(true, std::memory_order_release);
                return;
            }
        }
    });
    std::thread consumer_thread([&] {
        constexpr auto sentinel = std::byte{0xa5};
        std::vector<std::byte> bytes(payload_size, sentinel);
        for (std::uint64_t generation = 1;
             generation <= generations &&
             !failed.load(std::memory_order_acquire);
             ++generation) {
            bool copied = false;
            for (std::size_t retry = 0; retry < retry_limit; ++retry) {
                const auto result = store.copy(
                    generation,
                    bytes,
                    rt::detail::SnapshotRetention::retire);
                if (result == rt::detail::SnapshotStoreResult::ok) {
                    copied = true;
                    break;
                }
                if (result != rt::detail::SnapshotStoreResult::not_ready) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
                bool output_unchanged = true;
                for (std::size_t index = 0;
                     output_unchanged && index < bytes.size();
                     ++index) {
                    const auto expected = generation == 1
                        ? sentinel
                        : static_cast<std::byte>(
                              ((generation - 1) * 17u + index * 29u) &
                              0xffu);
                    output_unchanged = bytes[index] == expected;
                }
                if (!output_unchanged) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
                if (failed.load(std::memory_order_acquire)) {
                    return;
                }
                std::this_thread::yield();
            }
            bool bytes_match = copied;
            for (std::size_t index = 0;
                 bytes_match && index < bytes.size();
                 ++index) {
                bytes_match = bytes[index] == static_cast<std::byte>(
                    (generation * 17u + index * 29u) & 0xffu);
            }
            if (!bytes_match) {
                failed.store(true, std::memory_order_release);
                return;
            }
        }
    });
    producer.join();
    consumer_thread.join();

    EXPECT_FALSE(failed.load(std::memory_order_acquire));
}

TEST(MemoryPlan, CrossRateStorageIsExactlyInsideRateAndRuntimeControl) {
    rt::Runtime rate_only;
    rt::Runtime channel_runtime;
    rt::PhaseHandle rate_producer;
    rt::PhaseHandle rate_consumer;
    rt::PhaseHandle channel_producer;
    rt::PhaseHandle channel_consumer;
    ASSERT_EQ(rate_only.register_callback({"producer", &count_callback, nullptr}, rate_producer), rt::Status::ok);
    ASSERT_EQ(rate_only.register_callback({"consumer", &count_callback, nullptr}, rate_consumer), rt::Status::ok);
    ASSERT_EQ(channel_runtime.register_callback({"producer", &count_callback, nullptr}, channel_producer), rt::Status::ok);
    ASSERT_EQ(channel_runtime.register_callback({"consumer", &count_callback, nullptr}, channel_consumer), rt::Status::ok);
    rt::RateDomainHandle rate_fast;
    rt::RateDomainHandle rate_slow;
    rt::RateDomainHandle channel_fast;
    rt::RateDomainHandle channel_slow;
    ASSERT_EQ(rate_only.register_rate_domain({"fast", 2}, rate_fast), rt::Status::ok);
    ASSERT_EQ(rate_only.register_rate_domain({"slow", 3}, rate_slow), rt::Status::ok);
    ASSERT_EQ(channel_runtime.register_rate_domain({"fast", 2}, channel_fast), rt::Status::ok);
    ASSERT_EQ(channel_runtime.register_rate_domain({"slow", 3}, channel_slow), rt::Status::ok);
    ASSERT_EQ(rate_only.bind_phase_to_rate_domain(rate_producer, rate_fast), rt::Status::ok);
    ASSERT_EQ(rate_only.bind_phase_to_rate_domain(rate_consumer, rate_slow), rt::Status::ok);
    ASSERT_EQ(channel_runtime.bind_phase_to_rate_domain(channel_producer, channel_fast), rt::Status::ok);
    ASSERT_EQ(channel_runtime.bind_phase_to_rate_domain(channel_consumer, channel_slow), rt::Status::ok);
    const std::array initial{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    rt::CrossRateChannelHandle channel;
    ASSERT_EQ(
        channel_runtime.register_cross_rate_channel(
            {"edge", channel_producer, channel_consumer, 4, initial},
            channel),
        rt::Status::ok);
    ASSERT_EQ(rate_only.finalize(), rt::Status::ok);
    ASSERT_EQ(channel_runtime.finalize(), rt::Status::ok)
        << channel_runtime.last_error();
    rt::MemoryPlan rate_plan;
    rt::MemoryPlan channel_plan;
    ASSERT_TRUE(rate_only.memory_plan(rate_plan));
    ASSERT_TRUE(channel_runtime.memory_plan(channel_plan));
    EXPECT_EQ(rate_plan.cross_rate_channel_count, 0u);
    EXPECT_EQ(rate_plan.cross_rate_selection_count, 0u);
    EXPECT_EQ(channel_plan.cross_rate_channel_count, 1u);
    EXPECT_GT(channel_plan.cross_rate_selection_count, 0u);
    EXPECT_EQ(channel_plan.cross_rate_initial_sample_bytes, 4u);
    EXPECT_EQ(channel_plan.cross_rate_snapshot_slot_count, 2u);
    EXPECT_EQ(channel_plan.cross_rate_snapshot_bytes, 8u);
    EXPECT_EQ(
        channel_plan.runtime_control_bytes - rate_plan.runtime_control_bytes,
        channel_plan.rate_plan_bytes - rate_plan.rate_plan_bytes);
    EXPECT_EQ(
        channel_plan.planned_bytes,
        channel_plan.runtime_control_bytes +
            channel_plan.executor_control_bytes +
            channel_plan.device_control_bytes +
            channel_plan.phase_scratch_total_bytes +
            channel_plan.task_scratch_total_bytes +
            channel_plan.trace_storage_bytes);
    rt::CpuMemoryPolicyReport report;
    ASSERT_TRUE(channel_runtime.cpu_memory_policy_report(report));
    const auto row = std::find_if(
        report.memory.begin(),
        report.memory.begin() + report.memory_count,
        [](const auto& candidate) {
            return candidate.region == rt::memory_region_runtime_control;
        });
    ASSERT_NE(row, report.memory.begin() + report.memory_count);
    EXPECT_EQ(row->accounted_bytes, channel_plan.runtime_control_bytes);
}

TEST(HostRuntime, CrossRatePlanDoesNotChangeHostOrPeriodicDispatch) {
    ManualClock clock;
    rt::Runtime runtime(clock);
    rt::RuntimeConfig config;
    config.callback_capacity = 2;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    std::array<std::size_t, 2> calls{};
    rt::PhaseHandle producer;
    rt::PhaseHandle consumer;
    ASSERT_EQ(runtime.register_callback({"producer", &count_callback, &calls[0]}, producer), rt::Status::ok);
    ASSERT_EQ(runtime.register_callback({"consumer", &count_callback, &calls[1]}, consumer), rt::Status::ok);
    rt::RateDomainHandle fast;
    rt::RateDomainHandle slow;
    ASSERT_EQ(runtime.register_rate_domain({"fast", 2, 3}, fast), rt::Status::ok);
    ASSERT_EQ(runtime.register_rate_domain({"slow", 5, 1}, slow), rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(producer, fast), rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(consumer, slow), rt::Status::ok);
    const std::array initial{std::byte{0}};
    rt::CrossRateChannelHandle channel;
    ASSERT_EQ(runtime.register_cross_rate_channel({"edge", producer, consumer, 1, initial}, channel), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(runtime.step({0, 1ns, std::nullopt}), rt::Status::ok);
    EXPECT_EQ(calls, (std::array<std::size_t, 2>{1, 1}));
    rt::PeriodicRunConfig periodic;
    periodic.frame_count = 3;
    periodic.period = 10ns;
    periodic.relative_deadline = 10ns;
    ASSERT_EQ(runtime.run_periodic(periodic), rt::Status::ok);
    EXPECT_EQ(calls, (std::array<std::size_t, 2>{4, 4}));
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(HostRuntime, CrossRatePlanPreservesCompleteMockDeviceDispatch) {
    rt::MockDeviceBackend backend({4, 1, 1, 1'000});
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 3;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    config.device_outstanding_capacity = 4;
    config.device_completion_batch = 1;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    rt::DeviceBackendHandle backend_handle;
    ASSERT_EQ(
        runtime.register_device_backend({"mock", backend.api()}, backend_handle),
        rt::Status::ok);
    std::array<std::size_t, 2> cpu_calls{};
    std::size_t submissions = 0;
    rt::PhaseHandle producer;
    rt::PhaseHandle consumer;
    rt::PhaseHandle device;
    ASSERT_EQ(runtime.register_callback({"producer", &count_callback, &cpu_calls[0]}, producer), rt::Status::ok);
    ASSERT_EQ(runtime.register_callback({"consumer", &count_callback, &cpu_calls[1]}, consumer), rt::Status::ok);
    ASSERT_EQ(
        runtime.register_device_phase(
            {"device", backend_handle, &submit_noop, &submissions}, device),
        rt::Status::ok);
    rt::RateDomainHandle fast;
    rt::RateDomainHandle slow;
    rt::RateDomainHandle device_rate;
    ASSERT_EQ(runtime.register_rate_domain({"fast", 2}, fast), rt::Status::ok);
    ASSERT_EQ(runtime.register_rate_domain({"slow", 3}, slow), rt::Status::ok);
    ASSERT_EQ(runtime.register_rate_domain({"device.rate", 5}, device_rate), rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(producer, fast), rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(consumer, slow), rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(device, device_rate), rt::Status::ok);
    const std::array initial{std::byte{0}};
    rt::CrossRateChannelHandle channel;
    ASSERT_EQ(
        runtime.register_cross_rate_channel(
            {"edge", producer, consumer, 1, initial}, channel),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(runtime.step({0, 1ns, std::nullopt}), rt::Status::ok);
    EXPECT_EQ(cpu_calls, (std::array<std::size_t, 2>{1, 1}));
    EXPECT_EQ(submissions, 1u);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(DeterminismReplay, CrossRateIdentityCoversEverySemanticAndInitialByte) {
    const auto base = channel_graph_id("edge", false, 2, 7, std::byte{1});
    EXPECT_NE(base, channel_graph_id("renamed", false, 2, 7, std::byte{1}));
    EXPECT_NE(base, channel_graph_id("edge", true, 2, 7, std::byte{1}));
    EXPECT_NE(base, channel_graph_id("edge", false, 3, 7, std::byte{1}));
    EXPECT_NE(base, channel_graph_id("edge", false, 2, 8, std::byte{1}));
    EXPECT_NE(base, channel_graph_id("edge", false, 2, 7, std::byte{2}));

    rt::Runtime first;
    rt::Runtime second;
    rt::PhaseHandle first_phase;
    rt::PhaseHandle second_phase;
    ASSERT_EQ(first.register_callback({"phase", &count_callback, nullptr}, first_phase), rt::Status::ok);
    ASSERT_EQ(second.register_callback({"phase", &count_callback, nullptr}, second_phase), rt::Status::ok);
    rt::RateDomainHandle first_domain;
    rt::RateDomainHandle second_domain;
    ASSERT_EQ(first.register_rate_domain({"rate", 5}, first_domain), rt::Status::ok);
    ASSERT_EQ(second.register_rate_domain({"rate", 5}, second_domain), rt::Status::ok);
    ASSERT_EQ(first.bind_phase_to_rate_domain(first_phase, first_domain), rt::Status::ok);
    ASSERT_EQ(second.bind_phase_to_rate_domain(second_phase, second_domain), rt::Status::ok);
    ASSERT_EQ(first.finalize(), rt::Status::ok);
    ASSERT_EQ(second.finalize(), rt::Status::ok);
    const auto first_metadata = metadata(first);
    const auto second_metadata = metadata(second);
    EXPECT_EQ(first_metadata.graph_id, second_metadata.graph_id);
    EXPECT_EQ(first_metadata.replay_id, second_metadata.replay_id);
    EXPECT_EQ(first_metadata.schema_version, rt::checkpoint_schema_version);
}

TEST(CrossRateData, TwoRuntimeHandlesSamplesAndStoresRemainIsolated) {
    auto first = make_fixture();
    auto second = make_fixture();
    EXPECT_NE(first.channel.value, second.channel.value);
    EXPECT_EQ(
        first.runtime.replace_cross_rate_channel(second.channel, {}),
        rt::Status::invalid_handle);
    ASSERT_EQ(first.runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(second.runtime.finalize(), rt::Status::ok);
    std::array<std::byte, 4> first_bytes{};
    std::array<std::byte, 4> second_bytes{};
    ASSERT_EQ(first.runtime.copy_cross_rate_initial_sample(0, first_bytes), rt::Status::ok);
    ASSERT_EQ(second.runtime.copy_cross_rate_initial_sample(0, second_bytes), rt::Status::ok);
    EXPECT_EQ(first_bytes, second_bytes);
    rt::CompiledCrossRateChannel first_channel;
    rt::CompiledCrossRateChannel second_channel;
    ASSERT_TRUE(first.runtime.compiled_cross_rate_channel_at(0, first_channel));
    ASSERT_TRUE(second.runtime.compiled_cross_rate_channel_at(0, second_channel));
    EXPECT_NE(first_channel.channel, second_channel.channel);
}
