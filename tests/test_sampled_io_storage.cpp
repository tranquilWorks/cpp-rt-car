#include "rt/src/cross_rate_data.hpp"
#include "rt/src/sampled_io.hpp"
#include "sampled_io_storage/fixture.hpp"
#include <future>
#include <gtest/gtest.h>
namespace {
using sampled_storage::Fixture;
void ready(Fixture &f, unsigned output = 4, unsigned input = 4) {
  ASSERT_EQ(f.configure(output, input), rt::Status::ok);
  ASSERT_EQ(f.start(), rt::Status::ok) << f.runtime.last_error();
}
} // namespace
TEST(RateDispatch, SampledStorageFourRealSlotsAndRepeatedWrap) {
  for (auto slots : {2u, 4u}) {
    Fixture f;
    ready(f, slots, slots);
    for (std::size_t c = 0; c < 3; ++c) {
      rt::CompiledCrossRateChannel v;
      ASSERT_TRUE(f.runtime.compiled_cross_rate_channel_at(c, v));
      EXPECT_EQ(v.snapshot_slot_count, c == 2 ? 2u : slots);
      EXPECT_EQ(v.snapshot_bytes, c == 2 ? 16u : slots * 128u);
    }
    for (unsigned tick = 0; tick < 80; ++tick) {
      ASSERT_EQ(f.step(tick), rt::Status::ok) << f.runtime.last_error();
      ASSERT_TRUE(f.valid(tick)) << tick;
    }
    EXPECT_EQ(f.backend.stats().frames_copied, 81u);
    EXPECT_EQ(f.runtime.stop(), rt::Status::ok);
    EXPECT_EQ(f.backend.stats().frames_copied, 82u);
    EXPECT_EQ(f.backend.stats().submissions, f.backend.stats().completions);
  }
}
TEST(MemoryPlan, SampledStorageExactPayloadAndControlDeltas) {
  std::array<rt::MemoryPlan, 3> plans;
  for (std::size_t i = 0; i < 3; ++i) {
    Fixture f;
    ASSERT_EQ(f.configure(i ? 4u : 2u, i == 2 ? 4u : 2u), rt::Status::ok);
    ASSERT_EQ(f.runtime.finalize(), rt::Status::ok);
    ASSERT_TRUE(f.runtime.memory_plan(plans[i]));
  }
  for (std::size_t i = 1; i < 3; ++i) {
    const auto extra = 2 * i;
    EXPECT_EQ(plans[i].cross_rate_snapshot_slot_count -
                  plans[0].cross_rate_snapshot_slot_count,
              extra);
    EXPECT_EQ(plans[i].cross_rate_snapshot_bytes -
                  plans[0].cross_rate_snapshot_bytes,
              extra * 128);
    EXPECT_EQ(plans[i].rate_plan_bytes - plans[0].rate_plan_bytes,
              extra * (128 + sizeof(rt::detail::SnapshotSlotControl)));
    EXPECT_EQ(plans[i].planned_bytes - plans[0].planned_bytes,
              plans[i].runtime_control_bytes - plans[0].runtime_control_bytes);
  }
  Fixture rejected;
  ASSERT_EQ(rejected.configure(4, 4, plans[0].planned_bytes), rt::Status::ok);
  EXPECT_EQ(rejected.runtime.finalize(), rt::Status::invalid_config);
  EXPECT_EQ(rejected.runtime.last_error(),
            "finalized memory plan exceeds memory_budget_bytes");
  EXPECT_EQ(rejected.backend.stats().submissions, 0u);
}
TEST(RateDispatch, SampledStorageInvalidCountsCorrectBeforeStart) {
  for (auto slots : {0u, 1u, 3u, 5u, UINT32_MAX}) {
    Fixture f;
    ASSERT_EQ(f.configure(slots, 4), rt::Status::ok);
    EXPECT_EQ(f.runtime.finalize(), rt::Status::invalid_argument);
    EXPECT_EQ(f.runtime.state(), rt::RuntimeState::configuring);
    EXPECT_EQ(f.backend.stats().submissions, 0u);
    f.output_spec.ring_capacity = 4;
    ASSERT_EQ(f.runtime.replace_sampled_io_channel(f.output, f.output_spec),
              rt::Status::ok);
    ASSERT_EQ(f.start(), rt::Status::ok) << f.runtime.last_error();
    EXPECT_EQ(f.runtime.replace_sampled_io_channel(f.output, f.output_spec),
              rt::Status::invalid_state);
    ASSERT_EQ(f.step(0), rt::Status::ok);
    EXPECT_TRUE(f.valid(0));
    ASSERT_EQ(f.runtime.stop(), rt::Status::ok);
  }
}
TEST(RateDispatch, SampledStorageDuplicateForeignAndShapeRejectBeforeEffects) {
  Fixture first, second;
  ASSERT_EQ(first.configure(), rt::Status::ok);
  ASSERT_EQ(second.configure(), rt::Status::ok);
  EXPECT_EQ(first.runtime.register_sampled_io_channel(first.output_spec),
            rt::Status::invalid_argument);
  EXPECT_EQ(first.runtime.replace_sampled_io_channel(second.output,
                                                     first.output_spec),
            rt::Status::invalid_handle);
  auto bad = first.input_spec;
  bad.channel_identity = 101;
  EXPECT_EQ(first.runtime.replace_sampled_io_channel(first.input, bad),
            rt::Status::invalid_argument);
  bad = first.output_spec;
  bad.element_count = 2;
  ASSERT_EQ(first.runtime.replace_sampled_io_channel(first.output, bad),
            rt::Status::ok);
  EXPECT_EQ(first.runtime.finalize(), rt::Status::invalid_argument);
  EXPECT_EQ(first.backend.stats().submissions, 0u);
  bad.element_count = UINT32_MAX;
  bad.samples_per_frame = UINT32_MAX;
  ASSERT_EQ(first.runtime.replace_sampled_io_channel(first.output, bad),
            rt::Status::ok);
  EXPECT_EQ(first.runtime.finalize(), rt::Status::capacity_exceeded);
  ASSERT_EQ(
      first.runtime.replace_sampled_io_channel(first.output, first.output_spec),
      rt::Status::ok);
  ASSERT_EQ(first.start(), rt::Status::ok);
  ASSERT_EQ(first.step(0), rt::Status::ok);
  EXPECT_TRUE(first.valid(0));
  ASSERT_EQ(first.runtime.stop(), rt::Status::ok);
}
TEST(RateDispatch, SampledStorageUnderrunOverrunAndSafeStopRetry) {
  Fixture f;
  ready(f);
  rt::SampledIoChannelStatus status;
  ASSERT_TRUE(f.runtime.sampled_io_channel_status(f.output, status));
  EXPECT_EQ(status.safety_state,
            rt::SampledIoSafetyState::startup_acknowledged);
  for (unsigned t = 0; t < 5; ++t)
    ASSERT_EQ(f.step(t), rt::Status::ok);
  f.skip = true;
  ASSERT_EQ(f.step(5), rt::Status::ok);
  for (std::size_t i = 120; i < 128; ++i)
    EXPECT_EQ(f.state[i], std::byte{});
  ASSERT_TRUE(f.runtime.sampled_io_channel_status(f.output, status));
  EXPECT_EQ(status.underruns, 1u);
  EXPECT_EQ(status.substituted_frames, 1u);
  f.skip = false;
  ASSERT_EQ(f.step(6), rt::Status::ok);
  EXPECT_TRUE(f.valid(6));
  const auto before = f.state;
  f.twice = true;
  EXPECT_EQ(f.step(7), rt::Status::callback_failed);
  EXPECT_EQ(f.second, rt::Status::invalid_state);
  EXPECT_EQ(f.state, before);
  ASSERT_TRUE(f.runtime.sampled_io_channel_status(f.output, status));
  EXPECT_EQ(status.overruns, 1u);
  ASSERT_EQ(f.backend.inject_next(rt::SampledIoLoopbackFault::completion_error),
            rt::Status::ok);
  EXPECT_EQ(f.runtime.stop(), rt::Status::device_error);
  ASSERT_TRUE(f.runtime.sampled_io_channel_status(f.output, status));
  EXPECT_EQ(status.safety_state, rt::SampledIoSafetyState::unknown);
  rt::DeviceMemoryObjectInfo object;
  EXPECT_TRUE(f.runtime.device_memory_object_at(0, object));
  ASSERT_EQ(f.runtime.stop(), rt::Status::ok);
  ASSERT_TRUE(f.runtime.sampled_io_channel_status(f.output, status));
  EXPECT_EQ(status.safety_state,
            rt::SampledIoSafetyState::shutdown_acknowledged);
  EXPECT_EQ(f.backend.stats().submissions, f.backend.stats().completions);
}
TEST(RateDispatch, SampledStorageMalformedDeviceNeverPublishes) {
  Fixture f;
  ready(f);
  ASSERT_EQ(f.step(0), rt::Status::ok);
  const auto before = f.state;
  ASSERT_EQ(
      f.backend.inject_next(rt::SampledIoLoopbackFault::malformed_sequence),
      rt::Status::ok);
  EXPECT_NE(f.step(1), rt::Status::ok);
  EXPECT_EQ(f.state, before);
  rt::SampledIoChannelStatus status;
  ASSERT_TRUE(f.runtime.sampled_io_channel_status(f.input, status));
  EXPECT_EQ(status.accepted_frames, 2u); // Explicit initial frame plus tick0.
  EXPECT_EQ(status.last_sequence, 2u);
  EXPECT_EQ(f.runtime.stop(), rt::Status::ok);
}
TEST(MixedRateReplay, SampledStorageOriginReplayAndForeignCapacityRejection) {
  Fixture f;
  ready(f);
  const auto initial = f.checkpoint();
  ASSERT_FALSE(initial.empty());
  for (unsigned t = 0; t < 32; ++t)
    ASSERT_EQ(f.step(t), rt::Status::ok);
  const auto expected = f.state;
  const auto artifact = f.artifact(initial, 32);
  ASSERT_FALSE(artifact.empty());
  ASSERT_EQ(f.runtime.replay_active(artifact, Fixture::apply), rt::Status::ok)
      << f.runtime.last_error();
  EXPECT_EQ(f.state, expected);
  auto corrupt = artifact;
  corrupt.back() ^= std::byte{1};
  const auto submits = f.backend.stats().submissions;
  EXPECT_NE(f.runtime.replay_active(corrupt, Fixture::apply), rt::Status::ok);
  EXPECT_EQ(f.state, expected);
  EXPECT_EQ(f.backend.stats().submissions, submits);
  const auto checkpoint = f.checkpoint(32);
  ASSERT_FALSE(checkpoint.empty());
  for (auto slots : {2u, 4u}) {
    Fixture other;
    ready(other, slots, slots);
    const auto state = other.state;
    const auto prior = other.backend.stats().submissions;
    EXPECT_EQ(other.runtime.replay_active(artifact, Fixture::apply),
              rt::Status::incompatible_artifact);
    EXPECT_EQ(other.state, state);
    EXPECT_EQ(other.backend.stats().submissions, prior);
    if (slots == 2) {
      EXPECT_EQ(other.runtime.restore_checkpoint(checkpoint),
                rt::Status::incompatible_artifact);
      EXPECT_EQ(other.state, state);
      EXPECT_EQ(other.backend.stats().submissions, prior);
    } else {
      ASSERT_EQ(other.runtime.restore_checkpoint(checkpoint), rt::Status::ok)
          << other.runtime.last_error();
      EXPECT_EQ(other.state, expected);
      ASSERT_EQ(other.step(32), rt::Status::ok);
      EXPECT_TRUE(other.valid(32));
    }
    ASSERT_EQ(other.runtime.stop(), rt::Status::ok);
  }
  EXPECT_EQ(f.runtime.stop(), rt::Status::ok);
}
TEST(RateDispatch, SampledStorageConcurrentOwnersRemainIsolated) {
  Fixture a, b;
  ready(a);
  ready(b, 2, 4);
  auto run = [](Fixture &f) {
    for (unsigned i = 0; i < 64; ++i)
      if (f.step(i) != rt::Status::ok || !f.valid(i))
        return false;
    return true;
  };
  auto first = std::async(std::launch::async, [&] { return run(a); });
  auto second = std::async(std::launch::async, [&] { return run(b); });
  EXPECT_TRUE(first.get());
  EXPECT_TRUE(second.get());
  EXPECT_EQ(a.backend.stats().submissions, 65u);
  EXPECT_EQ(b.backend.stats().submissions, 65u);
  ASSERT_EQ(a.runtime.stop(), rt::Status::ok);
  EXPECT_EQ(b.backend.stats().submissions, 65u);
  ASSERT_EQ(b.runtime.stop(), rt::Status::ok);
}
TEST(CrossRateData, SampledStorageSlotSelectionRejectsTransactionally) {
  rt::detail::CrossRateChannelSpec cross;
  cross.payload_size = 128;
  const auto bytes =
      sampled_storage::frame(101, 1, rt::SampledIoFrameStatus::initial);
  cross.initial_sample.assign(bytes.begin(), bytes.end());
  rt::detail::SampledIoChannelSpec sample;
  sample.channel = {42, 0};
  sample.channel_identity = 101;
  sample.ring_capacity = 4;
  sample.encoding = rt::SampledIoEncoding::signed_int16_le;
  sample.element_count = 1;
  sample.samples_per_frame = 4;
  sample.initial_frame = cross.initial_sample;
  std::array<std::size_t, 1> counts{99};
  rt::detail::SampledIoCompileDiagnostic diagnostic;
  auto compile = [&](auto specs) {
    return rt::detail::sampled_io_snapshot_slots(
        42, specs, std::span(&cross, 1), counts, diagnostic);
  };
  sample.channel = {41, 0};
  EXPECT_EQ(compile(std::span(&sample, 1)), rt::Status::invalid_argument);
  EXPECT_EQ(counts[0], 99u);
  sample.channel = {42, 0};
  std::array duplicate{sample, sample};
  EXPECT_EQ(compile(std::span(duplicate)), rt::Status::invalid_argument);
  EXPECT_EQ(counts[0], 99u);
  EXPECT_EQ(compile(std::span(&sample, 1)), rt::Status::ok);
  EXPECT_EQ(counts[0], 4u);
}

