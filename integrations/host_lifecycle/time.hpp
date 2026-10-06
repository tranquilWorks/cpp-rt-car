#pragma once
#include <rt/runtime.hpp>
#include <chrono>
#include <cstdint>
#include <limits>

namespace rtfw_host {
// Exact floor(ticks * 1e9 / frequency), with no floating-point conversion.
// Frequencies above UINT64_MAX/1e9 are explicitly unsupported; no overflow or
// silent saturation. Ticks and deadlines must already share the host's domain.
inline rt::Status ticks_to_ns(std::uint64_t ticks, std::uint64_t frequency,
                             std::uint64_t& output) noexcept {
    constexpr std::uint64_t scale = 1000000000;
    constexpr auto limit = std::numeric_limits<std::uint64_t>::max();
    if (!frequency || frequency > limit / scale) return rt::Status::invalid_argument;
    const auto seconds = ticks / frequency;
    if (seconds > limit / scale) return rt::Status::invalid_argument;
    const auto whole = seconds * scale;
    const auto fraction = (ticks % frequency) * scale / frequency;
    if (fraction > limit - whole) return rt::Status::invalid_argument;
    output = whole + fraction;
    return rt::Status::ok;
}

inline rt::Status frame_from_ticks(std::uint64_t index, std::uint64_t delta,
                                  std::uint64_t frequency,
                                  std::optional<std::uint64_t> deadline,
                                  rt::HostFrameContext& output,
                                  std::optional<std::uint64_t> nominal_release = std::nullopt) noexcept {
    std::uint64_t converted = 0;
    const auto status = ticks_to_ns(delta, frequency, converted);
    if (status != rt::Status::ok ||
        converted > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return rt::Status::invalid_argument;
    std::optional<std::uint64_t> deadline_ns;
    if (deadline) {
        std::uint64_t value = 0;
        if (ticks_to_ns(*deadline, frequency, value) != rt::Status::ok)
            return rt::Status::invalid_argument;
        deadline_ns = value;
    }
    std::optional<std::uint64_t> nominal_ns;
    if (nominal_release) {
        std::uint64_t value = 0;
        if (ticks_to_ns(*nominal_release, frequency, value) != rt::Status::ok)
            return rt::Status::invalid_argument;
        nominal_ns = value;
    }
    output = {index, std::chrono::nanoseconds(static_cast<std::int64_t>(converted)), deadline_ns, nominal_ns};
    return rt::Status::ok;
}
}
