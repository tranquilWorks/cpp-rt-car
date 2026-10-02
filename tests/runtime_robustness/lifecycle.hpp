#pragma once
#include <rt/runtime.hpp>
#include <array>
#include <stdexcept>
#include <string>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace {
void check_condition(bool value, const char* file, int line) {
    if (!value) throw std::runtime_error(std::string(file) + ":" +
        std::to_string(line) + " regression assertion failed");
}
#define REQUIRE(...) check_condition((__VA_ARGS__), __FILE__, __LINE__)
struct Clock final : rt::RuntimeClock {
    std::uint64_t now_ns() noexcept override { return 1000; }
};
struct Owner {
    Clock clock;
    std::array<std::byte, 8> bytes{};
    unsigned calls = 0;
    rt::Runtime runtime{clock};
    std::vector<std::byte> saved;
    std::array<std::byte, 8> saved_state{};
    bool running = true;
    Owner() {
        rt::RuntimeConfig config;
        config.callback_capacity = 1;
        config.worker_count = 1;
        config.executor_queue_capacity = 2;
        config.task_scratch_slots = 2;
        config.scratch_bytes = config.task_scratch_bytes = 0;
        config.trace_capacity = 32;
        REQUIRE(runtime.configure(config) == rt::Status::ok);
        rt::PhaseHandle phase;
        REQUIRE(runtime.register_callback({"increment", [](void* opaque, const rt::CallbackContext&) {
            auto& self = *static_cast<Owner*>(opaque);
            self.bytes[0] = static_cast<std::byte>((std::to_integer<unsigned>(self.bytes[0])+1u)&255u);
            ++self.calls;
            return rt::CallbackResult::ok;
        }, this}, phase) == rt::Status::ok);
        REQUIRE(runtime.register_state({"state", 1, bytes}) == rt::Status::ok);
        REQUIRE(runtime.finalize() == rt::Status::ok);
        REQUIRE(runtime.start() == rt::Status::ok);
        capture();
    }
    void capture() {
        std::size_t n = 0;
        REQUIRE(runtime.checkpoint_size(n) == rt::Status::ok && n <= 65536);
        saved.resize(n);
        rt::ArtifactWriteResult result;
        REQUIRE(runtime.write_checkpoint(calls, saved, result) == rt::Status::ok && result.bytes_written == n);
        saved_state = bytes;
    }
    ~Owner() { (void)runtime.stop(); }
};
}
void lifecycle_case(const std::vector<std::uint8_t>& program) {
    const auto* data = program.data();
    const auto size = program.size();
    Owner a, b;
    for (std::size_t i = 0; i < size && i < 32; ++i) {
        auto& self = (data[i]&1) ? a : b;
        auto& other = (data[i]&1) ? b : a;
        const auto other_before = other.bytes;
        const auto other_calls = other.calls;
        auto expected = self.bytes;
        auto expected_calls = self.calls;
        switch ((data[i] >> 1) % 7) {
        case 0: {
            const auto status = self.runtime.step({self.calls, std::chrono::nanoseconds{100}});
            REQUIRE(status == (self.running ? rt::Status::ok : rt::Status::invalid_state));
            if (self.running) {
                expected[0] = static_cast<std::byte>((std::to_integer<unsigned>(expected[0])+1u)&255u);
                ++expected_calls;
            }
            break;
        }
        case 1: self.capture(); break;
        case 2:
            REQUIRE(self.runtime.restore_checkpoint(self.saved) == rt::Status::ok);
            expected = self.saved_state;
            break;
        case 3: {
            auto broken = self.saved;
            broken[0] ^= std::byte{1};
            REQUIRE(self.runtime.restore_checkpoint(broken) != rt::Status::ok);
            break;
        }
        case 4:
            REQUIRE(self.runtime.stop() == rt::Status::ok);
            self.running = false;
            break;
        case 5: REQUIRE(self.runtime.start() == rt::Status::invalid_state); break;
        case 6:
            REQUIRE(self.runtime.restore_checkpoint(other.saved) == rt::Status::ok);
            expected = other.saved_state;
            break;
        }
        REQUIRE(self.bytes == expected && self.calls == expected_calls);
        REQUIRE(other.bytes == other_before && other.calls == other_calls);
        REQUIRE(self.runtime.state() == (self.running ? rt::RuntimeState::running : rt::RuntimeState::stopped));
    }
    REQUIRE(a.runtime.stop() == rt::Status::ok);
    REQUIRE(b.runtime.stop() == rt::Status::ok);
}
