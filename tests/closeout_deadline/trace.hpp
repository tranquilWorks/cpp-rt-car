// Offline Linux diagnostic only. Never compiled into the shipped Runtime.
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <time.h>
namespace deadline_trace {
struct Event {
    const char* kind{};
    std::uint64_t wall{}, cpu{}, batch{}, deadline{};
    unsigned thread{};
    int state{}, status{};
};
inline std::array<Event, 4096> events{};
inline std::atomic<unsigned> cursor{0}, threads{0};
inline bool delay_submit = false; // Set before Runtime threads are created.
inline std::atomic<bool> delayed{false};
inline void record(const char* kind, std::uint64_t batch, std::uint64_t deadline,
                   int state, int status) noexcept {
    // Relaxed reservation adds no cross-lane happens-before relationship.
    // Each writer owns one cell. The caller dumps only after all owners join.
    static thread_local const unsigned thread = threads.fetch_add(1, std::memory_order_relaxed);
    const auto index = cursor.fetch_add(1, std::memory_order_relaxed);
    if (index >= events.size()) return;
    timespec cpu{};
    const int clock_status = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
    events[index] = {kind, static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()),
        clock_status == 0 ? static_cast<std::uint64_t>(cpu.tv_sec) * 1'000'000'000ULL +
                           static_cast<std::uint64_t>(cpu.tv_nsec) : 0,
        batch, deadline, thread, state, status};
}
inline void before_submit(std::uint64_t batch, std::uint64_t deadline, std::uint64_t budget) noexcept {
    record("selected", batch, deadline, 3, 0);
    if (delay_submit && budget == 500'000 && !delayed.exchange(true, std::memory_order_relaxed)) {
        record("delay_begin", batch, deadline, 3, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        record("delay_end", batch, deadline, 3, 0);
    }
    record("submit_enter", batch, deadline, 3, 0);
}
inline bool dump() noexcept {
    const auto n = cursor.load(std::memory_order_relaxed);
    std::printf("TRACE count=%u capacity=%zu overflow=%d\n", n, events.size(), n > events.size());
    for (unsigned i = 0; i < n && i < events.size(); ++i) {
        const auto& e = events[i];
        std::printf("EVENT %u %s wall=%llu cpu=%llu thread=%u batch=%llu deadline=%llu state=%d status=%d\n",
            i, e.kind, static_cast<unsigned long long>(e.wall), static_cast<unsigned long long>(e.cpu),
            e.thread, static_cast<unsigned long long>(e.batch),
            static_cast<unsigned long long>(e.deadline), e.state, e.status);
    }
    return n != 0 && n <= events.size();
}
}
