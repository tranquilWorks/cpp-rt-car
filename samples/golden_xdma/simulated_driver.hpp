#pragma once
#include "codec.hpp"
#include <chrono>
#include <rt/xdma_backend.hpp>
#include <thread>

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
  enum class Fault {
    none,
    short_transfer,
    transfer_timeout,
    device_loss,
    reset_required,
    stale_sequence,
    stale_timestamp,
    payload_corrupt,
    header_corrupt,
    delayed,
    missing_ack
  };
  std::atomic<Fault> fault{Fault::none};
  std::atomic<unsigned> fault_lane{1};
  std::atomic<bool> fail_initialize{false}, fail_shutdown{false},
      fail_reset{false};
  std::atomic<std::uint64_t> faults{0}, resets{0}, delayed_events{0};
  std::atomic<bool> live{false};
  // Explicit bounded protocol-test controls, never selected by the CLI.
  std::atomic<bool> fail_after_acquire{false}, hold_event{false},
      event_waiting{false};
  std::atomic<std::uint64_t> stop_requests{0};
  explicit SimulatedDriver(std::size_t count) : count_(count) {}
  rt::XdmaDriverApi api() noexcept {
    rt::XdmaDriverApi a;
    a.struct_size = sizeof(a);
    a.api_version = rt::xdma_driver_api_version_2;
    a.user_data = this;
    a.initialize = [](void *p) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      if (s.fail_initialize.exchange(false))
        return rt::XdmaDriverResult::io_error;
      ++s.initializes;
      s.live = true;
      if (s.fail_after_acquire.exchange(false))
        return rt::XdmaDriverResult::io_error;
      return rt::XdmaDriverResult::success;
    };
    a.shutdown = [](void *p) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      if (s.fail_shutdown.exchange(false))
        return rt::XdmaDriverResult::io_error;
      if (s.live.exchange(false))
        ++s.shutdowns;
      return rt::XdmaDriverResult::success;
    };
    a.reset = [](void *p) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      if (s.fail_reset.exchange(false))
        return rt::XdmaDriverResult::io_error;
      ++s.resets;
      for (auto &lane : s.lanes_)
        lane = {};
      return rt::XdmaDriverResult::success;
    };
    a.request_stop = [](void *p) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      ++s.stop_requests;
      s.hold_event = false;
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
      const auto injected = s.fault.load();
      if (channel == s.fault_lane &&
          direction == rt::XdmaDirection::host_to_card && offset == 0) {
        auto result = rt::XdmaDriverResult::success;
        if (injected == Fault::short_transfer) {
          s.fault = Fault::none;
          ++s.faults;
          return rt::XdmaTransferResult{result, bytes - 1};
        }
        if (injected == Fault::transfer_timeout)
          result = rt::XdmaDriverResult::timeout;
        if (injected == Fault::device_loss)
          result = rt::XdmaDriverResult::device_lost;
        if (injected == Fault::reset_required)
          result = rt::XdmaDriverResult::reset_required;
        if (result != rt::XdmaDriverResult::success) {
          s.fault = Fault::none;
          ++s.faults;
          return rt::XdmaTransferResult{result, 0};
        }
      }
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
      if (s.hold_event.load()) {
        s.event_waiting = true;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (s.hold_event.load() &&
               std::chrono::steady_clock::now() < deadline)
          std::this_thread::yield();
        s.event_waiting = false;
        if (s.hold_event.load())
          return rt::XdmaUserEventResult{rt::XdmaDriverResult::timeout, 0};
      }
      auto &lane = s.lanes_[index];
      rt::SampledIoFrameHeader in, out;
      std::memcpy(&in, lane.input.data(), sizeof(in));
      std::memcpy(&out, lane.output.data(), sizeof(out));
      const bool safe = in.status == static_cast<std::uint32_t>(
                                         rt::SampledIoFrameStatus::safe);
      const auto c = index * 2;
      const auto injected = s.fault.load();
      if (index == s.fault_lane && injected == Fault::missing_ack) {
        ++s.faults;
        return rt::XdmaUserEventResult{rt::XdmaDriverResult::timeout, 0};
      }
      if (index == s.fault_lane && injected == Fault::delayed) {
        // Bounded driver work; completion remains pending until the worker
        // returns. No clock-domain or logical deadline is changed.
        for (unsigned i = 0; i < 64; ++i)
          std::this_thread::yield();
        ++s.delayed_events;
        s.fault = Fault::none;
      }
      if (in.channel_identity != 26001 + c ||
          in.payload_checksum !=
              rt::sampled_io_payload_checksum(
                  frame_span(lane.input, c).subspan(header_bytes)))
        return rt::XdmaUserEventResult{rt::XdmaDriverResult::io_error, 0};
      // The card echo is a byte transfer; only active sensor lanes require
      // calibration arithmetic. Keep every inactive byte and both checksum
      // validations, while avoiding scalar codec work for a plain copy/zero.
      const auto payload_bytes = frame_size(c) - header_bytes;
      if (safe)
        std::memset(lane.output.data() + header_bytes, 0, payload_bytes);
      else {
        std::memcpy(lane.output.data() + header_bytes,
                    lane.input.data() + header_bytes, payload_bytes);
        if (index == 0)
          for (std::size_t field = 0; field < fixed::channel_elements[c];
               ++field)
            for (std::size_t i = 0; i < s.count_; ++i) {
              const auto at = header_bytes + 4 * (field * fixed::capacity + i);
              put32(lane.output, at,
                    get32(lane.input, at) +
                        std::bit_cast<std::int32_t>(lane.control));
            }
      }
      if (safe)
        ++s.safe_acks;
      // A substituted command is still a real produced device frame.
      // Lifecycle safe transitions discard this output after the event ACK.
      out.first_sample_timestamp = s.now.load();
      out.payload_checksum = rt::sampled_io_payload_checksum(
          frame_span(lane.output, c + 1).subspan(header_bytes));
      if (index == s.fault_lane && !safe) {
        if (injected == Fault::stale_sequence) {
          --out.sequence;
          --out.trigger_sequence;
        }
        if (injected == Fault::stale_timestamp)
          out.first_sample_timestamp = 0;
        if (injected == Fault::header_corrupt)
          out.reserved0 = 1;
        if (injected == Fault::payload_corrupt)
          lane.output[header_bytes] ^= std::byte{1};
        if (injected == Fault::stale_sequence ||
            injected == Fault::stale_timestamp ||
            injected == Fault::header_corrupt ||
            injected == Fault::payload_corrupt) {
          ++s.faults;
          s.fault = Fault::none;
        }
      }
      std::memcpy(lane.output.data(), &out, sizeof(out));
      lane.stage = 4;
      ++s.events;
      return rt::XdmaUserEventResult{rt::XdmaDriverResult::success, 1};
    };
    return a;
  }
};
} // namespace golden::xdma
