#pragma once
#include "../golden_system/replay.hpp"
namespace golden::cuda {
// This kit supports originating-owner active/trusted replay. Check the public
// envelope before calling Runtime; compatible checkpoint restore is separate.
struct OwnedReplay : golden::Replay {
  using golden::Replay::Replay;
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
    return apply(s, trusted, &result) == rt::Status::ok &&
           result.frames_replayed == inputs.size() &&
           result.mismatch_status == rt::Status::ok &&
           s.world.canonical == expected && s.world.decode();
  }
};
} // namespace golden::cuda
