#pragma once
// M26-05 bounded CPU phase experiments. Numerical/codec operations are derived
// from golden_system/world.hpp; this is not a replacement golden-loop oracle.
#include "../golden_system/session.hpp"
#include <numeric>
namespace golden::showcase {
struct Probe {
  Options options;
  std::size_t kind;
  std::array<std::byte,64-sizeof(Options)-sizeof(std::size_t)> options_padding{};
  Plant plant;
  Vector command{};
  Sensor sensor{};
  Frame frame{}, output{};
  std::array<std::int64_t, 6> sums{};
  std::uint64_t calls = 0, checks = 0;
  std::array<std::byte,16> storage_padding{};
  explicit Probe(Options o, std::size_t k)
      : options(o), kind(k), plant(initial(o.count)), command(plant.acceleration) {
    pack_vector(frame, plant.position);
    pack_vector(frame, plant.velocity, 3);
    seal(frame, 0, 0, 1);
  }
  static rt::TaskResult integrate(void *opaque, const rt::TaskContext &ctx,
                                   const rt::TaskRange &range) noexcept {
    auto &p = *static_cast<Probe *>(opaque);
    if (ctx.scratch().size() != fixed::scratch_bytes)
      return rt::TaskResult::error;
    std::fill(ctx.scratch().begin(), ctx.scratch().end(), std::byte{0x26});
    for (std::size_t i = range.begin; i < range.end; ++i)
      for (std::size_t a = 0; a < 3; ++a) {
        p.plant.velocity[a][i] += p.command[a][i];
        p.plant.position[a][i] += p.plant.velocity[a][i];
      }
    return rt::TaskResult::ok;
  }
  static rt::CallbackResult invoke(void *opaque, const rt::CallbackContext &ctx) noexcept {
    auto &p = *static_cast<Probe *>(opaque);
    ++p.calls;
    switch (p.kind) {
    case 0: // Seeded scenario input, full SoA initialization and command copy.
      p.plant = initial(p.options.count);
      p.command = p.plant.acceleration;
      break;
    case 1: // Public task dispatch with the frozen integer integrator.
      if (ctx.tasks.parallel_for(p.options.count, p.options.grain, integrate, &p) != rt::Status::ok)
        return rt::CallbackResult::error;
      break;
    case 2: // Explicit CPU host staging of both full-capacity fields.
      p.output.fill(std::byte{});
      pack_vector(p.output, p.plant.position);
      pack_vector(p.output, p.plant.velocity, 3);
      seal(p.output, 0, p.calls - 1, p.calls);
      break;
    case 3: // Sample-private input decode and calibration, no HAL claim.
      if (!valid_frame(p.frame, 0, p.options.count)) return rt::CallbackResult::error;
      unpack_vector(p.frame, p.sensor.position);
      unpack_vector(p.frame, p.sensor.velocity, 3);
      for (std::size_t a = 0; a < 3; ++a)
        for (std::size_t i = 0; i < p.options.count; ++i) {
          p.sensor.position[a][i] += 2;
          p.sensor.velocity[a][i] += 2;
        }
      break;
    case 4: // Saturating local control, all inactive lanes remain zero.
      for (std::size_t a = 0; a < 3; ++a)
        for (std::size_t i = 0; i < p.options.count; ++i)
          p.command[a][i] = effort(p.plant.velocity[a][i], 16, 2);
      break;
    case 5: // Actuator application-frame encoding, no safe-ACK claim.
      p.output.fill(std::byte{});
      pack_vector(p.output, p.command);
      seal(p.output, 3, p.calls - 1, p.calls);
      break;
    case 6: // Reduction of the full active SoA into six checked sums.
      p.sums.fill(0);
      for (std::size_t a = 0; a < 3; ++a)
        for (std::size_t i = 0; i < p.options.count; ++i) {
          p.sums[a] += p.plant.position[a][i];
          p.sums[a + 3] += p.plant.velocity[a][i];
        }
      break;
    default: return rt::CallbackResult::error;
    }
    return rt::CallbackResult::ok;
  }
  bool verify() noexcept {
    // Independent seed and closed-form reference; no calls to initial/effort
    // or the work callback. Compare active and inactive lanes on every step.
    Plant reference{};
    std::uint64_t seed = 1;
    const auto draw = [&](std::uint64_t mod, std::int32_t offset) {
      seed = (seed * 1664525 + 1013904223) & 0xffffffffULL;
      return static_cast<std::int32_t>(seed % mod) - offset;
    };
    for (std::size_t i = 0; i < options.count; ++i)
      for (std::size_t a = 0; a < 3; ++a) {
        reference.position[a][i] = draw(2049, 1024);
        reference.velocity[a][i] = draw(129, 64);
        reference.acceleration[a][i] = draw(9, 4);
      }
    std::array<std::int64_t, 6> expected_sums{};
    for (std::size_t a = 0; a < 3; ++a)
      for (std::size_t i = 0; i < fixed::capacity; ++i) {
        const auto x = reference.position[a][i], v = reference.velocity[a][i], f = reference.acceleration[a][i];
        const auto n = static_cast<std::int64_t>(calls);
        const auto expected_x = kind == 1 ? x + n * v + f * n * (n + 1) / 2 : x;
        const auto expected_v = kind == 1 ? v + n * f : v;
        if (plant.position[a][i] != expected_x || plant.velocity[a][i] != expected_v || plant.acceleration[a][i] != f)
          return false;
        const auto demand = std::int64_t{2} * (16 - v);
        const auto control = i >= options.count ? 0 : (demand < -4 ? -4 : demand > 4 ? 4 : demand);
        if (command[a][i] != (kind == 4 ? control : f)) return false;
        if (kind == 3 && (sensor.position[a][i] != x + (i < options.count ? 2 : 0) ||
                         sensor.velocity[a][i] != v + (i < options.count ? 2 : 0))) return false;
        expected_sums[a] += x;
        expected_sums[a + 3] += v;
        if (kind == 2 && (get32(output, header_bytes + 4 * (a * fixed::capacity + i)) != x ||
                         get32(output, header_bytes + 4 * ((a + 3) * fixed::capacity + i)) != v)) return false;
        if (kind == 5 && get32(output, header_bytes + 4 * (a * fixed::capacity + i)) != f) return false;
        ++checks;
      }
    if (kind == 6 && sums != expected_sums) return false;
    if ((kind == 2 || kind == 5) &&
        (!valid_frame(output, kind == 2 ? 0 : 3, options.count) ||
         get(output, 16, 8) != calls || get(output, 48, 8) != (calls - 1) * fixed::tick_ns)) return false;
    return true;
  }
};
static_assert(offsetof(Probe,plant)==64);
static_assert(offsetof(Probe,storage_padding)+16==sizeof(Probe));
} // namespace golden::showcase
