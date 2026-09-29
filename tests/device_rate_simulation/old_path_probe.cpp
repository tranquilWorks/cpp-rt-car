// Controlled positive probe, also linked locally to the preserved old Runtime
// as a negative control. The append-only binding tail leaves its old prefix intact.
#include "fixture.hpp"
#include <future>
#include <iostream>
int main() {
    using namespace simulation_test;
    using namespace std::chrono_literals;
    Fixture f; f.backend.allowed = false;
    if (f.configure() != rt::Status::ok) return 1;
    auto b = f.binding(); b.simulation = rt::DeviceRateSimulationPolicy{watchdog};
    if (f.runtime.bind_device_phase_to_rate_domain(b) != rt::Status::ok ||
        f.runtime.finalize() != rt::Status::ok || f.runtime.start() != rt::Status::ok) return 2;
    auto run = std::async(std::launch::async, [&] { return f.step(); });
    const auto bound = std::chrono::steady_clock::now() + 2s;
    while (f.backend.polled.load() < 10 && std::chrono::steady_clock::now() < bound) std::this_thread::yield();
    std::this_thread::sleep_for(30ms);
    f.backend.allowed = true;
    const auto result = run.get();
    std::cout << "status=" << int(result) << " publications=" << f.publications
              << " command_timeout=" << f.backend.timeout.load() << '\n';
    const bool good = result == rt::Status::ok && f.publications == 1 && f.backend.timeout.load() == budget;
    return f.runtime.stop() == rt::Status::ok && good ? 0 : 3;
}
