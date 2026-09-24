#pragma once

// Optional installed example sources, not an additional compiled SDK surface.
#include <rtfw/benchmark.hpp>
#include <memory>
#include <span>

namespace rtfw::benchmark::runtime {
enum class Family { rates, channels, shedding, controls, checkpoint, replay,
                    watchdog, telemetry, capacity, device, composition };
struct Case {
    const char* id;
    Family family;
    const char* mode;
    std::size_t count, width, bytes, capacity, variant;
    const char* scope;
    const char* unit;
    bool allocation_free;
};
[[nodiscard]] std::span<const Case> cases() noexcept;
[[nodiscard]] const char* family_name(Family) noexcept;
class Provider {
public:
    Provider();
    ~Provider();
    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;
    [[nodiscard]] ProviderV1 table() noexcept;
    [[nodiscard]] Status prepare(std::string_view);
    [[nodiscard]] Status finish() noexcept;
    [[nodiscard]] bool prepared() const noexcept;
    [[nodiscard]] std::uint64_t invocations() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
    static Status describe(void*, std::size_t, Descriptor&);
    static Status invoke(void*, std::string_view, std::uint64_t, Observation&);
};
} // namespace rtfw::benchmark::runtime
