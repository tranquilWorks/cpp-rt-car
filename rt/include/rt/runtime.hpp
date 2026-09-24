#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "core/units.hpp"
#include "rt/config.hpp"
#include "rt/device.hpp"
#include "rt/extension_abi.h"
#include "rt/graph.hpp"
#include "rt/status.hpp"
#include "rt/version.hpp"

class SimCore;

namespace rt {

// Capabilities report only completed target-path guarantees. A false value
// identifies a later roadmap milestone rather than a disabled build option.
struct Capabilities {
    bool compiled_graph;
    bool host_driven_time;
    bool unified_cpu_executor;
    bool host_executor_adapter;
    bool bounded_memory_plan;
    bool self_paced_time;
    bool frame_watchdog;
    bool strict_platform_preflight;
    bool versioned_observability;
    bool deterministic_replay;
    bool bounded_device_backend;
};

// Query runtime capabilities
Capabilities query_capabilities() noexcept;

// Deprecated 1.x compatibility shim. New code should use the typed duration
// fields on HostFrameContext and PeriodicRunConfig directly.
[[deprecated("use the runtime frame/period duration fields directly")]]
core::seconds tick_duration(core::seconds dt) noexcept;

inline constexpr std::uint32_t observability_schema_version = 2;
inline constexpr std::uint32_t observability_metadata_size = 184;
inline constexpr std::uint32_t rate_action_schema_version = 1;
inline constexpr std::size_t rate_action_counter_count = 20;
inline constexpr std::uint32_t mixed_rate_action_schema_version = 1;
inline constexpr std::uint32_t active_replay_schema_version = 1;
inline constexpr std::size_t mixed_rate_action_counter_count = 3;
inline constexpr std::size_t mixed_rate_action_capacity_limit =
    reference_release_capacity;
inline constexpr std::size_t active_replay_record_capacity_limit =
    reference_release_capacity;
inline constexpr std::size_t active_replay_absolute_max_bytes =
    std::size_t{1} << 30u;
inline constexpr std::size_t rate_telemetry_capacity_limit =
    reference_release_capacity;
inline constexpr std::uint32_t rate_policy_threshold_limit =
    static_cast<std::uint32_t>(reference_release_capacity);
inline constexpr std::uint32_t checkpoint_schema_version = 1;
inline constexpr std::uint32_t input_log_schema_version = 1;
inline constexpr std::size_t replay_identifier_capacity =
    observability_identifier_capacity;
inline constexpr std::uint32_t live_control_schema_version = 1;
inline constexpr std::uint32_t live_control_payload_canonical_little_endian = 1;
inline constexpr std::uint32_t live_control_mailbox_capacity_limit = 64;
inline constexpr std::uint32_t live_control_producer_capacity_limit = 256;
inline constexpr std::uint32_t live_control_record_capacity_limit = 65'536;
inline constexpr std::uint32_t live_control_payload_bytes_limit = 65'536;
inline constexpr std::uint32_t live_control_payload_alignment_limit = 4'096;
inline constexpr std::uint64_t live_control_total_storage_limit =
    std::uint64_t{1} << 30u;
inline constexpr std::uint32_t live_control_action_schema_version = 1;
inline constexpr std::uint32_t live_control_replay_schema_version = 1;
inline constexpr std::uint32_t live_control_lossless_replay_schema_version = 2;
inline constexpr std::size_t live_control_action_capacity_limit =
    live_control_record_capacity_limit * 4u;
inline constexpr std::size_t live_control_retained_generation_capacity_limit =
    reference_release_capacity;
inline constexpr std::size_t live_control_replay_absolute_max_bytes =
    std::size_t{1} << 30u;

enum class RuntimeState : std::uint8_t {
    configuring,
    finalized,
    running,
    stopped,
};

enum class ExtensionLifecycleState : std::uint32_t {
    configuring = 0,
    registered = 1,
    running = 2,
    stop_requested = 3,
    quiescent = 4,
    cleanup_pending = 5,
    detached = 6,
    failed = 7,
};

struct ExtensionHandle {
    std::uint32_t owner = 0;
    std::uint32_t kind = 0;
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return owner != 0 && kind == RTFW_EXTENSION_HANDLE_EXTENSION &&
               generation != 0;
    }

    friend constexpr bool operator==(
        ExtensionHandle,
        ExtensionHandle) noexcept = default;
};

struct ExtensionInfo {
    std::array<char, RTFW_EXTENSION_IDENTIFIER_CAPACITY> name{};
    std::array<char, RTFW_EXTENSION_IDENTIFIER_CAPACITY> version{};
    std::uint32_t negotiated_abi_version = 0;
    ExtensionLifecycleState state = ExtensionLifecycleState::configuring;
    std::uint32_t generation = 0;
    std::uint32_t phase_count = 0;
    std::uint32_t backend_count = 0;
    std::uint32_t service_count = 0;
    std::uint32_t resource_count = 0;
    std::uint32_t relationship_count = 0;
    bool unload_ready = false;
};

/*
 * One immutable job record submitted to a borrowed engine job system. The
 * adapter copies this record before submit() returns and invokes execute
 * exactly once for every accepted job. Scratch is runtime-owned, exclusive to
 * the invocation, and valid until execute returns. completion_context is an
 * opaque runtime token and must only be passed back to execute.
 */
using HostJobExecute = void (*)(
    void* execution_context,
    void* completion_context,
    std::uint64_t completion_token,
    std::uint32_t worker_index);

struct HostExecutorJob {
    HostJobExecute execute = nullptr;
    void* execution_context = nullptr;
    void* completion_context = nullptr;
    std::uint64_t completion_token = 0;
    std::byte* scratch = nullptr;
    std::size_t scratch_bytes = 0;
};

using HostExecutorSubmit = Status (*)(
    void* user_data,
    const HostExecutorJob& job) noexcept;
using HostExecutorTryExecuteOne = bool (*)(void* user_data) noexcept;

/*
 * A borrowed, already-running host job system. The declared worker/queue
 * capacities must exactly match RuntimeConfig when host_adapter is selected.
 * submit() is bounded and returns queue_full without accepting the job when
 * capacity is unavailable. try_execute_one() may execute at most one queued
 * job and is required so nested runtime work cannot deadlock a saturated host
 * team. The adapter and user_data outlive Runtime::stop().
 */
struct HostExecutorAdapter {
    void* user_data = nullptr;
    std::size_t worker_count = 0;
    std::size_t queue_capacity = 0;
    HostExecutorSubmit submit = nullptr;
    HostExecutorTryExecuteOne try_execute_one = nullptr;
};

enum class PlatformCheckId : std::uint8_t {
    absolute_monotonic_clock,
    realtime_kernel,
    memory_lock_limit,
    locked_memory,
    isolated_cpu_affinity,
    realtime_scheduler,
};

enum class PlatformCheckStatus : std::uint8_t {
    passed,
    failed,
    unsupported,
};

inline constexpr std::size_t platform_check_capacity = 6;
inline constexpr std::size_t platform_check_message_capacity = 96;

struct PlatformCheckResult {
    PlatformCheckId id = PlatformCheckId::absolute_monotonic_clock;
    PlatformCheckStatus status = PlatformCheckStatus::unsupported;
    std::int32_t system_error = 0;
    std::array<char, platform_check_message_capacity> message{};
};

struct PlatformPreflightReport {
    PlatformPreflightMode mode = PlatformPreflightMode::disabled;
    bool passed = false;
    std::size_t check_count = 0;
    std::array<PlatformCheckResult, platform_check_capacity> checks{};
};

enum class TaskResult : std::uint8_t {
    ok,
    error,
};

namespace detail {
class Executor;
struct RuntimeThreadPolicyTestAccess;
struct RuntimeLiveControlTestAccess;
}

class TaskContext;

struct TaskRange {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t task_index = 0;
};

using RangeTaskCallback = TaskResult (*)(
    void* user_data,
    const TaskContext& context,
    const TaskRange& range);

using ReductionTaskCallback = TaskResult (*)(
    void* user_data,
    const TaskContext& context,
    std::size_t left_task_index,
    std::size_t right_task_index);

// A TaskContext is valid only while its callback is running. Nested work is
// synchronous: these methods return only after every accepted child finishes.
class TaskContext {
public:
    [[nodiscard]] Status parallel_for(
        std::size_t item_count,
        std::size_t grain_size,
        RangeTaskCallback callback,
        void* user_data = nullptr) const noexcept;
    [[nodiscard]] Status parallel_reduce(
        std::size_t item_count,
        std::size_t grain_size,
        RangeTaskCallback range_callback,
        ReductionTaskCallback combine_callback,
        void* user_data = nullptr) const noexcept;

    [[nodiscard]] std::size_t worker_index() const noexcept {
        return worker_index_;
    }
    [[nodiscard]] std::size_t phase_index() const noexcept {
        return phase_index_;
    }
    [[nodiscard]] std::size_t task_index() const noexcept {
        return task_index_;
    }
    // This callback-local block is reserved before the task is accepted,
    // remains exclusively owned until the callback returns, and has
    // unspecified contents on entry.
    [[nodiscard]] std::span<std::byte> scratch() const noexcept {
        return scratch_;
    }

private:
    TaskContext(
        detail::Executor& executor,
        std::size_t worker_index,
        std::size_t phase_index,
        std::size_t task_index,
        std::span<std::byte> scratch) noexcept
        : executor_(&executor),
          worker_index_(worker_index),
          phase_index_(phase_index),
          task_index_(task_index),
          scratch_(scratch) {}

    detail::Executor* executor_ = nullptr;
    std::size_t worker_index_ = 0;
    std::size_t phase_index_ = 0;
    std::size_t task_index_ = 0;
    std::span<std::byte> scratch_{};

    friend class detail::Executor;
};

class RuntimeClock {
public:
    virtual ~RuntimeClock() = default;
    // Values must be monotonic nanoseconds in one clock domain. A Runtime
    // constructed with an injected clock borrows it for the Runtime lifetime.
    [[nodiscard]] virtual std::uint64_t now_ns() noexcept = 0;
    // Waits until an absolute timestamp in the same clock domain. Custom
    // clocks that do not implement absolute waiting keep host-driven stepping
    // but periodic execution returns clock_failure.
    [[nodiscard]] virtual Status sleep_until_ns(
        std::uint64_t absolute_ns) noexcept {
        (void)absolute_ns;
        return Status::clock_failure;
    }
    [[nodiscard]] virtual bool supports_absolute_sleep() const noexcept {
        return false;
    }
};

class PlatformPreflightProbe {
public:
    virtual ~PlatformPreflightProbe() = default;
    // Implementations must overwrite report without allocating or throwing.
    // A Runtime constructed with a probe borrows it for the Runtime lifetime.
    virtual void inspect(
        std::size_t planned_runtime_bytes,
        const RuntimeClock& clock,
        PlatformPreflightReport& report) noexcept = 0;
};

class NumericalPolicy {
public:
    explicit NumericalPolicy(
        NumericalMode mode = NumericalMode::precise) noexcept
        : mode_(mode) {}

    [[nodiscard]] NumericalMode mode() const noexcept { return mode_; }
    [[nodiscard]] double multiply_add(double a, double b, double c) const noexcept;

private:
    NumericalMode mode_;
};

struct HostFrameContext {
    std::uint64_t frame_index = 0;
    std::chrono::nanoseconds delta{0};
    std::optional<std::uint64_t> deadline_ns{};
    // Active rate execution maps logical epoch zero to this copied nominal
    // release. Reference-only execution ignores it. Appended for 1.x source
    // compatibility with pre-M16-03 aggregate initialization.
    std::optional<std::uint64_t> nominal_release_ns{};
};

enum class CallbackResult : std::uint8_t {
    ok,
    error,
};

struct StateRegistration {
    // State names are stable schema identifiers and use the same restricted
    // character set as workload_id. Storage is borrowed through Runtime
    // destruction. Its bytes must be an application-defined canonical
    // representation (for example, fixed-width little-endian fields), not a
    // compiler-native object layout with padding.
    std::string_view name;
    std::uint32_t schema_version = 1;
    std::span<std::byte> storage{};
};

struct ArtifactWriteResult {
    std::size_t required_bytes = 0;
    std::size_t bytes_written = 0;
    std::uint64_t checksum = 0;
};

struct CheckpointMetadata {
    std::uint32_t schema_version = checkpoint_schema_version;
    std::uint32_t runtime_version_major = version_major;
    std::uint32_t runtime_version_minor = version_minor;
    std::uint32_t runtime_version_patch = version_patch;
    DeterminismTier determinism_tier = DeterminismTier::unspecified;
    std::uint64_t config_id = 0;
    std::uint64_t replay_id = 0;
    std::uint64_t graph_id = 0;
    std::uint64_t state_schema_id = 0;
    std::uint64_t checkpoint_frame_index = 0;
    std::uint64_t state_payload_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t state_hash = 0;
    std::uint64_t artifact_checksum = 0;
    std::uint32_t state_count = 0;
    std::array<char, replay_identifier_capacity> build_id{};
    std::array<char, replay_identifier_capacity> workload_id{};
};

struct ReplayInputRecord {
    HostFrameContext frame{};
    std::uint32_t input_type = 0;
    std::span<const std::byte> payload{};
};

struct InputLogMetadata {
    std::uint32_t schema_version = input_log_schema_version;
    std::uint32_t runtime_version_major = version_major;
    std::uint32_t runtime_version_minor = version_minor;
    std::uint32_t runtime_version_patch = version_patch;
    DeterminismTier determinism_tier = DeterminismTier::unspecified;
    std::uint64_t replay_id = 0;
    std::uint64_t state_schema_id = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t artifact_checksum = 0;
    std::uint64_t first_frame_index = 0;
    std::uint64_t last_frame_index = 0;
    std::uint32_t record_count = 0;
    std::array<char, replay_identifier_capacity> workload_id{};
};

struct ReplayInputView {
    HostFrameContext frame{};
    std::uint32_t input_type = 0;
    std::span<const std::byte> payload{};
};

using ReplayInputCallback = CallbackResult (*)(
    void* user_data,
    const ReplayInputView& input);

struct ReplayResult {
    std::uint64_t checkpoint_frame_index = 0;
    std::uint64_t first_frame_index = 0;
    std::uint64_t last_frame_index = 0;
    std::size_t records_processed = 0;
    std::size_t frames_replayed = 0;
    std::uint64_t final_state_hash = 0;
};

// These inspectors are allocation-free and validate the complete artifact,
// including fixed-width little-endian headers, bounds, reserved fields, and
// per-record plus whole-artifact checksums. They do not apply runtime identity
// policy; Runtime::restore_checkpoint and Runtime::replay do.
[[nodiscard]] Status inspect_checkpoint_artifact(
    std::span<const std::byte> artifact,
    CheckpointMetadata& metadata) noexcept;
