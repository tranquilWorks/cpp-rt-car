#include "cli.hpp"
#include "simulated_driver.hpp"

int main(int argc, char** argv) {
    namespace p = rtfw::cuda_physics;
    p::Options options;
    const auto parsed = p::parse(argc, argv, options);
    if (parsed) return parsed == 3 ? 0 : parsed;
    try {
        auto driver = std::make_unique<p::SimulatedDriver>();
        auto scenario = std::make_unique<p::Scenario>(options, driver->session());
        auto s = scenario->prepare();
        if (s == rt::Status::ok) s = scenario->start();
        if (s == rt::Status::ok) s = scenario->run();
        if (s != rt::Status::ok) std::cerr << scenario->error() << '\n';
        const auto stopped = scenario->stop();
        // A failed cleanup stays owned and is retried; the original failure
        // still makes this invocation fail, never a successful summary.
        if (stopped != rt::Status::ok && scenario->stop() != rt::Status::ok) std::terminate();
        if (s != rt::Status::ok || stopped != rt::Status::ok || !driver->clean()) {
            std::cerr << "physics execution/validation/cleanup failed status=" << static_cast<int>(s) << '\n';
            return 1;
        }
        p::summary(options, scenario->completed(), "simulated-driver-protocol");
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
