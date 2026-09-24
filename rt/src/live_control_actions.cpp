#include "live_control_actions.hpp"

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

template <typename Range>
[[nodiscard]] bool zero(const Range& values) noexcept {
    for (const auto value : values) {
        if (value != typename Range::value_type{}) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool status_valid(std::int32_t value) noexcept {
    return value <= static_cast<std::int32_t>(Status::ok) &&
        value >= static_cast<std::int32_t>(Status::incompatible_abi);
}

[[nodiscard]] bool no_target(
    const LiveControlBoundaryTarget& target) noexcept {
    return target.frame_index == std::numeric_limits<std::uint64_t>::max() &&
        target.rate_release_sequence ==
            std::numeric_limits<std::uint64_t>::max() &&
        target.reference_release_index ==
            std::numeric_limits<std::uint32_t>::max() &&
        target.rate_domain_registration_index ==
            std::numeric_limits<std::uint32_t>::max() &&
        target.phase_index == std::numeric_limits<std::uint32_t>::max() &&
        target.rate_substep_ordinal ==
            std::numeric_limits<std::uint32_t>::max();
}

[[nodiscard]] bool exact_target(
    const LiveControlBoundaryTarget& target) noexcept {
    if (target.kind == LiveControlTargetKind::host_frame) {
        return target.frame_index !=
                std::numeric_limits<std::uint64_t>::max() &&
            target.rate_release_sequence ==
                std::numeric_limits<std::uint64_t>::max() &&
            target.reference_release_index ==
                std::numeric_limits<std::uint32_t>::max() &&
            target.rate_domain_registration_index ==
                std::numeric_limits<std::uint32_t>::max() &&
            target.phase_index ==
                std::numeric_limits<std::uint32_t>::max() &&
            target.rate_substep_ordinal ==
                std::numeric_limits<std::uint32_t>::max();
    }
    return target.kind == LiveControlTargetKind::rate_release &&
        target.frame_index == std::numeric_limits<std::uint64_t>::max() &&
        target.rate_release_sequence !=
            std::numeric_limits<std::uint64_t>::max() &&
        target.reference_release_index !=
            std::numeric_limits<std::uint32_t>::max() &&
        target.rate_domain_registration_index !=
            std::numeric_limits<std::uint32_t>::max() &&
        target.phase_index != std::numeric_limits<std::uint32_t>::max() &&
        target.rate_substep_ordinal !=
            std::numeric_limits<std::uint32_t>::max();
}

[[nodiscard]] bool record_identity_present(
    const LiveControlActionRecord& record) noexcept {
    return record.mailbox_identity != 0 && record.producer_identity != 0 &&
           record.mailbox_sequence != 0 && record.producer_sequence != 0 &&
           (record.payload_bytes != 0 ||
            record.update_kind == LiveControlUpdateKind::clear_fault);
}

[[nodiscard]] bool record_identity_absent(
    const LiveControlActionRecord& record) noexcept {
    return record.mailbox_identity == 0 && record.producer_identity == 0 &&
        record.mailbox_sequence == 0 && record.producer_sequence == 0 &&
        record.payload_digest == 0 && record.payload_bytes == 0;
}

[[nodiscard]] bool default_record_fields(
    const LiveControlActionRecord& record) noexcept {
    return record_identity_absent(record) &&
        record.update_kind == LiveControlUpdateKind::scenario_parameters &&
        record.admission_result == LiveControlAdmissionResult::accepted &&
        record.record_status == LiveControlRecordStatus::staged;
}

[[nodiscard]] bool no_correlations(
    const LiveControlActionRecord& record) noexcept {
    return record.checkpoint_correlation == 0 &&
        record.replay_correlation == 0;
}

[[nodiscard]] LiveControlActionReason admission_reason(
    LiveControlAdmissionResult result) noexcept {
    switch (result) {
    case LiveControlAdmissionResult::accepted:
        return LiveControlActionReason::normal;
    case LiveControlAdmissionResult::invalid:
        return LiveControlActionReason::invalid;
    case LiveControlAdmissionResult::full:
        return LiveControlActionReason::full;
    case LiveControlAdmissionResult::busy:
        return LiveControlActionReason::busy;
    case LiveControlAdmissionResult::stale:
        return LiveControlActionReason::stale;
    case LiveControlAdmissionResult::stopped:
        return LiveControlActionReason::stopped;
    case LiveControlAdmissionResult::exhausted:
        return LiveControlActionReason::exhausted;
    case LiveControlAdmissionResult::missed:
        return LiveControlActionReason::missed;
    }
    return LiveControlActionReason::invalid;
}

[[nodiscard]] bool target_shape_valid(
    const LiveControlActionRecord& record) noexcept {
    switch (record.action) {
    case LiveControlActionId::admission:
        return record.mailbox_identity == 0
            ? no_target(record.target)
            : exact_target(record.target);
    case LiveControlActionId::boundary_empty:
    case LiveControlActionId::provisional_publication:
    case LiveControlActionId::committed:
    case LiveControlActionId::replaced:
    case LiveControlActionId::missed:
    case LiveControlActionId::stopped:
    case LiveControlActionId::rolled_back:
        return exact_target(record.target);
    case LiveControlActionId::checkpointed:
    case LiveControlActionId::replay_verified:
        return record.generation_identity == 0
            ? no_target(record.target)
            : exact_target(record.target);
    }
    return false;
}

[[nodiscard]] bool shape_valid(
    const LiveControlActionRecord& record) noexcept {
    switch (record.action) {
    case LiveControlActionId::admission:
        return record.stage == LiveControlActionStage::attempt &&
            (record.result == LiveControlActionResult::accepted ||
             record.result == LiveControlActionResult::rejected) &&
            ((record.admission_result ==
                  LiveControlAdmissionResult::accepted) ==
             (record.result == LiveControlActionResult::accepted)) &&
            record.generation_identity == 0 &&
            record.prior_generation_identity == 0 &&
            record.survivor_count == 0 && record.replaced_count == 0 &&
            record.terminal_status == static_cast<std::int32_t>(Status::ok) &&
            record.reason == admission_reason(record.admission_result) &&
            no_correlations(record) &&
            (record.admission_result == LiveControlAdmissionResult::accepted
                ? record_identity_present(record) &&
                    record.record_status == LiveControlRecordStatus::staged
                : record.admission_result == LiveControlAdmissionResult::missed
                    ? record_identity_present(record) &&
                        record.record_status == LiveControlRecordStatus::missed
                    : record_identity_absent(record) &&
                        record.record_status == LiveControlRecordStatus::staged);
    case LiveControlActionId::boundary_empty:
        return record.stage == LiveControlActionStage::terminal &&
            record.reason == LiveControlActionReason::normal &&
            record.result == LiveControlActionResult::settled &&
            record.survivor_count == 0 &&
            record.replaced_count == 0 &&
            record.generation_identity == record.prior_generation_identity &&
            record.terminal_status == static_cast<std::int32_t>(Status::ok) &&
            default_record_fields(record) && no_correlations(record);
    case LiveControlActionId::provisional_publication:
        return record.stage == LiveControlActionStage::provisional &&
            record.reason == LiveControlActionReason::normal &&
            record.result == LiveControlActionResult::published &&
            record.generation_identity != 0 && record.survivor_count != 0 &&
            record.terminal_status == static_cast<std::int32_t>(Status::ok) &&
            default_record_fields(record) && no_correlations(record);
    case LiveControlActionId::committed:
        return record.stage == LiveControlActionStage::terminal &&
            record.reason == LiveControlActionReason::normal &&
            record.result == LiveControlActionResult::settled &&
            record.record_status == LiveControlRecordStatus::committed &&
            record.generation_identity != 0 &&
            record.terminal_status == static_cast<std::int32_t>(Status::ok) &&
            record_identity_present(record) && no_correlations(record) &&
            record.admission_result == LiveControlAdmissionResult::accepted &&
            record.survivor_count == 0 && record.replaced_count == 0;
    case LiveControlActionId::replaced:
        return record.stage == LiveControlActionStage::terminal &&
            record.reason == LiveControlActionReason::replaced &&
            record.result == LiveControlActionResult::settled &&
            record.record_status == LiveControlRecordStatus::replaced &&
            record.generation_identity != 0 &&
            record.terminal_status == static_cast<std::int32_t>(Status::ok) &&
            record_identity_present(record) && no_correlations(record) &&
            record.admission_result == LiveControlAdmissionResult::accepted &&
            record.survivor_count == 0 && record.replaced_count == 0;
    case LiveControlActionId::missed:
        return record.stage == LiveControlActionStage::terminal &&
            record.reason == LiveControlActionReason::missed &&
            record.result == LiveControlActionResult::settled &&
            record.record_status == LiveControlRecordStatus::missed &&
            record.generation_identity == 0 &&
            record.prior_generation_identity == 0 &&
            record.terminal_status == static_cast<std::int32_t>(Status::ok) &&
            record_identity_present(record) && no_correlations(record) &&
            record.admission_result == LiveControlAdmissionResult::missed &&
            record.survivor_count == 0 && record.replaced_count == 0;
    case LiveControlActionId::stopped:
        return record.stage == LiveControlActionStage::terminal &&
            record.reason == LiveControlActionReason::stopped &&
            record.result == LiveControlActionResult::settled &&
            record.record_status == LiveControlRecordStatus::stopped &&
            record.generation_identity == 0 &&
            record.prior_generation_identity == 0 &&
            record.terminal_status == static_cast<std::int32_t>(Status::ok) &&
            record_identity_present(record) && no_correlations(record) &&
            record.admission_result == LiveControlAdmissionResult::stopped &&
            record.survivor_count == 0 && record.replaced_count == 0;
    case LiveControlActionId::rolled_back:
        return record.stage == LiveControlActionStage::terminal &&
            record.reason == LiveControlActionReason::execution_failed &&
            record.result == LiveControlActionResult::rolled_back &&
            record.record_status == LiveControlRecordStatus::rolled_back &&
            record.generation_identity != 0 &&
            record.terminal_status != static_cast<std::int32_t>(Status::ok) &&
            record_identity_present(record) && no_correlations(record) &&
            record.admission_result == LiveControlAdmissionResult::accepted &&
            record.survivor_count == 0 && record.replaced_count == 0;
    case LiveControlActionId::checkpointed:
        return record.stage == LiveControlActionStage::checkpoint &&
            record.reason == LiveControlActionReason::checkpoint &&
            record.result == LiveControlActionResult::settled &&
            record.checkpoint_correlation != 0 &&
            record.replay_correlation == 0 &&
            default_record_fields(record) && record.survivor_count == 0 &&
            record.replaced_count == 0 &&
            record.terminal_status == static_cast<std::int32_t>(Status::ok);
    case LiveControlActionId::replay_verified:
        return record.stage == LiveControlActionStage::replay &&
            record.reason == LiveControlActionReason::replay &&
            record.replay_correlation != 0 &&
            record.checkpoint_correlation == 0 &&
            record.result == LiveControlActionResult::verified &&
            default_record_fields(record) && record.survivor_count == 0 &&
            record.replaced_count == 0 &&
            record.terminal_status == static_cast<std::int32_t>(Status::ok);
    }
    return false;
}

} // namespace

bool live_control_action_valid(
    const LiveControlActionRecord& record) noexcept {
    return record.schema_version == live_control_action_schema_version &&
        record.record_size == sizeof(LiveControlActionRecord) &&
        record.runtime_id != 0 && record.configuration_generation != 0 &&
        record.policy_identity != 0 &&
        between(
            record.update_kind,
            LiveControlUpdateKind::scenario_parameters,
            LiveControlUpdateKind::clear_fault) &&
        between(
            record.admission_result,
            LiveControlAdmissionResult::accepted,
            LiveControlAdmissionResult::missed) &&
        between(
            record.record_status,
            LiveControlRecordStatus::staged,
            LiveControlRecordStatus::rolled_back) &&
        between(
            record.action,
            LiveControlActionId::admission,
            LiveControlActionId::replay_verified) &&
        between(
            record.stage,
            LiveControlActionStage::attempt,
            LiveControlActionStage::replay) &&
        between(
            record.reason,
            LiveControlActionReason::normal,
            LiveControlActionReason::replay) &&
        between(
            record.result,
            LiveControlActionResult::accepted,
            LiveControlActionResult::verified) &&
        status_valid(record.terminal_status) && zero(record.target.reserved) &&
        zero(record.reserved) && target_shape_valid(record) &&
        shape_valid(record);
}

LiveControlActionRing::LiveControlActionRing(std::size_t capacity)
    : capacity_(capacity) {
    if (capacity > live_control_action_capacity_limit ||
        capacity > std::numeric_limits<std::size_t>::max() / sizeof(Slot)) {
        throw std::invalid_argument("live-control action capacity is invalid");
    }
    storage_.allocate(capacity * sizeof(Slot), alignof(Slot));
    slots_ = reinterpret_cast<Slot*>(storage_.data());
    for (std::size_t index = 0; index < capacity_; ++index) {
        ::new (static_cast<void*>(slots_ + index)) Slot{};
    }
}

LiveControlActionRing::~LiveControlActionRing() {
    for (std::size_t index = capacity_; index != 0; --index) {
        slots_[index - 1].~Slot();
    }
}

bool LiveControlActionRing::emit(
    LiveControlActionRecord record,
    std::uint64_t* assigned_sequence) noexcept {
    std::uint64_t sequence = invalid_sequence;
    const bool reserved = reserve_sequence(sequence);
    if (assigned_sequence) {
        *assigned_sequence = sequence;
    }
    return reserved && publish_reserved(record, sequence);
}

bool LiveControlActionRing::reserve_sequence(std::uint64_t& sequence) noexcept {
    sequence = next_sequence_.load(std::memory_order_relaxed);
    if (sequence == invalid_sequence ||
        !next_sequence_.compare_exchange_strong(sequence, sequence + 1,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
        sequence = invalid_sequence;
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool LiveControlActionRing::publish_reserved(LiveControlActionRecord record,
                                             std::uint64_t sequence) noexcept {
    if (sequence == invalid_sequence) {
        return false;
    }
    record.sequence = sequence;
    if (!live_control_action_valid(record) || capacity_ == 0) {
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

bool LiveControlActionRing::read_sequence(
    std::uint64_t sequence,
    LiveControlActionRecord& record) const noexcept {
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
    const auto candidate = std::bit_cast<LiveControlActionRecord>(words);
    if (slot.committed_sequence.load(std::memory_order_acquire) != sequence ||
        candidate.sequence != sequence ||
        !live_control_action_valid(candidate)) {
        return false;
    }
    record = candidate;
    return true;
}

std::uint64_t LiveControlActionRing::oldest_sequence(
    std::uint64_t end_sequence) const noexcept {
    const auto capacity = static_cast<std::uint64_t>(capacity_);
    return end_sequence > capacity ? end_sequence - capacity : 0;
}

bool LiveControlActionRing::gap_free(
    std::uint64_t first_sequence,
    std::uint64_t count) const noexcept {
    if (count > std::numeric_limits<std::uint64_t>::max() - first_sequence) {
        return false;
    }
    LiveControlActionRecord record;
    for (std::uint64_t offset = 0; offset < count; ++offset) {
        if (!read_sequence(first_sequence + offset, record)) {
            return false;
        }
    }
    return true;
}

void LiveControlActionRing::restore_sequence(
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