[[nodiscard]] Status inspect_input_log_artifact(
    std::span<const std::byte> artifact,
    InputLogMetadata& metadata) noexcept;

enum class RateLateAction : std::uint8_t {
    skip,
    bounded_catch_up,
    hold,
    degrade,
    fail,
};

enum class CrossRateSampleProvenance : std::uint8_t {
    initial_sample,
    produced,
};

enum class CrossRateFreshness : std::uint8_t {
    fresh,
    stale,
};

enum class CrossRateReadStatus : std::uint8_t {
    ok,
    invalid_channel,
    wrong_owner,
    size_mismatch,
    not_ready,
    stale_generation,
};

struct CrossRateReadResult {
    CrossRateReadStatus status = CrossRateReadStatus::not_ready;
    CrossRateSampleProvenance provenance =
        CrossRateSampleProvenance::initial_sample;
    CrossRateFreshness freshness = CrossRateFreshness::fresh;
    std::uint64_t generation = 0;
    std::uint64_t age_ns = 0;
    bool held = false;
    // M21-03 producer identity. Initial samples leave these fields zeroed.
    std::uint64_t producer_release_sequence = 0;
    std::uint32_t producer_substep_ordinal = 0;
    Status producer_completion_status = Status::ok;
    std::uint64_t producer_timestamp_domain_identity = 0;
    std::uint64_t producer_timestamp = 0;
    // M21-04 sampled-I/O metadata. Ordinary cross-rate channels leave these
    // fields zeroed. A substituted frame is copied from frozen Runtime-owned
    // initial/safe bytes, never inferred from zero-filled storage.
    std::uint64_t sampled_sequence = 0;
    std::uint64_t sampled_trigger_identity = 0;
    std::uint64_t sampled_trigger_sequence = 0;
    std::uint64_t sampled_calibration_identity = 0;
    bool sampled_substituted = false;
};

class RateReleaseView {
public:
    using PublishOperation = Status (*)(
        void*,
        CrossRateChannelHandle,
        std::span<const std::byte>) noexcept;
    using CopyOperation = CrossRateReadStatus (*)(
        void*,
        CrossRateChannelHandle,
        std::span<std::byte>,
        CrossRateReadResult&) noexcept;

    RateDomainHandle domain{};
    PhaseHandle phase{};
    std::uint64_t supercycle_cycle = 0;
    std::uint64_t domain_release_sequence = 0;
    std::uint32_t substep_ordinal = 0;
    std::uint64_t logical_release_ns = 0;
    std::uint64_t nominal_release_ns = 0;
    std::uint64_t absolute_deadline_ns = 0;
    std::uint64_t declared_budget_ns = 0;
    RateLateAction late_action = RateLateAction::fail;
    std::uint32_t degradation_level = 0;

    [[nodiscard]] Status publish(
        CrossRateChannelHandle channel,
        std::span<const std::byte> payload) const noexcept {
        return publish_operation_
            ? publish_operation_(operation_context_, channel, payload)
            : Status::invalid_state;
    }

    [[nodiscard]] CrossRateReadStatus copy(
        CrossRateChannelHandle channel,
        std::span<std::byte> output,
        CrossRateReadResult& result) const noexcept {
        result = {};
        if (!copy_operation_) {
            result.status = CrossRateReadStatus::not_ready;
            return result.status;
        }
        return copy_operation_(operation_context_, channel, output, result);
    }

    // Runtime-owned operation hooks. Hosts must treat these as opaque and
    // must not retain the view beyond its callback.
    void* operation_context_ = nullptr;
    PublishOperation publish_operation_ = nullptr;
    CopyOperation copy_operation_ = nullptr;
};

struct LiveControlGenerationView;

struct CallbackContext {
    const HostFrameContext& frame;
    // Valid only for this phase callback. Each phase owns a distinct block so
    // independent phases cannot race through runtime-provided scratch.
    std::span<std::byte> scratch;
    const NumericalPolicy& numerics;
    const TaskContext& tasks;
    // Runtime-owned degradation state. A watchdog event is applied only after
    // its frame returns, so callbacks observe the level committed by earlier
    // frames.
    std::uint32_t degradation_level = 0;
    // Non-null only for an active CPU rate callback. Appended for source
    // compatibility with the pre-M16-03 aggregate prefix.
    const RateReleaseView* rate_release = nullptr;
    // Runtime-owned immutable generation. Valid only for this callback.
    // Payload bytes remain host memory and are never transferred to a device
    // implicitly.
    const LiveControlGenerationView* live_control = nullptr;
};

using FrameCallback = CallbackResult (*)(void*, const CallbackContext&);

struct CallbackRegistration {
    // Runtime copies name. The host retains user_data ownership and must keep
    // it valid until no future step can invoke this callback.
    std::string_view name;
    FrameCallback callback = nullptr;
    void* user_data = nullptr;
};

struct DeviceCallbackContext {
    const HostFrameContext& frame;
    std::span<std::byte> scratch;
    const NumericalPolicy& numerics;
    const TaskContext& tasks;
    std::uint32_t degradation_level = 0;
    // Non-null only for an active admitted device-rate command provider.
    // Appended for source compatibility with the pre-M21-02 aggregate prefix.
    const RateReleaseView* rate_release = nullptr;
    const LiveControlGenerationView* live_control = nullptr;
};

using DeviceCommandCallback = CallbackResult (*)(
    void* user_data,
    const DeviceCallbackContext& context,
    DeviceSubmission& submission);

struct DevicePhaseRegistration {
    // Runtime copies name and borrows user_data until no future step can
    // invoke the command provider. The provider prepares one fixed-size
    // submission and must return without waiting for device completion.
    std::string_view name;
    DeviceBackendHandle backend{};
    DeviceCommandCallback callback = nullptr;
    void* user_data = nullptr;
};

struct DeviceTimelineRegistration {
    std::string_view name{};
    DeviceBackendHandle backend{};
    std::uint64_t initial_value = 0;
};

struct DeviceTimelineInfo {
    std::uint32_t struct_size = sizeof(DeviceTimelineInfo);
    std::uint32_t extension_version =
        hal_v2_command_timeline_extension_version;
    DeviceTimelineHandle timeline{};
    DeviceBackendHandle backend{};
    std::array<char, hal_v2_identifier_capacity> name{};
    std::uint64_t initial_value = 0;
    std::uint64_t last_accepted_value = 0;
    std::uint64_t completed_value = 0;
    std::array<std::uint64_t, 4> reserved{};
};

using DeviceBatchCommandCallback = CallbackResult (*)(
    void* user_data,
    const DeviceCallbackContext& context,
    DeviceCommandBatch& batch);

struct DeviceBatchPhaseRegistration {
    // The declaration is copied during configuration. Counts, ordered command
    // kinds/operations/references, and ordered wait/signal handles define the
    // compatibility skeleton. Runtime-generated IDs, frame/timeout values,
    // payload bytes, and timeline values remain invocation data.
    std::string_view name{};
    DeviceBackendHandle backend{};
    DeviceBatchCommandCallback callback = nullptr;
    void* user_data = nullptr;
    DeviceCommandBatch declaration{};
};

struct StepResult {
    std::size_t callbacks_executed = 0;
    std::uint64_t start_ns = 0;
    std::uint64_t finish_ns = 0;
    bool deadline_missed = false;
    bool watchdog_fired = false;
    std::uint32_t degradation_level = 0;
    struct RateSummary {
        std::uint64_t due_domain_releases = 0;
        std::uint64_t due_reference_records = 0;
        std::uint64_t executed_reference_records = 0;
        std::uint64_t on_time_domain_releases = 0;
        std::uint64_t late_domain_releases = 0;
        std::uint64_t caught_up_domain_releases = 0;
        std::uint64_t skipped_domain_releases = 0;
        std::uint64_t held_domain_releases = 0;
        std::uint64_t degraded_domain_releases = 0;
        std::uint64_t rejected_reference_records = 0;
        std::uint64_t stale_reads = 0;
        std::uint64_t failed_domain_releases = 0;
        bool has_first_failure = false;
        RateDomainHandle first_failing_domain{};
        std::uint64_t first_failing_sequence = 0;
        std::uint32_t first_failing_substep = 0;
        // M16-04 append-only optional-work and policy summary.
        std::uint64_t optional_due_domain_releases = 0;
        std::uint64_t optional_executed_domain_releases = 0;
        std::uint64_t shed_domain_releases = 0;
        std::uint64_t shed_transitions = 0;
        std::uint64_t recovery_transitions = 0;
        std::uint64_t currently_shed_domains = 0;
        std::uint64_t rate_policy_version = 0;
    } rate{};
};

struct PeriodicRunConfig {
    std::uint64_t first_frame_index = 0;
    std::size_t frame_count = 1;
    std::chrono::nanoseconds period{16'666'667};
    std::optional<std::uint64_t> first_release_ns{};
    std::chrono::nanoseconds relative_deadline{16'666'667};
};

struct PeriodicFrameResult {
    Status status = Status::ok;
    std::uint64_t frame_index = 0;
    std::uint64_t release_ns = 0;
    std::uint64_t wake_ns = 0;
    std::uint64_t start_ns = 0;
    std::uint64_t finish_ns = 0;
    std::int64_t slack_ns = 0;
    bool deadline_missed = false;
    bool watchdog_fired = false;
    std::uint32_t degradation_level = 0;
    StepResult::RateSummary rate{};
};

using PeriodicFrameObserver = CallbackResult (*)(
    void* user_data,
    const PeriodicFrameResult& frame);

struct PeriodicRunResult {
    std::size_t frames_executed = 0;
    std::size_t deadline_misses = 0;
    std::size_t watchdog_events = 0;
    std::uint32_t final_degradation_level = 0;
    std::uint64_t first_release_ns = 0;
    std::uint64_t next_release_ns = 0;
    PeriodicFrameResult last_frame{};
    StepResult::RateSummary rate{};
};

struct ExecutorStats {
    ExecutorPolicy policy = ExecutorPolicy::static_deterministic;
    std::size_t worker_count = 0;
    std::size_t queue_capacity = 0;
    std::uint64_t submitted_tasks = 0;
    std::uint64_t local_executions = 0;
    std::uint64_t steal_attempts = 0;
    std::uint64_t successful_steals = 0;
    std::uint64_t queue_full_rejections = 0;
    std::uint64_t scratch_exhaustions = 0;
    std::uint64_t worker_starts = 0;
};

struct StaticPhaseAssignment {
    PhaseHandle phase{};
    std::size_t worker_index = 0;
};

// M16-03 active execution remains an opt-in C++ source API and is not a
// RuntimeConfig/schema-7 or stable-C-ABI field. Presence of this copied policy
// enables dispatch; the maximum is a strict per-step callback-record bound.
struct RateExecutionPolicy {
    std::size_t maximum_dispatch_records_per_step = 0;
    // M16-04 append-only tail. These defaults retain valid M16-03 aggregate
    // construction while supplying a versioned bounded policy.
    std::uint64_t host_policy_version = 1;
    std::uint32_t consecutive_late_threshold = 1;
    std::uint32_t consecutive_on_time_threshold = 1;
    std::size_t rate_telemetry_capacity = 0;
};

enum class RateActionId : std::uint8_t {
    execute_on_time = 0,
    execute_catch_up = 1,
    skip = 2,
    hold = 3,
    execute_degraded = 4,
    fail = 5,
    optional_shed = 6,
};

enum class RateTransitionId : std::uint8_t {
    none = 0,
    shed = 1,
    recover = 2,
};

enum class RateActionReason : std::uint8_t {
    on_time = 0,
    deadline_late = 1,
    already_shed = 2,
    late_threshold = 3,
    on_time_threshold = 4,
    callback_failure = 5,
    dispatch_capacity = 6,
    arithmetic_failure = 7,
};

enum class RateCounterId : std::uint8_t {
    due_domain_releases = 0,
    executed_reference_records = 1,
    late_domain_releases = 2,
    caught_up_domain_releases = 3,
    skipped_domain_releases = 4,
    held_domain_releases = 5,
    degraded_domain_releases = 6,
    failed_domain_releases = 7,
    optional_due_domain_releases = 8,
    optional_executed_domain_releases = 9,
    shed_domain_releases = 10,
    shed_transitions = 11,
    recovery_transitions = 12,
    records_emitted = 13,
    records_overwritten = 14,
    records_dropped = 15,
    currently_shed_domains = 16,
    policy_version = 17,
    rejected_reference_records = 18,
    stale_reads = 19,
};

enum class RateCounterKind : std::uint8_t {
    counter,
    gauge,
};

struct RateCounterDefinition {
    RateCounterId id = RateCounterId::due_domain_releases;
    RateCounterKind kind = RateCounterKind::counter;
    std::string_view name{};
};

[[nodiscard]] bool rate_counter_definition(
    std::size_t schema_index,
    RateCounterDefinition& definition) noexcept;

// Fixed schema-1 action/range record. A range advances release sequence and
// logical/nominal release by release_period_ns for each covered release.
struct RateActionRecord {
    std::uint32_t schema_version = rate_action_schema_version;
    std::uint32_t record_size = sizeof(RateActionRecord);
    std::uint64_t sequence = 0;
    std::uint64_t host_policy_version = 0;
    std::uint64_t runtime_id = 0;
    std::uint64_t frame_index = 0;
    std::uint64_t first_domain_release_sequence = 0;
    std::uint64_t logical_release_ns = 0;
    std::uint64_t nominal_release_ns = 0;
    std::uint64_t release_period_ns = 0;
    std::uint64_t release_count = 0;
    std::uint64_t reference_record_count = 0;
    std::uint64_t shed_state_before = 0;
    std::uint64_t shed_state_after = 0;
    std::uint32_t domain_registration_index = 0;
    std::uint32_t transition_domain_registration_index =
        std::numeric_limits<std::uint32_t>::max();
    RateActionId action = RateActionId::execute_on_time;
    RateTransitionId transition = RateTransitionId::none;
    RateActionReason reason = RateActionReason::on_time;
    bool optional = false;
    bool late = false;
    std::array<std::uint8_t, 3> reserved0{};
    std::int32_t status = static_cast<std::int32_t>(Status::ok);
    std::uint32_t degradation_level = 0;
    std::array<std::byte, 32> reserved1{};
};

static_assert(sizeof(RateActionRecord) == 160);

