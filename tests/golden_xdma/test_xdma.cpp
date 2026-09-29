#include "../../samples/golden_cuda/owner.hpp"
#include "../../samples/golden_system/oracle.hpp"
#include "../../samples/golden_xdma/oracle.hpp"
#include "../../samples/golden_xdma/owner.hpp"
#include "../../samples/golden_xdma/replay.hpp"
#include "../../samples/golden_xdma/telemetry.hpp"
#include "../golden_cuda/allocation.hpp"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <vector>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::cerr << __func__ << ':' << __LINE__ << " failed: " #x << '\n';      \
      return false;                                                            \
    }                                                                          \
  } while (false)
using namespace golden;
namespace gx = golden::xdma;
namespace allocation = rtfw_physics_allocation;
using rt::Status;
bool same(const StateBytes &a, const StateBytes &b) {
  for (std::size_t i = 0; i < a.size(); ++i) {
    if ((i >= 64 && i < 72) || (i >= 480 && i < 488))
      continue;
    if (a[i] != b[i]) {
      std::cerr << "state differs at " << i << '\n';
      return false;
    }
  }
  auto first = a, second = b;
  put(first, 480, 0, 8);
  put(second, 480, 0, 8);
  return digest(first) == get(a, 480, 8) && digest(second) == get(b, 480, 8) &&
         bool(get(a, 64, 8)) == bool(get(b, 64, 8));
}
bool counts(gx::Owner &owner, std::uint64_t sensor, std::uint64_t actuator,
            std::uint64_t safe) {
  const auto native = owner.io->native_capabilities();
  CHECK(native.deterministic_mock == 0 && native.max_in_flight == 2);
  for (std::size_t i = 0; i < (owner.physics ? 3u : 2u); ++i) {
    rt::CompiledDeviceRatePhase phase;
    CHECK(owner.session->runtime->compiled_device_rate_phase_at(i, phase));
    CHECK(phase.maximum_in_flight == 1 && phase.simulation &&
          phase.simulation->host_watchdog_ns == 5'000'000'000ull);
    CHECK(phase.completion_budget_ns == (owner.physics && i == 0
                                             ? golden::cuda::completion_ns
                                             : gx::completion_ns));
  }
  for (std::size_t lane = 0; lane < 2; ++lane) {
    const auto expected = lane ? actuator : sensor;
    rt::DeviceTimelineInfo t;
    CHECK(owner.io->timeline(lane, t));
    // Startup safety uses these public timelines too.
    CHECK(t.completed_value == expected + 1 &&
          t.last_accepted_value == expected + 1);
    CHECK(owner.io->providers[lane] == expected &&
          owner.io->publications[lane] == expected);
  }
  const auto transfers = sensor + actuator + safe;
  CHECK(owner.driver->uploads == 2 * transfers &&
        owner.driver->downloads == transfers);
  CHECK(owner.driver->controls == transfers &&
        owner.driver->events == transfers);
  CHECK(owner.driver->safe_acks == safe && owner.driver->faults == 0);
  return true;
}
bool parity(Options o, int selected_dispatch = -1,
            const std::filesystem::path &path = {}) {
  std::vector<StateBytes> reference(o.ticks);
  if (selected_dispatch >= 0) {
    std::ifstream stream(path.string() + ".states",
                         std::ios::binary | std::ios::ate);
    CHECK(stream &&
          stream.tellg() == static_cast<std::streamoff>(reference.size() *
                                                        sizeof(StateBytes)));
    stream.seekg(0);
    stream.read(
        reinterpret_cast<char *>(reference.data()),
        static_cast<std::streamsize>(reference.size() * sizeof(StateBytes)));
    CHECK(bool(stream));
  } else {
    auto memory = std::make_unique<Memory>();
    auto jobs = std::make_unique<Jobs>();
    if (o.host)
      CHECK(jobs->start(o.workers) == Status::ok);
    auto s = std::make_unique<golden::Session>(o, o.host ? jobs.get() : nullptr,
                                               *memory);
    auto policy = Memory::policy();
    policy.thread_policy_count = 1;
    policy.thread_policies[0].role = rt::thread_role_executor_worker;
    policy.thread_policies[0].policy.wait_strategy = rt::WaitStrategy::park;
    CHECK(s->prepare(fixed::action_capacity, &policy) == Status::ok &&
          s->controls());
    Oracle oracle(o);
    for (std::size_t t = 0; t < o.ticks; ++t) {
      CHECK(s->step(t) == Status::ok && oracle.step(t, s->world));
      reference[t] = s->world.canonical;
    }
    CHECK(s->close() == Status::ok);
    s.reset();
    if (o.host)
      CHECK(jobs->close() == Status::ok);
  }
  if (selected_dispatch == -2) {
    std::ofstream stream(path.string() + ".states", std::ios::binary);
    stream.write(
        reinterpret_cast<const char *>(reference.data()),
        static_cast<std::streamsize>(reference.size() * sizeof(StateBytes)));
    stream.close();
    CHECK(!stream.fail());
    return true;
  }
  for (auto dispatch :
       {gx::Dispatch::cpu, gx::Dispatch::kernel, gx::Dispatch::graph}) {
    if (selected_dispatch >= 0 && int(dispatch) != selected_dispatch)
      continue;
    std::cout << "parity dispatch=" << int(dispatch) << " host=" << o.host
              << " count=" << o.count << " ticks=" << o.ticks << std::endl;
    auto owner = std::make_unique<gx::Owner>(o, dispatch);
    auto status = owner->prepare();
    if (status != Status::ok)
      std::cerr << "prepare=" << int(status) << ' '
                << owner->session->runtime->last_error() << '\n';
    CHECK(status == Status::ok);
    auto &s = *owner->session;
    gx::Replay replay(o.ticks);
    CHECK(replay.begin(s) && s.controls());
    Oracle oracle(o);
    for (std::size_t t = 0; t < o.ticks; ++t) {
      replay.record(s, t);
      allocation::begin();
      status = s.step(t);
      auto allocations = allocation::end();
      if (status != Status::ok)
        std::cerr << "tick=" << t << " status=" << int(status) << ' '
                  << s.runtime->last_error() << '\n';
      CHECK(status == Status::ok && allocations == 0);
      CHECK(oracle.step(t, s.world) && same(reference[t], s.world.canonical));
    }
    rt::LiveControlCommitInfo commit;
    CHECK(s.runtime->live_control_commit_info(commit));
    CHECK(get(s.world.canonical, 64, 8) == commit.generation_identity);
    CHECK(get(s.world.canonical, 392, 8) == commit.committed);
    CHECK(replay.seal(s));
    allocation::begin();
    const auto replayed = replay.verify(s);
    auto allocations = allocation::end();
    if (!replayed)
      std::cerr << "replay=" << int(replay.result.mismatch_status) << ' '
                << s.runtime->last_error() << '\n';
    CHECK(replayed && allocations == 0);
    CHECK(counts(*owner, 2 * ((o.ticks - 1) / 2 + 1),
                 2 * ((o.ticks - 1) / 3 + 1), 2));
    auto malformed = replay.trusted;
    malformed.back() ^= std::byte{1};
    const auto before = s.world.canonical;
    const auto uploads = owner->driver->uploads.load();
    CHECK(gx::Replay::apply(s, malformed) != Status::ok);
    CHECK(s.world.canonical == before && owner->driver->uploads == uploads);
    CHECK(owner->close() == Status::ok);
    CHECK(owner->driver->safe_acks == 4);
    CHECK(owner->memory->acquisitions == owner->memory->releases);
    CHECK(owner->driver->initializes == 1 && owner->driver->shutdowns == 1);
  }
  return true;
}
bool allocation_control() {
  allocation::begin();
  auto *ordinary = ::operator new(17);
  *static_cast<volatile unsigned char *>(ordinary) = 17;
  const auto ordinary_count = allocation::end();
  ::operator delete(ordinary);
  CHECK(ordinary_count == 1);
  allocation::begin();
  auto *aligned = ::operator new (64, std::align_val_t{64});
  *static_cast<volatile unsigned char *>(aligned) = 64;
  const auto aligned_count = allocation::end();
  ::operator delete (aligned, std::align_val_t{64});
  CHECK(aligned_count == 1);
  std::cout
      << "allocation controls ordinary=" << ordinary_count
      << " aligned=" << aligned_count
      << "; steady/replay assertions require zero including native workers\n";
  return true;
}

bool contracts();
bool transport_faults();
bool native_queue();
bool additional_replay();
int main(int argc, char **argv) {
  std::cout << std::unitbuf;
  int selected = -1, selected_dispatch = -1;
  std::filesystem::path reference;
  if (argc != 1) {
    if ((argc != 3 && argc != 7) || std::string_view(argv[1]) != "--case")
      return 64;
    const std::string_view value = argv[2];
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), selected);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        selected < 0 || selected > 18)
      return 64;
  }
  if (argc == 7) {
    if ((selected != 9 && selected != 18) ||
        std::string_view(argv[3]) != "--dispatch" ||
        std::string_view(argv[5]) != "--reference")
      return 64;
    const std::string_view dispatch = argv[4];
    selected_dispatch = dispatch == "reference" ? -2
                        : dispatch == "cpu"     ? 0
                        : dispatch == "kernel"  ? 1
                        : dispatch == "graph"   ? 2
                                                : -1;
    if (selected_dispatch == -1 || !*argv[6])
      return 64;
    reference = argv[6];
  }
  if (selected <= 0) {
    if (!allocation_control() || !contracts() || !transport_faults() ||
        !native_queue() || !additional_replay())
      return 1;
  }
  int case_number = 0;
  const auto run_case = [&](Options o) {
    ++case_number;
    return selected >= 0 && selected != case_number
               ? true
               : parity(o, selected_dispatch, reference);
  };
  for (bool host : {false, true}) {
    for (std::size_t i = 0; i < 4; ++i) {
      Options o;
      o.host = host;
      o.count = std::array<std::size_t, 4>{1, 17, 16, 256}[i];
      o.workers = static_cast<std::uint32_t>(i % 3 + 1);
      o.grain = std::array<std::size_t, 4>{1, 4, 16, 64}[i];
      if (!run_case(o))
        return 1;
    }
    for (auto campaign :
         {Campaign::control_rejected, Campaign::control_replaced,
          Campaign::stale_input, Campaign::peer_missing}) {
      Options o;
      o.host = host;
      o.campaign = campaign;
      if (!run_case(o))
        return 1;
    }
    Options max;
    max.host = host;
    max.count = 256;
    max.ticks = 1024;
    max.workers = 3;
    max.grain = 64;
    if (!run_case(max))
      return 1;
  }
  std::cout << "PASS requested golden XDMA test cases\n";
}

