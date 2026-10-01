#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
namespace m27_startup {
inline std::atomic<bool> entered{false}, released{false};
inline void entered_gate() {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!entered.load()) {
    if (std::chrono::steady_clock::now() > deadline)
      std::_Exit(43);
    std::this_thread::yield();
  }
}
inline void before_bind(std::size_t i, std::size_t n) {
  if (n > 1 && i == 0) {
    entered = true;
    while (!released.load())
      std::this_thread::yield();
  }
}
inline void before_wait(std::size_t n) {
  if (n > 1) {
    entered_gate();
    std::fprintf(stderr, "coordinator waits for arena initialization\n");
    released = true;
  }
}
inline void before_frame(std::size_t n) {
  if (n > 1 && !released.load()) {
    entered_gate();
    std::fprintf(stderr,
                 "REPRO: frame reset reached before worker arena binding\n");
    std::_Exit(44);
  }
}
} // namespace m27_startup
