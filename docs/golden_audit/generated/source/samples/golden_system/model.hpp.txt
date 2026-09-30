#pragma once
#include "fixed.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace golden {
using Axis = std::array<std::int32_t, fixed::capacity>;
using Vector = std::array<Axis, 3>;
struct alignas(64) Plant {
  Vector position{}, velocity{}, acceleration{};
};
static_assert(sizeof(Plant) == 9216 && offsetof(Plant, velocity) == 3072 &&
              offsetof(Plant, acceleration) == 6144);
struct alignas(64) Sensor {
  Vector position{}, velocity{};
};
enum class Campaign : std::uint32_t {
  nominal,
  overload,
  stale_input,
  control_rejected,
  control_replaced,
  peer_missing
};
struct Options {
  std::size_t count = fixed::default_count, ticks = fixed::default_ticks,
              workers = 2, grain = 4;
  bool host = false, external = false;
  Campaign campaign = Campaign::nominal;
  bool valid() const noexcept {
    return count && count <= fixed::capacity && ticks &&
           ticks <= fixed::maximum_ticks && workers >= 1 && workers <= 3 &&
           (grain == 1 || grain == 4 || grain == 16 || grain == 64) &&
           static_cast<unsigned>(campaign) <=
               static_cast<unsigned>(Campaign::peer_missing) &&
           (campaign == Campaign::nominal || ticks >= 19);
  }
};
inline constexpr std::array<std::string_view, 6> campaign_names{
    "nominal",          "overload",         "stale_input",
    "control_rejected", "control_replaced", "peer_missing"};
inline std::uint64_t digest(std::span<const std::byte> bytes) noexcept {
  std::uint64_t h = 14695981039346656037ULL;
  for (auto b : bytes) {
    h ^= std::to_integer<unsigned char>(b);
    h *= 1099511628211ULL;
  }
  return h;
}
inline void put(std::span<std::byte> b, std::size_t at, std::uint64_t value,
                std::size_t width) noexcept {
  for (std::size_t i = 0; i < width; ++i)
    b[at + i] = std::byte((value >> (8 * i)) & 255);
}
inline std::uint64_t get(std::span<const std::byte> b, std::size_t at,
                         std::size_t width) noexcept {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < width; ++i)
    v |= std::uint64_t(std::to_integer<unsigned char>(b[at + i])) << (8 * i);
  return v;
}
inline void put32(std::span<std::byte> b, std::size_t at,
                  std::int32_t v) noexcept {
  put(b, at, std::bit_cast<std::uint32_t>(v), 4);
}
inline std::int32_t get32(std::span<const std::byte> b,
                          std::size_t at) noexcept {
  return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(get(b, at, 4)));
}
inline Plant initial(std::size_t count) noexcept {
  Plant p;
  std::uint32_t seed = 1;
  auto next = [&](std::uint32_t modulus, std::int32_t offset) {
    seed = seed * 1664525U + 1013904223U;
    return static_cast<std::int32_t>(seed % modulus) - offset;
  };
  for (std::size_t i = 0; i < count; ++i)
    for (std::size_t a = 0; a < 3; ++a) {
      p.position[a][i] = next(2049, 1024);
      p.velocity[a][i] = next(129, 64);
      p.acceleration[a][i] = next(9, 4);
    }
  return p;
}
inline std::int32_t effort(std::int32_t velocity, std::int32_t target,
                           std::int32_t gain) noexcept {
  return static_cast<std::int32_t>(
      std::clamp(std::int64_t(gain) * (std::int64_t(target) - velocity),
                 std::int64_t{-4}, std::int64_t{4}));
}
// Fixed full-capacity frame: existing 120-byte sampled header geometry followed
// by LE field/axis/entity words. CPU paths apply these sample-owned checks; no
// HAL sampled-I/O registration or hardware claim is implied.
inline constexpr std::size_t header_bytes = 120,
                             maximum_frame_bytes =
                                 header_bytes + sizeof(Sensor);
using Frame = std::array<std::byte, maximum_frame_bytes>;
inline std::size_t frame_size(std::size_t channel) noexcept {
  return header_bytes + fixed::channel_elements[channel] * fixed::capacity * 4;
}
inline std::span<std::byte> frame_span(Frame &f, std::size_t c) noexcept {
  return std::span<std::byte>(f).first(frame_size(c));
}
inline std::span<const std::byte> frame_span(const Frame &f,
                                             std::size_t c) noexcept {
  return std::span<const std::byte>(f).first(frame_size(c));
}
inline void seal(Frame &f, std::size_t channel, std::uint64_t tick,
                 std::uint64_t sequence) noexcept {
  put(f, 0, 120, 4);
  put(f, 4, 1, 4);
  put(f, 8, 26001 + channel, 8);
  put(f, 16, sequence, 8);
  put(f, 24, sequence, 8);
  put(f, 32, 1, 4);
  put(f, 36, 3, 4);
  put(f, 40, 1, 8);
  put(f, 48, tick * fixed::tick_ns, 8);
  put(f, 56, fixed::tick_ns, 8);
  put(f, 64, 1, 8);
  put(f, 72, sequence, 8);
  put(f, 80, 1, 8);
  put(f, 88, digest(frame_span(f, channel).subspan(header_bytes)), 8);
  put(f, 96, 1,
      4); // Sample-owned CPU frame status; immutable SDK layouts unchanged.
}
inline bool valid_frame(const Frame &f, std::size_t channel,
                        std::size_t count) noexcept {
  if (get(f, 0, 4) != 120 || get(f, 4, 4) != 1 ||
      get(f, 8, 8) != 26001 + channel || !get(f, 16, 8) ||
      get(f, 24, 8) != get(f, 16, 8) || get(f, 32, 4) != 1 ||
      get(f, 36, 4) != 3 || get(f, 40, 8) != 1 ||
      get(f, 56, 8) != fixed::tick_ns || get(f, 64, 8) != 1 ||
      get(f, 72, 8) != get(f, 16, 8) || get(f, 80, 8) != 1 ||
      get(f, 88, 8) != digest(frame_span(f, channel).subspan(header_bytes)) ||
      get(f, 96, 4) != 1)
    return false;
  for (std::size_t i = 100; i < header_bytes; ++i)
    if (f[i] != std::byte{})
      return false;
  for (std::size_t field = 0; field < fixed::channel_elements[channel]; ++field)
    for (std::size_t i = count; i < fixed::capacity; ++i)
      if (get32(f, header_bytes + 4 * (field * fixed::capacity + i)) != 0)
        return false;
  return true;
}
inline void pack_vector(Frame &f, const Vector &v,
                        std::size_t field = 0) noexcept {
  for (std::size_t a = 0; a < 3; ++a)
    for (std::size_t i = 0; i < fixed::capacity; ++i)
      put32(f, header_bytes + 4 * ((field + a) * fixed::capacity + i), v[a][i]);
}
inline void unpack_vector(const Frame &f, Vector &v,
                          std::size_t field = 0) noexcept {
  for (std::size_t a = 0; a < 3; ++a)
    for (std::size_t i = 0; i < fixed::capacity; ++i)
      v[a][i] =
          get32(f, header_bytes + 4 * ((field + a) * fixed::capacity + i));
}
} // namespace golden
