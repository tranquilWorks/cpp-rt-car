#pragma once
#include "controls.hpp"
#include <atomic>
#include <limits>

namespace golden {
inline constexpr std::size_t state_header_bytes = 512,
                             state_bytes =
                                 state_header_bytes + 9216 + 3072 + 6144;
using StateBytes = std::array<std::byte, state_bytes>;
struct World {
  Options options;
  // Explicit storage gaps preserve required 64-byte SoA alignment on MSVC.
  // They are never part of the canonical state or channel bytes.
  std::array<std::byte, 64 - sizeof(Options)> options_padding{};
  Plant plant{};
  alignas(64) Vector command{};
  Sensor sensor{};
  Configuration configuration{};
  std::array<std::int64_t, 6> sums{};
  std::array<std::uint64_t, 8> calls{};
  std::array<std::uint64_t, 5> publications{}, selections{}, ages{},
      generations{}, producer_releases{}, timestamps{};
  std::uint64_t next_tick = 0, stale = 0, missing = 0;
  std::array<Frame, 5> buffers{};
  std::array<rt::CrossRateChannelHandle, 5> channels{};
  StateBytes canonical{};
  // One validated host snapshot per external controller release. I/O and waits
  // belong to host control; callback copies these fixed arrays once.
  Vector external_command{};
  bool external_ready = false;
  std::array<std::byte, 39> storage_padding{};
  explicit World(Options value)
      : options(value), plant(initial(value.count)),
        command(plant.acceleration) {
    encode();
  }
  void encode() noexcept {
    canonical.fill(std::byte{});
    put(canonical, 0, 0x5336324d, 4);
    put(canonical, 4, 1, 4);
    put(canonical, 8, options.count, 4);
    put(canonical, 12, options.ticks, 4);
    put(canonical, 16, options.workers, 4);
    put(canonical, 20, options.grain, 4);
    put(canonical, 24, options.host, 4);
    put(canonical, 28, options.external, 4);
    put(canonical, 32, static_cast<unsigned>(options.campaign), 4);
    put32(canonical, 40, configuration.target);
    put32(canonical, 44, configuration.gain);
    put32(canonical, 48, configuration.calibration);
    put32(canonical, 52, configuration.fault);
    put(canonical, 56, next_tick, 8);
    put(canonical, 64, configuration.generation, 8);
    const auto write_counts = [&](const auto &values, std::size_t offset) {
      for (auto value : values) {
        put(canonical, offset, value, 8);
        offset += 8;
      }
    };
    write_counts(calls, 72);
    write_counts(publications, 136);
    write_counts(selections, 176);
    write_counts(ages, 216);
    write_counts(generations, 256);
    write_counts(producer_releases, 296);
    write_counts(timestamps, 336);
    put(canonical, 376, stale, 8);
    put(canonical, 384, missing, 8);
    put(canonical, 392, configuration.applications, 8);
    for (std::size_t i = 0; i < 6; ++i)
      put(canonical, 400 + 8 * i, std::bit_cast<std::uint64_t>(sums[i]), 8);
    const auto hex = [](char c) {
      return static_cast<unsigned>(c >= '0' && c <= '9' ? c - '0'
                                                        : c - 'a' + 10);
    };
    for (std::size_t i = 0; i < 32; ++i)
      canonical[448 + i] = std::byte((hex(fixed::contract_sha256[2 * i]) << 4) |
                                     hex(fixed::contract_sha256[2 * i + 1]));
    std::size_t offset = state_header_bytes;
    for (const auto *vector :
         {&plant.position, &plant.velocity, &plant.acceleration, &command,
          &sensor.position, &sensor.velocity})
      for (const auto &axis : *vector)
        for (auto value : axis) {
          put32(canonical, offset, value);
          offset += 4;
        }
    put(canonical, 480, digest(canonical), 8);
  }
  bool decode() noexcept {
    auto checked = canonical;
    const auto checksum = get(checked, 480, 8);
    put(checked, 480, 0, 8);
    if (digest(checked) != checksum)
      return false;
    const auto hex = [](char c) {
      return static_cast<unsigned>(c >= '0' && c <= '9' ? c - '0'
                                                        : c - 'a' + 10);
    };
    for (std::size_t i = 0; i < 32; ++i)
      if (canonical[448 + i] !=
          std::byte((hex(fixed::contract_sha256[2 * i]) << 4) |
                    hex(fixed::contract_sha256[2 * i + 1])))
        return false;
    for (std::size_t i = 488; i < 512; ++i)
      if (canonical[i] != std::byte{})
        return false;
    if (get(canonical, 36, 4) || get32(canonical, 40) < -64 ||
        get32(canonical, 40) > 64 || get32(canonical, 44) < 0 ||
        get32(canonical, 44) > 4 || get32(canonical, 48) < -16 ||
        get32(canonical, 48) > 16 || get32(canonical, 52) < 0 ||
        get32(canonical, 52) > 11)
      return false;
    constexpr std::array<std::int32_t, 6> bounds{2165760, 4160,    4,
                                                 4,       2165776, 4176};
    for (std::size_t f = 0; f < 6; ++f)
      for (std::size_t a = 0; a < 3; ++a)
        for (std::size_t i = 0; i < fixed::capacity; ++i) {
          const auto v =
              get32(canonical, 512 + 4 * ((f * 3 + a) * fixed::capacity + i));
          if (v < -bounds[f] || v > bounds[f] || (i >= options.count && v))
            return false;
        }
    if (get(canonical, 0, 4) != 0x5336324d || get(canonical, 4, 4) != 1 ||
        get(canonical, 8, 4) != options.count ||
        get(canonical, 12, 4) != options.ticks ||
        get(canonical, 16, 4) != options.workers ||
        get(canonical, 20, 4) != options.grain ||
        get(canonical, 24, 4) != static_cast<std::uint64_t>(options.host) ||
        get(canonical, 28, 4) != static_cast<std::uint64_t>(options.external) ||
        get(canonical, 32, 4) != static_cast<unsigned>(options.campaign) ||
        get(canonical, 56, 8) > options.ticks)
      return false;
    configuration.target = get32(canonical, 40);
    configuration.gain = get32(canonical, 44);
    configuration.calibration = get32(canonical, 48);
    configuration.fault = get32(canonical, 52);
    configuration.generation = get(canonical, 64, 8);
    configuration.applications = get(canonical, 392, 8);
    next_tick = get(canonical, 56, 8);
    const auto read_counts = [&](auto &values, std::size_t offset) {
      for (auto &value : values) {
        value = get(canonical, offset, 8);
        offset += 8;
      }
    };
    read_counts(calls, 72);
    read_counts(publications, 136);
    read_counts(selections, 176);
    read_counts(ages, 216);
    read_counts(generations, 256);
    read_counts(producer_releases, 296);
    read_counts(timestamps, 336);
    stale = get(canonical, 376, 8);
    missing = get(canonical, 384, 8);
    for (std::size_t i = 0; i < 6; ++i)
      sums[i] = std::bit_cast<std::int64_t>(get(canonical, 400 + 8 * i, 8));
    std::size_t offset = state_header_bytes;
    for (auto *vector : {&plant.position, &plant.velocity, &plant.acceleration,
                         &command, &sensor.position, &sensor.velocity})
      for (auto &axis : *vector)
        for (auto &value : axis) {
          value = get32(canonical, offset);
          offset += 4;
        }
    return true;
  }
  bool read(const rt::RateReleaseView &release, std::size_t channel,
            std::uint64_t tick) noexcept {
    auto &f = buffers[channel];
    f.fill(std::byte{});
    rt::CrossRateReadResult result;
    if (release.copy(channels[channel], frame_span(f, channel), result) !=
            rt::CrossRateReadStatus::ok ||
        !valid_frame(f, channel, options.count))
      return false;
    ++selections[channel];
    ages[channel] = result.age_ns;
    generations[channel] = result.generation;
    producer_releases[channel] = result.producer_release_sequence;
    timestamps[channel] = get(f, 48, 8);
    if (result.age_ns > fixed::channel_age[channel] * fixed::tick_ns ||
        result.producer_completion_status != rt::Status::ok)
      return false;
    if (get(f, 48, 8) > tick * fixed::tick_ns)
      return false;
    return true;
  }
  bool publish(const rt::RateReleaseView &release, std::size_t channel,
               std::uint64_t tick) noexcept {
    seal(buffers[channel], channel, tick, ++publications[channel]);
    return release.publish(channels[channel],
                           frame_span(buffers[channel], channel)) ==
           rt::Status::ok;
  }
  struct AxisWork {
    World *world;
    std::size_t axis;
  };
  static rt::TaskResult entities(void *opaque, const rt::TaskContext &ctx,
                                 const rt::TaskRange &range) noexcept {
    auto &work = *static_cast<AxisWork *>(opaque);
    auto &w = *work.world;
    if (ctx.scratch().size() != fixed::scratch_bytes)
      return rt::TaskResult::error;
    std::fill(ctx.scratch().begin(), ctx.scratch().end(), std::byte{0x2a});
    for (auto i = range.begin; i < range.end; ++i) {
      const auto a = work.axis;
      const auto effort = w.command[a][i];
      const auto v = std::int64_t(w.plant.velocity[a][i]) + effort;
      const auto x = std::int64_t(w.plant.position[a][i]) + v;
      if (effort < -4 || effort > 4 || v < -4160 || v > 4160 || x < -2165760 ||
          x > 2165760)
        return rt::TaskResult::error;
      w.plant.acceleration[a][i] = effort;
      w.plant.velocity[a][i] = static_cast<std::int32_t>(v);
      w.plant.position[a][i] = static_cast<std::int32_t>(x);
    }
    return rt::TaskResult::ok;
  }
  static rt::TaskResult axes(void *opaque, const rt::TaskContext &ctx,
                             const rt::TaskRange &range) noexcept {
    auto &w = *static_cast<World *>(opaque);
    if (ctx.scratch().size() != fixed::scratch_bytes)
      return rt::TaskResult::error;
    std::fill(ctx.scratch().begin(), ctx.scratch().end(), std::byte{0x51});
    for (auto a = range.begin; a < range.end; ++a) {
      AxisWork work{&w, a};
      if (ctx.parallel_for(w.options.count, w.options.grain, &entities,
                           &work) != rt::Status::ok)
        return rt::TaskResult::error;
    }
    return std::all_of(ctx.scratch().begin(), ctx.scratch().end(),
                       [](auto b) { return b == std::byte{0x51}; })
               ? rt::TaskResult::ok
               : rt::TaskResult::error;
  }
  bool phase(std::size_t index, const rt::CallbackContext &ctx) noexcept {
    if (!ctx.rate_release || ctx.rate_release->phase.index() != index ||
        ctx.rate_release->logical_release_ns % fixed::tick_ns ||
        !configuration.consume(ctx.live_control))
      return false;
    const auto tick = ctx.rate_release->logical_release_ns / fixed::tick_ns;
    const auto &release = *ctx.rate_release;
    if (tick >= options.ticks ||
        tick % fixed::periods[fixed::phase_rates[index]])
      return false;
    ++calls[index];
    switch (index) {
    case 0:
      if (!read(release, 3, tick))
        return false;
      unpack_vector(buffers[3], command);
      next_tick = tick + 1;
      break;
    case 1:
      if (ctx.tasks.parallel_for(3, 1, &axes, this) != rt::Status::ok)
        return false;
      break;
    case 2:
      for (auto c : {std::size_t{0}, std::size_t{4}}) {
        buffers[c].fill(std::byte{});
        pack_vector(buffers[c], plant.position);
        pack_vector(buffers[c], plant.velocity, 3);
        if (!publish(release, c, tick))
          return false;
      }
      break;
    case 3:
      if (!read(release, 0, tick))
        return false;
      unpack_vector(buffers[0], sensor.position);
      unpack_vector(buffers[0], sensor.velocity, 3);
      for (std::size_t a = 0; a < 3; ++a)
        for (std::size_t i = 0; i < options.count; ++i) {
          sensor.position[a][i] += configuration.calibration;
          sensor.velocity[a][i] += configuration.calibration;
        }
      buffers[1].fill(std::byte{});
      pack_vector(buffers[1], sensor.position);
      pack_vector(buffers[1], sensor.velocity, 3);
      if (!publish(release, 1,
                   (configuration.fault == 2 && tick == 6) ? 2 : tick))
        return false;
      break;
    case 4: {
      if (!read(release, 1, tick))
        return false;
      const bool expired = tick * fixed::tick_ns - get(buffers[1], 48, 8) >
                           fixed::channel_age[1] * fixed::tick_ns;
      if (expired)
        ++stale;
      const bool absent =
          (options.external && !external_ready) || configuration.fault == 10;
      if (absent)
        ++missing;
      buffers[2].fill(std::byte{});
      for (std::size_t a = 0; a < 3; ++a)
        for (std::size_t i = 0; i < options.count; ++i) {
          const auto velocity =
              expired
                  ? 0
                  : get32(buffers[1],
                          header_bytes + 4 * ((3 + a) * fixed::capacity + i));
          const auto value =
              absent
                  ? 0
                  : (options.external ? external_command[a][i]
                                      : effort(velocity, configuration.target,
                                               configuration.gain));
          if (value < -4 || value > 4)
            return false;
          put32(buffers[2], header_bytes + 4 * (a * fixed::capacity + i),
                value);
        }
      if (!publish(release, 2, tick))
        return false;
      break;
    }
    case 5:
      if (!read(release, 2, tick))
        return false;
      buffers[3] = buffers[2];
      if (!publish(release, 3, tick))
        return false;
      break;
    case 6:
      if (!read(release, 4, tick))
        return false;
      sums.fill(0);
      for (std::size_t a = 0; a < 6; ++a)
        for (std::size_t i = 0; i < options.count; ++i)
          sums[a] +=
              get32(buffers[4], header_bytes + 4 * (a * fixed::capacity + i));
      break;
    case 7:
      break;
    default:
      return false;
    }
    encode();
    return true;
  }
};
static_assert(offsetof(World, plant) == 64);
static_assert(offsetof(World, external_ready) + 1 + 39 == sizeof(World));
struct Binding {
  World *world;
  std::size_t phase;
};
inline rt::CallbackResult invoke(void *opaque,
                                 const rt::CallbackContext &ctx) noexcept {
  auto &b = *static_cast<Binding *>(opaque);
  return b.world->phase(b.phase, ctx) ? rt::CallbackResult::ok
                                      : rt::CallbackResult::error;
}
} // namespace golden
