#include "experiments.hpp"
#include "../golden_system/oracle.hpp"
#include "../golden_cuda/physics.hpp"
#include <rt/loopback_backend.hpp>
#include <cstring>
#include <numeric>
namespace golden::showcase {
bool finite_value(std::string_view id,std::uint64_t v) noexcept {
  if (id == "workers") return v == 1 || v == 2 || v == 3;
  if (id == "grain") return v == 1 || v == 4 || v == 16 || v == 64;
  if (id == "rate_multiplier") return v == 1 || v == 2 || v == 4;
  if (id == "budget_percent") return v == 25 || v == 50 || v == 100;
  if (id == "queue_slots") return v == 4 || v == 32 || v == 1024;
  if (id == "scratch_bytes") return v == 0 || v == 64 || v == 256;
  if (id == "device_depth") return v == 1 || v == 2;
  if (id == "cuda_graph" || id == "overload_policy") return v <= 1;
  if (id == "transfer_batch") return v == 1 || v == 4 || v == 16;
  if (id == "staging_slots") return v == 2 || v == 4;
  if (id == "telemetry_capacity") return v == 0 || v == 64 || v == 16384;
  if (id == "control_burst") return v == 1 || v == 16 || v == 17;
  return false;
}
namespace {
bool metadata(rt::Runtime &runtime, Experiment &e) {
  rt::RuntimeMetricSnapshot metrics;
  if (runtime.metrics_snapshot(rt::RuntimeMetricWindow::cumulative,nullptr,metrics) != rt::Status::ok) return false;
  e.runtime_id = metrics.metadata.runtime_id; e.config_id = metrics.metadata.config_id;
  return e.runtime_id && e.config_id;
}
bool golden_loop(Experiment &e) {
  Options options;
  if (e.lever == "workers") options.workers = static_cast<std::size_t>(e.requested);
  if (e.lever == "grain") options.grain = static_cast<std::size_t>(e.requested);
  auto memory = std::make_unique<Memory>();
  std::unique_ptr<cuda::SimulatedDriver> driver;
  std::unique_ptr<cuda::CudaPhysics> physics;
  if (e.lever == "cuda_graph") {
    driver = std::make_unique<cuda::SimulatedDriver>(options.count);
    physics = std::make_unique<cuda::CudaPhysics>(*driver,e.requested == 1);
  }
  auto session = std::make_unique<golden::Session>(options,nullptr,*memory,physics.get());
  e.scope = "frozen-golden-loop";
  if (session->prepare() != rt::Status::ok || !session->controls()) return false;
  Oracle oracle(options);
  bool ok = true;
  for (std::size_t tick = 0; ok && tick < options.ticks; ++tick) {
    const auto status = session->step(tick); e.status = static_cast<std::int32_t>(status);
    ok = status == rt::Status::ok && oracle.step(tick,session->world);
    ++e.operations; if (ok) e.checks += 3 * fixed::capacity;
  }
  e.calls = std::accumulate(session->world.calls.begin(),session->world.calls.end(),std::uint64_t{});
  e.state = session->world.canonical; e.has_state = true;
  e.checksum = digest(session->world.canonical); e.bytes = session->plan.planned_bytes;
  ok = metadata(*session->runtime,e) && ok;
  e.cleanup = session->close() == rt::Status::ok && !memory->live_count() && memory->acquisitions == memory->releases;
  physics.reset();
  if (driver) {
    ok = ok && driver->protocol_ok && driver->kernels == (e.requested ? 0 : options.ticks) &&
         driver->graphs == (e.requested ? options.ticks : 0) && driver->uploads == 2 * options.ticks;
    e.accepted = driver->kernels + driver->graphs;
    e.cleanup = driver->close() && e.cleanup;
  }
  e.effective = e.requested;
  return ok && e.cleanup;
}
bool burst(Experiment &e) {
  e.scope = "public-control-boundary";
  Options options;
  auto memory = std::make_unique<Memory>();
  auto session = std::make_unique<golden::Session>(options,nullptr,*memory);
  if (session->prepare() != rt::Status::ok) return false;
  bool ok = true;
  for (std::uint64_t n = 0; n < e.requested; ++n) {
    rt::LiveControlTypedPayload<Control<2>> payload;
    rt::LiveControlUpdateRecord record;
    rt::LiveControlAdmissionResult admission;
    if (rt::make_live_control_host_update(session->producer,session->sequence,0,Control<2>{1},payload,record) != rt::LiveControlTypedStatus::ok ||
        session->runtime->stage_live_control_update(session->producer,record,payload,admission) != rt::Status::ok) return false;
    if (admission == rt::LiveControlAdmissionResult::accepted) { ++e.accepted; ++session->sequence; }
    else if (admission == rt::LiveControlAdmissionResult::full) ++e.rejected;
    else return false;
  }
  Oracle oracle(options);
  ok = session->step(0) == rt::Status::ok && oracle.step(0,session->world);
  rt::LiveControlCommitInfo commit;
  rt::LiveControlMailboxInfo mailbox;
  ok = ok && session->runtime->live_control_commit_info(commit) && session->runtime->live_control_mailbox_info(2601,mailbox);
  e.replaced = commit.replaced; e.operations = e.requested; e.checks = 3 * fixed::capacity;
  e.calls = std::accumulate(session->world.calls.begin(),session->world.calls.end(),std::uint64_t{});
  ok = ok && e.accepted == std::min(std::uint64_t{16},e.requested) && e.accepted + e.rejected == e.requested &&
       e.replaced + commit.committed == e.accepted && commit.committed == 1 &&
       mailbox.accepted == e.accepted && mailbox.full == e.rejected && !mailbox.occupancy &&
       session->sequence == e.accepted + 1;
  // A full rejection did not advance producer sequence. The same next sequence
  // is accepted after terminal reclamation, without modifying prior records.
  ok = ok && session->stage<2>(1,1) && session->sequence == e.accepted + 2;
  e.state = session->world.canonical; e.has_state = true;
  e.checksum = digest(session->world.canonical); ok = metadata(*session->runtime,e) && ok;
  e.cleanup = session->close() == rt::Status::ok && !memory->live_count();
  e.effective = e.requested; return ok && e.cleanup;
}
struct TaskProbe {
  std::size_t scratch = 64;
  std::array<std::uint64_t,64> values{};
  std::uint64_t calls = 0;
  static rt::TaskResult task(void *p,const rt::TaskContext &ctx,const rt::TaskRange &range) noexcept {
    auto &s = *static_cast<TaskProbe *>(p);
    if (ctx.scratch().size() != s.scratch) return rt::TaskResult::error;
    std::fill(ctx.scratch().begin(),ctx.scratch().end(),std::byte{0x26});
    for (auto n = range.begin; n < range.end; ++n) s.values[n] += n + 1;
    return rt::TaskResult::ok;
  }
  static rt::CallbackResult callback(void *p,const rt::CallbackContext &ctx) noexcept {
    auto &s = *static_cast<TaskProbe *>(p); ++s.calls;
    return ctx.tasks.parallel_for(s.values.size(),4,task,p) == rt::Status::ok ? rt::CallbackResult::ok : rt::CallbackResult::error;
  }
};
bool cpu_experiment(Experiment &e) {
  e.scope = "public-runtime-task-rate-telemetry";
  TaskProbe work;
  golden::Clock clock;
  rt::Runtime runtime(clock);
  rt::RuntimeConfig config;
  config.callback_capacity = 1; config.worker_count = 2;
  config.executor_queue_capacity = e.lever == "queue_slots" ? e.requested : 32;
  config.scratch_bytes = config.task_scratch_bytes = e.lever == "scratch_bytes" ? e.requested : 64;
  config.task_scratch_slots = 32;
  config.trace_capacity = e.lever == "telemetry_capacity" ? e.requested : 16384;
  work.scratch = config.task_scratch_bytes;
  const auto period = fixed::tick_ns * (e.lever == "rate_multiplier" ? e.requested : 1);
  const auto budget = std::uint64_t{1'000'000} * (e.lever == "budget_percent" ? e.requested : 100) / 100;
  rt::RateDomainHandle rate;
  rt::PhaseHandle phase;
  rt::RateDomainRegistration registration{"showcase.probe",period,1,period,budget};
  if (e.lever == "overload_policy" && e.requested) {
    registration.late_action = rt::RateLateAction::bounded_catch_up;
    registration.bounded_catch_up_limit = 1;
  }
  bool ok = runtime.configure(config) == rt::Status::ok &&
    runtime.set_rate_execution_policy({4,2605,1,1,16384}) == rt::Status::ok &&
    runtime.register_rate_domain(registration,rate) == rt::Status::ok &&
    runtime.register_callback({"showcase.probe",TaskProbe::callback,&work},phase) == rt::Status::ok &&
    runtime.bind_phase_to_rate_domain(phase,rate) == rt::Status::ok && runtime.finalize() == rt::Status::ok;
  rt::MemoryPlan plan{};
  rt::ReferenceRelease release;
  ok = ok && runtime.memory_plan(plan) && runtime.reference_release_at(0,release) &&
       release.budget_wcet_ns == budget && release.bounded_catch_up_limit == (e.lever == "overload_policy" ? e.requested : 0) &&
       runtime.start() == rt::Status::ok;
  rt::RuntimeTraceCursor cursor;
  rt::RuntimeTraceReadResult read;
  std::array<rt::RuntimeTraceEvent,64> events{};
  ok = runtime.read_trace(cursor,events,read) == rt::Status::ok && ok;
  bool queue_rejected = false;
  for (std::size_t tick = 0; ok && tick < 24; ++tick) {
    const auto nominal = 1000 + tick * period;
    clock.now = nominal + (e.lever == "overload_policy" && tick == 6 ? period + 1 : 0);
    const auto result = runtime.step({tick,std::chrono::nanoseconds(period),std::nullopt,nominal});
    e.status = static_cast<std::int32_t>(result); ++e.operations;
    if (e.lever == "overload_policy" && !e.requested && tick == 6) {
      ok = result == rt::Status::callback_failed; ++e.rejected; break;
    }
    if (e.lever == "queue_slots" && e.requested == 4 && result == rt::Status::callback_failed) {
      queue_rejected = true; ++e.rejected; break;
    }
    ok = result == rt::Status::ok;
  }
  e.calls = work.calls;
  for (std::size_t n = 0; n < work.values.size(); ++n) {
    ok = ok && (queue_rejected ? (work.values[n] == 0 || work.values[n] == n + 1) :
                                work.values[n] == work.calls * (n + 1)); ++e.checks;
  }
  std::uint64_t retained_events = 0;
  for (std::size_t batch = 0; batch < 257; ++batch) {
    ok = runtime.read_trace(cursor,events,read) == rt::Status::ok && ok;
    retained_events += read.events_read; e.gaps += read.lost_events;
    if (!read.remaining_sequence_count) break;
    if (batch == 256) ok = false;
  }
  e.accepted = work.calls;
  if (e.lever == "telemetry_capacity") ok = ok && read.metadata.trace_capacity == e.requested &&
    (e.requested == 0 ? retained_events == 0 && e.gaps == 96 : e.requested == 64 ? e.gaps > 0 && retained_events == 64 : !e.gaps && retained_events > 64);
  e.capacity = config.executor_queue_capacity; e.scratch = work.scratch;
  e.bytes = plan.planned_bytes; ok = metadata(runtime,e) && ok;
  e.cleanup = runtime.stop() == rt::Status::ok;
  e.effective = e.requested;
  e.checksum = std::accumulate(work.values.begin(),work.values.end(),std::uint64_t{});
  return ok && e.cleanup;
}
bool device_experiment(Experiment &e) {
  e.scope = "public-hal-v2-loopback-batch-copy";
  const auto depth = static_cast<std::uint32_t>(e.lever == "device_depth" ? e.requested : 2);
  const auto batch_size = static_cast<std::size_t>(e.lever == "transfer_batch" ? e.requested : 4);
  const auto slots = static_cast<std::size_t>(e.lever == "staging_slots" ? e.requested : 2);
  // Four fixed staging pairs; only the requested pairs are registered/used.
  // All comparisons transfer exactly64 full256-lane payloads.
  constexpr auto payload_size = fixed::capacity * 4;
  constexpr auto frame_bytes = sizeof(rt::SampledIoFrameHeader) + payload_size;
  std::array<std::array<std::byte,frame_bytes>,8> storage{};
  std::array<std::uint64_t,8> tokens{};
  rt::SampledIoLoopbackBackend backend({depth,static_cast<std::uint32_t>(2 * slots),frame_bytes,1,1});
  if (backend.add_route({2605,26001,26002,1,1,1}) != rt::Status::ok) return false;
  const auto registration = backend.hal_v2_registration();
  const auto api = registration.api;
  const auto commands = *registration.command_timeline;
  rt::HalV2InitializeConfig init;
  init.requested_in_flight = depth; init.requested_registered_buffers = 2 * slots;
  if (api.initialize(api.instance,&init) != rt::HalV2Status::ok) return false;
  bool ok = true;
  std::size_t registered = 0;
  for (; registered < 2 * slots; ++registered) {
    auto &data = storage[registered];
    rt::SampledIoFrameHeader header;
    header.channel_identity = 26001; header.sequence = 1;
    header.release_generation = 1; header.sample_count = fixed::capacity;
    header.sample_interval_ns = fixed::tick_ns; header.timestamp_domain_identity = 1;
    header.trigger_identity = header.trigger_sequence = header.calibration_identity = 1;
    for (std::size_t j = 0; j < payload_size; ++j) data[sizeof(header) + j] = std::byte(j % 251);
    header.payload_checksum = rt::sampled_io_payload_checksum(std::span(data).subspan(sizeof(header)));
    std::memcpy(data.data(),&header,sizeof(header));
    rt::HalV2BufferRegistration buffer;
    buffer.data = data.data(); buffer.bytes = data.size();
    buffer.flags = RTFW_DEVICE_BUFFER_HOST_READ | RTFW_DEVICE_BUFFER_HOST_WRITE |
                   RTFW_DEVICE_BUFFER_DEVICE_READ | RTFW_DEVICE_BUFFER_DEVICE_WRITE;
    if (api.register_buffer(api.instance,&buffer,&tokens[registered]) != rt::HalV2Status::ok) { ok = false; break; }
  }
  std::uint64_t submitted = 0, completed = 0;
  const auto make_batch = [&](std::uint64_t id) {
    rt::DeviceCommandBatch batch;
    batch.batch_id = id; batch.timeout_ns = 1'000'000;
    batch.command_count = static_cast<std::uint32_t>(batch_size);
    for (std::size_t c = 0; c < batch_size; ++c) {
      const auto slot = static_cast<std::size_t>((id - 1) * batch_size + c) % slots;
      auto &command = batch.commands[c];
      command.kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
      command.opcode = 2605; command.buffer_count = 2;
      command.buffers[0] = {tokens[2 * slot],RTFW_DEVICE_ACCESS_READ,0,0,frame_bytes};
      command.buffers[1] = {tokens[2 * slot + 1],RTFW_DEVICE_ACCESS_WRITE,0,0,frame_bytes};
    }
    return batch;
  };
  while (ok && submitted < 64 / batch_size) {
    for (std::size_t n = 0; ok && n < depth && submitted < 64 / batch_size; ++n) {
      const auto batch = make_batch(submitted + 1);
      ok = commands.submit(commands.instance,&batch) == rt::HalV2Status::ok;
      if (ok) ++submitted;
    }
    // The actual finite queue, not a catalog entry, rejects excess work.
    const auto excess = make_batch(1000 + submitted);
    if (commands.submit(commands.instance,&excess) != rt::HalV2Status::queue_full) ok = false;
    else ++e.rejected;
    std::array<rt::HalV2BatchCompletion,2> results{};
    std::uint64_t count = 0;
    ok = commands.poll(commands.instance,results.data(),results.size(),&count) == rt::HalV2Status::ok && ok;
    if (count != depth) ok = false;
    for (std::size_t n = 0; n < count; ++n) {
      ok = ok && results[n].status == static_cast<std::int32_t>(rt::HalV2Status::ok) &&
           results[n].batch_id > completed && results[n].batch_id <= submitted;
      ++completed;
    }
  }
  for (std::size_t slot = 0; ok && slot < slots; ++slot) {
    rt::SampledIoFrameHeader header;
    std::memcpy(&header,storage[2 * slot + 1].data(),sizeof(header));
    ok = header.channel_identity == 26002 && header.status == static_cast<std::uint32_t>(rt::SampledIoFrameStatus::produced);
    for (std::size_t j = 0; j < payload_size; ++j) {
      ok = ok && storage[2 * slot + 1][sizeof(header) + j] == std::byte(j % 251); ++e.checks;
    }
  }
  const auto stats = backend.stats();
  e.accepted = stats.submissions; e.operations = stats.frames_copied;
  e.calls = completed; e.capacity = depth; e.bytes = stats.frames_copied * payload_size;
  ok = ok && stats.frames_copied == 64 && stats.submissions == 64 / batch_size &&
       stats.completions == stats.submissions && stats.rejected == e.rejected;
  e.cleanup = commands.request_stop(commands.instance) == rt::HalV2Status::ok;
  for (std::size_t n = 0; n < registered; ++n)
    e.cleanup = api.unregister_buffer(api.instance,tokens[n]) == rt::HalV2Status::ok && e.cleanup;
  e.cleanup = api.shutdown(api.instance) == rt::HalV2Status::ok && e.cleanup;
  e.effective = e.requested; e.scratch = slots;
  return ok && e.cleanup;
}
} // namespace
bool experiment(Experiment &e) {
  if (!finite_value(e.lever,e.requested)) return false;
  if (e.lever == "workers" || e.lever == "grain" || e.lever == "cuda_graph") e.correct = golden_loop(e);
  else if (e.lever == "control_burst") e.correct = burst(e);
  else if (e.lever == "device_depth" || e.lever == "transfer_batch" || e.lever == "staging_slots") e.correct = device_experiment(e);
  else e.correct = cpu_experiment(e);
  return e.correct;
}
} // namespace golden::showcase
