#pragma once
#include "common.hpp"
#include "world.hpp"
namespace golden {
// Control-thread transport. Runtime callbacks only see the copied World
// snapshot.
class Peer {
  cil::Mapping mapping_;
  cil::Endpoint endpoint_{};
  cil::Options options_;

public:
  cil::Code result = cil::Code::ok, cleanup = cil::Code::ok;
  std::uint64_t requests = 0, responses = 0;
  explicit Peer(cil::Options options) : options_(options) {}
  ~Peer() { (void)close(); }
  bool start() {
    result = mapping_.open(options_.key, true);
    if (result != cil::Code::ok)
      return false;
    result = cil::initialize(*mapping_.region(), options_.generation);
    if (result != cil::Code::ok)
      return false;
    endpoint_ = {mapping_.region(), options_.generation, {}, {}, true};
    std::cout << "ready plant\n" << std::flush;
    const auto end = cil::deadline(options_);
    while (!cil::load(mapping_.region()->controller) && cil::now_ns() < end)
      cil::pause();
    if (!cil::load(mapping_.region()->controller))
      result = cil::Code::timeout;
    return true; // A missing peer is an explicit safe-input campaign.
  }
  void exchange(std::size_t tick, World &world) noexcept {
    world.external_command = {};
    world.external_ready = false;
    if (result != cil::Code::ok)
      return;
    cil::Record request;
    request.count = static_cast<std::uint32_t>(world.options.count);
    request.generation = options_.generation;
    request.sequence = endpoint_.sent.next;
    request.issued_tick = tick;
    request.expiry_tick = tick + 1;
    request.values = world.sensor.velocity;
    // This declared fault substitutes the initial sensor before the off-lane
    // request. The callback still detects the backdated application frame.
    if (world.options.campaign == Campaign::stale_input && tick == 6)
      request.values = {};
    request.target = tick >= 1 ? 16 : 8;
    request.gain = tick >= 3 ? 2 : 1;
    if (tick >= 6 && world.options.campaign == Campaign::control_replaced)
      request.gain = 3;
    result = endpoint_.send(request);
    if (result != cil::Code::ok)
      return;
    ++requests;
    cil::Record response;
    const auto end = cil::deadline(options_);
    do {
      result = endpoint_.receive(tick, response, &request);
      if (result != cil::Code::empty)
        break;
      cil::pause();
    } while (cil::now_ns() < end);
    if (result == cil::Code::empty)
      result = cil::Code::timeout;
    if (result == cil::Code::ok) {
      world.external_command = response.values;
      world.external_ready = true;
      ++responses;
    }
  }
  bool close() noexcept {
    if (auto *region = mapping_.region()) {
      cil::store(region->stopping, 1);
      const auto end = cil::deadline(options_);
      while (cil::load(region->controller) &&
             !cil::load(region->acknowledged) && cil::now_ns() < end)
        cil::pause();
      if (cil::load(region->controller) && !cil::load(region->acknowledged))
        cleanup = cil::Code::no_ack;
    }
    endpoint_.detach();
    auto code = mapping_.close();
    if (code != cil::Code::ok) {
      cleanup = code;
      if (mapping_.close() != cil::Code::ok)
        std::terminate();
    }
    return cleanup == cil::Code::ok;
  }
};
} // namespace golden
