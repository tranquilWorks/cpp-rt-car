#pragma once
#include "../../samples/golden_system/channel.hpp"
#include "../../samples/golden_system/wire.hpp"

bool protocol_checks() {
  namespace c = golden::cil;
  c::Record r;
  r.generation = 17;
  r.sequence = 1;
  r.count = 2;
  r.values[0][0] = -4176;
  r.values[2][1] = 4176;
  c::Wire wire;
  CHECK(c::encode(r, wire) == c::Code::ok);
  c::Record out;
  c::Sequence seq;
  CHECK(c::accept(wire, c::Kind::plant, 17, seq, 0, out) == c::Code::ok);
  CHECK(out.values == r.values && seq.next == 2);
  CHECK(c::accept(wire, c::Kind::plant, 17, seq, 0, out) == c::Code::sequence);
  seq = {};
  CHECK(c::accept(wire, c::Kind::plant, 18, seq, 0, out) ==
            c::Code::generation &&
        seq.next == 1);
  CHECK(c::accept(wire, c::Kind::plant, 17, seq, 1, out) == c::Code::expired &&
        seq.next == 1);
  for (std::size_t i = 0; i < wire.size(); ++i) {
    auto damaged = wire;
    damaged[i] ^= std::byte{1};
    CHECK(c::decode(damaged, out) != c::Code::ok);
  }
  auto bad = r;
  bad.count = 257;
  CHECK(c::encode(bad, wire) != c::Code::ok);
  bad = r;
  bad.values[1][2] = 1;
  CHECK(c::encode(bad, wire) != c::Code::ok);
  bad = r;
  bad.kind = c::Kind::command;
  bad.correlation = 1;
  CHECK(c::encode(bad, wire) != c::Code::ok);
  CHECK(c::encode(r, wire) == c::Code::ok);
  c::Slot slot{};
  CHECK(c::push(slot, wire) == c::Code::ok &&
        c::push(slot, wire) == c::Code::full);
  c::Wire copied;
  CHECK(c::pop(slot, copied) == c::Code::ok && copied == wire &&
        c::pop(slot, copied) == c::Code::empty);
  c::Region region{};
  CHECK(c::initialize(region, 17) == c::Code::ok);
  CHECK(c::initialize(region, 18) == c::Code::invalid &&
        c::attach(region, 18) == c::Code::generation);
  CHECK(c::attach(region, 17) == c::Code::ok &&
        c::attach(region, 17) == c::Code::attached);
  c::store(region.stopping, 1);
  CHECK(c::inspect(region, 17) == c::Code::closed);
  rt::LiveControlTypedPayload<Control<2>> payload{};
  CHECK(rt::encode_live_control_typed_payload(Control<2>{5}, payload) !=
        rt::LiveControlTypedStatus::ok);
  return true;
}
bool concurrent_owners() {
  Options o;
  o.host = true;
  o.workers = 3;
  auto jobs = std::make_unique<Jobs>();
  CHECK(jobs->start(3) == Status::ok);
  auto ma = std::make_unique<Memory>();
  auto mb = std::make_unique<Memory>();
  auto a = std::make_unique<Session>(o, jobs.get(), *ma);
  o.count = 3;
  auto b = std::make_unique<Session>(o, jobs.get(), *mb);
  CHECK(a->prepare() == Status::ok && b->prepare() == Status::ok &&
        jobs->owners() == 2);
  CHECK(jobs->close() == Status::invalid_state);
  CHECK(a->controls() && b->controls());
  Oracle oa(a->world.options), ob(o);
  for (std::size_t i = 0; i < 24; ++i) {
    CHECK(a->step(i) == Status::ok && oa.step(i, a->world));
    CHECK(b->step(i) == Status::ok && ob.step(i, b->world));
  }
  CHECK(a->close() == Status::ok);
  a.reset();
  CHECK(jobs->owners() == 1);
  CHECK(b->close() == Status::ok);
  b.reset();
  CHECK(jobs->owners() == 0 && jobs->accepted.load() == jobs->completed.load());
  CHECK(jobs->close() == Status::ok);
  CHECK(jobs->start(3, true, true) == Status::resource_exhausted &&
        jobs->close() == Status::ok);
  return true;
}
struct QueueProbe {
  std::array<std::atomic<unsigned>, 4096> hits{};
  std::atomic<bool> bad{false};
  static void execute(void *a, void *b, std::uint64_t token,
                      std::uint32_t worker) noexcept {
    auto &p = *static_cast<QueueProbe *>(a);
    if (a != b || token >= 4096 || worker >= 3) {
      p.bad.store(true);
      return;
    }
    p.hits[static_cast<std::size_t>(token)].fetch_add(1);
  }
  rt::HostExecutorJob job(std::uint64_t token) noexcept {
    return {execute, this, this, token, nullptr, 0};
  }
};
bool queue_checks() {
  auto jobs = std::make_unique<Jobs>();
  QueueProbe p;
  CHECK(jobs->start(1, false) == Status::ok);
  for (std::size_t i = 0; i < 1024; ++i)
    CHECK(Jobs::submit(jobs.get(), p.job(i)) == Status::ok);
  CHECK(Jobs::submit(jobs.get(), p.job(1024)) == Status::queue_full);
  CHECK(jobs->close() == Status::invalid_state);
  CHECK(jobs->quiesce() == Status::ok &&
        jobs->accepted.load() == jobs->completed.load());
  for (std::size_t i = 0; i < 1024; ++i)
    CHECK(p.hits[i].load() == 1);
  CHECK(jobs->close() == Status::ok && jobs->start(3) == Status::ok);
  for (auto &hit : p.hits)
    hit.store(0);
  std::atomic<bool> failed{false};
  auto produce = [&](std::size_t begin) {
    std::size_t done = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (done < 2048 && std::chrono::steady_clock::now() < end) {
      auto code = Jobs::submit(jobs.get(), p.job(begin + done));
      if (code == Status::ok)
        ++done;
      else if (code != Status::queue_full) {
        failed.store(true);
        break;
      }
    }
    if (done != 2048)
      failed.store(true);
  };
  std::thread a(produce, 0), b(produce, 2048);
  a.join();
  b.join();
  CHECK(jobs->quiesce() == Status::ok && jobs->close() == Status::ok &&
        !failed.load() && !p.bad.load());
  for (auto &hit : p.hits)
    CHECK(hit.load() == 1);
  CHECK(jobs->accepted.load() == jobs->completed.load());
  return true;
}
