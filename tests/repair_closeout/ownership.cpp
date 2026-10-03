#include "observed.hpp"
#include "fixture.hpp"
#include "hooks.hpp"
#include <atomic>
#include <chrono>
#include <thread>

namespace repair_observer {
std::atomic<int> selected{0};
std::atomic<bool> reached{false}, released{false};
void pause(int point) noexcept {
    if (selected.load(std::memory_order_acquire) != point) return;
    reached.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!released.load(std::memory_order_acquire)) {
        require(std::chrono::steady_clock::now() < deadline, "observer release timeout");
        std::this_thread::yield();
    }
}
void arm(int point) {
    reached.store(false); released.store(false); selected.store(point);
}
void await() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!reached.load(std::memory_order_acquire)) {
        require(std::chrono::steady_clock::now() < deadline, "observer reach timeout");
        std::this_thread::yield();
    }
}
void release() { released.store(true, std::memory_order_release); }
}
int main(int argc, char** argv) {
    const bool poll_case = argc == 2 && argv[1][0] == 'p';
    Fixture<rt::ObservedLoopbackBackend> f;
    require(f.submit(1) == H::ok, "initial submit");
    if (!poll_case) f.completed(1, H::ok);
    repair_observer::arm(poll_case ? 2 : 1);
    rt::HalV2BatchCompletion completion{};
    std::uint64_t count = 0;
    H status = H::error;
    std::thread owner([&] {
        if (poll_case) count = f.poll(completion);
        else status = f.submit(2);
    });
    repair_observer::await();
    // Storage is exclusively owned, although it still contains the old ID.
    const auto canceled = f.cancel(1);
    rt::HalV2BatchCompletion stray{};
    const auto stray_count = poll_case ? 0 : f.poll(stray);
    f.stop();
    require(f.submit(3) == H::invalid_state, "stop refuses later submit");
    repair_observer::release(); owner.join();
    repair_observer::selected.store(0);
    require(canceled == H::invalid_argument, "cancel cannot claim exclusively owned storage");
    require(stray_count == 0, "poll cannot consume stale owned storage");
    if (poll_case) {
        require(count == 1 && completion.batch_id == 1 && completion.status == 0, "poll owner unchanged");
        require(f.poll(stray) == 0, "poll owner retires exactly once");
    } else {
        require(status == H::ok, "already admitted submit completes after stop");
        f.completed(2, H::ok);
    }
    require(f.backend.stats().cancellations == 0, "exclusive owner not canceled");
    f.shutdown();
    std::puts("PASS deterministic exclusive completion ownership");
}
