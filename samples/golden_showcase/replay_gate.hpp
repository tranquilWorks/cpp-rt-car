#pragma once
#include "replay.hpp"
namespace golden::showcase {
// Off-lane source-kit policy. Validate observation continuity and originating
// owner before invoking Runtime; no foreign artifact identity is rewritten.
inline rt::Status checked_replay(Session &owner, Replay &replay,
                                  std::uint64_t gaps,
                                  std::uint64_t &runtime_calls) {
  if (gaps) return rt::Status::resource_exhausted;
  rt::RateTelemetryMetadata metadata;
  if (replay.trusted.size() < 400 ||
      owner.runtime->rate_telemetry_metadata(metadata) != rt::Status::ok ||
      get(replay.trusted,32,8) != metadata.runtime_id)
    return rt::Status::invalid_argument;
  ++runtime_calls;
  return replay.verify(owner) ? rt::Status::ok : rt::Status::invalid_state;
}
}
