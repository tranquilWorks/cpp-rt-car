#pragma once
#include "simulated_driver.hpp"
#include <memory>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
namespace golden::xdma {
class Io {
  struct alignas(64) HostLane {
    Frame input{}, output{};
    std::array<std::byte, 4> ack{};
  };
  std::array<HostLane, 2> host_{};
  SimulatedDriver *simulation_;
  std::unique_ptr<rt::XdmaDeviceBackend> backend_;
  rt::HalV2BackendApi original_{};
  rt::Runtime *runtime_ = nullptr;
  World *world_ = nullptr;
  rt::DeviceBackendHandle handle_{};
  std::array<rt::DeviceCommandBatch, 2> batches_{};
  std::array<rt::DeviceTimelineHandle, 2> timelines_{};
  std::array<std::uint64_t, 2> expected_{};
  rt::HalV2BackendRegistration
  simulated(rt::HalV2BackendRegistration r) noexcept {
    original_ = r.api;
    r.name = "golden.simulated.xdma";
    r.api.instance = this;
    r.api.get_capabilities = [](void *p, rt::HalV2Capabilities *c) {
      auto &s = *static_cast<Io *>(p);
      const auto status = s.original_.get_capabilities(s.original_.instance, c);
      if (status == rt::HalV2Status::ok) {
        if (!s.simulation_)
          return rt::HalV2Status::invalid_argument;
        c->deterministic_mock = 1;
        c->backend_id.fill(0);
        constexpr char name[] = "golden.sim.xdma.v1";
        std::copy_n(name, sizeof(name), c->backend_id.begin());
        c->control_storage_bytes += sizeof(rt::HalV2BackendApi);
      }
      return status;
    };
    r.api.initialize = [](void *p, const rt::HalV2InitializeConfig *v) {
      auto &s = *static_cast<Io *>(p);
      return s.original_.initialize(s.original_.instance, v);
    };
    r.api.register_buffer = [](void *p, const rt::HalV2BufferRegistration *v,
                               std::uint64_t *out) {
      auto &s = *static_cast<Io *>(p);
      return s.original_.register_buffer(s.original_.instance, v, out);
    };
    r.api.unregister_buffer = [](void *p, std::uint64_t v) {
      auto &s = *static_cast<Io *>(p);
      return s.original_.unregister_buffer(s.original_.instance, v);
    };
    r.api.submit = [](void *p, const rt::HalV2Submission *v) {
      auto &s = *static_cast<Io *>(p);
      return s.original_.submit(s.original_.instance, v);
    };
    r.api.poll = [](void *p, rt::HalV2Completion *v, std::uint64_t n,
                    std::uint64_t *out) {
      auto &s = *static_cast<Io *>(p);
      return s.original_.poll(s.original_.instance, v, n, out);
    };
    r.api.cancel = [](void *p, std::uint64_t v) {
      auto &s = *static_cast<Io *>(p);
      return s.original_.cancel(s.original_.instance, v);
    };
    r.api.get_health = [](void *p, rt::HalV2Health *v) {
      auto &s = *static_cast<Io *>(p);
      return s.original_.get_health(s.original_.instance, v);
    };
    r.api.reset = [](void *p) {
      auto &s = *static_cast<Io *>(p);
      return s.original_.reset(s.original_.instance);
    };
    r.api.shutdown = [](void *p) {
      auto &s = *static_cast<Io *>(p);
      return s.original_.shutdown(s.original_.instance);
    };
    // Memory and command extensions retain their original backend instances.
    return r;
  }

