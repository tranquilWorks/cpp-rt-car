#pragma once
#include <array>
#include <cstdint>
#include <type_traits>

namespace rtfw::cuda_physics {
inline constexpr std::uint32_t max_particles = 4096;
inline constexpr std::uint32_t max_steps = 1024;
struct Particle {
    std::int32_t position[3]{};
    std::int32_t velocity[3]{};
    std::int32_t acceleration[3]{};
};
static_assert(sizeof(Particle) == 9 * sizeof(std::int32_t));
static_assert(std::is_trivially_copyable_v<Particle>);
struct Options {
    std::uint32_t count = 256;
    std::uint32_t steps = 64;
    std::uint32_t seed = 1;
    std::uint32_t workers = 2;
    [[nodiscard]] bool valid() const noexcept {
        return count > 0 && count <= max_particles && steps > 0 &&
            steps <= max_steps && workers > 0 && workers <= 2;
    }
};
// LCG arithmetic deliberately wraps modulo 2^32. Each axis consumes x,v,a.
inline Particle initial_particle(std::uint32_t& state) noexcept {
    Particle p;
    auto next = [&state](std::uint32_t modulus, std::int32_t offset) {
        state = state * 1664525u + 1013904223u;
        return static_cast<std::int32_t>(state % modulus) - offset;
    };
    for (unsigned axis = 0; axis < 3; ++axis) {
        p.position[axis] = next(2049, 1024);
        p.velocity[axis] = next(129, 64);
        p.acceleration[axis] = next(9, 4);
    }
    return p;
}
// Independent closed form: never used by the injected iterative kernel.
inline bool matches_oracle(const Particle& value, const Particle& initial,
                           std::uint32_t step) noexcept {
    if (step > max_steps) return false;
    const auto n = static_cast<std::int64_t>(step);
    for (unsigned axis = 0; axis < 3; ++axis) {
        const auto a = static_cast<std::int64_t>(initial.acceleration[axis]);
        const auto v = static_cast<std::int64_t>(initial.velocity[axis]);
        const auto x = static_cast<std::int64_t>(initial.position[axis]);
        if (value.position[axis] != x + n*v + a*n*(n+1)/2 ||
            value.velocity[axis] != v + n*a || value.acceleration[axis] != a)
            return false;
    }
    return true;
}
} // namespace rtfw::cuda_physics
