#pragma once
#include "../golden_system/session.hpp"
#include "io.hpp"

#include <optional>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
namespace golden::xdma {
class Session {
  Physics *physics_;
  Io &io_;
  Jobs *jobs_;
  Memory &memory_;
  bool attached_ = false, closed_ = false;

public:
  Clock clock;
  World world;
  std::optional<rt::Runtime> runtime;
  std::array<Binding, 8> bindings{};
  std::array<rt::PhaseHandle, 10> phases{};
  std::array<rt::RateDomainHandle, 5> rates{};
  rt::MemoryPlan plan{};
  rt::LiveControlProducerHandle producer{};
  std::uint64_t sequence = 1;
  Session(Options options, Jobs *jobs, Memory &memory, Io &io,
          Physics *physics = nullptr)
      : physics_(physics), io_(io), jobs_(jobs), memory_(memory),
        world(options), runtime(std::in_place, clock) {}
  ~Session() {
    if (close() != rt::Status::ok)
      std::terminate();
  }
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
  rt::Status prepare(std::size_t retention_capacity = fixed::action_capacity,
                     const rt::CpuMemoryPolicy *cpu_policy = nullptr) noexcept {
    if (!world.options.valid())
      return rt::Status::invalid_argument;
    rt::RuntimeConfig c;
    c.callback_capacity = 10;
    c.worker_count = world.options.workers;
    c.executor_queue_capacity = fixed::queue_slots;
    c.scratch_bytes = fixed::scratch_bytes;
    c.task_scratch_bytes = fixed::scratch_bytes;
    c.task_scratch_slots = fixed::task_slots;
    c.trace_capacity = fixed::action_capacity;
    c.memory_budget_bytes = fixed::runtime_budget;
    c.snapshot_max_bytes = fixed::checkpoint_bytes;
    c.input_log_max_bytes = fixed::artifact_bytes;
    c.replay_input_capacity = fixed::maximum_ticks;
    c.determinism_tier = rt::DeterminismTier::unspecified;
    std::copy(fixed::scenario_id.begin(), fixed::scenario_id.end(),
              c.workload_id.begin());
    if (jobs_)
      c.executor_policy = rt::ExecutorPolicy::host_adapter;
    if (physics_)
      physics_->limits(c);
    Io::limits(c, physics_ != nullptr);
    const bool replay_enabled = !physics_ || physics_->replay_enabled();
    auto &r = *runtime;
    rt::Status s;
#define GOLDEN_TRY(expression)                                                 \
  do {                                                                         \
    s = (expression);                                                          \
    if (s != rt::Status::ok)                                                   \
      return s;                                                                \
  } while (false)
    GOLDEN_TRY(r.configure(c));
    if (jobs_) {
      GOLDEN_TRY(jobs_->attach());
      attached_ = true;
      GOLDEN_TRY(r.set_host_executor(jobs_->adapter()));
    }
    GOLDEN_TRY(r.set_memory_provider(memory_.table()));
    auto parked = Memory::policy();
    parked.thread_policy_count = 1;
    parked.thread_policies[0].role = rt::thread_role_executor_worker;
    parked.thread_policies[0].policy.wait_strategy = rt::WaitStrategy::park;
    GOLDEN_TRY(r.set_cpu_memory_policy(cpu_policy ? *cpu_policy : parked));
    GOLDEN_TRY(
        r.set_rate_execution_policy({128, 26, 1, 1, fixed::action_capacity}));
    GOLDEN_TRY(r.set_mixed_rate_closure_policy(
        {26,
         retention_capacity,
         retention_capacity,
         fixed::artifact_bytes,
         128,
         rt::MixedRateOverflowPolicy::overwrite_committed,
         replay_enabled,
         true,
         {}}));
    rt::LiveControlPolicy p;
    p.policy_identity = 2601;
    p.mailbox_capacity = 1;
    p.producer_capacity = 1;
    p.record_capacity = 16;
    p.payload_bytes_per_record = 64;
    p.total_payload_storage_bytes = 1024;
    GOLDEN_TRY(r.set_live_control_policy(p));
    rt::LiveControlMailboxRegistration m;
    m.mailbox_identity = 2601;
    m.record_capacity = 16;
    m.payload_bytes_per_record = 64;
    GOLDEN_TRY(r.register_live_control_mailbox(m));
    rt::LiveControlProducerRegistration producer_registration;
    producer_registration.mailbox_identity = 2601;
    producer_registration.producer_identity = 2601;
    GOLDEN_TRY(r.register_live_control_producer(producer_registration));
    rt::LiveControlClosurePolicy closure;
    closure.policy_identity = 2601;
    closure.action_capacity = retention_capacity;
    closure.retained_generation_capacity = 128;
    closure.retained_record_capacity = 256;
    closure.retained_payload_bytes = 16384;
    closure.replay_record_capacity = retention_capacity;
    closure.replay_max_bytes = fixed::artifact_bytes;
    closure.replay_enabled = replay_enabled;
    GOLDEN_TRY(r.set_live_control_closure_policy(closure));
    rt::LiveControlReplayRetentionPolicy retention;
    retention.policy_identity = 2601;
    retention.admission_capacity = 128;
    retention.payload_capacity_bytes = 8192;
    if (replay_enabled)
      GOLDEN_TRY(r.set_live_control_replay_retention_policy(retention));
    if (physics_)
      GOLDEN_TRY(physics_->configure(r, world));
    GOLDEN_TRY(io_.configure(r, world));
    constexpr std::array<std::size_t, 5> phases_per_domain{3, 2, 1, 2, 2};
    // Contract budgets are domain-release envelopes; Runtime charges each
    // phase.
    for (std::size_t i = 0; i < 5; ++i)
      GOLDEN_TRY(r.register_rate_domain(
          {fixed::rate_names[i], fixed::periods[i] * fixed::tick_ns, 1,
           fixed::periods[i] * fixed::tick_ns,
           fixed::budgets[i] / phases_per_domain[i]},
          rates[i]));
    for (std::size_t i = 0; i < 8; ++i) {
      bindings[i] = {&world, i};
      if (physics_ && i == 1) {
        GOLDEN_TRY(physics_->register_phase(r, rates[0], phases[i]));
      } else if (i == 3 || i == 5) {
        GOLDEN_TRY(
            io_.register_phase(r, i, rates[fixed::phase_rates[i]], phases[i]));
      } else {
        GOLDEN_TRY(r.register_callback(
            {fixed::phase_names[i], invoke_variant, this}, phases[i]));
        GOLDEN_TRY(r.bind_phase_to_rate_domain(phases[i],
                                               rates[fixed::phase_rates[i]]));
      }
    }
    for (std::size_t lane = 0; lane < 2; ++lane) {
      GOLDEN_TRY(
          r.register_callback({lane ? "actuator.complete" : "sensor.complete",
                               invoke_variant, this},
                              phases[8 + lane]));
      GOLDEN_TRY(
          r.bind_phase_to_rate_domain(phases[8 + lane], rates[lane ? 3 : 1]));
      GOLDEN_TRY(r.add_dependency(phases[lane ? 5 : 3], phases[8 + lane]));
    }
    for (auto edge :
         std::array<std::array<std::size_t, 2>, 3>{{{0, 1}, {1, 2}, {6, 7}}})
      GOLDEN_TRY(r.add_dependency(phases[edge[0]], phases[edge[1]]));
    constexpr std::array<std::string_view, 7> names{
        "command",    "plant",    "stage",    "sensor",
        "controller", "actuator", "aggregate"};
    std::array<rt::ResourceHandle, 7> resources{};
    for (std::size_t i = 0; i < 7; ++i) {
      GOLDEN_TRY(r.register_resource(names[i], resources[i]));
      GOLDEN_TRY(r.declare_resource_access(phases[i], resources[i],
                                           rt::ResourceAccess::write));
    }
    GOLDEN_TRY(r.declare_resource_access(phases[1], resources[0],
                                         rt::ResourceAccess::read));
    GOLDEN_TRY(r.declare_resource_access(phases[2], resources[1],
                                         rt::ResourceAccess::read));
    GOLDEN_TRY(r.declare_resource_access(phases[7], resources[6],
                                         rt::ResourceAccess::read));
    for (std::size_t i = 0; i < 5; ++i) {
      Frame f{};
      if (i == 3)
        pack_vector(f, world.plant.acceleration);
      seal(f, i, 0, 1);
      if (i == 4) {
        GOLDEN_TRY(r.register_cross_rate_channel(
            {fixed::channel_names[i], phases[2], phases[6], frame_size(i),
             frame_span(f, i), rt::CrossRateMode::sample_and_hold,
             fixed::channel_age[i] * fixed::tick_ns},
            world.channels[i]));
        continue;
      }
      sampled_header(f, i, 1, 0, 0, rt::SampledIoFrameStatus::initial);
      rt::CrossRateChannelRegistration cross{
          fixed::channel_names[i],
          phases[fixed::channel_producers[i]],
          phases[fixed::channel_consumers[i]],
          frame_size(i),
          frame_span(f, i),
          rt::CrossRateMode::sample_and_hold,
          fixed::channel_age[i] * fixed::tick_ns};
      if (i % 2)
        cross.producer_device = {3, frame_size(i)};
      else
        cross.consumer_device = {0, frame_size(i)};
      GOLDEN_TRY(r.register_cross_rate_channel(cross, world.channels[i]));
      rt::SampledIoChannelRegistration sampled;
      sampled.channel = world.channels[i];
      sampled.direction = i % 2 ? rt::SampledIoDirection::input
                                : rt::SampledIoDirection::output;
      sampled.channel_identity = 26001 + i;
      sampled.encoding = rt::SampledIoEncoding::signed_int32_le;
      sampled.element_count = static_cast<std::uint32_t>(
          fixed::channel_elements[i] * fixed::capacity);
      sampled.samples_per_frame = 1;
      sampled.units_identity = 26001 + i;
      sampled.calibration_identity = 1;
      sampled.sample_period_ns = fixed::tick_ns;
      sampled.timestamp_domain_identity = sampled.clock_domain_identity =
          sampled.trigger_identity = 1;
      sampled.ring_capacity = 4;
      sampled.initial_sequence = 1;
      sampled.maximum_age_ns = fixed::channel_age[i] * fixed::tick_ns;
      sampled.stale_policy = rt::SampledIoStalePolicy::substitute_initial;
      sampled.initial_frame = frame_span(f, i);
      Frame safe{};
      if (!(i % 2)) {
        sampled_header(safe, i, 1, 0, 0, rt::SampledIoFrameStatus::safe);
        sampled.underrun_policy = rt::SampledIoUnderrunPolicy::substitute_safe;
        sampled.safe_transition_timeout_ns = safe_timeout_ns;
        sampled.startup_safe_frame = sampled.failure_safe_frame =
            sampled.shutdown_safe_frame = frame_span(safe, i);
      }
      GOLDEN_TRY(r.register_sampled_io_channel(sampled));
    }
    GOLDEN_TRY(r.register_state({"golden.state", 1, world.canonical}));
    GOLDEN_TRY(r.finalize());
    if (!r.memory_plan(plan) || plan.planned_bytes > fixed::runtime_budget ||
        plan.phase_count != 10 || plan.rate_domain_count != 5 ||
        plan.rate_binding_count != 10 || plan.reference_release_count != 32 ||
        plan.cross_rate_channel_count != 5 ||
        plan.sampled_io_channel_count != 4 ||
        (plan.device_backend_count != (physics_ ? 2u : 1u) ||
         plan.device_buffer_count != (physics_ ? 8u : 6u) ||
         plan.device_timeline_count != (physics_ ? 3u : 2u)) ||
        memory_.live_count() != 3)
      return rt::Status::internal_error;
    const auto accounted =
        plan.runtime_control_bytes + plan.executor_control_bytes +
        plan.phase_scratch_total_bytes + plan.task_scratch_total_bytes +
        plan.trace_storage_bytes;
    // The six-row equation includes device metadata as its own control row.
    if (accounted + plan.device_control_bytes != plan.planned_bytes ||
        plan.phase_scratch_total_bytes > Memory::region_capacity ||
        plan.task_scratch_total_bytes > Memory::region_capacity ||
        plan.trace_storage_bytes > Memory::region_capacity)
      return rt::Status::internal_error;
    GOLDEN_TRY(r.live_control_producer_handle(2601, 2601, producer));
    // Diagnostic only: stop after actual finalization; no device initialization.
#undef GOLDEN_TRY
    return rt::Status::ok;
  }
  static rt::CallbackResult
  invoke_variant(void *opaque, const rt::CallbackContext &ctx) noexcept {
    return static_cast<Session *>(opaque)->phase(ctx)
               ? rt::CallbackResult::ok
               : rt::CallbackResult::error;
  }
  bool phase(const rt::CallbackContext &ctx) noexcept {
    if (!ctx.rate_release)
      return false;
    const auto p = ctx.rate_release->phase.index();
    auto &w = world;
    const auto &release = *ctx.rate_release;
    const auto tick = release.logical_release_ns / fixed::tick_ns;
    if (p >= 8) {
      if (p > 9 || !io_.complete(p - 8))
        return false;
      w.encode();
      return true;
    }
    if (p == 1 || p == 6 || p == 7)
      return w.phase(p, ctx);
    if (!w.configuration.consume(ctx.live_control) || tick >= w.options.ticks ||
        tick % fixed::periods[fixed::phase_rates[p]])
      return false;
    ++w.calls[p];
    if (p == 0) {
      if (!io_.read(release, 3))
        return false;
      unpack_vector(w.buffers[3], w.command);
      w.next_tick = tick + 1;
      if (physics_ && !physics_->input(ctx))
        return false;
    } else if (p == 2) {
      if (physics_ && !physics_->complete(ctx))
        return false;
      for (auto c : {std::size_t{0}, std::size_t{4}}) {
        w.buffers[c].fill(std::byte{});
        pack_vector(w.buffers[c], w.plant.position);
        pack_vector(w.buffers[c], w.plant.velocity, 3);
        if (!(c == 4 ? w.publish(release, c, tick) : io_.publish(release, c)))
          return false;
      }
    } else if (p == 4) {
      if (!io_.read(release, 1))
        return false;
      const bool absent = (w.options.external && !w.external_ready) ||
                          w.configuration.fault == 10;
      if (absent)
        ++w.missing;
      w.buffers[2].fill(std::byte{});
      for (std::size_t a = 0; a < 3; ++a)
        for (std::size_t i = 0; i < w.options.count; ++i) {
          const auto v = get32(
              w.buffers[1], header_bytes + 4 * ((3 + a) * fixed::capacity + i));
          const auto value =
              absent ? 0
                     : (w.options.external ? w.external_command[a][i]
                                           : effort(v, w.configuration.target,
                                                    w.configuration.gain));
          if (value < -4 || value > 4)
            return false;
          put32(w.buffers[2], header_bytes + 4 * (a * fixed::capacity + i),
                value);
        }
      if (!io_.publish(release, 2))
        return false;
    } else
      return false;
    w.encode();
    return true;
  }
  rt::HostFrameContext frame(std::size_t tick) const noexcept {
    return {tick, std::chrono::nanoseconds(fixed::tick_ns), std::nullopt,
            1000 + tick * fixed::tick_ns};
  }
  void time(std::uint64_t value) noexcept {
    clock.now.store(value, std::memory_order_relaxed);
    io_.time(value);
  }
  rt::Status step(std::size_t tick, bool late = false) noexcept {
    if (tick == 6 && world.options.campaign == Campaign::control_rejected &&
        !stage<1>(6, 16, false, 0, true))
      return rt::Status::internal_error;
    auto f = frame(tick);
    clock.now.store(*f.nominal_release_ns + (late ? 2 * fixed::tick_ns : 0),
                    std::memory_order_relaxed);
    io_.time(*f.nominal_release_ns);
    return runtime->step(f);
  }
  template <unsigned Kind>
  bool stage(std::size_t tick, std::int32_t value, bool rate = false,
             std::size_t phase = 0, bool corrupt = false) noexcept {
    rt::LiveControlTypedPayload<Control<Kind>> payload{};
    rt::LiveControlUpdateRecord record;
    rt::LiveControlTypedStatus typed;
    if (rate) {
      rt::LiveControlBoundaryTarget target;
      target.kind = rt::LiveControlTargetKind::rate_release;
      bool found = false;
      for (std::size_t i = 0; i < 32; ++i) {
        rt::ReferenceRelease ref;
        if (!runtime->reference_release_at(i, ref))
          return false;
        if (ref.release_time_ns == tick * fixed::tick_ns &&
            ref.phase.index() == phase) {
          target.reference_release_index = static_cast<std::uint32_t>(i);
          target.rate_release_sequence = ref.domain_release_sequence;
          target.rate_domain_registration_index =
              static_cast<std::uint32_t>(ref.domain_registration_index);
          target.phase_index = ref.phase.index();
          target.rate_substep_ordinal = ref.substep_ordinal;
          found = true;
          break;
        }
      }
      if (!found)
        return false;
      typed = rt::make_live_control_rate_update(
          producer, sequence, target, Control<Kind>{value}, payload, record);
    } else
      typed = rt::make_live_control_host_update(
          producer, sequence, tick, Control<Kind>{value}, payload, record);
    if (typed != rt::LiveControlTypedStatus::ok)
      return false;
    if (corrupt)
      record.payload_digest ^= 1;
    rt::LiveControlAdmissionResult admission;
    if (runtime->stage_live_control_update(producer, record, payload,
                                           admission) != rt::Status::ok)
      return false;
    if (corrupt)
      return admission == rt::LiveControlAdmissionResult::invalid;
    if (admission != rt::LiveControlAdmissionResult::accepted)
      return false;
    ++sequence;
    return true;
  }
  bool controls() noexcept {
    const auto n = world.options.ticks;
    const auto campaign = world.options.campaign;
    if (n > 1 && !stage<1>(1, 16, true, 0))
      return false;
    if (n > 2 && !stage<3>(2, 2, true, 3))
      return false;
    if (n > 3 && !stage<2>(3, 2, true, 4))
      return false;
    if (campaign == Campaign::control_replaced &&
        (!stage<2>(6, 1) || !stage<2>(6, 3)))
      return false;
    if (campaign == Campaign::stale_input && !stage<4>(6, 2))
      return false;
    if (campaign == Campaign::peer_missing && !stage<4>(6, 10))
      return false;
    return (n <= 12 || stage<4>(12, 0)) && (n <= 18 || stage<5>(18, 0));
  }
  std::vector<std::byte> checkpoint(std::uint64_t tick) {
    std::size_t size = 0;
    rt::ArtifactWriteResult result;
    if (runtime->checkpoint_size(size) != rt::Status::ok ||
        size > fixed::checkpoint_bytes)
      return {};
    std::vector<std::byte> bytes(size);
    if (runtime->write_checkpoint(tick, bytes, result) != rt::Status::ok)
      return {};
    bytes.resize(result.bytes_written);
    return bytes;
  }
  rt::Status close() noexcept {
    if (closed_)
      return rt::Status::ok;
    if (attached_) {
      auto s = jobs_->quiesce();
      if (s != rt::Status::ok)
        return s;
    }
    if (runtime && runtime->state() != rt::RuntimeState::configuring) {
      auto s = runtime->stop();
      if (s != rt::Status::ok)
        return s;
    }
    if (memory_.live_count() || memory_.violations)
      return rt::Status::internal_error;
    runtime.reset();
    if (attached_) {
      auto s = jobs_->detach();
      if (s != rt::Status::ok)
        return s;
      attached_ = false;
    }
    closed_ = true;
    return rt::Status::ok;
  }
};
} // namespace golden::xdma

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