bool contracts() {
  { // The typed owned driver can explicitly exercise unchanged native policy.
    auto memory = std::make_unique<Memory>();
    gx::SimulatedDriver driver(16);
    gx::Io native(driver, false);
    gx::Session s(Options{}, nullptr, *memory, native);
    const auto status = s.prepare();
    std::cout << "native rejection=" << int(status) << ' '
              << s.runtime->last_error() << '\n';
    CHECK(status == Status::invalid_config && driver.uploads == 0);
    CHECK(native.native_capabilities().deterministic_mock == 0);
    CHECK(s.close() == Status::ok && !driver.live);
  }
  { // Failure before initialization, then retryable native teardown failure.
    gx::Owner failed(Options{});
    failed.driver->fail_initialize = true;
    CHECK(failed.prepare() != Status::ok && failed.driver->uploads == 0);
    CHECK(failed.close() == Status::ok && !failed.driver->live);
    gx::Owner retry(Options{}, gx::Dispatch::graph);
    CHECK(retry.prepare() == Status::ok &&
          retry.session->step(0) == Status::ok);
    retry.driver->fail_shutdown = true;
    CHECK(retry.close() != Status::ok && retry.session && retry.io &&
          retry.driver->live);
    CHECK(retry.memory->live_count() == 3);
    const auto ack = retry.driver->safe_acks.load();
    CHECK(retry.close() == Status::ok && retry.driver->safe_acks == ack);
  }
  { // Acquire then fail initialize; failed rollback retains ownership for
    // retry.
    gx::SimulatedDriver driver(16);
    rt::XdmaBackendConfig config;
    config.queue_capacity = 2;
    config.buffer_capacity = 6;
    rt::XdmaDeviceBackend backend(driver.api(), config);
    const auto registration = backend.hal_v2_registration("partial.xdma");
    const auto api = registration.api;
    driver.fail_after_acquire = true;
    driver.fail_shutdown = true;
    rt::HalV2InitializeConfig request;
    request.requested_in_flight = 2;
    request.requested_registered_buffers = 6;
    CHECK(api.initialize(api.instance, &request) != rt::HalV2Status::ok &&
          driver.live);
    CHECK(api.shutdown(api.instance) == rt::HalV2Status::ok && !driver.live);
    CHECK(driver.initializes == 1 && driver.shutdowns == 1);
  }
  { // Lossless artifacts belong to the originating owner and configuration.
    gx::Owner first(Options{}), foreign(Options{}),
        variant(Options{}, gx::Dispatch::graph);
    CHECK(first.prepare() == Status::ok && foreign.prepare() == Status::ok &&
          variant.prepare() == Status::ok);
    gx::Replay replay(24);
    CHECK(replay.begin(*first.session) && first.session->controls());
    for (std::size_t t = 0; t < 24; ++t) {
      replay.record(*first.session, t);
      CHECK(first.session->step(t) == Status::ok);
    }
    CHECK(replay.seal(*first.session));
    for (auto *owner : {&foreign, &variant}) {
      const auto state = owner->session->world.canonical;
      const auto uploads = owner->driver->uploads.load();
      CHECK(gx::Replay::apply(*owner->session, replay.trusted) ==
            Status::incompatible_artifact);
      CHECK(owner->session->world.canonical == state &&
            owner->driver->uploads == uploads);
    }
    const auto uploads = variant.driver->uploads.load();
    CHECK(variant.session->runtime->restore_checkpoint(replay.initial) ==
          Status::incompatible_artifact);
    CHECK(variant.driver->uploads == uploads);
    CHECK(first.close() == Status::ok && foreign.close() == Status::ok &&
          variant.close() == Status::ok);
  }
  { // Concurrent native workers are owned independently; setup is off lane.
    Options o;
    o.workers = 2;
    gx::Owner first(o, gx::Dispatch::kernel), second(o, gx::Dispatch::graph);
    CHECK(first.prepare() == Status::ok && second.prepare() == Status::ok);
    CHECK(first.session->controls() && second.session->controls());
    auto run = [&](gx::Owner &owner) {
      Oracle oracle(o);
      for (std::size_t t = 0; t < o.ticks; ++t) {
        if (owner.session->step(t) != Status::ok ||
            !oracle.step(t, owner.session->world))
          return false;
      }
      return true;
    };
    auto a = std::async(std::launch::async, [&] { return run(first); });
    auto b = std::async(std::launch::async, [&] { return run(second); });
    CHECK(a.get() && b.get());
    CHECK(counts(first, 12, 8, 2) && counts(second, 12, 8, 2));
    CHECK(
        same(first.session->world.canonical, second.session->world.canonical));
    CHECK(first.close() == Status::ok && second.close() == Status::ok);
  }
  { // Small retention cannot silently manufacture a complete replay.
    gx::Owner owner(Options{});
    CHECK(owner.session->prepare(8) == Status::ok);
    gx::Replay replay(24);
    gx::Telemetry telemetry;
    CHECK(replay.begin(*owner.session) && owner.session->controls());
    for (std::size_t t = 0; t < 24; ++t) {
      replay.record(*owner.session, t);
      CHECK(owner.session->step(t) == Status::ok);
    }
    CHECK(telemetry.drain(*owner.session->runtime) ==
              Status::resource_exhausted &&
          telemetry.action_gaps > 0);
    CHECK(!replay.seal(*owner.session));
    CHECK(owner.close() == Status::ok);
  }
  return true;
}

