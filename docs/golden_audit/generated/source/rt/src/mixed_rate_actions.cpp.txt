#include "mixed_rate_actions.hpp"

#include <bit>
#include <new>
#include <stdexcept>

namespace rt::detail {
namespace {

template <typename Enum>
[[nodiscard]] constexpr bool between(
    Enum value,
    Enum first,
    Enum last) noexcept {
    return value >= first && value <= last;
}

[[nodiscard]] bool reserved_zero(
    const MixedRateActionRecord& record) noexcept {
    for (const auto byte : record.reserved) {
        if (byte != std::byte{0}) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool status_valid(std::int32_t value) noexcept {
    return value <= static_cast<std::int32_t>(Status::ok) &&
        value >= static_cast<std::int32_t>(Status::incompatible_abi);
}

[[nodiscard]] bool action_shape_valid(
    const MixedRateActionRecord& record) noexcept {
    const auto phase_unspecified =
        record.phase_index == std::numeric_limits<std::uint32_t>::max();
    const auto domain_unspecified =
        record.rate_domain_registration_index ==
            std::numeric_limits<std::uint32_t>::max();
    const auto is_rate =
        record.action >= MixedRateActionId::rate_execute &&
        record.action <= MixedRateActionId::rate_recover;
    if (is_rate) {
        return record.stage == MixedRateActionStage::decision &&
            phase_unspecified && !domain_unspecified &&
            record.backend_identity == 0 &&
            record.freshness == MixedRateSampleFreshness::not_applicable &&
            record.safety_state == MixedRateSafetyState::not_applicable &&
            record.shed == (record.action == MixedRateActionId::rate_shed) &&
            (!record.held || record.action == MixedRateActionId::rate_hold) &&
            !record.substituted;
    }
    switch (record.action) {
    case MixedRateActionId::device_terminal:
        return !phase_unspecified && !domain_unspecified &&
            record.backend_identity != 0 &&
            (record.stage == MixedRateActionStage::terminal ||
             record.stage == MixedRateActionStage::quarantined) &&
            !record.shed;
    case MixedRateActionId::sampled_publish:
        return !phase_unspecified && !domain_unspecified &&
            record.timeline_identity != 0 &&
            (record.stage == MixedRateActionStage::published ||
             record.stage == MixedRateActionStage::terminal) &&
            !record.shed;
    case MixedRateActionId::sampled_select:
        return !phase_unspecified && !domain_unspecified &&
            record.timeline_identity != 0 &&
            (record.stage == MixedRateActionStage::selected ||
             record.stage == MixedRateActionStage::terminal) &&
            !record.shed;
    case MixedRateActionId::safe_transition:
        return !phase_unspecified && !domain_unspecified &&
            record.backend_identity != 0 && record.timeline_identity != 0 &&
            (record.stage == MixedRateActionStage::acknowledged ||
             record.stage == MixedRateActionStage::terminal) &&
            record.freshness == MixedRateSampleFreshness::substituted &&
            record.substituted &&
            record.safety_state != MixedRateSafetyState::not_applicable &&
            !record.shed;
    case MixedRateActionId::watchdog_transition:
        return phase_unspecified &&
            record.stage == MixedRateActionStage::terminal &&
            record.reason == MixedRateActionReason::watchdog &&
            !record.shed;
    case MixedRateActionId::runtime_stop:
        return phase_unspecified && domain_unspecified &&
            record.stage == MixedRateActionStage::terminal &&
            (record.reason == MixedRateActionReason::normal ||
             record.reason == MixedRateActionReason::cleanup_pending) &&
            !record.shed;
    case MixedRateActionId::rate_execute:
    case MixedRateActionId::rate_skip:
    case MixedRateActionId::rate_hold:
    case MixedRateActionId::rate_shed:
    case MixedRateActionId::rate_recover:
        break;
    }
    return false;
}

} // namespace

bool mixed_rate_action_valid(
    const MixedRateActionRecord& record) noexcept {
    return record.schema_version == mixed_rate_action_schema_version &&
        record.record_size == sizeof(MixedRateActionRecord) &&
        record.runtime_id != 0 && record.host_policy_version != 0 &&
        between(
            record.action,
            MixedRateActionId::rate_execute,
            MixedRateActionId::runtime_stop) &&
        between(
            record.reason,
            MixedRateActionReason::normal,
            MixedRateActionReason::cleanup_pending) &&
        between(
            record.stage,
            MixedRateActionStage::decision,
            MixedRateActionStage::quarantined) &&
        between(
            record.freshness,
            MixedRateSampleFreshness::not_applicable,
            MixedRateSampleFreshness::substituted) &&
        between(
            record.safety_state,
            MixedRateSafetyState::not_applicable,
            MixedRateSafetyState::shutdown_acknowledged) &&
        status_valid(record.terminal_status) &&
        record.degradation_before <= record.degradation_after &&
        (!record.substituted ||
         record.freshness == MixedRateSampleFreshness::substituted) &&
        action_shape_valid(record) &&
        reserved_zero(record);
}

MixedRateActionRing::MixedRateActionRing(std::size_t capacity)
    : capacity_(capacity) {
    if (capacity > mixed_rate_action_capacity_limit ||
        capacity > std::numeric_limits<std::size_t>::max() / sizeof(Slot)) {
        throw std::invalid_argument("mixed-rate action capacity is invalid");
    }
    storage_.allocate(capacity * sizeof(Slot), alignof(Slot));
    slots_ = reinterpret_cast<Slot*>(storage_.data());
    for (std::size_t index = 0; index < capacity_; ++index) {
        ::new (static_cast<void*>(slots_ + index)) Slot{};
    }
}

MixedRateActionRing::~MixedRateActionRing() {
    for (std::size_t index = capacity_; index != 0; --index) {
        slots_[index - 1].~Slot();
    }
}

bool MixedRateActionRing::emit(
    MixedRateActionRecord record,
    std::uint64_t* assigned_sequence) noexcept {
    auto sequence = next_sequence_.load(std::memory_order_relaxed);
    while (sequence != std::numeric_limits<std::uint64_t>::max() &&
           !next_sequence_.compare_exchange_weak(
               sequence,
               sequence + 1,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
    if (assigned_sequence) {
        *assigned_sequence = sequence;
    }
    if (sequence == std::numeric_limits<std::uint64_t>::max()) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    record.sequence = sequence;
    if (!mixed_rate_action_valid(record) || capacity_ == 0) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto& slot = slots_[static_cast<std::size_t>(
        sequence % static_cast<std::uint64_t>(capacity_))];
    if (slot.writer.test_and_set(std::memory_order_acquire)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto previous = slot.committed_sequence.exchange(
        invalid_sequence, std::memory_order_acq_rel);
    const auto words =
        std::bit_cast<std::array<std::uint64_t, word_count>>(record);
    for (std::size_t index = 0; index < words.size(); ++index) {
        slot.words[index].store(words[index], std::memory_order_relaxed);
    }
    slot.committed_sequence.store(sequence, std::memory_order_release);
    slot.writer.clear(std::memory_order_release);
    emitted_.fetch_add(1, std::memory_order_relaxed);
    if (previous != invalid_sequence) {
        overwritten_.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool MixedRateActionRing::read_sequence(
    std::uint64_t sequence,
    MixedRateActionRecord& record) const noexcept {
    record = {};
    if (capacity_ == 0) {
        return false;
    }
    const auto& slot = slots_[static_cast<std::size_t>(
        sequence % static_cast<std::uint64_t>(capacity_))];
    if (slot.committed_sequence.load(std::memory_order_acquire) != sequence) {
        return false;
    }
    std::array<std::uint64_t, word_count> words{};
    for (std::size_t index = 0; index < words.size(); ++index) {
        words[index] = slot.words[index].load(std::memory_order_relaxed);
    }
    const auto candidate = std::bit_cast<MixedRateActionRecord>(words);
    if (slot.committed_sequence.load(std::memory_order_acquire) != sequence ||
        candidate.sequence != sequence || !mixed_rate_action_valid(candidate)) {
        return false;
    }
    record = candidate;
    return true;
}

std::uint64_t MixedRateActionRing::oldest_sequence(
    std::uint64_t end_sequence) const noexcept {
    const auto capacity = static_cast<std::uint64_t>(capacity_);
    return end_sequence > capacity ? end_sequence - capacity : 0;
}

bool MixedRateActionRing::gap_free(
    std::uint64_t first_sequence,
    std::uint64_t count) const noexcept {
    if (count > std::numeric_limits<std::uint64_t>::max() - first_sequence) {
        return false;
    }
    MixedRateActionRecord record;
    for (std::uint64_t offset = 0; offset < count; ++offset) {
        if (!read_sequence(first_sequence + offset, record)) {
            return false;
        }
    }
    return true;
}

void MixedRateActionRing::restore_sequence(
    std::uint64_t next_sequence) noexcept {
    for (std::size_t index = 0; index < capacity_; ++index) {
        auto& slot = slots_[index];
        slot.committed_sequence.store(invalid_sequence, std::memory_order_release);
        slot.writer.clear(std::memory_order_release);
    }
    next_sequence_.store(next_sequence, std::memory_order_release);
    emitted_.store(0, std::memory_order_release);
    overwritten_.store(0, std::memory_order_release);
    dropped_.store(0, std::memory_order_release);
}

} // namespace rt::detail
