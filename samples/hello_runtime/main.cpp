#include <rt/runtime.hpp>
#include <chrono>
#include <cstdint>
#include <iostream>

struct State { int produced = 0; int consumed = 0; };
rt::CallbackResult produce(void* user, const rt::CallbackContext&) noexcept {
    ++static_cast<State*>(user)->produced;
    return rt::CallbackResult::ok;
}
rt::CallbackResult consume(void* user, const rt::CallbackContext&) noexcept {
    auto& state = *static_cast<State*>(user);
    if (state.produced != state.consumed + 1) return rt::CallbackResult::error;
    state.consumed = state.produced;
    return rt::CallbackResult::ok;
}

int main() {
    State state; // Borrowed callback state must outlive Runtime and checked stop.
    rt::Runtime runtime;
    const auto check = [&](rt::Status status, const char* operation) {
        if (status == rt::Status::ok) return true;
        std::cerr << operation << " failed (" << static_cast<int>(status)
                  << "): " << runtime.last_error() << '\n';
        return false;
    };
    rt::RuntimeConfig config;
    config.callback_capacity = 2;
    config.worker_count = 2;
    config.executor_queue_capacity = 2;
    config.scratch_bytes = 0;
    config.task_scratch_bytes = 0;
    config.task_scratch_slots = 2;
    config.trace_capacity = 32;
    rt::PhaseHandle producer, consumer;
    rt::ResourceHandle shared_state;
    if (!check(runtime.configure(config), "configure") ||
        !check(runtime.register_callback({"produce", produce, &state}, producer), "register produce") ||
        !check(runtime.register_callback({"consume", consume, &state}, consumer), "register consume") ||
        !check(runtime.register_resource("state", shared_state), "register resource") ||
        !check(runtime.add_dependency(producer, consumer), "add dependency") ||
        !check(runtime.declare_resource_access(producer, shared_state, rt::ResourceAccess::write), "producer access") ||
        !check(runtime.declare_resource_access(consumer, shared_state, rt::ResourceAccess::write), "consumer access") ||
        !check(runtime.finalize(), "finalize")) return 1;
    bool ok = check(runtime.start(), "start");
    for (std::uint64_t frame = 0; ok && frame < 3; ++frame) {
        rt::HostFrameContext context;
        context.frame_index = frame;
        context.delta = std::chrono::milliseconds(1);
        ok = check(runtime.step(context), "step");
    }
    const bool stopped = check(runtime.stop(), "stop"); // Also after start/step failure.
    if (!ok || !stopped) return 1;
    if (state.produced != 3 || state.consumed != 3) return 2;
    std::cout << "hello_runtime: frames=3 produced=3 consumed=3 stopped=ok\n";
    return 0;
}
