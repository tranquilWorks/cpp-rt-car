#include "oracle.hpp"
#include "peer.hpp"
#include "replay.hpp"
#include "session.hpp"
#include "telemetry.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
namespace fs = std::filesystem;
using namespace golden;
struct Arguments {
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
    if (key == "--mode") {
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
  auto memory = std::make_unique<Memory>();
  auto jobs = std::make_unique<Jobs>();
  if (o.host && jobs->start(o.workers) != rt::Status::ok)
    return 1;
  auto s = std::make_unique<Session>(o, o.host ? jobs.get() : nullptr, *memory);
  auto status = s->prepare();
  if (status != rt::Status::ok) {
    std::cerr << "prepare " << s->runtime->last_error() << '\n';
    return 1;
  }
  std::unique_ptr<Peer> peer;
  if (o.external) {
    peer = std::make_unique<Peer>(a.peer);
    if (!peer->start())
      return 2;
  }
  Replay replay(o.ticks, o.external);
  if (!replay.begin(*s) || !s->controls())
    return 1;
  Oracle oracle(o);
  Telemetry telemetry;
  std::uint64_t prior_events = 0, prior_failures = 0;
  std::array<std::uint64_t, 3> prior_actions{};
  std::size_t recoveries = 0;
  std::vector<std::byte> recovery;
  for (std::size_t t = 0; t < o.ticks; ++t) {
    if (o.campaign == Campaign::overload && t == 6) {
      recovery = s->checkpoint(5);
      if (recovery.empty() || s->step(t, true) != rt::Status::callback_failed ||
          s->world.calls[0] != 6 ||
          telemetry.drain(*s->runtime) != rt::Status::ok ||
          telemetry.deadline_failures != 1)
        return 1;
      if (s->close() != rt::Status::ok)
        return 1;
      s.reset();
      s = std::make_unique<Session>(o, o.host ? jobs.get() : nullptr, *memory);
      if (s->prepare() != rt::Status::ok ||
          s->runtime->restore_checkpoint(recovery) != rt::Status::ok ||
          !s->world.decode())
        return 1;
      prior_events = telemetry.events;
      prior_actions = telemetry.action_records;
      prior_failures = telemetry.deadline_failures;
      Telemetry resumed;
      if (!resumed.resume(*s->runtime, telemetry))
        return 1;
      telemetry = resumed;
      replay = Replay(o.ticks, false, 6);
      replay.initial = s->checkpoint(5);
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
  if (s->close() != rt::Status::ok)
    return 1;
  s.reset();
  if (o.host && jobs->close() != rt::Status::ok)
    return 1;
  if (peer)
    (void)peer->close();
  const auto peer_result = peer ? peer->result : cil::Code::ok,
             peer_cleanup = peer ? peer->cleanup : cil::Code::ok;
  const auto sample_bytes =
      sizeof(Session) + sizeof(Memory) + sizeof(Jobs) + sizeof(Oracle) +
      sizeof(Replay) + sizeof(Telemetry) + 3 * sizeof(StateBytes) +
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
    f << "{\"schema\":1,\"contract_sha256\":\"" << fixed::contract_sha256
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
      << ",\"jobs_accepted\":" << jobs->accepted.load()
      << ",\"jobs_completed\":" << jobs->completed.load()
      << ",\"memory_acquired\":" << memory->acquisitions
      << ",\"memory_released\":" << memory->releases
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
int main(int argc, char **argv) {
  try {
    Arguments a;
    if (!arguments(argc, argv, a)) {
      std::cerr << "usage: golden_system [--mode native|host] [--count 1..256] "
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
