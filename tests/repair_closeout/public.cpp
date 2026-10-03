#include "fixture.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <thread>
#include "allocation_guard.hpp"

int main() {
    Fixture<rt::SampledIoLoopbackBackend> f, isolated;
    // All construction/thread creation precedes allocation tracking.
    constexpr std::uint64_t iterations = 2048;
    std::barrier gate(4);
    std::atomic<std::uint64_t> observed{0}, successes{0};
    std::array<std::thread, 3> workers;
    for (std::size_t lane = 0; lane < workers.size(); ++lane) {
        workers[lane] = std::thread([&, lane] {
            for (std::uint64_t id = 1; id <= iterations; ++id) {
                gate.arrive_and_wait();
                if (lane < 2) {
                    const auto canceled = f.cancel(id);
                    require(canceled == H::ok || canceled == H::invalid_argument, "competing cancel result");
                    if (canceled == H::ok) successes.fetch_add(1);
                } else {
                    rt::HalV2BatchCompletion completion{};
                    if (f.poll(completion)) {
                        require(completion.batch_id == id && completion.signals[0].value == id,
                                "concurrent completion identity");
                        require(completion.status == static_cast<std::int32_t>(H::ok) ||
                                completion.status == static_cast<std::int32_t>(H::canceled), "concurrent status");
                        observed.fetch_add(1);
                    }
                }
                gate.arrive_and_wait();
            }
            gate.arrive_and_wait(); // remain alive until tracking has ended
        });
    }
    rtfw_physics_allocation::begin();
    for (std::uint64_t id = 1; id <= iterations; ++id) {
        const bool timeout = id % 2 == 0;
        if (timeout) require(f.backend.inject_next(rt::SampledIoLoopbackFault::completion_timeout) == rt::Status::ok, "timeout injection");
        const bool prepublished = id <= iterations / 2;
        rt::HalV2BatchCompletion completion{};
        if (prepublished) {
            require(f.submit(id) == H::ok, "bounded submit");
            require(f.submit(id + iterations) == H::queue_full, "capacity before retirement");
            require(f.cancel(id + iterations) == H::invalid_argument, "wrong ID restores publication");
            if (timeout) require(f.poll(completion) == 0, "timeout remains unpublished to poll");
        }
        successes.store(0);
        gate.arrive_and_wait();
        if (!prepublished) require(f.submit(id) == H::ok, "submit competing with cancel and poll");
        gate.arrive_and_wait();
        // A bounded competing scan may miss a temporarily claimed slot; drain
        // only after all competing callbacks have returned.
        if (timeout && successes.load() == 0) require(f.cancel(id) == H::ok, "terminal timeout cancellation");
        if (f.poll(completion)) {
            require(completion.batch_id == id && completion.signals[0].value == id, "terminal identity");
            require(completion.status == static_cast<std::int32_t>(H::canceled) ||
                    (!timeout && completion.status == static_cast<std::int32_t>(H::ok)), "terminal status");
            observed.fetch_add(1);
        }
        require(f.poll(completion) == 0 && f.cancel(id) == H::invalid_argument, "retired slot cannot reappear");
        require(observed.load() == id, "exactly one terminal per accepted batch");
        require(isolated.backend.stats().submissions == 0 && isolated.poll(completion) == 0, "instance isolation");
    }
    require(f.backend.stats().submissions == iterations && f.backend.stats().completions == iterations,
            "accepted and terminal conservation");
    require(f.backend.inject_next(rt::SampledIoLoopbackFault::completion_timeout) == rt::Status::ok, "held injection");
    require(f.submit(iterations + 1) == H::ok, "held submit");
    const auto canceled_before = f.backend.stats().cancellations;
    require(f.cancel(iterations + 1) == H::ok && f.cancel(iterations + 1) == H::ok, "idempotent cancellation");
    require(f.backend.stats().cancellations == canceled_before + 1, "one cancellation transition");
    f.completed(iterations + 1, H::canceled);
    f.stop(); require(f.submit(iterations + 2) == H::invalid_state, "stopped admission");
    f.shutdown();
    require(f.backend.reset_logical_actions() == rt::Status::ok, "quiescent action reset");
    require(f.registration.api.reset(f.registration.api.instance) == H::ok, "quiescent slot reset");
    require(f.submit(iterations + 2) == H::invalid_state, "reset does not initialize");
    f.initialize(); require(f.submit(iterations + 2) == H::ok, "restart admission");
    f.completed(iterations + 2, H::ok); f.stop(); f.shutdown(); isolated.stop(); isolated.shutdown();
    const auto allocations = rtfw_physics_allocation::end();
    gate.arrive_and_wait();
    for (auto& worker : workers) worker.join();
    require(allocations == 0, "no allocation in initialized callbacks and lifecycle");
    std::puts("PASS 2048 competing completion cycles, identity/conservation, timeout/reuse, lifecycle/isolation, zero allocation");
}
