#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
namespace m27_schedule {
inline std::atomic<unsigned> ready{0}, parked{0}, exited{0};
inline std::atomic<bool> release{false};
inline void wait_count(const std::atomic<unsigned> &value, unsigned expected) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (value.load() != expected) {
    if (std::chrono::steady_clock::now() > deadline) {
      std::fprintf(stderr, "schedule gate failed %u/%u\n", value.load(),
                   expected);
      std::_Exit(43);
    }
    std::this_thread::yield();
  }
}
inline void worker_ready(std::size_t n) {
  if (n > 1)
    ++ready;
}
inline void worker_enter(std::uint64_t token, std::size_t n) {
  if (n > 1 && token == 1) {
    ++parked;
    while (!release.load())
      std::this_thread::yield();
  }
}
inline void worker_exit(std::uint64_t token, std::size_t n) {
  if (n > 1 && token == 1)
    ++exited;
}
inline void before_publish(std::int64_t frame, std::size_t n) {
  if (n > 1 && frame == 0)
    wait_count(ready, static_cast<unsigned>(n));
}
inline void published(std::int64_t frame, std::size_t n) {
  if (n > 1 && frame == 0)
    wait_count(parked, static_cast<unsigned>(n));
}
inline void quiescence(std::int64_t frame, std::size_t n) {
  if (n > 1 && frame == 0) {
    std::fprintf(stderr,
                 "coordinator waits for %zu parked workers before reuse\n", n);
    release = true;
  }
}
inline void reset(std::int64_t frame, std::size_t n,
                  std::atomic<std::size_t> &remaining,
                  std::atomic<std::size_t> &next) {
  if (n > 1 && frame == 1 && !release.load()) {
    std::fprintf(stderr,
                 "REPRO: metadata reused with %u parked workers, remaining=%zu "
                 "next=%zu\n",
                 parked.load(), remaining.load(), next.load());
    release = true;
    wait_count(exited, static_cast<unsigned>(n));
    std::fprintf(stderr,
                 "REPRO: unpublished work consumed, remaining=%zu next=%zu\n",
                 remaining.load(), next.load());
    std::thread([] {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      std::fprintf(
          stderr,
          "REPRO: no progress after publication overwrote stale decrements\n");
      std::_Exit(42);
    }).detach();
  }
}
} // namespace m27_schedule
