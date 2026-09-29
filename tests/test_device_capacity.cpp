#include "device_capacity/fixture.hpp"
#include "rt/src/device_manager.hpp"
#include <future>
#include <gtest/gtest.h>
namespace {
using device_capacity::Fixture;
using rt::Status;
void ready(Fixture& f) {
  ASSERT_EQ(f.configure(), Status::ok) << f.runtime.last_error();
  ASSERT_EQ(f.start(), Status::ok) << f.runtime.last_error();
}
}
TEST(RateDispatch, DeviceCapacityActualOneTwoSlotsAggregateThree) {
  Fixture f; ready(f);
  rt::MemoryPlan plan; ASSERT_TRUE(f.runtime.memory_plan(plan));
  EXPECT_EQ(plan.device_batch_queue_slots, 3u);
  EXPECT_EQ(plan.device_outstanding_capacity, 3u);
  EXPECT_EQ(f.backends[0].requested, 1u); EXPECT_EQ(f.backends[1].requested, 2u);
  rt::DeviceRateAdmissionReport report;
  ASSERT_TRUE(f.runtime.device_rate_admission_report(report));
  EXPECT_EQ(report.peak_global_in_flight, 3u);
  for (unsigned i = 0; i < 80; ++i) ASSERT_EQ(f.step(i), Status::ok) << f.runtime.last_error();
  EXPECT_EQ(f.state[0], std::byte{80});
  EXPECT_EQ(f.backends[0].submitted, 80u); EXPECT_EQ(f.backends[1].submitted, 160u);
  EXPECT_EQ(f.backends[0].max_poll, 1u); EXPECT_EQ(f.backends[1].max_poll, 2u);
  EXPECT_EQ(f.runtime.stop(), Status::ok);
  for (auto& b : f.backends) EXPECT_EQ(b.registered, b.unregistered);
}
TEST(RateDispatch, DeviceCapacityDefaultRejectsAndAggregateBoundStillApplies) {
  Fixture uniform;
  EXPECT_EQ(uniform.configure(false), Status::capacity_exceeded);
  EXPECT_EQ(uniform.backends[0].initialized, 0u);
  Fixture capped;
  ASSERT_EQ(capped.configure(true, 2), Status::ok);
  EXPECT_EQ(capped.runtime.finalize(), Status::invalid_config);
  EXPECT_EQ(capped.backends[0].initialized, 0u);
  Fixture backend_capped;
  backend_capped.backends[1].core_capacity = 1;
  ASSERT_EQ(backend_capped.configure(), Status::ok);
  EXPECT_EQ(backend_capped.runtime.finalize(), Status::invalid_config);
  EXPECT_EQ(backend_capped.backends[1].initialized, 0u);
}
TEST(CommandBatch, DeviceCapacityPolicyMalformedOrderFrozenAndUnsupported) {
  rt::Runtime r;
  EXPECT_EQ(r.set_device_capacity_policy(static_cast<rt::DeviceCapacityPolicy>(255)), Status::invalid_argument);
  EXPECT_EQ(r.set_device_capacity_policy(rt::DeviceCapacityPolicy::native_per_backend), Status::ok);
  rt::DeviceBackendHandle h;
  EXPECT_EQ(r.register_device_backend(rt::DeviceBackendRegistration{}, h), Status::invalid_argument);
  EXPECT_FALSE(h.valid());
  device_capacity::Backend b;
  auto core = b.core(); auto memory = b.memory(); auto commands = b.commands();
  EXPECT_EQ(r.register_device_backend({"core", core, nullptr, nullptr}, h), Status::invalid_argument);
  EXPECT_EQ(r.register_device_backend({"no_commands", core, &memory, nullptr}, h), Status::invalid_argument);
  EXPECT_EQ(r.register_device_backend({"no_memory", core, nullptr, &commands}, h), Status::invalid_argument);
  b.malformed = true;
  EXPECT_EQ(r.register_device_backend({"bad", core, &memory, &commands}, h), Status::invalid_argument);
  EXPECT_EQ(b.initialized, 0u);
  b.malformed = false;
  ASSERT_EQ(r.register_device_backend({"good", core, &memory, &commands}, h), Status::ok);
  EXPECT_EQ(r.set_device_capacity_policy(rt::DeviceCapacityPolicy::uniform), Status::invalid_state);
  Fixture f; ready(f);
  EXPECT_EQ(f.runtime.set_device_capacity_policy(rt::DeviceCapacityPolicy::native_per_backend), Status::invalid_state);
  ASSERT_EQ(f.runtime.stop(), Status::ok);
  EXPECT_EQ(f.runtime.set_device_capacity_policy(rt::DeviceCapacityPolicy::uniform), Status::invalid_state);
}
TEST(MemoryPlan, DeviceCapacityExactResolvedControlAccounting) {
  Fixture small, uniform, bounded;
  for (auto* f : {&uniform, &bounded}) for (auto& b : f->backends)
    b.pending_capacity = b.core_capacity = b.completion_capacity = 3;
  ASSERT_EQ(small.configure(), Status::ok);
  ASSERT_EQ(uniform.configure(false), Status::ok);
  ASSERT_EQ(bounded.configure(), Status::ok);
  std::array<rt::MemoryPlan, 3> plans;
  unsigned i = 0;
  for (auto* f : {&small, &uniform, &bounded}) {
    ASSERT_EQ(f->runtime.finalize(), Status::ok) << f->runtime.last_error();
    ASSERT_TRUE(f->runtime.memory_plan(plans[i++]));
  }
  EXPECT_EQ(plans[0].device_batch_queue_slots, 3u);
  EXPECT_EQ(plans[1].device_batch_queue_slots, 6u);
  EXPECT_EQ(plans[2].device_batch_queue_slots, 6u);
  EXPECT_EQ(plans[1].device_control_bytes, plans[2].device_control_bytes);
  EXPECT_EQ(plans[0].runtime_control_bytes, plans[1].runtime_control_bytes);
  std::size_t a = 0, b = 0;
  ASSERT_TRUE(rt::detail::DeviceManager::estimate_control_storage(2, 0, 0, 2, 3, 2, 3, 3, a, 3));
  ASSERT_TRUE(rt::detail::DeviceManager::estimate_control_storage(2, 0, 0, 2, 3, 2, 3, 3, b, 6));
  EXPECT_EQ(plans[1].device_control_bytes - plans[0].device_control_bytes, b - a);
  EXPECT_EQ(plans[1].planned_bytes - plans[0].planned_bytes, b - a);
}
TEST(MixedRateReplay, DeviceCapacityOwnerReplayPairedRecoveryAndPolicyRejection) {
  Fixture f; ready(f); auto initial = f.checkpoint(); ASSERT_FALSE(initial.empty());
  for (unsigned i = 0; i < 16; ++i) ASSERT_EQ(f.step(i), Status::ok);
  auto expected = f.state; auto active = f.artifact(initial, 16); ASSERT_FALSE(active.empty());
  ASSERT_EQ(f.runtime.replay_active(active, Fixture::apply), Status::ok) << f.runtime.last_error();
  EXPECT_EQ(f.state, expected);
  const auto checkpoint = f.checkpoint();
  Fixture other; ready(other);
  const auto providers = other.providers.load();
  EXPECT_EQ(other.runtime.replay_active(active, Fixture::apply), Status::incompatible_artifact);
  EXPECT_EQ(other.providers, providers);
  ASSERT_EQ(other.runtime.restore_checkpoint(checkpoint), Status::ok) << other.runtime.last_error();
  EXPECT_EQ(other.state, expected);
  ASSERT_EQ(other.step(16), Status::ok);
  Fixture different;
  for (auto& b : different.backends) b.pending_capacity = b.core_capacity = b.completion_capacity = 3;
  ASSERT_EQ(different.configure(false), Status::ok); ASSERT_EQ(different.start(), Status::ok);
  EXPECT_EQ(different.runtime.restore_checkpoint(checkpoint), Status::incompatible_artifact);
  EXPECT_EQ(different.providers, 0u);
  auto corrupt = active; corrupt.back() ^= std::byte{1};
  EXPECT_NE(f.runtime.replay_active(corrupt, Fixture::apply), Status::ok);
  EXPECT_EQ(f.state, expected);
  EXPECT_EQ(other.runtime.stop(), Status::ok); EXPECT_EQ(different.runtime.stop(), Status::ok); EXPECT_EQ(f.runtime.stop(), Status::ok);
}
TEST(CommandBatch, DeviceCapacityPartialInitializationAndRetainedCleanupRetry) {
  Fixture f; ASSERT_EQ(f.configure(), Status::ok); ASSERT_EQ(f.runtime.finalize(), Status::ok);
  f.backends[1].fail_initialize = true;
  EXPECT_NE(f.runtime.start(), Status::ok);
  EXPECT_GT(f.backends[0].shutdowns, 0u);
  ASSERT_EQ(f.runtime.stop(), Status::ok);
  Fixture running; ready(running);
  ASSERT_EQ(running.step(0), Status::ok);
  running.backends[1].fail_unregister = true;
  EXPECT_NE(running.runtime.stop(), Status::ok);
  EXPECT_EQ(running.backends[1].unregistered, 0u);
  EXPECT_EQ(running.runtime.stop(), Status::ok);
  EXPECT_EQ(running.backends[1].registered, running.backends[1].unregistered);
}
TEST(RateDispatch, DeviceCapacityConcurrentOwnersRemainIsolated) {
  Fixture a, b; ready(a); ready(b);
  auto run = [](Fixture& f) { for (unsigned i = 0; i < 40; ++i) if (f.step(i) != Status::ok) return false; return true; };
  auto worker = std::async(std::launch::async, [&] { return run(a); });
  EXPECT_TRUE(run(b)); EXPECT_TRUE(worker.get());
  EXPECT_EQ(a.state, b.state); EXPECT_EQ(a.providers, 120u); EXPECT_EQ(b.providers, 120u);
  EXPECT_EQ(a.runtime.stop(), Status::ok); EXPECT_EQ(b.runtime.stop(), Status::ok);
}
TEST(CommandBatch, DeviceCapacityIndependentCompletionBoundAndShutdownRetry) {
  Fixture f;
  f.backends[1].completion_capacity = 1;
  ASSERT_EQ(f.configure(true, 3, true, 1), Status::ok);
  ASSERT_EQ(f.start(), Status::ok) << f.runtime.last_error();
  EXPECT_EQ(f.backends[1].requested, 2u);
  for (unsigned i = 0; i < 12; ++i) ASSERT_EQ(f.step(i), Status::ok);
  EXPECT_EQ(f.backends[1].max_poll, 1u);
  f.backends[1].fail_stop = true;
  EXPECT_NE(f.runtime.stop(), Status::ok);
  EXPECT_EQ(f.runtime.stop(), Status::ok);
  Fixture shutdown; ready(shutdown);
  shutdown.backends[0].fail_shutdown = true;
  EXPECT_NE(shutdown.runtime.stop(), Status::ok);
  EXPECT_EQ(shutdown.runtime.stop(), Status::ok);
}
TEST(MixedRateReplay, DeviceCapacityMarkerRejectsIdenticalUniformTopology) {
  Fixture opted, uniform;
  for (auto* f : {&opted, &uniform}) for (auto& b : f->backends)
    b.pending_capacity = b.core_capacity = b.completion_capacity = 3;
  ASSERT_EQ(opted.configure(), Status::ok); ASSERT_EQ(opted.start(), Status::ok);
  ASSERT_EQ(uniform.configure(false), Status::ok); ASSERT_EQ(uniform.start(), Status::ok);
  const auto state = uniform.state;
  EXPECT_EQ(uniform.runtime.restore_checkpoint(opted.checkpoint()), Status::incompatible_artifact);
  EXPECT_EQ(uniform.state, state); EXPECT_EQ(uniform.providers, 0u);
  EXPECT_EQ(uniform.runtime.stop(), Status::ok); EXPECT_EQ(opted.runtime.stop(), Status::ok);
}
TEST(CommandBatch, DeviceCapacityActualRuntimeSlotSaturation) {
  device_capacity::Backend backend;
  backend.core_capacity = backend.pending_capacity = backend.completion_capacity = 1;
  backend.allowed = false;
  rt::Runtime runtime;
  rt::RuntimeConfig cfg;
  cfg.worker_count = 2; cfg.callback_capacity = 2;
  cfg.device_backend_capacity = 1; cfg.device_outstanding_capacity = cfg.device_completion_batch = 3;
  ASSERT_EQ(runtime.configure(cfg), Status::ok);
  ASSERT_EQ(runtime.set_device_capacity_policy(rt::DeviceCapacityPolicy::native_per_backend), Status::ok);
  auto m = backend.memory(); auto c = backend.commands(); rt::DeviceBackendHandle device;
  ASSERT_EQ(runtime.register_device_backend({"saturation", backend.core(), &m, &c}, device), Status::ok);
  struct Provider {
    device_capacity::Backend* backend;
    bool wait;
    rt::DeviceCommandBatch batch{};
    static rt::CallbackResult call(void* p, const rt::DeviceCallbackContext&, rt::DeviceCommandBatch& out) {
      auto& self = *static_cast<Provider*>(p);
      if (self.wait) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (self.backend->submitted.load() == 0) {
          if (std::chrono::steady_clock::now() > deadline) return rt::CallbackResult::error;
          std::this_thread::yield();
        }
      }
      out = self.batch;
      out.timeout_ns = 100'000'000;
      out.signals[0].value = 1;
      return rt::CallbackResult::ok;
    }
  };
  std::array<Provider, 2> providers{{{&backend, false, {}}, {&backend, true, {}}}};
  constexpr std::array names{"saturation.first", "saturation.second"};
  for (unsigned i = 0; i < 2; ++i) {
    rt::DeviceTimelineHandle timeline;
    ASSERT_EQ(runtime.register_device_timeline({names[i], device, 0}, timeline), Status::ok);
    auto& d = providers[i].batch;
    d.command_count = d.signal_count = 1;
    d.commands[0].kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
    d.commands[0].opcode = 2642; d.signals[0].timeline_handle = timeline.value;
    rt::PhaseHandle phase;
    ASSERT_EQ(runtime.register_device_batch_phase({names[i], device, Provider::call, &providers[i], d}, phase), Status::ok);
  }
  ASSERT_EQ(runtime.finalize(), Status::ok) << runtime.last_error();
  ASSERT_EQ(runtime.start(), Status::ok) << runtime.last_error();
  EXPECT_NE(runtime.step({0, std::chrono::milliseconds(1)}), Status::ok);
  rt::RuntimeMetricSnapshot metrics;
  rt::RuntimeMetricCursor cursor;
  ASSERT_EQ(runtime.metrics_snapshot(rt::RuntimeMetricWindow::cumulative, &cursor, metrics), Status::ok);
  EXPECT_EQ(metrics.samples[static_cast<std::size_t>(rt::RuntimeMetricId::device_queue_rejections)].value, 1u);
  EXPECT_EQ(backend.submitted, 1u);
  EXPECT_EQ(backend.requested, 1u);
  EXPECT_GT(backend.canceled, 0u);
  EXPECT_EQ(runtime.stop(), Status::ok);
}
TEST(CommandBatch, DeviceCapacityNativePollOverflowAndTerminalErrors) {
  Fixture overflow; ready(overflow);
  overflow.backends[0].excessive_completion = true;
  EXPECT_NE(overflow.step(0), Status::ok);
  EXPECT_EQ(overflow.runtime.stop(), Status::ok);
  for (auto status : {rt::HalV2Status::error, rt::HalV2Status::lost, rt::HalV2Status::reset_required}) {
    Fixture f; ready(f);
    f.backends[1].completion_status = status;
    EXPECT_NE(f.step(0), Status::ok);
    EXPECT_EQ(f.runtime.stop(), Status::ok);
    EXPECT_EQ(f.backends[1].registered, f.backends[1].unregistered);
  }
}
