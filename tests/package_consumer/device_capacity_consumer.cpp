// Independent public SDK capacity/lifecycle fixture. Fixed SPSC completion slots.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <rt/runtime.hpp>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
namespace device_capacity {
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
  unsigned core_capacity = 2, completion_capacity = 2;
  bool malformed = false;
  std::atomic<bool> excessive_completion{false};
  std::atomic<unsigned> initialized{0}, shutdowns{0}, requested{0}, max_poll{0};
  std::atomic<bool> fail_initialize{false}, fail_stop{false};
  struct Slot {
    std::atomic<bool> occupied{false};
    rt::HalV2BatchCompletion value{};
  };
  std::array<Slot, 4> queue{};
  static Backend &self(void *p) { return *static_cast<Backend *>(p); }
  rt::HalV2BackendApi core() {
    rt::HalV2BackendApi a;
    a.instance = this;
    a.get_capabilities = [](void *p, rt::HalV2Capabilities *c) {
      *c = {};
      constexpr char id[] = "test.device.capacity";
      std::copy_n(id, sizeof(id), c->backend_id.begin());
      c->max_in_flight = self(p).core_capacity;
      c->max_registered_buffers = 1;
      c->max_buffer_bytes = 64;
      c->supports_cancel = 1;
      c->supports_reset = 1;
      c->deterministic_mock = self(p).deterministic;
      return rt::HalV2Status::ok;
    };
    a.initialize = [](void *p, const rt::HalV2InitializeConfig *c) {
      auto& s = self(p);
      ++s.initialized;
      s.requested = static_cast<unsigned>(c->requested_in_flight);
      if (s.fail_initialize.exchange(false)) return rt::HalV2Status::error;
      return c->requested_in_flight <= s.core_capacity &&
                     c->requested_in_flight <= s.pending_capacity
                 ? rt::HalV2Status::ok : rt::HalV2Status::resource_exhausted;
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
    a.reset = [](void *p) {
      for (auto& slot : self(p).queue) slot.occupied = false;
      return rt::HalV2Status::ok;
    };
    a.shutdown = [](void *p) {
      ++self(p).shutdowns;
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
      c->max_timelines = 4;
      c->completion_batch_capacity = self(p).malformed ? 0 : self(p).completion_capacity;
      c->backend_control_storage_bytes = sizeof(Backend);
      return rt::HalV2Status::ok;
    };
    a.submit = [](void *p, const rt::DeviceCommandBatch *b) {
      auto &s = self(p);
      s.timeout = b->timeout_ns;
      Slot* selected = nullptr;
      for (unsigned i = 0; i < s.pending_capacity; ++i)
        if (!s.queue[i].occupied.load(std::memory_order_acquire)) {
          selected = &s.queue[i]; break;
        }
      if (!selected) return rt::HalV2Status::queue_full;
      auto& result = selected->value;
      result = {};
      result.status = static_cast<std::int32_t>(s.completion_status);
      result.batch_id = b->batch_id;
      result.signal_count = b->signal_count;
      result.signals = b->signals;
      result.timestamp_domain_identity = 201;
      result.device_timestamp = 900'000'000'000ull;
      selected->occupied.store(true, std::memory_order_release);
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
      if (capacity > s.completion_capacity) return rt::HalV2Status::invalid_argument;
      s.max_poll = std::max(s.max_poll.load(), static_cast<unsigned>(capacity));
      if (s.excessive_completion.exchange(false)) {
        *n = capacity + 1;
        return rt::HalV2Status::ok;
      }
      if (s.allowed.load()) for (auto& slot : s.queue) {
        if (*n == capacity) break;
        if (slot.occupied.load(std::memory_order_acquire)) {
          out[(*n)++] = slot.value;
          slot.occupied.store(false, std::memory_order_release);
        }
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
      if (s.fail_stop.exchange(false)) return rt::HalV2Status::error;
      s.release_submit = true;
      for (auto& slot : s.queue) slot.occupied = false;
      return rt::HalV2Status::ok;
    };
    return a;
  }
};
struct Fixture {
  Clock clock;
  std::array<Backend, 2> backends;
  rt::Runtime runtime{clock};
  std::array<rt::DeviceBackendHandle, 2> devices{};
  std::array<rt::DeviceTimelineHandle, 3> timelines{};
  std::array<rt::PhaseHandle, 3> phases{};
  std::array<rt::DeviceCommandBatch, 3> declarations{};
  std::array<std::array<std::byte, 16>, 2> storage{};
  std::array<std::byte, 8> state{};
  rt::RateDomainHandle rate{};
  rt::PhaseHandle stage{};
  std::atomic<unsigned> providers{0};
  static constexpr std::array<unsigned, 3> lane{0, 1, 1};
  Fixture() {
    backends[0].pending_capacity = backends[0].core_capacity =
        backends[0].completion_capacity = 1;
    backends[1].pending_capacity = 2;
  }
  rt::Status configure(bool opt_in = true, unsigned aggregate = 3,
                       bool closure = true, unsigned second_phases = 2) {
#define CAP_TRY(x) do { auto result = (x); if (result != rt::Status::ok) return result; } while (false)
    rt::RuntimeConfig cfg;
    cfg.callback_capacity = 4;
    cfg.worker_count = 2;
    cfg.executor_queue_capacity = cfg.task_scratch_slots = 16;
    cfg.trace_capacity = 128;
    cfg.device_backend_capacity = cfg.device_buffer_capacity = 2;
    cfg.device_outstanding_capacity = cfg.device_completion_batch = aggregate;
    cfg.snapshot_max_bytes = cfg.input_log_max_bytes = 131072;
    cfg.replay_input_capacity = 64;
    CAP_TRY(runtime.configure(cfg));
#ifndef RTFW_CAPACITY_BASELINE
    if (opt_in) CAP_TRY(runtime.set_device_capacity_policy(rt::DeviceCapacityPolicy::native_per_backend));
#else
    (void)opt_in;
#endif
    rt::CpuMemoryPolicy cpu;
    cpu.thread_policy_count = 1;
    cpu.thread_policies[0].role = rt::thread_role_executor_worker;
    cpu.thread_policies[0].policy.wait_strategy = rt::WaitStrategy::park;
    CAP_TRY(runtime.set_cpu_memory_policy(cpu));
    CAP_TRY(runtime.set_rate_execution_policy({16}));
    if (closure) CAP_TRY(runtime.set_mixed_rate_closure_policy(
        {2642, 1024, 1024, 131072, 64,
         rt::MixedRateOverflowPolicy::overwrite_committed, true, true, {}}));
    constexpr std::array names{"capacity.one", "capacity.two"};
    for (unsigned i = 0; i < 2; ++i) {
      auto m = backends[i].memory(); auto c = backends[i].commands();
      CAP_TRY(runtime.register_device_backend({names[i], backends[i].core(), &m, &c}, devices[i]));
      rt::DeviceBufferHandle buffer;
      CAP_TRY(runtime.register_device_buffer({names[i], devices[i], storage[i]}, buffer));
      for (unsigned j = 0; j < 3; ++j) if (lane[j] == i) {
        auto& d = declarations[j];
        d.command_count = d.signal_count = 1;
        d.commands[0].kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
        d.commands[0].opcode = 2642;
        d.commands[0].buffer_count = 1;
        d.commands[0].buffers[0] = {buffer.value, RTFW_DEVICE_ACCESS_READ, 0, 0, 16};
      }
    }
    CAP_TRY(runtime.register_rate_domain({"capacity.rate", period, 1, period, budget}, rate));
    CAP_TRY(runtime.register_callback({"capacity.stage", publish, this}, stage));
    CAP_TRY(runtime.bind_phase_to_rate_domain(stage, rate));
    constexpr std::array phase_names{"capacity.first", "capacity.second", "capacity.third"};
    for (unsigned j = 0; j <= second_phases; ++j) {
      CAP_TRY(runtime.register_device_timeline({phase_names[j], devices[lane[j]], 0}, timelines[j]));
      declarations[j].signals[0].timeline_handle = timelines[j].value;
      CAP_TRY(runtime.register_device_batch_phase({phase_names[j], devices[lane[j]], provide, this, declarations[j]}, phases[j]));
      std::array roles{rt::DeviceRatePayloadRole::input};
      rt::DeviceRatePhaseBinding binding{phases[j], rate, budget, 1, roles};
      binding.simulation = rt::DeviceRateSimulationPolicy{watchdog};
      CAP_TRY(runtime.bind_device_phase_to_rate_domain(binding));
      CAP_TRY(runtime.add_dependency(phases[j], stage));
    }
    CAP_TRY(runtime.register_state({"capacity.state", 1, state}));
#undef CAP_TRY
    return rt::Status::ok;
  }
  static rt::CallbackResult provide(void* p, const rt::DeviceCallbackContext& c,
                                     rt::DeviceCommandBatch& batch) {
    auto& f = *static_cast<Fixture*>(p);
    if (!c.rate_release) return rt::CallbackResult::error;
    for (unsigned i = 0; i < 3; ++i) if (phases_equal(f.phases[i], c.rate_release->phase)) {
      rt::DeviceTimelineInfo t;
      if (!f.runtime.device_timeline_at(f.devices[lane[i]], i == 2 ? 1 : 0, t)) return rt::CallbackResult::error;
      batch = f.declarations[i]; batch.timeout_ns = budget;
      batch.signals[0].value = t.last_accepted_value + 1;
      ++f.providers;
      return rt::CallbackResult::ok;
    }
    return rt::CallbackResult::error;
  }
  static bool phases_equal(rt::PhaseHandle a, rt::PhaseHandle b) { return a == b; }
  static rt::CallbackResult publish(void* p, const rt::CallbackContext&) {
    auto& s = static_cast<Fixture*>(p)->state;
    s[0] = std::byte(std::to_integer<unsigned>(s[0]) + 1);
    return rt::CallbackResult::ok;
  }
  rt::HostFrameContext frame(std::uint64_t tick) const {
    return {tick, std::chrono::nanoseconds(period), std::nullopt, 1000 + tick * period};
  }
  rt::Status step(std::uint64_t tick) { clock.now = 1000 + tick * period; return runtime.step(frame(tick)); }
  rt::Status start() { auto s = runtime.finalize(); return s == rt::Status::ok ? runtime.start() : s; }
  std::vector<std::byte> checkpoint() {
    std::size_t n = 0; rt::ArtifactWriteResult out;
    if (runtime.checkpoint_size(n) != rt::Status::ok) return {};
    std::vector<std::byte> bytes(n);
    if (runtime.write_checkpoint(0, bytes, out) != rt::Status::ok) return {};
    bytes.resize(out.bytes_written); return bytes;
  }
  std::vector<std::byte> artifact(std::span<const std::byte> initial, unsigned n) {
    std::array<rt::ReplayInputRecord, 64> inputs{};
    if (n > inputs.size()) return {};
    for (unsigned i = 0; i < n; ++i) inputs[i] = {frame(i), 1, {}};
    std::vector<std::byte> bytes(131072); rt::ArtifactWriteResult out;
    if (runtime.write_active_replay_artifact(initial, std::span(inputs).first(n), bytes, out) != rt::Status::ok) return {};
    bytes.resize(out.bytes_written); return bytes;
  }
  static rt::CallbackResult apply(void*, const rt::ReplayInputView&) { return rt::CallbackResult::ok; }
};
} // namespace device_capacity
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#ifndef RTFW_DEVICE_CAPACITY_NO_MAIN
#include <cstdio>
int main() {
  device_capacity::Fixture f;
  auto s = f.configure(); if (s == rt::Status::ok) s = f.start();
  if (s != rt::Status::ok) { std::fprintf(stderr, "capacity start %d: %.*s\n", static_cast<int>(s), static_cast<int>(f.runtime.last_error().size()), f.runtime.last_error().data()); return 1; }
  rt::MemoryPlan plan;
  if (!f.runtime.memory_plan(plan) || plan.device_batch_queue_slots != 3 || f.backends[0].requested != 1 || f.backends[1].requested != 2) return 2;
  const auto initial = f.checkpoint();
  for (unsigned i = 0; i < 32; ++i) if (f.step(i) != rt::Status::ok) return 3;
  const auto expected = f.state; const auto active = f.artifact(initial, 32);
  if (active.empty() || f.runtime.replay_active(active, decltype(f)::apply) != rt::Status::ok || f.state != expected) return 4;
  if (f.runtime.stop() != rt::Status::ok) return 5;
  std::printf("capacity native1/2 aggregate3 execute32/replay PASS\n"); return 0;
}
#endif
