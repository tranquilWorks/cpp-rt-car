// Public-SDK-only fixture, shared verbatim by the installed consumer and new
// storage regressions. No Runtime private header or old fixture modification.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <rt/loopback_backend.hpp>
#include <vector>

namespace sampled_storage {
inline constexpr std::size_t frame_bytes = 128;
inline constexpr std::uint64_t period = 100'000'000;
using Frame = std::array<std::byte, frame_bytes>;
inline Frame frame(std::uint64_t id, std::uint64_t domain,
                   rt::SampledIoFrameStatus status) {
  Frame bytes{};
  rt::SampledIoFrameHeader h;
  h.channel_identity = id;
  h.sequence = h.trigger_sequence = 1;
  h.sample_count = 4;
  h.encoding = 1;
  h.timestamp_domain_identity = domain;
  h.sample_interval_ns = 1000;
  h.trigger_identity = 404;
  h.calibration_identity = 303;
  h.status = static_cast<std::uint32_t>(status);
  h.payload_checksum =
      rt::sampled_io_payload_checksum(std::span(bytes).subspan(120));
  std::memcpy(bytes.data(), &h, sizeof(h));
  return bytes;
}
struct Clock final : rt::RuntimeClock {
  std::atomic<std::uint64_t> now{1000};
  std::uint64_t now_ns() noexcept override { return now.load(); }
};
// Borrowed transfer storage deliberately has cache-line alignment. MSVC's
// padding diagnostic is expected for this fixture, not a layout defect.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct Fixture {
  Clock clock;
  alignas(64) std::array<std::byte, frame_bytes * 4> storage{};
  rt::SampledIoLoopbackBackend backend{{4, 1, 4096, 1, 7}};
  rt::Runtime runtime{clock};
  rt::DeviceBackendHandle device;
  std::array<rt::PhaseHandle, 3> phases{};
  std::array<rt::RateDomainHandle, 3> rates{};
  rt::CrossRateChannelHandle output, input, ordinary;
  rt::DeviceCommandBatch declaration;
  rt::SampledIoChannelRegistration output_spec, input_spec;
  Frame initial_output = frame(101, 1, rt::SampledIoFrameStatus::initial),
        initial_input = frame(202, 7, rt::SampledIoFrameStatus::initial),
        safe = frame(101, 1, rt::SampledIoFrameStatus::safe), produced{};
  std::array<std::byte, 144> state{};
  rt::CrossRateReadResult read{};
  bool skip = false, twice = false;
  rt::Status second = rt::Status::ok;
  std::uint64_t count() const noexcept {
    std::uint64_t n;
    std::memcpy(&n, state.data() + 136, 8);
    return n;
  }
  static rt::CallbackResult produce(void *p, const rt::CallbackContext &c) {
    auto &s = *static_cast<Fixture *>(p);
    if (!c.rate_release)
      return rt::CallbackResult::error;
    std::array<std::byte, 8> plain{};
    for (std::size_t i = 0; i < 8; ++i)
      plain[i] = std::byte((c.frame.frame_index * 17 + i) & 255);
    if (c.rate_release->publish(s.ordinary, plain) != rt::Status::ok)
      return rt::CallbackResult::error;
    if (s.skip)
      return rt::CallbackResult::ok;
    s.produced = s.initial_output;
    std::copy(plain.begin(), plain.end(), s.produced.begin() + 120);
    rt::SampledIoFrameHeader h;
    std::memcpy(&h, s.produced.data(), sizeof(h));
    h.sequence = c.rate_release->domain_release_sequence + 2;
    h.release_generation = h.sequence - 1;
    h.trigger_sequence = h.sequence;
    h.first_sample_timestamp = c.rate_release->logical_release_ns;
    h.status = static_cast<std::uint32_t>(rt::SampledIoFrameStatus::produced);
    h.payload_checksum =
        rt::sampled_io_payload_checksum(std::span(s.produced).subspan(120));
    std::memcpy(s.produced.data(), &h, sizeof(h));
    if (c.rate_release->publish(s.output, s.produced) != rt::Status::ok)
      return rt::CallbackResult::error;
    if (s.twice) {
      s.second = c.rate_release->publish(s.output, s.produced);
      return rt::CallbackResult::error;
    }
    return rt::CallbackResult::ok;
  }
  static rt::CallbackResult provide(void *p, const rt::DeviceCallbackContext &,
                                    rt::DeviceCommandBatch &batch) {
    auto &s = *static_cast<Fixture *>(p);
    rt::DeviceTimelineInfo t;
    if (!s.runtime.device_timeline_at(s.device, 0, t))
      return rt::CallbackResult::error;
    batch = s.declaration;
    batch.timeout_ns = 10'000'000;
    batch.signals[0].value = t.last_accepted_value + 1;
    return rt::CallbackResult::ok;
  }
  static rt::CallbackResult consume(void *p, const rt::CallbackContext &c) {
    auto &s = *static_cast<Fixture *>(p);
    rt::CrossRateReadResult plain;
    if (!c.rate_release ||
        c.rate_release->copy(s.input, std::span(s.state).first(128), s.read) !=
            rt::CrossRateReadStatus::ok ||
        c.rate_release->copy(s.ordinary, std::span(s.state).subspan(128, 8),
                             plain) != rt::CrossRateReadStatus::ok)
      return rt::CallbackResult::error;
    auto n = s.count() + 1;
    std::memcpy(s.state.data() + 136, &n, 8);
    return rt::CallbackResult::ok;
  }
  rt::Status configure(std::uint32_t slots = 4, std::uint32_t input_slots = 4,
                       std::size_t budget = 64 * 1024 * 1024) {
    rt::Status status;
#define STORAGE_TRY(x)                                                         \
  do {                                                                         \
    status = (x);                                                              \
    if (status != rt::Status::ok)                                              \
      return status;                                                           \
  } while (false)
    rt::RuntimeConfig cfg;
    cfg.callback_capacity = 3;
    cfg.worker_count = 2;
    cfg.executor_queue_capacity = 16;
    cfg.task_scratch_slots = 16;
    cfg.trace_capacity = 32;
    cfg.device_backend_capacity = 1;
    cfg.device_buffer_capacity = 1;
    cfg.device_outstanding_capacity = 4;
    cfg.device_completion_batch = 4;
    cfg.memory_budget_bytes = budget;
    cfg.snapshot_max_bytes = 1024 * 1024;
    cfg.input_log_max_bytes = 1024 * 1024;
    cfg.replay_input_capacity = 128;
    STORAGE_TRY(runtime.configure(cfg));
    rt::CpuMemoryPolicy cpu;
    cpu.thread_policy_count = 1;
    cpu.thread_policies[0].role = rt::thread_role_executor_worker;
    cpu.thread_policies[0].policy.wait_strategy = rt::WaitStrategy::park;
    STORAGE_TRY(runtime.set_cpu_memory_policy(cpu));
    STORAGE_TRY(runtime.set_rate_execution_policy({32, 264, 1, 1, 4096}));
    STORAGE_TRY(runtime.set_mixed_rate_closure_policy(
        {264,
         4096,
         4096,
         1024 * 1024,
         128,
         rt::MixedRateOverflowPolicy::overwrite_committed,
         true,
         true,
         {}}));
    STORAGE_TRY(backend.add_route({17, 101, 202, 7, 303, 404}));
    STORAGE_TRY(
        runtime.register_device_backend(backend.hal_v2_registration(), device));
    rt::DeviceMemoryDomainHandle domain;
    rt::HalV2MemoryDomain description;
    if (!runtime.device_memory_domain_at(device, 0, domain, description))
      return rt::Status::internal_error;
    rt::DeviceBufferHandle buffer;
    STORAGE_TRY(runtime.register_device_buffer(
        {"storage.buffer",
         device,
         domain,
         storage,
         {},
         storage.size(),
         rt::HalV2MemoryOwnership::borrowed_host,
         RTFW_DEVICE_BUFFER_HOST_READ | RTFW_DEVICE_BUFFER_HOST_WRITE |
             RTFW_DEVICE_BUFFER_DEVICE_READ | RTFW_DEVICE_BUFFER_DEVICE_WRITE,
         rt::HalV2MemoryCoherency::host_coherent,
         rt::hal_v2_memory_sync_none},
        buffer));
    rt::DeviceTimelineHandle timeline;
    STORAGE_TRY(runtime.register_device_timeline(
        {"storage.timeline", device, 0}, timeline));
    declaration.command_count = 1;
    declaration.signal_count = 1;
    declaration.signals[0].timeline_handle = timeline.value;
    auto &cmd = declaration.commands[0];
    cmd.kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
    cmd.opcode = 17;
    cmd.buffer_count = 2;
    cmd.buffers[0] = {buffer.value, RTFW_DEVICE_ACCESS_READ, 0, 0,
                      frame_bytes * 2};
    cmd.buffers[1] = {buffer.value, RTFW_DEVICE_ACCESS_WRITE, 0,
                      frame_bytes * 2, frame_bytes * 2};
    STORAGE_TRY(runtime.register_callback({"storage.producer", produce, this},
                                          phases[0]));
    STORAGE_TRY(runtime.register_device_batch_phase(
        {"storage.device", device, provide, this, declaration}, phases[1]));
    STORAGE_TRY(runtime.register_callback({"storage.consumer", consume, this},
                                          phases[2]));
    const std::array<std::string_view, 3> names{"storage.producer.rate",
                                                "storage.device.rate",
                                                "storage.consumer.rate"};
    for (std::size_t i = 0; i < 3; ++i)
      STORAGE_TRY(runtime.register_rate_domain(
          {names[i], period, 1, period, 10'000'000}, rates[i]));
    STORAGE_TRY(runtime.bind_phase_to_rate_domain(phases[0], rates[0]));
    STORAGE_TRY(runtime.bind_phase_to_rate_domain(phases[2], rates[2]));
    const std::array roles{rt::DeviceRatePayloadRole::input,
                           rt::DeviceRatePayloadRole::output};
    rt::DeviceRatePhaseBinding binding{phases[1], rates[1], 10'000'000, 2,
                                       roles};
    binding.simulation = rt::DeviceRateSimulationPolicy{5'000'000'000};
    STORAGE_TRY(runtime.bind_device_phase_to_rate_domain(binding));
    STORAGE_TRY(
        runtime.register_cross_rate_channel({"storage.output",
                                             phases[0],
                                             phases[1],
                                             frame_bytes,
                                             initial_output,
                                             rt::CrossRateMode::sample_and_hold,
                                             period,
                                             {},
                                             {0, frame_bytes}},
                                            output));
    STORAGE_TRY(
        runtime.register_cross_rate_channel({"storage.input",
                                             phases[1],
                                             phases[2],
                                             frame_bytes,
                                             initial_input,
                                             rt::CrossRateMode::sample_and_hold,
                                             period,
                                             {1, frame_bytes},
                                             {}},
                                            input));
    std::array<std::byte, 8> zero{};
    STORAGE_TRY(runtime.register_cross_rate_channel(
        {"storage.ordinary", phases[0], phases[2], 8, zero,
         rt::CrossRateMode::sample_and_hold, period},
        ordinary));
    output_spec.channel = output;
    output_spec.direction = rt::SampledIoDirection::output;
    output_spec.channel_identity = 101;
    output_spec.element_count = 1;
    output_spec.samples_per_frame = 4;
    output_spec.units_identity = 202;
    output_spec.calibration_identity = 303;
    output_spec.sample_period_ns = 1000;
    output_spec.timestamp_domain_identity = 1;
    output_spec.clock_domain_identity = 1;
    output_spec.trigger_identity = 404;
    output_spec.ring_capacity = slots;
    output_spec.initial_sequence = 1;
    output_spec.maximum_age_ns = period;
    output_spec.underrun_policy = rt::SampledIoUnderrunPolicy::substitute_safe;
    output_spec.safe_transition_timeout_ns = 100'000'000;
    output_spec.initial_frame = initial_output;
    output_spec.startup_safe_frame = output_spec.failure_safe_frame =
        output_spec.shutdown_safe_frame = safe;
    input_spec = output_spec;
    input_spec.channel = input;
    input_spec.direction = rt::SampledIoDirection::input;
    input_spec.channel_identity = 202;
    input_spec.timestamp_domain_identity = 7;
    input_spec.ring_capacity = input_slots;
    input_spec.underrun_policy = rt::SampledIoUnderrunPolicy::fail_release;
    input_spec.stale_policy = rt::SampledIoStalePolicy::substitute_initial;
    input_spec.initial_frame = initial_input;
    input_spec.safe_transition_timeout_ns = 0;
    input_spec.startup_safe_frame = input_spec.failure_safe_frame =
        input_spec.shutdown_safe_frame = {};
    STORAGE_TRY(runtime.register_sampled_io_channel(output_spec));
    STORAGE_TRY(runtime.register_sampled_io_channel(input_spec));
    STORAGE_TRY(runtime.register_state({"storage.state", 1, state}));
#undef STORAGE_TRY
    return rt::Status::ok;
  }
  rt::Status start() {
    auto s = runtime.finalize();
    return s == rt::Status::ok ? runtime.start() : s;
  }
  rt::HostFrameContext context(std::uint64_t tick) const {
    return {tick, std::chrono::nanoseconds(period), std::nullopt,
            1000 + tick * period};
  }
  rt::Status step(std::uint64_t tick) {
    auto c = context(tick);
    clock.now = *c.nominal_release_ns;
    return runtime.step(c);
  }
  bool valid(std::uint64_t tick) const {
    rt::SampledIoFrameHeader h;
    std::memcpy(&h, state.data(), sizeof(h));
    if (count() != tick + 1 || h.sequence != tick + 2 ||
        h.release_generation != tick + 1 || h.channel_identity != 202 ||
        h.timestamp_domain_identity != 7 || read.sampled_sequence != tick + 2 ||
        read.age_ns != 0 ||
        h.payload_checksum !=
            rt::sampled_io_payload_checksum(std::span(state).subspan(120, 8)))
      return false;
    for (std::size_t i = 0; i < 8; ++i)
      if (state[120 + i] != std::byte((tick * 17 + i) & 255) ||
          state[128 + i] != state[120 + i])
        return false;
    return true;
  }
  std::vector<std::byte> checkpoint(std::uint64_t tick = 0) {
    std::size_t n = 0;
    rt::ArtifactWriteResult out;
    if (runtime.checkpoint_size(n) != rt::Status::ok)
      return {};
    std::vector<std::byte> bytes(n);
    if (runtime.write_checkpoint(tick, bytes, out) != rt::Status::ok)
      return {};
    bytes.resize(out.bytes_written);
    return bytes;
  }
  std::vector<std::byte> artifact(std::span<const std::byte> initial,
                                  std::size_t n) {
    std::array<rt::ReplayInputRecord, 128> records{};
    if (n > records.size())
      return {};
    for (std::size_t i = 0; i < n; ++i)
      records[i] = {context(i), 1, {}};
    rt::ArtifactWriteResult out;
    std::vector<std::byte> bytes(1024 * 1024);
    if (runtime.write_active_replay_artifact(
            initial, std::span(records).first(n), bytes, out) != rt::Status::ok)
      return {};
    bytes.resize(out.bytes_written);
    return bytes;
  }
  static rt::CallbackResult apply(void *, const rt::ReplayInputView &) {
    return rt::CallbackResult::ok;
  }
};
} // namespace sampled_storage
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#ifndef RTFW_SAMPLED_STORAGE_NO_MAIN
int main(int argc, char **argv) {
  const unsigned slots =
      argc > 1 && std::string_view(argv[1]) == "--two" ? 2u : 4u;
  sampled_storage::Fixture f;
  auto s = f.configure(slots, slots);
  if (s == rt::Status::ok)
    s = f.start();
  if (s != rt::Status::ok) {
    std::fprintf(stderr, "storage start %d: %.*s\n", static_cast<int>(s),
                 static_cast<int>(f.runtime.last_error().size()),
                 f.runtime.last_error().data());
    return 1;
  }
  rt::MemoryPlan plan;
  if (!f.runtime.memory_plan(plan) ||
      plan.cross_rate_snapshot_slot_count != 2 * slots + 2 ||
      plan.cross_rate_snapshot_bytes != 2 * slots * 128 + 16)
    return 2;
  const auto initial = f.checkpoint();
  if (initial.empty())
    return 3;
  if (argc > 2) {
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char *>(initial.data()),
              static_cast<std::streamsize>(initial.size()));
    if (!out)
      return 4;
  }
  for (std::uint64_t i = 0; i < 32; ++i)
    if (f.step(i) != rt::Status::ok || !f.valid(i))
      return 5;
  const auto expected = f.state;
  const auto artifact = f.artifact(initial, 32);
  if (artifact.empty() ||
      f.runtime.replay_active(artifact, decltype(f)::apply) != rt::Status::ok ||
      f.state != expected)
    return 6;
  if (argc > 2) {
    std::ofstream out(std::string(argv[2]) + ".active", std::ios::binary);
    out.write(reinterpret_cast<const char *>(artifact.data()),
              static_cast<std::streamsize>(artifact.size()));
    if (!out)
      return 8;
  }
  if (f.runtime.stop() != rt::Status::ok)
    return 7;
  std::printf("sampled storage slots=%u wrap32 replay PASS\n", slots);
  return 0;
}
#endif
