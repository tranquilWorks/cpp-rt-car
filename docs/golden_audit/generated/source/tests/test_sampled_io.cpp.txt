#include <gtest/gtest.h>

#include <rt/loopback_backend.hpp>

#include "rt/src/sampled_io.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <vector>

namespace {

std::vector<std::byte> sampled_frame(
    std::uint64_t channel_identity,
    rt::SampledIoFrameStatus status,
    std::uint64_t timestamp_domain,
    std::uint64_t calibration,
    std::uint64_t trigger,
    std::uint64_t sequence = 1,
    std::uint64_t release_generation = 0) {
    std::vector<std::byte> bytes(sizeof(rt::SampledIoFrameHeader) + 8);
    for (std::size_t index = sizeof(rt::SampledIoFrameHeader);
         index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::byte>(index);
    }
    rt::SampledIoFrameHeader header{};
    header.channel_identity = channel_identity;
    header.sequence = sequence;
    header.release_generation = release_generation;
    header.sample_count = 4;
    header.encoding = static_cast<std::uint32_t>(
        rt::SampledIoEncoding::signed_int16_le);
    header.timestamp_domain_identity = timestamp_domain;
    header.sample_interval_ns = 1000;
    header.trigger_identity = trigger;
    header.trigger_sequence = sequence;
    header.calibration_identity = calibration;
    header.status = static_cast<std::uint32_t>(status);
    header.payload_checksum = rt::sampled_io_payload_checksum(
        std::span<const std::byte>(bytes).subspan(sizeof(header)));
    std::memcpy(bytes.data(), &header, sizeof(header));
    return bytes;
}

struct CompilerFixture {
    static constexpr std::uint32_t owner = 42;
    rt::PhaseHandle device_phase{owner, 0};
    rt::PhaseHandle cpu_phase{owner, 1};
    rt::RateDomainHandle domain{owner, 0};
    rt::DeviceBackendHandle backend{owner, 0};
    rt::DeviceBufferHandle buffer{owner, 0};
    rt::CrossRateChannelHandle channel{owner, 0};
    std::vector<std::byte> initial = sampled_frame(
        101, rt::SampledIoFrameStatus::initial, 7, 303, 404);
    std::vector<std::byte> device_storage =
        std::vector<std::byte>(initial.size() * 2);
    rt::detail::SampledIoChannelSpec sampled{};
    rt::detail::CrossRateChannelSpec cross_spec{};
    rt::detail::CompiledCrossRatePlan cross_plan{};
    rt::detail::CompiledDeviceRatePlan device_plan{};
    rt::detail::CompiledRatePlan rate_plan{};

    CompilerFixture() {
        sampled.channel = channel;
        sampled.direction = rt::SampledIoDirection::input;
        sampled.channel_identity = 101;
        sampled.encoding = rt::SampledIoEncoding::signed_int16_le;
        sampled.element_count = 1;
        sampled.samples_per_frame = 4;
        sampled.units_identity = 202;
        sampled.calibration_identity = 303;
        sampled.sample_period_ns = 1000;
        sampled.timestamp_domain_identity = 7;
        sampled.clock_domain_identity = 8;
        sampled.trigger_identity = 404;
        sampled.ring_capacity = rt::cross_rate_snapshot_slot_count;
        sampled.initial_sequence = 1;
        sampled.maximum_age_ns = 2000;
        sampled.initial_frame = initial;

        cross_spec.name = "sensor";
        cross_spec.producer = device_phase;
        cross_spec.consumer = cpu_phase;
        cross_spec.payload_size = initial.size();
        cross_spec.initial_sample = initial;
        cross_spec.maximum_age_ns = 2000;
        cross_spec.producer_device = {0, initial.size()};

        rt::CompiledCrossRateChannel compiled{};
        compiled.channel = channel;
        compiled.producer = device_phase;
        compiled.consumer = cpu_phase;
        compiled.payload_size = initial.size();
        compiled.snapshot_slot_count = rt::cross_rate_snapshot_slot_count;
        compiled.maximum_age_ns = 2000;
        compiled.producer_device = cross_spec.producer_device;
        cross_plan.channels.push_back(compiled);
        cross_plan.device_endpoint_index_by_channel.push_back(0);
        rt::detail::CompiledCrossRateDeviceEndpoint endpoint{};
        endpoint.channel_index = 0;
        endpoint.producer = true;
        endpoint.phase = device_phase;
        endpoint.backend = backend;
        endpoint.buffer = buffer;
        endpoint.envelope_bytes = initial.size();
        endpoint.slot_stride_bytes = initial.size();
        endpoint.slot_count = 2;
        endpoint.timestamp_domain_identity = 7;
        endpoint.host_storage = device_storage;
        cross_plan.device_endpoints.push_back(endpoint);

        rt::CompiledDeviceRatePhase phase{};
        phase.phase = device_phase;
        phase.domain = domain;
        phase.backend = backend;
        phase.maximum_in_flight = 2;
        phase.payload_reference_count = 1;
        phase.signal_count = 1;
        phase.completion_timestamp_domain_identity = 7;
        device_plan.phases.push_back(phase);
    }

