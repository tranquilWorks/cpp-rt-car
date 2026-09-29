#pragma once
// Independent public-API fixture for M26-03R. The HAL table follows the
// existing command-batch contract; no private Runtime access or prior test
// mutation.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <rt/runtime.hpp>
#include <thread>
#include <vector>

namespace simulation_test {
inline constexpr std::uint64_t budget = 1'000'000;
inline constexpr std::uint64_t watchdog = 5'000'000'000;
inline constexpr std::uint64_t period = 10'000'000;
struct Clock final : rt::RuntimeClock {
  std::atomic<std::uint64_t> now{1000};
  std::uint64_t now_ns() noexcept override { return now.load(); }
};
struct Backend {
  bool deterministic = true;
  unsigned pending_capacity = 1;
  rt::HalV2Status completion_status = rt::HalV2Status::ok;
  std::atomic<bool> allowed{true}, ready{false}, block_submit{false},
      release_submit{false};
  std::atomic<bool> fail_unregister{false}, fail_shutdown{false};
  std::atomic<unsigned> submitted{0}, polled{0}, canceled{0}, stopped{0},
      registered{0}, unregistered{0};
  std::atomic<std::uint64_t> timeout{0};
  rt::HalV2BatchCompletion completion{};
  static Backend &self(void *p) { return *static_cast<Backend *>(p); }
  rt::HalV2BackendApi core() {
    rt::HalV2BackendApi a;
    a.instance = this;
    a.get_capabilities = [](void *p, rt::HalV2Capabilities *c) {
      *c = {};
      constexpr char id[] = "test.simulation.timing";
      std::copy_n(id, sizeof(id), c->backend_id.begin());
      c->max_in_flight = self(p).pending_capacity;
      c->max_registered_buffers = 1;
      c->max_buffer_bytes = 64;
      c->supports_cancel = 1;
      c->supports_reset = 1;
      c->deterministic_mock = self(p).deterministic;
      return rt::HalV2Status::ok;
    };
    a.initialize = [](void *, const rt::HalV2InitializeConfig *) {
      return rt::HalV2Status::ok;
    };
    a.register_buffer = [](void *p, const rt::HalV2BufferRegistration *,
                           std::uint64_t *t) {
      ++self(p).registered;
      *t = 1;
      return rt::HalV2Status::ok;
    };
    a.unregister_buffer = [](void *p, std::uint64_t) {
      if (self(p).fail_unregister.exchange(false))
        return rt::HalV2Status::error;
      ++self(p).unregistered;
      return rt::HalV2Status::ok;
    };
    a.submit = [](void *, const rt::HalV2Submission *) {
      return rt::HalV2Status::unsupported;
    };
    a.poll = [](void *, rt::HalV2Completion *, std::uint64_t,
                std::uint64_t *n) {
      *n = 0;
      return rt::HalV2Status::ok;
    };
    a.cancel = [](void *, std::uint64_t) {
      return rt::HalV2Status::unsupported;
    };
    a.get_health = [](void *, rt::HalV2Health *h) {
      *h = {};
      h->state = static_cast<std::uint32_t>(rt::HalV2HealthState::healthy);
      return rt::HalV2Status::ok;
    };
    a.reset = [](void *) { return rt::HalV2Status::ok; };
    a.shutdown = [](void *p) {
      return self(p).fail_shutdown.exchange(false) ? rt::HalV2Status::error
                                                   : rt::HalV2Status::ok;
    };
    return a;
  }
  rt::HalV2MemoryTopologyExtension memory() {
    rt::HalV2MemoryTopologyExtension a;
    a.instance = this;
    a.discover = [](void *, rt::HalV2MemoryTopologySnapshot *s) {
      *s = {};
      s->memory_domain_count = 1;
      s->topology_node_count = 1;
      s->timestamp_domain_count = 1;
      s->completion_timestamp_domain_identity = 201;
      s->topology_nodes[0].identity = 101;
      s->topology_nodes[0].kind =
          static_cast<std::uint32_t>(rt::HalV2TopologyNodeKind::host);
      auto &t = s->timestamp_domains[0];
      t.identity = 201;
      t.kind = static_cast<std::uint32_t>(
          rt::HalV2TimestampDomainKind::backend_device);
      t.tick_numerator_ns = t.tick_denominator = 1;
      t.monotonic = 1;
      auto &d = s->memory_domains[0];
      d.identity = 1;
      d.kind = static_cast<std::uint32_t>(rt::HalV2MemoryDomainKind::host);
      d.ownership_modes = rt::hal_v2_memory_ownership_borrowed_host;
      d.maximum_bytes = 64;
      d.byte_granularity = d.alignment = d.offset_granularity = 1;
      d.access = RTFW_DEVICE_BUFFER_HOST_READ | RTFW_DEVICE_BUFFER_HOST_WRITE |
                 RTFW_DEVICE_BUFFER_DEVICE_READ |
                 RTFW_DEVICE_BUFFER_DEVICE_WRITE;
      d.coherency =
          static_cast<std::uint32_t>(rt::HalV2MemoryCoherency::host_coherent);
      d.topology_node_identity = 101;
      d.timestamp_domain_identity = 201;
      return rt::HalV2Status::ok;
    };
    a.register_memory = [](void *p, const rt::HalV2MemoryRegistration *,
                           rt::HalV2MemoryToken *t) {
      ++self(p).registered;
      *t = {};
      t->submission_token = 1;
      t->native_token.size = 1;
      t->native_token.bytes[0] = std::byte{1};
      return rt::HalV2Status::ok;
    };
    a.unregister_memory = [](void *p, const rt::HalV2MemoryRegistration *,
                             const rt::HalV2MemoryToken *) {
      if (self(p).fail_unregister.exchange(false))
        return rt::HalV2Status::error;
      ++self(p).unregistered;
      return rt::HalV2Status::ok;
    };
    a.query_timestamp_correlation =
        [](void *, const rt::HalV2TimestampCorrelationQuery *,
           rt::HalV2TimestampCorrelation *) {
          return rt::HalV2Status::unsupported;
        };
    return a;
  }
  rt::HalV2CommandTimelineExtension commands() {
    rt::HalV2CommandTimelineExtension a;
    a.instance = this;
    a.get_capabilities = [](void *p, rt::HalV2CommandTimelineCapabilities *c) {
      *c = {};
      c->max_in_flight_batches = self(p).pending_capacity;
      c->max_commands_per_batch = 1;
      c->max_wait_points = c->max_signal_points = 1;
      c->max_timelines = self(p).pending_capacity;
      c->completion_batch_capacity = self(p).pending_capacity;
      c->backend_control_storage_bytes = 256;
      return rt::HalV2Status::ok;
    };
    a.submit = [](void *p, const rt::DeviceCommandBatch *b) {
      auto &s = self(p);
      s.timeout = b->timeout_ns;
      s.completion = {};
      s.completion.status = static_cast<std::int32_t>(s.completion_status);
      s.completion.batch_id = b->batch_id;
      s.completion.signal_count = b->signal_count;
      s.completion.signals = b->signals;
      // An explicitly uncorrelated backend timestamp, not Runtime ns.
      s.completion.timestamp_domain_identity = 201;
      s.completion.device_timestamp = 900'000'000'000ull;
      s.ready.store(true, std::memory_order_release);
      ++s.submitted;
      while (s.block_submit.load() && !s.release_submit.load())
        std::this_thread::yield();
      return rt::HalV2Status::ok;
    };
    a.poll = [](void *p, rt::HalV2BatchCompletion *out, std::uint64_t capacity,
                std::uint64_t *n) {
      auto &s = self(p);
      ++s.polled;
      *n = 0;
      if (capacity && s.allowed.load() &&
          s.ready.exchange(false, std::memory_order_acq_rel)) {
        out[0] = s.completion;
        *n = 1;
      }
      return rt::HalV2Status::ok;
    };
    a.cancel = [](void *p, std::uint64_t) {
      ++self(p).canceled;
      return rt::HalV2Status::ok;
    };
    a.request_stop = [](void *p) {
      auto &s = self(p);
      ++s.stopped;
      s.release_submit = true;
      s.ready = false;
      return rt::HalV2Status::ok;
    };
    return a;
  }
};
struct Fixture {
  Clock clock;
  Backend backend;
  rt::Runtime runtime{clock};
  rt::DeviceBackendHandle device{};
  rt::DeviceBufferHandle buffer{};
  rt::DeviceTimelineHandle timeline{}, second_timeline{};
  rt::RateDomainHandle rate{};
  rt::PhaseHandle physics{}, stage{}, second_physics{};
  std::array<std::byte, 16> storage{};
  std::array<std::byte, 8> state{};
  std::array<rt::DeviceRatePayloadRole, 1> roles{
      rt::DeviceRatePayloadRole::input};
  rt::DeviceCommandBatch declaration{}, second_declaration{};
  unsigned publications = 0;
  std::atomic<unsigned> provider_calls{0};
  rt::Status configure(bool closure = false, bool twin = false) {
    rt::RuntimeConfig c;
    c.callback_capacity = twin ? 3 : 2;
    c.worker_count = 1;
    c.executor_queue_capacity = 4;
    c.task_scratch_slots = 4;
    c.trace_capacity = 128;
    c.device_backend_capacity = c.device_buffer_capacity =
        c.device_outstanding_capacity = c.device_completion_batch = 1;
    if (twin) {
      backend.pending_capacity = 2;
      c.device_outstanding_capacity = 2;
      c.device_completion_batch = 2;
    }
    c.snapshot_max_bytes = c.input_log_max_bytes = 65536;
    c.state_capacity = 1;
    c.replay_input_capacity = 16;
#define SIM_TRY(x)                                                             \
  do {                                                                         \
    auto s = (x);                                                              \
    if (s != rt::Status::ok)                                                   \
      return s;                                                                \
  } while (false)
    SIM_TRY(runtime.configure(c));
    SIM_TRY(runtime.set_rate_execution_policy({16}));
    if (closure)
      SIM_TRY(runtime.set_mixed_rate_closure_policy(
          {26,
           256,
           256,
           65536,
           32,
           rt::MixedRateOverflowPolicy::overwrite_committed,
           true,
           true,
           {}}));
    auto m = backend.memory();
    auto commands = backend.commands();
    SIM_TRY(runtime.register_device_backend(
        {"simulation", backend.core(), &m, &commands}, device));
    SIM_TRY(runtime.register_device_buffer(
        {"simulation.buffer", device, storage}, buffer));
    SIM_TRY(runtime.register_device_timeline({"simulation.timeline", device, 0},
                                             timeline));
    if (twin)
      SIM_TRY(runtime.register_device_timeline(
          {"simulation.second.timeline", device, 0}, second_timeline));
    declaration.command_count = declaration.signal_count = 1;
    auto &command = declaration.commands[0];
    command.kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
    command.opcode = 26;
    command.buffer_count = 1;
    command.buffers[0].buffer_token = buffer.value;
    command.buffers[0].bytes = storage.size();
    command.buffers[0].access = RTFW_DEVICE_ACCESS_READ;
    declaration.signals[0].timeline_handle = timeline.value;
    SIM_TRY(runtime.register_device_batch_phase(
        {"simulation.physics", device, provide, this, declaration}, physics));
    if (twin) {
      second_declaration = declaration;
      second_declaration.signals[0].timeline_handle = second_timeline.value;
      SIM_TRY(runtime.register_device_batch_phase(
          {"simulation.second", device, provide, this, second_declaration},
          second_physics));
    }
    SIM_TRY(
        runtime.register_callback({"simulation.stage", publish, this}, stage));
    if (twin)
      SIM_TRY(runtime.add_dependency(second_physics, stage));
    SIM_TRY(runtime.add_dependency(physics, stage));
    SIM_TRY(runtime.register_rate_domain(
        {"simulation.rate", period, 1, period, budget}, rate));
    SIM_TRY(runtime.bind_phase_to_rate_domain(stage, rate));
    SIM_TRY(runtime.register_state({"simulation.state", 1, state}));
#undef SIM_TRY
    return rt::Status::ok;
  }
  rt::DeviceRatePhaseBinding binding() const {
    return {physics, rate, budget, 1, roles};
  }
  static rt::CallbackResult provide(void *p,
                                    const rt::DeviceCallbackContext &context,
                                    rt::DeviceCommandBatch &b) {
    auto &s = *static_cast<Fixture *>(p);
    ++s.provider_calls;
    rt::DeviceTimelineInfo info;
    const bool second =
        context.rate_release && context.rate_release->phase == s.second_physics;
    if (!s.runtime.device_timeline_at(s.device, second ? 1 : 0, info))
      return rt::CallbackResult::error;
    b = second ? s.second_declaration : s.declaration;
    b.timeout_ns = budget;
    b.signals[0].value = info.last_accepted_value + 1;
    return rt::CallbackResult::ok;
  }
  static rt::CallbackResult publish(void *p, const rt::CallbackContext &) {
    auto &s = *static_cast<Fixture *>(p);
    ++s.publications;
    s.state[0] = std::byte(std::to_integer<unsigned>(s.state[0]) + 1);
    return rt::CallbackResult::ok;
  }
  rt::HostFrameContext frame(std::uint64_t tick = 0) const {
    return {tick, std::chrono::nanoseconds(period), std::nullopt,
            1000 + tick * period};
  }
  rt::Status step(std::uint64_t tick = 0) {
    clock.now = 1000 + tick * period;
    return runtime.step(frame(tick));
  }
  std::uint64_t completed() const {
    rt::DeviceTimelineInfo i;
    return runtime.device_timeline_at(device, 0, i) ? i.completed_value
                                                    : UINT64_MAX;
  }
};
} // namespace simulation_test
