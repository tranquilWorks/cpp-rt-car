#pragma once
#include "../golden_system/world.hpp"
#include <cstring>

namespace golden::xdma {
inline constexpr std::uint64_t clock_origin = 1000;
inline constexpr std::uint64_t completion_ns = 500'000;
inline constexpr std::uint64_t safe_timeout_ns = 8'000'000;
// Both formats have 120-byte headers, but different checksum seeds, sequence
// origins, generation, status and timestamp semantics. Never reinterpret one
// as the other or edit a Runtime replay artifact to make it fit.
inline void sampled_header(Frame &f, std::size_t channel,
                           std::uint64_t sequence, std::uint64_t generation,
                           std::uint64_t timestamp,
                           rt::SampledIoFrameStatus status) noexcept {
  rt::SampledIoFrameHeader h;
  h.channel_identity = 26001 + channel;
  h.sequence = sequence;
  h.release_generation = generation;
  h.sample_count = 1;
  h.encoding =
      static_cast<std::uint32_t>(rt::SampledIoEncoding::signed_int32_le);
  h.timestamp_domain_identity = 1;
  h.first_sample_timestamp = timestamp;
  h.sample_interval_ns = fixed::tick_ns;
  h.trigger_identity = h.calibration_identity = 1;
  h.trigger_sequence = sequence;
  h.status = static_cast<std::uint32_t>(status);
  h.payload_checksum = rt::sampled_io_payload_checksum(
      frame_span(f, channel).subspan(header_bytes));
  std::memcpy(f.data(), &h, sizeof(h));
}
inline bool application_frame(const Frame &sampled, Frame &application,
                              std::size_t channel, std::size_t count) noexcept {
  rt::SampledIoFrameHeader h;
  std::memcpy(&h, sampled.data(), sizeof(h));
  if (h.struct_size != 120 || h.version != 1 ||
      h.channel_identity != 26001 + channel || !h.sequence ||
      h.sample_count != 1 || h.encoding != 3 ||
      h.timestamp_domain_identity != 1 ||
      h.sample_interval_ns != fixed::tick_ns || h.trigger_identity != 1 ||
      h.calibration_identity != 1 || h.reserved0 || h.reserved[0] ||
      h.reserved[1] || h.status < 1 || h.status > 3 ||
      h.payload_checksum !=
          rt::sampled_io_payload_checksum(
              frame_span(sampled, channel).subspan(header_bytes)))
    return false;
  const auto logical = h.first_sample_timestamp >= clock_origin
                           ? h.first_sample_timestamp - clock_origin
                           : 0;
  if (logical % fixed::tick_ns)
    return false;
  application = sampled;
  // Application-only codec. Real channel/action headers are retained unchanged.
  seal(application, channel, logical / fixed::tick_ns,
       h.status == 2 ? h.release_generation : 1);
  return valid_frame(application, channel, count);
}
} // namespace golden::xdma
