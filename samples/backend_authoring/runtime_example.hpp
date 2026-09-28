#pragma once
#include "backend.hpp"
#include <rt/mock_device.hpp>
#include <rt/sdk.hpp>
#include <array>
#include <chrono>

namespace backend_kit {
// Borrowed state is declared before Runtime/guard by every caller.
struct RuntimeState {
    std::array<std::byte, 32> source{}, destination{}, legacy{};
    rt::DeviceBufferHandle source_handle{}, destination_handle{}, legacy_handle{};
    std::uint64_t submitted = 0, verified = 0;
    static rt::CallbackResult copy(void* p, const rt::DeviceCallbackContext&,
                                   rt::DeviceSubmission& c) noexcept {
        auto& s = *static_cast<RuntimeState*>(p);
        ++s.submitted;
        // Host writes are safe here: last step completed before the next step.
        s.source.fill(static_cast<std::byte>(s.submitted));
        c.opcode = copy_opcode; c.timeout_ns = 10'000'000'000; c.buffer_count = 2;
        c.buffers[0] = {s.source_handle.value, RTFW_DEVICE_ACCESS_READ, 0, 0, 32};
        c.buffers[1] = {s.destination_handle.value, RTFW_DEVICE_ACCESS_WRITE, 0, 0, 32};
        return rt::CallbackResult::ok;
    }
    static rt::CallbackResult fill(void* p, const rt::DeviceCallbackContext&,
                                   rt::DeviceSubmission& c) noexcept {
        auto& s = *static_cast<RuntimeState*>(p);
        c.opcode = rt::mock_device_opcode_fill; c.timeout_ns = 10'000'000'000;
        c.payload_size = 1; c.payload[0] = 0x5a; c.buffer_count = 1;
        c.buffers[0] = {s.legacy_handle.value, RTFW_DEVICE_ACCESS_WRITE, 0, 0, 32};
        return rt::CallbackResult::ok;
    }
    static rt::CallbackResult verify(void* p, const rt::CallbackContext&) noexcept {
        auto& s = *static_cast<RuntimeState*>(p);
        if (s.submitted != s.verified + 1 ||
            !std::all_of(s.destination.begin(), s.destination.end(), [&](auto x) {
                return x == static_cast<std::byte>(s.submitted);
            }) || !std::all_of(s.legacy.begin(), s.legacy.end(), [](auto x) { return x == std::byte{0x5a}; }))
            return rt::CallbackResult::error;
        ++s.verified; return rt::CallbackResult::ok;
    }
};
inline rt::Status configure(rt::Runtime& runtime, Backend& native,
                            rt::MockDeviceBackend& legacy, RuntimeState& state) {
    rt::RuntimeConfig c; c.callback_capacity = 3; c.worker_count = 2;
    c.executor_queue_capacity = 4; c.task_scratch_slots = 4;
    c.device_backend_capacity = 2; c.device_buffer_capacity = 3;
    c.device_outstanding_capacity = 1; c.device_completion_batch = 1;
    auto result = runtime.configure(c); if (result != rt::Status::ok) return result;
    rt::DeviceBackendHandle n, v1; rt::PhaseHandle copy, fill, verify;
    result = runtime.register_device_backend(rt::HalV2BackendRegistration{"native.copy", native.api()}, n);
    if (result != rt::Status::ok) return result;
    result = runtime.register_device_backend(rt::DeviceBackendRegistration{"legacy.fill", legacy.api()}, v1);
    if (result != rt::Status::ok) return result;
    result = runtime.register_device_buffer({"copy.source", n, state.source}, state.source_handle);
    if (result != rt::Status::ok) return result;
    result = runtime.register_device_buffer({"copy.destination", n, state.destination}, state.destination_handle);
    if (result != rt::Status::ok) return result;
    result = runtime.register_device_buffer({"legacy.destination", v1, state.legacy}, state.legacy_handle);
    if (result != rt::Status::ok) return result;
    result = runtime.register_device_phase({"copy", n, RuntimeState::copy, &state}, copy);
    if (result != rt::Status::ok) return result;
    result = runtime.register_device_phase({"fill", v1, RuntimeState::fill, &state}, fill);
    if (result != rt::Status::ok) return result;
    result = runtime.register_callback({"verify", RuntimeState::verify, &state}, verify);
    if (result != rt::Status::ok) return result;
    result = runtime.add_dependency(copy, fill); if (result != rt::Status::ok) return result;
    result = runtime.add_dependency(copy, verify); if (result != rt::Status::ok) return result;
    result = runtime.add_dependency(fill, verify); if (result != rt::Status::ok) return result;
    return runtime.finalize();
}
} // namespace backend_kit