bool transport_faults() {
  using F = gx::SimulatedDriver::Fault;
  for (auto dispatch :
       {gx::Dispatch::cpu, gx::Dispatch::kernel, gx::Dispatch::graph}) {
    for (auto fault : {F::short_transfer, F::transfer_timeout, F::device_loss,
                       F::reset_required, F::stale_sequence, F::stale_timestamp,
                       F::payload_corrupt, F::header_corrupt, F::delayed}) {
      gx::Owner owner(Options{}, dispatch);
      CHECK(owner.prepare() == Status::ok);
      owner.driver->fault_lane = 0;
      owner.driver->fault = fault;
      const auto before = owner.session->world.sensor;
      const auto result = owner.session->step(0);
      std::cout << "transport dispatch=" << int(dispatch)
                << " fault=" << int(fault) << " status=" << int(result) << '\n';
      if (fault == F::delayed) {
        CHECK(result == Status::ok && owner.driver->delayed_events == 1);
        CHECK(counts(owner, 1, 1, 2));
      } else {
        const auto expected =
            fault == F::transfer_timeout ? Status::device_timeout
            : fault == F::device_loss    ? Status::device_lost
            : fault == F::reset_required ? Status::device_reset_required
                                         : Status::device_error;
        CHECK(result == expected && owner.driver->faults == 1);
        CHECK(owner.session->world.sensor.position == before.position &&
              owner.session->world.sensor.velocity == before.velocity);
        CHECK(owner.io->publications[0] == 0 &&
              owner.session->world.calls[4] == 0);
        if (fault == F::reset_required) {
          owner.driver->fail_reset = true;
          CHECK(owner.io->reset() != Status::ok && owner.driver->live &&
                owner.memory->live_count() == 3);
          CHECK(owner.io->reset() == Status::ok);
        }
      }
      CHECK(owner.close() == Status::ok);
      CHECK(!owner.driver->live &&
            owner.memory->acquisitions == owner.memory->releases);
    }
  }
  return true;
}

