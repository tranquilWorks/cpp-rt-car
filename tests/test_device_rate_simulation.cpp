#include "device_rate_simulation/fixture.hpp"
#include <future>
#include <gtest/gtest.h>
#include <limits>
using namespace std::chrono_literals;
namespace {
using namespace simulation_test;
rt::Status start(Fixture &f,
                 std::optional<rt::DeviceRateSimulationPolicy> policy =
                     rt::DeviceRateSimulationPolicy{watchdog},
                 bool replay = false) {
  auto s = f.configure(replay);
  if (s != rt::Status::ok)
    return s;
  auto b = f.binding();
  b.simulation = policy;
  s = f.runtime.bind_device_phase_to_rate_domain(b);
  if (s != rt::Status::ok)
    return s;
  s = f.runtime.finalize();
  return s == rt::Status::ok ? f.runtime.start() : s;
}
template <class F> bool wait_for(F predicate) {
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (!predicate() && std::chrono::steady_clock::now() < end)
    std::this_thread::yield();
  return predicate();
}
std::vector<std::byte> checkpoint(Fixture &f) {
  std::size_t n = 0;
  if (f.runtime.checkpoint_size(n) != rt::Status::ok)
    return {};
  std::vector<std::byte> bytes(n);
  rt::ArtifactWriteResult r;
  if (f.runtime.write_checkpoint(0, bytes, r) != rt::Status::ok)
    return {};
  bytes.resize(r.bytes_written);
  return bytes;
}
rt::CallbackResult apply(void *, const rt::ReplayInputView &) {
  return rt::CallbackResult::ok;
}
} // namespace
TEST(CommandBatch, SimulationHostDelayPreservesLogicalBudget) {
  Fixture f;
  f.backend.allowed = false;
  ASSERT_EQ(start(f), rt::Status::ok) << f.runtime.last_error();
  auto run = std::async(std::launch::async, [&] { return f.step(); });
  const bool entered =
      wait_for([&] { return f.backend.submitted.load() == 1; });
  std::this_thread::sleep_for(
      20ms); // Controlled delay exceeds unchanged 1 ms logical budget.
  const bool still_pending = run.wait_for(0ms) != std::future_status::ready;
  f.backend.allowed = true;
  EXPECT_TRUE(entered);
  EXPECT_TRUE(still_pending);
  EXPECT_EQ(run.get(), rt::Status::ok) << f.runtime.last_error();
  EXPECT_EQ(f.backend.timeout.load(), budget);
  EXPECT_EQ(f.publications, 1u);
  EXPECT_EQ(f.completed(), 1u);
  EXPECT_EQ(f.clock.now.load(), 1000u);
  EXPECT_EQ(f.runtime.stop(), rt::Status::ok);
}
TEST(CommandBatch, SimulationDefaultPathNegativeControlRetainsWallDeadline) {
  Fixture f;
  f.backend.allowed = false;
  ASSERT_EQ(start(f, std::nullopt), rt::Status::ok);
  auto run = std::async(std::launch::async, [&] { return f.step(); });
  const bool terminal = run.wait_for(5s) == std::future_status::ready;
  if (!terminal)
    f.backend.allowed = true; // Bounded rescue, never a passing substitute.
  EXPECT_TRUE(terminal);
  EXPECT_EQ(run.get(), rt::Status::device_timeout);
  EXPECT_EQ(f.publications, 0u);
  EXPECT_EQ(f.completed(), 0u);
  EXPECT_EQ(f.runtime.stop(), rt::Status::ok);
}
TEST(CommandBatch, SimulationFrozenClockStillHasFiniteWatchdog) {
  Fixture f;
  f.backend.allowed = false;
  ASSERT_EQ(start(f, rt::DeviceRateSimulationPolicy{50'000'000}),
            rt::Status::ok);
  auto run = std::async(std::launch::async, [&] { return f.step(); });
  const bool terminal = run.wait_for(5s) == std::future_status::ready;
  if (!terminal)
    f.backend.allowed = true;
  EXPECT_TRUE(terminal);
  EXPECT_EQ(run.get(), rt::Status::device_timeout);
  EXPECT_EQ(f.clock.now.load(), 1000u);
  EXPECT_EQ(f.publications, 0u);
  EXPECT_EQ(f.completed(), 0u);
  EXPECT_EQ(f.backend.registered.load(), 1u);
  EXPECT_EQ(f.backend.unregistered.load(), 0u);
  EXPECT_EQ(f.runtime.reset_device(f.device), rt::Status::invalid_state);
  EXPECT_EQ(f.runtime.stop(), rt::Status::ok);
  EXPECT_EQ(f.backend.unregistered.load(), 1u);
}
TEST(CommandBatch, SimulationLogicalExpiryCannotPublishLateCompletion) {
  for (auto advance : {budget, budget + 1}) {
    Fixture f;
    f.backend.allowed = false;
    ASSERT_EQ(start(f), rt::Status::ok);
    auto run = std::async(std::launch::async, [&] { return f.step(); });
    const bool entered =
        wait_for([&] { return f.backend.submitted.load() == 1; });
    f.clock.now = 1000 + advance;
    f.backend.allowed = true;
    EXPECT_TRUE(entered);
    EXPECT_EQ(run.get(), rt::Status::device_timeout);
    EXPECT_EQ(f.publications, 0u);
    EXPECT_EQ(f.completed(), 0u);
    EXPECT_EQ(f.runtime.stop(), rt::Status::ok);
  }
}
TEST(CommandBatch, SimulationSubmittingAndEarlyCompletionRetainOwnership) {
  for (bool complete_early : {false, true}) {
    Fixture f;
    f.backend.allowed = complete_early;
    f.backend.block_submit = true;
    ASSERT_EQ(start(f, rt::DeviceRateSimulationPolicy{50'000'000}),
              rt::Status::ok);
    auto run = std::async(std::launch::async, [&] { return f.step(); });
    const bool terminal = run.wait_for(5s) == std::future_status::ready;
    if (!terminal)
      f.backend.release_submit = true;
    EXPECT_TRUE(terminal);
    EXPECT_EQ(run.get(), rt::Status::device_timeout);
    EXPECT_EQ(f.publications, 0u);
    EXPECT_EQ(f.completed(), 0u);
    EXPECT_EQ(f.backend.unregistered.load(), 0u);
    EXPECT_EQ(f.runtime.stop(), rt::Status::ok);
    EXPECT_EQ(f.backend.unregistered.load(), 1u);
  }
}
TEST(CommandBatch, SimulationCleanupFailuresKeepOwnershipForRetry) {
  Fixture f;
  f.backend.allowed = false;
  ASSERT_EQ(start(f, rt::DeviceRateSimulationPolicy{50'000'000}),
            rt::Status::ok);
  EXPECT_EQ(f.step(), rt::Status::device_timeout);
  f.backend.fail_unregister = true;
  EXPECT_NE(f.runtime.stop(), rt::Status::ok);
  EXPECT_EQ(f.backend.unregistered.load(), 0u);
  EXPECT_EQ(f.runtime.stop(), rt::Status::ok);
  EXPECT_EQ(f.backend.unregistered.load(), 1u);
}
TEST(CommandBatch, SimulationPolicyValidationReplacementAndFrozenMutation) {
  Fixture f;
  ASSERT_EQ(f.configure(), rt::Status::ok);
  auto b = f.binding();
  for (auto bound :
       {std::uint64_t{0}, rt::device_rate_simulation_watchdog_limit_ns + 1,
        UINT64_MAX}) {
    b.simulation = rt::DeviceRateSimulationPolicy{bound};
    EXPECT_EQ(f.runtime.bind_device_phase_to_rate_domain(b),
              rt::Status::invalid_argument);
  }
  b.simulation = rt::DeviceRateSimulationPolicy{watchdog};
  ASSERT_EQ(f.runtime.bind_device_phase_to_rate_domain(b), rt::Status::ok);
  b.simulation->host_watchdog_ns = 0;
  EXPECT_EQ(f.runtime.replace_device_rate_binding(b),
            rt::Status::invalid_argument);
  b.simulation->host_watchdog_ns = watchdog / 2;
  EXPECT_EQ(f.runtime.replace_device_rate_binding(b), rt::Status::ok);
  ASSERT_EQ(f.runtime.finalize(), rt::Status::ok);
  rt::CompiledDeviceRatePhase phase;
  ASSERT_TRUE(f.runtime.compiled_device_rate_phase_at(0, phase));
  ASSERT_TRUE(phase.simulation);
  EXPECT_EQ(phase.simulation->host_watchdog_ns, watchdog / 2);
  EXPECT_EQ(phase.completion_budget_ns, budget);
  EXPECT_EQ(f.runtime.replace_device_rate_binding(b),
            rt::Status::invalid_state);
  EXPECT_EQ(f.runtime.stop(), rt::Status::ok);
}
TEST(CommandBatch, SimulationNativeCapabilityCannotOptIn) {
  Fixture f;
  f.backend.deterministic = false;
  ASSERT_EQ(f.configure(), rt::Status::ok);
  auto b = f.binding();
  b.simulation = rt::DeviceRateSimulationPolicy{watchdog};
  EXPECT_EQ(f.runtime.bind_device_phase_to_rate_domain(b),
            rt::Status::invalid_config);
  b.simulation.reset();
  ASSERT_EQ(f.runtime.bind_device_phase_to_rate_domain(b), rt::Status::ok);
  b.simulation = rt::DeviceRateSimulationPolicy{watchdog};
  EXPECT_EQ(f.runtime.replace_device_rate_binding(b),
            rt::Status::invalid_config);
  ASSERT_EQ(f.runtime.finalize(), rt::Status::ok);
  rt::CompiledDeviceRatePhase p;
  ASSERT_TRUE(f.runtime.compiled_device_rate_phase_at(0, p));
  EXPECT_FALSE(p.simulation);
  EXPECT_EQ(f.runtime.stop(), rt::Status::ok);
}
TEST(CommandBatch, SimulationIndependentOwnersAndMemoryAccounting) {
  Fixture a, b;
  ASSERT_EQ(start(a), rt::Status::ok);
  ASSERT_EQ(start(b), rt::Status::ok);
  auto first = std::async(std::launch::async, [&] {
    for (unsigned i = 0; i < 32; ++i)
      if (a.step(i) != rt::Status::ok)
        return false;
    return true;
  });
  auto second = std::async(std::launch::async, [&] {
    for (unsigned i = 0; i < 32; ++i)
      if (b.step(i) != rt::Status::ok)
        return false;
    return true;
  });
  EXPECT_TRUE(first.get());
  EXPECT_TRUE(second.get());
  EXPECT_EQ(a.publications, 32u);
  EXPECT_EQ(b.publications, 32u);
  EXPECT_EQ(a.completed(), 32u);
  EXPECT_EQ(b.completed(), 32u);
  rt::MemoryPlan p;
  ASSERT_TRUE(a.runtime.memory_plan(p));
  EXPECT_EQ(p.device_rate_phase_count, 1u);
  EXPECT_EQ(p.planned_bytes,
            p.runtime_control_bytes + p.executor_control_bytes +
                p.device_control_bytes + p.phase_scratch_total_bytes +
                p.task_scratch_total_bytes + p.trace_storage_bytes);
  EXPECT_EQ(a.runtime.stop(), rt::Status::ok);
  EXPECT_EQ(b.runtime.stop(), rt::Status::ok);
}
TEST(MixedRateReplay,
     SimulationPolicyIdentityRejectsForeignCheckpointBeforeMutation) {
  Fixture a, b;
  ASSERT_EQ(start(a), rt::Status::ok);
  ASSERT_EQ(start(b, rt::DeviceRateSimulationPolicy{watchdog / 2}),
            rt::Status::ok);
  auto bytes = checkpoint(a);
  ASSERT_FALSE(bytes.empty());
  rt::CheckpointMetadata x, y;
  auto other = checkpoint(b);
  ASSERT_EQ(rt::inspect_checkpoint_artifact(bytes, x), rt::Status::ok);
  ASSERT_EQ(rt::inspect_checkpoint_artifact(other, y), rt::Status::ok);
  EXPECT_EQ(x.config_id,
            y.config_id); // RuntimeConfig is intentionally unchanged.
  EXPECT_NE(x.graph_id,
            y.graph_id); // Policy is part of the compiled graph identity.
  const auto before = b.state;
  rt::CheckpointMetadata metadata;
  EXPECT_EQ(b.runtime.restore_checkpoint(bytes, &metadata),
            rt::Status::incompatible_artifact);
  EXPECT_EQ(b.state, before);
  EXPECT_EQ(b.backend.submitted.load(), 0u);
  EXPECT_EQ(a.runtime.stop(), rt::Status::ok);
  EXPECT_EQ(b.runtime.stop(), rt::Status::ok);
}
TEST(MixedRateReplay, SimulationReplayUsesRecordedTimeAndKeepsHostWatchdog) {
  Fixture f;
  ASSERT_EQ(start(f, rt::DeviceRateSimulationPolicy{50'000'000}, true),
            rt::Status::ok)
      << f.runtime.last_error();
  auto initial = checkpoint(f);
  ASSERT_FALSE(initial.empty());
  std::array<rt::ReplayInputRecord, 3> inputs;
  for (unsigned i = 0; i < inputs.size(); ++i) {
    inputs[i] = {f.frame(i), 1, {}};
    ASSERT_EQ(f.step(i), rt::Status::ok);
  }
  std::vector<std::byte> artifact(65536);
  rt::ArtifactWriteResult out;
  ASSERT_EQ(
      f.runtime.write_active_replay_artifact(initial, inputs, artifact, out),
      rt::Status::ok)
      << f.runtime.last_error();
  artifact.resize(out.bytes_written);
  const auto expected = f.state;
  f.clock.now =
      900'000'000'000ull; // Deliberately unrelated current clock on replay.
  rt::ActiveReplayResult result;
  EXPECT_EQ(f.runtime.replay_active(artifact, apply, nullptr, &result),
            rt::Status::ok)
      << f.runtime.last_error();
  EXPECT_EQ(f.state, expected);
  EXPECT_EQ(result.replay.frames_replayed, inputs.size());
  EXPECT_EQ(f.backend.timeout.load(), budget);
  EXPECT_EQ(f.completed(), 6u);
  f.backend.allowed = false;
  EXPECT_NE(f.runtime.replay_active(artifact, apply, nullptr, &result),
            rt::Status::ok);
  EXPECT_EQ(f.completed(), 6u);
  EXPECT_EQ(f.runtime.stop(), rt::Status::ok);
}

