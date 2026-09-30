#include "../../samples/golden_system/oracle.hpp"
#include "../../samples/golden_system/replay.hpp"
#include "../cuda_physics/allocation_guard.hpp"
#include <iostream>
#include <memory>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::cerr << __func__ << ':' << __LINE__ << " failed: " #x << '\n';      \
      return false;                                                            \
    }                                                                          \
  } while (false)
using namespace golden;
using rt::Status;
namespace allocation = rtfw_physics_allocation;
bool run(Options o) {
  auto memory = std::make_unique<Memory>();
  auto jobs = std::make_unique<Jobs>();
  if (o.host)
    CHECK(jobs->start(o.workers) == Status::ok);
  auto s = std::make_unique<Session>(o, o.host ? jobs.get() : nullptr, *memory);
  CHECK(s->prepare() == Status::ok);
  CHECK(s->plan.phase_count == 8 &&
        s->plan.registered_state_bytes == state_bytes);
  Replay replay(o.ticks);
  CHECK(replay.begin(*s));
  CHECK(s->controls());
  Oracle oracle(o);
  for (std::size_t t = 0; t < o.ticks; ++t) {
    replay.record(*s, t);
    allocation::begin();
    auto status = s->step(t);
    auto count = allocation::end();
    CHECK(status == Status::ok && count == 0);
    CHECK(oracle.step(t, s->world));
  }
  rt::LiveControlMailboxInfo mailbox;
  rt::LiveControlCommitInfo commit;
  CHECK(s->runtime->live_control_mailbox_info(2601, mailbox) &&
        s->runtime->live_control_commit_info(commit));
  CHECK(mailbox.occupancy == 0 &&
        mailbox.invalid ==
            (o.campaign == Campaign::control_rejected ? 1u : 0u));
  CHECK(commit.replaced ==
        (o.campaign == Campaign::control_replaced ? 1u : 0u));
  CHECK(replay.seal(*s));
  allocation::begin();
  bool matched = replay.verify(*s);
  auto count = allocation::end();
  if (!matched)
    std::cerr << s->runtime->last_error() << '\n';
  CHECK(matched && count == 0);
  auto corrupted = replay.trusted;
  corrupted.back() ^= std::byte{1};
  auto before = s->world.canonical;
  CHECK(s->runtime->replay_live_control(corrupted, Replay::input, s.get()) !=
            Status::ok &&
        s->world.canonical == before);
  // Active artifacts bind the originating instance; a fresh instance must
  // reject.
  auto memory2 = std::make_unique<Memory>();
  auto other =
      std::make_unique<Session>(o, o.host ? jobs.get() : nullptr, *memory2);
  CHECK(other->prepare() == Status::ok);
  CHECK(other->runtime->replay_active(replay.active, Replay::input,
                                      other.get()) != Status::ok);
  CHECK(other->world.next_tick == 0);
  auto cp = s->checkpoint(o.ticks - 1);
  CHECK(!cp.empty());
  CHECK(other->runtime->restore_checkpoint(cp) == Status::ok &&
        other->world.decode() && other->world.canonical == s->world.canonical);
  CHECK(other->close() == Status::ok);
  other.reset();
  CHECK(s->close() == Status::ok);
  s.reset();
  CHECK(memory->acquisitions == memory->releases &&
        memory2->acquisitions == memory2->releases);
  if (o.host) {
    CHECK(jobs->accepted.load() > 0 &&
          jobs->accepted.load() == jobs->completed.load());
    CHECK(jobs->close() == Status::ok);
  }
  return true;
}
bool recovery(bool host) {
  Options o;
  o.host = host;
  o.campaign = Campaign::overload;
  auto memory = std::make_unique<Memory>();
  auto jobs = std::make_unique<Jobs>();
  if (host)
    CHECK(jobs->start() == Status::ok);
  auto s = std::make_unique<Session>(o, host ? jobs.get() : nullptr, *memory);
  CHECK(s->prepare() == Status::ok && s->controls());
  Oracle oracle(o);
  for (std::size_t t = 0; t < 6; ++t) {
    CHECK(s->step(t) == Status::ok);
    CHECK(oracle.step(t, s->world));
  }
  auto checkpoint = s->checkpoint(5);
  CHECK(!checkpoint.empty());
  const auto old_handle = s->producer;
  CHECK(s->step(6, true) != Status::ok);
  CHECK(s->world.calls[0] == 6);
  CHECK(s->close() == Status::ok);
  s.reset();
  s = std::make_unique<Session>(o, host ? jobs.get() : nullptr, *memory);
  CHECK(s->prepare() == Status::ok);
  CHECK(s->runtime->restore_checkpoint(checkpoint) == Status::ok &&
        s->world.decode());
  rt::LiveControlTypedPayload<Control<1>> payload;
  rt::LiveControlUpdateRecord record;
  CHECK(rt::make_live_control_host_update(old_handle, 99, 7, Control<1>{16},
                                          payload, record) ==
        rt::LiveControlTypedStatus::ok);
  rt::LiveControlAdmissionResult admission;
  CHECK(s->runtime->stage_live_control_update(old_handle, record, payload,
                                              admission) == Status::ok &&
        admission == rt::LiveControlAdmissionResult::stale);
  for (std::size_t t = 6; t < o.ticks; ++t) {
    CHECK(s->step(t) == Status::ok);
    CHECK(oracle.step(t, s->world));
  }
  CHECK(s->close() == Status::ok);
  s.reset();
  if (host)
    CHECK(jobs->close() == Status::ok);
  return true;
}
bool gap_rejection() {
  Options o;
  o.ticks = 128;
  auto memory = std::make_unique<Memory>();
  auto s = std::make_unique<Session>(o, nullptr, *memory);
  CHECK(s->prepare(128) == Status::ok);
  Replay replay(o.ticks);
  CHECK(replay.begin(*s) && s->controls());
  Oracle oracle(o);
  for (std::size_t tick = 0; tick < o.ticks; ++tick) {
    replay.record(*s, tick);
    CHECK(s->step(tick) == Status::ok && oracle.step(tick, s->world));
  }
  rt::MixedRateActionMetadata metadata;
  CHECK(s->runtime->mixed_rate_action_metadata(metadata) == Status::ok &&
        metadata.records_dropped + metadata.records_overwritten > 0 &&
        !metadata.replay_eligible);
  auto before = s->world.canonical;
  CHECK(!replay.seal(*s) && s->world.canonical == before);
  CHECK(s->close() == Status::ok);
  return true;
}
#include "protocol_checks.hpp"
int main() {
  if (!gap_rejection())
    return 1;
  if (!protocol_checks() || !concurrent_owners() || !queue_checks())
    return 1;
  for (bool host : {false, true}) {
    for (std::size_t workers : {1u, 2u, 3u})
      for (std::size_t grain : {1u, 4u, 16u, 64u}) {
        Options o;
        o.host = host;
        o.workers = workers;
        o.grain = grain;
        o.count = workers == 3 ? 256 : 16;
        if (!run(o))
          return 1;
      }
    for (auto campaign : {Campaign::stale_input, Campaign::control_rejected,
                          Campaign::control_replaced, Campaign::peer_missing}) {
      Options o;
      o.host = host;
      o.campaign = campaign;
      if (!run(o))
        return 1;
    }
    if (!recovery(host))
      return 1;
  }
  std::cout << "golden state, controls, replay, recovery, allocation: PASS\n";
}
