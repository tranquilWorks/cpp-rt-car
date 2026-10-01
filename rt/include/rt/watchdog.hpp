#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace rt {

class Watchdog {
public:
    using Callback = std::function<void()>;

    Watchdog(std::chrono::milliseconds budget,
             std::chrono::milliseconds limp_budget,
             Callback limp_cb)
        : budget_(budget),
          limp_budget_(limp_budget),
          limp_cb_(std::move(limp_cb)),
          stop_(false),
          armed_(false) {
        last_ = std::chrono::steady_clock::now();
        worker_ = std::thread([this] { run(); });
    }

    ~Watchdog() {
        {
            std::lock_guard<std::mutex> lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void touch() {
        std::lock_guard<std::mutex> lk(m_);
        last_ = std::chrono::steady_clock::now();
        cv_.notify_all();
    }

    void arm(std::chrono::milliseconds budget) {
        std::lock_guard<std::mutex> lk(m_);
        budget_ = budget;
        limp_budget_ = budget * 2;
        last_ = std::chrono::steady_clock::now();
        armed_ = true;
        cv_.notify_all();
    }

    void disarm() {
        std::unique_lock<std::mutex> lk(m_);
        if (armed_) {
            auto now = std::chrono::steady_clock::now();
            auto budget = limp_.load() ? limp_budget_ : budget_;
            if (now - last_ >= budget) {
                limp_ = true;
                trips_.fetch_add(1, std::memory_order_relaxed);
                auto cb = limp_cb_;
                lk.unlock();
                if (cb)
                    cb();
                lk.lock();
                last_ = now;
            }
        }
        armed_ = false;
        cv_.notify_all();
    }

    bool limp() const noexcept { return limp_.load(); }
    int trips() const noexcept { return trips_.load(); }

private:
    void run() {
        std::unique_lock<std::mutex> lk(m_);
        while (!stop_) {
            if (!armed_) {
                cv_.wait(lk, [this] { return stop_.load() || armed_; });
                continue;
            }
            auto budget = limp_.load() ? limp_budget_ : budget_;
            auto start = last_;
            if (cv_.wait_for(lk, budget, [this] { return stop_.load() || !armed_; })) {
                continue;
            }
            if (!armed_)
                continue;
            auto now = std::chrono::steady_clock::now();
            if (now - start >= budget && start == last_) {
                limp_ = true;
                trips_.fetch_add(1, std::memory_order_relaxed);
                lk.unlock();
                if (limp_cb_) {
                    limp_cb_();
                }
                lk.lock();
                last_ = now;
            }
        }
    }

    std::chrono::milliseconds budget_;
    std::chrono::milliseconds limp_budget_;
    Callback limp_cb_;
    std::atomic<bool> limp_{false};
    std::atomic<bool> stop_;
    std::atomic<bool> armed_;
    std::chrono::steady_clock::time_point last_;
    std::thread worker_;
    std::mutex m_;
    std::condition_variable cv_;
    std::atomic<int> trips_{0};
};

} // namespace rt