struct RateTelemetryMetadata {
    std::uint32_t schema_version = rate_action_schema_version;
    std::uint32_t record_size = sizeof(RateActionRecord);
    std::uint32_t counter_count = rate_action_counter_count;
    std::uint32_t reserved0 = 0;
    std::uint64_t host_policy_version = 0;
    std::uint64_t runtime_id = 0;
    std::uint64_t capacity = 0;
    std::uint64_t next_sequence = 0;
    std::uint64_t records_emitted = 0;
    std::uint64_t records_overwritten = 0;
    std::uint64_t records_dropped = 0;
};

struct RateTelemetryCursor {
    std::uint32_t schema_version = rate_action_schema_version;
    std::uint32_t reserved0 = 0;
    std::uint64_t runtime_id = 0;
    std::uint64_t next_sequence = 0;
};

struct RateTelemetryReadResult {
    RateTelemetryMetadata metadata{};
    std::uint64_t first_sequence = 0;
    std::uint64_t next_sequence = 0;
    std::size_t records_read = 0;
    std::uint64_t lost_records = 0;
    std::uint64_t remaining_sequence_count = 0;
};

struct RateCounterSnapshot {
    RateTelemetryMetadata metadata{};
    std::array<std::uint64_t, rate_action_counter_count> values{};
};

// M21-05 is a distinct additive C++ closure. It does not extend checkpoint,
// input-log, rate-action, or observability schemas in place.
enum class MixedRateOverflowPolicy : std::uint8_t {
    overwrite_committed = 1,
};

struct MixedRateClosurePolicy {
    std::uint64_t host_policy_version = 1;
    std::size_t action_capacity = 0;
    std::size_t active_replay_record_capacity = 0;
    std::size_t active_replay_max_bytes = 0;
    std::size_t maximum_actions_per_step = 0;
    MixedRateOverflowPolicy overflow_policy =
        MixedRateOverflowPolicy::overwrite_committed;
    bool active_replay_enabled = false;
    bool require_deterministic_backends = true;
    std::array<std::byte, 5> reserved{};
};

enum class MixedRateActionId : std::uint8_t {
    rate_execute = 1,
    rate_skip = 2,
    rate_hold = 3,
    rate_shed = 4,
    rate_recover = 5,
    device_terminal = 6,
    sampled_publish = 7,
    sampled_select = 8,
    safe_transition = 9,
    watchdog_transition = 10,
    runtime_stop = 11,
};

enum class MixedRateActionReason : std::uint8_t {
    normal = 1,
    deadline_late = 2,
    already_shed = 3,
    callback_failure = 4,
    submission_rejected = 5,
    completion_error = 6,
    timeout = 7,
    canceled = 8,
    lost = 9,
    quarantined = 10,
    stale = 11,
    overrun = 12,
    underrun = 13,
    safe_acknowledged = 14,
    safe_failed = 15,
    watchdog = 16,
    late_threshold = 17,
    on_time_threshold = 18,
    cleanup_pending = 19,
};

enum class MixedRateActionStage : std::uint8_t {
    decision = 1,
    submitted = 2,
    terminal = 3,
    published = 4,
    selected = 5,
    acknowledged = 6,
    quarantined = 7,
};

enum class MixedRateSampleFreshness : std::uint8_t {
    not_applicable = 0,
    fresh = 1,
    held = 2,
    stale = 3,
    substituted = 4,
};

enum class MixedRateSafetyState : std::uint8_t {
    not_applicable = 0,
    unknown = 1,
    startup_acknowledged = 2,
    active = 3,
    failure_acknowledged = 4,
    shutdown_acknowledged = 5,
};

// Fixed schema-1 record. It carries identity and content digests only; no
// payload bytes, addresses, callbacks, vendor handles, or thread identities.
struct MixedRateActionRecord {
    std::uint32_t schema_version = mixed_rate_action_schema_version;
    std::uint32_t record_size = sizeof(MixedRateActionRecord);
    std::uint64_t sequence = 0;
    std::uint64_t runtime_id = 0;
    std::uint64_t host_policy_version = 0;
    std::uint64_t frame_index = 0;
    std::uint64_t logical_release_ns = 0;
    std::uint64_t nominal_release_ns = 0;
    std::uint64_t release_sequence = 0;
    std::uint64_t backend_identity = 0;
    std::uint64_t batch_identity = 0;
    std::uint64_t timeline_identity = 0;
    std::uint64_t completion_generation = 0;
    std::uint64_t timestamp_domain_identity = 0;
    std::uint64_t timestamp = 0;
    std::uint64_t payload_content_identity = 0;
    std::uint64_t sampled_content_identity = 0;
    std::uint64_t sampled_sequence = 0;
    std::uint64_t sampled_age_ns = 0;
    std::uint64_t shed_state_before = 0;
    std::uint64_t shed_state_after = 0;
    std::uint64_t watchdog_identity = 0;
    std::uint32_t rate_domain_registration_index = 0;
    std::uint32_t phase_index = 0;
    std::uint32_t substep_ordinal = 0;
    std::int32_t terminal_status = static_cast<std::int32_t>(Status::ok);
    std::uint32_t degradation_before = 0;
    std::uint32_t degradation_after = 0;
    MixedRateActionId action = MixedRateActionId::rate_execute;
    MixedRateActionReason reason = MixedRateActionReason::normal;
    MixedRateActionStage stage = MixedRateActionStage::decision;
    bool optional = false;
    bool late = false;
    bool shed = false;
    MixedRateSampleFreshness freshness =
        MixedRateSampleFreshness::not_applicable;
    bool held = false;
    bool substituted = false;
    MixedRateSafetyState safety_state =
        MixedRateSafetyState::not_applicable;
    std::array<std::byte, 54> reserved{};
};

static_assert(sizeof(MixedRateActionRecord) == 256);

struct MixedRateActionMetadata {
    std::uint32_t schema_version = mixed_rate_action_schema_version;
    std::uint32_t record_size = sizeof(MixedRateActionRecord);
    std::uint32_t counter_count = mixed_rate_action_counter_count;
    std::uint32_t reserved0 = 0;
    std::uint64_t host_policy_version = 0;
    std::uint64_t runtime_id = 0;
    std::uint64_t capacity = 0;
    std::uint64_t next_sequence = 0;
    std::uint64_t records_emitted = 0;
    std::uint64_t records_overwritten = 0;
    std::uint64_t records_dropped = 0;
    bool replay_eligible = false;
    std::array<std::byte, 7> reserved1{};
};

struct MixedRateActionCursor {
    std::uint32_t schema_version = mixed_rate_action_schema_version;
    std::uint32_t reserved0 = 0;
    std::uint64_t runtime_id = 0;
    std::uint64_t next_sequence = 0;
};

struct MixedRateActionReadResult {
    MixedRateActionMetadata metadata{};
    std::uint64_t first_sequence = 0;
    std::uint64_t next_sequence = 0;
    std::size_t records_read = 0;
    std::uint64_t lost_records = 0;
    std::uint64_t remaining_sequence_count = 0;
};

struct ActiveReplayMetadata {
    std::uint32_t schema_version = active_replay_schema_version;
    std::uint32_t header_size = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t artifact_checksum = 0;
    std::uint64_t runtime_id = 0;
    std::uint64_t config_id = 0;
    std::uint64_t replay_id = 0;
    std::uint64_t graph_id = 0;
    std::uint64_t state_schema_id = 0;
    std::uint64_t host_policy_version = 0;
    std::uint64_t checkpoint_frame_index = 0;
    std::uint64_t first_frame_index = 0;
    std::uint64_t last_frame_index = 0;
    std::uint64_t nominal_epoch_ns = 0;
    std::uint64_t final_state_hash = 0;
    std::uint64_t checkpoint_bytes = 0;
    std::uint64_t input_payload_bytes = 0;
    std::uint64_t first_action_sequence = 0;
    std::uint64_t last_action_sequence = 0;
    std::uint32_t input_record_count = 0;
    std::uint32_t action_record_count = 0;
    DeterminismTier determinism_tier = DeterminismTier::unspecified;
    std::array<std::byte, 7> reserved{};
    std::array<char, replay_identifier_capacity> build_id{};
    std::array<char, replay_identifier_capacity> workload_id{};
};

struct ActiveReplayResult {
    ReplayResult replay{};
    std::size_t actions_compared = 0;
    std::uint64_t mismatch_sequence = 0;
    Status mismatch_status = Status::ok;
};

[[nodiscard]] Status inspect_active_replay_artifact(
    std::span<const std::byte> artifact,
    ActiveReplayMetadata& metadata) noexcept;

// M22-01 is an additive, data-only staging surface. It does not apply,
// schedule, checkpoint, replay, roll back, or emit telemetry for an update.
enum class LiveControlAdmissionPolicy : std::uint8_t {
    reject_new = 1,
};

enum class LiveControlResetRule : std::uint8_t {
    discard_with_runtime = 1,
};

enum class LiveControlTargetKind : std::uint8_t {
    host_frame = 1,
    rate_release = 2,
};

enum class LiveControlUpdateKind : std::uint8_t {
    scenario_parameters = 1,
    controller_parameters = 2,
    sensor_calibration = 3,
    fault_configuration = 4,
    clear_fault = 5,
};

enum class LiveControlAdmissionResult : std::uint8_t {
    accepted = 1,
    invalid = 2,
    full = 3,
    busy = 4,
    stale = 5,
    stopped = 6,
    exhausted = 7,
    missed = 8,
};

enum class LiveControlRecordStatus : std::uint8_t {
    staged = 1,
    committed = 2,
    replaced = 3,
    missed = 4,
    stopped = 5,
    rolled_back = 6,
};

struct LiveControlBoundaryTarget {
    std::uint64_t frame_index = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t rate_release_sequence =
        std::numeric_limits<std::uint64_t>::max();
    std::uint32_t reference_release_index =
        std::numeric_limits<std::uint32_t>::max();
    std::uint32_t rate_domain_registration_index =
        std::numeric_limits<std::uint32_t>::max();
    std::uint32_t phase_index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t rate_substep_ordinal =
        std::numeric_limits<std::uint32_t>::max();
    LiveControlTargetKind kind = LiveControlTargetKind::host_frame;
    std::array<std::byte, 7> reserved{};
};

static_assert(sizeof(LiveControlBoundaryTarget) == 40);
static_assert(alignof(LiveControlBoundaryTarget) == 8);

struct LiveControlPolicy {
    std::uint32_t schema_version = live_control_schema_version;
    std::uint32_t struct_size = sizeof(LiveControlPolicy);
    std::uint64_t policy_identity = 0;
    std::uint32_t mailbox_capacity = 0;
    std::uint32_t producer_capacity = 0;
    std::uint32_t record_capacity = 0;
    std::uint32_t payload_bytes_per_record = 0;
    std::uint64_t total_payload_storage_bytes = 0;
    LiveControlAdmissionPolicy admission_policy =
        LiveControlAdmissionPolicy::reject_new;
    LiveControlResetRule reset_rule =
        LiveControlResetRule::discard_with_runtime;
    std::array<std::byte, 14> reserved{};
};

static_assert(sizeof(LiveControlPolicy) == 56);
static_assert(alignof(LiveControlPolicy) == 8);

struct LiveControlMailboxRegistration {
    std::uint32_t schema_version = live_control_schema_version;
    std::uint32_t struct_size = sizeof(LiveControlMailboxRegistration);
    std::uint64_t mailbox_identity = 0;
    std::uint32_t record_capacity = 0;
    std::uint32_t payload_bytes_per_record = 0;
    std::array<std::byte, 16> reserved{};
};

static_assert(sizeof(LiveControlMailboxRegistration) == 40);
static_assert(alignof(LiveControlMailboxRegistration) == 8);

struct LiveControlProducerRegistration {
    std::uint32_t schema_version = live_control_schema_version;
    std::uint32_t struct_size = sizeof(LiveControlProducerRegistration);
    std::uint64_t mailbox_identity = 0;
    std::uint64_t producer_identity = 0;
    std::uint64_t first_sequence = 1;
    std::uint64_t reserved = 0;
};

static_assert(sizeof(LiveControlProducerRegistration) == 40);
static_assert(alignof(LiveControlProducerRegistration) == 8);

struct LiveControlProducerHandle {
    std::uint64_t runtime_id = 0;
    std::uint64_t configuration_generation = 0;
    std::uint64_t mailbox_identity = 0;
    std::uint64_t producer_identity = 0;
    std::uint32_t producer_index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t reserved = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return runtime_id != 0 && configuration_generation != 0 &&
            mailbox_identity != 0 && producer_identity != 0 &&
            producer_index != std::numeric_limits<std::uint32_t>::max() &&
            reserved == 0;
    }

    friend constexpr bool operator==(
        LiveControlProducerHandle,
        LiveControlProducerHandle) noexcept = default;
};

static_assert(sizeof(LiveControlProducerHandle) == 40);
static_assert(alignof(LiveControlProducerHandle) == 8);

struct LiveControlUpdateRecord {
    std::uint32_t schema_version = live_control_schema_version;
    std::uint32_t record_size = sizeof(LiveControlUpdateRecord);
    // Zero on input. Runtime assigns a positive value only to the immutable
    // copied record after successful reservation and payload copy.
    std::uint64_t mailbox_sequence = 0;
    std::uint64_t runtime_id = 0;
    std::uint64_t configuration_generation = 0;
    std::uint64_t mailbox_identity = 0;
    std::uint64_t producer_identity = 0;
    std::uint64_t producer_sequence = 0;
    std::uint64_t target_frame_index = 0;
    std::uint64_t rate_release_sequence =
        std::numeric_limits<std::uint64_t>::max();
    std::uint64_t payload_digest = 0;
    std::uint32_t reference_release_index =
        std::numeric_limits<std::uint32_t>::max();
    std::uint32_t rate_domain_registration_index =
        std::numeric_limits<std::uint32_t>::max();
    std::uint32_t phase_index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t rate_substep_ordinal =
        std::numeric_limits<std::uint32_t>::max();
    std::uint32_t payload_bytes = 0;
    std::uint32_t payload_alignment = 1;
    std::uint32_t policy_flags =
        live_control_payload_canonical_little_endian;
    LiveControlTargetKind target_kind = LiveControlTargetKind::host_frame;
    LiveControlUpdateKind update_kind =
        LiveControlUpdateKind::scenario_parameters;
    std::array<std::byte, 18> reserved{};
};

static_assert(sizeof(LiveControlUpdateRecord) == 128);
static_assert(alignof(LiveControlUpdateRecord) == 8);

struct LiveControlRecordView {
    LiveControlUpdateRecord record{};
    std::span<const std::byte> payload{};
};

static_assert(sizeof(LiveControlRecordView) == 144);
static_assert(alignof(LiveControlRecordView) == 8);

struct LiveControlGenerationView {
    std::uint64_t runtime_id = 0;
    std::uint64_t configuration_generation = 0;
    std::uint64_t generation_identity = 0;
    LiveControlBoundaryTarget target{};
    std::span<const LiveControlRecordView> records{};
};

