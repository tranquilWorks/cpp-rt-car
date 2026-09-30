#include "host.hpp"
#include <iostream>
#include <memory>
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 2 || (std::string_view(argv[1]) != "native" && std::string_view(argv[1]) != "host")) {
        std::cerr << "usage: host_adapter native|host\n"; return 2;
    }
    const bool adapted = std::string_view(argv[1]) == "host";
    auto host = std::make_unique<host_example::Host>(); // Fixed arenas allocated on the control thread.
    auto status = adapted ? host->jobs.start() : rt::Status::ok;
    host_example::Session first(host->clock, adapted ? &host->jobs : nullptr, host->memory[0], 1);
    host_example::Session second(host->clock, adapted ? &host->jobs : nullptr, host->memory[1], 3);
    if (status == rt::Status::ok) status = first.prepare();
    if (status == rt::Status::ok) status = second.prepare();
    for (unsigned frame = 0; frame < 16 && status == rt::Status::ok; ++frame) {
        status = first.step();
        if (status == rt::Status::ok) status = second.step();
    }
    if (status == rt::Status::ok &&
        (!first.telemetry.metrics(*first.runtime, 16) || !second.telemetry.metrics(*second.runtime, 16) ||
         first.telemetry.runtime_id == second.telemetry.runtime_id ||
         first.telemetry.begins != 16 || first.telemetry.ends != 16 || first.telemetry.callbacks != 32 ||
         second.telemetry.begins != 16 || second.telemetry.ends != 16 || second.telemetry.callbacks != 32)) status = rt::Status::internal_error;
    const auto a = first.close(), b = second.close();
    if (a != rt::Status::ok || b != rt::Status::ok) {
        std::cerr << "checked stop failed; host ownership retained\n";
        // One explicit retry; unresolved cleanup fails closed in the destructor.
        if (first.close() != rt::Status::ok || second.close() != rt::Status::ok) std::terminate();
        status = rt::Status::internal_error;
    }
    if (host->jobs.close() != rt::Status::ok) std::terminate();
    if (host->jobs.accepted.load() != host->jobs.completed.load() || host->jobs.pending.load() ||
        (adapted && !host->jobs.accepted.load()) || (!adapted && host->jobs.accepted.load()) ||
        host->memory[0].acquisitions != host->memory[0].releases || host->memory[1].acquisitions != host->memory[1].releases)
        status = rt::Status::internal_error;
    if (status != rt::Status::ok) { std::cerr << "host failed: " << rt::status_message(status) << '\n'; return 1; }
    std::cout << "host_adapter: mode=" << argv[1] << " frames=16 instances=2 checksums="
              << first.world.checksum << ',' << second.world.checksum
              << " telemetry=32/32 lost=" << first.telemetry.lost + second.telemetry.lost
              << " jobs=balanced memory=6/6 stopped=ok\n";
}
