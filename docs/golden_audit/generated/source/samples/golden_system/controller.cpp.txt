#include "common.hpp"
#include <cstdlib>
namespace cil = golden::cil;
int main(int argc, char **argv) {
  cil::Options o;
  if (!cil::options(argc, argv, o)) {
    std::cerr << "usage: golden_controller NAME GENERATION TIMEOUT_MS "
                 "[FIXTURE_MODE]\n";
    return 2;
  }
  constexpr std::array modes{"normal", "schema",  "generation",  "sequence",
                             "future", "expired", "correlation", "replay",
                             "crash",  "partial", "no_ack",      "hold",
                             "linger"};
  if (std::find(modes.begin(), modes.end(), o.mode) == modes.end())
    return 2;
  cil::Mapping mapping;
  auto status = cil::Code::not_found;
  const auto startup = cil::deadline(o);
  do {
    status = mapping.open(o.key, false);
    if (status == cil::Code::ok)
      status = cil::attach(*mapping.region(), o.generation);
    if (status == cil::Code::ok)
      break;
    if (mapping.close() != cil::Code::ok)
      return 2;
    if (status != cil::Code::not_found && status != cil::Code::not_ready)
      break;
    cil::pause();
  } while (cil::now_ns() < startup);
  if (status != cil::Code::ok) {
    std::cerr << "setup=" << cil::name(status) << '\n';
    return cil::closed(mapping, status);
  }
  auto &region = *mapping.region();
  cil::Endpoint endpoint{&region, o.generation, {}, {}, false};
  std::cout << "ready controller\n" << std::flush;
  cil::Wire first{};
  std::size_t sent = 0;
  const auto lifetime = cil::now_ns() + 30'000'000'000ULL;
  while (!cil::load(region.stopping) && cil::now_ns() < lifetime) {
    if (o.mode == "hold" || o.mode == "linger") {
      cil::pause();
      continue;
    }
    cil::Record plant;
    status = endpoint.receive(0, plant);
    if (status == cil::Code::empty) {
      cil::pause();
      continue;
    }
    if (status != cil::Code::ok)
      break;
    if (o.mode == "crash")
      std::_Exit(77);
    cil::Record command = plant;
    command.kind = cil::Kind::command;
    command.sequence = endpoint.sent.next;
    command.correlation = plant.sequence;
    for (std::size_t a = 0; a < 3; ++a)
      for (std::size_t i = 0; i < plant.count; ++i)
        command.values[a][i] =
            golden::effort(plant.values[a][i], plant.target, plant.gain);
    cil::Wire wire;
    if (cil::encode(command, wire) != cil::Code::ok) {
      status = cil::Code::invalid;
      break;
    }
    if (!sent)
      first = wire;
    if (o.mode == "partial") {
      region.commands.bytes[0] = std::byte{0xee};
      std::_Exit(78);
    }
    if (o.mode == "schema")
      wire[4] = std::byte{9};
    else if (o.mode == "generation")
      golden::put(wire, 32, o.generation + 1, 8);
    else if (o.mode == "sequence")
      golden::put(wire, 40, command.sequence + 1, 8);
    else if (o.mode == "future") {
      golden::put(wire, 48, plant.issued_tick + 1, 8);
      golden::put(wire, 56, plant.expiry_tick + 1, 8);
    } else if (o.mode == "expired" && plant.issued_tick) {
      golden::put(wire, 48, plant.issued_tick - 1, 8);
      golden::put(wire, 56, plant.expiry_tick - 1, 8);
    } else if (o.mode == "correlation")
      golden::put(wire, 64, plant.sequence + 1, 8);
    if (o.mode != "schema") {
      golden::put(wire, 72, 0, 8);
      golden::put(wire, 72, golden::digest(wire), 8);
    }
    if (o.mode == "replay" && sent == 1)
      wire = first;
    status = cil::push(region.commands, wire);
    if (status != cil::Code::ok)
      break;
    endpoint.sent.advance();
    ++sent;
  }
  if (o.mode == "linger") {
    std::cout << "stopped old controller\n" << std::flush;
    const auto until = cil::now_ns() + 2'000'000'000ULL;
    while (cil::now_ns() < until) {
      (void)cil::push(region.commands, first);
      cil::pause();
    }
  }
  endpoint.detach();
  if (o.mode != "no_ack" && o.mode != "linger")
    cil::store(region.acknowledged, 1);
  if (status == cil::Code::empty || status == cil::Code::closed)
    status = cil::Code::ok;
  std::cout << "controller status=" << cil::name(status) << " sent=" << sent
            << '\n';
  return cil::closed(mapping, status);
}