static_assert(sizeof(LiveControlGenerationView) == 80);
static_assert(alignof(LiveControlGenerationView) == 8);

struct LiveControlCommitInfo {
    std::uint32_t struct_size = sizeof(LiveControlCommitInfo);
    std::uint32_t survivor_count = 0;
    std::uint64_t runtime_id = 0;
    std::uint64_t configuration_generation = 0;
    std::uint64_t generation_identity = 0;
    LiveControlBoundaryTarget target{};
    std::uint64_t committed = 0;
    std::uint64_t replaced = 0;
    std::uint64_t missed = 0;
    std::uint64_t stopped = 0;
    std::uint32_t staged_occupancy = 0;
    std::uint32_t reserved32 = 0;
    std::array<std::uint64_t, 2> reserved{};
};

static_assert(sizeof(LiveControlCommitInfo) == 128);
static_assert(alignof(LiveControlCommitInfo) == 8);

struct LiveControlRecordStatusInfo {
    std::uint32_t struct_size = sizeof(LiveControlRecordStatusInfo);
    std::uint32_t reserved32 = 0;
    std::uint64_t runtime_id = 0;
    std::uint64_t configuration_generation = 0;
    std::uint64_t mailbox_identity = 0;
    std::uint64_t mailbox_sequence = 0;
    std::uint64_t producer_identity = 0;
    std::uint64_t producer_sequence = 0;
    std::uint64_t generation_identity = 0;
    LiveControlRecordStatus status = LiveControlRecordStatus::staged;
    LiveControlUpdateKind update_kind =
        LiveControlUpdateKind::scenario_parameters;
    LiveControlTargetKind target_kind = LiveControlTargetKind::host_frame;
    std::array<std::byte, 13> reserved{};
};

static_assert(sizeof(LiveControlRecordStatusInfo) == 80);
static_assert(alignof(LiveControlRecordStatusInfo) == 8);

struct LiveControlMailboxInfo {
    std::uint32_t schema_version = live_control_schema_version;
    std::uint32_t struct_size = sizeof(LiveControlMailboxInfo);
    std::uint64_t runtime_id = 0;
    std::uint64_t configuration_generation = 0;
    std::uint64_t policy_identity = 0;
    std::uint64_t mailbox_identity = 0;
    std::uint64_t next_mailbox_sequence = 0;
    std::uint64_t accepted = 0;
    std::uint64_t invalid = 0;
    std::uint64_t full = 0;
    std::uint64_t busy = 0;
    std::uint64_t stale = 0;
    std::uint64_t stopped = 0;
    std::uint64_t exhausted = 0;
    std::uint32_t record_capacity = 0;
    std::uint32_t payload_bytes_per_record = 0;
    std::uint32_t occupancy = 0;
    std::uint32_t producer_count = 0;
    std::uint8_t admission_open = 0;
    std::array<std::byte, 7> reserved{};
};

static_assert(sizeof(LiveControlMailboxInfo) == 128);
static_assert(alignof(LiveControlMailboxInfo) == 8);

// M22-03 is a distinct additive C++ closure. Existing checkpoint, input-log,
// rate-action, mixed-rate-action, active-replay, and observability schemas are
// not extended in place.
enum class LiveControlRollbackRule : std::uint8_t {
    restore_step_entry_generation = 1,
};

struct LiveControlClosurePolicy {
    std::uint32_t schema_version = live_control_action_schema_version;
    std::uint32_t struct_size = sizeof(LiveControlClosurePolicy);
    std::uint64_t policy_identity = 0;
    std::size_t action_capacity = 0;
    std::size_t retained_generation_capacity = 0;
    std::size_t retained_record_capacity = 0;
    std::size_t retained_payload_bytes = 0;
    std::size_t replay_record_capacity = 0;
    std::size_t replay_max_bytes = 0;
    LiveControlRollbackRule rollback_rule =
        LiveControlRollbackRule::restore_step_entry_generation;
    bool replay_enabled = false;
    std::array<std::byte, 6> reserved{};
};

static_assert(sizeof(LiveControlClosurePolicy) == 72);
static_assert(alignof(LiveControlClosurePolicy) == 8);

// Explicit trusted retention: includes original accepted payloads even when
// replaced, rolled back, missed or stopped. Default replay remains format v1.
struct LiveControlReplayRetentionPolicy {
    std::uint32_t schema_version = live_control_lossless_replay_schema_version;
    std::uint32_t struct_size = sizeof(LiveControlReplayRetentionPolicy);
    std::uint64_t policy_identity = 0;
    std::size_t admission_capacity = 0;
    std::size_t payload_capacity_bytes = 0;
    std::array<std::byte, 16> reserved{};
};

static_assert(sizeof(LiveControlReplayRetentionPolicy) == 48);
static_assert(alignof(LiveControlReplayRetentionPolicy) == 8);

enum class LiveControlActionId : std::uint8_t {
    admission = 1,
    boundary_empty = 2,
    provisional_publication = 3,
    committed = 4,
    replaced = 5,
    missed = 6,
    stopped = 7,
    rolled_back = 8,
    checkpointed = 9,
    replay_verified = 10,
};

enum class LiveControlActionStage : std::uint8_t {
    attempt = 1,
    provisional = 2,
    terminal = 3,
    checkpoint = 4,
    replay = 5,
};

enum class LiveControlActionReason : std::uint8_t {
    normal = 1,
    invalid = 2,
    full = 3,
    busy = 4,
    stale = 5,
    stopped = 6,
    exhausted = 7,
    missed = 8,
    replaced = 9,
    execution_failed = 10,
    checkpoint = 11,
    replay = 12,
};

enum class LiveControlActionResult : std::uint8_t {
    accepted = 1,
    rejected = 2,
    published = 3,
    settled = 4,
    rolled_back = 5,
    verified = 6,
};

// Fixed schema-1 action record. It carries only bounded identities and
// digests. Payload bytes, addresses, callbacks, vendor handles, wall-clock
// observations, and producer thread identities are deliberately absent.
struct LiveControlActionRecord {
    std::uint32_t schema_version = live_control_action_schema_version;
    std::uint32_t record_size = sizeof(LiveControlActionRecord);
    std::uint64_t sequence = 0;
    std::uint64_t runtime_id = 0;
    std::uint64_t configuration_generation = 0;
    std::uint64_t policy_identity = 0;
    LiveControlBoundaryTarget target{};
    std::uint64_t mailbox_identity = 0;
    std::uint64_t producer_identity = 0;
    std::uint64_t mailbox_sequence = 0;
    std::uint64_t producer_sequence = 0;
    std::uint64_t payload_digest = 0;
    std::uint64_t generation_identity = 0;
    std::uint64_t prior_generation_identity = 0;
    std::uint64_t checkpoint_correlation = 0;
    std::uint64_t replay_correlation = 0;
    std::uint32_t payload_bytes = 0;
    std::uint32_t survivor_count = 0;
    std::uint32_t replaced_count = 0;
    std::int32_t terminal_status = static_cast<std::int32_t>(Status::ok);
    LiveControlUpdateKind update_kind =
        LiveControlUpdateKind::scenario_parameters;
    LiveControlAdmissionResult admission_result =
        LiveControlAdmissionResult::accepted;
    LiveControlRecordStatus record_status = LiveControlRecordStatus::staged;
    LiveControlActionId action = LiveControlActionId::admission;
    LiveControlActionStage stage = LiveControlActionStage::attempt;
    LiveControlActionReason reason = LiveControlActionReason::normal;
    LiveControlActionResult result = LiveControlActionResult::accepted;
    bool replay_eligible = false;
    std::array<std::byte, 80> reserved{};
};

static_assert(sizeof(LiveControlActionRecord) == 256);
static_assert(alignof(LiveControlActionRecord) == 8);

struct LiveControlActionMetadata {
    std::uint32_t schema_version = live_control_action_schema_version;
    std::uint32_t record_size = sizeof(LiveControlActionRecord);
    std::uint64_t runtime_id = 0;
    std::uint64_t configuration_generation = 0;
    std::uint64_t policy_identity = 0;
    std::uint64_t capacity = 0;
    std::uint64_t next_sequence = 0;
    std::uint64_t records_emitted = 0;
    std::uint64_t records_overwritten = 0;
    std::uint64_t records_dropped = 0;
    std::uint64_t retained_generation_count = 0;
    bool replay_eligible = false;
    std::array<std::byte, 7> reserved{};
};

struct LiveControlActionCursor {
    std::uint32_t schema_version = live_control_action_schema_version;
    std::uint32_t reserved32 = 0;
    std::uint64_t runtime_id = 0;
    std::uint64_t configuration_generation = 0;
    std::uint64_t next_sequence = 0;
};

struct LiveControlActionReadResult {
    LiveControlActionMetadata metadata{};
    std::uint64_t first_sequence = 0;
    std::uint64_t next_sequence = 0;
    std::size_t records_read = 0;
    std::uint64_t lost_records = 0;
    std::uint64_t remaining_sequence_count = 0;
};

enum class LiveControlNestedArtifactKind : std::uint8_t {
    input_log = 1,
    active_replay = 2,
};

struct LiveControlReplayMetadata {
    std::uint32_t schema_version = live_control_replay_schema_version;
    std::uint32_t header_size = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t artifact_checksum = 0;
    std::uint64_t runtime_id = 0;
    std::uint64_t configuration_generation = 0;
    std::uint64_t config_id = 0;
    std::uint64_t replay_id = 0;
    std::uint64_t graph_id = 0;
    std::uint64_t state_schema_id = 0;
    std::uint64_t policy_identity = 0;
    std::uint64_t final_state_hash = 0;
    std::uint64_t checkpoint_bytes = 0;
    std::uint64_t nested_artifact_bytes = 0;
    std::uint64_t first_action_sequence = 0;
    std::uint64_t last_action_sequence = 0;
    std::uint32_t action_record_count = 0;
    std::uint32_t retained_generation_count = 0;
    std::uint32_t retained_record_count = 0;
    std::uint32_t retained_payload_bytes = 0;
    LiveControlNestedArtifactKind nested_kind =
        LiveControlNestedArtifactKind::input_log;
    DeterminismTier determinism_tier = DeterminismTier::unspecified;
    std::array<std::byte, 6> reserved{};
    std::array<char, replay_identifier_capacity> build_id{};
    std::array<char, replay_identifier_capacity> workload_id{};
};

struct LiveControlReplayResult {
    std::uint64_t checkpoint_frame_index = 0;
    std::uint64_t first_frame_index = 0;
    std::uint64_t last_frame_index = 0;
    std::uint64_t mismatch_frame_index = 0;
    LiveControlBoundaryTarget mismatch_target{};
    std::uint64_t mismatch_action_sequence = 0;
    std::uint64_t mismatch_generation_identity = 0;
    std::size_t frames_replayed = 0;
    std::size_t actions_compared = 0;
    std::size_t generations_compared = 0;
    std::uint64_t final_state_hash = 0;
    Status mismatch_status = Status::ok;
};

[[nodiscard]] Status inspect_live_control_replay_artifact(
    std::span<const std::byte> artifact,
    LiveControlReplayMetadata& metadata) noexcept;

[[nodiscard]] std::uint64_t live_control_payload_digest(
    std::span<const std::byte> payload) noexcept;

// M16-01 rate metadata is an additive C++ source API. Periods, deadlines, and
// budget/WCET estimates are integral nanoseconds and are not schema-7 fields.
struct RateDomainRegistration {
    std::string_view name{};
    std::uint64_t period_ns = 0;
    std::uint32_t substep_count = 1;
    std::uint64_t relative_deadline_ns = 0;
    std::uint64_t budget_wcet_ns = 0;
    RateCriticality criticality = RateCriticality::normal;
    bool optional = false;
    // Appended M16-03 fields preserve the M16-01 aggregate prefix. They are
    // ignored by reference-only plans and participate in identity only when
    // active execution is explicitly enabled.
    RateLateAction late_action = RateLateAction::fail;
    std::uint32_t bounded_catch_up_limit = 0;
};

enum class RatePhaseKind : std::uint8_t {
    cpu,
    device,
};

struct RatePhaseBinding {
    PhaseHandle phase{};
    RateDomainHandle domain{};
};

// M21-01 device-rate metadata names only roles for references that already
// exist in the copied M17 command-batch declaration. The role must agree with
// that reference's declared access; no command, buffer range, or address is
// declared a second time here.
enum class DeviceRatePayloadRole : std::uint8_t {
    input = 1,
    output = 2,
    input_output = 3,
};

struct DeviceRatePhaseBinding {
    PhaseHandle phase{};
    RateDomainHandle domain{};
    std::uint64_t completion_budget_ns = 0;
    std::uint32_t maximum_in_flight = 0;
    std::span<const DeviceRatePayloadRole> payload_roles{};
};

enum class DeviceRateReferenceKind : std::uint8_t {
    dispatch_buffer = 1,
    copy_source = 2,
    copy_destination = 3,
    synchronization_target = 4,
};

enum class DeviceRateTimelineRole : std::uint8_t {
    wait = 1,
    signal = 2,
};

struct CompiledDeviceRatePhase {
    PhaseHandle phase{};
    RateDomainHandle domain{};
    DeviceBackendHandle backend{};
    std::size_t compiled_phase_index = 0;
    std::uint64_t completion_budget_ns = 0;
    std::uint32_t maximum_in_flight = 0;
    std::uint32_t command_count = 0;
    std::uint32_t wait_count = 0;
    std::uint32_t signal_count = 0;
    std::size_t first_command_index = 0;
    std::size_t first_payload_reference_index = 0;
    std::size_t payload_reference_count = 0;
    std::size_t first_timeline_reference_index = 0;
    std::size_t timeline_reference_count = 0;
    std::uint64_t completion_timestamp_domain_identity = 0;
};

struct CompiledDeviceRateCommand {
    PhaseHandle phase{};
    std::uint32_t command_index = 0;
    HalV2CommandKind kind = HalV2CommandKind::invalid;
    HalV2MemoryOperation operation = HalV2MemoryOperation::invalid;
    std::uint32_t opcode = 0;
    std::uint32_t flags = 0;
    std::size_t first_payload_reference_index = 0;
    std::size_t payload_reference_count = 0;
};

struct CompiledDeviceRatePayloadReference {
    PhaseHandle phase{};
    DeviceBackendHandle backend{};
    DeviceBufferHandle buffer{};
    std::uint32_t command_index = 0;
    std::uint32_t command_reference_index = 0;
    DeviceRateReferenceKind kind =
        DeviceRateReferenceKind::dispatch_buffer;
    DeviceRatePayloadRole role = DeviceRatePayloadRole::input;
    std::uint32_t access = 0;
    std::uint64_t offset = 0;
    std::uint64_t bytes = 0;
};

