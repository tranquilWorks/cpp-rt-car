#pragma once
#include "../golden_system/memory.hpp"
#include "../golden_cuda/simulated_driver.hpp"
namespace golden::cuda {
// The adapter is an explicit simulator contract around an actual CUDA backend.
// Its factory accepts only this kit's owned deterministic driver. Native
// resource construction never opts into mock capabilities or trusted replay.
class CapacityProbePhysics final : public golden::Physics {
  Resources resources_;
  SimulatedDriver *simulation_ = nullptr;
  bool graph_;
  std::unique_ptr<Storage> staging_ = std::make_unique<Storage>();
  std::unique_ptr<rt::CudaDeviceBackend> backend_;
  rt::HalV2BackendApi original_{};
  rt::Runtime *runtime_ = nullptr;
  World *world_ = nullptr;
  rt::DeviceCommandBatch batch_{};
  rt::DeviceTimelineHandle timeline_{};
  rt::DeviceBackendHandle handle_{};
  std::uint64_t expected_signal_ = 0;
  rt::HalV2BackendRegistration
  simulated(rt::HalV2BackendRegistration r) noexcept {
    original_ = r.api;
    r.name = "golden.simulated.cuda";
    r.api.instance = this;
    r.api.get_capabilities = [](void *p, rt::HalV2Capabilities *c) {
      auto &s = *static_cast<CapacityProbePhysics *>(p);
      const auto status = s.original_.get_capabilities(s.original_.instance, c);
      if (status == rt::HalV2Status::ok) {
        if (!s.simulation_)
          return rt::HalV2Status::invalid_argument;
        c->deterministic_mock = 1;
        c->backend_id.fill(0);
        constexpr char name[] = "golden.sim.cuda.v1";
        std::copy_n(name, sizeof(name), c->backend_id.begin());
        c->control_storage_bytes += sizeof(rt::HalV2BackendApi);
      }
      return status;
    };
    r.api.initialize = [](void *p, const rt::HalV2InitializeConfig *v) {
      auto &s = *static_cast<CapacityProbePhysics *>(p);
      return s.original_.initialize(s.original_.instance, v);
    };
    r.api.register_buffer = [](void *p, const rt::HalV2BufferRegistration *v,
                               std::uint64_t *out) {
      auto &s = *static_cast<CapacityProbePhysics *>(p);
      return s.original_.register_buffer(s.original_.instance, v, out);
    };
    r.api.unregister_buffer = [](void *p, std::uint64_t v) {
      auto &s = *static_cast<CapacityProbePhysics *>(p);
      return s.original_.unregister_buffer(s.original_.instance, v);
    };
    r.api.submit = [](void *p, const rt::HalV2Submission *v) {
      auto &s = *static_cast<CapacityProbePhysics *>(p);
      return s.original_.submit(s.original_.instance, v);
    };
    r.api.poll = [](void *p, rt::HalV2Completion *v, std::uint64_t n,
                    std::uint64_t *out) {
      auto &s = *static_cast<CapacityProbePhysics *>(p);
      return s.original_.poll(s.original_.instance, v, n, out);
    };
    r.api.cancel = [](void *p, std::uint64_t v) {
      auto &s = *static_cast<CapacityProbePhysics *>(p);
      return s.original_.cancel(s.original_.instance, v);
    };
    r.api.get_health = [](void *p, rt::HalV2Health *v) {
      auto &s = *static_cast<CapacityProbePhysics *>(p);
      return s.original_.get_health(s.original_.instance, v);
    };
    r.api.reset = [](void *p) {
      auto &s = *static_cast<CapacityProbePhysics *>(p);
      return s.original_.reset(s.original_.instance);
    };
    r.api.shutdown = [](void *p) {
      auto &s = *static_cast<CapacityProbePhysics *>(p);
      return s.original_.shutdown(s.original_.instance);
    };
    // Memory and command extensions retain their original backend instances.
    return r;
  }
  static rt::CallbackResult provide(void *p, const rt::DeviceCallbackContext &c,
                                    rt::DeviceCommandBatch &b) noexcept {
    auto &s = *static_cast<CapacityProbePhysics *>(p);
    if (!c.rate_release || c.rate_release->phase.index() != 1 ||
        c.rate_release->logical_release_ns !=
            c.frame.frame_index * fixed::tick_ns ||
        !s.world_->configuration.consume(c.live_control))
      return rt::CallbackResult::error;
    rt::DeviceTimelineInfo info;
    if (!s.runtime_->device_timeline_at(s.handle_, 0, info) ||
        info.last_accepted_value == UINT64_MAX)
      return rt::CallbackResult::error;
    b = s.batch_;
    b.timeout_ns = completion_ns;
    s.expected_signal_ = info.last_accepted_value + 1;
    b.signals[0].value = s.expected_signal_;
    ++s.providers;
    return rt::CallbackResult::ok;
  }

public:
  std::uint64_t providers = 0, publications = 0;
  CapacityProbePhysics(Resources resources, bool graph)
      : resources_(resources), graph_(graph) {}
  CapacityProbePhysics(SimulatedDriver &driver, bool graph)
      : resources_(driver.resources()), simulation_(&driver), graph_(graph) {}
  bool replay_enabled() const noexcept override {
    return simulation_ != nullptr;
  }
  void limits(rt::RuntimeConfig &c) const noexcept override {
    c.device_backend_capacity = 1;
    c.device_buffer_capacity = 2;
    c.device_outstanding_capacity = 1;
    c.device_completion_batch = 1;
  }
  rt::Status configure(rt::Runtime &r, World &w) noexcept override {
    runtime_ = &r;
    world_ = &w;
    // This portable device sample parks idle CPU workers while Runtime's
    // backend lane services a release. It makes no spinning/RT guarantee.
    auto cpu_policy = Memory::policy();
    cpu_policy.thread_policy_count = 1;
    cpu_policy.thread_policies[0].role = rt::thread_role_executor_worker;
    cpu_policy.thread_policies[0].policy.wait_strategy = rt::WaitStrategy::park;
    const auto cpu_status = r.set_cpu_memory_policy(cpu_policy);
    if (cpu_status != rt::Status::ok)
      return cpu_status;
    if (!resources_.context || !resources_.stream || !resources_.function ||
        (graph_ && !resources_.graph) || !resources_.addresses[0] ||
        !resources_.addresses[1])
      return rt::Status::invalid_argument;
    const auto a = resources_.addresses[0], b = resources_.addresses[1];
    if (a > UINT64_MAX - sizeof(State) || b > UINT64_MAX - sizeof(State) ||
        (a < b + sizeof(State) && b < a + sizeof(State)))
      return rt::Status::invalid_argument;
    rt::CudaBackendConfig c;
    c.context = resources_.context;
    c.streams = std::span(&resources_.stream, 1);
    c.queue_capacity = 3;
    c.buffer_capacity = 2;
    c.kernel_capacity = 1;
    c.allocate_device_mirrors = false;
    try {
      backend_ = std::make_unique<rt::CudaDeviceBackend>(resources_.driver, c);
    } catch (...) {
      return rt::Status::resource_exhausted;
    }
    std::uint64_t kernel{};
    if (backend_->register_kernel(resources_.function, kernel) !=
        RTFW_DEVICE_STATUS_OK)
      return rt::Status::invalid_argument;
    for (std::size_t i = 0; i < 2; ++i)
      if (backend_->bind_device_buffer(buffer_names[i], resources_.addresses[i],
                                       sizeof(State)) != RTFW_DEVICE_STATUS_OK)
        return rt::Status::invalid_argument;
    if (graph_) {
      const std::array bindings{rt::CudaGraphBufferBinding{
          buffer_names[1], RTFW_DEVICE_ACCESS_READ_WRITE}};
      if (backend_->register_graph(2603, resources_.graph, bindings) !=
          RTFW_DEVICE_STATUS_OK)
        return rt::Status::invalid_argument;
    }
    auto registration = backend_->hal_v2_registration("golden.native.cuda");
    if (simulation_)
      registration = simulated(registration);
    auto s = r.register_device_backend(registration, handle_);
    if (s != rt::Status::ok)
      return s;
    rt::DeviceMemoryDomainHandle memory;
    rt::HalV2MemoryDomain descriptor;
    if (!r.device_memory_domain_at(handle_, 1, memory, descriptor))
      return rt::Status::device_error;
    std::array<rt::HalV2BufferReference, 2> refs{};
    for (std::size_t i = 0; i < 2; ++i) {
      auto bytes = std::as_writable_bytes(std::span(staging_->values[i]));
      rt::DeviceBufferHandle buffer;
      s = r.register_device_buffer(
          {buffer_names[i],
           handle_,
           memory,
           bytes,
           {},
           bytes.size(),
           rt::HalV2MemoryOwnership::borrowed_host,
           RTFW_DEVICE_BUFFER_HOST_READ | RTFW_DEVICE_BUFFER_HOST_WRITE |
               RTFW_DEVICE_BUFFER_DEVICE_READ | RTFW_DEVICE_BUFFER_DEVICE_WRITE,
           rt::HalV2MemoryCoherency::staged_copy,
           rt::hal_v2_memory_sync_copy_to_device |
               rt::hal_v2_memory_sync_copy_from_device},
          buffer);
      if (s != rt::Status::ok)
        return s;
      refs[i].buffer_token = buffer.value;
      refs[i].bytes = bytes.size();
    }
    s = r.register_device_timeline({"golden.cuda.timeline", handle_, 0},
                                   timeline_);
    if (s != rt::Status::ok)
      return s;
    auto &batch = batch_;
    batch.command_count = 5;
    batch.signal_count = 1;
    batch.signals[0].timeline_handle = timeline_.value;
    auto &up = batch.commands[0];
    up.kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::copy);
    up.operation =
        static_cast<std::uint32_t>(rt::HalV2MemoryOperation::copy_to_device);
    up.source = up.destination = refs[0];
    up.source.access = RTFW_DEVICE_ACCESS_READ;
    up.destination.access = RTFW_DEVICE_ACCESS_WRITE;
    auto &init = batch.commands[1];
    init = up;
    init.source = init.destination = refs[1];
    init.source.access = RTFW_DEVICE_ACCESS_READ;
    init.destination.access = RTFW_DEVICE_ACCESS_WRITE;
    auto &copy = batch.commands[2];
    copy.kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
    copy.opcode = rt::cuda_device_opcode_copy_device_to_device;
    copy.buffer_count = 2;
    copy.buffers[0] = refs[0];
    copy.buffers[0].access = RTFW_DEVICE_ACCESS_READ;
    copy.buffers[1] = refs[1];
    copy.buffers[1].access = RTFW_DEVICE_ACCESS_WRITE;
    auto &launch = batch.commands[3];
    launch.kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
    launch.buffer_count = 1;
    launch.buffers[0] = refs[1];
    launch.buffers[0].access = RTFW_DEVICE_ACCESS_READ_WRITE;
    if (graph_)
      launch.opcode = rt::cuda_device_opcode_graph(2603);
    else {
      launch.opcode = rt::cuda_device_opcode_launch_kernel;
      rt::CudaKernelLaunch args;
      args.kernel_token = kernel;
      args.block_x = 128;
      args.grid_x = static_cast<std::uint32_t>((w.options.count + 127) / 128);
      if (!rt::cuda_kernel_add_buffer_argument(args, 0) ||
          !rt::cuda_kernel_add_scalar_argument(
              args, static_cast<std::uint32_t>(w.options.count)))
        return rt::Status::internal_error;
      launch.payload_size = sizeof(args);
      std::memcpy(launch.payload.data(), &args, sizeof(args));
    }
    auto &down = batch.commands[4];
    down = init;
    down.operation =
        static_cast<std::uint32_t>(rt::HalV2MemoryOperation::copy_from_device);
    return rt::Status::ok;
  }
  rt::Status register_phase(rt::Runtime &r, rt::RateDomainHandle rate,
                            rt::PhaseHandle &phase) noexcept override {
    auto s = r.register_device_batch_phase(
        {fixed::phase_names[1], handle_, provide, this, batch_}, phase);
    if (s != rt::Status::ok)
      return s;
    using Role = rt::DeviceRatePayloadRole;
    const std::array roles{Role::input,        Role::output, Role::input,
                           Role::output,       Role::input,  Role::output,
                           Role::input_output, Role::input,  Role::output};
    rt::DeviceRatePhaseBinding binding{phase, rate, completion_ns, 1, roles};
    if (simulation_)
      binding.simulation = rt::DeviceRateSimulationPolicy{5'000'000'000ull};
    return r.bind_device_phase_to_rate_domain(binding);
  }
  bool input(const rt::CallbackContext &c) noexcept override {
    if (!c.rate_release || world_->next_tick != c.frame.frame_index + 1)
      return false;
    if (simulation_)
      simulation_->time(c.rate_release->nominal_release_ns);
    auto &in = staging_->values[0];
    for (std::size_t a = 0; a < 3; ++a)
      for (std::size_t i = 0; i < fixed::capacity; ++i) {
        in[a * fixed::capacity + i] = world_->plant.position[a][i];
        in[(a + 3) * fixed::capacity + i] = world_->plant.velocity[a][i];
        in[(a + 6) * fixed::capacity + i] = world_->command[a][i];
      }
    staging_->values[1].fill(0);
    return true;
  }
  bool complete(const rt::CallbackContext &) noexcept override {
    rt::DeviceTimelineInfo info;
    if (!runtime_->device_timeline_at(handle_, 0, info) || !expected_signal_ ||
        info.completed_value != expected_signal_)
      return false;
    // Validate before touching application state or publishing a channel.
    const auto &out = staging_->values[1];
    for (std::size_t a = 0; a < 3; ++a)
      for (std::size_t i = 0; i < fixed::capacity; ++i) {
        const auto effort = world_->command[a][i];
        const auto v = std::int64_t(world_->plant.velocity[a][i]) + effort;
        const auto x = std::int64_t(world_->plant.position[a][i]) + v;
        if (v < -4160 || v > 4160 || x < -2165760 || x > 2165760 ||
            effort < -4 || effort > 4 || out[a * fixed::capacity + i] != x ||
            out[(a + 3) * fixed::capacity + i] != v ||
            out[(a + 6) * fixed::capacity + i] != effort)
          return false;
      }
    for (std::size_t a = 0; a < 3; ++a)
      for (std::size_t i = 0; i < fixed::capacity; ++i) {
        world_->plant.position[a][i] = out[a * fixed::capacity + i];
        world_->plant.velocity[a][i] = out[(a + 3) * fixed::capacity + i];
        world_->plant.acceleration[a][i] = out[(a + 6) * fixed::capacity + i];
      }
    ++world_->calls[1];
    ++publications;
    return true;
  }
  bool plan(const rt::MemoryPlan &p) const noexcept override {
    return p.device_backend_count == 1 && p.device_buffer_count == 2 &&
           p.device_timeline_count == 1;
  }
  rt::Status health(rt::DeviceHealth &out) noexcept {
    return runtime_->device_health(handle_, out);
  }
  rt::Status reset() noexcept { return runtime_->reset_device(handle_); }
  bool timeline(rt::DeviceTimelineInfo &out) noexcept {
    return runtime_->device_timeline_at(handle_, 0, out);
  }
};
} // namespace golden::cuda
