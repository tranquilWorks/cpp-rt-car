#pragma once
#include "session.hpp"
namespace golden::xdma {
// Derived from golden_system/replay.hpp; same public artifact construction.
// The owned XDMA clock is explicitly correlated at each replayed frame.
struct Replay {
  std::vector<std::byte> initial, active, trusted;
  std::vector<rt::ReplayInputRecord> inputs;
  std::vector<std::array<std::byte, 3073>> external;
  std::size_t first_tick = 0;
  rt::LiveControlReplayResult result{};
  explicit Replay(std::size_t ticks, bool peer = false, std::size_t first = 0)
      : inputs(ticks - first), external(peer ? ticks - first : 0),
        first_tick(first) {}
  bool begin(Session &s) {
    initial = s.checkpoint(0);
    return !initial.empty();
  }
  void record(const Session &s, std::size_t tick) {
    const auto i = tick - first_tick;
    inputs[i] = {s.frame(tick), 2601, {}};
    if (!external.empty()) {
      auto &bytes = external[i];
      bytes[0] = std::byte(s.world.external_ready ? 1 : 0);
      std::size_t at = 1;
      for (const auto &axis : s.world.external_command)
        for (auto value : axis) {
          put32(bytes, at, value);
          at += 4;
        }
      inputs[i].payload = bytes;
    }
  }
  static rt::CallbackResult input(void *opaque,
                                  const rt::ReplayInputView &in) noexcept {
    auto &s = *static_cast<Session *>(opaque);
    if (in.input_type != 2601 || !in.frame.nominal_release_ns ||
        !s.world.decode())
      return rt::CallbackResult::error;
    if (s.world.options.external) {
      if (in.payload.size() != 3073 ||
          std::to_integer<unsigned>(in.payload[0]) > 1)
        return rt::CallbackResult::error;
      s.world.external_ready = in.payload[0] == std::byte{1};
      std::size_t at = 1;
      for (auto &axis : s.world.external_command)
        for (std::size_t i = 0; i < fixed::capacity; ++i) {
          const auto value = get32(in.payload, at);
          at += 4;
          if (value < -4 || value > 4 || (i >= s.world.options.count && value))
            return rt::CallbackResult::error;
          axis[i] = value;
        }
    } else if (!in.payload.empty())
      return rt::CallbackResult::error;
    s.time(*in.frame.nominal_release_ns);
    return rt::CallbackResult::ok;
  }
  bool seal(Session &s) {
    rt::ArtifactWriteResult write;
    auto status =
        s.runtime->write_active_replay_artifact(initial, inputs, {}, write);
    if (status != rt::Status::capacity_exceeded || !write.required_bytes ||
        write.required_bytes > fixed::artifact_bytes)
      return false;
    active.resize(write.required_bytes);
    if (s.runtime->write_active_replay_artifact(initial, inputs, active,
                                                write) != rt::Status::ok)
      return false;
    status = s.runtime->write_live_control_replay_artifact(
        initial, active, rt::LiveControlNestedArtifactKind::active_replay, {},
        write);
    if (status != rt::Status::capacity_exceeded || !write.required_bytes ||
        write.required_bytes > fixed::artifact_bytes)
      return false;
    trusted.resize(write.required_bytes);
    return s.runtime->write_live_control_replay_artifact(
               initial, active,
               rt::LiveControlNestedArtifactKind::active_replay, trusted,
               write) == rt::Status::ok;
  }
  static rt::Status
  apply(Session &s, std::span<const std::byte> artifact,
        rt::LiveControlReplayResult *result = nullptr) noexcept {
    rt::LiveControlReplayMetadata envelope;
    auto status = rt::inspect_live_control_replay_artifact(artifact, envelope);
    if (status != rt::Status::ok)
      return status;
    rt::LiveControlActionMetadata owner;
    status = s.runtime->live_control_action_metadata(owner);
    if (status != rt::Status::ok)
      return status;
    if (envelope.runtime_id != owner.runtime_id)
      return rt::Status::incompatible_artifact;
    return s.runtime->replay_live_control(artifact, input, &s, result);
  }
  bool verify(Session &s) noexcept {
    const auto expected = s.world.canonical;
    auto status = apply(s, trusted, &result);
    return status == rt::Status::ok &&
           result.frames_replayed == inputs.size() &&
           result.mismatch_status == rt::Status::ok &&
           s.world.canonical == expected && s.world.decode();
  }
};
} // namespace golden::xdma
