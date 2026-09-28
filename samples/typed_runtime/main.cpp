#include <rt/sdk.hpp>
#include <array>
#include <chrono>
#include <iostream>

struct State {
    int produced = 0;
    int consumed = 0;
    rt::CallbackResult produce(const rt::CallbackContext&) noexcept {
        ++produced;
        return rt::CallbackResult::ok;
    }
    rt::CallbackResult consume(const rt::CallbackContext&) noexcept {
        if (produced != consumed + 1) return rt::CallbackResult::error;
        consumed = produced;
        return rt::CallbackResult::ok;
    }
};

int main() {
    State state; // Borrowed state outlives Runtime and the stop guard.
    rt::Runtime runtime;
    auto check = [&](rt::Status status, std::string_view operation) {
        if (status == rt::Status::ok) return true;
        std::array<char, 512> text{};
        const auto rendered = rt::sdk::render_error(text, operation, status, runtime.last_error());
        std::cerr << text.data() << (rendered.truncated ? " [truncated]\n" : "\n");
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
    rt::sdk::ConfigBuilder configuration(config);
    if (!check(configuration.apply(runtime), "configure")) return 1;
    rt::sdk::NativeStorageEstimate estimate;
    if (!check(rt::sdk::estimate_native_storage(configuration.value(), 2, estimate), "estimate")) return 1;
    rt::sdk::GraphBuilder graph(runtime);
    rt::PhaseHandle producer, consumer;
    rt::ResourceHandle resource;
    if (!check(graph.phase<&State::produce>("produce", state, producer), "register produce") ||
        !check(graph.phase<&State::consume>("consume", state, consumer), "register consume") ||
        !check(graph.resource("state", resource), "register resource") ||
        !check(graph.depends_on(consumer, producer), "dependency") ||
        !check(graph.access(producer, resource, rt::ResourceAccess::write), "produce access") ||
        !check(graph.access(consumer, resource, rt::ResourceAccess::write), "consume access") ||
        !check(graph.finalize(), "finalize")) return 1;
    rt::sdk::CheckedStopGuard stop(runtime); // Construct only after successful finalize.
    rt::MemoryPlan plan;
    if (!runtime.memory_plan(plan) || plan.queue_slots != estimate.queue_slots) return 1;
    bool ok = check(runtime.start(), "start");
    for (std::uint64_t frame = 0; ok && frame < 3; ++frame)
        ok = check(runtime.step({frame, std::chrono::milliseconds(1)}), "step");
    const bool stopped = check(stop.close(), "stop");
    if (!ok || !stopped || state.produced != 3 || state.consumed != 3) return 1;
    std::cout << "typed_runtime: frames=3 produced=3 consumed=3 stopped=ok\n";
}
