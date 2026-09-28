#include "plant_runtime.hpp"

int main(int argc, char** argv) {
    cil::Options o;
    if (!cil::options(argc, argv, o) || o.mode != "normal") {
        std::cerr << "usage: cil_plant NAME GENERATION TIMEOUT_MS\n"; return 2;
    }
    cil::Mapping mapping; auto status = mapping.open(o.key, true);
    if (status == cil::Code::ok) status = cil::initialize(*mapping.region(), o.generation);
    if (status != cil::Code::ok) {
        std::cerr << "plant setup=" << cil::name(status) << '\n'; return cil::closed(mapping, status);
    }
    auto& region = *mapping.region();
    cil::Plant plant; plant.endpoint = {&region, o.generation, {}, {}, true};
    plant.ttl = o.timeout_ms * 1'000'000;
    rt::Runtime runtime;
    auto configured = cil::configure(runtime, plant);
    if (configured != rt::Status::ok) {
        plant.endpoint.detach(); std::cerr << runtime.last_error() << '\n'; return cil::closed(mapping, cil::Code::invalid);
    }
    rt::sdk::CheckedStopGuard stop(runtime);
    bool runtime_ok = runtime.start() == rt::Status::ok;
    if (runtime_ok) std::cout << "ready plant\n" << std::flush;
    // Startup has its own finite budget; command freshness starts only once a
    // controller owns the session, not while the OS is starting that process.
    const auto startup = cil::now_ns() + 5'000'000'000;
    while (runtime_ok && !cil::load(region.controller) && cil::now_ns() < startup) cil::pause();
    if (!cil::load(region.controller)) plant.status = cil::Code::timeout;
    plant.now = cil::now_ns();
    if (runtime_ok && plant.status == cil::Code::ok)
        runtime_ok = runtime.step({0, std::chrono::milliseconds(1)}) == rt::Status::ok;
    for (std::uint64_t frame = 1; runtime_ok && plant.status == cil::Code::ok && frame <= cil::steps; ++frame) {
        const auto limit = cil::deadline(o);
        while (cil::load(region.commands.state) == 0 && cil::now_ns() < limit) cil::pause();
        if (cil::load(region.commands.state) == 0) { plant.status = cil::Code::timeout; break; }
        plant.now = cil::now_ns();
        runtime_ok = runtime.step({frame, std::chrono::milliseconds(1)}) == rt::Status::ok;
    }
    const auto stopped = stop.close();
    if (stopped != rt::Status::ok) { std::cerr << runtime.last_error() << '\n'; std::terminate(); }
    status = runtime_ok ? plant.status : cil::Code::invalid;
    plant.endpoint.detach(); // No callback or endpoint can access the map after this point.
    cil::store(region.stopping, 1);
    const auto limit = cil::deadline(o);
    while (cil::load(region.controller) && !cil::load(region.acknowledged) && cil::now_ns() < limit) cil::pause();
    const bool acknowledged = cil::load(region.acknowledged) == 1;
    if (status == cil::Code::ok && !acknowledged) status = cil::Code::no_ack;
    std::cout << "plant status=" << cil::name(status) << " generation=" << o.generation
              << " applied=" << plant.applied << " position=" << plant.position << " ack=" << acknowledged << " history=";
    for (std::size_t i = 0; i < plant.applied; ++i) std::cout << (i ? "," : "") << plant.history[i];
    std::cout << '\n';
    return cil::closed(mapping, status);
}
