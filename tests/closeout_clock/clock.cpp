#include <simcore/highres_clock.hpp>
#include <array>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>

namespace {
thread_local bool track_allocations = false;
std::atomic<unsigned> allocations{0};
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL %s\n", message); std::abort(); }
}
std::uint64_t steady() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::uint64_t zero() { return 0; }
std::atomic<unsigned> calls{0};
std::atomic<bool> release{false};
std::uint64_t calibrating() {
    if (calls.fetch_add(1) == 0)
        while (!release.load()) std::this_thread::yield();
    return steady();
}
std::atomic<bool> hold_sample{false}, entered{false};
std::uint64_t stale() {
    if (hold_sample.load()) {
        entered = true;
        while (!release.load()) std::this_thread::yield();
        return 0;
    }
    return steady();
}
void publication() {
    HighResClock::init(steady);
    check(HighResClock::using_tsc(), "initial calibration");
    calls = 0; release = false;
    std::thread writer([] { HighResClock::init(calibrating); });
    while (calls.load() == 0) std::this_thread::yield();
    // Must return while the writer is held, and must not call its new reader.
    const auto before = HighResClock::now();
    const auto during = calls.load();
    release = true; writer.join();
    check(during == 1, "unpublished reader observed");
    check(HighResClock::now() > before, "reinitialization regressed time");
    HighResClock::init(zero);
    check(!HighResClock::using_tsc(), "failed calibration retained old enable");
    HighResClock::init(steady);
    check(HighResClock::using_tsc(), "valid recalibration did not recover");
    HighResClock::set_tsc_reader(nullptr);
    check(!HighResClock::using_tsc(), "null reader remained enabled");
    check(HighResClock::now() > before, "null fallback regressed");
}
void stale_failure() {
    hold_sample = false; release = false; entered = false;
    HighResClock::init(stale);
    hold_sample = true;
    std::thread delayed([] { (void)HighResClock::now(); });
    while (!entered.load()) std::this_thread::yield();
    HighResClock::init(steady);
    check(HighResClock::using_tsc(), "replacement calibration failed");
    release = true; delayed.join();
    check(HighResClock::using_tsc(), "stale sample disabled replacement");
}
void concurrent() {
    HighResClock::init(steady);
    std::barrier start(4);
    std::array<bool, 4> okay{true, true, true, true};
    const auto run = [&](std::size_t index) {
        start.arrive_and_wait();
        auto previous = HighResClock::now();
        for (unsigned cycle = 0; cycle < 4; ++cycle) {
            if (index < 2) HighResClock::init(steady);
            else if (index == 2) HighResClock::set_tsc_reader(steady);
            for (unsigned i = 0; i < 4000; ++i) {
                const auto next = HighResClock::now();
                okay[index] = okay[index] && next > previous;
                previous = next;
            }
        }
    };
    std::array<std::thread, 4> workers;
    for (std::size_t i = 0; i < workers.size(); ++i) workers[i] = std::thread(run, i);
    for (auto& worker : workers) worker.join();
    for (bool result : okay) check(result, "concurrent monotonicity");
}
void allocation() {
    HighResClock::init(steady);
    allocations = 0;
    track_allocations = true;
    auto previous = HighResClock::now();
    for (unsigned i = 0; i < 4096; ++i) {
        const auto next = HighResClock::now();
        check(next > previous, "allocation probe monotonicity"); previous = next;
    }
    HighResClock::set_tsc_reader(nullptr);
    HighResClock::init(zero);
    (void)HighResClock::now();
    track_allocations = false;
    check(allocations.load() == 0, "clock allocated");
    // Positive control proves this executable's allocation counter is active.
    track_allocations = true;
    void* control = ::operator new(17);
    track_allocations = false;
    ::operator delete(control);
    check(allocations.load() == 1, "allocation counter control");
}
}
#if defined(_MSC_VER)
#define CLOCK_NOINLINE __declspec(noinline)
#else
#define CLOCK_NOINLINE __attribute__((noinline))
#endif
CLOCK_NOINLINE void* operator new(std::size_t size) {
    if (track_allocations) allocations.fetch_add(1);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
CLOCK_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
CLOCK_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
CLOCK_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
CLOCK_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
int main() {
    publication(); stale_failure(); concurrent(); allocation();
    HighResClock::init();
    std::puts("PASS calibration publication, stale failure, concurrent read/init/replacement and allocation controls");
}
