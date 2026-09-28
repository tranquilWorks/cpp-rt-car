#pragma once
#include <rt/device.hpp>
#include <algorithm>
namespace backend_kit {
using Status = rt::HalV2Status;
inline constexpr std::uint32_t copy_opcode = 1;
enum class Fault { none, initialize, registration, completion_error,
                   completion_timeout, completion_loss, unregister, shutdown };
template<class T> bool zero(const T& values) noexcept {
    return std::all_of(values.begin(), values.end(), [](auto x) { return x == 0; });
}
inline bool identifier(const auto& text) noexcept {
    const auto end = std::find(text.begin(), text.end(), '\0');
    return end != text.begin() && end != text.end() &&
        std::all_of(text.begin(), end, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        });
}

} // namespace backend_kit