struct CompiledDeviceRateTimelineReference {
    PhaseHandle phase{};
    DeviceBackendHandle backend{};
    DeviceTimelineHandle timeline{};
    DeviceRateTimelineRole role = DeviceRateTimelineRole::wait;
    std::uint32_t declaration_index = 0;
};

enum class DeviceRateAdmissionConstraint : std::uint8_t {
    phase_in_flight = 1,
    backend_in_flight = 2,
    runtime_outstanding = 3,
    completion_batch = 4,
};

struct DeviceRateAdmissionBackend {
    DeviceBackendHandle backend{};
    std::uint32_t maximum_in_flight = 0;
    std::uint32_t completion_batch_capacity = 0;
    std::uint32_t peak_in_flight = 0;
    std::uint32_t peak_completions = 0;
};

struct DeviceRateAdmissionPhase {
    PhaseHandle phase{};
    DeviceBackendHandle backend{};
    std::uint64_t completion_budget_ns = 0;
    std::uint32_t maximum_in_flight = 0;
    std::uint32_t peak_in_flight = 0;
};

struct DeviceRateAdmissionInterval {
    std::size_t reference_index =
        std::numeric_limits<std::size_t>::max();
    PhaseHandle phase{};
    RateDomainHandle domain{};
    DeviceBackendHandle backend{};
    std::uint64_t release_time_ns = 0;
    std::uint64_t completion_deadline_ns = 0;
    std::uint32_t substep_ordinal = 0;
    bool optional = false;
    bool carries_across_supercycle = false;
    DeviceRateAdmissionConstraint constraining_capacity =
        DeviceRateAdmissionConstraint::phase_in_flight;
    std::uint32_t constraining_limit = 0;
    std::uint32_t demand_at_release = 0;
};

struct DeviceRateAdmissionReport {
    std::uint64_t supercycle_ns = 0;
    std::uint32_t runtime_outstanding_capacity = 0;
    std::uint32_t runtime_completion_batch_capacity = 0;
    std::uint32_t peak_global_in_flight = 0;
    std::size_t backend_count = 0;
    std::size_t phase_count = 0;
    std::size_t interval_count = 0;
};

struct DeviceRateAdmissionDiagnostic {
    Status status = Status::ok;
    std::size_t reference_index =
        std::numeric_limits<std::size_t>::max();
    PhaseHandle phase{};
};

struct CompiledRateDomain {
    RateDomainHandle domain{};
    std::array<char, rate_domain_name_capacity> name{};
    std::size_t registration_index = 0;
    std::uint64_t period_ns = 0;
    std::uint32_t substep_count = 0;
    std::uint64_t relative_deadline_ns = 0;
    std::uint64_t budget_wcet_ns = 0;
    RateCriticality criticality = RateCriticality::normal;
    bool optional = false;
    std::uint64_t releases_per_supercycle = 0;
    // Exact reduced period ratio against registration-order domain zero.
    std::uint64_t period_ratio_numerator = 0;
    std::uint64_t period_ratio_denominator = 0;
    // Appended M16-03 tail preserves the M16-01 positional aggregate prefix.
    RateLateAction late_action = RateLateAction::fail;
    std::uint32_t bounded_catch_up_limit = 0;
};

struct CompiledRateBinding {
    PhaseHandle phase{};
    RateDomainHandle domain{};
    RatePhaseKind phase_kind = RatePhaseKind::cpu;
    std::size_t compiled_phase_index = 0;
};

struct ReferenceRelease {
    std::uint64_t release_time_ns = 0;
    RateDomainHandle domain{};
    std::size_t domain_registration_index = 0;
    std::uint64_t domain_release_sequence = 0;
    PhaseHandle phase{};
    RatePhaseKind phase_kind = RatePhaseKind::cpu;
    std::size_t compiled_phase_index = 0;
    std::uint32_t substep_ordinal = 0;
    std::uint32_t substep_count = 0;
    std::uint64_t relative_deadline_ns = 0;
    std::uint64_t deadline_time_ns = 0;
    std::uint64_t budget_wcet_ns = 0;
    RateCriticality criticality = RateCriticality::normal;
    bool optional = false;
    RateLateAction late_action = RateLateAction::fail;
    std::uint32_t bounded_catch_up_limit = 0;
};

inline constexpr std::size_t invalid_reference_release_index =
    std::numeric_limits<std::size_t>::max();
inline constexpr std::uint64_t invalid_cross_rate_sequence =
    std::numeric_limits<std::uint64_t>::max();
inline constexpr std::uint32_t invalid_cross_rate_substep =
    std::numeric_limits<std::uint32_t>::max();
inline constexpr std::size_t invalid_device_rate_payload_reference_ordinal =
    std::numeric_limits<std::size_t>::max();
inline constexpr std::uint64_t
    cross_rate_runtime_logical_timestamp_domain_identity = 1;

enum class CrossRateMode : std::uint8_t {
    sample_and_hold,
};

enum class CrossRateSelectionHorizon : std::uint8_t {
    first_supercycle,
    repeating_supercycle,
};

struct CrossRateDeviceEndpointSelector {
    // Phase-local ordinal in the ordered M21-01 payload-reference slice.
    std::size_t payload_reference_ordinal =
        invalid_device_rate_payload_reference_ordinal;
    // Positive only for a device endpoint. One exact payload-sized subrange
    // begins at envelope.offset + execution_slot * slot_stride_bytes.
    std::uint64_t slot_stride_bytes = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return payload_reference_ordinal !=
                   invalid_device_rate_payload_reference_ordinal &&
               slot_stride_bytes != 0;
    }
};

// Channels are copied while configuring. Initial bytes must exactly match
// payload_size. CPU endpoints retain invalid/default selectors; each device
// endpoint names one copied M21-01 payload-reference envelope explicitly.
struct CrossRateChannelRegistration {
    std::string_view name{};
    PhaseHandle producer{};
    PhaseHandle consumer{};
    std::size_t payload_size = 0;
    std::span<const std::byte> initial_sample{};
    CrossRateMode mode = CrossRateMode::sample_and_hold;
    // Zero accepts only an age-zero sample. UINT64_MAX is explicitly
    // unbounded; all other values are inclusive integral-nanosecond limits.
    std::uint64_t maximum_age_ns =
        std::numeric_limits<std::uint64_t>::max();
    CrossRateDeviceEndpointSelector producer_device{};
    CrossRateDeviceEndpointSelector consumer_device{};
};

inline constexpr std::uint32_t sampled_io_frame_version = 1;
inline constexpr std::size_t sampled_io_channel_capacity =
    cross_rate_channel_capacity;

enum class SampledIoDirection : std::uint8_t {
    input = 1,
    output = 2,
};

enum class SampledIoEncoding : std::uint8_t {
    signed_int16_le = 1,
    unsigned_int16_le = 2,
    signed_int32_le = 3,
    unsigned_int32_le = 4,
};

enum class SampledIoTriggerMode : std::uint8_t {
    periodic = 1,
    software = 2,
    external = 3,
};

enum class SampledIoStalePolicy : std::uint8_t {
    fail_release = 1,
    hold_last = 2,
    substitute_initial = 3,
};

enum class SampledIoOverrunPolicy : std::uint8_t {
    fail_release = 1,
    reject_newest = 2,
};

enum class SampledIoUnderrunPolicy : std::uint8_t {
    fail_release = 1,
    substitute_safe = 2,
};

enum class SampledIoFrameStatus : std::uint32_t {
    initial = 1,
    produced = 2,
    safe = 3,
};

enum class SampledIoSafetyState : std::uint8_t {
    not_applicable = 0,
    unknown = 1,
    startup_acknowledged = 2,
    active = 3,
    failure_acknowledged = 4,
    shutdown_acknowledged = 5,
};

// This header is part of the additive C++ sampled-I/O contract, not a stable
// C ABI or a versioned artifact schema. Multi-byte fields and sample payloads
// are little-endian. payload_checksum covers only bytes after this header.
struct SampledIoFrameHeader {
    std::uint32_t struct_size = sizeof(SampledIoFrameHeader);
    std::uint32_t version = sampled_io_frame_version;
    std::uint64_t channel_identity = 0;
    std::uint64_t sequence = 0;
    std::uint64_t release_generation = 0;
    std::uint32_t sample_count = 0;
    std::uint32_t encoding = 0;
    std::uint64_t timestamp_domain_identity = 0;
    std::uint64_t first_sample_timestamp = 0;
    std::uint64_t sample_interval_ns = 0;
    std::uint64_t trigger_identity = 0;
    std::uint64_t trigger_sequence = 0;
    std::uint64_t calibration_identity = 0;
    std::uint64_t payload_checksum = 0;
    std::uint32_t status =
        static_cast<std::uint32_t>(SampledIoFrameStatus::initial);
    std::uint32_t reserved0 = 0;
    std::array<std::uint64_t, 2> reserved{};
};

static_assert(sizeof(SampledIoFrameHeader) == 120);

struct SampledIoChannelRegistration {
    CrossRateChannelHandle channel{};
    SampledIoDirection direction = SampledIoDirection::input;
    std::uint64_t channel_identity = 0;
    SampledIoEncoding encoding = SampledIoEncoding::signed_int16_le;
    std::uint32_t element_count = 0;
    std::uint32_t samples_per_frame = 0;
    // Exact affine engineering conversion: value * scale_num / scale_den +
    // offset_num / offset_den. Denominators are positive and nonzero.
    std::int64_t scale_numerator = 1;
    std::uint64_t scale_denominator = 1;
    std::int64_t offset_numerator = 0;
    std::uint64_t offset_denominator = 1;
    std::uint64_t units_identity = 0;
    std::uint64_t calibration_identity = 0;
    std::uint64_t sample_period_ns = 0;
    std::uint64_t timestamp_domain_identity = 0;
    std::uint64_t clock_domain_identity = 0;
    SampledIoTriggerMode trigger_mode = SampledIoTriggerMode::periodic;
    std::uint64_t trigger_identity = 0;
    std::uint32_t ring_capacity = 0;
    std::uint64_t initial_sequence = 0;
    std::uint64_t maximum_age_ns = 0;
    SampledIoStalePolicy stale_policy = SampledIoStalePolicy::fail_release;
    SampledIoOverrunPolicy overrun_policy =
        SampledIoOverrunPolicy::fail_release;
    SampledIoUnderrunPolicy underrun_policy =
        SampledIoUnderrunPolicy::fail_release;
    std::uint64_t safe_transition_timeout_ns = 0;
    std::span<const std::byte> initial_frame{};
    std::span<const std::byte> startup_safe_frame{};
    std::span<const std::byte> failure_safe_frame{};
    std::span<const std::byte> shutdown_safe_frame{};
};

struct CompiledSampledIoChannel {
    CrossRateChannelHandle channel{};
    std::size_t registration_index = 0;
    SampledIoDirection direction = SampledIoDirection::input;
    std::uint64_t channel_identity = 0;
    SampledIoEncoding encoding = SampledIoEncoding::signed_int16_le;
    std::uint32_t element_count = 0;
    std::uint32_t samples_per_frame = 0;
    std::size_t frame_bytes = 0;
    std::int64_t scale_numerator = 1;
    std::uint64_t scale_denominator = 1;
    std::int64_t offset_numerator = 0;
    std::uint64_t offset_denominator = 1;
    std::uint64_t units_identity = 0;
    std::uint64_t calibration_identity = 0;
    std::uint64_t sample_period_ns = 0;
    std::uint64_t timestamp_domain_identity = 0;
    std::uint64_t clock_domain_identity = 0;
    SampledIoTriggerMode trigger_mode = SampledIoTriggerMode::periodic;
    std::uint64_t trigger_identity = 0;
    std::uint32_t ring_capacity = 0;
    std::uint64_t initial_sequence = 0;
    std::uint64_t maximum_age_ns = 0;
    SampledIoStalePolicy stale_policy = SampledIoStalePolicy::fail_release;
    SampledIoOverrunPolicy overrun_policy =
        SampledIoOverrunPolicy::fail_release;
    SampledIoUnderrunPolicy underrun_policy =
        SampledIoUnderrunPolicy::fail_release;
    std::uint64_t safe_transition_timeout_ns = 0;
    PhaseHandle device_phase{};
    DeviceBackendHandle backend{};
    std::size_t payload_reference_ordinal =
        invalid_device_rate_payload_reference_ordinal;
};

struct SampledIoChannelStatus {
    CrossRateChannelHandle channel{};
    SampledIoSafetyState safety_state =
        SampledIoSafetyState::not_applicable;
    std::uint64_t last_sequence = 0;
    std::uint64_t accepted_frames = 0;
    std::uint64_t stale_frames = 0;
    std::uint64_t overruns = 0;
    std::uint64_t underruns = 0;
    std::uint64_t substituted_frames = 0;
    Status last_status = Status::ok;
};

[[nodiscard]] std::size_t sampled_io_encoding_bytes(
    SampledIoEncoding encoding) noexcept;
[[nodiscard]] std::uint64_t sampled_io_payload_checksum(
    std::span<const std::byte> payload) noexcept;

struct CompiledCrossRateChannel {
    CrossRateChannelHandle channel{};
    std::array<char, cross_rate_channel_name_capacity> name{};
    std::size_t registration_index = 0;
    PhaseHandle producer{};
    PhaseHandle consumer{};
    RateDomainHandle producer_domain{};
    RateDomainHandle consumer_domain{};
    std::size_t producer_compiled_phase_index = 0;
    std::size_t consumer_compiled_phase_index = 0;
    std::size_t payload_size = 0;
    CrossRateMode mode = CrossRateMode::sample_and_hold;
    std::uint64_t maximum_age_ns =
        std::numeric_limits<std::uint64_t>::max();
    std::size_t first_selection_index = 0;
    std::size_t selection_count = 0;
    std::size_t snapshot_slot_count = 0;
    std::size_t snapshot_bytes = 0;
    CrossRateDeviceEndpointSelector producer_device{};
    CrossRateDeviceEndpointSelector consumer_device{};
    DeviceBackendHandle producer_device_backend{};
    DeviceBackendHandle consumer_device_backend{};
    DeviceBufferHandle producer_device_buffer{};
    DeviceBufferHandle consumer_device_buffer{};
    std::uint64_t producer_device_base_offset = 0;
    std::uint64_t consumer_device_base_offset = 0;
    std::uint32_t producer_device_slot_count = 0;
    std::uint32_t consumer_device_slot_count = 0;
    std::uint64_t producer_timestamp_domain_identity =
        cross_rate_runtime_logical_timestamp_domain_identity;
};

