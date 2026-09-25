#pragma once
#include <rtfw/benchmark.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace example {
namespace b = rtfw::benchmark;

// Copy this source-owned class into your subsystem. The Runner is unchanged.
// Transform/cleanup and their user object are borrowed through successful finish.
// A failed cleanup retains prepared ownership so the host can retry it.
class Provider {
public:
    using Transform = bool (*)(void*, const std::uint64_t*, std::uint64_t*, std::size_t);
    using Cleanup = bool (*)(void*);
    explicit Provider(void* user = nullptr, Transform transform = nullptr, Cleanup cleanup = nullptr) noexcept;
    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;
    [[nodiscard]] b::ProviderV1 table() noexcept;
    [[nodiscard]] b::Status prepare(std::string_view case_id, bool available = true) noexcept;
    [[nodiscard]] b::Status finish() noexcept;
    [[nodiscard]] std::uint64_t completed() const noexcept { return completed_; }
    [[nodiscard]] bool owns_session() const noexcept { return prepared_; }
private:
    static b::Status describe(void*, std::size_t, b::Descriptor&);
    static b::Status invoke(void*, std::string_view, std::uint64_t, b::Observation&);
    static bool transform_default(void*, const std::uint64_t*, std::uint64_t*, std::size_t);
    std::array<std::uint64_t, 1024> input_{}, output_{};
    void* user_{};
    Transform transform_{};
    Cleanup cleanup_{};
    std::string_view selected_{};
    std::size_t count_{};
    std::uint64_t next_{}, completed_{};
    bool prepared_{}, available_{}, failed_{};
};
// Host transaction: destination validation, prepare, run, checked finish, publish.
// If finish fails, provider retains its session; the caller must retry finish.
[[nodiscard]] b::Status run_to_directory(Provider&, std::string_view, const b::ClockV1&,
                                       const b::Identity&, const std::filesystem::path&, bool available = true);
}  // namespace example
