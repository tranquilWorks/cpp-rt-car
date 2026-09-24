#include "live_control_mailbox.hpp"
#include "live_control_actions.hpp"
#include "live_control_replay.hpp"
#include "snapshot_codec.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::uint32_t kInvalidIndex =
    std::numeric_limits<std::uint32_t>::max();
constexpr std::uint64_t kInvalidSequence =
    std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kCheckpointMagic = 0x31434c5746545252ull;
constexpr std::size_t kCheckpointHeaderBytes = 256;
constexpr std::size_t kCheckpointMailboxBytes = 128;
constexpr std::size_t kCheckpointProducerBytes = 48;
constexpr std::size_t kCheckpointSlotBytes = 160;
constexpr std::size_t kCheckpointGenerationRecordBytes = 144;
constexpr std::size_t kCheckpointRateTargetBytes = 8;

template <std::size_t Size>
[[nodiscard]] bool all_zero(
    const std::array<std::byte, Size>& bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](std::byte value) {
        return value == std::byte{0};
    });
}

[[nodiscard]] bool store_u32(
    std::span<std::byte> bytes,
    std::size_t offset,
    std::uint32_t value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        return false;
    }
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::byte>(value >> (8u * index));
    }
    return true;
}

[[nodiscard]] bool store_u64(
    std::span<std::byte> bytes,
    std::size_t offset,
    std::uint64_t value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 8) {
        return false;
    }
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<std::byte>(value >> (8u * index));
    }
    return true;
}

[[nodiscard]] bool load_u32(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::uint32_t& value) noexcept {
    value = 0;
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        return false;
    }
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(bytes[offset + index])) <<
            (8u * index);
    }
    return true;
}

[[nodiscard]] bool load_u64(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::uint64_t& value) noexcept {
    value = 0;
    if (offset > bytes.size() || bytes.size() - offset < 8) {
        return false;
    }
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(bytes[offset + index])) <<
            (8u * index);
    }
    return true;
}

[[nodiscard]] std::uint64_t checkpoint_checksum(
    std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = kFnvOffset;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto value = index >= 24 && index < 32
            ? std::uint8_t{0}
            : std::to_integer<std::uint8_t>(bytes[index]);
        hash ^= value;
        hash *= kFnvPrime;
    }
    return hash;
}

void increment(std::atomic<std::uint64_t>& value) noexcept {
    (void)value.fetch_add(1, std::memory_order_relaxed);
}

class FlagGuard final {
public:
    explicit FlagGuard(std::atomic_flag& flag) noexcept : flag_(flag) {}
    ~FlagGuard() { flag_.clear(std::memory_order_release); }
    FlagGuard(const FlagGuard&) = delete;
    FlagGuard& operator=(const FlagGuard&) = delete;

private:
    std::atomic_flag& flag_;
};

} // namespace

namespace rt {

std::uint64_t live_control_payload_digest(
    std::span<const std::byte> payload) noexcept {
    std::uint64_t hash = kFnvOffset;
    for (const auto value : payload) {
        hash ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(value));
        hash *= kFnvPrime;
    }
    return hash;
}

} // namespace rt

namespace rt::detail {

static_assert(
    std::atomic<std::uint64_t>::is_always_lock_free,
    "live-control mailboxes require lock-free 64-bit atomics");

struct LiveControlMailboxSet::Impl {
    enum class SlotState : std::uint8_t {
        free = 0,
        writing = 1,
        staged = 2,
        boundary_owned = 3,
        committed = 4,
        replaced = 5,
        missed = 6,
        stopped = 7,
        rolled_back = 8,
    };

    struct RateTarget {
        std::uint64_t release_time_ns = 0;
        std::uint64_t release_sequence = 0;
        std::uint32_t domain_registration_index = 0;
        std::uint32_t phase_index = 0;
        std::uint32_t substep_ordinal = 0;
        std::atomic<bool> closed{false};
    };

    struct Slot {
        std::atomic<SlotState> state{SlotState::free};
        std::atomic<std::uint64_t> terminal_generation{0};
        LiveControlUpdateRecord record{};
    };

    struct Mailbox {
        LiveControlMailboxRegistration registration{};
        std::uint32_t producer_count = 0;
        std::atomic_flag reservation = ATOMIC_FLAG_INIT;
        std::atomic<bool> reclaiming{false};
        std::atomic<std::uint32_t> inspection_readers{0};
        std::unique_ptr<Slot[]> slots{};
        std::unique_ptr<std::byte[]> payload_storage{};
        std::atomic<std::uint64_t> next_sequence{1};
        std::atomic<std::uint32_t> occupancy{0};
        std::atomic<std::uint64_t> accepted{0};
        std::atomic<std::uint64_t> invalid{0};
        std::atomic<std::uint64_t> full{0};
        std::atomic<std::uint64_t> busy{0};
        std::atomic<std::uint64_t> stale{0};
        std::atomic<std::uint64_t> stopped{0};
        std::atomic<std::uint64_t> exhausted{0};
        std::atomic<std::uint64_t> missed{0};
    };

    struct Candidate {
        std::uint32_t mailbox_index = 0;
        std::uint32_t slot_index = 0;
        std::uint64_t mailbox_identity = 0;
        std::uint64_t mailbox_sequence = 0;
        bool survivor = true;
        std::array<std::byte, 7> reserved{};
    };

    struct Generation {
        std::unique_ptr<LiveControlRecordView[]> records{};
        std::unique_ptr<std::byte[]> payloads{};
        LiveControlGenerationView view{};
    };

    struct Producer {
        std::uint64_t mailbox_identity = 0;
        std::uint64_t producer_identity = 0;
        std::uint32_t mailbox_index = kInvalidIndex;
        std::uint64_t first_sequence = 1;
        std::atomic<std::uint64_t> next_sequence{1};
    };

    struct ProvisionalRecord {
        std::uint32_t mailbox_index = kInvalidIndex;
        std::uint32_t slot_index = kInvalidIndex;
        std::uint64_t generation_identity = 0;
        std::uint64_t prior_generation_identity = 0;
        bool survivor = false;
        std::array<std::byte, 7> reserved{};
    };

    struct RetainedRecord {
        LiveControlUpdateRecord record{};
        std::uint64_t payload_offset = 0;
    };

    struct RetainedGeneration {
        LiveControlBoundaryTarget target{};
        std::uint64_t generation_identity = 0;
        std::uint64_t prior_generation_identity = 0;
        std::uint64_t first_action_sequence = 0;
        std::uint64_t first_record_index = 0;
        std::uint64_t first_payload_offset = 0;
        std::uint32_t record_count = 0;
        std::uint32_t payload_bytes = 0;
        Status terminal_status = Status::ok;
        bool settled = false;
        std::array<std::byte, 3> reserved{};
    };

    struct LosslessRetention {
        LiveControlReplayRetentionPolicy policy{};
        std::unique_ptr<LiveControlRetainedAdmission[]> admissions{};
        std::unique_ptr<std::byte[]> payloads{};
        std::unique_ptr<std::size_t[]> export_order{};
        std::unique_ptr<std::byte[]> validation_state{};
        std::atomic<std::size_t> active_admissions{0};
        std::atomic<std::size_t> admission_count{0};
        std::atomic<std::size_t> payload_count{0};
        std::atomic<std::uint64_t> admission_close_sequence{kInvalidSequence};
        std::size_t export_count = 0;
        std::size_t replay_admission_index = 0;
    };

    struct Closure {
        LiveControlClosurePolicy policy{};
        std::unique_ptr<LosslessRetention> lossless{};
        std::unique_ptr<LiveControlActionRing> actions{};
        Generation rollback_generation{};
        std::unique_ptr<ProvisionalRecord[]> provisional_records{};
        std::size_t provisional_count = 0;
        std::unique_ptr<RetainedGeneration[]> retained_generations{};
        std::unique_ptr<RetainedRecord[]> retained_records{};
        std::unique_ptr<std::byte[]> retained_payloads{};
        std::unique_ptr<std::byte[]> checkpoint_state{};
        std::size_t checkpoint_state_bytes = 0;
        std::size_t retained_generation_count = 0;
        std::atomic<std::size_t> retained_generation_published{0};
        std::size_t retained_record_count = 0;
        std::size_t retained_payload_count = 0;
        std::size_t transaction_retained_begin = 0;
        std::atomic<bool> transaction_active{false};
        std::atomic<bool> host_claim{false};
        std::atomic<bool> replay_active{false};
        const LiveControlReplayArtifactView* replay_view = nullptr;
        std::size_t replay_generation_index = 0;
        std::size_t replay_action_index = 0;
        bool replay_history_applied = false;
        std::size_t transaction_replay_generation_begin = 0;
        Status replay_mismatch = Status::ok;
        std::uint64_t replay_mismatch_action_sequence = 0;
        bool rollback_has_generation = false;
        std::uint64_t prior_generation_identity = 0;
        LiveControlBoundaryTarget prior_target{};
        std::uint32_t prior_survivor_count = 0;
        std::atomic<bool> replay_eligible{true};
    };

    LiveControlPolicy policy{};
    std::uint64_t runtime_id = 0;
    std::uint64_t configuration_generation = 0;
    std::atomic<bool> admission_open{true};
    std::vector<std::unique_ptr<Mailbox>> mailboxes{};
    std::unique_ptr<Producer[]> producers{};
    std::size_t producer_count = 0;
    std::unique_ptr<RateTarget[]> rate_targets{};
    std::size_t rate_target_count = 0;
    std::unique_ptr<Candidate[]> candidates{};
    std::array<Generation, 2> generations{};
    std::atomic<std::uint8_t> active_generation{0};
    std::atomic<bool> has_generation{false};
    std::atomic<bool> host_boundary_started{false};
    std::atomic<std::uint64_t> last_host_boundary{0};
    std::atomic<std::uint64_t> committed{0};
    std::atomic<std::uint64_t> replaced{0};
    std::atomic<std::uint64_t> missed{0};
    std::atomic<std::uint64_t> stopped{0};
    std::atomic<std::uint64_t> inspection_version{0};
    std::atomic<std::uint64_t> latest_generation_identity{0};
    std::atomic<std::uint64_t> latest_frame_index{kInvalidSequence};
    std::atomic<std::uint64_t> latest_rate_sequence{kInvalidSequence};
    std::atomic<std::uint32_t> latest_reference_index{kInvalidIndex};
    std::atomic<std::uint32_t> latest_domain_index{kInvalidIndex};
    std::atomic<std::uint32_t> latest_phase_index{kInvalidIndex};
    std::atomic<std::uint32_t> latest_substep{kInvalidIndex};
    std::atomic<std::uint32_t> latest_survivor_count{0};
    std::atomic<LiveControlTargetKind> latest_target_kind{
        LiveControlTargetKind::host_frame};
    std::unique_ptr<Closure> closure{};
    std::size_t total_payload_storage_bytes = 0;
};

namespace {

[[nodiscard]] std::size_t find_mailbox(
    const LiveControlMailboxSet::Impl& impl,
    std::uint64_t identity) noexcept {
    for (std::size_t index = 0; index < impl.mailboxes.size(); ++index) {
        if (impl.mailboxes[index]->registration.mailbox_identity == identity) {
            return index;
        }
    }
    return impl.mailboxes.size();
}

[[nodiscard]] std::size_t total_record_capacity(
    const LiveControlMailboxSet::Impl& impl) noexcept {
    std::size_t total = 0;
    for (const auto& mailbox : impl.mailboxes) {
        total += mailbox->registration.record_capacity;
    }
    return total;
}

struct CheckpointLayout {
    std::size_t mailbox_offset = 0;
    std::size_t producer_offset = 0;
    std::size_t slot_offset = 0;
    std::size_t slot_payload_offset = 0;
    std::size_t generation_record_offset = 0;
    std::size_t generation_payload_offset = 0;
    std::size_t rate_target_offset = 0;
    std::size_t total_bytes = 0;
};

[[nodiscard]] bool checkpoint_layout(
    const LiveControlMailboxSet::Impl& impl,
    CheckpointLayout& layout) noexcept {
    layout = {};
    layout.mailbox_offset = kCheckpointHeaderBytes;
    std::size_t bytes = 0;
    if (!checked_multiply(
            impl.mailboxes.size(), kCheckpointMailboxBytes, bytes) ||
        !checked_add(layout.mailbox_offset, bytes, layout.producer_offset) ||
        !checked_multiply(
            impl.producer_count, kCheckpointProducerBytes, bytes) ||
        !checked_add(layout.producer_offset, bytes, layout.slot_offset) ||
        !checked_multiply(
            total_record_capacity(impl), kCheckpointSlotBytes, bytes) ||
        !checked_add(layout.slot_offset, bytes, layout.slot_payload_offset) ||
        !checked_add(
            layout.slot_payload_offset,
            impl.total_payload_storage_bytes,
            layout.generation_record_offset) ||
        !checked_multiply(
            total_record_capacity(impl),
            kCheckpointGenerationRecordBytes,
            bytes) ||
        !checked_add(
            layout.generation_record_offset,
            bytes,
            layout.generation_payload_offset) ||
        !checked_add(
            layout.generation_payload_offset,
            impl.total_payload_storage_bytes,
            layout.rate_target_offset) ||
        !checked_multiply(
            impl.rate_target_count, kCheckpointRateTargetBytes, bytes) ||
        !checked_add(layout.rate_target_offset, bytes, layout.total_bytes)) {
        layout = {};
        return false;
    }
    return true;
}

[[nodiscard]] bool update_kind_valid(LiveControlUpdateKind kind) noexcept {
    return kind >= LiveControlUpdateKind::scenario_parameters &&
        kind <= LiveControlUpdateKind::clear_fault;
}

[[nodiscard]] bool target_valid(
    const LiveControlMailboxSet::Impl& impl,
    const LiveControlUpdateRecord& update) noexcept {
    if (update.target_kind == LiveControlTargetKind::host_frame) {
        return update.target_frame_index != kInvalidSequence &&
            update.reference_release_index == kInvalidIndex &&
            update.rate_domain_registration_index == kInvalidIndex &&
            update.phase_index == kInvalidIndex &&
            update.rate_substep_ordinal == kInvalidIndex &&
            update.rate_release_sequence == kInvalidSequence;
    }
    if (update.target_kind != LiveControlTargetKind::rate_release ||
        update.target_frame_index != kInvalidSequence ||
        update.reference_release_index >= impl.rate_target_count) {
        return false;
    }
    const auto& target = impl.rate_targets[update.reference_release_index];
    return update.rate_domain_registration_index ==
               target.domain_registration_index &&
        update.phase_index == target.phase_index &&
        update.rate_substep_ordinal == target.substep_ordinal &&
        update.rate_release_sequence == target.release_sequence;
}

void hash_u64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        hash ^= static_cast<std::uint8_t>(value & 0xffu);
        hash *= kFnvPrime;
        value >>= 8u;
    }
}

void hash_target(
    std::uint64_t& hash,
    const LiveControlBoundaryTarget& target) noexcept {
    hash_u64(hash, static_cast<std::uint8_t>(target.kind));
    hash_u64(hash, target.frame_index);
    hash_u64(hash, target.rate_release_sequence);
    hash_u64(hash, target.reference_release_index);
    hash_u64(hash, target.rate_domain_registration_index);
    hash_u64(hash, target.phase_index);
    hash_u64(hash, target.rate_substep_ordinal);
}

[[nodiscard]] LiveControlRecordStatus public_status(
    LiveControlMailboxSet::Impl::SlotState state) noexcept {
    using State = LiveControlMailboxSet::Impl::SlotState;
    switch (state) {
    case State::staged:
    case State::boundary_owned:
        return LiveControlRecordStatus::staged;
    case State::committed:
        return LiveControlRecordStatus::committed;
    case State::replaced:
        return LiveControlRecordStatus::replaced;
    case State::missed:
        return LiveControlRecordStatus::missed;
    case State::stopped:
        return LiveControlRecordStatus::stopped;
    case State::rolled_back:
        return LiveControlRecordStatus::rolled_back;
    case State::free:
    case State::writing:
        return LiveControlRecordStatus::staged;
    }
    return LiveControlRecordStatus::staged;
}

[[nodiscard]] bool structurally_valid(
    const LiveControlMailboxSet::Impl& impl,
    const LiveControlMailboxSet::Impl::Mailbox& mailbox,
    const LiveControlMailboxSet::Impl::Producer& producer,
    const LiveControlUpdateRecord& update,
    std::span<const std::byte> payload) noexcept {
    const auto alignment = update.payload_alignment;
    const auto alignment_valid = alignment != 0 &&
        alignment <= live_control_payload_alignment_limit &&
        (alignment & (alignment - 1u)) == 0 &&
        (payload.size() % alignment) == 0;
    // The raw M22-01 empty clear-fault form remains valid. M22-04 also permits
    // a canonical typed clear-fault payload so an application can carry a
    // fixed fault identity and authorization value through the same envelope
    // and digest checks as every other typed update.
    const auto zero_payload_valid = !payload.empty() ||
        update.update_kind == LiveControlUpdateKind::clear_fault;
    return update.schema_version == live_control_schema_version &&
        update.record_size == sizeof(LiveControlUpdateRecord) &&
        update.mailbox_sequence == 0 &&
        update.runtime_id == impl.runtime_id &&
        update.configuration_generation == impl.configuration_generation &&
        update.mailbox_identity == producer.mailbox_identity &&
        update.mailbox_identity == mailbox.registration.mailbox_identity &&
        update.producer_identity == producer.producer_identity &&
        update.producer_sequence != 0 &&
        update.payload_bytes == payload.size() &&
        payload.size() <= mailbox.registration.payload_bytes_per_record &&
        update.payload_digest == live_control_payload_digest(payload) &&
        update.policy_flags ==
            live_control_payload_canonical_little_endian &&
        update_kind_valid(update.update_kind) && alignment_valid &&
        zero_payload_valid && target_valid(impl, update) &&
        all_zero(update.reserved);
}

void count_result(
    LiveControlMailboxSet::Impl::Mailbox& mailbox,
    LiveControlAdmissionResult result) noexcept {
    switch (result) {
    case LiveControlAdmissionResult::accepted:
        increment(mailbox.accepted);
        return;
    case LiveControlAdmissionResult::invalid:
        increment(mailbox.invalid);
        return;
    case LiveControlAdmissionResult::full:
        increment(mailbox.full);
        return;
    case LiveControlAdmissionResult::busy:
        increment(mailbox.busy);
        return;
    case LiveControlAdmissionResult::stale:
        increment(mailbox.stale);
        return;
    case LiveControlAdmissionResult::stopped:
        increment(mailbox.stopped);
        return;
    case LiveControlAdmissionResult::exhausted:
        increment(mailbox.exhausted);
        return;
    case LiveControlAdmissionResult::missed:
        increment(mailbox.missed);
        return;
    }
}

[[nodiscard]] bool target_closed(
    const LiveControlMailboxSet::Impl& impl,
    const LiveControlUpdateRecord& update) noexcept {
    if (update.target_kind == LiveControlTargetKind::host_frame) {
        return impl.host_boundary_started.load(std::memory_order_acquire) &&
            update.target_frame_index <=
                impl.last_host_boundary.load(std::memory_order_acquire);
    }
    return update.reference_release_index >= impl.rate_target_count ||
        impl.rate_targets[update.reference_release_index].closed.load(
            std::memory_order_acquire);
}

[[nodiscard]] bool terminal_state(
    LiveControlMailboxSet::Impl::SlotState state) noexcept {
    using State = LiveControlMailboxSet::Impl::SlotState;
    return state == State::committed || state == State::replaced ||
        state == State::missed || state == State::stopped ||
        state == State::rolled_back;
}

[[nodiscard]] LiveControlBoundaryTarget target_from_record(
    const LiveControlUpdateRecord& record) noexcept;
[[nodiscard]] LiveControlActionRecord base_action(
    const LiveControlMailboxSet::Impl& impl) noexcept;
void emit_action(
    LiveControlMailboxSet::Impl& impl,
    LiveControlActionRecord action,
    std::uint64_t* sequence = nullptr) noexcept;
[[nodiscard]] LiveControlActionReason reason_for_admission(
    LiveControlAdmissionResult result) noexcept;

} // namespace