// Two records are emitted for each consumer reference release: one for the
// first supercycle and one for the repeated steady-state supercycle. This
// keeps an initial sample distinct from a real prior-cycle generation.
struct CompiledCrossRateSelection {
    CrossRateChannelHandle channel{};
    std::size_t channel_registration_index = 0;
    PhaseHandle producer{};
    PhaseHandle consumer{};
    RateDomainHandle producer_domain{};
    RateDomainHandle consumer_domain{};
    CrossRateSelectionHorizon horizon =
        CrossRateSelectionHorizon::first_supercycle;
    std::size_t consumer_reference_index =
        invalid_reference_release_index;
    std::uint64_t consumer_release_sequence = invalid_cross_rate_sequence;
    std::uint32_t consumer_substep_ordinal = invalid_cross_rate_substep;
    std::size_t producer_reference_index =
        invalid_reference_release_index;
    std::uint64_t producer_release_sequence = invalid_cross_rate_sequence;
    std::uint32_t producer_substep_ordinal = invalid_cross_rate_substep;
    std::int32_t source_cycle_offset = 0;
    std::uint64_t age_ns = 0;
    CrossRateSampleProvenance provenance =
        CrossRateSampleProvenance::initial_sample;
    bool held = false;
    CrossRateFreshness freshness = CrossRateFreshness::fresh;
};

struct MemoryPlan {
    // planned_bytes is the sum of the three control fields and the three
    // *_total/storage fields. It describes requested runtime storage, not RSS.
    std::size_t memory_budget_bytes = 0;
    std::size_t planned_bytes = 0;
    std::size_t runtime_control_bytes = 0;
    std::size_t executor_control_bytes = 0;
    std::size_t phase_count = 0;
    std::size_t phase_scratch_bytes = 0;
    std::size_t phase_scratch_stride = 0;
    std::size_t phase_scratch_total_bytes = 0;
    std::size_t task_scratch_bytes = 0;
    std::size_t task_scratch_stride = 0;
    std::size_t task_scratch_slots = 0;
    std::size_t task_scratch_total_bytes = 0;
    std::size_t trace_capacity = 0;
    std::size_t trace_slot_bytes = 0;
    std::size_t trace_storage_bytes = 0;
    std::size_t state_count = 0;
    // Borrowed application storage is reported but is not included in
    // planned_bytes. Registry metadata is included in runtime_control_bytes.
    std::size_t registered_state_bytes = 0;
    std::size_t snapshot_max_bytes = 0;
    std::size_t replay_input_capacity = 0;
    std::size_t input_log_max_bytes = 0;
    std::size_t device_backend_count = 0;
    std::size_t device_buffer_count = 0;
    std::size_t device_outstanding_capacity = 0;
    std::size_t device_completion_batch = 0;
    std::size_t device_control_bytes = 0;
    // M17-03 command/timeline storage remains a subcomponent of the existing
    // device-control row and does not alter the six-row plan equation.
    std::size_t device_batch_backend_count = 0;
    std::size_t device_timeline_count = 0;
    std::size_t device_batch_queue_slots = 0;
    // Backend-reported private control storage is informational and excluded
    // from planned_bytes because the backend owns it.
    std::size_t device_backend_reported_bytes = 0;
    std::size_t queue_slots = 0;
    std::size_t scratch_alignment = 0;
    OverloadPolicy overload_policy = OverloadPolicy::reject_submission;
    // M16-01 rate-plan storage is a subcomponent of runtime_control_bytes and
    // does not add a seventh planned row or change the six-row equation.
    std::size_t rate_domain_count = 0;
    std::size_t rate_binding_count = 0;
    std::size_t reference_release_count = 0;
    std::size_t rate_plan_bytes = 0;
    // M16-02 cross-rate storage is included once in rate_plan_bytes and the
    // existing runtime_control_bytes row.
    std::size_t cross_rate_channel_count = 0;
    std::size_t cross_rate_selection_count = 0;
    std::size_t cross_rate_initial_sample_bytes = 0;
    std::size_t cross_rate_snapshot_slot_count = 0;
    std::size_t cross_rate_snapshot_bytes = 0;
    // M21-03 endpoint metadata is Runtime-owned; registered device-buffer
    // payload bytes remain borrowed and are excluded from planned_bytes.
    std::size_t cross_rate_device_endpoint_count = 0;
    std::size_t cross_rate_device_staging_bytes = 0;
    // M21-04 sampled descriptors, copied initial/safe frames, direct maps,
    // and status records remain inside rate_plan_bytes/runtime_control_bytes.
    std::size_t sampled_io_channel_count = 0;
    std::size_t sampled_io_frame_bytes = 0;
    std::size_t sampled_io_safe_frame_bytes = 0;
    std::size_t sampled_io_control_bytes = 0;
    // Active dispatcher/control/canonical checkpoint bytes remain a
    // subcomponent of rate_plan_bytes and runtime_control_bytes.
    std::size_t rate_dispatch_state_bytes = 0;
    std::size_t rate_checkpoint_state_bytes = 0;
    // M16-04 storage remains inside rate_plan_bytes/runtime_control_bytes.
    std::size_t optional_rate_domain_count = 0;
    std::size_t rate_shedding_state_bytes = 0;
    std::size_t rate_telemetry_capacity = 0;
    std::size_t rate_telemetry_slot_bytes = 0;
    std::size_t rate_telemetry_storage_bytes = 0;
    std::size_t rate_telemetry_counter_bytes = 0;
    // M21-05 remains inside rate_plan_bytes/runtime_control_bytes and adds no
    // planned row or provider-backed region.
    std::size_t mixed_rate_action_capacity = 0;
    std::size_t mixed_rate_action_slot_bytes = 0;
    std::size_t mixed_rate_action_storage_bytes = 0;
    std::size_t mixed_rate_replay_control_bytes = 0;
    // M21-01 device-rate model/admission storage is included exactly once in
    // rate_plan_bytes and the existing runtime_control_bytes row.
    std::size_t device_rate_phase_count = 0;
    std::size_t device_rate_command_count = 0;
    std::size_t device_rate_payload_reference_count = 0;
    std::size_t device_rate_timeline_reference_count = 0;
    std::size_t device_rate_admission_backend_count = 0;
    std::size_t device_rate_admission_interval_count = 0;
    std::size_t device_rate_plan_bytes = 0;
    // M21-02 immutable dispatch indexes and preallocated completion tickets
    // remain inside rate_plan_bytes/runtime_control_bytes.
    std::size_t device_rate_dispatch_record_count = 0;
    std::size_t device_rate_dependency_count = 0;
    std::size_t device_rate_execution_slot_count = 0;
    std::size_t device_rate_execution_bytes = 0;
    // M22-01 copied declarations, fixed mailbox slots/payloads, producer
    // state, counters, and target inspection state remain inside the existing
    // runtime-control row and add no provider-backed region.
    std::size_t live_control_mailbox_count = 0;
    std::size_t live_control_producer_count = 0;
    std::size_t live_control_record_capacity = 0;
    std::size_t live_control_payload_storage_bytes = 0;
    std::size_t live_control_control_bytes = 0;
    std::size_t live_control_action_capacity = 0;
    std::size_t live_control_action_storage_bytes = 0;
    std::size_t live_control_retained_generation_capacity = 0;
    std::size_t live_control_retained_record_capacity = 0;
    std::size_t live_control_retained_payload_storage_bytes = 0;
    std::size_t live_control_closure_control_bytes = 0;
    std::size_t live_control_checkpoint_state_bytes = 0;
};

inline constexpr std::uint32_t cpu_memory_policy_schema_version = 1;
inline constexpr std::size_t thread_role_report_capacity =
    6 + thread_policy_request_capacity;
inline constexpr std::size_t memory_region_report_capacity = 12;
inline constexpr std::size_t resource_accounting_name_capacity = 48;

struct ResourceAccountingKey {
    std::uint64_t value = 0;

    [[nodiscard]] constexpr bool operator==(
        const ResourceAccountingKey&) const noexcept = default;
};

enum class ResourceOwnership : std::uint8_t {
    runtime,
    caller,
    host_executor,
    backend,
    vendor,
};

enum class PolicyApplicationMode : std::uint8_t {
    apply_and_verify,
    verify_only,
};

enum class PolicyResolutionState : std::uint8_t {
    portable_default,
    portable_noop,
    unsupported_best_effort,
    inactive,
    external_verify_only,
    native_supported,
    native_best_effort_fallback,
};

enum class PolicyOperationState : std::uint8_t {
    not_attempted,
    succeeded,
    failed,
    unsupported,
    mismatched,
};

enum class MemoryAccountingScope : std::uint8_t {
    planned,
    informational_external,
    excluded,
};

enum class ResourceAccountingExactness : std::uint8_t {
    exact,
    declared_only,
    partial,
    unknown,
    not_applicable,
};

struct MemoryAccountingTotal {
    std::size_t accounted_bytes = 0;
    ResourceAccountingExactness exactness =
        ResourceAccountingExactness::not_applicable;
};

struct ThreadPolicyReport {
    ThreadRoleId role{};
    ResourceAccountingKey accounting_key{};
    std::array<char, resource_accounting_name_capacity> stable_name{};
    ResourceOwnership ownership = ResourceOwnership::runtime;
    PolicyApplicationMode application_mode =
        PolicyApplicationMode::apply_and_verify;
    std::size_t logical_instance_count = 0;
    bool cardinality_known = true;
    ThreadPolicy requested{};
    ThreadPolicy resolved{};
    PolicyResolutionState resolution =
        PolicyResolutionState::portable_default;
    PolicyOperationState applied = PolicyOperationState::not_attempted;
    PolicyOperationState verified = PolicyOperationState::not_attempted;
    ThreadPolicy applied_policy{};
    ThreadPolicy read_back{};
    std::size_t attempted_instance_count = 0;
    std::size_t applied_instance_count = 0;
    std::size_t verified_instance_count = 0;
    std::size_t fallback_instance_count = 0;
    std::int32_t resolution_error = 0;
    std::int32_t apply_error = 0;
    std::int32_t verify_error = 0;
    // Appended M15-04 accounting metadata. declared_accounted_bytes is trusted
    // host metadata for an externally owned role, never native observation.
    std::size_t declared_accounted_bytes = 0;
    ResourceAccountingExactness accounting_exactness =
        ResourceAccountingExactness::unknown;
};

struct MemoryPolicyReport {
    MemoryRegionId region{};
    ResourceAccountingKey accounting_key{};
    std::array<char, resource_accounting_name_capacity> stable_name{};
    ResourceOwnership ownership = ResourceOwnership::runtime;
    MemoryAccountingScope accounting_scope =
        MemoryAccountingScope::planned;
    std::size_t logical_region_count = 0;
    bool cardinality_known = true;
    // accounted_bytes is the existing finalized-plan or informational payload
    // identity assigned to this row. Planned rows sum exactly to
    // MemoryPlan::planned_bytes; excluded rows never contribute to that sum.
    std::size_t accounted_bytes = 0;
    std::size_t committed_bytes = 0;
    std::size_t resident_bytes = 0;
    std::size_t locked_bytes = 0;
    std::size_t pinned_bytes = 0;
    bool used_huge_page_fallback = false;
    MemoryPolicy requested{};
    MemoryPolicy resolved{};
    PolicyResolutionState resolution =
        PolicyResolutionState::portable_default;
    PolicyOperationState applied = PolicyOperationState::not_attempted;
    PolicyOperationState verified = PolicyOperationState::not_attempted;
    // M15-03 fields are appended so the pre-existing aggregate prefix remains
    // source-compatible for 1.x callers that use positional initialization.
    std::size_t actual_guard_bytes_before = 0;
    std::size_t actual_guard_bytes_after = 0;
    std::size_t actual_page_bytes = 0;
    bool used_explicit_huge_pages = false;
    PolicyOperationState acquired = PolicyOperationState::not_attempted;
    std::int32_t provider_error = 0;
    std::int32_t apply_error = 0;
    std::int32_t verify_error = 0;
    std::int32_t rollback_error = 0;
    // Appended M15-04 closure metadata preserves the M15-03 aggregate prefix.
    ResourceAccountingExactness accounting_exactness =
        ResourceAccountingExactness::unknown;
};

struct CpuMemoryPolicyReport {
    std::uint32_t schema_version = cpu_memory_policy_schema_version;
    std::size_t thread_count = 0;
    std::array<ThreadPolicyReport, thread_role_report_capacity> threads{};
    std::size_t memory_count = 0;
    std::array<MemoryPolicyReport, memory_region_report_capacity> memory{};
    // Appended M15-04 aggregate closure keeps exact, declared, and incomplete
    // totals distinct without changing the six-row MemoryPlan equation.
    PolicyRequirement accounting_requirement =
        PolicyRequirement::best_effort;
    MemoryAccountingTotal planned_total{};
    MemoryAccountingTotal informational_total{};
    MemoryAccountingTotal excluded_total{};
    MemoryAccountingTotal closed_total{};
    bool accounting_complete = false;
};

enum class RuntimeTraceEventType : std::uint16_t {
    finalized = 1,
    started = 2,
    periodic_release = 3,
    periodic_wake = 4,
    step_begin = 5,
    callback_begin = 6,
    callback_end = 7,
    watchdog_fired = 8,
    degradation_applied = 9,
    step_end = 10,
    stopped = 11,
    device_submitted = 12,
    device_completed = 13,
    device_reset = 14,
};

enum class RuntimeTraceProducer : std::uint16_t {
    host = 0,
    worker = 1,
    device_service = 2,
};

struct RuntimeTraceEvent {
    std::uint32_t schema_version = observability_schema_version;
    std::uint16_t record_size = 64;
    RuntimeTraceEventType type = RuntimeTraceEventType::step_begin;
    Status status = Status::ok;
    RuntimeTraceProducer producer = RuntimeTraceProducer::host;
    std::uint16_t reserved0 = 0;
    std::uint64_t sequence = 0;
    std::uint64_t timestamp_ns = 0;
    std::uint64_t frame_index = 0;
    std::uint32_t callback_index =
        std::numeric_limits<std::uint32_t>::max();
    std::uint32_t worker_index =
        std::numeric_limits<std::uint32_t>::max();
    // Event-specific fixed-width value. Finalization carries config_id,
    // callback events carry task_index, periodic release/wake carry their
    // absolute release, watchdog carries its timeout, and degradation carries
    // the applied level.
    std::uint64_t value = 0;
    std::uint64_t reserved1 = 0;
};

static_assert(sizeof(RuntimeTraceEvent) == 64);

enum class RuntimeMetricKind : std::uint8_t {
    counter = 0,
    gauge = 1,
};

enum class RuntimeMetricWindow : std::uint8_t {
    cumulative = 0,
    interval = 1,
};

