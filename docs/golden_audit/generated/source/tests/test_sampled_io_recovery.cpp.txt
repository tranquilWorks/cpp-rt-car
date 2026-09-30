#include "rt/src/snapshot_codec.hpp"
#include "sampled_io_recovery/fixture.hpp"
#include <future>
#include <gtest/gtest.h>
namespace {
using rt::Status;
using sampled_recovery::Fixture;
void ready(Fixture &f) {
  ASSERT_EQ(f.configure(), Status::ok) << f.runtime.last_error();
  ASSERT_EQ(f.start(), Status::ok) << f.runtime.last_error();
}
std::vector<std::byte>
corrupt_produced_frame(const std::vector<std::byte> &original, unsigned field) {
  rt::CheckpointMetadata metadata;
  if (rt::inspect_checkpoint_artifact(original, metadata) != Status::ok)
    return {};
  rt::detail::CheckpointRecordCursor cursor;
  std::vector<rt::detail::StateWriteView> records;
  std::vector<std::byte> changed;
  for (unsigned i = 0; i < metadata.state_count; ++i) {
    rt::detail::CheckpointRecordView r;
    if (!rt::detail::next_checkpoint_record(original, metadata, cursor, r))
      return {};
    if (r.name == "rtfw.rate-dispatch") {
      changed.assign(r.payload.begin(), r.payload.end());
      // Three frozen channel records; sampled device producer is channel1.
      constexpr std::size_t at = 96 + 3 * 64 + 128;
      rt::SampledIoFrameHeader h;
      std::memcpy(&h, changed.data() + at, sizeof(h));
      switch (field) {
      case 0:
        h.channel_identity ^= 1;
        break;
      case 1:
        --h.sequence;
        break;
      case 2:
        h.release_generation = 0;
        break;
      case 3:
        h.release_generation += 100;
        h.sequence += 100;
        h.trigger_sequence += 100;
        break;
      case 4:
        h.timestamp_domain_identity = 99;
        break;
      case 5:
        h.status = static_cast<unsigned>(rt::SampledIoFrameStatus::safe);
        break;
      case 6:
        h.reserved[0] = 1;
        break;
      case 7:
        h.payload_checksum ^= 1;
        break;
      case 8:
        h.sample_count += 1;
        break;
      case 9:
        h.trigger_sequence += 1;
        break;
      case 10:
        h.calibration_identity += 1;
        break;
      case 11:
        h.release_generation = UINT64_MAX;
        h.sequence = UINT64_MAX;
        break;
      default:
        return {};
      }
      std::memcpy(changed.data() + at, &h, sizeof(h));
      records.push_back({r.name, r.schema_version, changed});
    } else
      records.push_back({r.name, r.schema_version, r.payload});
  }
  std::vector<std::byte> result(original.size());
  const auto provider = [](void *p, std::size_t i,
                           rt::detail::StateWriteView &out) noexcept {
    auto &r = *static_cast<std::vector<rt::detail::StateWriteView> *>(p);
    if (i >= r.size())
      return false;
    out = r[i];
    return true;
  };
  rt::ArtifactWriteResult write;
  if (rt::detail::encode_checkpoint_artifact(metadata, records.size(), provider,
                                             &records, 1024 * 1024, result,
                                             write) != Status::ok)
    return {};
  return result;
}
} // namespace
TEST(CommandBatch, SampledIoRecoveryNativeShutdownRetryDoesNotResubmit) {
  Fixture f;
  ready(f);
  ASSERT_EQ(f.step(0), Status::ok);
  f.backend.card.fail_shutdown = true;
  EXPECT_EQ(f.runtime.stop(), Status::device_error);
  const auto acks = f.backend.card.acks.load(),
             uploads = f.backend.card.uploads.load();
  EXPECT_TRUE(f.backend.card.live);
  EXPECT_EQ(f.backend.card.shutdown_calls, 1u);
  EXPECT_EQ(f.runtime.state(), rt::RuntimeState::running);
  EXPECT_EQ(f.step(1), Status::invalid_state);
  EXPECT_EQ(f.runtime.reset_device(f.device), Status::invalid_state);
  EXPECT_TRUE(f.checkpoint().empty());
  rt::SampledIoChannelStatus safety;
  ASSERT_TRUE(f.runtime.sampled_io_channel_status(f.output, safety));
  EXPECT_EQ(safety.safety_state,
            rt::SampledIoSafetyState::shutdown_acknowledged);
  EXPECT_EQ(f.runtime.stop(), Status::ok) << f.runtime.last_error();
  EXPECT_FALSE(f.backend.card.live);
  EXPECT_EQ(f.backend.card.acks, acks);
  EXPECT_EQ(f.backend.card.uploads, uploads);
  EXPECT_EQ(f.backend.card.shutdown_calls, 2u);
  EXPECT_EQ(f.backend.card.shutdowns, 1u);
  EXPECT_EQ(f.runtime.stop(), Status::ok);
  EXPECT_EQ(f.backend.card.shutdown_calls, 2u);
}
TEST(CommandBatch, SampledIoRecoveryMissingAckMustExecuteBeforeTeardown) {
  Fixture f;
  ready(f);
  const auto startup = f.backend.card.acks.load();
  f.backend.card.missing_ack = true;
  EXPECT_EQ(f.runtime.stop(), Status::device_timeout);
  EXPECT_TRUE(f.backend.card.live);
  EXPECT_EQ(f.backend.card.shutdown_calls, 0u);
  EXPECT_EQ(f.backend.card.acks, startup);
  rt::SampledIoChannelStatus safety;
  ASSERT_TRUE(f.runtime.sampled_io_channel_status(f.output, safety));
  EXPECT_EQ(safety.safety_state, rt::SampledIoSafetyState::unknown);
  f.backend.card.missing_ack = false;
  EXPECT_EQ(f.runtime.stop(), Status::ok) << f.runtime.last_error();
  EXPECT_EQ(f.backend.card.acks, startup + 1);
  EXPECT_EQ(f.backend.card.shutdowns, 1u);
}
TEST(CommandBatch, SampledIoRecoveryFinalizedStopRequiresNoSafeSubmission) {
  Fixture f;
  ASSERT_EQ(f.configure(), Status::ok);
  ASSERT_EQ(f.runtime.finalize(), Status::ok);
  EXPECT_EQ(f.runtime.stop(), Status::ok);
  EXPECT_EQ(f.backend.card.acks, 0u);
  EXPECT_FALSE(f.backend.card.live);
}
TEST(MixedRateReplay, SampledIoRecoveryFreshAndSameOwnerReadPriorDeviceFrame) {
  Fixture original;
  ready(original);
  for (unsigned i = 0; i <= 6; ++i) {
    ASSERT_EQ(original.step(i), Status::ok);
    ASSERT_TRUE(original.valid(i));
  }
  auto checkpoint = original.checkpoint(6);
  ASSERT_FALSE(checkpoint.empty());
  ASSERT_EQ(original.step(7), Status::ok);
  ASSERT_TRUE(original.valid(7));
  const auto expected = original.state;
  const auto read = original.read;
  Fixture fresh;
  ready(fresh);
  const auto startup = fresh.backend.card.acks.load();
  ASSERT_EQ(fresh.runtime.restore_checkpoint(checkpoint), Status::ok);
  EXPECT_EQ(fresh.backend.card.acks, startup);
  ASSERT_EQ(fresh.step(7), Status::ok) << fresh.runtime.last_error();
  EXPECT_TRUE(fresh.valid(7));
  EXPECT_EQ(fresh.state, expected);
  EXPECT_EQ(fresh.read.producer_timestamp, read.producer_timestamp);
  EXPECT_EQ(fresh.read.producer_timestamp_domain_identity,
            read.producer_timestamp_domain_identity);
  EXPECT_EQ(fresh.read.producer_release_sequence, 3u);
  EXPECT_EQ(fresh.read.producer_timestamp, 9000 + 6 * sampled_recovery::period);
  EXPECT_NE(fresh.read.producer_timestamp, fresh.clock.now.load());
  ASSERT_EQ(original.runtime.restore_checkpoint(checkpoint), Status::ok);
  ASSERT_EQ(original.step(7), Status::ok);
  EXPECT_EQ(original.state, expected);
  for (unsigned i = 8; i < 18; ++i) {
    ASSERT_EQ(original.step(i), Status::ok);
    ASSERT_EQ(fresh.step(i), Status::ok);
    EXPECT_TRUE(fresh.valid(i));
    EXPECT_EQ(original.state, fresh.state);
  }
  EXPECT_EQ(original.runtime.stop(), Status::ok);
  EXPECT_EQ(fresh.runtime.stop(), Status::ok);
}
TEST(MixedRateReplay,
     SampledIoRecoveryUnderflowPayloadAndFollowingNormalRelease) {
  Fixture original;
  ready(original);
  for (unsigned i = 0; i < 6; ++i)
    ASSERT_EQ(original.step(i), Status::ok);
  original.skip = true;
  ASSERT_EQ(original.step(6), Status::ok);
  auto checkpoint = original.checkpoint(6);
  ASSERT_FALSE(checkpoint.empty());
  ASSERT_EQ(original.step(7), Status::ok);
  auto expected = original.state;
  EXPECT_TRUE(std::all_of(expected.begin() + 120, expected.begin() + 128,
                          [](auto b) { return b == std::byte{}; }));
  Fixture fresh;
  ready(fresh);
  ASSERT_EQ(fresh.runtime.restore_checkpoint(checkpoint), Status::ok);
  ASSERT_EQ(fresh.step(7), Status::ok);
  EXPECT_EQ(fresh.state, expected);
  EXPECT_EQ(fresh.read.producer_release_sequence, 3u);
  EXPECT_EQ(fresh.read.producer_completion_status, Status::ok);
  original.skip = false;
  ASSERT_EQ(original.step(8), Status::ok);
  ASSERT_EQ(fresh.step(8), Status::ok);
  EXPECT_EQ(original.state, fresh.state);
  EXPECT_TRUE(fresh.valid(8));
  EXPECT_EQ(original.runtime.stop(), Status::ok);
  EXPECT_EQ(fresh.runtime.stop(), Status::ok);
}
TEST(MixedRateReplay,
     SampledIoRecoveryMalformedProducedMetadataRejectsBeforeEffects) {
  Fixture f;
  ready(f);
  for (unsigned i = 0; i <= 6; ++i)
    ASSERT_EQ(f.step(i), Status::ok);
  auto checkpoint = f.checkpoint(6);
  ASSERT_FALSE(checkpoint.empty());
  const auto state = f.state;
  const auto acks = f.backend.card.acks.load();
  for (unsigned field = 0; field < 12; ++field) {
    auto malformed = corrupt_produced_frame(checkpoint, field);
    ASSERT_FALSE(malformed.empty());
    rt::CheckpointMetadata metadata;
    ASSERT_EQ(rt::inspect_checkpoint_artifact(malformed, metadata), Status::ok);
    EXPECT_EQ(f.runtime.restore_checkpoint(malformed),
              Status::incompatible_artifact)
        << field;
    EXPECT_EQ(f.state, state);
    EXPECT_EQ(f.backend.card.acks, acks);
    EXPECT_EQ(f.checkpoint(6), checkpoint) << field;
  }
  ASSERT_EQ(f.step(7), Status::ok);
  EXPECT_TRUE(f.valid(7));
  EXPECT_EQ(f.runtime.stop(), Status::ok);
}
TEST(MixedRateReplay, SampledIoRecoveryInitialReplayAndIncompatibleStorage) {
  Fixture f;
  ready(f);
  auto initial = f.checkpoint();
  ASSERT_FALSE(initial.empty());
  for (unsigned i = 0; i < 16; ++i)
    ASSERT_EQ(f.step(i), Status::ok);
  auto expected = f.state;
  auto active = f.artifact(initial, 16);
  ASSERT_FALSE(active.empty());
  ASSERT_EQ(f.runtime.replay_active(active, Fixture::apply, &f), Status::ok)
      << f.runtime.last_error();
  EXPECT_EQ(f.state, expected);
  Fixture other;
  ASSERT_EQ(other.configure(2, 2), Status::ok);
  ASSERT_EQ(other.start(), Status::ok);
  auto before = other.state;
  auto acks = other.backend.card.acks.load();
  EXPECT_EQ(other.runtime.restore_checkpoint(initial),
            Status::incompatible_artifact);
  EXPECT_EQ(other.runtime.replay_active(active, Fixture::apply, &other),
            Status::incompatible_artifact);
  EXPECT_EQ(other.state, before);
  EXPECT_EQ(other.backend.card.acks, acks);
  EXPECT_EQ(f.runtime.stop(), Status::ok);
  EXPECT_EQ(other.runtime.stop(), Status::ok);
}
TEST(CommandBatch, SampledIoRecoveryIndependentConcurrentOwners) {
  const auto work = [](bool fail) {
    Fixture f;
    if (f.configure() != Status::ok || f.start() != Status::ok)
      return false;
    for (unsigned i = 0; i < 32; ++i)
      if (f.step(i) != Status::ok || !f.valid(i))
        return false;
    f.backend.card.fail_shutdown = fail;
    auto s = f.runtime.stop();
    if (fail && s != Status::device_error)
      return false;
    if (fail)
      s = f.runtime.stop();
    return s == Status::ok && !f.backend.card.live &&
           f.backend.card.shutdowns == 1;
  };
  auto first = std::async(std::launch::async, work, true);
  auto second = std::async(std::launch::async, work, false);
  EXPECT_TRUE(first.get());
  EXPECT_TRUE(second.get());
}

