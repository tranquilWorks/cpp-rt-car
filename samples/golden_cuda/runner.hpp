#pragma once
// Derived from the M26-02 CLI; shared CPU model, controls and replay remain
// authoritative.
#include "../golden_system/oracle.hpp"
#include "../golden_system/peer.hpp"
#include "../golden_system/session.hpp"
#include "owner.hpp"
#include "replay.hpp"
#include "telemetry.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
namespace fs = std::filesystem;
using namespace golden;
namespace golden::cuda {
enum class Fault { none, device_loss, reset_failure };
struct Arguments {
  std::string dispatch = "graph";
  Fault fault = Fault::none;
  Options scenario;
  cil::Options peer;
  fs::path output;
};
bool arguments(int argc, char **argv, Arguments &a) {
  for (int i = 1; i < argc; ++i) {
    std::string_view key = argv[i];
    if (i + 1 >= argc)
      return false;
    std::string_view value = argv[++i];
    std::uint64_t number = 0;
    if (key == "--dispatch") {
      if (value != "cpu" && value != "kernel" && value != "graph")
        return false;
      a.dispatch = value;
    } else if (key == "--fault") {
      if (value == "none")
        a.fault = Fault::none;
      else if (value == "device_loss")
        a.fault = Fault::device_loss;
      else if (value == "reset_failure")
        a.fault = Fault::reset_failure;
      else
        return false;
    } else if (key == "--mode") {
      if (value != "native" && value != "host")
        return false;
      a.scenario.host = value == "host";
    } else if (key == "--output")
      a.output = argv[i];
    else if (key == "--campaign") {
      auto it = std::find(campaign_names.begin(), campaign_names.end(), value);
      if (it == campaign_names.end())
        return false;
      a.scenario.campaign = static_cast<Campaign>(it - campaign_names.begin());
    } else if (key == "--peer") {
      a.scenario.external = true;
      a.peer.key = value;
    } else {
      if (!cil::number(value, number))
        return false;
      if (key == "--count")
        a.scenario.count = number;
      else if (key == "--ticks")
        a.scenario.ticks = number;
      else if (key == "--workers")
        a.scenario.workers = number;
      else if (key == "--grain")
        a.scenario.grain = number;
      else if (key == "--generation")
        a.peer.generation = number;
      else if (key == "--timeout-ms")
        a.peer.timeout_ms = number;
      else
        return false;
    }
  }
  return a.scenario.valid() &&
         (a.fault == Fault::none ||
          (a.dispatch != "cpu" && !a.scenario.external &&
           a.scenario.campaign == Campaign::nominal &&
           a.scenario.ticks >= 19)) &&
         (!a.scenario.external ||
          (cil::valid_key(a.peer.key) && a.peer.generation &&
           a.peer.timeout_ms >= 100 && a.peer.timeout_ms <= 5000)) &&
         !(a.scenario.external && a.scenario.campaign == Campaign::overload);
}
bool write(const fs::path &path, std::span<const std::byte> bytes) {
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char *>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
  f.close();
  return !f.fail();
}
int execute(const Arguments &a) {
  const auto o = a.scenario;
#if defined(_WIN32)
  const auto process_id = GetCurrentProcessId();
#else
  const auto process_id = getpid();
#endif
  const auto session_identity =
      std::to_string(cil::now_ns()) + "-" + std::to_string(process_id);
  const bool device = a.dispatch != "cpu";
  const auto dispatch = a.dispatch == "cpu"      ? Dispatch::cpu
                        : a.dispatch == "kernel" ? Dispatch::kernel
                                                 : Dispatch::graph;
  auto owner = std::make_unique<Owner>(o, dispatch);
  auto *s = owner->session.get();
  auto status = owner->prepare();
  if (status != rt::Status::ok) {
    std::cerr << "prepare " << s->runtime->last_error() << '\n';
    return 1;
  }
  struct Totals {
    std::uint64_t providers = 0, publications = 0, uploads = 0, copies = 0,
                  downloads = 0, kernels = 0, graphs = 0, records = 0,
                  registrations = 0, unregistrations = 0, event_creates = 0,
                  event_destroys = 0, backend_allocations = 0,
                  backend_frees = 0, faults = 0, jobs_accepted = 0,
                  jobs_completed = 0, acquisitions = 0, releases = 0;
    bool protocol_ok = true;
    bool close(Owner &v) {
      if (v.physics) {
        providers += v.physics->providers;
        publications += v.physics->publications;
      }
      if (v.close() != rt::Status::ok)
        return false;
      if (v.driver) {
        auto &d = *v.driver;
#define GOLDEN_SUM(field) field += d.field.load()
        GOLDEN_SUM(uploads);
        GOLDEN_SUM(copies);
        GOLDEN_SUM(downloads);
        GOLDEN_SUM(kernels);
        GOLDEN_SUM(graphs);
        GOLDEN_SUM(records);
        GOLDEN_SUM(registrations);
        GOLDEN_SUM(unregistrations);
        GOLDEN_SUM(event_creates);
        GOLDEN_SUM(event_destroys);
        GOLDEN_SUM(backend_allocations);
        GOLDEN_SUM(backend_frees);
        GOLDEN_SUM(faults);
#undef GOLDEN_SUM
        protocol_ok = protocol_ok && d.protocol_ok && d.clean();
      }
      jobs_accepted += v.jobs->accepted.load();
      jobs_completed += v.jobs->completed.load();
      acquisitions += v.memory->acquisitions;
      releases += v.memory->releases;
      return protocol_ok && jobs_accepted == jobs_completed &&
             acquisitions == releases;
    }
  } totals;
  std::int32_t fault_status = 0, reset_status = 0, retry_status = 0,
               stop_status = 0;
  std::uint64_t fault_health = 0, fault_outstanding = 0,
                prior_device_failures = 0;
  std::unique_ptr<Peer> peer;
  if (o.external) {
    peer = std::make_unique<Peer>(a.peer);
    if (!peer->start())
      return 2;
  }
  OwnedReplay replay(o.ticks, o.external);
  if (!replay.begin(*s) || !s->controls())
    return 1;
  Oracle oracle(o);
  Telemetry telemetry;
  std::uint64_t prior_events = 0, prior_failures = 0;
  std::array<std::uint64_t, 3> prior_actions{};
  std::size_t recoveries = 0;
  std::vector<std::byte> recovery;
  for (std::size_t t = 0; t < o.ticks; ++t) {
    if ((o.campaign == Campaign::overload || a.fault != Fault::none) &&
        t == 6) {
      recovery = s->checkpoint(5);
      if (recovery.empty())
        return 1;
      const bool loss = a.fault == Fault::device_loss;
      const auto expected = a.fault == Fault::none ? rt::Status::callback_failed
                            : loss                 ? rt::Status::device_lost
                                   : rt::Status::device_reset_required;
      if (a.fault != Fault::none) {
        telemetry.expected_device_failure = expected;
        if (loss)
          owner->driver->lose_query = true;
        else
          owner->driver->fail_query = true;
      }
      const auto plant = s->world.plant;
      status = s->step(t, a.fault == Fault::none);
      fault_status = static_cast<std::int32_t>(status);
      if (status != expected || s->world.calls[1] != 6 ||
          s->world.calls[2] != 6 || s->world.plant.position != plant.position ||
          s->world.plant.velocity != plant.velocity ||
          s->world.plant.acceleration != plant.acceleration ||
          telemetry.drain(*s->runtime) != rt::Status::ok ||
          telemetry.deadline_failures != (a.fault == Fault::none ? 1u : 0u) ||
          telemetry.device_failures != (a.fault == Fault::none ? 0u : 1u))
        return 1;
      if (a.fault != Fault::none) {
        rt::DeviceHealth health = rt::make_device_health(),
                         after = rt::make_device_health();
        rt::DeviceTimelineInfo timeline;
        if (owner->driver->faults != 1 || owner->physics->providers != 7 ||
            owner->physics->publications != 6 ||
            !owner->physics->timeline(timeline) ||
            timeline.completed_value != 6 ||
            owner->physics->health(health) != rt::Status::ok ||
            health.outstanding != 1)
          return 1;
        fault_health = health.state;
        fault_outstanding = health.outstanding;
        if (loss) {
          reset_status = static_cast<std::int32_t>(owner->physics->reset());
          if (health.state != RTFW_DEVICE_HEALTH_LOST || health.losses != 1 ||
              reset_status != fault_status)
            return 1;
        } else {
          if (health.state != RTFW_DEVICE_HEALTH_RESET_REQUIRED)
            return 1;
          owner->driver->fail_stream_sync = true;
          reset_status = static_cast<std::int32_t>(owner->physics->reset());
          if (!reset_status || owner->driver->faults != 2 ||
              owner->driver->clean() || owner->memory->live_count() != 3)
            return 1;
          retry_status = static_cast<std::int32_t>(owner->physics->reset());
          if (retry_status || owner->physics->health(after) != rt::Status::ok ||
              after.state != RTFW_DEVICE_HEALTH_HEALTHY || after.outstanding ||
              after.resets != health.resets + 1 ||
              after.generation <= health.generation)
            return 1;
        }
        owner->driver->fail_unregister = true;
        stop_status = static_cast<std::int32_t>(owner->close());
        if (!stop_status || !owner->session || !owner->physics ||
            owner->driver->clean() || owner->memory->live_count() != 3)
          return 1;
      }
      if (!totals.close(*owner))
        return 1;
      owner.reset();
      owner = std::make_unique<Owner>(o, dispatch);
      s = owner->session.get();
      if (owner->prepare() != rt::Status::ok ||
          s->runtime->restore_checkpoint(recovery) != rt::Status::ok ||
          !s->world.decode())
        return 1;
      prior_events = telemetry.events;
      prior_actions = telemetry.action_records;
      prior_failures = telemetry.deadline_failures;
      prior_device_failures = telemetry.device_failures;
      Telemetry resumed;
      if (!resumed.resume(*s->runtime, telemetry))
        return 1;
      telemetry = resumed;
      replay = OwnedReplay(o.ticks, false, 6);
      replay.initial = s->checkpoint(5);
      if (replay.initial.empty())
        return 1;
      ++recoveries;
    }
    if (peer && t % 3 == 0)
      peer->exchange(t, s->world);
    replay.record(*s, t);
    status = s->step(t);
    if (status != rt::Status::ok) {
      std::cerr << "step " << t << ' ' << s->runtime->last_error() << '\n';
      return 1;
    }
    if (!oracle.step(t, s->world)) {
      std::cerr << "oracle " << t << '\n';
      return 1;
    }
    const auto trace_status = telemetry.drain(*s->runtime);
    if (trace_status != rt::Status::ok) {
      std::cerr << "telemetry tick=" << t << " stream=" << telemetry.last_stream
                << " status=" << static_cast<int>(trace_status)
                << " gaps=" << telemetry.action_gaps << '\n';
      return 1;
    }
  }
  const auto final = s->world.canonical;
  const auto calls = s->world.calls;
  const auto planned = s->plan.planned_bytes;
  const auto callback_count =
      std::accumulate(calls.begin(), calls.end(), std::uint64_t{0});
  if (!recoveries && !telemetry.metrics(*s->runtime, o.ticks, callback_count))
    return 1;
  rt::LiveControlMailboxInfo mailbox;
  rt::LiveControlCommitInfo commit;
  if (!s->runtime->live_control_mailbox_info(2601, mailbox) ||
      !s->runtime->live_control_commit_info(commit))
    return 1;
  if (!replay.seal(*s) || !replay.verify(*s)) {
    std::cerr << "replay " << s->runtime->last_error() << '\n';
    return 1;
  }
  rt::DeviceTimelineInfo timeline;
  if (device && !owner->physics->timeline(timeline))
    return 1;
  if (!totals.close(*owner))
    return 1;
  if (peer)
    (void)peer->close();
  const auto peer_result = peer ? peer->result : cil::Code::ok,
             peer_cleanup = peer ? peer->cleanup : cil::Code::ok;
  const auto sample_bytes =
      sizeof(Session) + sizeof(Memory) + sizeof(Jobs) + sizeof(Oracle) +
      sizeof(OwnedReplay) + sizeof(Telemetry) + 3 * sizeof(StateBytes) +
      sizeof(SimulatedDriver) + sizeof(CudaPhysics) + 2 * sizeof(Storage) +
      (peer ? sizeof(Peer) + sizeof(cil::Region) : 0) +
      replay.initial.capacity() + replay.active.capacity() +
      replay.trusted.capacity() +
      replay.inputs.capacity() * sizeof(rt::ReplayInputRecord) +
      replay.external.capacity() * 3073 + recovery.capacity();
  if (sample_bytes > fixed::sample_budget)
    return 1;
  if (!a.output.empty()) {
    fs::create_directories(a.output);
    if (!write(a.output / "state.bin", final) ||
        !write(a.output / "checkpoint.bin", replay.initial) ||
        !write(a.output / "active.bin", replay.active) ||
        !write(a.output / "trusted.bin", replay.trusted))
      return 1;
    std::ofstream f(a.output / "execution.json");
    f << "{\"dispatch\":\"" << a.dispatch << "\",\"variant\":\""
      << (device ? "sim_cuda" : "cpu")
      << "\",\"device_providers\":" << totals.providers
      << ",\"device_publications\":" << totals.publications
      << ",\"device_uploads\":" << totals.uploads
      << ",\"device_copies\":" << totals.copies
      << ",\"device_downloads\":" << totals.downloads
      << ",\"device_kernels\":" << totals.kernels
      << ",\"device_graphs\":" << totals.graphs
      << ",\"device_events\":" << totals.records
      << ",\"device_registrations\":" << totals.registrations
      << ",\"device_unregistrations\":" << totals.unregistrations
      << ",\"event_creates\":" << totals.event_creates
      << ",\"event_destroys\":" << totals.event_destroys
      << ",\"device_timeline\":" << timeline.completed_value
      << ",\"device_protocol\":" << (totals.protocol_ok ? "true" : "false")
      << ",\"device_allocations\":" << totals.backend_allocations
      << ",\"device_frees\":" << totals.backend_frees << ",\"fault\":\""
      << (a.fault == Fault::none          ? "none"
          : a.fault == Fault::device_loss ? "device_loss"
                                          : "reset_failure")
      << "\",\"fault_status\":" << fault_status
      << ",\"reset_status\":" << reset_status
      << ",\"reset_retry_status\":" << retry_status
      << ",\"stop_status\":" << stop_status
      << ",\"fault_health\":" << fault_health
      << ",\"fault_outstanding\":" << fault_outstanding
      << ",\"device_faults\":" << totals.faults << ",\"device_failures\":"
      << prior_device_failures + telemetry.device_failures
      << ",\"schema\":1,\"contract_sha256\":\"" << fixed::contract_sha256
      << "\",\"mode\":\"" << (o.host ? "host" : "native")
      << "\",\"count\":" << o.count << ",\"ticks\":" << o.ticks
      << ",\"workers\":" << o.workers << ",\"grain\":" << o.grain
      << ",\"campaign\":\""
      << campaign_names[static_cast<std::size_t>(o.campaign)]
      << "\",\"external\":" << (o.external ? "true" : "false")
      << ",\"session_identity\":\"" << session_identity << "\""
      << ",\"runtime_id\":" << telemetry.runtime_id
      << ",\"runtime_planned_bytes\":" << planned
      << ",\"sample_owned_bytes\":" << sample_bytes << ",\"phase_calls\":[";
    for (std::size_t i = 0; i < 8; ++i)
      f << (i ? "," : "") << calls[i];
    f << "],\"oracle\":true,\"trace_events\":"
      << telemetry.events + prior_events << ",\"trace_lost\":" << telemetry.lost
      << ",\"rate_actions\":" << prior_actions[0] + telemetry.action_records[0]
      << ",\"mixed_actions\":" << prior_actions[1] + telemetry.action_records[1]
      << ",\"control_actions\":"
      << prior_actions[2] + telemetry.action_records[2]
      << ",\"action_gaps\":" << telemetry.action_gaps
      << ",\"deadline_failures\":"
      << prior_failures + telemetry.deadline_failures
      << ",\"control_accepted\":" << mailbox.accepted
      << ",\"control_invalid\":" << mailbox.invalid
      << ",\"control_replaced\":" << commit.replaced
      << ",\"control_committed\":" << commit.committed
      << ",\"replay_frames\":" << replay.result.frames_replayed
      << ",\"replay_actions\":" << replay.result.actions_compared
      << ",\"replay_generations\":" << replay.result.generations_compared
      << ",\"recoveries\":" << recoveries
      << ",\"jobs_accepted\":" << totals.jobs_accepted
      << ",\"jobs_completed\":" << totals.jobs_completed
      << ",\"memory_acquired\":" << totals.acquisitions
      << ",\"memory_released\":" << totals.releases
      << ",\"cleanup\":true,\"peer_status\":\"" << cil::name(peer_result)
      << "\",\"peer_cleanup\":\"" << cil::name(peer_cleanup)
      << "\",\"peer_responses\":" << (peer ? peer->responses : 0) << "}\n";
    f.close();
    if (f.fail())
      return 1;
  }
  std::cout << "golden ticks=" << o.ticks << " callbacks=" << callback_count
            << " replay=" << replay.result.frames_replayed
            << " peer=" << cil::name(peer_result)
            << " cleanup=" << cil::name(peer_cleanup) << '\n';
  return peer_result == cil::Code::ok && peer_cleanup == cil::Code::ok ? 0 : 2;
}
int portable_main(int argc, char **argv) {
  try {
    Arguments a;
    if (!arguments(argc, argv, a)) {
      std::cerr << "usage: golden_cuda [--dispatch cpu|kernel|graph] [--fault "
                   "none|device_loss|reset_failure] [--mode native|host] "
                   "[--count 1..256] "
                   "[--ticks 1..1024] [--workers 1..3] [--grain 1|4|16|64] "
                   "[--campaign NAME] [--output DIRECTORY] [--peer NAME "
                   "--generation N --timeout-ms N]\n";
      return 2;
    }
    return execute(a);
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

} // namespace golden::cuda