// Numeric values are schema identifiers and remain stable for observability
// schema version 2. IDs 0-21 retain their schema version 1 meanings.
enum class RuntimeMetricId : std::uint16_t {
    frames_started = 0,
    frames_completed = 1,
    frames_failed = 2,
    callbacks_started = 3,
    callbacks_completed = 4,
    callback_failures = 5,
    deadline_misses = 6,
    watchdog_events = 7,
    degradation_events = 8,
    periodic_releases = 9,
    periodic_wakes = 10,
    trace_events_emitted = 11,
    trace_events_overwritten = 12,
    trace_events_dropped = 13,
    executor_submitted_tasks = 14,
    executor_local_executions = 15,
    executor_steal_attempts = 16,
    executor_successful_steals = 17,
    executor_queue_rejections = 18,
    executor_scratch_exhaustions = 19,
    executor_worker_starts = 20,
    degradation_level = 21,
    device_submissions = 22,
    device_completions = 23,
    device_failures = 24,
    device_queue_rejections = 25,
    device_timeouts = 26,
    device_losses = 27,
    device_resets = 28,
    device_service_polls = 29,
    device_outstanding = 30,
    device_service_starts = 31,
    count = 32,
};

inline constexpr std::size_t runtime_metric_count =
    static_cast<std::size_t>(RuntimeMetricId::count);

struct RuntimeMetricDefinition {
    RuntimeMetricId id = RuntimeMetricId::frames_started;
    RuntimeMetricKind kind = RuntimeMetricKind::counter;
    std::string_view name{};
};

[[nodiscard]] bool runtime_metric_definition(
    std::size_t schema_index,
    RuntimeMetricDefinition& definition) noexcept;
[[nodiscard]] const char* runtime_trace_event_name(
    RuntimeTraceEventType type) noexcept;

struct ObservabilityMetadata {
    std::uint32_t struct_size = observability_metadata_size;
    std::uint32_t schema_version = observability_schema_version;
    std::uint32_t runtime_version_major = version_major;
    std::uint32_t runtime_version_minor = version_minor;
    std::uint32_t runtime_version_patch = version_patch;
    std::uint32_t trace_event_size =
        static_cast<std::uint32_t>(sizeof(RuntimeTraceEvent));
    std::uint32_t metric_sample_size = 16;
    std::uint32_t metric_count = runtime_metric_count;
    std::uint64_t config_id = 0;
    std::uint64_t runtime_id = 0;
    std::uint64_t trace_capacity = 0;
    std::array<char, observability_identifier_capacity> build_id{};
    std::array<char, observability_identifier_capacity> workload_id{};
};

static_assert(sizeof(ObservabilityMetadata) == observability_metadata_size);

struct RuntimeMetricSample {
    RuntimeMetricId id = RuntimeMetricId::frames_started;
    RuntimeMetricKind kind = RuntimeMetricKind::counter;
    std::uint8_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    std::uint64_t value = 0;
};

static_assert(sizeof(RuntimeMetricSample) == 16);

struct RuntimeMetricCursor {
    std::uint32_t schema_version = observability_schema_version;
    std::uint32_t reserved0 = 0;
    std::uint64_t runtime_id = 0;
    std::uint64_t window_end_ns = 0;
    std::array<std::uint64_t, runtime_metric_count> counters{};
};

struct RuntimeMetricSnapshot {
    ObservabilityMetadata metadata{};
    RuntimeMetricWindow window = RuntimeMetricWindow::cumulative;
    std::array<std::uint8_t, 7> reserved0{};
    std::uint64_t snapshot_sequence = 0;
    std::uint64_t window_start_ns = 0;
    std::uint64_t window_end_ns = 0;
    std::size_t sample_count = 0;
    std::array<RuntimeMetricSample, runtime_metric_count> samples{};
};

struct RuntimeTraceCursor {
    std::uint32_t schema_version = observability_schema_version;
    std::uint32_t reserved0 = 0;
    std::uint64_t runtime_id = 0;
    std::uint64_t next_sequence = 0;
};

struct RuntimeTraceReadResult {
    ObservabilityMetadata metadata{};
    std::uint64_t first_sequence = 0;
    std::uint64_t next_sequence = 0;
    std::size_t events_read = 0;
    std::uint64_t lost_events = 0;
    std::uint64_t remaining_sequence_count = 0;
};

// Host-driven lifecycle introduced by M1. Control methods are single-host-
// thread operations. A step invokes callbacks synchronously and never paces or
// sleeps; self-paced execution uses the separate M5 run_periodic API.
class Runtime {
public:
    Runtime();
    explicit Runtime(RuntimeClock& clock);
    Runtime(RuntimeClock& clock, PlatformPreflightProbe& preflight);
    ~Runtime();

    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    [[nodiscard]] Status configure(const RuntimeConfig& config) noexcept;
    // Copies the bounded C++ policy model. Schema-7 JSON profiles and stable
    // C ABI v8 intentionally do not include this additive source API.
    // Validation and portable resolution occur transactionally in finalize().
    [[nodiscard]] Status set_cpu_memory_policy(
        const CpuMemoryPolicy& policy) noexcept;
    // Copies the bounded callback table and borrows provider.user_data through
    // checked stop. The provider is used only for active phase scratch, task
    // scratch, and trace storage and may be attached only while configuring.
    [[nodiscard]] Status set_memory_provider(
        const MemoryProvider& provider) noexcept;
    // Copies the callback table and borrows adapter.user_data through stop().
    // May be called only while configuring.
    [[nodiscard]] Status set_host_executor(
        const HostExecutorAdapter& adapter) noexcept;
    // Copies the opt-in active CPU rate policy while configuring. There is no
    // unset operation; reference-only behavior is retained by not calling it.
    [[nodiscard]] Status set_rate_execution_policy(
        const RateExecutionPolicy& policy) noexcept;
    [[nodiscard]] Status set_mixed_rate_closure_policy(
        const MixedRateClosurePolicy& policy) noexcept;
    [[nodiscard]] Status set_live_control_policy(
        const LiveControlPolicy& policy) noexcept;
    [[nodiscard]] Status set_live_control_closure_policy(
        const LiveControlClosurePolicy& policy) noexcept;
    [[nodiscard]] Status set_live_control_replay_retention_policy(
        const LiveControlReplayRetentionPolicy& policy) noexcept;
    [[nodiscard]] Status configure_key(
        std::string_view key,
        std::string_view value) noexcept;
    [[nodiscard]] Status register_callback(
        const CallbackRegistration& registration) noexcept;
    // The returned phase handle is required when defining dependencies or
    // logical resource access. Handles are valid only for this Runtime.
    [[nodiscard]] Status register_callback(
        const CallbackRegistration& registration,
        PhaseHandle& out_phase) noexcept;
    // Invokes an already-resolved ABI-v1 entry function only while
    // configuring. The complete extension is staged and published as one
    // transaction; Runtime never resolves, loads, or unloads a module.
    [[nodiscard]] Status register_extension(
        rtfw_extension_entry_fn_v1 entry,
        ExtensionHandle& out_extension) noexcept;
    [[nodiscard]] Status register_device_backend(
        const DeviceBackendRegistration& registration,
        DeviceBackendHandle& out_backend) noexcept;
    [[nodiscard]] Status register_device_backend(
        const HalV2BackendRegistration& registration,
        DeviceBackendHandle& out_backend) noexcept;
    [[nodiscard]] Status register_device_buffer(
        const DeviceBufferRegistration& registration,
        DeviceBufferHandle& out_buffer) noexcept;
    [[nodiscard]] Status register_device_buffer(
        const HeterogeneousDeviceBufferRegistration& registration,
        DeviceBufferHandle &out_buffer) noexcept;
    [[nodiscard]] Status register_device_phase(
        const DevicePhaseRegistration& registration,
        PhaseHandle& out_phase) noexcept;
    [[nodiscard]] Status register_device_timeline(
        const DeviceTimelineRegistration& registration,
        DeviceTimelineHandle& out_timeline) noexcept;
    [[nodiscard]] Status register_device_batch_phase(
        const DeviceBatchPhaseRegistration& registration,
        PhaseHandle& out_phase) noexcept;
    [[nodiscard]] Status register_rate_domain(
        const RateDomainRegistration& registration,
        RateDomainHandle& out_domain) noexcept;
    // Replaces copied metadata without changing the instance-owned identity.
    // This supports correcting a plan after transactional finalize rejection.
    [[nodiscard]] Status replace_rate_domain(
        RateDomainHandle domain,
        const RateDomainRegistration& registration) noexcept;
    [[nodiscard]] Status bind_phase_to_rate_domain(
        PhaseHandle phase,
        RateDomainHandle domain) noexcept;
    [[nodiscard]] Status bind_phase_to_rate_domain(
        const RatePhaseBinding& binding) noexcept;
    // Copies only the role span. Backend, commands, buffer slices,
    // synchronization, and timelines are derived from the phase's copied M17
    // declaration during finalization.
    [[nodiscard]] Status bind_device_phase_to_rate_domain(
        const DeviceRatePhaseBinding& binding) noexcept;
    [[nodiscard]] Status replace_device_rate_binding(
        const DeviceRatePhaseBinding& binding) noexcept;
    [[nodiscard]] Status register_cross_rate_channel(
        const CrossRateChannelRegistration& registration,
        CrossRateChannelHandle& out_channel) noexcept;
    // Replaces copied semantics without changing the instance-owned handle,
    // allowing correction after transactional finalization rejection.
    [[nodiscard]] Status replace_cross_rate_channel(
        CrossRateChannelHandle channel,
        const CrossRateChannelRegistration& registration) noexcept;
    [[nodiscard]] Status register_sampled_io_channel(
        const SampledIoChannelRegistration& registration) noexcept;
    [[nodiscard]] Status replace_sampled_io_channel(
        CrossRateChannelHandle channel,
        const SampledIoChannelRegistration& registration) noexcept;
    [[nodiscard]] Status register_live_control_mailbox(
        const LiveControlMailboxRegistration& registration) noexcept;
    [[nodiscard]] Status register_live_control_producer(
        const LiveControlProducerRegistration& registration) noexcept;
    [[nodiscard]] Status register_resource(
        std::string_view name,
        ResourceHandle& out_resource) noexcept;
    [[nodiscard]] Status register_state(
        const StateRegistration& registration) noexcept;
    [[nodiscard]] Status add_dependency(
        PhaseHandle prerequisite,
        PhaseHandle dependent) noexcept;
    [[nodiscard]] Status declare_resource_access(
        PhaseHandle phase,
        ResourceHandle resource,
        ResourceAccess access) noexcept;
    [[nodiscard]] Status finalize() noexcept;
    [[nodiscard]] Status start() noexcept;
    [[nodiscard]] Status step(
        const HostFrameContext& frame,
        StepResult* result = nullptr) noexcept;
    // Runs a finite runtime-owned loop on the calling frame thread. Releases
    // are first_release + i * period; late frames never shift that epoch.
    [[nodiscard]] Status run_periodic(
        const PeriodicRunConfig& config,
        PeriodicFrameObserver observer = nullptr,
        void* observer_data = nullptr,
        PeriodicRunResult* result = nullptr) noexcept;
    // Device cleanup failures retain borrowed ownership, quiesce execution,
    // and leave the public lifecycle state unchanged. Retry stop() until it
    // succeeds before releasing borrowed resources. The destructor cannot
    // report this status and is only a best-effort fallback.
    [[nodiscard]] Status stop() noexcept;
    [[nodiscard]] Status device_health(
        DeviceBackendHandle backend,
        DeviceHealth& health) noexcept;
    [[nodiscard]] Status reset_device(
        DeviceBackendHandle backend) noexcept;
    // Checked detach never performs an operating-system unload. It succeeds
    // only after checked stop released every borrowed extension owner, clears
    // all copied callable pointers, retires the generation, and then reports
    // unload readiness.
    [[nodiscard]] Status detach_extension(
        ExtensionHandle extension,
        bool& unload_ready) noexcept;
    // Optional service status runs synchronously on the caller's host-control
    // path and preserves output on rejection or callback failure.
    [[nodiscard]] Status extension_service_status(
        ExtensionHandle extension,
        std::size_t service_index,
        rtfw_extension_service_status_v1& status) noexcept;

