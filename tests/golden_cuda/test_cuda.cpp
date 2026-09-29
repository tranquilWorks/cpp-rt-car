#include "../../samples/golden_cuda/owner.hpp"
#include "../../samples/golden_cuda/replay.hpp"
#include "../../samples/golden_system/oracle.hpp"
#include "../cuda_physics/allocation_guard.hpp"
#include <algorithm>
#include <cstddef>
#include <future>
#include <iostream>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::cerr << __func__ << ':' << __LINE__ << " failed: " #x << '\n';      \
      return false;                                                            \
    }                                                                          \
  } while (false)
using namespace golden;
using namespace golden::cuda;
using rt::Status;
namespace allocation = rtfw_physics_allocation;

bool same_semantics(const World &a, const World &b) {
  // Each complete state retains its genuine generation and checksum. Those
  // opaque owner-bound values are checked by that owner's trusted replay.
  for (std::size_t i = 0; i < a.canonical.size(); ++i) {
    if ((i >= 64 && i < 72) || (i >= 480 && i < 488))
      continue;
    if (a.canonical[i] != b.canonical[i])
      return false;
  }
  return bool(a.configuration.generation) == bool(b.configuration.generation);
}
bool counts(Owner &owner, std::uint64_t steps) {
  auto &d = *owner.driver;
  rt::DeviceTimelineInfo t;
  CHECK(owner.physics->timeline(t));
  CHECK(owner.physics->providers == steps &&
        owner.physics->publications == steps);
  CHECK(t.completed_value == steps && t.last_accepted_value == steps);
  CHECK(d.uploads == 2 * steps && d.copies == steps && d.downloads == steps);
  CHECK(d.kernels == (owner.dispatch == Dispatch::kernel ? steps : 0));
  CHECK(d.graphs == (owner.dispatch == Dispatch::graph ? steps : 0));
  CHECK(d.records == steps && d.queries >= steps);
  CHECK(d.registrations == 2 && d.backend_allocations == 0 &&
        d.backend_frees == 0);
  CHECK(d.protocol_ok && d.faults == 0);
  return true;
}
bool close(Owner &owner) {
  CHECK(owner.close() == Status::ok);
  CHECK(owner.memory->acquisitions == owner.memory->releases);
  CHECK(owner.jobs->accepted == owner.jobs->completed);
  if (owner.driver) {
    CHECK(owner.driver->clean() && owner.driver->protocol_ok);
    CHECK(owner.driver->registrations == owner.driver->unregistrations);
    CHECK(owner.driver->event_creates == owner.driver->event_destroys);
  }
  return true;
}
bool parity(Options options) {
  Owner cpu(options, Dispatch::cpu), kernel(options, Dispatch::kernel),
      graph(options, Dispatch::graph);
  std::array owners{&cpu, &kernel, &graph};
  std::array<std::unique_ptr<OwnedReplay>, 3> replays;
  std::array<std::unique_ptr<Oracle>, 3> oracles;
  for (std::size_t i = 0; i < owners.size(); ++i) {
    auto &owner = *owners[i];
    CHECK(owner.prepare() == Status::ok);
    auto &s = *owner.session;
    CHECK(s.plan.phase_count == 8 &&
          s.plan.registered_state_bytes == state_bytes);
    CHECK(s.plan.planned_bytes ==
          s.plan.runtime_control_bytes + s.plan.executor_control_bytes +
              s.plan.device_control_bytes + s.plan.phase_scratch_total_bytes +
              s.plan.task_scratch_total_bytes + s.plan.trace_storage_bytes);
    if (owner.physics) {
      rt::CompiledDeviceRatePhase phase;
      CHECK(s.runtime->compiled_device_rate_phase_at(0, phase));
      CHECK(phase.simulation &&
            phase.simulation->host_watchdog_ns == 5'000'000'000ull);
      CHECK(phase.completion_budget_ns == completion_ns &&
            phase.maximum_in_flight == 1);
    }
    replays[i] = std::make_unique<OwnedReplay>(options.ticks);
    oracles[i] = std::make_unique<Oracle>(options);
    CHECK(replays[i]->begin(s) && s.controls());
  }
  for (std::size_t tick = 0; tick < options.ticks; ++tick) {
    for (std::size_t i = 0; i < owners.size(); ++i) {
      auto &s = *owners[i]->session;
      replays[i]->record(s, tick);
      allocation::begin();
      const auto status = s.step(tick);
      const auto allocations = allocation::end();
      if (status != Status::ok)
        std::cerr << s.runtime->last_error() << '\n';
      CHECK(status == Status::ok && allocations == 0);
      CHECK(oracles[i]->step(tick, s.world));
    }
    CHECK(same_semantics(cpu.session->world, kernel.session->world));
    CHECK(same_semantics(cpu.session->world, graph.session->world));
  }
  for (std::size_t i = 0; i < owners.size(); ++i) {
    auto &owner = *owners[i];
    auto &s = *owner.session;
    CHECK(replays[i]->seal(s));
    allocation::begin();
    const bool replayed = replays[i]->verify(s);
    const auto allocations = allocation::end();
    if (!replayed)
      std::cerr << s.runtime->last_error() << '\n';
    CHECK(replayed && allocations == 0);
    if (owner.physics)
      CHECK(counts(owner, 2 * options.ticks));
    auto bad = replays[i]->trusted;
    bad.back() ^= std::byte{1};
    const auto before = s.world.canonical;
    const auto submissions = owner.driver ? owner.driver->records.load() : 0;
    CHECK(OwnedReplay::apply(s, bad) != Status::ok);
    CHECK(s.world.canonical == before);
    CHECK(!owner.driver || owner.driver->records == submissions);
  }
  // CPU and device artifacts carry actual incompatible graph identities.
  const auto cpu_checkpoint = cpu.session->checkpoint(options.ticks - 1);
  const auto before = graph.session->world.canonical;
  const auto submissions = graph.driver->records.load();
  CHECK(graph.session->runtime->restore_checkpoint(cpu_checkpoint) ==
        Status::incompatible_artifact);
  CHECK(graph.session->world.canonical == before &&
        graph.driver->records == submissions);
  for (auto *owner : owners)
    CHECK(close(*owner));
  return true;
}

bool recovery(bool host, Dispatch dispatch, bool lost) {
  Options o;
  o.host = host;
  auto old = std::make_unique<Owner>(o, dispatch);
  CHECK(old->prepare() == Status::ok && old->session->controls());
  Oracle oracle(o);
  for (std::size_t tick = 0; tick < 6; ++tick) {
    CHECK(old->session->step(tick) == Status::ok);
    CHECK(oracle.step(tick, old->session->world));
  }
  auto cp = old->session->checkpoint(5);
  CHECK(!cp.empty());
  const auto plant = old->session->world.plant;
  if (lost)
    old->driver->lose_query = true;
  else
    old->driver->fail_query = true;
  const auto failure = old->session->step(6);
  if (failure != (lost ? Status::device_lost : Status::device_reset_required))
    std::cerr << "fault status=" << int(failure) << ' '
              << old->session->runtime->last_error() << '\n';
  CHECK(failure ==
        (lost ? Status::device_lost : Status::device_reset_required));
  CHECK(old->driver->faults == 1 && old->physics->publications == 6 &&
        old->physics->providers == 7);
  CHECK(old->session->world.calls[1] == 6 && old->session->world.calls[2] == 6);
  CHECK(old->session->world.plant.position == plant.position &&
        old->session->world.plant.velocity == plant.velocity &&
        old->session->world.plant.acceleration == plant.acceleration);
  rt::DeviceTimelineInfo timeline;
  CHECK(old->physics->timeline(timeline) && timeline.completed_value == 6);
  rt::DeviceHealth before = rt::make_device_health(),
                   after = rt::make_device_health();
  CHECK(old->physics->health(before) == Status::ok && before.outstanding == 1);
  if (lost) {
    CHECK(before.state == RTFW_DEVICE_HEALTH_LOST && before.losses == 1);
    CHECK(old->physics->reset() == Status::device_lost);
  } else {
    CHECK(before.state == RTFW_DEVICE_HEALTH_RESET_REQUIRED);
    old->driver->fail_stream_sync = true;
    CHECK(old->physics->reset() != Status::ok && old->driver->faults == 2);
    CHECK(!old->driver->clean() && old->memory->live_count() == 3);
    CHECK(old->physics->reset() == Status::ok);
    CHECK(old->physics->health(after) == Status::ok &&
          after.state == RTFW_DEVICE_HEALTH_HEALTHY);
    CHECK(after.resets == before.resets + 1 &&
          after.generation > before.generation && after.outstanding == 0);
  }
  // Reset may already have retired stream work. Fail an actual borrowed-buffer
  // unregister to exercise checked teardown retention after either reset
  // result.
  old->driver->fail_unregister = true;
  CHECK(old->close() != Status::ok && old->session && old->physics);
  CHECK(!old->driver->clean() && old->memory->live_count() == 3);
  CHECK(close(*old));
  old.reset();
  Owner resumed(o, dispatch);
  CHECK(resumed.prepare() == Status::ok);
  CHECK(resumed.session->runtime->restore_checkpoint(cp) == Status::ok &&
        resumed.session->world.decode());
  OwnedReplay replay(o.ticks, false, 6);
  replay.initial = resumed.session->checkpoint(5);
  CHECK(!replay.initial.empty());
  for (std::size_t tick = 6; tick < o.ticks; ++tick) {
    replay.record(*resumed.session, tick);
    CHECK(resumed.session->step(tick) == Status::ok &&
          oracle.step(tick, resumed.session->world));
  }
  CHECK(replay.seal(*resumed.session) && replay.verify(*resumed.session));
  CHECK(counts(resumed, 2 * (o.ticks - 6)));
  CHECK(close(resumed));
  return true;
}
bool negatives(Dispatch dispatch) {
  for (unsigned fault = 0; fault < 5; ++fault) {
    Owner owner(Options{}, dispatch);
    CHECK(owner.prepare() == Status::ok);
    if (fault == 0)
      owner.driver->delayed_queries = 8;
    if (fault == 1)
      owner.driver->hold = true;
    if (fault == 2)
      owner.driver->corrupt_output = true;
    if (fault == 3)
      owner.driver->fail_kernel = true;
    const auto status = owner.session->step(0);
    if (fault == 0 || fault == 4) {
      CHECK(status == Status::ok && counts(owner, 1));
      if (fault == 0)
        CHECK(owner.driver->not_ready == 8);
      if (fault == 4) {
        owner.driver->fail_unregister = true;
        CHECK(owner.close() != Status::ok && !owner.driver->clean());
      }
    } else {
      const auto expected = fault == 1   ? Status::device_timeout
                            : fault == 2 ? Status::callback_failed
                                         : Status::device_reset_required;
      if (status != expected)
        std::cerr << "negative=" << fault << " status=" << int(status) << ' '
                  << owner.session->runtime->last_error() << '\n';
      CHECK(status == expected && owner.physics->publications == 0 &&
            owner.session->world.calls[2] == 0);
      rt::DeviceTimelineInfo t;
      CHECK(owner.physics->timeline(t));
      CHECK(t.completed_value ==
            (fault == 2 ? 1u : 0u)); // Corrupt output is rejected after genuine
                                     // device success.
      owner.driver->hold = false;
      if (fault == 3)
        CHECK(owner.physics->reset() == Status::ok);
    }
    CHECK(close(owner));
  }
  Owner cleanup(Options{}, dispatch);
  CHECK(cleanup.prepare() == Status::ok &&
        cleanup.session->step(0) == Status::ok);
  cleanup.driver->fail_destroy = true;
  CHECK(cleanup.close() != Status::ok && !cleanup.driver->clean());
  CHECK(close(cleanup));
  return true;
}
bool independent_owners() {
  Options o;
  o.workers = 2;
  Owner first(o, Dispatch::kernel), second(o, Dispatch::graph);
  CHECK(first.prepare() == Status::ok && second.prepare() == Status::ok);
  CHECK(first.session->controls() && second.session->controls());
  auto run = [&](Owner &owner) {
    Oracle oracle(o);
    for (std::size_t tick = 0; tick < o.ticks; ++tick)
      if (owner.session->step(tick) != Status::ok ||
          !oracle.step(tick, owner.session->world))
        return false;
    return true;
  };
  auto a = std::async(std::launch::async, [&] { return run(first); });
  auto b = std::async(std::launch::async, [&] { return run(second); });
  CHECK(a.get() && b.get());
  CHECK(counts(first, o.ticks) && counts(second, o.ticks));
  CHECK(same_semantics(first.session->world, second.session->world));
  CHECK(close(first) && close(second));
  return true;
}

// Deliberately ask the unchanged native backend for a replay-enabled Session.
// This test-only wrapper forwards every other operation without changing caps.
struct ReplayRequest final : Physics {
  CudaPhysics native;
  explicit ReplayRequest(Resources resources) : native(resources, false) {}
  void limits(rt::RuntimeConfig &c) const noexcept override {
    native.limits(c);
  }
  bool replay_enabled() const noexcept override { return true; }
  Status configure(rt::Runtime &r, World &w) noexcept override {
    return native.configure(r, w);
  }
  Status register_phase(rt::Runtime &r, rt::RateDomainHandle d,
                        rt::PhaseHandle &p) noexcept override {
    return native.register_phase(r, d, p);
  }
  bool input(const rt::CallbackContext &c) noexcept override {
    return native.input(c);
  }
  bool complete(const rt::CallbackContext &c) noexcept override {
    return native.complete(c);
  }
  bool plan(const rt::MemoryPlan &p) const noexcept override {
    return native.plan(p);
  }
};
bool native_contract() {
  {
    SimulatedDriver driver(16);
    const auto resources = driver.resources();
    Owner native(Options{}, Dispatch::kernel, &resources);
    CHECK(native.prepare() == Status::ok && !native.physics->replay_enabled());
    rt::CompiledDeviceRatePhase p;
    CHECK(native.session->runtime->compiled_device_rate_phase_at(0, p) &&
          !p.simulation);
    // This is only native capability/configuration evidence with an injected
    // Driver API, never a physical CUDA execution/replay claim.
    CHECK(driver.records == 0);
    CHECK(close(native) && driver.clean());
  }
  {
    SimulatedDriver driver(16);
    ReplayRequest physics(driver.resources());
    Memory memory;
    Session session(Options{}, nullptr, memory, &physics);
    const auto status = session.prepare();
    std::cout << "native replay rejected status=" << int(status) << ' '
              << session.runtime->last_error() << '\n';
    CHECK(status == Status::invalid_config && driver.records == 0 &&
          physics.native.providers == 0);
    CHECK(session.close() == Status::ok && driver.clean() &&
          memory.acquisitions == memory.releases);
  }
  for (unsigned malformed = 0; malformed < 4; ++malformed) {
    SimulatedDriver driver(16);
    auto resources = driver.resources();
    if (malformed == 0)
      resources.stream = 0;
    if (malformed == 1)
      resources.addresses[1] = resources.addresses[0];
    if (malformed == 2)
      resources.addresses[1] = UINT64_MAX;
    if (malformed == 3)
      resources.graph = 0;
    Owner owner(Options{}, Dispatch::graph, &resources);
    CHECK(owner.prepare() == Status::invalid_argument && driver.records == 0);
    CHECK(close(owner) && driver.clean());
  }
  return true;
}
bool foreign_artifacts() {
  Options options;
  Owner first(options, Dispatch::kernel), same(options, Dispatch::kernel),
      other(options, Dispatch::graph);
  CHECK(first.prepare() == Status::ok && same.prepare() == Status::ok &&
        other.prepare() == Status::ok);
  OwnedReplay replay(options.ticks);
  CHECK(replay.begin(*first.session) && first.session->controls());
  for (std::size_t tick = 0; tick < options.ticks; ++tick) {
    replay.record(*first.session, tick);
    CHECK(first.session->step(tick) == Status::ok);
  }
  CHECK(replay.seal(*first.session));
  for (auto *owner : {&same, &other}) {
    const auto before = owner->session->world.canonical;
    const auto rejected = OwnedReplay::apply(*owner->session, replay.trusted);
    std::cout << "foreign rejected=" << int(rejected) << ' '
              << owner->session->runtime->last_error() << '\n';
    CHECK(rejected == Status::incompatible_artifact);
    CHECK(owner->session->world.canonical == before &&
          owner->driver->records == 0);
  }
  const auto before = other.session->world.canonical;
  CHECK(other.session->runtime->restore_checkpoint(replay.initial) ==
        Status::incompatible_artifact);
  CHECK(other.session->world.canonical == before && other.driver->records == 0);
  CHECK(close(first) && close(same) && close(other));
  return true;
}

bool partial_startup() {
  for (unsigned failure = 0; failure < 3; ++failure) {
    Owner owner(Options{}, Dispatch::graph);
    if (failure == 0)
      owner.driver->fail_event_create = true;
    else
      owner.driver->fail_registration_at = failure;
    const auto status = owner.prepare();
    CHECK(status != Status::ok && owner.driver->faults == 1 &&
          owner.driver->records == 0);
    CHECK(close(owner));
  }
  // A failed owner's outstanding work does not alter another owner's timeline.
  Owner failed(Options{}, Dispatch::kernel),
      healthy(Options{}, Dispatch::graph);
  CHECK(failed.prepare() == Status::ok && healthy.prepare() == Status::ok);
  failed.driver->lose_query = true;
  CHECK(failed.session->step(0) == Status::device_lost);
  CHECK(healthy.session->step(0) == Status::ok && counts(healthy, 1));
  CHECK(close(failed) && close(healthy));
  return true;
}

int main() {
  if (!native_contract() || !foreign_artifacts() || !partial_startup())
    return 1;
  for (auto dispatch : {Dispatch::kernel, Dispatch::graph}) {
    if (!negatives(dispatch))
      return 1;
    for (bool host : {false, true})
      for (bool lost : {false, true})
        if (!recovery(host, dispatch, lost))
          return 1;
  }
  if (!independent_owners())
    return 1;
  for (bool host : {false, true}) {
    for (std::size_t i = 0; i < 4; ++i) {
      Options o;
      o.host = host;
      o.count = std::array<std::size_t, 4>{1, 17, 16, 256}[i];
      o.workers = static_cast<std::uint32_t>(i % 3 + 1);
      o.grain = std::array<std::size_t, 4>{1, 4, 16, 64}[i];
      if (!parity(o))
        return 1;
    }
    for (auto campaign :
         {Campaign::control_rejected, Campaign::control_replaced,
          Campaign::stale_input, Campaign::peer_missing}) {
      Options o;
      o.host = host;
      o.campaign = campaign;
      if (!parity(o))
        return 1;
    }
    Options max;
    max.host = host;
    max.count = 256;
    max.ticks = 1024;
    max.workers = 3;
    max.grain = 64;
    if (!parity(max))
      return 1;
  }
  std::cout << "PASS CPU/kernel/Graph semantic parity, exact oracles, "
               "counters, zero allocation and trusted replay\n";
}
