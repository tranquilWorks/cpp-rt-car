#include "../samples/cuda_physics/simulated_driver.hpp"
#include "cuda_physics/allocation_guard.hpp"
#include <future>
#include <iostream>
#include <thread>

namespace p = rtfw::cuda_physics;
#define CHECK(x) do { if (!(x)) { std::cerr << __func__ << ':' << __LINE__ << " failed: " #x << '\n'; return false; } } while (false)

bool run_case(p::Options options, bool measure = false) {
    auto d = std::make_unique<p::SimulatedDriver>();
    auto s = std::make_unique<p::Scenario>(options, d->session());
    CHECK(s->prepare() == rt::Status::ok);
    CHECK(s->start() == rt::Status::ok);
    if (measure) rtfw_physics_allocation::begin();
    const auto ran = s->run();
    const auto allocations = measure ? rtfw_physics_allocation::end() : 0;
    CHECK(ran == rt::Status::ok);
    CHECK(allocations == 0);
    CHECK(s->completed() == options.steps && s->publications() == options.steps);
    CHECK(s->preparations() == options.steps && s->submissions() == options.steps);
    CHECK(d->uploads == options.steps && d->launches == options.steps && d->downloads == options.steps);
    CHECK(d->upload_bytes == static_cast<std::uint64_t>(options.count)*sizeof(p::Particle)*options.steps);
    CHECK(d->download_bytes == d->upload_bytes.load() && d->protocol_ok.load());
    CHECK(s->step() == rt::Status::invalid_argument);
    if (options.seed == 0 && options.steps == 7) {
        const auto& actual = s->particle(0);
        CHECK(actual.position[0] == 844 && actual.position[1] == 1065 && actual.position[2] == 350);
        CHECK(actual.velocity[0] == 22 && actual.velocity[1] == 50 && actual.velocity[2] == -20);
        CHECK(actual.acceleration[0] == -3 && actual.acceleration[1] == -1 && actual.acceleration[2] == -2);
    }
    CHECK(s->stop() == rt::Status::ok);
    CHECK(s->stop() == rt::Status::ok && d->clean());
    CHECK(d->allocations == 1 && d->frees == 1 && d->registrations == 1 && d->unregistrations == 1);
    return true;
}

bool known_answers() {
    std::uint32_t seed{};
    const auto first = p::initial_particle(seed);
    CHECK(first.position[0] == 627 && first.position[1] == 694 && first.position[2] == 448);
    CHECK(first.velocity[0] == 43 && first.velocity[1] == 57 && first.velocity[2] == -6);
    CHECK(first.acceleration[0] == -3 && first.acceleration[1] == -1 && first.acceleration[2] == -2);
    p::Particle answer{{844,1065,350},{22,50,-20},{-3,-1,-2}};
    CHECK(p::matches_oracle(answer, first, 7));
    ++answer.position[2];
    CHECK(!p::matches_oracle(answer, first, 7));
    p::Particle maximum{{1024,1024,1024},{64,64,64},{4,4,4}};
    p::Particle maximum_answer{{2165760,2165760,2165760},{4160,4160,4160},{4,4,4}};
    CHECK(p::matches_oracle(maximum_answer, maximum, 1024));
    CHECK(!p::matches_oracle(maximum_answer, maximum, 1025));
    return run_case({1,7,0,1});
}

bool invalid_options() {
    for (const auto o : {p::Options{0,1,0,1}, {4097,1,0,1}, {1,0,0,1},
                         {1,1025,0,1}, {1,1,0,0}, {1,1,0,3}}) {
        auto d = std::make_unique<p::SimulatedDriver>();
        auto s = std::make_unique<p::Scenario>(o, d->session());
        CHECK(s->prepare() == rt::Status::invalid_argument);
        CHECK(d->allocations == 0 && d->clean());
    }
    // A supplied session is consumed, not silently replaced by the fake driver.
    auto d = std::make_unique<p::SimulatedDriver>();
    auto session = d->session(); session.function = 0;
    auto s = std::make_unique<p::Scenario>(p::Options{}, session);
    CHECK(s->prepare() == rt::Status::invalid_argument);
    return true;
}

bool failures() {
    using Flag = std::atomic<bool> p::SimulatedDriver::*;
    for (Flag flag : {&p::SimulatedDriver::fail_upload, &p::SimulatedDriver::fail_launch,
                      &p::SimulatedDriver::fail_download, &p::SimulatedDriver::corrupt_output}) {
        auto d = std::make_unique<p::SimulatedDriver>();
        auto s = std::make_unique<p::Scenario>(p::Options{17,1,1,2}, d->session());
        CHECK(s->prepare() == rt::Status::ok && s->start() == rt::Status::ok);
        (d.get()->*flag).store(true);
        CHECK(s->step() != rt::Status::ok);
        CHECK(s->completed() == 0 && s->publications() == 0);
        CHECK(s->stop() == rt::Status::ok && d->clean());
        CHECK(d->stream_syncs == (flag == &p::SimulatedDriver::corrupt_output ? 0u : 1u));
    }
    auto d = std::make_unique<p::SimulatedDriver>();
    auto s = std::make_unique<p::Scenario>(p::Options{17,1,1,2}, d->session());
    CHECK(s->prepare() == rt::Status::ok && s->start() == rt::Status::ok && s->run() == rt::Status::ok);
    d->fail_free = true;
    CHECK(s->stop() != rt::Status::ok);
    CHECK(d->allocated && d->frees == 0);
    CHECK(s->stop() == rt::Status::ok && d->clean() && d->frees == 1);
    return true;
}

bool delayed_completion() {
    auto d = std::make_unique<p::SimulatedDriver>();
    auto s = std::make_unique<p::Scenario>(p::Options{17,1,1,2}, d->session());
    CHECK(s->prepare() == rt::Status::ok && s->start() == rt::Status::ok);
    d->hold_completion = true;
    rt::Status result = rt::Status::internal_error;
    std::thread worker([&] { result = s->step(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (d->not_ready_polls == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const bool observed = d->not_ready_polls > 0;
    const bool premature = s->publications() != 0;
    d->hold_completion = false;
    worker.join(); // Always release and join before any failing assertion.
    CHECK(observed && !premature && result == rt::Status::ok);
    CHECK(s->publications() == 1 && s->completed() == 1);
    CHECK(s->stop() == rt::Status::ok && d->clean());
    return true;
}

int main() {
    if (!known_answers() || !invalid_options() || !failures() || !delayed_completion()) return 1;
    constexpr p::Options cases[]{{1,1,1,1},{17,7,0,2},{256,64,1,1},{1024,7,0xffffffffu,2},
        {4096,64,0,1},{4096,1024,0xffffffffu,2},{17,64,1,2}};
    for (auto o : cases) if (!run_case(o, true)) return 1;
    if (!run_case({17,7,0,1}) || !run_case({17,7,0,1})) return 1;
    auto first = std::async(std::launch::async, [] { return run_case({256,64,0,1}); });
    auto second = std::async(std::launch::async, [] { return run_case({256,64,0xffffffffu,2}); });
    if (!first.get() || !second.get()) return 1;
    std::cout << "CUDA physics: oracle, capacity, failure, lifetime, isolation and no-allocation PASS\n";
}