TEST(RateDispatch, SampledStorageAggregateBytesAreBoundedAndRetryable) {
  constexpr std::size_t channels = 9, bytes = rt::cross_rate_payload_capacity;
  sampled_storage::Clock clock;
  std::vector<std::byte> storage(bytes * (channels + 1));
  rt::SampledIoLoopbackBackend backend({16, 1, storage.size(), 1, 7});
  rt::Runtime runtime(clock);
  rt::RuntimeConfig cfg;
  cfg.callback_capacity = channels + 1;
  cfg.worker_count = 1;
  cfg.executor_queue_capacity = 32;
  cfg.task_scratch_slots = 32;
  cfg.device_backend_capacity = 1;
  cfg.device_buffer_capacity = 1;
  cfg.device_outstanding_capacity = 16;
  cfg.device_completion_batch = 16;
  cfg.memory_budget_bytes = 64 * 1024 * 1024;
  ASSERT_EQ(runtime.configure(cfg), rt::Status::ok);
  ASSERT_EQ(runtime.set_rate_execution_policy({128}), rt::Status::ok);
  rt::DeviceBackendHandle device;
  ASSERT_EQ(
      runtime.register_device_backend(backend.hal_v2_registration(), device),
      rt::Status::ok);
  rt::DeviceMemoryDomainHandle memory;
  rt::HalV2MemoryDomain desc;
  ASSERT_TRUE(runtime.device_memory_domain_at(device, 0, memory, desc));
  rt::DeviceBufferHandle buffer;
  ASSERT_EQ(
      runtime.register_device_buffer({"aggregate.buffer",
                                      device,
                                      memory,
                                      storage,
                                      {},
                                      storage.size(),
                                      rt::HalV2MemoryOwnership::borrowed_host,
                                      15,
                                      rt::HalV2MemoryCoherency::host_coherent,
                                      rt::hal_v2_memory_sync_none},
                                     buffer),
      rt::Status::ok);
  rt::RateDomainHandle producers, consumers;
  ASSERT_EQ(runtime.register_rate_domain({"aggregate.producers",
                                          sampled_storage::period, 1,
                                          sampled_storage::period, 1'000'000},
                                         producers),
            rt::Status::ok);
  ASSERT_EQ(runtime.register_rate_domain({"aggregate.consumers",
                                          sampled_storage::period, 1,
                                          sampled_storage::period, 1'000'000},
                                         consumers),
            rt::Status::ok);
  rt::PhaseHandle consumer;
  ASSERT_EQ(runtime.register_callback({"aggregate.consumer",
                                       [](void *, const rt::CallbackContext &) {
                                         return rt::CallbackResult::ok;
                                       },
                                       nullptr},
                                      consumer),
            rt::Status::ok);
  ASSERT_EQ(runtime.bind_phase_to_rate_domain(consumer, consumers),
            rt::Status::ok);
  std::array<rt::SampledIoChannelRegistration, channels> sampled;
  std::array<std::vector<std::byte>, channels> initial;
  for (std::size_t i = 0; i < channels; ++i) {
    rt::DeviceTimelineHandle timeline;
    ASSERT_EQ(
        runtime.register_device_timeline(
            {"aggregate.timeline." + std::to_string(i), device, 0}, timeline),
        rt::Status::ok);
    rt::DeviceCommandBatch declaration;
    declaration.signal_count = 1;
    declaration.signals[0].timeline_handle = timeline.value;
    declaration.command_count = 1;
    auto &cmd = declaration.commands[0];
    cmd.kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
    cmd.opcode = 17;
    cmd.buffer_count = 2;
    cmd.buffers[0] = {buffer.value, RTFW_DEVICE_ACCESS_READ, 0, 0, bytes};
    cmd.buffers[1] = {buffer.value, RTFW_DEVICE_ACCESS_WRITE, 0,
                      bytes * (i + 1), bytes};
    rt::PhaseHandle phase;
    ASSERT_EQ(
        runtime.register_device_batch_phase(
            {"aggregate.device." + std::to_string(i), device,
             [](void *, const rt::DeviceCallbackContext &,
                rt::DeviceCommandBatch &) { return rt::CallbackResult::error; },
             nullptr, declaration},
            phase),
        rt::Status::ok);
    const std::array roles{rt::DeviceRatePayloadRole::input,
                           rt::DeviceRatePayloadRole::output};
    ASSERT_EQ(runtime.bind_device_phase_to_rate_domain(
                  {phase, producers, 1'000'000, 1, roles}),
              rt::Status::ok);
    initial[i].resize(bytes);
    rt::SampledIoFrameHeader h;
    h.channel_identity = 101 + i;
    h.sequence = h.trigger_sequence = 1;
    h.sample_count = static_cast<std::uint32_t>((bytes - sizeof(h)) / 2);
    h.encoding = 1;
    h.timestamp_domain_identity = 7;
    h.sample_interval_ns = 1000;
    h.calibration_identity = 303;
    h.trigger_identity = 404;
    h.payload_checksum = rt::sampled_io_payload_checksum(
        std::span(initial[i]).subspan(sizeof(h)));
    std::memcpy(initial[i].data(), &h, sizeof(h));
    auto &s = sampled[i];
    ASSERT_EQ(runtime.register_cross_rate_channel(
                  {"aggregate.channel." + std::to_string(i),
                   phase,
                   consumer,
                   bytes,
                   initial[i],
                   rt::CrossRateMode::sample_and_hold,
                   sampled_storage::period,
                   {1, bytes},
                   {}},
                  s.channel),
              rt::Status::ok);
    s.channel_identity = h.channel_identity;
    s.element_count = 1;
    s.samples_per_frame = h.sample_count;
    s.units_identity = 202;
    s.calibration_identity = 303;
    s.sample_period_ns = 1000;
    s.timestamp_domain_identity = 7;
    s.clock_domain_identity = 1;
    s.trigger_identity = 404;
    s.ring_capacity = 4;
    s.initial_sequence = 1;
    s.maximum_age_ns = sampled_storage::period;
    s.initial_frame = initial[i];
    ASSERT_EQ(runtime.register_sampled_io_channel(s), rt::Status::ok);
  }
  EXPECT_EQ(runtime.finalize(), rt::Status::capacity_exceeded)
      << runtime.last_error();
  EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
  EXPECT_EQ(backend.stats().submissions, 0u);
  for (auto &s : sampled) {
    s.ring_capacity = 2;
    ASSERT_EQ(runtime.replace_sampled_io_channel(s.channel, s), rt::Status::ok);
  }
  ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();
  rt::MemoryPlan plan;
  ASSERT_TRUE(runtime.memory_plan(plan));
  EXPECT_EQ(plan.cross_rate_snapshot_slot_count, channels * 2);
  EXPECT_EQ(plan.cross_rate_snapshot_bytes, channels * 2 * bytes);
  EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(RateDispatch, SampledStorageDelayedDeviceInputUsesDeclaredInitial) {
  Fixture f;
  ASSERT_EQ(f.configure(), rt::Status::ok);
  ASSERT_EQ(f.runtime.replace_rate_domain(
                f.rates[1], {"storage.device.rate", sampled_storage::period * 2,
                             1, sampled_storage::period, 10'000'000}),
            rt::Status::ok);
  ASSERT_EQ(f.runtime.replace_cross_rate_channel(
                f.input, {"storage.input",
                          f.phases[1],
                          f.phases[2],
                          128,
                          f.initial_input,
                          rt::CrossRateMode::sample_and_hold,
                          0,
                          {1, 128},
                          {}}),
            rt::Status::ok);
  f.input_spec.maximum_age_ns = 0;
  ASSERT_EQ(f.runtime.replace_sampled_io_channel(f.input, f.input_spec),
            rt::Status::ok);
  ASSERT_EQ(f.start(), rt::Status::ok) << f.runtime.last_error();
  ASSERT_EQ(f.step(0), rt::Status::ok);
  ASSERT_TRUE(f.valid(0));
  ASSERT_EQ(f.step(1), rt::Status::ok);
  EXPECT_EQ(f.count(), 2u);
  EXPECT_TRUE(f.read.sampled_substituted);
  EXPECT_EQ(f.read.sampled_sequence, 1u);
  for (std::size_t i = 120; i < 128; ++i)
    EXPECT_EQ(f.state[i], std::byte{});
  rt::SampledIoChannelStatus status;
  ASSERT_TRUE(f.runtime.sampled_io_channel_status(f.input, status));
  EXPECT_EQ(status.stale_frames, 1u);
  EXPECT_EQ(status.substituted_frames, 1u);
  ASSERT_EQ(f.runtime.stop(), rt::Status::ok);
}
TEST(CrossRateData, SampledStorageFourSlotsRetainExactlyFourGenerations) {
  rt::detail::SnapshotStore store;
  ASSERT_EQ(rt::detail::SnapshotStore::create(8, 4, store), rt::Status::ok);
  EXPECT_EQ(store.slot_count(), 4u);
  EXPECT_EQ(store.payload_storage_bytes(), 32u);
  std::array<std::byte, 8> frame{}, out{};
  for (std::uint64_t generation = 1; generation <= 4; ++generation) {
    frame.fill(std::byte(generation));
    ASSERT_EQ(store.publish(generation, frame),
              rt::detail::SnapshotStoreResult::ok);
  }
  frame.fill(std::byte{5});
  EXPECT_EQ(store.publish(5, frame),
            rt::detail::SnapshotStoreResult::capacity_exceeded);
  for (std::uint64_t generation = 1; generation <= 4; ++generation) {
    ASSERT_EQ(
        store.copy(generation, out, rt::detail::SnapshotRetention::retain),
        rt::detail::SnapshotStoreResult::ok);
    EXPECT_TRUE(std::all_of(out.begin(), out.end(), [=](auto b) {
      return b == std::byte(generation);
    }));
  }
  ASSERT_EQ(store.retire(1), rt::detail::SnapshotStoreResult::ok);
  ASSERT_EQ(store.publish(5, frame), rt::detail::SnapshotStoreResult::ok);
  out.fill(std::byte{9});
  EXPECT_EQ(store.copy(1, out, rt::detail::SnapshotRetention::retain),
            rt::detail::SnapshotStoreResult::stale_generation);
  EXPECT_TRUE(std::all_of(out.begin(), out.end(),
                          [](auto b) { return b == std::byte{9}; }));
}