  static rt::CallbackResult provide(void *p,
                                    const rt::DeviceCallbackContext &ctx,
                                    rt::DeviceCommandBatch &batch) noexcept {
    auto &s = *static_cast<Io *>(p);
    if (!ctx.rate_release || !s.world_->configuration.consume(ctx.live_control))
      return rt::CallbackResult::error;
    const auto phase = ctx.rate_release->phase.index();
    if (phase != 3 && phase != 5)
      return rt::CallbackResult::error;
    const auto lane = (phase - 3) / 2;
    if (!s.read(*ctx.rate_release, lane * 2))
      return rt::CallbackResult::error;
    rt::DeviceTimelineInfo info;
    if (!s.runtime_->device_timeline_at(s.handle_, lane, info) ||
        info.last_accepted_value == UINT64_MAX)
      return rt::CallbackResult::error;
    batch = s.batches_[lane];
    batch.timeout_ns = completion_ns;
    s.expected_[lane] = info.last_accepted_value + 1;
    batch.signals[0].value = s.expected_[lane];
    if (!rt::set_xdma_control_write(
            batch.commands[2], static_cast<std::uint32_t>(lane * 4),
            std::bit_cast<std::uint32_t>(s.world_->configuration.calibration)))
      return rt::CallbackResult::error;
    ++s.world_->calls[phase];
    ++s.providers[lane];
    return rt::CallbackResult::ok;
  }

public:
  std::array<std::uint64_t, 2> providers{}, publications{};
  explicit Io(SimulatedDriver &driver) : simulation_(&driver) {}
  static void limits(rt::RuntimeConfig &c, bool combined) noexcept {
    c.device_backend_capacity = combined ? 2 : 1;
    c.device_buffer_capacity = combined ? 8 : 6;
    c.device_outstanding_capacity = combined ? 3 : 2;
    c.device_completion_batch = combined ? 3 : 2;
  }
  rt::Status configure(rt::Runtime &r, World &w) noexcept {
    runtime_ = &r;
    world_ = &w;
    rt::XdmaBackendConfig c;
    c.queue_capacity = 2;
    c.buffer_capacity = 6;
    c.worker_count = 2;
    c.h2c_channel_count = c.c2h_channel_count = 2;
    c.max_transfer_bytes = c.max_buffer_bytes = maximum_frame_bytes;
    c.transfer_alignment = 1;
    c.control_aperture_bytes = 8;
    c.user_event_count = 2;
    try {
      backend_ = std::make_unique<rt::XdmaDeviceBackend>(simulation_->api(), c);
    } catch (...) {
      return rt::Status::resource_exhausted;
    }
    auto s = r.register_device_backend(
        simulated(backend_->hal_v2_registration("golden.native.xdma")),
        handle_);
    if (s != rt::Status::ok)
      return s;
    rt::DeviceMemoryDomainHandle memory;
    rt::HalV2MemoryDomain descriptor;
    if (!r.device_memory_domain_at(handle_, 0, memory, descriptor))
      return rt::Status::device_error;
    constexpr std::array<std::string_view, 6> names{
        "sensor.in",   "sensor.out",   "sensor.ack",
        "actuator.in", "actuator.out", "actuator.ack"};
    constexpr std::array<std::string_view, 2> timeline_names{
        "sensor.timeline", "actuator.timeline"};
    for (std::size_t lane = 0; lane < 2; ++lane) {
      std::array<rt::HalV2BufferReference, 3> refs{};
      std::array<std::span<std::byte>, 3> storage{
          frame_span(host_[lane].input, lane * 2),
          frame_span(host_[lane].output, lane * 2 + 1), host_[lane].ack};
      for (std::size_t i = 0; i < 3; ++i) {
        rt::DeviceBufferHandle buffer;
        s = r.register_device_buffer({names[lane * 3 + i],
                                      handle_,
                                      memory,
                                      storage[i],
                                      {},
                                      storage[i].size(),
                                      rt::HalV2MemoryOwnership::borrowed_host,
                                      RTFW_DEVICE_BUFFER_HOST_READ |
                                          RTFW_DEVICE_BUFFER_HOST_WRITE |
                                          RTFW_DEVICE_BUFFER_DEVICE_READ |
                                          RTFW_DEVICE_BUFFER_DEVICE_WRITE,
                                      rt::HalV2MemoryCoherency::host_coherent,
                                      rt::hal_v2_memory_sync_none},
                                     buffer);
        if (s != rt::Status::ok)
          return s;
        refs[i].buffer_token = buffer.value;
        refs[i].bytes = storage[i].size();
      }
      s = r.register_device_timeline({timeline_names[lane], handle_, 0},
                                     timelines_[lane]);
      if (s != rt::Status::ok)
        return s;
      auto &b = batches_[lane];
      b.command_count = 5;
      b.signal_count = 1;
      b.signals[0].timeline_handle = timelines_[lane].value;
      const auto transfer = [&](std::size_t at, std::size_t ref, bool down) {
        auto &cmd = b.commands[at];
        cmd.kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
        cmd.opcode = down ? rt::xdma_device_opcode_card_to_host
                          : rt::xdma_device_opcode_host_to_card;
        cmd.buffer_count = 1;
        cmd.buffers[0] = refs[ref];
        cmd.buffers[0].access =
            down ? RTFW_DEVICE_ACCESS_WRITE : RTFW_DEVICE_ACCESS_READ;
        rt::XdmaTransfer t;
        t.device_offset = ref ? 8192 : 0;
        t.channel = static_cast<std::uint32_t>(lane);
        cmd.payload_size = sizeof(t);
        std::memcpy(cmd.payload.data(), &t, sizeof(t));
      };
      transfer(0, 0, false);
      transfer(1, 1, false);
      transfer(4, 1, true);
      if (!rt::set_xdma_control_write(b.commands[2],
                                      static_cast<std::uint32_t>(lane * 4), 0))
        return rt::Status::internal_error;
      refs[2].access = RTFW_DEVICE_ACCESS_WRITE;
      if (!rt::set_xdma_user_event_wait(
              b.commands[3], static_cast<std::uint32_t>(lane), refs[2]))
        return rt::Status::internal_error;
    }
    return rt::Status::ok;
  }
  rt::Status register_phase(rt::Runtime &r, std::size_t phase,
                            rt::RateDomainHandle domain,
                            rt::PhaseHandle &out) noexcept {
    const auto lane = (phase - 3) / 2;
    auto s = r.register_device_batch_phase(
        {fixed::phase_names[phase], handle_, provide, this, batches_[lane]},
        out);
    if (s != rt::Status::ok)
      return s;
    using R = rt::DeviceRatePayloadRole;
    const std::array roles{R::input, R::input, R::output, R::output};
    rt::DeviceRatePhaseBinding binding{out, domain, completion_ns, 1, roles};
    binding.simulation = rt::DeviceRateSimulationPolicy{5'000'000'000ull};
    return r.bind_device_phase_to_rate_domain(binding);
  }
  bool read(const rt::RateReleaseView &release, std::size_t c) noexcept {
    Frame sampled{};
    rt::CrossRateReadResult result;
    if (release.copy(world_->channels[c], frame_span(sampled, c), result) !=
            rt::CrossRateReadStatus::ok ||
        !application_frame(sampled, world_->buffers[c], c,
                           world_->options.count))
      return false;
    ++world_->selections[c];
    world_->ages[c] = result.age_ns;
    world_->generations[c] = result.generation;
    world_->producer_releases[c] = result.producer_release_sequence;
    world_->timestamps[c] = get(world_->buffers[c], 48, 8);
    return result.producer_completion_status == rt::Status::ok;
  }
  bool publish(const rt::RateReleaseView &release, std::size_t c) noexcept {
    Frame sampled = world_->buffers[c];
    const auto generation = release.domain_release_sequence + 1;
    sampled_header(sampled, c, generation + 1, generation,
                   release.nominal_release_ns,
                   rt::SampledIoFrameStatus::produced);
    if (release.publish(world_->channels[c], frame_span(sampled, c)) !=
        rt::Status::ok)
      return false;
    seal(world_->buffers[c], c, release.logical_release_ns / fixed::tick_ns,
         ++world_->publications[c]);
    return true;
  }
  bool complete(std::size_t lane) noexcept {
    rt::DeviceTimelineInfo info;
    if (!runtime_->device_timeline_at(handle_, lane, info) ||
        !expected_[lane] || info.completed_value != expected_[lane])
      return false;
    const auto c = lane * 2 + 1;
    if (!application_frame(host_[lane].output, world_->buffers[c], c,
                           world_->options.count))
      return false;
    if (!lane) {
      unpack_vector(world_->buffers[c], world_->sensor.position);
      unpack_vector(world_->buffers[c], world_->sensor.velocity, 3);
    }
    ++world_->publications[c];
    ++publications[lane];
    return true;
  }
  void time(std::uint64_t value) noexcept { simulation_->now.store(value); }
};
} // namespace golden::xdma

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