LiveControlMailboxSet::LiveControlMailboxSet(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

LiveControlMailboxSet::~LiveControlMailboxSet() = default;

Status LiveControlMailboxSet::create(
    const LiveControlPolicy& policy, const LiveControlClosurePolicy& closure_policy,
    bool closure_enabled, const LiveControlReplayRetentionPolicy& retention_policy,
    std::uint64_t runtime_id, std::uint64_t configuration_generation,
    std::size_t memory_budget_bytes,
    std::span<const LiveControlMailboxRegistration> mailbox_declarations,
    std::span<const LiveControlProducerRegistration> producer_declarations,
    std::span<const ReferenceRelease> rate_releases,
    std::unique_ptr<LiveControlMailboxSet>& output, const char*& diagnostic) noexcept {
    output.reset();
    diagnostic = nullptr;
    if (policy.schema_version != live_control_schema_version ||
        policy.struct_size != sizeof(LiveControlPolicy) ||
        policy.policy_identity == 0 || runtime_id == 0 ||
        configuration_generation == 0 ||
        policy.mailbox_capacity == 0 ||
        policy.mailbox_capacity > live_control_mailbox_capacity_limit ||
        policy.producer_capacity == 0 ||
        policy.producer_capacity > live_control_producer_capacity_limit ||
        policy.record_capacity == 0 ||
        policy.record_capacity > live_control_record_capacity_limit ||
        policy.payload_bytes_per_record == 0 ||
        policy.payload_bytes_per_record > live_control_payload_bytes_limit ||
        policy.total_payload_storage_bytes == 0 ||
        policy.total_payload_storage_bytes > live_control_total_storage_limit ||
        policy.total_payload_storage_bytes >
            std::numeric_limits<std::size_t>::max() ||
        policy.admission_policy != LiveControlAdmissionPolicy::reject_new ||
        policy.reset_rule != LiveControlResetRule::discard_with_runtime ||
        !all_zero(policy.reserved) || mailbox_declarations.empty() ||
        mailbox_declarations.size() > policy.mailbox_capacity ||
        producer_declarations.empty() ||
        producer_declarations.size() > policy.producer_capacity) {
        diagnostic = "live-control policy or declaration counts are invalid";
        return Status::invalid_config;
    }
    if (closure_enabled &&
        (closure_policy.schema_version !=
             live_control_action_schema_version ||
         closure_policy.struct_size != sizeof(LiveControlClosurePolicy) ||
         closure_policy.policy_identity == 0 ||
         closure_policy.action_capacity >
             live_control_action_capacity_limit ||
         closure_policy.retained_generation_capacity >
             live_control_retained_generation_capacity_limit ||
         closure_policy.retained_record_capacity >
             live_control_record_capacity_limit ||
         closure_policy.retained_payload_bytes >
             live_control_total_storage_limit ||
         closure_policy.replay_record_capacity >
             live_control_record_capacity_limit ||
         closure_policy.replay_max_bytes >
             live_control_replay_absolute_max_bytes ||
         closure_policy.rollback_rule !=
             LiveControlRollbackRule::restore_step_entry_generation ||
         !all_zero(closure_policy.reserved) ||
         (closure_policy.replay_enabled &&
          (closure_policy.retained_generation_capacity == 0 ||
           closure_policy.retained_record_capacity == 0 ||
           closure_policy.retained_payload_bytes == 0 ||
           closure_policy.replay_record_capacity == 0 ||
           closure_policy.replay_max_bytes == 0)))) {
        diagnostic = "live-control closure policy is invalid";
        return Status::invalid_config;
    }

    if (retention_policy.policy_identity != 0 &&
        (!closure_enabled || !closure_policy.replay_enabled ||
         retention_policy.schema_version !=
             live_control_lossless_replay_schema_version ||
         retention_policy.struct_size != sizeof(retention_policy) ||
         retention_policy.admission_capacity == 0 ||
         retention_policy.admission_capacity > live_control_action_capacity_limit ||
         retention_policy.payload_capacity_bytes == 0 ||
         retention_policy.payload_capacity_bytes > live_control_total_storage_limit ||
         !all_zero(retention_policy.reserved))) {
        diagnostic = "trusted replay retention policy is invalid";
        return Status::invalid_config;
    }
    std::size_t total_records = 0;
    std::size_t total_payload = 0;
    for (std::size_t index = 0; index < mailbox_declarations.size(); ++index) {
        const auto& declaration = mailbox_declarations[index];
        if (declaration.schema_version != live_control_schema_version ||
            declaration.struct_size != sizeof(LiveControlMailboxRegistration) ||
            declaration.mailbox_identity == 0 ||
            declaration.record_capacity == 0 ||
            declaration.payload_bytes_per_record == 0 ||
            declaration.payload_bytes_per_record >
                policy.payload_bytes_per_record ||
            !all_zero(declaration.reserved)) {
            diagnostic = "live-control mailbox declaration is invalid";
            return Status::invalid_config;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (mailbox_declarations[prior].mailbox_identity ==
                declaration.mailbox_identity) {
                diagnostic = "live-control mailbox identity is duplicated";
                return Status::invalid_config;
            }
        }
        std::size_t storage = 0;
        if (!checked_add(total_records, declaration.record_capacity,
                         total_records) ||
            !checked_multiply(declaration.record_capacity,
                              declaration.payload_bytes_per_record, storage) ||
            !checked_add(total_payload, storage, total_payload) ||
            total_records > policy.record_capacity ||
            total_payload > policy.total_payload_storage_bytes) {
            diagnostic = "live-control mailbox storage exceeds policy bounds";
            return Status::capacity_exceeded;
        }
    }

    for (std::size_t index = 0; index < producer_declarations.size(); ++index) {
        const auto& declaration = producer_declarations[index];
        if (declaration.schema_version != live_control_schema_version ||
            declaration.struct_size != sizeof(LiveControlProducerRegistration) ||
            declaration.mailbox_identity == 0 ||
            declaration.producer_identity == 0 ||
            declaration.first_sequence == 0 ||
            declaration.first_sequence == kInvalidSequence ||
            declaration.reserved != 0) {
            diagnostic = "live-control producer declaration is invalid";
            return Status::invalid_config;
        }
        bool mailbox_found = false;
        for (const auto& mailbox : mailbox_declarations) {
            mailbox_found = mailbox_found ||
                mailbox.mailbox_identity == declaration.mailbox_identity;
        }
        if (!mailbox_found) {
            diagnostic = "live-control producer names an unknown mailbox";
            return Status::invalid_config;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (producer_declarations[prior].producer_identity ==
                declaration.producer_identity) {
                diagnostic = "live-control producer identity is duplicated";
                return Status::invalid_config;
            }
        }
    }

    std::size_t minimum_control_bytes =
        sizeof(LiveControlMailboxSet) + sizeof(Impl);
    std::size_t bytes = 0;
    if (!checked_multiply(
            mailbox_declarations.size(),
            sizeof(std::unique_ptr<Impl::Mailbox>),
            bytes) ||
        !checked_add(minimum_control_bytes, bytes, minimum_control_bytes)) {
        diagnostic = "live-control control storage overflows";
        return Status::capacity_exceeded;
    }
    for (const auto& declaration : mailbox_declarations) {
        if (!checked_add(
                minimum_control_bytes,
                sizeof(Impl::Mailbox),
                minimum_control_bytes) ||
            !checked_multiply(
                declaration.record_capacity,
                sizeof(Impl::Slot),
                bytes) ||
            !checked_add(minimum_control_bytes, bytes, minimum_control_bytes) ||
            !checked_multiply(
                declaration.record_capacity,
                declaration.payload_bytes_per_record,
                bytes) ||
            !checked_add(minimum_control_bytes, bytes, minimum_control_bytes)) {
            diagnostic = "live-control control storage overflows";
            return Status::capacity_exceeded;
        }
    }
    if (!checked_multiply(
            producer_declarations.size(), sizeof(Impl::Producer), bytes) ||
        !checked_add(minimum_control_bytes, bytes, minimum_control_bytes) ||
        !checked_multiply(
            rate_releases.size(), sizeof(Impl::RateTarget), bytes) ||
        !checked_add(minimum_control_bytes, bytes, minimum_control_bytes) ||
        !checked_multiply(total_records, sizeof(Impl::Candidate), bytes) ||
        !checked_add(minimum_control_bytes, bytes, minimum_control_bytes) ||
        !checked_multiply(total_records, sizeof(LiveControlRecordView), bytes) ||
        !checked_multiply(bytes, 2u, bytes) ||
        !checked_add(minimum_control_bytes, bytes, minimum_control_bytes) ||
        !checked_multiply(total_payload, 2u, bytes) ||
        !checked_add(minimum_control_bytes, bytes, minimum_control_bytes)) {
        diagnostic = "live-control control storage overflows";
        return Status::capacity_exceeded;
    }
    if (closure_enabled) {
        std::size_t closure_bytes = sizeof(Impl::Closure) +
            sizeof(LiveControlActionRing);
        std::size_t checkpoint_bytes = kCheckpointHeaderBytes;
        if (!checked_multiply(
                mailbox_declarations.size(),
                kCheckpointMailboxBytes,
                bytes) ||
            !checked_add(checkpoint_bytes, bytes, checkpoint_bytes) ||
            !checked_multiply(
                producer_declarations.size(),
                kCheckpointProducerBytes,
                bytes) ||
            !checked_add(checkpoint_bytes, bytes, checkpoint_bytes) ||
            !checked_multiply(
                total_records, kCheckpointSlotBytes, bytes) ||
            !checked_add(checkpoint_bytes, bytes, checkpoint_bytes) ||
            !checked_add(checkpoint_bytes, total_payload, checkpoint_bytes) ||
            !checked_multiply(
                total_records,
                kCheckpointGenerationRecordBytes,
                bytes) ||
            !checked_add(checkpoint_bytes, bytes, checkpoint_bytes) ||
            !checked_add(checkpoint_bytes, total_payload, checkpoint_bytes) ||
            !checked_multiply(
                rate_releases.size(), kCheckpointRateTargetBytes, bytes) ||
            !checked_add(checkpoint_bytes, bytes, checkpoint_bytes) ||
            !checked_add(closure_bytes, checkpoint_bytes, closure_bytes)) {
            diagnostic = "live-control checkpoint storage overflows";
            return Status::capacity_exceeded;
        }
        if (!checked_multiply(
                closure_policy.action_capacity,
                LiveControlActionRing::slot_size(),
                bytes) ||
            !checked_add(closure_bytes, bytes, closure_bytes) ||
            !checked_multiply(
                total_records,
                sizeof(LiveControlRecordView),
                bytes) ||
            !checked_add(closure_bytes, bytes, closure_bytes) ||
            !checked_add(closure_bytes, total_payload, closure_bytes) ||
            !checked_multiply(
                total_records,
                sizeof(Impl::ProvisionalRecord),
                bytes) ||
            !checked_add(closure_bytes, bytes, closure_bytes) ||
            !checked_multiply(
                closure_policy.retained_generation_capacity,
                sizeof(Impl::RetainedGeneration),
                bytes) ||
            !checked_add(closure_bytes, bytes, closure_bytes) ||
            !checked_multiply(
                closure_policy.retained_record_capacity,
                sizeof(Impl::RetainedRecord),
                bytes) ||
            !checked_add(closure_bytes, bytes, closure_bytes) ||
            !checked_add(
                closure_bytes,
                closure_policy.retained_payload_bytes,
                closure_bytes) ||
            !checked_add(
                minimum_control_bytes,
                closure_bytes,
                minimum_control_bytes)) {
            diagnostic = "live-control closure storage overflows";
            return Status::capacity_exceeded;
        }
        if (retention_policy.policy_identity != 0) {
            std::size_t extra = sizeof(Impl::LosslessRetention);
            if (!checked_multiply(retention_policy.admission_capacity,
                                  sizeof(LiveControlRetainedAdmission) +
                                      sizeof(std::size_t),
                                  bytes) ||
                !checked_add(extra, bytes, extra) ||
                !checked_add(extra, retention_policy.payload_capacity_bytes, extra) ||
                !checked_add(extra, checkpoint_bytes, extra) ||
                !checked_add(minimum_control_bytes, extra, minimum_control_bytes)) {
                diagnostic = "trusted replay retention storage overflows";
                return Status::capacity_exceeded;
            }
        }
    }
    if (minimum_control_bytes > memory_budget_bytes) {
        diagnostic = "live-control control storage exceeds Runtime memory budget";
        return Status::invalid_config;
    }

    try {
        auto impl = std::make_unique<Impl>();
        impl->policy = policy;
        impl->runtime_id = runtime_id;
        impl->configuration_generation = configuration_generation;
        impl->total_payload_storage_bytes = total_payload;
        impl->mailboxes.reserve(mailbox_declarations.size());
        for (const auto& declaration : mailbox_declarations) {
            auto mailbox = std::make_unique<Impl::Mailbox>();
            mailbox->registration = declaration;
            mailbox->slots =
                std::make_unique<Impl::Slot[]>(declaration.record_capacity);
            std::size_t storage = 0;
            (void)checked_multiply(declaration.record_capacity,
                                   declaration.payload_bytes_per_record,
                                   storage);
            mailbox->payload_storage = std::make_unique<std::byte[]>(storage);
            std::fill_n(mailbox->payload_storage.get(), storage, std::byte{0});
            impl->mailboxes.push_back(std::move(mailbox));
        }
        impl->producer_count = producer_declarations.size();
        impl->producers =
            std::make_unique<Impl::Producer[]>(impl->producer_count);
        for (std::size_t index = 0; index < impl->producer_count; ++index) {
            const auto& declaration = producer_declarations[index];
            auto& producer = impl->producers[index];
            producer.mailbox_identity = declaration.mailbox_identity;
            producer.producer_identity = declaration.producer_identity;
            producer.mailbox_index = static_cast<std::uint32_t>(
                find_mailbox(*impl, declaration.mailbox_identity));
            producer.first_sequence = declaration.first_sequence;
            producer.next_sequence.store(
                declaration.first_sequence, std::memory_order_relaxed);
            ++impl->mailboxes[producer.mailbox_index]->producer_count;
        }
        impl->rate_target_count = rate_releases.size();
        impl->rate_targets =
            std::make_unique<Impl::RateTarget[]>(impl->rate_target_count);
        for (std::size_t index = 0; index < rate_releases.size(); ++index) {
            const auto& release = rate_releases[index];
            if (release.domain_registration_index > kInvalidIndex) {
                diagnostic = "live-control rate target identity overflows";
                return Status::capacity_exceeded;
            }
            auto& target = impl->rate_targets[index];
            target.release_time_ns = release.release_time_ns;
            target.release_sequence = release.domain_release_sequence;
            target.domain_registration_index =
                static_cast<std::uint32_t>(release.domain_registration_index);
            target.phase_index = release.phase.index();
            target.substep_ordinal = release.substep_ordinal;
        }
        impl->candidates = std::make_unique<Impl::Candidate[]>(total_records);
        for (auto& generation : impl->generations) {
            generation.records =
                std::make_unique<LiveControlRecordView[]>(total_records);
            generation.payloads = std::make_unique<std::byte[]>(total_payload);
            std::fill_n(
                generation.payloads.get(), total_payload, std::byte{0});
        }
        if (closure_enabled) {
            impl->closure = std::make_unique<Impl::Closure>();
            auto& closure = *impl->closure;
            closure.policy = closure_policy;
            closure.actions = std::make_unique<LiveControlActionRing>(
                closure_policy.action_capacity);
            closure.rollback_generation.records =
                std::make_unique<LiveControlRecordView[]>(total_records);
            closure.rollback_generation.payloads =
                std::make_unique<std::byte[]>(total_payload);
            std::fill_n(
                closure.rollback_generation.payloads.get(),
                total_payload,
                std::byte{0});
            closure.provisional_records =
                std::make_unique<Impl::ProvisionalRecord[]>(total_records);
            closure.retained_generations =
                std::make_unique<Impl::RetainedGeneration[]>(
                    closure_policy.retained_generation_capacity);
            closure.retained_records =
                std::make_unique<Impl::RetainedRecord[]>(
                    closure_policy.retained_record_capacity);
            closure.retained_payloads = std::make_unique<std::byte[]>(
                closure_policy.retained_payload_bytes);
            std::fill_n(
                closure.retained_payloads.get(),
                closure_policy.retained_payload_bytes,
                std::byte{0});
            CheckpointLayout layout;
            if (!checkpoint_layout(*impl, layout)) {
                diagnostic = "live-control checkpoint layout overflows";
                return Status::capacity_exceeded;
            }
            if (retention_policy.policy_identity != 0) {
                closure.lossless = std::make_unique<Impl::LosslessRetention>();
                auto& journal = *closure.lossless;
                journal.policy = retention_policy;
                journal.admissions = std::make_unique<LiveControlRetainedAdmission[]>(
                    retention_policy.admission_capacity);
                journal.payloads = std::make_unique<std::byte[]>(
                    retention_policy.payload_capacity_bytes);
                journal.export_order = std::make_unique<std::size_t[]>(
                    retention_policy.admission_capacity);
                journal.validation_state =
                    std::make_unique<std::byte[]>(layout.total_bytes);
            }
            closure.checkpoint_state_bytes = layout.total_bytes;
            closure.checkpoint_state =
                std::make_unique<std::byte[]>(layout.total_bytes);
            std::fill_n(
                closure.checkpoint_state.get(),
                layout.total_bytes,
                std::byte{0});
        }
        output = std::unique_ptr<LiveControlMailboxSet>(
            new LiveControlMailboxSet(std::move(impl)));
    } catch (const std::bad_alloc&) {
        diagnostic = "live-control mailbox allocation failed";
        return Status::resource_exhausted;
    } catch (...) {
        diagnostic = "live-control mailbox construction failed";
        return Status::internal_error;
    }
    return Status::ok;
}

Status LiveControlMailboxSet::producer_handle(
    std::uint64_t mailbox_identity,
    std::uint64_t producer_identity,
    LiveControlProducerHandle& handle) const noexcept {
    if (!impl_) {
        return Status::invalid_state;
    }
    for (std::size_t index = 0; index < impl_->producer_count; ++index) {
        const auto& producer = impl_->producers[index];
        if (producer.mailbox_identity == mailbox_identity &&
            producer.producer_identity == producer_identity) {
            handle = {
                impl_->runtime_id,
                impl_->configuration_generation,
                mailbox_identity,
                producer_identity,
                static_cast<std::uint32_t>(index),
                0,
            };
            return Status::ok;
        }
    }
    return Status::invalid_handle;
}

LiveControlAdmissionResult LiveControlMailboxSet::stage(
    LiveControlProducerHandle handle,
    const LiveControlUpdateRecord& update,
    std::span<const std::byte> payload) noexcept {
    struct AdmissionGuard {
        Impl::LosslessRetention* journal = nullptr;
        ~AdmissionGuard() {
            if (journal) {
                journal->active_admissions.fetch_sub(1, std::memory_order_seq_cst);
            }
        }
    } admission_guard;
    if (impl_ && impl_->closure && impl_->closure->lossless) {
        admission_guard.journal = impl_->closure->lossless.get();
        admission_guard.journal->active_admissions.fetch_add(1,
                                                             std::memory_order_seq_cst);
        if (impl_->closure->host_claim.load(std::memory_order_seq_cst)) {
            // Quiescent export/restore owns all state: this busy result has no effect.
            return LiveControlAdmissionResult::busy;
        }
    }
    std::size_t owner = impl_ ? impl_->mailboxes.size() : 0;
    std::uint32_t assigned_slot = kInvalidIndex;
    std::uint64_t action_sequence = kInvalidSequence;
    const auto finish = [&](LiveControlAdmissionResult result) noexcept {
        if (impl_ && impl_->closure && impl_->closure->lossless &&
            !impl_->closure->replay_active.load(std::memory_order_acquire)) {
            retain_admission(handle, update, payload, result, owner, assigned_slot,
                             action_sequence);
        }
        return result;
    };
    if (!impl_ || !handle.valid() ||
        handle.runtime_id != impl_->runtime_id ||
        handle.configuration_generation != impl_->configuration_generation ||
        handle.producer_index >= impl_->producer_count) {
        return finish(LiveControlAdmissionResult::stale);
    }
    auto& producer = impl_->producers[handle.producer_index];
    if (producer.mailbox_identity != handle.mailbox_identity ||
        producer.producer_identity != handle.producer_identity ||
        producer.mailbox_index >= impl_->mailboxes.size()) {
        return finish(LiveControlAdmissionResult::stale);
    }
    owner = producer.mailbox_index;
    auto& mailbox = *impl_->mailboxes[owner];
    if (!impl_->admission_open.load(std::memory_order_acquire)) {
        count_result(mailbox, LiveControlAdmissionResult::stopped);
        return finish(LiveControlAdmissionResult::stopped);
    }
    const auto observed_producer_sequence =
        producer.next_sequence.load(std::memory_order_acquire);
    if (observed_producer_sequence == kInvalidSequence) {
        count_result(mailbox, LiveControlAdmissionResult::exhausted);
        return finish(LiveControlAdmissionResult::exhausted);
    }
    if (update.producer_sequence != observed_producer_sequence ||
        update.runtime_id != handle.runtime_id ||
        update.configuration_generation != handle.configuration_generation ||
        update.mailbox_identity != handle.mailbox_identity ||
        update.producer_identity != handle.producer_identity) {
        count_result(mailbox, LiveControlAdmissionResult::stale);
        return finish(LiveControlAdmissionResult::stale);
    }
    if (!structurally_valid(*impl_, mailbox, producer, update, payload)) {
        count_result(mailbox, LiveControlAdmissionResult::invalid);
        return finish(LiveControlAdmissionResult::invalid);
    }
    if (mailbox.reservation.test_and_set(std::memory_order_acquire)) {
        count_result(mailbox, LiveControlAdmissionResult::busy);
        return finish(LiveControlAdmissionResult::busy);
    }
    FlagGuard guard(mailbox.reservation);
    if (!impl_->admission_open.load(std::memory_order_acquire)) {
        count_result(mailbox, LiveControlAdmissionResult::stopped);
        return finish(LiveControlAdmissionResult::stopped);
    }
    const auto expected_producer_sequence =
        producer.next_sequence.load(std::memory_order_relaxed);
    if (expected_producer_sequence == kInvalidSequence) {
        count_result(mailbox, LiveControlAdmissionResult::exhausted);
        return finish(LiveControlAdmissionResult::exhausted);
    }
    if (update.producer_sequence != expected_producer_sequence ||
        update.runtime_id != handle.runtime_id ||
        update.configuration_generation != handle.configuration_generation ||
        update.mailbox_identity != handle.mailbox_identity ||
        update.producer_identity != handle.producer_identity) {
        count_result(mailbox, LiveControlAdmissionResult::stale);
        return finish(LiveControlAdmissionResult::stale);
    }
    const auto mailbox_sequence =
        mailbox.next_sequence.load(std::memory_order_relaxed);
    if (mailbox_sequence == kInvalidSequence) {
        count_result(mailbox, LiveControlAdmissionResult::exhausted);
        return finish(LiveControlAdmissionResult::exhausted);
    }

    using SlotState = Impl::SlotState;
    std::size_t slot_index = mailbox.registration.record_capacity;
    for (std::size_t index = 0;
         index < mailbox.registration.record_capacity; ++index) {
        auto expected_state = SlotState::free;
        if (mailbox.slots[index].state.compare_exchange_strong(
                expected_state,
                SlotState::writing,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            slot_index = index;
            break;
        }
    }
    if (slot_index == mailbox.registration.record_capacity) {
        bool expected_reclaiming = false;
        if (mailbox.reclaiming.compare_exchange_strong(
                expected_reclaiming,
                true,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            if (mailbox.inspection_readers.load(std::memory_order_acquire) ==
                0) {
                for (std::size_t index = 0;
                     index < mailbox.registration.record_capacity; ++index) {
                    auto state = mailbox.slots[index].state.load(
                        std::memory_order_acquire);
                    if (terminal_state(state) &&
                        mailbox.slots[index].state.compare_exchange_strong(
                            state,
                            SlotState::writing,
                            std::memory_order_acq_rel,
                            std::memory_order_relaxed)) {
                        slot_index = index;
                        break;
                    }
                }
            }
            mailbox.reclaiming.store(false, std::memory_order_release);
        }
    }
    if (slot_index == mailbox.registration.record_capacity) {
        count_result(mailbox, LiveControlAdmissionResult::full);
        return finish(LiveControlAdmissionResult::full);
    }

    const auto stride = static_cast<std::size_t>(
        mailbox.registration.payload_bytes_per_record);
    auto* destination = mailbox.payload_storage.get() + slot_index * stride;
    std::copy(payload.begin(), payload.end(), destination);
    std::fill(
        destination + static_cast<std::ptrdiff_t>(payload.size()),
        destination + static_cast<std::ptrdiff_t>(stride),
        std::byte{0});
    auto committed = update;
    committed.mailbox_sequence = mailbox_sequence;
    mailbox.slots[slot_index].record = committed;
    mailbox.slots[slot_index].terminal_generation.store(
        0, std::memory_order_relaxed);
    assigned_slot = static_cast<std::uint32_t>(slot_index);
    if (impl_->closure && impl_->closure->lossless &&
        !impl_->closure->actions->reserve_sequence(action_sequence)) {
        impl_->closure->replay_eligible.store(false, std::memory_order_release);
    }
    mailbox.occupancy.fetch_add(1, std::memory_order_relaxed);
    mailbox.slots[slot_index].state.store(
        SlotState::staged, std::memory_order_release);
    auto admission_result = LiveControlAdmissionResult::accepted;
    if (!impl_->admission_open.load(std::memory_order_acquire)) {
        auto expected_state = SlotState::staged;
        if (mailbox.slots[slot_index].state.compare_exchange_strong(
                expected_state,
                SlotState::stopped,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            mailbox.occupancy.fetch_sub(1, std::memory_order_relaxed);
            increment(impl_->stopped);
            admission_result = LiveControlAdmissionResult::stopped;
        }
    } else if (target_closed(*impl_, committed)) {
        auto expected_state = SlotState::staged;
        if (mailbox.slots[slot_index].state.compare_exchange_strong(
                expected_state,
                SlotState::missed,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            mailbox.occupancy.fetch_sub(1, std::memory_order_relaxed);
            increment(impl_->missed);
            admission_result = LiveControlAdmissionResult::missed;
        }
    }
    mailbox.next_sequence.store(mailbox_sequence + 1, std::memory_order_release);
    producer.next_sequence.store(
        expected_producer_sequence + 1, std::memory_order_release);
    count_result(mailbox, admission_result);
    return finish(admission_result);
}

void LiveControlMailboxSet::record_admission(
    LiveControlProducerHandle producer_handle,
    const LiveControlUpdateRecord& update,
    LiveControlAdmissionResult result) noexcept {
    if (!impl_ || !impl_->closure || impl_->closure->lossless ||
        impl_->closure->replay_active.load(std::memory_order_acquire)) {
        return;
    }
    const LiveControlUpdateRecord* canonical = nullptr;
    if ((result == LiveControlAdmissionResult::accepted ||
         result == LiveControlAdmissionResult::missed) &&
        producer_handle.producer_index < impl_->producer_count) {
        const auto& producer = impl_->producers[
            producer_handle.producer_index];
        if (producer.mailbox_index < impl_->mailboxes.size()) {
            const auto& mailbox = *impl_->mailboxes[producer.mailbox_index];
            for (std::size_t index = 0;
                 index < mailbox.registration.record_capacity;
                 ++index) {
                const auto& candidate = mailbox.slots[index].record;
                if (candidate.producer_identity ==
                        producer_handle.producer_identity &&
                    candidate.producer_sequence == update.producer_sequence) {
                    canonical = &candidate;
                    break;
                }
            }
        }
    }
    auto action = base_action(*impl_);
    action.admission_result = result;
    action.action = LiveControlActionId::admission;
    action.stage = LiveControlActionStage::attempt;
    action.reason = reason_for_admission(result);
    action.result = result == LiveControlAdmissionResult::accepted
        ? LiveControlActionResult::accepted
        : LiveControlActionResult::rejected;
    action.replay_eligible = impl_->closure->replay_eligible.load(
        std::memory_order_acquire);
    if (canonical) {
        action.target = target_from_record(*canonical);
        action.mailbox_identity = canonical->mailbox_identity;
        action.producer_identity = canonical->producer_identity;
        action.mailbox_sequence = canonical->mailbox_sequence;
        action.producer_sequence = canonical->producer_sequence;
        action.payload_digest = canonical->payload_digest;
        action.payload_bytes = canonical->payload_bytes;
        action.update_kind = canonical->update_kind;
        action.record_status = result == LiveControlAdmissionResult::missed
            ? LiveControlRecordStatus::missed
            : LiveControlRecordStatus::staged;
    }
    emit_action(*impl_, action);
    if (result == LiveControlAdmissionResult::missed && canonical) {
        action.action = LiveControlActionId::missed;
        action.stage = LiveControlActionStage::terminal;
        action.reason = LiveControlActionReason::missed;
        action.result = LiveControlActionResult::settled;
        action.record_status = LiveControlRecordStatus::missed;
        emit_action(*impl_, action);
    }
}

void LiveControlMailboxSet::retain_admission(LiveControlProducerHandle handle,
                                             const LiveControlUpdateRecord& update,
                                             std::span<const std::byte> payload,
                                             LiveControlAdmissionResult result,
                                             std::size_t owner,
                                             std::uint32_t slot_index,
                                             std::uint64_t action_sequence) noexcept {
    auto& closure = *impl_->closure;
    auto& journal = *closure.lossless;
    LiveControlRetainedAdmission retained;
    retained.record = update;
    retained.slot_index = slot_index;
    retained.outcome = result;
    retained.counter_changed = owner < impl_->mailboxes.size();
    if (retained.counter_changed) {
        retained.mailbox_identity =
            impl_->mailboxes[owner]->registration.mailbox_identity;
        retained.producer_identity = handle.producer_identity;
    }
    if (slot_index != kInvalidIndex) {
        retained.record = impl_->mailboxes[owner]->slots[slot_index].record;
    } else {
        payload = {};
        if (!closure.actions->reserve_sequence(action_sequence)) {
            closure.replay_eligible.store(false, std::memory_order_release);
        }
    }
    retained.action_sequence = action_sequence;
    auto action = base_action(*impl_);
    action.admission_result = result;
    action.reason = reason_for_admission(result);
    action.result = result == LiveControlAdmissionResult::accepted
                        ? LiveControlActionResult::accepted
                        : LiveControlActionResult::rejected;
    // Schema-1 rejection telemetry stays payload-free and owner-free.
    if (result == LiveControlAdmissionResult::accepted ||
        result == LiveControlAdmissionResult::missed) {
        const auto& record = retained.record;
        action.target = target_from_record(record);
        action.mailbox_identity = record.mailbox_identity;
        action.producer_identity = record.producer_identity;
        action.mailbox_sequence = record.mailbox_sequence;
        action.producer_sequence = record.producer_sequence;
        action.payload_digest = record.payload_digest;
        action.payload_bytes = record.payload_bytes;
        action.update_kind = record.update_kind;
        action.record_status = result == LiveControlAdmissionResult::missed
                                   ? LiveControlRecordStatus::missed
                                   : LiveControlRecordStatus::staged;
    }
    const auto index = journal.admission_count.fetch_add(1, std::memory_order_relaxed);
    if (index < journal.policy.admission_capacity) {
        const auto offset =
            journal.payload_count.fetch_add(payload.size(), std::memory_order_relaxed);
        if (offset <= journal.policy.payload_capacity_bytes &&
            payload.size() <= journal.policy.payload_capacity_bytes - offset) {
            retained.payload_offset = offset;
            std::copy(payload.begin(), payload.end(), journal.payloads.get() + offset);
            journal.admissions[index] = retained;
        } else {
            closure.replay_eligible.store(false, std::memory_order_release);
        }
    } else {
        closure.replay_eligible.store(false, std::memory_order_release);
    }
    action.replay_eligible = closure.replay_eligible.load(std::memory_order_acquire);
    if (!closure.actions->publish_reserved(action, action_sequence)) {
        closure.replay_eligible.store(false, std::memory_order_release);
    }
    if (result == LiveControlAdmissionResult::missed) {
        action.action = LiveControlActionId::missed;
        action.stage = LiveControlActionStage::terminal;
        action.reason = LiveControlActionReason::missed;
        action.result = LiveControlActionResult::settled;
        emit_action(*impl_, action);
    }
}

namespace {

[[nodiscard]] const LiveControlGenerationView* active_view(
    const LiveControlMailboxSet::Impl& impl) noexcept {
    if (!impl.has_generation.load(std::memory_order_acquire)) {
        return nullptr;
    }
    const auto index = impl.active_generation.load(std::memory_order_acquire);
    return &impl.generations[index].view;
}

[[nodiscard]] LiveControlBoundaryTarget target_from_record(
    const LiveControlUpdateRecord& record) noexcept {
    LiveControlBoundaryTarget target;
    target.kind = record.target_kind;
    target.frame_index = record.target_frame_index;
    target.rate_release_sequence = record.rate_release_sequence;
    target.reference_release_index = record.reference_release_index;
    target.rate_domain_registration_index =
        record.rate_domain_registration_index;
    target.phase_index = record.phase_index;
    target.rate_substep_ordinal = record.rate_substep_ordinal;
    return target;
}

[[nodiscard]] LiveControlActionRecord base_action(
    const LiveControlMailboxSet::Impl& impl) noexcept {
    LiveControlActionRecord action;
    action.runtime_id = impl.runtime_id;
    action.configuration_generation = impl.configuration_generation;
    action.policy_identity = impl.closure->policy.policy_identity;
    return action;
}

[[nodiscard]] bool replay_historical_only(
    LiveControlActionId action) noexcept {
    return action == LiveControlActionId::admission ||
        action == LiveControlActionId::missed ||
        action == LiveControlActionId::stopped ||
        action == LiveControlActionId::checkpointed ||
        action == LiveControlActionId::replay_verified;
}

void consume_replay_history(LiveControlMailboxSet::Impl& impl) noexcept {
    auto& closure = *impl.closure;
    if (!closure.replay_view) {
        return;
    }
    const auto& view = *closure.replay_view;
    const bool lossless =
        view.metadata.schema_version == live_control_lossless_replay_schema_version;
    while (closure.replay_action_index < view.metadata.action_record_count) {
        LiveControlActionRecord expected;
        if (!live_control_replay_action_at(view, closure.replay_action_index,
                                           expected)) {
            closure.replay_mismatch = Status::invalid_artifact;
            return;
        }
        if (lossless && expected.sequence >= view.admission_close_sequence) {
            impl.admission_open.store(false, std::memory_order_release);
        }
        if (!replay_historical_only(expected.action)) {
            return;
        }
        if (lossless) {
            if (!closure.lossless) {
                closure.replay_mismatch = Status::incompatible_artifact;
                return;
            }
            if (expected.action == LiveControlActionId::admission) {
                LiveControlRetainedAdmission admission;
                std::span<const std::byte> payload;
                if (!live_control_replay_admission_at(
                        view, closure.lossless->replay_admission_index++, admission,
                        payload) ||
                    admission.action_sequence != expected.sequence) {
                    closure.replay_mismatch = Status::invalid_artifact;
                    return;
                }
                if (admission.counter_changed) {
                    const auto owner = find_mailbox(impl, admission.mailbox_identity);
                    if (owner >= impl.mailboxes.size()) {
                        closure.replay_mismatch = Status::incompatible_artifact;
                        return;
                    }
                    auto& mailbox = *impl.mailboxes[owner];
                    count_result(mailbox, admission.outcome);
                    if (admission.slot_index != kInvalidIndex) {
                        auto& slot = mailbox.slots[admission.slot_index];
                        const auto previous =
                            slot.state.load(std::memory_order_acquire);
                        if (previous != LiveControlMailboxSet::Impl::SlotState::free &&
                            !terminal_state(previous)) {
                            closure.replay_mismatch = Status::incompatible_artifact;
                            closure.replay_mismatch_action_sequence = expected.sequence;
                            return;
                        }
                        slot.record = admission.record;
                        slot.record.runtime_id = impl.runtime_id;
                        slot.record.configuration_generation =
                            impl.configuration_generation;
                        const auto stride =
                            mailbox.registration.payload_bytes_per_record;
                        auto* destination = mailbox.payload_storage.get() +
                                            admission.slot_index * stride;
                        std::copy(payload.begin(), payload.end(), destination);
                        std::fill(destination + payload.size(), destination + stride,
                                  std::byte{0});
                        slot.terminal_generation.store(0, std::memory_order_relaxed);
                        auto state = LiveControlMailboxSet::Impl::SlotState::staged;
                        if (admission.outcome == LiveControlAdmissionResult::missed) {
                            state = LiveControlMailboxSet::Impl::SlotState::missed;
                            increment(impl.missed);
                        } else if (admission.outcome ==
                                   LiveControlAdmissionResult::stopped) {
                            state = LiveControlMailboxSet::Impl::SlotState::stopped;
                            increment(impl.stopped);
                        } else {
                            mailbox.occupancy.fetch_add(1, std::memory_order_relaxed);
                        }
                        slot.state.store(state, std::memory_order_release);
                        mailbox.next_sequence.store(admission.record.mailbox_sequence +
                                                        1,
                                                    std::memory_order_relaxed);
                        for (std::size_t index = 0; index < impl.producer_count;
                             ++index) {
                            auto& producer = impl.producers[index];
                            if (producer.mailbox_identity ==
                                    admission.mailbox_identity &&
                                producer.producer_identity ==
                                    admission.producer_identity) {
                                producer.next_sequence.store(
                                    admission.record.producer_sequence + 1,
                                    std::memory_order_relaxed);
                                break;
                            }
                        }
                    }
                }
            } else if (expected.action == LiveControlActionId::missed ||
                       expected.action == LiveControlActionId::stopped) {
                const auto owner = find_mailbox(impl, expected.mailbox_identity);
                if (owner >= impl.mailboxes.size()) {
                    closure.replay_mismatch = Status::invalid_artifact;
                    return;
                }
                auto& mailbox = *impl.mailboxes[owner];
                for (std::size_t index = 0;
                     index < mailbox.registration.record_capacity; ++index) {
                    auto& slot = mailbox.slots[index];
                    if (slot.record.mailbox_sequence == expected.mailbox_sequence &&
                        slot.state.load(std::memory_order_acquire) ==
                            LiveControlMailboxSet::Impl::SlotState::staged) {
                        const bool missed =
                            expected.action == LiveControlActionId::missed;
                        slot.state.store(
                            missed ? LiveControlMailboxSet::Impl::SlotState::missed
                                   : LiveControlMailboxSet::Impl::SlotState::stopped,
                            std::memory_order_release);
                        mailbox.occupancy.fetch_sub(1, std::memory_order_relaxed);
                        increment(missed ? impl.missed : impl.stopped);
                        break;
                    }
                }
            }
        }
        ++closure.replay_action_index;
    }
    if (lossless && view.admission_close_sequence != kInvalidSequence) {
        impl.admission_open.store(false, std::memory_order_release);
    }
}

void emit_action(
    LiveControlMailboxSet::Impl& impl,
    LiveControlActionRecord action,
    std::uint64_t* sequence) noexcept {
    if (impl.closure && impl.closure->replay_view) {
        auto& closure = *impl.closure;
        consume_replay_history(impl);
        LiveControlActionRecord expected;
        if (closure.replay_mismatch != Status::ok ||
            closure.replay_action_index >=
                closure.replay_view->metadata.action_record_count ||
            !live_control_replay_action_at(
                *closure.replay_view,
                closure.replay_action_index,
                expected)) {
            closure.replay_mismatch = Status::invalid_artifact;
            return;
        }
        action.sequence = expected.sequence;
        action.runtime_id = expected.runtime_id;
        action.configuration_generation = expected.configuration_generation;
        if (std::memcmp(&action, &expected, sizeof(action)) != 0) {
            closure.replay_mismatch = Status::incompatible_artifact;
            closure.replay_mismatch_action_sequence = expected.sequence;
            return;
        }
        if (sequence) {
            *sequence = expected.sequence;
        }
        ++closure.replay_action_index;
        return;
    }
    if (!impl.closure || !impl.closure->actions->emit(action, sequence)) {
        if (impl.closure) {
            impl.closure->replay_eligible.store(
                false, std::memory_order_release);
        }
    }
}

[[nodiscard]] LiveControlActionReason reason_for_admission(
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

[[nodiscard]] bool targets_equal(
    const LiveControlBoundaryTarget& left,
    const LiveControlBoundaryTarget& right) noexcept {
    return left.frame_index == right.frame_index &&
        left.rate_release_sequence == right.rate_release_sequence &&
        left.reference_release_index == right.reference_release_index &&
        left.rate_domain_registration_index ==
            right.rate_domain_registration_index &&
        left.phase_index == right.phase_index &&
        left.rate_substep_ordinal == right.rate_substep_ordinal &&
        left.kind == right.kind;
}

[[nodiscard]] const LiveControlGenerationView* inject_replay_boundary(
    LiveControlMailboxSet::Impl& impl,
    const LiveControlBoundaryTarget& target) noexcept {
    auto& closure = *impl.closure;
    const bool lossless = closure.replay_view->metadata.schema_version ==
                          live_control_lossless_replay_schema_version;
    if (lossless) {
        consume_replay_history(impl);
        if (closure.replay_mismatch != Status::ok) {
            return active_view(impl);
        }
    }
    const auto& view = *closure.replay_view;
    LiveControlRetainedGenerationView retained;
    const bool has_retained = closure.replay_generation_index <
            view.metadata.retained_generation_count &&
        live_control_replay_generation_at(
            view, closure.replay_generation_index, retained);
    if (!has_retained || !targets_equal(retained.target, target)) {
        bool empty_boundary = false;
        for (std::size_t index = 0;
             index < view.metadata.action_record_count;
             ++index) {
            LiveControlActionRecord action;
            if (live_control_replay_action_at(view, index, action) &&
                action.action == LiveControlActionId::boundary_empty &&
                targets_equal(action.target, target)) {
                empty_boundary = true;
                break;
            }
        }
        if (!empty_boundary) {
            closure.replay_mismatch = Status::invalid_artifact;
        } else {
            auto action = base_action(impl);
            action.target = target;
            const auto* current = active_view(impl);
            action.generation_identity = current
                ? current->generation_identity
                : 0;
            action.prior_generation_identity = action.generation_identity;
            action.action = LiveControlActionId::boundary_empty;
            action.stage = LiveControlActionStage::terminal;
            action.result = LiveControlActionResult::settled;
            action.replay_eligible = closure.replay_eligible.load(
                std::memory_order_acquire);
            emit_action(impl, action);
        }
        return active_view(impl);
    }
    const auto* prior = active_view(impl);
    const auto prior_identity = prior ? prior->generation_identity : 0;
    if (prior_identity != retained.prior_generation_identity ||
        retained.record_count > total_record_capacity(impl) ||
        retained.payload_bytes > impl.total_payload_storage_bytes) {
        closure.replay_mismatch = Status::incompatible_artifact;
        return active_view(impl);
    }

    // Recreate every source record from the validated transcript. Records
    // already staged in the checkpoint retain their exact slots. Accepted
    // post-checkpoint records are synthesized into reusable slots from action
    // metadata; survivor payloads come only from the explicit retained journal.
    // This removes external producer timing from replay while preserving the
    // normal transaction settlement path.
    std::size_t candidate_count = 0;
    std::size_t survivor_count = 0;
    for (std::size_t action_index = 0;
         action_index < view.metadata.action_record_count;
         ++action_index) {
        LiveControlActionRecord terminal;
        if (!live_control_replay_action_at(view, action_index, terminal)) {
            closure.replay_mismatch = Status::invalid_artifact;
            return active_view(impl);
        }
        if (terminal.generation_identity != retained.generation_identity ||
            (terminal.action != LiveControlActionId::committed &&
             terminal.action != LiveControlActionId::replaced &&
             terminal.action != LiveControlActionId::rolled_back)) {
            continue;
        }
        const auto mailbox_index = find_mailbox(impl, terminal.mailbox_identity);
        const auto producer = std::find_if(
            impl.producers.get(),
            impl.producers.get() + impl.producer_count,
            [&](const LiveControlMailboxSet::Impl::Producer& candidate) {
                return candidate.mailbox_identity == terminal.mailbox_identity &&
                    candidate.producer_identity == terminal.producer_identity;
            });
        if (mailbox_index == impl.mailboxes.size() ||
            producer == impl.producers.get() + impl.producer_count ||
            producer->mailbox_index != mailbox_index ||
            candidate_count == total_record_capacity(impl)) {
            closure.replay_mismatch = Status::incompatible_artifact;
            return active_view(impl);
        }
        auto& mailbox = *impl.mailboxes[mailbox_index];
        LiveControlUpdateRecord retained_record;
        std::span<const std::byte> retained_payload;
        bool survivor = false;
        for (std::size_t record_index = 0;
             record_index < retained.record_count;
             ++record_index) {
            LiveControlUpdateRecord candidate;
            std::span<const std::byte> payload;
            if (!live_control_replay_record_at(
                    view,
                    closure.replay_generation_index,
                    record_index,
                    candidate,
                    payload)) {
                closure.replay_mismatch = Status::invalid_artifact;
                return active_view(impl);
            }
            if (candidate.mailbox_identity == terminal.mailbox_identity &&
                candidate.mailbox_sequence == terminal.mailbox_sequence &&
                candidate.producer_identity == terminal.producer_identity &&
                candidate.producer_sequence == terminal.producer_sequence &&
                candidate.update_kind == terminal.update_kind &&
                candidate.payload_bytes == terminal.payload_bytes &&
                candidate.payload_digest == terminal.payload_digest) {
                retained_record = candidate;
                retained_payload = payload;
                survivor = true;
                break;
            }
        }
        std::size_t slot_index = mailbox.registration.record_capacity;
        for (std::size_t index = 0;
             index < mailbox.registration.record_capacity;
             ++index) {
            const auto& slot = mailbox.slots[index];
            if (slot.state.load(std::memory_order_acquire) !=
                    LiveControlMailboxSet::Impl::SlotState::staged ||
                slot.record.mailbox_identity != terminal.mailbox_identity ||
                slot.record.mailbox_sequence != terminal.mailbox_sequence ||
                slot.record.producer_identity != terminal.producer_identity ||
                slot.record.producer_sequence != terminal.producer_sequence ||
                slot.record.update_kind != terminal.update_kind ||
                slot.record.payload_bytes != terminal.payload_bytes ||
                slot.record.payload_digest != terminal.payload_digest ||
                !targets_equal(target_from_record(slot.record), target)) {
                continue;
            }
            slot_index = index;
            break;
        }
        if (lossless && slot_index == mailbox.registration.record_capacity) {
            closure.replay_mismatch = Status::incompatible_artifact;
            return active_view(impl);
        }
        if (slot_index == mailbox.registration.record_capacity) {
            for (std::size_t index = 0;
                 index < mailbox.registration.record_capacity;
                 ++index) {
                const auto state = mailbox.slots[index].state.load(
                    std::memory_order_acquire);
                if (state == LiveControlMailboxSet::Impl::SlotState::free) {
                    slot_index = index;
                    break;
                }
            }
        }
        if (slot_index == mailbox.registration.record_capacity) {
            // Match stage(): unused slots precede reclaimable terminal slots.
            // Terminal records remain part of the canonical checkpoint state;
            // reclaiming one early changes nested active replay's state hash.
            for (std::size_t index = 0;
                 index < mailbox.registration.record_capacity;
                 ++index) {
                if (terminal_state(mailbox.slots[index].state.load(
                        std::memory_order_acquire))) {
                    slot_index = index;
                    break;
                }
            }
        }
        if (slot_index == mailbox.registration.record_capacity) {
            closure.replay_mismatch = Status::incompatible_artifact;
            return active_view(impl);
        }
        auto& slot = mailbox.slots[slot_index];
        if (slot.state.load(std::memory_order_acquire) !=
                LiveControlMailboxSet::Impl::SlotState::staged) {
            LiveControlUpdateRecord record;
            if (survivor) {
                record = retained_record;
            } else {
                record.target_kind = terminal.target.kind;
                record.target_frame_index = terminal.target.frame_index;
                record.rate_release_sequence =
                    terminal.target.rate_release_sequence;
                record.reference_release_index =
                    terminal.target.reference_release_index;
                record.rate_domain_registration_index =
                    terminal.target.rate_domain_registration_index;
                record.phase_index = terminal.target.phase_index;
                record.rate_substep_ordinal =
                    terminal.target.rate_substep_ordinal;
                record.mailbox_identity = terminal.mailbox_identity;
                record.producer_identity = terminal.producer_identity;
                record.mailbox_sequence = terminal.mailbox_sequence;
                record.producer_sequence = terminal.producer_sequence;
                record.payload_digest = terminal.payload_digest;
                record.payload_bytes = terminal.payload_bytes;
                record.payload_alignment = 1;
                record.update_kind = terminal.update_kind;
            }
            record.runtime_id = impl.runtime_id;
            record.configuration_generation = impl.configuration_generation;
            slot.record = record;
            const auto stride = static_cast<std::size_t>(
                mailbox.registration.payload_bytes_per_record);
            auto* destination = mailbox.payload_storage.get() +
                slot_index * stride;
            if (survivor) {
                std::copy(
                    retained_payload.begin(),
                    retained_payload.end(),
                    destination);
            } else {
                std::fill_n(destination, terminal.payload_bytes, std::byte{0});
            }
            std::fill(
                destination + terminal.payload_bytes,
                destination + stride,
                std::byte{0});
            slot.terminal_generation.store(0, std::memory_order_relaxed);
            slot.state.store(
                LiveControlMailboxSet::Impl::SlotState::staged,
                std::memory_order_release);
            mailbox.occupancy.fetch_add(1, std::memory_order_relaxed);
        }
        impl.candidates[candidate_count++] = {
            static_cast<std::uint32_t>(mailbox_index),
            static_cast<std::uint32_t>(slot_index),
            mailbox.registration.mailbox_identity,
            terminal.mailbox_sequence,
            survivor,
        };
        if (survivor) {
            ++survivor_count;
        }
    }
    if (candidate_count > total_record_capacity(impl) ||
        survivor_count != retained.record_count ||
        closure.provisional_count >
            total_record_capacity(impl) - candidate_count) {
        closure.replay_mismatch = Status::incompatible_artifact;
        return active_view(impl);
    }
    std::sort(
        impl.candidates.get(),
        impl.candidates.get() + candidate_count,
        [](const auto& left, const auto& right) noexcept {
            if (left.mailbox_identity != right.mailbox_identity) {
                return left.mailbox_identity < right.mailbox_identity;
            }
            return left.mailbox_sequence < right.mailbox_sequence;
        });

    impl.inspection_version.fetch_add(1, std::memory_order_acq_rel);
    for (std::size_t index = 0; index < candidate_count; ++index) {
        const auto& candidate = impl.candidates[index];
        auto& slot = impl.mailboxes[candidate.mailbox_index]
                         ->slots[candidate.slot_index];
        auto expected = LiveControlMailboxSet::Impl::SlotState::staged;
        if (!slot.state.compare_exchange_strong(
                expected,
                LiveControlMailboxSet::Impl::SlotState::boundary_owned,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            closure.replay_mismatch = Status::incompatible_artifact;
            impl.inspection_version.fetch_add(1, std::memory_order_release);
            return active_view(impl);
        }
        auto& provisional = closure.provisional_records[
            closure.provisional_count++];
        provisional.mailbox_index = candidate.mailbox_index;
        provisional.slot_index = candidate.slot_index;
        provisional.generation_identity = retained.generation_identity;
        provisional.prior_generation_identity =
            retained.prior_generation_identity;
        provisional.survivor = candidate.survivor;
    }
    const auto active = impl.active_generation.load(std::memory_order_relaxed);
    const auto inactive = static_cast<std::uint8_t>(active == 0 ? 1 : 0);
    auto& generation = impl.generations[inactive];
    std::size_t payload_offset = 0;
    for (std::size_t index = 0; index < retained.record_count; ++index) {
        LiveControlUpdateRecord record;
        std::span<const std::byte> payload;
        if (!live_control_replay_record_at(
                view,
                closure.replay_generation_index,
                index,
                record,
                payload)) {
            closure.replay_mismatch = Status::invalid_artifact;
            return active_view(impl);
        }
        record.runtime_id = impl.runtime_id;
        record.configuration_generation = impl.configuration_generation;
        auto& output = generation.records[index];
        output.record = record;
        std::copy(
            payload.begin(),
            payload.end(),
            generation.payloads.get() + payload_offset);
        output.payload = std::span<const std::byte>(
            generation.payloads.get() + payload_offset,
            payload.size());
        payload_offset += payload.size();
    }
    generation.view.runtime_id = impl.runtime_id;
    generation.view.configuration_generation = impl.configuration_generation;
    generation.view.generation_identity = retained.generation_identity;
    generation.view.target = target;
    generation.view.records = std::span<const LiveControlRecordView>(
        generation.records.get(), retained.record_count);
    impl.active_generation.store(inactive, std::memory_order_release);
    impl.has_generation.store(true, std::memory_order_release);
    impl.latest_generation_identity.store(
        retained.generation_identity, std::memory_order_relaxed);
    impl.latest_frame_index.store(target.frame_index, std::memory_order_relaxed);
    impl.latest_rate_sequence.store(
        target.rate_release_sequence, std::memory_order_relaxed);
    impl.latest_reference_index.store(
        target.reference_release_index, std::memory_order_relaxed);
    impl.latest_domain_index.store(
        target.rate_domain_registration_index, std::memory_order_relaxed);
    impl.latest_phase_index.store(target.phase_index, std::memory_order_relaxed);
    impl.latest_substep.store(
        target.rate_substep_ordinal, std::memory_order_relaxed);
    impl.latest_survivor_count.store(
        static_cast<std::uint32_t>(retained.record_count),
        std::memory_order_relaxed);
    impl.latest_target_kind.store(target.kind, std::memory_order_relaxed);
    auto action = base_action(impl);
    action.target = target;
    action.generation_identity = retained.generation_identity;
    action.prior_generation_identity = retained.prior_generation_identity;
    action.survivor_count = static_cast<std::uint32_t>(survivor_count);
    action.replaced_count = static_cast<std::uint32_t>(
        candidate_count - survivor_count);
    action.action = LiveControlActionId::provisional_publication;
    action.stage = LiveControlActionStage::provisional;
    action.result = LiveControlActionResult::published;
    action.replay_eligible = closure.replay_eligible.load(
        std::memory_order_acquire);
    emit_action(impl, action);
    impl.inspection_version.fetch_add(1, std::memory_order_release);
    ++closure.replay_generation_index;
    return &generation.view;
}

void copy_generation(
    LiveControlMailboxSet::Impl::Generation& destination,
    const LiveControlMailboxSet::Impl::Generation& source) noexcept {
    destination.view = source.view;
    std::size_t payload_offset = 0;
    for (std::size_t index = 0; index < source.view.records.size(); ++index) {
        const auto& input = source.view.records[index];
        auto& output = destination.records[index];
        output.record = input.record;
        std::copy(
            input.payload.begin(),
            input.payload.end(),
            destination.payloads.get() + payload_offset);
        output.payload = std::span<const std::byte>(
            destination.payloads.get() + payload_offset,
            input.payload.size());
        payload_offset += input.payload.size();
    }
    destination.view.records = std::span<const LiveControlRecordView>(
        destination.records.get(), source.view.records.size());
}

[[nodiscard]] bool retain_generation(
    LiveControlMailboxSet::Impl& impl,
    const LiveControlMailboxSet::Impl::Generation& generation,
    std::uint64_t prior_generation_identity,
    std::uint64_t first_action_sequence) noexcept {
    auto& closure = *impl.closure;
    if (!closure.policy.replay_enabled) {
        return true;
    }
    std::size_t payload_bytes = 0;
    for (const auto& record : generation.view.records) {
        if (!checked_add(payload_bytes, record.payload.size(), payload_bytes)) {
            closure.replay_eligible.store(false, std::memory_order_release);
            return false;
        }
    }
    if (closure.retained_generation_count >=
            closure.policy.retained_generation_capacity ||
        generation.view.records.size() >
            closure.policy.retained_record_capacity -
                closure.retained_record_count ||
        payload_bytes > closure.policy.retained_payload_bytes -
            closure.retained_payload_count) {
        closure.replay_eligible.store(false, std::memory_order_release);
        return false;
    }
    auto& retained = closure.retained_generations[
        closure.retained_generation_count++];
    retained.target = generation.view.target;
    retained.generation_identity = generation.view.generation_identity;
    retained.prior_generation_identity = prior_generation_identity;
    retained.first_action_sequence = first_action_sequence;
    retained.first_record_index = closure.retained_record_count;
    retained.first_payload_offset = closure.retained_payload_count;
    retained.record_count = static_cast<std::uint32_t>(
        generation.view.records.size());
    retained.payload_bytes = static_cast<std::uint32_t>(payload_bytes);
    std::size_t payload_offset = closure.retained_payload_count;
    for (const auto& record : generation.view.records) {
        auto& copy = closure.retained_records[
            closure.retained_record_count++];
        copy.record = record.record;
        copy.payload_offset = payload_offset;
        std::copy(
            record.payload.begin(),
            record.payload.end(),
            closure.retained_payloads.get() + payload_offset);
        payload_offset += record.payload.size();
    }
    closure.retained_payload_count = payload_offset;
    closure.retained_generation_published.store(
        closure.retained_generation_count,
        std::memory_order_release);
    return true;
}

[[nodiscard]] const LiveControlGenerationView* close_boundary(
    LiveControlMailboxSet::Impl& impl,
    const LiveControlBoundaryTarget& target) noexcept {
    using SlotState = LiveControlMailboxSet::Impl::SlotState;
    if (impl.closure && impl.closure->replay_view) {
        return inject_replay_boundary(impl, target);
    }
    impl.inspection_version.fetch_add(1, std::memory_order_acq_rel);
    std::size_t candidate_count = 0;

    for (std::size_t mailbox_index = 0;
         mailbox_index < impl.mailboxes.size(); ++mailbox_index) {
        auto& mailbox = *impl.mailboxes[mailbox_index];
        for (std::size_t slot_index = 0;
             slot_index < mailbox.registration.record_capacity; ++slot_index) {
            auto& slot = mailbox.slots[slot_index];
            auto state = slot.state.load(std::memory_order_acquire);
            if (state != SlotState::staged) {
                continue;
            }
            const auto& record = slot.record;
            const bool current = target.kind == LiveControlTargetKind::host_frame
                ? record.target_kind == LiveControlTargetKind::host_frame &&
                    record.target_frame_index == target.frame_index
                : record.target_kind == LiveControlTargetKind::rate_release &&
                    record.reference_release_index ==
                        target.reference_release_index &&
                    record.rate_release_sequence ==
                        target.rate_release_sequence;
            if (!current && !target_closed(impl, record)) {
                continue;
            }
            if (!slot.state.compare_exchange_strong(
                    state,
                    SlotState::boundary_owned,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                continue;
            }
            if (!current) {
                slot.terminal_generation.store(0, std::memory_order_relaxed);
                mailbox.occupancy.fetch_sub(1, std::memory_order_relaxed);
                increment(impl.missed);
                if (impl.closure) {
                    auto action = base_action(impl);
                    action.target = target_from_record(slot.record);
                    action.mailbox_identity = slot.record.mailbox_identity;
                    action.producer_identity = slot.record.producer_identity;
                    action.mailbox_sequence = slot.record.mailbox_sequence;
                    action.producer_sequence = slot.record.producer_sequence;
                    action.payload_digest = slot.record.payload_digest;
                    action.payload_bytes = slot.record.payload_bytes;
                    action.update_kind = slot.record.update_kind;
                    action.admission_result = LiveControlAdmissionResult::missed;
                    action.record_status = LiveControlRecordStatus::missed;
                    action.action = LiveControlActionId::missed;
                    action.stage = LiveControlActionStage::terminal;
                    action.reason = LiveControlActionReason::missed;
                    action.result = LiveControlActionResult::settled;
                    action.replay_eligible = impl.closure->replay_eligible.load(
                        std::memory_order_acquire);
                    emit_action(impl, action);
                }
                slot.state.store(SlotState::missed, std::memory_order_release);
                continue;
            }
            impl.candidates[candidate_count++] = {
                static_cast<std::uint32_t>(mailbox_index),
                static_cast<std::uint32_t>(slot_index),
                mailbox.registration.mailbox_identity,
                record.mailbox_sequence,
            };
        }
    }

    std::sort(
        impl.candidates.get(),
        impl.candidates.get() + candidate_count,
        [](const auto& left, const auto& right) noexcept {
            if (left.mailbox_identity != right.mailbox_identity) {
                return left.mailbox_identity < right.mailbox_identity;
            }
            return left.mailbox_sequence < right.mailbox_sequence;
        });

    std::array<std::size_t, 5> last_by_kind{};
    last_by_kind.fill(std::numeric_limits<std::size_t>::max());
    std::uint64_t current_mailbox = 0;
    for (std::size_t index = 0; index < candidate_count; ++index) {
        const auto& candidate = impl.candidates[index];
        if (index == 0 || candidate.mailbox_identity != current_mailbox) {
            current_mailbox = candidate.mailbox_identity;
            last_by_kind.fill(std::numeric_limits<std::size_t>::max());
        }
        const auto& record = impl.mailboxes[candidate.mailbox_index]
                                 ->slots[candidate.slot_index]
                                 .record;
        const auto kind_index = static_cast<std::size_t>(record.update_kind) - 1;
        const auto previous = last_by_kind[kind_index];
        if (previous != std::numeric_limits<std::size_t>::max()) {
            impl.candidates[previous].survivor = false;
        }
        last_by_kind[kind_index] = index;
    }

    std::size_t survivor_count = 0;
    for (std::size_t index = 0; index < candidate_count; ++index) {
        const auto& candidate = impl.candidates[index];
        if (candidate.survivor) {
            ++survivor_count;
        }
    }
    if (survivor_count == 0) {
        if (impl.closure &&
            ((target.kind == LiveControlTargetKind::host_frame &&
              target.frame_index != kInvalidSequence) ||
             (target.kind == LiveControlTargetKind::rate_release &&
              target.reference_release_index != kInvalidIndex))) {
            auto action = base_action(impl);
            action.target = target;
            const auto* current = active_view(impl);
            action.generation_identity = current
                ? current->generation_identity
                : 0;
            action.prior_generation_identity = action.generation_identity;
            action.action = LiveControlActionId::boundary_empty;
            action.stage = LiveControlActionStage::terminal;
            action.result = LiveControlActionResult::settled;
            action.replay_eligible = impl.closure->replay_eligible.load(
                std::memory_order_acquire);
            emit_action(impl, action);
        }
        impl.inspection_version.fetch_add(1, std::memory_order_release);
        return active_view(impl);
    }

    const auto active = impl.active_generation.load(std::memory_order_relaxed);
    const auto inactive = static_cast<std::uint8_t>(active == 0 ? 1 : 0);
    auto& generation = impl.generations[inactive];
    const auto* prior_view = active_view(impl);
    const auto prior_generation_identity = prior_view
        ? prior_view->generation_identity
        : 0;
    std::uint64_t identity = kFnvOffset;
    hash_target(identity, target);
    std::size_t output_index = 0;
    std::size_t payload_offset = 0;
    for (std::size_t index = 0; index < candidate_count; ++index) {
        const auto& candidate = impl.candidates[index];
        auto& mailbox = *impl.mailboxes[candidate.mailbox_index];
        auto& slot = mailbox.slots[candidate.slot_index];
        if (!candidate.survivor) {
            continue;
        }
        const auto& record = slot.record;
        const auto stride = static_cast<std::size_t>(
            mailbox.registration.payload_bytes_per_record);
        const auto* source = mailbox.payload_storage.get() +
            static_cast<std::size_t>(candidate.slot_index) * stride;
        std::copy_n(
            source,
            record.payload_bytes,
            generation.payloads.get() + payload_offset);
        auto& view = generation.records[output_index++];
        view.record = record;
        view.payload = std::span<const std::byte>(
            generation.payloads.get() + payload_offset,
            record.payload_bytes);
        payload_offset += record.payload_bytes;
        hash_u64(identity, record.mailbox_identity);
        hash_u64(identity, record.mailbox_sequence);
        hash_u64(identity, record.producer_identity);
        hash_u64(identity, record.producer_sequence);
        hash_u64(identity, static_cast<std::uint8_t>(record.update_kind));
        hash_u64(identity, record.payload_digest);
    }
    if (identity == 0) {
        identity = 1;
    }
    generation.view.runtime_id = impl.runtime_id;
    generation.view.configuration_generation = impl.configuration_generation;
    generation.view.generation_identity = identity;
    generation.view.target = target;
    generation.view.records = std::span<const LiveControlRecordView>(
        generation.records.get(), survivor_count);

    impl.active_generation.store(inactive, std::memory_order_release);
    impl.has_generation.store(true, std::memory_order_release);

    if (impl.closure) {
        auto& closure = *impl.closure;
        for (std::size_t index = 0; index < candidate_count; ++index) {
            const auto& candidate = impl.candidates[index];
            auto& provisional = closure.provisional_records[
                closure.provisional_count++];
            provisional.mailbox_index = candidate.mailbox_index;
            provisional.slot_index = candidate.slot_index;
            provisional.generation_identity = identity;
            provisional.prior_generation_identity =
                prior_generation_identity;
            provisional.survivor = candidate.survivor;
        }
        auto action = base_action(impl);
        action.target = target;
        action.generation_identity = identity;
        action.prior_generation_identity = prior_generation_identity;
        action.survivor_count = static_cast<std::uint32_t>(survivor_count);
        action.replaced_count = static_cast<std::uint32_t>(
            candidate_count - survivor_count);
        action.action = LiveControlActionId::provisional_publication;
        action.stage = LiveControlActionStage::provisional;
        action.result = LiveControlActionResult::published;
        action.replay_eligible = closure.replay_eligible.load(
            std::memory_order_acquire);
        std::uint64_t action_sequence = 0;
        emit_action(impl, action, &action_sequence);
        (void)retain_generation(
            impl,
            generation,
            prior_generation_identity,
            action_sequence);
    } else {
        for (std::size_t index = 0; index < candidate_count; ++index) {
            const auto& candidate = impl.candidates[index];
            auto& mailbox = *impl.mailboxes[candidate.mailbox_index];
            auto& slot = mailbox.slots[candidate.slot_index];
            if (candidate.survivor) {
                slot.terminal_generation.store(identity, std::memory_order_relaxed);
                slot.state.store(SlotState::committed, std::memory_order_release);
                mailbox.occupancy.fetch_sub(1, std::memory_order_relaxed);
                increment(impl.committed);
            } else {
                slot.terminal_generation.store(identity, std::memory_order_relaxed);
                slot.state.store(SlotState::replaced, std::memory_order_release);
                mailbox.occupancy.fetch_sub(1, std::memory_order_relaxed);
                increment(impl.replaced);
            }
        }
    }

    impl.latest_generation_identity.store(identity, std::memory_order_relaxed);
    impl.latest_frame_index.store(target.frame_index, std::memory_order_relaxed);
    impl.latest_rate_sequence.store(
        target.rate_release_sequence, std::memory_order_relaxed);
    impl.latest_reference_index.store(
        target.reference_release_index, std::memory_order_relaxed);
    impl.latest_domain_index.store(
        target.rate_domain_registration_index, std::memory_order_relaxed);
    impl.latest_phase_index.store(target.phase_index, std::memory_order_relaxed);
    impl.latest_substep.store(
        target.rate_substep_ordinal, std::memory_order_relaxed);
    impl.latest_survivor_count.store(
        static_cast<std::uint32_t>(survivor_count),
        std::memory_order_relaxed);
    impl.latest_target_kind.store(target.kind, std::memory_order_relaxed);
    impl.inspection_version.fetch_add(1, std::memory_order_release);
    return &generation.view;
}

} // namespace

const LiveControlGenerationView* LiveControlMailboxSet::close_host_frame(
    std::uint64_t frame_index) noexcept {
    if (!impl_ || !impl_->admission_open.load(std::memory_order_acquire)) {
        return impl_ ? active_view(*impl_) : nullptr;
    }
    if (impl_->host_boundary_started.load(std::memory_order_acquire) &&
        frame_index <= impl_->last_host_boundary.load(std::memory_order_acquire)) {
        return active_view(*impl_);
    }
    impl_->last_host_boundary.store(frame_index, std::memory_order_release);
    impl_->host_boundary_started.store(true, std::memory_order_release);
    LiveControlBoundaryTarget target;
    target.kind = LiveControlTargetKind::host_frame;
    target.frame_index = frame_index;
    return close_boundary(*impl_, target);
}

const LiveControlGenerationView* LiveControlMailboxSet::close_rate_release(
    std::size_t reference_release_index,
    std::uint64_t release_sequence) noexcept {
    if (!impl_ || reference_release_index >= impl_->rate_target_count ||
        !impl_->admission_open.load(std::memory_order_acquire)) {
        return impl_ ? active_view(*impl_) : nullptr;
    }
    auto& current = impl_->rate_targets[reference_release_index];
    for (std::size_t index = 0; index < impl_->rate_target_count; ++index) {
        if (impl_->rate_targets[index].release_time_ns <
            current.release_time_ns) {
            impl_->rate_targets[index].closed.store(
                true, std::memory_order_release);
        }
    }
    if (current.closed.exchange(true, std::memory_order_acq_rel)) {
        return active_view(*impl_);
    }
    LiveControlBoundaryTarget target;
    target.kind = LiveControlTargetKind::rate_release;
    target.frame_index = kInvalidSequence;
    target.rate_release_sequence = release_sequence;
    target.reference_release_index =
        static_cast<std::uint32_t>(reference_release_index);
    target.rate_domain_registration_index = current.domain_registration_index;
    target.phase_index = current.phase_index;
    target.rate_substep_ordinal = current.substep_ordinal;
    return close_boundary(*impl_, target);
}

const LiveControlGenerationView*
LiveControlMailboxSet::active_generation_view() const noexcept {
    return impl_ ? active_view(*impl_) : nullptr;
}

bool LiveControlMailboxSet::begin_step_transaction() noexcept {
    if (!impl_ || !impl_->closure) {
        return true;
    }
    auto& closure = *impl_->closure;
    if (closure.host_claim.load(std::memory_order_acquire) &&
        !closure.replay_view) {
        return false;
    }
    bool expected = false;
    if (!closure.transaction_active.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return false;
    }
    if (closure.host_claim.load(std::memory_order_acquire) &&
        !closure.replay_view) {
        closure.transaction_active.store(false, std::memory_order_release);
        return false;
    }
    closure.provisional_count = 0;
    closure.transaction_retained_begin = closure.retained_generation_count;
    closure.transaction_replay_generation_begin =
        closure.replay_generation_index;
    const auto* current = active_view(*impl_);
    closure.rollback_has_generation = current != nullptr;
    closure.prior_generation_identity = current
        ? current->generation_identity
        : 0;
    closure.prior_target = current
        ? current->target
        : LiveControlBoundaryTarget{};
    closure.prior_survivor_count = current
        ? static_cast<std::uint32_t>(current->records.size())
        : 0;
    if (current) {
        const auto active = impl_->active_generation.load(
            std::memory_order_acquire);
        copy_generation(
            closure.rollback_generation,
            impl_->generations[active]);
    } else {
        closure.rollback_generation.view = {};
    }
    return true;
}

void LiveControlMailboxSet::settle_step_transaction(Status status) noexcept {
    if (!impl_ || !impl_->closure ||
        !impl_->closure->transaction_active.load(std::memory_order_acquire)) {
        return;
    }
    auto& closure = *impl_->closure;
    const bool success = status == Status::ok;
    if (!success) {
        if (closure.rollback_has_generation) {
            const auto active = impl_->active_generation.load(
                std::memory_order_relaxed);
            const auto restored = static_cast<std::uint8_t>(
                active == 0 ? 1 : 0);
            copy_generation(
                impl_->generations[restored],
                closure.rollback_generation);
            impl_->active_generation.store(restored, std::memory_order_release);
            impl_->has_generation.store(true, std::memory_order_release);
        } else {
            impl_->has_generation.store(false, std::memory_order_release);
        }
        impl_->latest_generation_identity.store(
            closure.prior_generation_identity,
            std::memory_order_relaxed);
        impl_->latest_frame_index.store(
            closure.prior_target.frame_index,
            std::memory_order_relaxed);
        impl_->latest_rate_sequence.store(
            closure.prior_target.rate_release_sequence,
            std::memory_order_relaxed);
        impl_->latest_reference_index.store(
            closure.prior_target.reference_release_index,
            std::memory_order_relaxed);
        impl_->latest_domain_index.store(
            closure.prior_target.rate_domain_registration_index,
            std::memory_order_relaxed);
        impl_->latest_phase_index.store(
            closure.prior_target.phase_index,
            std::memory_order_relaxed);
        impl_->latest_substep.store(
            closure.prior_target.rate_substep_ordinal,
            std::memory_order_relaxed);
        impl_->latest_survivor_count.store(
            closure.prior_survivor_count,
            std::memory_order_relaxed);
        impl_->latest_target_kind.store(
            closure.prior_target.kind,
            std::memory_order_relaxed);
    }
    for (std::size_t index = 0; index < closure.provisional_count; ++index) {
        const auto& provisional = closure.provisional_records[index];
        auto& mailbox = *impl_->mailboxes[provisional.mailbox_index];
        auto& slot = mailbox.slots[provisional.slot_index];
        slot.terminal_generation.store(
            provisional.generation_identity,
            std::memory_order_relaxed);
        LiveControlActionRecord action = base_action(*impl_);
        action.target = target_from_record(slot.record);
        action.mailbox_identity = slot.record.mailbox_identity;
        action.producer_identity = slot.record.producer_identity;
        action.mailbox_sequence = slot.record.mailbox_sequence;
        action.producer_sequence = slot.record.producer_sequence;
        action.payload_digest = slot.record.payload_digest;
        action.payload_bytes = slot.record.payload_bytes;
        action.update_kind = slot.record.update_kind;
        action.admission_result = LiveControlAdmissionResult::accepted;
        action.generation_identity = provisional.generation_identity;
        action.prior_generation_identity =
            provisional.prior_generation_identity;
        action.stage = LiveControlActionStage::terminal;
        action.terminal_status = static_cast<std::int32_t>(status);
        action.replay_eligible = closure.replay_eligible.load(
            std::memory_order_acquire);
        std::uint64_t terminal_sequence = kInvalidSequence;
        if (closure.lossless && !closure.replay_view &&
            !closure.actions->reserve_sequence(terminal_sequence)) {
            closure.replay_eligible.store(false, std::memory_order_release);
            action.replay_eligible = false;
        }
        if (success) {
            if (provisional.survivor) {
                slot.state.store(
                    Impl::SlotState::committed,
                    std::memory_order_release);
                increment(impl_->committed);
                action.record_status = LiveControlRecordStatus::committed;
                action.action = LiveControlActionId::committed;
                action.reason = LiveControlActionReason::normal;
            } else {
                slot.state.store(
                    Impl::SlotState::replaced,
                    std::memory_order_release);
                increment(impl_->replaced);
                action.record_status = LiveControlRecordStatus::replaced;
                action.action = LiveControlActionId::replaced;
                action.reason = LiveControlActionReason::replaced;
            }
            action.result = LiveControlActionResult::settled;
        } else {
            slot.state.store(
                Impl::SlotState::rolled_back,
                std::memory_order_release);
            action.record_status = LiveControlRecordStatus::rolled_back;
            action.action = LiveControlActionId::rolled_back;
            action.reason = LiveControlActionReason::execution_failed;
            action.result = LiveControlActionResult::rolled_back;
        }
        mailbox.occupancy.fetch_sub(1, std::memory_order_relaxed);
        if (closure.lossless && !closure.replay_view) {
            if (!closure.actions->publish_reserved(action, terminal_sequence)) {
                closure.replay_eligible.store(false, std::memory_order_release);
            }
        } else {
            emit_action(*impl_, action);
        }
    }
    for (std::size_t index = closure.transaction_retained_begin;
         index < closure.retained_generation_count;
         ++index) {
        closure.retained_generations[index].terminal_status = status;
        closure.retained_generations[index].settled = true;
    }
    if (closure.replay_view) {
        for (std::size_t index = closure.transaction_replay_generation_begin;
             index < closure.replay_generation_index;
             ++index) {
            LiveControlRetainedGenerationView generation;
            if (!live_control_replay_generation_at(
                    *closure.replay_view, index, generation) ||
                generation.terminal_status != status) {
                closure.replay_mismatch = Status::invalid_artifact;
                break;
            }
        }
    }
    if (closure.replay_view && closure.lossless) {
        consume_replay_history(*impl_);
    }
    closure.provisional_count = 0;
    closure.transaction_active.store(false, std::memory_order_release);
}

bool LiveControlMailboxSet::transaction_active() const noexcept {
    return impl_ && impl_->closure &&
        impl_->closure->transaction_active.load(std::memory_order_acquire);
}

void LiveControlMailboxSet::expire_rate_releases_before(
    std::uint64_t logical_time_ns) noexcept {
    if (!impl_ || !impl_->admission_open.load(std::memory_order_acquire)) {
        return;
    }
    bool changed = false;
    for (std::size_t index = 0; index < impl_->rate_target_count; ++index) {
        if (impl_->rate_targets[index].release_time_ns < logical_time_ns &&
            !impl_->rate_targets[index].closed.exchange(
                true, std::memory_order_acq_rel)) {
            changed = true;
        }
    }
    if (changed) {
        LiveControlBoundaryTarget no_current_target;
        no_current_target.kind = LiveControlTargetKind::rate_release;
        no_current_target.frame_index = kInvalidSequence;
        (void)close_boundary(*impl_, no_current_target);
    }
}

bool LiveControlMailboxSet::mailbox_info(
    std::uint64_t mailbox_identity,
    LiveControlMailboxInfo& info) const noexcept {
    if (!impl_) {
        return false;
    }
    const auto index = find_mailbox(*impl_, mailbox_identity);
    if (index == impl_->mailboxes.size()) {
        return false;
    }
    const auto& mailbox = *impl_->mailboxes[index];
    LiveControlMailboxInfo candidate;
    candidate.runtime_id = impl_->runtime_id;
    candidate.configuration_generation = impl_->configuration_generation;
    candidate.policy_identity = impl_->policy.policy_identity;
    candidate.mailbox_identity = mailbox_identity;
    candidate.next_mailbox_sequence =
        mailbox.next_sequence.load(std::memory_order_acquire);
    candidate.accepted = mailbox.accepted.load(std::memory_order_acquire);
    candidate.invalid = mailbox.invalid.load(std::memory_order_acquire);
    candidate.full = mailbox.full.load(std::memory_order_acquire);
    candidate.busy = mailbox.busy.load(std::memory_order_acquire);
    candidate.stale = mailbox.stale.load(std::memory_order_acquire);
    candidate.stopped = mailbox.stopped.load(std::memory_order_acquire);
    candidate.exhausted = mailbox.exhausted.load(std::memory_order_acquire);
    candidate.record_capacity = mailbox.registration.record_capacity;
    candidate.payload_bytes_per_record =
        mailbox.registration.payload_bytes_per_record;
    candidate.occupancy = mailbox.occupancy.load(std::memory_order_acquire);
    candidate.producer_count = mailbox.producer_count;
    candidate.admission_open =
        impl_->admission_open.load(std::memory_order_acquire) ? 1u : 0u;
    info = candidate;
    return true;
}

bool LiveControlMailboxSet::record_at(
    std::uint64_t mailbox_identity,
    std::uint64_t mailbox_sequence,
    LiveControlUpdateRecord& record) const noexcept {
    if (!impl_ || mailbox_sequence == 0) {
        return false;
    }
    const auto index = find_mailbox(*impl_, mailbox_identity);
    if (index == impl_->mailboxes.size()) {
        return false;
    }
    auto& mailbox = *impl_->mailboxes[index];
    if (mailbox.reclaiming.load(std::memory_order_acquire)) {
        return false;
    }
    mailbox.inspection_readers.fetch_add(1, std::memory_order_acq_rel);
    if (mailbox.reclaiming.load(std::memory_order_acquire)) {
        mailbox.inspection_readers.fetch_sub(1, std::memory_order_release);
        return false;
    }
    bool found = false;
    LiveControlUpdateRecord candidate;
    for (std::size_t slot_index = 0;
         slot_index < mailbox.registration.record_capacity; ++slot_index) {
        const auto state = mailbox.slots[slot_index].state.load(
            std::memory_order_acquire);
        if (state == Impl::SlotState::free ||
            state == Impl::SlotState::writing) {
            continue;
        }
        if (mailbox.slots[slot_index].record.mailbox_sequence ==
            mailbox_sequence) {
            candidate = mailbox.slots[slot_index].record;
            found = true;
            break;
        }
    }
    mailbox.inspection_readers.fetch_sub(1, std::memory_order_release);
    if (found) {
        record = candidate;
    }
    return found;
}

Status LiveControlMailboxSet::copy_payload(
    std::uint64_t mailbox_identity,
    std::uint64_t mailbox_sequence,
    std::span<std::byte> output) const noexcept {
    if (!impl_ || mailbox_sequence == 0) {
        return Status::invalid_argument;
    }
    const auto mailbox_index = find_mailbox(*impl_, mailbox_identity);
    if (mailbox_index == impl_->mailboxes.size()) {
        return Status::invalid_argument;
    }
    auto& mailbox = *impl_->mailboxes[mailbox_index];
    if (mailbox.reclaiming.load(std::memory_order_acquire)) {
        return Status::invalid_argument;
    }
    mailbox.inspection_readers.fetch_add(1, std::memory_order_acq_rel);
    if (mailbox.reclaiming.load(std::memory_order_acquire)) {
        mailbox.inspection_readers.fetch_sub(1, std::memory_order_release);
        return Status::invalid_argument;
    }
    auto status = Status::invalid_argument;
    for (std::size_t slot_index = 0;
         slot_index < mailbox.registration.record_capacity; ++slot_index) {
        const auto state = mailbox.slots[slot_index].state.load(
            std::memory_order_acquire);
        if (state == Impl::SlotState::free ||
            state == Impl::SlotState::writing ||
            mailbox.slots[slot_index].record.mailbox_sequence !=
                mailbox_sequence) {
            continue;
        }
        const auto payload_bytes =
            mailbox.slots[slot_index].record.payload_bytes;
        if (output.size() != payload_bytes) {
            status = Status::capacity_exceeded;
            break;
        }
        const auto stride = static_cast<std::size_t>(
            mailbox.registration.payload_bytes_per_record);
        const auto* source = mailbox.payload_storage.get() +
            slot_index * stride;
        std::copy_n(source, output.size(), output.begin());
        status = Status::ok;
        break;
    }
    mailbox.inspection_readers.fetch_sub(1, std::memory_order_release);
    return status;
}

bool LiveControlMailboxSet::commit_info(
    LiveControlCommitInfo& info) const noexcept {
    if (!impl_) {
        return false;
    }
    const auto before = impl_->inspection_version.load(std::memory_order_acquire);
    if ((before & 1u) != 0) {
        return false;
    }
    LiveControlCommitInfo candidate;
    candidate.runtime_id = impl_->runtime_id;
    candidate.configuration_generation = impl_->configuration_generation;
    candidate.generation_identity =
        impl_->latest_generation_identity.load(std::memory_order_relaxed);
    candidate.survivor_count =
        impl_->latest_survivor_count.load(std::memory_order_relaxed);
    candidate.target.kind =
        impl_->latest_target_kind.load(std::memory_order_relaxed);
    candidate.target.frame_index =
        impl_->latest_frame_index.load(std::memory_order_relaxed);
    candidate.target.rate_release_sequence =
        impl_->latest_rate_sequence.load(std::memory_order_relaxed);
    candidate.target.reference_release_index =
        impl_->latest_reference_index.load(std::memory_order_relaxed);
    candidate.target.rate_domain_registration_index =
        impl_->latest_domain_index.load(std::memory_order_relaxed);
    candidate.target.phase_index =
        impl_->latest_phase_index.load(std::memory_order_relaxed);
    candidate.target.rate_substep_ordinal =
        impl_->latest_substep.load(std::memory_order_relaxed);
    candidate.committed = impl_->committed.load(std::memory_order_acquire);
    candidate.replaced = impl_->replaced.load(std::memory_order_acquire);
    candidate.missed = impl_->missed.load(std::memory_order_acquire);
    candidate.stopped = impl_->stopped.load(std::memory_order_acquire);
    std::uint64_t occupancy = 0;
    for (const auto& mailbox : impl_->mailboxes) {
        occupancy += mailbox->occupancy.load(std::memory_order_acquire);
    }
    if (occupancy > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    candidate.staged_occupancy = static_cast<std::uint32_t>(occupancy);
    const auto after = impl_->inspection_version.load(std::memory_order_acquire);
    if (before != after || (after & 1u) != 0) {
        return false;
    }
    info = candidate;
    return true;
}

bool LiveControlMailboxSet::record_status(
    std::uint64_t mailbox_identity,
    std::uint64_t mailbox_sequence,
    LiveControlRecordStatusInfo& info) const noexcept {
    if (!impl_ || mailbox_sequence == 0) {
        return false;
    }
    const auto mailbox_index = find_mailbox(*impl_, mailbox_identity);
    if (mailbox_index == impl_->mailboxes.size()) {
        return false;
    }
    auto& mailbox = *impl_->mailboxes[mailbox_index];
    if (mailbox.reclaiming.load(std::memory_order_acquire)) {
        return false;
    }
    mailbox.inspection_readers.fetch_add(1, std::memory_order_acq_rel);
    if (mailbox.reclaiming.load(std::memory_order_acquire)) {
        mailbox.inspection_readers.fetch_sub(1, std::memory_order_release);
        return false;
    }
    bool found = false;
    LiveControlRecordStatusInfo candidate;
    for (std::size_t index = 0;
         index < mailbox.registration.record_capacity; ++index) {
        const auto state = mailbox.slots[index].state.load(
            std::memory_order_acquire);
        if (state == Impl::SlotState::free ||
            state == Impl::SlotState::writing ||
            mailbox.slots[index].record.mailbox_sequence != mailbox_sequence) {
            continue;
        }
        const auto& record = mailbox.slots[index].record;
        candidate.runtime_id = impl_->runtime_id;
        candidate.configuration_generation = impl_->configuration_generation;
        candidate.mailbox_identity = record.mailbox_identity;
        candidate.mailbox_sequence = record.mailbox_sequence;
        candidate.producer_identity = record.producer_identity;
        candidate.producer_sequence = record.producer_sequence;
        candidate.generation_identity =
            mailbox.slots[index].terminal_generation.load(
                std::memory_order_acquire);
        candidate.status = public_status(state);
        candidate.update_kind = record.update_kind;
        candidate.target_kind = record.target_kind;
        found = true;
        break;
    }
    mailbox.inspection_readers.fetch_sub(1, std::memory_order_release);
    if (found) {
        info = candidate;
    }
    return found;
}

bool LiveControlMailboxSet::action_metadata(
    LiveControlActionMetadata& metadata) const noexcept {
    metadata = {};
    if (!impl_ || !impl_->closure) {
        return false;
    }
    const auto& closure = *impl_->closure;
    metadata.runtime_id = impl_->runtime_id;
    metadata.configuration_generation = impl_->configuration_generation;
    metadata.policy_identity = closure.policy.policy_identity;
    metadata.capacity = closure.actions->capacity();
    metadata.next_sequence = closure.actions->next_sequence();
    metadata.records_emitted = closure.actions->emitted();
    metadata.records_overwritten = closure.actions->overwritten();
    metadata.records_dropped = closure.actions->dropped();
    metadata.retained_generation_count =
        closure.retained_generation_published.load(std::memory_order_acquire);
    metadata.replay_eligible = closure.replay_eligible.load(
        std::memory_order_acquire) &&
        metadata.records_dropped == 0 && metadata.records_overwritten == 0;
    return true;
}

Status LiveControlMailboxSet::read_actions(
    LiveControlActionCursor& cursor,
    std::span<LiveControlActionRecord> output,
    LiveControlActionReadResult& result) const noexcept {
    result = {};
    if (!impl_ || !impl_->closure) {
        return Status::invalid_state;
    }
    if (cursor.schema_version != live_control_action_schema_version ||
        cursor.reserved32 != 0) {
        return Status::invalid_argument;
    }
    const bool fresh = cursor.runtime_id == 0 &&
        cursor.configuration_generation == 0 && cursor.next_sequence == 0;
    if (!fresh &&
        (cursor.runtime_id != impl_->runtime_id ||
         cursor.configuration_generation != impl_->configuration_generation)) {
        return Status::invalid_argument;
    }
    (void)action_metadata(result.metadata);
    const auto end = result.metadata.next_sequence;
    auto next = fresh
        ? impl_->closure->actions->oldest_sequence(end)
        : cursor.next_sequence;
    if (next > end) {
        return Status::invalid_argument;
    }
    const auto oldest = impl_->closure->actions->oldest_sequence(end);
    if (next < oldest) {
        result.lost_records = oldest - next;
        next = oldest;
    }
    result.first_sequence = next;
    while (next < end && result.records_read < output.size()) {
        LiveControlActionRecord record;
        if (impl_->closure->actions->read_sequence(next, record)) {
            output[result.records_read++] = record;
        } else {
            ++result.lost_records;
        }
        ++next;
    }
    result.next_sequence = next;
    result.remaining_sequence_count = end - next;
    cursor.runtime_id = impl_->runtime_id;
    cursor.configuration_generation = impl_->configuration_generation;
    cursor.next_sequence = next;
    return Status::ok;
}

std::size_t LiveControlMailboxSet::checkpoint_state_size() const noexcept {
    CheckpointLayout layout;
    return impl_ && impl_->closure && checkpoint_layout(*impl_, layout)
        ? layout.total_bytes
        : 0;
}

bool LiveControlMailboxSet::sync_checkpoint_state(
    std::span<const std::byte>& output) noexcept {
    output = {};
    if (!impl_ || !impl_->closure ||
        !write_checkpoint_state(std::span<std::byte>(
            impl_->closure->checkpoint_state.get(),
            impl_->closure->checkpoint_state_bytes))) {
        return false;
    }
    output = std::span<const std::byte>(
        impl_->closure->checkpoint_state.get(),
        impl_->closure->checkpoint_state_bytes);
    return true;
}

bool LiveControlMailboxSet::write_checkpoint_state(
    std::span<std::byte> output) const noexcept {
    CheckpointLayout layout;
    if (!impl_ || !impl_->closure ||
        impl_->closure->transaction_active.load(std::memory_order_acquire) ||
        !checkpoint_layout(*impl_, layout) ||
        output.size() != layout.total_bytes) {
        return false;
    }
    std::fill(output.begin(), output.end(), std::byte{0});
    const auto* generation = active_view(*impl_);
    const auto action_sequence = impl_->closure->actions->next_sequence();
    if (!store_u64(output, 0, kCheckpointMagic) ||
        !store_u32(output, 8, live_control_action_schema_version) ||
        !store_u32(output, 12, kCheckpointHeaderBytes) ||
        !store_u64(output, 16, layout.total_bytes) ||
        !store_u64(output, 32, impl_->closure->policy.policy_identity) ||
        !store_u64(output, 40, 0) ||
        !store_u64(output, 48, 0) ||
        !store_u32(output, 56, static_cast<std::uint32_t>(
            impl_->mailboxes.size())) ||
        !store_u32(output, 60, static_cast<std::uint32_t>(
            impl_->producer_count)) ||
        !store_u64(output, 64, total_record_capacity(*impl_)) ||
        !store_u64(output, 72, impl_->total_payload_storage_bytes) ||
        !store_u64(output, 80, impl_->rate_target_count) ||
        !store_u64(output, 88, layout.mailbox_offset) ||
        !store_u64(output, 96, layout.producer_offset) ||
        !store_u64(output, 104, layout.slot_offset) ||
        !store_u64(output, 112, layout.slot_payload_offset) ||
        !store_u64(output, 120, layout.generation_record_offset) ||
        !store_u64(output, 128, layout.generation_payload_offset) ||
        !store_u64(output, 136, layout.rate_target_offset) ||
        !store_u32(output, 144, generation ? 1u : 0u) ||
        !store_u32(output, 148, generation
            ? static_cast<std::uint32_t>(generation->records.size())
            : 0u) ||
        !store_u64(output, 152, generation
            ? generation->generation_identity : 0u) ||
        !store_u64(output, 160, generation
            ? generation->target.frame_index : kInvalidSequence) ||
        !store_u64(output, 168, generation
            ? generation->target.rate_release_sequence : kInvalidSequence) ||
        !store_u32(output, 176, generation
            ? generation->target.reference_release_index : kInvalidIndex) ||
        !store_u32(output, 180, generation
            ? generation->target.rate_domain_registration_index : kInvalidIndex) ||
        !store_u32(output, 184, generation
            ? generation->target.phase_index : kInvalidIndex) ||
        !store_u32(output, 188, generation
            ? generation->target.rate_substep_ordinal : kInvalidIndex) ||
        !store_u64(output, 200,
            impl_->last_host_boundary.load(std::memory_order_acquire)) ||
        !store_u32(output, 208,
            impl_->host_boundary_started.load(std::memory_order_acquire)
                ? 1u : 0u) ||
        !store_u32(output, 212,
            impl_->admission_open.load(std::memory_order_acquire) ? 1u : 0u) ||
        !store_u64(output, 216,
            impl_->committed.load(std::memory_order_acquire)) ||
        !store_u64(output, 224,
            impl_->replaced.load(std::memory_order_acquire)) ||
        !store_u64(output, 232,
            impl_->missed.load(std::memory_order_acquire)) ||
        !store_u64(output, 240,
            impl_->stopped.load(std::memory_order_acquire)) ||
        !store_u64(output, 248, action_sequence)) {
        return false;
    }
    output[192] = generation
        ? static_cast<std::byte>(generation->target.kind)
        : std::byte{0};

    std::size_t slot_ordinal = 0;
    std::size_t payload_ordinal = 0;
    for (std::size_t mailbox_index = 0;
         mailbox_index < impl_->mailboxes.size();
         ++mailbox_index) {
        const auto& mailbox = *impl_->mailboxes[mailbox_index];
        const auto offset = layout.mailbox_offset +
            mailbox_index * kCheckpointMailboxBytes;
        if (!store_u64(output, offset, mailbox.registration.mailbox_identity) ||
            !store_u64(output, offset + 8,
                mailbox.next_sequence.load(std::memory_order_acquire)) ||
            !store_u64(output, offset + 16,
                mailbox.accepted.load(std::memory_order_acquire)) ||
            !store_u64(output, offset + 24,
                mailbox.invalid.load(std::memory_order_acquire)) ||
            !store_u64(output, offset + 32,
                mailbox.full.load(std::memory_order_acquire)) ||
            !store_u64(output, offset + 40,
                mailbox.busy.load(std::memory_order_acquire)) ||
            !store_u64(output, offset + 48,
                mailbox.stale.load(std::memory_order_acquire)) ||
            !store_u64(output, offset + 56,
                mailbox.stopped.load(std::memory_order_acquire)) ||
            !store_u64(output, offset + 64,
                mailbox.exhausted.load(std::memory_order_acquire)) ||
            !store_u64(output, offset + 72,
                mailbox.missed.load(std::memory_order_acquire)) ||
            !store_u32(output, offset + 80,
                mailbox.occupancy.load(std::memory_order_acquire)) ||
            !store_u32(output, offset + 84,
                mailbox.registration.record_capacity) ||
            !store_u32(output, offset + 88,
                mailbox.registration.payload_bytes_per_record) ||
            !store_u32(output, offset + 92, mailbox.producer_count) ||
            !store_u32(output, offset + 96,
                impl_->admission_open.load(std::memory_order_acquire)
                    ? 1u : 0u)) {
            return false;
        }
        for (std::size_t slot_index = 0;
             slot_index < mailbox.registration.record_capacity;
             ++slot_index, ++slot_ordinal) {
            const auto& slot = mailbox.slots[slot_index];
            const auto state = slot.state.load(std::memory_order_acquire);
            if (state == Impl::SlotState::writing ||
                state == Impl::SlotState::boundary_owned) {
                return false;
            }
            const auto slot_offset = layout.slot_offset +
                slot_ordinal * kCheckpointSlotBytes;
            if (state != Impl::SlotState::free) {
                auto checkpoint_record = slot.record;
                checkpoint_record.runtime_id = 0;
                checkpoint_record.configuration_generation = 0;
                std::memcpy(
                    output.data() + slot_offset,
                    &checkpoint_record,
                    sizeof(checkpoint_record));
                const auto source = mailbox.payload_storage.get() +
                    slot_index *
                        mailbox.registration.payload_bytes_per_record;
                std::copy_n(
                    source,
                    slot.record.payload_bytes,
                    output.data() + layout.slot_payload_offset +
                        payload_ordinal);
            }
            if (!store_u64(output, slot_offset + 128,
                    slot.terminal_generation.load(std::memory_order_acquire))) {
                return false;
            }
            output[slot_offset + 136] = static_cast<std::byte>(state);
            payload_ordinal +=
                mailbox.registration.payload_bytes_per_record;
        }
    }
    for (std::size_t index = 0; index < impl_->producer_count; ++index) {
        const auto& producer = impl_->producers[index];
        const auto offset = layout.producer_offset +
            index * kCheckpointProducerBytes;
        if (!store_u64(output, offset, producer.mailbox_identity) ||
            !store_u64(output, offset + 8, producer.producer_identity) ||
            !store_u64(output, offset + 16, producer.first_sequence) ||
            !store_u64(output, offset + 24,
                producer.next_sequence.load(std::memory_order_acquire)) ||
            !store_u32(output, offset + 32, producer.mailbox_index)) {
            return false;
        }
    }
    if (generation) {
        std::size_t payload_offset = 0;
        for (std::size_t index = 0;
             index < generation->records.size();
             ++index) {
            const auto& record = generation->records[index];
            auto checkpoint_record = record.record;
            checkpoint_record.runtime_id = 0;
            checkpoint_record.configuration_generation = 0;
            const auto offset = layout.generation_record_offset +
                index * kCheckpointGenerationRecordBytes;
            std::memcpy(output.data() + offset, &checkpoint_record,
                        sizeof(checkpoint_record));
            if (!store_u64(output, offset + 128, payload_offset) ||
                !store_u32(output, offset + 136,
                    static_cast<std::uint32_t>(record.payload.size()))) {
                return false;
            }
            std::copy(
                record.payload.begin(),
                record.payload.end(),
                output.data() + layout.generation_payload_offset +
                    payload_offset);
            payload_offset += record.payload.size();
        }
    }
    for (std::size_t index = 0; index < impl_->rate_target_count; ++index) {
        if (!store_u32(
                output,
                layout.rate_target_offset +
                    index * kCheckpointRateTargetBytes,
                impl_->rate_targets[index].closed.load(
                    std::memory_order_acquire) ? 1u : 0u)) {
            return false;
        }
    }
    return store_u64(output, 24, checkpoint_checksum(output));
}

bool LiveControlMailboxSet::validate_checkpoint_state(
    std::span<const std::byte> input) const noexcept {
    CheckpointLayout layout;
    if (!impl_ || !impl_->closure ||
        !checkpoint_layout(*impl_, layout) || input.size() != layout.total_bytes ||
        impl_->configuration_generation == kInvalidSequence) {
        return false;
    }
    std::uint64_t magic = 0;
    std::uint32_t schema = 0;
    std::uint32_t header = 0;
    std::uint64_t total = 0;
    std::uint64_t checksum = 0;
    std::uint64_t policy_identity = 0;
    std::uint32_t mailbox_count = 0;
    std::uint32_t producer_count = 0;
    std::uint64_t record_count = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t rate_count = 0;
    std::array<std::uint64_t, 7> offsets{};
    std::uint32_t has_generation = 0;
    std::uint32_t active_record_count = 0;
    std::uint64_t generation_identity = 0;
    std::uint32_t host_started = 0;
    std::uint32_t admission_open = 0;
    std::uint64_t next_action = 0;
    if (!load_u64(input, 0, magic) || !load_u32(input, 8, schema) ||
        !load_u32(input, 12, header) || !load_u64(input, 16, total) ||
        !load_u64(input, 24, checksum) ||
        !load_u64(input, 32, policy_identity) ||
        !load_u32(input, 56, mailbox_count) ||
        !load_u32(input, 60, producer_count) ||
        !load_u64(input, 64, record_count) ||
        !load_u64(input, 72, payload_bytes) ||
        !load_u64(input, 80, rate_count) ||
        !load_u64(input, 88, offsets[0]) ||
        !load_u64(input, 96, offsets[1]) ||
        !load_u64(input, 104, offsets[2]) ||
        !load_u64(input, 112, offsets[3]) ||
        !load_u64(input, 120, offsets[4]) ||
        !load_u64(input, 128, offsets[5]) ||
        !load_u64(input, 136, offsets[6]) ||
        !load_u32(input, 144, has_generation) ||
        !load_u32(input, 148, active_record_count) ||
        !load_u64(input, 152, generation_identity) ||
        !load_u32(input, 208, host_started) ||
        !load_u32(input, 212, admission_open) ||
        !load_u64(input, 248, next_action) ||
        magic != kCheckpointMagic ||
        schema != live_control_action_schema_version ||
        header != kCheckpointHeaderBytes || total != input.size() ||
        checksum != checkpoint_checksum(input) ||
        policy_identity != impl_->closure->policy.policy_identity ||
        mailbox_count != impl_->mailboxes.size() ||
        producer_count != impl_->producer_count ||
        record_count != total_record_capacity(*impl_) ||
        payload_bytes != impl_->total_payload_storage_bytes ||
        rate_count != impl_->rate_target_count ||
        offsets != std::array<std::uint64_t, 7>{
            layout.mailbox_offset,
            layout.producer_offset,
            layout.slot_offset,
            layout.slot_payload_offset,
            layout.generation_record_offset,
            layout.generation_payload_offset,
            layout.rate_target_offset} ||
        has_generation > 1 || host_started > 1 || admission_open > 1 ||
        active_record_count > total_record_capacity(*impl_) ||
        (has_generation == 0 &&
         (active_record_count != 0 || generation_identity != 0)) ||
        (has_generation != 0 &&
         (active_record_count == 0 || generation_identity == 0)) ||
        next_action == kInvalidSequence) {
        return false;
    }

    std::size_t slot_ordinal = 0;
    std::size_t payload_ordinal = 0;
    for (std::size_t mailbox_index = 0;
         mailbox_index < impl_->mailboxes.size();
         ++mailbox_index) {
        const auto& mailbox = *impl_->mailboxes[mailbox_index];
        const auto offset = layout.mailbox_offset +
            mailbox_index * kCheckpointMailboxBytes;
        std::uint64_t identity = 0;
        std::uint64_t next_sequence = 0;
        std::uint32_t occupancy = 0;
        std::uint32_t capacity = 0;
        std::uint32_t stride = 0;
        std::uint32_t mailbox_producers = 0;
        std::uint32_t open = 0;
        if (!load_u64(input, offset, identity) ||
            !load_u64(input, offset + 8, next_sequence) ||
            !load_u32(input, offset + 80, occupancy) ||
            !load_u32(input, offset + 84, capacity) ||
            !load_u32(input, offset + 88, stride) ||
            !load_u32(input, offset + 92, mailbox_producers) ||
            !load_u32(input, offset + 96, open) ||
            identity != mailbox.registration.mailbox_identity ||
            next_sequence == 0 || next_sequence == kInvalidSequence ||
            capacity != mailbox.registration.record_capacity ||
            stride != mailbox.registration.payload_bytes_per_record ||
            mailbox_producers != mailbox.producer_count || open != admission_open ||
            occupancy > capacity) {
            return false;
        }
        std::uint32_t staged_count = 0;
        for (std::size_t slot_index = 0;
             slot_index < mailbox.registration.record_capacity;
             ++slot_index, ++slot_ordinal) {
            const auto slot_offset = layout.slot_offset +
                slot_ordinal * kCheckpointSlotBytes;
            const auto state = static_cast<Impl::SlotState>(
                std::to_integer<std::uint8_t>(input[slot_offset + 136]));
            if (state == Impl::SlotState::writing ||
                state == Impl::SlotState::boundary_owned ||
                state > Impl::SlotState::rolled_back) {
                return false;
            }
            if (state == Impl::SlotState::free) {
                for (std::size_t byte = 0; byte < sizeof(LiveControlUpdateRecord);
                     ++byte) {
                    if (input[slot_offset + byte] != std::byte{0}) {
                        return false;
                    }
                }
            } else {
                LiveControlUpdateRecord record;
                std::memcpy(&record, input.data() + slot_offset, sizeof(record));
                const auto producer_index = std::find_if(
                    impl_->producers.get(),
                    impl_->producers.get() + impl_->producer_count,
                    [&](const Impl::Producer& producer) {
                        return producer.mailbox_identity == record.mailbox_identity &&
                            producer.producer_identity == record.producer_identity;
                    });
                if (producer_index == impl_->producers.get() +
                        impl_->producer_count ||
                    record.mailbox_sequence == 0 ||
                    record.mailbox_sequence >= next_sequence ||
                    record.payload_bytes > stride) {
                    return false;
                }
                const auto payload = input.subspan(
                    layout.slot_payload_offset + payload_ordinal,
                    record.payload_bytes);
                auto candidate = record;
                candidate.mailbox_sequence = 0;
                candidate.runtime_id = impl_->runtime_id;
                candidate.configuration_generation =
                    impl_->configuration_generation;
                if (!structurally_valid(
                        *impl_, mailbox, *producer_index, candidate, payload)) {
                    return false;
                }
                staged_count += state == Impl::SlotState::staged ? 1u : 0u;
            }
            payload_ordinal += stride;
        }
        if (staged_count != occupancy) {
            return false;
        }
    }
    for (std::size_t index = 0; index < impl_->producer_count; ++index) {
        const auto& producer = impl_->producers[index];
        const auto offset = layout.producer_offset +
            index * kCheckpointProducerBytes;
        std::uint64_t mailbox_identity = 0;
        std::uint64_t producer_identity = 0;
        std::uint64_t first_sequence = 0;
        std::uint64_t next_sequence = 0;
        std::uint32_t mailbox_index = 0;
        if (!load_u64(input, offset, mailbox_identity) ||
            !load_u64(input, offset + 8, producer_identity) ||
            !load_u64(input, offset + 16, first_sequence) ||
            !load_u64(input, offset + 24, next_sequence) ||
            !load_u32(input, offset + 32, mailbox_index) ||
            mailbox_identity != producer.mailbox_identity ||
            producer_identity != producer.producer_identity ||
            first_sequence != producer.first_sequence ||
            next_sequence < first_sequence || next_sequence == kInvalidSequence ||
            mailbox_index != producer.mailbox_index) {
            return false;
        }
    }
    if (has_generation != 0) {
        LiveControlBoundaryTarget target;
        std::uint8_t target_kind =
            std::to_integer<std::uint8_t>(input[192]);
        target.kind = static_cast<LiveControlTargetKind>(target_kind);
        if (!load_u64(input, 160, target.frame_index) ||
            !load_u64(input, 168, target.rate_release_sequence) ||
            !load_u32(input, 176, target.reference_release_index) ||
            !load_u32(input, 180, target.rate_domain_registration_index) ||
            !load_u32(input, 184, target.phase_index) ||
            !load_u32(input, 188, target.rate_substep_ordinal) ||
            (target.kind != LiveControlTargetKind::host_frame &&
             target.kind != LiveControlTargetKind::rate_release)) {
            return false;
        }
        std::uint64_t identity = kFnvOffset;
        hash_target(identity, target);
        std::uint64_t previous_mailbox = 0;
        std::uint64_t previous_sequence = 0;
        for (std::size_t index = 0; index < active_record_count; ++index) {
            const auto offset = layout.generation_record_offset +
                index * kCheckpointGenerationRecordBytes;
            LiveControlUpdateRecord record;
            std::memcpy(&record, input.data() + offset, sizeof(record));
            std::uint64_t payload_offset = 0;
            std::uint32_t record_payload_bytes = 0;
            if (!load_u64(input, offset + 128, payload_offset) ||
                !load_u32(input, offset + 136, record_payload_bytes) ||
                record.payload_bytes != record_payload_bytes ||
                payload_offset > impl_->total_payload_storage_bytes ||
                record_payload_bytes > impl_->total_payload_storage_bytes -
                    payload_offset ||
                (index != 0 &&
                 (record.mailbox_identity < previous_mailbox ||
                  (record.mailbox_identity == previous_mailbox &&
                   record.mailbox_sequence <= previous_sequence)))) {
                return false;
            }
            const auto mailbox_index = find_mailbox(
                *impl_, record.mailbox_identity);
            const auto producer_index = std::find_if(
                impl_->producers.get(),
                impl_->producers.get() + impl_->producer_count,
                [&](const Impl::Producer& producer) {
                    return producer.mailbox_identity == record.mailbox_identity &&
                        producer.producer_identity == record.producer_identity;
                });
            if (mailbox_index == impl_->mailboxes.size() ||
                producer_index == impl_->producers.get() + impl_->producer_count) {
                return false;
            }
            auto candidate = record;
            candidate.mailbox_sequence = 0;
            candidate.runtime_id = impl_->runtime_id;
            candidate.configuration_generation = impl_->configuration_generation;
            const auto payload = input.subspan(
                layout.generation_payload_offset + payload_offset,
                record_payload_bytes);
            if (!structurally_valid(
                    *impl_,
                    *impl_->mailboxes[mailbox_index],
                    *producer_index,
                    candidate,
                    payload)) {
                return false;
            }
            hash_u64(identity, record.mailbox_identity);
            hash_u64(identity, record.mailbox_sequence);
            hash_u64(identity, record.producer_identity);
            hash_u64(identity, record.producer_sequence);
            hash_u64(identity, static_cast<std::uint8_t>(record.update_kind));
            hash_u64(identity, record.payload_digest);
            previous_mailbox = record.mailbox_identity;
            previous_sequence = record.mailbox_sequence;
        }
        if (identity == 0) {
            identity = 1;
        }
        if (identity != generation_identity) {
            return false;
        }
    }
    for (std::size_t index = 0; index < impl_->rate_target_count; ++index) {
        std::uint32_t closed = 0;
        if (!load_u32(
                input,
                layout.rate_target_offset +
                    index * kCheckpointRateTargetBytes,
                closed) || closed > 1) {
            return false;
        }
    }
    return true;
}

bool LiveControlMailboxSet::restore_checkpoint_state(
    std::span<const std::byte> input) noexcept {
    if (!validate_checkpoint_state(input)) {
        return false;
    }
    CheckpointLayout layout;
    (void)checkpoint_layout(*impl_, layout);
    ++impl_->configuration_generation;
    std::uint32_t has_generation = 0;
    std::uint32_t active_record_count = 0;
    std::uint64_t generation_identity = 0;
    std::uint32_t host_started = 0;
    std::uint32_t admission_open = 0;
    std::uint64_t last_host = 0;
    std::uint64_t committed = 0;
    std::uint64_t replaced = 0;
    std::uint64_t missed = 0;
    std::uint64_t stopped = 0;
    std::uint64_t next_action = 0;
    (void)load_u32(input, 144, has_generation);
    (void)load_u32(input, 148, active_record_count);
    (void)load_u64(input, 152, generation_identity);
    (void)load_u64(input, 200, last_host);
    (void)load_u32(input, 208, host_started);
    (void)load_u32(input, 212, admission_open);
    (void)load_u64(input, 216, committed);
    (void)load_u64(input, 224, replaced);
    (void)load_u64(input, 232, missed);
    (void)load_u64(input, 240, stopped);
    (void)load_u64(input, 248, next_action);
    impl_->host_boundary_started.store(host_started != 0, std::memory_order_release);
    impl_->last_host_boundary.store(last_host, std::memory_order_release);
    impl_->admission_open.store(admission_open != 0, std::memory_order_release);
    impl_->committed.store(committed, std::memory_order_release);
    impl_->replaced.store(replaced, std::memory_order_release);
    impl_->missed.store(missed, std::memory_order_release);
    impl_->stopped.store(stopped, std::memory_order_release);

    std::size_t slot_ordinal = 0;
    std::size_t payload_ordinal = 0;
    for (std::size_t mailbox_index = 0;
         mailbox_index < impl_->mailboxes.size();
         ++mailbox_index) {
        auto& mailbox = *impl_->mailboxes[mailbox_index];
        const auto offset = layout.mailbox_offset +
            mailbox_index * kCheckpointMailboxBytes;
        std::uint64_t value64 = 0;
        std::uint32_t value32 = 0;
        (void)load_u64(input, offset + 8, value64);
        mailbox.next_sequence.store(value64, std::memory_order_release);
        const std::array<std::atomic<std::uint64_t>*, 8> counters{
            &mailbox.accepted, &mailbox.invalid, &mailbox.full, &mailbox.busy,
            &mailbox.stale, &mailbox.stopped, &mailbox.exhausted, &mailbox.missed};
        for (std::size_t counter = 0; counter < counters.size(); ++counter) {
            (void)load_u64(input, offset + 16 + counter * 8, value64);
            counters[counter]->store(value64, std::memory_order_release);
        }
        (void)load_u32(input, offset + 80, value32);
        mailbox.occupancy.store(value32, std::memory_order_release);
        for (std::size_t slot_index = 0;
             slot_index < mailbox.registration.record_capacity;
             ++slot_index, ++slot_ordinal) {
            auto& slot = mailbox.slots[slot_index];
            const auto slot_offset = layout.slot_offset +
                slot_ordinal * kCheckpointSlotBytes;
            const auto state = static_cast<Impl::SlotState>(
                std::to_integer<std::uint8_t>(input[slot_offset + 136]));
            slot.record = {};
            std::fill_n(
                mailbox.payload_storage.get() +
                    slot_index * mailbox.registration.payload_bytes_per_record,
                mailbox.registration.payload_bytes_per_record,
                std::byte{0});
            if (state != Impl::SlotState::free) {
                std::memcpy(&slot.record, input.data() + slot_offset,
                            sizeof(slot.record));
                slot.record.runtime_id = impl_->runtime_id;
                slot.record.configuration_generation =
                    impl_->configuration_generation;
                std::copy_n(
                    input.data() + layout.slot_payload_offset + payload_ordinal,
                    slot.record.payload_bytes,
                    mailbox.payload_storage.get() +
                        slot_index *
                            mailbox.registration.payload_bytes_per_record);
            }
            (void)load_u64(input, slot_offset + 128, value64);
            slot.terminal_generation.store(value64, std::memory_order_release);
            slot.state.store(state, std::memory_order_release);
            payload_ordinal += mailbox.registration.payload_bytes_per_record;
        }
    }
    for (std::size_t index = 0; index < impl_->producer_count; ++index) {
        std::uint64_t next_sequence = 0;
        (void)load_u64(
            input,
            layout.producer_offset + index * kCheckpointProducerBytes + 24,
            next_sequence);
        impl_->producers[index].next_sequence.store(
            next_sequence, std::memory_order_release);
    }
    if (has_generation != 0) {
        auto& generation = impl_->generations[0];
        generation.view.runtime_id = impl_->runtime_id;
        generation.view.configuration_generation =
            impl_->configuration_generation;
        generation.view.generation_identity = generation_identity;
        generation.view.target.kind = static_cast<LiveControlTargetKind>(
            std::to_integer<std::uint8_t>(input[192]));
        (void)load_u64(input, 160, generation.view.target.frame_index);
        (void)load_u64(
            input, 168, generation.view.target.rate_release_sequence);
        (void)load_u32(
            input, 176, generation.view.target.reference_release_index);
        (void)load_u32(
            input, 180,
            generation.view.target.rate_domain_registration_index);
        (void)load_u32(input, 184, generation.view.target.phase_index);
        (void)load_u32(
            input, 188, generation.view.target.rate_substep_ordinal);
        for (std::size_t index = 0; index < active_record_count; ++index) {
            const auto offset = layout.generation_record_offset +
                index * kCheckpointGenerationRecordBytes;
            auto& record = generation.records[index];
            std::memcpy(&record.record, input.data() + offset,
                        sizeof(record.record));
            record.record.runtime_id = impl_->runtime_id;
            record.record.configuration_generation =
                impl_->configuration_generation;
            std::uint64_t payload_offset = 0;
            std::uint32_t record_payload_bytes = 0;
            (void)load_u64(input, offset + 128, payload_offset);
            (void)load_u32(input, offset + 136, record_payload_bytes);
            std::copy_n(
                input.data() + layout.generation_payload_offset + payload_offset,
                record_payload_bytes,
                generation.payloads.get() + payload_offset);
            record.payload = std::span<const std::byte>(
                generation.payloads.get() + payload_offset,
                record_payload_bytes);
        }
        generation.view.records = std::span<const LiveControlRecordView>(
            generation.records.get(), active_record_count);
        impl_->active_generation.store(0, std::memory_order_release);
        impl_->has_generation.store(true, std::memory_order_release);
        impl_->latest_generation_identity.store(
            generation_identity, std::memory_order_release);
        impl_->latest_frame_index.store(
            generation.view.target.frame_index, std::memory_order_release);
        impl_->latest_rate_sequence.store(
            generation.view.target.rate_release_sequence,
            std::memory_order_release);
        impl_->latest_reference_index.store(
            generation.view.target.reference_release_index,
            std::memory_order_release);
        impl_->latest_domain_index.store(
            generation.view.target.rate_domain_registration_index,
            std::memory_order_release);
        impl_->latest_phase_index.store(
            generation.view.target.phase_index, std::memory_order_release);
        impl_->latest_substep.store(
            generation.view.target.rate_substep_ordinal,
            std::memory_order_release);
        impl_->latest_survivor_count.store(
            active_record_count, std::memory_order_release);
        impl_->latest_target_kind.store(
            generation.view.target.kind, std::memory_order_release);
    } else {
        impl_->has_generation.store(false, std::memory_order_release);
        impl_->latest_generation_identity.store(0, std::memory_order_release);
        impl_->latest_survivor_count.store(0, std::memory_order_release);
    }
    for (std::size_t index = 0; index < impl_->rate_target_count; ++index) {
        std::uint32_t closed = 0;
        (void)load_u32(
            input,
            layout.rate_target_offset + index * kCheckpointRateTargetBytes,
            closed);
        impl_->rate_targets[index].closed.store(
            closed != 0, std::memory_order_release);
    }
    impl_->closure->actions->restore_sequence(
        impl_->closure->replay_view
            ? impl_->closure->replay_view->metadata.last_action_sequence + 1
            : next_action);
    if (impl_->closure->lossless) {
        auto& journal = *impl_->closure->lossless;
        journal.admission_count.store(0, std::memory_order_relaxed);
        journal.payload_count.store(0, std::memory_order_relaxed);
        journal.admission_close_sequence.store(kInvalidSequence,
                                               std::memory_order_relaxed);
        journal.export_count = 0;
        journal.replay_admission_index = 0;
    }
    impl_->closure->retained_generation_count = 0;
    impl_->closure->retained_generation_published.store(
        0, std::memory_order_release);
    impl_->closure->retained_record_count = 0;
    impl_->closure->retained_payload_count = 0;
    impl_->closure->replay_eligible.store(true, std::memory_order_release);
    if (impl_->closure->replay_view) {
        apply_replay_history();
        return impl_->closure->replay_mismatch == Status::ok;
    }
    return true;
}

void LiveControlMailboxSet::record_checkpoint(
    std::uint64_t correlation) noexcept {
    if (!impl_ || !impl_->closure || correlation == 0) {
        return;
    }
    auto action = base_action(*impl_);
    const auto* current = active_view(*impl_);
    if (current) {
        action.target = current->target;
        action.generation_identity = current->generation_identity;
        action.prior_generation_identity = current->generation_identity;
    }
    action.checkpoint_correlation = correlation;
    action.action = LiveControlActionId::checkpointed;
    action.stage = LiveControlActionStage::checkpoint;
    action.reason = LiveControlActionReason::checkpoint;
    action.result = LiveControlActionResult::settled;
    action.replay_eligible = impl_->closure->replay_eligible.load(
        std::memory_order_acquire);
    emit_action(*impl_, action);
}

void LiveControlMailboxSet::record_replay_verified(
    std::uint64_t correlation) noexcept {
    if (!impl_ || !impl_->closure || correlation == 0 ||
        impl_->closure->replay_view) {
        return;
    }
    auto action = base_action(*impl_);
    const auto* current = active_view(*impl_);
    if (current) {
        action.target = current->target;
        action.generation_identity = current->generation_identity;
        action.prior_generation_identity = current->generation_identity;
    }
    action.replay_correlation = correlation;
    action.action = LiveControlActionId::replay_verified;
    action.stage = LiveControlActionStage::replay;
    action.reason = LiveControlActionReason::replay;
    action.result = LiveControlActionResult::verified;
    action.replay_eligible = impl_->closure->replay_eligible.load(
        std::memory_order_acquire);
    emit_action(*impl_, action);
}

bool LiveControlMailboxSet::checkpoint_action_sequence(
    std::span<const std::byte> state,
    std::uint64_t& sequence) const noexcept {
    sequence = 0;
    return validate_checkpoint_state(state) &&
        load_u64(state, 248, sequence) && sequence != kInvalidSequence;
}

Status LiveControlMailboxSet::write_replay_artifact(
    LiveControlReplayMetadata metadata,
    std::span<const std::byte> checkpoint,
    std::span<const std::byte> nested_artifact,
    std::uint64_t first_action_sequence,
    std::span<std::byte> output,
    ArtifactWriteResult& result) noexcept {
    result = {};
    if (!impl_ || !impl_->closure ||
        !impl_->closure->policy.replay_enabled ||
        impl_->closure->transaction_active.load(std::memory_order_acquire) ||
        impl_->closure->replay_view) {
        return Status::invalid_state;
    }
    auto& closure = *impl_->closure;
    const auto next_action = closure.actions->next_sequence();
    if (first_action_sequence > next_action ||
        first_action_sequence == next_action ||
        next_action - first_action_sequence >
            closure.policy.replay_record_capacity ||
        closure.actions->dropped() != 0 ||
        closure.actions->overwritten() != 0 ||
        !closure.replay_eligible.load(std::memory_order_acquire) ||
        !closure.actions->gap_free(
            first_action_sequence,
            next_action - first_action_sequence)) {
        return Status::invalid_artifact;
    }
    if (closure.lossless) {
        auto& journal = *closure.lossless;
        const auto count = journal.admission_count.load(std::memory_order_acquire);
        const auto bytes = journal.payload_count.load(std::memory_order_acquire);
        if (count > journal.policy.admission_capacity ||
            bytes > journal.policy.payload_capacity_bytes) {
            return Status::invalid_artifact;
        }
        journal.export_count = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const auto& admission = journal.admissions[index];
            if (admission.action_sequence >= first_action_sequence) {
                if (admission.action_sequence >= next_action ||
                    admission.payload_offset > bytes ||
                    (admission.slot_index != kInvalidIndex &&
                     admission.record.payload_bytes >
                         bytes - admission.payload_offset)) {
                    return Status::invalid_artifact;
                }
                journal.export_order[journal.export_count++] = index;
            }
        }
        std::sort(journal.export_order.get(),
                  journal.export_order.get() + journal.export_count,
                  [&](std::size_t left, std::size_t right) {
                      return journal.admissions[left].action_sequence <
                             journal.admissions[right].action_sequence;
                  });
    }
    std::size_t first_generation = 0;
    while (first_generation < closure.retained_generation_count &&
           closure.retained_generations[first_generation]
                   .first_action_sequence < first_action_sequence) {
        ++first_generation;
    }
    struct ReaderContext {
        Impl* impl = nullptr;
        std::size_t first_generation = 0;
    } context{impl_.get(), first_generation};
    return encode_live_control_replay_artifact(
        metadata, checkpoint, nested_artifact, first_action_sequence,
        static_cast<std::size_t>(next_action - first_action_sequence),
        [](void* opaque, std::uint64_t sequence,
           LiveControlActionRecord& record) noexcept {
            auto& reader = *static_cast<ReaderContext*>(opaque);
            return reader.impl->closure->actions->read_sequence(
                sequence, record);
        },
        closure.retained_generation_count - first_generation,
        [](void* opaque, std::size_t index,
           LiveControlRetainedGenerationView& descriptor) noexcept {
            auto& reader = *static_cast<ReaderContext*>(opaque);
            const auto resolved = reader.first_generation + index;
            if (resolved >=
                    reader.impl->closure->retained_generation_count) {
                return false;
            }
            const auto& generation =
                reader.impl->closure->retained_generations[resolved];
            descriptor.target = generation.target;
            descriptor.generation_identity = generation.generation_identity;
            descriptor.prior_generation_identity =
                generation.prior_generation_identity;
            descriptor.first_action_sequence = generation.first_action_sequence;
            descriptor.record_count = generation.record_count;
            descriptor.payload_bytes = generation.payload_bytes;
            descriptor.terminal_status = generation.terminal_status;
            descriptor.settled = generation.settled;
            return true;
        },
        [](void* opaque, std::size_t generation_index, std::size_t record_index,
           LiveControlUpdateRecord& record,
           std::span<const std::byte>& payload) noexcept {
            auto& reader = *static_cast<ReaderContext*>(opaque);
            const auto resolved =
                reader.first_generation + generation_index;
            if (resolved >=
                    reader.impl->closure->retained_generation_count) {
                return false;
            }
            const auto& generation =
                reader.impl->closure->retained_generations[resolved];
            if (record_index >= generation.record_count) {
                return false;
            }
            const auto& retained = reader.impl->closure->retained_records[
                generation.first_record_index + record_index];
            record = retained.record;
            payload = std::span<const std::byte>(
                reader.impl->closure->retained_payloads.get() +
                    retained.payload_offset,
                retained.record.payload_bytes);
            return true;
        },
        &context, closure.policy.replay_max_bytes, output, result,
        closure.lossless ? closure.lossless->policy.policy_identity : 0,
        closure.lossless ? closure.lossless->export_count : 0,
        [](void* opaque, std::size_t index, LiveControlRetainedAdmission& admission,
           std::span<const std::byte>& payload) noexcept {
            auto& reader = *static_cast<ReaderContext*>(opaque);
            if (!reader.impl->closure->lossless) {
                return false;
            }
            const auto& journal = *reader.impl->closure->lossless;
            if (index >= journal.export_count) {
                return false;
            }
            admission = journal.admissions[journal.export_order[index]];
            payload = std::span<const std::byte>(
                journal.payloads.get() + admission.payload_offset,
                admission.slot_index == kInvalidIndex ? 0
                                                      : admission.record.payload_bytes);
            return true;
        },
        closure.lossless
            ? closure.lossless->admission_close_sequence.load(std::memory_order_acquire)
            : kInvalidSequence);
}

bool LiveControlMailboxSet::validate_replay_artifact(
    const LiveControlReplayArtifactView& view) const noexcept {
    if (!impl_ || !impl_->closure ||
        view.metadata.policy_identity != impl_->closure->policy.policy_identity) {
        return false;
    }
    CheckpointMetadata checkpoint_metadata;
    if (inspect_checkpoint_artifact(
            view.checkpoint, checkpoint_metadata) != Status::ok) {
        return false;
    }
    CheckpointRecordCursor cursor;
    CheckpointRecordView checkpoint_record;
    std::span<const std::byte> state;
    for (std::size_t index = 0;
         index < checkpoint_metadata.state_count;
         ++index) {
        if (!next_checkpoint_record(
                view.checkpoint,
                checkpoint_metadata,
                cursor,
                checkpoint_record)) {
            return false;
        }
        if (checkpoint_record.name == "rtfw.live-control") {
            if (!state.empty() ||
                checkpoint_record.schema_version !=
                    live_control_action_schema_version) {
                return false;
            }
            state = checkpoint_record.payload;
        }
    }
    CheckpointLayout layout;
    if (state.empty() || !validate_checkpoint_state(state) ||
        !checkpoint_layout(*impl_, layout)) {
        return false;
    }

    const auto producer_for = [&](std::uint64_t mailbox_identity,
                                  std::uint64_t producer_identity)
        -> const Impl::Producer* {
        for (std::size_t index = 0; index < impl_->producer_count; ++index) {
            const auto& producer = impl_->producers[index];
            if (producer.mailbox_identity == mailbox_identity &&
                producer.producer_identity == producer_identity) {
                return &producer;
            }
        }
        return nullptr;
    };
    const auto action_topology_valid = [&](const LiveControlActionRecord& action) {
        const auto mailbox_index = find_mailbox(*impl_, action.mailbox_identity);
        const auto* producer = producer_for(
            action.mailbox_identity, action.producer_identity);
        if (mailbox_index == impl_->mailboxes.size() || !producer ||
            producer->mailbox_index != mailbox_index) {
            return false;
        }
        const auto& mailbox = *impl_->mailboxes[mailbox_index];
        LiveControlUpdateRecord record;
        record.target_kind = action.target.kind;
        record.target_frame_index = action.target.frame_index;
        record.rate_release_sequence = action.target.rate_release_sequence;
        record.reference_release_index = action.target.reference_release_index;
        record.rate_domain_registration_index =
            action.target.rate_domain_registration_index;
        record.phase_index = action.target.phase_index;
        record.rate_substep_ordinal = action.target.rate_substep_ordinal;
        record.update_kind = action.update_kind;
        return action.mailbox_sequence != kInvalidSequence &&
               action.producer_sequence != kInvalidSequence &&
               action.producer_sequence >= producer->first_sequence &&
               action.payload_bytes <= mailbox.registration.payload_bytes_per_record &&
               (action.payload_bytes != 0 ||
                action.update_kind == LiveControlUpdateKind::clear_fault) &&
               target_valid(*impl_, record);
    };
    const auto action_record_equal = [](const LiveControlActionRecord& action,
                                        const LiveControlUpdateRecord& record) {
        return action.mailbox_identity == record.mailbox_identity &&
            action.producer_identity == record.producer_identity &&
            action.mailbox_sequence == record.mailbox_sequence &&
            action.producer_sequence == record.producer_sequence &&
            action.update_kind == record.update_kind &&
            action.payload_bytes == record.payload_bytes &&
            action.payload_digest == record.payload_digest &&
            targets_equal(action.target, target_from_record(record));
    };
    const auto checkpoint_has_staged = [&](const LiveControlActionRecord& action) {
        std::size_t slot_ordinal = 0;
        for (const auto& mailbox : impl_->mailboxes) {
            for (std::size_t slot = 0;
                 slot < mailbox->registration.record_capacity;
                 ++slot, ++slot_ordinal) {
                const auto offset = layout.slot_offset +
                    slot_ordinal * kCheckpointSlotBytes;
                if (static_cast<Impl::SlotState>(
                        std::to_integer<std::uint8_t>(state[offset + 136])) !=
                    Impl::SlotState::staged) {
                    continue;
                }
                LiveControlUpdateRecord record;
                std::memcpy(&record, state.data() + offset, sizeof(record));
                if (action_record_equal(action, record)) {
                    return true;
                }
            }
        }
        return false;
    };
    const auto matching_accepted_before = [&](std::size_t end,
                                               const LiveControlActionRecord& terminal) {
        for (std::size_t index = 0; index < end; ++index) {
            LiveControlActionRecord admission;
            if (!live_control_replay_action_at(view, index, admission)) {
                return false;
            }
            if (admission.action == LiveControlActionId::admission &&
                admission.admission_result ==
                    LiveControlAdmissionResult::accepted &&
                admission.mailbox_identity == terminal.mailbox_identity &&
                admission.producer_identity == terminal.producer_identity &&
                admission.mailbox_sequence == terminal.mailbox_sequence &&
                admission.producer_sequence == terminal.producer_sequence &&
                admission.update_kind == terminal.update_kind &&
                admission.payload_bytes == terminal.payload_bytes &&
                admission.payload_digest == terminal.payload_digest &&
                targets_equal(admission.target, terminal.target)) {
                return true;
            }
        }
        return false;
    };

    for (std::size_t generation = 0;
         generation < view.metadata.retained_generation_count;
         ++generation) {
        LiveControlRetainedGenerationView descriptor;
        if (!live_control_replay_generation_at(view, generation, descriptor)) {
            return false;
        }
        for (std::size_t record_index = 0;
             record_index < descriptor.record_count;
             ++record_index) {
            LiveControlUpdateRecord record;
            std::span<const std::byte> payload;
            if (!live_control_replay_record_at(
                    view, generation, record_index, record, payload)) {
                return false;
            }
            const auto mailbox_index = find_mailbox(
                *impl_, record.mailbox_identity);
            const auto* producer = producer_for(
                record.mailbox_identity, record.producer_identity);
            if (mailbox_index == impl_->mailboxes.size() || !producer ||
                record.producer_sequence < producer->first_sequence) {
                return false;
            }
            auto candidate = record;
            candidate.runtime_id = impl_->runtime_id;
            candidate.configuration_generation = impl_->configuration_generation;
            candidate.mailbox_sequence = 0;
            if (!structurally_valid(
                    *impl_,
                    *impl_->mailboxes[mailbox_index],
                    *producer,
                    candidate,
                    payload)) {
                return false;
            }
        }
    }

    if (view.metadata.schema_version == live_control_lossless_replay_schema_version) {
        if (!impl_->closure->lossless ||
            view.retention_policy_identity !=
                impl_->closure->lossless->policy.policy_identity ||
            view.admission_count >
                impl_->closure->lossless->policy.admission_capacity ||
            view.admission_payload_bytes >
                impl_->closure->lossless->policy.payload_capacity_bytes) {
            return false;
        }
        // Validate ownership on finalized private scratch before restoring any
        // application or mailbox state. The caller holds the exclusive claim.
        auto scratch = std::span<std::byte>(
            impl_->closure->lossless->validation_state.get(), state.size());
        std::copy(state.begin(), state.end(), scratch.begin());
        const auto slot_offset = [&](std::size_t owner, std::size_t slot) {
            for (std::size_t index = 0; index < owner; ++index) {
                slot += impl_->mailboxes[index]->registration.record_capacity;
            }
            return layout.slot_offset + slot * kCheckpointSlotBytes;
        };
        const auto slot_state = [&](std::size_t offset) {
            return static_cast<Impl::SlotState>(
                std::to_integer<std::uint8_t>(scratch[offset + 136]));
        };
        const auto adjust_occupancy = [&](std::size_t owner, bool add) {
            const auto offset =
                layout.mailbox_offset + owner * kCheckpointMailboxBytes + 80;
            std::uint32_t count = 0;
            (void)load_u32(scratch, offset, count);
            if (add ? count >= impl_->mailboxes[owner]->registration.record_capacity
                    : count == 0) {
                return false;
            }
            return store_u32(scratch, offset, add ? count + 1 : count - 1);
        };
        std::size_t next_admission = 0;
        for (std::size_t index = 0; index < view.metadata.action_record_count;
             ++index) {
            LiveControlActionRecord action;
            if (!live_control_replay_action_at(view, index, action) ||
                !action.replay_eligible) {
                return false;
            }
            if (action.action == LiveControlActionId::admission) {
                LiveControlRetainedAdmission admission;
                std::span<const std::byte> payload;
                if (!live_control_replay_admission_at(view, next_admission++, admission,
                                                      payload) ||
                    admission.action_sequence != action.sequence ||
                    admission.outcome != action.admission_result) {
                    return false;
                }
                if (!admission.counter_changed) {
                    if (admission.outcome != LiveControlAdmissionResult::stale) {
                        return false;
                    }
                    continue;
                }
                const auto owner = find_mailbox(*impl_, admission.mailbox_identity);
                const auto* producer = producer_for(admission.mailbox_identity,
                                                    admission.producer_identity);
                if (owner >= impl_->mailboxes.size() || !producer ||
                    producer->mailbox_index != owner) {
                    return false;
                }
                const auto& mailbox = *impl_->mailboxes[owner];
                if (admission.slot_index == kInvalidIndex) {
                    if (admission.outcome == LiveControlAdmissionResult::accepted ||
                        admission.outcome == LiveControlAdmissionResult::missed) {
                        return false;
                    }
                    continue;
                }
                if (admission.slot_index >= mailbox.registration.record_capacity ||
                    (admission.outcome != LiveControlAdmissionResult::accepted &&
                     admission.outcome != LiveControlAdmissionResult::missed &&
                     admission.outcome != LiveControlAdmissionResult::stopped)) {
                    return false;
                }
                const auto& record = admission.record;
                auto candidate = record;
                candidate.runtime_id = impl_->runtime_id;
                candidate.configuration_generation = impl_->configuration_generation;
                candidate.mailbox_sequence = 0;
                if (record.mailbox_identity != admission.mailbox_identity ||
                    record.producer_identity != admission.producer_identity ||
                    !structurally_valid(*impl_, mailbox, *producer, candidate,
                                        payload) ||
                    (admission.outcome != LiveControlAdmissionResult::stopped &&
                     !action_record_equal(action, record))) {
                    return false;
                }
                const auto producer_index =
                    static_cast<std::size_t>(producer - impl_->producers.get());
                const auto mailbox_next =
                    layout.mailbox_offset + owner * kCheckpointMailboxBytes + 8;
                const auto producer_next = layout.producer_offset +
                                           producer_index * kCheckpointProducerBytes +
                                           24;
                std::uint64_t next_mailbox = 0, next_producer = 0;
                (void)load_u64(scratch, mailbox_next, next_mailbox);
                (void)load_u64(scratch, producer_next, next_producer);
                if (next_mailbox == kInvalidSequence ||
                    next_producer == kInvalidSequence ||
                    record.mailbox_sequence != next_mailbox ||
                    record.producer_sequence != next_producer) {
                    return false;
                }
                const auto offset = slot_offset(owner, admission.slot_index);
                const auto previous = slot_state(offset);
                if ((previous != Impl::SlotState::free && !terminal_state(previous)) ||
                    (previous == Impl::SlotState::missed &&
                     scratch[offset + 137] != std::byte{0})) {
                    return false;
                }
                auto settled = Impl::SlotState::staged;
                if (admission.outcome == LiveControlAdmissionResult::missed) {
                    settled = Impl::SlotState::missed;
                } else if (admission.outcome == LiveControlAdmissionResult::stopped) {
                    settled = Impl::SlotState::stopped;
                } else if (!adjust_occupancy(owner, true)) {
                    return false;
                }
                std::memcpy(scratch.data() + offset, &record, sizeof(record));
                scratch[offset + 136] = static_cast<std::byte>(settled);
                scratch[offset + 137] =
                    settled == Impl::SlotState::staged ? std::byte{0} : std::byte{1};
                (void)store_u64(scratch, mailbox_next, next_mailbox + 1);
                (void)store_u64(scratch, producer_next, next_producer + 1);
            } else if (action.action == LiveControlActionId::provisional_publication) {
                std::size_t matched = 0;
                for (std::size_t owner = 0; owner < impl_->mailboxes.size(); ++owner) {
                    for (std::size_t slot = 0;
                         slot < impl_->mailboxes[owner]->registration.record_capacity;
                         ++slot) {
                        const auto offset = slot_offset(owner, slot);
                        LiveControlUpdateRecord record;
                        std::memcpy(&record, scratch.data() + offset, sizeof(record));
                        if (slot_state(offset) == Impl::SlotState::staged &&
                            targets_equal(target_from_record(record), action.target)) {
                            scratch[offset + 136] =
                                static_cast<std::byte>(Impl::SlotState::boundary_owned);
                            ++matched;
                        }
                    }
                }
                if (matched != static_cast<std::size_t>(action.survivor_count) +
                                   action.replaced_count) {
                    return false;
                }
            } else if (action.action == LiveControlActionId::committed ||
                       action.action == LiveControlActionId::replaced ||
                       action.action == LiveControlActionId::rolled_back ||
                       action.action == LiveControlActionId::missed ||
                       action.action == LiveControlActionId::stopped) {
                if (!action_topology_valid(action)) {
                    return false;
                }
                const auto owner = find_mailbox(*impl_, action.mailbox_identity);
                bool found = false;
                const auto desired = action.action == LiveControlActionId::committed
                                         ? Impl::SlotState::committed
                                     : action.action == LiveControlActionId::replaced
                                         ? Impl::SlotState::replaced
                                     : action.action == LiveControlActionId::rolled_back
                                         ? Impl::SlotState::rolled_back
                                     : action.action == LiveControlActionId::missed
                                         ? Impl::SlotState::missed
                                         : Impl::SlotState::stopped;
                for (std::size_t slot = 0;
                     slot < impl_->mailboxes[owner]->registration.record_capacity;
                     ++slot) {
                    const auto offset = slot_offset(owner, slot);
                    LiveControlUpdateRecord record;
                    std::memcpy(&record, scratch.data() + offset, sizeof(record));
                    if (!action_record_equal(action, record)) {
                        continue;
                    }
                    const auto previous = slot_state(offset);
                    if (previous == desired && scratch[offset + 137] == std::byte{1} &&
                        (desired == Impl::SlotState::missed ||
                         desired == Impl::SlotState::stopped)) {
                        scratch[offset + 137] = std::byte{0};
                    } else {
                        const auto required =
                            desired == Impl::SlotState::missed ||
                                    desired == Impl::SlotState::stopped
                                ? Impl::SlotState::staged
                                : Impl::SlotState::boundary_owned;
                        if (previous != required || !adjust_occupancy(owner, false)) {
                            return false;
                        }
                        scratch[offset + 136] = static_cast<std::byte>(desired);
                    }
                    found = true;
                    break;
                }
                if (!found) {
                    return false;
                }
            }
        }
        for (std::size_t owner = 0; owner < impl_->mailboxes.size(); ++owner) {
            for (std::size_t slot = 0;
                 slot < impl_->mailboxes[owner]->registration.record_capacity; ++slot) {
                const auto offset = slot_offset(owner, slot);
                if (slot_state(offset) == Impl::SlotState::boundary_owned ||
                    (slot_state(offset) == Impl::SlotState::missed &&
                     scratch[offset + 137] != std::byte{0})) {
                    return false;
                }
            }
        }
        return next_admission == view.admission_count;
    }

    // V1 never retained rejection ownership or non-survivor payloads. Reject
    // incomplete histories before any restore instead of inventing bytes.
    for (std::size_t index = 0; index < view.metadata.action_record_count; ++index) {
        LiveControlActionRecord action;
        if (!live_control_replay_action_at(view, index, action)) {
            return false;
        }
        if (action.action == LiveControlActionId::admission &&
            action.admission_result != LiveControlAdmissionResult::accepted) {
            return false;
        }
        if (action.action != LiveControlActionId::replaced &&
            action.action != LiveControlActionId::rolled_back) {
            continue;
        }
        bool retained = checkpoint_has_staged(action);
        for (std::size_t generation = 0;
             !retained && generation < view.metadata.retained_generation_count;
             ++generation) {
            LiveControlRetainedGenerationView descriptor;
            if (!live_control_replay_generation_at(view, generation, descriptor)) {
                return false;
            }
            for (std::size_t record_index = 0; record_index < descriptor.record_count;
                 ++record_index) {
                LiveControlUpdateRecord record;
                std::span<const std::byte> payload;
                if (!live_control_replay_record_at(view, generation, record_index,
                                                   record, payload)) {
                    return false;
                }
                retained = retained || action_record_equal(action, record);
            }
        }
        if (!retained) {
            return false;
        }
    }

    for (std::size_t mailbox_index = 0;
         mailbox_index < impl_->mailboxes.size();
         ++mailbox_index) {
        const auto& mailbox = *impl_->mailboxes[mailbox_index];
        std::uint32_t occupancy = 0;
        std::uint64_t next_mailbox_sequence = 0;
        if (!load_u32(
                state,
                layout.mailbox_offset +
                    mailbox_index * kCheckpointMailboxBytes + 80,
                occupancy) ||
            !load_u64(
                state,
                layout.mailbox_offset +
                    mailbox_index * kCheckpointMailboxBytes + 8,
                next_mailbox_sequence)) {
            return false;
        }
        for (std::size_t action_index = 0;
             action_index < view.metadata.action_record_count;
             ++action_index) {
            LiveControlActionRecord action;
            if (!live_control_replay_action_at(view, action_index, action)) {
                return false;
            }
            const bool has_record = action.action ==
                    LiveControlActionId::admission
                ? action.admission_result ==
                      LiveControlAdmissionResult::accepted ||
                      action.admission_result ==
                          LiveControlAdmissionResult::missed
                : action.action == LiveControlActionId::committed ||
                      action.action == LiveControlActionId::replaced ||
                      action.action == LiveControlActionId::rolled_back ||
                      action.action == LiveControlActionId::missed ||
                      action.action == LiveControlActionId::stopped;
            if (has_record && !action_topology_valid(action)) {
                return false;
            }
            if (action.mailbox_identity !=
                    mailbox.registration.mailbox_identity) {
                continue;
            }
            if (action.action == LiveControlActionId::admission &&
                (action.admission_result ==
                     LiveControlAdmissionResult::accepted ||
                 action.admission_result ==
                     LiveControlAdmissionResult::missed)) {
                if (next_mailbox_sequence == kInvalidSequence ||
                    action.mailbox_sequence != next_mailbox_sequence) {
                    return false;
                }
                ++next_mailbox_sequence;
            }
            if (action.action == LiveControlActionId::admission &&
                action.admission_result ==
                    LiveControlAdmissionResult::accepted) {
                if (occupancy == mailbox.registration.record_capacity) {
                    return false;
                }
                ++occupancy;
                continue;
            }
            const bool releases_slot =
                action.action == LiveControlActionId::committed ||
                action.action == LiveControlActionId::replaced ||
                action.action == LiveControlActionId::rolled_back ||
                action.action == LiveControlActionId::stopped;
            if (!releases_slot) {
                continue;
            }
            if ((!matching_accepted_before(action_index, action) &&
                 !checkpoint_has_staged(action)) || occupancy == 0) {
                return false;
            }
            --occupancy;
        }
    }
    for (std::size_t producer_index = 0;
         producer_index < impl_->producer_count;
         ++producer_index) {
        const auto& producer = impl_->producers[producer_index];
        std::uint64_t next_producer_sequence = 0;
        if (!load_u64(
                state,
                layout.producer_offset +
                    producer_index * kCheckpointProducerBytes + 24,
                next_producer_sequence)) {
            return false;
        }
        for (std::size_t action_index = 0;
             action_index < view.metadata.action_record_count;
             ++action_index) {
            LiveControlActionRecord action;
            if (!live_control_replay_action_at(view, action_index, action)) {
                return false;
            }
            if (action.action != LiveControlActionId::admission ||
                (action.admission_result !=
                     LiveControlAdmissionResult::accepted &&
                 action.admission_result !=
                     LiveControlAdmissionResult::missed) ||
                action.mailbox_identity != producer.mailbox_identity ||
                action.producer_identity != producer.producer_identity) {
                continue;
            }
            if (next_producer_sequence == kInvalidSequence ||
                action.producer_sequence != next_producer_sequence) {
                return false;
            }
            ++next_producer_sequence;
        }
    }
    return true;
}

bool LiveControlMailboxSet::begin_replay(
    const LiveControlReplayArtifactView& view) noexcept {
    if (!impl_ || !impl_->closure || impl_->closure->replay_view ||
        impl_->closure->transaction_active.load(std::memory_order_acquire) ||
        view.metadata.policy_identity != impl_->closure->policy.policy_identity ||
        view.metadata.retained_record_count >
            impl_->closure->policy.retained_record_capacity ||
        view.metadata.retained_payload_bytes >
            impl_->closure->policy.retained_payload_bytes) {
        return false;
    }
    impl_->closure->replay_active.store(true, std::memory_order_release);
    impl_->closure->replay_view = &view;
    impl_->closure->replay_generation_index = 0;
    impl_->closure->replay_action_index = 0;
    if (impl_->closure->lossless) {
        impl_->closure->lossless->replay_admission_index = 0;
    }
    impl_->closure->replay_history_applied = false;
    impl_->closure->replay_mismatch = Status::ok;
    impl_->closure->replay_mismatch_action_sequence = 0;
    impl_->closure->actions->restore_sequence(
        view.metadata.last_action_sequence + 1);
    return true;
}

void LiveControlMailboxSet::apply_replay_history() noexcept {
    if (!impl_ || !impl_->closure || !impl_->closure->replay_view ||
        impl_->closure->replay_history_applied) {
        return;
    }
    auto& closure = *impl_->closure;
    const auto& view = *closure.replay_view;
    if (view.metadata.schema_version == live_control_lossless_replay_schema_version) {
        consume_replay_history(*impl_);
        closure.replay_history_applied = true;
        return;
    }
    for (std::size_t action_index = 0;
         action_index < view.metadata.action_record_count;
         ++action_index) {
        LiveControlActionRecord action;
        if (!live_control_replay_action_at(view, action_index, action)) {
            closure.replay_mismatch = Status::invalid_artifact;
            return;
        }
        if (action.action != LiveControlActionId::admission ||
            (action.admission_result !=
                 LiveControlAdmissionResult::accepted &&
             action.admission_result !=
                 LiveControlAdmissionResult::missed)) {
            continue;
        }
        const auto mailbox_index = find_mailbox(
            *impl_, action.mailbox_identity);
        const auto producer = std::find_if(
            impl_->producers.get(),
            impl_->producers.get() + impl_->producer_count,
            [&](const Impl::Producer& candidate) {
                return candidate.mailbox_identity == action.mailbox_identity &&
                    candidate.producer_identity == action.producer_identity;
            });
        if (mailbox_index == impl_->mailboxes.size() ||
            producer == impl_->producers.get() + impl_->producer_count) {
            closure.replay_mismatch = Status::incompatible_artifact;
            return;
        }
        auto& mailbox = *impl_->mailboxes[mailbox_index];
        mailbox.next_sequence.store(
            action.mailbox_sequence + 1, std::memory_order_relaxed);
        producer->next_sequence.store(
            action.producer_sequence + 1, std::memory_order_relaxed);
        if (action.admission_result ==
                LiveControlAdmissionResult::accepted) {
            increment(mailbox.accepted);
        } else {
            increment(mailbox.missed);
            increment(impl_->missed);
        }
    }
    closure.replay_history_applied = true;
}

void LiveControlMailboxSet::end_replay() noexcept {
    if (impl_ && impl_->closure) {
        if (impl_->closure->replay_view) {
            consume_replay_history(*impl_);
            if (impl_->closure->replay_action_index !=
                    impl_->closure->replay_view->metadata
                        .action_record_count &&
                impl_->closure->replay_mismatch == Status::ok) {
                LiveControlActionRecord expected;
                if (live_control_replay_action_at(
                        *impl_->closure->replay_view,
                        impl_->closure->replay_action_index,
                        expected)) {
                    impl_->closure->replay_mismatch_action_sequence =
                        expected.sequence;
                }
                impl_->closure->replay_mismatch = Status::invalid_artifact;
            }
        }
        if (impl_->closure->replay_view &&
            impl_->closure->replay_generation_index !=
                impl_->closure->replay_view->metadata
                    .retained_generation_count &&
            impl_->closure->replay_mismatch == Status::ok) {
            impl_->closure->replay_mismatch = Status::invalid_artifact;
        }
        if (impl_->closure->replay_view) {
            impl_->closure->actions->restore_sequence(
                impl_->closure->replay_view->metadata
                    .last_action_sequence + 1);
        }
        impl_->closure->replay_view = nullptr;
        impl_->closure->replay_history_applied = false;
        impl_->closure->replay_active.store(false, std::memory_order_release);
    }
}

std::size_t LiveControlMailboxSet::replay_generations_compared() const noexcept {
    return impl_ && impl_->closure
        ? impl_->closure->replay_generation_index
        : 0;
}

Status LiveControlMailboxSet::replay_mismatch_status() const noexcept {
    return impl_ && impl_->closure
        ? impl_->closure->replay_mismatch
        : Status::invalid_state;
}

std::uint64_t
LiveControlMailboxSet::replay_mismatch_action_sequence() const noexcept {
    return impl_ && impl_->closure
        ? impl_->closure->replay_mismatch_action_sequence
        : 0;
}

bool LiveControlMailboxSet::claim_all() noexcept {
    if (!impl_ || !impl_->closure) {
        return false;
    }
    bool expected = false;
    if (!impl_->closure->host_claim.compare_exchange_strong(
            expected, true, std::memory_order_seq_cst, std::memory_order_seq_cst) ||
        (impl_->closure->lossless && impl_->closure->lossless->active_admissions.load(
                                         std::memory_order_seq_cst) != 0) ||
        impl_->closure->transaction_active.load(std::memory_order_acquire)) {
        if (!expected) {
            impl_->closure->host_claim.store(false, std::memory_order_seq_cst);
        }
        return false;
    }
    std::size_t claimed = 0;
    for (; claimed < impl_->mailboxes.size(); ++claimed) {
        if (impl_->mailboxes[claimed]->reservation.test_and_set(
                std::memory_order_acquire)) {
            break;
        }
    }
    if (claimed == impl_->mailboxes.size()) {
        return true;
    }
    while (claimed != 0) {
        impl_->mailboxes[--claimed]->reservation.clear(
            std::memory_order_release);
    }
    impl_->closure->host_claim.store(false, std::memory_order_seq_cst);
    return false;
}

void LiveControlMailboxSet::release_all() noexcept {
    if (!impl_) {
        return;
    }
    for (auto& mailbox : impl_->mailboxes) {
        mailbox->reservation.clear(std::memory_order_release);
    }
    if (impl_->closure) {
        impl_->closure->host_claim.store(false, std::memory_order_seq_cst);
    }
}

bool LiveControlMailboxSet::host_claimed() const noexcept {
    return impl_ && impl_->closure &&
        impl_->closure->host_claim.load(std::memory_order_acquire);
}

void LiveControlMailboxSet::close_admission() noexcept {
    if (impl_) {
        if (impl_->closure && impl_->closure->lossless) {
            auto expected = kInvalidSequence;
            (void)impl_->closure->lossless->admission_close_sequence
                .compare_exchange_strong(expected,
                                         impl_->closure->actions->next_sequence(),
                                         std::memory_order_acq_rel);
        }
        impl_->admission_open.store(false, std::memory_order_release);
    }
}

void LiveControlMailboxSet::terminalize_staged_on_stop() noexcept {
    if (!impl_) {
        return;
    }
    for (auto& mailbox : impl_->mailboxes) {
        for (std::size_t index = 0;
             index < mailbox->registration.record_capacity; ++index) {
            auto expected = Impl::SlotState::staged;
            if (mailbox->slots[index].state.compare_exchange_strong(
                    expected, Impl::SlotState::boundary_owned,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                mailbox->slots[index].terminal_generation.store(
                    0, std::memory_order_release);
                mailbox->occupancy.fetch_sub(1, std::memory_order_relaxed);
                increment(impl_->stopped);
                if (impl_->closure) {
                    const auto& record = mailbox->slots[index].record;
                    auto action = base_action(*impl_);
                    action.target = target_from_record(record);
                    action.mailbox_identity = record.mailbox_identity;
                    action.producer_identity = record.producer_identity;
                    action.mailbox_sequence = record.mailbox_sequence;
                    action.producer_sequence = record.producer_sequence;
                    action.payload_digest = record.payload_digest;
                    action.payload_bytes = record.payload_bytes;
                    action.update_kind = record.update_kind;
                    action.admission_result =
                        LiveControlAdmissionResult::stopped;
                    action.record_status = LiveControlRecordStatus::stopped;
                    action.action = LiveControlActionId::stopped;
                    action.stage = LiveControlActionStage::terminal;
                    action.reason = LiveControlActionReason::stopped;
                    action.result = LiveControlActionResult::settled;
                    action.replay_eligible =
                        impl_->closure->replay_eligible.load(
                            std::memory_order_acquire);
                    emit_action(*impl_, action);
                }
                mailbox->slots[index].state.store(Impl::SlotState::stopped,
                                                  std::memory_order_release);
            }
        }
    }
}

bool LiveControlMailboxSet::has_active_reservation() const noexcept {
    if (!impl_) {
        return false;
    }
    return std::any_of(
        impl_->mailboxes.begin(),
        impl_->mailboxes.end(),
        [](const std::unique_ptr<Impl::Mailbox>& mailbox) {
            return mailbox->reservation.test(std::memory_order_acquire);
        });
}

std::size_t LiveControlMailboxSet::mailbox_count() const noexcept {
    return impl_ ? impl_->mailboxes.size() : 0;
}

std::size_t LiveControlMailboxSet::producer_count() const noexcept {
    return impl_ ? impl_->producer_count : 0;
}

std::size_t LiveControlMailboxSet::record_capacity() const noexcept {
    return impl_ ? total_record_capacity(*impl_) : 0;
}

std::size_t LiveControlMailboxSet::payload_storage_bytes() const noexcept {
    return impl_ ? impl_->total_payload_storage_bytes : 0;
}

std::size_t LiveControlMailboxSet::control_bytes() const noexcept {
    if (!impl_) {
        return 0;
    }
    std::size_t total = sizeof(LiveControlMailboxSet) + sizeof(Impl);
    (void)checked_add(
        total,
        impl_->mailboxes.capacity() * sizeof(std::unique_ptr<Impl::Mailbox>),
        total);
    for (const auto& mailbox : impl_->mailboxes) {
        (void)checked_add(total, sizeof(Impl::Mailbox), total);
        (void)checked_add(
            total,
            static_cast<std::size_t>(mailbox->registration.record_capacity) *
                sizeof(Impl::Slot),
            total);
        (void)checked_add(
            total,
            static_cast<std::size_t>(mailbox->registration.record_capacity) *
                mailbox->registration.payload_bytes_per_record,
            total);
    }
    (void)checked_add(
        total, impl_->producer_count * sizeof(Impl::Producer), total);
    (void)checked_add(
        total, impl_->rate_target_count * sizeof(Impl::RateTarget), total);
    (void)checked_add(
        total, total_record_capacity(*impl_) * sizeof(Impl::Candidate), total);
    for (const auto& generation : impl_->generations) {
        (void)generation;
        (void)checked_add(
            total,
            total_record_capacity(*impl_) * sizeof(LiveControlRecordView),
            total);
        (void)checked_add(
            total, impl_->total_payload_storage_bytes, total);
    }
    return total;
}

std::size_t LiveControlMailboxSet::action_capacity() const noexcept {
    return impl_ && impl_->closure
        ? impl_->closure->actions->capacity()
        : 0;
}

std::size_t LiveControlMailboxSet::action_storage_bytes() const noexcept {
    return impl_ && impl_->closure
        ? impl_->closure->actions->slot_storage_bytes()
        : 0;
}

std::size_t LiveControlMailboxSet::retained_generation_capacity() const noexcept {
    return impl_ && impl_->closure
        ? impl_->closure->policy.retained_generation_capacity
        : 0;
}

std::size_t LiveControlMailboxSet::retained_record_capacity() const noexcept {
    return impl_ && impl_->closure
        ? impl_->closure->policy.retained_record_capacity
        : 0;
}

std::size_t LiveControlMailboxSet::retained_payload_storage_bytes() const noexcept {
    return impl_ && impl_->closure
        ? impl_->closure->policy.retained_payload_bytes
        : 0;
}

std::size_t LiveControlMailboxSet::closure_control_bytes() const noexcept {
    if (!impl_ || !impl_->closure) {
        return 0;
    }
    const auto& closure = *impl_->closure;
    const auto records = total_record_capacity(*impl_);
    std::size_t total = sizeof(Impl::Closure) + sizeof(LiveControlActionRing);
    (void)checked_add(total, closure.actions->slot_storage_bytes(), total);
    (void)checked_add(
        total, records * sizeof(LiveControlRecordView), total);
    (void)checked_add(total, impl_->total_payload_storage_bytes, total);
    (void)checked_add(
        total, records * sizeof(Impl::ProvisionalRecord), total);
    (void)checked_add(
        total,
        closure.policy.retained_generation_capacity *
            sizeof(Impl::RetainedGeneration),
        total);
    (void)checked_add(
        total,
        closure.policy.retained_record_capacity * sizeof(Impl::RetainedRecord),
        total);
    (void)checked_add(total, closure.policy.retained_payload_bytes, total);
    (void)checked_add(total, closure.checkpoint_state_bytes, total);
    if (closure.lossless) {
        total += sizeof(Impl::LosslessRetention) + closure.checkpoint_state_bytes +
                 closure.lossless->policy.admission_capacity *
                     (sizeof(LiveControlRetainedAdmission) + sizeof(std::size_t)) +
                 closure.lossless->policy.payload_capacity_bytes;
    }
    return total;
}

std::size_t LiveControlMailboxSet::extent_count() const noexcept {
    if (!impl_) {
        return 0;
    }
    std::size_t count = 2;
    count += impl_->mailboxes.capacity() != 0 ? 1u : 0u;
    count += impl_->mailboxes.size() * 3u;
    count += impl_->producer_count != 0 ? 1u : 0u;
    count += impl_->rate_target_count != 0 ? 1u : 0u;
    const auto records = total_record_capacity(*impl_);
    count += records != 0 ? 1u : 0u;
    count += records != 0 ? 2u : 0u;
    count += impl_->total_payload_storage_bytes != 0 ? 2u : 0u;
    if (impl_->closure) {
        count += 2u;
        count += impl_->closure->actions->slot_storage_bytes() != 0 ? 1u : 0u;
        count += records != 0 ? 2u : 0u;
        count += impl_->total_payload_storage_bytes != 0 ? 1u : 0u;
        count += impl_->closure->policy.retained_generation_capacity != 0
            ? 1u : 0u;
        count += impl_->closure->policy.retained_record_capacity != 0
            ? 1u : 0u;
        count += impl_->closure->policy.retained_payload_bytes != 0 ? 1u : 0u;
        count += impl_->closure->checkpoint_state_bytes != 0 ? 1u : 0u;
        count += impl_->closure->lossless ? 5u : 0u;
    }
    return count;
}

bool LiveControlMailboxSet::extent_at(
    std::size_t requested,
    LiveControlStorageExtent& extent) const noexcept {
    extent = {};
    if (!impl_) {
        return false;
    }
    std::size_t current = 0;
    const auto emit = [&](const void* data, std::size_t bytes) {
        if (bytes == 0) {
            return false;
        }
        if (current++ == requested) {
            extent = {data, bytes};
            return true;
        }
        return false;
    };
    if (emit(this, sizeof(LiveControlMailboxSet)) ||
        emit(impl_.get(), sizeof(Impl)) ||
        emit(impl_->mailboxes.data(),
             impl_->mailboxes.capacity() *
                 sizeof(std::unique_ptr<Impl::Mailbox>))) {
        return true;
    }
    for (const auto& mailbox : impl_->mailboxes) {
        if (emit(mailbox.get(), sizeof(Impl::Mailbox)) ||
            emit(mailbox->slots.get(),
                 static_cast<std::size_t>(mailbox->registration.record_capacity) *
                     sizeof(Impl::Slot)) ||
            emit(mailbox->payload_storage.get(),
                 static_cast<std::size_t>(mailbox->registration.record_capacity) *
                     mailbox->registration.payload_bytes_per_record)) {
            return true;
        }
    }
    if (emit(impl_->producers.get(),
             impl_->producer_count * sizeof(Impl::Producer)) ||
        emit(impl_->rate_targets.get(),
             impl_->rate_target_count * sizeof(Impl::RateTarget)) ||
        emit(impl_->candidates.get(),
             total_record_capacity(*impl_) * sizeof(Impl::Candidate))) {
        return true;
    }
    for (const auto& generation : impl_->generations) {
        if (emit(generation.records.get(),
                 total_record_capacity(*impl_) *
                     sizeof(LiveControlRecordView)) ||
            emit(generation.payloads.get(),
                 impl_->total_payload_storage_bytes)) {
            return true;
        }
    }
    if (impl_->closure) {
        const auto records = total_record_capacity(*impl_);
        auto& closure = *impl_->closure;
        if (emit(&closure, sizeof(Impl::Closure)) ||
            emit(closure.actions.get(), sizeof(LiveControlActionRing)) ||
            emit(closure.actions->slot_data(),
                 closure.actions->slot_storage_bytes()) ||
            emit(closure.rollback_generation.records.get(),
                 records * sizeof(LiveControlRecordView)) ||
            emit(closure.rollback_generation.payloads.get(),
                 impl_->total_payload_storage_bytes) ||
            emit(closure.provisional_records.get(),
                 records * sizeof(Impl::ProvisionalRecord)) ||
            emit(closure.retained_generations.get(),
                 closure.policy.retained_generation_capacity *
                     sizeof(Impl::RetainedGeneration)) ||
            emit(closure.retained_records.get(),
                 closure.policy.retained_record_capacity *
                     sizeof(Impl::RetainedRecord)) ||
            emit(closure.retained_payloads.get(),
                 closure.policy.retained_payload_bytes) ||
            emit(closure.checkpoint_state.get(),
                 closure.checkpoint_state_bytes)) {
            return true;
        }
        if (closure.lossless) {
            const auto& journal = *closure.lossless;
            if (emit(&journal, sizeof(Impl::LosslessRetention)) ||
                emit(journal.admissions.get(),
                     journal.policy.admission_capacity *
                         sizeof(LiveControlRetainedAdmission)) ||
                emit(journal.payloads.get(), journal.policy.payload_capacity_bytes) ||
                emit(journal.export_order.get(),
                     journal.policy.admission_capacity * sizeof(std::size_t)) ||
                emit(journal.validation_state.get(), closure.checkpoint_state_bytes)) {
                return true;
            }
        }
    }
    return false;
}

std::uint64_t LiveControlMailboxSet::runtime_id() const noexcept {
    return impl_ ? impl_->runtime_id : 0;
}

std::uint64_t LiveControlMailboxSet::configuration_generation() const noexcept {
    return impl_ ? impl_->configuration_generation : 0;
}

bool LiveControlMailboxSet::claim_for_test(
    std::uint64_t mailbox_identity) noexcept {
    if (!impl_) {
        return false;
    }
    const auto index = find_mailbox(*impl_, mailbox_identity);
    return index != impl_->mailboxes.size() &&
        !impl_->mailboxes[index]->reservation.test_and_set(
            std::memory_order_acquire);
}

void LiveControlMailboxSet::release_for_test(
    std::uint64_t mailbox_identity) noexcept {
    if (!impl_) {
        return;
    }
    const auto index = find_mailbox(*impl_, mailbox_identity);
    if (index != impl_->mailboxes.size()) {
        impl_->mailboxes[index]->reservation.clear(std::memory_order_release);
    }
}

} // namespace rt::detail
