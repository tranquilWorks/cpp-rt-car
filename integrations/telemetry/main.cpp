#include "telemetry.hpp"
#include <atomic>
#include <iostream>
#include <stdexcept>

namespace {
class Clock final : public rt::RuntimeClock {
public:
    std::uint64_t now_ns() noexcept override { return now_.fetch_add(1, std::memory_order_relaxed); }
private:
    std::atomic<std::uint64_t> now_{1000};
};
void check(rt::Status status) {
    if (status != rt::Status::ok) throw std::runtime_error(rt::status_message(status));
}
rt::CallbackResult callback(void*, const rt::CallbackContext&) { return rt::CallbackResult::ok; }
}
int main() {
    try {
        // Deliberately synthetic sample clock and explicit synthetic Unix anchor.
        // Real applications supply a measured/certified mapping for their clock.
        for (const auto* session : {"example-owner-a", "example-owner-b"}) {
            Clock clock;
            rt::Runtime runtime(clock);
            rt::RuntimeConfig config;
            config.trace_capacity = 64;
            check(runtime.configure(config));
            check(runtime.register_callback({"telemetry.example", callback, nullptr}));
            check(runtime.finalize());
            check(runtime.start());
            rtfw_telemetry::Queue queue({session, "synthetic-ns", 1000, 1700000000000000000ULL, 0}, 2);
            check(queue.capture(runtime));
            for (std::uint64_t frame = 0; frame != 3; ++frame)
                check(runtime.step({frame, std::chrono::milliseconds(1), {}}));
            check(runtime.stop());
            check(queue.capture(runtime));
            queue.close();
            while (queue.drain_one([](const auto& s) { return rtfw_telemetry::write_json(s, std::cout); })) {}
            if (queue.statistics().pending != 0) throw std::runtime_error("output failed; snapshot retained");
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