TEST(CommandBatch, SimulationQueuedWatchdogExpiresBehindBlockedSubmit) {
  Fixture f;
  f.backend.block_submit = true;
  ASSERT_EQ(f.configure(false, true), rt::Status::ok);
  auto first = f.binding();
  first.simulation = rt::DeviceRateSimulationPolicy{watchdog};
  auto second = first;
  second.phase = f.second_physics;
  second.simulation->host_watchdog_ns = 50'000'000;
  ASSERT_EQ(f.runtime.bind_device_phase_to_rate_domain(first), rt::Status::ok);
  ASSERT_EQ(f.runtime.bind_device_phase_to_rate_domain(second), rt::Status::ok);
  ASSERT_EQ(f.runtime.finalize(), rt::Status::ok) << f.runtime.last_error();
  ASSERT_EQ(f.runtime.start(), rt::Status::ok);
  auto run = std::async(std::launch::async, [&] { return f.step(); });
  const bool both = wait_for([&] {
    return f.provider_calls.load() == 2 && f.backend.submitted.load() == 1;
  });
  std::this_thread::sleep_for(100ms);
  f.backend.release_submit = true;
  EXPECT_TRUE(both);
  EXPECT_EQ(run.get(), rt::Status::device_timeout) << f.runtime.last_error();
  EXPECT_EQ(f.backend.submitted.load(),
            1u); // Second batch never reached vendor code.
  EXPECT_EQ(f.publications, 0u);
  EXPECT_EQ(f.completed(), 1u);
  EXPECT_EQ(f.runtime.stop(), rt::Status::ok);
}
TEST(CommandBatch, SimulationFailedBackendCompletionCannotAdvanceTimeline) {
  Fixture f;
  f.backend.completion_status = rt::HalV2Status::error;
  ASSERT_EQ(start(f), rt::Status::ok);
  EXPECT_EQ(f.step(), rt::Status::device_error);
  EXPECT_EQ(f.completed(), 0u);
  EXPECT_EQ(f.publications, 0u);
  EXPECT_EQ(f.runtime.stop(), rt::Status::ok);
}

