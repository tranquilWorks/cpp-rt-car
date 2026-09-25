#pragma once

#include <array>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include <rt/loopback_backend.hpp>

namespace rtfw_test {

struct MixedRateConformanceResult {
    rt::Status status = rt::Status::internal_error;
    // CPU plant, device sensor, CPU controller, device actuator, CPU observer.
    std::array<std::uint64_t, 5> callback_counts{};
    std::size_t action_count = 0;
    std::size_t device_terminal_actions = 0;
    std::size_t sampled_publish_actions = 0;
    std::size_t sampled_select_actions = 0;
    std::size_t safe_transition_actions = 0;
    std::uint64_t first_logical_release_ns = 0;
    std::uint64_t last_logical_release_ns = 0;
    std::uint64_t loopback_logical_actions = 0;
    std::uint32_t last_terminal_phase = 0;
    std::int32_t last_terminal_status = 0;
    rt::MixedRateActionReason last_terminal_reason =
        rt::MixedRateActionReason::normal;
    rt::MixedRateActionStage last_terminal_stage =
        rt::MixedRateActionStage::decision;
    std::int32_t last_backend_status = 0;
    std::uint64_t replay_mismatch_sequence = 0;
    std::uint64_t replay_expected_batch = 0;
    std::uint64_t replay_actual_batch = 0;
    std::uint64_t replay_expected_timestamp = 0;
    std::uint64_t replay_actual_timestamp = 0;
    std::uint64_t replay_expected_payload = 0;
    std::uint64_t replay_actual_payload = 0;
    std::uint8_t replay_expected_action = 0;
    std::uint8_t replay_actual_action = 0;
    std::size_t replay_first_different_byte =
        sizeof(rt::MixedRateActionRecord);
    std::uint32_t failure_stage = 0;
    std::array<char, 256> diagnostic{};
    bool startup_safe_acknowledged = false;
    bool failure_safe_acknowledged = false;
    bool shutdown_safe_acknowledged = false;
    bool active_replay_exact = false;
    std::size_t replay_actions_compared = 0;
    bool memory_accounting_exact = false;
    rt::Status startup_cleanup_status = rt::Status::invalid_state;
};

namespace detail {

inline constexpr std::size_t conformance_frame_bytes =
    sizeof(rt::SampledIoFrameHeader) + 8;
inline constexpr std::uint64_t plant_period_ns = 100'000'000;
inline constexpr std::uint64_t sensor_period_ns = 150'000'000;
inline constexpr std::uint64_t controller_period_ns = 225'000'000;
inline constexpr std::uint64_t supercycle_ns = 900'000'000;
inline constexpr std::size_t checkpoint_capacity = 64 * 1024;
inline constexpr std::size_t replay_artifact_capacity = 128 * 1024;

struct ManualClock final : rt::RuntimeClock {
    std::uint64_t now = 1'000;
    std::uint64_t now_ns() noexcept override { return now; }
    rt::Status sleep_until_ns(std::uint64_t release) noexcept override {
        now = release;
        return rt::Status::ok;
    }
    bool supports_absolute_sleep() const noexcept override { return true; }
};

inline std::array<std::byte, conformance_frame_bytes> sampled_frame(
    std::uint64_t channel_identity,
    rt::SampledIoFrameStatus status,
    std::uint64_t timestamp_domain_identity) {
    std::array<std::byte, conformance_frame_bytes> bytes{};
    for (std::size_t index = sizeof(rt::SampledIoFrameHeader);
         index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::byte>(index);
    }
    rt::SampledIoFrameHeader header{};
    header.channel_identity = channel_identity;
    header.sequence = 1;
    header.sample_count = 4;
    header.encoding = static_cast<std::uint32_t>(
        rt::SampledIoEncoding::signed_int16_le);
    header.timestamp_domain_identity = timestamp_domain_identity;
    header.sample_interval_ns = 1'000'000;
    header.trigger_identity = 404;
    header.trigger_sequence = 1;
    header.calibration_identity = 303;
    header.status = static_cast<std::uint32_t>(status);
    header.payload_checksum = rt::sampled_io_payload_checksum(
        std::span<const std::byte>(bytes).subspan(sizeof(header)));
    std::memcpy(bytes.data(), &header, sizeof(header));
    return bytes;
}

struct FixtureState {
    rt::CrossRateChannelHandle plant_output{};
    rt::CrossRateChannelHandle sensor_input{};
    rt::CrossRateChannelHandle controller_output{};
    rt::CrossRateChannelHandle actuator_input{};
    rt::DeviceCommandBatch sensor_declaration{};
    rt::DeviceCommandBatch actuator_declaration{};
    std::array<std::byte, conformance_frame_bytes> produced{};
    std::array<std::byte, conformance_frame_bytes> consumed{};
    std::array<std::byte, conformance_frame_bytes> controller_produced{};
    std::array<std::byte, conformance_frame_bytes> actuator_consumed{};
    rt::CrossRateReadResult read{};
    std::array<std::uint64_t, 5> callback_counts{};
    std::uint64_t signal_value = 1;
};

inline rt::CallbackResult run_plant(
    void* opaque,
    const rt::CallbackContext& context) {
    auto& state = *static_cast<FixtureState*>(opaque);
    if (!context.rate_release) {
        return rt::CallbackResult::error;
    }
    ++state.callback_counts[0];
    rt::SampledIoFrameHeader header{};
    header.channel_identity = 101;
    header.sequence = context.rate_release->domain_release_sequence + 2;
    header.release_generation =
        context.rate_release->domain_release_sequence + 1;
    header.sample_count = 4;
    header.encoding = static_cast<std::uint32_t>(
        rt::SampledIoEncoding::signed_int16_le);
    header.timestamp_domain_identity =
        rt::cross_rate_runtime_logical_timestamp_domain_identity;
    header.first_sample_timestamp = context.rate_release->logical_release_ns;
    header.sample_interval_ns = 1'000'000;
    header.trigger_identity = 404;
    header.trigger_sequence = header.sequence;
    header.calibration_identity = 303;
    header.status = static_cast<std::uint32_t>(
        rt::SampledIoFrameStatus::produced);
    header.payload_checksum = rt::sampled_io_payload_checksum(
        std::span<const std::byte>(state.produced).subspan(sizeof(header)));
    std::memcpy(state.produced.data(), &header, sizeof(header));
    return context.rate_release->publish(
               state.plant_output, state.produced) == rt::Status::ok
        ? rt::CallbackResult::ok
        : rt::CallbackResult::error;
}

inline rt::CallbackResult run_sensor(
    void* opaque,
    const rt::DeviceCallbackContext& context,
    rt::DeviceCommandBatch& batch) {
    auto& state = *static_cast<FixtureState*>(opaque);
    if (!context.rate_release) {
        return rt::CallbackResult::error;
    }
    ++state.callback_counts[1];
    batch = state.sensor_declaration;
    batch.timeout_ns = 80'000'000;
    batch.signals[0].value = ++state.signal_value;
    return rt::CallbackResult::ok;
}

inline rt::CallbackResult run_controller(
    void* opaque,
    const rt::CallbackContext& context) {
    auto& state = *static_cast<FixtureState*>(opaque);
    if (!context.rate_release ||
        context.rate_release->copy(
            state.sensor_input,
            state.consumed,
            state.read) != rt::CrossRateReadStatus::ok) {
        return rt::CallbackResult::error;
    }
    ++state.callback_counts[2];
    rt::SampledIoFrameHeader header{};
    header.channel_identity = 303;
    header.sequence = context.rate_release->domain_release_sequence + 2;
    header.release_generation =
        context.rate_release->domain_release_sequence + 1;
    header.sample_count = 4;
    header.encoding = static_cast<std::uint32_t>(
        rt::SampledIoEncoding::signed_int16_le);
    header.timestamp_domain_identity =
        rt::cross_rate_runtime_logical_timestamp_domain_identity;
    header.first_sample_timestamp = context.rate_release->logical_release_ns;
    header.sample_interval_ns = 1'000'000;
    header.trigger_identity = 404;
    header.trigger_sequence = header.sequence;
    header.calibration_identity = 303;
    header.status = static_cast<std::uint32_t>(
        rt::SampledIoFrameStatus::produced);
    header.payload_checksum = rt::sampled_io_payload_checksum(
        std::span<const std::byte>(state.controller_produced).subspan(
            sizeof(header)));
    std::memcpy(
        state.controller_produced.data(), &header, sizeof(header));
    return context.rate_release->publish(
               state.controller_output,
               state.controller_produced) == rt::Status::ok
        ? rt::CallbackResult::ok
        : rt::CallbackResult::error;
}

inline rt::CallbackResult run_actuator(
    void* opaque,
    const rt::DeviceCallbackContext& context,
    rt::DeviceCommandBatch& batch) {
    auto& state = *static_cast<FixtureState*>(opaque);
    if (!context.rate_release) {
        return rt::CallbackResult::error;
    }
    ++state.callback_counts[3];
    batch = state.actuator_declaration;
    batch.timeout_ns = 80'000'000;
    batch.signals[0].value = ++state.signal_value;
    return rt::CallbackResult::ok;
}

inline rt::CallbackResult run_observer(
    void* opaque,
    const rt::CallbackContext& context) {
    auto& state = *static_cast<FixtureState*>(opaque);
    if (!context.rate_release ||
        context.rate_release->copy(
            state.actuator_input,
            state.actuator_consumed,
            state.read) != rt::CrossRateReadStatus::ok) {
        return rt::CallbackResult::error;
    }
    ++state.callback_counts[4];
    return rt::CallbackResult::ok;
}

} // namespace detail

// Reusable installed-surface fixture. The 100/150/225 ms schedule has a
// 900 ms supercycle, and the controller/actuator pair exercises equal-time
// CPU-to-device ordering after the sampled plant->sensor->controller path.
inline MixedRateConformanceResult run_mixed_rate_conformance(
    rt::SampledIoLoopbackFault active_fault =
        rt::SampledIoLoopbackFault::none,
    std::uint64_t safe_transition_timeout_ns = 5'000'000'000,
    rt::SampledIoLoopbackFault startup_fault =
        rt::SampledIoLoopbackFault::none) {
    MixedRateConformanceResult result;
    detail::ManualClock clock;
    detail::FixtureState state;
    rt::SampledIoLoopbackBackend backend({8, 2, 4096, 1, 7});
    if (backend.add_route({17, 101, 202, 7, 303, 404}) != rt::Status::ok ||
        backend.add_route({18, 303, 404, 7, 303, 404}) != rt::Status::ok) {
        result.status = rt::Status::invalid_config;
        result.failure_stage = 10;
        return result;
    }

    rt::Runtime runtime(clock);
    rt::RuntimeConfig config;
    config.callback_capacity = 5;
    config.worker_count = 2;
    config.executor_queue_capacity = 16;
    config.task_scratch_slots = 16;
    config.trace_capacity = 64;
    config.device_backend_capacity = 1;
    config.device_buffer_capacity = 1;
    config.device_outstanding_capacity = 8;
    config.device_completion_batch = 8;
    config.snapshot_max_bytes = detail::checkpoint_capacity;
    config.memory_budget_bytes = 8 * 1024 * 1024;
    if (runtime.configure(config) != rt::Status::ok ||
        runtime.set_rate_execution_policy({64, 9, 2, 2, 64}) !=
            rt::Status::ok ||
        runtime.set_mixed_rate_closure_policy({
            21,
            128,
            128,
            detail::replay_artifact_capacity,
            128,
            rt::MixedRateOverflowPolicy::overwrite_committed,
            true,
            true,
            {},
        }) != rt::Status::ok) {
        result.status = rt::Status::invalid_config;
        result.failure_stage = 11;
        return result;
    }
    rt::DeviceBackendHandle backend_handle;
    if (runtime.register_device_backend(
            backend.hal_v2_registration(), backend_handle) != rt::Status::ok) {
        result.status = rt::Status::invalid_config;
        result.failure_stage = 12;
        return result;
    }
    rt::DeviceMemoryDomainHandle memory_domain;
    rt::HalV2MemoryDomain memory_description;
    if (!runtime.device_memory_domain_at(
            backend_handle, 0, memory_domain, memory_description)) {
        result.status = rt::Status::invalid_config;
        result.failure_stage = 13;
        return result;
    }
    alignas(64) std::array<
        std::byte,
        detail::conformance_frame_bytes * 8> storage{};
    rt::DeviceBufferHandle buffer;
    if (runtime.register_device_buffer(
            {
                "conformance.buffer",
                backend_handle,
                memory_domain,
                storage,
                {},
                storage.size(),
                rt::HalV2MemoryOwnership::borrowed_host,
                RTFW_DEVICE_BUFFER_HOST_READ |
                    RTFW_DEVICE_BUFFER_HOST_WRITE |
                    RTFW_DEVICE_BUFFER_DEVICE_READ |
                    RTFW_DEVICE_BUFFER_DEVICE_WRITE,
                rt::HalV2MemoryCoherency::host_coherent,
                rt::hal_v2_memory_sync_none,
            },
            buffer) != rt::Status::ok) {
        result.status = rt::Status::invalid_config;
        result.failure_stage = 14;
        return result;
    }
    rt::DeviceTimelineHandle timeline;
    if (runtime.register_device_timeline(
            {"conformance.timeline", backend_handle, 0}, timeline) !=
        rt::Status::ok) {
        result.status = rt::Status::invalid_config;
        result.failure_stage = 15;
        return result;
    }
    rt::DeviceTimelineHandle actuator_timeline;
    if (runtime.register_device_timeline(
            {"conformance.actuator.timeline", backend_handle, 1},
            actuator_timeline) != rt::Status::ok) {
        result.status = rt::Status::invalid_config;
        result.failure_stage = 23;
        return result;
    }

    rt::PhaseHandle plant_phase;
    rt::PhaseHandle sensor_phase;
    rt::PhaseHandle controller_phase;
    rt::PhaseHandle actuator_phase;
    rt::PhaseHandle observer_phase;
    if (runtime.register_callback(
            {"plant.cpu", &detail::run_plant, &state}, plant_phase) !=
            rt::Status::ok) {
        result.status = rt::Status::invalid_config;
        result.failure_stage = 16;
        return result;
    }
    state.sensor_declaration.command_count = 1;
    state.sensor_declaration.signal_count = 1;
    state.sensor_declaration.commands[0].kind = static_cast<std::uint32_t>(
        rt::HalV2CommandKind::dispatch);
    state.sensor_declaration.commands[0].opcode = 17;
    state.sensor_declaration.commands[0].buffer_count = 2;
    state.sensor_declaration.commands[0].buffers[0] = {
        buffer.value,
        RTFW_DEVICE_ACCESS_READ,
        0,
        0,
        detail::conformance_frame_bytes * 2,
    };
    state.sensor_declaration.commands[0].buffers[1] = {
        buffer.value,
        RTFW_DEVICE_ACCESS_WRITE,
        0,
        detail::conformance_frame_bytes * 2,
        detail::conformance_frame_bytes * 2,
    };
    state.sensor_declaration.signals[0].timeline_handle = timeline.value;
    if (runtime.register_device_batch_phase(
            {
                "sensor.device.loopback",
                backend_handle,
                &detail::run_sensor,
                &state,
                state.sensor_declaration,
            },
            sensor_phase) != rt::Status::ok) {
        result.status = rt::Status::invalid_config;
        result.failure_stage = 17;
        return result;
    }
    if (runtime.register_callback(
            {"controller.cpu", &detail::run_controller, &state},
            controller_phase) != rt::Status::ok) {
        result.status = rt::Status::invalid_config;
        result.failure_stage = 18;
        return result;
    }
    state.actuator_declaration.command_count = 1;
    state.actuator_declaration.signal_count = 1;
    state.actuator_declaration.commands[0].kind =
        static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
    state.actuator_declaration.commands[0].opcode = 18;
    state.actuator_declaration.commands[0].buffer_count = 2;
    state.actuator_declaration.commands[0].buffers[0] = {
        buffer.value,
        RTFW_DEVICE_ACCESS_READ,
        0,
        detail::conformance_frame_bytes * 4,
        detail::conformance_frame_bytes * 2,
    };
    state.actuator_declaration.commands[0].buffers[1] = {
        buffer.value,
        RTFW_DEVICE_ACCESS_WRITE,
        0,
        detail::conformance_frame_bytes * 6,
        detail::conformance_frame_bytes * 2,
    };
    state.actuator_declaration.signals[0].timeline_handle =
        actuator_timeline.value;
    if (runtime.register_device_batch_phase(
            {
                "actuator.device",
                backend_handle,
                &detail::run_actuator,
                &state,
                state.actuator_declaration,
            },
            actuator_phase) != rt::Status::ok) {
        result.status = rt::Status::invalid_config;
        result.failure_stage = 19;
        return result;
    }
    if (runtime.register_callback(
            {"actuator.observer.cpu", &detail::run_observer, &state},
            observer_phase) != rt::Status::ok) {
        result.status = rt::Status::invalid_config;
        result.failure_stage = 24;
        return result;
    }

    rt::RateDomainHandle plant_rate;
    rt::RateDomainHandle sensor_rate;
    rt::RateDomainHandle controller_rate;
    rt::RateDomainHandle actuator_rate;
    rt::RateDomainHandle observer_rate;
    const auto register_rate = [&runtime](
                                   const char* name,
                                   std::uint64_t period,
                                   rt::RateDomainHandle& handle) {
        return runtime.register_rate_domain(
            {
                name,
                period,
                1,
                period,
                1'000'000,
                rt::RateCriticality::normal,
                false,
                rt::RateLateAction::fail,
                0,
            },
            handle);
    };
    if (register_rate("plant.rate", detail::plant_period_ns, plant_rate) !=
            rt::Status::ok ||
        register_rate("sensor.rate", detail::sensor_period_ns, sensor_rate) !=
            rt::Status::ok ||
        register_rate(
            "controller.rate",
            detail::controller_period_ns,
            controller_rate) != rt::Status::ok ||
        register_rate(
            "actuator.rate",
            detail::controller_period_ns,
            actuator_rate) != rt::Status::ok ||
        register_rate("observer.rate", 450'000'000, observer_rate) !=
            rt::Status::ok ||
        runtime.bind_phase_to_rate_domain(plant_phase, plant_rate) !=
            rt::Status::ok) {
        result.status = rt::Status::invalid_config;
        result.failure_stage = 20;
        return result;
    }
    const std::array sensor_roles{
        rt::DeviceRatePayloadRole::input,
        rt::DeviceRatePayloadRole::output,
    };
    if (runtime.bind_device_phase_to_rate_domain(
            {sensor_phase, sensor_rate, 80'000'000, 2, sensor_roles}) !=
            rt::Status::ok ||
        runtime.bind_phase_to_rate_domain(controller_phase, controller_rate) !=
            rt::Status::ok ||
        runtime.bind_device_phase_to_rate_domain(
            {actuator_phase, actuator_rate, 80'000'000, 2, sensor_roles}) !=
            rt::Status::ok ||
        runtime.bind_phase_to_rate_domain(observer_phase, observer_rate) !=
            rt::Status::ok) {
        result.status = rt::Status::invalid_config;
        result.failure_stage = 21;
        return result;
    }

    const auto plant_initial = detail::sampled_frame(
        101,
        rt::SampledIoFrameStatus::initial,
        rt::cross_rate_runtime_logical_timestamp_domain_identity);
    const auto plant_safe = detail::sampled_frame(
        101,
        rt::SampledIoFrameStatus::safe,
        rt::cross_rate_runtime_logical_timestamp_domain_identity);
    const auto sensor_initial = detail::sampled_frame(
        202, rt::SampledIoFrameStatus::initial, 7);
    const auto controller_initial = detail::sampled_frame(
        303,
        rt::SampledIoFrameStatus::initial,
        rt::cross_rate_runtime_logical_timestamp_domain_identity);
    const auto controller_safe = detail::sampled_frame(
        303,
        rt::SampledIoFrameStatus::safe,
        rt::cross_rate_runtime_logical_timestamp_domain_identity);
    const auto actuator_initial = detail::sampled_frame(
        404, rt::SampledIoFrameStatus::initial, 7);
    if (runtime.register_cross_rate_channel(
            {
                "plant.sensor",
                plant_phase,
                sensor_phase,
                detail::conformance_frame_bytes,
                plant_initial,
                rt::CrossRateMode::sample_and_hold,
                detail::sensor_period_ns,
                {},
                {0, detail::conformance_frame_bytes},
            },
            state.plant_output) != rt::Status::ok ||
        runtime.register_cross_rate_channel(
            {
                "sensor.controller",
                sensor_phase,
                controller_phase,
                detail::conformance_frame_bytes,
                sensor_initial,
                rt::CrossRateMode::sample_and_hold,
                detail::controller_period_ns,
                {1, detail::conformance_frame_bytes},
                {},
            },
            state.sensor_input) != rt::Status::ok ||
        runtime.register_cross_rate_channel(
            {
                "controller.actuator",
                controller_phase,
                actuator_phase,
                detail::conformance_frame_bytes,
                controller_initial,
                rt::CrossRateMode::sample_and_hold,
                detail::controller_period_ns,
                {},
                {0, detail::conformance_frame_bytes},
            },
            state.controller_output) != rt::Status::ok ||
        runtime.register_cross_rate_channel(
            {
                "actuator.observer",
                actuator_phase,
                observer_phase,
                detail::conformance_frame_bytes,
                actuator_initial,
                rt::CrossRateMode::sample_and_hold,
                450'000'000,
                {1, detail::conformance_frame_bytes},
                {},
            },
            state.actuator_input) != rt::Status::ok) {
        result.status = rt::Status::invalid_config;
        result.failure_stage = 22;
        return result;
    }
    rt::SampledIoChannelRegistration plant_sampled{};
    plant_sampled.channel = state.plant_output;
    plant_sampled.direction = rt::SampledIoDirection::output;
    plant_sampled.channel_identity = 101;
    plant_sampled.element_count = 1;
    plant_sampled.samples_per_frame = 4;
    plant_sampled.units_identity = 202;
    plant_sampled.calibration_identity = 303;
    plant_sampled.sample_period_ns = 1'000'000;
    plant_sampled.timestamp_domain_identity =
        rt::cross_rate_runtime_logical_timestamp_domain_identity;
    plant_sampled.clock_domain_identity = 1;
    plant_sampled.trigger_identity = 404;
    plant_sampled.ring_capacity = rt::cross_rate_snapshot_slot_count;
    plant_sampled.initial_sequence = 1;
    plant_sampled.maximum_age_ns = detail::sensor_period_ns;
    plant_sampled.underrun_policy =
        rt::SampledIoUnderrunPolicy::substitute_safe;
    // Safe transitions are setup/cleanup handshakes on shared portable hosts.
    // Their finite liveness guard is separate from the unchanged 80-ms active
    // provider/completion budgets. The startup fault test explicitly selects
    // the original 80-ms value to verify missing acknowledgement still expires.
    plant_sampled.safe_transition_timeout_ns = safe_transition_timeout_ns;
    plant_sampled.initial_frame = plant_initial;
    plant_sampled.startup_safe_frame = plant_safe;
    plant_sampled.failure_safe_frame = plant_safe;
    plant_sampled.shutdown_safe_frame = plant_safe;
    auto sensor_sampled = plant_sampled;
    sensor_sampled.channel = state.sensor_input;
    sensor_sampled.direction = rt::SampledIoDirection::input;
    sensor_sampled.channel_identity = 202;
    sensor_sampled.timestamp_domain_identity = 7;
    sensor_sampled.maximum_age_ns = detail::controller_period_ns;
    sensor_sampled.stale_policy =
        rt::SampledIoStalePolicy::substitute_initial;
    sensor_sampled.safe_transition_timeout_ns = 0;
    sensor_sampled.underrun_policy =
        rt::SampledIoUnderrunPolicy::fail_release;
    sensor_sampled.initial_frame = sensor_initial;
    sensor_sampled.startup_safe_frame = {};
    sensor_sampled.failure_safe_frame = {};
    sensor_sampled.shutdown_safe_frame = {};
    auto controller_sampled = plant_sampled;
    controller_sampled.channel = state.controller_output;
    controller_sampled.channel_identity = 303;
    controller_sampled.maximum_age_ns = detail::controller_period_ns;
    controller_sampled.initial_frame = controller_initial;
    controller_sampled.startup_safe_frame = controller_safe;
    controller_sampled.failure_safe_frame = controller_safe;
    controller_sampled.shutdown_safe_frame = controller_safe;
    auto actuator_sampled = sensor_sampled;
    actuator_sampled.channel = state.actuator_input;
    actuator_sampled.channel_identity = 404;
    actuator_sampled.maximum_age_ns = 450'000'000;
    actuator_sampled.initial_frame = actuator_initial;
    auto setup_status = runtime.register_sampled_io_channel(plant_sampled);
    if (setup_status != rt::Status::ok) {
        result.status = setup_status;
        result.failure_stage = 1;
        return result;
    }
    setup_status = runtime.register_sampled_io_channel(sensor_sampled);
    if (setup_status != rt::Status::ok) {
        result.status = setup_status;
        result.failure_stage = 2;
        return result;
    }
    setup_status = runtime.register_sampled_io_channel(controller_sampled);
    if (setup_status != rt::Status::ok) {
        result.status = setup_status;
        result.failure_stage = 5;
        return result;
    }
    setup_status = runtime.register_sampled_io_channel(actuator_sampled);
    if (setup_status != rt::Status::ok) {
        result.status = setup_status;
        result.failure_stage = 6;
        return result;
    }
    setup_status = runtime.finalize();
    if (setup_status != rt::Status::ok) {
        result.status = setup_status;
        result.failure_stage = 3;
        const auto message = runtime.last_error();
        std::copy_n(
            message.begin(),
            std::min<std::size_t>(
                message.size(), result.diagnostic.size() - 1),
            result.diagnostic.begin());
        return result;
    }
    if (startup_fault != rt::SampledIoLoopbackFault::none &&
        backend.inject_next(startup_fault) != rt::Status::ok) {
        result.status = rt::Status::invalid_config;
        result.failure_stage = 33;
        return result;
    }
    setup_status = runtime.start();
    if (setup_status != rt::Status::ok) {
        result.status = setup_status;
        result.failure_stage = 4;
        result.startup_cleanup_status = runtime.stop();
        result.loopback_logical_actions = backend.stats().logical_actions;
        return result;
    }
    rt::SampledIoChannelStatus plant_status;
    result.startup_safe_acknowledged = runtime.sampled_io_channel_status(
        state.plant_output, plant_status) &&
        plant_status.safety_state ==
            rt::SampledIoSafetyState::startup_acknowledged;
    std::array<std::byte, detail::checkpoint_capacity> checkpoint{};
    rt::ArtifactWriteResult checkpoint_write;
    if (runtime.write_checkpoint(0, checkpoint, checkpoint_write) !=
        rt::Status::ok) {
        result.status = rt::Status::invalid_artifact;
        result.failure_stage = 31;
        (void)runtime.stop();
        return result;
    }
    rt::StepResult step_result;
    if (active_fault != rt::SampledIoLoopbackFault::none &&
        backend.inject_next(active_fault) != rt::Status::ok) {
        result.status = rt::Status::invalid_state;
        result.failure_stage = 29;
        (void)runtime.stop();
        return result;
    }
    const auto step_status = runtime.step(
        {
            1,
            std::chrono::nanoseconds(detail::supercycle_ns),
            std::nullopt,
            std::uint64_t{1'000},
        },
        &step_result);
    if (step_status != rt::Status::ok) {
        result.status = step_status;
        result.callback_counts = state.callback_counts;
        result.failure_stage = 30;
        result.loopback_logical_actions = backend.stats().logical_actions;
        rt::SampledIoLoopbackLogicalAction backend_action;
        if (result.loopback_logical_actions != 0 &&
            backend.logical_action_at(
                static_cast<std::size_t>(
                    result.loopback_logical_actions - 1),
                backend_action)) {
            result.last_backend_status = backend_action.status;
        }
        rt::MixedRateActionCursor failure_cursor;
        std::array<rt::MixedRateActionRecord, 128> failure_actions{};
        rt::MixedRateActionReadResult failure_read;
        if (runtime.read_mixed_rate_actions(
                failure_cursor, failure_actions, failure_read) ==
            rt::Status::ok) {
            result.action_count = failure_read.records_read;
            for (std::size_t index = 0;
                 index < failure_read.records_read; ++index) {
                if (failure_actions[index].action ==
                    rt::MixedRateActionId::device_terminal) {
                    ++result.device_terminal_actions;
                    result.last_terminal_phase =
                        failure_actions[index].phase_index;
                    result.last_terminal_status =
                        failure_actions[index].terminal_status;
                    result.last_terminal_reason =
                        failure_actions[index].reason;
                    result.last_terminal_stage =
                        failure_actions[index].stage;
                }
            }
        }
        const auto expected_fault_status = [&]() {
            switch (active_fault) {
            case rt::SampledIoLoopbackFault::completion_error:
                return rt::Status::device_error;
            case rt::SampledIoLoopbackFault::completion_timeout:
                return rt::Status::device_timeout;
            case rt::SampledIoLoopbackFault::completion_lost:
                return rt::Status::device_lost;
            case rt::SampledIoLoopbackFault::malformed_sequence:
                return rt::Status::invalid_argument;
            case rt::SampledIoLoopbackFault::reject_submission:
                return rt::Status::device_queue_full;
            case rt::SampledIoLoopbackFault::none:
                return rt::Status::ok;
            }
            return rt::Status::internal_error;
        }();
        if (active_fault != rt::SampledIoLoopbackFault::none &&
            step_status == expected_fault_status) {
            const rt::ReplayInputRecord replay_input{
                {
                    1,
                    std::chrono::nanoseconds(detail::supercycle_ns),
                    std::nullopt,
                    std::uint64_t{1'000},
                },
                1,
                {},
            };
            std::array<
                std::byte,
                detail::replay_artifact_capacity> replay_artifact{};
            rt::ArtifactWriteResult replay_write;
            auto replay_status = runtime.write_active_replay_artifact(
                std::span<const std::byte>(checkpoint).first(
                    checkpoint_write.bytes_written),
                std::span<const rt::ReplayInputRecord>(&replay_input, 1),
                replay_artifact,
                replay_write);
            rt::ActiveReplayMetadata replay_metadata;
            if (replay_status == rt::Status::ok) {
                replay_status = rt::inspect_active_replay_artifact(
                    std::span<const std::byte>(replay_artifact).first(
                        replay_write.bytes_written),
                    replay_metadata);
            }
            if (replay_status == rt::Status::ok) {
                replay_status = backend.inject_next(active_fault);
            }
            rt::ActiveReplayResult replay_result;
            if (replay_status == rt::Status::ok) {
                replay_status = runtime.replay_active(
                    std::span<const std::byte>(replay_artifact).first(
                        replay_write.bytes_written),
                    [](void*, const rt::ReplayInputView&) {
                        return rt::CallbackResult::ok;
                    },
                    nullptr,
                    &replay_result);
            }
            result.replay_actions_compared =
                replay_result.actions_compared;
            result.replay_mismatch_sequence =
                replay_result.mismatch_sequence;
            if (replay_result.mismatch_sequence <
                failure_read.records_read) {
                const auto& expected = failure_actions[
                    static_cast<std::size_t>(
                        replay_result.mismatch_sequence)];
                result.replay_expected_batch = expected.batch_identity;
                result.replay_expected_timestamp = expected.timestamp;
                result.replay_expected_payload =
                    expected.payload_content_identity;
                result.replay_expected_action =
                    static_cast<std::uint8_t>(expected.action);
            }
            rt::MixedRateActionMetadata replay_action_metadata;
            if (runtime.mixed_rate_action_metadata(
                    replay_action_metadata) == rt::Status::ok) {
                rt::MixedRateActionCursor replay_cursor;
                replay_cursor.runtime_id = replay_action_metadata.runtime_id;
                replay_cursor.next_sequence =
                    replay_result.mismatch_sequence;
                rt::MixedRateActionRecord actual;
                rt::MixedRateActionReadResult actual_read;
                if (runtime.read_mixed_rate_actions(
                        replay_cursor,
                        std::span<rt::MixedRateActionRecord>(&actual, 1),
                        actual_read) == rt::Status::ok &&
                    actual_read.records_read == 1) {
                    result.replay_actual_batch = actual.batch_identity;
                    result.replay_actual_timestamp = actual.timestamp;
                    result.replay_actual_payload =
                        actual.payload_content_identity;
                    result.replay_actual_action =
                        static_cast<std::uint8_t>(actual.action);
                    if (replay_result.mismatch_sequence <
                        failure_read.records_read) {
                        const auto& expected = failure_actions[
                            static_cast<std::size_t>(
                                replay_result.mismatch_sequence)];
                        const auto expected_bytes = std::as_bytes(
                            std::span<const rt::MixedRateActionRecord>(
                                &expected, 1));
                        const auto actual_bytes = std::as_bytes(
                            std::span<const rt::MixedRateActionRecord>(
                                &actual, 1));
                        for (std::size_t byte = 0;
                             byte < expected_bytes.size(); ++byte) {
                            if (expected_bytes[byte] != actual_bytes[byte]) {
                                result.replay_first_different_byte = byte;
                                break;
                            }
                        }
                    }
                }
            }
            result.active_replay_exact =
                replay_status == expected_fault_status &&
                replay_result.mismatch_status == rt::Status::ok &&
                replay_result.actions_compared ==
                    replay_metadata.action_record_count;
            const auto stop_status = runtime.stop();
            result.loopback_logical_actions =
                backend.stats().logical_actions;
            rt::SampledIoChannelStatus failed_status;
            result.failure_safe_acknowledged =
                runtime.sampled_io_channel_status(
                    state.plant_output, failed_status) &&
                failed_status.safety_state ==
                    rt::SampledIoSafetyState::failure_acknowledged;
            result.status =
                result.active_replay_exact &&
                    result.failure_safe_acknowledged &&
                    stop_status == rt::Status::ok
                ? rt::Status::ok
                : replay_status;
            if (result.status != rt::Status::ok) {
                result.failure_stage = 33;
                const auto message = runtime.last_error();
                std::copy_n(
                    message.begin(),
                    std::min<std::size_t>(
                        message.size(), result.diagnostic.size() - 1),
                    result.diagnostic.begin());
            }
            return result;
        }
        const auto message = runtime.last_error();
        std::copy_n(
            message.begin(),
            std::min<std::size_t>(
                message.size(), result.diagnostic.size() - 1),
            result.diagnostic.begin());
        (void)runtime.stop();
        return result;
    }
    result.callback_counts = state.callback_counts;
    rt::MixedRateActionCursor cursor;
    std::array<rt::MixedRateActionRecord, 128> actions{};
    rt::MixedRateActionReadResult action_result;
    if (runtime.read_mixed_rate_actions(
            cursor, actions, action_result) != rt::Status::ok ||
        action_result.lost_records != 0) {
        result.status = rt::Status::invalid_artifact;
        (void)runtime.stop();
        return result;
    }
    result.action_count = action_result.records_read;
    bool has_release = false;
    for (std::size_t index = 0; index < result.action_count; ++index) {
        const auto& action = actions[index];
        if (action.stage == rt::MixedRateActionStage::decision &&
            action.action >= rt::MixedRateActionId::rate_execute &&
            action.action <= rt::MixedRateActionId::rate_recover) {
            if (!has_release) {
                result.first_logical_release_ns = action.logical_release_ns;
                has_release = true;
            }
            result.last_logical_release_ns = action.logical_release_ns;
        }
        result.device_terminal_actions +=
            action.action == rt::MixedRateActionId::device_terminal ? 1u : 0u;
        result.sampled_publish_actions +=
            action.action == rt::MixedRateActionId::sampled_publish ? 1u : 0u;
        result.sampled_select_actions +=
            action.action == rt::MixedRateActionId::sampled_select ? 1u : 0u;
        result.safe_transition_actions +=
            action.action == rt::MixedRateActionId::safe_transition ? 1u : 0u;
    }
    rt::MemoryPlan plan;
    result.memory_accounting_exact = runtime.memory_plan(plan) &&
        plan.planned_bytes ==
            plan.runtime_control_bytes +
            plan.executor_control_bytes +
            plan.device_control_bytes +
            plan.phase_scratch_total_bytes +
            plan.task_scratch_total_bytes +
            plan.trace_storage_bytes;
    const rt::ReplayInputRecord replay_input{
        {
            1,
            std::chrono::nanoseconds(detail::supercycle_ns),
            std::nullopt,
            std::uint64_t{1'000},
        },
        1,
        {},
    };
    std::array<
        std::byte,
        detail::replay_artifact_capacity> replay_artifact{};
    rt::ArtifactWriteResult replay_write;
    auto replay_status = runtime.write_active_replay_artifact(
        std::span<const std::byte>(checkpoint).first(
            checkpoint_write.bytes_written),
        std::span<const rt::ReplayInputRecord>(&replay_input, 1),
        replay_artifact,
        replay_write);
    rt::ActiveReplayResult replay_result;
    if (replay_status == rt::Status::ok) {
        replay_status = runtime.replay_active(
            std::span<const std::byte>(replay_artifact).first(
                replay_write.bytes_written),
            [](void*, const rt::ReplayInputView&) {
                return rt::CallbackResult::ok;
            },
            nullptr,
            &replay_result);
    }
    result.active_replay_exact = replay_status == rt::Status::ok;
    result.replay_actions_compared = replay_result.actions_compared;
    if (replay_status != rt::Status::ok) {
        result.status = replay_status;
        result.failure_stage = 32;
        result.replay_mismatch_sequence = replay_result.mismatch_sequence;
        if (replay_result.mismatch_sequence < result.action_count) {
            const auto& expected_action =
                actions[static_cast<std::size_t>(
                    replay_result.mismatch_sequence)];
            result.replay_expected_batch = expected_action.batch_identity;
            result.replay_expected_timestamp = expected_action.timestamp;
            result.replay_expected_payload =
                expected_action.payload_content_identity;
            result.replay_expected_action =
                static_cast<std::uint8_t>(expected_action.action);
        }
        rt::MixedRateActionMetadata failure_metadata;
        if (runtime.mixed_rate_action_metadata(failure_metadata) ==
            rt::Status::ok) {
            rt::MixedRateActionCursor mismatch_cursor;
            mismatch_cursor.runtime_id = failure_metadata.runtime_id;
            mismatch_cursor.next_sequence = replay_result.mismatch_sequence;
            rt::MixedRateActionRecord actual_action;
            rt::MixedRateActionReadResult mismatch_read;
            if (runtime.read_mixed_rate_actions(
                    mismatch_cursor,
                    std::span<rt::MixedRateActionRecord>(&actual_action, 1),
                    mismatch_read) == rt::Status::ok &&
                mismatch_read.records_read == 1) {
                result.replay_actual_batch = actual_action.batch_identity;
                result.replay_actual_timestamp = actual_action.timestamp;
                result.replay_actual_payload =
                    actual_action.payload_content_identity;
                result.replay_actual_action =
                    static_cast<std::uint8_t>(actual_action.action);
            }
        }
        const auto message = runtime.last_error();
        std::copy_n(
            message.begin(),
            std::min<std::size_t>(
                message.size(), result.diagnostic.size() - 1),
            result.diagnostic.begin());
        (void)runtime.stop();
        return result;
    }
    result.status = runtime.stop();
    result.loopback_logical_actions = backend.stats().logical_actions;
    result.shutdown_safe_acknowledged = runtime.sampled_io_channel_status(
        state.plant_output, plant_status) &&
        plant_status.safety_state ==
            rt::SampledIoSafetyState::shutdown_acknowledged;
    return result;
}

} // namespace rtfw_test
