#include "session.hpp"
#include "replay.hpp"
#include "replay_gate.hpp"
#include "../golden_system/oracle.hpp"
#include "../golden_system/telemetry.hpp"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>
namespace s = golden::showcase;
using namespace golden;
bool save(const std::filesystem::path &path,std::span<const std::byte> bytes) {
  std::ofstream f(path,std::ios::binary);
  f.write(reinterpret_cast<const char *>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
  f.close();return !f.fail();
}
int failure(int line) { std::cerr << "telemetry loss validation failed at line " << line << '\n'; return 1; }
int main(int argc, char **argv) {
  try {
    if (argc != 2) return 2;
    const auto directory=std::filesystem::path(argv[1]).parent_path();
    Options options;
    auto memory = std::make_unique<Memory>();
    auto session = std::make_unique<s::Session>(options,nullptr,*memory);
    if (session->prepare(fixed::action_capacity,nullptr,64) != rt::Status::ok) return failure(__LINE__);
    s::Replay replay(options.ticks);
    if (!replay.begin(*session) || !session->controls()) return failure(__LINE__);
    Oracle oracle(options);
    Telemetry telemetry;
    std::uint64_t detected = 0, before_sequence = 0, first_sequence = 0,
                  next_sequence = 0, lost = 0, emitted = 0;
    std::vector<std::byte> checkpoint;
    rt::Status status = rt::Status::ok;
    // Injection at tick6 is an explicit paused consumer, while the real
    // producer continues into its finite64-slot trace store. Resume at tick12.
    for (std::size_t tick = 0; tick < options.ticks; ++tick) {
      if (tick == 6) {
        checkpoint = session->checkpoint(5);
        before_sequence = telemetry.cursor.next_sequence;
      }
      replay.record(*session,tick);
      if (session->step(tick) != rt::Status::ok || !oracle.step(tick,session->world)) return failure(__LINE__);
      if (tick < 6) {
        if (telemetry.drain(*session->runtime) != rt::Status::ok) return failure(__LINE__);
      } else if (tick == 12) {
        std::array<rt::RuntimeTraceEvent,64> events{};
        rt::RuntimeTraceReadResult read;
        status = session->runtime->read_trace(telemetry.cursor,events,read);
        if (status != rt::Status::ok || !read.lost_events || read.events_read != events.size() || read.remaining_sequence_count) return failure(__LINE__);
        first_sequence = read.first_sequence;
        next_sequence = telemetry.cursor.next_sequence;
        lost = read.lost_events;
        if (first_sequence < before_sequence || first_sequence - before_sequence != lost ||
            next_sequence - first_sequence != events.size()) return failure(__LINE__);
        for (std::size_t i = 0; i < events.size(); ++i)
          if (events[i].sequence != first_sequence + i) return failure(__LINE__);
        detected = tick;
        telemetry.next = next_sequence;
      } else if (tick > 12 && telemetry.drain(*session->runtime) != rt::Status::ok) {
        return failure(__LINE__);
      }
    }
    // The adapter refuses replay on a measured gap before calling Runtime.
    // Record the exact state on both sides; Runtime itself does not promise
    // that a host trace gap invalidates an otherwise valid action artifact.
    const auto final = session->world.canonical;
    const bool sealed = replay.seal(*session);
    const auto before = session->world.canonical;
    std::uint64_t replay_calls=0;
    const auto refusal = s::checked_replay(*session,replay,lost,replay_calls);
    if (!sealed || refusal != rt::Status::resource_exhausted || before != session->world.canonical || checkpoint.empty()) return failure(__LINE__);
    rt::RuntimeMetricSnapshot metrics;
    if (session->runtime->metrics_snapshot(rt::RuntimeMetricWindow::cumulative,nullptr,metrics) != rt::Status::ok) return failure(__LINE__);
    for (std::size_t i = 0; i < metrics.sample_count; ++i)
      if (metrics.samples[i].id == rt::RuntimeMetricId::trace_events_emitted) emitted = metrics.samples[i].value;
    rt::RateTelemetryMetadata identity;
    if (session->runtime->rate_telemetry_metadata(identity) != rt::Status::ok) return failure(__LINE__);
    const auto old_id = identity.runtime_id;
    if (!save(directory/"state.bin",final) || !save(directory/"checkpoint.bin",replay.initial) ||
        !save(directory/"active.bin",replay.active) || !save(directory/"trusted.bin",replay.trusted)) return failure(__LINE__);
    if (session->close() != rt::Status::ok) return failure(__LINE__);
    session = std::make_unique<s::Session>(options,nullptr,*memory);
    if (session->prepare(fixed::action_capacity,nullptr,64) != rt::Status::ok ||
        session->runtime->restore_checkpoint(checkpoint) != rt::Status::ok || !session->world.decode() ||
        session->runtime->rate_telemetry_metadata(identity) != rt::Status::ok || identity.runtime_id == old_id) return failure(__LINE__);
    const auto restored=session->world.canonical;
    if (s::checked_replay(*session,replay,0,replay_calls)!=rt::Status::invalid_argument || replay_calls!=0 ||
        session->world.canonical!=restored) return failure(__LINE__);
    // Recovery gets its own paired checkpoint/active/trusted identities. No
    // patching of the originating owner's replay artifact is permitted.
    s::Replay recovery(options.ticks,false,6);
    recovery.initial = session->checkpoint(5);
    for (std::size_t tick = 6; tick < options.ticks; ++tick) {
      recovery.record(*session,tick);
      if (session->step(tick) != rt::Status::ok) return failure(__LINE__);
    }
    if (session->world.canonical != final) return failure(__LINE__);
    if (!recovery.seal(*session)) { std::cerr << session->runtime->last_error() << '\n'; return failure(__LINE__); }
    if (!recovery.verify(*session)) { std::cerr << session->runtime->last_error() << '\n'; return failure(__LINE__); }
    if (session->close() != rt::Status::ok || memory->live_count() || memory->acquisitions != memory->releases) return failure(__LINE__);
    if (!save(directory/"recovery_checkpoint.bin",recovery.initial) || !save(directory/"recovery_active.bin",recovery.active) ||
        !save(directory/"recovery_trusted.bin",recovery.trusted)) return failure(__LINE__);
    std::ofstream f(argv[1]);
    f << "{\"schema\":1,\"fault\":\"telemetry_loss\",\"injected_tick\":6,\"detected_tick\":" << detected
      << ",\"capacity\":64,\"consumer_before\":" << before_sequence << ",\"first_sequence\":" << first_sequence
      << ",\"next_sequence\":" << next_sequence << ",\"lost\":" << lost << ",\"emitted\":" << emitted
      << ",\"refusal_status\":" << static_cast<int>(refusal) << ",\"state_unchanged\":true,\"replay_calls_before_refusal\":0"
      << ",\"original_runtime\":" << old_id << ",\"recovery_runtime\":" << identity.runtime_id
      << ",\"recovery_frames\":" << recovery.result.frames_replayed << ",\"memory_acquired\":" << memory->acquisitions
      << ",\"memory_released\":" << memory->releases << ",\"cleanup\":true,\"correct\":true}\n";
    f.close(); return f.fail() ? 1 : 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return failure(__LINE__); }
}
