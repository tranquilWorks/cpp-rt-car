#include "provider.hpp"
#include "variants.hpp"
#include "probes.hpp"
#include "../golden_system/oracle.hpp"
#include "../golden_system/peer.hpp"
#include "../golden_system/replay.hpp"
#include "../golden_system/telemetry.hpp"
#include <fstream>
#include <memory>
#include <stdexcept>
namespace golden::showcase {
namespace b = rtfw::benchmark;
namespace fs = std::filesystem;
namespace {
bool bytes(const fs::path &p, std::span<const std::byte> data) {
  std::ofstream f(p, std::ios::binary);
  f.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
  f.close(); return !f.fail();
}
}
Provider::Provider(Options o, fs::path output, std::string peer_prefix, std::string variant, std::string dispatch)
    : options_(o), output_(std::move(output)), peer_prefix_(std::move(peer_prefix)), variant_(std::move(variant)), dispatch_(std::move(dispatch)) {
  if (!((variant_ == "cpu" && dispatch_ == "cpu") ||
        (variant_ == "sim_xdma" && dispatch_ == "cpu") ||
        ((variant_ == "sim_cuda" || variant_ == "sim_combined") && (dispatch_ == "kernel" || dispatch_ == "graph"))))
    throw std::invalid_argument("incompatible variant and dispatch");
  if (!o.valid() || o.external || o.campaign != Campaign::nominal ||
      (!peer_prefix_.empty() && !cil::valid_key(peer_prefix_ + "_6")))
    throw std::invalid_argument("invalid showcase configuration");
}
b::ProviderV1 Provider::table() noexcept {
  b::ProviderV1 p; p.id = "rtfw.golden"; p.user = this;
  p.case_count = case_ids.size(); p.describe = describe; p.invoke = invoke;
  return p;
}
b::Status Provider::describe(void *opaque, std::size_t i, b::Descriptor &d) {
  if (!opaque || i >= case_ids.size()) return b::Status::invalid;
  const auto &p = *static_cast<Provider *>(opaque);
  d = {}; d.case_id = case_ids[i]; d.subsystem = subsystems[i];
  d.implementation = "golden-public-sdk-v1";
  d.configuration = i < 7 ? "bounded-cpu-phase-experiment" : "frozen-golden-cpu-scenario";
  if (i == 14) d.configuration = p.variant_ + "-" + p.dispatch_;
  // Every sample includes owner setup, Runtime dispatch, the declared work,
  // full oracle, telemetry when selected, checked stop and evidence writes.
  // No kernel-only or isolated callback latency is asserted.
  constexpr std::array<std::string_view,15> work{
    "seed-and-command", "integer-task-integrator", "host-frame-staging",
    "sample-decode-calibration", "saturating-controller", "actuator-frame-encoding",
    "six-field-aggregation", "loop-and-all-telemetry-drains", "independent-host-loop",
    "memory-plan-acquire-observe-release", "reference-schedule-and-loop",
    "replacement-control-loop", "loop-capture-and-origin-owner-replay",
    "external-process-cil-loop", "full-loop-telemetry-and-replay"};
  d.workload_kind = std::string(work[i]) + ".lifecycle-oracle-io";
  d.workload_sha256 = b::sha256(std::string(fixed::contract_sha256) + ":" + d.workload_kind);
  d.parameters = {{"entities",p.options_.count,1,256}, {"ticks",p.options_.ticks,1,1024},
                  {"workers",p.options_.workers,1,3}, {"grain",p.options_.grain,1,64},
                  {"host",i == 8 || p.options_.host ? 1u : 0u,0,1}};
  for (const char *name : {"operations", "phase_calls", "entity_checks", "faults", "gaps", "cleanup_failures"})
    d.counters.push_back({name,"count",0,b::max_integer});
  return b::Status::ok;
}
b::Status Provider::invoke(void *opaque, std::string_view id, std::uint64_t ordinal, b::Observation &o) {
  if (!opaque) return b::Status::invalid;
  auto &p = *static_cast<Provider *>(opaque);
  const auto it = std::find(case_ids.begin(), case_ids.end(), id);
  if (it == case_ids.end()) return b::Status::not_found;
  const auto i = static_cast<std::size_t>(it - case_ids.begin());
  if (ordinal != p.next_[i] || ordinal >= 7) return b::Status::invalid;
  if ((p.variant_ != "cpu" && i != 14) || (p.options_.host && i < 7) ||
      (!p.peer_prefix_.empty() && i != 13)) return b::Status::invalid;
  if (i == 13 && p.peer_prefix_.empty()) return b::Status::not_run;
  Invocation v; v.ordinal = ordinal;
  try { v.correct = p.perform(i, v); }
  catch (...) { v.correct = false; }
  const bool saved = p.persist(i, v);
  o.correct = v.correct && saved;
  o.checksum = v.checksum & b::max_integer;
  o.counters = {v.operations,v.phase_calls,v.entity_checks,v.faults,v.gaps,v.cleanup_failures};
  ++p.next_[i];
  return o.correct ? b::Status::ok : b::Status::invariant_failed;
}
bool Provider::perform(std::size_t i, Invocation &v) {
  const auto destination = output_.empty() ? fs::path{} : output_ / std::string(case_ids[i]) / std::to_string(v.ordinal);
  if (!destination.empty() && !fs::create_directories(destination)) return false;
  if (variant_ != "cpu") return i == 14 && variant_run(variant_,dispatch_,options_,destination,v);
  if (i < 7) {
    auto probe = std::make_unique<Probe>(options_,i);
    rt::Runtime r;
    rt::RuntimeConfig config;
    config.callback_capacity = 1; config.worker_count = options_.workers;
    config.executor_queue_capacity = fixed::queue_slots;
    config.scratch_bytes = config.task_scratch_bytes = fixed::scratch_bytes;
    config.task_scratch_slots = fixed::task_slots; config.trace_capacity = 0;
    rt::PhaseHandle phase;
    if (r.configure(config) != rt::Status::ok ||
        r.register_callback({case_ids[i],Probe::invoke,probe.get()},phase) != rt::Status::ok ||
        r.finalize() != rt::Status::ok) return false;
    rt::MemoryPlan plan{};
    bool ok = r.memory_plan(plan) && r.start() == rt::Status::ok;
    for (std::size_t t = 0; ok && t < options_.ticks; ++t)
      ok = r.step({t,std::chrono::nanoseconds(fixed::tick_ns),std::nullopt,std::nullopt}) == rt::Status::ok && probe->verify();
    v.planned_bytes = plan.planned_bytes; v.operations = probe->calls;
    v.phase_calls = probe->calls; v.entity_checks = probe->checks;
    // Explicit encoding avoids compiler padding/endian dependent evidence.
    World snapshot(options_); snapshot.plant = probe->plant;
    snapshot.command = probe->command; snapshot.sensor = probe->sensor;
    snapshot.sums = probe->sums; snapshot.calls[i] = probe->calls;
    snapshot.next_tick = probe->calls; snapshot.encode();
    v.checksum = digest(snapshot.canonical);
    if (r.stop() != rt::Status::ok) { ++v.cleanup_failures; ok = false; }
    if (!destination.empty()) ok = bytes(destination / "state.bin", snapshot.canonical) && ok;
    return ok;
  }
  Options o = options_;
  o.host = i == 8 || o.host; o.external = i == 13;
  if (i == 11) o.campaign = Campaign::control_replaced;
  auto memory = std::make_unique<Memory>();
  auto jobs = std::make_unique<Jobs>();
  if (o.host && jobs->start(o.workers) != rt::Status::ok) return false;
  auto s = std::make_unique<Session>(o,o.host ? jobs.get() : nullptr,*memory);
  if (s->prepare() != rt::Status::ok) return false;
  v.planned_bytes = s->plan.planned_bytes;
  bool ok = true;
  const bool replay_case = i == 12 || i == 14;
  Replay replay(o.ticks, o.external);
  Oracle oracle(o);
  Telemetry telemetry;
  std::unique_ptr<Peer> peer;
  const std::string peer_key = peer_prefix_ + "_" + std::to_string(v.ordinal);
  if (o.external) {
    cil::Options peer_options;
    peer_options.key = peer_key;
    peer_options.generation = 26; peer_options.timeout_ms = 5000;
    std::cout << "showcase_peer " << peer_options.key << '\n' << std::flush;
    peer = std::make_unique<Peer>(peer_options);
    ok = peer->start();
  }
  if (i == 10) {
    std::array<std::uint64_t,8> counts{};
    for (std::size_t n = 0; n < 27; ++n) {
      rt::ReferenceRelease release;
      if (!s->runtime->reference_release_at(n,release) || release.phase.index() >= 8 ||
          release.release_time_ns % (fixed::periods[fixed::phase_rates[release.phase.index()]] * fixed::tick_ns)) ok = false;
      else ++counts[release.phase.index()];
    }
    constexpr std::array<std::uint64_t,8> expected{6,6,6,3,2,2,1,1};
    ok = ok && counts == expected; v.operations = 27;
  }
  if (i != 9) {
    ok = ok && (!replay_case || replay.begin(*s)) && s->controls();
    for (std::size_t t = 0; ok && t < o.ticks; ++t) {
      if (peer && t % 3 == 0) peer->exchange(t,s->world);
      if (replay_case) replay.record(*s,t);
      ok = s->step(t) == rt::Status::ok && oracle.step(t,s->world);
      if (ok) v.entity_checks += fixed::capacity * 3;
      if (i == 7 || i == 14) ok = ok && telemetry.drain(*s->runtime) == rt::Status::ok;
      ++v.operations;
    }
    v.phase_calls = std::accumulate(s->world.calls.begin(),s->world.calls.end(),std::uint64_t{});
    if (i == 7 || i == 14) ok = ok && telemetry.metrics(*s->runtime,o.ticks,v.phase_calls);
    rt::LiveControlMailboxInfo mailbox;
    rt::LiveControlCommitInfo commit;
    ok = ok && s->runtime->live_control_mailbox_info(2601,mailbox) && s->runtime->live_control_commit_info(commit);
    v.accepted = mailbox.accepted; v.replaced = commit.replaced;
    if (i == 11) ok = ok && v.replaced == 1 && v.accepted == 7;
  } else v.operations = memory->acquisitions + memory->observations;
  v.checksum = digest(s->world.canonical);
  if (!destination.empty()) ok = bytes(destination / "state.bin",s->world.canonical) && ok;
  if (replay_case) {
    ok = ok && replay.seal(*s) && replay.verify(*s);
    v.replay_frames = replay.result.frames_replayed;
    if (!destination.empty()) ok = bytes(destination / "checkpoint.bin",replay.initial) &&
      bytes(destination / "active.bin",replay.active) && bytes(destination / "trusted.bin",replay.trusted) && ok;
  }
  rt::RateTelemetryMetadata metadata;
  ok = ok && s->runtime->rate_telemetry_metadata(metadata) == rt::Status::ok;
  v.runtime_id = metadata.runtime_id;
  v.trace_events = telemetry.events; v.actions = std::accumulate(telemetry.action_records.begin(),telemetry.action_records.end(),std::uint64_t{});
  v.gaps = telemetry.lost + telemetry.action_gaps;
  if (s->close() != rt::Status::ok) { ++v.cleanup_failures; ok = false; }
  if (o.host && jobs->close() != rt::Status::ok) { ++v.cleanup_failures; ok = false; }
  v.jobs = jobs->accepted; v.memory_acquired = memory->acquisitions; v.memory_released = memory->releases;
  ok = ok && !memory->live_count() && !memory->violations && v.memory_acquired == v.memory_released && jobs->accepted == jobs->completed;
  if (peer) {
    ok = peer->close() && ok && peer->result == cil::Code::ok && peer->cleanup == cil::Code::ok && peer->responses == (o.ticks + 2) / 3;
    v.operations += peer->responses;
  }
  return ok;
}
bool Provider::persist(std::size_t i, const Invocation &v) {
  if (output_.empty()) return true;
  const auto destination = output_ / std::string(case_ids[i]) / std::to_string(v.ordinal);
  if (!fs::is_directory(destination)) return false;
  std::ofstream f(destination / "invocation.json");
  f << "{\"schema\":1,\"case_id\":\"" << case_ids[i] << "\",\"contract_sha256\":\"" << fixed::contract_sha256 << '"';
#define FIELD(name) f << ",\"" #name "\":" << v.name
  FIELD(ordinal); FIELD(operations); FIELD(phase_calls); FIELD(entity_checks);
  FIELD(faults); FIELD(gaps); FIELD(cleanup_failures); FIELD(checksum);
  FIELD(runtime_id); FIELD(planned_bytes); FIELD(trace_events); FIELD(actions);
  FIELD(replay_frames); FIELD(accepted); FIELD(replaced); FIELD(jobs);
  FIELD(memory_acquired); FIELD(memory_released);
#undef FIELD
  f << ",\"correct\":" << (v.correct ? "true" : "false") << "}\n";
  f.close(); return !f.fail();
}
} // namespace golden::showcase
