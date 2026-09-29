// M26-04R4 fixture derived from preserved M26-04R3 public consumer.
// Public-SDK-only fixture, shared verbatim by the installed consumer and new
// recovery regressions. No Runtime private header or old fixture modification.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <rt/runtime.hpp>
#include <rt/xdma_backend.hpp>
#include <thread>
#include <vector>

namespace sampled_simulation {
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

// Owned deterministic card. Every byte passes through the actual native XDMA
// worker and Driver API v2. Backend extensions retain native instances.
struct Card {
  Frame input{}, output{};
  std::uint32_t sequence = 0;
  std::atomic<std::uint64_t> now{9000}, uploads{0}, downloads{0}, acks{0},
      shutdown_calls{0}, shutdowns{0};
  std::atomic<bool> live{false}, fail_shutdown{false}, missing_ack{false},
      fail_initialize{false};
  std::atomic<bool> hold{false}, entered{false};
  std::atomic<std::uint64_t> event_timeout{0};
  rt::XdmaDriverApi api() noexcept {
    rt::XdmaDriverApi a;
    a.struct_size = sizeof(a);
    a.api_version = rt::xdma_driver_api_version_2;
    a.user_data = this;
    a.initialize = [](void *p) noexcept {
      auto &card = *static_cast<Card *>(p);
      if (card.fail_initialize.exchange(false))
        return rt::XdmaDriverResult::io_error;
      card.live = true;
      return rt::XdmaDriverResult::success;
    };
    a.shutdown = [](void *p) noexcept {
      auto &s = *static_cast<Card *>(p);
      ++s.shutdown_calls;
      if (s.fail_shutdown.exchange(false))
        return rt::XdmaDriverResult::io_error;
      if (s.live.exchange(false))
        ++s.shutdowns;
      return rt::XdmaDriverResult::success;
    };
    a.reset = [](void *) noexcept { return rt::XdmaDriverResult::success; };
    a.request_stop = [](void *p) noexcept {
      static_cast<Card *>(p)->hold = false;
      return rt::XdmaDriverResult::success;
    };
    a.monotonic_time_ns = [](void *p) noexcept {
      return static_cast<Card *>(p)->now.load();
    };
    a.transfer = [](void *p, rt::XdmaDirection direction, std::uint32_t channel,
                    std::uint64_t offset, void *host,
                    std::uint64_t bytes) noexcept {
      auto &card = *static_cast<Card *>(p);
      if (!card.live || channel || !host)
        return rt::XdmaTransferResult{rt::XdmaDriverResult::invalid_value, 0};
      if (direction == rt::XdmaDirection::host_to_card && offset == 0 &&
          bytes == 128) {
        std::memcpy(card.input.data(), host, 128);
        ++card.uploads;
      } else if (direction == rt::XdmaDirection::card_to_host &&
                 offset == 128 && bytes == 128) {
        std::memcpy(host, card.output.data(), 128);
        ++card.downloads;
      } else
        return rt::XdmaTransferResult{rt::XdmaDriverResult::invalid_value, 0};
      return rt::XdmaTransferResult{rt::XdmaDriverResult::success, bytes};
    };
    a.control_read32 = [](void *, std::uint32_t) noexcept {
      return rt::XdmaControlReadResult{rt::XdmaDriverResult::invalid_value, 0};
    };
    a.control_write32 = [](void *p, std::uint32_t offset,
                           std::uint32_t value) noexcept {
      if (offset != 0)
        return rt::XdmaDriverResult::invalid_value;
      static_cast<Card *>(p)->sequence = value;
      return rt::XdmaDriverResult::success;
    };
    a.wait_user_event = [](void *p, std::uint32_t index,
                           std::uint64_t timeout) noexcept {
      auto &s = *static_cast<Card *>(p);
      if (index || !timeout)
        return rt::XdmaUserEventResult{rt::XdmaDriverResult::invalid_value, 0};
      s.event_timeout = timeout;
      if (s.hold) {
        s.entered = true;
        const auto limit =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (s.hold && std::chrono::steady_clock::now() < limit)
          std::this_thread::yield();
        if (s.hold)
          return rt::XdmaUserEventResult{rt::XdmaDriverResult::timeout, 0};
      }
      if (s.missing_ack)
        return rt::XdmaUserEventResult{rt::XdmaDriverResult::timeout, 0};
      rt::SampledIoFrameHeader in, out;
      std::memcpy(&in, s.input.data(), 120);
      s.output = frame(202, 1, rt::SampledIoFrameStatus::produced);
      std::memcpy(&out, s.output.data(), 120);
      out.sequence = out.trigger_sequence = s.sequence ? s.sequence : 1;
      out.release_generation = out.sequence - 1;
      if (in.payload_checksum !=
          rt::sampled_io_payload_checksum(std::span(s.input).subspan(120)))
        return rt::XdmaUserEventResult{rt::XdmaDriverResult::io_error, 0};
      std::copy(s.input.begin() + 120, s.input.end(), s.output.begin() + 120);
      out.first_sample_timestamp = s.now;
      out.payload_checksum =
          rt::sampled_io_payload_checksum(std::span(s.output).subspan(120));
      std::memcpy(s.output.data(), &out, 120);
      ++s.acks;
      return rt::XdmaUserEventResult{rt::XdmaDriverResult::success, 1};
    };
    return a;
  }
};
struct Backend {
  Card card;
  std::unique_ptr<rt::XdmaDeviceBackend> native;
  rt::HalV2BackendApi original{};
  rt::HalV2CommandTimelineExtension original_commands{}, commands{};
  std::atomic<bool> hold_completions{false};
  std::atomic<unsigned> held_polls{0};
  bool deterministic = true;
  Backend() {
    rt::XdmaBackendConfig c;
    c.queue_capacity = 4;
    c.buffer_capacity = 1;
    c.worker_count = 1;
    c.h2c_channel_count = c.c2h_channel_count = 1;
    c.max_buffer_bytes = 4096;
    c.max_transfer_bytes = 128;
    c.transfer_alignment = 1;
    c.control_aperture_bytes = 4;
    c.user_event_count = 1;
    native = std::make_unique<rt::XdmaDeviceBackend>(card.api(), c);
  }
  rt::HalV2BackendRegistration
  hal_v2_registration(std::string_view name = "sampled.simulation.owned.card") {
    auto r = native->hal_v2_registration("sampled.simulation.native");
    original = r.api;
    original_commands = *r.command_timeline;
    commands = original_commands;
    commands.instance = this;
    commands.get_capabilities = [](void *p, rt::HalV2CommandTimelineCapabilities *v) {
      auto &s = *static_cast<Backend *>(p);
      return s.original_commands.get_capabilities(s.original_commands.instance, v);
    };
    commands.submit = [](void *p, const rt::DeviceCommandBatch *v) {
      auto &s = *static_cast<Backend *>(p);
      return s.original_commands.submit(s.original_commands.instance, v);
    };
    commands.poll = [](void *p, rt::HalV2BatchCompletion *v, std::uint64_t capacity, std::uint64_t *count) {
      auto &s = *static_cast<Backend *>(p);
      if (s.hold_completions) { ++s.held_polls; *count = 0; return rt::HalV2Status::ok; }
      return s.original_commands.poll(s.original_commands.instance, v, capacity, count);
    };
    commands.cancel = [](void *p, std::uint64_t id) {
      auto &s = *static_cast<Backend *>(p);
      return s.original_commands.cancel(s.original_commands.instance, id);
    };
    commands.request_stop = [](void *p) {
      auto &s = *static_cast<Backend *>(p);
      return s.original_commands.request_stop(s.original_commands.instance);
    };
    r.command_timeline = &commands;
    r.name = name;
    r.api.instance = this;
    r.api.get_capabilities = [](void *p, rt::HalV2Capabilities *c) {
      auto &s = *static_cast<Backend *>(p);
      auto z = s.original.get_capabilities(s.original.instance, c);
      if (z == rt::HalV2Status::ok)
        c->deterministic_mock = s.deterministic;
      return z;
    };
    r.api.initialize = [](void *p, const rt::HalV2InitializeConfig *v) {
      auto &s = *static_cast<Backend *>(p);
      return s.original.initialize(s.original.instance, v);
    };
    r.api.register_buffer = [](void *p, const rt::HalV2BufferRegistration *v,
                               std::uint64_t *out) {
      auto &s = *static_cast<Backend *>(p);
      return s.original.register_buffer(s.original.instance, v, out);
    };
    r.api.unregister_buffer = [](void *p, std::uint64_t v) {
      auto &s = *static_cast<Backend *>(p);
      return s.original.unregister_buffer(s.original.instance, v);
    };
    r.api.submit = [](void *p, const rt::HalV2Submission *v) {
      auto &s = *static_cast<Backend *>(p);
      return s.original.submit(s.original.instance, v);
    };
    r.api.poll = [](void *p, rt::HalV2Completion *v, std::uint64_t n,
                    std::uint64_t *out) {
      auto &s = *static_cast<Backend *>(p);
      return s.original.poll(s.original.instance, v, n, out);
    };
    r.api.cancel = [](void *p, std::uint64_t v) {
      auto &s = *static_cast<Backend *>(p);
      return s.original.cancel(s.original.instance, v);
    };
    r.api.get_health = [](void *p, rt::HalV2Health *v) {
      auto &s = *static_cast<Backend *>(p);
      return s.original.get_health(s.original.instance, v);
    };
    r.api.reset = [](void *p) {
      auto &s = *static_cast<Backend *>(p);
      return s.original.reset(s.original.instance);
    };
    r.api.shutdown = [](void *p) {
      auto &s = *static_cast<Backend *>(p);
      return s.original.shutdown(s.original.instance);
    };
    return r;
  }
};

struct Fixture {
  Clock clock;
  alignas(64) std::array<std::byte, frame_bytes * 8 + 4> storage{};
  Backend backend;
  std::unique_ptr<Backend> companion;
  rt::Runtime runtime{clock};
  rt::DeviceBackendHandle device;
  std::array<rt::PhaseHandle, 3> phases{};
  std::array<rt::RateDomainHandle, 3> rates{};
  rt::CrossRateChannelHandle output, input, ordinary;
  rt::DeviceCommandBatch declaration;
  rt::SampledIoChannelRegistration output_spec, input_spec;
  Frame initial_output = frame(101, 1, rt::SampledIoFrameStatus::initial),
        initial_input = frame(202, 1, rt::SampledIoFrameStatus::initial),
        safe = frame(101, 1, rt::SampledIoFrameStatus::safe), produced{};
  std::array<std::byte, 144> state{};
  rt::CrossRateReadResult read{};
  bool skip = false, twice = false;
  std::optional<rt::DeviceRateSimulationPolicy> simulation =
      rt::DeviceRateSimulationPolicy{5'000'000'000};
  unsigned substeps = 1;
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
  static rt::CallbackResult provide(void *p,
                                    const rt::DeviceCallbackContext &ctx,
                                    rt::DeviceCommandBatch &batch) {
    auto &s = *static_cast<Fixture *>(p);
    rt::DeviceTimelineInfo t;
    if (!s.runtime.device_timeline_at(s.device, 0, t))
      return rt::CallbackResult::error;
    batch = s.declaration;
    batch.timeout_ns = 10'000'000;
    batch.signals[0].value = t.last_accepted_value + 1;
    if (!ctx.rate_release)
      return rt::CallbackResult::error;
    const auto sequence =
        ctx.rate_release->domain_release_sequence * s.substeps +
        ctx.rate_release->substep_ordinal + 2;
    if (sequence > UINT32_MAX ||
        !rt::set_xdma_control_write(batch.commands[1], 0,
                                    static_cast<std::uint32_t>(sequence)))
      return rt::CallbackResult::error;
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
    cfg.device_backend_capacity = companion ? 2 : 1;
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
         backend.deterministic,
         true,
         {}}));
    STORAGE_TRY(
        runtime.register_device_backend(backend.hal_v2_registration(), device));
    if (companion) {
      rt::DeviceBackendHandle other;
      STORAGE_TRY(runtime.register_device_backend(
          companion->hal_v2_registration("sampled.simulation.companion"),
          other));
    }
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
    declaration.command_count = 4;
    declaration.signal_count = 1;
    declaration.signals[0].timeline_handle = timeline.value;
    const auto transfer = [&](std::size_t at, bool down,
                              std::uint64_t host_offset,
                              std::uint64_t card_offset) {
      auto &cmd = declaration.commands[at];
      cmd.kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
      cmd.opcode = down ? rt::xdma_device_opcode_card_to_host
                        : rt::xdma_device_opcode_host_to_card;
      cmd.buffer_count = 1;
      cmd.buffers[0] = {buffer.value,
                        down ? RTFW_DEVICE_ACCESS_WRITE
                             : RTFW_DEVICE_ACCESS_READ,
                        0, host_offset, frame_bytes * 4};
      rt::XdmaTransfer t;
      t.channel = 0;
      t.device_offset = card_offset;
      cmd.payload_size = sizeof(t);
      std::memcpy(cmd.payload.data(), &t, sizeof(t));
    };
    transfer(0, false, 0, 0);
    if (!rt::set_xdma_control_write(declaration.commands[1], 0, 0))
      return rt::Status::internal_error;
    transfer(3, true, 512, 128);
    rt::HalV2BufferReference ack{buffer.value, RTFW_DEVICE_ACCESS_WRITE, 0,
                                 1024, 4};
    if (!rt::set_xdma_user_event_wait(declaration.commands[2], 0, ack))
      return rt::Status::internal_error;
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
          {names[i], i == 2 ? period : period * 2, i == 1 ? substeps : 1,
           i == 2 ? period : period * 2, 10'000'000},
          rates[i]));
    STORAGE_TRY(runtime.bind_phase_to_rate_domain(phases[0], rates[0]));
    STORAGE_TRY(runtime.bind_phase_to_rate_domain(phases[2], rates[2]));
    const std::array roles{rt::DeviceRatePayloadRole::input,
                           rt::DeviceRatePayloadRole::output,
                           rt::DeviceRatePayloadRole::output};
    rt::DeviceRatePhaseBinding binding{phases[1], rates[1], 10'000'000,
                                       substeps, roles};
    binding.simulation = simulation;
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
                                             {2, frame_bytes},
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
    output_spec.safe_transition_timeout_ns = 8'000'000;
    output_spec.initial_frame = initial_output;
    output_spec.startup_safe_frame = output_spec.failure_safe_frame =
        output_spec.shutdown_safe_frame = safe;
    input_spec = output_spec;
    input_spec.channel = input;
    input_spec.direction = rt::SampledIoDirection::input;
    input_spec.channel_identity = 202;
    input_spec.timestamp_domain_identity = 1;
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
    backend.card.now = 9000 + tick * period;
    return runtime.step(c);
  }
  bool valid(std::uint64_t tick) const {
    rt::SampledIoFrameHeader h;
    std::memcpy(&h, state.data(), 120);
    const auto source = tick / 2 * 2;
    if (count() != tick + 1 || h.sequence != (tick / 2 + 1) * substeps + 1 ||
        h.release_generation != (tick / 2 + 1) * substeps ||
        h.timestamp_domain_identity != 1 ||
        h.first_sample_timestamp != 9000 + source * period ||
        read.producer_release_sequence != tick / 2 ||
        read.producer_substep_ordinal != substeps - 1 ||
        read.producer_timestamp_domain_identity != 1 ||
        read.producer_timestamp != h.first_sample_timestamp ||
        read.producer_completion_status != rt::Status::ok ||
        read.age_ns != (tick - source) * period ||
        h.payload_checksum !=
            rt::sampled_io_payload_checksum(std::span(state).subspan(120, 8)))
      return false;
    for (std::size_t i = 0; i < 8; ++i)
      if (state[120 + i] != std::byte((source * 17 + i) & 255) ||
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
                                  std::size_t n, std::size_t first = 0) {
    std::array<rt::ReplayInputRecord, 128> records{};
    if (n > records.size())
      return {};
    for (std::size_t i = 0; i < n; ++i)
      records[i] = {context(i + first), 1, {}};
    rt::ArtifactWriteResult out;
    std::vector<std::byte> bytes(1024 * 1024);
    if (runtime.write_active_replay_artifact(
            initial, std::span(records).first(n), bytes, out) != rt::Status::ok)
      return {};
    bytes.resize(out.bytes_written);
    return bytes;
  }
  static rt::CallbackResult apply(void *opaque, const rt::ReplayInputView &in) {
    if (!opaque || !in.frame.nominal_release_ns)
      return rt::CallbackResult::error;
    auto &s = *static_cast<Fixture *>(opaque);
    s.clock.now = *in.frame.nominal_release_ns;
    s.backend.card.now = 9000 + in.frame.frame_index * period;
    return rt::CallbackResult::ok;
  }
};
} // namespace sampled_simulation
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace sampled_simulation {
// Test-only bounded host scheduling delay, established after native event
// entry. Logical time advances only for the explicit logical-expiry negative.
struct Gate {
  Card &card;
  Clock &clock;
  std::thread worker;
  std::atomic<bool> observed{false};
  Gate(Fixture &f, bool expire_logical = false, unsigned hold_ms = 30)
      : card(f.backend.card), clock(f.clock) {
    card.entered = false;
    card.hold = true;
    worker = std::thread([this, expire_logical, hold_ms] {
      const auto limit =
          std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!card.entered && std::chrono::steady_clock::now() < limit)
        std::this_thread::yield();
      observed = card.entered.load();
      if (observed) {
        if (expire_logical)
          clock.now.fetch_add(8'000'001);
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
      }
      card.hold = false;
    });
  }
  ~Gate() {
    worker.join();
    card.hold = false;
  }
};
inline rt::Status checked_cleanup(Fixture &f) {
  // Timeout rollback may retain native ownership. Complete the public checked
  // stop protocol before destroying the owner, independently of the timeout
  // assertion. No failed execution/start is retried.
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    const auto status = f.runtime.stop();
    if (status == rt::Status::ok) return status;
    std::fprintf(stderr, "checked cleanup attempt=%u status=%d detail=%.*s\n",
        attempt, int(status), int(f.runtime.last_error().size()),
        f.runtime.last_error().data());
  }
  return rt::Status::internal_error;
}
inline bool run_case(unsigned id) {
  using rt::Status;
  Fixture f;
  if (id == 4 || id == 5)
    f.simulation.reset();
  if (id == 5 || id == 9)
    f.backend.deterministic = false;
  if (id == 7)
    f.simulation = rt::DeviceRateSimulationPolicy{100'000'000};
  if (id == 10 || id == 11) {
    f.backend.hold_completions = true;
    if (id == 10) f.simulation = rt::DeviceRateSimulationPolicy{100'000'000};
    else f.simulation.reset();
  }
  auto status = f.configure();
  if (id == 9) {
    if (status == Status::ok)
      status = f.runtime.finalize();
    std::printf("native opt-in rejection=%d\n", int(status));
    return status == Status::invalid_config && !f.backend.card.live;
  }
  if (status != Status::ok || f.runtime.finalize() != Status::ok)
    return false;
  if (id == 10 || id == 11) {
    const auto failed_start = f.runtime.start();
    rt::SampledIoChannelStatus info;
    const bool unknown = f.runtime.sampled_io_channel_status(f.output, info) &&
        info.safety_state == rt::SampledIoSafetyState::unknown;
    const auto before = f.backend.held_polls.load();
    const auto pending_stop = f.runtime.stop();
    const auto after = f.backend.held_polls.load();
    const bool retained = f.backend.card.live;
    f.backend.hold_completions = false;
    const auto cleanup = checked_cleanup(f);
    std::fprintf(stderr, "held completion case=%u start=%d pending_stop=%d retained=%d unknown=%d stop_polls=%u cleanup=%d live=%d\n",
        id, int(failed_start), int(pending_stop), retained, unknown, after-before,
        int(cleanup), f.backend.card.live.load());
    return failed_start == Status::invalid_state && pending_stop == Status::invalid_state &&
        retained && unknown && after-before == 1 && cleanup == Status::ok && !f.backend.card.live;
  }
  if (id == 0 || id == 4 || id == 5 || id == 6 || id == 7) {
    bool entered = false;
    {
      Gate gate(f, id == 6, id == 7 ? 200u : 30u);
      status = f.runtime.start();
      entered = gate.observed;
    }
    const auto wanted = id == 0 ? Status::ok : Status::device_timeout;
    const auto timeout = f.backend.card.event_timeout.load();
    const bool good = entered && status == wanted && timeout == 8'000'000;
    const auto cleanup = checked_cleanup(f);
    std::printf("startup case=%u status=%d expected=%d native_timeout=%llu "
                "entered=%d cleanup=%d live=%d\n",
                id, int(status), int(wanted),
                static_cast<unsigned long long>(timeout), entered, int(cleanup),
                f.backend.card.live.load());
    return good && cleanup == Status::ok && !f.backend.card.live;
  }
  if (f.runtime.start() != Status::ok)
    return false;
  if (id == 3) {
    Gate gate(f);
    status = f.step(0);
    if (status != Status::ok || !gate.observed)
      return false;
  } else if (id == 8) {
    f.backend.card.missing_ack = true;
    status = f.runtime.stop();
    rt::SampledIoChannelStatus info;
    const bool retained =
        f.backend.card.live &&
        f.runtime.sampled_io_channel_status(f.output, info) &&
        info.safety_state == rt::SampledIoSafetyState::unknown;
    f.backend.card.missing_ack = false;
    const auto cleanup = checked_cleanup(f);
    return status == Status::device_timeout && retained &&
           cleanup == Status::ok && !f.backend.card.live;
  } else {
    if (id == 2) {
      f.backend.card.missing_ack = true;
      const auto failed_step = f.step(0);
      f.backend.card.missing_ack = false;
      if (failed_step == Status::ok)
        return false;
    }
    bool entered = false;
    {
      Gate gate(f);
      status = f.runtime.stop();
      entered = gate.observed;
    }
    rt::SampledIoChannelStatus info;
    const auto wanted = id == 2
                            ? rt::SampledIoSafetyState::failure_acknowledged
                            : rt::SampledIoSafetyState::shutdown_acknowledged;
    const bool good = status == Status::ok && entered &&
                      f.runtime.sampled_io_channel_status(f.output, info) &&
                      info.safety_state == wanted;
    const auto cleanup = checked_cleanup(f);
    std::printf("stop case=%u status=%d entered=%d cleanup=%d live=%d safety=%u expected=%u\n", id,
                int(status), entered, int(cleanup), f.backend.card.live.load(),
                unsigned(info.safety_state), unsigned(wanted));
    return good && cleanup == Status::ok && !f.backend.card.live;
  }
  return f.runtime.stop() == Status::ok && !f.backend.card.live;
}
} // namespace sampled_simulation
#ifndef RTFW_SAMPLED_SIMULATION_NO_MAIN
int main(int argc, char **argv) {
  if (argc == 2)
    return sampled_simulation::run_case(
               static_cast<unsigned>(std::stoul(argv[1])))
               ? 0
               : 1;
  for (unsigned i = 0; i < 12; ++i)
    if (!sampled_simulation::run_case(i))
      return int(i + 1);
  return 0;
}
#endif
