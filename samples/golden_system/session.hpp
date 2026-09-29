#pragma once
#include "jobs.hpp"
#include "memory.hpp"
#include "physics.hpp"
#include <optional>
#include <vector>

namespace golden {
class Clock final : public rt::RuntimeClock {
public:
  std::atomic<std::uint64_t> now{1000};
  std::uint64_t now_ns() noexcept override {
    return now.load(std::memory_order_relaxed);
  }
};
class Session {
  Physics *physics_;
  Jobs *jobs_;
  Memory &memory_;
  bool attached_ = false, closed_ = false;

public:
  Clock clock;
  World world;
  std::optional<rt::Runtime> runtime;
  std::array<Binding, 8> bindings{};
  std::array<rt::PhaseHandle, 8> phases{};
  std::array<rt::RateDomainHandle, 5> rates{};
  rt::MemoryPlan plan{};
  rt::LiveControlProducerHandle producer{};
  std::uint64_t sequence = 1;
  Session(Options options, Jobs *jobs, Memory &memory, Physics *physics = nullptr)
      : physics_(physics), jobs_(jobs), memory_(memory), world(options),
        runtime(std::in_place, clock) {}
  ~Session() {
    if (close() != rt::Status::ok)
      std::terminate();
  }
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
  rt::Status
  prepare(std::size_t retention_capacity = fixed::action_capacity,
          const rt::CpuMemoryPolicy *cpu_policy = nullptr) noexcept {
    if (!world.options.valid())
      return rt::Status::invalid_argument;
    rt::RuntimeConfig c;
    c.callback_capacity = 8;
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
    GOLDEN_TRY(r.set_cpu_memory_policy(cpu_policy ? *cpu_policy : Memory::policy()));
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
    constexpr std::array<std::size_t, 5> phases_per_domain{3, 1, 1, 1, 2};
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
      } else {
        GOLDEN_TRY(r.register_callback(
            {fixed::phase_names[i], physics_ ? invoke_variant : invoke,
             physics_ ? static_cast<void *>(this) : static_cast<void *>(&bindings[i])},
            phases[i]));
        GOLDEN_TRY(r.bind_phase_to_rate_domain(phases[i], rates[fixed::phase_rates[i]]));
      }
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
      GOLDEN_TRY(r.register_cross_rate_channel(
          {fixed::channel_names[i], phases[fixed::channel_producers[i]],
           phases[fixed::channel_consumers[i]], frame_size(i), frame_span(f, i),
           rt::CrossRateMode::sample_and_hold,
           fixed::channel_age[i] * fixed::tick_ns},
          world.channels[i]));
    }
    GOLDEN_TRY(r.register_state({"golden.state", 1, world.canonical}));
    GOLDEN_TRY(r.finalize());
    if (!r.memory_plan(plan) || plan.planned_bytes > fixed::runtime_budget ||
        plan.phase_count != 8 || plan.rate_domain_count != 5 ||
        plan.rate_binding_count != 8 || plan.reference_release_count != 27 ||
        plan.cross_rate_channel_count != 5 ||
        plan.sampled_io_channel_count != 0 ||
        (physics_ ? !physics_->plan(plan) : plan.device_backend_count != 0) ||
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
    GOLDEN_TRY(r.start());
#undef GOLDEN_TRY
    return rt::Status::ok;
  }
  static rt::CallbackResult invoke_variant(void *opaque, const rt::CallbackContext &ctx) noexcept {
    auto &s = *static_cast<Session *>(opaque);
    if (!ctx.rate_release || !s.physics_)
      return rt::CallbackResult::error;
    const auto phase = ctx.rate_release->phase.index();
    if (phase == 2 && !s.physics_->complete(ctx))
      return rt::CallbackResult::error;
    if (!s.world.phase(phase, ctx) || (phase == 0 && !s.physics_->input(ctx)))
      return rt::CallbackResult::error;
    return rt::CallbackResult::ok;
  }
  rt::HostFrameContext frame(std::size_t tick) const noexcept {
    return {tick, std::chrono::nanoseconds(fixed::tick_ns), std::nullopt,
            1000 + tick * fixed::tick_ns};
  }
  rt::Status step(std::size_t tick, bool late = false) noexcept {
    if (tick == 6 && world.options.campaign == Campaign::control_rejected &&
        !stage<1>(6, 16, false, 0, true))
      return rt::Status::internal_error;
    auto f = frame(tick);
    clock.now.store(*f.nominal_release_ns + (late ? 2 * fixed::tick_ns : 0),
                    std::memory_order_relaxed);
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
      for (std::size_t i = 0; i < 27; ++i) {
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
} // namespace golden