bool native_queue() {
  // Independent public native-protocol fixture: two slots and one worker make
  // queued cancellation observable. This is not the scenario's physical graph.
  gx::SimulatedDriver driver(16);
  Frame input{}, output{};
  std::array<std::byte, 4> ack{};
  gx::sampled_header(input, 0, 1, 0, 0, rt::SampledIoFrameStatus::initial);
  gx::sampled_header(output, 1, 2, 1, gx::clock_origin,
                     rt::SampledIoFrameStatus::produced);
  rt::XdmaBackendConfig config;
  config.queue_capacity = 2;
  config.buffer_capacity = 3;
  config.worker_count = 1;
  config.max_transfer_bytes = config.max_buffer_bytes = maximum_frame_bytes;
  config.control_aperture_bytes = 8;
  config.user_event_count = 2;
  rt::XdmaDeviceBackend backend(driver.api(), config);
  auto registration = backend.hal_v2_registration("bounded.native.xdma");
  auto api = registration.api;
  auto command = *registration.command_timeline;
  rt::HalV2InitializeConfig request;
  request.requested_in_flight = 2;
  request.requested_registered_buffers = 3;
  CHECK(api.initialize(api.instance, &request) == rt::HalV2Status::ok);
  const std::array<std::span<std::byte>, 3> storage{frame_span(input, 0),
                                                    frame_span(output, 1), ack};
  std::array<std::uint64_t, 3> tokens{};
  for (std::size_t i = 0; i < 3; ++i) {
    rt::HalV2BufferRegistration buffer;
    buffer.data = storage[i].data();
    buffer.bytes = storage[i].size();
    buffer.name[0] = static_cast<char>('a' + i);
    buffer.flags =
        RTFW_DEVICE_BUFFER_HOST_READ | RTFW_DEVICE_BUFFER_HOST_WRITE |
        RTFW_DEVICE_BUFFER_DEVICE_READ | RTFW_DEVICE_BUFFER_DEVICE_WRITE;
    CHECK(api.register_buffer(api.instance, &buffer, &tokens[i]) ==
          rt::HalV2Status::ok);
  }
  rt::DeviceCommandBatch batch;
  batch.batch_id = 1;
  batch.timeout_ns = 8'000'000;
  batch.command_count = 5;
  batch.signal_count = 1;
  batch.signals[0].timeline_handle = 1;
  batch.signals[0].value = 1;
  const auto transfer = [&](std::size_t at, std::size_t buffer, bool down) {
    auto &c = batch.commands[at];
    c.kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
    c.opcode = down ? rt::xdma_device_opcode_card_to_host
                    : rt::xdma_device_opcode_host_to_card;
    c.buffer_count = 1;
    c.buffers[0].buffer_token = tokens[buffer];
    c.buffers[0].bytes = storage[buffer].size();
    c.buffers[0].access =
        down ? RTFW_DEVICE_ACCESS_WRITE : RTFW_DEVICE_ACCESS_READ;
    rt::XdmaTransfer t;
    t.device_offset = buffer ? 8192 : 0;
    c.payload_size = sizeof(t);
    std::memcpy(c.payload.data(), &t, sizeof(t));
  };
  transfer(0, 0, false);
  transfer(1, 1, false);
  transfer(4, 1, true);
  CHECK(rt::set_xdma_control_write(batch.commands[2], 0, 0));
  rt::HalV2BufferReference reference;
  reference.buffer_token = tokens[2];
  reference.bytes = 4;
  reference.access = RTFW_DEVICE_ACCESS_WRITE;
  CHECK(rt::set_xdma_user_event_wait(batch.commands[3], 0, reference));
  driver.hold_event = true;
  CHECK(command.submit(command.instance, &batch) == rt::HalV2Status::ok);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!driver.event_waiting && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  CHECK(driver.event_waiting);
  rt::HalV2BatchCompletion completion;
  std::uint64_t n = 0;
  CHECK(command.poll(command.instance, &completion, 1, &n) ==
            rt::HalV2Status::ok &&
        n == 0);
  batch.batch_id = 2;
  batch.signals[0].value = 2;
  CHECK(command.submit(command.instance, &batch) == rt::HalV2Status::ok);
  batch.batch_id = 3;
  batch.signals[0].value = 3;
  CHECK(command.submit(command.instance, &batch) ==
        rt::HalV2Status::queue_full);
  CHECK(command.cancel(command.instance, 2) == rt::HalV2Status::ok &&
        driver.stop_requests == 0);
  CHECK(command.poll(command.instance, &completion, 1, &n) ==
            rt::HalV2Status::ok &&
        n == 1);
  CHECK(completion.batch_id == 2 &&
        completion.status ==
            static_cast<std::int32_t>(rt::HalV2Status::canceled));
  driver.hold_event = false;
  const auto finish =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  n = 0;
  while (!n && std::chrono::steady_clock::now() < finish) {
    CHECK(command.poll(command.instance, &completion, 1, &n) ==
          rt::HalV2Status::ok);
    std::this_thread::yield();
  }
  CHECK(n == 1 && completion.batch_id == 1 &&
        completion.status == static_cast<std::int32_t>(rt::HalV2Status::ok));
  rt::HalV2Health health;
  CHECK(api.get_health(api.instance, &health) == rt::HalV2Status::ok);
  CHECK(health.submissions == 2 && health.completions == 2 &&
        health.cancellations == 1 && health.outstanding == 0);
  CHECK(driver.uploads == 2 && driver.downloads == 1 && driver.events == 1 &&
        driver.controls == 1);
  for (auto token : tokens)
    CHECK(api.unregister_buffer(api.instance, token) == rt::HalV2Status::ok);
  CHECK(api.shutdown(api.instance) == rt::HalV2Status::ok && !driver.live);
  return true;
}

