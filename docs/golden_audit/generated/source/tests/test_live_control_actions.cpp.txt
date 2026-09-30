#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <rt/runtime.hpp>

#include "rt/src/live_control_actions.hpp"

namespace {

rt::LiveControlActionRecord action_record() {
    rt::LiveControlActionRecord record;
    record.runtime_id = 11;
    record.configuration_generation = 12;
    record.policy_identity = 13;
    record.target.frame_index = 7;
    record.mailbox_identity = 14;
    record.producer_identity = 15;
    record.mailbox_sequence = 1;
    record.producer_sequence = 1;
    record.payload_digest = 16;
    record.payload_bytes = 4;
    return record;
}

} // namespace

TEST(LiveControlActions, PublicLayoutAndClosedTablesRejectMalformedRecords) {
    EXPECT_EQ(sizeof(rt::LiveControlClosurePolicy), 72u);
    EXPECT_EQ(alignof(rt::LiveControlClosurePolicy), 8u);
    EXPECT_EQ(sizeof(rt::LiveControlActionRecord), 256u);
    EXPECT_EQ(alignof(rt::LiveControlActionRecord), 8u);

    auto record = action_record();
    EXPECT_TRUE(rt::detail::live_control_action_valid(record));
    record.reserved.front() = std::byte{1};
    EXPECT_FALSE(rt::detail::live_control_action_valid(record));
    record = action_record();
    record.target.reserved.back() = std::byte{1};
    EXPECT_FALSE(rt::detail::live_control_action_valid(record));
    record = action_record();
    record.action = static_cast<rt::LiveControlActionId>(0);
    EXPECT_FALSE(rt::detail::live_control_action_valid(record));
    record = action_record();
    record.action = rt::LiveControlActionId::rolled_back;
    EXPECT_FALSE(rt::detail::live_control_action_valid(record));
}

TEST(LiveControlActions, FixedRingReportsOverwriteGapAndResetExactly) {
    rt::detail::LiveControlActionRing ring(2);
    for (std::uint64_t sequence = 0; sequence < 3; ++sequence) {
        auto record = action_record();
        record.mailbox_sequence = sequence + 1;
        std::uint64_t assigned = 99;
        ASSERT_TRUE(ring.emit(record, &assigned));
        EXPECT_EQ(assigned, sequence);
    }
    EXPECT_EQ(ring.next_sequence(), 3u);
    EXPECT_EQ(ring.emitted(), 3u);
    EXPECT_EQ(ring.overwritten(), 1u);
    EXPECT_EQ(ring.dropped(), 0u);
    EXPECT_EQ(ring.oldest_sequence(ring.next_sequence()), 1u);

    rt::LiveControlActionRecord read;
    EXPECT_FALSE(ring.read_sequence(0, read));
    ASSERT_TRUE(ring.read_sequence(1, read));
    EXPECT_EQ(read.sequence, 1u);
    EXPECT_EQ(read.mailbox_sequence, 2u);
    EXPECT_TRUE(ring.gap_free(1, 2));
    EXPECT_FALSE(ring.gap_free(0, 3));

    ring.restore_sequence(10);
    EXPECT_EQ(ring.next_sequence(), 10u);
    EXPECT_EQ(ring.emitted(), 0u);
    EXPECT_EQ(ring.overwritten(), 0u);
    EXPECT_EQ(ring.dropped(), 0u);
    EXPECT_FALSE(ring.read_sequence(1, read));
    std::uint64_t assigned = 0;
    ASSERT_TRUE(ring.emit(action_record(), &assigned));
    EXPECT_EQ(assigned, 10u);
}

TEST(LiveControlActions, ZeroCapacityDropsButStillReservesSequence) {
    rt::detail::LiveControlActionRing ring(0);
    std::uint64_t assigned = 99;
    EXPECT_FALSE(ring.emit(action_record(), &assigned));
    EXPECT_EQ(assigned, 0u);
    EXPECT_EQ(ring.next_sequence(), 1u);
    EXPECT_EQ(ring.emitted(), 0u);
    EXPECT_EQ(ring.dropped(), 1u);
}

TEST(LiveControlActions, SequenceExhaustionFailsBeforeWrap) {
    rt::detail::LiveControlActionRing ring(1);
    ring.restore_sequence(std::numeric_limits<std::uint64_t>::max() - 1);
    std::uint64_t assigned = 0;
    ASSERT_TRUE(ring.emit(action_record(), &assigned));
    EXPECT_EQ(assigned, std::numeric_limits<std::uint64_t>::max() - 1);
    EXPECT_EQ(ring.next_sequence(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_FALSE(ring.emit(action_record(), &assigned));
    EXPECT_EQ(assigned, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(ring.next_sequence(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(ring.dropped(), 1u);
}