TEST(MixedRateReplay, SimulationFailedCompletionRetainsBackendTimestamp) {
  for (bool complete_early : {false, true}) {
    Fixture f;
    f.backend.completion_status = rt::HalV2Status::error;
    f.backend.allowed = complete_early;
    f.backend.block_submit = complete_early;
    ASSERT_EQ(start(f, rt::DeviceRateSimulationPolicy{watchdog}, true),
              rt::Status::ok);
    auto run = std::async(std::launch::async, [&] { return f.step(); });
    const bool entered = wait_for([&] {
      return f.backend.submitted.load() == 1 &&
             (!complete_early || !f.backend.ready.load());
    });
    if (!complete_early)
      std::this_thread::sleep_for(20ms);
    f.backend.allowed = true;
    f.backend.release_submit = true;
    EXPECT_TRUE(entered);
    EXPECT_EQ(run.get(), rt::Status::device_error);
    EXPECT_EQ(f.completed(), 0u);
    EXPECT_EQ(f.publications, 0u);
    rt::MixedRateActionCursor cursor{};
    std::array<rt::MixedRateActionRecord, 16> actions{};
    rt::MixedRateActionReadResult result{};
    ASSERT_EQ(f.runtime.read_mixed_rate_actions(cursor, actions, result),
              rt::Status::ok);
    unsigned terminals = 0;
    for (std::size_t i = 0; i < result.records_read; ++i) {
      if (actions[i].action != rt::MixedRateActionId::device_terminal)
        continue;
      ++terminals;
      EXPECT_EQ(actions[i].timestamp_domain_identity, 201u);
      EXPECT_EQ(actions[i].timestamp, 900'000'000'000ull);
    }
    EXPECT_EQ(terminals, 1u);
    EXPECT_EQ(f.runtime.stop(), rt::Status::ok);
  }
}
