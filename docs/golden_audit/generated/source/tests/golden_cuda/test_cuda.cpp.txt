#include "../../samples/golden_cuda/owner.hpp"
#include "../../samples/golden_cuda/replay.hpp"
#include "../../samples/golden_system/oracle.hpp"
#include "allocation.hpp"
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
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
bool parity(Options options, int selected_dispatch = -1,
            const std::filesystem::path &reference = {}) {
  std::cout << "parity host=" << options.host << " count=" << options.count
            << " ticks=" << options.ticks << " workers=" << options.workers
            << " grain=" << options.grain << std::endl;
  // Keep exact CPU bytes from every tick, then compare both device paths.
  // Only the owner under test runs workers; concurrency has a separate test.
  std::vector<StateBytes> cpu_states(options.ticks);
  std::vector<std::byte> cpu_checkpoint;
  World expected(options);
  if (selected_dispatch > 0) {
    std::ifstream states(reference.string() + ".states",
                         std::ios::binary | std::ios::ate);
    CHECK(states &&
          states.tellg() == static_cast<std::streamoff>(cpu_states.size() *
                                                        sizeof(StateBytes)));
    states.seekg(0);
    states.read(
        reinterpret_cast<char *>(cpu_states.data()),
        static_cast<std::streamsize>(cpu_states.size() * sizeof(StateBytes)));
    CHECK(bool(states));
    std::ifstream checkpoint(reference.string() + ".checkpoint",
                             std::ios::binary | std::ios::ate);
    CHECK(checkpoint && checkpoint.tellg() > 0 &&
          checkpoint.tellg() <=
              static_cast<std::streamoff>(fixed::checkpoint_bytes));
    cpu_checkpoint.resize(static_cast<std::size_t>(checkpoint.tellg()));
    checkpoint.seekg(0);
    checkpoint.read(reinterpret_cast<char *>(cpu_checkpoint.data()),
                    static_cast<std::streamsize>(cpu_checkpoint.size()));
    CHECK(bool(checkpoint));
  }
  for (auto dispatch : {Dispatch::cpu, Dispatch::kernel, Dispatch::graph}) {
    if (selected_dispatch >= 0 && int(dispatch) != selected_dispatch)
      continue;
    std::cout << "dispatch=" << int(dispatch) << " begin" << std::endl;
    Owner owner(options, dispatch);
    auto policy = Memory::policy();
    policy.thread_policy_count = 1;
    policy.thread_policies[0].role = rt::thread_role_executor_worker;
    policy.thread_policies[0].policy.wait_strategy = rt::WaitStrategy::park;
    CHECK(owner.prepare(&policy) == Status::ok);
    rt::CpuMemoryPolicyReport report;
    CHECK(owner.session->runtime->cpu_memory_policy_report(report));
    bool native_policy = false;
    if (!options.host) {
      for (std::size_t i = 0; i < report.thread_count; ++i) {
        const auto &row = report.threads[i];
        if (row.role != rt::thread_role_executor_worker)
          continue;
        native_policy = true;
        CHECK(row.requested.wait_strategy == rt::WaitStrategy::park);
#if defined(__linux__)
        CHECK(row.resolved.wait_strategy == rt::WaitStrategy::park);
#else
        CHECK(row.resolution ==
              rt::PolicyResolutionState::unsupported_best_effort);
        CHECK(row.applied == rt::PolicyOperationState::unsupported &&
              row.verified == rt::PolicyOperationState::unsupported);
        CHECK(row.resolved.wait_strategy != rt::WaitStrategy::park);
#endif
      }
      CHECK(native_policy);
    }
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
    OwnedReplay replay(options.ticks);
    Oracle oracle(options);
    CHECK(replay.begin(s) && s.controls());
    for (std::size_t tick = 0; tick < options.ticks; ++tick) {
      replay.record(s, tick);
      allocation::begin();
      const auto status = s.step(tick);
      const auto allocations = allocation::end();
      if (status != Status::ok)
        std::cerr << s.runtime->last_error() << '\n';
      CHECK(status == Status::ok && allocations == 0);
      CHECK(oracle.step(tick, s.world));
      if (dispatch == Dispatch::cpu)
        cpu_states[tick] = s.world.canonical;
      else {
        expected.canonical = cpu_states[tick];
        CHECK(expected.decode() && same_semantics(expected, s.world));
      }
    }
    std::cout << "dispatch=" << int(dispatch) << " steps complete" << std::endl;
    CHECK(replay.seal(s));
    allocation::begin();
    const bool replayed = replay.verify(s);
    const auto allocations = allocation::end();
    if (!replayed)
      std::cerr << s.runtime->last_error() << '\n';
    CHECK(replayed && allocations == 0);
    std::cout << "dispatch=" << int(dispatch) << " replay complete"
              << std::endl;
    if (owner.physics)
      CHECK(counts(owner, 2 * options.ticks));
    auto bad = replay.trusted;
    bad.back() ^= std::byte{1};
    const auto before = s.world.canonical;
    const auto submissions = owner.driver ? owner.driver->records.load() : 0;
    CHECK(OwnedReplay::apply(s, bad) != Status::ok);
    CHECK(s.world.canonical == before);
    CHECK(!owner.driver || owner.driver->records == submissions);
    if (dispatch == Dispatch::cpu)
      cpu_checkpoint = s.checkpoint(options.ticks - 1);
    else {
      CHECK(s.runtime->restore_checkpoint(cpu_checkpoint) ==
            Status::incompatible_artifact);
      CHECK(s.world.canonical == before &&
            owner.driver->records == submissions);
    }
    CHECK(close(owner));
  }
  if (selected_dispatch == 0) {
    std::ofstream states(reference.string() + ".states", std::ios::binary);
    states.write(
        reinterpret_cast<const char *>(cpu_states.data()),
        static_cast<std::streamsize>(cpu_states.size() * sizeof(StateBytes)));
    states.close();
    CHECK(!states.fail());
    std::ofstream checkpoint(reference.string() + ".checkpoint",
                             std::ios::binary);
    checkpoint.write(reinterpret_cast<const char *>(cpu_checkpoint.data()),
                     static_cast<std::streamsize>(cpu_checkpoint.size()));
    checkpoint.close();
    CHECK(!checkpoint.fail());
  }
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
    auto memory = std::make_unique<Memory>();
    Session session(Options{}, nullptr, *memory, &physics);
    const auto status = session.prepare();
    std::cout << "native replay rejected status=" << int(status) << ' '
              << session.runtime->last_error() << '\n';
    CHECK(status == Status::invalid_config && driver.records == 0 &&
          physics.native.providers == 0);
    CHECK(session.close() == Status::ok && driver.clean() &&
          memory->acquisitions == memory->releases);
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

bool allocation_control() {
  allocation::begin();
  auto *ordinary = ::operator new(17);
  *static_cast<volatile unsigned char *>(ordinary) = 17;
  const auto ordinary_count = allocation::end();
  ::operator delete(ordinary);
  CHECK(ordinary_count == 1);
  allocation::begin();
  auto *aligned = ::operator new (64, std::align_val_t{64});
  *static_cast<volatile unsigned char *>(aligned) = 64;
  const auto aligned_count = allocation::end();
  ::operator delete (aligned, std::align_val_t{64});
  CHECK(aligned_count == 1);
  return true;
}

int main(int argc, char **argv) {
  std::cout << std::unitbuf;
  int selected = -1, selected_dispatch = -1;
  std::filesystem::path reference;
  if (argc != 1) {
    if ((argc != 3 && argc != 7) || std::string_view(argv[1]) != "--case")
      return 64;
    const std::string_view value = argv[2];
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), selected);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        selected < 0 || selected > 18)
      return 64;
  }
  if (argc == 7) {
    if ((selected != 9 && selected != 18) ||
        std::string_view(argv[3]) != "--dispatch" ||
        std::string_view(argv[5]) != "--reference")
      return 64;
    const std::string_view dispatch = argv[4];
    selected_dispatch = dispatch == "cpu"      ? 0
                        : dispatch == "kernel" ? 1
                        : dispatch == "graph"  ? 2
                                               : -1;
    if (selected_dispatch < 0 || !*argv[6])
      return 64;
    reference = argv[6];
  }
  if (selected <= 0) {
    if (!allocation_control() || !native_contract() || !foreign_artifacts() ||
        !partial_startup())
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
  }
  int case_number = 0;
  const auto run_case = [&](Options o) {
    ++case_number;
    return selected >= 0 && selected != case_number
               ? true
               : parity(o, selected_dispatch, reference);
  };
  for (bool host : {false, true}) {
    for (std::size_t i = 0; i < 4; ++i) {
      Options o;
      o.host = host;
      o.count = std::array<std::size_t, 4>{1, 17, 16, 256}[i];
      o.workers = static_cast<std::uint32_t>(i % 3 + 1);
      o.grain = std::array<std::size_t, 4>{1, 4, 16, 64}[i];
      if (!run_case(o))
        return 1;
    }
    for (auto campaign :
         {Campaign::control_rejected, Campaign::control_replaced,
          Campaign::stale_input, Campaign::peer_missing}) {
      Options o;
      o.host = host;
      o.campaign = campaign;
      if (!run_case(o))
        return 1;
    }
    Options max;
    max.host = host;
    max.count = 256;
    max.ticks = 1024;
    max.workers = 3;
    max.grain = 64;
    if (!run_case(max))
      return 1;
  }
  std::cout << "PASS requested golden CUDA test cases\n";
}