TEST(MixedRateReplay,
     SampledIoRecoverySubstepsAndIntermediateOriginatingReplay) {
  Fixture f;
  f.substeps = 3;
  ready(f);
  for (unsigned i = 0; i <= 6; ++i) {
    ASSERT_EQ(f.step(i), Status::ok);
    ASSERT_TRUE(f.valid(i));
  }
  auto checkpoint = f.checkpoint(6);
  ASSERT_FALSE(checkpoint.empty());
  for (unsigned i = 7; i < 16; ++i) {
    ASSERT_EQ(f.step(i), Status::ok);
    ASSERT_TRUE(f.valid(i));
  }
  auto expected = f.state;
  auto active = f.artifact(checkpoint, 9, 7);
  ASSERT_FALSE(active.empty());
  ASSERT_EQ(f.runtime.replay_active(active, Fixture::apply, &f), Status::ok)
      << f.runtime.last_error();
  EXPECT_EQ(f.state, expected);
  EXPECT_EQ(f.read.producer_substep_ordinal, 2u);
  Fixture other;
  other.substeps = 3;
  ready(other);
  ASSERT_EQ(other.runtime.restore_checkpoint(checkpoint), Status::ok);
  ASSERT_EQ(other.step(7), Status::ok);
  EXPECT_TRUE(other.valid(7));
  EXPECT_EQ(other.read.producer_release_sequence, 3u);
  EXPECT_EQ(other.read.producer_substep_ordinal, 2u);
  EXPECT_EQ(other.runtime.stop(), Status::ok);
  EXPECT_EQ(f.runtime.stop(), Status::ok);
}

