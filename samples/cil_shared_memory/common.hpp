#pragma once
#include "mapping.hpp"
#include <charconv>
#include <chrono>
#include <iostream>
#include <thread>
namespace cil {
inline std::uint64_t now_ns() noexcept {
    const auto n = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return n > 0 ? static_cast<std::uint64_t>(n) : 0;
}
struct Options {
    std::string_view key{}, mode = "normal";
    std::uint64_t generation = 0, timeout_ms = 5000;
};
inline bool number(std::string_view s, std::uint64_t& out) noexcept {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}
inline bool options(int argc, char** argv, Options& out) noexcept {
    if (argc != 4 && argc != 5) return false;
    out.key = argv[1];
    if (!valid_key(out.key) || !number(argv[2], out.generation) || !out.generation ||
        !number(argv[3], out.timeout_ms) || out.timeout_ms < 100 || out.timeout_ms > 10000) return false;
    if (argc == 5) out.mode = argv[4];
    return true;
}
inline void pause() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
inline std::uint64_t deadline(const Options& o) noexcept { return now_ns() + o.timeout_ms * 1'000'000; }
inline int closed(Mapping& mapping, Code result) {
    const auto cleanup = mapping.close();
    if (cleanup != Code::ok) {
        std::cerr << "cleanup=" << name(cleanup) << '\n';
        // Explicit bounded retry keeps the owning Mapping alive.
        if (mapping.close() != Code::ok) std::terminate();
        result = cleanup;
    }
    return result == Code::ok ? 0 : 2;
}
} // namespace cil
