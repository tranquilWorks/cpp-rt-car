#pragma once
#include "codec.hpp"
#include <rt/xdma_backend.hpp>

namespace golden::xdma {
// Fixed card model: only Driver API transfers move bytes across its boundary.
// Each lane permits one in-flight ordered batch. Runtime/backend own
// scheduling.
class SimulatedDriver {
  struct Lane {
    Frame input{}, output{};
    std::uint32_t control = 0;
    unsigned stage = 0;
  };
  std::array<Lane, 2> lanes_{};
  std::size_t count_;

public:
  std::atomic<std::uint64_t> now{clock_origin}, uploads{0}, downloads{0},
      controls{0}, events{0}, safe_acks{0}, initializes{0}, shutdowns{0};
  std::atomic<bool> live{false};
  explicit SimulatedDriver(std::size_t count) : count_(count) {}
  rt::XdmaDriverApi api() noexcept {
    rt::XdmaDriverApi a;
    a.struct_size = sizeof(a);
    a.api_version = rt::xdma_driver_api_version_2;
    a.user_data = this;
    a.initialize = [](void *p) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      ++s.initializes;
      s.live = true;
      return rt::XdmaDriverResult::success;
    };
    a.shutdown = [](void *p) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      if (s.live.exchange(false))
        ++s.shutdowns;
      return rt::XdmaDriverResult::success;
    };
    a.reset = [](void *p) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      for (auto &lane : s.lanes_)
        lane = {};
      return rt::XdmaDriverResult::success;
    };
    a.request_stop = [](void *) noexcept {
      return rt::XdmaDriverResult::success;
    };
    a.monotonic_time_ns = [](void *p) noexcept {
      return static_cast<SimulatedDriver *>(p)->now.load();
    };
    a.transfer = [](void *p, rt::XdmaDirection direction, std::uint32_t channel,
                    std::uint64_t offset, void *host,
                    std::uint64_t bytes) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      const auto bad =
          rt::XdmaTransferResult{rt::XdmaDriverResult::invalid_value, 0};
      if (!s.live || channel >= 2 || !host || bytes != frame_size(channel * 2))
        return bad;
      auto &lane = s.lanes_[channel];
      if (direction == rt::XdmaDirection::host_to_card) {
        if (offset == 0) {
          lane.stage = 1;
          std::memcpy(lane.input.data(), host, bytes);
        } else if (offset == 8192 && lane.stage == 1) {
          lane.stage = 2;
          std::memcpy(lane.output.data(), host, bytes);
        } else
          return bad;
        ++s.uploads;
      } else if (direction == rt::XdmaDirection::card_to_host &&
                 offset == 8192 && lane.stage == 4) {
        std::memcpy(host, lane.output.data(), bytes);
        lane.stage = 0;
        ++s.downloads;
      } else
        return bad;
      return rt::XdmaTransferResult{rt::XdmaDriverResult::success, bytes};
    };
    a.control_write32 = [](void *p, std::uint32_t offset,
                           std::uint32_t value) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      if (offset >= 8 || offset % 4 || s.lanes_[offset / 4].stage != 2)
        return rt::XdmaDriverResult::invalid_value;
      auto &lane = s.lanes_[offset / 4];
      lane.control = value;
      lane.stage = 3;
      ++s.controls;
      return rt::XdmaDriverResult::success;
    };
    a.control_read32 = [](void *, std::uint32_t) noexcept {
      return rt::XdmaControlReadResult{rt::XdmaDriverResult::invalid_value, 0};
    };
    a.wait_user_event = [](void *p, std::uint32_t index,
                           std::uint64_t timeout) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      if (index >= 2 || !timeout || s.lanes_[index].stage != 3)
        return rt::XdmaUserEventResult{rt::XdmaDriverResult::invalid_value, 0};
      auto &lane = s.lanes_[index];
      rt::SampledIoFrameHeader in, out;
      std::memcpy(&in, lane.input.data(), sizeof(in));
      std::memcpy(&out, lane.output.data(), sizeof(out));
      const bool safe = in.status == static_cast<std::uint32_t>(
                                         rt::SampledIoFrameStatus::safe);
      const auto c = index * 2;
      if (in.channel_identity != 26001 + c ||
          in.payload_checksum !=
              rt::sampled_io_payload_checksum(
                  frame_span(lane.input, c).subspan(header_bytes)))
        return rt::XdmaUserEventResult{rt::XdmaDriverResult::io_error, 0};
      for (std::size_t field = 0; field < fixed::channel_elements[c]; ++field)
        for (std::size_t i = 0; i < fixed::capacity; ++i) {
          const auto at = header_bytes + 4 * (field * fixed::capacity + i);
          auto value = safe ? 0 : get32(lane.input, at);
          if (!safe && index == 0 && i < s.count_)
            value += std::bit_cast<std::int32_t>(lane.control);
          put32(lane.output, at, value);
        }
      if (safe) {
        ++s.safe_acks;
      } else {
        out.first_sample_timestamp = s.now.load();
        out.payload_checksum = rt::sampled_io_payload_checksum(
            frame_span(lane.output, c + 1).subspan(header_bytes));
        std::memcpy(lane.output.data(), &out, sizeof(out));
      }
      lane.stage = 4;
      ++s.events;
      return rt::XdmaUserEventResult{rt::XdmaDriverResult::success, 1};
    };
    return a;
  }
};
} // namespace golden::xdma