bool additional_replay() {
  for (auto dispatch :
       {gx::Dispatch::cpu, gx::Dispatch::kernel, gx::Dispatch::graph}) {
    Options options;
    gx::Owner owner(options, dispatch);
    CHECK(owner.prepare() == Status::ok);
    auto &s = *owner.session;
    gx::Replay replay(options.ticks);
    gx::Oracle oracle(options, true);
    CHECK(replay.begin(s) && s.controls() && s.stage<4>(6, 3));
    for (std::size_t t = 0; t < options.ticks; ++t) {
      replay.record(s, t);
      allocation::begin();
      const auto result = s.step(t);
      const auto allocations = allocation::end();
      CHECK(result == Status::ok && allocations == 0 &&
            oracle.step(t, s.world));
      if (t == 6 || t == 9) {
        rt::SampledIoChannelStatus status;
        CHECK(
            s.runtime->sampled_io_channel_status(s.world.channels[2], status));
        CHECK(status.underruns == (t == 6 ? 1u : 2u) &&
              status.substituted_frames == status.underruns);
        CHECK(owner.driver->safe_acks == (t == 6 ? 3u : 4u));
        CHECK(owner.io->publications[1] == t / 3 + 1);
      }
    }
    CHECK(replay.seal(s));
    allocation::begin();
    const auto result = replay.verify(s);
    const auto allocations = allocation::end();
    CHECK(result && allocations == 0);
    CHECK(owner.close() == Status::ok && owner.driver->safe_acks == 8);
  }
  // CPU-only and CUDA-only snapshots cannot restore a sampled graph.
  gx::Owner sampled(Options{}, gx::Dispatch::graph);
  CHECK(sampled.prepare() == Status::ok);
  for (auto dispatch :
       {golden::cuda::Dispatch::cpu, golden::cuda::Dispatch::kernel}) {
    golden::cuda::Owner source(Options{}, dispatch);
    CHECK(source.prepare() == Status::ok);
    auto cp = source.session->checkpoint(0);
    const auto state = sampled.session->world.canonical;
    const auto uploads = sampled.driver->uploads.load();
    CHECK(sampled.session->runtime->restore_checkpoint(cp) ==
          Status::incompatible_artifact);
    CHECK(sampled.session->world.canonical == state &&
          sampled.driver->uploads == uploads);
    CHECK(source.close() == Status::ok);
  }
  gx::Owner kernel(Options{}, gx::Dispatch::kernel);
  CHECK(kernel.prepare() == Status::ok);
  auto checkpoint = kernel.session->checkpoint(0);
  const auto uploads = sampled.driver->uploads.load();
  CHECK(sampled.session->runtime->restore_checkpoint(checkpoint) ==
        Status::incompatible_artifact);
  CHECK(sampled.driver->uploads == uploads);
  CHECK(kernel.close() == Status::ok && sampled.close() == Status::ok);
  return true;
}
