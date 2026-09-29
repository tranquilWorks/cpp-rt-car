#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <new>
#include <rt/runtime.hpp>
#include <thread>

namespace golden {
// Derived from the unchanged M25-05 jobs.hpp; one/two/three execution indices.
// Up to two host workers plus one registered frame-thread helper. All Runtime
// frame control sharing this pool belongs to that one frame thread. No queue
// mutex.
class Jobs {
  struct Slot {
    std::atomic<unsigned> state{0};
    rt::HostExecutorJob job{};
  };
  std::array<Slot, 1024> slots_{};
  std::array<std::thread, 2> threads_{};
  std::atomic<bool> running_{false};
  std::thread::id control_{};
  std::size_t workers_ = 2;
  std::size_t owners_ = 0; // Frame-thread lifecycle only.
  inline static thread_local Jobs *current_ = nullptr;
  inline static thread_local std::uint32_t worker_ = 0;
  bool execute_one(std::uint32_t worker) noexcept {
    for (auto &slot : slots_) {
      unsigned expected = 2;
      if (!slot.state.compare_exchange_strong(expected, 3,
                                              std::memory_order_acquire))
        continue;
      const auto job = slot.job;
      // Keep the slot reserved through callback return, including nested help.
      try {
        job.execute(job.execution_context, job.completion_context,
                    job.completion_token, worker);
      } catch (...) {
        std::terminate();
      } // An accepted throwing job cannot be safely replayed.
      completed.fetch_add(1, std::memory_order_relaxed);
      slot.state.store(0, std::memory_order_release);
      pending.fetch_sub(1, std::memory_order_release);
      return true;
    }
    return false;
  }

public:
  static constexpr std::size_t capacity = 1024;
  static_assert(std::atomic<unsigned>::is_always_lock_free &&
                std::atomic<std::size_t>::is_always_lock_free);
  std::atomic<std::size_t> accepted{0}, completed{0}, rejected{0}, pending{0},
      helped{0};
  Jobs() = default;
  Jobs(const Jobs &) = delete;
  Jobs &operator=(const Jobs &) = delete;
  ~Jobs() {
    if (close() != rt::Status::ok)
      std::terminate();
  }
  // workers_enabled=false is a deterministic capacity/help fixture, still a
  // running cooperative job system driven by its registered frame thread.
  rt::Status start(std::size_t workers = 2, bool workers_enabled = true,
                   bool fail_second_for_test = false) noexcept {
    if (workers < 1 || workers > 3 || running_.load() || owners_ ||
        pending.load())
      return rt::Status::invalid_state;
    workers_ = workers;
    control_ = std::this_thread::get_id();
    running_.store(true, std::memory_order_release);
    if (!workers_enabled)
      return rt::Status::ok;
    try {
      for (std::size_t i = 0; i + 1 < workers_; ++i) {
        if (i == 1 && fail_second_for_test)
          throw std::bad_alloc{};
        threads_[i] = std::thread([this, i] {
          current_ = this;
          worker_ = static_cast<std::uint32_t>(i + 1);
          while (running_.load(std::memory_order_acquire))
            if (!execute_one(worker_))
              std::this_thread::yield();
          current_ = nullptr;
        });
      }
    } catch (...) {
      running_.store(false, std::memory_order_release);
      for (auto &t : threads_)
        if (t.joinable())
          t.join();
      return rt::Status::resource_exhausted;
    }
    return rt::Status::ok;
  }
  rt::HostExecutorAdapter adapter() noexcept {
    return {this, workers_, capacity, &submit, &help};
  }
  static rt::Status submit(void *opaque,
                           const rt::HostExecutorJob &job) noexcept {
    if (!opaque || !job.execute)
      return rt::Status::invalid_argument;
    auto &self = *static_cast<Jobs *>(opaque);
    if (!self.running_.load(std::memory_order_acquire))
      return rt::Status::invalid_state;
    for (auto &slot : self.slots_) {
      unsigned expected = 0;
      if (!slot.state.compare_exchange_strong(expected, 1,
                                              std::memory_order_acquire))
        continue;
      self.pending.fetch_add(1, std::memory_order_relaxed);
      slot.job = job;
      self.accepted.fetch_add(1, std::memory_order_relaxed);
      slot.state.store(2, std::memory_order_release);
      return rt::Status::ok;
    }
    self.rejected.fetch_add(1, std::memory_order_relaxed);
    return rt::Status::queue_full;
  }
  static bool help(void *opaque) noexcept {
    if (!opaque)
      return false;
    auto &self = *static_cast<Jobs *>(opaque);
    if (current_ != &self && self.control_ != std::this_thread::get_id())
      return false;
    const bool did = self.execute_one(current_ == &self ? worker_ : 0);
    if (did)
      self.helped.fetch_add(1, std::memory_order_relaxed);
    return did;
  }
  rt::Status attach() noexcept {
    if (!running_.load() || control_ != std::this_thread::get_id() ||
        owners_ == 2)
      return rt::Status::invalid_state;
    ++owners_;
    return rt::Status::ok;
  }
  rt::Status detach() noexcept {
    if (!owners_ || pending.load(std::memory_order_acquire) ||
        control_ != std::this_thread::get_id())
      return rt::Status::invalid_state;
    --owners_;
    return rt::Status::ok;
  }
  std::size_t owners() const noexcept { return owners_; }
  // Call only after all frame/submit callers are quiescent. Workers may still
  // be returning from the final accepted job. No new external admission here.
  rt::Status quiesce() noexcept {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (pending.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= end)
        return rt::Status::resource_exhausted;
      if (!help(this))
        std::this_thread::yield();
    }
    return rt::Status::ok;
  }
  rt::Status close() noexcept {
    // Never cancel/discard an accepted job or invalidate an attached owner.
    if (owners_ || pending.load(std::memory_order_acquire))
      return rt::Status::invalid_state;
    running_.store(false, std::memory_order_release);
    for (auto &t : threads_)
      if (t.joinable())
        t.join();
    return rt::Status::ok;
  }
};
} // namespace golden