    [[nodiscard]] RuntimeState state() const noexcept;
    [[nodiscard]] const RuntimeConfig& config() const noexcept;
    [[nodiscard]] std::size_t callback_count() const noexcept;
    [[nodiscard]] std::size_t extension_count() const noexcept;
    [[nodiscard]] bool extension_at(
        std::size_t index,
        ExtensionInfo& info) const noexcept;
    [[nodiscard]] Status extension_info(
        ExtensionHandle extension,
        ExtensionInfo& info) const noexcept;
    [[nodiscard]] std::size_t device_backend_count() const noexcept;
    [[nodiscard]] std::size_t device_buffer_count() const noexcept;
    [[nodiscard]] std::size_t device_phase_count() const noexcept;
    [[nodiscard]] std::size_t
    device_timeline_count(DeviceBackendHandle backend) const noexcept;
    [[nodiscard]] bool device_timeline_at(
        DeviceBackendHandle backend,
        std::size_t index,
        DeviceTimelineInfo& info) const noexcept;
    [[nodiscard]] std::size_t
    device_memory_domain_count(DeviceBackendHandle backend) const noexcept;
    [[nodiscard]] bool
    device_memory_domain_at(DeviceBackendHandle backend, std::size_t index,
                            DeviceMemoryDomainHandle &handle,
                            HalV2MemoryDomain &domain) const noexcept;
    [[nodiscard]] std::size_t
    device_topology_node_count(DeviceBackendHandle backend) const noexcept;
    [[nodiscard]] bool
    device_topology_node_at(DeviceBackendHandle backend, std::size_t index,
                            DeviceTopologyNodeHandle &handle,
                            HalV2TopologyNode &node) const noexcept;
    [[nodiscard]] std::size_t
    device_topology_link_count(DeviceBackendHandle backend) const noexcept;
    [[nodiscard]] bool
    device_topology_link_at(DeviceBackendHandle backend, std::size_t index,
                            HalV2TopologyLink &link) const noexcept;
    [[nodiscard]] bool
    device_topology_link_at(DeviceBackendHandle backend, std::size_t index,
                            DeviceTopologyLinkHandle &handle,
                            HalV2TopologyLink &link) const noexcept;
    [[nodiscard]] std::size_t
    device_timestamp_domain_count(DeviceBackendHandle backend) const noexcept;
    [[nodiscard]] bool
    device_timestamp_domain_at(DeviceBackendHandle backend, std::size_t index,
                               DeviceTimestampDomainHandle &handle,
                               HalV2TimestampDomain &domain) const noexcept;
    [[nodiscard]] bool device_completion_timestamp_domain(
        DeviceBackendHandle backend,
        DeviceTimestampDomainHandle &domain) const noexcept;
    [[nodiscard]] bool
    device_memory_object_at(std::size_t index,
                            DeviceMemoryObjectInfo &object) const noexcept;
    // Explicit non-RT host query. Samples are not cached or interpreted as
    // runtime-monotonic time and are invalid across backend reset generations.
    [[nodiscard]] Status query_device_timestamp_correlation(
        DeviceBackendHandle backend, DeviceTimestampDomainHandle source,
        DeviceTimestampDomainHandle destination,
        HalV2TimestampCorrelation &correlation) noexcept;
    [[nodiscard]] std::size_t resource_count() const noexcept;
    [[nodiscard]] std::size_t dependency_count() const noexcept;
    [[nodiscard]] std::size_t resource_access_count() const noexcept;
    // Available after successful finalization. The order is deterministic and
    // remains stable through running and stopped states.
    [[nodiscard]] bool compiled_phase_at(
        std::size_t execution_index,
        PhaseHandle& phase) const noexcept;
    [[nodiscard]] bool rate_model_enabled() const noexcept;
    [[nodiscard]] bool rate_execution_enabled() const noexcept;
    [[nodiscard]] std::size_t rate_domain_count() const noexcept;
    [[nodiscard]] std::size_t rate_binding_count() const noexcept;
    [[nodiscard]] std::size_t reference_release_count() const noexcept;
    [[nodiscard]] std::uint64_t reference_supercycle_ns() const noexcept;
    [[nodiscard]] bool compiled_rate_domain_at(
        std::size_t registration_index,
        CompiledRateDomain& domain) const noexcept;
    [[nodiscard]] bool compiled_rate_binding_at(
        std::size_t compiled_phase_index,
        CompiledRateBinding& binding) const noexcept;
    [[nodiscard]] bool reference_release_at(
        std::size_t release_index,
        ReferenceRelease& release) const noexcept;
    [[nodiscard]] bool device_rate_model_enabled() const noexcept;
    [[nodiscard]] std::size_t device_rate_phase_count() const noexcept;
    [[nodiscard]] std::size_t device_rate_command_count() const noexcept;
    [[nodiscard]] std::size_t
    device_rate_payload_reference_count() const noexcept;
    [[nodiscard]] std::size_t
    device_rate_timeline_reference_count() const noexcept;
    [[nodiscard]] bool compiled_device_rate_phase_at(
        std::size_t phase_index,
        CompiledDeviceRatePhase& phase) const noexcept;
    [[nodiscard]] bool compiled_device_rate_command_at(
        std::size_t command_index,
        CompiledDeviceRateCommand& command) const noexcept;
    [[nodiscard]] bool compiled_device_rate_payload_reference_at(
        std::size_t reference_index,
        CompiledDeviceRatePayloadReference& reference) const noexcept;
    [[nodiscard]] bool compiled_device_rate_timeline_reference_at(
        std::size_t reference_index,
        CompiledDeviceRateTimelineReference& reference) const noexcept;
    [[nodiscard]] bool device_rate_admission_report(
        DeviceRateAdmissionReport& report) const noexcept;
    [[nodiscard]] bool device_rate_admission_diagnostic(
        DeviceRateAdmissionDiagnostic& diagnostic) const noexcept;
    [[nodiscard]] bool device_rate_admission_backend_at(
        std::size_t backend_index,
        DeviceRateAdmissionBackend& backend) const noexcept;
    [[nodiscard]] bool device_rate_admission_phase_at(
        std::size_t phase_index,
        DeviceRateAdmissionPhase& phase) const noexcept;
    [[nodiscard]] bool device_rate_admission_interval_at(
        std::size_t interval_index,
        DeviceRateAdmissionInterval& interval) const noexcept;
    [[nodiscard]] bool cross_rate_model_enabled() const noexcept;
    [[nodiscard]] std::size_t cross_rate_channel_count() const noexcept;
    [[nodiscard]] std::size_t cross_rate_selection_count() const noexcept;
    [[nodiscard]] bool compiled_cross_rate_channel_at(
        std::size_t registration_index,
        CompiledCrossRateChannel& channel) const noexcept;
    [[nodiscard]] bool compiled_cross_rate_selection_at(
        std::size_t selection_index,
        CompiledCrossRateSelection& selection) const noexcept;
    [[nodiscard]] bool sampled_io_model_enabled() const noexcept;
    [[nodiscard]] std::size_t sampled_io_channel_count() const noexcept;
    [[nodiscard]] bool compiled_sampled_io_channel_at(
        std::size_t registration_index,
        CompiledSampledIoChannel& channel) const noexcept;
    [[nodiscard]] bool sampled_io_channel_status(
        CrossRateChannelHandle channel,
        SampledIoChannelStatus& status) const noexcept;
    [[nodiscard]] bool live_control_enabled() const noexcept;
    [[nodiscard]] std::size_t live_control_mailbox_count() const noexcept;
    [[nodiscard]] std::size_t live_control_producer_count() const noexcept;
    [[nodiscard]] Status live_control_producer_handle(
        std::uint64_t mailbox_identity,
        std::uint64_t producer_identity,
        LiveControlProducerHandle& handle) const noexcept;
    [[nodiscard]] Status stage_live_control_update(
        LiveControlProducerHandle producer,
        const LiveControlUpdateRecord& update,
        std::span<const std::byte> payload,
        LiveControlAdmissionResult& result) noexcept;
    [[nodiscard]] bool live_control_mailbox_info(
        std::uint64_t mailbox_identity,
        LiveControlMailboxInfo& info) const noexcept;
    [[nodiscard]] bool live_control_record_at(
        std::uint64_t mailbox_identity,
        std::uint64_t mailbox_sequence,
        LiveControlUpdateRecord& record) const noexcept;
    [[nodiscard]] Status copy_live_control_payload(
        std::uint64_t mailbox_identity,
        std::uint64_t mailbox_sequence,
        std::span<std::byte> output) const noexcept;
    [[nodiscard]] bool live_control_commit_info(
        LiveControlCommitInfo& info) const noexcept;
    [[nodiscard]] bool live_control_record_status(
        std::uint64_t mailbox_identity,
        std::uint64_t mailbox_sequence,
        LiveControlRecordStatusInfo& info) const noexcept;
    [[nodiscard]] bool live_control_closure_enabled() const noexcept;
    [[nodiscard]] Status live_control_action_metadata(
        LiveControlActionMetadata& metadata) noexcept;
    [[nodiscard]] Status read_live_control_actions(
        LiveControlActionCursor& cursor,
        std::span<LiveControlActionRecord> output,
        LiveControlActionReadResult& result) noexcept;
    // Copies only into an exact-size caller span. Failure leaves output
    // unchanged and no inspector allocates or mutates the frozen plan.
    [[nodiscard]] Status copy_cross_rate_initial_sample(
        std::size_t registration_index,
        std::span<std::byte> output) const noexcept;
    [[nodiscard]] bool static_phase_assignment_at(
        std::size_t registration_index,
        StaticPhaseAssignment& assignment) const noexcept;
    [[nodiscard]] ExecutorStats executor_stats() const noexcept;
    // Available after successful finalization. Counts describe requested
    // runtime payload/control storage and exclude allocator metadata and OS
    // thread stacks.
    [[nodiscard]] bool memory_plan(MemoryPlan& plan) const noexcept;
    // Available after successful finalization. The report inventories every
    // current role and memory accounting identity exactly once. M15-01 does
    // not apply native policy, so applied/verified remain not_attempted.
    [[nodiscard]] bool cpu_memory_policy_report(
        CpuMemoryPolicyReport& report) const noexcept;
    // Available after start is attempted. Strict failures leave the runtime
    // finalized so the host can inspect every failed prerequisite.
    [[nodiscard]] bool platform_preflight_report(
        PlatformPreflightReport& report) const noexcept;
    [[nodiscard]] std::uint32_t degradation_level() const noexcept;
    [[nodiscard]] std::uint64_t now_ns() noexcept;
    // The view remains valid until the next control operation or destruction.
    [[nodiscard]] std::string_view last_error() const noexcept;

    // Observability export is a non-RT host operation. These calls reject an
    // active step/periodic loop and never invoke host callbacks.
    [[nodiscard]] Status observability_metadata(
        ObservabilityMetadata& metadata) noexcept;
    // Interval windows use a caller-owned cursor. The first interval covers
    // finalization through this call; later intervals partition cumulative
    // counters without mutating runtime-global baselines. Gauges are sampled
    // at the window end and are never differenced. Fresh cursors must be
    // default initialized.
    [[nodiscard]] Status metrics_snapshot(
        RuntimeMetricWindow window,
        RuntimeMetricCursor* cursor,
        RuntimeMetricSnapshot& snapshot) noexcept;
    // A zeroed/default cursor starts at the oldest retained event. If an
    // established cursor falls behind, lost_events reports the exact skipped
    // sequence span. The caller owns output storage; fresh cursors must be
    // default initialized.
    [[nodiscard]] Status read_trace(
        RuntimeTraceCursor& cursor,
        std::span<RuntimeTraceEvent> output,
        RuntimeTraceReadResult& result) noexcept;
    [[nodiscard]] Status rate_telemetry_metadata(
        RateTelemetryMetadata& metadata) noexcept;
    [[nodiscard]] Status rate_counters_snapshot(
        RateCounterSnapshot& snapshot) noexcept;
    [[nodiscard]] Status read_rate_actions(
        RateTelemetryCursor& cursor,
        std::span<RateActionRecord> output,
        RateTelemetryReadResult& result) noexcept;
    [[nodiscard]] Status mixed_rate_action_metadata(
        MixedRateActionMetadata& metadata) noexcept;
    [[nodiscard]] Status read_mixed_rate_actions(
        MixedRateActionCursor& cursor,
        std::span<MixedRateActionRecord> output,
        MixedRateActionReadResult& result) noexcept;

    // Checkpoint and replay calls are non-RT host operations. Buffers are
    // caller-owned, and their maximum accepted sizes are frozen by
    // RuntimeConfig. A failed restore never mutates registered state.
    [[nodiscard]] Status checkpoint_size(
        std::size_t& required_bytes) noexcept;
    [[nodiscard]] Status write_checkpoint(
        std::uint64_t checkpoint_frame_index,
        std::span<std::byte> output,
        ArtifactWriteResult& result) noexcept;
    [[nodiscard]] Status restore_checkpoint(
        std::span<const std::byte> checkpoint,
        CheckpointMetadata* metadata = nullptr) noexcept;
    [[nodiscard]] Status write_input_log(
        std::span<const ReplayInputRecord> records,
        std::span<std::byte> output,
        ArtifactWriteResult& result) noexcept;
    [[nodiscard]] Status replay(
        std::span<const std::byte> checkpoint,
        std::span<const std::byte> input_log,
        ReplayInputCallback input_callback,
        void* input_user_data = nullptr,
        ReplayResult* result = nullptr) noexcept;
    [[nodiscard]] Status write_active_replay_artifact(
        std::span<const std::byte> checkpoint,
        std::span<const ReplayInputRecord> records,
        std::span<std::byte> output,
        ArtifactWriteResult& result) noexcept;
    [[nodiscard]] Status replay_active(
        std::span<const std::byte> artifact,
        ReplayInputCallback input_callback,
        void* input_user_data = nullptr,
        ActiveReplayResult* result = nullptr) noexcept;
    [[nodiscard]] Status write_live_control_replay_artifact(
        std::span<const std::byte> checkpoint,
        std::span<const std::byte> nested_artifact,
        LiveControlNestedArtifactKind nested_kind,
        std::span<std::byte> output,
        ArtifactWriteResult& result) noexcept;
    [[nodiscard]] Status replay_live_control(
        std::span<const std::byte> artifact,
        ReplayInputCallback input_callback,
        void* input_user_data = nullptr,
        LiveControlReplayResult* result = nullptr) noexcept;
    [[nodiscard]] Status registered_state_hash(
        std::uint64_t& hash) noexcept;

    // Compatibility accessors over the latest retained trace window.
    [[nodiscard]] std::size_t trace_event_count() const noexcept;
    [[nodiscard]] bool trace_event(
        std::size_t chronological_index,
        RuntimeTraceEvent& event) const noexcept;

private:
    friend struct detail::RuntimeThreadPolicyTestAccess;
    friend struct detail::RuntimeLiveControlTestAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct DemoPipeline {
    struct State {
        std::atomic<std::uint64_t> ingestCount{0};
        std::atomic<std::uint64_t> gpuCount{0};
        std::atomic<std::uint64_t> ioCount{0};
        std::atomic<std::uint64_t> composeCount{0};
        std::atomic<std::uint64_t> fenceWaits{0};
        std::array<std::atomic<std::uint64_t>, 4> rungEventsSeen{};
    };

    DemoPipeline() = default;
    explicit DemoPipeline(std::shared_ptr<State> state) : state_(std::move(state)) {}

    [[nodiscard]] bool valid() const { return static_cast<bool>(state_); }
    [[nodiscard]] std::uint64_t ingest_frames() const;
    [[nodiscard]] std::uint64_t gpu_frames() const;
    [[nodiscard]] std::uint64_t io_frames() const;
    [[nodiscard]] std::uint64_t compose_frames() const;
    [[nodiscard]] std::uint64_t fence_waits() const;

private:
    std::shared_ptr<State> state_{};

    [[nodiscard]] std::uint64_t loadCounter(const std::atomic<std::uint64_t>& counter) const;

    friend DemoPipeline build_demo_pipeline(SimCore& sim);
};

[[deprecated(
    "legacy SimCore demo; use rt::Runtime and the rtfw::runtime target")]]
DemoPipeline build_demo_pipeline(SimCore& sim);

inline std::uint64_t DemoPipeline::loadCounter(const std::atomic<std::uint64_t>& counter) const {
    return counter.load(std::memory_order_acquire);
}

inline std::uint64_t DemoPipeline::ingest_frames() const {
    return state_ ? loadCounter(state_->ingestCount) : 0;
}

inline std::uint64_t DemoPipeline::gpu_frames() const {
    return state_ ? loadCounter(state_->gpuCount) : 0;
}

inline std::uint64_t DemoPipeline::io_frames() const {
    return state_ ? loadCounter(state_->ioCount) : 0;
}

inline std::uint64_t DemoPipeline::compose_frames() const {
    return state_ ? loadCounter(state_->composeCount) : 0;
}

inline std::uint64_t DemoPipeline::fence_waits() const {
    return state_ ? loadCounter(state_->fenceWaits) : 0;
}

} // namespace rt
