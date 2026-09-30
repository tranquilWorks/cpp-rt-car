#pragma once
#include "../golden_system/model.hpp"
#include <rtfw/benchmark.hpp>
#include <filesystem>
namespace golden::showcase {
inline constexpr std::array<std::string_view,15> case_ids{
  "golden-input", "golden-physics", "golden-stage", "golden-sensor",
  "golden-controller", "golden-actuator", "golden-aggregate", "golden-telemetry",
  "golden-host", "golden-memory", "golden-rates", "golden-controls",
  "golden-replay", "golden-external", "golden-loop"};
inline constexpr std::array<std::string_view,15> subsystems{
  "scenario", "physics", "host_staging", "sampled_io", "controller", "hal",
  "aggregation", "observability", "executor", "memory", "multi_rate",
  "live_control", "checkpoint_replay", "external_cil", "whole_loop"};
struct Invocation {
  std::uint64_t ordinal = 0, operations = 0, phase_calls = 0, entity_checks = 0,
                faults = 0, gaps = 0, cleanup_failures = 0, checksum = 0,
                runtime_id = 0, planned_bytes = 0, trace_events = 0,
                actions = 0, replay_frames = 0, accepted = 0, replaced = 0,
                jobs = 0, memory_acquired = 0, memory_released = 0;
  bool correct = false;
};
class Provider {
  Options options_;
  std::filesystem::path output_;
  std::string peer_prefix_, variant_, dispatch_;
  std::array<std::uint64_t,15> next_{};
  static rtfw::benchmark::Status describe(void*, std::size_t, rtfw::benchmark::Descriptor&);
  static rtfw::benchmark::Status invoke(void*, std::string_view, std::uint64_t, rtfw::benchmark::Observation&);
  bool perform(std::size_t, Invocation&);
  bool persist(std::size_t, const Invocation&);
public:
  Provider(Options, std::filesystem::path = {}, std::string peer_prefix = {},
           std::string variant = "cpu", std::string dispatch = "cpu");
  rtfw::benchmark::ProviderV1 table() noexcept;
};
} // namespace golden::showcase