TEST(CommandBatch,
     SampledIoRecoveryPartialInitializationRetainsCorrectStopStage) {
  Fixture f;
  ASSERT_EQ(f.configure(), Status::ok);
  f.backend.card.fail_initialize = true;
  EXPECT_EQ(f.start(), Status::device_error);
  EXPECT_FALSE(f.backend.card.live);
  EXPECT_EQ(f.backend.card.acks, 0u);
  EXPECT_EQ(f.runtime.stop(), Status::ok) << f.runtime.last_error();
  EXPECT_EQ(f.backend.card.acks, 0u);
}

TEST(CommandBatch, SampledIoRecoveryPartialMultiBackendTeardown) {
  Fixture f;
  f.companion = std::make_unique<sampled_recovery::Backend>();
  ready(f);
  ASSERT_EQ(f.step(0), Status::ok);
  f.companion->card.fail_shutdown = true;
  EXPECT_EQ(f.runtime.stop(), Status::device_error);
  const auto acknowledged = f.backend.card.acks.load();
  EXPECT_FALSE(f.backend.card.live);
  EXPECT_TRUE(f.companion->card.live);
  EXPECT_EQ(f.backend.card.shutdowns, 1u);
  EXPECT_EQ(f.companion->card.shutdowns, 0u);
  EXPECT_EQ(f.step(1), Status::invalid_state);
  EXPECT_EQ(f.runtime.stop(), Status::ok) << f.runtime.last_error();
  EXPECT_EQ(f.backend.card.acks, acknowledged);
  EXPECT_EQ(f.backend.card.shutdowns, 1u);
  EXPECT_EQ(f.companion->card.shutdowns, 1u);
  EXPECT_FALSE(f.companion->card.live);
  EXPECT_EQ(f.runtime.stop(), Status::ok);
}
