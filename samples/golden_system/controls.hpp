#pragma once
#include "model.hpp"
#include <rt/live_control.hpp>
namespace golden {
template <unsigned Kind> struct Control { std::int32_t value = 0; };
struct Configuration {
  std::int32_t target = 8, gain = 1, calibration = 0, fault = 0;
  std::uint64_t generation = 0, applications = 0;
  bool consume(const rt::LiveControlGenerationView *view) noexcept;
};
} // namespace golden
namespace rt {
template <unsigned Kind> struct LiveControlTypeTraits<golden::Control<Kind>> {
  static_assert(Kind >= 1 && Kind <= 5);
  static constexpr std::uint32_t application_type_identity = 2600 + Kind,
                                 application_schema_version = 1;
  static constexpr auto update_kind = static_cast<LiveControlUpdateKind>(Kind);
  static constexpr std::size_t encoded_extent = 16;
  static bool validate(const golden::Control<Kind> &x) noexcept {
    constexpr std::array<int, 5> low{-64, 0, -16, 0, 0}, high{64, 4, 16, 11, 0};
    return x.value >= low[Kind - 1] && x.value <= high[Kind - 1];
  }
  static bool encode(const golden::Control<Kind> &x,
                     std::span<std::byte, 16> out) noexcept {
    if (!validate(x))
      return false;
    std::fill(out.begin(), out.end(), std::byte{});
    golden::put32(out, 0, x.value);
    return true;
  }
  static bool decode(std::span<const std::byte, 16> in,
                     golden::Control<Kind> &out) noexcept {
    for (std::size_t i = 4; i < in.size(); ++i)
      if (in[i] != std::byte{})
        return false;
    golden::Control<Kind> candidate{golden::get32(in, 0)};
    if (!validate(candidate))
      return false;
    out = candidate;
    return true;
  }
};
} // namespace rt
namespace golden {
template <unsigned Kind>
inline bool apply(const rt::LiveControlRecordView &record,
                  std::int32_t &out) noexcept {
  Control<Kind> value;
  if (rt::decode_live_control_typed_payload(record.record, record.payload,
                                            value) !=
      rt::LiveControlTypedStatus::ok)
    return false;
  out = value.value;
  return true;
}
inline bool
Configuration::consume(const rt::LiveControlGenerationView *view) noexcept {
  if (!view || !view->generation_identity ||
      view->generation_identity == generation)
    return true;
  Configuration next = *this;
  for (const auto &record : view->records) {
    bool ok = false;
    switch (record.record.update_kind) {
    case rt::LiveControlUpdateKind::scenario_parameters:
      ok = apply<1>(record, next.target);
      break;
    case rt::LiveControlUpdateKind::controller_parameters:
      ok = apply<2>(record, next.gain);
      break;
    case rt::LiveControlUpdateKind::sensor_calibration:
      ok = apply<3>(record, next.calibration);
      break;
    case rt::LiveControlUpdateKind::fault_configuration:
      ok = apply<4>(record, next.fault);
      break;
    case rt::LiveControlUpdateKind::clear_fault: {
      std::int32_t zero = 0;
      ok = apply<5>(record, zero);
      next.fault = 0;
      break;
    }
    }
    if (!ok)
      return false;
    ++next.applications;
  }
  next.generation = view->generation_identity;
  *this = next;
  return true;
}
} // namespace golden