    rt::Status compile(
        rt::detail::CompiledSampledIoPlan& output,
        rt::detail::SampledIoCompileDiagnostic& diagnostic) {
        const std::array specs{cross_spec};
        const std::array sampled_specs{sampled};
        return rt::detail::compile_sampled_io(
            owner, sampled_specs, rate_plan, device_plan, specs,
            cross_plan, output, diagnostic);
    }

    void configure_output() {
        initial = sampled_frame(
            101, rt::SampledIoFrameStatus::initial,
            rt::cross_rate_runtime_logical_timestamp_domain_identity,
            303, 404);
        const auto safe = sampled_frame(
            101, rt::SampledIoFrameStatus::safe,
            rt::cross_rate_runtime_logical_timestamp_domain_identity,
            303, 404);
        device_storage.assign(initial.size() * 2, std::byte{});
        sampled.direction = rt::SampledIoDirection::output;
        sampled.timestamp_domain_identity =
            rt::cross_rate_runtime_logical_timestamp_domain_identity;
        sampled.underrun_policy =
            rt::SampledIoUnderrunPolicy::substitute_safe;
        sampled.safe_transition_timeout_ns = 1000;
        sampled.initial_frame = initial;
        sampled.startup_safe_frame = safe;
        sampled.failure_safe_frame = safe;
        sampled.shutdown_safe_frame = safe;

        cross_spec.producer = cpu_phase;
        cross_spec.consumer = device_phase;
        cross_spec.initial_sample = initial;
        cross_spec.producer_device = {};
        cross_spec.consumer_device = {0, initial.size()};
        auto& compiled = cross_plan.channels[0];
        compiled.producer = cpu_phase;
        compiled.consumer = device_phase;
        compiled.producer_device = {};
        compiled.consumer_device = cross_spec.consumer_device;
        auto& endpoint = cross_plan.device_endpoints[0];
        endpoint.producer = false;
        endpoint.timestamp_domain_identity =
            rt::cross_rate_runtime_logical_timestamp_domain_identity;
        endpoint.host_storage = device_storage;
        rt::ReferenceRelease release{};
        release.phase = device_phase;
        rate_plan.releases.push_back(release);
    }
};

TEST(SampledIo, CompilesExactInputDescriptorAndDirectMap) {
    CompilerFixture fixture;
    rt::detail::CompiledSampledIoPlan plan;
    rt::detail::SampledIoCompileDiagnostic diagnostic;
    ASSERT_EQ(fixture.compile(plan, diagnostic), rt::Status::ok);
    ASSERT_EQ(plan.channels.size(), 1u);
    EXPECT_EQ(plan.channels[0].public_record.channel_identity, 101u);
    EXPECT_EQ(plan.channels[0].public_record.frame_bytes,
              fixture.initial.size());
    ASSERT_EQ(plan.channel_index_by_cross_rate.size(), 1u);
    EXPECT_EQ(plan.channel_index_by_cross_rate[0], 0u);
    EXPECT_EQ(plan.frame_storage, fixture.initial);
}

TEST(SampledIo, FailureIsTransactionalAndCorrectionIsRetryable) {
    CompilerFixture fixture;
    rt::detail::CompiledSampledIoPlan plan;
    rt::detail::SampledIoCompileDiagnostic diagnostic;
    fixture.sampled.initial_frame.back() ^= std::byte{1};
    EXPECT_EQ(fixture.compile(plan, diagnostic), rt::Status::invalid_argument);
    EXPECT_TRUE(plan.channels.empty());
    fixture.sampled.initial_frame = fixture.initial;
    EXPECT_EQ(fixture.compile(plan, diagnostic), rt::Status::ok);
    EXPECT_EQ(plan.channels.size(), 1u);
}

TEST(SampledIo, RejectsMalformedDescriptorsAndEndpointDisagreement) {
    const auto expect_rejected = [](auto mutate, rt::Status expected) {
        CompilerFixture fixture;
        mutate(fixture);
        rt::detail::CompiledSampledIoPlan plan;
        rt::detail::SampledIoCompileDiagnostic diagnostic;
        EXPECT_EQ(fixture.compile(plan, diagnostic), expected);
        EXPECT_TRUE(plan.channels.empty());
    };
    expect_rejected(
        [](CompilerFixture& fixture) {
            fixture.sampled.direction =
                static_cast<rt::SampledIoDirection>(255);
        },
        rt::Status::invalid_argument);
    expect_rejected(
        [](CompilerFixture& fixture) {
            fixture.sampled.encoding =
                static_cast<rt::SampledIoEncoding>(255);
        },
        rt::Status::invalid_argument);
    expect_rejected(
        [](CompilerFixture& fixture) {
            fixture.sampled.scale_denominator = 0;
        },
        rt::Status::invalid_argument);
    expect_rejected(
        [](CompilerFixture& fixture) {
            fixture.sampled.ring_capacity = 0;
        },
        rt::Status::invalid_argument);
    expect_rejected(
        [](CompilerFixture& fixture) {
            fixture.sampled.element_count = rt::cross_rate_payload_capacity;
            fixture.sampled.samples_per_frame =
                rt::cross_rate_payload_capacity;
        },
        rt::Status::capacity_exceeded);
    expect_rejected(
        [](CompilerFixture& fixture) {
            ++fixture.cross_plan.channels[0].payload_size;
        },
        rt::Status::invalid_argument);
    expect_rejected(
        [](CompilerFixture& fixture) {
            ++fixture.cross_plan.device_endpoints[0]
                  .timestamp_domain_identity;
        },
        rt::Status::invalid_argument);
    expect_rejected(
        [](CompilerFixture& fixture) {
            fixture.cross_plan.device_endpoints[0].host_storage = {};
        },
        rt::Status::invalid_argument);
    expect_rejected(
        [](CompilerFixture& fixture) {
            fixture.device_plan.phases.clear();
        },
        rt::Status::invalid_argument);
    expect_rejected(
        [](CompilerFixture& fixture) {
            fixture.sampled.startup_safe_frame = fixture.initial;
        },
        rt::Status::invalid_argument);
}

TEST(SampledIo, OutputRequiresExactSafeFramesAndSignalOnlyPhase) {
    CompilerFixture accepted;
    accepted.configure_output();
    rt::detail::CompiledSampledIoPlan plan;
    rt::detail::SampledIoCompileDiagnostic diagnostic;
    ASSERT_EQ(accepted.compile(plan, diagnostic), rt::Status::ok);
    ASSERT_EQ(plan.channels.size(), 1u);
    EXPECT_EQ(plan.safe_channel_indices.size(), 1u);
    EXPECT_EQ(plan.safe_phases.size(), 1u);
    EXPECT_EQ(
        plan.frame_storage.size(),
        accepted.initial.size() * 4);

    const auto expect_invalid = [](auto mutate) {
        CompilerFixture fixture;
        fixture.configure_output();
        mutate(fixture);
        rt::detail::CompiledSampledIoPlan rejected;
        rt::detail::SampledIoCompileDiagnostic rejected_diagnostic;
        EXPECT_EQ(
            fixture.compile(rejected, rejected_diagnostic),
            rt::Status::invalid_argument);
        EXPECT_TRUE(rejected.channels.empty());
    };
    expect_invalid([](CompilerFixture& fixture) {
        fixture.sampled.safe_transition_timeout_ns = 0;
    });
    expect_invalid([](CompilerFixture& fixture) {
        fixture.sampled.failure_safe_frame = {};
    });
    expect_invalid([](CompilerFixture& fixture) {
        fixture.device_plan.phases[0].wait_count = 1;
    });
    expect_invalid([](CompilerFixture& fixture) {
        fixture.device_plan.phases[0].signal_count = 0;
    });
    expect_invalid([](CompilerFixture& fixture) {
        fixture.rate_plan.releases.clear();
    });
}

TEST(SampledIo, FrameValidationAcceptsUnalignedStorageWithoutAliasing) {
    CompilerFixture fixture;
    rt::detail::CompiledSampledIoPlan plan;
    rt::detail::SampledIoCompileDiagnostic diagnostic;
    ASSERT_EQ(fixture.compile(plan, diagnostic), rt::Status::ok);
    std::vector<std::byte> unaligned(fixture.initial.size() + 1);
    std::copy(fixture.initial.begin(), fixture.initial.end(),
              unaligned.begin() + 1);
    EXPECT_TRUE(rt::detail::sampled_io_frame_valid(
        std::span<const std::byte>(unaligned).subspan(1),
        plan.channels[0].public_record,
        rt::SampledIoFrameStatus::initial, 1, 0, false));
}

TEST(SampledIo, FrameValidationRejectsEveryCorrelatedFieldAndChecksumDrift) {
    CompilerFixture fixture;
    rt::detail::CompiledSampledIoPlan plan;
    rt::detail::SampledIoCompileDiagnostic diagnostic;
    ASSERT_EQ(fixture.compile(plan, diagnostic), rt::Status::ok);
    const auto& channel = plan.channels[0].public_record;
    const auto valid = sampled_frame(
        101, rt::SampledIoFrameStatus::produced, 7, 303, 404, 5, 9);
    ASSERT_TRUE(rt::detail::sampled_io_frame_valid(
        valid, channel, rt::SampledIoFrameStatus::produced, 5, 9, true));

    const auto expect_invalid = [&](auto mutate) {
        auto candidate = valid;
        rt::SampledIoFrameHeader header{};
        std::memcpy(&header, candidate.data(), sizeof(header));
        mutate(header, candidate);
        std::memcpy(candidate.data(), &header, sizeof(header));
        EXPECT_FALSE(rt::detail::sampled_io_frame_valid(
            candidate, channel, rt::SampledIoFrameStatus::produced,
            5, 9, true));
    };
    expect_invalid([](auto& header, auto&) { --header.struct_size; });
    expect_invalid([](auto& header, auto&) { ++header.version; });
    expect_invalid([](auto& header, auto&) { ++header.channel_identity; });
    expect_invalid([](auto& header, auto&) { ++header.sequence; });
    expect_invalid([](auto& header, auto&) {
        ++header.release_generation;
    });
    expect_invalid([](auto& header, auto&) { ++header.sample_count; });
    expect_invalid([](auto& header, auto&) { ++header.encoding; });
    expect_invalid([](auto& header, auto&) {
        ++header.timestamp_domain_identity;
    });
    expect_invalid([](auto& header, auto&) {
        ++header.sample_interval_ns;
    });
    expect_invalid([](auto& header, auto&) { ++header.trigger_identity; });
    expect_invalid([](auto& header, auto&) { ++header.trigger_sequence; });
    expect_invalid([](auto& header, auto&) {
        ++header.calibration_identity;
    });
    expect_invalid([](auto& header, auto&) { ++header.status; });
    expect_invalid([](auto& header, auto&) { ++header.reserved0; });
    expect_invalid([](auto&, auto& bytes) {
        bytes.back() ^= std::byte{1};
    });
}

TEST(SampledIo, RuntimeLoopbackAcknowledgesSafeAndPublishesTerminalInput) {
    constexpr std::size_t frame_bytes = sizeof(rt::SampledIoFrameHeader) + 8;
    struct ManualClock final : rt::RuntimeClock {
        std::uint64_t now_ns() noexcept override { return 1'000; }
        rt::Status sleep_until_ns(std::uint64_t) noexcept override {
            return rt::Status::ok;
        }
    } clock;
    struct State {
        rt::CrossRateChannelHandle output{};
        rt::CrossRateChannelHandle input{};
        rt::DeviceCommandBatch declaration{};
        std::array<std::byte, frame_bytes> produced{};
        std::array<std::byte, frame_bytes> consumed{};
        rt::CrossRateReadResult read{};
        std::uint64_t signal_value = 1;
        std::size_t consume_count = 0;
        bool skip_output = false;
        bool publish_twice = false;
        rt::Status second_publish_status = rt::Status::ok;
    } state;
    rt::SampledIoLoopbackBackend backend({4, 2, 4096, 1, 7});
    ASSERT_EQ(backend.add_route({17, 101, 202, 7, 303, 404}), rt::Status::ok);

    rt::Runtime runtime(clock);
    rt::RuntimeConfig config;
    config.callback_capacity = 3;
    config.worker_count = 2;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    config.trace_capacity = 32;
    config.device_backend_capacity = 1;
    config.device_buffer_capacity = 1;
    config.device_outstanding_capacity = 4;
    config.device_completion_batch = 4;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(runtime.set_rate_execution_policy({6}), rt::Status::ok);
    ASSERT_EQ(
        runtime.set_mixed_rate_closure_policy({
            42,
            128,
            0,
            0,
            0,
            rt::MixedRateOverflowPolicy::overwrite_committed,
            false,
            true,
            {},
        }),
        rt::Status::ok);
    rt::DeviceBackendHandle backend_handle;
    ASSERT_EQ(runtime.register_device_backend(
        backend.hal_v2_registration(), backend_handle), rt::Status::ok);
    rt::DeviceMemoryDomainHandle memory_domain;
    rt::HalV2MemoryDomain memory_description;
    ASSERT_TRUE(runtime.device_memory_domain_at(
        backend_handle, 0, memory_domain, memory_description));
    alignas(64) std::array<std::byte, frame_bytes * 4> storage{};
    rt::DeviceBufferHandle buffer;
    ASSERT_EQ(runtime.register_device_buffer(
        {"sampled.loopback.buffer", backend_handle, memory_domain, storage, {},
         storage.size(), rt::HalV2MemoryOwnership::borrowed_host,
         RTFW_DEVICE_BUFFER_HOST_READ | RTFW_DEVICE_BUFFER_HOST_WRITE |
             RTFW_DEVICE_BUFFER_DEVICE_READ |
             RTFW_DEVICE_BUFFER_DEVICE_WRITE,
         rt::HalV2MemoryCoherency::host_coherent,
         rt::hal_v2_memory_sync_none},
        buffer), rt::Status::ok);
    rt::DeviceTimelineHandle timeline;
    ASSERT_EQ(runtime.register_device_timeline(
        {"sampled.loopback.timeline", backend_handle, 0}, timeline),
        rt::Status::ok);

    rt::PhaseHandle output_phase;
    ASSERT_EQ(runtime.register_callback(
        {"sampled.output",
         [](void* opaque, const rt::CallbackContext& context) {
             auto& value = *static_cast<State*>(opaque);
             if (!context.rate_release) {
                 return rt::CallbackResult::error;
             }
             if (value.skip_output) {
                 return rt::CallbackResult::ok;
             }
             rt::SampledIoFrameHeader header{};
             header.channel_identity = 101;
             header.sequence =
                 context.rate_release->domain_release_sequence + 2;
             header.release_generation =
                 context.rate_release->domain_release_sequence + 1;
             header.sample_count = 4;
             header.encoding = static_cast<std::uint32_t>(
                 rt::SampledIoEncoding::signed_int16_le);
             header.timestamp_domain_identity = 1;
             header.first_sample_timestamp =
                 context.rate_release->logical_release_ns;
             header.sample_interval_ns = 1000;
             header.trigger_identity = 404;
             header.trigger_sequence = header.sequence;
             header.calibration_identity = 303;
             header.status = static_cast<std::uint32_t>(
                 rt::SampledIoFrameStatus::produced);
             header.payload_checksum = rt::sampled_io_payload_checksum(
                 std::span<const std::byte>(value.produced).subspan(
                     sizeof(header)));
             std::memcpy(value.produced.data(), &header, sizeof(header));
             if (context.rate_release->publish(
                     value.output, value.produced) != rt::Status::ok) {
                 return rt::CallbackResult::error;
             }
             if (value.publish_twice) {
                 value.second_publish_status = context.rate_release->publish(
                     value.output, value.produced);
                 return rt::CallbackResult::error;
             }
             return rt::CallbackResult::ok;
         }, &state}, output_phase), rt::Status::ok);

    state.declaration.command_count = 1;
    state.declaration.signal_count = 1;
    state.declaration.commands[0].kind = static_cast<std::uint32_t>(
        rt::HalV2CommandKind::dispatch);
    state.declaration.commands[0].opcode = 17;
    state.declaration.commands[0].buffer_count = 2;
    state.declaration.commands[0].buffers[0] = {
        buffer.value, RTFW_DEVICE_ACCESS_READ, 0, 0, frame_bytes * 2};
    state.declaration.commands[0].buffers[1] = {
        buffer.value, RTFW_DEVICE_ACCESS_WRITE, 0,
        frame_bytes * 2, frame_bytes * 2};
    state.declaration.signals[0].timeline_handle = timeline.value;
    rt::PhaseHandle device_phase;
    ASSERT_EQ(runtime.register_device_batch_phase(
        {"sampled.loopback.device", backend_handle,
         [](void* opaque, const rt::DeviceCallbackContext&,
            rt::DeviceCommandBatch& batch) {
             auto& value = *static_cast<State*>(opaque);
             batch = value.declaration;
             batch.timeout_ns = 100'000'000;
             batch.signals[0].value = ++value.signal_value;
             return rt::CallbackResult::ok;
         }, &state, state.declaration}, device_phase), rt::Status::ok);

    rt::PhaseHandle input_phase;
    ASSERT_EQ(runtime.register_callback(
        {"sampled.input",
         [](void* opaque, const rt::CallbackContext& context) {
             auto& value = *static_cast<State*>(opaque);
             if (!context.rate_release ||
                 context.rate_release->copy(
                     value.input, value.consumed, value.read) !=
                     rt::CrossRateReadStatus::ok) {
                 return rt::CallbackResult::error;
             }
             ++value.consume_count;
             return rt::CallbackResult::ok;
         }, &state}, input_phase), rt::Status::ok);
    rt::RateDomainHandle output_rate;
    rt::RateDomainHandle device_rate;
    rt::RateDomainHandle input_rate;
    ASSERT_EQ(runtime.register_rate_domain(
        {"sampled.output.rate", 2'000'000'000, 1, 900'000'000, 10},
        output_rate), rt::Status::ok);
    ASSERT_EQ(runtime.register_rate_domain(
        {"sampled.device.rate", 4'000'000'000, 1, 900'000'000, 0},
        device_rate), rt::Status::ok);
    ASSERT_EQ(runtime.register_rate_domain(
        {"sampled.input.rate", 1'000'000'000, 1, 900'000'000, 10},
        input_rate), rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(output_phase, output_rate),
              rt::Status::ok);
    const std::array roles{
        rt::DeviceRatePayloadRole::input,
        rt::DeviceRatePayloadRole::output};
    ASSERT_EQ(runtime.bind_device_phase_to_rate_domain(
        {device_phase, device_rate, 800'000'000, 2, roles}), rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(input_phase, input_rate),
              rt::Status::ok);

    auto output_initial = sampled_frame(
        101, rt::SampledIoFrameStatus::initial, 1, 303, 404);
    auto output_safe = sampled_frame(
        101, rt::SampledIoFrameStatus::safe, 1, 303, 404);
    auto input_initial = sampled_frame(
        202, rt::SampledIoFrameStatus::initial, 7, 303, 404);
    ASSERT_EQ(runtime.register_cross_rate_channel(
        {"sampled.output.channel", output_phase, device_phase, frame_bytes,
         output_initial, rt::CrossRateMode::sample_and_hold,
         4'000'000'000, {}, {0, frame_bytes}}, state.output), rt::Status::ok);
    ASSERT_EQ(runtime.register_cross_rate_channel(
        {"sampled.input.channel", device_phase, input_phase, frame_bytes,
         input_initial, rt::CrossRateMode::sample_and_hold,
         1'000'000'000, {1, frame_bytes}, {}}, state.input), rt::Status::ok);
    rt::SampledIoChannelRegistration output_registration{};
    output_registration.channel = state.output;
    output_registration.direction = rt::SampledIoDirection::output;
    output_registration.channel_identity = 101;
    output_registration.element_count = 1;
    output_registration.samples_per_frame = 4;
    output_registration.units_identity = 202;
    output_registration.calibration_identity = 303;
    output_registration.sample_period_ns = 1000;
    output_registration.timestamp_domain_identity = 1;
    output_registration.clock_domain_identity = 1;
    output_registration.trigger_identity = 404;
    output_registration.ring_capacity = rt::cross_rate_snapshot_slot_count;
    output_registration.initial_sequence = 1;
    output_registration.maximum_age_ns = 4'000'000'000;
    output_registration.underrun_policy =
        rt::SampledIoUnderrunPolicy::substitute_safe;
    output_registration.safe_transition_timeout_ns = 100'000'000;
    output_registration.initial_frame = output_initial;
    output_registration.startup_safe_frame = output_safe;
    output_registration.failure_safe_frame = output_safe;
    output_registration.shutdown_safe_frame = output_safe;
    ASSERT_EQ(runtime.register_sampled_io_channel(output_registration),
              rt::Status::ok);
    auto input_registration = output_registration;
    input_registration.channel = state.input;
    input_registration.direction = rt::SampledIoDirection::input;
    input_registration.channel_identity = 202;
    input_registration.timestamp_domain_identity = 7;
    input_registration.maximum_age_ns = 1'000'000'000;
    input_registration.stale_policy =
        rt::SampledIoStalePolicy::substitute_initial;
    input_registration.safe_transition_timeout_ns = 0;
    input_registration.underrun_policy =
        rt::SampledIoUnderrunPolicy::fail_release;
    input_registration.initial_frame = input_initial;
    input_registration.startup_safe_frame = {};
    input_registration.failure_safe_frame = {};
    input_registration.shutdown_safe_frame = {};
    ASSERT_EQ(runtime.register_sampled_io_channel(input_registration),
              rt::Status::ok);

    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();
    rt::MemoryPlan plan;
    ASSERT_TRUE(runtime.memory_plan(plan));
    EXPECT_EQ(plan.sampled_io_channel_count, 2u);
    EXPECT_EQ(plan.sampled_io_frame_bytes, frame_bytes * 2);
    EXPECT_EQ(plan.sampled_io_safe_frame_bytes, frame_bytes * 3);
    ASSERT_EQ(runtime.start(), rt::Status::ok) << runtime.last_error();
    rt::SampledIoChannelStatus output_status;
    ASSERT_TRUE(runtime.sampled_io_channel_status(state.output, output_status));
    EXPECT_EQ(output_status.safety_state,
              rt::SampledIoSafetyState::startup_acknowledged);
    rt::StepResult result;
    ASSERT_EQ(runtime.step(
        {0, std::chrono::nanoseconds(1), std::nullopt, 1'000}, &result),
        rt::Status::ok) << runtime.last_error();
    EXPECT_EQ(state.consume_count, 1u);
    EXPECT_EQ(state.read.sampled_sequence, 2u);
    EXPECT_EQ(state.read.sampled_trigger_identity, 404u);
    EXPECT_EQ(state.read.sampled_calibration_identity, 303u);

    ASSERT_EQ(runtime.step(
        {1, std::chrono::seconds(1), std::nullopt, 1'001}, &result),
        rt::Status::ok) << runtime.last_error();
    EXPECT_EQ(state.consume_count, 2u);
    EXPECT_TRUE(state.read.held);
    EXPECT_EQ(state.read.age_ns, 1'000'000'000u);
    EXPECT_EQ(state.read.freshness, rt::CrossRateFreshness::fresh);
    EXPECT_FALSE(state.read.sampled_substituted);

    state.skip_output = true;
    ASSERT_EQ(runtime.step(
        {2, std::chrono::seconds(1), std::nullopt, 1'000'001'001}, &result),
        rt::Status::ok) << runtime.last_error();
    EXPECT_EQ(state.consume_count, 3u);
    EXPECT_TRUE(state.read.sampled_substituted);
    EXPECT_EQ(state.read.sampled_sequence, 1u);
    rt::SampledIoChannelStatus input_status;
    ASSERT_TRUE(runtime.sampled_io_channel_status(state.input, input_status));
    EXPECT_EQ(input_status.stale_frames, 1u);
    EXPECT_EQ(input_status.substituted_frames, 1u);
    ASSERT_TRUE(runtime.sampled_io_channel_status(state.output, output_status));
    EXPECT_EQ(output_status.underruns, 1u);
    EXPECT_EQ(output_status.substituted_frames, 1u);

    state.skip_output = false;
    ASSERT_EQ(runtime.step(
        {3, std::chrono::seconds(2), std::nullopt, 2'000'001'001}, &result),
        rt::Status::ok) << runtime.last_error();
    EXPECT_EQ(state.consume_count, 5u);
    EXPECT_FALSE(state.read.sampled_substituted);
    EXPECT_EQ(state.read.sampled_sequence, 3u);
    EXPECT_EQ(
        state.read.producer_timestamp_domain_identity,
        7u);

    state.publish_twice = true;
    EXPECT_EQ(runtime.step(
        {4, std::chrono::seconds(2), std::nullopt, 4'000'001'001}, &result),
        rt::Status::callback_failed);
    EXPECT_EQ(state.second_publish_status, rt::Status::invalid_state);
    ASSERT_TRUE(runtime.sampled_io_channel_status(state.output, output_status));
    EXPECT_EQ(output_status.overruns, 1u);
    rt::MixedRateActionCursor action_cursor;
    std::array<rt::MixedRateActionRecord, 128> actions{};
    rt::MixedRateActionReadResult action_read;
    ASSERT_EQ(
        runtime.read_mixed_rate_actions(
            action_cursor, actions, action_read),
        rt::Status::ok);
    EXPECT_EQ(action_read.lost_records, 0u);
    EXPECT_TRUE(std::any_of(
        actions.begin(),
        actions.begin() +
            static_cast<std::ptrdiff_t>(action_read.records_read),
        [](const rt::MixedRateActionRecord& action) {
            return action.action == rt::MixedRateActionId::sampled_publish &&
                action.reason == rt::MixedRateActionReason::overrun &&
                action.stage == rt::MixedRateActionStage::terminal &&
                action.terminal_status == static_cast<std::int32_t>(
                    rt::Status::capacity_exceeded);
        }));
    ASSERT_EQ(backend.inject_next(
        rt::SampledIoLoopbackFault::completion_error), rt::Status::ok);
    EXPECT_EQ(runtime.stop(), rt::Status::device_error);
    ASSERT_TRUE(runtime.sampled_io_channel_status(state.output, output_status));
    EXPECT_EQ(output_status.safety_state,
              rt::SampledIoSafetyState::unknown);
    ASSERT_EQ(runtime.stop(), rt::Status::ok) << runtime.last_error();
    ASSERT_TRUE(runtime.sampled_io_channel_status(state.output, output_status));
    EXPECT_EQ(output_status.safety_state,
              rt::SampledIoSafetyState::shutdown_acknowledged);
    ASSERT_EQ(
        runtime.read_mixed_rate_actions(
            action_cursor, actions, action_read),
        rt::Status::ok);
    bool saw_failed_stop = false;
    bool saw_successful_stop = false;
    for (std::size_t index = 0; index < action_read.records_read; ++index) {
        const auto& action = actions[index];
        if (action.action != rt::MixedRateActionId::runtime_stop) {
            continue;
        }
        saw_failed_stop = saw_failed_stop ||
            action.reason == rt::MixedRateActionReason::cleanup_pending;
        saw_successful_stop = saw_successful_stop ||
            (action.reason == rt::MixedRateActionReason::normal &&
             action.terminal_status == static_cast<std::int32_t>(
                 rt::Status::ok));
    }
    EXPECT_TRUE(saw_failed_stop);
    EXPECT_TRUE(saw_successful_stop);
    EXPECT_GE(backend.stats().frames_copied, 4u);
}

} // namespace
