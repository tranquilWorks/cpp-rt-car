#pragma once
#include "model.hpp"
#include <rt/cuda_backend.hpp>
#include <rt/runtime.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>

namespace rtfw::cuda_physics {
struct Session {
    rt::CudaDriverApi driver{};
    rt::CudaContext context{};
    rt::CudaStream stream{};
    rt::CudaFunction function{};
};
// Session resources must outlive this owner and successful checked stop.
class Scenario {
public:
    Scenario(const Options& options, const Session& session)
        : options_(options), streams_{session.stream},
          backend_(session.driver, backend_config(session)), function_(session.function) {}
    Scenario(const Scenario&) = delete;
    Scenario& operator=(const Scenario&) = delete;
    ~Scenario() { if (stop() != rt::Status::ok) std::terminate(); }

    rt::Status prepare() noexcept {
        if (!options_.valid() || !function_ || !streams_[0]) return rt::Status::invalid_argument;
        auto seed = options_.seed;
        for (std::uint32_t i = 0; i < options_.count; ++i)
            state_[i] = initial_[i] = initial_particle(seed);
        rt::RuntimeConfig config;
        config.worker_count = options_.workers;
        config.callback_capacity = 3;
        config.device_backend_capacity = 1;
        config.device_buffer_capacity = 1;
        config.device_outstanding_capacity = 1;
        config.device_completion_batch = 1;
        config.memory_budget_bytes = 128 * 1024 * 1024;
        config.scratch_bytes = 4096;
        config.trace_capacity = 128;
        config.executor_queue_capacity = 16;
        config.task_scratch_bytes = 256;
        config.task_scratch_slots = 16;
        auto s = runtime_.configure(config);
        if (s != rt::Status::ok) return s;
        std::uint64_t kernel{};
        if (backend_.register_kernel(function_, kernel) != RTFW_DEVICE_STATUS_OK)
            return rt::Status::device_error;
        s = runtime_.register_device_backend(backend_.hal_v2_registration("physics.cuda"), backend_handle_);
        if (s != rt::Status::ok) return s;
        rt::DeviceMemoryDomainHandle domain;
        rt::HalV2MemoryDomain descriptor;
        if (!runtime_.device_memory_domain_at(backend_handle_, 1, domain, descriptor))
            return rt::Status::device_error;
        auto bytes = std::as_writable_bytes(std::span(state_.data(), options_.count));
        rt::DeviceBufferHandle buffer;
        s = runtime_.register_device_buffer(
            {"physics.state", backend_handle_, domain, bytes, {}, bytes.size(),
             rt::HalV2MemoryOwnership::borrowed_host,
             RTFW_DEVICE_BUFFER_HOST_READ | RTFW_DEVICE_BUFFER_HOST_WRITE |
             RTFW_DEVICE_BUFFER_DEVICE_READ | RTFW_DEVICE_BUFFER_DEVICE_WRITE,
             rt::HalV2MemoryCoherency::staged_copy,
             rt::hal_v2_memory_sync_copy_to_device | rt::hal_v2_memory_sync_copy_from_device}, buffer);
        if (s != rt::Status::ok) return s;
        s = runtime_.register_device_timeline({"physics.done", backend_handle_, 0}, timeline_);
        if (s != rt::Status::ok) return s;
        rt::HalV2BufferReference reference;
        reference.buffer_token = buffer.value;
        reference.bytes = bytes.size();
        reference.access = RTFW_DEVICE_ACCESS_READ_WRITE;
        batch_.command_count = 3;
        batch_.signal_count = 1;
        batch_.signals[0].timeline_handle = timeline_.value;
        auto& upload = batch_.commands[0];
        upload.kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::copy);
        upload.operation = static_cast<std::uint32_t>(rt::HalV2MemoryOperation::copy_to_device);
        upload.source = reference; upload.source.access = RTFW_DEVICE_ACCESS_READ;
        upload.destination = reference; upload.destination.access = RTFW_DEVICE_ACCESS_WRITE;
        auto& launch = batch_.commands[1];
        launch.kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
        launch.opcode = rt::cuda_device_opcode_launch_kernel;
        launch.buffer_count = 1; launch.buffers[0] = reference;
        rt::CudaKernelLaunch args;
        args.kernel_token = kernel;
        args.block_x = 128;
        args.grid_x = (options_.count + 127)/128;
        if (!rt::cuda_kernel_add_buffer_argument(args, 0) ||
            !rt::cuda_kernel_add_scalar_argument(args, options_.count)) return rt::Status::internal_error;
        launch.payload_size = sizeof(args);
        std::memcpy(launch.payload.data(), &args, sizeof(args));
        batch_.commands[2] = upload;
        batch_.commands[2].operation = static_cast<std::uint32_t>(rt::HalV2MemoryOperation::copy_from_device);
        rt::PhaseHandle prepare_phase, device_phase, validate_phase;
        s = runtime_.register_callback({"physics.prepare", &prepare_frame, this}, prepare_phase);
        if (s != rt::Status::ok) return s;
        s = runtime_.register_device_batch_phase(
            {"physics.integrate", backend_handle_, &provide, this, batch_}, device_phase);
        if (s != rt::Status::ok) return s;
        s = runtime_.register_callback({"physics.validate", &validate, this}, validate_phase);
        if (s != rt::Status::ok) return s;
        s = runtime_.add_dependency(prepare_phase, device_phase);
        if (s != rt::Status::ok) return s;
        s = runtime_.add_dependency(device_phase, validate_phase);
        if (s != rt::Status::ok) return s;
        rt::ResourceHandle resource;
        s = runtime_.register_resource("physics.state", resource);
        if (s != rt::Status::ok) return s;
        s = runtime_.declare_resource_access(device_phase, resource, rt::ResourceAccess::write);
        if (s != rt::Status::ok) return s;
        s = runtime_.declare_resource_access(validate_phase, resource, rt::ResourceAccess::read);
        return s == rt::Status::ok ? runtime_.finalize() : s;
    }
    rt::Status start() noexcept { return runtime_.start(); }
    rt::Status step() noexcept {
        if (failed_) return rt::Status::invalid_state;
        if (completed_ >= options_.steps) return rt::Status::invalid_argument;
        const auto s = runtime_.step({completed_+1, std::chrono::nanoseconds(3'906'250), std::nullopt});
        needs_reset_ = s == rt::Status::device_reset_required;
        failed_ = s != rt::Status::ok;
        if (s == rt::Status::ok) {
            rt::DeviceTimelineInfo info;
            if (!runtime_.device_timeline_at(backend_handle_, 0, info) ||
                info.completed_value != completed_+1 || info.last_accepted_value != completed_+1)
                return rt::Status::internal_error;
            ++completed_;
        }
        return s;
    }
    rt::Status run() noexcept {
        while (completed_ < options_.steps) {
            const auto s = step();
            if (s != rt::Status::ok) return s;
        }
        return rt::Status::ok;
    }
    rt::Status stop() noexcept {
        // A rejected CUDA launch/copy can leave vendor work quarantined.
        // Settle it through the public host-control reset before unregistering
        // buffers. A failed reset retains ownership and can be retried.
        if (needs_reset_) {
            const auto reset = runtime_.reset_device(backend_handle_);
            if (reset != rt::Status::ok) return reset;
            needs_reset_ = false;
        }
        // No device ownership is acquired before start/finalize. Failed start
        // retains finalized state so checked stop still retries live cleanup.
        return runtime_.state() == rt::RuntimeState::configuring ? rt::Status::ok : runtime_.stop();
    }
    std::string_view error() const noexcept { return runtime_.last_error(); }
    std::uint64_t completed() const noexcept { return completed_; }
    std::uint64_t publications() const noexcept { return publications_.load(); }
    std::uint64_t preparations() const noexcept { return preparations_.load(); }
    std::uint64_t submissions() const noexcept { return submissions_.load(); }
    const Particle& particle(std::size_t i) const { return state_.at(i); }
private:
    rt::CudaBackendConfig backend_config(const Session& session) const noexcept {
        rt::CudaBackendConfig c;
        c.context = session.context; c.streams = streams_;
        c.queue_capacity = 1; c.buffer_capacity = 1; c.kernel_capacity = 1;
        return c;
    }
    static rt::CallbackResult prepare_frame(void* p, const rt::CallbackContext& c) noexcept {
        auto& self = *static_cast<Scenario*>(p);
        if (c.frame.frame_index != self.completed_+1) return rt::CallbackResult::error;
        self.preparations_.fetch_add(1);
        return rt::CallbackResult::ok;
    }
    static rt::CallbackResult provide(void* p, const rt::DeviceCallbackContext& c,
                                     rt::DeviceCommandBatch& batch) noexcept {
        auto& self = *static_cast<Scenario*>(p);
        batch = self.batch_;
        batch.timeout_ns = 5'000'000'000ULL;
        batch.signals[0].value = c.frame.frame_index;
        self.submissions_.fetch_add(1);
        return rt::CallbackResult::ok;
    }
    static rt::CallbackResult validate(void* p, const rt::CallbackContext& c) noexcept {
        auto& self = *static_cast<Scenario*>(p);
        if (c.frame.frame_index > self.options_.steps) return rt::CallbackResult::error;
        for (std::uint32_t i = 0; i < self.options_.count; ++i)
            if (!matches_oracle(self.state_[i], self.initial_[i],
                                static_cast<std::uint32_t>(c.frame.frame_index)))
                return rt::CallbackResult::error;
        self.publications_.fetch_add(1);
        return rt::CallbackResult::ok;
    }
    Options options_;
    std::array<rt::CudaStream, 1> streams_;
    std::array<Particle, max_particles> initial_{}, state_{};
    rt::CudaDeviceBackend backend_;
    rt::CudaFunction function_;
    rt::Runtime runtime_;
    rt::DeviceBackendHandle backend_handle_;
    rt::DeviceTimelineHandle timeline_;
    rt::DeviceCommandBatch batch_;
    std::uint64_t completed_{};
    bool needs_reset_{};
    bool failed_{};
    std::atomic<std::uint64_t> publications_{}, preparations_{}, submissions_{};
};
static_assert(sizeof(Scenario) < 32 * 1024 * 1024);
} // namespace rtfw::cuda_physics
