#pragma once
#include "../golden_system/world.hpp"
namespace golden::xdma {
// Derived from the frozen CPU oracle, adding only missing-command zero effort.
// Serial reference uses a closed-form segment for every constant acceleration.
// It never calls the production integrator or controller and checks all 256
// lanes.
class Oracle {
  Options options_;
  bool underflow_;
  Vector origin_x_{}, origin_v_{}, accel_{}, actuator_{}, sample_x_{},
      sample_v_{};
  std::array<std::array<std::uint64_t, fixed::capacity>, 3> lengths_{};
  Vector x_{}, v_{};
  std::array<std::int64_t, 6> sums_{};
  std::int32_t target_ = 8, gain_ = 1, calibration_ = 0;
  std::uint64_t stale_ = 0, missing_ = 0;

public:
  explicit Oracle(Options o, bool underflow = false)
      : options_(o), underflow_(underflow) {
    std::uint64_t seed = 1;
    const auto draw = [&](std::uint64_t modulus, int offset) {
      seed = (seed * 1664525 + 1013904223) & 0xffffffffULL;
      return static_cast<std::int32_t>(seed % modulus) - offset;
    };
    for (std::size_t i = 0; i < o.count; ++i)
      for (std::size_t a = 0; a < 3; ++a) {
        x_[a][i] = draw(2049, 1024);
        v_[a][i] = draw(129, 64);
        accel_[a][i] = draw(9, 4);
      }
    origin_x_ = x_;
    origin_v_ = v_;
    actuator_ = accel_;
  }
  bool step(std::size_t tick, const World &actual) noexcept {
    if (tick == 1)
      target_ = 16;
    if (tick == 2)
      calibration_ = 2;
    if (tick == 3)
      gain_ = 2;
    if (tick == 6 && options_.campaign == Campaign::control_replaced)
      gain_ = 3;
    const Vector old_sensor = sample_v_;
    const auto command = actuator_;
    for (std::size_t a = 0; a < 3; ++a)
      for (std::size_t i = 0; i < options_.count; ++i) {
        if (command[a][i] != accel_[a][i]) {
          origin_x_[a][i] = x_[a][i];
          origin_v_[a][i] = v_[a][i];
          accel_[a][i] = command[a][i];
          lengths_[a][i] = 0;
        }
        const auto n = static_cast<std::int64_t>(++lengths_[a][i]);
        v_[a][i] =
            static_cast<std::int32_t>(origin_v_[a][i] + n * accel_[a][i]);
        x_[a][i] =
            static_cast<std::int32_t>(origin_x_[a][i] + n * origin_v_[a][i] +
                                      accel_[a][i] * n * (n + 1) / 2);
      }
    if (tick % 2 == 0)
      for (std::size_t a = 0; a < 3; ++a)
        for (std::size_t i = 0; i < options_.count; ++i) {
          sample_x_[a][i] = x_[a][i] + calibration_;
          sample_v_[a][i] = v_[a][i] + calibration_;
        }
    if (tick % 3 == 0) {
      const bool stale =
          options_.campaign == Campaign::stale_input && tick == 6;
      const bool absent = (options_.campaign == Campaign::peer_missing &&
                           tick >= 6 && tick < 12) ||
                          (options_.external && !actual.external_ready);
      stale_ += stale ? 1 : 0;
      missing_ += absent ? 1 : 0;
      for (std::size_t a = 0; a < 3; ++a)
        for (std::size_t i = 0; i < options_.count; ++i) {
          const auto velocity =
              stale ? 0
                    : (options_.external ? old_sensor[a][i] : sample_v_[a][i]);
          const auto demand =
              static_cast<std::int64_t>(gain_) * (target_ - velocity);
          actuator_[a][i] =
              (absent || (underflow_ && tick >= 6 && tick < 12))
                  ? 0
                  : static_cast<std::int32_t>(
                        demand < -4 ? -4 : (demand > 4 ? 4 : demand));
        }
    }
    if (tick % 6 == 0) {
      sums_.fill(0);
      for (std::size_t a = 0; a < 3; ++a)
        for (std::size_t i = 0; i < options_.count; ++i) {
          sums_[a] += x_[a][i];
          sums_[a + 3] += v_[a][i];
        }
    }
    if (actual.plant.position != x_ || actual.plant.velocity != v_ ||
        actual.plant.acceleration != accel_ || actual.command != command ||
        actual.sensor.position != sample_x_ ||
        actual.sensor.velocity != sample_v_ || actual.sums != sums_ ||
        actual.stale != stale_ || actual.missing != missing_)
      return false;
    if (actual.configuration.target != target_ ||
        actual.configuration.gain != gain_ ||
        actual.configuration.calibration != calibration_)
      return false;
    constexpr std::array<std::size_t, 8> phase_periods{1, 1, 1, 2, 3, 3, 6, 6};
    for (std::size_t i = 0; i < 8; ++i)
      if (actual.calls[i] != tick / phase_periods[i] + 1)
        return false;
    std::array<std::uint64_t, 5> publications{
        tick + 1, tick / 2 + 1, tick / 3 + 1, tick / 3 + 1, tick + 1};
    if (underflow_ && tick >= 6)
      publications[2] -= std::min<std::size_t>(2, tick / 3 - 1);
    const std::array<std::uint64_t, 5> selections{
        tick / 2 + 1, tick / 3 + 1, tick / 3 + 1, tick + 1, tick / 6 + 1};
    const std::array<std::uint64_t, 5> ages{
        0, (tick / 3 * 3 % 2) * fixed::tick_ns, 0,
        tick ? ((tick - 1) % 3 + 1) * fixed::tick_ns : 0, 0};
    const std::array<std::uint64_t, 5> generations{
        tick / 2 * 2 + 2, tick / 3 * 3 / 2 + 2, tick / 3 + 2,
        tick ? (tick - 1) / 3 + 2 : 1, tick / 6 * 6 + 2};
    return actual.publications == publications &&
           actual.selections == selections && actual.ages == ages &&
           actual.generations == generations;
  }
};
} // namespace golden::xdma
