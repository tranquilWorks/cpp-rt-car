#include "common.hpp"
#include <cstdlib>

int main(int argc, char** argv) {
    cil::Options o;
    if (!cil::options(argc, argv, o)) {
        std::cerr << "usage: cil_controller NAME GENERATION TIMEOUT_MS [FIXTURE_MODE]\n"; return 2;
    }
    constexpr std::array modes{"normal", "schema", "generation", "sequence", "future", "expired",
                               "correlation", "replay", "crash", "partial", "no_ack", "hold", "linger"};
    if (std::find(modes.begin(), modes.end(), o.mode) == modes.end()) { std::cerr << "invalid mode\n"; return 2; }
    cil::Mapping mapping;
    auto status = cil::Code::not_found; const auto startup = cil::deadline(o);
    do {
        status = mapping.open(o.key, false);
        if (status == cil::Code::ok) status = cil::attach(*mapping.region(), o.generation);
        if (status == cil::Code::ok) break;
        if (mapping.close() != cil::Code::ok) return 2;
        if (status != cil::Code::not_found && status != cil::Code::not_ready) break;
        cil::pause();
    } while (cil::now_ns() < startup);
    if (status != cil::Code::ok) {
        std::cerr << "controller setup=" << cil::name(status) << '\n'; return cil::closed(mapping, status);
    }
    auto& region = *mapping.region();
    cil::Endpoint endpoint{&region, o.generation, {}, {}, false};
    std::cout << "ready controller\n" << std::flush;
    std::array<std::int64_t, 8> efforts{}; std::size_t sent = 0;
    cil::Wire first{};
    const auto lifetime = cil::now_ns() + 30'000'000'000;
    while (!cil::load(region.stopping) && cil::now_ns() < lifetime) {
        if (o.mode == "hold" || o.mode == "linger") { cil::pause(); continue; }
        cil::Record plant;
        status = endpoint.receive(0, plant);
        if (status == cil::Code::empty) { cil::pause(); continue; }
        if (status != cil::Code::ok) break;
        if (plant.frame == 8) {
            if (plant.value != 255 || sent != 8) status = cil::Code::invalid;
            break;
        }
        if (sent >= efforts.size() || plant.frame != sent) { status = cil::Code::sequence; break; }
        if (o.mode == "crash") std::_Exit(77);
        cil::Record command{cil::Kind::command, o.generation, endpoint.sent.next,
            plant.issued_ns, plant.expiry_ns, plant.sequence, plant.frame, (256 - plant.value) / 2};
        cil::Wire w; status = cil::encode(command, w); if (status != cil::Code::ok) break;
        if (!sent) first = w;
        if (o.mode == "partial") {
            // Fixture death before release-publication: state remains empty.
            region.commands.bytes[0] = std::byte{0xee}; std::_Exit(78);
        }
        if (o.mode == "schema") w[4] = std::byte{9};
        else if (o.mode == "generation") cil::put(w, 16, o.generation + 1, 8);
        else if (o.mode == "sequence") cil::put(w, 24, command.sequence + 1, 8);
        else if (o.mode == "future") {
            cil::put(w, 32, UINT64_MAX - 1000, 8); cil::put(w, 40, UINT64_MAX, 8);
        } else if (o.mode == "expired") {
            cil::put(w, 32, 1, 8); cil::put(w, 40, 2, 8);
        } else if (o.mode == "correlation") cil::put(w, 48, plant.sequence + 1, 8);
        else if (o.mode == "replay" && sent == 1) w = first;
        status = cil::push(region.commands, w);
        if (status != cil::Code::ok) break;
        endpoint.sent.advance(); efforts[sent++] = command.value;
        if (o.mode != "normal" && o.mode != "no_ack" && !(o.mode == "replay" && sent == 1)) break;
    }
    // All waits and process hooks are on this executable's control thread.
    const auto stopping = cil::deadline(o);
    while (!cil::load(region.stopping) && cil::now_ns() < stopping) cil::pause();
    if (!cil::load(region.stopping) && status == cil::Code::ok) status = cil::Code::timeout;
    if (o.mode == "linger") {
        // Keep the old OS view alive while the supervisor runs a fresh session.
        std::cout << "stopped old controller\n" << std::flush;
        const auto until = cil::now_ns() + 2'000'000'000;
        // Simulate an old writer resuming after its earlier stop check. This
        // view must never alias a fresh mapping used by the next generation.
        cil::Record old{cil::Kind::command, o.generation, 1, 1, 2, 1, 0, 123};
        cil::Wire old_wire; (void)cil::encode(old, old_wire);
        while (cil::now_ns() < until) { (void)cil::push(region.commands, old_wire); cil::pause(); }
    }
    // Drain a final already-published plant before acknowledging stop.
    if (cil::load(region.plants.state) == 1) {
        cil::Record final;
        const auto drained = endpoint.receive(0, final);
        if ((o.mode == "normal" || o.mode == "no_ack") &&
            (drained != cil::Code::ok || final.frame != 8 || final.value != 255)) status = cil::Code::invalid;
    }
    endpoint.detach();
    if (o.mode != "no_ack" && o.mode != "linger") cil::store(region.acknowledged, 1);
    if (status == cil::Code::empty || status == cil::Code::closed) status = cil::Code::ok;
    std::cout << "controller status=" << cil::name(status) << " sent=" << sent << " efforts=";
    for (std::size_t i = 0; i < sent; ++i) std::cout << (i ? "," : "") << efforts[i];
    std::cout << '\n';
    return cil::closed(mapping, status);
}
