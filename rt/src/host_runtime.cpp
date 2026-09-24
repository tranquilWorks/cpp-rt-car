#include <rt/runtime.hpp>

#include "compiled_graph.hpp"
#include "cross_rate_data.hpp"
#include "command_batch.hpp"
#include "device_manager.hpp"
#include "executor.hpp"
#include "extension_registration.hpp"
#include "memory_policy.hpp"
#include "native_platform_preflight.hpp"
#include "live_control_mailbox.hpp"
#include "live_control_replay.hpp"
#include "rate_dispatch.hpp"
#include "mixed_rate_actions.hpp"
#include "mixed_rate_replay.hpp"
#include "rate_telemetry.hpp"
#include "rate_timeline.hpp"
#include "resource_policy.hpp"
#include "sampled_io.hpp"
#include "snapshot_codec.hpp"
#include "telemetry.hpp"
#include "watchdog_monitor.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <rt/arch.hpp>
#include <rt/canonical_bytes.hpp>

namespace {

constexpr std::size_t kMaxCallbacks = 65'536;
constexpr std::size_t kMaxResources = 65'536;
constexpr std::size_t kMaxScratchBytes = std::size_t{1} << 30;
constexpr std::size_t kMaxTraceEvents = std::size_t{1} << 20;
constexpr std::size_t kMaxWorkers = 256;
constexpr std::size_t kMaxExecutorQueueCapacity = std::size_t{1} << 20;
constexpr std::size_t kMaxTaskScratchBytes = std::size_t{1} << 20;
constexpr std::size_t kMaxTaskScratchSlots = std::size_t{1} << 20;
constexpr std::size_t kMaxScratchAlignment = std::size_t{1} << 12;
constexpr std::size_t kMaxMemoryBudgetBytes =
    sizeof(std::size_t) >= sizeof(std::uint64_t)
    ? static_cast<std::size_t>(std::uint64_t{1} << 40)
    : std::numeric_limits<std::size_t>::max();
constexpr std::uint64_t kMaxWatchdogTimeoutNs =
    std::uint64_t{24} * 60 * 60 * 1'000'000'000;
constexpr std::uint32_t kMaxDegradationLevel = 255;
constexpr std::size_t kMaxRegisteredStates =
    rt::detail::artifact_absolute_max_records;
constexpr std::size_t kMaxArtifactBytes =
    rt::detail::artifact_absolute_max_bytes;
constexpr std::size_t kMaxReplayInputs =
    rt::detail::artifact_absolute_max_records;
constexpr std::size_t kMaxDeviceBackends = 256;
constexpr std::size_t kMaxDeviceBuffers = 65'536;
constexpr std::size_t kMaxDeviceOutstanding = std::size_t{1} << 20;
constexpr std::size_t kMaxDeviceCompletionBatch = std::size_t{1} << 16;
constexpr std::size_t kNoCallback = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kNoWorker = std::numeric_limits<std::size_t>::max();
constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ull;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ull;
constexpr std::string_view kRateDispatchStateName = "rtfw.rate-dispatch";
constexpr std::uint32_t kRateDispatchStateSchema = 1;
constexpr std::string_view kMixedRateStateName = "rtfw.mixed-rate";
constexpr std::uint32_t kMixedRateStateSchema = 1;
constexpr std::string_view kLiveControlStateName = "rtfw.live-control";
constexpr std::uint32_t kLiveControlStateSchema = 1;
constexpr std::uint64_t kMixedRateStateMagic = 0x31535441524d4652ull;
constexpr std::size_t kMixedRateStateHeaderBytes = 128;
constexpr std::size_t kMixedRateSampledStateBytes = 64;
constexpr std::uint64_t kRateDispatchStateMagic = 0x3154534457465452ull;
constexpr std::size_t kRateDispatchStateHeaderBytes = 96;
constexpr std::size_t kRateDispatchChannelStateBytes = 64;
constexpr std::uint64_t kRateOptionalStateMagic = 0x3154504f57465452ull;
constexpr std::size_t kRateOptionalStateHeaderBytes = 48;

#ifndef RTFW_BUILD_ID_STRING
#define RTFW_BUILD_ID_STRING "rtfw-" RTFW_VERSION_STRING
#endif

constexpr char kBuildId[] = RTFW_BUILD_ID_STRING;
static_assert(
    sizeof(kBuildId) <= rt::observability_identifier_capacity,
    "RTFW_BUILD_ID_STRING exceeds the observability identifier capacity");

std::atomic<std::uint32_t> g_next_graph_owner{1};
std::atomic<std::uint64_t> g_next_live_control_runtime_id{1};
thread_local const void* g_active_runtime_callback = nullptr;

std::uint32_t next_graph_owner() noexcept {
    for (;;) {
        const auto candidate =
            g_next_graph_owner.fetch_add(1, std::memory_order_relaxed);
        if (candidate != 0 &&
            candidate != std::numeric_limits<std::uint32_t>::max()) {
            return candidate;
        }
    }
}

std::uint64_t next_live_control_runtime_id() noexcept {
    auto candidate =
        g_next_live_control_runtime_id.load(std::memory_order_relaxed);
    while (candidate != std::numeric_limits<std::uint64_t>::max()) {
        if (g_next_live_control_runtime_id.compare_exchange_weak(
                candidate,
                candidate + 1,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return candidate;
        }
    }
    return 0;
}

bool checked_time_add(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& result) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        result = 0;
        return false;
    }
    result = left + right;
    return true;
}

bool checked_time_multiply(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& result) noexcept {
    if (left != 0 &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        result = 0;
        return false;
    }
    result = left * right;
    return true;
}

bool byte_spans_overlap(
    std::span<const std::byte> left,
    std::span<const std::byte> right) noexcept {
    if (left.empty() || right.empty()) {
        return false;
    }
    const auto left_begin =
        reinterpret_cast<std::uintptr_t>(left.data());
    const auto right_begin =
        reinterpret_cast<std::uintptr_t>(right.data());
    if (left.size() >
            std::numeric_limits<std::uintptr_t>::max() -
                left_begin ||
        right.size() >
            std::numeric_limits<std::uintptr_t>::max() -
                right_begin) {
        return true;
    }
    const auto left_end = left_begin + left.size();
    const auto right_end = right_begin + right.size();
    return left_begin < right_end && right_begin < left_end;
}

std::int64_t deadline_slack(
    std::uint64_t deadline,
    std::uint64_t finish) noexcept {
    if (finish <= deadline) {
        const auto magnitude = deadline - finish;
        return magnitude >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())
            ? std::numeric_limits<std::int64_t>::max()
            : static_cast<std::int64_t>(magnitude);
    }
    const auto magnitude = finish - deadline;
    if (magnitude >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(magnitude);
}

class SteadyRuntimeClock final : public rt::RuntimeClock {
public:
    SteadyRuntimeClock() noexcept
        : origin_(std::chrono::steady_clock::now()) {}

    std::uint64_t now_ns() noexcept override {
        const auto elapsed = std::chrono::steady_clock::now() - origin_;
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    }

    rt::Status sleep_until_ns(
        std::uint64_t absolute_ns) noexcept override {
        if (absolute_ns >
            static_cast<std::uint64_t>(
                std::chrono::nanoseconds::max().count())) {
            return rt::Status::clock_failure;
        }
        const auto offset =
            std::chrono::ceil<std::chrono::steady_clock::duration>(
                std::chrono::nanoseconds(
                    static_cast<std::chrono::nanoseconds::rep>(
                        absolute_ns)));
        if (offset < std::chrono::steady_clock::duration::zero() ||
            origin_ >
                std::chrono::steady_clock::time_point::max() -
                    offset) {
            return rt::Status::clock_failure;
        }
        std::this_thread::sleep_until(
            origin_ + offset);
        return rt::Status::ok;
    }

    bool supports_absolute_sleep() const noexcept override {
        return std::chrono::steady_clock::is_steady;
    }

private:
    std::chrono::steady_clock::time_point origin_;
};

bool parse_u64(
    std::string_view value,
    std::uint64_t& parsed) noexcept {
    if (value.empty()) {
        return false;
    }

    const auto* begin = value.data();
    const auto* end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    return true;
}

bool parse_size(std::string_view value, std::size_t& parsed) noexcept {
    std::uint64_t raw = 0;
    if (!parse_u64(value, raw) ||
        raw > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    parsed = static_cast<std::size_t>(raw);
    return true;
}

bool identifier_character(char value) noexcept {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') ||
           value == '.' || value == '_' || value == ':' ||
           value == '/' || value == '@' || value == '-';
}

bool valid_identifier(
    const std::array<
        char,
        rt::observability_identifier_capacity>& identifier) noexcept {
    std::size_t length = 0;
    while (length < identifier.size() &&
           identifier[length] != '\0') {
        if (!identifier_character(identifier[length])) {
            return false;
        }
        ++length;
    }
    return length != 0 && length < identifier.size();
}

template <std::size_t Capacity>
bool valid_identifier(
    const char (&identifier)[Capacity]) noexcept {
    std::size_t length = 0;
    while (length < Capacity && identifier[length] != '\0') {
        if (!identifier_character(identifier[length])) {
            return false;
        }
        ++length;
    }
    if (length == 0 || length == Capacity) {
        return false;
    }
    return std::all_of(
        identifier + length + 1,
        identifier + Capacity,
        [](char value) { return value == '\0'; });
}

template <std::size_t Capacity>
bool reserved_zero(const std::uint64_t (&values)[Capacity]) noexcept {
    return std::all_of(
        std::begin(values),
        std::end(values),
        [](std::uint64_t value) { return value == 0; });
}

bool set_identifier(
    std::array<
        char,
        rt::observability_identifier_capacity>& identifier,
    std::string_view value) noexcept {
    if (value.empty() || value.size() >= identifier.size()) {
        return false;
    }
    for (const char character : value) {
        if (!identifier_character(character)) {
            return false;
        }
    }
    identifier.fill('\0');
    std::copy(value.begin(), value.end(), identifier.begin());
    return true;
}

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
}

void hash_u64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        hash_byte(
            hash,
            static_cast<std::uint8_t>(
                (value >> (byte * 8u)) & 0xffu));
    }
}

void hash_bytes(
    std::uint64_t& hash,
    std::span<const std::byte> bytes) noexcept {
    hash_u64(hash, bytes.size());
    for (const auto value : bytes) {
        hash_byte(hash, static_cast<std::uint8_t>(value));
    }
}

void hash_string(
    std::uint64_t& hash,
    std::string_view value) noexcept {
    hash_bytes(hash, std::as_bytes(std::span(value)));
}

[[nodiscard]] std::uint64_t content_identity(
    std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_bytes(hash, bytes);
    return hash;
}

std::string_view identifier_view(
    const std::array<
        char,
        rt::observability_identifier_capacity>& identifier) noexcept {
    const auto end =
        std::find(identifier.begin(), identifier.end(), '\0');
    return std::string_view(
        identifier.data(),
        static_cast<std::size_t>(end - identifier.begin()));
}

std::uint64_t config_identifier(
    const rt::RuntimeConfig& config) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_u64(hash, rt::runtime_config_schema_version);
    hash_u64(hash, config.callback_capacity);
    hash_u64(hash, config.scratch_bytes);
    hash_u64(hash, config.trace_capacity);
    hash_u64(
        hash,
        static_cast<std::uint64_t>(config.numerical_mode));
    hash_u64(
        hash,
        static_cast<std::uint64_t>(config.executor_policy));
    hash_u64(hash, config.worker_count);
    hash_u64(hash, config.executor_queue_capacity);
    hash_u64(hash, config.scratch_alignment);
    hash_u64(hash, config.task_scratch_bytes);
    hash_u64(hash, config.task_scratch_slots);
    hash_u64(hash, config.memory_budget_bytes);
    hash_u64(
        hash,
        static_cast<std::uint64_t>(config.overload_policy));
    hash_u64(hash, config.watchdog_timeout_ns);
    hash_u64(
        hash,
        config.watchdog_max_degradation_level);
    hash_u64(
        hash,
        static_cast<std::uint64_t>(
            config.platform_preflight_mode));
    hash_u64(
        hash,
        static_cast<std::uint64_t>(
            config.determinism_tier));
    hash_u64(hash, config.state_capacity);
    hash_u64(hash, config.snapshot_max_bytes);
    hash_u64(hash, config.replay_input_capacity);
    hash_u64(hash, config.input_log_max_bytes);
    hash_u64(hash, config.device_backend_capacity);
    hash_u64(hash, config.device_buffer_capacity);
    hash_u64(hash, config.device_outstanding_capacity);
    hash_u64(hash, config.device_completion_batch);
    hash_string(hash, identifier_view(config.workload_id));
    return hash;
}

rt::Status validate_config(const rt::RuntimeConfig& config) noexcept {
    if (config.callback_capacity == 0 ||
        config.callback_capacity > kMaxCallbacks) {
        return rt::Status::invalid_config;
    }
    if (config.scratch_bytes > kMaxScratchBytes ||
        config.trace_capacity > kMaxTraceEvents) {
        return rt::Status::invalid_config;
    }
    if (config.worker_count == 0 ||
        config.worker_count > kMaxWorkers ||
        config.executor_queue_capacity < 2 ||
        config.executor_queue_capacity > kMaxExecutorQueueCapacity ||
        (config.executor_queue_capacity &
         (config.executor_queue_capacity - 1)) != 0) {
        return rt::Status::invalid_config;
    }
    if (config.scratch_alignment < alignof(std::max_align_t) ||
        config.scratch_alignment > kMaxScratchAlignment ||
        (config.scratch_alignment &
         (config.scratch_alignment - 1)) != 0 ||
        config.task_scratch_bytes > kMaxTaskScratchBytes ||
        config.task_scratch_slots == 0 ||
        config.task_scratch_slots > kMaxTaskScratchSlots ||
        config.memory_budget_bytes == 0 ||
        config.memory_budget_bytes > kMaxMemoryBudgetBytes ||
        config.watchdog_timeout_ns > kMaxWatchdogTimeoutNs ||
        config.watchdog_max_degradation_level >
            kMaxDegradationLevel ||
        config.state_capacity > kMaxRegisteredStates ||
        config.snapshot_max_bytes <
            rt::detail::checkpoint_header_size ||
        config.snapshot_max_bytes > kMaxArtifactBytes ||
        config.replay_input_capacity > kMaxReplayInputs ||
        config.input_log_max_bytes <
            rt::detail::input_log_header_size ||
        config.input_log_max_bytes > kMaxArtifactBytes ||
        config.device_backend_capacity == 0 ||
        config.device_backend_capacity > kMaxDeviceBackends ||
        config.device_buffer_capacity > kMaxDeviceBuffers ||
        config.device_outstanding_capacity == 0 ||
        config.device_outstanding_capacity > kMaxDeviceOutstanding ||
        config.device_completion_batch == 0 ||
        config.device_completion_batch > kMaxDeviceCompletionBatch ||
        config.device_completion_batch >
            config.device_outstanding_capacity ||
        !valid_identifier(config.workload_id)) {
        return rt::Status::invalid_config;
    }
    switch (config.numerical_mode) {
    case rt::NumericalMode::precise:
    case rt::NumericalMode::fused_multiply_add:
        break;
    default:
        return rt::Status::invalid_config;
    }
    switch (config.executor_policy) {
    case rt::ExecutorPolicy::static_deterministic:
    case rt::ExecutorPolicy::bounded_throughput:
    case rt::ExecutorPolicy::host_adapter:
        break;
    default:
        return rt::Status::invalid_config;
    }
    switch (config.overload_policy) {
    case rt::OverloadPolicy::reject_submission:
    case rt::OverloadPolicy::fail_frame:
        break;
    default:
        return rt::Status::invalid_config;
    }
    switch (config.platform_preflight_mode) {
    case rt::PlatformPreflightMode::disabled:
    case rt::PlatformPreflightMode::strict:
        break;
    default:
        return rt::Status::invalid_config;
    }
    switch (config.determinism_tier) {
    case rt::DeterminismTier::unspecified:
        break;
    case rt::DeterminismTier::schedule_independent:
        if (config.executor_policy !=
                rt::ExecutorPolicy::static_deterministic ||
            config.watchdog_timeout_ns != 0) {
            return rt::Status::invalid_config;
        }
        break;
    case rt::DeterminismTier::reproducible_build:
    case rt::DeterminismTier::portable_deterministic:
        return rt::Status::invalid_config;
    default:
        return rt::Status::invalid_config;
    }
    return rt::Status::ok;
}

} // namespace

namespace rt {

Capabilities query_capabilities() noexcept {
    // M11 adds the borrowed host job-system executor policy.
    return {
        true, true, true, true, true, true,
        true, true, true, true, true};
}

const char* status_message(Status status) noexcept {
    switch (status) {
    case Status::ok:
        return "ok";
    case Status::invalid_argument:
        return "invalid argument";
    case Status::invalid_state:
        return "invalid runtime state";
    case Status::invalid_config:
        return "invalid runtime configuration";
    case Status::capacity_exceeded:
        return "configured capacity exceeded";
    case Status::callback_failed:
        return "user callback failed";
    case Status::resource_exhausted:
        return "resource allocation failed";
    case Status::internal_error:
        return "internal runtime error";
    case Status::invalid_handle:
        return "invalid graph handle";
    case Status::graph_cycle:
        return "graph contains a cycle";
    case Status::resource_conflict:
        return "unordered conflicting resource access";
    case Status::queue_full:
        return "executor queue is full";
    case Status::scratch_exhausted:
        return "task scratch plan is exhausted";
    case Status::platform_preflight_failed:
        return "strict platform preflight failed";
    case Status::clock_failure:
        return "runtime clock operation failed";
    case Status::invalid_artifact:
        return "checkpoint or input-log artifact is invalid";
    case Status::incompatible_artifact:
        return "checkpoint or input-log artifact is incompatible";
    case Status::device_queue_full:
        return "device submission queue is full";
    case Status::device_timeout:
        return "device submission timed out";
    case Status::device_error:
        return "device backend error";
    case Status::device_lost:
        return "device was lost";
    case Status::device_canceled:
        return "device submission was canceled";
    case Status::device_reset_required:
        return "device reset is required";
    case Status::incompatible_abi:
        return "C ABI version or layout is incompatible";
    }
    return "unknown runtime status";
}

Status set_runtime_config_value(
    RuntimeConfig& config,
    std::string_view key,
    std::string_view value) noexcept {
    RuntimeConfig candidate = config;
    std::size_t parsed = 0;
    std::uint64_t parsed_u64 = 0;

    if (key == "callback_capacity") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.callback_capacity = parsed;
    } else if (key == "scratch_bytes") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.scratch_bytes = parsed;
    } else if (key == "trace_capacity") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.trace_capacity = parsed;
    } else if (key == "numerical_mode") {
        if (value == "precise") {
            candidate.numerical_mode = NumericalMode::precise;
        } else if (value == "fused_multiply_add") {
            candidate.numerical_mode = NumericalMode::fused_multiply_add;
        } else {
            return Status::invalid_config;
        }
    } else if (key == "executor_policy") {
        if (value == "static_deterministic") {
            candidate.executor_policy =
                ExecutorPolicy::static_deterministic;
        } else if (value == "bounded_throughput") {
            candidate.executor_policy =
                ExecutorPolicy::bounded_throughput;
        } else if (value == "host_adapter") {
            candidate.executor_policy =
                ExecutorPolicy::host_adapter;
        } else {
            return Status::invalid_config;
        }
    } else if (key == "worker_count") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.worker_count = parsed;
    } else if (key == "executor_queue_capacity") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.executor_queue_capacity = parsed;
    } else if (key == "scratch_alignment") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.scratch_alignment = parsed;
    } else if (key == "task_scratch_bytes") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.task_scratch_bytes = parsed;
    } else if (key == "task_scratch_slots") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.task_scratch_slots = parsed;
    } else if (key == "memory_budget_bytes") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.memory_budget_bytes = parsed;
    } else if (key == "overload_policy") {
        if (value == "reject_submission") {
            candidate.overload_policy =
                OverloadPolicy::reject_submission;
        } else if (value == "fail_frame") {
            candidate.overload_policy =
                OverloadPolicy::fail_frame;
        } else {
            return Status::invalid_config;
        }
    } else if (key == "watchdog_timeout_ns") {
        if (!parse_u64(value, parsed_u64)) {
            return Status::invalid_config;
        }
        candidate.watchdog_timeout_ns = parsed_u64;
    } else if (key == "watchdog_max_degradation_level") {
        if (!parse_size(value, parsed) ||
            parsed > std::numeric_limits<std::uint32_t>::max()) {
            return Status::invalid_config;
        }
        candidate.watchdog_max_degradation_level =
            static_cast<std::uint32_t>(parsed);
    } else if (key == "platform_preflight_mode") {
        if (value == "disabled") {
            candidate.platform_preflight_mode =
                PlatformPreflightMode::disabled;
        } else if (value == "strict") {
            candidate.platform_preflight_mode =
                PlatformPreflightMode::strict;
        } else {
            return Status::invalid_config;
        }
    } else if (key == "determinism_tier") {
        if (value == "unspecified" || value == "d0") {
            candidate.determinism_tier =
                DeterminismTier::unspecified;
        } else if (
            value == "schedule_independent" ||
            value == "d1") {
            candidate.determinism_tier =
                DeterminismTier::schedule_independent;
        } else {
            return Status::invalid_config;
        }
    } else if (key == "state_capacity") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.state_capacity = parsed;
    } else if (key == "snapshot_max_bytes") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.snapshot_max_bytes = parsed;
    } else if (key == "replay_input_capacity") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.replay_input_capacity = parsed;
    } else if (key == "input_log_max_bytes") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.input_log_max_bytes = parsed;
    } else if (key == "device_backend_capacity") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.device_backend_capacity = parsed;
    } else if (key == "device_buffer_capacity") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.device_buffer_capacity = parsed;
    } else if (key == "device_outstanding_capacity") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.device_outstanding_capacity = parsed;
    } else if (key == "device_completion_batch") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.device_completion_batch = parsed;
    } else if (key == "workload_id") {
        if (!set_identifier(candidate.workload_id, value)) {
            return Status::invalid_config;
        }
    } else {
        return Status::invalid_config;
    }

    const auto status = validate_config(candidate);
    if (status == Status::ok) {
        config = candidate;
    }
    return status;
}

double NumericalPolicy::multiply_add(
    double a,
    double b,
    double c) const noexcept {
    if (mode_ == NumericalMode::fused_multiply_add) {
        return std::fma(a, b, c);
    }

    volatile double product = a * b;
    return product + c;
}

struct Runtime::Impl {
    class SpinGuard {
    public:
        explicit SpinGuard(std::atomic_flag& lock) noexcept
            : lock_(lock) {
            while (lock_.test_and_set(std::memory_order_acquire)) {
                rt::cpu_relax();
            }
        }

        ~SpinGuard() {
            lock_.clear(std::memory_order_release);
        }

        SpinGuard(const SpinGuard&) = delete;
        SpinGuard& operator=(const SpinGuard&) = delete;

    private:
        std::atomic_flag& lock_;
    };

    enum class PhaseKind : std::uint8_t {
        cpu,
        device,
        device_batch,
    };

    struct RegisteredCallback {
        std::string name;
        PhaseKind kind = PhaseKind::cpu;
        FrameCallback callback = nullptr;
        DeviceCommandCallback device_callback = nullptr;
        DeviceBatchCommandCallback batch_callback = nullptr;
        void* user_data = nullptr;
        std::uint32_t device_backend_index = 0;
        DeviceCommandBatch batch_declaration{};
    };

    struct RegisteredResource {
        std::string name;
    };

    struct RegisteredState {
        std::array<char, replay_identifier_capacity> name{};
        std::uint32_t schema_version = 0;
        std::span<std::byte> storage{};
    };

    struct ActiveChannelState {
        std::size_t payload_offset = 0;
        std::uint64_t generation = 1;
        std::uint64_t next_generation = 2;
        std::uint64_t source_logical_release_ns = 0;
        CrossRateSampleProvenance provenance =
            CrossRateSampleProvenance::initial_sample;
        bool held = false;
        std::uint64_t producer_release_sequence = 0;
        std::uint32_t producer_substep_ordinal = 0;
        Status producer_completion_status = Status::ok;
        std::uint64_t producer_timestamp_domain_identity = 0;
        std::uint64_t producer_timestamp = 0;
    };

    struct ActiveSheddingState {
        std::uint64_t shed_mask = 0;
        std::uint32_t consecutive_late = 0;
        std::uint32_t consecutive_on_time = 0;
    };

    explicit Impl(
        RuntimeClock* injected_clock,
        PlatformPreflightProbe* injected_preflight)
        : graph_owner(next_graph_owner()),
          clock(injected_clock ? injected_clock : &owned_clock),
          preflight(
              injected_preflight
                  ? injected_preflight
                  : &owned_preflight),
          thread_policy(&owned_thread_policy) {
        error[0] = '\0';
    }

    ~Impl() {
        const auto extension_stop = request_extension_stop();
        bool lanes_clean = true;
        if (devices) {
            lanes_clean = devices->stop() == Status::ok;
        }
        if (executor) {
            lanes_clean = executor->stop() == Status::ok && lanes_clean;
        }
        lanes_clean = watchdog.stop() == Status::ok && lanes_clean;
        const auto extension_cleanup = cleanup_extension_services();
        bool memory_clean = lanes_clean && extension_cleanup == Status::ok;
        if (resident_regions && memory_clean) {
            memory_clean = resident_regions->rollback(cpu_memory_policy_report);
        }
        if (extension_stop != Status::ok || !lanes_clean ||
            extension_cleanup != Status::ok || !memory_clean) {
            // A destructor cannot return cleanup status. Returning here would
            // destroy ownership records and provider tokens while a lane,
            // backend, or stack cleanup is unresolved. Checked stop is the
            // recoverable path; misuse at destruction fails closed.
            std::terminate();
        }
        telemetry.reset();
        executor.reset();
        if (resident_regions && memory_clean) {
            resident_regions->release();
        }
    }

    [[nodiscard]] bool provider_callback_active() const noexcept {
        return memory_provider_callback_active.load(std::memory_order_acquire) ||
               extension_control_callback_active.load(
                   std::memory_order_acquire);
    }

    void clear_error() noexcept {
        SpinGuard guard(error_lock);
        error[0] = '\0';
    }

    Status fail(Status status, const char* message) noexcept {
        SpinGuard guard(error_lock);
        if (!message) {
            message = status_message(status);
        }
        std::snprintf(error.data(), error.size(), "%s", message);
        return status;
    }

    Status fail_callback(std::size_t index) noexcept {
        SpinGuard guard(error_lock);
        const char* name = index < callbacks.size()
            ? callbacks[index].name.c_str()
            : "<unknown>";
        std::snprintf(
            error.data(),
            error.size(),
            "callback %zu ('%s') failed",
            index,
            name);
        return Status::callback_failed;
    }

    [[nodiscard]] bool valid_phase(PhaseHandle phase) const noexcept {
        return phase.valid() &&
               phase.owner() == graph_owner &&
               phase.index() < callbacks.size();
    }

    [[nodiscard]] bool valid_resource(ResourceHandle resource) const noexcept {
        return resource.valid() &&
               resource.owner() == graph_owner &&
               resource.index() < resources.size();
    }

    [[nodiscard]] bool valid_rate_domain(
        RateDomainHandle domain) const noexcept {
        return domain.valid() && domain.owner() == graph_owner &&
               domain.index() < rate_domains.size();
    }

    [[nodiscard]] bool valid_cross_rate_channel(
        CrossRateChannelHandle channel) const noexcept {
        return channel.valid() && channel.owner() == graph_owner &&
               channel.index() < cross_rate_channels.size();
    }

    [[nodiscard]] bool valid_device_backend(
        DeviceBackendHandle backend) const noexcept {
        return backend.valid() &&
               backend.owner() == graph_owner &&
               backend.index() < device_backends.size();
    }

    [[nodiscard]] bool valid_device_buffer(
        DeviceBufferHandle buffer) const noexcept {
        return buffer.valid() &&
               buffer.owner() == graph_owner &&
               buffer.index() < device_buffers.size();
    }

    [[nodiscard]] bool valid_device_timeline(
        DeviceTimelineHandle timeline) const noexcept {
        return timeline.valid() && timeline.owner() == graph_owner &&
               timeline.index() < device_timelines.size();
    }

    [[nodiscard]] bool valid_extension(
        ExtensionHandle extension) const noexcept {
        if (!extension.valid() || extension.owner != graph_owner ||
            extension.slot >= extensions.size() ||
            !extensions[extension.slot]) {
            return false;
        }
        const auto& record = *extensions[extension.slot];
        return record.generation.load(std::memory_order_acquire) ==
                   extension.generation &&
               record.state.load(std::memory_order_acquire) !=
                   ExtensionLifecycleState::detached;
    }

    [[nodiscard]] Status initialize_extension_services() noexcept {
        for (auto& extension : extensions) {
            if (!extension || extension->unload_ready.load(
                    std::memory_order_acquire)) {
                continue;
            }
            extension->open_admission();
            extension->state.store(
                ExtensionLifecycleState::registered,
                std::memory_order_release);
            for (std::size_t index = 0;
                 index < extension->service_count; ++index) {
                extension_control_callback_active.store(
                    true, std::memory_order_release);
                const auto status =
                    extension->services[index].call_initialize();
                extension_control_callback_active.store(
                    false, std::memory_order_release);
                if (status != Status::ok) {
                    extension->state.store(
                        ExtensionLifecycleState::failed,
                        std::memory_order_release);
                    (void)request_extension_stop();
                    const auto cleanup = cleanup_extension_services();
                    return cleanup == Status::ok ? status : cleanup;
                }
            }
        }
        return Status::ok;
    }

    [[nodiscard]] Status request_extension_stop() noexcept {
        Status first = Status::ok;
        for (auto& extension : extensions) {
            if (!extension || extension->unload_ready.load(
                    std::memory_order_acquire)) {
                continue;
            }
            extension->close_admission();
            extension->state.store(
                ExtensionLifecycleState::stop_requested,
                std::memory_order_release);
            for (std::size_t index = 0;
                 index < extension->service_count; ++index) {
                extension_control_callback_active.store(
                    true, std::memory_order_release);
                const auto status =
                    extension->services[index].call_request_stop();
                extension_control_callback_active.store(
                    false, std::memory_order_release);
                if (first == Status::ok && status != Status::ok) {
                    first = status;
                }
            }
        }
        return first;
    }

    [[nodiscard]] Status cleanup_extension_services() noexcept {
        Status first = Status::ok;
        for (std::size_t extension_index = extensions.size();
             extension_index != 0; --extension_index) {
            auto& extension = extensions[extension_index - 1];
            if (!extension || extension->unload_ready.load(
                    std::memory_order_acquire)) {
                continue;
            }
            for (std::size_t service_index = extension->service_count;
                 service_index != 0; --service_index) {
                const auto local_service = service_index - 1;
                if (!extension->services[local_service].released.load(
                        std::memory_order_acquire) &&
                    !extension->services[local_service].stop_requested.load(
                        std::memory_order_acquire)) {
                    if (first == Status::ok) {
                        first = Status::invalid_state;
                    }
                    continue;
                }
                bool backend_pending = false;
                for (std::size_t backend = 0;
                     backend < extension->backend_count; ++backend) {
                    if (extension->service_backend_relationships
                            [local_service][backend] &&
                        !extension->backends[backend].released.load(
                            std::memory_order_acquire)) {
                        backend_pending = true;
                        break;
                    }
                }
                if (backend_pending) {
                    if (first == Status::ok) {
                        first = Status::invalid_state;
                    }
                    continue;
                }
                extension_control_callback_active.store(
                    true, std::memory_order_release);
                const auto quiesce =
                    extension->services[local_service].call_quiesce();
                Status shutdown = Status::ok;
                if (quiesce == Status::ok) {
                    shutdown = extension->services[local_service]
                        .call_shutdown();
                }
                extension_control_callback_active.store(
                    false, std::memory_order_release);
                if (first == Status::ok && quiesce != Status::ok) {
                    first = quiesce;
                }
                if (first == Status::ok && shutdown != Status::ok) {
                    first = shutdown;
                }
            }
            if (extension->callbacks_quiescent() &&
                extension->backends_released() &&
                extension->services_released()) {
                extension->state.store(
                    ExtensionLifecycleState::quiescent,
                    std::memory_order_release);
            } else {
                extension->state.store(
                    ExtensionLifecycleState::cleanup_pending,
                    std::memory_order_release);
                if (first == Status::ok) {
                    first = Status::invalid_state;
                }
            }
        }
        return first;
    }

    [[nodiscard]] std::uint64_t compute_graph_id() const noexcept {
        std::uint64_t hash = kFnvOffset;
        hash_u64(hash, 1);
        hash_u64(hash, callbacks.size());
        for (const auto& callback : callbacks) {
            hash_string(hash, callback.name);
            hash_u64(
                hash,
                static_cast<std::uint64_t>(callback.kind));
            if (callback.kind == PhaseKind::device ||
                callback.kind == PhaseKind::device_batch) {
                hash_u64(hash, callback.device_backend_index);
            }
            if (callback.kind == PhaseKind::device_batch) {
                hash_u64(hash, 0x4d31372d62617433ull);
                const auto& declaration = callback.batch_declaration;
                hash_u64(hash, declaration.command_count);
                hash_u64(hash, declaration.wait_count);
                hash_u64(hash, declaration.signal_count);
                const auto hash_reference = [&](const HalV2BufferReference& ref) {
                    hash_u64(hash, ref.buffer_token == 0
                        ? 0
                        : DeviceBufferHandle{ref.buffer_token}.index() + 1u);
                    hash_u64(hash, ref.access);
                    hash_u64(hash, ref.offset);
                    hash_u64(hash, ref.bytes);
                };
                for (std::size_t index = 0;
                     index < declaration.command_count; ++index) {
                    const auto& command = declaration.commands[index];
                    hash_u64(hash, command.kind);
                    hash_u64(hash, command.operation);
                    hash_u64(hash, command.opcode);
                    hash_u64(hash, command.flags);
                    hash_u64(hash, command.buffer_count);
                    for (std::size_t ref = 0; ref < command.buffer_count; ++ref) {
                        hash_reference(command.buffers[ref]);
                    }
                    hash_reference(command.source);
                    hash_reference(command.destination);
                    hash_reference(command.target);
                }
                for (std::size_t index = 0; index < declaration.wait_count;
                     ++index) {
                    hash_u64(hash, DeviceTimelineHandle{
                        declaration.waits[index].timeline_handle}.index());
                }
                for (std::size_t index = 0; index < declaration.signal_count;
                     ++index) {
                    hash_u64(hash, DeviceTimelineHandle{
                        declaration.signals[index].timeline_handle}.index());
                }
            }
        }
        hash_u64(hash, device_backends.size());
        for (const auto& backend : device_backends) {
            hash_string(hash, backend.name);
            hash_string(
                hash,
                std::string_view(
                    backend.capabilities.backend_id.data(),
                    std::char_traits<char>::length(
                        backend.capabilities.backend_id.data())));
            if (backend.kind == detail::HalBackendKind::native_hal_v2) {
                hash_u64(hash, 0x4d31372d68616c32ull);
                hash_u64(
                    hash,
                    static_cast<std::uint64_t>(backend.kind));
                hash_u64(hash, hal_v2_api_version);
            }
            if (backend.memory_state &&
                backend.memory_state->native_extension) {
              hash_u64(hash, 0x4d31372d6d656d32ull);
              const auto &snapshot = backend.memory_state->snapshot;
              hash_u64(hash, snapshot.memory_domain_count);
              for (std::size_t index = 0; index < snapshot.memory_domain_count;
                   ++index) {
                const auto &domain = snapshot.memory_domains[index];
                hash_u64(hash, domain.identity);
                hash_u64(hash, domain.kind);
                hash_u64(hash, domain.ownership_modes);
                hash_u64(hash, domain.maximum_bytes);
                hash_u64(hash, domain.byte_granularity);
                hash_u64(hash, domain.alignment);
                hash_u64(hash, domain.offset_granularity);
                hash_u64(hash, domain.access);
                hash_u64(hash, domain.coherency);
                hash_u64(hash, domain.required_synchronization);
                hash_u64(hash, domain.topology_node_identity);
                hash_u64(hash, domain.timestamp_domain_identity);
              }
              hash_u64(hash, snapshot.topology_node_count);
              for (std::size_t index = 0; index < snapshot.topology_node_count;
                   ++index) {
                const auto &node = snapshot.topology_nodes[index];
                hash_u64(hash, node.identity);
                hash_u64(hash, node.kind);
              }
              hash_u64(hash, snapshot.topology_link_count);
              for (std::size_t index = 0; index < snapshot.topology_link_count;
                   ++index) {
                const auto &link = snapshot.topology_links[index];
                hash_u64(hash, link.identity);
                hash_u64(hash, link.source_node_identity);
                hash_u64(hash, link.destination_node_identity);
                hash_u64(hash, link.kind);
              }
              hash_u64(hash, snapshot.timestamp_domain_count);
              for (std::size_t index = 0;
                   index < snapshot.timestamp_domain_count; ++index) {
                const auto &domain = snapshot.timestamp_domains[index];
                hash_u64(hash, domain.identity);
                hash_u64(hash, domain.kind);
                hash_u64(hash, domain.tick_numerator_ns);
                hash_u64(hash, domain.tick_denominator);
                hash_u64(hash, domain.wrap_ticks);
                hash_u64(hash, domain.correlation_destination_identity);
                hash_u64(hash, domain.monotonic);
                hash_u64(hash, domain.resets_on_backend_reset);
                hash_u64(hash, domain.supports_correlation);
              }
              hash_u64(hash, snapshot.completion_timestamp_domain_identity);
            }
            if (backend.command_state) {
              hash_u64(hash, 0x4d31372d636d6433ull);
              const auto& command = backend.command_state->capabilities;
              hash_u64(hash, command.max_in_flight_batches);
              hash_u64(hash, command.max_commands_per_batch);
              hash_u64(hash, command.max_wait_points);
              hash_u64(hash, command.max_signal_points);
              hash_u64(hash, command.max_timelines);
              hash_u64(hash, command.completion_batch_capacity);
              hash_u64(hash, command.backend_control_storage_bytes);
            }
        }
        if (!device_timelines.empty()) {
            hash_u64(hash, 0x4d31372d746c6e33ull);
            hash_u64(hash, device_timelines.size());
            for (const auto& timeline : device_timelines) {
                hash_string(hash, std::string_view(
                    timeline.name.data(),
                    std::char_traits<char>::length(timeline.name.data())));
                hash_u64(hash, timeline.backend_index);
                hash_u64(hash, timeline.initial_value);
            }
        }
        hash_u64(hash, device_buffers.size());
        for (const auto& buffer : device_buffers) {
            hash_string(
                hash,
                std::string_view(
                    buffer.name.data(),
                    std::char_traits<char>::length(
                        buffer.name.data())));
            hash_u64(hash, buffer.backend_index);
            if (!buffer.heterogeneous) {
              hash_u64(hash, buffer.storage.size());
              hash_u64(hash, buffer.flags);
            } else {
              hash_u64(hash, 0x4d31372d6f626a32ull);
              hash_u64(hash, buffer.bytes);
              hash_u64(hash, buffer.domain_identity);
              hash_u64(hash, buffer.ownership);
              hash_u64(hash, buffer.flags);
              hash_u64(hash, buffer.coherency);
              hash_u64(hash, buffer.synchronization);
              hash_u64(hash, buffer.opaque_handle.size);
              hash_bytes(hash, std::span<const std::byte>(
                                   buffer.opaque_handle.bytes.data(),
                                   buffer.opaque_handle.size));
            }
        }
        hash_u64(hash, resources.size());
        for (const auto& resource : resources) {
            hash_string(hash, resource.name);
        }
        hash_u64(hash, dependencies.size());
        for (const auto& dependency : dependencies) {
            hash_u64(hash, dependency.prerequisite.index());
            hash_u64(hash, dependency.dependent.index());
        }
        hash_u64(hash, resource_accesses.size());
        for (const auto& access : resource_accesses) {
            hash_u64(hash, access.phase.index());
            hash_u64(hash, access.resource.index());
            hash_u64(
                hash,
                static_cast<std::uint64_t>(access.access));
        }
        if (!rate_domains.empty()) {
            // Conditional marker preserves the exact pre-M16 no-plan hash.
            hash_u64(hash, 0x4d31362d72617465ull);
            hash_u64(hash, rate_domains.size());
            for (const auto& domain : rate_domains) {
                hash_string(hash, domain.name);
                hash_u64(hash, domain.period_ns);
                hash_u64(hash, domain.substep_count);
                hash_u64(hash, domain.relative_deadline_ns);
                hash_u64(hash, domain.budget_wcet_ns);
                hash_u64(
                    hash,
                    static_cast<std::uint64_t>(domain.criticality));
                hash_u64(hash, domain.optional ? 1u : 0u);
            }
            hash_u64(hash, compiled_rate_plan.bindings.size());
            for (const auto& binding : compiled_rate_plan.bindings) {
                hash_u64(hash, binding.phase.index());
                hash_u64(hash, binding.domain.index());
                hash_u64(
                    hash,
                    static_cast<std::uint64_t>(binding.phase_kind));
            }
        }
        if (!cross_rate_channels.empty()) {
            // Conditional marker preserves both the legacy no-rate identity
            // and the exact M16-01 explicit-rate identity when no channel is
            // declared.
            hash_u64(hash, 0x4d31362d78726174ull);
            hash_u64(hash, cross_rate_channels.size());
            for (const auto& channel : cross_rate_channels) {
                hash_string(hash, channel.name);
                hash_u64(hash, channel.producer.index());
                hash_u64(hash, channel.consumer.index());
                hash_u64(hash, channel.payload_size);
                hash_u64(
                    hash,
                    static_cast<std::uint64_t>(channel.mode));
                hash_u64(hash, channel.maximum_age_ns);
                hash_u64(
                    hash,
                    channel.producer_device.payload_reference_ordinal);
                hash_u64(
                    hash,
                    channel.producer_device.slot_stride_bytes);
                hash_u64(
                    hash,
                    channel.consumer_device.payload_reference_ordinal);
                hash_u64(
                    hash,
                    channel.consumer_device.slot_stride_bytes);
                hash_bytes(
                    hash,
                    std::span<const std::byte>(
                        channel.initial_sample.data(),
                        channel.initial_sample.size()));
            }
            hash_u64(
                hash, compiled_cross_rate_plan.device_endpoints.size());
            for (const auto& endpoint :
                 compiled_cross_rate_plan.device_endpoints) {
                hash_u64(hash, endpoint.channel_index);
                hash_u64(hash, endpoint.producer ? 1u : 0u);
                hash_u64(hash, endpoint.phase.index());
                hash_u64(hash, endpoint.backend.index());
                hash_u64(hash, endpoint.buffer.index());
                hash_u64(
                    hash, static_cast<std::uint64_t>(endpoint.reference_kind));
                hash_u64(hash, endpoint.command_index);
                hash_u64(hash, endpoint.command_reference_index);
                hash_u64(hash, endpoint.envelope_offset);
                hash_u64(hash, endpoint.envelope_bytes);
                hash_u64(hash, endpoint.slot_stride_bytes);
                hash_u64(hash, endpoint.slot_count);
                hash_u64(hash, endpoint.timestamp_domain_identity);
            }
        }
        if (!sampled_io_channels.empty()) {
            hash_u64(hash, 0x4d32312d73696f34ull);
            hash_u64(hash, sampled_io_channels.size());
            std::uint64_t previous_identity = 0;
            for (std::size_t ordinal = 0;
                 ordinal < sampled_io_channels.size(); ++ordinal) {
                const detail::SampledIoChannelSpec* selected = nullptr;
                for (const auto& candidate : sampled_io_channels) {
                    if (candidate.channel_identity > previous_identity &&
                        (!selected || candidate.channel_identity <
                            selected->channel_identity)) {
                        selected = &candidate;
                    }
                }
                if (!selected) {
                    break;
                }
                previous_identity = selected->channel_identity;
                hash_u64(hash, selected->channel_identity);
                hash_u64(hash, static_cast<std::uint64_t>(
                    selected->direction));
                hash_u64(hash, static_cast<std::uint64_t>(
                    selected->encoding));
                hash_u64(hash, selected->element_count);
                hash_u64(hash, selected->samples_per_frame);
                hash_u64(hash, static_cast<std::uint64_t>(
                    selected->scale_numerator));
                hash_u64(hash, selected->scale_denominator);
                hash_u64(hash, static_cast<std::uint64_t>(
                    selected->offset_numerator));
                hash_u64(hash, selected->offset_denominator);
                hash_u64(hash, selected->units_identity);
                hash_u64(hash, selected->calibration_identity);
                hash_u64(hash, selected->sample_period_ns);
                hash_u64(hash, selected->timestamp_domain_identity);
                hash_u64(hash, selected->clock_domain_identity);
                hash_u64(hash, static_cast<std::uint64_t>(
                    selected->trigger_mode));
                hash_u64(hash, selected->trigger_identity);
                hash_u64(hash, selected->ring_capacity);
                hash_u64(hash, selected->initial_sequence);
                hash_u64(hash, selected->maximum_age_ns);
                hash_u64(hash, static_cast<std::uint64_t>(
                    selected->stale_policy));
                hash_u64(hash, static_cast<std::uint64_t>(
                    selected->overrun_policy));
                hash_u64(hash, static_cast<std::uint64_t>(
                    selected->underrun_policy));
                hash_u64(hash, selected->safe_transition_timeout_ns);
                hash_bytes(hash, selected->initial_frame);
                hash_bytes(hash, selected->startup_safe_frame);
                hash_bytes(hash, selected->failure_safe_frame);
                hash_bytes(hash, selected->shutdown_safe_frame);
            }
        }
        if (!compiled_device_rate_plan.phases.empty()) {
            hash_u64(hash, 0x4d32312d64726174ull);
            hash_u64(hash, compiled_device_rate_plan.phases.size());
            for (const auto& phase : compiled_device_rate_plan.phases) {
                hash_u64(hash, phase.phase.index());
                hash_u64(hash, phase.domain.index());
                hash_u64(hash, phase.completion_budget_ns);
                hash_u64(hash, phase.maximum_in_flight);
                hash_u64(hash, phase.payload_reference_count);
                const auto end = phase.first_payload_reference_index +
                    phase.payload_reference_count;
                for (auto index = phase.first_payload_reference_index;
                     index < end; ++index) {
                    hash_u64(hash, static_cast<std::uint64_t>(
                        compiled_device_rate_plan
                            .payload_references[index].role));
                }
            }
        }
        if (rate_execution_policy_set) {
            // Conditional marker preserves exact M16-01/M16-02 identities for
            // reference-only plans.
            hash_u64(hash, 0x4d31362d64697370ull);
            hash_u64(
                hash,
                rate_execution_policy.maximum_dispatch_records_per_step);
            for (const auto& domain : rate_domains) {
                hash_u64(
                    hash,
                    static_cast<std::uint64_t>(domain.late_action));
                hash_u64(hash, domain.bounded_catch_up_limit);
            }
            if (!compiled_rate_dispatch_plan.optional_shed_order.empty()) {
                hash_u64(hash, 0x4d31362d73686564ull);
                hash_u64(hash, rate_execution_policy.host_policy_version);
                hash_u64(
                    hash,
                    rate_execution_policy.consecutive_late_threshold);
                hash_u64(
                    hash,
                    rate_execution_policy.consecutive_on_time_threshold);
                hash_u64(
                    hash,
                    compiled_rate_dispatch_plan.optional_shed_order.size());
                for (const auto domain_index :
                     compiled_rate_dispatch_plan.optional_shed_order) {
                    hash_u64(hash, domain_index);
                }
            }
        }
        if (mixed_rate_closure_policy_set) {
            hash_u64(hash, 0x4d32312d636c7331ull);
            hash_u64(hash, mixed_rate_action_schema_version);
            hash_u64(hash, active_replay_schema_version);
            hash_u64(hash, mixed_rate_closure_policy.host_policy_version);
            hash_u64(hash, static_cast<std::uint64_t>(
                mixed_rate_closure_policy.overflow_policy));
            hash_u64(hash, mixed_rate_closure_policy.active_replay_enabled);
            hash_u64(
                hash,
                mixed_rate_closure_policy.require_deterministic_backends);
            hash_u64(hash, mixed_rate_closure_policy.maximum_actions_per_step);
        }
        if (live_control_policy_set) {
            hash_u64(hash, 0x4d32322d6d626f78ull);
            hash_u64(hash, live_control_schema_version);
            hash_u64(hash, sizeof(LiveControlPolicy));
            hash_u64(hash, sizeof(LiveControlMailboxRegistration));
            hash_u64(hash, sizeof(LiveControlProducerRegistration));
            hash_u64(hash, sizeof(LiveControlProducerHandle));
            hash_u64(hash, sizeof(LiveControlUpdateRecord));
            hash_u64(hash, sizeof(LiveControlMailboxInfo));
            hash_u64(hash, 0x4d32322d636d7432ull);
            hash_u64(hash, sizeof(LiveControlBoundaryTarget));
            hash_u64(hash, sizeof(LiveControlRecordView));
            hash_u64(hash, sizeof(LiveControlGenerationView));
            hash_u64(hash, sizeof(LiveControlCommitInfo));
            hash_u64(hash, sizeof(LiveControlRecordStatusInfo));
            hash_u64(hash, static_cast<std::uint64_t>(
                LiveControlRecordStatus::committed));
            hash_u64(hash, static_cast<std::uint64_t>(
                LiveControlRecordStatus::replaced));
            hash_u64(hash, static_cast<std::uint64_t>(
                LiveControlRecordStatus::missed));
            hash_u64(hash, static_cast<std::uint64_t>(
                LiveControlRecordStatus::stopped));
            // Frozen M22-02 semantics: exact-target close, mailbox/sequence
            // order, mailbox+kind replacement, reject-late, terminal reuse,
            // and callback-lifetime immutable views.
            hash_u64(hash, 0x010101010101ull);
            hash_u64(hash, alignof(LiveControlPolicy));
            hash_u64(hash, alignof(LiveControlMailboxRegistration));
            hash_u64(hash, alignof(LiveControlProducerRegistration));
            hash_u64(hash, alignof(LiveControlProducerHandle));
            hash_u64(hash, alignof(LiveControlUpdateRecord));
            hash_u64(hash, alignof(LiveControlMailboxInfo));
            hash_u64(hash, live_control_payload_canonical_little_endian);
            hash_u64(hash, live_control_mailbox_capacity_limit);
            hash_u64(hash, live_control_producer_capacity_limit);
            hash_u64(hash, live_control_record_capacity_limit);
            hash_u64(hash, live_control_payload_bytes_limit);
            hash_u64(hash, live_control_payload_alignment_limit);
            hash_u64(hash, live_control_total_storage_limit);
            hash_u64(hash, live_control_policy.policy_identity);
            hash_u64(hash, live_control_policy.mailbox_capacity);
            hash_u64(hash, live_control_policy.producer_capacity);
            hash_u64(hash, live_control_policy.record_capacity);
            hash_u64(hash, live_control_policy.payload_bytes_per_record);
            hash_u64(hash, live_control_policy.total_payload_storage_bytes);
            hash_u64(hash, static_cast<std::uint64_t>(
                live_control_policy.admission_policy));
            hash_u64(hash, static_cast<std::uint64_t>(
                live_control_policy.reset_rule));
            hash_u64(hash, static_cast<std::uint64_t>(
                LiveControlTargetKind::host_frame));
            hash_u64(hash, static_cast<std::uint64_t>(
                LiveControlTargetKind::rate_release));
            hash_u64(hash, static_cast<std::uint64_t>(
                LiveControlUpdateKind::scenario_parameters));
            hash_u64(hash, static_cast<std::uint64_t>(
                LiveControlUpdateKind::controller_parameters));
            hash_u64(hash, static_cast<std::uint64_t>(
                LiveControlUpdateKind::sensor_calibration));
            hash_u64(hash, static_cast<std::uint64_t>(
                LiveControlUpdateKind::fault_configuration));
            hash_u64(hash, static_cast<std::uint64_t>(
                LiveControlUpdateKind::clear_fault));
            hash_u64(hash, live_control_mailbox_declarations.size());
            std::uint64_t previous_mailbox = 0;
            for (std::size_t emitted = 0;
                 emitted < live_control_mailbox_declarations.size();
                 ++emitted) {
                const LiveControlMailboxRegistration* selected = nullptr;
                for (const auto& candidate :
                     live_control_mailbox_declarations) {
                    if (candidate.mailbox_identity > previous_mailbox &&
                        (!selected || candidate.mailbox_identity <
                            selected->mailbox_identity)) {
                        selected = &candidate;
                    }
                }
                if (!selected) {
                    break;
                }
                hash_u64(hash, selected->mailbox_identity);
                hash_u64(hash, selected->record_capacity);
                hash_u64(hash, selected->payload_bytes_per_record);
                previous_mailbox = selected->mailbox_identity;
            }
            hash_u64(hash, live_control_producer_declarations.size());
            std::uint64_t previous_producer = 0;
            for (std::size_t emitted = 0;
                 emitted < live_control_producer_declarations.size();
                 ++emitted) {
                const LiveControlProducerRegistration* selected = nullptr;
                for (const auto& candidate :
                     live_control_producer_declarations) {
                    if (candidate.producer_identity > previous_producer &&
                        (!selected || candidate.producer_identity <
                            selected->producer_identity)) {
                        selected = &candidate;
                    }
                }
                if (!selected) {
                    break;
                }
                hash_u64(hash, selected->mailbox_identity);
                hash_u64(hash, selected->producer_identity);
                hash_u64(hash, selected->first_sequence);
                previous_producer = selected->producer_identity;
            }
            if (live_control_closure_policy_set) {
                hash_u64(hash, 0x4d32322d636c7333ull);
                hash_u64(hash, live_control_action_schema_version);
                hash_u64(hash, live_control_replay_schema_version);
                hash_u64(hash, sizeof(LiveControlClosurePolicy));
                hash_u64(hash, sizeof(LiveControlActionRecord));
                hash_u64(hash, static_cast<std::uint64_t>(
                    live_control_closure_policy.rollback_rule));
                hash_u64(hash, live_control_closure_policy.policy_identity);
                hash_u64(hash, static_cast<std::uint64_t>(
                    LiveControlActionId::admission));
                hash_u64(hash, static_cast<std::uint64_t>(
                    LiveControlActionId::replay_verified));
                hash_u64(hash, static_cast<std::uint64_t>(
                    LiveControlActionReason::normal));
                hash_u64(hash, static_cast<std::uint64_t>(
                    LiveControlActionReason::replay));
                hash_u64(hash, static_cast<std::uint64_t>(
                    LiveControlActionResult::accepted));
                hash_u64(hash, static_cast<std::uint64_t>(
                    LiveControlActionResult::verified));
                hash_u64(hash,
                    live_control_closure_policy.replay_enabled ? 1u : 0u);
                if (live_control_closure_policy.replay_enabled) {
                    hash_u64(hash,
                        live_control_closure_policy.retained_generation_capacity);
                    hash_u64(hash,
                        live_control_closure_policy.retained_record_capacity);
                    hash_u64(hash,
                        live_control_closure_policy.retained_payload_bytes);
                    hash_u64(hash,
                        live_control_closure_policy.replay_record_capacity);
                    hash_u64(hash,
                        live_control_closure_policy.replay_max_bytes);
                    if (live_control_retention_policy.policy_identity != 0) {
                        hash_u64(hash, live_control_lossless_replay_schema_version);
                        hash_u64(hash, live_control_retention_policy.policy_identity);
                        hash_u64(hash,
                                 live_control_retention_policy.admission_capacity);
                        hash_u64(hash,
                                 live_control_retention_policy.payload_capacity_bytes);
                    }
                }
            }
        }
        if (!extensions.empty()) {
            hash_u64(hash, 0x4d31392d65787431ull);
            hash_u64(hash, extensions.size());
            for (const auto& extension_ptr : extensions) {
                const auto& extension = *extension_ptr;
                hash_string(hash, identifier_view(extension.name));
                hash_string(hash, identifier_view(extension.version));
                hash_u64(hash, extension.negotiated_abi_version);
                hash_u64(hash, extension.phase_count);
                hash_u64(hash, extension.backend_count);
                hash_u64(hash, extension.service_count);
                hash_u64(hash, extension.resource_count);
                hash_u64(hash, extension.relationship_count);
                for (std::size_t index = 0;
                     index < extension.service_count; ++index) {
                    const auto& service =
                        extension.service_descriptors[index];
                    hash_string(hash, std::string_view(service.name));
                    hash_string(
                        hash, std::string_view(service.interface_name));
                    hash_u64(hash, service.interface_version);
                }
                for (std::size_t index = 0;
                     index < extension.relationship_count; ++index) {
                    const auto& relation = extension.relationships[index];
                    hash_u64(hash, relation.kind);
                    hash_u64(hash, relation.access);
                    hash_u64(hash, relation.first.kind);
                    hash_u64(hash, relation.first.slot);
                    hash_u64(hash, relation.second.kind);
                    hash_u64(hash, relation.second.slot);
                }
            }
        }
        return hash;
    }

    [[nodiscard]] std::uint64_t
    compute_state_schema_id() const noexcept {
        std::uint64_t hash = kFnvOffset;
        hash_u64(hash, checkpoint_schema_version);
        hash_u64(hash, checkpoint_state_count());
        for (const auto& registered_state : states) {
            hash_string(hash, identifier_view(registered_state.name));
            hash_u64(hash, registered_state.schema_version);
            hash_u64(hash, registered_state.storage.size());
        }
        if (rate_execution_policy_set) {
            hash_string(hash, kRateDispatchStateName);
            hash_u64(hash, kRateDispatchStateSchema);
            hash_u64(hash, active_checkpoint_state.size());
        }
        if (mixed_rate_closure_policy_set) {
            hash_string(hash, kMixedRateStateName);
            hash_u64(hash, kMixedRateStateSchema);
            hash_u64(hash, mixed_rate_checkpoint_state.size());
        }
        if (live_control_closure_policy_set) {
            hash_string(hash, kLiveControlStateName);
            hash_u64(hash, kLiveControlStateSchema);
            hash_u64(hash, live_control_mailboxes
                ? live_control_mailboxes->checkpoint_state_size()
                : 0);
        }
        return hash;
    }

    [[nodiscard]] std::uint64_t compute_config_id() const noexcept {
        const auto base = config_identifier(config);
        if (extensions.empty() && !live_control_policy_set) {
            return base;
        }
        std::uint64_t hash = kFnvOffset;
        hash_u64(hash, base);
        if (!extensions.empty()) {
            hash_u64(hash, 0x4d31392d63666731ull);
            for (const auto& extension_ptr : extensions) {
                const auto& extension = *extension_ptr;
                hash_string(hash, identifier_view(extension.name));
                hash_string(hash, identifier_view(extension.version));
                hash_u64(hash, extension.negotiated_abi_version);
                for (std::size_t index = 0;
                     index < extension.service_count; ++index) {
                    const auto& service = extension.service_descriptors[index];
                    hash_string(hash, std::string_view(service.name));
                    hash_string(hash, std::string_view(service.interface_name));
                    hash_u64(hash, service.interface_version);
                }
            }
        }
        if (live_control_policy_set) {
            hash_u64(hash, 0x4d32322d63666731ull);
            hash_u64(hash, live_control_policy.policy_identity);
            hash_u64(hash, compute_graph_id());
        }
        return hash;
    }

    [[nodiscard]] std::uint64_t compute_replay_id(
        std::uint64_t resolved_graph_id,
        std::uint64_t resolved_state_schema_id) const noexcept {
        std::uint64_t hash = kFnvOffset;
        hash_u64(hash, checkpoint_schema_version);
        hash_u64(
            hash,
            static_cast<std::uint64_t>(
                config.determinism_tier));
        if (config.determinism_tier ==
            DeterminismTier::unspecified) {
            hash_u64(hash, config_identifier(config));
        } else {
            // Operational capacities and worker_count are deliberately
            // excluded from D1 compatibility. Semantic callback-visible
            // choices remain part of the identity.
            hash_u64(
                hash,
                static_cast<std::uint64_t>(
                    config.numerical_mode));
            hash_u64(hash, config.scratch_bytes);
            hash_u64(hash, config.scratch_alignment);
            hash_u64(hash, config.task_scratch_bytes);
            hash_u64(
                hash,
                static_cast<std::uint64_t>(
                    config.overload_policy));
        }
        hash_u64(hash, resolved_graph_id);
        hash_u64(hash, resolved_state_schema_id);
        hash_string(hash, identifier_view(config.workload_id));
        return hash;
    }

    [[nodiscard]] std::size_t checkpoint_state_count() const noexcept {
        return states.size() + (rate_execution_policy_set ? 1u : 0u) +
            (mixed_rate_closure_policy_set ? 1u : 0u) +
            (live_control_closure_policy_set ? 1u : 0u);
    }

    void sync_active_checkpoint_state() noexcept {
        if (!rate_execution_policy_set || active_checkpoint_state.empty()) {
            return;
        }
        std::fill(
            active_checkpoint_state.begin(),
            active_checkpoint_state.end(),
            std::byte{0});
        auto bytes = std::span<std::byte>(active_checkpoint_state);
        std::uint32_t flags = 0;
        flags |= active_epoch_mapped ? 1u : 0u;
        flags |= active_faulted ? 2u : 0u;
        (void)store_u64_le(bytes, 0, kRateDispatchStateMagic);
        (void)store_u32_le(bytes, 8, kRateDispatchStateSchema);
        (void)store_u32_le(bytes, 12, flags);
        (void)store_u64_le(bytes, 16, active_logical_cursor_ns);
        (void)store_u64_le(bytes, 24, active_nominal_epoch_ns);
        (void)store_u32_le(
            bytes,
            32,
            degradation_level.load(std::memory_order_acquire));
        (void)store_u32_le(
            bytes,
            36,
            static_cast<std::uint32_t>(compiled_rate_plan.domains.size()));
        (void)store_u32_le(
            bytes,
            40,
            static_cast<std::uint32_t>(active_channel_states.size()));
        (void)store_u64_le(
            bytes,
            48,
            active_faulted
                ? active_fault_domain.index()
                : std::numeric_limits<std::uint64_t>::max());
        (void)store_u64_le(bytes, 56, active_fault_sequence);
        (void)store_u32_le(bytes, 64, active_fault_substep);
        (void)store_u64_le(
            bytes,
            72,
            active_committed_payloads.size());
        (void)store_u64_le(
            bytes,
            80,
            active_checkpoint_state.size());
        for (std::size_t index = 0;
             index < active_channel_states.size();
             ++index) {
            const auto& channel_state = active_channel_states[index];
            const auto& channel = cross_rate_channels[index];
            const auto offset = kRateDispatchStateHeaderBytes +
                index * kRateDispatchChannelStateBytes;
            (void)store_u64_le(bytes, offset, channel_state.generation);
            (void)store_u64_le(
                bytes,
                offset + 8,
                channel_state.next_generation);
            (void)store_u64_le(
                bytes,
                offset + 16,
                channel_state.source_logical_release_ns);
            (void)store_u64_le(
                bytes,
                offset + 24,
                channel_state.payload_offset);
            (void)store_u64_le(bytes, offset + 32, channel.payload_size);
            bytes[offset + 40] =
                static_cast<std::byte>(channel_state.provenance);
            bytes[offset + 41] =
                static_cast<std::byte>(channel_state.held ? 1 : 0);
        }
        const auto payload_offset = kRateDispatchStateHeaderBytes +
            active_channel_states.size() * kRateDispatchChannelStateBytes;
        std::copy(
            active_committed_payloads.begin(),
            active_committed_payloads.end(),
            active_checkpoint_state.begin() +
                static_cast<std::ptrdiff_t>(payload_offset));
        if (!compiled_rate_dispatch_plan.optional_shed_order.empty()) {
            const auto optional_offset =
                payload_offset + active_committed_payloads.size();
            (void)store_u64_le(bytes, optional_offset, kRateOptionalStateMagic);
            (void)store_u64_le(
                bytes,
                optional_offset + 8,
                rate_execution_policy.host_policy_version);
            (void)store_u32_le(
                bytes,
                optional_offset + 16,
                rate_execution_policy.consecutive_late_threshold);
            (void)store_u32_le(
                bytes,
                optional_offset + 20,
                rate_execution_policy.consecutive_on_time_threshold);
            (void)store_u32_le(
                bytes, optional_offset + 24,
                active_shedding_state->consecutive_late);
            (void)store_u32_le(
                bytes, optional_offset + 28,
                active_shedding_state->consecutive_on_time);
            (void)store_u64_le(
                bytes, optional_offset + 32,
                active_shedding_state->shed_mask);
            (void)store_u32_le(
                bytes,
                optional_offset + 40,
                static_cast<std::uint32_t>(
                    compiled_rate_dispatch_plan.optional_shed_order.size()));
            (void)store_u32_le(
                bytes,
                optional_offset + 44,
                static_cast<std::uint32_t>(
                    std::popcount(active_shedding_state->shed_mask)));
            for (std::size_t index = 0;
                 index < compiled_rate_dispatch_plan.optional_shed_order.size();
                 ++index) {
                bytes[optional_offset + kRateOptionalStateHeaderBytes + index] =
                    static_cast<std::byte>(
                        compiled_rate_dispatch_plan.optional_shed_order[index]);
            }
        }
    }

    [[nodiscard]] bool validate_active_checkpoint_state(
        std::span<const std::byte> bytes) const noexcept {
        if (!rate_execution_policy_set ||
            bytes.size() != active_checkpoint_state.size()) {
            return false;
        }
        std::uint64_t magic = 0;
        std::uint32_t schema = 0;
        std::uint32_t flags = 0;
        std::uint32_t degradation = 0;
        std::uint32_t domain_count = 0;
        std::uint32_t channel_count = 0;
        std::uint64_t logical_cursor = 0;
        std::uint64_t nominal_epoch = 0;
        std::uint64_t payload_bytes = 0;
        std::uint64_t total_bytes = 0;
        std::uint64_t fault_domain = 0;
        std::uint64_t fault_sequence = 0;
        std::uint32_t fault_substep = 0;
        if (!load_u64_le(bytes, 0, magic) ||
            !load_u32_le(bytes, 8, schema) ||
            !load_u32_le(bytes, 12, flags) ||
            !load_u64_le(bytes, 16, logical_cursor) ||
            !load_u64_le(bytes, 24, nominal_epoch) ||
            !load_u32_le(bytes, 32, degradation) ||
            !load_u32_le(bytes, 36, domain_count) ||
            !load_u32_le(bytes, 40, channel_count) ||
            !load_u64_le(bytes, 48, fault_domain) ||
            !load_u64_le(bytes, 56, fault_sequence) ||
            !load_u32_le(bytes, 64, fault_substep) ||
            !load_u64_le(bytes, 72, payload_bytes) ||
            !load_u64_le(bytes, 80, total_bytes) ||
            magic != kRateDispatchStateMagic ||
            schema != kRateDispatchStateSchema || (flags & ~3u) != 0 ||
            degradation > config.watchdog_max_degradation_level ||
            domain_count != compiled_rate_plan.domains.size() ||
            channel_count != active_channel_states.size() ||
            payload_bytes != active_committed_payloads.size() ||
            total_bytes != bytes.size() ||
            (((flags & 2u) != 0) !=
             (fault_domain < compiled_rate_plan.domains.size())) ||
            ((flags & 1u) == 0 &&
             (logical_cursor != 0 || nominal_epoch != 0 ||
              (flags & 2u) != 0))) {
            return false;
        }
        std::uint64_t nominal_cursor = 0;
        if ((flags & 1u) != 0 &&
            !checked_time_add(
                nominal_epoch,
                logical_cursor,
                nominal_cursor)) {
            return false;
        }
        (void)nominal_cursor;
        if ((flags & 2u) == 0) {
            if (fault_domain != std::numeric_limits<std::uint64_t>::max() ||
                fault_sequence != 0 || fault_substep != 0) {
                return false;
            }
        } else {
            const auto& faulted_domain = compiled_rate_plan.domains[
                static_cast<std::size_t>(fault_domain)];
            std::uint64_t fault_release_ns = 0;
            if (fault_substep >= faulted_domain.substep_count ||
                !checked_time_multiply(
                    fault_sequence,
                    faulted_domain.period_ns,
                    fault_release_ns) ||
                fault_release_ns != logical_cursor) {
                return false;
            }
        }
        for (std::size_t offset = 44; offset < 48; ++offset) {
            if (bytes[offset] != std::byte{0}) {
                return false;
            }
        }
        for (std::size_t offset = 68; offset < 72; ++offset) {
            if (bytes[offset] != std::byte{0}) {
                return false;
            }
        }
        for (std::size_t offset = 88;
             offset < kRateDispatchStateHeaderBytes;
             ++offset) {
            if (bytes[offset] != std::byte{0}) {
                return false;
            }
        }
        std::size_t expected_payload_offset = 0;
        for (std::size_t index = 0;
             index < active_channel_states.size();
             ++index) {
            const auto offset = kRateDispatchStateHeaderBytes +
                index * kRateDispatchChannelStateBytes;
            std::uint64_t generation = 0;
            std::uint64_t next_generation = 0;
            std::uint64_t source_logical_release_ns = 0;
            std::uint64_t payload_offset = 0;
            std::uint64_t payload_size = 0;
            if (!load_u64_le(bytes, offset, generation) ||
                !load_u64_le(bytes, offset + 8, next_generation) ||
                !load_u64_le(
                    bytes,
                    offset + 16,
                    source_logical_release_ns) ||
                !load_u64_le(bytes, offset + 24, payload_offset) ||
                !load_u64_le(bytes, offset + 32, payload_size) ||
                generation == 0 ||
                generation > detail::SnapshotStore::maximum_generation() ||
                (generation == detail::SnapshotStore::maximum_generation()
                     ? next_generation != 0
                     : next_generation != generation + 1) ||
                source_logical_release_ns > logical_cursor ||
                payload_offset != expected_payload_offset ||
                payload_size != cross_rate_channels[index].payload_size ||
                (static_cast<std::uint8_t>(bytes[offset + 40]) !=
                     static_cast<std::uint8_t>(
                         CrossRateSampleProvenance::initial_sample) &&
                 static_cast<std::uint8_t>(bytes[offset + 40]) !=
                     static_cast<std::uint8_t>(
                         CrossRateSampleProvenance::produced)) ||
                static_cast<std::uint8_t>(bytes[offset + 41]) > 1) {
                return false;
            }
            const auto provenance =
                static_cast<CrossRateSampleProvenance>(
                    static_cast<std::uint8_t>(bytes[offset + 40]));
            if ((generation == 1) !=
                    (provenance ==
                     CrossRateSampleProvenance::initial_sample) ||
                (provenance ==
                     CrossRateSampleProvenance::initial_sample &&
                 source_logical_release_ns != 0)) {
                return false;
            }
            for (std::size_t reserved = offset + 42;
                 reserved < offset + kRateDispatchChannelStateBytes;
                 ++reserved) {
                if (bytes[reserved] != std::byte{0}) {
                    return false;
                }
            }
            expected_payload_offset += cross_rate_channels[index].payload_size;
        }
        if (expected_payload_offset != active_committed_payloads.size()) {
            return false;
        }
        if (compiled_rate_dispatch_plan.optional_shed_order.empty()) {
            return bytes.size() == kRateDispatchStateHeaderBytes +
                active_channel_states.size() * kRateDispatchChannelStateBytes +
                active_committed_payloads.size();
        }
        const auto optional_offset = kRateDispatchStateHeaderBytes +
            active_channel_states.size() * kRateDispatchChannelStateBytes +
            active_committed_payloads.size();
        std::uint64_t optional_magic = 0;
        std::uint64_t policy_version = 0;
        std::uint32_t late_threshold = 0;
        std::uint32_t on_time_threshold = 0;
        std::uint32_t late_streak = 0;
        std::uint32_t on_time_streak = 0;
        std::uint64_t shed_mask = 0;
        std::uint32_t order_count = 0;
        std::uint32_t shed_count = 0;
        std::uint64_t optional_mask = 0;
        for (const auto index : compiled_rate_dispatch_plan.optional_shed_order) {
            optional_mask |= std::uint64_t{1} << index;
        }
        if (!load_u64_le(bytes, optional_offset, optional_magic) ||
            !load_u64_le(bytes, optional_offset + 8, policy_version) ||
            !load_u32_le(bytes, optional_offset + 16, late_threshold) ||
            !load_u32_le(bytes, optional_offset + 20, on_time_threshold) ||
            !load_u32_le(bytes, optional_offset + 24, late_streak) ||
            !load_u32_le(bytes, optional_offset + 28, on_time_streak) ||
            !load_u64_le(bytes, optional_offset + 32, shed_mask) ||
            !load_u32_le(bytes, optional_offset + 40, order_count) ||
            !load_u32_le(bytes, optional_offset + 44, shed_count) ||
            optional_magic != kRateOptionalStateMagic ||
            policy_version != rate_execution_policy.host_policy_version ||
            late_threshold != rate_execution_policy.consecutive_late_threshold ||
            on_time_threshold != rate_execution_policy.consecutive_on_time_threshold ||
            order_count != compiled_rate_dispatch_plan.optional_shed_order.size() ||
            bytes.size() != optional_offset + kRateOptionalStateHeaderBytes +
                order_count ||
            (shed_mask & ~optional_mask) != 0 ||
            shed_count != static_cast<std::uint32_t>(
                std::popcount(shed_mask)) ||
            late_streak > late_threshold || on_time_streak > on_time_threshold ||
            (late_streak != 0 && on_time_streak != 0)) {
            return false;
        }
        for (std::size_t index = 0; index < order_count; ++index) {
            const auto expected =
                compiled_rate_dispatch_plan.optional_shed_order[index];
            if (static_cast<std::uint8_t>(
                    bytes[optional_offset + kRateOptionalStateHeaderBytes + index]) !=
                    expected) {
                return false;
            }
            if ((shed_mask & (std::uint64_t{1} << expected)) == 0) {
                for (std::size_t later = index + 1; later < order_count; ++later) {
                    if ((shed_mask & (std::uint64_t{1} <<
                         compiled_rate_dispatch_plan.optional_shed_order[later])) != 0) {
                        return false;
                    }
                }
                break;
            }
        }
        return (shed_count == order_count || late_streak < late_threshold) &&
            (shed_count == 0 || on_time_streak < on_time_threshold);
    }

    [[nodiscard]] bool apply_active_checkpoint_state(
        std::span<const std::byte> bytes) noexcept {
        std::uint32_t flags = 0;
        std::uint32_t degradation = 0;
        std::uint64_t fault_domain = 0;
        (void)load_u32_le(bytes, 12, flags);
        (void)load_u64_le(bytes, 16, active_logical_cursor_ns);
        (void)load_u64_le(bytes, 24, active_nominal_epoch_ns);
        (void)load_u32_le(bytes, 32, degradation);
        (void)load_u64_le(bytes, 48, fault_domain);
        (void)load_u64_le(bytes, 56, active_fault_sequence);
        (void)load_u32_le(bytes, 64, active_fault_substep);
        active_epoch_mapped = (flags & 1u) != 0;
        active_faulted = (flags & 2u) != 0;
        active_fault_domain = active_faulted
            ? RateDomainHandle{
                  graph_owner,
                  static_cast<std::uint32_t>(fault_domain)}
            : RateDomainHandle{};
        degradation_level.store(degradation, std::memory_order_release);
        const auto payload_begin = kRateDispatchStateHeaderBytes +
            active_channel_states.size() * kRateDispatchChannelStateBytes;
        const auto payload_end = payload_begin + active_committed_payloads.size();
        std::copy(
            bytes.begin() + static_cast<std::ptrdiff_t>(payload_begin),
            bytes.begin() + static_cast<std::ptrdiff_t>(payload_end),
            active_committed_payloads.begin());
        std::copy(
            active_committed_payloads.begin(),
            active_committed_payloads.end(),
            active_staging_payloads.begin());
        for (std::size_t index = 0;
             index < active_channel_states.size();
             ++index) {
            auto& channel_state = active_channel_states[index];
            const auto& channel = cross_rate_channels[index];
            const auto offset = kRateDispatchStateHeaderBytes +
                index * kRateDispatchChannelStateBytes;
            (void)load_u64_le(bytes, offset, channel_state.generation);
            (void)load_u64_le(
                bytes,
                offset + 8,
                channel_state.next_generation);
            (void)load_u64_le(
                bytes,
                offset + 16,
                channel_state.source_logical_release_ns);
            std::uint64_t payload_offset = 0;
            (void)load_u64_le(bytes, offset + 24, payload_offset);
            channel_state.payload_offset =
                static_cast<std::size_t>(payload_offset);
            channel_state.provenance =
                static_cast<CrossRateSampleProvenance>(
                static_cast<std::uint8_t>(bytes[offset + 40]));
            channel_state.held =
                static_cast<std::uint8_t>(bytes[offset + 41]) != 0;
            channel_state.producer_release_sequence = 0;
            channel_state.producer_substep_ordinal = 0;
            channel_state.producer_completion_status = Status::ok;
            channel_state.producer_timestamp_domain_identity = 0;
            channel_state.producer_timestamp = 0;
            active_publication_claims[index].store(
                0,
                std::memory_order_relaxed);
            if (compiled_cross_rate_plan.stores[index].restore_committed(
                    channel_state.generation,
                    channel_state.next_generation,
                    std::span<const std::byte>(
                        active_committed_payloads.data() +
                            channel_state.payload_offset,
                        channel.payload_size)) !=
                detail::SnapshotStoreResult::ok) {
                return false;
            }
        }
        if (!compiled_rate_dispatch_plan.optional_shed_order.empty()) {
            (void)load_u32_le(
                bytes, payload_end + 24,
                active_shedding_state->consecutive_late);
            (void)load_u32_le(
                bytes, payload_end + 28,
                active_shedding_state->consecutive_on_time);
            (void)load_u64_le(
                bytes, payload_end + 32,
                active_shedding_state->shed_mask);
            rate_counters->set(
                RateCounterId::currently_shed_domains,
                static_cast<std::uint64_t>(
                    std::popcount(active_shedding_state->shed_mask)));
        }
        std::copy(
            bytes.begin(),
            bytes.end(),
            active_checkpoint_state.begin());
        return true;
    }

    void sync_mixed_rate_checkpoint_state() noexcept {
        if (!mixed_rate_closure_policy_set ||
            mixed_rate_checkpoint_state.empty() || !mixed_rate_actions) {
            return;
        }
        std::fill(
            mixed_rate_checkpoint_state.begin(),
            mixed_rate_checkpoint_state.end(),
            std::byte{0});
        auto bytes = std::span<std::byte>(mixed_rate_checkpoint_state);
        mixed_rate_checkpoint_sequence = mixed_rate_actions->next_sequence();
        (void)store_u64_le(bytes, 0, kMixedRateStateMagic);
        (void)store_u32_le(bytes, 8, kMixedRateStateSchema);
        (void)store_u32_le(
            bytes, 12,
            mixed_rate_closure_policy.active_replay_enabled ? 1u : 0u);
        (void)store_u64_le(bytes, 16, bytes.size());
        (void)store_u64_le(
            bytes, 24, mixed_rate_closure_policy.host_policy_version);
        (void)store_u64_le(bytes, 32, mixed_rate_checkpoint_sequence);
        (void)store_u64_le(bytes, 40, active_logical_cursor_ns);
        (void)store_u64_le(bytes, 48, active_nominal_epoch_ns);
        (void)store_u64_le(bytes, 56, replay_id);
        (void)store_u64_le(bytes, 64, graph_id);
        (void)store_u64_le(
            bytes, 72,
            active_shedding_state ? active_shedding_state->shed_mask : 0);
        (void)store_u32_le(
            bytes, 80,
            degradation_level.load(std::memory_order_acquire));
        (void)store_u32_le(
            bytes, 84,
            static_cast<std::uint32_t>(sampled_io_statuses.size()));
        (void)store_u64_le(bytes, 88, mixed_rate_checkpoint_sequence);
        for (std::size_t index = 0; index < sampled_io_statuses.size(); ++index) {
            const auto& status = sampled_io_statuses[index];
            const auto& channel = compiled_sampled_io_plan.channels[index];
            const auto offset = kMixedRateStateHeaderBytes +
                index * kMixedRateSampledStateBytes;
            (void)store_u64_le(
                bytes, offset, channel.public_record.channel_identity);
            (void)store_u64_le(bytes, offset + 8, status.last_sequence);
            (void)store_u64_le(bytes, offset + 16, status.accepted_frames);
            (void)store_u64_le(bytes, offset + 24, status.stale_frames);
            (void)store_u64_le(bytes, offset + 32, status.overruns);
            (void)store_u64_le(bytes, offset + 40, status.underruns);
            (void)store_u64_le(bytes, offset + 48, status.substituted_frames);
            (void)store_u32_le(
                bytes, offset + 56,
                static_cast<std::uint32_t>(status.last_status));
            bytes[offset + 60] = static_cast<std::byte>(status.safety_state);
        }
    }

    [[nodiscard]] bool validate_mixed_rate_checkpoint_state(
        std::span<const std::byte> bytes) const noexcept {
        if (!mixed_rate_closure_policy_set ||
            bytes.size() != mixed_rate_checkpoint_state.size()) {
            return false;
        }
        std::uint64_t magic = 0;
        std::uint32_t schema = 0;
        std::uint32_t flags = 0;
        std::uint64_t total = 0;
        std::uint64_t policy = 0;
        std::uint64_t next_action = 0;
        std::uint64_t logical_cursor = 0;
        std::uint64_t nominal_epoch = 0;
        std::uint64_t artifact_replay_id = 0;
        std::uint64_t artifact_graph_id = 0;
        std::uint64_t shed_mask = 0;
        std::uint32_t degradation = 0;
        std::uint32_t channel_count = 0;
        std::uint64_t replay_position = 0;
        const auto expected_flags =
            mixed_rate_closure_policy.active_replay_enabled ? 1u : 0u;
        if (!load_u64_le(bytes, 0, magic) ||
            !load_u32_le(bytes, 8, schema) ||
            !load_u32_le(bytes, 12, flags) ||
            !load_u64_le(bytes, 16, total) ||
            !load_u64_le(bytes, 24, policy) ||
            !load_u64_le(bytes, 32, next_action) ||
            !load_u64_le(bytes, 40, logical_cursor) ||
            !load_u64_le(bytes, 48, nominal_epoch) ||
            !load_u64_le(bytes, 56, artifact_replay_id) ||
            !load_u64_le(bytes, 64, artifact_graph_id) ||
            !load_u64_le(bytes, 72, shed_mask) ||
            !load_u32_le(bytes, 80, degradation) ||
            !load_u32_le(bytes, 84, channel_count) ||
            !load_u64_le(bytes, 88, replay_position) ||
            magic != kMixedRateStateMagic || schema != kMixedRateStateSchema ||
            flags != expected_flags || total != bytes.size() ||
            policy != mixed_rate_closure_policy.host_policy_version ||
            artifact_replay_id != replay_id || artifact_graph_id != graph_id ||
            channel_count != sampled_io_statuses.size() ||
            replay_position != next_action ||
            next_action == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        (void)logical_cursor;
        (void)nominal_epoch;
        const auto valid_shed_mask = compiled_rate_plan.domains.size() >= 64
            ? std::numeric_limits<std::uint64_t>::max()
            : (std::uint64_t{1} << compiled_rate_plan.domains.size()) - 1;
        if ((shed_mask & ~valid_shed_mask) != 0 ||
            degradation > config.watchdog_max_degradation_level) {
            return false;
        }
        for (std::size_t offset = 96;
             offset < kMixedRateStateHeaderBytes; ++offset) {
            if (bytes[offset] != std::byte{0}) {
                return false;
            }
        }
        for (std::size_t index = 0; index < sampled_io_statuses.size(); ++index) {
            const auto offset = kMixedRateStateHeaderBytes +
                index * kMixedRateSampledStateBytes;
            std::uint64_t channel_identity = 0;
            std::uint64_t last_sequence = 0;
            std::uint32_t last_status = 0;
            if (!load_u64_le(bytes, offset, channel_identity) ||
                !load_u64_le(bytes, offset + 8, last_sequence) ||
                !load_u32_le(bytes, offset + 56, last_status) ||
                channel_identity != compiled_sampled_io_plan.channels[index]
                    .public_record.channel_identity ||
                last_sequence == 0 ||
                (last_status != 0 &&
                 last_status < std::bit_cast<std::uint32_t>(
                     static_cast<std::int32_t>(
                         Status::incompatible_abi))) ||
                static_cast<std::uint8_t>(bytes[offset + 60]) >
                    static_cast<std::uint8_t>(
                        SampledIoSafetyState::shutdown_acknowledged)) {
                return false;
            }
            (void)last_status;
            for (std::size_t reserved = offset + 61;
                 reserved < offset + kMixedRateSampledStateBytes; ++reserved) {
                if (bytes[reserved] != std::byte{0}) {
                    return false;
                }
            }
        }
        return true;
    }

    void apply_mixed_rate_checkpoint_state(
        std::span<const std::byte> bytes) noexcept {
        (void)load_u64_le(bytes, 32, mixed_rate_checkpoint_sequence);
        for (std::size_t index = 0; index < sampled_io_statuses.size(); ++index) {
            auto& status = sampled_io_statuses[index];
            const auto offset = kMixedRateStateHeaderBytes +
                index * kMixedRateSampledStateBytes;
            (void)load_u64_le(bytes, offset + 8, status.last_sequence);
            (void)load_u64_le(bytes, offset + 16, status.accepted_frames);
            (void)load_u64_le(bytes, offset + 24, status.stale_frames);
            (void)load_u64_le(bytes, offset + 32, status.overruns);
            (void)load_u64_le(bytes, offset + 40, status.underruns);
            (void)load_u64_le(bytes, offset + 48, status.substituted_frames);
            std::uint32_t last_status = 0;
            (void)load_u32_le(bytes, offset + 56, last_status);
            status.last_status = static_cast<Status>(
                static_cast<std::int32_t>(last_status));
            status.safety_state = static_cast<SampledIoSafetyState>(
                static_cast<std::uint8_t>(bytes[offset + 60]));
        }
        mixed_rate_actions->restore_sequence(mixed_rate_checkpoint_sequence);
        std::copy(bytes.begin(), bytes.end(), mixed_rate_checkpoint_state.begin());
    }

    static bool provide_state(
        void* context,
        std::size_t index,
        detail::StateWriteView& output) noexcept {
        auto& self = *static_cast<Impl*>(context);
        if (index < self.states.size()) {
            const auto& state = self.states[index];
            output.name = identifier_view(state.name);
            output.schema_version = state.schema_version;
            output.payload = std::as_bytes(state.storage);
            return true;
        }
        if (self.rate_execution_policy_set && index == self.states.size()) {
            self.sync_active_checkpoint_state();
            output.name = kRateDispatchStateName;
            output.schema_version = kRateDispatchStateSchema;
            output.payload = self.active_checkpoint_state;
            return true;
        }
        const auto mixed_index = self.states.size() +
            (self.rate_execution_policy_set ? 1u : 0u);
        if (self.mixed_rate_closure_policy_set && index == mixed_index) {
            self.sync_mixed_rate_checkpoint_state();
            output.name = kMixedRateStateName;
            output.schema_version = kMixedRateStateSchema;
            output.payload = self.mixed_rate_checkpoint_state;
            return true;
        }
        const auto live_control_index = mixed_index +
            (self.mixed_rate_closure_policy_set ? 1u : 0u);
        std::span<const std::byte> live_control_state;
        if (self.live_control_closure_policy_set &&
            index == live_control_index && self.live_control_mailboxes &&
            self.live_control_mailboxes->sync_checkpoint_state(
                live_control_state)) {
            output.name = kLiveControlStateName;
            output.schema_version = kLiveControlStateSchema;
            output.payload = live_control_state;
            return true;
        }
        {
            output = {};
            return false;
        }
    }

    [[nodiscard]] std::uint64_t registered_application_state_hash()
        const noexcept {
        std::uint64_t hash = kFnvOffset;
        const auto append = [&hash](std::span<const std::byte> bytes) {
            for (const auto value : bytes) {
                hash ^= static_cast<std::uint8_t>(value);
                hash *= kFnvPrime;
            }
        };
        for (const auto& registered_state : states) {
            std::array<
                std::byte,
                detail::checkpoint_record_header_size> header{};
            const auto name = identifier_view(registered_state.name);
            std::memcpy(header.data(), name.data(), name.size());
            store_u32_le(header, 64, registered_state.schema_version);
            store_u64_le(header, 72, registered_state.storage.size());
            store_u64_le(
                header,
                80,
                detail::artifact_checksum(
                    std::as_bytes(registered_state.storage)));
            append(header);
            append(std::as_bytes(registered_state.storage));
        }
        return hash;
    }

    [[nodiscard]] std::uint64_t state_hash() noexcept {
        std::uint64_t hash = kFnvOffset;
        const auto append =
            [&hash](std::span<const std::byte> bytes) {
                for (const auto value : bytes) {
                    hash ^= static_cast<std::uint8_t>(value);
                    hash *= kFnvPrime;
                }
            };
        for (const auto& registered_state : states) {
            std::array<
                std::byte,
                detail::checkpoint_record_header_size> header{};
            const auto name = identifier_view(registered_state.name);
            std::memcpy(
                header.data(),
                name.data(),
                name.size());
            store_u32_le(
                header,
                64,
                registered_state.schema_version);
            store_u64_le(
                header,
                72,
                registered_state.storage.size());
            store_u64_le(
                header,
                80,
                detail::artifact_checksum(
                    std::as_bytes(registered_state.storage)));
            append(header);
            append(std::as_bytes(registered_state.storage));
        }
        if (rate_execution_policy_set) {
            sync_active_checkpoint_state();
            std::array<
                std::byte,
                detail::checkpoint_record_header_size> header{};
            std::memcpy(
                header.data(),
                kRateDispatchStateName.data(),
                kRateDispatchStateName.size());
            store_u32_le(header, 64, kRateDispatchStateSchema);
            store_u64_le(header, 72, active_checkpoint_state.size());
            store_u64_le(
                header,
                80,
                detail::artifact_checksum(active_checkpoint_state));
            append(header);
            append(active_checkpoint_state);
        }
        if (mixed_rate_closure_policy_set) {
            sync_mixed_rate_checkpoint_state();
            std::array<
                std::byte,
                detail::checkpoint_record_header_size> header{};
            std::memcpy(
                header.data(),
                kMixedRateStateName.data(),
                kMixedRateStateName.size());
            store_u32_le(header, 64, kMixedRateStateSchema);
            store_u64_le(header, 72, mixed_rate_checkpoint_state.size());
            store_u64_le(
                header,
                80,
                detail::artifact_checksum(mixed_rate_checkpoint_state));
            append(header);
            append(mixed_rate_checkpoint_state);
        }
        std::span<const std::byte> live_control_state;
        if (live_control_closure_policy_set && live_control_mailboxes &&
            live_control_mailboxes->sync_checkpoint_state(
                live_control_state)) {
            std::array<
                std::byte,
                detail::checkpoint_record_header_size> header{};
            std::memcpy(
                header.data(),
                kLiveControlStateName.data(),
                kLiveControlStateName.size());
            store_u32_le(header, 64, kLiveControlStateSchema);
            store_u64_le(header, 72, live_control_state.size());
            store_u64_le(
                header,
                80,
                detail::artifact_checksum(live_control_state));
            append(header);
            append(live_control_state);
        }
        return hash;
    }

    Status fail_compile(
        Status status,
        const detail::GraphCompileDiagnostic& diagnostic) noexcept {
        SpinGuard guard(error_lock);
        if (status == Status::graph_cycle) {
            const char* name = valid_phase(diagnostic.first_phase)
                ? callbacks[diagnostic.first_phase.index()].name.c_str()
                : "<unknown>";
            std::snprintf(
                error.data(),
                error.size(),
                "dependency cycle includes phase %u ('%s')",
                static_cast<unsigned>(diagnostic.first_phase.index()),
                name);
            return status;
        }
        if (status == Status::resource_conflict) {
            const char* first = valid_phase(diagnostic.first_phase)
                ? callbacks[diagnostic.first_phase.index()].name.c_str()
                : "<unknown>";
            const char* second = valid_phase(diagnostic.second_phase)
                ? callbacks[diagnostic.second_phase.index()].name.c_str()
                : "<unknown>";
            const char* resource = valid_resource(diagnostic.resource)
                ? resources[diagnostic.resource.index()].name.c_str()
                : "<unknown>";
            std::snprintf(
                error.data(),
                error.size(),
                "resource '%s' has unordered conflicting access by phases "
                "'%s' and '%s'; add a dependency path",
                resource,
                first,
                second);
            return status;
        }
        if (status == Status::invalid_handle) {
            std::snprintf(
                error.data(),
                error.size(),
                "%s",
                "graph contains an invalid phase or resource handle");
            return status;
        }
        std::snprintf(
            error.data(),
            error.size(),
            "%s",
            status_message(status));
        return status;
    }

    [[nodiscard]] std::uint64_t clock_now() noexcept {
        SpinGuard guard(clock_lock);
        return clock->now_ns();
    }

    [[nodiscard]] Status clock_sleep_until(
        std::uint64_t absolute_ns) noexcept {
        SpinGuard guard(clock_lock);
        return clock->sleep_until_ns(absolute_ns);
    }

    Status fail_preflight() noexcept {
        for (std::size_t index = 0;
             index < preflight_report.check_count;
             ++index) {
            const auto& check = preflight_report.checks[index];
            if (check.status != PlatformCheckStatus::passed) {
                return fail(
                    Status::platform_preflight_failed,
                    check.message[0] != '\0'
                        ? check.message.data()
                        : "strict platform preflight prerequisite failed");
            }
        }
        return fail(
            Status::platform_preflight_failed,
            "strict platform preflight report is incomplete or contains "
            "duplicate prerequisites");
    }

    void record(
        RuntimeTraceEventType type,
        Status status,
        std::uint64_t timestamp_ns,
        std::uint64_t frame_index,
        std::size_t callback_index = kNoCallback,
        RuntimeTraceProducer producer =
            RuntimeTraceProducer::host,
        std::size_t worker_index = kNoWorker,
        std::uint64_t value = 0) noexcept {
        switch (type) {
        case RuntimeTraceEventType::step_begin:
            telemetry_counters.increment(
                RuntimeMetricId::frames_started);
            break;
        case RuntimeTraceEventType::step_end:
            telemetry_counters.increment(
                RuntimeMetricId::frames_completed);
            if (status != Status::ok) {
                telemetry_counters.increment(
                    RuntimeMetricId::frames_failed);
            }
            break;
        case RuntimeTraceEventType::callback_begin:
            telemetry_counters.increment(
                RuntimeMetricId::callbacks_started);
            break;
        case RuntimeTraceEventType::callback_end:
            telemetry_counters.increment(
                RuntimeMetricId::callbacks_completed);
            if (status != Status::ok) {
                telemetry_counters.increment(
                    RuntimeMetricId::callback_failures);
            }
            break;
        case RuntimeTraceEventType::watchdog_fired:
            telemetry_counters.increment(
                RuntimeMetricId::watchdog_events);
            break;
        case RuntimeTraceEventType::degradation_applied:
            telemetry_counters.increment(
                RuntimeMetricId::degradation_events);
            break;
        case RuntimeTraceEventType::periodic_release:
            telemetry_counters.increment(
                RuntimeMetricId::periodic_releases);
            break;
        case RuntimeTraceEventType::periodic_wake:
            telemetry_counters.increment(
                RuntimeMetricId::periodic_wakes);
            break;
        case RuntimeTraceEventType::finalized:
        case RuntimeTraceEventType::started:
        case RuntimeTraceEventType::stopped:
        case RuntimeTraceEventType::device_submitted:
        case RuntimeTraceEventType::device_completed:
        case RuntimeTraceEventType::device_reset:
            break;
        }

        if (!telemetry) {
            return;
        }
        RuntimeTraceEvent event;
        event.type = type;
        event.status = status;
        event.producer = producer;
        event.timestamp_ns = timestamp_ns;
        event.frame_index = frame_index;
        event.callback_index =
            callback_index == kNoCallback
            ? std::numeric_limits<std::uint32_t>::max()
            : static_cast<std::uint32_t>(callback_index);
        event.worker_index =
            worker_index == kNoWorker
            ? std::numeric_limits<std::uint32_t>::max()
            : static_cast<std::uint32_t>(worker_index);
        event.value = value;
        (void)telemetry->emit(event);
    }

    [[nodiscard]] std::array<
        std::uint64_t,
        runtime_metric_count> metric_values() const noexcept {
        std::array<std::uint64_t, runtime_metric_count> values{};
        for (std::size_t index = 0;
             index < values.size();
             ++index) {
            values[index] = telemetry_counters.load(
                static_cast<RuntimeMetricId>(index));
        }

        const auto set =
            [&values](
                RuntimeMetricId id,
                std::uint64_t value) {
                values[static_cast<std::size_t>(id)] = value;
            };
        if (telemetry) {
            set(
                RuntimeMetricId::trace_events_emitted,
                telemetry->emitted());
            set(
                RuntimeMetricId::trace_events_overwritten,
                telemetry->overwritten());
            set(
                RuntimeMetricId::trace_events_dropped,
                telemetry->dropped());
        }
        if (executor) {
            const auto stats = executor->stats();
            set(
                RuntimeMetricId::executor_submitted_tasks,
                stats.submitted_tasks);
            set(
                RuntimeMetricId::executor_local_executions,
                stats.local_executions);
            set(
                RuntimeMetricId::executor_steal_attempts,
                stats.steal_attempts);
            set(
                RuntimeMetricId::executor_successful_steals,
                stats.successful_steals);
            set(
                RuntimeMetricId::executor_queue_rejections,
                stats.queue_full_rejections);
            set(
                RuntimeMetricId::executor_scratch_exhaustions,
                stats.scratch_exhaustions);
            set(
                RuntimeMetricId::executor_worker_starts,
                stats.worker_starts);
        }
        if (devices) {
            const auto stats = devices->stats();
            set(RuntimeMetricId::device_submissions, stats.submissions);
            set(RuntimeMetricId::device_completions, stats.completions);
            set(RuntimeMetricId::device_failures, stats.failures);
            set(
                RuntimeMetricId::device_queue_rejections,
                stats.queue_rejections);
            set(RuntimeMetricId::device_timeouts, stats.timeouts);
            set(RuntimeMetricId::device_losses, stats.losses);
            set(RuntimeMetricId::device_resets, stats.resets);
            set(
                RuntimeMetricId::device_service_polls,
                stats.service_polls);
            set(RuntimeMetricId::device_outstanding, stats.outstanding);
            set(
                RuntimeMetricId::device_service_starts,
                stats.service_starts);
        }
        set(
            RuntimeMetricId::degradation_level,
            degradation_level.load(std::memory_order_acquire));
        return values;
    }

    static void observe_device_event(
        void* opaque,
        const detail::DeviceEvent& event) noexcept {
        auto& self = *static_cast<Impl*>(opaque);
        RuntimeTraceEventType type =
            RuntimeTraceEventType::device_completed;
        switch (event.kind) {
        case detail::DeviceEventKind::submitted:
            type = RuntimeTraceEventType::device_submitted;
            break;
        case detail::DeviceEventKind::completed:
            type = RuntimeTraceEventType::device_completed;
            break;
        case detail::DeviceEventKind::reset:
            type = RuntimeTraceEventType::device_reset;
            break;
        }
        self.record(
            type,
            event.status,
            self.clock_now(),
            event.frame_index,
            event.phase_index,
            event.producer,
            event.worker_index,
            event.kind == detail::DeviceEventKind::reset
                ? event.backend_index
                : event.submission_id);
    }

    static Status publish_active_channel(
        void* opaque,
        CrossRateChannelHandle channel,
        std::span<const std::byte> payload) noexcept {
        auto& self = *static_cast<Impl*>(opaque);
        if (self.active_reference_index ==
            invalid_reference_release_index) {
            return Status::invalid_state;
        }
        if (!channel.valid() || channel.owner() != self.graph_owner) {
            return Status::invalid_handle;
        }
        if (channel.index() >= self.cross_rate_channels.size()) {
            return Status::invalid_handle;
        }
        const auto index = static_cast<std::size_t>(channel.index());
        const auto& spec = self.cross_rate_channels[index];
        if (spec.producer != self.active_rate_view.phase) {
            return Status::invalid_handle;
        }
        if (payload.size() != spec.payload_size) {
            return Status::invalid_argument;
        }
        const auto* sampled = self.sampled_io_record(index);
        std::size_t sampled_index = 0;
        if (sampled) {
            sampled_index = self.sampled_io_index(index);
            std::uint64_t expected_sequence = 0;
            std::uint64_t expected_generation = 0;
            if (!self.sampled_io_expected_sequence(
                    *sampled,
                    self.compiled_rate_plan.releases[
                        self.active_reference_index],
                    self.active_rate_view.domain_release_sequence,
                    expected_sequence,
                    expected_generation) ||
                !detail::sampled_io_frame_valid(
                    payload,
                    sampled->public_record,
                    SampledIoFrameStatus::produced,
                    expected_sequence,
                    expected_generation,
                    true)) {
                self.sampled_io_statuses[sampled_index].last_status =
                    Status::invalid_argument;
                return Status::invalid_argument;
            }
        }
        std::uint8_t expected = 0;
        if (!self.active_publication_claims[index].compare_exchange_strong(
                expected,
                1,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            if (sampled) {
                ++self.sampled_io_statuses[sampled_index].overruns;
                self.sampled_io_statuses[sampled_index].last_status =
                    Status::capacity_exceeded;
                self.emit_sampled_failure(
                    index,
                    payload,
                    MixedRateActionId::sampled_publish,
                    MixedRateActionReason::overrun,
                    Status::capacity_exceeded,
                    MixedRateSampleFreshness::fresh,
                    false,
                    false);
                if (sampled->public_record.overrun_policy ==
                    SampledIoOverrunPolicy::reject_newest) {
                    return Status::capacity_exceeded;
                }
            }
            self.active_publication_claims[index].store(
                3,
                std::memory_order_release);
            return Status::invalid_state;
        }
        const auto offset = self.active_channel_states[index].payload_offset;
        std::copy(
            payload.begin(),
            payload.end(),
            self.active_staging_payloads.begin() +
                static_cast<std::ptrdiff_t>(offset));
        expected = 1;
        if (!self.active_publication_claims[index].compare_exchange_strong(
                expected,
                2,
                std::memory_order_release,
                std::memory_order_acquire)) {
            return Status::invalid_state;
        }
        if (sampled) {
            SampledIoFrameHeader header{};
            (void)detail::sampled_io_read_header(payload, header);
            auto& status = self.sampled_io_statuses[sampled_index];
            status.last_sequence = header.sequence;
            ++status.accepted_frames;
            status.last_status = Status::ok;
        }
        return Status::ok;
    }

    static CrossRateReadStatus copy_active_channel(
        void* opaque,
        CrossRateChannelHandle channel,
        std::span<std::byte> output,
        CrossRateReadResult& result) noexcept {
        auto& self = *static_cast<Impl*>(opaque);
        result = {};
        if (self.active_reference_index ==
            invalid_reference_release_index) {
            result.status = CrossRateReadStatus::not_ready;
            return result.status;
        }
        if (!channel.valid() || channel.owner() != self.graph_owner) {
            result.status = CrossRateReadStatus::wrong_owner;
            return result.status;
        }
        if (channel.index() >= self.cross_rate_channels.size()) {
            result.status = CrossRateReadStatus::invalid_channel;
            return result.status;
        }
        const auto index = static_cast<std::size_t>(channel.index());
        const auto& spec = self.cross_rate_channels[index];
        if (spec.consumer != self.active_rate_view.phase) {
            result.status = CrossRateReadStatus::wrong_owner;
            return result.status;
        }
        if (output.size() != spec.payload_size) {
            result.status = CrossRateReadStatus::size_mismatch;
            return result.status;
        }
        const auto& active_release = self.compiled_rate_plan.releases[
            self.active_reference_index];
        const auto& descriptor =
            self.compiled_cross_rate_plan.channels[index];
        std::uint64_t consumer_ordinal = 0;
        std::uint64_t selection_offset = 0;
        if (!checked_time_multiply(
                active_release.domain_release_sequence,
                active_release.substep_count,
                consumer_ordinal) ||
            !checked_time_add(
                consumer_ordinal,
                active_release.substep_ordinal,
                consumer_ordinal) ||
            !checked_time_multiply(
                consumer_ordinal,
                2,
                selection_offset) ||
            !checked_time_add(
                descriptor.first_selection_index,
                selection_offset,
                selection_offset) ||
            !checked_time_add(
                selection_offset,
                self.active_supercycle_cycle == 0 ? 0u : 1u,
                selection_offset) ||
            selection_offset >=
                self.compiled_cross_rate_plan.selections.size()) {
            result.status = CrossRateReadStatus::not_ready;
            return result.status;
        }
        const auto& selection =
            self.compiled_cross_rate_plan.selections[
                static_cast<std::size_t>(selection_offset)];
        if (selection.channel != channel ||
            selection.consumer_reference_index !=
                self.active_reference_index ||
            selection.horizon !=
                (self.active_supercycle_cycle == 0
                     ? CrossRateSelectionHorizon::first_supercycle
                     : CrossRateSelectionHorizon::repeating_supercycle)) {
            result.status = CrossRateReadStatus::not_ready;
            return result.status;
        }
        const auto& state = self.active_channel_states[index];
        if (descriptor.producer_device.valid() &&
            state.provenance == CrossRateSampleProvenance::produced &&
            state.producer_timestamp_domain_identity == 0) {
            result.status = CrossRateReadStatus::not_ready;
            return result.status;
        }
        if (!state.held) {
            if (selection.provenance != state.provenance) {
                result.status = CrossRateReadStatus::stale_generation;
                return result.status;
            }
            if (selection.provenance ==
                    CrossRateSampleProvenance::produced) {
                if (selection.producer_reference_index >=
                    self.compiled_rate_plan.releases.size()) {
                    result.status = CrossRateReadStatus::not_ready;
                    return result.status;
                }
                std::uint64_t source_cycle = self.active_supercycle_cycle;
                if (selection.source_cycle_offset == -1) {
                    if (source_cycle == 0) {
                        result.status = CrossRateReadStatus::not_ready;
                        return result.status;
                    }
                    --source_cycle;
                } else if (selection.source_cycle_offset != 0) {
                    result.status = CrossRateReadStatus::not_ready;
                    return result.status;
                }
                std::uint64_t source_cycle_base = 0;
                std::uint64_t expected_source = 0;
                if (!checked_time_multiply(
                        source_cycle,
                        self.compiled_rate_plan.supercycle_ns,
                        source_cycle_base) ||
                    !checked_time_add(
                        source_cycle_base,
                        self.compiled_rate_plan
                            .releases[selection.producer_reference_index]
                            .release_time_ns,
                        expected_source) ||
                    expected_source !=
                        state.source_logical_release_ns) {
                    result.status = CrossRateReadStatus::stale_generation;
                    return result.status;
                }
            }
        }
        const auto store_status =
            self.compiled_cross_rate_plan.stores[index].copy(
                state.generation,
                output,
                detail::SnapshotRetention::retain);
        if (store_status != detail::SnapshotStoreResult::ok) {
            result.status =
                store_status == detail::SnapshotStoreResult::stale_generation
                ? CrossRateReadStatus::stale_generation
                : CrossRateReadStatus::not_ready;
            return result.status;
        }
        result.status = CrossRateReadStatus::ok;
        result.provenance = state.provenance;
        result.generation = state.generation;
        result.held = state.held || selection.held;
        result.age_ns = self.active_logical_release_ns >=
                state.source_logical_release_ns
            ? self.active_logical_release_ns -
                state.source_logical_release_ns
            : std::numeric_limits<std::uint64_t>::max();
        result.freshness =
            spec.maximum_age_ns ==
                    std::numeric_limits<std::uint64_t>::max() ||
                result.age_ns <= spec.maximum_age_ns
            ? CrossRateFreshness::fresh
            : CrossRateFreshness::stale;
        result.producer_release_sequence =
            state.producer_release_sequence;
        result.producer_substep_ordinal =
            state.producer_substep_ordinal;
        result.producer_completion_status =
            state.producer_completion_status;
        result.producer_timestamp_domain_identity =
            state.producer_timestamp_domain_identity;
        result.producer_timestamp = state.producer_timestamp;
        if (result.freshness == CrossRateFreshness::stale) {
            self.active_stale_reads.fetch_add(1, std::memory_order_relaxed);
        }
        const bool sampled_was_stale =
            result.freshness == CrossRateFreshness::stale;
        const auto* sampled = self.sampled_io_record(index);
        if (sampled) {
            auto& sampled_status = self.sampled_io_statuses[
                self.sampled_io_index(index)];
            SampledIoFrameHeader header{};
            if (!detail::sampled_io_read_header(output, header)) {
                result.status = CrossRateReadStatus::not_ready;
                sampled_status.last_status = Status::invalid_argument;
                return result.status;
            }
            if (result.freshness == CrossRateFreshness::stale) {
                ++sampled_status.stale_frames;
                if (sampled->public_record.direction ==
                        SampledIoDirection::output) {
                    ++sampled_status.underruns;
                    if (sampled->public_record.underrun_policy !=
                        SampledIoUnderrunPolicy::substitute_safe) {
                        result.status = CrossRateReadStatus::stale_generation;
                        sampled_status.last_status = Status::invalid_state;
                        self.emit_sampled_failure(
                            index,
                            std::span<const std::byte>(output),
                            MixedRateActionId::sampled_select,
                            MixedRateActionReason::underrun,
                            Status::invalid_state,
                            MixedRateSampleFreshness::stale,
                            result.held,
                            false);
                        return result.status;
                    }
                    const auto safe = self.sampled_io_frame(
                        sampled->failure_safe_offset,
                        sampled->public_record.frame_bytes);
                    std::copy(safe.begin(), safe.end(), output.begin());
                    (void)detail::sampled_io_read_header(output, header);
                    result.sampled_substituted = true;
                    result.freshness = CrossRateFreshness::fresh;
                    ++sampled_status.substituted_frames;
                } else if (sampled->public_record.stale_policy !=
                    SampledIoStalePolicy::substitute_initial) {
                    result.status = CrossRateReadStatus::stale_generation;
                    sampled_status.last_status = Status::invalid_state;
                    self.emit_sampled_failure(
                        index,
                        std::span<const std::byte>(output),
                        MixedRateActionId::sampled_select,
                        MixedRateActionReason::stale,
                        Status::invalid_state,
                        MixedRateSampleFreshness::stale,
                        result.held,
                        false);
                    return result.status;
                } else {
                    const auto initial = self.sampled_io_frame(
                        sampled->initial_offset,
                        sampled->public_record.frame_bytes);
                    std::copy(initial.begin(), initial.end(), output.begin());
                    (void)detail::sampled_io_read_header(output, header);
                    result.sampled_substituted = true;
                    result.freshness = CrossRateFreshness::fresh;
                    ++sampled_status.substituted_frames;
                }
            }
            result.sampled_sequence = header.sequence;
            result.sampled_trigger_identity = header.trigger_identity;
            result.sampled_trigger_sequence = header.trigger_sequence;
            result.sampled_calibration_identity =
                header.calibration_identity;
            sampled_status.last_sequence = header.sequence;
            sampled_status.last_status = Status::ok;
            self.emit_sampled_selection(
                index,
                std::span<const std::byte>(output),
                result,
                sampled_was_stale);
        }
        return result.status;
    }

    static void record_active_failure(
        StepResult::RateSummary& summary,
        const ReferenceRelease& release,
        std::uint64_t domain_release_sequence) noexcept {
        if (!summary.has_first_failure) {
            summary.has_first_failure = true;
            summary.first_failing_domain = release.domain;
            summary.first_failing_sequence = domain_release_sequence;
            summary.first_failing_substep = release.substep_ordinal;
        }
    }

    void set_active_failure(
        const ReferenceRelease& release,
        std::uint64_t domain_release_sequence) noexcept {
        active_faulted = true;
        active_fault_domain = release.domain;
        active_fault_sequence = domain_release_sequence;
        active_fault_substep = release.substep_ordinal;
        if (active_rate_summary) {
            record_active_failure(
                *active_rate_summary,
                release,
                domain_release_sequence);
        }
    }

    struct PolicyTransition {
        RateTransitionId id = RateTransitionId::none;
        RateActionReason reason = RateActionReason::on_time;
        std::uint64_t before = 0;
        std::uint64_t after = 0;
        std::uint32_t domain_index =
            std::numeric_limits<std::uint32_t>::max();
    };

    [[nodiscard]] PolicyTransition current_policy_state(
        RateActionReason reason) const noexcept {
        PolicyTransition transition;
        transition.reason = reason;
        transition.before = active_shedding_state
            ? active_shedding_state->shed_mask
            : 0;
        transition.after = transition.before;
        return transition;
    }

    [[nodiscard]] PolicyTransition observe_mandatory_release(
        bool late,
        StepResult::RateSummary& summary) noexcept {
        PolicyTransition transition;
        transition.before = active_shedding_state
            ? active_shedding_state->shed_mask
            : 0;
        transition.after = transition.before;
        transition.reason = late
            ? RateActionReason::deadline_late
            : RateActionReason::on_time;
        if (!active_shedding_state) {
            return transition;
        }
        if (late) {
            active_shedding_state->consecutive_on_time = 0;
            if (active_shedding_state->consecutive_late <
                rate_execution_policy.consecutive_late_threshold) {
                ++active_shedding_state->consecutive_late;
            }
            if (active_shedding_state->consecutive_late ==
                rate_execution_policy.consecutive_late_threshold) {
                for (const auto index :
                     compiled_rate_dispatch_plan.optional_shed_order) {
                    const auto bit = std::uint64_t{1} << index;
                    if ((active_shedding_state->shed_mask & bit) == 0) {
                        active_shedding_state->shed_mask |= bit;
                        active_shedding_state->consecutive_late = 0;
                        transition.id = RateTransitionId::shed;
                        transition.reason = RateActionReason::late_threshold;
                        transition.domain_index = static_cast<std::uint32_t>(index);
                        transition.after = active_shedding_state->shed_mask;
                        ++summary.shed_transitions;
                        break;
                    }
                }
            }
        } else {
            active_shedding_state->consecutive_late = 0;
            if (active_shedding_state->consecutive_on_time <
                rate_execution_policy.consecutive_on_time_threshold) {
                ++active_shedding_state->consecutive_on_time;
            }
            if (active_shedding_state->consecutive_on_time ==
                rate_execution_policy.consecutive_on_time_threshold) {
                for (auto iterator =
                         compiled_rate_dispatch_plan.optional_shed_order.rbegin();
                     iterator !=
                         compiled_rate_dispatch_plan.optional_shed_order.rend();
                     ++iterator) {
                    const auto bit = std::uint64_t{1} << *iterator;
                    if ((active_shedding_state->shed_mask & bit) != 0) {
                        active_shedding_state->shed_mask &= ~bit;
                        active_shedding_state->consecutive_on_time = 0;
                        transition.id = RateTransitionId::recover;
                        transition.reason = RateActionReason::on_time_threshold;
                        transition.domain_index =
                            static_cast<std::uint32_t>(*iterator);
                        transition.after = active_shedding_state->shed_mask;
                        ++summary.recovery_transitions;
                        break;
                    }
                }
            }
        }
        transition.after = active_shedding_state->shed_mask;
        summary.currently_shed_domains =
            static_cast<std::uint64_t>(
                std::popcount(active_shedding_state->shed_mask));
        return transition;
    }

    [[nodiscard]] const detail::CompiledCrossRateDeviceEndpoint*
    device_endpoint_for_channel(std::size_t channel_index) const noexcept {
        if (channel_index >=
                compiled_cross_rate_plan.device_endpoint_index_by_channel.size()) {
            return nullptr;
        }
        const auto endpoint_index = compiled_cross_rate_plan
            .device_endpoint_index_by_channel[channel_index];
        return endpoint_index < compiled_cross_rate_plan.device_endpoints.size()
            ? &compiled_cross_rate_plan.device_endpoints[endpoint_index]
            : nullptr;
    }

    [[nodiscard]] std::size_t sampled_io_index(
        std::size_t cross_rate_channel_index) const noexcept {
        if (cross_rate_channel_index >= compiled_sampled_io_plan
                .channel_index_by_cross_rate.size()) {
            return std::numeric_limits<std::size_t>::max();
        }
        return compiled_sampled_io_plan
            .channel_index_by_cross_rate[cross_rate_channel_index];
    }

    [[nodiscard]] const detail::CompiledSampledIoRecord* sampled_io_record(
        std::size_t cross_rate_channel_index) const noexcept {
        const auto index = sampled_io_index(cross_rate_channel_index);
        return index < compiled_sampled_io_plan.channels.size()
            ? &compiled_sampled_io_plan.channels[index]
            : nullptr;
    }

    [[nodiscard]] std::span<const std::byte> sampled_io_frame(
        std::size_t offset,
        std::size_t bytes) const noexcept {
        if (offset > compiled_sampled_io_plan.frame_storage.size() ||
            bytes > compiled_sampled_io_plan.frame_storage.size() - offset) {
            return {};
        }
        return std::span<const std::byte>(
            compiled_sampled_io_plan.frame_storage.data() + offset, bytes);
    }

    [[nodiscard]] static bool sampled_io_expected_sequence(
        const detail::CompiledSampledIoRecord& channel,
        const ReferenceRelease& release,
        std::uint64_t domain_release_sequence,
        std::uint64_t& sequence,
        std::uint64_t& generation) noexcept {
        std::uint64_t ordinal = 0;
        return checked_time_multiply(
                   domain_release_sequence,
                   release.substep_count,
                   ordinal) &&
            checked_time_add(ordinal, release.substep_ordinal, ordinal) &&
            checked_time_add(ordinal, 1, generation) &&
            checked_time_add(
                channel.public_record.initial_sequence,
                generation,
                sequence);
    }

    [[nodiscard]] bool device_payload_slot(
        std::size_t reference_index,
        const ReferenceRelease& release,
        std::uint64_t domain_release_sequence,
        std::uint32_t& slot) const noexcept {
        if (release.phase_kind != RatePhaseKind::device ||
            reference_index >= compiled_rate_dispatch_plan
                .device_phase_by_reference.size()) {
            return false;
        }
        const auto device_phase_index = compiled_rate_dispatch_plan
            .device_phase_by_reference[reference_index];
        if (device_phase_index >= compiled_device_rate_plan.phases.size()) {
            return false;
        }
        const auto slot_count = compiled_device_rate_plan
            .phases[device_phase_index].maximum_in_flight;
        std::uint64_t release_ordinal = 0;
        if (slot_count == 0 ||
            !checked_time_multiply(
                domain_release_sequence,
                release.substep_count,
                release_ordinal) ||
            !checked_time_add(
                release_ordinal,
                release.substep_ordinal,
                release_ordinal)) {
            return false;
        }
        slot = static_cast<std::uint32_t>(release_ordinal % slot_count);
        return true;
    }

    [[nodiscard]] Status prepare_device_inputs(
        const ReferenceRelease& release,
        std::uint32_t payload_slot) noexcept {
        const auto consumer_begin = compiled_rate_dispatch_plan
            .consumer_channel_offsets.empty()
            ? 0
            : compiled_rate_dispatch_plan
                  .consumer_channel_offsets[release.phase.index()];
        const auto consumer_end = compiled_rate_dispatch_plan
            .consumer_channel_offsets.empty()
            ? 0
            : compiled_rate_dispatch_plan
                  .consumer_channel_offsets[release.phase.index() + 1];
        for (std::size_t cursor = consumer_begin;
             cursor < consumer_end; ++cursor) {
            const auto channel_index = compiled_rate_dispatch_plan
                .consumer_channel_indices[cursor];
            const auto& descriptor =
                compiled_cross_rate_plan.channels[channel_index];
            if (!descriptor.consumer_device.valid()) {
                continue;
            }
            const auto* endpoint = device_endpoint_for_channel(channel_index);
            if (!endpoint || endpoint->producer ||
                payload_slot >= endpoint->slot_count) {
                return Status::internal_error;
            }
            std::uint64_t relative_offset = 0;
            std::uint64_t payload_offset = 0;
            if (!checked_time_multiply(
                    payload_slot,
                    endpoint->slot_stride_bytes,
                    relative_offset) ||
                !checked_time_add(
                    endpoint->envelope_offset,
                    relative_offset,
                    payload_offset) ||
                payload_offset > endpoint->host_storage.size() ||
                cross_rate_channels[channel_index].payload_size >
                    endpoint->host_storage.size() - payload_offset) {
                return Status::internal_error;
            }
            CrossRateReadResult read;
            const auto read_status = copy_active_channel(
                this,
                CrossRateChannelHandle{
                    graph_owner, static_cast<std::uint32_t>(channel_index)},
                std::span<std::byte>(
                    endpoint->host_storage.data() + payload_offset,
                    cross_rate_channels[channel_index].payload_size),
                read);
            if (read_status != CrossRateReadStatus::ok) {
                return Status::invalid_argument;
            }
            if (read.freshness != CrossRateFreshness::fresh) {
                return Status::invalid_state;
            }
        }
        const auto producer_begin = compiled_rate_dispatch_plan
            .producer_channel_offsets.empty()
            ? 0
            : compiled_rate_dispatch_plan
                  .producer_channel_offsets[release.phase.index()];
        const auto producer_end = compiled_rate_dispatch_plan
            .producer_channel_offsets.empty()
            ? 0
            : compiled_rate_dispatch_plan
                  .producer_channel_offsets[release.phase.index() + 1];
        for (std::size_t cursor = producer_begin;
             cursor < producer_end; ++cursor) {
            const auto channel_index = compiled_rate_dispatch_plan
                .producer_channel_indices[cursor];
            const auto& descriptor =
                compiled_cross_rate_plan.channels[channel_index];
            if (!descriptor.producer_device.valid()) {
                continue;
            }
            const auto* sampled = sampled_io_record(channel_index);
            const auto* endpoint = device_endpoint_for_channel(channel_index);
            if (!sampled) {
                continue;
            }
            if (!endpoint || !endpoint->producer ||
                payload_slot >= endpoint->slot_count) {
                return Status::internal_error;
            }
            std::uint64_t relative_offset = 0;
            std::uint64_t payload_offset = 0;
            if (!checked_time_multiply(
                    payload_slot,
                    endpoint->slot_stride_bytes,
                    relative_offset) ||
                !checked_time_add(
                    endpoint->envelope_offset,
                    relative_offset,
                    payload_offset) ||
                payload_offset > endpoint->host_storage.size() ||
                sampled->public_record.frame_bytes >
                    endpoint->host_storage.size() - payload_offset ||
                sampled->public_record.frame_bytes <
                    sizeof(SampledIoFrameHeader)) {
                return Status::internal_error;
            }
            std::uint64_t sequence = 0;
            std::uint64_t generation = 0;
            if (!sampled_io_expected_sequence(
                    *sampled,
                    release,
                    active_rate_view.domain_release_sequence,
                    sequence,
                    generation)) {
                return Status::capacity_exceeded;
            }
            // Runtime owns release correlation. Pre-materialize only the
            // expected header in the selected output slot; the backend must
            // still produce a complete frame that passes terminal validation.
            SampledIoFrameHeader header{};
            header.channel_identity =
                sampled->public_record.channel_identity;
            header.sequence = sequence;
            header.release_generation = generation;
            header.sample_count =
                sampled->public_record.samples_per_frame;
            header.encoding = static_cast<std::uint32_t>(
                sampled->public_record.encoding);
            header.timestamp_domain_identity =
                sampled->public_record.timestamp_domain_identity;
            header.sample_interval_ns =
                sampled->public_record.sample_period_ns;
            header.trigger_identity =
                sampled->public_record.trigger_identity;
            header.trigger_sequence = sequence;
            header.calibration_identity =
                sampled->public_record.calibration_identity;
            header.status = static_cast<std::uint32_t>(
                SampledIoFrameStatus::produced);
            std::memcpy(
                endpoint->host_storage.data() + payload_offset,
                &header,
                sizeof(header));
        }
        return Status::ok;
    }

    [[nodiscard]] bool materialize_device_payload_references(
        DeviceCommandBatch& batch,
        DeviceCommandBatch& declaration,
        const ReferenceRelease& release,
        std::uint32_t payload_slot) const noexcept {
        const auto reference_at = [](
            DeviceCommandBatch& candidate,
            const detail::CompiledCrossRateDeviceEndpoint& endpoint)
            -> HalV2BufferReference* {
            if (endpoint.command_index >= candidate.command_count) {
                return nullptr;
            }
            auto& command = candidate.commands[endpoint.command_index];
            switch (endpoint.reference_kind) {
            case DeviceRateReferenceKind::dispatch_buffer:
                return endpoint.command_reference_index < command.buffer_count
                    ? &command.buffers[endpoint.command_reference_index]
                    : nullptr;
            case DeviceRateReferenceKind::copy_source:
                return &command.source;
            case DeviceRateReferenceKind::copy_destination:
                return &command.destination;
            case DeviceRateReferenceKind::synchronization_target:
                return &command.target;
            }
            return nullptr;
        };
        const auto materialize_channel = [&](std::size_t channel_index) {
            const auto* endpoint = device_endpoint_for_channel(channel_index);
            if (!endpoint || endpoint->phase != release.phase ||
                payload_slot >= endpoint->slot_count) {
                return false;
            }
            auto* requested = reference_at(batch, *endpoint);
            auto* copied = reference_at(declaration, *endpoint);
            if (!requested || !copied ||
                requested->buffer_token != endpoint->buffer.value ||
                requested->offset != endpoint->envelope_offset ||
                requested->bytes != endpoint->envelope_bytes) {
                return false;
            }
            std::uint64_t slot_offset = 0;
            if (!checked_time_multiply(
                    payload_slot,
                    endpoint->slot_stride_bytes,
                    slot_offset) ||
                !checked_time_add(
                    endpoint->envelope_offset,
                    slot_offset,
                    slot_offset)) {
                return false;
            }
            requested->offset = slot_offset;
            requested->bytes = cross_rate_channels[channel_index].payload_size;
            copied->offset = requested->offset;
            copied->bytes = requested->bytes;
            return true;
        };
        const auto apply_slice = [&](const std::vector<std::size_t>& offsets,
                                     const std::vector<std::size_t>& indices) {
            if (offsets.empty()) {
                return true;
            }
            const auto begin = offsets[release.phase.index()];
            const auto end = offsets[release.phase.index() + 1];
            for (std::size_t cursor = begin; cursor < end; ++cursor) {
                const auto channel_index = indices[cursor];
                const auto& channel =
                    compiled_cross_rate_plan.channels[channel_index];
                if ((channel.producer == release.phase &&
                     channel.producer_device.valid()) ||
                    (channel.consumer == release.phase &&
                     channel.consumer_device.valid())) {
                    if (!materialize_channel(channel_index)) {
                        return false;
                    }
                }
            }
            return true;
        };
        return apply_slice(
                   compiled_rate_dispatch_plan.producer_channel_offsets,
                   compiled_rate_dispatch_plan.producer_channel_indices) &&
               apply_slice(
                   compiled_rate_dispatch_plan.consumer_channel_offsets,
                   compiled_rate_dispatch_plan.consumer_channel_indices);
    }

    [[nodiscard]] bool normalize_replay_device_completion(
        const detail::DeviceRateTicket& ticket,
        Status status,
        detail::DeviceRateCompletion& completion) noexcept {
        if (!active_replay_view) {
            return true;
        }
        MixedRateActionRecord expected;
        if (!detail::active_replay_action_at(
                *active_replay_view,
                active_replay_expected_action,
                expected) ||
            expected.action != MixedRateActionId::device_terminal ||
            (expected.stage != MixedRateActionStage::terminal &&
             expected.stage != MixedRateActionStage::quarantined) ||
            expected.rate_domain_registration_index !=
                ticket.identity.domain.index() ||
            expected.phase_index != ticket.identity.phase.index() ||
            expected.release_sequence !=
                ticket.identity.domain_release_sequence ||
            expected.substep_ordinal != ticket.identity.substep_ordinal ||
            expected.terminal_status != static_cast<std::int32_t>(status) ||
            expected.timestamp_domain_identity !=
                completion.timestamp_domain_identity) {
            active_replay_mismatch_sequence =
                active_replay_view->metadata.first_action_sequence +
                active_replay_expected_action;
            active_replay_mismatch_status = Status::incompatible_artifact;
            return false;
        }
        completion.timestamp = expected.timestamp;
        if (status != Status::ok ||
            ticket.identity.reference_index >=
                compiled_rate_plan.releases.size()) {
            return true;
        }
        const auto& release = compiled_rate_plan.releases[
            ticket.identity.reference_index];
        std::uint32_t payload_slot = 0;
        if (!device_payload_slot(
                ticket.identity.reference_index,
                release,
                ticket.identity.domain_release_sequence,
                payload_slot)) {
            return false;
        }
        const auto begin = compiled_rate_dispatch_plan
            .producer_channel_offsets[release.phase.index()];
        const auto end = compiled_rate_dispatch_plan
            .producer_channel_offsets[release.phase.index() + 1];
        for (std::size_t cursor = begin; cursor < end; ++cursor) {
            const auto channel_index = compiled_rate_dispatch_plan
                .producer_channel_indices[cursor];
            if (!sampled_io_record(channel_index)) {
                continue;
            }
            const auto* endpoint = device_endpoint_for_channel(channel_index);
            std::uint64_t relative_offset = 0;
            std::uint64_t payload_offset = 0;
            if (!endpoint || !endpoint->producer ||
                payload_slot >= endpoint->slot_count ||
                !checked_time_multiply(
                    payload_slot,
                    endpoint->slot_stride_bytes,
                    relative_offset) ||
                !checked_time_add(
                    endpoint->envelope_offset,
                    relative_offset,
                    payload_offset) ||
                payload_offset > endpoint->host_storage.size() ||
                sizeof(SampledIoFrameHeader) >
                    endpoint->host_storage.size() - payload_offset) {
                return false;
            }
            SampledIoFrameHeader header{};
            std::memcpy(
                &header,
                endpoint->host_storage.data() + payload_offset,
                sizeof(header));
            header.first_sample_timestamp = expected.timestamp;
            std::memcpy(
                endpoint->host_storage.data() + payload_offset,
                &header,
                sizeof(header));
        }
        return true;
    }

    [[nodiscard]] Status capture_device_outputs(
        const detail::DeviceRateTicket& ticket,
        const detail::DeviceRateCompletion& completion) noexcept {
        if (completion.status != Status::ok ||
            !completion.terminal_slot_owned ||
            ticket.identity.reference_index >= compiled_rate_plan.releases.size()) {
            return completion.status;
        }
        const auto& release =
            compiled_rate_plan.releases[ticket.identity.reference_index];
        std::uint32_t payload_slot = 0;
        if (!device_payload_slot(
                ticket.identity.reference_index,
                release,
                ticket.identity.domain_release_sequence,
                payload_slot)) {
            return Status::internal_error;
        }
        const auto begin = compiled_rate_dispatch_plan
            .producer_channel_offsets[release.phase.index()];
        const auto end = compiled_rate_dispatch_plan
            .producer_channel_offsets[release.phase.index() + 1];
        // Validate every device-produced payload before mutating any snapshot
        // store. A multi-output terminal completion is one publication unit:
        // one malformed frame must not leave earlier channels committed.
        for (std::size_t cursor = begin; cursor < end; ++cursor) {
            const auto channel_index = compiled_rate_dispatch_plan
                .producer_channel_indices[cursor];
            const auto& descriptor =
                compiled_cross_rate_plan.channels[channel_index];
            if (!descriptor.producer_device.valid()) {
                continue;
            }
            const auto* endpoint = device_endpoint_for_channel(channel_index);
            if (!endpoint || !endpoint->producer ||
                payload_slot >= endpoint->slot_count ||
                completion.timestamp_domain_identity !=
                    endpoint->timestamp_domain_identity) {
                return Status::device_error;
            }
            std::uint64_t relative_offset = 0;
            std::uint64_t payload_offset = 0;
            if (!checked_time_multiply(
                    payload_slot,
                    endpoint->slot_stride_bytes,
                    relative_offset) ||
                !checked_time_add(
                    endpoint->envelope_offset,
                    relative_offset,
                    payload_offset) ||
                payload_offset > endpoint->host_storage.size() ||
                cross_rate_channels[channel_index].payload_size >
                    endpoint->host_storage.size() - payload_offset) {
                return Status::internal_error;
            }
            const auto source = std::span<const std::byte>(
                endpoint->host_storage.data() + payload_offset,
                cross_rate_channels[channel_index].payload_size);
            const auto* sampled = sampled_io_record(channel_index);
            if (!sampled) {
                continue;
            }
            const auto sampled_index = sampled_io_index(channel_index);
            std::uint64_t expected_sequence = 0;
            std::uint64_t expected_generation = 0;
            SampledIoFrameHeader sampled_header{};
            if (!sampled_io_expected_sequence(
                    *sampled,
                    release,
                    ticket.identity.domain_release_sequence,
                    expected_sequence,
                    expected_generation) ||
                !detail::sampled_io_frame_valid(
                    source,
                    sampled->public_record,
                    SampledIoFrameStatus::produced,
                    expected_sequence,
                    expected_generation,
                    true) ||
                !detail::sampled_io_read_header(source, sampled_header) ||
                sampled_header.first_sample_timestamp !=
                    completion.timestamp) {
                sampled_io_statuses[sampled_index].last_status =
                    Status::device_error;
                return Status::device_error;
            }
        }
        for (std::size_t cursor = begin; cursor < end; ++cursor) {
            const auto channel_index = compiled_rate_dispatch_plan
                .producer_channel_indices[cursor];
            const auto& descriptor =
                compiled_cross_rate_plan.channels[channel_index];
            if (!descriptor.producer_device.valid()) {
                continue;
            }
            const auto* endpoint = device_endpoint_for_channel(channel_index);
            if (!endpoint || !endpoint->producer ||
                payload_slot >= endpoint->slot_count ||
                completion.timestamp_domain_identity !=
                    endpoint->timestamp_domain_identity) {
                return Status::device_error;
            }
            std::uint64_t relative_offset = 0;
            std::uint64_t payload_offset = 0;
            if (!checked_time_multiply(
                    payload_slot,
                    endpoint->slot_stride_bytes,
                    relative_offset) ||
                !checked_time_add(
                    endpoint->envelope_offset,
                    relative_offset,
                    payload_offset) ||
                payload_offset > endpoint->host_storage.size() ||
                cross_rate_channels[channel_index].payload_size >
                    endpoint->host_storage.size() - payload_offset) {
                return Status::internal_error;
            }
            auto& channel_state = active_channel_states[channel_index];
            auto& store = compiled_cross_rate_plan.stores[channel_index];
            if (!store.can_publish(channel_state.next_generation)) {
                const auto slots = static_cast<std::uint64_t>(
                    store.slot_count());
                if (channel_state.next_generation <= slots ||
                    store.retire(channel_state.next_generation - slots) !=
                        detail::SnapshotStoreResult::ok) {
                    return Status::internal_error;
                }
            }
            const auto source = std::span<const std::byte>(
                endpoint->host_storage.data() + payload_offset,
                cross_rate_channels[channel_index].payload_size);
            const auto* sampled = sampled_io_record(channel_index);
            std::size_t sampled_index = 0;
            SampledIoFrameHeader sampled_header{};
            if (sampled) {
                sampled_index = sampled_io_index(channel_index);
                std::uint64_t expected_sequence = 0;
                std::uint64_t expected_generation = 0;
                if (!sampled_io_expected_sequence(
                        *sampled,
                        release,
                        ticket.identity.domain_release_sequence,
                        expected_sequence,
                        expected_generation) ||
                    !detail::sampled_io_frame_valid(
                        source,
                        sampled->public_record,
                        SampledIoFrameStatus::produced,
                        expected_sequence,
                        expected_generation,
                        true) ||
                    !detail::sampled_io_read_header(
                        source, sampled_header) ||
                    sampled_header.first_sample_timestamp !=
                        completion.timestamp) {
                    sampled_io_statuses[sampled_index].last_status =
                        Status::device_error;
                    return Status::device_error;
                }
            }
            if (store.publish(channel_state.next_generation, source) !=
                detail::SnapshotStoreResult::ok) {
                return Status::internal_error;
            }
            std::copy(
                source.begin(), source.end(),
                active_committed_payloads.begin() +
                    static_cast<std::ptrdiff_t>(channel_state.payload_offset));
            std::copy(
                source.begin(), source.end(),
                active_staging_payloads.begin() +
                    static_cast<std::ptrdiff_t>(channel_state.payload_offset));
            channel_state.generation = channel_state.next_generation;
            channel_state.next_generation = store.next_generation();
            channel_state.source_logical_release_ns =
                ticket.identity.logical_release_ns;
            channel_state.provenance = CrossRateSampleProvenance::produced;
            channel_state.held = false;
            channel_state.producer_release_sequence =
                ticket.identity.domain_release_sequence;
            channel_state.producer_substep_ordinal =
                ticket.identity.substep_ordinal;
            channel_state.producer_completion_status = completion.status;
            channel_state.producer_timestamp_domain_identity =
                completion.timestamp_domain_identity;
            channel_state.producer_timestamp = completion.timestamp;
            if (sampled) {
                auto& status = sampled_io_statuses[sampled_index];
                status.last_sequence = sampled_header.sequence;
                ++status.accepted_frames;
                status.last_status = Status::ok;
            }
        }
        if (compiled_rate_dispatch_plan.consumer_channel_offsets.empty()) {
            return Status::ok;
        }
        const auto consumer_begin = compiled_rate_dispatch_plan
            .consumer_channel_offsets[release.phase.index()];
        const auto consumer_end = compiled_rate_dispatch_plan
            .consumer_channel_offsets[release.phase.index() + 1];
        for (std::size_t cursor = consumer_begin;
             cursor < consumer_end; ++cursor) {
            const auto channel_index = compiled_rate_dispatch_plan
                .consumer_channel_indices[cursor];
            const auto* sampled = sampled_io_record(channel_index);
            if (sampled && sampled->public_record.direction ==
                    SampledIoDirection::output) {
                auto& status = sampled_io_statuses[
                    sampled_io_index(channel_index)];
                status.safety_state = SampledIoSafetyState::active;
                status.last_status = Status::ok;
            }
        }
        return Status::ok;
    }

    [[nodiscard]] Status apply_sampled_io_safe_transition(
        SampledIoSafetyState transition) noexcept {
        if (compiled_sampled_io_plan.safe_phases.empty()) {
            return Status::ok;
        }
        if (!devices) {
            return Status::invalid_state;
        }
        const auto frame_offset = [transition](
            const detail::CompiledSampledIoRecord& channel) {
            switch (transition) {
            case SampledIoSafetyState::startup_acknowledged:
                return channel.startup_safe_offset;
            case SampledIoSafetyState::failure_acknowledged:
                return channel.failure_safe_offset;
            case SampledIoSafetyState::shutdown_acknowledged:
                return channel.shutdown_safe_offset;
            default:
                return std::numeric_limits<std::size_t>::max();
            }
        };
        for (const auto& safe_phase : compiled_sampled_io_plan.safe_phases) {
            if (!safe_phase.phase.valid() ||
                safe_phase.phase.index() >= callbacks.size() ||
                safe_phase.reference_index >= compiled_rate_plan.releases.size()) {
                return Status::internal_error;
            }
            const auto& callback = callbacks[safe_phase.phase.index()];
            auto batch = callback.batch_declaration;
            auto declaration = callback.batch_declaration;
            std::uint64_t timeout_ns =
                std::numeric_limits<std::uint64_t>::max();
            for (std::size_t offset = 0;
                 offset < safe_phase.channel_count; ++offset) {
                const auto sampled_index = compiled_sampled_io_plan
                    .safe_channel_indices[safe_phase.first_channel_index + offset];
                const auto& channel =
                    compiled_sampled_io_plan.channels[sampled_index];
                const auto* endpoint = device_endpoint_for_channel(
                    channel.cross_rate_channel_index);
                const auto source = sampled_io_frame(
                    frame_offset(channel), channel.public_record.frame_bytes);
                if (!endpoint || endpoint->producer || source.empty() ||
                    endpoint->host_storage.size() < endpoint->envelope_offset ||
                    source.size() > endpoint->host_storage.size() -
                        endpoint->envelope_offset) {
                    return Status::internal_error;
                }
                std::copy(
                    source.begin(), source.end(),
                    endpoint->host_storage.begin() +
                        static_cast<std::ptrdiff_t>(endpoint->envelope_offset));
                timeout_ns = std::min(
                    timeout_ns,
                    channel.public_record.safe_transition_timeout_ns);
            }
            if (timeout_ns == 0 ||
                timeout_ns == std::numeric_limits<std::uint64_t>::max()) {
                return Status::invalid_argument;
            }
            const auto& release =
                compiled_rate_plan.releases[safe_phase.reference_index];
            if (!materialize_device_payload_references(
                    batch, declaration, release, 0)) {
                return Status::internal_error;
            }
            batch.timeout_ns = timeout_ns;
            for (std::size_t signal_index = 0;
                 signal_index < batch.signal_count; ++signal_index) {
                DeviceTimelineInfo info;
                if (!devices->timeline_info(
                        DeviceTimelineHandle{
                            batch.signals[signal_index].timeline_handle},
                        info) || info.backend != safe_phase.backend ||
                    info.last_accepted_value ==
                        std::numeric_limits<std::uint64_t>::max()) {
                    return Status::capacity_exceeded;
                }
                batch.signals[signal_index].value =
                    info.last_accepted_value + 1;
            }
            const auto now = clock_now();
            if (timeout_ns > std::numeric_limits<std::uint64_t>::max() - now) {
                return Status::capacity_exceeded;
            }
            const detail::DeviceRateReleaseIdentity identity{
                safe_phase.reference_index,
                release.domain,
                release.phase,
                0,
                0,
                release.substep_ordinal,
                now,
                now,
                now + timeout_ns,
                timeout_ns,
            };
            detail::DeviceRateTicket ticket;
            std::uint64_t batch_id = 0;
            auto status = devices->submit_batch(
                safe_phase.backend.index(), safe_phase.phase.index(),
                kNoWorker, 0, batch, declaration, batch_id, &identity, &ticket);
            detail::DeviceRateCompletion completion;
            if (status == Status::ok) {
                status = devices->wait_rate_batch(ticket, completion);
            }
            if (ticket.valid() && completion.terminal_slot_owned) {
                const auto release_status = devices->release_rate_batch(ticket);
                if (status == Status::ok) {
                    status = release_status;
                }
            }
            for (std::size_t offset = 0;
                 offset < safe_phase.channel_count; ++offset) {
                const auto sampled_index = compiled_sampled_io_plan
                    .safe_channel_indices[safe_phase.first_channel_index + offset];
                auto& channel_status = sampled_io_statuses[sampled_index];
                channel_status.safety_state = status == Status::ok
                    ? transition
                    : SampledIoSafetyState::unknown;
                channel_status.last_status = status;
                if (mixed_rate_actions) {
                    const auto& channel =
                        compiled_sampled_io_plan.channels[sampled_index];
                    const auto source = sampled_io_frame(
                        frame_offset(channel),
                        channel.public_record.frame_bytes);
                    SampledIoFrameHeader header{};
                    (void)detail::sampled_io_read_header(source, header);
                    const auto degradation =
                        degradation_level.load(std::memory_order_acquire);
                    emit_mixed_rate_action(MixedRateActionRecord{
                        mixed_rate_action_schema_version,
                        sizeof(MixedRateActionRecord),
                        0,
                        observability.runtime_id,
                        mixed_rate_closure_policy.host_policy_version,
                        active_frame ? active_frame->frame_index : 0,
                        identity.logical_release_ns,
                        identity.nominal_release_ns,
                        identity.domain_release_sequence,
                        static_cast<std::uint64_t>(safe_phase.backend.index()) + 1,
                        batch_id,
                        channel.public_record.channel_identity,
                        batch_id,
                        completion.timestamp_domain_identity,
                        completion.timestamp,
                        header.payload_checksum,
                        header.payload_checksum,
                        header.sequence,
                        0,
                        active_shedding_state ? active_shedding_state->shed_mask : 0,
                        active_shedding_state ? active_shedding_state->shed_mask : 0,
                        config.watchdog_timeout_ns,
                        release.domain.index(),
                        release.phase.index(),
                        release.substep_ordinal,
                        static_cast<std::int32_t>(status),
                        degradation,
                        degradation,
                        MixedRateActionId::safe_transition,
                        status == Status::ok
                            ? MixedRateActionReason::safe_acknowledged
                            : MixedRateActionReason::safe_failed,
                        status == Status::ok
                            ? MixedRateActionStage::acknowledged
                            : MixedRateActionStage::terminal,
                        false,
                        false,
                        false,
                        MixedRateSampleFreshness::substituted,
                        false,
                        true,
                        static_cast<MixedRateSafetyState>(
                            channel_status.safety_state),
                        {},
                    });
                }
            }
            if (status != Status::ok) {
                return status;
            }
        }
        return Status::ok;
    }

    void emit_mixed_rate_action(MixedRateActionRecord record) noexcept {
        if (!mixed_rate_actions) {
            return;
        }
        std::uint64_t assigned_sequence = 0;
        const auto retained = mixed_rate_actions->emit(
            record, &assigned_sequence);
        record.sequence = assigned_sequence;
        if (!active_replay_view || active_replay_mismatch_status != Status::ok) {
            return;
        }
        MixedRateActionRecord expected;
        if (!retained ||
            !detail::active_replay_action_at(
                *active_replay_view,
                active_replay_expected_action,
                expected) ||
            std::memcmp(&record, &expected, sizeof(record)) != 0) {
            active_replay_mismatch_sequence = assigned_sequence;
            active_replay_mismatch_status = Status::incompatible_artifact;
            return;
        }
        ++active_replay_expected_action;
        ++active_replay_actions_compared;
    }

    void emit_sampled_publication(
        std::size_t channel_index,
        std::span<const std::byte> payload,
        const ReferenceRelease& release,
        std::uint64_t domain_release_sequence,
        std::uint64_t logical_release_ns,
        std::uint64_t nominal_release_ns,
        std::uint64_t backend_identity,
        std::uint64_t batch_identity,
        std::uint64_t completion_generation,
        std::uint64_t timestamp_domain_identity,
        std::uint64_t timestamp) noexcept {
        const auto* sampled = sampled_io_record(channel_index);
        if (!mixed_rate_actions || !sampled) {
            return;
        }
        SampledIoFrameHeader header{};
        if (!detail::sampled_io_read_header(payload, header)) {
            return;
        }
        const bool substituted = header.status == static_cast<std::uint32_t>(
            SampledIoFrameStatus::safe);
        const auto degradation =
            degradation_level.load(std::memory_order_acquire);
        emit_mixed_rate_action(MixedRateActionRecord{
            mixed_rate_action_schema_version,
            sizeof(MixedRateActionRecord),
            0,
            observability.runtime_id,
            mixed_rate_closure_policy.host_policy_version,
            active_frame ? active_frame->frame_index : 0,
            logical_release_ns,
            nominal_release_ns,
            domain_release_sequence,
            backend_identity,
            batch_identity,
            sampled->public_record.channel_identity,
            completion_generation,
            timestamp_domain_identity,
            timestamp,
            content_identity(payload),
            header.payload_checksum,
            header.sequence,
            0,
            active_shedding_state ? active_shedding_state->shed_mask : 0,
            active_shedding_state ? active_shedding_state->shed_mask : 0,
            config.watchdog_timeout_ns,
            static_cast<std::uint32_t>(release.domain_registration_index),
            release.phase.index(),
            release.substep_ordinal,
            static_cast<std::int32_t>(Status::ok),
            degradation,
            degradation,
            MixedRateActionId::sampled_publish,
            substituted
                ? MixedRateActionReason::underrun
                : MixedRateActionReason::normal,
            MixedRateActionStage::published,
            release.optional,
            false,
            false,
            substituted
                ? MixedRateSampleFreshness::substituted
                : MixedRateSampleFreshness::fresh,
            false,
            substituted,
            static_cast<MixedRateSafetyState>(
                sampled_io_statuses[sampled_io_index(channel_index)]
                    .safety_state),
            {},
        });
    }

    void emit_sampled_selection(
        std::size_t channel_index,
        std::span<const std::byte> payload,
        const CrossRateReadResult& result,
        bool was_stale) noexcept {
        const auto* sampled = sampled_io_record(channel_index);
        if (!mixed_rate_actions || !sampled ||
            active_reference_index == invalid_reference_release_index) {
            return;
        }
        SampledIoFrameHeader header{};
        if (!detail::sampled_io_read_header(payload, header)) {
            return;
        }
        const auto& release =
            compiled_rate_plan.releases[active_reference_index];
        const auto degradation =
            degradation_level.load(std::memory_order_acquire);
        const auto freshness = result.sampled_substituted
            ? MixedRateSampleFreshness::substituted
            : was_stale
                ? MixedRateSampleFreshness::stale
                : result.held
                    ? MixedRateSampleFreshness::held
                    : MixedRateSampleFreshness::fresh;
        emit_mixed_rate_action(MixedRateActionRecord{
            mixed_rate_action_schema_version,
            sizeof(MixedRateActionRecord),
            0,
            observability.runtime_id,
            mixed_rate_closure_policy.host_policy_version,
            active_frame ? active_frame->frame_index : 0,
            active_logical_release_ns,
            active_nominal_release_ns,
            active_rate_view.domain_release_sequence,
            0,
            0,
            sampled->public_record.channel_identity,
            result.generation,
            result.producer_timestamp_domain_identity,
            result.producer_timestamp,
            content_identity(payload),
            header.payload_checksum,
            header.sequence,
            result.age_ns,
            active_shedding_state ? active_shedding_state->shed_mask : 0,
            active_shedding_state ? active_shedding_state->shed_mask : 0,
            config.watchdog_timeout_ns,
            static_cast<std::uint32_t>(release.domain_registration_index),
            release.phase.index(),
            release.substep_ordinal,
            static_cast<std::int32_t>(Status::ok),
            degradation,
            degradation,
            MixedRateActionId::sampled_select,
            result.sampled_substituted
                ? (sampled->public_record.direction ==
                           SampledIoDirection::output
                       ? MixedRateActionReason::underrun
                       : MixedRateActionReason::stale)
                : was_stale
                    ? MixedRateActionReason::stale
                    : MixedRateActionReason::normal,
            MixedRateActionStage::selected,
            release.optional,
            false,
            false,
            freshness,
            result.held,
            result.sampled_substituted,
            static_cast<MixedRateSafetyState>(
                sampled_io_statuses[sampled_io_index(channel_index)]
                    .safety_state),
            {},
        });
    }

    void emit_sampled_failure(
        std::size_t channel_index,
        std::span<const std::byte> payload,
        MixedRateActionId action,
        MixedRateActionReason reason,
        Status terminal_status,
        MixedRateSampleFreshness freshness,
        bool held,
        bool substituted) noexcept {
        const auto* sampled = sampled_io_record(channel_index);
        if (!mixed_rate_actions || !sampled ||
            active_reference_index == invalid_reference_release_index ||
            active_reference_index >= compiled_rate_plan.releases.size()) {
            return;
        }
        SampledIoFrameHeader header{};
        const auto has_header =
            detail::sampled_io_read_header(payload, header);
        const auto& release = compiled_rate_plan.releases[
            active_reference_index];
        const auto degradation =
            degradation_level.load(std::memory_order_acquire);
        emit_mixed_rate_action(MixedRateActionRecord{
            mixed_rate_action_schema_version,
            sizeof(MixedRateActionRecord),
            0,
            observability.runtime_id,
            mixed_rate_closure_policy.host_policy_version,
            active_frame ? active_frame->frame_index : 0,
            active_logical_release_ns,
            active_nominal_release_ns,
            active_rate_view.domain_release_sequence,
            0,
            0,
            sampled->public_record.channel_identity,
            0,
            has_header ? header.timestamp_domain_identity : 0,
            has_header ? header.first_sample_timestamp : 0,
            payload.empty() ? 0 : content_identity(payload),
            has_header ? header.payload_checksum : 0,
            has_header ? header.sequence : 0,
            0,
            active_shedding_state ? active_shedding_state->shed_mask : 0,
            active_shedding_state ? active_shedding_state->shed_mask : 0,
            config.watchdog_timeout_ns,
            static_cast<std::uint32_t>(release.domain_registration_index),
            release.phase.index(),
            release.substep_ordinal,
            static_cast<std::int32_t>(terminal_status),
            degradation,
            degradation,
            action,
            reason,
            MixedRateActionStage::terminal,
            release.optional,
            false,
            false,
            freshness,
            held,
            substituted,
            static_cast<MixedRateSafetyState>(
                sampled_io_statuses[sampled_io_index(channel_index)]
                    .safety_state),
            {},
        });
    }

    void emit_runtime_stop_action(Status status) noexcept {
        if (!mixed_rate_actions) {
            return;
        }
        std::uint64_t nominal_release_ns = 0;
        if (active_epoch_mapped &&
            !checked_time_add(
                active_nominal_epoch_ns,
                active_logical_cursor_ns,
                nominal_release_ns)) {
            nominal_release_ns = 0;
        }
        const auto degradation =
            degradation_level.load(std::memory_order_acquire);
        emit_mixed_rate_action(MixedRateActionRecord{
            mixed_rate_action_schema_version,
            sizeof(MixedRateActionRecord),
            0,
            observability.runtime_id,
            mixed_rate_closure_policy.host_policy_version,
            0,
            active_logical_cursor_ns,
            nominal_release_ns,
            0,
            0,
            0,
            0,
            0,
            0,
            clock_now(),
            0,
            0,
            0,
            0,
            active_shedding_state ? active_shedding_state->shed_mask : 0,
            active_shedding_state ? active_shedding_state->shed_mask : 0,
            config.watchdog_timeout_ns,
            std::numeric_limits<std::uint32_t>::max(),
            std::numeric_limits<std::uint32_t>::max(),
            0,
            static_cast<std::int32_t>(status),
            degradation,
            degradation,
            MixedRateActionId::runtime_stop,
            status == Status::ok
                ? MixedRateActionReason::normal
                : MixedRateActionReason::cleanup_pending,
            MixedRateActionStage::terminal,
            false,
            false,
            false,
            MixedRateSampleFreshness::not_applicable,
            false,
            false,
            MixedRateSafetyState::not_applicable,
            {},
        });
    }

    [[nodiscard]] std::uint64_t mixed_rate_decision_now(
        std::uint32_t domain_index,
        std::uint64_t release_sequence,
        std::uint64_t deadline_ns) noexcept {
        if (!active_replay_view) {
            return clock_now();
        }
        const auto available = static_cast<std::size_t>(
            active_replay_view->metadata.action_record_count);
        if (active_replay_expected_action > available) {
            return deadline_ns;
        }
        const auto remaining = available - active_replay_expected_action;
        const auto scan_count = std::min(
            remaining,
            mixed_rate_closure_policy.maximum_actions_per_step);
        const auto bound = active_replay_expected_action + scan_count;
        for (auto index = active_replay_expected_action;
             index < bound; ++index) {
            MixedRateActionRecord expected;
            if (!detail::active_replay_action_at(
                    *active_replay_view, index, expected)) {
                break;
            }
            if (expected.stage == MixedRateActionStage::decision &&
                expected.rate_domain_registration_index == domain_index &&
                expected.release_sequence == release_sequence) {
                if (!expected.late) {
                    return deadline_ns;
                }
                return deadline_ns == std::numeric_limits<std::uint64_t>::max()
                    ? deadline_ns
                    : deadline_ns + 1;
            }
        }
        // Barrier discovery probes domains that may never settle after an
        // earlier recorded terminal failure. Absence is therefore neutral at
        // this speculative point; an actually executed domain must still emit
        // and compare its exact decision action below.
        return deadline_ns;
    }

    void emit_rate_action(
        const HostFrameContext& frame,
        const CompiledRateDomain& domain,
        std::uint64_t first_sequence,
        std::uint64_t logical_release_ns,
        std::uint64_t nominal_release_ns,
        std::uint64_t release_count,
        std::uint64_t reference_record_count,
        RateActionId action,
        RateActionReason reason,
        bool late,
        const PolicyTransition& transition,
        Status status) noexcept {
        if (!rate_telemetry) {
            return;
        }
        (void)rate_telemetry->emit(RateActionRecord{
            rate_action_schema_version,
            sizeof(RateActionRecord),
            0,
            rate_execution_policy.host_policy_version,
            observability.runtime_id,
            frame.frame_index,
            first_sequence,
            logical_release_ns,
            nominal_release_ns,
            domain.period_ns,
            release_count,
            reference_record_count,
            transition.before,
            transition.after,
            static_cast<std::uint32_t>(domain.registration_index),
            transition.domain_index,
            action,
            transition.id,
            reason,
            domain.optional,
            late,
            {},
            static_cast<std::int32_t>(status),
            degradation_level.load(std::memory_order_acquire),
            {},
        });
        if (mixed_rate_actions) {
            const auto degradation_after =
                degradation_level.load(std::memory_order_acquire);
            const auto mapped_action = transition.id == RateTransitionId::shed
                ? MixedRateActionId::rate_shed
                : transition.id == RateTransitionId::recover
                    ? MixedRateActionId::rate_recover
                : action == RateActionId::skip
                    ? MixedRateActionId::rate_skip
                    : action == RateActionId::hold
                        ? MixedRateActionId::rate_hold
                        : action == RateActionId::optional_shed
                            ? MixedRateActionId::rate_shed
                            : MixedRateActionId::rate_execute;
            const auto mapped_reason = reason == RateActionReason::deadline_late
                ? MixedRateActionReason::deadline_late
                : reason == RateActionReason::already_shed
                    ? MixedRateActionReason::already_shed
                    : reason == RateActionReason::callback_failure
                        ? MixedRateActionReason::callback_failure
                        : reason == RateActionReason::late_threshold
                            ? MixedRateActionReason::late_threshold
                            : reason == RateActionReason::on_time_threshold
                                ? MixedRateActionReason::on_time_threshold
                        : MixedRateActionReason::normal;
            emit_mixed_rate_action(MixedRateActionRecord{
                mixed_rate_action_schema_version,
                sizeof(MixedRateActionRecord),
                0,
                observability.runtime_id,
                mixed_rate_closure_policy.host_policy_version,
                frame.frame_index,
                logical_release_ns,
                nominal_release_ns,
                first_sequence,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                transition.before,
                transition.after,
                config.watchdog_timeout_ns,
                static_cast<std::uint32_t>(domain.registration_index),
                std::numeric_limits<std::uint32_t>::max(),
                0,
                static_cast<std::int32_t>(status),
                action == RateActionId::execute_degraded && degradation_after != 0
                    ? degradation_after - 1
                    : degradation_after,
                degradation_after,
                mapped_action,
                mapped_reason,
                MixedRateActionStage::decision,
                domain.optional,
                late,
                mapped_action == MixedRateActionId::rate_shed,
                MixedRateSampleFreshness::not_applicable,
                false,
                false,
                MixedRateSafetyState::not_applicable,
                {},
            });
        }
    }

    [[nodiscard]] bool commit_rate_counters(
        const StepResult::RateSummary& summary) noexcept {
        if (!rate_counters) {
            return true;
        }
        const std::array<std::pair<RateCounterId, std::uint64_t>, 15> additions{{
            {RateCounterId::due_domain_releases, summary.due_domain_releases},
            {RateCounterId::executed_reference_records, summary.executed_reference_records},
            {RateCounterId::late_domain_releases, summary.late_domain_releases},
            {RateCounterId::caught_up_domain_releases, summary.caught_up_domain_releases},
            {RateCounterId::skipped_domain_releases, summary.skipped_domain_releases},
            {RateCounterId::held_domain_releases, summary.held_domain_releases},
            {RateCounterId::degraded_domain_releases, summary.degraded_domain_releases},
            {RateCounterId::failed_domain_releases, summary.failed_domain_releases},
            {RateCounterId::optional_due_domain_releases, summary.optional_due_domain_releases},
            {RateCounterId::optional_executed_domain_releases, summary.optional_executed_domain_releases},
            {RateCounterId::shed_domain_releases, summary.shed_domain_releases},
            {RateCounterId::shed_transitions, summary.shed_transitions},
            {RateCounterId::recovery_transitions, summary.recovery_transitions},
            {RateCounterId::rejected_reference_records, summary.rejected_reference_records},
            {RateCounterId::stale_reads, summary.stale_reads},
        }};
        for (const auto& [id, amount] : additions) {
            if (!rate_counters->add(id, amount)) {
                return false;
            }
        }
        rate_counters->set(
            RateCounterId::currently_shed_domains,
            summary.currently_shed_domains);
        rate_counters->set(
            RateCounterId::policy_version,
            rate_execution_policy.host_policy_version);
        return true;
    }

    Status execute_active_group(
        const detail::RateReleaseGroup& group,
        std::uint64_t supercycle_cycle,
        std::uint64_t domain_release_sequence,
        std::uint64_t logical_release_ns,
        std::uint64_t nominal_release_ns,
        std::uint64_t absolute_deadline_ns,
        StepResult::RateSummary& summary,
        std::size_t& failed_phase) noexcept {
        const auto wait_for_reference = [&](std::size_t reference_index) {
            auto& ticket = active_device_tickets[reference_index];
            if (!ticket.valid()) {
                return Status::ok;
            }
            detail::DeviceRateCompletion completion;
            auto status = devices
                ? devices->wait_rate_batch(ticket, completion)
                : Status::invalid_state;
            if (!normalize_replay_device_completion(
                    ticket, status, completion)) {
                status = Status::incompatible_artifact;
            }
            if (status == Status::ok) {
                status = capture_device_outputs(ticket, completion);
            }
            if (completion.terminal_slot_owned && devices) {
                const auto release_status =
                    devices->release_rate_batch(ticket);
                if (status == Status::ok) {
                    status = release_status;
                }
            }
            if (mixed_rate_actions) {
                const auto& identity = ticket.identity;
                const auto phase_index = identity.phase.index();
                const auto backend_index = phase_index < callbacks.size()
                    ? callbacks[phase_index].device_backend_index
                    : std::numeric_limits<std::uint32_t>::max();
                auto reason = MixedRateActionReason::normal;
                auto stage = MixedRateActionStage::terminal;
                if (status == Status::device_timeout) {
                    reason = MixedRateActionReason::timeout;
                    stage = MixedRateActionStage::quarantined;
                } else if (status == Status::device_canceled) {
                    reason = MixedRateActionReason::canceled;
                } else if (status == Status::device_lost) {
                    reason = MixedRateActionReason::lost;
                    stage = MixedRateActionStage::quarantined;
                } else if (status != Status::ok) {
                    reason = MixedRateActionReason::completion_error;
                }
                std::uint64_t payload_identity = kFnvOffset;
                std::uint64_t sampled_identity = 0;
                std::uint64_t sampled_sequence = 0;
                auto freshness = MixedRateSampleFreshness::not_applicable;
                auto safety = MixedRateSafetyState::not_applicable;
                const auto producer_begin = compiled_rate_dispatch_plan
                    .producer_channel_offsets[phase_index];
                const auto producer_end = compiled_rate_dispatch_plan
                    .producer_channel_offsets[phase_index + 1];
                for (std::size_t cursor = producer_begin;
                     cursor < producer_end; ++cursor) {
                    const auto channel_index = compiled_rate_dispatch_plan
                        .producer_channel_indices[cursor];
                    const auto& channel_state = active_channel_states[channel_index];
                    const auto payload = std::span<const std::byte>(
                        active_committed_payloads.data() +
                            channel_state.payload_offset,
                        cross_rate_channels[channel_index].payload_size);
                    for (const auto byte : payload) {
                        payload_identity ^= static_cast<std::uint8_t>(byte);
                        payload_identity *= kFnvPrime;
                    }
                    const auto* sampled = sampled_io_record(channel_index);
                    if (sampled) {
                        SampledIoFrameHeader header{};
                        if (detail::sampled_io_read_header(payload, header)) {
                            sampled_identity = header.payload_checksum;
                            sampled_sequence = header.sequence;
                            freshness = MixedRateSampleFreshness::fresh;
                        }
                        const auto sampled_status = sampled_io_statuses[
                            sampled_io_index(channel_index)].safety_state;
                        safety = static_cast<MixedRateSafetyState>(sampled_status);
                    }
                }
                const auto degradation =
                    degradation_level.load(std::memory_order_acquire);
                auto recorded_batch_identity = ticket.batch_id;
                auto recorded_completion_generation = ticket.batch_id;
                if (active_replay_view) {
                    MixedRateActionRecord expected;
                    if (detail::active_replay_action_at(
                            *active_replay_view,
                            active_replay_expected_action,
                            expected) &&
                        expected.action ==
                            MixedRateActionId::device_terminal) {
                        recorded_batch_identity = expected.batch_identity;
                        recorded_completion_generation =
                            expected.completion_generation;
                    }
                }
                emit_mixed_rate_action(MixedRateActionRecord{
                    mixed_rate_action_schema_version,
                    sizeof(MixedRateActionRecord),
                    0,
                    observability.runtime_id,
                    mixed_rate_closure_policy.host_policy_version,
                    active_frame ? active_frame->frame_index : 0,
                    identity.logical_release_ns,
                    identity.nominal_release_ns,
                    identity.domain_release_sequence,
                    static_cast<std::uint64_t>(backend_index) + 1,
                    recorded_batch_identity,
                    0,
                    recorded_completion_generation,
                    completion.timestamp_domain_identity,
                    completion.timestamp,
                    payload_identity,
                    sampled_identity,
                    sampled_sequence,
                    0,
                    active_shedding_state ? active_shedding_state->shed_mask : 0,
                    active_shedding_state ? active_shedding_state->shed_mask : 0,
                    config.watchdog_timeout_ns,
                    identity.domain.index(),
                    phase_index,
                    identity.substep_ordinal,
                    static_cast<std::int32_t>(status),
                    degradation,
                    degradation,
                    MixedRateActionId::device_terminal,
                    reason,
                    stage,
                    compiled_rate_plan.domains[identity.domain.index()].optional,
                    false,
                    false,
                    freshness,
                    false,
                    false,
                    safety,
                    {},
                });
                if (status == Status::ok) {
                    const auto& release =
                        compiled_rate_plan.releases[reference_index];
                    for (std::size_t cursor = producer_begin;
                         cursor < producer_end; ++cursor) {
                        const auto channel_index = compiled_rate_dispatch_plan
                            .producer_channel_indices[cursor];
                        const auto& channel_state =
                            active_channel_states[channel_index];
                        emit_sampled_publication(
                            channel_index,
                            std::span<const std::byte>(
                                active_committed_payloads.data() +
                                    channel_state.payload_offset,
                                cross_rate_channels[channel_index]
                                    .payload_size),
                            release,
                            identity.domain_release_sequence,
                            identity.logical_release_ns,
                            identity.nominal_release_ns,
                            static_cast<std::uint64_t>(backend_index) + 1,
                            recorded_batch_identity,
                            recorded_completion_generation,
                            completion.timestamp_domain_identity,
                            completion.timestamp);
                    }
                }
            }
            ticket = {};
            if (status != Status::ok) {
                const auto& failed = compiled_rate_plan.releases[
                    reference_index];
                failed_phase = failed.phase.index();
                record_active_failure(
                    summary, failed, domain_release_sequence);
            }
            return status;
        };
        for (std::size_t record_offset = 0;
             record_offset < group.reference_count;
             ++record_offset) {
            const auto reference_index =
                group.first_reference_index + record_offset;
            const auto& release =
                compiled_rate_plan.releases[reference_index];
            const auto channel_begin = compiled_rate_dispatch_plan
                .producer_channel_offsets[release.phase.index()];
            const auto channel_end = compiled_rate_dispatch_plan
                .producer_channel_offsets[release.phase.index() + 1];
            const auto dependency_begin =
                compiled_rate_dispatch_plan
                    .device_dependency_offsets[reference_index];
            const auto dependency_end =
                compiled_rate_dispatch_plan
                    .device_dependency_offsets[reference_index + 1];
            for (std::size_t cursor = dependency_begin;
                 cursor < dependency_end; ++cursor) {
                const auto dependency = compiled_rate_dispatch_plan
                    .device_dependencies[cursor];
                const auto dependency_status =
                    wait_for_reference(dependency);
                if (dependency_status != Status::ok) {
                    active_reference_index =
                        invalid_reference_release_index;
                    return dependency_status;
                }
            }
            for (std::size_t channel_cursor = channel_begin;
                 channel_cursor < channel_end; ++channel_cursor) {
                const auto channel_index = compiled_rate_dispatch_plan
                    .producer_channel_indices[channel_cursor];
                if (active_channel_states[channel_index].next_generation ==
                    0) {
                    active_rate_summary = &summary;
                    set_active_failure(release, domain_release_sequence);
                    active_rate_summary = nullptr;
                    active_reference_index =
                        invalid_reference_release_index;
                    return Status::capacity_exceeded;
                }
                active_publication_claims[channel_index].store(
                    0,
                    std::memory_order_relaxed);
            }
            active_reference_index = reference_index;
            active_supercycle_cycle = supercycle_cycle;
            active_logical_release_ns = logical_release_ns;
            active_nominal_release_ns = nominal_release_ns;
            active_absolute_deadline_ns = absolute_deadline_ns;
            active_late_action = release.late_action;
            active_rate_view = {
                release.domain,
                release.phase,
                supercycle_cycle,
                domain_release_sequence,
                release.substep_ordinal,
                logical_release_ns,
                nominal_release_ns,
                absolute_deadline_ns,
                release.budget_wcet_ns,
                release.late_action,
                degradation_level.load(std::memory_order_acquire),
                this,
                &Impl::publish_active_channel,
                &Impl::copy_active_channel,
            };
            if (live_control_mailboxes) {
                (void)live_control_mailboxes->close_rate_release(
                    reference_index,
                    domain_release_sequence);
            }
            active_device_ticket = {};
            active_device_payload_slot = 0;
            if (release.phase_kind == RatePhaseKind::device) {
                if (!device_payload_slot(
                        reference_index,
                        release,
                        domain_release_sequence,
                        active_device_payload_slot)) {
                    failed_phase = release.phase.index();
                    record_active_failure(
                        summary, release, domain_release_sequence);
                    active_reference_index =
                        invalid_reference_release_index;
                    return Status::capacity_exceeded;
                }
                const auto input_status = prepare_device_inputs(
                    release, active_device_payload_slot);
                if (input_status != Status::ok) {
                    failed_phase = release.phase.index();
                    record_active_failure(
                        summary, release, domain_release_sequence);
                    active_reference_index =
                        invalid_reference_release_index;
                    return input_status;
                }
            }
            const auto status = executor->run_selected(
                release.phase.index(),
                &Impl::run_phase,
                this);
            ++summary.executed_reference_records;
            if (status != Status::ok) {
                failed_phase = release.phase.index();
                record_active_failure(
                    summary,
                    release,
                    domain_release_sequence);
                active_reference_index = invalid_reference_release_index;
                return status;
            }
            if (release.phase_kind == RatePhaseKind::device) {
                if (!active_device_ticket.valid()) {
                    failed_phase = release.phase.index();
                    record_active_failure(
                        summary, release, domain_release_sequence);
                    active_reference_index =
                        invalid_reference_release_index;
                    return Status::internal_error;
                }
                active_device_tickets[reference_index] =
                    active_device_ticket;
            }
            for (std::size_t channel_cursor = channel_begin;
                 channel_cursor < channel_end; ++channel_cursor) {
                const auto channel_index = compiled_rate_dispatch_plan
                    .producer_channel_indices[channel_cursor];
                if (compiled_cross_rate_plan.channels[channel_index]
                        .producer_device.valid()) {
                    continue;
                }
                if (active_publication_claims[channel_index].load(
                        std::memory_order_acquire) != 2) {
                    const auto* sampled = sampled_io_record(channel_index);
                    if (sampled && sampled->public_record.direction ==
                            SampledIoDirection::output &&
                        sampled->public_record.underrun_policy ==
                            SampledIoUnderrunPolicy::substitute_safe) {
                        const auto frame = sampled_io_frame(
                            sampled->failure_safe_offset,
                            sampled->public_record.frame_bytes);
                        auto& sampled_status = sampled_io_statuses[
                            sampled_io_index(channel_index)];
                        ++sampled_status.underruns;
                        if (frame.size() ==
                            cross_rate_channels[channel_index].payload_size) {
                            std::copy(
                                frame.begin(), frame.end(),
                                active_staging_payloads.begin() +
                                    static_cast<std::ptrdiff_t>(
                                        active_channel_states[channel_index]
                                            .payload_offset));
                            active_publication_claims[channel_index].store(
                                2, std::memory_order_release);
                            ++sampled_status.substituted_frames;
                            sampled_status.last_status = Status::ok;
                            continue;
                        }
                    } else if (sampled) {
                        auto& sampled_status = sampled_io_statuses[
                            sampled_io_index(channel_index)];
                        ++sampled_status.underruns;
                        sampled_status.last_status = Status::callback_failed;
                    }
                    if (sampled) {
                        emit_sampled_failure(
                            channel_index,
                            {},
                            MixedRateActionId::sampled_publish,
                            MixedRateActionReason::underrun,
                            Status::callback_failed,
                            MixedRateSampleFreshness::stale,
                            false,
                            false);
                    }
                    failed_phase = release.phase.index();
                    record_active_failure(
                        summary,
                        release,
                        domain_release_sequence);
                    active_reference_index =
                        invalid_reference_release_index;
                    return Status::callback_failed;
                }
            }
            for (std::size_t channel_cursor = channel_begin;
                 channel_cursor < channel_end; ++channel_cursor) {
                const auto channel_index = compiled_rate_dispatch_plan
                    .producer_channel_indices[channel_cursor];
                if (compiled_cross_rate_plan.channels[channel_index]
                        .producer_device.valid()) {
                    continue;
                }
                auto& channel_state = active_channel_states[channel_index];
                auto& store = compiled_cross_rate_plan.stores[channel_index];
                if (!store.can_publish(channel_state.next_generation)) {
                    const auto slots = static_cast<std::uint64_t>(
                        store.slot_count());
                    if (channel_state.next_generation <= slots ||
                        store.retire(
                            channel_state.next_generation - slots) !=
                            detail::SnapshotStoreResult::ok) {
                        failed_phase = release.phase.index();
                        active_rate_summary = &summary;
                        set_active_failure(
                            release,
                            domain_release_sequence);
                        active_rate_summary = nullptr;
                        active_reference_index =
                            invalid_reference_release_index;
                        return Status::internal_error;
                    }
                }
                if (store.publish(
                        channel_state.next_generation,
                        std::span<const std::byte>(
                            active_staging_payloads.data() +
                                channel_state.payload_offset,
                            cross_rate_channels[channel_index].payload_size)) !=
                        detail::SnapshotStoreResult::ok) {
                    failed_phase = release.phase.index();
                    active_rate_summary = &summary;
                    set_active_failure(release, domain_release_sequence);
                    active_rate_summary = nullptr;
                    active_reference_index =
                        invalid_reference_release_index;
                    return Status::internal_error;
                }
                std::copy_n(
                    active_staging_payloads.begin() +
                        static_cast<std::ptrdiff_t>(
                            channel_state.payload_offset),
                    cross_rate_channels[channel_index].payload_size,
                    active_committed_payloads.begin() +
                        static_cast<std::ptrdiff_t>(
                            channel_state.payload_offset));
                channel_state.generation = channel_state.next_generation;
                channel_state.next_generation = store.next_generation();
                channel_state.source_logical_release_ns = logical_release_ns;
                channel_state.provenance =
                    CrossRateSampleProvenance::produced;
                channel_state.held = false;
                channel_state.producer_release_sequence =
                    domain_release_sequence;
                channel_state.producer_substep_ordinal =
                    release.substep_ordinal;
                channel_state.producer_completion_status = Status::ok;
                channel_state.producer_timestamp_domain_identity =
                    cross_rate_runtime_logical_timestamp_domain_identity;
                channel_state.producer_timestamp = logical_release_ns;
                emit_sampled_publication(
                    channel_index,
                    std::span<const std::byte>(
                        active_committed_payloads.data() +
                            channel_state.payload_offset,
                        cross_rate_channels[channel_index].payload_size),
                    release,
                    domain_release_sequence,
                    logical_release_ns,
                    nominal_release_ns,
                    0,
                    0,
                    channel_state.generation,
                    cross_rate_runtime_logical_timestamp_domain_identity,
                    logical_release_ns);
            }
        }

        const auto group_end =
            group.first_reference_index + group.reference_count;
        for (std::size_t reference_index = group.first_reference_index;
             reference_index < group_end; ++reference_index) {
            const auto status = wait_for_reference(reference_index);
            if (status != Status::ok) {
                active_reference_index = invalid_reference_release_index;
                return status;
            }
        }

        active_reference_index = invalid_reference_release_index;
        return Status::ok;
    }

    Status run_active_step(
        const HostFrameContext& frame,
        StepResult& output,
        std::size_t& failed_phase) noexcept {
        if (active_faulted) {
            return Status::invalid_state;
        }
        if (frame.delta.count() <= 0 || !frame.nominal_release_ns) {
            return Status::invalid_argument;
        }

        const auto delta_ns =
            static_cast<std::uint64_t>(frame.delta.count());
        std::uint64_t end_ns = 0;
        if (!checked_time_add(active_logical_cursor_ns, delta_ns, end_ns)) {
            return Status::invalid_argument;
        }

        std::uint64_t epoch_ns = active_nominal_epoch_ns;
        if (active_epoch_mapped) {
            std::uint64_t expected_nominal_ns = 0;
            if (!checked_time_add(
                    active_nominal_epoch_ns,
                    active_logical_cursor_ns,
                    expected_nominal_ns) ||
                *frame.nominal_release_ns != expected_nominal_ns) {
                return Status::invalid_argument;
            }
        } else {
            if (active_logical_cursor_ns != 0) {
                return Status::invalid_state;
            }
            epoch_ns = *frame.nominal_release_ns;
        }

        std::uint64_t nominal_end_ns = 0;
        if (!checked_time_add(epoch_ns, end_ns, nominal_end_ns)) {
            return Status::invalid_argument;
        }
        (void)nominal_end_ns;

        detail::RateDueCounts due_counts;
        const auto count_status = detail::count_due_rate_work(
            compiled_rate_plan,
            compiled_rate_dispatch_plan,
            active_logical_cursor_ns,
            end_ns,
            due_counts);
        if (count_status != Status::ok) {
            return count_status;
        }
        output.rate.due_domain_releases = due_counts.domain_releases;
        output.rate.due_reference_records = due_counts.reference_records;
        output.rate.rate_policy_version =
            rate_execution_policy.host_policy_version;
        output.rate.currently_shed_domains = active_shedding_state
            ? static_cast<std::uint64_t>(
                  std::popcount(active_shedding_state->shed_mask))
            : std::uint64_t{0};
        for (std::size_t domain_index = 0;
             domain_index < compiled_rate_plan.domains.size();
             ++domain_index) {
            if (!compiled_rate_plan.domains[domain_index].optional) {
                continue;
            }
            std::uint64_t optional_releases = 0;
            std::uint64_t optional_records = 0;
            if (detail::count_due_domain_releases(
                    compiled_rate_plan,
                    compiled_rate_dispatch_plan,
                    domain_index,
                    active_logical_cursor_ns,
                    end_ns,
                    optional_releases,
                    optional_records) != Status::ok ||
                !checked_time_add(
                    output.rate.optional_due_domain_releases,
                    optional_releases,
                    output.rate.optional_due_domain_releases)) {
                return Status::capacity_exceeded;
            }
            (void)optional_records;
        }
        active_stale_reads.store(0, std::memory_order_relaxed);

        const auto ceil_sequence = [](std::uint64_t value,
                                      std::uint64_t period) noexcept {
            return value / period + (value % period != 0 ? 1u : 0u);
        };
        std::array<std::uint64_t, rate_domain_capacity> next_sequence{};
        std::array<std::uint64_t, rate_domain_capacity> end_sequence{};
        std::array<std::uint32_t, rate_domain_capacity> catch_up_counts{};
        for (std::size_t domain_index = 0;
             domain_index < compiled_rate_plan.domains.size();
             ++domain_index) {
            const auto period =
                compiled_rate_plan.domains[domain_index].period_ns;
            next_sequence[domain_index] =
                ceil_sequence(active_logical_cursor_ns, period);
            end_sequence[domain_index] = ceil_sequence(end_ns, period);
        }

        std::uint64_t settled_records = 0;
        Status execution_status = Status::ok;
        bool done = false;
        while (!done) {
            std::uint64_t optional_mask = 0;
            for (const auto optional_index :
                 compiled_rate_dispatch_plan.optional_shed_order) {
                optional_mask |= std::uint64_t{1} << optional_index;
            }
            const bool aggregate_mandatory_omissions =
                compiled_rate_dispatch_plan.optional_shed_order.empty() ||
                (active_shedding_state &&
                 (active_shedding_state->shed_mask & optional_mask) ==
                     optional_mask);
            std::uint64_t barrier_ns = end_ns;
            for (std::size_t domain_index = 0;
                 domain_index < compiled_rate_plan.domains.size();
                 ++domain_index) {
                if (next_sequence[domain_index] >=
                    end_sequence[domain_index]) {
                    continue;
                }
                const auto& domain =
                    compiled_rate_plan.domains[domain_index];
                std::uint64_t next_release_ns = 0;
                std::uint64_t next_nominal_ns = 0;
                std::uint64_t next_deadline_ns = 0;
                if (!checked_time_multiply(
                        next_sequence[domain_index],
                        domain.period_ns,
                        next_release_ns) ||
                    !checked_time_add(
                        epoch_ns,
                        next_release_ns,
                        next_nominal_ns) ||
                    !checked_time_add(
                        next_nominal_ns,
                        domain.relative_deadline_ns,
                        next_deadline_ns)) {
                    execution_status = Status::capacity_exceeded;
                    done = true;
                    break;
                }
                const bool already_shed = domain.optional &&
                    active_shedding_state &&
                    (active_shedding_state->shed_mask &
                     (std::uint64_t{1} << domain_index)) != 0;
                const auto domain_decision_now_ns = mixed_rate_decision_now(
                    static_cast<std::uint32_t>(domain_index),
                    next_sequence[domain_index],
                    next_deadline_ns);
                const bool late_omission =
                    domain_decision_now_ns > next_deadline_ns &&
                    (domain.late_action == RateLateAction::skip ||
                     domain.late_action == RateLateAction::hold);
                const bool omission =
                    already_shed ||
                    (late_omission &&
                     (domain.optional || aggregate_mandatory_omissions));
                if (!omission) {
                    barrier_ns = std::min(barrier_ns, next_release_ns);
                    continue;
                }
                if (domain_decision_now_ns > next_deadline_ns) {
                    const auto late_end_ns = domain_decision_now_ns - epoch_ns -
                        domain.relative_deadline_ns;
                    const auto first_nonlate = std::max(
                        next_sequence[domain_index],
                        ceil_sequence(late_end_ns, domain.period_ns));
                    if (first_nonlate < end_sequence[domain_index]) {
                        std::uint64_t first_nonlate_ns = 0;
                        if (!checked_time_multiply(
                                first_nonlate,
                                domain.period_ns,
                                first_nonlate_ns)) {
                            execution_status = Status::capacity_exceeded;
                            done = true;
                            break;
                        }
                        barrier_ns = std::min(barrier_ns, first_nonlate_ns);
                    }
                }
            }
            if (done) {
                break;
            }

            for (std::size_t domain_index = 0;
                 domain_index < compiled_rate_plan.domains.size();
                 ++domain_index) {
                const auto& domain =
                    compiled_rate_plan.domains[domain_index];
                const bool already_shed = domain.optional &&
                    active_shedding_state &&
                    (active_shedding_state->shed_mask &
                     (std::uint64_t{1} << domain_index)) != 0;
                if (!already_shed &&
                    domain.late_action != RateLateAction::skip &&
                    domain.late_action != RateLateAction::hold) {
                    continue;
                }
                const auto target = std::min(
                    end_sequence[domain_index],
                    ceil_sequence(barrier_ns, domain.period_ns));
                if (target <= next_sequence[domain_index]) {
                    continue;
                }
                const auto releases =
                    target - next_sequence[domain_index];
                const auto first_sequence = next_sequence[domain_index];
                std::uint64_t records = 0;
                std::uint64_t first_logical_release_ns = 0;
                std::uint64_t first_nominal_release_ns = 0;
                if (!checked_time_multiply(
                        releases,
                        compiled_rate_dispatch_plan
                            .records_per_domain_release[domain_index],
                        records) ||
                    !checked_time_multiply(
                        first_sequence,
                        domain.period_ns,
                        first_logical_release_ns) ||
                    !checked_time_add(
                        epoch_ns,
                        first_logical_release_ns,
                        first_nominal_release_ns)) {
                    execution_status = Status::capacity_exceeded;
                    done = true;
                    break;
                }
                std::uint64_t first_deadline_ns = 0;
                const bool range_late = checked_time_add(
                        first_nominal_release_ns,
                        domain.relative_deadline_ns,
                        first_deadline_ns) &&
                    mixed_rate_decision_now(
                        static_cast<std::uint32_t>(domain_index),
                        first_sequence,
                        first_deadline_ns) > first_deadline_ns;
                settled_records += records;
                if (range_late) {
                    output.rate.late_domain_releases += releases;
                } else {
                    output.rate.on_time_domain_releases += releases;
                }
                if (already_shed) {
                    output.rate.shed_domain_releases += releases;
                } else if (domain.late_action == RateLateAction::skip) {
                    output.rate.skipped_domain_releases += releases;
                } else {
                    output.rate.held_domain_releases += releases;
                    for (std::size_t channel_index = 0;
                         channel_index <
                             compiled_cross_rate_plan.channels.size();
                         ++channel_index) {
                        if (compiled_cross_rate_plan.channels[channel_index]
                                .producer_domain == domain.domain) {
                            active_channel_states[channel_index].held = true;
                        }
                    }
                }
                emit_rate_action(
                    frame,
                    domain,
                    first_sequence,
                    first_logical_release_ns,
                    first_nominal_release_ns,
                    releases,
                    records,
                    already_shed
                        ? RateActionId::optional_shed
                        : domain.late_action == RateLateAction::skip
                            ? RateActionId::skip
                            : RateActionId::hold,
                    already_shed
                        ? RateActionReason::already_shed
                        : RateActionReason::deadline_late,
                    range_late,
                    current_policy_state(
                        already_shed
                            ? RateActionReason::already_shed
                            : RateActionReason::deadline_late),
                    Status::ok);
                if (!domain.optional && active_shedding_state && releases != 0) {
                    active_shedding_state->consecutive_on_time = 0;
                    active_shedding_state->consecutive_late =
                        rate_execution_policy.consecutive_late_threshold;
                }
                next_sequence[domain_index] = target;
            }
            if (done) {
                break;
            }

            std::uint64_t next_release_ns = end_ns;
            bool has_release = false;
            for (std::size_t domain_index = 0;
                 domain_index < compiled_rate_plan.domains.size();
                 ++domain_index) {
                if (next_sequence[domain_index] >=
                    end_sequence[domain_index]) {
                    continue;
                }
                std::uint64_t candidate_ns = 0;
                if (!checked_time_multiply(
                        next_sequence[domain_index],
                        compiled_rate_plan.domains[domain_index].period_ns,
                        candidate_ns)) {
                    execution_status = Status::capacity_exceeded;
                    done = true;
                    break;
                }
                next_release_ns = std::min(next_release_ns, candidate_ns);
                has_release = true;
            }
            if (done || !has_release) {
                break;
            }

            for (std::size_t domain_index = 0;
                 domain_index < compiled_rate_plan.domains.size();
                 ++domain_index) {
                if (next_sequence[domain_index] >=
                    end_sequence[domain_index]) {
                    continue;
                }
                const auto& domain =
                    compiled_rate_plan.domains[domain_index];
                std::uint64_t logical_release_ns = 0;
                if (!checked_time_multiply(
                        next_sequence[domain_index],
                        domain.period_ns,
                        logical_release_ns)) {
                    execution_status = Status::capacity_exceeded;
                    done = true;
                    break;
                }
                if (logical_release_ns != next_release_ns) {
                    continue;
                }
                const auto within_cycle = next_sequence[domain_index] %
                    domain.releases_per_supercycle;
                const auto group_slice_begin =
                    compiled_rate_dispatch_plan
                        .domain_group_offsets[domain_index];
                const auto group_slice_end =
                    compiled_rate_dispatch_plan
                        .domain_group_offsets[domain_index + 1];
                if (within_cycle >= group_slice_end - group_slice_begin) {
                    execution_status = Status::internal_error;
                    done = true;
                    break;
                }
                const auto group_index =
                    compiled_rate_dispatch_plan.domain_group_indices[
                        group_slice_begin +
                        static_cast<std::size_t>(within_cycle)];
                const auto& group =
                    compiled_rate_dispatch_plan.groups[group_index];
                const auto& release = compiled_rate_plan.releases[
                    group.first_reference_index];
                std::uint64_t nominal_release_ns = 0;
                std::uint64_t absolute_deadline_ns = 0;
                if (!checked_time_add(
                        epoch_ns,
                        logical_release_ns,
                        nominal_release_ns) ||
                    !checked_time_add(
                        nominal_release_ns,
                        release.relative_deadline_ns,
                        absolute_deadline_ns)) {
                    execution_status = Status::capacity_exceeded;
                    done = true;
                    break;
                }
                const bool late = mixed_rate_decision_now(
                    static_cast<std::uint32_t>(domain_index),
                    next_sequence[domain_index],
                    absolute_deadline_ns) > absolute_deadline_ns;
                if (domain.optional && active_shedding_state &&
                    (active_shedding_state->shed_mask &
                     (std::uint64_t{1} << domain_index)) != 0) {
                    ++output.rate.shed_domain_releases;
                    settled_records += group.reference_count;
                    PolicyTransition transition;
                    transition.before = active_shedding_state->shed_mask;
                    transition.after = transition.before;
                    emit_rate_action(
                        frame,
                        domain,
                        next_sequence[domain_index],
                        logical_release_ns,
                        nominal_release_ns,
                        1,
                        group.reference_count,
                        RateActionId::optional_shed,
                        RateActionReason::already_shed,
                        late,
                        transition,
                        Status::ok);
                    ++next_sequence[domain_index];
                    continue;
                }
                if (late && release.late_action == RateLateAction::skip) {
                    ++output.rate.late_domain_releases;
                    ++output.rate.skipped_domain_releases;
                    settled_records += group.reference_count;
                    const auto transition = domain.optional
                        ? current_policy_state(RateActionReason::deadline_late)
                        : observe_mandatory_release(true, output.rate);
                    emit_rate_action(
                        frame, domain, next_sequence[domain_index],
                        logical_release_ns, nominal_release_ns, 1,
                        group.reference_count, RateActionId::skip,
                        transition.id == RateTransitionId::none
                            ? RateActionReason::deadline_late
                            : transition.reason,
                        true, transition, Status::ok);
                    ++next_sequence[domain_index];
                    continue;
                }
                if (late && release.late_action == RateLateAction::hold) {
                    ++output.rate.late_domain_releases;
                    ++output.rate.held_domain_releases;
                    for (std::size_t channel_index = 0;
                         channel_index <
                             compiled_cross_rate_plan.channels.size();
                         ++channel_index) {
                        if (compiled_cross_rate_plan.channels[channel_index]
                                .producer_domain == release.domain) {
                            active_channel_states[channel_index].held = true;
                        }
                    }
                    settled_records += group.reference_count;
                    const auto transition = domain.optional
                        ? current_policy_state(RateActionReason::deadline_late)
                        : observe_mandatory_release(true, output.rate);
                    emit_rate_action(
                        frame, domain, next_sequence[domain_index],
                        logical_release_ns, nominal_release_ns, 1,
                        group.reference_count, RateActionId::hold,
                        transition.id == RateTransitionId::none
                            ? RateActionReason::deadline_late
                            : transition.reason,
                        true, transition, Status::ok);
                    ++next_sequence[domain_index];
                    continue;
                }
                if (late && release.late_action == RateLateAction::fail) {
                    ++output.rate.late_domain_releases;
                    ++output.rate.failed_domain_releases;
                    output.rate.rejected_reference_records +=
                        due_counts.reference_records - settled_records;
                    active_logical_release_ns = logical_release_ns;
                    active_rate_summary = &output.rate;
                    set_active_failure(
                        release,
                        next_sequence[domain_index]);
                    active_rate_summary = nullptr;
                    PolicyTransition transition;
                    transition.before = active_shedding_state
                        ? active_shedding_state->shed_mask
                        : 0;
                    transition.after = transition.before;
                    emit_rate_action(
                        frame, domain, next_sequence[domain_index],
                        logical_release_ns, nominal_release_ns, 1,
                        group.reference_count, RateActionId::fail,
                        RateActionReason::deadline_late, true, transition,
                        Status::callback_failed);
                    execution_status = Status::callback_failed;
                    done = true;
                    break;
                }
                if (late && release.late_action ==
                        RateLateAction::bounded_catch_up &&
                    catch_up_counts[domain_index] >=
                        release.bounded_catch_up_limit) {
                    output.rate.rejected_reference_records +=
                        due_counts.reference_records - settled_records;
                    PolicyTransition transition;
                    transition.before = active_shedding_state
                        ? active_shedding_state->shed_mask
                        : 0;
                    transition.after = transition.before;
                    emit_rate_action(
                        frame, domain, next_sequence[domain_index],
                        logical_release_ns, nominal_release_ns, 1,
                        group.reference_count,
                        RateActionId::execute_catch_up,
                        RateActionReason::dispatch_capacity, true,
                        transition, Status::capacity_exceeded);
                    execution_status = Status::capacity_exceeded;
                    done = true;
                    break;
                }
                if (output.rate.executed_reference_records +
                        group.reference_count >
                    compiled_rate_dispatch_plan.policy
                        .maximum_dispatch_records_per_step) {
                    output.rate.rejected_reference_records +=
                        due_counts.reference_records - settled_records;
                    PolicyTransition transition;
                    transition.before = active_shedding_state
                        ? active_shedding_state->shed_mask
                        : 0;
                    transition.after = transition.before;
                    emit_rate_action(
                        frame, domain, next_sequence[domain_index],
                        logical_release_ns, nominal_release_ns, 1,
                        group.reference_count,
                        late && release.late_action == RateLateAction::bounded_catch_up
                            ? RateActionId::execute_catch_up
                            : RateActionId::execute_on_time,
                        RateActionReason::dispatch_capacity, late,
                        transition, Status::capacity_exceeded);
                    execution_status = Status::capacity_exceeded;
                    done = true;
                    break;
                }

                if (late) {
                    ++output.rate.late_domain_releases;
                    if (release.late_action ==
                        RateLateAction::bounded_catch_up) {
                        ++catch_up_counts[domain_index];
                        ++output.rate.caught_up_domain_releases;
                    } else if (release.late_action ==
                               RateLateAction::degrade) {
                        const auto current = degradation_level.load(
                            std::memory_order_relaxed);
                        if (current <
                            config.watchdog_max_degradation_level) {
                            degradation_level.store(
                                current + 1,
                                std::memory_order_release);
                        }
                        ++output.rate.degraded_domain_releases;
                    }
                } else {
                    ++output.rate.on_time_domain_releases;
                }

                const auto supercycle_cycle =
                    next_sequence[domain_index] /
                    domain.releases_per_supercycle;
                const auto executed_before_group =
                    output.rate.executed_reference_records;
                const auto group_status = execute_active_group(
                    group,
                    supercycle_cycle,
                    next_sequence[domain_index],
                    logical_release_ns,
                    nominal_release_ns,
                    absolute_deadline_ns,
                    output.rate,
                    failed_phase);
                if (group_status != Status::ok) {
                    ++output.rate.failed_domain_releases;
                    const auto attempted_in_group =
                        output.rate.executed_reference_records -
                        executed_before_group;
                    output.rate.rejected_reference_records +=
                        due_counts.reference_records - settled_records -
                        attempted_in_group;
                    active_logical_cursor_ns = end_ns;
                    active_nominal_epoch_ns = epoch_ns;
                    active_epoch_mapped = true;
                    bool terminal_device_failure = false;
                    for (std::size_t record_offset = 0;
                         record_offset < group.reference_count;
                         ++record_offset) {
                        const auto& failed_release =
                            compiled_rate_plan.releases[
                                group.first_reference_index + record_offset];
                        terminal_device_failure =
                            terminal_device_failure ||
                            (failed_release.phase.index() == failed_phase &&
                             failed_release.phase_kind ==
                                 RatePhaseKind::device);
                    }
                    if (terminal_device_failure) {
                        active_rate_summary = &output.rate;
                        set_active_failure(
                            release, next_sequence[domain_index]);
                        active_rate_summary = nullptr;
                    }
                    PolicyTransition transition;
                    transition.before = active_shedding_state
                        ? active_shedding_state->shed_mask
                        : 0;
                    transition.after = transition.before;
                    emit_rate_action(
                        frame, domain, next_sequence[domain_index],
                        logical_release_ns, nominal_release_ns, 1,
                        group.reference_count,
                        late && release.late_action == RateLateAction::bounded_catch_up
                            ? RateActionId::execute_catch_up
                            : late && release.late_action == RateLateAction::degrade
                                ? RateActionId::execute_degraded
                                : RateActionId::execute_on_time,
                        RateActionReason::callback_failure, late,
                        transition, group_status);
                    execution_status = group_status;
                    done = true;
                    break;
                }
                if (domain.optional) {
                    ++output.rate.optional_executed_domain_releases;
                }
                const auto transition = domain.optional
                    ? current_policy_state(
                          late ? RateActionReason::deadline_late
                               : RateActionReason::on_time)
                    : observe_mandatory_release(late, output.rate);
                emit_rate_action(
                    frame, domain, next_sequence[domain_index],
                    logical_release_ns, nominal_release_ns, 1,
                    group.reference_count,
                    late && release.late_action == RateLateAction::bounded_catch_up
                        ? RateActionId::execute_catch_up
                        : late && release.late_action == RateLateAction::degrade
                            ? RateActionId::execute_degraded
                            : RateActionId::execute_on_time,
                    transition.id == RateTransitionId::none
                        ? (late ? RateActionReason::deadline_late
                                : RateActionReason::on_time)
                        : transition.reason,
                    late, transition, Status::ok);
                settled_records += group.reference_count;
                ++next_sequence[domain_index];
            }
        }

        output.callbacks_executed = static_cast<std::size_t>(
            output.rate.executed_reference_records);
        output.rate.stale_reads =
            active_stale_reads.load(std::memory_order_acquire);
        if (live_control_mailboxes) {
            live_control_mailboxes->expire_rate_releases_before(
                execution_status == Status::ok ||
                        (execution_status == Status::capacity_exceeded &&
                         !active_faulted)
                    ? end_ns
                    : active_logical_release_ns);
        }
        if (execution_status == Status::ok ||
            (execution_status == Status::capacity_exceeded &&
             !active_faulted)) {
            active_logical_cursor_ns = end_ns;
            active_nominal_epoch_ns = epoch_ns;
            active_epoch_mapped = true;
        } else if (active_faulted) {
            active_logical_cursor_ns = active_logical_release_ns;
            active_nominal_epoch_ns = epoch_ns;
            active_epoch_mapped = true;
        }
        return execution_status;
    }

    static detail::PhaseTaskDispatch run_phase(
        void* opaque,
        std::uint32_t phase_index,
        const TaskContext& task_context) {
        auto& self = *static_cast<Impl*>(opaque);
        const auto index = static_cast<std::size_t>(phase_index);
        if (index >= self.callbacks.size() || self.active_frame == nullptr) {
            return {Status::callback_failed, false};
        }

        auto& callback = self.callbacks[index];
        struct CallbackMarker {
            explicit CallbackMarker(const void* runtime) noexcept
                : previous(g_active_runtime_callback) {
                g_active_runtime_callback = runtime;
            }
            ~CallbackMarker() { g_active_runtime_callback = previous; }
            const void* previous;
        } callback_marker{&self};
        self.record(
            RuntimeTraceEventType::callback_begin,
            Status::ok,
            self.clock_now(),
            self.active_frame->frame_index,
            index,
            RuntimeTraceProducer::worker,
            task_context.worker_index(),
            task_context.task_index());

        std::span<std::byte> phase_scratch;
        if (self.config.scratch_bytes != 0) {
            auto backing = self.resident_regions->span(
                memory_region_phase_scratch);
            phase_scratch = std::span<std::byte>(
                backing.data() +
                    (index *
                     self.finalized_memory_plan.phase_scratch_stride),
                self.config.scratch_bytes);
        }
        CallbackResult result = CallbackResult::error;
        Status status = Status::callback_failed;
        bool pending = false;
        if (callback.kind == PhaseKind::cpu) {
            CallbackContext callback_context{
                *self.active_frame,
                phase_scratch,
                self.numerics,
                task_context,
                self.degradation_level.load(std::memory_order_acquire),
                self.active_reference_index !=
                        invalid_reference_release_index
                    ? &self.active_rate_view
                    : nullptr,
                self.live_control_mailboxes
                    ? self.live_control_mailboxes->active_generation_view()
                    : nullptr,
            };
            try {
                result = callback.callback(
                    callback.user_data,
                    callback_context);
            } catch (...) {
                result = CallbackResult::error;
            }
            status = result == CallbackResult::ok
                ? Status::ok
                : Status::callback_failed;
        } else if (callback.kind == PhaseKind::device) {
            DeviceCallbackContext callback_context{
                *self.active_frame,
                phase_scratch,
                self.numerics,
                task_context,
                self.degradation_level.load(std::memory_order_acquire),
                self.active_reference_index !=
                        invalid_reference_release_index
                    ? &self.active_rate_view
                    : nullptr,
                self.live_control_mailboxes
                    ? self.live_control_mailboxes->active_generation_view()
                    : nullptr,
            };
            auto submission = make_device_submission();
            try {
                result = callback.device_callback(
                    callback.user_data,
                    callback_context,
                    submission);
            } catch (...) {
                result = CallbackResult::error;
            }
            if (result == CallbackResult::ok && self.devices) {
                std::uint64_t submission_id = 0;
                status = self.devices->submit(
                    callback.device_backend_index,
                    index,
                    task_context.worker_index(),
                    self.active_frame->frame_index,
                    submission,
                    submission_id);
                pending = status == Status::ok;
            }
        } else {
            DeviceCallbackContext callback_context{
                *self.active_frame,
                phase_scratch,
                self.numerics,
                task_context,
                self.degradation_level.load(std::memory_order_acquire),
                self.active_reference_index !=
                        invalid_reference_release_index
                    ? &self.active_rate_view
                    : nullptr,
                self.live_control_mailboxes
                    ? self.live_control_mailboxes->active_generation_view()
                    : nullptr,
            };
            DeviceCommandBatch batch;
            try {
                result = callback.batch_callback(
                    callback.user_data, callback_context, batch);
            } catch (...) {
                result = CallbackResult::error;
            }
            std::uint64_t batch_id = 0;
            if (result == CallbackResult::ok && self.devices) {
                auto materialized_batch = batch;
                auto materialized_declaration =
                    callback.batch_declaration;
                const auto active_rate = self.active_reference_index !=
                    invalid_reference_release_index;
                detail::DeviceRateReleaseIdentity identity;
                bool dispatch_ready = true;
                if (active_rate) {
                    if (!detail::batch_matches_declaration(
                            batch, callback.batch_declaration)) {
                        status = Status::invalid_argument;
                        dispatch_ready = false;
                    }
                    const auto device_phase_index = self
                        .compiled_rate_dispatch_plan
                        .device_phase_by_reference[
                            self.active_reference_index];
                    if (dispatch_ready && device_phase_index >=
                        self.compiled_device_rate_plan.phases.size()) {
                        status = Status::internal_error;
                        dispatch_ready = false;
                    } else if (dispatch_ready) {
                        const auto completion_budget = self
                            .compiled_device_rate_plan
                            .phases[device_phase_index]
                            .completion_budget_ns;
                        if (completion_budget == 0 ||
                            completion_budget >
                                std::numeric_limits<std::uint64_t>::max() -
                                    self.active_nominal_release_ns) {
                            status = Status::capacity_exceeded;
                            dispatch_ready = false;
                        } else {
                            const auto budget_deadline =
                                self.active_nominal_release_ns +
                                completion_budget;
                            const auto completion_deadline = std::min(
                                budget_deadline,
                                self.active_absolute_deadline_ns);
                            const auto now = self.active_replay_view
                                ? self.active_nominal_release_ns
                                : self.clock_now();
                            if (materialized_batch.timeout_ns == 0) {
                                status = Status::invalid_argument;
                                dispatch_ready = false;
                            } else if (now >= completion_deadline) {
                                status = Status::device_timeout;
                                dispatch_ready = false;
                            } else {
                                materialized_batch.timeout_ns = std::min(
                                    materialized_batch.timeout_ns,
                                    completion_deadline - now);
                                const auto& release = self
                                    .compiled_rate_plan.releases[
                                        self.active_reference_index];
                                identity = {
                                    self.active_reference_index,
                                    release.domain,
                                    release.phase,
                                    self.active_supercycle_cycle,
                                    self.active_rate_view
                                        .domain_release_sequence,
                                    release.substep_ordinal,
                                    self.active_logical_release_ns,
                                    self.active_nominal_release_ns,
                                    self.active_absolute_deadline_ns,
                                    completion_budget,
                                };
                                if (!self.materialize_device_payload_references(
                                        materialized_batch,
                                        materialized_declaration,
                                        release,
                                        self.active_device_payload_slot)) {
                                    status = Status::invalid_argument;
                                    dispatch_ready = false;
                                }
                            }
                        }
                    }
                }
                if (dispatch_ready) {
                    status = self.devices->submit_batch(
                        callback.device_backend_index, index,
                        task_context.worker_index(),
                        self.active_frame->frame_index, materialized_batch,
                        materialized_declaration, batch_id,
                        active_rate ? &identity : nullptr,
                        active_rate ? &self.active_device_ticket : nullptr);
                    pending = status == Status::ok && !active_rate;
                }
            }
            if (self.mixed_rate_actions &&
                self.active_reference_index !=
                    invalid_reference_release_index &&
                status != Status::ok && !self.active_device_ticket.valid()) {
                const auto& release = self.compiled_rate_plan.releases[
                    self.active_reference_index];
                const auto degradation = self.degradation_level.load(
                    std::memory_order_acquire);
                const auto reason = status == Status::device_timeout
                    ? MixedRateActionReason::timeout
                    : status == Status::callback_failed
                        ? MixedRateActionReason::callback_failure
                        : MixedRateActionReason::submission_rejected;
                self.emit_mixed_rate_action(MixedRateActionRecord{
                    mixed_rate_action_schema_version,
                    sizeof(MixedRateActionRecord),
                    0,
                    self.observability.runtime_id,
                    self.mixed_rate_closure_policy.host_policy_version,
                    self.active_frame->frame_index,
                    self.active_logical_release_ns,
                    self.active_nominal_release_ns,
                    self.active_rate_view.domain_release_sequence,
                    static_cast<std::uint64_t>(
                        callback.device_backend_index) + 1,
                    batch_id,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    self.active_shedding_state
                        ? self.active_shedding_state->shed_mask
                        : 0,
                    self.active_shedding_state
                        ? self.active_shedding_state->shed_mask
                        : 0,
                    self.config.watchdog_timeout_ns,
                    static_cast<std::uint32_t>(
                        release.domain_registration_index),
                    release.phase.index(),
                    release.substep_ordinal,
                    static_cast<std::int32_t>(status),
                    degradation,
                    degradation,
                    MixedRateActionId::device_terminal,
                    reason,
                    status == Status::device_timeout
                        ? MixedRateActionStage::quarantined
                        : MixedRateActionStage::terminal,
                    release.optional,
                    false,
                    false,
                    MixedRateSampleFreshness::not_applicable,
                    false,
                    false,
                    MixedRateSafetyState::not_applicable,
                    {},
                });
            }
        }

        self.record(
            RuntimeTraceEventType::callback_end,
            status,
            self.clock_now(),
            self.active_frame->frame_index,
            index,
            RuntimeTraceProducer::worker,
            task_context.worker_index(),
            task_context.task_index());
        return {status, pending};
    }

    std::uint32_t graph_owner;
    SteadyRuntimeClock owned_clock;
    RuntimeClock* clock;
    detail::NativePlatformPreflightProbe owned_preflight;
    PlatformPreflightProbe* preflight;
    detail::NativeThreadPolicyProvider owned_thread_policy;
    detail::ThreadPolicyProvider* thread_policy;
    detail::ThreadStartupGate thread_startup_gate;
    detail::ThreadStartupResult frame_startup_result;
    RuntimeConfig config{};
    RateExecutionPolicy rate_execution_policy{};
    bool rate_execution_policy_set = false;
    MixedRateClosurePolicy mixed_rate_closure_policy{};
    bool mixed_rate_closure_policy_set = false;
    LiveControlPolicy live_control_policy{};
    bool live_control_policy_set = false;
    bool live_control_policy_configured = false;
    LiveControlClosurePolicy live_control_closure_policy{};
    LiveControlReplayRetentionPolicy live_control_retention_policy{};
    bool live_control_closure_policy_set = false;
    bool live_control_closure_policy_configured = false;
    CpuMemoryPolicy cpu_memory_policy{};
    CpuMemoryPolicyReport cpu_memory_policy_report{};
    bool cpu_memory_policy_report_available = false;
    MemoryProvider memory_provider{};
    bool memory_provider_set = false;
    std::atomic<bool> memory_provider_callback_active{false};
    std::atomic<bool> extension_control_callback_active{false};
    HostExecutorAdapter host_executor{};
    bool host_executor_set = false;
    RuntimeState state = RuntimeState::configuring;
    NumericalPolicy numerics{};
    std::vector<std::unique_ptr<detail::ExtensionRegistrationRecord>>
        extensions;
    std::uint32_t next_extension_generation = 1;
    std::vector<RegisteredCallback> callbacks;
    std::vector<detail::DeviceBackendSpec> device_backends;
    std::vector<std::unique_ptr<detail::DeviceV1CompatibilityAdapter>>
        device_v1_adapters;
    std::vector<std::unique_ptr<detail::HeterogeneousMemoryState>>
        device_memory_states;
    std::vector<std::unique_ptr<detail::CommandTimelineExtensionState>>
        device_command_states;
    std::vector<detail::DeviceBufferSpec> device_buffers;
    std::vector<detail::DeviceTimelineSpec> device_timelines;
    std::vector<RegisteredResource> resources;
    std::vector<RegisteredState> states;
    std::vector<detail::GraphDependency> dependencies;
    std::vector<detail::GraphResourceAccess> resource_accesses;
    std::vector<PhaseHandle> compiled_order;
    std::vector<detail::RateDomainSpec> rate_domains;
    std::vector<detail::RateBindingSpec> rate_bindings;
    std::vector<detail::DeviceRateBindingSpec> device_rate_bindings;
    detail::CompiledRatePlan compiled_rate_plan;
    detail::CompiledRateDispatchPlan compiled_rate_dispatch_plan;
    detail::CompiledDeviceRatePlan compiled_device_rate_plan;
    DeviceRateAdmissionDiagnostic device_rate_diagnostic{};
    std::vector<detail::CrossRateChannelSpec> cross_rate_channels;
    detail::CompiledCrossRatePlan compiled_cross_rate_plan;
    std::vector<detail::SampledIoChannelSpec> sampled_io_channels;
    std::vector<LiveControlMailboxRegistration>
        live_control_mailbox_declarations;
    std::vector<LiveControlProducerRegistration>
        live_control_producer_declarations;
    detail::CompiledSampledIoPlan compiled_sampled_io_plan;
    std::vector<SampledIoChannelStatus> sampled_io_statuses;
    std::vector<ActiveChannelState> active_channel_states;
    std::vector<std::byte> active_committed_payloads;
    std::vector<std::byte> active_staging_payloads;
    std::vector<std::byte> active_checkpoint_state;
    std::vector<std::byte> mixed_rate_checkpoint_state;
    std::unique_ptr<std::atomic<std::uint8_t>[]> active_publication_claims;
    std::vector<detail::DeviceRateTicket> active_device_tickets;
    detail::DeviceRateTicket active_device_ticket{};
    std::uint32_t active_device_payload_slot = 0;
    std::unique_ptr<ActiveSheddingState> active_shedding_state;
    std::uint64_t active_logical_cursor_ns = 0;
    std::uint64_t active_nominal_epoch_ns = 0;
    bool active_epoch_mapped = false;
    bool active_faulted = false;
    RateDomainHandle active_fault_domain{};
    std::uint64_t active_fault_sequence = 0;
    std::uint32_t active_fault_substep = 0;
    std::size_t active_reference_index = invalid_reference_release_index;
    std::uint64_t active_supercycle_cycle = 0;
    std::uint64_t active_logical_release_ns = 0;
    std::uint64_t active_nominal_release_ns = 0;
    std::uint64_t active_absolute_deadline_ns = 0;
    RateLateAction active_late_action = RateLateAction::fail;
    StepResult::RateSummary* active_rate_summary = nullptr;
    std::atomic<std::uint64_t> active_stale_reads{0};
    RateReleaseView active_rate_view{};
    std::unique_ptr<detail::ResidentRegionSet> resident_regions;
    std::unique_ptr<detail::TelemetryRing> telemetry;
    detail::TelemetryCounters telemetry_counters;
    std::unique_ptr<detail::RateTelemetryRing> rate_telemetry;
    std::unique_ptr<detail::RateCounters> rate_counters;
    std::unique_ptr<detail::MixedRateActionRing> mixed_rate_actions;
    std::unique_ptr<detail::LiveControlMailboxSet> live_control_mailboxes;
    std::uint64_t mixed_rate_checkpoint_sequence = 0;
    const detail::ActiveReplayArtifactView* active_replay_view = nullptr;
    std::size_t active_replay_expected_action = 0;
    std::size_t active_replay_actions_compared = 0;
    std::uint64_t active_replay_mismatch_sequence = 0;
    Status active_replay_mismatch_status = Status::ok;
    bool active_replay_restore_allowed = false;
    ObservabilityMetadata observability{};
    std::uint64_t graph_id = 0;
    std::uint64_t state_schema_id = 0;
    std::uint64_t replay_id = 0;
    std::uint64_t telemetry_epoch_ns = 0;
    std::uint64_t metric_snapshot_sequence = 0;
    std::unique_ptr<detail::Executor> executor;
    std::unique_ptr<detail::DeviceManager> devices;
    detail::WatchdogMonitor watchdog;
    MemoryPlan finalized_memory_plan{};
    PlatformPreflightReport preflight_report{};
    bool preflight_report_available = false;
    std::atomic<bool> in_step{false};
    std::atomic<bool> in_periodic_run{false};
    std::atomic<bool> periodic_dispatch{false};
    std::atomic<bool> in_replay{false};
    std::atomic<bool> replay_dispatch{false};
    std::atomic<std::uint32_t> degradation_level{0};
    bool watchdog_started = false;
    bool stop_pending = false;
    bool lane_cleanup_pending = false;
    bool memory_cleanup_pending = false;
    bool runtime_stack_results_available = false;
    const HostFrameContext* active_frame = nullptr;
    std::array<char, 256> error{};
    mutable std::atomic_flag error_lock = ATOMIC_FLAG_INIT;
    std::atomic_flag clock_lock = ATOMIC_FLAG_INIT;
};

Runtime::Runtime()
    : impl_(std::make_unique<Impl>(nullptr, nullptr)) {}

Runtime::Runtime(RuntimeClock& clock)
    : impl_(std::make_unique<Impl>(&clock, nullptr)) {}

Runtime::Runtime(
    RuntimeClock& clock,
    PlatformPreflightProbe& preflight)
    : impl_(std::make_unique<Impl>(&clock, &preflight)) {}

void detail::RuntimeThreadPolicyTestAccess::set_provider(
    Runtime& runtime,
    detail::ThreadPolicyProvider& provider) noexcept {
    if (runtime.impl_ &&
        runtime.impl_->state == RuntimeState::configuring) {
        runtime.impl_->thread_policy = &provider;
    }
}

bool detail::RuntimeLiveControlTestAccess::claim(
    Runtime& runtime,
    std::uint64_t mailbox_identity) noexcept {
    return runtime.impl_ && runtime.impl_->live_control_mailboxes &&
        runtime.impl_->live_control_mailboxes->claim_for_test(
            mailbox_identity);
}

void detail::RuntimeLiveControlTestAccess::release(
    Runtime& runtime,
    std::uint64_t mailbox_identity) noexcept {
    if (runtime.impl_ && runtime.impl_->live_control_mailboxes) {
        runtime.impl_->live_control_mailboxes->release_for_test(
            mailbox_identity);
    }
}

Runtime::~Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

Status Runtime::configure(const RuntimeConfig& config) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state, "configure requires configuring state");
    }
    if (validate_config(config) != Status::ok ||
        config.callback_capacity < impl_->callbacks.size() ||
        config.state_capacity < impl_->states.size() ||
        config.device_backend_capacity <
            impl_->device_backends.size() ||
        config.device_buffer_capacity <
            impl_->device_buffers.size()) {
        return impl_->fail(Status::invalid_config, nullptr);
    }
    impl_->config = config;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::set_cpu_memory_policy(
    const CpuMemoryPolicy& policy) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "CPU/memory policy is frozen");
    }
    // Counts and entries are validated together in finalize() so malformed,
    // duplicate, contradictory, and unsupported-strict requests fail at the
    // same pre-start transactional boundary as the compiled resource plan.
    impl_->cpu_memory_policy = policy;
    impl_->cpu_memory_policy_report = {};
    impl_->cpu_memory_policy_report_available = false;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::set_memory_provider(
    const MemoryProvider& provider) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "memory provider attachment is frozen");
    }
    impl_->memory_provider = provider;
    impl_->memory_provider_set = true;
    impl_->cpu_memory_policy_report = {};
    impl_->cpu_memory_policy_report_available = false;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::set_host_executor(
    const HostExecutorAdapter& adapter) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "host executor attachment is frozen");
    }
    if (adapter.worker_count == 0 ||
        adapter.worker_count > kMaxWorkers ||
        adapter.queue_capacity < 2 ||
        adapter.queue_capacity > kMaxExecutorQueueCapacity ||
        (adapter.queue_capacity & (adapter.queue_capacity - 1)) != 0 ||
        adapter.submit == nullptr ||
        adapter.try_execute_one == nullptr) {
        return impl_->fail(
            Status::invalid_argument,
            "host executor adapter is incomplete or has invalid capacities");
    }
    impl_->host_executor = adapter;
    impl_->host_executor_set = true;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::set_rate_execution_policy(
    const RateExecutionPolicy& policy) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "rate execution policy is frozen");
    }
    if (policy.maximum_dispatch_records_per_step == 0 ||
        policy.maximum_dispatch_records_per_step >
            reference_release_capacity ||
        policy.host_policy_version == 0 ||
        policy.consecutive_late_threshold == 0 ||
        policy.consecutive_on_time_threshold == 0 ||
        policy.consecutive_late_threshold > rate_policy_threshold_limit ||
        policy.consecutive_on_time_threshold > rate_policy_threshold_limit ||
        policy.rate_telemetry_capacity > rate_telemetry_capacity_limit) {
        return impl_->fail(
            Status::invalid_argument,
            "rate execution policy fields or telemetry capacity are invalid");
    }
    impl_->rate_execution_policy = policy;
    impl_->rate_execution_policy_set = true;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::set_mixed_rate_closure_policy(
    const MixedRateClosurePolicy& policy) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring ||
        impl_->mixed_rate_closure_policy_set) {
        return impl_->fail(
            Status::invalid_state,
            "mixed-rate closure policy is frozen or already set");
    }
    const auto reserved_zero = std::all_of(
        policy.reserved.begin(), policy.reserved.end(),
        [](std::byte value) { return value == std::byte{0}; });
    const auto replay_fields_valid = !policy.active_replay_enabled ||
        (policy.action_capacity != 0 &&
         policy.active_replay_record_capacity != 0 &&
         policy.active_replay_max_bytes != 0 &&
         policy.maximum_actions_per_step != 0 &&
         policy.require_deterministic_backends);
    if (policy.host_policy_version == 0 || !reserved_zero ||
        policy.action_capacity > mixed_rate_action_capacity_limit ||
        policy.active_replay_record_capacity >
            active_replay_record_capacity_limit ||
        policy.active_replay_max_bytes > active_replay_absolute_max_bytes ||
        policy.maximum_actions_per_step > mixed_rate_action_capacity_limit ||
        policy.overflow_policy !=
            MixedRateOverflowPolicy::overwrite_committed ||
        !replay_fields_valid) {
        return impl_->fail(
            Status::invalid_argument,
            "mixed-rate closure policy fields or replay bounds are invalid");
    }
    impl_->mixed_rate_closure_policy = policy;
    impl_->mixed_rate_closure_policy_set = true;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::set_live_control_policy(
    const LiveControlPolicy& policy) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring ||
        impl_->live_control_policy_configured) {
        return impl_->fail(
            Status::invalid_state,
            "live-control policy is frozen or already set");
    }
    const auto reserved_is_zero = std::all_of(
        policy.reserved.begin(), policy.reserved.end(),
        [](std::byte value) { return value == std::byte{0}; });
    const auto header_valid =
        policy.schema_version == live_control_schema_version &&
        policy.struct_size == sizeof(LiveControlPolicy) &&
        policy.admission_policy == LiveControlAdmissionPolicy::reject_new &&
        policy.reset_rule == LiveControlResetRule::discard_with_runtime &&
        reserved_is_zero;
    const auto disabled = policy.policy_identity == 0 &&
        policy.mailbox_capacity == 0 && policy.producer_capacity == 0 &&
        policy.record_capacity == 0 &&
        policy.payload_bytes_per_record == 0 &&
        policy.total_payload_storage_bytes == 0;
    const auto enabled = policy.policy_identity != 0 &&
        policy.mailbox_capacity != 0 &&
        policy.mailbox_capacity <= live_control_mailbox_capacity_limit &&
        policy.producer_capacity != 0 &&
        policy.producer_capacity <= live_control_producer_capacity_limit &&
        policy.record_capacity != 0 &&
        policy.record_capacity <= live_control_record_capacity_limit &&
        policy.payload_bytes_per_record != 0 &&
        policy.payload_bytes_per_record <= live_control_payload_bytes_limit &&
        policy.total_payload_storage_bytes != 0 &&
        policy.total_payload_storage_bytes <= live_control_total_storage_limit &&
        policy.total_payload_storage_bytes <=
            std::numeric_limits<std::size_t>::max();
    if (!header_valid || (!disabled && !enabled)) {
        return impl_->fail(
            Status::invalid_argument,
            "live-control policy fields or capacities are invalid");
    }
    impl_->live_control_policy = policy;
    impl_->live_control_policy_set = enabled;
    impl_->live_control_policy_configured = true;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::set_live_control_closure_policy(
    const LiveControlClosurePolicy& policy) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring ||
        impl_->live_control_closure_policy_configured) {
        return impl_->fail(
            Status::invalid_state,
            "live-control closure policy is frozen or already set");
    }
    const auto reserved_is_zero = std::all_of(
        policy.reserved.begin(), policy.reserved.end(),
        [](std::byte value) { return value == std::byte{0}; });
    const auto header_valid =
        policy.schema_version == live_control_action_schema_version &&
        policy.struct_size == sizeof(LiveControlClosurePolicy) &&
        policy.rollback_rule ==
            LiveControlRollbackRule::restore_step_entry_generation &&
        reserved_is_zero;
    const auto disabled = policy.policy_identity == 0 &&
        policy.action_capacity == 0 &&
        policy.retained_generation_capacity == 0 &&
        policy.retained_record_capacity == 0 &&
        policy.retained_payload_bytes == 0 &&
        policy.replay_record_capacity == 0 &&
        policy.replay_max_bytes == 0 && !policy.replay_enabled;
    const auto enabled = policy.policy_identity != 0 &&
        policy.action_capacity <= live_control_action_capacity_limit &&
        policy.retained_generation_capacity <=
            live_control_retained_generation_capacity_limit &&
        policy.retained_record_capacity <=
            live_control_record_capacity_limit &&
        policy.retained_payload_bytes <= live_control_total_storage_limit &&
        policy.replay_record_capacity <=
            live_control_record_capacity_limit &&
        policy.replay_max_bytes <=
            live_control_replay_absolute_max_bytes &&
        (!policy.replay_enabled ||
         (policy.retained_generation_capacity != 0 &&
          policy.retained_record_capacity != 0 &&
          policy.retained_payload_bytes != 0 &&
          policy.replay_record_capacity != 0 &&
          policy.replay_max_bytes != 0));
    if (!header_valid || (!disabled && !enabled)) {
        return impl_->fail(
            Status::invalid_argument,
            "live-control closure fields or capacities are invalid");
    }
    impl_->live_control_closure_policy = policy;
    impl_->live_control_closure_policy_set = enabled;
    impl_->live_control_closure_policy_configured = true;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::set_live_control_replay_retention_policy(
    const LiveControlReplayRetentionPolicy& policy) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active() ||
        impl_->state != RuntimeState::configuring ||
        impl_->live_control_retention_policy.policy_identity != 0 ||
        !impl_->live_control_closure_policy_set ||
        !impl_->live_control_closure_policy.replay_enabled) {
        return impl_->fail(Status::invalid_state,
                           "trusted retention requires an unfrozen replay closure");
    }
    if (policy.schema_version != live_control_lossless_replay_schema_version ||
        policy.struct_size != sizeof(policy) || policy.policy_identity == 0 ||
        policy.admission_capacity == 0 ||
        policy.admission_capacity > live_control_action_capacity_limit ||
        policy.payload_capacity_bytes == 0 ||
        policy.payload_capacity_bytes > live_control_total_storage_limit ||
        !std::all_of(policy.reserved.begin(), policy.reserved.end(),
                     [](std::byte b) { return b == std::byte{0}; })) {
        return impl_->fail(Status::invalid_argument,
                           "trusted retention fields or capacities are invalid");
    }
    impl_->live_control_retention_policy = policy;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::configure_key(
    std::string_view key,
    std::string_view value) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state, "configuration is frozen");
    }

    RuntimeConfig candidate = impl_->config;
    const auto status = set_runtime_config_value(candidate, key, value);
    if (status != Status::ok ||
        candidate.callback_capacity < impl_->callbacks.size() ||
        candidate.state_capacity < impl_->states.size() ||
        candidate.device_backend_capacity <
            impl_->device_backends.size() ||
        candidate.device_buffer_capacity <
            impl_->device_buffers.size()) {
        return impl_->fail(Status::invalid_config, "unknown or invalid configuration key/value");
    }
    impl_->config = candidate;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_callback(
    const CallbackRegistration& registration) noexcept {
    PhaseHandle ignored;
    return register_callback(registration, ignored);
}

Status Runtime::register_callback(
    const CallbackRegistration& registration,
    PhaseHandle& out_phase) noexcept {
    out_phase = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state, "callback registration is frozen");
    }
    if (registration.name.empty() || !registration.callback) {
        return impl_->fail(Status::invalid_argument, "callback name and function are required");
    }
    if (impl_->callbacks.size() >= impl_->config.callback_capacity) {
        return impl_->fail(Status::capacity_exceeded, nullptr);
    }
    const auto duplicate = std::find_if(
        impl_->callbacks.begin(),
        impl_->callbacks.end(),
        [&](const Impl::RegisteredCallback& callback) {
            return callback.name == registration.name;
        });
    if (duplicate != impl_->callbacks.end()) {
        return impl_->fail(Status::invalid_argument, "callback names must be unique");
    }

    try {
        const auto index = static_cast<std::uint32_t>(impl_->callbacks.size());
        impl_->callbacks.push_back(Impl::RegisteredCallback{
            std::string(registration.name),
            Impl::PhaseKind::cpu,
            registration.callback,
            nullptr,
            nullptr,
            registration.user_data,
            0,
            {},
        });
        out_phase = PhaseHandle{impl_->graph_owner, index};
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }

    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_extension(
    rtfw_extension_entry_fn_v1 entry,
    ExtensionHandle& out_extension) noexcept {
    out_extension = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return impl_->fail(
            Status::invalid_state,
            "extension registration or another control callback is active");
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "extension registration requires configuring state");
    }
    if (!entry) {
        return impl_->fail(
            Status::invalid_argument,
            "extension entry function is required");
    }
    if (impl_->extensions.size() >= RTFW_RUNTIME_EXTENSION_CAPACITY ||
        impl_->next_extension_generation == 0 ||
        impl_->next_extension_generation ==
            std::numeric_limits<std::uint32_t>::max()) {
        return impl_->fail(
            Status::capacity_exceeded,
            "extension capacity or generation space is exhausted");
    }

    const auto registration_generation =
        impl_->next_extension_generation++;
    detail::ExtensionRegistrationTransaction transaction;
    impl_->extension_control_callback_active.store(
        true, std::memory_order_release);
    const auto entry_status = detail::invoke_extension_entry(
        entry,
        impl_->graph_owner,
        registration_generation,
        impl_->config.callback_capacity - impl_->callbacks.size(),
        impl_->config.device_backend_capacity -
            impl_->device_backends.size(),
        transaction);
    impl_->extension_control_callback_active.store(
        false, std::memory_order_release);
    if (entry_status != Status::ok) {
        return impl_->fail(
            entry_status,
            "extension entry or staged ABI records were rejected");
    }
    if (transaction.phase_count >
            impl_->config.callback_capacity - impl_->callbacks.size() ||
        transaction.backend_count >
            impl_->config.device_backend_capacity -
                impl_->device_backends.size()) {
        return impl_->fail(
            Status::capacity_exceeded,
            "extension exceeds configured Runtime capacities");
    }

    const auto fixed_name = [](const auto& name) {
        return std::string_view(
            name,
            std::char_traits<char>::length(name));
    };
    const auto duplicate_fixed = [&](const auto& records, std::size_t count) {
        for (std::size_t index = 0; index < count; ++index) {
            for (std::size_t earlier = 0; earlier < index; ++earlier) {
                if (fixed_name(records[index].name) ==
                    fixed_name(records[earlier].name)) {
                    return true;
                }
            }
        }
        return false;
    };
    if (duplicate_fixed(transaction.phases, transaction.phase_count) ||
        duplicate_fixed(transaction.backends, transaction.backend_count) ||
        duplicate_fixed(transaction.services, transaction.service_count) ||
        duplicate_fixed(transaction.resources, transaction.resource_count)) {
        return impl_->fail(
            Status::invalid_argument,
            "extension-local names must be unique by kind");
    }
    const auto extension_name = fixed_name(transaction.descriptor.name);
    for (const auto& existing : impl_->extensions) {
        if (existing && identifier_view(existing->name) == extension_name) {
            return impl_->fail(
                Status::invalid_argument,
                "extension names must be unique");
        }
    }
    for (std::size_t index = 0; index < transaction.phase_count; ++index) {
        const auto name = fixed_name(transaction.phases[index].name);
        if (std::any_of(
                impl_->callbacks.begin(), impl_->callbacks.end(),
                [&](const Impl::RegisteredCallback& callback) {
                    return callback.name == name;
                })) {
            return impl_->fail(
                Status::invalid_argument,
                "extension phase conflicts with an existing phase");
        }
    }
    for (std::size_t index = 0; index < transaction.backend_count; ++index) {
        const auto name = fixed_name(transaction.backends[index].name);
        if (std::any_of(
                impl_->device_backends.begin(), impl_->device_backends.end(),
                [&](const detail::DeviceBackendSpec& backend) {
                    return backend.name == name;
                })) {
            return impl_->fail(
                Status::invalid_argument,
                "extension backend conflicts with an existing backend");
        }
    }
    for (std::size_t index = 0; index < transaction.resource_count; ++index) {
        const auto name = fixed_name(transaction.resources[index].name);
        if (std::any_of(
                impl_->resources.begin(), impl_->resources.end(),
                [&](const Impl::RegisteredResource& resource) {
                    return resource.name == name;
                })) {
            return impl_->fail(
                Status::invalid_argument,
                "extension resource conflicts with an existing resource");
        }
    }

    struct RegistrationStorage {
        std::unique_ptr<detail::ExtensionRegistrationRecord> record;
        std::vector<Impl::RegisteredCallback> staged_callbacks;
        std::vector<std::unique_ptr<detail::DeviceV1CompatibilityAdapter>>
            staged_adapters;
        std::vector<std::unique_ptr<detail::HeterogeneousMemoryState>>
            staged_memory_states;
        std::vector<detail::DeviceBackendSpec> staged_backends;
        std::vector<Impl::RegisteredResource> staged_resources;
        std::vector<detail::GraphDependency> staged_dependencies;
        std::vector<detail::GraphResourceAccess> staged_accesses;
        std::vector<std::unique_ptr<detail::ExtensionRegistrationRecord>>
            replacement_extensions;
        std::vector<Impl::RegisteredCallback> replacement_callbacks;
        std::vector<std::unique_ptr<detail::DeviceV1CompatibilityAdapter>>
            replacement_adapters;
        std::vector<std::unique_ptr<detail::HeterogeneousMemoryState>>
            replacement_memory_states;
        std::vector<detail::DeviceBackendSpec> replacement_backends;
        std::vector<Impl::RegisteredResource> replacement_resources;
    };
    std::unique_ptr<RegistrationStorage> storage;
    try {
        // Allocate the owner before constructing checked-iterator containers.
        // Their default constructors are noexcept even when a debug runtime
        // allocates an empty-container proxy, so the catchable allocation must
        // happen first.
        storage = std::make_unique<RegistrationStorage>();
        auto& record = storage->record;
        auto& staged_callbacks = storage->staged_callbacks;
        auto& staged_adapters = storage->staged_adapters;
        auto& staged_memory_states = storage->staged_memory_states;
        auto& staged_backends = storage->staged_backends;
        auto& staged_resources = storage->staged_resources;
        auto& staged_dependencies = storage->staged_dependencies;
        auto& staged_accesses = storage->staged_accesses;
        auto& replacement_extensions = storage->replacement_extensions;
        auto& replacement_callbacks = storage->replacement_callbacks;
        auto& replacement_adapters = storage->replacement_adapters;
        auto& replacement_memory_states = storage->replacement_memory_states;
        auto& replacement_backends = storage->replacement_backends;
        auto& replacement_resources = storage->replacement_resources;
        staged_dependencies = impl_->dependencies;
        staged_accesses = impl_->resource_accesses;
        record =
            std::make_unique<detail::ExtensionRegistrationRecord>();
        std::copy_n(
            transaction.descriptor.name,
            RTFW_EXTENSION_IDENTIFIER_CAPACITY,
            record->name.begin());
        std::copy_n(
            transaction.descriptor.version,
            RTFW_EXTENSION_IDENTIFIER_CAPACITY,
            record->version.begin());
        record->negotiated_abi_version = RTFW_EXTENSION_ABI_VERSION;
        record->generation = registration_generation;
        record->slot = static_cast<std::uint32_t>(impl_->extensions.size());
        record->phase_count = transaction.phase_count;
        record->backend_count = transaction.backend_count;
        record->service_count = transaction.service_count;
        record->resource_count = transaction.resource_count;
        record->relationship_count = transaction.relationship_count;

        staged_callbacks.reserve(transaction.phase_count);
        for (std::size_t index = 0;
             index < transaction.phase_count; ++index) {
            record->phase_descriptors[index] = transaction.phases[index];
            record->phases[index].callback =
                transaction.phases[index].callback;
            record->phases[index].user_data =
                transaction.phases[index].user_data;
            record->phase_indices[index] = static_cast<std::uint32_t>(
                impl_->callbacks.size() + index);
            staged_callbacks.push_back(Impl::RegisteredCallback{
                std::string(fixed_name(transaction.phases[index].name)),
                Impl::PhaseKind::cpu,
                &detail::ExtensionPhaseOwner::invoke,
                nullptr,
                nullptr,
                &record->phases[index],
                0,
                {},
            });
        }

        staged_adapters.reserve(transaction.backend_count);
        staged_memory_states.reserve(transaction.backend_count);
        staged_backends.reserve(transaction.backend_count);
        for (std::size_t index = 0;
             index < transaction.backend_count; ++index) {
            record->backend_descriptors[index] =
                transaction.backends[index];
            record->backends[index].initialize_from(
                transaction.backends[index].api);
            auto adapter =
                std::make_unique<detail::DeviceV1CompatibilityAdapter>(
                    record->backends[index].forwarding);
            HalV2Capabilities capabilities;
            HalV2Status hal_status = HalV2Status::internal_error;
            impl_->extension_control_callback_active.store(
                true, std::memory_order_release);
            try {
                hal_status = adapter->api().get_capabilities(
                    adapter->api().instance, &capabilities);
            } catch (...) {
                hal_status = HalV2Status::internal_error;
            }
            impl_->extension_control_callback_active.store(
                false, std::memory_order_release);
            const auto status = detail::hal_v2_status_to_runtime(hal_status);
            if (status != Status::ok ||
                !detail::validate_hal_v2_capabilities(capabilities)) {
                return impl_->fail(
                    status == Status::ok ? Status::invalid_argument : status,
                    "extension backend capabilities were rejected");
            }
            auto memory_state =
                std::make_unique<detail::HeterogeneousMemoryState>();
            detail::make_implicit_host_memory_state(
                capabilities, *memory_state);
            record->backend_indices[index] = static_cast<std::uint32_t>(
                impl_->device_backends.size() + index);
            auto* adapter_instance = adapter.get();
            auto* memory_instance = memory_state.get();
            staged_adapters.push_back(std::move(adapter));
            staged_memory_states.push_back(std::move(memory_state));
            staged_backends.push_back(detail::DeviceBackendSpec{
                std::string(fixed_name(transaction.backends[index].name)),
                adapter_instance->api(),
                capabilities,
                detail::HalBackendKind::adapted_device_abi_v1,
                adapter_instance,
                memory_instance,
                nullptr,
            });
        }

        staged_resources.reserve(transaction.resource_count);
        for (std::size_t index = 0;
             index < transaction.resource_count; ++index) {
            record->resource_descriptors[index] =
                transaction.resources[index];
            record->resource_indices[index] = static_cast<std::uint32_t>(
                impl_->resources.size() + index);
            staged_resources.push_back(Impl::RegisteredResource{
                std::string(fixed_name(transaction.resources[index].name))});
        }
        for (std::size_t index = 0;
             index < transaction.service_count; ++index) {
            record->service_descriptors[index] =
                transaction.services[index];
            record->services[index].initialize_from(
                transaction.services[index].api);
        }
        for (std::size_t index = 0;
             index < transaction.relationship_count; ++index) {
            const auto& relationship = transaction.relationships[index];
            record->relationships[index] = relationship;
            switch (relationship.kind) {
            case RTFW_EXTENSION_RELATIONSHIP_PHASE_DEPENDENCY:
                staged_dependencies.push_back(detail::GraphDependency{
                    PhaseHandle{
                        impl_->graph_owner,
                        record->phase_indices[relationship.first.slot]},
                    PhaseHandle{
                        impl_->graph_owner,
                        record->phase_indices[relationship.second.slot]},
                });
                break;
            case RTFW_EXTENSION_RELATIONSHIP_PHASE_RESOURCE:
                staged_accesses.push_back(detail::GraphResourceAccess{
                    PhaseHandle{
                        impl_->graph_owner,
                        record->phase_indices[relationship.first.slot]},
                    ResourceHandle{
                        impl_->graph_owner,
                        record->resource_indices[relationship.second.slot]},
                    relationship.access == RTFW_EXTENSION_RESOURCE_ACCESS_READ
                        ? ResourceAccess::read
                        : ResourceAccess::write,
                });
                break;
            case RTFW_EXTENSION_RELATIONSHIP_SERVICE_BACKEND:
                record->service_backend_relationships
                    [relationship.first.slot][relationship.second.slot] = true;
                break;
            default:
                return impl_->fail(Status::internal_error, nullptr);
            }
        }

        std::vector<PhaseHandle> validation_order;
        detail::GraphCompileDiagnostic diagnostic;
        const auto graph_status = detail::compile_graph(
            impl_->graph_owner,
            impl_->callbacks.size() + transaction.phase_count,
            impl_->resources.size() + transaction.resource_count,
            staged_dependencies,
            staged_accesses,
            validation_order,
            diagnostic);
        if (graph_status != Status::ok) {
            return impl_->fail_compile(graph_status, diagnostic);
        }

        replacement_extensions.reserve(impl_->extensions.size() + 1);
        replacement_callbacks = impl_->callbacks;
        replacement_callbacks.reserve(
            impl_->callbacks.size() + staged_callbacks.size());
        for (auto& callback : staged_callbacks) {
            replacement_callbacks.push_back(std::move(callback));
        }
        replacement_adapters.reserve(
            impl_->device_v1_adapters.size() + staged_adapters.size());
        replacement_memory_states.reserve(
            impl_->device_memory_states.size() +
                staged_memory_states.size());
        replacement_backends = impl_->device_backends;
        replacement_backends.reserve(
            impl_->device_backends.size() + staged_backends.size());
        for (auto& backend : staged_backends) {
            replacement_backends.push_back(std::move(backend));
        }
        replacement_resources = impl_->resources;
        replacement_resources.reserve(
            impl_->resources.size() + staged_resources.size());
        for (auto& resource : staged_resources) {
            replacement_resources.push_back(std::move(resource));
        }
        staged_dependencies.reserve(staged_dependencies.size());
        staged_accesses.reserve(staged_accesses.size());
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }

    auto& record = storage->record;
    auto& staged_adapters = storage->staged_adapters;
    auto& staged_memory_states = storage->staged_memory_states;
    auto& staged_dependencies = storage->staged_dependencies;
    auto& staged_accesses = storage->staged_accesses;
    auto& replacement_extensions = storage->replacement_extensions;
    auto& replacement_callbacks = storage->replacement_callbacks;
    auto& replacement_adapters = storage->replacement_adapters;
    auto& replacement_memory_states = storage->replacement_memory_states;
    auto& replacement_backends = storage->replacement_backends;
    auto& replacement_resources = storage->replacement_resources;
    for (auto& extension : impl_->extensions) {
        replacement_extensions.push_back(std::move(extension));
    }
    replacement_extensions.push_back(std::move(record));
    impl_->extensions.swap(replacement_extensions);
    impl_->callbacks.swap(replacement_callbacks);
    for (auto& adapter : impl_->device_v1_adapters) {
        replacement_adapters.push_back(std::move(adapter));
    }
    for (auto& adapter : staged_adapters) {
        replacement_adapters.push_back(std::move(adapter));
    }
    impl_->device_v1_adapters.swap(replacement_adapters);
    for (auto& memory : impl_->device_memory_states) {
        replacement_memory_states.push_back(std::move(memory));
    }
    for (auto& memory : staged_memory_states) {
        replacement_memory_states.push_back(std::move(memory));
    }
    impl_->device_memory_states.swap(replacement_memory_states);
    impl_->device_backends.swap(replacement_backends);
    impl_->resources.swap(replacement_resources);
    impl_->dependencies.swap(staged_dependencies);
    impl_->resource_accesses.swap(staged_accesses);
    const auto& committed = *impl_->extensions.back();
    const auto slot = committed.slot;
    const auto generation =
        committed.generation.load(std::memory_order_acquire);
    out_extension = ExtensionHandle{
        impl_->graph_owner,
        RTFW_EXTENSION_HANDLE_EXTENSION,
        slot,
        generation,
    };
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_device_backend(
    const DeviceBackendRegistration& registration,
    DeviceBackendHandle& out_backend) noexcept {
    out_backend = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "device backend registration is frozen");
    }
    std::array<char, RTFW_DEVICE_IDENTIFIER_CAPACITY> name{};
    if (!set_identifier(name, registration.name)) {
        return impl_->fail(
            Status::invalid_argument,
            "device backend name must be a stable identifier");
    }
    if (impl_->device_backends.size() >=
        impl_->config.device_backend_capacity) {
        return impl_->fail(
            Status::capacity_exceeded,
            "configured device backend capacity exceeded");
    }
    if (!detail::validate_device_v1_api(registration.api)) {
        return impl_->fail(
            Status::invalid_argument,
            "device backend function table is malformed or incompatible");
    }
    const auto duplicate = std::find_if(
        impl_->device_backends.begin(),
        impl_->device_backends.end(),
        [&](const detail::DeviceBackendSpec& backend) {
            return backend.name == registration.name;
        });
    if (duplicate != impl_->device_backends.end()) {
        return impl_->fail(
            Status::invalid_argument,
            "device backend names must be unique");
    }

    std::unique_ptr<detail::DeviceV1CompatibilityAdapter> adapter;
    try {
        adapter = std::make_unique<detail::DeviceV1CompatibilityAdapter>(
            registration.api);
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    HalV2Capabilities capabilities;
    HalV2Status hal_status = HalV2Status::internal_error;
    try {
        hal_status = adapter->api().get_capabilities(
            adapter->api().instance, &capabilities);
    } catch (...) {
        hal_status = HalV2Status::internal_error;
    }
    const auto status = detail::hal_v2_status_to_runtime(hal_status);
    if (status != Status::ok) {
        return impl_->fail(
            status,
            "device backend capability query failed");
    }
    if (!detail::validate_hal_v2_capabilities(capabilities)) {
        return impl_->fail(
            Status::invalid_argument,
            "device backend reported malformed capabilities");
    }

    std::unique_ptr<detail::HeterogeneousMemoryState> memory_state;
    try {
      memory_state = std::make_unique<detail::HeterogeneousMemoryState>();
      detail::make_implicit_host_memory_state(capabilities, *memory_state);
      const auto index =
          static_cast<std::uint32_t>(impl_->device_backends.size());
      auto *adapter_instance = adapter.get();
      auto *memory_instance = memory_state.get();
      impl_->device_v1_adapters.push_back(std::move(adapter));
      try {
        impl_->device_memory_states.push_back(std::move(memory_state));
      } catch (...) {
        impl_->device_v1_adapters.pop_back();
        throw;
      }
        try {
          impl_->device_backends.push_back(detail::DeviceBackendSpec{
              std::string(registration.name),
              adapter_instance->api(),
              capabilities,
              detail::HalBackendKind::adapted_device_abi_v1,
              adapter_instance,
              memory_instance,
              nullptr,
          });
        } catch (...) {
          impl_->device_memory_states.pop_back();
          impl_->device_v1_adapters.pop_back();
          throw;
        }
        out_backend =
            DeviceBackendHandle{impl_->graph_owner, index};
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_device_backend(
    const HalV2BackendRegistration& registration,
    DeviceBackendHandle& out_backend) noexcept {
    out_backend = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "device backend registration is frozen");
    }
    std::array<char, RTFW_DEVICE_IDENTIFIER_CAPACITY> name{};
    if (!set_identifier(name, registration.name)) {
        return impl_->fail(
            Status::invalid_argument,
            "device backend name must be a stable identifier");
    }
    if (impl_->device_backends.size() >=
        impl_->config.device_backend_capacity) {
        return impl_->fail(
            Status::capacity_exceeded,
            "configured device backend capacity exceeded");
    }
    if (!detail::validate_hal_v2_api(registration.api)) {
        return impl_->fail(
            Status::invalid_argument,
            "HAL v2 backend function table is malformed or incompatible");
    }
    const auto duplicate = std::find_if(
        impl_->device_backends.begin(),
        impl_->device_backends.end(),
        [&](const detail::DeviceBackendSpec& backend) {
            return backend.name == registration.name;
        });
    if (duplicate != impl_->device_backends.end()) {
        return impl_->fail(
            Status::invalid_argument,
            "device backend names must be unique");
    }

    HalV2Capabilities capabilities;
    HalV2Status hal_status = HalV2Status::internal_error;
    try {
        hal_status = registration.api.get_capabilities(
            registration.api.instance, &capabilities);
    } catch (...) {
        hal_status = HalV2Status::internal_error;
    }
    const auto status = detail::hal_v2_status_to_runtime(hal_status);
    if (status != Status::ok) {
        return impl_->fail(status, "HAL v2 backend capability query failed");
    }
    if (!detail::validate_hal_v2_capabilities(capabilities)) {
        return impl_->fail(
            Status::invalid_argument,
            "HAL v2 backend reported malformed capabilities");
    }
    std::unique_ptr<detail::HeterogeneousMemoryState> memory_state;
    std::unique_ptr<detail::CommandTimelineExtensionState> command_state;
    try {
      memory_state = std::make_unique<detail::HeterogeneousMemoryState>();
    } catch (const std::bad_alloc &) {
      return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
      return impl_->fail(Status::internal_error, nullptr);
    }
    if (registration.memory_topology) {
      const auto memory_status = detail::discover_memory_topology(
          *registration.memory_topology, *memory_state);
      if (memory_status != Status::ok) {
        return impl_->fail(
            memory_status,
            "HAL v2 memory/topology discovery failed or was malformed");
      }
    } else {
      detail::make_implicit_host_memory_state(capabilities, *memory_state);
    }
    if (registration.command_timeline) {
      if (!registration.memory_topology ||
          memory_state->snapshot.completion_timestamp_domain_identity == 0) {
        return impl_->fail(
            Status::invalid_argument,
            "command/timeline extension requires a native completion "
            "timestamp domain");
      }
      try {
        command_state =
            std::make_unique<detail::CommandTimelineExtensionState>();
      } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
      } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
      }
      const auto command_status = detail::discover_command_timeline_extension(
          *registration.command_timeline, *command_state);
      if (command_status != Status::ok) {
        return impl_->fail(
            command_status,
            "HAL v2 command/timeline capability discovery failed or was "
            "malformed");
      }
      if (command_state->capabilities.max_in_flight_batches <
              impl_->config.device_outstanding_capacity ||
          command_state->capabilities.completion_batch_capacity <
              impl_->config.device_completion_batch) {
        return impl_->fail(
            Status::capacity_exceeded,
            "command/timeline backend capacities are below Runtime bounds");
      }
    }
    try {
        const auto index = static_cast<std::uint32_t>(
            impl_->device_backends.size());
        auto *memory_instance = memory_state.get();
        auto *command_instance = command_state.get();
        impl_->device_memory_states.push_back(std::move(memory_state));
        try {
          impl_->device_command_states.push_back(std::move(command_state));
        } catch (...) {
          impl_->device_memory_states.pop_back();
          throw;
        }
        try {
          impl_->device_backends.push_back(detail::DeviceBackendSpec{
              std::string(registration.name),
              registration.api,
              capabilities,
              detail::HalBackendKind::native_hal_v2,
              nullptr,
              memory_instance,
              command_instance,
          });
        } catch (...) {
          impl_->device_command_states.pop_back();
          impl_->device_memory_states.pop_back();
          throw;
        }
        out_backend = DeviceBackendHandle{impl_->graph_owner, index};
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_device_buffer(
    const DeviceBufferRegistration& registration,
    DeviceBufferHandle& out_buffer) noexcept {
    out_buffer = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "device buffer registration is frozen");
    }
    if (!impl_->valid_device_backend(registration.backend)) {
        return impl_->fail(
            Status::invalid_handle,
            "device buffer references an invalid or foreign backend");
    }
    std::array<char, RTFW_DEVICE_IDENTIFIER_CAPACITY> name{};
    if (!set_identifier(name, registration.name) ||
        registration.storage.empty()) {
        return impl_->fail(
            Status::invalid_argument,
            "device buffer requires a stable name and non-empty storage");
    }
    constexpr auto allowed_flags =
        RTFW_DEVICE_BUFFER_HOST_READ |
        RTFW_DEVICE_BUFFER_HOST_WRITE |
        RTFW_DEVICE_BUFFER_DEVICE_READ |
        RTFW_DEVICE_BUFFER_DEVICE_WRITE;
    if (registration.flags == 0 ||
        (registration.flags & ~allowed_flags) != 0) {
        return impl_->fail(
            Status::invalid_argument,
            "device buffer flags are invalid");
    }
    if (impl_->device_buffers.size() >=
        impl_->config.device_buffer_capacity) {
        return impl_->fail(
            Status::capacity_exceeded,
            "configured device buffer capacity exceeded");
    }
    const auto backend_index = static_cast<std::size_t>(
        registration.backend.index());
    const auto& backend = impl_->device_backends[backend_index];
    if (registration.storage.size() >
        backend.capabilities.max_buffer_bytes) {
        return impl_->fail(
            Status::capacity_exceeded,
            "device buffer exceeds backend byte capacity");
    }
    std::size_t backend_buffer_count = 0;
    for (const auto& buffer : impl_->device_buffers) {
        if (buffer.backend_index == backend_index) {
            ++backend_buffer_count;
        }
        if (buffer.name == name) {
            return impl_->fail(
                Status::invalid_argument,
                "device buffer names must be unique");
        }
        if (byte_spans_overlap(
                std::as_bytes(buffer.storage),
                std::as_bytes(registration.storage))) {
            return impl_->fail(
                Status::invalid_argument,
                "registered device buffer regions must not overlap");
        }
    }
    if (backend_buffer_count >=
        backend.capabilities.max_registered_buffers) {
        return impl_->fail(
            Status::capacity_exceeded,
            "backend registered-buffer capacity exceeded");
    }
    const auto *host_domain =
        backend.memory_state ? detail::find_legacy_host_domain(
                                   *backend.memory_state, registration.flags)
                             : nullptr;
    if (!host_domain) {
      return impl_->fail(
          Status::invalid_argument,
          "device backend has no coherent borrowed-host memory domain");
    }
    if (registration.storage.size() > host_domain->maximum_bytes ||
        registration.storage.size() % host_domain->byte_granularity != 0 ||
        reinterpret_cast<std::uintptr_t>(registration.storage.data()) %
                host_domain->alignment !=
            0) {
      return impl_->fail(
          Status::invalid_argument,
          "legacy host buffer violates its discovered host-domain contract");
    }

    try {
        const auto index = static_cast<std::uint32_t>(
            impl_->device_buffers.size());
        impl_->device_buffers.push_back(detail::DeviceBufferSpec{
            name,
            static_cast<std::uint32_t>(backend_index),
            registration.storage,
            registration.flags,
            registration.storage.size(),
            host_domain->identity,
        });
        out_buffer = DeviceBufferHandle{impl_->graph_owner, index};
    } catch (const std::bad_alloc &) {
      return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
      return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_device_buffer(
    const HeterogeneousDeviceBufferRegistration& registration,
    DeviceBufferHandle &out_buffer) noexcept {
  out_buffer = {};
  if (!impl_) {
    return Status::internal_error;
  }
  if (impl_->provider_callback_active()) {
    return Status::invalid_state;
  }
  if (impl_->state != RuntimeState::configuring) {
    return impl_->fail(Status::invalid_state,
                       "heterogeneous memory registration is frozen");
  }
  if (!impl_->valid_device_backend(registration.backend) ||
      !registration.domain.valid() ||
      registration.domain.backend != registration.backend) {
    return impl_->fail(
        Status::invalid_handle,
        "heterogeneous memory references a foreign backend or domain");
  }
  const auto backend_index =
      static_cast<std::size_t>(registration.backend.index());
  const auto &backend = impl_->device_backends[backend_index];
  if (!backend.memory_state || !backend.memory_state->native_extension) {
    return impl_->fail(
        Status::invalid_argument,
        "heterogeneous memory requires a native memory/topology extension");
  }
  const auto *domain = detail::find_memory_domain(*backend.memory_state,
                                                  registration.domain.identity);
  if (!domain) {
    return impl_->fail(
        Status::invalid_handle,
        "heterogeneous memory domain is unknown to this backend");
  }
  std::array<char, RTFW_DEVICE_IDENTIFIER_CAPACITY> name{};
  if (!set_identifier(name, registration.name) || registration.bytes == 0 ||
      !detail::valid_memory_access(registration.access) ||
      !detail::valid_memory_synchronization(registration.synchronization)) {
    return impl_->fail(Status::invalid_argument,
                       "heterogeneous memory descriptor is malformed");
  }
  const auto ownership = detail::ownership_bit(registration.ownership);
  const auto coherency = static_cast<std::uint32_t>(registration.coherency);
  const bool host_backed = !registration.host_storage.empty();
  const bool opaque_backed = registration.opaque_handle.size != 0;
  if (ownership == 0 || (domain->ownership_modes & ownership) == 0 ||
      (registration.access & ~domain->access) != 0 ||
      coherency != domain->coherency ||
      registration.synchronization != domain->required_synchronization ||
      host_backed == opaque_backed ||
      !detail::validate_opaque_handle(registration.opaque_handle,
                                      opaque_backed) ||
      (host_backed &&
       (registration.ownership != HalV2MemoryOwnership::borrowed_host ||
        registration.bytes != registration.host_storage.size())) ||
      (opaque_backed &&
       registration.ownership == HalV2MemoryOwnership::borrowed_host) ||
      registration.bytes > domain->maximum_bytes ||
      registration.bytes > std::numeric_limits<std::size_t>::max() ||
      registration.bytes % domain->byte_granularity != 0) {
    return impl_->fail(Status::invalid_argument,
                       "heterogeneous memory backing or declared capabilities "
                       "conflict with its domain");
  }
  if (host_backed &&
      reinterpret_cast<std::uintptr_t>(registration.host_storage.data()) %
              domain->alignment !=
          0) {
    return impl_->fail(
        Status::invalid_argument,
        "heterogeneous host memory does not satisfy domain alignment");
  }
  if (impl_->device_buffers.size() >= impl_->config.device_buffer_capacity) {
    return impl_->fail(Status::capacity_exceeded,
                       "configured device buffer capacity exceeded");
  }
  if (registration.bytes > backend.capabilities.max_buffer_bytes) {
    return impl_->fail(Status::capacity_exceeded,
                       "heterogeneous memory exceeds backend byte capacity");
  }
  std::size_t backend_buffer_count = 0;
  for (const auto &buffer : impl_->device_buffers) {
    if (buffer.backend_index == backend_index) {
      ++backend_buffer_count;
    }
    if (buffer.name == name) {
      return impl_->fail(Status::invalid_argument,
                         "device buffer names must be unique");
    }
    if (host_backed && !buffer.storage.empty() &&
        byte_spans_overlap(std::as_bytes(buffer.storage),
                           std::as_bytes(registration.host_storage))) {
      return impl_->fail(
          Status::invalid_argument,
          "comparable heterogeneous host regions must not overlap");
    }
  }
  if (backend_buffer_count >= backend.capabilities.max_registered_buffers) {
    return impl_->fail(Status::capacity_exceeded,
                       "backend registered-buffer capacity exceeded");
  }

  try {
    const auto index = static_cast<std::uint32_t>(impl_->device_buffers.size());
    impl_->device_buffers.push_back(detail::DeviceBufferSpec{
        name,
        static_cast<std::uint32_t>(backend_index),
        registration.host_storage,
        registration.access,
        registration.bytes,
        domain->identity,
        static_cast<std::uint32_t>(registration.ownership),
        coherency,
        registration.synchronization,
        true,
        registration.opaque_handle,
    });
    out_buffer = DeviceBufferHandle{impl_->graph_owner, index};
  } catch (const std::bad_alloc &) {
    return impl_->fail(Status::resource_exhausted, nullptr);
  } catch (...) {
    return impl_->fail(Status::internal_error, nullptr);
  }
  impl_->clear_error();
  return Status::ok;
}

Status Runtime::register_device_phase(
    const DevicePhaseRegistration& registration,
    PhaseHandle& out_phase) noexcept {
    out_phase = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "device phase registration is frozen");
    }
    if (registration.name.empty() ||
        !registration.callback) {
        return impl_->fail(
            Status::invalid_argument,
            "device phase name and command provider are required");
    }
    if (!impl_->valid_device_backend(registration.backend)) {
        return impl_->fail(
            Status::invalid_handle,
            "device phase references an invalid or foreign backend");
    }
    if (impl_->callbacks.size() >=
        impl_->config.callback_capacity) {
        return impl_->fail(Status::capacity_exceeded, nullptr);
    }
    const auto duplicate = std::find_if(
        impl_->callbacks.begin(),
        impl_->callbacks.end(),
        [&](const Impl::RegisteredCallback& callback) {
            return callback.name == registration.name;
        });
    if (duplicate != impl_->callbacks.end()) {
        return impl_->fail(
            Status::invalid_argument,
            "phase names must be unique");
    }

    try {
        const auto index = static_cast<std::uint32_t>(
            impl_->callbacks.size());
        impl_->callbacks.push_back(Impl::RegisteredCallback{
            std::string(registration.name),
            Impl::PhaseKind::device,
            nullptr,
            registration.callback,
            nullptr,
            registration.user_data,
            registration.backend.index(),
            {},
        });
        out_phase = PhaseHandle{impl_->graph_owner, index};
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_device_timeline(
    const DeviceTimelineRegistration& registration,
    DeviceTimelineHandle& out_timeline) noexcept {
    out_timeline = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state,
                           "device timeline registration is frozen");
    }
    if (!impl_->valid_device_backend(registration.backend)) {
        return impl_->fail(Status::invalid_handle,
                           "device timeline references a foreign backend");
    }
    const auto backend_index =
        static_cast<std::size_t>(registration.backend.index());
    const auto& backend = impl_->device_backends[backend_index];
    std::array<char, hal_v2_identifier_capacity> name{};
    if (!backend.command_state || !set_identifier(name, registration.name)) {
        return impl_->fail(
            Status::invalid_argument,
            "device timeline requires an opted-in backend and stable name");
    }
    std::size_t backend_count = 0;
    for (const auto& timeline : impl_->device_timelines) {
        backend_count += timeline.backend_index == backend_index ? 1u : 0u;
        if (timeline.name == name) {
            return impl_->fail(Status::invalid_argument,
                               "device timeline names must be unique");
        }
    }
    if (backend_count >= hal_v2_timeline_capacity ||
        backend_count >= backend.command_state->capabilities.max_timelines) {
        return impl_->fail(Status::capacity_exceeded,
                           "device timeline capacity exceeded");
    }
    if (impl_->device_timelines.size() >= device_handle_index_mask) {
        return impl_->fail(Status::capacity_exceeded, nullptr);
    }
    try {
        const auto index = static_cast<std::uint32_t>(
            impl_->device_timelines.size());
        impl_->device_timelines.push_back(detail::DeviceTimelineSpec{
            name, static_cast<std::uint32_t>(backend_index),
            registration.initial_value});
        out_timeline = DeviceTimelineHandle{impl_->graph_owner, index};
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_device_batch_phase(
    const DeviceBatchPhaseRegistration& registration,
    PhaseHandle& out_phase) noexcept {
    out_phase = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state,
                           "device batch phase registration is frozen");
    }
    if (registration.name.empty() || !registration.callback ||
        !impl_->valid_device_backend(registration.backend)) {
        return impl_->fail(
            Status::invalid_argument,
            "device batch phase requires a name, provider, and backend");
    }
    const auto backend_index =
        static_cast<std::size_t>(registration.backend.index());
    if (!impl_->device_backends[backend_index].command_state ||
        !detail::validate_batch_declaration(registration.declaration)) {
        return impl_->fail(
            Status::invalid_argument,
            "device batch declaration or backend extension is invalid");
    }
    const auto valid_reference = [&](const HalV2BufferReference& reference) {
        const DeviceBufferHandle handle{reference.buffer_token};
        return impl_->valid_device_buffer(handle) &&
               impl_->device_buffers[handle.index()].backend_index ==
                   backend_index;
    };
    for (std::size_t index = 0;
         index < registration.declaration.command_count; ++index) {
        const auto& command = registration.declaration.commands[index];
        const auto kind = static_cast<HalV2CommandKind>(command.kind);
        if (kind == HalV2CommandKind::dispatch) {
            for (std::size_t ref = 0; ref < command.buffer_count; ++ref) {
                if (!valid_reference(command.buffers[ref])) {
                    return impl_->fail(
                        Status::invalid_handle,
                        "device batch declaration references a foreign buffer");
                }
            }
        } else if (kind == HalV2CommandKind::copy) {
            if (!valid_reference(command.source) ||
                !valid_reference(command.destination)) {
                return impl_->fail(
                    Status::invalid_handle,
                    "device copy declaration references a foreign buffer");
            }
        } else if (!valid_reference(command.target)) {
            return impl_->fail(
                Status::invalid_handle,
                "device synchronization declaration references a foreign buffer");
        }
    }
    const auto valid_point = [&](const HalV2TimelinePoint& point) {
        const DeviceTimelineHandle handle{point.timeline_handle};
        return impl_->valid_device_timeline(handle) &&
               impl_->device_timelines[handle.index()].backend_index ==
                   backend_index;
    };
    for (std::size_t index = 0; index < registration.declaration.wait_count;
         ++index) {
        if (!valid_point(registration.declaration.waits[index])) {
            return impl_->fail(
                Status::invalid_handle,
                "device batch wait declaration references a foreign timeline");
        }
    }
    for (std::size_t index = 0; index < registration.declaration.signal_count;
         ++index) {
        if (!valid_point(registration.declaration.signals[index])) {
            return impl_->fail(
                Status::invalid_handle,
                "device batch signal declaration references a foreign timeline");
        }
    }
    if (impl_->callbacks.size() >= impl_->config.callback_capacity) {
        return impl_->fail(Status::capacity_exceeded, nullptr);
    }
    const auto duplicate = std::find_if(
        impl_->callbacks.begin(), impl_->callbacks.end(),
        [&](const Impl::RegisteredCallback& callback) {
            return callback.name == registration.name;
        });
    if (duplicate != impl_->callbacks.end()) {
        return impl_->fail(Status::invalid_argument,
                           "phase names must be unique");
    }
    try {
        const auto index = static_cast<std::uint32_t>(impl_->callbacks.size());
        impl_->callbacks.push_back(Impl::RegisteredCallback{
            std::string(registration.name), Impl::PhaseKind::device_batch,
            nullptr, nullptr, registration.callback, registration.user_data,
            registration.backend.index(), registration.declaration});
        out_phase = PhaseHandle{impl_->graph_owner, index};
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_rate_domain(
    const RateDomainRegistration& registration,
    RateDomainHandle& out_domain) noexcept {
    out_domain = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state, "rate-domain registration is frozen");
    }
    std::array<char, rate_domain_name_capacity> name{};
    if (!set_identifier(name, registration.name) ||
        registration.period_ns == 0 ||
        registration.substep_count == 0 ||
        registration.substep_count > rate_domain_substep_capacity ||
        (registration.criticality != RateCriticality::background &&
         registration.criticality != RateCriticality::normal &&
         registration.criticality != RateCriticality::critical)) {
        return impl_->fail(
            Status::invalid_argument,
            "rate domain has an invalid name, period, substeps, or criticality");
    }
    if (impl_->rate_domains.size() >= rate_domain_capacity) {
        return impl_->fail(Status::capacity_exceeded, "rate-domain capacity exceeded");
    }
    if (std::any_of(
            impl_->rate_domains.begin(),
            impl_->rate_domains.end(),
            [&](const detail::RateDomainSpec& domain) {
                return domain.name == registration.name;
            })) {
        return impl_->fail(Status::invalid_argument, "rate-domain names must be unique");
    }
    try {
        const auto index = static_cast<std::uint32_t>(impl_->rate_domains.size());
        impl_->rate_domains.push_back({
            std::string(registration.name),
            registration.period_ns,
            registration.substep_count,
            registration.relative_deadline_ns,
            registration.budget_wcet_ns,
            registration.criticality,
            registration.optional,
            registration.late_action,
            registration.bounded_catch_up_limit,
        });
        out_domain = RateDomainHandle{impl_->graph_owner, index};
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::replace_rate_domain(
    RateDomainHandle domain,
    const RateDomainRegistration& registration) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state, "rate-domain replacement is frozen");
    }
    if (!impl_->valid_rate_domain(domain)) {
        return impl_->fail(Status::invalid_handle, "rate-domain handle is invalid or foreign");
    }
    std::array<char, rate_domain_name_capacity> name{};
    if (!set_identifier(name, registration.name) ||
        registration.period_ns == 0 || registration.substep_count == 0 ||
        registration.substep_count > rate_domain_substep_capacity ||
        (registration.criticality != RateCriticality::background &&
         registration.criticality != RateCriticality::normal &&
         registration.criticality != RateCriticality::critical)) {
        return impl_->fail(Status::invalid_argument, "replacement rate domain is malformed");
    }
    for (std::size_t index = 0; index < impl_->rate_domains.size(); ++index) {
        if (index != domain.index() &&
            impl_->rate_domains[index].name == registration.name) {
            return impl_->fail(Status::invalid_argument, "rate-domain names must be unique");
        }
    }
    try {
        detail::RateDomainSpec candidate{
            std::string(registration.name),
            registration.period_ns,
            registration.substep_count,
            registration.relative_deadline_ns,
            registration.budget_wcet_ns,
            registration.criticality,
            registration.optional,
            registration.late_action,
            registration.bounded_catch_up_limit,
        };
        impl_->rate_domains[domain.index()] = std::move(candidate);
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::bind_phase_to_rate_domain(
    PhaseHandle phase,
    RateDomainHandle domain) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state, "rate-phase bindings are frozen");
    }
    if (!impl_->valid_phase(phase) || !impl_->valid_rate_domain(domain)) {
        return impl_->fail(Status::invalid_handle, "rate binding contains an invalid or foreign handle");
    }
    if (impl_->callbacks[phase.index()].kind ==
        Impl::PhaseKind::device_batch) {
        return impl_->fail(
            Status::invalid_argument,
            "device batch phases cannot use M16 device-rate execution");
    }
    if (std::any_of(
            impl_->rate_bindings.begin(),
            impl_->rate_bindings.end(),
            [&](const detail::RateBindingSpec& binding) {
                return binding.phase == phase;
            })) {
        return impl_->fail(Status::invalid_argument, "phase already has a rate-domain owner");
    }
    try {
        impl_->rate_bindings.push_back({
            phase,
            domain,
            impl_->callbacks[phase.index()].kind == Impl::PhaseKind::cpu
                ? RatePhaseKind::cpu
                : RatePhaseKind::device,
        });
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::bind_phase_to_rate_domain(
    const RatePhaseBinding& binding) noexcept {
    return bind_phase_to_rate_domain(binding.phase, binding.domain);
}

Status Runtime::bind_device_phase_to_rate_domain(
    const DeviceRatePhaseBinding& binding) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state,
                           "device-rate bindings are frozen");
    }
    if (!impl_->valid_phase(binding.phase) ||
        !impl_->valid_rate_domain(binding.domain)) {
        return impl_->fail(Status::invalid_handle,
                           "device-rate binding contains an invalid or foreign handle");
    }
    const auto& callback = impl_->callbacks[binding.phase.index()];
    if (callback.kind != Impl::PhaseKind::device_batch) {
        return impl_->fail(
            Status::invalid_argument,
            "active device-rate admission requires a HAL-v2 command-batch phase");
    }
    if (binding.completion_budget_ns == 0 ||
        binding.maximum_in_flight == 0) {
        return impl_->fail(Status::invalid_argument,
                           "device-rate completion budget and in-flight demand must be positive");
    }
    std::size_t reference_count = 0;
    for (std::size_t index = 0;
         index < callback.batch_declaration.command_count; ++index) {
        const auto& command = callback.batch_declaration.commands[index];
        const auto kind = static_cast<HalV2CommandKind>(command.kind);
        reference_count += kind == HalV2CommandKind::dispatch
            ? command.buffer_count
            : kind == HalV2CommandKind::copy ? 2u : 1u;
    }
    if (binding.payload_roles.size() != reference_count ||
        std::any_of(
            binding.payload_roles.begin(), binding.payload_roles.end(),
            [](DeviceRatePayloadRole role) {
                return role != DeviceRatePayloadRole::input &&
                    role != DeviceRatePayloadRole::output &&
                    role != DeviceRatePayloadRole::input_output;
            })) {
        return impl_->fail(Status::invalid_argument,
                           "device-rate payload roles must exactly cover the copied batch references");
    }
    if (std::any_of(
            impl_->rate_bindings.begin(), impl_->rate_bindings.end(),
            [&](const auto& existing) {
                return existing.phase == binding.phase;
            })) {
        return impl_->fail(Status::invalid_argument,
                           "phase already has a rate-domain owner");
    }
    try {
        auto rate_bindings = impl_->rate_bindings;
        auto device_bindings = impl_->device_rate_bindings;
        rate_bindings.push_back({
            binding.phase, binding.domain, RatePhaseKind::device});
        device_bindings.push_back({
            binding.phase,
            binding.domain,
            binding.completion_budget_ns,
            binding.maximum_in_flight,
            std::vector<DeviceRatePayloadRole>(
                binding.payload_roles.begin(), binding.payload_roles.end()),
        });
        impl_->rate_bindings.swap(rate_bindings);
        impl_->device_rate_bindings.swap(device_bindings);
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::replace_device_rate_binding(
    const DeviceRatePhaseBinding& binding) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state,
                           "device-rate binding replacement is frozen");
    }
    if (!impl_->valid_phase(binding.phase) ||
        !impl_->valid_rate_domain(binding.domain) ||
        impl_->callbacks[binding.phase.index()].kind !=
            Impl::PhaseKind::device_batch ||
        binding.completion_budget_ns == 0 ||
        binding.maximum_in_flight == 0) {
        return impl_->fail(Status::invalid_argument,
                           "replacement device-rate binding is malformed");
    }
    const auto existing = std::find_if(
        impl_->device_rate_bindings.begin(),
        impl_->device_rate_bindings.end(), [&](const auto& candidate) {
            return candidate.phase == binding.phase;
        });
    const auto rate_existing = std::find_if(
        impl_->rate_bindings.begin(), impl_->rate_bindings.end(),
        [&](const auto& candidate) { return candidate.phase == binding.phase; });
    if (existing == impl_->device_rate_bindings.end() ||
        rate_existing == impl_->rate_bindings.end()) {
        return impl_->fail(Status::invalid_handle,
                           "device-rate binding does not exist");
    }
    std::size_t reference_count = 0;
    for (std::size_t index = 0;
         index < impl_->callbacks[binding.phase.index()]
             .batch_declaration.command_count; ++index) {
        const auto& command = impl_->callbacks[binding.phase.index()]
            .batch_declaration.commands[index];
        const auto kind = static_cast<HalV2CommandKind>(command.kind);
        reference_count += kind == HalV2CommandKind::dispatch
            ? command.buffer_count
            : kind == HalV2CommandKind::copy ? 2u : 1u;
    }
    if (binding.payload_roles.size() != reference_count) {
        return impl_->fail(Status::invalid_argument,
                           "replacement payload roles do not cover the copied declaration");
    }
    try {
        detail::DeviceRateBindingSpec replacement{
            binding.phase,
            binding.domain,
            binding.completion_budget_ns,
            binding.maximum_in_flight,
            std::vector<DeviceRatePayloadRole>(
                binding.payload_roles.begin(), binding.payload_roles.end()),
        };
        auto rate_bindings = impl_->rate_bindings;
        auto device_bindings = impl_->device_rate_bindings;
        rate_bindings[static_cast<std::size_t>(
            rate_existing - impl_->rate_bindings.begin())].domain =
            binding.domain;
        device_bindings[static_cast<std::size_t>(
            existing - impl_->device_rate_bindings.begin())] =
            std::move(replacement);
        impl_->rate_bindings.swap(rate_bindings);
        impl_->device_rate_bindings.swap(device_bindings);
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_cross_rate_channel(
    const CrossRateChannelRegistration& registration,
    CrossRateChannelHandle& out_channel) noexcept {
    out_channel = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "cross-rate channel registration is frozen");
    }
    std::array<char, cross_rate_channel_name_capacity> name{};
    if (!set_identifier(name, registration.name)) {
        return impl_->fail(
            Status::invalid_argument,
            "cross-rate channel name is malformed");
    }
    if (!impl_->valid_phase(registration.producer) ||
        !impl_->valid_phase(registration.consumer)) {
        return impl_->fail(
            Status::invalid_handle,
            "cross-rate channel endpoint handle is invalid or foreign");
    }
    if (registration.producer == registration.consumer ||
        registration.payload_size == 0 ||
        registration.payload_size > cross_rate_payload_capacity ||
        registration.initial_sample.size() != registration.payload_size ||
        registration.mode != CrossRateMode::sample_and_hold) {
        return impl_->fail(
            Status::invalid_argument,
            "cross-rate channel endpoints, payload, initial sample, or mode is invalid");
    }
    if (impl_->cross_rate_channels.size() >= cross_rate_channel_capacity) {
        return impl_->fail(
            Status::capacity_exceeded,
            "cross-rate channel capacity exceeded");
    }
    for (const auto& channel : impl_->cross_rate_channels) {
        if (channel.name == registration.name) {
            return impl_->fail(
                Status::invalid_argument,
                "cross-rate channel names must be unique");
        }
        if (channel.producer == registration.producer &&
            channel.consumer == registration.consumer) {
            return impl_->fail(
                Status::invalid_argument,
                "duplicate cross-rate semantic edge");
        }
    }
    try {
        detail::CrossRateChannelSpec candidate;
        candidate.name = std::string(registration.name);
        candidate.producer = registration.producer;
        candidate.consumer = registration.consumer;
        candidate.payload_size = registration.payload_size;
        candidate.initial_sample.assign(
            registration.initial_sample.begin(),
            registration.initial_sample.end());
        candidate.mode = registration.mode;
        candidate.maximum_age_ns = registration.maximum_age_ns;
        candidate.producer_device = registration.producer_device;
        candidate.consumer_device = registration.consumer_device;
        const auto index = static_cast<std::uint32_t>(
            impl_->cross_rate_channels.size());
        impl_->cross_rate_channels.push_back(std::move(candidate));
        out_channel = CrossRateChannelHandle{impl_->graph_owner, index};
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::replace_cross_rate_channel(
    CrossRateChannelHandle channel,
    const CrossRateChannelRegistration& registration) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "cross-rate channel replacement is frozen");
    }
    if (!impl_->valid_cross_rate_channel(channel) ||
        !impl_->valid_phase(registration.producer) ||
        !impl_->valid_phase(registration.consumer)) {
        return impl_->fail(
            Status::invalid_handle,
            "cross-rate channel or endpoint handle is invalid or foreign");
    }
    std::array<char, cross_rate_channel_name_capacity> name{};
    if (!set_identifier(name, registration.name) ||
        registration.producer == registration.consumer ||
        registration.payload_size == 0 ||
        registration.payload_size > cross_rate_payload_capacity ||
        registration.initial_sample.size() != registration.payload_size ||
        registration.mode != CrossRateMode::sample_and_hold) {
        return impl_->fail(
            Status::invalid_argument,
            "replacement cross-rate channel is malformed");
    }
    for (std::size_t index = 0;
         index < impl_->cross_rate_channels.size();
         ++index) {
        if (index == channel.index()) {
            continue;
        }
        const auto& existing = impl_->cross_rate_channels[index];
        if (existing.name == registration.name ||
            (existing.producer == registration.producer &&
             existing.consumer == registration.consumer)) {
            return impl_->fail(
                Status::invalid_argument,
                "replacement duplicates a channel name or semantic edge");
        }
    }
    try {
        detail::CrossRateChannelSpec candidate;
        candidate.name = std::string(registration.name);
        candidate.producer = registration.producer;
        candidate.consumer = registration.consumer;
        candidate.payload_size = registration.payload_size;
        candidate.initial_sample.assign(
            registration.initial_sample.begin(),
            registration.initial_sample.end());
        candidate.mode = registration.mode;
        candidate.maximum_age_ns = registration.maximum_age_ns;
        candidate.producer_device = registration.producer_device;
        candidate.consumer_device = registration.consumer_device;
        impl_->cross_rate_channels[channel.index()] = std::move(candidate);
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_sampled_io_channel(
    const SampledIoChannelRegistration& registration) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "sampled-I/O registration is frozen");
    }
    if (!impl_->valid_cross_rate_channel(registration.channel)) {
        return impl_->fail(
            Status::invalid_handle,
            "sampled-I/O channel handle is invalid or foreign");
    }
    if (impl_->sampled_io_channels.size() >= sampled_io_channel_capacity ||
        std::any_of(
            impl_->sampled_io_channels.begin(),
            impl_->sampled_io_channels.end(),
            [&](const detail::SampledIoChannelSpec& existing) {
                return existing.channel == registration.channel ||
                    (registration.channel_identity != 0 &&
                     existing.channel_identity == registration.channel_identity);
            })) {
        return impl_->fail(
            impl_->sampled_io_channels.size() >= sampled_io_channel_capacity
                ? Status::capacity_exceeded
                : Status::invalid_argument,
            "sampled-I/O capacity or unique identity was violated");
    }
    try {
        detail::SampledIoChannelSpec candidate;
        candidate.channel = registration.channel;
        candidate.direction = registration.direction;
        candidate.channel_identity = registration.channel_identity;
        candidate.encoding = registration.encoding;
        candidate.element_count = registration.element_count;
        candidate.samples_per_frame = registration.samples_per_frame;
        candidate.scale_numerator = registration.scale_numerator;
        candidate.scale_denominator = registration.scale_denominator;
        candidate.offset_numerator = registration.offset_numerator;
        candidate.offset_denominator = registration.offset_denominator;
        candidate.units_identity = registration.units_identity;
        candidate.calibration_identity = registration.calibration_identity;
        candidate.sample_period_ns = registration.sample_period_ns;
        candidate.timestamp_domain_identity =
            registration.timestamp_domain_identity;
        candidate.clock_domain_identity = registration.clock_domain_identity;
        candidate.trigger_mode = registration.trigger_mode;
        candidate.trigger_identity = registration.trigger_identity;
        candidate.ring_capacity = registration.ring_capacity;
        candidate.initial_sequence = registration.initial_sequence;
        candidate.maximum_age_ns = registration.maximum_age_ns;
        candidate.stale_policy = registration.stale_policy;
        candidate.overrun_policy = registration.overrun_policy;
        candidate.underrun_policy = registration.underrun_policy;
        candidate.safe_transition_timeout_ns =
            registration.safe_transition_timeout_ns;
        candidate.initial_frame.assign(
            registration.initial_frame.begin(),
            registration.initial_frame.end());
        candidate.startup_safe_frame.assign(
            registration.startup_safe_frame.begin(),
            registration.startup_safe_frame.end());
        candidate.failure_safe_frame.assign(
            registration.failure_safe_frame.begin(),
            registration.failure_safe_frame.end());
        candidate.shutdown_safe_frame.assign(
            registration.shutdown_safe_frame.begin(),
            registration.shutdown_safe_frame.end());
        impl_->sampled_io_channels.push_back(std::move(candidate));
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::replace_sampled_io_channel(
    CrossRateChannelHandle channel,
    const SampledIoChannelRegistration& registration) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "sampled-I/O replacement is frozen");
    }
    if (!impl_->valid_cross_rate_channel(channel) ||
        registration.channel != channel) {
        return impl_->fail(
            Status::invalid_handle,
            "sampled-I/O replacement handle is invalid or changed");
    }
    const auto existing = std::find_if(
        impl_->sampled_io_channels.begin(),
        impl_->sampled_io_channels.end(),
        [&](const detail::SampledIoChannelSpec& value) {
            return value.channel == channel;
        });
    if (existing == impl_->sampled_io_channels.end() ||
        std::any_of(
            impl_->sampled_io_channels.begin(),
            impl_->sampled_io_channels.end(),
            [&](const detail::SampledIoChannelSpec& value) {
                return &value != &*existing &&
                    value.channel_identity == registration.channel_identity;
            })) {
        return impl_->fail(
            Status::invalid_argument,
            "sampled-I/O replacement identity is missing or duplicated");
    }
    try {
        detail::SampledIoChannelSpec candidate;
        candidate.channel = registration.channel;
        candidate.direction = registration.direction;
        candidate.channel_identity = registration.channel_identity;
        candidate.encoding = registration.encoding;
        candidate.element_count = registration.element_count;
        candidate.samples_per_frame = registration.samples_per_frame;
        candidate.scale_numerator = registration.scale_numerator;
        candidate.scale_denominator = registration.scale_denominator;
        candidate.offset_numerator = registration.offset_numerator;
        candidate.offset_denominator = registration.offset_denominator;
        candidate.units_identity = registration.units_identity;
        candidate.calibration_identity = registration.calibration_identity;
        candidate.sample_period_ns = registration.sample_period_ns;
        candidate.timestamp_domain_identity =
            registration.timestamp_domain_identity;
        candidate.clock_domain_identity = registration.clock_domain_identity;
        candidate.trigger_mode = registration.trigger_mode;
        candidate.trigger_identity = registration.trigger_identity;
        candidate.ring_capacity = registration.ring_capacity;
        candidate.initial_sequence = registration.initial_sequence;
        candidate.maximum_age_ns = registration.maximum_age_ns;
        candidate.stale_policy = registration.stale_policy;
        candidate.overrun_policy = registration.overrun_policy;
        candidate.underrun_policy = registration.underrun_policy;
        candidate.safe_transition_timeout_ns =
            registration.safe_transition_timeout_ns;
        candidate.initial_frame.assign(
            registration.initial_frame.begin(),
            registration.initial_frame.end());
        candidate.startup_safe_frame.assign(
            registration.startup_safe_frame.begin(),
            registration.startup_safe_frame.end());
        candidate.failure_safe_frame.assign(
            registration.failure_safe_frame.begin(),
            registration.failure_safe_frame.end());
        candidate.shutdown_safe_frame.assign(
            registration.shutdown_safe_frame.begin(),
            registration.shutdown_safe_frame.end());
        *existing = std::move(candidate);
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_live_control_mailbox(
    const LiveControlMailboxRegistration& registration) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring ||
        !impl_->live_control_policy_set) {
        return impl_->fail(
            Status::invalid_state,
            "live-control mailbox registration requires an enabled configuring policy");
    }
    const auto reserved_is_zero = std::all_of(
        registration.reserved.begin(), registration.reserved.end(),
        [](std::byte value) { return value == std::byte{0}; });
    if (registration.schema_version != live_control_schema_version ||
        registration.struct_size != sizeof(LiveControlMailboxRegistration) ||
        registration.mailbox_identity == 0 ||
        registration.record_capacity == 0 ||
        registration.payload_bytes_per_record == 0 ||
        registration.payload_bytes_per_record >
            impl_->live_control_policy.payload_bytes_per_record ||
        !reserved_is_zero) {
        return impl_->fail(
            Status::invalid_argument,
            "live-control mailbox declaration is malformed");
    }
    if (impl_->live_control_mailbox_declarations.size() >=
        impl_->live_control_policy.mailbox_capacity) {
        return impl_->fail(
            Status::capacity_exceeded,
            "live-control mailbox capacity is exhausted");
    }
    if (std::any_of(
            impl_->live_control_mailbox_declarations.begin(),
            impl_->live_control_mailbox_declarations.end(),
            [&](const LiveControlMailboxRegistration& existing) {
                return existing.mailbox_identity ==
                    registration.mailbox_identity;
            })) {
        return impl_->fail(
            Status::invalid_argument,
            "live-control mailbox identity is duplicated");
    }
    std::size_t total_records = registration.record_capacity;
    std::size_t total_payload = 0;
    if (!detail::checked_multiply(
            registration.record_capacity,
            registration.payload_bytes_per_record,
            total_payload)) {
        return impl_->fail(
            Status::capacity_exceeded,
            "live-control mailbox storage overflows");
    }
    for (const auto& existing :
         impl_->live_control_mailbox_declarations) {
        std::size_t existing_payload = 0;
        if (!detail::checked_add(
                total_records,
                existing.record_capacity,
                total_records) ||
            !detail::checked_multiply(
                existing.record_capacity,
                existing.payload_bytes_per_record,
                existing_payload) ||
            !detail::checked_add(
                total_payload,
                existing_payload,
                total_payload)) {
            return impl_->fail(
                Status::capacity_exceeded,
                "live-control mailbox storage overflows");
        }
    }
    if (total_records > impl_->live_control_policy.record_capacity ||
        total_payload >
            impl_->live_control_policy.total_payload_storage_bytes) {
        return impl_->fail(
            Status::capacity_exceeded,
            "live-control mailbox storage exceeds the configured policy");
    }
    try {
        impl_->live_control_mailbox_declarations.push_back(registration);
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_live_control_producer(
    const LiveControlProducerRegistration& registration) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring ||
        !impl_->live_control_policy_set) {
        return impl_->fail(
            Status::invalid_state,
            "live-control producer registration requires an enabled configuring policy");
    }
    if (registration.schema_version != live_control_schema_version ||
        registration.struct_size != sizeof(LiveControlProducerRegistration) ||
        registration.mailbox_identity == 0 ||
        registration.producer_identity == 0 ||
        registration.first_sequence == 0 ||
        registration.first_sequence ==
            std::numeric_limits<std::uint64_t>::max() ||
        registration.reserved != 0) {
        return impl_->fail(
            Status::invalid_argument,
            "live-control producer declaration is malformed");
    }
    if (impl_->live_control_producer_declarations.size() >=
        impl_->live_control_policy.producer_capacity) {
        return impl_->fail(
            Status::capacity_exceeded,
            "live-control producer capacity is exhausted");
    }
    if (std::none_of(
            impl_->live_control_mailbox_declarations.begin(),
            impl_->live_control_mailbox_declarations.end(),
            [&](const LiveControlMailboxRegistration& mailbox) {
                return mailbox.mailbox_identity ==
                    registration.mailbox_identity;
            }) ||
        std::any_of(
            impl_->live_control_producer_declarations.begin(),
            impl_->live_control_producer_declarations.end(),
            [&](const LiveControlProducerRegistration& existing) {
                return existing.producer_identity ==
                    registration.producer_identity;
            })) {
        return impl_->fail(
            Status::invalid_argument,
            "live-control producer mailbox is missing or identity is duplicated");
    }
    try {
        impl_->live_control_producer_declarations.push_back(registration);
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_resource(
    std::string_view name,
    ResourceHandle& out_resource) noexcept {
    out_resource = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state, "resource registration is frozen");
    }
    if (name.empty()) {
        return impl_->fail(Status::invalid_argument, "resource name is required");
    }
    if (impl_->resources.size() >= kMaxResources) {
        return impl_->fail(Status::capacity_exceeded, "resource safety limit exceeded");
    }
    const auto duplicate = std::find_if(
        impl_->resources.begin(),
        impl_->resources.end(),
        [&](const Impl::RegisteredResource& resource) {
            return resource.name == name;
        });
    if (duplicate != impl_->resources.end()) {
        return impl_->fail(Status::invalid_argument, "resource names must be unique");
    }

    try {
        const auto index = static_cast<std::uint32_t>(impl_->resources.size());
        impl_->resources.push_back(
            Impl::RegisteredResource{std::string(name)});
        out_resource = ResourceHandle{impl_->graph_owner, index};
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }

    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_state(
    const StateRegistration& registration) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "state registration is frozen");
    }
    if (registration.schema_version == 0 ||
        registration.storage.empty()) {
        return impl_->fail(
            Status::invalid_argument,
            "state schema version and non-empty storage are required");
    }
    if (impl_->states.size() >= impl_->config.state_capacity) {
        return impl_->fail(
            Status::capacity_exceeded,
            "configured state capacity exceeded");
    }

    std::array<char, replay_identifier_capacity> name{};
    if (!set_identifier(name, registration.name)) {
        return impl_->fail(
            Status::invalid_argument,
            "state name must be a stable replay identifier");
    }
    const auto duplicate = std::find_if(
        impl_->states.begin(),
        impl_->states.end(),
        [&](const Impl::RegisteredState& state) {
            return state.name == name;
        });
    if (duplicate != impl_->states.end()) {
        return impl_->fail(
            Status::invalid_argument,
            "state names must be unique");
    }
    for (const auto& state : impl_->states) {
        if (byte_spans_overlap(
                std::as_bytes(registration.storage),
                std::as_bytes(state.storage))) {
            return impl_->fail(
                Status::invalid_argument,
                "registered state storage regions must not overlap");
        }
    }

    std::size_t payload_bytes = registration.storage.size();
    std::size_t record_bytes = 0;
    std::size_t required_bytes = 0;
    if (!detail::checked_artifact_multiply(
            impl_->states.size() + 1,
            detail::checkpoint_record_header_size,
            record_bytes)) {
        return impl_->fail(
            Status::capacity_exceeded,
            "registered state size overflows the snapshot format");
    }
    for (const auto& state : impl_->states) {
        if (!detail::checked_artifact_add(
                payload_bytes,
                state.storage.size(),
                payload_bytes)) {
            return impl_->fail(
                Status::capacity_exceeded,
                "registered state size overflows the snapshot format");
        }
    }
    if (!detail::checked_artifact_add(
            detail::checkpoint_header_size,
            record_bytes,
            required_bytes) ||
        !detail::checked_artifact_add(
            required_bytes,
            payload_bytes,
            required_bytes) ||
        required_bytes > impl_->config.snapshot_max_bytes) {
        return impl_->fail(
            Status::capacity_exceeded,
            "registered state exceeds snapshot_max_bytes");
    }

    try {
        impl_->states.push_back(Impl::RegisteredState{
            name,
            registration.schema_version,
            registration.storage,
        });
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::add_dependency(
    PhaseHandle prerequisite,
    PhaseHandle dependent) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state, "graph topology is frozen");
    }
    if (!impl_->valid_phase(prerequisite) ||
        !impl_->valid_phase(dependent)) {
        return impl_->fail(
            Status::invalid_handle,
            "dependency contains an invalid or foreign phase handle");
    }
    if (prerequisite == dependent) {
        return impl_->fail(
            Status::graph_cycle,
            "a phase cannot depend on itself");
    }
    const auto duplicate = std::find_if(
        impl_->dependencies.begin(),
        impl_->dependencies.end(),
        [&](const detail::GraphDependency& dependency) {
            return dependency.prerequisite == prerequisite &&
                   dependency.dependent == dependent;
        });
    if (duplicate != impl_->dependencies.end()) {
        return impl_->fail(
            Status::invalid_argument,
            "dependency is already registered");
    }

    try {
        impl_->dependencies.push_back(
            detail::GraphDependency{prerequisite, dependent});
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }

    impl_->clear_error();
    return Status::ok;
}

Status Runtime::declare_resource_access(
    PhaseHandle phase,
    ResourceHandle resource,
    ResourceAccess access) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state, "graph topology is frozen");
    }
    if (!impl_->valid_phase(phase) ||
        !impl_->valid_resource(resource)) {
        return impl_->fail(
            Status::invalid_handle,
            "resource access contains an invalid or foreign handle");
    }
    if (access != ResourceAccess::read &&
        access != ResourceAccess::write) {
        return impl_->fail(
            Status::invalid_argument,
            "resource access mode is invalid");
    }
    const auto duplicate = std::find_if(
        impl_->resource_accesses.begin(),
        impl_->resource_accesses.end(),
        [&](const detail::GraphResourceAccess& declaration) {
            return declaration.phase == phase &&
                   declaration.resource == resource;
        });
    if (duplicate != impl_->resource_accesses.end()) {
        return impl_->fail(
            Status::invalid_argument,
            "phase already declares access to this resource");
    }

    try {
        impl_->resource_accesses.push_back(
            detail::GraphResourceAccess{phase, resource, access});
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }

    impl_->clear_error();
    return Status::ok;
}

Status Runtime::finalize() noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state, "finalize requires configuring state");
    }
    impl_->device_rate_diagnostic = {};
    if (validate_config(impl_->config) != Status::ok ||
        impl_->callbacks.size() > impl_->config.callback_capacity ||
        impl_->states.size() > impl_->config.state_capacity ||
        impl_->device_backends.size() >
            impl_->config.device_backend_capacity ||
        impl_->device_buffers.size() >
            impl_->config.device_buffer_capacity) {
        return impl_->fail(Status::invalid_config, nullptr);
    }
    if (impl_->mixed_rate_closure_policy_set &&
        !impl_->rate_execution_policy_set) {
        return impl_->fail(
            Status::invalid_config,
            "mixed-rate closure requires the active rate policy");
    }
    if (impl_->config.executor_policy == ExecutorPolicy::host_adapter &&
        (!impl_->host_executor_set ||
         impl_->host_executor.worker_count !=
             impl_->config.worker_count ||
         impl_->host_executor.queue_capacity !=
             impl_->config.executor_queue_capacity)) {
        return impl_->fail(
            Status::invalid_config,
            "host_adapter requires an attached adapter with matching capacities");
    }
    std::size_t backend_reported_bytes = 0;
    for (std::size_t backend_index = 0;
         backend_index < impl_->device_backends.size();
         ++backend_index) {
        const auto& backend =
            impl_->device_backends[backend_index];
        if (impl_->config.device_outstanding_capacity >
            backend.capabilities.max_in_flight) {
            return impl_->fail(
                Status::invalid_config,
                "device_outstanding_capacity exceeds a backend limit");
        }
        if (impl_->config.determinism_tier ==
                DeterminismTier::schedule_independent &&
            backend.capabilities.deterministic_mock == 0) {
            return impl_->fail(
                Status::invalid_config,
                "D1 requires every device backend to declare deterministic_mock");
        }
        if (impl_->mixed_rate_closure_policy_set &&
            impl_->mixed_rate_closure_policy.active_replay_enabled &&
            impl_->mixed_rate_closure_policy.require_deterministic_backends &&
            backend.capabilities.deterministic_mock == 0) {
            return impl_->fail(
                Status::invalid_config,
                "active replay requires deterministic mock/loopback backends");
        }
        if (impl_->live_control_closure_policy_set &&
            impl_->live_control_closure_policy.replay_enabled &&
            backend.capabilities.deterministic_mock == 0) {
            return impl_->fail(
                Status::invalid_config,
                "live-control replay requires deterministic mock/loopback backends");
        }
        std::size_t buffer_count = 0;
        for (const auto& buffer : impl_->device_buffers) {
            buffer_count +=
                buffer.backend_index == backend_index ? 1u : 0u;
        }
        if (buffer_count >
            backend.capabilities.max_registered_buffers) {
            return impl_->fail(
                Status::invalid_config,
                "registered buffers exceed a backend limit");
        }
        std::size_t total = 0;
        if (!detail::checked_add(
                backend_reported_bytes,
                static_cast<std::size_t>(
                    backend.capabilities.control_storage_bytes),
                total)) {
            return impl_->fail(
                Status::invalid_config,
                "backend-reported control storage overflows");
        }
        backend_reported_bytes = total;
        if (backend.command_state) {
            if (!detail::checked_add(
                    backend_reported_bytes,
                    static_cast<std::size_t>(backend.command_state
                        ->capabilities.backend_control_storage_bytes),
                    total)) {
                return impl_->fail(
                    Status::invalid_config,
                    "command backend-reported control storage overflows");
            }
            backend_reported_bytes = total;
        }
    }

    std::vector<PhaseHandle> compiled_order;
    detail::GraphCompileDiagnostic diagnostic;
    const auto compile_status = detail::compile_graph(
        impl_->graph_owner,
        impl_->callbacks.size(),
        impl_->resources.size(),
        impl_->dependencies,
        impl_->resource_accesses,
        compiled_order,
        diagnostic);
    if (compile_status != Status::ok) {
        return impl_->fail_compile(compile_status, diagnostic);
    }

    detail::CompiledRatePlan compiled_rate_plan;
    detail::RateCompileDiagnostic rate_diagnostic;
    const auto rate_status = detail::compile_rate_timeline(
        impl_->graph_owner,
        impl_->callbacks.size(),
        compiled_order,
        impl_->rate_domains,
        impl_->rate_bindings,
        compiled_rate_plan,
        rate_diagnostic);
    if (rate_status != Status::ok) {
        return impl_->fail(rate_status, rate_diagnostic.message);
    }

    std::unique_ptr<detail::LiveControlMailboxSet> live_control_mailboxes;
    if (impl_->live_control_closure_policy_set &&
        !impl_->live_control_policy_set) {
        return impl_->fail(
            Status::invalid_config,
            "live-control closure requires an enabled mailbox policy");
    }
    if (impl_->live_control_policy_set) {
        const auto live_control_runtime_id =
            next_live_control_runtime_id();
        if (live_control_runtime_id == 0) {
            return impl_->fail(
                Status::resource_exhausted,
                "live-control Runtime identity is exhausted");
        }
        const char* live_control_diagnostic = nullptr;
        const auto live_control_status = detail::LiveControlMailboxSet::create(
            impl_->live_control_policy, impl_->live_control_closure_policy,
            impl_->live_control_closure_policy_set,
            impl_->live_control_retention_policy, live_control_runtime_id, 1,
            impl_->config.memory_budget_bytes, impl_->live_control_mailbox_declarations,
            impl_->live_control_producer_declarations, compiled_rate_plan.releases,
            live_control_mailboxes, live_control_diagnostic);
        if (live_control_status != Status::ok) {
            return impl_->fail(
                live_control_status,
                live_control_diagnostic);
        }
    }

    detail::CompiledDeviceRatePlan compiled_device_rate_plan;
    if (!impl_->device_rate_bindings.empty()) {
        if (impl_->config.device_outstanding_capacity >
                std::numeric_limits<std::uint32_t>::max() ||
            impl_->config.device_completion_batch >
                std::numeric_limits<std::uint32_t>::max()) {
            impl_->device_rate_diagnostic.status = Status::invalid_config;
            return impl_->fail(
                Status::invalid_config,
                "device-rate Runtime capacities exceed the compiled model");
        }
        try {
            std::vector<detail::DeviceRateBackendSource> backend_sources;
            std::vector<detail::DeviceRateBufferSource> buffer_sources;
            std::vector<detail::DeviceRateTimelineSource> timeline_sources;
            std::vector<detail::DeviceRatePhaseSource> phase_sources;
            backend_sources.reserve(impl_->device_backends.size());
            buffer_sources.reserve(impl_->device_buffers.size());
            timeline_sources.reserve(impl_->device_timelines.size());
            phase_sources.reserve(impl_->device_rate_bindings.size());
            for (std::size_t index = 0;
                 index < impl_->device_backends.size(); ++index) {
                const auto& backend = impl_->device_backends[index];
                detail::DeviceRateBackendSource source;
                source.backend = DeviceBackendHandle{
                    impl_->graph_owner, static_cast<std::uint32_t>(index)};
                if (backend.command_state) {
                    source.capabilities =
                        backend.command_state->capabilities;
                }
                if (backend.memory_state) {
                    const auto identity = backend.memory_state->snapshot
                        .completion_timestamp_domain_identity;
                    const auto* domain = detail::find_timestamp_domain(
                        *backend.memory_state, identity);
                    source.completion_timestamp_domain_identity = identity;
                    source.completion_timestamp_domain_valid =
                        domain && domain->monotonic != 0;
                }
                backend_sources.push_back(source);
            }
            for (std::size_t index = 0;
                 index < impl_->device_buffers.size(); ++index) {
                const auto& buffer = impl_->device_buffers[index];
                const auto& backend =
                    impl_->device_backends[buffer.backend_index];
                const auto* domain = backend.memory_state
                    ? detail::find_memory_domain(
                          *backend.memory_state, buffer.domain_identity)
                    : nullptr;
                buffer_sources.push_back({
                    DeviceBufferHandle{
                        impl_->graph_owner, static_cast<std::uint32_t>(index)},
                    DeviceBackendHandle{
                        impl_->graph_owner, buffer.backend_index},
                    buffer.bytes,
                    buffer.flags,
                    domain ? domain->byte_granularity : 1u,
                    domain ? domain->offset_granularity : 1u,
                });
            }
            for (std::size_t index = 0;
                 index < impl_->device_timelines.size(); ++index) {
                const auto& timeline = impl_->device_timelines[index];
                timeline_sources.push_back({
                    DeviceTimelineHandle{
                        impl_->graph_owner, static_cast<std::uint32_t>(index)},
                    DeviceBackendHandle{
                        impl_->graph_owner, timeline.backend_index},
                    timeline.initial_value,
                });
            }
            for (const auto& binding : impl_->device_rate_bindings) {
                const auto& callback = impl_->callbacks[binding.phase.index()];
                phase_sources.push_back({
                    binding.phase,
                    DeviceBackendHandle{
                        impl_->graph_owner, callback.device_backend_index},
                    &callback.batch_declaration,
                });
            }
            detail::DeviceRateDiagnostic device_rate_diagnostic;
            const auto device_rate_status = detail::compile_device_rate_plan(
                impl_->graph_owner,
                static_cast<std::uint32_t>(
                    impl_->config.device_outstanding_capacity),
                static_cast<std::uint32_t>(
                    impl_->config.device_completion_batch),
                compiled_rate_plan,
                impl_->device_rate_bindings,
                backend_sources,
                buffer_sources,
                timeline_sources,
                phase_sources,
                compiled_device_rate_plan,
                device_rate_diagnostic);
            if (device_rate_status != Status::ok) {
                impl_->device_rate_diagnostic = {
                    device_rate_status,
                    device_rate_diagnostic.reference_index,
                    device_rate_diagnostic.phase,
                };
                return impl_->fail(
                    device_rate_status, device_rate_diagnostic.message);
            }
        } catch (const std::bad_alloc&) {
            impl_->device_rate_diagnostic.status =
                Status::resource_exhausted;
            return impl_->fail(Status::resource_exhausted, nullptr);
        } catch (...) {
            impl_->device_rate_diagnostic.status = Status::internal_error;
            return impl_->fail(Status::internal_error, nullptr);
        }
    }

    std::vector<detail::CrossRateDeviceBufferSource>
        cross_rate_device_buffers;
    try {
        cross_rate_device_buffers.reserve(impl_->device_buffers.size());
        for (std::size_t index = 0;
             index < impl_->device_buffers.size(); ++index) {
            const auto& buffer = impl_->device_buffers[index];
            const auto& backend =
                impl_->device_backends[buffer.backend_index];
            const auto* domain = backend.memory_state
                ? detail::find_memory_domain(
                      *backend.memory_state, buffer.domain_identity)
                : nullptr;
            cross_rate_device_buffers.push_back({
                DeviceBufferHandle{
                    impl_->graph_owner, static_cast<std::uint32_t>(index)},
                DeviceBackendHandle{
                    impl_->graph_owner, buffer.backend_index},
                buffer.storage,
                buffer.bytes,
                buffer.flags,
                buffer.coherency,
                buffer.synchronization,
                domain ? domain->byte_granularity : 1u,
                domain ? domain->offset_granularity : 1u,
            });
        }
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }

    detail::CompiledRateDispatchPlan compiled_rate_dispatch_plan;
    if (impl_->rate_execution_policy_set) {
        detail::RateDispatchDiagnostic dispatch_diagnostic;
        const auto dispatch_status = detail::compile_rate_dispatch(
            impl_->graph_owner,
            impl_->config.determinism_tier,
            impl_->rate_execution_policy,
            impl_->dependencies,
            compiled_rate_plan,
            impl_->cross_rate_channels,
            impl_->device_rate_bindings.empty()
                ? nullptr
                : &compiled_device_rate_plan,
            compiled_rate_dispatch_plan,
            dispatch_diagnostic);
        if (dispatch_status != Status::ok) {
            return impl_->fail(
                dispatch_status,
                dispatch_diagnostic.message);
        }
    }

    detail::CompiledCrossRatePlan compiled_cross_rate_plan;
    detail::CrossRateCompileDiagnostic cross_rate_diagnostic;
    const auto cross_rate_status = detail::compile_cross_rate_data(
        impl_->graph_owner,
        impl_->callbacks.size(),
        compiled_rate_plan,
        impl_->cross_rate_channels,
        impl_->device_rate_bindings.empty()
            ? nullptr
            : &compiled_device_rate_plan,
        cross_rate_device_buffers,
        compiled_cross_rate_plan,
        cross_rate_diagnostic);
    if (cross_rate_status != Status::ok) {
        return impl_->fail(
            cross_rate_status,
            cross_rate_diagnostic.message);
    }

    detail::CompiledSampledIoPlan compiled_sampled_io_plan;
    detail::SampledIoCompileDiagnostic sampled_io_diagnostic;
    const auto sampled_io_status = detail::compile_sampled_io(
        impl_->graph_owner,
        impl_->sampled_io_channels,
        compiled_rate_plan,
        compiled_device_rate_plan,
        impl_->cross_rate_channels,
        compiled_cross_rate_plan,
        compiled_sampled_io_plan,
        sampled_io_diagnostic);
    if (sampled_io_status != Status::ok) {
        return impl_->fail(
            sampled_io_status,
            sampled_io_diagnostic.message);
    }

    std::vector<SampledIoChannelStatus> sampled_io_statuses;
    try {
        sampled_io_statuses.reserve(compiled_sampled_io_plan.channels.size());
        for (const auto& channel : compiled_sampled_io_plan.channels) {
            sampled_io_statuses.push_back({
                channel.public_record.channel,
                channel.public_record.direction == SampledIoDirection::output
                    ? SampledIoSafetyState::unknown
                    : SampledIoSafetyState::not_applicable,
                channel.public_record.initial_sequence,
                1,
                0,
                0,
                0,
                0,
                Status::ok,
            });
        }
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }

    std::vector<Impl::ActiveChannelState> active_channel_states;
    std::vector<std::byte> active_committed_payloads;
    std::vector<std::byte> active_staging_payloads;
    std::vector<std::byte> active_checkpoint_state;
    std::vector<std::byte> mixed_rate_checkpoint_state;
    std::unique_ptr<std::atomic<std::uint8_t>[]>
        active_publication_claims;
    std::vector<detail::DeviceRateTicket> active_device_tickets;
    std::unique_ptr<Impl::ActiveSheddingState> active_shedding_state;
    std::unique_ptr<detail::RateTelemetryRing> rate_telemetry;
    std::unique_ptr<detail::RateCounters> rate_counters;
    std::unique_ptr<detail::MixedRateActionRing> mixed_rate_actions;
    if (impl_->mixed_rate_closure_policy_set) {
        try {
            mixed_rate_actions =
                std::make_unique<detail::MixedRateActionRing>(
                    impl_->mixed_rate_closure_policy.action_capacity);
            std::size_t sampled_state_bytes = 0;
            std::size_t checkpoint_bytes = 0;
            if (!detail::checked_multiply(
                    compiled_sampled_io_plan.channels.size(),
                    kMixedRateSampledStateBytes,
                    sampled_state_bytes) ||
                !detail::checked_add(
                    kMixedRateStateHeaderBytes,
                    sampled_state_bytes,
                    checkpoint_bytes)) {
                return impl_->fail(
                    Status::capacity_exceeded,
                    "mixed-rate checkpoint state size overflows");
            }
            mixed_rate_checkpoint_state.resize(checkpoint_bytes);
        } catch (const std::bad_alloc&) {
            return impl_->fail(Status::resource_exhausted, nullptr);
        } catch (...) {
            return impl_->fail(
                Status::invalid_config,
                "mixed-rate action ring allocation failed");
        }
    }
    if (impl_->rate_execution_policy_set) {
        try {
            rate_telemetry = std::make_unique<detail::RateTelemetryRing>(
                impl_->rate_execution_policy.rate_telemetry_capacity);
            rate_counters = std::make_unique<detail::RateCounters>();
            rate_counters->reset(
                impl_->rate_execution_policy.host_policy_version);
            active_device_tickets.resize(
                compiled_rate_plan.releases.size());
            if (!compiled_rate_dispatch_plan.optional_shed_order.empty()) {
                active_shedding_state =
                    std::make_unique<Impl::ActiveSheddingState>();
            }
            active_channel_states.reserve(
                impl_->cross_rate_channels.size());
            std::size_t payload_bytes = 0;
            for (const auto& channel : impl_->cross_rate_channels) {
                if (!detail::checked_add(
                        payload_bytes,
                        channel.payload_size,
                        payload_bytes)) {
                    return impl_->fail(
                        Status::capacity_exceeded,
                        "active cross-rate payload storage overflows");
                }
            }
            active_committed_payloads.resize(payload_bytes);
            active_staging_payloads.resize(payload_bytes);
            if (!impl_->cross_rate_channels.empty()) {
                active_publication_claims =
                    std::make_unique<std::atomic<std::uint8_t>[]>(
                        impl_->cross_rate_channels.size());
            }
            std::size_t payload_offset = 0;
            for (std::size_t index = 0;
                 index < impl_->cross_rate_channels.size();
                 ++index) {
                const auto& channel = impl_->cross_rate_channels[index];
                std::copy(
                    channel.initial_sample.begin(),
                    channel.initial_sample.end(),
                    active_committed_payloads.begin() +
                        static_cast<std::ptrdiff_t>(payload_offset));
                std::copy(
                    channel.initial_sample.begin(),
                    channel.initial_sample.end(),
                    active_staging_payloads.begin() +
                        static_cast<std::ptrdiff_t>(payload_offset));
                active_channel_states.push_back({payload_offset});
                active_publication_claims[index].store(
                    0,
                    std::memory_order_relaxed);
                const auto publish_status =
                    compiled_cross_rate_plan.stores[index].publish(
                        1,
                        std::span<const std::byte>(
                            channel.initial_sample.data(),
                            channel.initial_sample.size()));
                if (publish_status !=
                    detail::SnapshotStoreResult::ok) {
                    return impl_->fail(
                        Status::internal_error,
                        "active initial channel publication failed");
                }
                payload_offset += channel.payload_size;
            }
            std::size_t channel_state_bytes = 0;
            std::size_t checkpoint_state_bytes = 0;
            if (!detail::checked_multiply(
                    impl_->cross_rate_channels.size(),
                    kRateDispatchChannelStateBytes,
                    channel_state_bytes) ||
                !detail::checked_add(
                    kRateDispatchStateHeaderBytes,
                    channel_state_bytes,
                    checkpoint_state_bytes) ||
                !detail::checked_add(
                    checkpoint_state_bytes,
                    payload_bytes,
                    checkpoint_state_bytes) ||
                (!compiled_rate_dispatch_plan.optional_shed_order.empty() &&
                 (!detail::checked_add(
                      checkpoint_state_bytes,
                      kRateOptionalStateHeaderBytes,
                      checkpoint_state_bytes) ||
                  !detail::checked_add(
                      checkpoint_state_bytes,
                      compiled_rate_dispatch_plan.optional_shed_order.size(),
                      checkpoint_state_bytes)))) {
                return impl_->fail(
                    Status::capacity_exceeded,
                    "active canonical state size overflows");
            }
            active_checkpoint_state.resize(checkpoint_state_bytes);
        } catch (const std::bad_alloc&) {
            return impl_->fail(Status::resource_exhausted, nullptr);
        } catch (...) {
            return impl_->fail(Status::internal_error, nullptr);
        }
    }

    const auto minimum_graph_queue_capacity =
        impl_->callbacks.empty()
        ? std::size_t{0}
        : impl_->config.executor_policy ==
                ExecutorPolicy::host_adapter
            ? impl_->callbacks.size()
            : 1 + ((impl_->callbacks.size() - 1) /
                   impl_->config.worker_count);
    if (impl_->config.executor_queue_capacity <
        minimum_graph_queue_capacity) {
        return impl_->fail(
            Status::invalid_config,
            "executor_queue_capacity is too small for the compiled graph");
    }
    if (impl_->config.task_scratch_slots <
        impl_->callbacks.size()) {
        return impl_->fail(
            Status::invalid_config,
            "task_scratch_slots is too small for the compiled graph");
    }

    std::size_t registered_state_bytes = 0;
    for (const auto& state : impl_->states) {
        if (impl_->rate_execution_policy_set &&
            identifier_view(state.name) == kRateDispatchStateName) {
            return impl_->fail(
                Status::invalid_config,
                "application state name collides with the active rate state record");
        }
        if (impl_->mixed_rate_closure_policy_set &&
            identifier_view(state.name) == kMixedRateStateName) {
            return impl_->fail(
                Status::invalid_config,
                "application state name collides with the mixed-rate state record");
        }
        if (impl_->live_control_closure_policy_set &&
            identifier_view(state.name) == kLiveControlStateName) {
            return impl_->fail(
                Status::invalid_config,
                "application state name collides with the live-control state record");
        }
        if (!detail::checked_artifact_add(
                registered_state_bytes,
                state.storage.size(),
                registered_state_bytes)) {
            return impl_->fail(
                Status::invalid_config,
                "registered state size overflows the snapshot format");
        }
    }
    std::size_t registered_device_buffer_bytes = 0;
    bool registered_device_buffer_bytes_exact = true;
    for (const auto& buffer : impl_->device_buffers) {
        std::size_t total = 0;
        if (!detail::checked_add(registered_device_buffer_bytes,
                                 static_cast<std::size_t>(buffer.bytes),
                                 total)) {
          return impl_->fail(
              Status::invalid_config,
              "registered device buffer bytes overflow accounting");
        }
        registered_device_buffer_bytes = total;
        registered_device_buffer_bytes_exact =
            registered_device_buffer_bytes_exact && !buffer.storage.empty();
    }
    std::size_t checkpoint_record_bytes = 0;
    std::size_t checkpoint_required_bytes = 0;
    const auto checkpoint_state_count = impl_->states.size() +
        (impl_->rate_execution_policy_set ? std::size_t{1} : 0) +
        (impl_->mixed_rate_closure_policy_set ? std::size_t{1} : 0) +
        (impl_->live_control_closure_policy_set ? std::size_t{1} : 0);
    std::size_t checkpoint_payload_bytes = registered_state_bytes;
    if ((impl_->rate_execution_policy_set &&
         !detail::checked_artifact_add(
             checkpoint_payload_bytes,
             active_checkpoint_state.size(),
             checkpoint_payload_bytes)) ||
        (impl_->mixed_rate_closure_policy_set &&
         !detail::checked_artifact_add(
             checkpoint_payload_bytes,
             mixed_rate_checkpoint_state.size(),
             checkpoint_payload_bytes)) ||
        (impl_->live_control_closure_policy_set &&
         !detail::checked_artifact_add(
             checkpoint_payload_bytes,
             live_control_mailboxes->checkpoint_state_size(),
             checkpoint_payload_bytes)) ||
        checkpoint_state_count > kMaxRegisteredStates) {
        return impl_->fail(
            Status::invalid_config,
            "active checkpoint state exceeds the schema-1 record bound");
    }
    if (!detail::checked_artifact_multiply(
            checkpoint_state_count,
            detail::checkpoint_record_header_size,
            checkpoint_record_bytes) ||
        !detail::checked_artifact_add(
            detail::checkpoint_header_size,
            checkpoint_record_bytes,
            checkpoint_required_bytes) ||
        !detail::checked_artifact_add(
            checkpoint_required_bytes,
            checkpoint_payload_bytes,
            checkpoint_required_bytes) ||
        checkpoint_required_bytes >
            impl_->config.snapshot_max_bytes) {
        return impl_->fail(
            Status::invalid_config,
            "registered state exceeds snapshot_max_bytes");
    }

    MemoryPlan memory_plan;
    memory_plan.memory_budget_bytes =
        impl_->config.memory_budget_bytes;
    memory_plan.phase_count = impl_->callbacks.size();
    memory_plan.phase_scratch_bytes =
        impl_->config.scratch_bytes;
    memory_plan.task_scratch_bytes =
        impl_->config.task_scratch_bytes;
    memory_plan.task_scratch_slots =
        impl_->config.task_scratch_slots;
    memory_plan.trace_capacity =
        impl_->config.trace_capacity;
    memory_plan.trace_slot_bytes =
        detail::TelemetryRing::slot_size();
    memory_plan.state_count = impl_->states.size();
    memory_plan.registered_state_bytes =
        registered_state_bytes;
    memory_plan.snapshot_max_bytes =
        impl_->config.snapshot_max_bytes;
    memory_plan.replay_input_capacity =
        impl_->config.replay_input_capacity;
    memory_plan.input_log_max_bytes =
        impl_->config.input_log_max_bytes;
    memory_plan.device_backend_count =
        impl_->device_backends.size();
    memory_plan.device_buffer_count =
        impl_->device_buffers.size();
    memory_plan.device_outstanding_capacity =
        impl_->device_backends.empty()
        ? 0
        : impl_->config.device_outstanding_capacity;
    memory_plan.device_completion_batch =
        impl_->device_backends.empty()
        ? 0
        : impl_->config.device_completion_batch;
    memory_plan.device_backend_reported_bytes =
        backend_reported_bytes;
    memory_plan.device_batch_backend_count =
        static_cast<std::size_t>(std::count_if(
            impl_->device_backends.begin(), impl_->device_backends.end(),
            [](const detail::DeviceBackendSpec& backend) {
                return backend.command_state != nullptr;
            }));
    memory_plan.device_timeline_count = impl_->device_timelines.size();
    bool plan_valid = detail::checked_multiply(
        memory_plan.device_batch_backend_count,
        impl_->config.device_outstanding_capacity,
        memory_plan.device_batch_queue_slots);
    memory_plan.scratch_alignment =
        impl_->config.scratch_alignment;
    memory_plan.overload_policy =
        impl_->config.overload_policy;
    memory_plan.rate_domain_count = compiled_rate_plan.domains.size();
    memory_plan.rate_binding_count = compiled_rate_plan.bindings.size();
    memory_plan.reference_release_count = compiled_rate_plan.releases.size();
    if (live_control_mailboxes) {
        memory_plan.live_control_mailbox_count =
            live_control_mailboxes->mailbox_count();
        memory_plan.live_control_producer_count =
            live_control_mailboxes->producer_count();
        memory_plan.live_control_record_capacity =
            live_control_mailboxes->record_capacity();
        memory_plan.live_control_payload_storage_bytes =
            live_control_mailboxes->payload_storage_bytes();
        memory_plan.live_control_action_capacity =
            live_control_mailboxes->action_capacity();
        memory_plan.live_control_action_storage_bytes =
            live_control_mailboxes->action_storage_bytes();
        memory_plan.live_control_retained_generation_capacity =
            live_control_mailboxes->retained_generation_capacity();
        memory_plan.live_control_retained_record_capacity =
            live_control_mailboxes->retained_record_capacity();
        memory_plan.live_control_retained_payload_storage_bytes =
            live_control_mailboxes->retained_payload_storage_bytes();
        memory_plan.live_control_closure_control_bytes =
            live_control_mailboxes->closure_control_bytes();
        memory_plan.live_control_checkpoint_state_bytes =
            live_control_mailboxes->checkpoint_state_size();
    }
    memory_plan.device_rate_phase_count =
        compiled_device_rate_plan.phases.size();
    memory_plan.device_rate_command_count =
        compiled_device_rate_plan.commands.size();
    memory_plan.device_rate_payload_reference_count =
        compiled_device_rate_plan.payload_references.size();
    memory_plan.device_rate_timeline_reference_count =
        compiled_device_rate_plan.timeline_references.size();
    memory_plan.device_rate_admission_backend_count =
        compiled_device_rate_plan.admission_backends.size();
    memory_plan.device_rate_admission_interval_count =
        compiled_device_rate_plan.admission_intervals.size();
    memory_plan.device_rate_dispatch_record_count =
        compiled_rate_dispatch_plan.device_phase_by_reference.size();
    memory_plan.device_rate_dependency_count =
        compiled_rate_dispatch_plan.device_dependencies.size();
    memory_plan.device_rate_execution_slot_count =
        active_device_tickets.size();
    memory_plan.cross_rate_channel_count =
        compiled_cross_rate_plan.channels.size();
    memory_plan.cross_rate_selection_count =
        compiled_cross_rate_plan.selections.size();
    memory_plan.cross_rate_device_endpoint_count =
        compiled_cross_rate_plan.device_endpoints.size();
    memory_plan.sampled_io_channel_count =
        compiled_sampled_io_plan.channels.size();
    for (const auto& channel : compiled_sampled_io_plan.channels) {
        const auto frame_bytes = channel.public_record.frame_bytes;
        plan_valid = plan_valid && detail::checked_add(
            memory_plan.sampled_io_frame_bytes,
            frame_bytes,
            memory_plan.sampled_io_frame_bytes);
        if (channel.public_record.direction == SampledIoDirection::output) {
            std::size_t safe_bytes = 0;
            plan_valid = plan_valid && detail::checked_multiply(
                frame_bytes, std::size_t{3}, safe_bytes) &&
                detail::checked_add(
                    memory_plan.sampled_io_safe_frame_bytes,
                    safe_bytes,
                    memory_plan.sampled_io_safe_frame_bytes);
        }
    }
    for (const auto& endpoint :
         compiled_cross_rate_plan.device_endpoints) {
        plan_valid = plan_valid && detail::checked_add(
            memory_plan.cross_rate_device_staging_bytes,
            impl_->cross_rate_channels[endpoint.channel_index].payload_size,
            memory_plan.cross_rate_device_staging_bytes);
    }
    plan_valid = plan_valid && detail::checked_add(
        active_checkpoint_state.size(),
        mixed_rate_checkpoint_state.size(),
        memory_plan.rate_checkpoint_state_bytes);
    memory_plan.optional_rate_domain_count =
        compiled_rate_dispatch_plan.optional_shed_order.size();
    memory_plan.rate_shedding_state_bytes = active_shedding_state
        ? sizeof(Impl::ActiveSheddingState)
        : 0;
    memory_plan.rate_telemetry_capacity =
        impl_->rate_execution_policy_set
        ? impl_->rate_execution_policy.rate_telemetry_capacity
        : 0;
    memory_plan.rate_telemetry_slot_bytes =
        impl_->rate_execution_policy_set
        ? detail::RateTelemetryRing::slot_size()
        : 0;
    memory_plan.rate_telemetry_storage_bytes = rate_telemetry
        ? rate_telemetry->slot_storage_bytes()
        : 0;
    memory_plan.rate_telemetry_counter_bytes = rate_counters
        ? sizeof(detail::RateCounters)
        : 0;
    memory_plan.mixed_rate_action_capacity = mixed_rate_actions
        ? mixed_rate_actions->capacity()
        : 0;
    memory_plan.mixed_rate_action_slot_bytes =
        impl_->mixed_rate_closure_policy_set
        ? detail::MixedRateActionRing::slot_size()
        : 0;
    memory_plan.mixed_rate_action_storage_bytes = mixed_rate_actions
        ? mixed_rate_actions->slot_storage_bytes()
        : 0;
    memory_plan.mixed_rate_replay_control_bytes = 0;
    if (impl_->mixed_rate_closure_policy_set) {
        constexpr auto fixed_replay_control_bytes =
            sizeof(MixedRateClosurePolicy) +
            sizeof(const detail::ActiveReplayArtifactView*) +
            sizeof(std::size_t) * 2 +
            sizeof(std::uint64_t) * 2 +
            sizeof(Status) +
            sizeof(bool);
        plan_valid = plan_valid && detail::checked_add(
            fixed_replay_control_bytes,
            mixed_rate_checkpoint_state.capacity(),
            memory_plan.mixed_rate_replay_control_bytes);
    }
    for (std::size_t index = 0;
         index < impl_->cross_rate_channels.size();
         ++index) {
        const auto& channel = impl_->cross_rate_channels[index];
        const auto& store = compiled_cross_rate_plan.stores[index];
        plan_valid = plan_valid && detail::checked_add(
            memory_plan.cross_rate_initial_sample_bytes,
            channel.initial_sample.size(),
            memory_plan.cross_rate_initial_sample_bytes);
        plan_valid = plan_valid && detail::checked_add(
            memory_plan.cross_rate_snapshot_slot_count,
            store.slot_count(),
            memory_plan.cross_rate_snapshot_slot_count);
        plan_valid = plan_valid && detail::checked_add(
            memory_plan.cross_rate_snapshot_bytes,
            store.payload_storage_bytes(),
            memory_plan.cross_rate_snapshot_bytes);
    }

    plan_valid = plan_valid && detail::checked_align_up(
        memory_plan.phase_scratch_bytes,
        memory_plan.scratch_alignment,
        memory_plan.phase_scratch_stride);
    plan_valid = plan_valid && detail::checked_multiply(
        memory_plan.phase_count,
        memory_plan.phase_scratch_stride,
        memory_plan.phase_scratch_total_bytes);
    plan_valid = plan_valid && detail::checked_align_up(
        memory_plan.task_scratch_bytes,
        memory_plan.scratch_alignment,
        memory_plan.task_scratch_stride);
    plan_valid = plan_valid && detail::checked_multiply(
        memory_plan.task_scratch_slots,
        memory_plan.task_scratch_stride,
        memory_plan.task_scratch_total_bytes);
    plan_valid = plan_valid && detail::checked_multiply(
        memory_plan.trace_capacity,
        memory_plan.trace_slot_bytes,
        memory_plan.trace_storage_bytes);
    if (impl_->config.executor_policy ==
        ExecutorPolicy::host_adapter) {
        memory_plan.queue_slots =
            impl_->config.executor_queue_capacity;
    } else {
        plan_valid = plan_valid && detail::checked_multiply(
            impl_->config.worker_count,
            impl_->config.executor_queue_capacity,
            memory_plan.queue_slots);
    }
    plan_valid = plan_valid &&
        detail::Executor::estimate_control_storage(
            impl_->config.executor_policy,
            impl_->config.worker_count,
            impl_->config.executor_queue_capacity,
            impl_->callbacks.size(),
            impl_->dependencies.size(),
            impl_->config.task_scratch_slots,
            memory_plan.executor_control_bytes);
    if (!impl_->device_backends.empty()) {
        std::size_t backend_name_bytes = 0;
        std::size_t adapted_v1_count = 0;
        for (const auto& backend : impl_->device_backends) {
            adapted_v1_count +=
                backend.kind ==
                    detail::HalBackendKind::adapted_device_abi_v1
                ? 1u
                : 0u;
            const auto object_begin =
                reinterpret_cast<std::uintptr_t>(&backend);
            const auto object_end = object_begin + sizeof(backend);
            const auto name_begin = reinterpret_cast<std::uintptr_t>(
                backend.name.data());
            if ((name_begin < object_begin || name_begin >= object_end) &&
                !detail::checked_add(
                    backend_name_bytes,
                    backend.name.capacity() + 1,
                    backend_name_bytes)) {
                plan_valid = false;
                break;
            }
        }
        plan_valid = plan_valid &&
            detail::DeviceManager::estimate_control_storage(
                impl_->device_backends.size(),
                backend_name_bytes,
                adapted_v1_count,
                impl_->device_buffers.size(),
                impl_->device_timelines.size(),
                memory_plan.device_batch_backend_count,
                impl_->config.device_outstanding_capacity,
                impl_->config.device_completion_batch,
                memory_plan.device_control_bytes);
    }

    memory_plan.runtime_control_bytes = sizeof(Impl);
    const auto add_runtime_bytes =
        [&](std::size_t bytes) {
            std::size_t total = 0;
            if (!detail::checked_add(
                    memory_plan.runtime_control_bytes,
                    bytes,
                    total)) {
                return false;
            }
            memory_plan.runtime_control_bytes = total;
            return true;
        };
    const auto add_runtime_array =
        [&](std::size_t count, std::size_t element_size) {
            std::size_t bytes = 0;
            return detail::checked_multiply(
                       count,
                       element_size,
                       bytes) &&
                   add_runtime_bytes(bytes);
        };

    const auto add_rate_plan_bytes =
        [&](std::size_t bytes) {
            std::size_t total = 0;
            if (!detail::checked_add(
                    memory_plan.rate_plan_bytes,
                    bytes,
                    total) ||
                !add_runtime_bytes(bytes)) {
                return false;
            }
            memory_plan.rate_plan_bytes = total;
            return true;
        };
    const auto add_rate_plan_array =
        [&](std::size_t count, std::size_t element_size) {
            std::size_t bytes = 0;
            return detail::checked_multiply(count, element_size, bytes) &&
                add_rate_plan_bytes(bytes);
        };

    plan_valid = plan_valid && add_runtime_array(
        impl_->callbacks.capacity(),
        sizeof(Impl::RegisteredCallback));
    plan_valid = plan_valid && add_runtime_array(
        impl_->extensions.capacity(),
        sizeof(std::unique_ptr<detail::ExtensionRegistrationRecord>));
    plan_valid = plan_valid && add_runtime_array(
        impl_->extensions.size(),
        sizeof(detail::ExtensionRegistrationRecord));
    std::size_t live_control_mailbox_declaration_bytes = 0;
    std::size_t live_control_producer_declaration_bytes = 0;
    plan_valid = plan_valid && detail::checked_multiply(
        impl_->live_control_mailbox_declarations.capacity(),
        sizeof(LiveControlMailboxRegistration),
        live_control_mailbox_declaration_bytes);
    plan_valid = plan_valid && detail::checked_multiply(
        impl_->live_control_producer_declarations.capacity(),
        sizeof(LiveControlProducerRegistration),
        live_control_producer_declaration_bytes);
    plan_valid = plan_valid && detail::checked_add(
        live_control_mailbox_declaration_bytes,
        live_control_producer_declaration_bytes,
        memory_plan.live_control_control_bytes);
    if (live_control_mailboxes) {
        plan_valid = plan_valid && detail::checked_add(
            memory_plan.live_control_control_bytes,
            live_control_mailboxes->control_bytes(),
            memory_plan.live_control_control_bytes);
    }
    plan_valid = plan_valid && add_runtime_array(
        impl_->live_control_mailbox_declarations.capacity(),
        sizeof(LiveControlMailboxRegistration));
    plan_valid = plan_valid && add_runtime_array(
        impl_->live_control_producer_declarations.capacity(),
        sizeof(LiveControlProducerRegistration));
    if (live_control_mailboxes) {
        plan_valid = plan_valid &&
            add_runtime_bytes(live_control_mailboxes->control_bytes()) &&
            add_runtime_bytes(
                live_control_mailboxes->closure_control_bytes());
    }
    for (const auto& callback : impl_->callbacks) {
        const auto object_begin = reinterpret_cast<std::uintptr_t>(&callback);
        const auto object_end = object_begin + sizeof(callback);
        const auto name_begin = reinterpret_cast<std::uintptr_t>(
            callback.name.data());
        if (name_begin < object_begin || name_begin >= object_end) {
            plan_valid = plan_valid &&
                add_runtime_bytes(callback.name.capacity() + 1);
        }
    }
    plan_valid = plan_valid && add_runtime_array(
        impl_->device_backends.capacity(),
        sizeof(detail::DeviceBackendSpec));
    plan_valid = plan_valid && add_runtime_array(
        impl_->device_v1_adapters.capacity(),
        sizeof(std::unique_ptr<detail::DeviceV1CompatibilityAdapter>));
    plan_valid = plan_valid &&
                 add_runtime_array(
                     impl_->device_memory_states.capacity(),
                     sizeof(std::unique_ptr<detail::HeterogeneousMemoryState>));
    plan_valid = plan_valid &&
                 add_runtime_array(
                     impl_->device_command_states.capacity(),
                     sizeof(std::unique_ptr<detail::CommandTimelineExtensionState>));
    plan_valid = plan_valid && add_runtime_array(
        impl_->device_timelines.capacity(), sizeof(detail::DeviceTimelineSpec));
    for (const auto& backend : impl_->device_backends) {
        const auto object_begin = reinterpret_cast<std::uintptr_t>(&backend);
        const auto object_end = object_begin + sizeof(backend);
        const auto name_begin = reinterpret_cast<std::uintptr_t>(
            backend.name.data());
        if (name_begin < object_begin || name_begin >= object_end) {
            plan_valid = plan_valid &&
                add_runtime_bytes(backend.name.capacity() + 1);
        }
    }
    plan_valid = plan_valid && add_runtime_array(
        impl_->device_buffers.capacity(),
        sizeof(detail::DeviceBufferSpec));
    plan_valid = plan_valid && add_runtime_array(
        impl_->resources.capacity(),
        sizeof(Impl::RegisteredResource));
    for (const auto& resource : impl_->resources) {
        const auto object_begin = reinterpret_cast<std::uintptr_t>(&resource);
        const auto object_end = object_begin + sizeof(resource);
        const auto name_begin = reinterpret_cast<std::uintptr_t>(
            resource.name.data());
        if (name_begin < object_begin || name_begin >= object_end) {
            plan_valid = plan_valid &&
                add_runtime_bytes(resource.name.capacity() + 1);
        }
    }
    plan_valid = plan_valid && add_runtime_array(
        impl_->states.capacity(),
        sizeof(Impl::RegisteredState));
    plan_valid = plan_valid && add_runtime_array(
        impl_->dependencies.capacity(),
        sizeof(detail::GraphDependency));
    plan_valid = plan_valid && add_runtime_array(
        impl_->resource_accesses.capacity(),
        sizeof(detail::GraphResourceAccess));
    plan_valid = plan_valid && add_rate_plan_array(
        impl_->rate_domains.capacity(),
        sizeof(detail::RateDomainSpec));
    for (const auto& domain : impl_->rate_domains) {
        const auto object_begin = reinterpret_cast<std::uintptr_t>(&domain);
        const auto object_end = object_begin + sizeof(domain);
        const auto name_begin = reinterpret_cast<std::uintptr_t>(
            domain.name.data());
        if (name_begin < object_begin || name_begin >= object_end) {
            const auto bytes = domain.name.capacity() + 1;
            plan_valid = plan_valid && add_rate_plan_bytes(bytes);
        }
    }
    plan_valid = plan_valid && add_rate_plan_array(
        impl_->rate_bindings.capacity(),
        sizeof(detail::RateBindingSpec));
    const auto device_rate_bytes_begin = memory_plan.rate_plan_bytes;
    plan_valid = plan_valid && add_rate_plan_array(
        impl_->device_rate_bindings.capacity(),
        sizeof(detail::DeviceRateBindingSpec));
    for (const auto& binding : impl_->device_rate_bindings) {
        plan_valid = plan_valid && add_rate_plan_array(
            binding.payload_roles.capacity(),
            sizeof(DeviceRatePayloadRole));
    }
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_device_rate_plan.phases.capacity(),
        sizeof(CompiledDeviceRatePhase));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_device_rate_plan.commands.capacity(),
        sizeof(CompiledDeviceRateCommand));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_device_rate_plan.payload_references.capacity(),
        sizeof(CompiledDeviceRatePayloadReference));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_device_rate_plan.timeline_references.capacity(),
        sizeof(CompiledDeviceRateTimelineReference));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_device_rate_plan.admission_backends.capacity(),
        sizeof(DeviceRateAdmissionBackend));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_device_rate_plan.admission_phases.capacity(),
        sizeof(DeviceRateAdmissionPhase));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_device_rate_plan.admission_intervals.capacity(),
        sizeof(DeviceRateAdmissionInterval));
    memory_plan.device_rate_plan_bytes =
        memory_plan.rate_plan_bytes - device_rate_bytes_begin;
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_rate_plan.domains.capacity(),
        sizeof(CompiledRateDomain));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_rate_plan.bindings.capacity(),
        sizeof(CompiledRateBinding));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_rate_plan.releases.capacity(),
        sizeof(ReferenceRelease));
    plan_valid = plan_valid && add_rate_plan_array(
        impl_->cross_rate_channels.capacity(),
        sizeof(detail::CrossRateChannelSpec));
    for (const auto& channel : impl_->cross_rate_channels) {
        const auto object_begin = reinterpret_cast<std::uintptr_t>(&channel);
        const auto object_end = object_begin + sizeof(channel);
        const auto name_begin = reinterpret_cast<std::uintptr_t>(
            channel.name.data());
        if (name_begin < object_begin || name_begin >= object_end) {
            plan_valid = plan_valid &&
                add_rate_plan_bytes(channel.name.capacity() + 1);
        }
        plan_valid = plan_valid && add_rate_plan_array(
            channel.initial_sample.capacity(),
            sizeof(std::byte));
    }
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_cross_rate_plan.channels.capacity(),
        sizeof(CompiledCrossRateChannel));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_cross_rate_plan.selections.capacity(),
        sizeof(CompiledCrossRateSelection));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_cross_rate_plan.stores.capacity(),
        sizeof(detail::SnapshotStore));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_cross_rate_plan.device_endpoints.capacity(),
        sizeof(detail::CompiledCrossRateDeviceEndpoint));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_cross_rate_plan.device_endpoint_index_by_channel.capacity(),
        sizeof(std::size_t));
    for (const auto& store : compiled_cross_rate_plan.stores) {
        plan_valid = plan_valid && add_rate_plan_array(
            store.slot_count(),
            sizeof(detail::SnapshotSlotControl));
        plan_valid = plan_valid &&
            add_rate_plan_bytes(store.payload_storage_bytes());
    }
    const auto sampled_io_bytes_begin = memory_plan.rate_plan_bytes;
    plan_valid = plan_valid && add_rate_plan_array(
        impl_->sampled_io_channels.capacity(),
        sizeof(detail::SampledIoChannelSpec));
    for (const auto& channel : impl_->sampled_io_channels) {
        plan_valid = plan_valid && add_rate_plan_array(
            channel.initial_frame.capacity(), sizeof(std::byte));
        plan_valid = plan_valid && add_rate_plan_array(
            channel.startup_safe_frame.capacity(), sizeof(std::byte));
        plan_valid = plan_valid && add_rate_plan_array(
            channel.failure_safe_frame.capacity(), sizeof(std::byte));
        plan_valid = plan_valid && add_rate_plan_array(
            channel.shutdown_safe_frame.capacity(), sizeof(std::byte));
    }
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_sampled_io_plan.channels.capacity(),
        sizeof(detail::CompiledSampledIoRecord));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_sampled_io_plan.channel_index_by_cross_rate.capacity(),
        sizeof(std::size_t));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_sampled_io_plan.safe_channel_indices.capacity(),
        sizeof(std::size_t));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_sampled_io_plan.safe_phases.capacity(),
        sizeof(detail::CompiledSampledIoSafePhase));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_sampled_io_plan.frame_storage.capacity(),
        sizeof(std::byte));
    plan_valid = plan_valid && add_rate_plan_array(
        sampled_io_statuses.capacity(), sizeof(SampledIoChannelStatus));
    memory_plan.sampled_io_control_bytes =
        memory_plan.rate_plan_bytes - sampled_io_bytes_begin;
    const auto rate_dispatch_bytes_begin = memory_plan.rate_plan_bytes;
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_rate_dispatch_plan.admission.capacity(),
        sizeof(detail::RateAdmissionRecord));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_rate_dispatch_plan.groups.capacity(),
        sizeof(detail::RateReleaseGroup));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_rate_dispatch_plan.domain_group_indices.capacity(),
        sizeof(std::size_t));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_rate_dispatch_plan.optional_shed_order.capacity(),
        sizeof(std::size_t));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_rate_dispatch_plan.producer_channel_offsets.capacity(),
        sizeof(std::size_t));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_rate_dispatch_plan.producer_channel_indices.capacity(),
        sizeof(std::size_t));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_rate_dispatch_plan.consumer_channel_offsets.capacity(),
        sizeof(std::size_t));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_rate_dispatch_plan.consumer_channel_indices.capacity(),
        sizeof(std::size_t));
    const auto device_execution_bytes_begin = memory_plan.rate_plan_bytes;
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_rate_dispatch_plan.device_phase_by_reference.capacity(),
        sizeof(std::size_t));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_rate_dispatch_plan.device_dependency_offsets.capacity(),
        sizeof(std::size_t));
    plan_valid = plan_valid && add_rate_plan_array(
        compiled_rate_dispatch_plan.device_dependencies.capacity(),
        sizeof(std::size_t));
    plan_valid = plan_valid && add_rate_plan_array(
        active_device_tickets.capacity(),
        sizeof(detail::DeviceRateTicket));
    memory_plan.device_rate_execution_bytes =
        memory_plan.rate_plan_bytes - device_execution_bytes_begin;
    plan_valid = plan_valid && add_rate_plan_array(
        active_channel_states.capacity(),
        sizeof(Impl::ActiveChannelState));
    plan_valid = plan_valid && add_rate_plan_array(
        active_committed_payloads.capacity(),
        sizeof(std::byte));
    plan_valid = plan_valid && add_rate_plan_array(
        active_staging_payloads.capacity(),
        sizeof(std::byte));
    plan_valid = plan_valid && add_rate_plan_array(
        impl_->rate_execution_policy_set
            ? impl_->cross_rate_channels.size()
            : 0,
        sizeof(std::atomic<std::uint8_t>));
    plan_valid = plan_valid && add_rate_plan_array(
        active_checkpoint_state.capacity(),
        sizeof(std::byte));
    plan_valid = plan_valid && add_rate_plan_array(
        mixed_rate_checkpoint_state.capacity(),
        sizeof(std::byte));
    if (active_shedding_state) {
        plan_valid = plan_valid &&
            add_rate_plan_bytes(sizeof(Impl::ActiveSheddingState));
    }
    if (rate_telemetry && rate_counters) {
        plan_valid = plan_valid &&
            add_rate_plan_bytes(sizeof(detail::RateTelemetryRing)) &&
            add_rate_plan_bytes(rate_telemetry->slot_storage_bytes()) &&
            add_rate_plan_bytes(sizeof(detail::RateCounters));
    }
    if (mixed_rate_actions) {
        plan_valid = plan_valid &&
            add_rate_plan_bytes(sizeof(detail::MixedRateActionRing)) &&
            add_rate_plan_bytes(mixed_rate_actions->slot_storage_bytes());
    }
    memory_plan.rate_dispatch_state_bytes =
        memory_plan.rate_plan_bytes - rate_dispatch_bytes_begin;
    plan_valid = plan_valid && add_runtime_array(
        compiled_order.capacity(),
        sizeof(PhaseHandle));
    plan_valid = plan_valid &&
        add_runtime_bytes(sizeof(detail::ResidentRegionSet)) &&
        add_runtime_bytes(sizeof(detail::TelemetryRing));

    memory_plan.planned_bytes =
        memory_plan.runtime_control_bytes;
    const auto add_planned_bytes =
        [&](std::size_t bytes) {
            std::size_t total = 0;
            if (!detail::checked_add(
                    memory_plan.planned_bytes,
                    bytes,
                    total)) {
                return false;
            }
            memory_plan.planned_bytes = total;
            return true;
        };
    plan_valid = plan_valid &&
        add_planned_bytes(memory_plan.executor_control_bytes) &&
        add_planned_bytes(memory_plan.device_control_bytes) &&
        add_planned_bytes(memory_plan.phase_scratch_total_bytes) &&
        add_planned_bytes(memory_plan.task_scratch_total_bytes) &&
        add_planned_bytes(memory_plan.trace_storage_bytes);
    if (!plan_valid) {
        return impl_->fail(
            Status::invalid_config,
            "finalized memory plan overflows addressable storage");
    }
    if (memory_plan.planned_bytes >
        memory_plan.memory_budget_bytes) {
        return impl_->fail(
            Status::invalid_config,
            "finalized memory plan exceeds memory_budget_bytes");
    }

    for (const auto& adapter : impl_->device_v1_adapters) {
        const auto adapter_status = adapter->prepare_completion_storage(
            impl_->config.device_completion_batch);
        if (adapter_status != Status::ok) {
            return impl_->fail(
                adapter_status,
                "device ABI v1 adapter storage allocation failed");
        }
    }

    CpuMemoryPolicyReport cpu_memory_policy_report;
    const char* policy_diagnostic = nullptr;
    const MemoryProvider* selected_memory_provider =
        impl_->memory_provider_set ? &impl_->memory_provider : nullptr;
    const auto provider_status =
        detail::ResidentRegionSet::validate_provider(
            selected_memory_provider,
            policy_diagnostic);
    if (provider_status != Status::ok) {
        return impl_->fail(provider_status, policy_diagnostic);
    }
    const auto policy_status = detail::build_cpu_memory_policy_report(
        impl_->cpu_memory_policy, impl_->config, memory_plan,
        registered_device_buffer_bytes, registered_device_buffer_bytes_exact,
        selected_memory_provider, *impl_->thread_policy,
        cpu_memory_policy_report, policy_diagnostic);
    if (policy_status != Status::ok) {
        return impl_->fail(policy_status, policy_diagnostic);
    }

    std::unique_ptr<detail::ResidentRegionSet> resident_regions;
    std::unique_ptr<detail::Executor> executor;
    std::unique_ptr<detail::DeviceManager> devices;
    std::unique_ptr<detail::TelemetryRing> telemetry;
    try {
        resident_regions =
            std::make_unique<detail::ResidentRegionSet>(
                selected_memory_provider,
                impl_->memory_provider_callback_active);
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    const auto acquisition_status = resident_regions->acquire(
        cpu_memory_policy_report,
        policy_diagnostic);
    if (acquisition_status != Status::ok) {
        return impl_->fail(acquisition_status, policy_diagnostic);
    }
    try {
        executor = std::make_unique<detail::Executor>(
            impl_->config.executor_policy,
            impl_->config.worker_count,
            impl_->config.executor_queue_capacity,
            impl_->callbacks.size(),
            impl_->config.task_scratch_bytes,
            impl_->config.task_scratch_slots,
            impl_->config.scratch_alignment,
            resident_regions->span(memory_region_task_scratch),
            impl_->config.overload_policy,
            impl_->dependencies,
            impl_->host_executor_set
                ? &impl_->host_executor
                : nullptr);
        if (!impl_->device_backends.empty()) {
            devices = std::make_unique<detail::DeviceManager>(
                impl_->graph_owner,
                impl_->device_backends,
                impl_->device_buffers,
                impl_->device_timelines,
                impl_->config.device_outstanding_capacity,
                impl_->config.device_completion_batch);
        }
        telemetry =
            std::make_unique<detail::TelemetryRing>(
                impl_->config.trace_capacity,
                resident_regions->span(memory_region_trace_storage));
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }

    std::vector<detail::LogicalControlExtent> control_extents;
    try {
        control_extents.reserve(
            32 + impl_->config.worker_count +
            impl_->callbacks.size() + impl_->device_backends.size() +
            impl_->resources.size() + impl_->extensions.size());
        std::uint64_t next_extent_id = 1;
        const auto add_runtime_extent =
            [&](const void* data, std::size_t count, std::size_t size) {
                if (count == 0) {
                    return;
                }
                control_extents.push_back({
                    next_extent_id++,
                    detail::ControlExtentOwner::runtime,
                    data,
                    count * size,
                });
            };
        const auto add_external_string =
            [&](const auto& owner, const std::string& value) {
                const auto object_begin =
                    reinterpret_cast<std::uintptr_t>(&owner);
                const auto object_end = object_begin + sizeof(owner);
                const auto data_begin = reinterpret_cast<std::uintptr_t>(
                    value.data());
                if (data_begin < object_begin || data_begin >= object_end) {
                    add_runtime_extent(value.data(), value.capacity() + 1, 1);
                }
            };
        add_runtime_extent(impl_.get(), 1, sizeof(Impl));
        add_runtime_extent(
            impl_->extensions.data(),
            impl_->extensions.capacity(),
            sizeof(std::unique_ptr<detail::ExtensionRegistrationRecord>));
        for (const auto& extension : impl_->extensions) {
            add_runtime_extent(
                extension.get(),
                extension ? 1 : 0,
                sizeof(detail::ExtensionRegistrationRecord));
        }
        add_runtime_extent(
            impl_->live_control_mailbox_declarations.data(),
            impl_->live_control_mailbox_declarations.capacity(),
            sizeof(LiveControlMailboxRegistration));
        add_runtime_extent(
            impl_->live_control_producer_declarations.data(),
            impl_->live_control_producer_declarations.capacity(),
            sizeof(LiveControlProducerRegistration));
        if (live_control_mailboxes) {
            for (std::size_t index = 0;
                 index < live_control_mailboxes->extent_count();
                 ++index) {
                detail::LiveControlStorageExtent extent;
                if (!live_control_mailboxes->extent_at(index, extent)) {
                    return impl_->fail(
                        Status::internal_error,
                        "live-control control extent enumeration failed");
                }
                add_runtime_extent(extent.data, extent.bytes, 1);
            }
        }
        add_runtime_extent(
            impl_->callbacks.data(),
            impl_->callbacks.capacity(),
            sizeof(Impl::RegisteredCallback));
        for (const auto& callback : impl_->callbacks) {
            add_external_string(callback, callback.name);
        }
        add_runtime_extent(
            impl_->device_backends.data(),
            impl_->device_backends.capacity(),
            sizeof(detail::DeviceBackendSpec));
        add_runtime_extent(
            impl_->device_v1_adapters.data(),
            impl_->device_v1_adapters.capacity(),
            sizeof(std::unique_ptr<detail::DeviceV1CompatibilityAdapter>));
        add_runtime_extent(
            impl_->device_memory_states.data(),
            impl_->device_memory_states.capacity(),
            sizeof(std::unique_ptr<detail::HeterogeneousMemoryState>));
        add_runtime_extent(
            impl_->device_command_states.data(),
            impl_->device_command_states.capacity(),
            sizeof(std::unique_ptr<detail::CommandTimelineExtensionState>));
        add_runtime_extent(
            impl_->device_timelines.data(),
            impl_->device_timelines.capacity(),
            sizeof(detail::DeviceTimelineSpec));
        for (const auto& backend : impl_->device_backends) {
            add_external_string(backend, backend.name);
        }
        add_runtime_extent(
            impl_->device_buffers.data(),
            impl_->device_buffers.capacity(),
            sizeof(detail::DeviceBufferSpec));
        add_runtime_extent(
            impl_->resources.data(),
            impl_->resources.capacity(),
            sizeof(Impl::RegisteredResource));
        for (const auto& resource : impl_->resources) {
            add_external_string(resource, resource.name);
        }
        add_runtime_extent(
            impl_->states.data(),
            impl_->states.capacity(),
            sizeof(Impl::RegisteredState));
        add_runtime_extent(
            impl_->dependencies.data(),
            impl_->dependencies.capacity(),
            sizeof(detail::GraphDependency));
        add_runtime_extent(
            impl_->resource_accesses.data(),
            impl_->resource_accesses.capacity(),
            sizeof(detail::GraphResourceAccess));
        add_runtime_extent(
            impl_->rate_domains.data(),
            impl_->rate_domains.capacity(),
            sizeof(detail::RateDomainSpec));
        for (const auto& domain : impl_->rate_domains) {
            add_external_string(domain, domain.name);
        }
        add_runtime_extent(
            impl_->rate_bindings.data(),
            impl_->rate_bindings.capacity(),
            sizeof(detail::RateBindingSpec));
        add_runtime_extent(
            impl_->device_rate_bindings.data(),
            impl_->device_rate_bindings.capacity(),
            sizeof(detail::DeviceRateBindingSpec));
        for (const auto& binding : impl_->device_rate_bindings) {
            add_runtime_extent(
                binding.payload_roles.data(),
                binding.payload_roles.capacity(),
                sizeof(DeviceRatePayloadRole));
        }
        add_runtime_extent(
            compiled_device_rate_plan.phases.data(),
            compiled_device_rate_plan.phases.capacity(),
            sizeof(CompiledDeviceRatePhase));
        add_runtime_extent(
            compiled_device_rate_plan.commands.data(),
            compiled_device_rate_plan.commands.capacity(),
            sizeof(CompiledDeviceRateCommand));
        add_runtime_extent(
            compiled_device_rate_plan.payload_references.data(),
            compiled_device_rate_plan.payload_references.capacity(),
            sizeof(CompiledDeviceRatePayloadReference));
        add_runtime_extent(
            compiled_device_rate_plan.timeline_references.data(),
            compiled_device_rate_plan.timeline_references.capacity(),
            sizeof(CompiledDeviceRateTimelineReference));
        add_runtime_extent(
            compiled_device_rate_plan.admission_backends.data(),
            compiled_device_rate_plan.admission_backends.capacity(),
            sizeof(DeviceRateAdmissionBackend));
        add_runtime_extent(
            compiled_device_rate_plan.admission_phases.data(),
            compiled_device_rate_plan.admission_phases.capacity(),
            sizeof(DeviceRateAdmissionPhase));
        add_runtime_extent(
            compiled_device_rate_plan.admission_intervals.data(),
            compiled_device_rate_plan.admission_intervals.capacity(),
            sizeof(DeviceRateAdmissionInterval));
        add_runtime_extent(
            compiled_rate_plan.domains.data(),
            compiled_rate_plan.domains.capacity(),
            sizeof(CompiledRateDomain));
        add_runtime_extent(
            compiled_rate_plan.bindings.data(),
            compiled_rate_plan.bindings.capacity(),
            sizeof(CompiledRateBinding));
        add_runtime_extent(
            compiled_rate_plan.releases.data(),
            compiled_rate_plan.releases.capacity(),
            sizeof(ReferenceRelease));
        add_runtime_extent(
            impl_->cross_rate_channels.data(),
            impl_->cross_rate_channels.capacity(),
            sizeof(detail::CrossRateChannelSpec));
        for (const auto& channel : impl_->cross_rate_channels) {
            add_external_string(channel, channel.name);
            add_runtime_extent(
                channel.initial_sample.data(),
                channel.initial_sample.capacity(),
                sizeof(std::byte));
        }
        add_runtime_extent(
            compiled_cross_rate_plan.channels.data(),
            compiled_cross_rate_plan.channels.capacity(),
            sizeof(CompiledCrossRateChannel));
        add_runtime_extent(
            compiled_cross_rate_plan.selections.data(),
            compiled_cross_rate_plan.selections.capacity(),
            sizeof(CompiledCrossRateSelection));
        add_runtime_extent(
            compiled_cross_rate_plan.stores.data(),
            compiled_cross_rate_plan.stores.capacity(),
            sizeof(detail::SnapshotStore));
        add_runtime_extent(
            compiled_cross_rate_plan.device_endpoints.data(),
            compiled_cross_rate_plan.device_endpoints.capacity(),
            sizeof(detail::CompiledCrossRateDeviceEndpoint));
        add_runtime_extent(
            compiled_cross_rate_plan.device_endpoint_index_by_channel.data(),
            compiled_cross_rate_plan.device_endpoint_index_by_channel.capacity(),
            sizeof(std::size_t));
        for (const auto& store : compiled_cross_rate_plan.stores) {
            add_runtime_extent(
                store.control_data(),
                store.control_storage_bytes(),
                1);
            add_runtime_extent(
                store.payload_data(),
                store.payload_storage_bytes(),
                1);
        }
        add_runtime_extent(
            impl_->sampled_io_channels.data(),
            impl_->sampled_io_channels.capacity(),
            sizeof(detail::SampledIoChannelSpec));
        for (const auto& channel : impl_->sampled_io_channels) {
            add_runtime_extent(
                channel.initial_frame.data(),
                channel.initial_frame.capacity(), sizeof(std::byte));
            add_runtime_extent(
                channel.startup_safe_frame.data(),
                channel.startup_safe_frame.capacity(), sizeof(std::byte));
            add_runtime_extent(
                channel.failure_safe_frame.data(),
                channel.failure_safe_frame.capacity(), sizeof(std::byte));
            add_runtime_extent(
                channel.shutdown_safe_frame.data(),
                channel.shutdown_safe_frame.capacity(), sizeof(std::byte));
        }
        add_runtime_extent(
            compiled_sampled_io_plan.channels.data(),
            compiled_sampled_io_plan.channels.capacity(),
            sizeof(detail::CompiledSampledIoRecord));
        add_runtime_extent(
            compiled_sampled_io_plan.channel_index_by_cross_rate.data(),
            compiled_sampled_io_plan.channel_index_by_cross_rate.capacity(),
            sizeof(std::size_t));
        add_runtime_extent(
            compiled_sampled_io_plan.safe_channel_indices.data(),
            compiled_sampled_io_plan.safe_channel_indices.capacity(),
            sizeof(std::size_t));
        add_runtime_extent(
            compiled_sampled_io_plan.safe_phases.data(),
            compiled_sampled_io_plan.safe_phases.capacity(),
            sizeof(detail::CompiledSampledIoSafePhase));
        add_runtime_extent(
            compiled_sampled_io_plan.frame_storage.data(),
            compiled_sampled_io_plan.frame_storage.capacity(),
            sizeof(std::byte));
        add_runtime_extent(
            sampled_io_statuses.data(), sampled_io_statuses.capacity(),
            sizeof(SampledIoChannelStatus));
        add_runtime_extent(
            compiled_rate_dispatch_plan.admission.data(),
            compiled_rate_dispatch_plan.admission.capacity(),
            sizeof(detail::RateAdmissionRecord));
        add_runtime_extent(
            compiled_rate_dispatch_plan.groups.data(),
            compiled_rate_dispatch_plan.groups.capacity(),
            sizeof(detail::RateReleaseGroup));
        add_runtime_extent(
            compiled_rate_dispatch_plan.domain_group_indices.data(),
            compiled_rate_dispatch_plan.domain_group_indices.capacity(),
            sizeof(std::size_t));
        add_runtime_extent(
            compiled_rate_dispatch_plan.optional_shed_order.data(),
            compiled_rate_dispatch_plan.optional_shed_order.capacity(),
            sizeof(std::size_t));
        add_runtime_extent(
            compiled_rate_dispatch_plan.producer_channel_offsets.data(),
            compiled_rate_dispatch_plan.producer_channel_offsets.capacity(),
            sizeof(std::size_t));
        add_runtime_extent(
            compiled_rate_dispatch_plan.producer_channel_indices.data(),
            compiled_rate_dispatch_plan.producer_channel_indices.capacity(),
            sizeof(std::size_t));
        add_runtime_extent(
            compiled_rate_dispatch_plan.consumer_channel_offsets.data(),
            compiled_rate_dispatch_plan.consumer_channel_offsets.capacity(),
            sizeof(std::size_t));
        add_runtime_extent(
            compiled_rate_dispatch_plan.consumer_channel_indices.data(),
            compiled_rate_dispatch_plan.consumer_channel_indices.capacity(),
            sizeof(std::size_t));
        add_runtime_extent(
            compiled_rate_dispatch_plan.device_phase_by_reference.data(),
            compiled_rate_dispatch_plan.device_phase_by_reference.capacity(),
            sizeof(std::size_t));
        add_runtime_extent(
            compiled_rate_dispatch_plan.device_dependency_offsets.data(),
            compiled_rate_dispatch_plan.device_dependency_offsets.capacity(),
            sizeof(std::size_t));
        add_runtime_extent(
            compiled_rate_dispatch_plan.device_dependencies.data(),
            compiled_rate_dispatch_plan.device_dependencies.capacity(),
            sizeof(std::size_t));
        add_runtime_extent(
            active_device_tickets.data(),
            active_device_tickets.capacity(),
            sizeof(detail::DeviceRateTicket));
        add_runtime_extent(
            active_channel_states.data(),
            active_channel_states.capacity(),
            sizeof(Impl::ActiveChannelState));
        add_runtime_extent(
            active_committed_payloads.data(),
            active_committed_payloads.capacity(),
            sizeof(std::byte));
        add_runtime_extent(
            active_staging_payloads.data(),
            active_staging_payloads.capacity(),
            sizeof(std::byte));
        add_runtime_extent(
            active_publication_claims.get(),
            impl_->rate_execution_policy_set
                ? impl_->cross_rate_channels.size()
                : 0,
            sizeof(std::atomic<std::uint8_t>));
        add_runtime_extent(
            active_checkpoint_state.data(),
            active_checkpoint_state.capacity(),
            sizeof(std::byte));
        add_runtime_extent(
            mixed_rate_checkpoint_state.data(),
            mixed_rate_checkpoint_state.capacity(),
            sizeof(std::byte));
        add_runtime_extent(
            active_shedding_state.get(),
            active_shedding_state ? 1 : 0,
            sizeof(Impl::ActiveSheddingState));
        add_runtime_extent(
            rate_telemetry.get(),
            rate_telemetry ? 1 : 0,
            sizeof(detail::RateTelemetryRing));
        if (rate_telemetry) {
            add_runtime_extent(
                rate_telemetry->slot_data(),
                rate_telemetry->slot_storage_bytes(),
                1);
        }
        add_runtime_extent(
            rate_counters.get(),
            rate_counters ? 1 : 0,
            sizeof(detail::RateCounters));
        add_runtime_extent(
            mixed_rate_actions.get(),
            mixed_rate_actions ? 1 : 0,
            sizeof(detail::MixedRateActionRing));
        if (mixed_rate_actions) {
            add_runtime_extent(
                mixed_rate_actions->slot_data(),
                mixed_rate_actions->slot_storage_bytes(),
                1);
        }
        add_runtime_extent(
            compiled_order.data(),
            compiled_order.capacity(),
            sizeof(PhaseHandle));
        add_runtime_extent(
            resident_regions.get(),
            1,
            sizeof(detail::ResidentRegionSet));
        add_runtime_extent(
            telemetry.get(),
            1,
            sizeof(detail::TelemetryRing));
        executor->append_control_extents(control_extents, next_extent_id);
        if (devices) {
            devices->append_control_extents(control_extents, next_extent_id);
        }
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    std::array<detail::ControlExtentExpectation, 3> extent_expectations{};
    for (const auto& extent : control_extents) {
        const auto owner = static_cast<std::size_t>(extent.owner);
        if (owner < extent_expectations.size()) {
            ++extent_expectations[owner].extent_count;
        }
    }
    extent_expectations[static_cast<std::size_t>(
        detail::ControlExtentOwner::runtime)].accounted_bytes =
        memory_plan.runtime_control_bytes;
    extent_expectations[static_cast<std::size_t>(
        detail::ControlExtentOwner::executor)].accounted_bytes =
        memory_plan.executor_control_bytes;
    extent_expectations[static_cast<std::size_t>(
        detail::ControlExtentOwner::device)].accounted_bytes =
        memory_plan.device_control_bytes;
    detail::ControlExtentLedger control_ledger;
    const char* extent_diagnostic = nullptr;
    const auto extent_status = detail::validate_control_extent_ledger(
        control_extents,
        extent_expectations,
        control_ledger,
        extent_diagnostic);
    if (extent_status != Status::ok) {
        return impl_->fail(extent_status, extent_diagnostic);
    }
    for (std::size_t index = 0;
         index < cpu_memory_policy_report.memory_count;
         ++index) {
        auto& row = cpu_memory_policy_report.memory[index];
        if (row.region == memory_region_runtime_control ||
            row.region == memory_region_executor_control ||
            row.region == memory_region_device_control) {
            row.accounting_exactness =
                ResourceAccountingExactness::exact;
        }
    }
    detail::refresh_accounting_totals(cpu_memory_policy_report);

    impl_->numerics = NumericalPolicy(impl_->config.numerical_mode);
    impl_->compiled_order = std::move(compiled_order);
    impl_->compiled_rate_plan = std::move(compiled_rate_plan);
    impl_->compiled_rate_dispatch_plan =
        std::move(compiled_rate_dispatch_plan);
    impl_->compiled_device_rate_plan =
        std::move(compiled_device_rate_plan);
    impl_->compiled_cross_rate_plan = std::move(compiled_cross_rate_plan);
    impl_->compiled_sampled_io_plan =
        std::move(compiled_sampled_io_plan);
    impl_->sampled_io_statuses = std::move(sampled_io_statuses);
    impl_->active_channel_states = std::move(active_channel_states);
    impl_->active_committed_payloads =
        std::move(active_committed_payloads);
    impl_->active_staging_payloads = std::move(active_staging_payloads);
    impl_->active_checkpoint_state = std::move(active_checkpoint_state);
    impl_->mixed_rate_checkpoint_state =
        std::move(mixed_rate_checkpoint_state);
    impl_->active_publication_claims =
        std::move(active_publication_claims);
    impl_->active_device_tickets =
        std::move(active_device_tickets);
    impl_->active_shedding_state = std::move(active_shedding_state);
    impl_->rate_telemetry = std::move(rate_telemetry);
    impl_->rate_counters = std::move(rate_counters);
    impl_->mixed_rate_actions = std::move(mixed_rate_actions);
    impl_->live_control_mailboxes = std::move(live_control_mailboxes);
    impl_->resident_regions = std::move(resident_regions);
    impl_->telemetry = std::move(telemetry);
    impl_->executor = std::move(executor);
    impl_->devices = std::move(devices);
    impl_->finalized_memory_plan = memory_plan;
    impl_->cpu_memory_policy_report = cpu_memory_policy_report;
    impl_->cpu_memory_policy_report_available = true;
    impl_->telemetry_counters.reset();
    impl_->metric_snapshot_sequence = 0;
    impl_->graph_id = impl_->compute_graph_id();
    impl_->state_schema_id =
        impl_->compute_state_schema_id();
    impl_->replay_id = impl_->compute_replay_id(
        impl_->graph_id,
        impl_->state_schema_id);
    impl_->observability = {};
    impl_->observability.config_id =
        impl_->compute_config_id();
    impl_->observability.runtime_id =
        static_cast<std::uint64_t>(impl_->graph_owner);
    impl_->observability.trace_capacity =
        static_cast<std::uint64_t>(
            impl_->config.trace_capacity);
    std::copy_n(
        kBuildId,
        sizeof(kBuildId),
        impl_->observability.build_id.begin());
    impl_->observability.workload_id =
        impl_->config.workload_id;
    impl_->telemetry_epoch_ns = impl_->clock_now();
    impl_->state = RuntimeState::finalized;
    impl_->clear_error();
    impl_->record(
        RuntimeTraceEventType::finalized,
        Status::ok,
        impl_->telemetry_epoch_ns,
        0,
        kNoCallback,
        RuntimeTraceProducer::host,
        kNoWorker,
        impl_->observability.config_id);
    return Status::ok;
}

Status Runtime::start() noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::finalized) {
        return impl_->fail(Status::invalid_state, "start requires finalized state");
    }
    if (impl_->lane_cleanup_pending) {
        return impl_->fail(
            Status::invalid_state,
            "device or runtime-lane cleanup is pending; retry stop");
    }
    if (impl_->memory_cleanup_pending) {
        if (!impl_->resident_regions ||
            !impl_->resident_regions->rollback(
                impl_->cpu_memory_policy_report)) {
            return impl_->fail(
                Status::internal_error,
                "resident-memory rollback is pending; retry start or stop");
        }
        impl_->memory_cleanup_pending = false;
        impl_->stop_pending = false;
    }
    if (!impl_->executor) {
        return impl_->fail(
            Status::internal_error,
            "finalized runtime has no executor");
    }

    impl_->preflight_report = {};
    impl_->preflight_report.mode =
        impl_->config.platform_preflight_mode;
    impl_->preflight_report_available = true;
    if (impl_->config.platform_preflight_mode ==
        PlatformPreflightMode::strict) {
        impl_->preflight->inspect(
            impl_->finalized_memory_plan.planned_bytes,
            *impl_->clock,
            impl_->preflight_report);
        impl_->preflight_report.mode =
            PlatformPreflightMode::strict;
        if (impl_->preflight_report.check_count >
            platform_check_capacity) {
            impl_->preflight_report.check_count =
                platform_check_capacity;
        }
        bool passed =
            impl_->preflight_report.check_count ==
            platform_check_capacity;
        std::array<bool, platform_check_capacity> seen{};
        for (std::size_t index = 0;
             index < impl_->preflight_report.check_count;
             ++index) {
            auto& check = impl_->preflight_report.checks[index];
            check.message.back() = '\0';
            const auto id = static_cast<std::size_t>(check.id);
            const bool id_valid = id < seen.size();
            passed = passed && id_valid &&
                check.status == PlatformCheckStatus::passed;
            if (id_valid) {
                passed = passed && !seen[id];
                seen[id] = true;
            }
        }
        for (const bool was_seen : seen) {
            passed = passed && was_seen;
        }
        impl_->preflight_report.passed = passed;
        if (!passed) {
            return impl_->fail_preflight();
        }
    } else {
        impl_->preflight_report.passed = true;
    }

    if (!impl_->resident_regions) {
        return impl_->fail(
            Status::internal_error,
            "finalized runtime has no resident-region transaction");
    }
    if (impl_->resident_regions->has_pending_operations() &&
        !impl_->resident_regions->rollback(
            impl_->cpu_memory_policy_report)) {
        return impl_->fail(
            Status::internal_error,
            "resident-memory rollback is pending; retry start or stop");
    }
    const char* memory_diagnostic = nullptr;
    const auto memory_status = impl_->resident_regions->apply_and_verify(
        impl_->cpu_memory_policy_report,
        memory_diagnostic);
    if (memory_status != Status::ok) {
        return impl_->fail(memory_status, memory_diagnostic);
    }
    const auto rollback_memory = [&] {
        return impl_->resident_regions->rollback(
            impl_->cpu_memory_policy_report);
    };

    const auto find_thread_report =
        [&](ThreadRoleId role) -> ThreadPolicyReport* {
            for (std::size_t index = 0;
                 index < impl_->cpu_memory_policy_report.thread_count;
                 ++index) {
                auto& row = impl_->cpu_memory_policy_report.threads[index];
                if (row.role == role) {
                    return &row;
                }
            }
            return nullptr;
        };
    const auto strict_failed = [](const ThreadPolicyReport& row) {
        if (row.requested.requirement != PolicyRequirement::strict) {
            return false;
        }
        if (row.application_mode == PolicyApplicationMode::verify_only) {
            return row.verified != PolicyOperationState::succeeded;
        }
        return row.applied != PolicyOperationState::succeeded ||
               row.verified != PolicyOperationState::succeeded;
    };
    for (std::size_t index = 0;
         index < impl_->cpu_memory_policy_report.thread_count;
         ++index) {
        detail::reset_thread_report_operations(
            impl_->cpu_memory_policy_report.threads[index]);
    }

    auto* frame_report = find_thread_report(thread_role_frame);
    if (!frame_report) {
        (void)rollback_memory();
        return impl_->fail(
            Status::internal_error,
            "finalized thread policy inventory has no frame role");
    }
    const auto frame_plan = detail::make_thread_role_plan(*frame_report);
    impl_->frame_startup_result.reset();
    impl_->thread_policy->verify_current(
        thread_role_frame,
        frame_plan,
        impl_->frame_startup_result);
    impl_->frame_startup_result.publish();
    detail::aggregate_thread_startup_results(
        *frame_report,
        &impl_->frame_startup_result,
        1);
    if (strict_failed(*frame_report)) {
        const bool memory_rollback_complete = rollback_memory();
        return impl_->fail(
            Status::internal_error,
            memory_rollback_complete
                ? "strict caller-frame policy did not match native readback"
                : "strict caller-frame policy failed and resident-memory rollback is pending");
    }

    for (std::size_t index = 0;
         index < impl_->cpu_memory_policy_report.thread_count;
         ++index) {
        auto& row = impl_->cpu_memory_policy_report.threads[index];
        if (row.role == thread_role_frame ||
            row.application_mode != PolicyApplicationMode::verify_only ||
            row.resolution_error == 0) {
            continue;
        }
        row.verified = PolicyOperationState::unsupported;
        row.verify_error = row.resolution_error;
    }

    auto* executor_report = find_thread_report(thread_role_executor_worker);
    auto* watchdog_report = find_thread_report(thread_role_watchdog);
    auto* device_report = find_thread_report(thread_role_device_service);
    auto* submission_report =
        find_thread_report(thread_role_device_submission);
    if (!executor_report || !watchdog_report || !device_report ||
        !submission_report) {
        (void)rollback_memory();
        return impl_->fail(
            Status::internal_error,
            "finalized thread policy inventory is incomplete");
    }
    MemoryPolicyReport* runtime_stack_report = nullptr;
    for (std::size_t index = 0;
         index < impl_->cpu_memory_policy_report.memory_count;
         ++index) {
        auto& row = impl_->cpu_memory_policy_report.memory[index];
        if (row.region == memory_region_runtime_thread_stack) {
            runtime_stack_report = &row;
            break;
        }
    }
    if (!runtime_stack_report) {
        (void)rollback_memory();
        return impl_->fail(
            Status::internal_error,
            "finalized memory policy inventory has no runtime stack row");
    }
    const auto executor_plan =
        detail::make_thread_role_plan(*executor_report, *runtime_stack_report);
    const auto watchdog_plan =
        detail::make_thread_role_plan(*watchdog_report, *runtime_stack_report);
    const auto device_plan =
        detail::make_thread_role_plan(*device_report, *runtime_stack_report);
    const auto submission_plan =
        detail::make_thread_role_plan(*submission_report,
                                      *runtime_stack_report);

    impl_->thread_startup_gate.reset();
    if (!impl_->rate_execution_policy_set ||
        (!impl_->active_epoch_mapped &&
         impl_->active_logical_cursor_ns == 0 &&
         !impl_->active_faulted)) {
        impl_->degradation_level.store(0, std::memory_order_release);
    }
    impl_->runtime_stack_results_available = false;
    const auto rollback_startup =
        [&](Status failure,
            const char* diagnostic,
            bool retry_device_cleanup = true) {
            const auto extension_stop = impl_->request_extension_stop();
            impl_->thread_startup_gate.abort();
            const bool watchdog_lane_present =
                impl_->runtime_stack_results_available &&
                impl_->config.watchdog_timeout_ns != 0;
            Status cleanup_status = Status::ok;
            if (impl_->devices) {
                if (retry_device_cleanup) {
                    cleanup_status = impl_->devices->stop();
                } else {
                    cleanup_status = impl_->devices->stop_lane();
                }
            }
            const auto executor_cleanup = impl_->executor->stop();
            if (cleanup_status == Status::ok &&
                executor_cleanup != Status::ok) {
                cleanup_status = executor_cleanup;
            }
            const auto watchdog_cleanup = impl_->watchdog.stop();
            if (cleanup_status == Status::ok &&
                watchdog_cleanup != Status::ok) {
                cleanup_status = watchdog_cleanup;
            }
            if (watchdog_cleanup == Status::ok) {
                impl_->watchdog_started = false;
            }
            const auto extension_cleanup =
                impl_->cleanup_extension_services();
            if (cleanup_status == Status::ok &&
                extension_stop != Status::ok) {
                cleanup_status = extension_stop;
            }
            if (cleanup_status == Status::ok &&
                extension_cleanup != Status::ok) {
                cleanup_status = extension_cleanup;
            }
            if (impl_->runtime_stack_results_available) {
                (void)detail::aggregate_runtime_stack_startup_results(
                    *runtime_stack_report,
                    impl_->config.executor_policy ==
                            ExecutorPolicy::host_adapter
                        ? nullptr
                        : impl_->executor->startup_results(),
                    impl_->config.executor_policy ==
                            ExecutorPolicy::host_adapter
                        ? 0
                        : impl_->config.worker_count,
                    watchdog_lane_present
                        ? &impl_->watchdog.startup_result()
                        : nullptr,
                    watchdog_lane_present ? 1 : 0,
                    impl_->devices
                        ? &impl_->devices->startup_result()
                        : nullptr,
                    impl_->devices ? 1 : 0,
                    impl_->devices
                        ? impl_->devices->submission_startup_results()
                        : nullptr,
                    impl_->devices
                        ? impl_->devices->submission_startup_result_count()
                        : 0);
                detail::refresh_accounting_totals(
                    impl_->cpu_memory_policy_report);
            }
            const bool lanes_clean = cleanup_status == Status::ok &&
                (!impl_->devices || !impl_->devices->cleanup_pending());
            const bool memory_rollback_complete =
                lanes_clean ? rollback_memory() : false;
            impl_->lane_cleanup_pending = !lanes_clean;
            impl_->memory_cleanup_pending =
                lanes_clean && !memory_rollback_complete;
            impl_->stop_pending = impl_->lane_cleanup_pending ||
                impl_->memory_cleanup_pending;
            return impl_->fail(
                cleanup_status == Status::ok ? failure : cleanup_status,
                impl_->lane_cleanup_pending
                    ? "startup failed and device/thread/stack cleanup is pending; retry stop"
                    : impl_->memory_cleanup_pending
                        ? "startup failed and resident-memory rollback is pending; retry start or stop"
                        : diagnostic);
        };

    const auto extension_initialize_status =
        impl_->initialize_extension_services();
    if (extension_initialize_status != Status::ok) {
        return rollback_startup(
            extension_initialize_status,
            "extension service initialization failed");
    }

    if (impl_->config.watchdog_timeout_ns != 0) {
        const auto status = impl_->watchdog.start(
            *impl_->thread_policy,
            impl_->thread_startup_gate,
            watchdog_plan);
        detail::aggregate_thread_startup_results(
            *watchdog_report,
            &impl_->watchdog.startup_result(),
            1);
        if (status != Status::ok) {
            return rollback_startup(
                status,
                "failed to create watchdog service lane");
        }
        impl_->watchdog_started = true;
        if (strict_failed(*watchdog_report)) {
            return rollback_startup(
                Status::internal_error,
                "strict watchdog thread policy failed apply or readback");
        }
    }

    const auto executor_status = impl_->executor->start(
        *impl_->thread_policy,
        impl_->thread_startup_gate,
        executor_plan);
    if (impl_->config.executor_policy != ExecutorPolicy::host_adapter) {
        detail::aggregate_thread_startup_results(
            *executor_report,
            impl_->executor->startup_results(),
            impl_->config.worker_count);
    }
    if (executor_status != Status::ok) {
        return rollback_startup(
            executor_status,
            "failed to create executor worker lane");
    }
    if (strict_failed(*executor_report)) {
        return rollback_startup(
            Status::internal_error,
            "strict executor thread policy failed apply or readback");
    }

    if (impl_->devices) {
        const auto device_lane_status = impl_->devices->start_lane(
            *impl_->executor,
            &Impl::observe_device_event,
            impl_.get(),
            *impl_->thread_policy,
            impl_->thread_startup_gate,
            device_plan);
        detail::aggregate_thread_startup_results(
            *device_report,
            &impl_->devices->startup_result(),
            1);
        if (device_lane_status != Status::ok) {
            return rollback_startup(
                device_lane_status,
                "failed to create device service lane");
        }
        if (strict_failed(*device_report)) {
            return rollback_startup(
                Status::internal_error,
                "strict device-service thread policy failed apply or readback");
        }
        const auto submission_status =
            impl_->devices->start_submission_lanes(
                *impl_->thread_policy, impl_->thread_startup_gate,
                submission_plan);
        detail::aggregate_thread_startup_results(
            *submission_report,
            impl_->devices->submission_startup_results(),
            impl_->devices->submission_startup_result_count());
        if (submission_status != Status::ok) {
            return rollback_startup(
                submission_status,
                "failed to create device submission lanes");
        }
        if (strict_failed(*submission_report)) {
            return rollback_startup(
                Status::internal_error,
                "strict device-submission thread policy failed apply or "
                "readback");
        }
    }

    const auto stack_status = detail::aggregate_runtime_stack_startup_results(
        *runtime_stack_report,
        impl_->config.executor_policy == ExecutorPolicy::host_adapter
            ? nullptr
            : impl_->executor->startup_results(),
        impl_->config.executor_policy == ExecutorPolicy::host_adapter
            ? 0
            : impl_->config.worker_count,
        impl_->watchdog_started
            ? &impl_->watchdog.startup_result()
            : nullptr,
        impl_->watchdog_started ? 1 : 0,
        impl_->devices
            ? &impl_->devices->startup_result()
            : nullptr,
        impl_->devices ? 1 : 0,
        impl_->devices
            ? impl_->devices->submission_startup_results()
            : nullptr,
        impl_->devices
            ? impl_->devices->submission_startup_result_count()
            : 0);
    impl_->runtime_stack_results_available = true;
    detail::refresh_accounting_totals(
        impl_->cpu_memory_policy_report);
    if (stack_status != Status::ok ||
        (runtime_stack_report->requested.requirement ==
             PolicyRequirement::strict &&
         (runtime_stack_report->applied !=
              PolicyOperationState::succeeded ||
          runtime_stack_report->verified !=
              PolicyOperationState::succeeded))) {
        return rollback_startup(
            stack_status == Status::ok
                ? Status::internal_error
                : stack_status,
            "strict runtime-stack policy failed live apply or observation");
    }
    if (impl_->cpu_memory_policy_report.accounting_requirement ==
        PolicyRequirement::strict) {
        const char* closure_diagnostic = nullptr;
        const auto closure_status = detail::validate_accounting_closure(
            impl_->cpu_memory_policy_report,
            true,
            closure_diagnostic);
        if (closure_status != Status::ok) {
            return rollback_startup(
                closure_status,
                closure_diagnostic
                    ? closure_diagnostic
                    : "strict accounting closure is incomplete");
        }
    }

    if (impl_->devices) {
        const auto initialize_status = impl_->devices->initialize();
        if (initialize_status != Status::ok) {
            return rollback_startup(
                initialize_status,
                "failed to initialize device backends after thread-policy verification",
                false);
        }
    }

    impl_->thread_startup_gate.commit();
    if (impl_->watchdog_started) {
        impl_->watchdog.wait_started();
    }
    impl_->executor->wait_started();
    if (impl_->devices) {
        impl_->devices->wait_started();
    }
    const auto safe_start_status = impl_->apply_sampled_io_safe_transition(
        SampledIoSafetyState::startup_acknowledged);
    if (safe_start_status != Status::ok) {
        return rollback_startup(
            safe_start_status,
            "sampled-I/O startup safe output was not terminally acknowledged");
    }
    impl_->stop_pending = false;
    impl_->lane_cleanup_pending = false;
    impl_->memory_cleanup_pending = false;
    impl_->state = RuntimeState::running;
    for (auto& extension : impl_->extensions) {
        if (extension && !extension->unload_ready.load(
                std::memory_order_acquire)) {
            extension->state.store(
                ExtensionLifecycleState::running,
                std::memory_order_release);
        }
    }
    impl_->clear_error();
    impl_->record(
        RuntimeTraceEventType::started,
        Status::ok,
        impl_->clock_now(),
        0);
    return Status::ok;
}

Status Runtime::step(
    const HostFrameContext& frame,
    StepResult* result) noexcept {
    StepResult local_result{};
    StepResult& output = result ? *result : local_result;
    output = {};

    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::running) {
        return impl_->fail(Status::invalid_state, "step requires non-reentrant running state");
    }
    if (impl_->stop_pending) {
        return impl_->fail(
            Status::invalid_state,
            "device teardown is pending; retry stop");
    }
    if (impl_->rate_execution_policy_set && impl_->active_faulted) {
        return impl_->fail(
            Status::invalid_state,
            "active policy failure is terminal for execution");
    }
    if (impl_->in_periodic_run.load(std::memory_order_acquire) &&
        !impl_->periodic_dispatch.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "step cannot be entered from a periodic observer");
    }
    if (impl_->in_replay.load(std::memory_order_acquire) &&
        !impl_->replay_dispatch.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "step cannot be entered from a replay input callback");
    }
    bool expected = false;
    if (!impl_->in_step.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return impl_->fail(
            Status::invalid_state,
            "step requires non-reentrant running state");
    }
    if (frame.delta.count() < 0) {
        impl_->in_step.store(false, std::memory_order_release);
        return impl_->fail(Status::invalid_argument, "frame delta cannot be negative");
    }
    if (impl_->rate_execution_policy_set) {
        std::uint64_t checked_end_ns = 0;
        std::uint64_t expected_nominal_ns = 0;
        if (impl_->active_faulted) {
            impl_->in_step.store(false, std::memory_order_release);
            return impl_->fail(
                Status::invalid_state,
                "active rate execution is fault-gated until checked stop");
        }
        if (frame.delta.count() <= 0 || !frame.nominal_release_ns ||
            !checked_time_add(
                impl_->active_logical_cursor_ns,
                static_cast<std::uint64_t>(frame.delta.count()),
                checked_end_ns) ||
            (impl_->active_epoch_mapped &&
             (!checked_time_add(
                  impl_->active_nominal_epoch_ns,
                  impl_->active_logical_cursor_ns,
                  expected_nominal_ns) ||
              *frame.nominal_release_ns != expected_nominal_ns))) {
            impl_->in_step.store(false, std::memory_order_release);
            return impl_->fail(
                Status::invalid_argument,
                "active rate step requires a positive bounded contiguous nominal window");
        }
    }

    struct StepGuard {
        std::atomic<bool>& flag;
        explicit StepGuard(std::atomic<bool>& value) : flag(value) {}
        ~StepGuard() { flag.store(false, std::memory_order_release); }
    } guard(impl_->in_step);

    if (impl_->live_control_mailboxes &&
        !impl_->live_control_mailboxes->begin_step_transaction()) {
        return impl_->fail(
            Status::invalid_state,
            "live-control step transaction is already active");
    }
    struct LiveControlTransactionGuard {
        detail::LiveControlMailboxSet* mailboxes = nullptr;
        Status status = Status::internal_error;
        ~LiveControlTransactionGuard() {
            if (mailboxes) {
                mailboxes->settle_step_transaction(status);
            }
        }
    } live_control_transaction{impl_->live_control_mailboxes.get()};

    impl_->clear_error();
    output.start_ns = impl_->clock_now();
    std::uint64_t watchdog_token = 0;
    if (impl_->config.watchdog_timeout_ns != 0) {
        std::uint64_t watchdog_deadline = 0;
        if (!checked_time_add(
                output.start_ns,
                impl_->config.watchdog_timeout_ns,
                watchdog_deadline)) {
            return impl_->fail(
                Status::clock_failure,
                "watchdog deadline overflows the runtime clock domain");
        }
        watchdog_token = impl_->watchdog.arm(
            watchdog_deadline,
            impl_->config.watchdog_timeout_ns);
        if (watchdog_token == 0) {
            return impl_->fail(
                Status::internal_error,
                "watchdog service lane is unavailable");
        }
    }
    impl_->record(
        RuntimeTraceEventType::step_begin,
        Status::ok,
        output.start_ns,
        frame.frame_index);

    if (impl_->live_control_mailboxes) {
        (void)impl_->live_control_mailboxes->close_host_frame(
            frame.frame_index);
    }
    impl_->active_frame = &frame;
    std::size_t failed_phase = impl_->callbacks.size();
    auto execution_status = impl_->rate_execution_policy_set
        ? impl_->run_active_step(frame, output, failed_phase)
        : impl_->executor->run(
              &Impl::run_phase,
              impl_.get(),
              output.callbacks_executed,
              failed_phase);
    impl_->active_frame = nullptr;
    if (impl_->rate_execution_policy_set &&
        !impl_->commit_rate_counters(output.rate) &&
        execution_status == Status::ok) {
        execution_status = Status::capacity_exceeded;
    }

    output.finish_ns = impl_->clock_now();
    if (watchdog_token != 0) {
        const auto live_watchdog_fired = impl_->watchdog.complete(
            watchdog_token, output.finish_ns);
        if (impl_->active_replay_view) {
            MixedRateActionRecord expected_action;
            if (detail::active_replay_action_at(
                    *impl_->active_replay_view,
                    impl_->active_replay_expected_action,
                    expected_action) &&
                expected_action.action == MixedRateActionId::watchdog_transition &&
                expected_action.frame_index == frame.frame_index &&
                expected_action.degradation_before == impl_->degradation_level.load(
                    std::memory_order_acquire) &&
                expected_action.degradation_after <=
                    impl_->config.watchdog_max_degradation_level) {
                output.watchdog_fired = true;
                impl_->degradation_level.store(
                    expected_action.degradation_after,
                    std::memory_order_release);
                impl_->emit_mixed_rate_action(expected_action);
            }
        } else {
            output.watchdog_fired = live_watchdog_fired;
        }
        if (output.watchdog_fired) {
            impl_->record(
                RuntimeTraceEventType::watchdog_fired,
                Status::ok,
                output.finish_ns,
                frame.frame_index,
                kNoCallback,
                RuntimeTraceProducer::host,
                kNoWorker,
                impl_->config.watchdog_timeout_ns);
            const auto current =
                impl_->degradation_level.load(
                    std::memory_order_relaxed);
            if (!impl_->active_replay_view && current <
                impl_->config.watchdog_max_degradation_level) {
                const auto next = current + 1;
                impl_->degradation_level.store(
                    next,
                    std::memory_order_release);
                impl_->record(
                    RuntimeTraceEventType::degradation_applied,
                    Status::ok,
                    output.finish_ns,
                    frame.frame_index,
                    kNoCallback,
                    RuntimeTraceProducer::host,
                    kNoWorker,
                    next);
            }
            if (impl_->mixed_rate_actions && !impl_->active_replay_view) {
                const auto after = impl_->degradation_level.load(
                    std::memory_order_acquire);
                impl_->emit_mixed_rate_action(MixedRateActionRecord{
                    mixed_rate_action_schema_version,
                    sizeof(MixedRateActionRecord),
                    0,
                    impl_->observability.runtime_id,
                    impl_->mixed_rate_closure_policy.host_policy_version,
                    frame.frame_index,
                    impl_->active_logical_cursor_ns,
                    frame.nominal_release_ns.value_or(0),
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    output.finish_ns,
                    0,
                    0,
                    0,
                    0,
                    impl_->active_shedding_state
                        ? impl_->active_shedding_state->shed_mask
                        : 0,
                    impl_->active_shedding_state
                        ? impl_->active_shedding_state->shed_mask
                        : 0,
                    impl_->config.watchdog_timeout_ns,
                    0,
                    std::numeric_limits<std::uint32_t>::max(),
                    0,
                    static_cast<std::int32_t>(Status::ok),
                    after == 0 ? 0 : after - 1,
                    after,
                    MixedRateActionId::watchdog_transition,
                    MixedRateActionReason::watchdog,
                    MixedRateActionStage::terminal,
                    false,
                    false,
                    false,
                    MixedRateSampleFreshness::not_applicable,
                    false,
                    false,
                    MixedRateSafetyState::not_applicable,
                    {},
                });
            }
        }
    }
    output.degradation_level =
        impl_->degradation_level.load(std::memory_order_acquire);
    output.deadline_missed =
        frame.deadline_ns && output.finish_ns > *frame.deadline_ns;
    if (output.deadline_missed) {
        impl_->telemetry_counters.increment(
            RuntimeMetricId::deadline_misses);
    }
    impl_->record(
        RuntimeTraceEventType::step_end,
        execution_status,
        output.finish_ns,
        frame.frame_index);

    live_control_transaction.status = execution_status;

    if (execution_status == Status::callback_failed &&
        failed_phase < impl_->callbacks.size()) {
        return impl_->fail_callback(failed_phase);
    }
    if (execution_status != Status::ok) {
        return impl_->fail(execution_status, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::run_periodic(
    const PeriodicRunConfig& config,
    PeriodicFrameObserver observer,
    void* observer_data,
    PeriodicRunResult* result) noexcept {
    PeriodicRunResult local_result{};
    PeriodicRunResult& output = result ? *result : local_result;
    output = {};

    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::running) {
        return impl_->fail(
            Status::invalid_state,
            "periodic execution requires running state");
    }
    if (impl_->stop_pending) {
        return impl_->fail(
            Status::invalid_state,
            "device teardown is pending; retry stop");
    }
    output.final_degradation_level =
        impl_->degradation_level.load(std::memory_order_acquire);
    if (impl_->in_step.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "periodic execution cannot be entered from a frame callback");
    }
    if (impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "periodic execution cannot run during replay");
    }
    if (config.frame_count == 0 ||
        config.period.count() <= 0 ||
        config.relative_deadline.count() <= 0) {
        return impl_->fail(
            Status::invalid_argument,
            "periodic frame_count, period, and relative deadline must be positive");
    }

    bool expected = false;
    if (!impl_->in_periodic_run.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return impl_->fail(
            Status::invalid_state,
            "periodic execution is already active");
    }
    struct PeriodicGuard {
        std::atomic<bool>& flag;
        explicit PeriodicGuard(std::atomic<bool>& value) : flag(value) {}
        ~PeriodicGuard() {
            flag.store(false, std::memory_order_release);
        }
    } guard(impl_->in_periodic_run);

    const auto period_ns =
        static_cast<std::uint64_t>(config.period.count());
    const auto relative_deadline_ns =
        static_cast<std::uint64_t>(
            config.relative_deadline.count());
    const auto first_release =
        config.first_release_ns.value_or(impl_->clock_now());
    output.first_release_ns = first_release;
    output.next_release_ns = first_release;

    const auto frames_minus_one =
        static_cast<std::uint64_t>(config.frame_count - 1);
    std::uint64_t last_offset = 0;
    std::uint64_t last_release = 0;
    std::uint64_t last_deadline = 0;
    std::uint64_t requested_next_release = 0;
    std::uint64_t last_frame_index = 0;
    if (!checked_time_multiply(
            frames_minus_one,
            period_ns,
            last_offset) ||
        !checked_time_add(
            first_release,
            last_offset,
            last_release) ||
        !checked_time_add(
            last_release,
            relative_deadline_ns,
            last_deadline) ||
        !checked_time_add(
            last_release,
            period_ns,
            requested_next_release) ||
        !checked_time_add(
            config.first_frame_index,
            frames_minus_one,
            last_frame_index)) {
        return impl_->fail(
            Status::invalid_argument,
            "periodic release, deadline, or frame index overflows");
    }
    (void)last_deadline;
    (void)last_frame_index;

    impl_->clear_error();
    std::uint64_t release = first_release;
    for (std::size_t frame_offset = 0;
         frame_offset < config.frame_count;
         ++frame_offset) {
        const auto frame_index =
            config.first_frame_index +
            static_cast<std::uint64_t>(frame_offset);
        impl_->record(
            RuntimeTraceEventType::periodic_release,
            Status::ok,
            release,
            frame_index,
            kNoCallback,
            RuntimeTraceProducer::host,
            kNoWorker,
            release);

        if (impl_->clock_sleep_until(release) != Status::ok) {
            output.final_degradation_level =
                impl_->degradation_level.load(
                    std::memory_order_acquire);
            return impl_->fail(
                Status::clock_failure,
                "runtime clock rejected an absolute periodic wait");
        }
        const auto wake = impl_->clock_now();
        impl_->record(
            RuntimeTraceEventType::periodic_wake,
            Status::ok,
            wake,
            frame_index,
            kNoCallback,
            RuntimeTraceProducer::host,
            kNoWorker,
            release);

        std::uint64_t deadline = 0;
        if (!checked_time_add(
                release,
                relative_deadline_ns,
                deadline)) {
            return impl_->fail(
                Status::invalid_argument,
                "periodic deadline overflows");
        }

        StepResult step_result;
        Status step_status = Status::internal_error;
        {
            struct PeriodicDispatchGuard {
                std::atomic<bool>& active;
                explicit PeriodicDispatchGuard(
                    std::atomic<bool>& value)
                    : active(value) {
                    active.store(true, std::memory_order_release);
                }
                ~PeriodicDispatchGuard() {
                    active.store(false, std::memory_order_release);
                }
            } dispatch_guard(impl_->periodic_dispatch);
            step_status = step(
                HostFrameContext{
                    frame_index,
                    config.period,
                    deadline,
                    release,
                },
                &step_result);
        }

        PeriodicFrameResult frame_result;
        frame_result.status = step_status;
        frame_result.frame_index = frame_index;
        frame_result.release_ns = release;
        frame_result.wake_ns = wake;
        frame_result.start_ns = step_result.start_ns;
        frame_result.finish_ns = step_result.finish_ns;
        frame_result.slack_ns =
            deadline_slack(deadline, step_result.finish_ns);
        frame_result.deadline_missed =
            step_result.deadline_missed;
        frame_result.watchdog_fired =
            step_result.watchdog_fired;
        frame_result.degradation_level =
            step_result.degradation_level;
        frame_result.rate = step_result.rate;

        const auto add_rate = [](std::uint64_t& total,
                                 std::uint64_t value) noexcept {
            return checked_time_add(total, value, total);
        };
        if (!add_rate(
                output.rate.due_domain_releases,
                step_result.rate.due_domain_releases) ||
            !add_rate(
                output.rate.due_reference_records,
                step_result.rate.due_reference_records) ||
            !add_rate(
                output.rate.executed_reference_records,
                step_result.rate.executed_reference_records) ||
            !add_rate(
                output.rate.on_time_domain_releases,
                step_result.rate.on_time_domain_releases) ||
            !add_rate(
                output.rate.late_domain_releases,
                step_result.rate.late_domain_releases) ||
            !add_rate(
                output.rate.caught_up_domain_releases,
                step_result.rate.caught_up_domain_releases) ||
            !add_rate(
                output.rate.skipped_domain_releases,
                step_result.rate.skipped_domain_releases) ||
            !add_rate(
                output.rate.held_domain_releases,
                step_result.rate.held_domain_releases) ||
            !add_rate(
                output.rate.degraded_domain_releases,
                step_result.rate.degraded_domain_releases) ||
            !add_rate(
                output.rate.rejected_reference_records,
                step_result.rate.rejected_reference_records) ||
            !add_rate(
                output.rate.stale_reads,
                step_result.rate.stale_reads) ||
            !add_rate(
                output.rate.failed_domain_releases,
                step_result.rate.failed_domain_releases) ||
            !add_rate(
                output.rate.optional_due_domain_releases,
                step_result.rate.optional_due_domain_releases) ||
            !add_rate(
                output.rate.optional_executed_domain_releases,
                step_result.rate.optional_executed_domain_releases) ||
            !add_rate(
                output.rate.shed_domain_releases,
                step_result.rate.shed_domain_releases) ||
            !add_rate(
                output.rate.shed_transitions,
                step_result.rate.shed_transitions) ||
            !add_rate(
                output.rate.recovery_transitions,
                step_result.rate.recovery_transitions)) {
            return impl_->fail(
                Status::capacity_exceeded,
                "periodic active-rate summary overflowed");
        }
        output.rate.currently_shed_domains =
            step_result.rate.currently_shed_domains;
        output.rate.rate_policy_version =
            step_result.rate.rate_policy_version;
        if (!output.rate.has_first_failure &&
            step_result.rate.has_first_failure) {
            output.rate.has_first_failure = true;
            output.rate.first_failing_domain =
                step_result.rate.first_failing_domain;
            output.rate.first_failing_sequence =
                step_result.rate.first_failing_sequence;
            output.rate.first_failing_substep =
                step_result.rate.first_failing_substep;
        }

        ++output.frames_executed;
        if (frame_result.deadline_missed) {
            ++output.deadline_misses;
        }
        if (frame_result.watchdog_fired) {
            ++output.watchdog_events;
        }
        output.final_degradation_level =
            frame_result.degradation_level;
        output.last_frame = frame_result;
        output.next_release_ns =
            frame_offset + 1 == config.frame_count
            ? requested_next_release
            : release + period_ns;

        CallbackResult observer_result = CallbackResult::ok;
        if (observer) {
            try {
                observer_result = observer(
                    observer_data,
                    frame_result);
            } catch (...) {
                observer_result = CallbackResult::error;
            }
        }
        if (observer_result != CallbackResult::ok) {
            return impl_->fail(
                Status::callback_failed,
                "periodic frame observer failed");
        }
        if (step_status != Status::ok) {
            return step_status;
        }
        release += period_ns;
    }

    impl_->clear_error();
    return Status::ok;
}

Status Runtime::stop() noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state == RuntimeState::stopped) {
        impl_->clear_error();
        return Status::ok;
    }
    if (impl_->state != RuntimeState::running &&
        impl_->state != RuntimeState::finalized) {
        return impl_->fail(Status::invalid_state, "stop requires finalized or running state");
    }
    if (impl_->live_control_mailboxes) {
        impl_->live_control_mailboxes->close_admission();
        if (impl_->live_control_mailboxes->has_active_reservation()) {
            return impl_->fail(
                Status::invalid_state,
                "live-control admission is quiescing; retry checked stop");
        }
    }

    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        const auto extension_stop_status = impl_->request_extension_stop();
        (void)extension_stop_status;
        if (g_active_runtime_callback != impl_.get() && impl_->devices &&
            impl_->devices->batch_backend_count() != 0) {
            (void)impl_->devices->request_batch_stop();
            return impl_->fail(
                Status::invalid_state,
                "extension and batch stop requested; retry checked stop "
                "after active execution quiesces");
        }
        return impl_->fail(
            Status::invalid_state,
            "extension stop requested; retry checked stop after active "
            "execution quiesces");
    }
    if (impl_->live_control_mailboxes) {
        impl_->live_control_mailboxes->terminalize_staged_on_stop();
    }

    Status sampled_io_safe_status = Status::ok;
    if (impl_->state == RuntimeState::running) {
        sampled_io_safe_status = impl_->apply_sampled_io_safe_transition(
            impl_->active_faulted
                ? SampledIoSafetyState::failure_acknowledged
                : SampledIoSafetyState::shutdown_acknowledged);
    }
    if (sampled_io_safe_status != Status::ok) {
        impl_->stop_pending = true;
        impl_->emit_runtime_stop_action(sampled_io_safe_status);
        return impl_->fail(
            sampled_io_safe_status,
            "sampled-I/O safe output was not terminally acknowledged; retry checked stop while retaining backend ownership");
    }
    const auto extension_stop_status = impl_->request_extension_stop();
    const bool watchdog_lane_present =
        impl_->runtime_stack_results_available &&
        impl_->config.watchdog_timeout_ns != 0;
    Status device_cleanup_status = Status::ok;
    if (impl_->devices) {
        device_cleanup_status = impl_->devices->stop();
    }
    Status cleanup_status = extension_stop_status;
    if (cleanup_status == Status::ok &&
        device_cleanup_status != Status::ok) {
        cleanup_status = device_cleanup_status;
    }
    if (impl_->executor) {
        const auto status = impl_->executor->stop();
        if (cleanup_status == Status::ok && status != Status::ok) {
            cleanup_status = status;
        }
    }
    if (impl_->watchdog_started) {
        const auto status = impl_->watchdog.stop();
        if (cleanup_status == Status::ok && status != Status::ok) {
            cleanup_status = status;
        }
        if (status == Status::ok) {
            impl_->watchdog_started = false;
        }
    }
    if (impl_->runtime_stack_results_available) {
        MemoryPolicyReport* runtime_stack_report = nullptr;
        for (std::size_t index = 0;
             index < impl_->cpu_memory_policy_report.memory_count;
             ++index) {
            auto& row = impl_->cpu_memory_policy_report.memory[index];
            if (row.region == memory_region_runtime_thread_stack) {
                runtime_stack_report = &row;
                break;
            }
        }
        const auto aggregation_status = runtime_stack_report
            ? detail::aggregate_runtime_stack_startup_results(
                  *runtime_stack_report,
                  impl_->config.executor_policy == ExecutorPolicy::host_adapter
                      ? nullptr
                      : impl_->executor->startup_results(),
                  impl_->config.executor_policy == ExecutorPolicy::host_adapter
                      ? 0
                      : impl_->config.worker_count,
                  watchdog_lane_present
                      ? &impl_->watchdog.startup_result()
                      : nullptr,
                  watchdog_lane_present ? 1 : 0,
                  impl_->devices
                      ? &impl_->devices->startup_result()
                      : nullptr,
                  impl_->devices ? 1 : 0,
                  impl_->devices
                      ? impl_->devices->submission_startup_results()
                      : nullptr,
                  impl_->devices
                      ? impl_->devices->submission_startup_result_count()
                      : 0)
            : Status::internal_error;
        detail::refresh_accounting_totals(
            impl_->cpu_memory_policy_report);
        if (cleanup_status == Status::ok &&
            aggregation_status != Status::ok) {
            cleanup_status = aggregation_status;
        }
    }
    const auto extension_cleanup_status =
        impl_->cleanup_extension_services();
    if (cleanup_status == Status::ok &&
        extension_cleanup_status != Status::ok) {
        cleanup_status = extension_cleanup_status;
    }
    const bool lanes_clean = cleanup_status == Status::ok &&
        (!impl_->devices || !impl_->devices->cleanup_pending()) &&
        extension_cleanup_status == Status::ok;
    if (!lanes_clean) {
        impl_->lane_cleanup_pending = true;
        impl_->memory_cleanup_pending = false;
        impl_->stop_pending = true;
        impl_->emit_runtime_stop_action(
            cleanup_status == Status::ok
                ? Status::internal_error
                : cleanup_status);
        return impl_->fail(
            cleanup_status == Status::ok
                ? Status::internal_error
                : cleanup_status,
            device_cleanup_status != Status::ok
                ? "device teardown failed; device/thread/stack cleanup failed; retry stop before releasing borrowed resources"
                : extension_cleanup_status != Status::ok ||
                      extension_stop_status != Status::ok
                    ? "extension cleanup failed; retry stop before releasing borrowed resources"
                    : "thread/stack cleanup failed; retry stop before releasing borrowed resources");
    }
    impl_->lane_cleanup_pending = false;
    if (impl_->resident_regions) {
        if (!impl_->resident_regions->rollback(
                impl_->cpu_memory_policy_report)) {
            impl_->memory_cleanup_pending = true;
            impl_->stop_pending = true;
            impl_->emit_runtime_stop_action(Status::internal_error);
            return impl_->fail(
                Status::internal_error,
                "resident-memory rollback failed; retry stop before releasing borrowed resources");
        }
        if (impl_->resident_regions->provider_backed()) {
            impl_->telemetry.reset();
            impl_->executor.reset();
            impl_->resident_regions->release();
        }
    }
    impl_->memory_cleanup_pending = false;
    impl_->stop_pending = false;
    impl_->record(
        RuntimeTraceEventType::stopped,
        Status::ok,
        impl_->clock_now(),
        0);
    impl_->emit_runtime_stop_action(Status::ok);
    impl_->state = RuntimeState::stopped;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::detach_extension(
    ExtensionHandle extension,
    bool& unload_ready) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::stopped) {
        return impl_->fail(
            Status::invalid_state,
            "extension detach requires successful checked stop");
    }
    if (!impl_->valid_extension(extension)) {
        return impl_->fail(
            Status::invalid_handle,
            "extension detach received a stale, foreign, or wrong-kind handle");
    }
    auto& record = *impl_->extensions[extension.slot];
    if (record.state.load(std::memory_order_acquire) !=
            ExtensionLifecycleState::quiescent ||
        !record.callbacks_quiescent() || !record.backends_released() ||
        !record.services_released()) {
        return impl_->fail(
            Status::invalid_state,
            "extension ownership remains unresolved; retry checked stop");
    }
    record.clear_borrowed();
    record.generation.fetch_add(1, std::memory_order_acq_rel);
    record.state.store(
        ExtensionLifecycleState::detached,
        std::memory_order_release);
    record.unload_ready.store(true, std::memory_order_release);
    unload_ready = true;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::extension_service_status(
    ExtensionHandle extension,
    std::size_t service_index,
    rtfw_extension_service_status_v1& status) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (!impl_->valid_extension(extension)) {
        return impl_->fail(
            Status::invalid_handle,
            "service status received a stale, foreign, or wrong-kind handle");
    }
    auto& record = *impl_->extensions[extension.slot];
    if (service_index >= record.service_count) {
        return impl_->fail(
            Status::invalid_argument,
            "service status index is outside the extension service table");
    }
    impl_->extension_control_callback_active.store(
        true, std::memory_order_release);
    const auto result =
        record.services[service_index].call_status(status);
    impl_->extension_control_callback_active.store(
        false, std::memory_order_release);
    if (result != Status::ok) {
        return impl_->fail(result, "extension service status failed");
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::device_health(
    DeviceBackendHandle backend,
    DeviceHealth& health) noexcept {
    health = make_device_health();
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::running ||
        !impl_->devices) {
        return impl_->fail(
            Status::invalid_state,
            "device health requires a running device service");
    }
    if (impl_->stop_pending) {
        return impl_->fail(
            Status::invalid_state,
            "device teardown is pending; retry stop");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "device health cannot run during a frame or replay");
    }
    if (!impl_->valid_device_backend(backend)) {
        return impl_->fail(
            Status::invalid_handle,
            "device health received an invalid or foreign backend");
    }
    const auto status = impl_->devices->health(
        backend.index(),
        health);
    if (status != Status::ok) {
        return impl_->fail(status, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::reset_device(
    DeviceBackendHandle backend) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::running ||
        !impl_->devices) {
        return impl_->fail(
            Status::invalid_state,
            "device reset requires a running device service");
    }
    if (impl_->stop_pending) {
        return impl_->fail(
            Status::invalid_state,
            "device teardown is pending; retry stop");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "device reset cannot run during a frame or replay");
    }
    if (!impl_->valid_device_backend(backend)) {
        return impl_->fail(
            Status::invalid_handle,
            "device reset received an invalid or foreign backend");
    }
    const auto status = impl_->devices->reset(backend.index());
    if (status != Status::ok) {
        return impl_->fail(status, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

RuntimeState Runtime::state() const noexcept {
    return impl_ && !impl_->provider_callback_active()
        ? impl_->state
        : RuntimeState::configuring;
}

const RuntimeConfig& Runtime::config() const noexcept {
    static const RuntimeConfig empty{};
    return impl_ && !impl_->provider_callback_active() ? impl_->config : empty;
}

std::size_t Runtime::callback_count() const noexcept {
    return impl_ && !impl_->provider_callback_active()
        ? impl_->callbacks.size()
        : 0;
}

std::size_t Runtime::extension_count() const noexcept {
    return impl_ && !impl_->provider_callback_active()
        ? impl_->extensions.size()
        : 0;
}

bool Runtime::extension_at(
    std::size_t index,
    ExtensionInfo& info) const noexcept {
    if (!impl_ || impl_->provider_callback_active() ||
        index >= impl_->extensions.size() || !impl_->extensions[index]) {
        return false;
    }
    const auto& record = *impl_->extensions[index];
    ExtensionInfo candidate;
    candidate.name = record.name;
    candidate.version = record.version;
    candidate.negotiated_abi_version = record.negotiated_abi_version;
    candidate.state = record.state.load(std::memory_order_acquire);
    candidate.generation =
        record.generation.load(std::memory_order_acquire);
    candidate.phase_count = static_cast<std::uint32_t>(record.phase_count);
    candidate.backend_count = static_cast<std::uint32_t>(record.backend_count);
    candidate.service_count = static_cast<std::uint32_t>(record.service_count);
    candidate.resource_count = static_cast<std::uint32_t>(record.resource_count);
    candidate.relationship_count =
        static_cast<std::uint32_t>(record.relationship_count);
    candidate.unload_ready =
        record.unload_ready.load(std::memory_order_acquire);
    info = candidate;
    return true;
}

Status Runtime::extension_info(
    ExtensionHandle extension,
    ExtensionInfo& info) const noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (!impl_->valid_extension(extension)) {
        return Status::invalid_handle;
    }
    ExtensionInfo candidate;
    if (!extension_at(extension.slot, candidate)) {
        return Status::internal_error;
    }
    info = candidate;
    return Status::ok;
}

std::size_t Runtime::device_backend_count() const noexcept {
    return impl_ && !impl_->provider_callback_active()
        ? impl_->device_backends.size()
        : 0;
}

std::size_t Runtime::device_buffer_count() const noexcept {
    return impl_ && !impl_->provider_callback_active()
        ? impl_->device_buffers.size()
        : 0;
}

std::size_t Runtime::device_phase_count() const noexcept {
    if (!impl_ || impl_->provider_callback_active()) {
        return 0;
    }
    return static_cast<std::size_t>(std::count_if(
        impl_->callbacks.begin(),
        impl_->callbacks.end(),
        [](const Impl::RegisteredCallback& callback) {
            return callback.kind == Impl::PhaseKind::device;
        }));
}

std::size_t Runtime::device_timeline_count(
    DeviceBackendHandle backend) const noexcept {
    if (!impl_ || impl_->provider_callback_active() ||
        !impl_->valid_device_backend(backend)) {
        return 0;
    }
    if (impl_->devices) {
        return impl_->devices->timeline_count(backend.index());
    }
    return static_cast<std::size_t>(std::count_if(
        impl_->device_timelines.begin(), impl_->device_timelines.end(),
        [&](const detail::DeviceTimelineSpec& timeline) {
            return timeline.backend_index == backend.index();
        }));
}

bool Runtime::device_timeline_at(
    DeviceBackendHandle backend,
    std::size_t index,
    DeviceTimelineInfo& info) const noexcept {
    info = {};
    if (!impl_ || impl_->provider_callback_active() ||
        !impl_->valid_device_backend(backend)) {
        return false;
    }
    if (impl_->devices) {
        return impl_->devices->timeline_at(backend.index(), index, info);
    }
    std::size_t ordinal = 0;
    for (std::size_t timeline_index = 0;
         timeline_index < impl_->device_timelines.size(); ++timeline_index) {
        const auto& timeline = impl_->device_timelines[timeline_index];
        if (timeline.backend_index != backend.index()) {
            continue;
        }
        if (ordinal++ != index) {
            continue;
        }
        DeviceTimelineInfo candidate;
        candidate.timeline = DeviceTimelineHandle{
            impl_->graph_owner, static_cast<std::uint32_t>(timeline_index)};
        candidate.backend = backend;
        candidate.name = timeline.name;
        candidate.initial_value = timeline.initial_value;
        candidate.last_accepted_value = timeline.initial_value;
        candidate.completed_value = timeline.initial_value;
        info = candidate;
        return true;
    }
    return false;
}

std::size_t Runtime::device_memory_domain_count(
    DeviceBackendHandle backend) const noexcept {
  if (!impl_ || impl_->provider_callback_active() ||
      !impl_->valid_device_backend(backend)) {
    return 0;
  }
  const auto *state = impl_->device_backends[backend.index()].memory_state;
  return state ? state->snapshot.memory_domain_count : 0;
}

bool Runtime::device_memory_domain_at(
    DeviceBackendHandle backend, std::size_t index,
    DeviceMemoryDomainHandle &handle,
    HalV2MemoryDomain &domain) const noexcept {
  handle = {};
  domain = {};
  if (!impl_ || impl_->provider_callback_active() ||
      !impl_->valid_device_backend(backend)) {
    return false;
  }
  const auto *state = impl_->device_backends[backend.index()].memory_state;
  if (!state || index >= state->snapshot.memory_domain_count) {
    return false;
  }
  domain = state->snapshot.memory_domains[index];
  handle = {backend, domain.identity};
  return true;
}

std::size_t Runtime::device_topology_node_count(
    DeviceBackendHandle backend) const noexcept {
  if (!impl_ || impl_->provider_callback_active() ||
      !impl_->valid_device_backend(backend)) {
    return 0;
  }
  const auto *state = impl_->device_backends[backend.index()].memory_state;
  return state ? state->snapshot.topology_node_count : 0;
}

bool Runtime::device_topology_node_at(DeviceBackendHandle backend,
                                      std::size_t index,
                                      DeviceTopologyNodeHandle &handle,
                                      HalV2TopologyNode &node) const noexcept {
  handle = {};
  node = {};
  if (!impl_ || impl_->provider_callback_active() ||
      !impl_->valid_device_backend(backend)) {
    return false;
  }
  const auto *state = impl_->device_backends[backend.index()].memory_state;
  if (!state || index >= state->snapshot.topology_node_count) {
    return false;
  }
  node = state->snapshot.topology_nodes[index];
  handle = {backend, node.identity};
  return true;
}

std::size_t Runtime::device_topology_link_count(
    DeviceBackendHandle backend) const noexcept {
  if (!impl_ || impl_->provider_callback_active() ||
      !impl_->valid_device_backend(backend)) {
    return 0;
  }
  const auto *state = impl_->device_backends[backend.index()].memory_state;
  return state ? state->snapshot.topology_link_count : 0;
}

bool Runtime::device_topology_link_at(DeviceBackendHandle backend,
                                      std::size_t index,
                                      HalV2TopologyLink &link) const noexcept {
  DeviceTopologyLinkHandle handle;
  return device_topology_link_at(backend, index, handle, link);
}

bool Runtime::device_topology_link_at(DeviceBackendHandle backend,
                                      std::size_t index,
                                      DeviceTopologyLinkHandle &handle,
                                      HalV2TopologyLink &link) const noexcept {
  handle = {};
  link = {};
  if (!impl_ || impl_->provider_callback_active() ||
      !impl_->valid_device_backend(backend)) {
    return false;
  }
  const auto *state = impl_->device_backends[backend.index()].memory_state;
  if (!state || index >= state->snapshot.topology_link_count) {
    return false;
  }
  link = state->snapshot.topology_links[index];
  handle = {backend, link.identity};
  return true;
}

std::size_t Runtime::device_timestamp_domain_count(
    DeviceBackendHandle backend) const noexcept {
  if (!impl_ || impl_->provider_callback_active() ||
      !impl_->valid_device_backend(backend)) {
    return 0;
  }
  const auto *state = impl_->device_backends[backend.index()].memory_state;
  return state ? state->snapshot.timestamp_domain_count : 0;
}

bool Runtime::device_timestamp_domain_at(
    DeviceBackendHandle backend, std::size_t index,
    DeviceTimestampDomainHandle &handle,
    HalV2TimestampDomain &domain) const noexcept {
  handle = {};
  domain = {};
  if (!impl_ || impl_->provider_callback_active() ||
      !impl_->valid_device_backend(backend)) {
    return false;
  }
  const auto *state = impl_->device_backends[backend.index()].memory_state;
  if (!state || index >= state->snapshot.timestamp_domain_count) {
    return false;
  }
  domain = state->snapshot.timestamp_domains[index];
  handle = {backend, domain.identity};
  return true;
}

bool Runtime::device_completion_timestamp_domain(
    DeviceBackendHandle backend,
    DeviceTimestampDomainHandle &domain) const noexcept {
  domain = {};
  if (!impl_ || impl_->provider_callback_active() ||
      !impl_->valid_device_backend(backend)) {
    return false;
  }
  const auto *state = impl_->device_backends[backend.index()].memory_state;
  if (!state || state->snapshot.completion_timestamp_domain_identity == 0) {
    return false;
  }
  domain = {
      backend,
      state->snapshot.completion_timestamp_domain_identity,
  };
  return true;
}

bool Runtime::device_memory_object_at(
    std::size_t index, DeviceMemoryObjectInfo &object) const noexcept {
  object = {};
  if (!impl_ || impl_->provider_callback_active() ||
      index >= impl_->device_buffers.size()) {
    return false;
  }
  const auto &buffer = impl_->device_buffers[index];
  const DeviceBackendHandle backend{impl_->graph_owner, buffer.backend_index};
  object.buffer =
      DeviceBufferHandle{impl_->graph_owner, static_cast<std::uint32_t>(index)};
  object.backend = backend;
  object.domain = {backend, buffer.domain_identity};
  object.bytes = buffer.bytes;
  object.ownership = buffer.ownership;
  object.access = buffer.flags;
  object.coherency = buffer.coherency;
  object.synchronization = buffer.synchronization;
  object.heterogeneous = buffer.heterogeneous ? 1 : 0;
  object.host_addressable = buffer.storage.empty() ? 0 : 1;
  object.opaque_handle_size = buffer.opaque_handle.size;
  return true;
}

Status Runtime::query_device_timestamp_correlation(
    DeviceBackendHandle backend, DeviceTimestampDomainHandle source,
    DeviceTimestampDomainHandle destination,
    HalV2TimestampCorrelation &correlation) noexcept {
  correlation = {};
  if (!impl_) {
    return Status::internal_error;
  }
  if (impl_->provider_callback_active()) {
    return Status::invalid_state;
  }
  if (impl_->state != RuntimeState::running || !impl_->devices ||
      impl_->stop_pending || impl_->in_step.load(std::memory_order_acquire) ||
      impl_->in_periodic_run.load(std::memory_order_acquire) ||
      impl_->in_replay.load(std::memory_order_acquire)) {
    return impl_->fail(
        Status::invalid_state,
        "timestamp correlation requires idle running host control");
  }
  if (!impl_->valid_device_backend(backend) || !source.valid() ||
      !destination.valid() || source.backend != backend ||
      destination.backend != backend) {
    return impl_->fail(Status::invalid_handle,
                       "timestamp correlation received a foreign domain");
  }
  auto *state = impl_->device_backends[backend.index()].memory_state;
  const auto *source_descriptor =
      state ? detail::find_timestamp_domain(*state, source.identity) : nullptr;
  const auto *destination_descriptor =
      state ? detail::find_timestamp_domain(*state, destination.identity)
            : nullptr;
  if (!state || !state->native_extension || !source_descriptor ||
      !destination_descriptor || source_descriptor->supports_correlation == 0 ||
      source_descriptor->correlation_destination_identity !=
          destination.identity) {
    return impl_->fail(
        Status::device_error,
        "timestamp correlation is unsupported for these domains");
  }
  HalV2TimestampCorrelationQuery query;
  query.source_domain_identity = source.identity;
  query.destination_domain_identity = destination.identity;
  HalV2TimestampCorrelation candidate;
  HalV2Status hal_status = HalV2Status::internal_error;
  try {
    hal_status = state->extension.query_timestamp_correlation(
        state->extension.instance, &query, &candidate);
  } catch (...) {
    hal_status = HalV2Status::internal_error;
  }
  const auto status = detail::hal_v2_status_to_runtime(hal_status);
  if (status != Status::ok) {
    return impl_->fail(status, "timestamp correlation query failed");
  }
  if (!detail::validate_timestamp_correlation(query, candidate)) {
    return impl_->fail(Status::device_error,
                       "timestamp correlation output is malformed");
  }
  correlation = candidate;
  impl_->clear_error();
  return Status::ok;
}

std::size_t Runtime::resource_count() const noexcept {
    return impl_ && !impl_->provider_callback_active()
        ? impl_->resources.size()
        : 0;
}

std::size_t Runtime::dependency_count() const noexcept {
    return impl_ && !impl_->provider_callback_active()
        ? impl_->dependencies.size()
        : 0;
}

std::size_t Runtime::resource_access_count() const noexcept {
    return impl_ && !impl_->provider_callback_active()
        ? impl_->resource_accesses.size()
        : 0;
}

bool Runtime::compiled_phase_at(
    std::size_t execution_index,
    PhaseHandle& phase) const noexcept {
    phase = {};
    if (!impl_ || impl_->provider_callback_active() ||
        impl_->state == RuntimeState::configuring ||
        execution_index >= impl_->compiled_order.size()) {
        return false;
    }
    phase = impl_->compiled_order[execution_index];
    return true;
}

bool Runtime::rate_model_enabled() const noexcept {
    return impl_ && !impl_->provider_callback_active() &&
        impl_->state != RuntimeState::configuring &&
        !impl_->compiled_rate_plan.domains.empty();
}

bool Runtime::rate_execution_enabled() const noexcept {
    return impl_ && !impl_->provider_callback_active() &&
        impl_->state != RuntimeState::configuring &&
        impl_->rate_execution_policy_set;
}

std::size_t Runtime::rate_domain_count() const noexcept {
    return rate_model_enabled() ? impl_->compiled_rate_plan.domains.size() : 0;
}

std::size_t Runtime::rate_binding_count() const noexcept {
    return rate_model_enabled() ? impl_->compiled_rate_plan.bindings.size() : 0;
}

std::size_t Runtime::reference_release_count() const noexcept {
    return rate_model_enabled() ? impl_->compiled_rate_plan.releases.size() : 0;
}

std::uint64_t Runtime::reference_supercycle_ns() const noexcept {
    return rate_model_enabled() ? impl_->compiled_rate_plan.supercycle_ns : 0;
}

bool Runtime::compiled_rate_domain_at(
    std::size_t registration_index,
    CompiledRateDomain& domain) const noexcept {
    domain = {};
    if (!rate_model_enabled() ||
        registration_index >= impl_->compiled_rate_plan.domains.size()) {
        return false;
    }
    domain = impl_->compiled_rate_plan.domains[registration_index];
    return true;
}

bool Runtime::compiled_rate_binding_at(
    std::size_t compiled_phase_index,
    CompiledRateBinding& binding) const noexcept {
    binding = {};
    if (!rate_model_enabled() ||
        compiled_phase_index >= impl_->compiled_rate_plan.bindings.size()) {
        return false;
    }
    binding = impl_->compiled_rate_plan.bindings[compiled_phase_index];
    return true;
}

bool Runtime::reference_release_at(
    std::size_t release_index,
    ReferenceRelease& release) const noexcept {
    release = {};
    if (!rate_model_enabled() ||
        release_index >= impl_->compiled_rate_plan.releases.size()) {
        return false;
    }
    release = impl_->compiled_rate_plan.releases[release_index];
    return true;
}

bool Runtime::device_rate_model_enabled() const noexcept {
    return impl_ && !impl_->provider_callback_active() &&
        impl_->state != RuntimeState::configuring &&
        !impl_->compiled_device_rate_plan.phases.empty();
}

std::size_t Runtime::device_rate_phase_count() const noexcept {
    return device_rate_model_enabled()
        ? impl_->compiled_device_rate_plan.phases.size() : 0;
}

std::size_t Runtime::device_rate_command_count() const noexcept {
    return device_rate_model_enabled()
        ? impl_->compiled_device_rate_plan.commands.size() : 0;
}

std::size_t Runtime::device_rate_payload_reference_count() const noexcept {
    return device_rate_model_enabled()
        ? impl_->compiled_device_rate_plan.payload_references.size() : 0;
}

std::size_t Runtime::device_rate_timeline_reference_count() const noexcept {
    return device_rate_model_enabled()
        ? impl_->compiled_device_rate_plan.timeline_references.size() : 0;
}

bool Runtime::compiled_device_rate_phase_at(
    std::size_t phase_index,
    CompiledDeviceRatePhase& phase) const noexcept {
    phase = {};
    if (!device_rate_model_enabled() ||
        phase_index >= impl_->compiled_device_rate_plan.phases.size()) {
        return false;
    }
    phase = impl_->compiled_device_rate_plan.phases[phase_index];
    return true;
}

bool Runtime::compiled_device_rate_command_at(
    std::size_t command_index,
    CompiledDeviceRateCommand& command) const noexcept {
    command = {};
    if (!device_rate_model_enabled() ||
        command_index >= impl_->compiled_device_rate_plan.commands.size()) {
        return false;
    }
    command = impl_->compiled_device_rate_plan.commands[command_index];
    return true;
}

bool Runtime::compiled_device_rate_payload_reference_at(
    std::size_t reference_index,
    CompiledDeviceRatePayloadReference& reference) const noexcept {
    reference = {};
    if (!device_rate_model_enabled() ||
        reference_index >=
            impl_->compiled_device_rate_plan.payload_references.size()) {
        return false;
    }
    reference =
        impl_->compiled_device_rate_plan.payload_references[reference_index];
    return true;
}

bool Runtime::compiled_device_rate_timeline_reference_at(
    std::size_t reference_index,
    CompiledDeviceRateTimelineReference& reference) const noexcept {
    reference = {};
    if (!device_rate_model_enabled() ||
        reference_index >=
            impl_->compiled_device_rate_plan.timeline_references.size()) {
        return false;
    }
    reference =
        impl_->compiled_device_rate_plan.timeline_references[reference_index];
    return true;
}

bool Runtime::device_rate_admission_report(
    DeviceRateAdmissionReport& report) const noexcept {
    report = {};
    if (!device_rate_model_enabled()) {
        return false;
    }
    report = impl_->compiled_device_rate_plan.report;
    return true;
}

bool Runtime::device_rate_admission_diagnostic(
    DeviceRateAdmissionDiagnostic& diagnostic) const noexcept {
    diagnostic = {};
    if (!impl_ || impl_->provider_callback_active() ||
        impl_->device_rate_diagnostic.status == Status::ok) {
        return false;
    }
    diagnostic = impl_->device_rate_diagnostic;
    return true;
}

bool Runtime::sampled_io_model_enabled() const noexcept {
    return impl_ && !impl_->provider_callback_active() &&
        impl_->state != RuntimeState::configuring &&
        !impl_->compiled_sampled_io_plan.channels.empty();
}

std::size_t Runtime::sampled_io_channel_count() const noexcept {
    return sampled_io_model_enabled()
        ? impl_->compiled_sampled_io_plan.channels.size()
        : 0;
}

bool Runtime::compiled_sampled_io_channel_at(
    std::size_t registration_index,
    CompiledSampledIoChannel& channel) const noexcept {
    channel = {};
    if (!sampled_io_model_enabled() || registration_index >=
        impl_->compiled_sampled_io_plan.channels.size()) {
        return false;
    }
    channel = impl_->compiled_sampled_io_plan
        .channels[registration_index].public_record;
    return true;
}

bool Runtime::sampled_io_channel_status(
    CrossRateChannelHandle channel,
    SampledIoChannelStatus& status) const noexcept {
    status = {};
    if (!sampled_io_model_enabled() || !channel.valid() ||
        channel.owner() != impl_->graph_owner || channel.index() >=
            impl_->compiled_sampled_io_plan.channel_index_by_cross_rate.size()) {
        return false;
    }
    const auto index = impl_->compiled_sampled_io_plan
        .channel_index_by_cross_rate[channel.index()];
    if (index >= impl_->sampled_io_statuses.size()) {
        return false;
    }
    status = impl_->sampled_io_statuses[index];
    return true;
}

bool Runtime::live_control_enabled() const noexcept {
    return impl_ && !impl_->provider_callback_active() &&
        impl_->live_control_mailboxes != nullptr;
}

std::size_t Runtime::live_control_mailbox_count() const noexcept {
    return live_control_enabled()
        ? impl_->live_control_mailboxes->mailbox_count()
        : 0;
}

std::size_t Runtime::live_control_producer_count() const noexcept {
    return live_control_enabled()
        ? impl_->live_control_mailboxes->producer_count()
        : 0;
}

Status Runtime::live_control_producer_handle(
    std::uint64_t mailbox_identity,
    std::uint64_t producer_identity,
    LiveControlProducerHandle& handle) const noexcept {
    handle = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (!live_control_enabled()) {
        return impl_->fail(
            Status::invalid_state,
            "live-control handles require a finalized enabled policy");
    }
    const auto status = impl_->live_control_mailboxes->producer_handle(
        mailbox_identity,
        producer_identity,
        handle);
    if (status != Status::ok) {
        return impl_->fail(
            status,
            "live-control producer identity is not registered in the mailbox");
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::stage_live_control_update(
    LiveControlProducerHandle producer,
    const LiveControlUpdateRecord& update,
    std::span<const std::byte> payload,
    LiveControlAdmissionResult& result) noexcept {
    result = LiveControlAdmissionResult::invalid;
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (g_active_runtime_callback == impl_.get()) {
        return impl_->fail(
            Status::invalid_state,
            "live-control admission is restricted to non-runtime producer threads");
    }
    if (!live_control_enabled()) {
        return impl_->fail(
            Status::invalid_state,
            "live-control admission requires a finalized enabled policy");
    }
    result = impl_->live_control_mailboxes->stage(
        producer,
        update,
        payload);
    impl_->live_control_mailboxes->record_admission(
        producer,
        update,
        result);
    return Status::ok;
}

bool Runtime::live_control_mailbox_info(
    std::uint64_t mailbox_identity,
    LiveControlMailboxInfo& info) const noexcept {
    info = {};
    return live_control_enabled() &&
        impl_->live_control_mailboxes->mailbox_info(mailbox_identity, info);
}

bool Runtime::live_control_record_at(
    std::uint64_t mailbox_identity,
    std::uint64_t mailbox_sequence,
    LiveControlUpdateRecord& record) const noexcept {
    record = {};
    return live_control_enabled() &&
        impl_->live_control_mailboxes->record_at(
            mailbox_identity,
            mailbox_sequence,
            record);
}

Status Runtime::copy_live_control_payload(
    std::uint64_t mailbox_identity,
    std::uint64_t mailbox_sequence,
    std::span<std::byte> output) const noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (!live_control_enabled()) {
        return impl_->fail(
            Status::invalid_state,
            "live-control payload inspection requires a finalized enabled policy");
    }
    const auto status = impl_->live_control_mailboxes->copy_payload(
        mailbox_identity,
        mailbox_sequence,
        output);
    if (status != Status::ok) {
        return impl_->fail(
            status,
            "live-control payload destination or sequence is invalid");
    }
    impl_->clear_error();
    return Status::ok;
}

bool Runtime::live_control_commit_info(
    LiveControlCommitInfo& info) const noexcept {
    return live_control_enabled() &&
        impl_->live_control_mailboxes->commit_info(info);
}

bool Runtime::live_control_record_status(
    std::uint64_t mailbox_identity,
    std::uint64_t mailbox_sequence,
    LiveControlRecordStatusInfo& info) const noexcept {
    return live_control_enabled() &&
        impl_->live_control_mailboxes->record_status(
            mailbox_identity,
            mailbox_sequence,
            info);
}

bool Runtime::live_control_closure_enabled() const noexcept {
    return live_control_enabled() && impl_->live_control_closure_policy_set;
}

Status Runtime::live_control_action_metadata(
    LiveControlActionMetadata& metadata) noexcept {
    metadata = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (!live_control_closure_enabled()) {
        return impl_->fail(
            Status::invalid_state,
            "live-control action metadata requires enabled closure");
    }
    if (!impl_->live_control_mailboxes->action_metadata(metadata)) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::read_live_control_actions(
    LiveControlActionCursor& cursor,
    std::span<LiveControlActionRecord> output,
    LiveControlActionReadResult& result) noexcept {
    result = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (!live_control_closure_enabled()) {
        return impl_->fail(
            Status::invalid_state,
            "live-control action reading requires enabled closure");
    }
    const auto status = impl_->live_control_mailboxes->read_actions(
        cursor,
        output,
        result);
    if (status != Status::ok) {
        return impl_->fail(status, "live-control action cursor is invalid");
    }
    impl_->clear_error();
    return Status::ok;
}

bool Runtime::device_rate_admission_backend_at(
    std::size_t backend_index,
    DeviceRateAdmissionBackend& backend) const noexcept {
    backend = {};
    if (!device_rate_model_enabled() || backend_index >=
        impl_->compiled_device_rate_plan.admission_backends.size()) {
        return false;
    }
    backend =
        impl_->compiled_device_rate_plan.admission_backends[backend_index];
    return true;
}

bool Runtime::device_rate_admission_phase_at(
    std::size_t phase_index,
    DeviceRateAdmissionPhase& phase) const noexcept {
    phase = {};
    if (!device_rate_model_enabled() || phase_index >=
        impl_->compiled_device_rate_plan.admission_phases.size()) {
        return false;
    }
    phase = impl_->compiled_device_rate_plan.admission_phases[phase_index];
    return true;
}

bool Runtime::device_rate_admission_interval_at(
    std::size_t interval_index,
    DeviceRateAdmissionInterval& interval) const noexcept {
    interval = {};
    if (!device_rate_model_enabled() || interval_index >=
        impl_->compiled_device_rate_plan.admission_intervals.size()) {
        return false;
    }
    interval =
        impl_->compiled_device_rate_plan.admission_intervals[interval_index];
    return true;
}

bool Runtime::cross_rate_model_enabled() const noexcept {
    return impl_ && !impl_->provider_callback_active() &&
        impl_->state != RuntimeState::configuring &&
        !impl_->compiled_cross_rate_plan.channels.empty();
}

std::size_t Runtime::cross_rate_channel_count() const noexcept {
    return cross_rate_model_enabled()
        ? impl_->compiled_cross_rate_plan.channels.size()
        : 0;
}

std::size_t Runtime::cross_rate_selection_count() const noexcept {
    return cross_rate_model_enabled()
        ? impl_->compiled_cross_rate_plan.selections.size()
        : 0;
}

bool Runtime::compiled_cross_rate_channel_at(
    std::size_t registration_index,
    CompiledCrossRateChannel& channel) const noexcept {
    channel = {};
    if (!cross_rate_model_enabled() ||
        registration_index >=
            impl_->compiled_cross_rate_plan.channels.size()) {
        return false;
    }
    channel = impl_->compiled_cross_rate_plan.channels[registration_index];
    return true;
}

bool Runtime::compiled_cross_rate_selection_at(
    std::size_t selection_index,
    CompiledCrossRateSelection& selection) const noexcept {
    selection = {};
    if (!cross_rate_model_enabled() ||
        selection_index >=
            impl_->compiled_cross_rate_plan.selections.size()) {
        return false;
    }
    selection = impl_->compiled_cross_rate_plan.selections[selection_index];
    return true;
}

Status Runtime::copy_cross_rate_initial_sample(
    std::size_t registration_index,
    std::span<std::byte> output) const noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (!cross_rate_model_enabled() ||
        registration_index >= impl_->cross_rate_channels.size()) {
        return Status::invalid_argument;
    }
    const auto& initial =
        impl_->cross_rate_channels[registration_index].initial_sample;
    if (output.size() < initial.size()) {
        return Status::capacity_exceeded;
    }
    if (output.size() != initial.size()) {
        return Status::invalid_argument;
    }
    std::copy(initial.begin(), initial.end(), output.begin());
    return Status::ok;
}

bool Runtime::static_phase_assignment_at(
    std::size_t registration_index,
    StaticPhaseAssignment& assignment) const noexcept {
    assignment = {};
    if (!impl_ || impl_->provider_callback_active() || !impl_->executor ||
        impl_->state == RuntimeState::configuring ||
        registration_index >= impl_->callbacks.size()) {
        return false;
    }

    std::size_t worker_index = 0;
    if (!impl_->executor->static_assignment(
            registration_index,
            worker_index)) {
        return false;
    }
    assignment.phase = PhaseHandle{
        impl_->graph_owner,
        static_cast<std::uint32_t>(registration_index)};
    assignment.worker_index = worker_index;
    return true;
}

ExecutorStats Runtime::executor_stats() const noexcept {
    if (!impl_ || impl_->provider_callback_active() || !impl_->executor) {
        return {};
    }
    return impl_->executor->stats();
}

bool Runtime::memory_plan(MemoryPlan& plan) const noexcept {
    plan = {};
    if (!impl_ || impl_->provider_callback_active() ||
        impl_->state == RuntimeState::configuring) {
        return false;
    }
    plan = impl_->finalized_memory_plan;
    return true;
}

bool Runtime::cpu_memory_policy_report(
    CpuMemoryPolicyReport& report) const noexcept {
    report = {};
    if (!impl_ || impl_->provider_callback_active() ||
        !impl_->cpu_memory_policy_report_available ||
        impl_->state == RuntimeState::configuring) {
        return false;
    }
    report = impl_->cpu_memory_policy_report;
    return true;
}

bool Runtime::platform_preflight_report(
    PlatformPreflightReport& report) const noexcept {
    report = {};
    if (!impl_ || impl_->provider_callback_active() ||
        !impl_->preflight_report_available) {
        return false;
    }
    report = impl_->preflight_report;
    return true;
}

std::uint32_t Runtime::degradation_level() const noexcept {
    return impl_ && !impl_->provider_callback_active()
        ? impl_->degradation_level.load(std::memory_order_acquire)
        : 0;
}

std::uint64_t Runtime::now_ns() noexcept {
    return impl_ && !impl_->provider_callback_active()
        ? impl_->clock_now()
        : 0;
}

std::string_view Runtime::last_error() const noexcept {
    return impl_ && !impl_->provider_callback_active()
        ? std::string_view(impl_->error.data())
        : std::string_view{};
}

Status Runtime::observability_metadata(
    ObservabilityMetadata& metadata) noexcept {
    metadata = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state == RuntimeState::configuring ||
        !impl_->telemetry) {
        return impl_->fail(
            Status::invalid_state,
            "observability metadata requires a finalized runtime");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "observability export cannot run during a frame or periodic loop");
    }
    metadata = impl_->observability;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::metrics_snapshot(
    RuntimeMetricWindow window,
    RuntimeMetricCursor* cursor,
    RuntimeMetricSnapshot& snapshot) noexcept {
    snapshot = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state == RuntimeState::configuring ||
        !impl_->telemetry) {
        return impl_->fail(
            Status::invalid_state,
            "metric export requires a finalized runtime");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "metric export cannot run during a frame or periodic loop");
    }
    if (window != RuntimeMetricWindow::cumulative &&
        window != RuntimeMetricWindow::interval) {
        return impl_->fail(
            Status::invalid_argument,
            "metric window is invalid");
    }
    if (window == RuntimeMetricWindow::interval &&
        (!cursor ||
         cursor->schema_version != observability_schema_version ||
         cursor->reserved0 != 0 ||
         (cursor->runtime_id != 0 &&
          cursor->runtime_id != impl_->observability.runtime_id))) {
        return impl_->fail(
            Status::invalid_argument,
            "interval metric cursor is invalid or belongs to another runtime");
    }
    const bool fresh_interval_cursor =
        window == RuntimeMetricWindow::interval &&
        cursor->runtime_id == 0;
    if (fresh_interval_cursor &&
        (cursor->window_end_ns != 0 ||
         !std::all_of(
             cursor->counters.begin(),
             cursor->counters.end(),
             [](std::uint64_t value) {
                 return value == 0;
             }))) {
        return impl_->fail(
            Status::invalid_argument,
            "fresh interval metric cursor is not zero initialized");
    }

    const auto current = impl_->metric_values();
    const auto end_ns = impl_->clock_now();
    if (window == RuntimeMetricWindow::interval &&
        !fresh_interval_cursor &&
        (cursor->window_end_ns < impl_->telemetry_epoch_ns ||
         cursor->window_end_ns > end_ns)) {
        return impl_->fail(
            Status::invalid_argument,
            "interval metric cursor window is outside the runtime clock range");
    }
    snapshot.metadata = impl_->observability;
    snapshot.window = window;
    snapshot.window_start_ns =
        window == RuntimeMetricWindow::interval &&
            !fresh_interval_cursor
        ? cursor->window_end_ns
        : impl_->telemetry_epoch_ns;
    snapshot.window_end_ns = end_ns;
    snapshot.sample_count = runtime_metric_count;

    for (std::size_t index = 0;
         index < runtime_metric_count;
         ++index) {
        RuntimeMetricDefinition definition;
        if (!runtime_metric_definition(index, definition)) {
            return impl_->fail(
                Status::internal_error,
                "metric schema is incomplete");
        }
        auto value = current[index];
        if (window == RuntimeMetricWindow::interval &&
            definition.kind == RuntimeMetricKind::counter) {
            if (value < cursor->counters[index]) {
                return impl_->fail(
                    Status::internal_error,
                    "monotonic metric counter regressed");
            }
            value -= cursor->counters[index];
        }
        snapshot.samples[index] = RuntimeMetricSample{
            definition.id,
            definition.kind,
            0,
            0,
            value,
        };
    }

    snapshot.snapshot_sequence =
        ++impl_->metric_snapshot_sequence;
    if (window == RuntimeMetricWindow::interval) {
        cursor->runtime_id =
            impl_->observability.runtime_id;
        cursor->window_end_ns = end_ns;
        cursor->counters = current;
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::read_trace(
    RuntimeTraceCursor& cursor,
    std::span<RuntimeTraceEvent> output,
    RuntimeTraceReadResult& result) noexcept {
    result = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state == RuntimeState::configuring ||
        !impl_->telemetry) {
        return impl_->fail(
            Status::invalid_state,
            "trace export requires a finalized runtime");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "trace export cannot run during a frame or periodic loop");
    }
    if (cursor.schema_version != observability_schema_version ||
        cursor.reserved0 != 0 ||
        (cursor.runtime_id == 0 &&
         cursor.next_sequence != 0) ||
        (cursor.runtime_id != 0 &&
         cursor.runtime_id != impl_->observability.runtime_id)) {
        return impl_->fail(
            Status::invalid_argument,
            "trace cursor is invalid or belongs to another runtime");
    }

    const auto end = impl_->telemetry->next_sequence();
    const auto oldest =
        impl_->telemetry->oldest_sequence(end);
    std::uint64_t sequence = cursor.next_sequence;
    if (cursor.runtime_id == 0) {
        cursor.runtime_id = impl_->observability.runtime_id;
        sequence = oldest;
    } else if (sequence > end) {
        return impl_->fail(
            Status::invalid_argument,
            "trace cursor points beyond the current sequence");
    }

    result.metadata = impl_->observability;
    if (sequence < oldest) {
        result.lost_events = oldest - sequence;
        sequence = oldest;
    }
    result.first_sequence = sequence;

    while (sequence < end &&
           result.events_read < output.size()) {
        RuntimeTraceEvent event;
        if (impl_->telemetry->read_sequence(
                sequence,
                event)) {
            output[result.events_read] = event;
            ++result.events_read;
        } else {
            ++result.lost_events;
        }
        ++sequence;
    }

    cursor.next_sequence = sequence;
    result.next_sequence = sequence;
    result.remaining_sequence_count = end - sequence;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::rate_telemetry_metadata(
    RateTelemetryMetadata& metadata) noexcept {
    metadata = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state == RuntimeState::configuring ||
        !impl_->rate_telemetry || !impl_->rate_counters) {
        return impl_->fail(
            Status::invalid_state,
            "rate telemetry requires a finalized active-rate runtime");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "rate telemetry inspection cannot run during execution");
    }
    metadata.schema_version = rate_action_schema_version;
    metadata.record_size = sizeof(RateActionRecord);
    metadata.counter_count = rate_action_counter_count;
    metadata.host_policy_version =
        impl_->rate_execution_policy.host_policy_version;
    metadata.runtime_id = impl_->observability.runtime_id;
    metadata.capacity = impl_->rate_telemetry->capacity();
    metadata.next_sequence = impl_->rate_telemetry->next_sequence();
    metadata.records_emitted = impl_->rate_telemetry->emitted();
    metadata.records_overwritten = impl_->rate_telemetry->overwritten();
    metadata.records_dropped = impl_->rate_telemetry->dropped();
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::rate_counters_snapshot(
    RateCounterSnapshot& snapshot) noexcept {
    snapshot = {};
    const auto status = rate_telemetry_metadata(snapshot.metadata);
    if (status != Status::ok) {
        return status;
    }
    for (std::size_t index = 0;
         index < rate_action_counter_count;
         ++index) {
        snapshot.values[index] = impl_->rate_counters->load(
            static_cast<RateCounterId>(index));
    }
    snapshot.values[static_cast<std::size_t>(RateCounterId::records_emitted)] =
        snapshot.metadata.records_emitted;
    snapshot.values[static_cast<std::size_t>(RateCounterId::records_overwritten)] =
        snapshot.metadata.records_overwritten;
    snapshot.values[static_cast<std::size_t>(RateCounterId::records_dropped)] =
        snapshot.metadata.records_dropped;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::read_rate_actions(
    RateTelemetryCursor& cursor,
    std::span<RateActionRecord> output,
    RateTelemetryReadResult& result) noexcept {
    result = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state == RuntimeState::configuring ||
        !impl_->rate_telemetry || !impl_->rate_counters) {
        return impl_->fail(
            Status::invalid_state,
            "rate action inspection requires a finalized active-rate runtime");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "rate action inspection cannot run during execution");
    }
    if (cursor.schema_version != rate_action_schema_version ||
        cursor.reserved0 != 0 ||
        (cursor.runtime_id == 0 && cursor.next_sequence != 0) ||
        (cursor.runtime_id != 0 &&
         cursor.runtime_id != impl_->observability.runtime_id)) {
        return impl_->fail(
            Status::invalid_argument,
            "rate telemetry cursor is malformed or foreign");
    }
    const auto end = impl_->rate_telemetry->next_sequence();
    if (cursor.runtime_id != 0 && cursor.next_sequence > end) {
        return impl_->fail(
            Status::invalid_argument,
            "rate telemetry cursor points beyond the stream");
    }
    const auto oldest = impl_->rate_telemetry->oldest_sequence(end);
    auto sequence = cursor.next_sequence;
    RateTelemetryReadResult candidate;
    candidate.metadata.schema_version = rate_action_schema_version;
    candidate.metadata.record_size = sizeof(RateActionRecord);
    candidate.metadata.counter_count = rate_action_counter_count;
    candidate.metadata.host_policy_version =
        impl_->rate_execution_policy.host_policy_version;
    candidate.metadata.runtime_id = impl_->observability.runtime_id;
    candidate.metadata.capacity = impl_->rate_telemetry->capacity();
    candidate.metadata.next_sequence = end;
    candidate.metadata.records_emitted = impl_->rate_telemetry->emitted();
    candidate.metadata.records_overwritten = impl_->rate_telemetry->overwritten();
    candidate.metadata.records_dropped = impl_->rate_telemetry->dropped();
    if (sequence < oldest) {
        candidate.lost_records = oldest - sequence;
        sequence = oldest;
    }
    candidate.first_sequence = sequence;
    while (sequence < end && candidate.records_read < output.size()) {
        RateActionRecord record;
        if (impl_->rate_telemetry->read_sequence(sequence, record)) {
            output[candidate.records_read] = record;
            ++candidate.records_read;
        } else {
            ++candidate.lost_records;
        }
        ++sequence;
    }
    candidate.next_sequence = sequence;
    candidate.remaining_sequence_count = end - sequence;
    RateTelemetryCursor committed = cursor;
    committed.runtime_id = impl_->observability.runtime_id;
    committed.next_sequence = sequence;
    cursor = committed;
    result = candidate;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::mixed_rate_action_metadata(
    MixedRateActionMetadata& metadata) noexcept {
    metadata = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state == RuntimeState::configuring ||
        !impl_->mixed_rate_actions ||
        !impl_->mixed_rate_closure_policy_set) {
        return impl_->fail(
            Status::invalid_state,
            "mixed-rate action inspection requires finalized closure");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "mixed-rate action inspection cannot run during execution");
    }
    metadata.host_policy_version =
        impl_->mixed_rate_closure_policy.host_policy_version;
    metadata.runtime_id = impl_->observability.runtime_id;
    metadata.capacity = impl_->mixed_rate_actions->capacity();
    metadata.next_sequence = impl_->mixed_rate_actions->next_sequence();
    metadata.records_emitted = impl_->mixed_rate_actions->emitted();
    metadata.records_overwritten = impl_->mixed_rate_actions->overwritten();
    metadata.records_dropped = impl_->mixed_rate_actions->dropped();
    metadata.replay_eligible =
        impl_->mixed_rate_closure_policy.active_replay_enabled &&
        metadata.records_overwritten == 0 && metadata.records_dropped == 0;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::read_mixed_rate_actions(
    MixedRateActionCursor& cursor,
    std::span<MixedRateActionRecord> output,
    MixedRateActionReadResult& result) noexcept {
    result = {};
    MixedRateActionMetadata metadata;
    const auto metadata_status = mixed_rate_action_metadata(metadata);
    if (metadata_status != Status::ok) {
        return metadata_status;
    }
    if (cursor.schema_version != mixed_rate_action_schema_version ||
        cursor.reserved0 != 0 ||
        (cursor.runtime_id == 0 && cursor.next_sequence != 0) ||
        (cursor.runtime_id != 0 &&
         cursor.runtime_id != impl_->observability.runtime_id)) {
        return impl_->fail(
            Status::invalid_argument,
            "mixed-rate action cursor is malformed or foreign");
    }
    const auto end = metadata.next_sequence;
    if (cursor.runtime_id != 0 && cursor.next_sequence > end) {
        return impl_->fail(
            Status::invalid_argument,
            "mixed-rate action cursor points beyond the stream");
    }
    const auto oldest = impl_->mixed_rate_actions->oldest_sequence(end);
    auto sequence = cursor.next_sequence;
    MixedRateActionReadResult candidate;
    candidate.metadata = metadata;
    if (sequence < oldest) {
        candidate.lost_records = oldest - sequence;
        sequence = oldest;
    }
    candidate.first_sequence = sequence;
    while (sequence < end && candidate.records_read < output.size()) {
        MixedRateActionRecord record;
        if (impl_->mixed_rate_actions->read_sequence(sequence, record)) {
            output[candidate.records_read] = record;
            ++candidate.records_read;
        } else {
            ++candidate.lost_records;
        }
        ++sequence;
    }
    candidate.next_sequence = sequence;
    candidate.remaining_sequence_count = end - sequence;
    cursor.runtime_id = impl_->observability.runtime_id;
    cursor.next_sequence = sequence;
    result = candidate;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::checkpoint_size(
    std::size_t& required_bytes) noexcept {
    required_bytes = 0;
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state == RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "checkpoint sizing requires a finalized runtime");
    }
    std::size_t record_bytes = 0;
    std::size_t payload_bytes = 0;
    if (!detail::checked_artifact_multiply(
            impl_->checkpoint_state_count(),
            detail::checkpoint_record_header_size,
            record_bytes)) {
        return impl_->fail(
            Status::internal_error,
            "finalized checkpoint size overflowed");
    }
    for (const auto& state : impl_->states) {
        if (!detail::checked_artifact_add(
                payload_bytes,
                state.storage.size(),
                payload_bytes)) {
            return impl_->fail(
                Status::internal_error,
                "finalized checkpoint size overflowed");
        }
    }
    if (impl_->rate_execution_policy_set &&
        !detail::checked_artifact_add(
            payload_bytes,
            impl_->active_checkpoint_state.size(),
            payload_bytes)) {
        return impl_->fail(
            Status::internal_error,
            "finalized checkpoint size overflowed");
    }
    if (impl_->mixed_rate_closure_policy_set &&
        !detail::checked_artifact_add(
            payload_bytes,
            impl_->mixed_rate_checkpoint_state.size(),
            payload_bytes)) {
        return impl_->fail(
            Status::internal_error,
            "finalized mixed-rate checkpoint size overflowed");
    }
    if (impl_->live_control_closure_policy_set &&
        (!impl_->live_control_mailboxes ||
         !detail::checked_artifact_add(
             payload_bytes,
             impl_->live_control_mailboxes->checkpoint_state_size(),
             payload_bytes))) {
        return impl_->fail(
            Status::internal_error,
            "finalized live-control checkpoint size overflowed");
    }
    if (!detail::checked_artifact_add(
            detail::checkpoint_header_size,
            record_bytes,
            required_bytes) ||
        !detail::checked_artifact_add(
            required_bytes,
            payload_bytes,
            required_bytes) ||
        required_bytes > impl_->config.snapshot_max_bytes) {
        required_bytes = 0;
        return impl_->fail(
            Status::internal_error,
            "finalized checkpoint exceeds its frozen bound");
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::write_checkpoint(
    std::uint64_t checkpoint_frame_index,
    std::span<std::byte> output,
    ArtifactWriteResult& result) noexcept {
    result = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state == RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "checkpoint export requires a finalized runtime");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "checkpoint export cannot run during execution");
    }
    if (impl_->stop_pending || impl_->lane_cleanup_pending ||
        std::any_of(
            impl_->active_device_tickets.begin(),
            impl_->active_device_tickets.end(),
            [](const detail::DeviceRateTicket& ticket) {
                return ticket.valid();
            })) {
        return impl_->fail(
            Status::invalid_state,
            "mixed-rate checkpoint requires a quiescent release boundary");
    }
    const bool live_control_claim_already_held =
        impl_->live_control_closure_policy_set &&
        impl_->live_control_mailboxes->host_claimed();
    if (impl_->live_control_closure_policy_set &&
        !live_control_claim_already_held &&
        !impl_->live_control_mailboxes->claim_all()) {
        return impl_->fail(
            Status::invalid_state,
            "live-control checkpoint claim is busy; retry");
    }
    struct LiveControlCheckpointClaimGuard {
        detail::LiveControlMailboxSet* mailboxes = nullptr;
        ~LiveControlCheckpointClaimGuard() {
            if (mailboxes) {
                mailboxes->release_all();
            }
        }
    } live_control_claim{
        impl_->live_control_closure_policy_set &&
                !live_control_claim_already_held
            ? impl_->live_control_mailboxes.get()
            : nullptr};
    const auto output_bytes =
        std::span<const std::byte>(output.data(), output.size());
    for (const auto& state : impl_->states) {
        if (byte_spans_overlap(
                output_bytes,
                std::as_bytes(state.storage))) {
            return impl_->fail(
                Status::invalid_argument,
                "checkpoint output cannot overlap registered state");
        }
    }

    CheckpointMetadata metadata;
    metadata.determinism_tier =
        impl_->config.determinism_tier;
    metadata.config_id = impl_->observability.config_id;
    metadata.replay_id = impl_->replay_id;
    metadata.graph_id = impl_->graph_id;
    metadata.state_schema_id = impl_->state_schema_id;
    metadata.checkpoint_frame_index =
        checkpoint_frame_index;
    metadata.build_id = impl_->observability.build_id;
    metadata.workload_id =
        impl_->observability.workload_id;
    const auto status = detail::encode_checkpoint_artifact(
        metadata,
        impl_->checkpoint_state_count(),
        &Impl::provide_state,
        impl_.get(),
        impl_->config.snapshot_max_bytes,
        output,
        result);
    if (status != Status::ok) {
        return impl_->fail(
            status,
            status == Status::capacity_exceeded
                ? "checkpoint output buffer is too small"
                : "checkpoint encoding failed");
    }
    if (impl_->live_control_closure_policy_set) {
        impl_->live_control_mailboxes->record_checkpoint(result.checksum);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::restore_checkpoint(
    std::span<const std::byte> checkpoint,
    CheckpointMetadata* metadata) noexcept {
    if (metadata) {
        *metadata = {};
    }
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state == RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "checkpoint restore requires a finalized runtime");
    }
    if (impl_->stop_pending) {
        return impl_->fail(
            Status::invalid_state,
            "device teardown is pending; retry stop");
    }
    if (impl_->rate_execution_policy_set && impl_->active_faulted &&
        !impl_->active_replay_restore_allowed) {
        return impl_->fail(
            Status::invalid_state,
            "active policy failure is terminal for checkpoint restore");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "checkpoint restore cannot run during execution");
    }
    const bool live_control_restore_claim_already_held =
        impl_->live_control_closure_policy_set &&
        impl_->live_control_mailboxes->host_claimed();
    if (impl_->live_control_closure_policy_set &&
        !live_control_restore_claim_already_held &&
        !impl_->live_control_mailboxes->claim_all()) {
        return impl_->fail(
            Status::invalid_state,
            "live-control restore claim is busy; retry");
    }
    struct LiveControlRestoreClaimGuard {
        detail::LiveControlMailboxSet* mailboxes = nullptr;
        ~LiveControlRestoreClaimGuard() {
            if (mailboxes) {
                mailboxes->release_all();
            }
        }
    } live_control_claim{
        impl_->live_control_closure_policy_set &&
                !live_control_restore_claim_already_held
            ? impl_->live_control_mailboxes.get()
            : nullptr};
    for (const auto& state : impl_->states) {
        if (byte_spans_overlap(
                checkpoint,
                std::as_bytes(state.storage))) {
            return impl_->fail(
                Status::invalid_argument,
                "checkpoint input cannot overlap registered state");
        }
    }

    CheckpointMetadata parsed;
    const auto parse_status =
        detail::parse_checkpoint_artifact(
            checkpoint,
            impl_->config.snapshot_max_bytes,
            impl_->checkpoint_state_count(),
            parsed);
    if (parse_status != Status::ok) {
        return impl_->fail(
            parse_status,
            "checkpoint validation failed before state restore");
    }
    if (parsed.runtime_version_major != version_major ||
        parsed.determinism_tier !=
            impl_->config.determinism_tier ||
        parsed.replay_id != impl_->replay_id ||
        parsed.graph_id != impl_->graph_id ||
        parsed.state_schema_id != impl_->state_schema_id ||
        parsed.state_count != impl_->checkpoint_state_count() ||
        parsed.workload_id != impl_->observability.workload_id ||
        (impl_->config.determinism_tier ==
             DeterminismTier::unspecified &&
         parsed.config_id != impl_->observability.config_id)) {
        return impl_->fail(
            Status::incompatible_artifact,
            "checkpoint identity does not match the finalized runtime");
    }

    // First pass verifies the exact registered schema and every destination
    // size. No application byte is changed until this pass succeeds.
    detail::CheckpointRecordCursor cursor;
    for (std::size_t index = 0;
         index < impl_->states.size();
         ++index) {
        detail::CheckpointRecordView record;
        const auto& state = impl_->states[index];
        if (!detail::next_checkpoint_record(
                checkpoint,
                parsed,
                cursor,
                record) ||
            record.name != identifier_view(state.name) ||
            record.schema_version != state.schema_version ||
            record.payload.size() != state.storage.size()) {
            return impl_->fail(
                Status::incompatible_artifact,
                "checkpoint state schema does not match registration");
        }
    }
    detail::CheckpointRecordView active_record;
    if (impl_->rate_execution_policy_set &&
        (!detail::next_checkpoint_record(
             checkpoint,
             parsed,
             cursor,
             active_record) ||
         active_record.name != kRateDispatchStateName ||
         active_record.schema_version != kRateDispatchStateSchema ||
         !impl_->validate_active_checkpoint_state(
             active_record.payload))) {
        return impl_->fail(
            Status::incompatible_artifact,
            "active checkpoint state is malformed or incompatible");
    }
    detail::CheckpointRecordView mixed_record;
    if (impl_->mixed_rate_closure_policy_set &&
        (!detail::next_checkpoint_record(
             checkpoint,
             parsed,
             cursor,
             mixed_record) ||
         mixed_record.name != kMixedRateStateName ||
         mixed_record.schema_version != kMixedRateStateSchema ||
         !impl_->validate_mixed_rate_checkpoint_state(
             mixed_record.payload))) {
        return impl_->fail(
            Status::incompatible_artifact,
            "mixed-rate checkpoint state is malformed or incompatible");
    }
    detail::CheckpointRecordView live_control_record;
    if (impl_->live_control_closure_policy_set &&
        (!detail::next_checkpoint_record(
             checkpoint,
             parsed,
             cursor,
             live_control_record) ||
         live_control_record.name != kLiveControlStateName ||
         live_control_record.schema_version != kLiveControlStateSchema ||
         !impl_->live_control_mailboxes->validate_checkpoint_state(
             live_control_record.payload))) {
        return impl_->fail(
            Status::incompatible_artifact,
            "live-control checkpoint state is malformed or incompatible");
    }

    // Rebuild internal active stores before application bytes. Semantic
    // validation above makes this operation infallible; retaining the checked
    // result protects the transaction if those invariants ever diverge.
    if (impl_->rate_execution_policy_set &&
        !impl_->apply_active_checkpoint_state(active_record.payload)) {
        return impl_->fail(
            Status::internal_error,
            "active checkpoint store rebuild failed");
    }
    if (impl_->mixed_rate_closure_policy_set) {
        impl_->apply_mixed_rate_checkpoint_state(mixed_record.payload);
    }
    if (impl_->live_control_closure_policy_set &&
        !impl_->live_control_mailboxes->restore_checkpoint_state(
            live_control_record.payload)) {
        return impl_->fail(
            Status::internal_error,
            "live-control checkpoint store rebuild failed");
    }

    // The second pass cannot fail: the complete source and every destination
    // were validated above, so registered application bytes remain
    // transactional.
    cursor = {};
    for (std::size_t index = 0;
         index < impl_->states.size();
         ++index) {
        detail::CheckpointRecordView record;
        (void)detail::next_checkpoint_record(
            checkpoint,
            parsed,
            cursor,
            record);
        std::memcpy(
            impl_->states[index].storage.data(),
            record.payload.data(),
            record.payload.size());
    }
    if (metadata) {
        *metadata = parsed;
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::write_input_log(
    std::span<const ReplayInputRecord> records,
    std::span<std::byte> output,
    ArtifactWriteResult& result) noexcept {
    result = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state == RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "input-log export requires a finalized runtime");
    }
    if (impl_->rate_execution_policy_set) {
        return impl_->fail(
            Status::invalid_state,
            "schema-1 input logs cannot encode active rate actions");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "input-log export cannot run during execution");
    }
    const auto output_bytes =
        std::span<const std::byte>(output.data(), output.size());
    if (byte_spans_overlap(
            output_bytes,
            std::as_bytes(records))) {
        return impl_->fail(
            Status::invalid_argument,
            "input-log output cannot overlap input records");
    }
    for (const auto& state : impl_->states) {
        if (byte_spans_overlap(
                output_bytes,
                std::as_bytes(state.storage))) {
            return impl_->fail(
                Status::invalid_argument,
                "input-log output cannot overlap registered state");
        }
    }
    for (const auto& record : records) {
        if (byte_spans_overlap(output_bytes, record.payload)) {
            return impl_->fail(
                Status::invalid_argument,
                "input-log output cannot overlap an input payload");
        }
    }

    InputLogMetadata metadata;
    metadata.determinism_tier =
        impl_->config.determinism_tier;
    metadata.replay_id = impl_->replay_id;
    metadata.state_schema_id = impl_->state_schema_id;
    metadata.workload_id =
        impl_->observability.workload_id;
    const auto status = detail::encode_input_log_artifact(
        metadata,
        records,
        impl_->config.replay_input_capacity,
        impl_->config.input_log_max_bytes,
        output,
        result);
    if (status != Status::ok) {
        return impl_->fail(
            status,
            status == Status::capacity_exceeded
                ? "input-log output buffer is too small or exceeds its bound"
                : "input-log encoding failed");
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::replay(
    std::span<const std::byte> checkpoint,
    std::span<const std::byte> input_log,
    ReplayInputCallback input_callback,
    void* input_user_data,
    ReplayResult* result) noexcept {
    ReplayResult local_result;
    ReplayResult& output = result ? *result : local_result;
    output = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::running) {
        return impl_->fail(
            Status::invalid_state,
            "replay requires a running runtime");
    }
    if (impl_->rate_execution_policy_set) {
        return impl_->fail(
            Status::invalid_state,
            "schema-1 replay cannot reproduce active nominal releases or late actions");
    }
    if (impl_->stop_pending) {
        return impl_->fail(
            Status::invalid_state,
            "device teardown is pending; retry stop");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "replay requires non-reentrant host control");
    }
    for (const auto& state : impl_->states) {
        if (byte_spans_overlap(
                input_log,
                std::as_bytes(state.storage))) {
            return impl_->fail(
                Status::invalid_argument,
                "input log cannot overlap registered state");
        }
    }

    CheckpointMetadata checkpoint_metadata;
    InputLogMetadata input_metadata;
    const auto checkpoint_status =
        detail::parse_checkpoint_artifact(
            checkpoint,
            impl_->config.snapshot_max_bytes,
            impl_->config.state_capacity,
            checkpoint_metadata);
    if (checkpoint_status != Status::ok) {
        return impl_->fail(
            checkpoint_status,
            "checkpoint validation failed before replay");
    }
    const auto input_status =
        detail::parse_input_log_artifact(
            input_log,
            impl_->config.input_log_max_bytes,
            impl_->config.replay_input_capacity,
            input_metadata);
    if (input_status != Status::ok) {
        return impl_->fail(
            input_status,
            "input-log validation failed before replay");
    }
    if (checkpoint_metadata.runtime_version_major !=
            version_major ||
        checkpoint_metadata.determinism_tier !=
            impl_->config.determinism_tier ||
        checkpoint_metadata.replay_id != impl_->replay_id ||
        checkpoint_metadata.graph_id != impl_->graph_id ||
        checkpoint_metadata.state_schema_id !=
            impl_->state_schema_id ||
        checkpoint_metadata.workload_id !=
            impl_->observability.workload_id ||
        input_metadata.runtime_version_major != version_major ||
        input_metadata.determinism_tier !=
            impl_->config.determinism_tier ||
        input_metadata.replay_id != impl_->replay_id ||
        input_metadata.state_schema_id !=
            impl_->state_schema_id ||
        input_metadata.workload_id !=
            impl_->observability.workload_id ||
        (input_metadata.record_count != 0 &&
         input_metadata.first_frame_index <=
             checkpoint_metadata.checkpoint_frame_index) ||
        (impl_->config.determinism_tier ==
             DeterminismTier::unspecified &&
         checkpoint_metadata.config_id !=
             impl_->observability.config_id)) {
        return impl_->fail(
            Status::incompatible_artifact,
            "checkpoint/input-log identity does not match the runtime");
    }
    if (input_metadata.record_count != 0 && !input_callback) {
        return impl_->fail(
            Status::invalid_argument,
            "replay input callback is required for a non-empty log");
    }

    const auto restore_status =
        restore_checkpoint(checkpoint, nullptr);
    if (restore_status != Status::ok) {
        return restore_status;
    }

    bool expected = false;
    if (!impl_->in_replay.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return impl_->fail(
            Status::invalid_state,
            "replay is already active");
    }
    struct ReplayGuard {
        std::atomic<bool>& flag;
        explicit ReplayGuard(std::atomic<bool>& value)
            : flag(value) {}
        ~ReplayGuard() {
            flag.store(false, std::memory_order_release);
        }
    } replay_guard(impl_->in_replay);

    output.checkpoint_frame_index =
        checkpoint_metadata.checkpoint_frame_index;
    output.first_frame_index =
        input_metadata.first_frame_index;
    output.last_frame_index =
        input_metadata.last_frame_index;
    detail::InputLogRecordCursor cursor;
    for (std::size_t index = 0;
         index < input_metadata.record_count;
         ++index) {
        detail::InputLogRecordView record;
        if (!detail::next_input_log_record(
                input_log,
                input_metadata,
                cursor,
                record)) {
            return impl_->fail(
                Status::internal_error,
                "validated input log could not be traversed");
        }

        CallbackResult callback_result =
            CallbackResult::error;
        try {
            callback_result = input_callback(
                input_user_data,
                ReplayInputView{
                    record.frame,
                    record.input_type,
                    record.payload,
                });
        } catch (...) {
            callback_result = CallbackResult::error;
        }
        if (callback_result != CallbackResult::ok) {
            return impl_->fail(
                Status::callback_failed,
                "replay input callback failed");
        }
        ++output.records_processed;

        Status step_status = Status::internal_error;
        {
            struct ReplayDispatchGuard {
                std::atomic<bool>& flag;
                explicit ReplayDispatchGuard(
                    std::atomic<bool>& value)
                    : flag(value) {
                    flag.store(true, std::memory_order_release);
                }
                ~ReplayDispatchGuard() {
                    flag.store(false, std::memory_order_release);
                }
            } dispatch_guard(impl_->replay_dispatch);
            step_status = step(record.frame);
        }
        if (step_status != Status::ok) {
            return step_status;
        }
        ++output.frames_replayed;
    }
    output.final_state_hash = impl_->state_hash();
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::write_active_replay_artifact(
    std::span<const std::byte> checkpoint,
    std::span<const ReplayInputRecord> records,
    std::span<std::byte> output,
    ArtifactWriteResult& result) noexcept {
    result = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state == RuntimeState::configuring ||
        !impl_->mixed_rate_closure_policy_set ||
        !impl_->mixed_rate_closure_policy.active_replay_enabled ||
        !impl_->mixed_rate_actions) {
        return impl_->fail(
            Status::invalid_state,
            "active replay artifact construction is not enabled");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire) ||
        impl_->stop_pending || impl_->lane_cleanup_pending) {
        return impl_->fail(
            Status::invalid_state,
            "active replay artifact requires quiescent host control");
    }
    if (records.empty() ||
        records.size() >
            impl_->mixed_rate_closure_policy.active_replay_record_capacity) {
        return impl_->fail(
            Status::invalid_argument,
            "active replay input count is outside the frozen bound");
    }
    CheckpointMetadata checkpoint_metadata;
    const auto checkpoint_status = detail::parse_checkpoint_artifact(
        checkpoint,
        impl_->config.snapshot_max_bytes,
        impl_->checkpoint_state_count(),
        checkpoint_metadata);
    if (checkpoint_status != Status::ok ||
        checkpoint_metadata.replay_id != impl_->replay_id ||
        checkpoint_metadata.graph_id != impl_->graph_id ||
        checkpoint_metadata.state_schema_id != impl_->state_schema_id ||
        checkpoint_metadata.workload_id != impl_->observability.workload_id) {
        return impl_->fail(
            checkpoint_status == Status::ok
                ? Status::incompatible_artifact
                : checkpoint_status,
            "active replay checkpoint is malformed or foreign");
    }
    detail::CheckpointRecordCursor checkpoint_cursor;
    detail::CheckpointRecordView checkpoint_record;
    for (std::size_t index = 0; index < impl_->states.size(); ++index) {
        if (!detail::next_checkpoint_record(
                checkpoint,
                checkpoint_metadata,
                checkpoint_cursor,
                checkpoint_record)) {
            return impl_->fail(
                Status::invalid_artifact,
                "active replay checkpoint record traversal failed");
        }
    }
    if (impl_->rate_execution_policy_set &&
        !detail::next_checkpoint_record(
            checkpoint,
            checkpoint_metadata,
            checkpoint_cursor,
            checkpoint_record)) {
        return impl_->fail(
            Status::invalid_artifact,
            "active replay checkpoint lacks active rate state");
    }
    if (!detail::next_checkpoint_record(
            checkpoint,
            checkpoint_metadata,
            checkpoint_cursor,
            checkpoint_record) ||
        checkpoint_record.name != kMixedRateStateName ||
        checkpoint_record.schema_version != kMixedRateStateSchema ||
        !impl_->validate_mixed_rate_checkpoint_state(
            checkpoint_record.payload)) {
        return impl_->fail(
            Status::incompatible_artifact,
            "active replay checkpoint lacks compatible mixed-rate state");
    }
    std::uint64_t first_action_sequence = 0;
    (void)load_u64_le(
        checkpoint_record.payload, 32, first_action_sequence);
    const auto next_action_sequence =
        impl_->mixed_rate_actions->next_sequence();
    if (first_action_sequence > next_action_sequence) {
        return impl_->fail(
            Status::invalid_artifact,
            "active replay action range is reversed");
    }
    const auto action_count_u64 =
        next_action_sequence - first_action_sequence;
    if (action_count_u64 == 0 ||
        action_count_u64 >
            impl_->mixed_rate_closure_policy.active_replay_record_capacity ||
        action_count_u64 > std::numeric_limits<std::size_t>::max() ||
        impl_->mixed_rate_actions->dropped() != 0 ||
        impl_->mixed_rate_actions->overwritten() != 0 ||
        !impl_->mixed_rate_actions->gap_free(
            first_action_sequence, action_count_u64)) {
        return impl_->fail(
            Status::invalid_artifact,
            "active replay requires a complete gap-free action transcript");
    }
    ActiveReplayMetadata metadata;
    metadata.runtime_id = impl_->observability.runtime_id;
    metadata.config_id = impl_->observability.config_id;
    metadata.replay_id = impl_->replay_id;
    metadata.graph_id = impl_->graph_id;
    metadata.state_schema_id = impl_->state_schema_id;
    metadata.host_policy_version =
        impl_->mixed_rate_closure_policy.host_policy_version;
    metadata.nominal_epoch_ns = impl_->active_nominal_epoch_ns;
    metadata.final_state_hash = impl_->state_hash();
    metadata.determinism_tier = impl_->config.determinism_tier;
    metadata.build_id = impl_->observability.build_id;
    metadata.workload_id = impl_->observability.workload_id;
    const auto encode_status = detail::encode_active_replay_artifact(
        metadata,
        checkpoint,
        records,
        first_action_sequence,
        static_cast<std::size_t>(action_count_u64),
        [](void* context,
           std::uint64_t sequence,
           MixedRateActionRecord& record) noexcept {
            return static_cast<detail::MixedRateActionRing*>(context)
                ->read_sequence(sequence, record);
        },
        impl_->mixed_rate_actions.get(),
        impl_->mixed_rate_closure_policy.active_replay_max_bytes,
        output,
        result);
    if (encode_status != Status::ok) {
        return impl_->fail(
            encode_status,
            encode_status == Status::capacity_exceeded
                ? "active replay output is too small or exceeds its bound"
                : "active replay artifact encoding failed");
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::replay_active(
    std::span<const std::byte> artifact,
    ReplayInputCallback input_callback,
    void* input_user_data,
    ActiveReplayResult* result) noexcept {
    ActiveReplayResult local_result;
    auto& output = result ? *result : local_result;
    output = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::running ||
        !impl_->mixed_rate_closure_policy_set ||
        !impl_->mixed_rate_closure_policy.active_replay_enabled ||
        !impl_->mixed_rate_actions || !input_callback) {
        return impl_->fail(
            Status::invalid_state,
            "active replay requires a running closure-enabled runtime and input callback");
    }
    if (impl_->stop_pending || impl_->lane_cleanup_pending ||
        impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "active replay requires non-reentrant quiescent host control");
    }
    for (const auto& state : impl_->states) {
        if (byte_spans_overlap(artifact, std::as_bytes(state.storage))) {
            return impl_->fail(
                Status::invalid_argument,
                "active replay artifact cannot overlap registered state");
        }
    }
    detail::ActiveReplayArtifactView view;
    const auto parse_status = detail::parse_active_replay_artifact(
        artifact,
        impl_->mixed_rate_closure_policy.active_replay_max_bytes,
        view);
    if (parse_status != Status::ok) {
        return impl_->fail(
            parse_status,
            "active replay artifact validation failed before restore");
    }
    if (view.metadata.runtime_id != impl_->observability.runtime_id ||
        view.metadata.config_id != impl_->observability.config_id ||
        view.metadata.replay_id != impl_->replay_id ||
        view.metadata.graph_id != impl_->graph_id ||
        view.metadata.state_schema_id != impl_->state_schema_id ||
        view.metadata.host_policy_version !=
            impl_->mixed_rate_closure_policy.host_policy_version ||
        view.metadata.determinism_tier != impl_->config.determinism_tier ||
        view.metadata.build_id != impl_->observability.build_id ||
        view.metadata.workload_id != impl_->observability.workload_id ||
        view.metadata.input_record_count >
            impl_->mixed_rate_closure_policy.active_replay_record_capacity ||
        view.metadata.action_record_count >
            impl_->mixed_rate_closure_policy.active_replay_record_capacity) {
        return impl_->fail(
            Status::incompatible_artifact,
            "active replay identity does not match the finalized runtime");
    }

    CheckpointMetadata replay_checkpoint_metadata;
    if (detail::parse_checkpoint_artifact(
            view.checkpoint,
            impl_->config.snapshot_max_bytes,
            impl_->checkpoint_state_count(),
            replay_checkpoint_metadata) != Status::ok) {
        return impl_->fail(
            Status::invalid_artifact,
            "active replay checkpoint validation failed");
    }
    detail::CheckpointRecordCursor checkpoint_cursor;
    detail::CheckpointRecordView checkpoint_record;
    for (std::size_t index = 0; index < impl_->states.size(); ++index) {
        if (!detail::next_checkpoint_record(
                view.checkpoint,
                replay_checkpoint_metadata,
                checkpoint_cursor,
                checkpoint_record)) {
            return impl_->fail(
                Status::invalid_artifact,
                "active replay checkpoint record traversal failed");
        }
    }
    if (impl_->rate_execution_policy_set &&
        !detail::next_checkpoint_record(
            view.checkpoint,
            replay_checkpoint_metadata,
            checkpoint_cursor,
            checkpoint_record)) {
        return impl_->fail(
            Status::invalid_artifact,
            "active replay checkpoint lacks active rate state");
    }
    if (!detail::next_checkpoint_record(
            view.checkpoint,
            replay_checkpoint_metadata,
            checkpoint_cursor,
            checkpoint_record) ||
        checkpoint_record.name != kMixedRateStateName ||
        checkpoint_record.schema_version != kMixedRateStateSchema ||
        !impl_->validate_mixed_rate_checkpoint_state(
            checkpoint_record.payload)) {
        return impl_->fail(
            Status::incompatible_artifact,
            "active replay checkpoint lacks compatible mixed-rate state");
    }
    std::uint64_t checkpoint_action_sequence = 0;
    if (!load_u64_le(
            checkpoint_record.payload,
            32,
            checkpoint_action_sequence) ||
        checkpoint_action_sequence !=
            view.metadata.first_action_sequence) {
        return impl_->fail(
            Status::incompatible_artifact,
            "active replay checkpoint/action position does not match");
    }

    Status restore_status = Status::internal_error;
    {
        struct ActiveReplayRestorePermit {
            bool& allowed;
            explicit ActiveReplayRestorePermit(bool& value) : allowed(value) {
                allowed = true;
            }
            ~ActiveReplayRestorePermit() { allowed = false; }
        } restore_permit(impl_->active_replay_restore_allowed);
        restore_status = restore_checkpoint(view.checkpoint, nullptr);
    }
    if (restore_status != Status::ok) {
        return restore_status;
    }
    if (impl_->mixed_rate_actions->next_sequence() !=
        view.metadata.first_action_sequence) {
        return impl_->fail(
            Status::incompatible_artifact,
            "active replay checkpoint/action position does not match");
    }

    bool expected = false;
    if (!impl_->in_replay.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return impl_->fail(Status::invalid_state, "active replay is already active");
    }
    struct ActiveReplayGuard {
        Impl& impl;
        explicit ActiveReplayGuard(Impl& value) : impl(value) {}
        ~ActiveReplayGuard() {
            impl.active_replay_view = nullptr;
            impl.in_replay.store(false, std::memory_order_release);
        }
    } replay_guard(*impl_);
    impl_->active_replay_view = &view;
    impl_->active_replay_expected_action = 0;
    impl_->active_replay_actions_compared = 0;
    impl_->active_replay_mismatch_sequence = 0;
    impl_->active_replay_mismatch_status = Status::ok;
    output.replay.checkpoint_frame_index =
        view.metadata.checkpoint_frame_index;
    output.replay.first_frame_index = view.metadata.first_frame_index;
    output.replay.last_frame_index = view.metadata.last_frame_index;

    for (std::size_t index = 0;
         index < view.metadata.input_record_count; ++index) {
        ReplayInputView input;
        if (!detail::active_replay_input_at(view, index, input)) {
            return impl_->fail(
                Status::internal_error,
                "validated active replay input traversal failed");
        }
        CallbackResult callback_result = CallbackResult::error;
        try {
            callback_result = input_callback(input_user_data, input);
        } catch (...) {
            callback_result = CallbackResult::error;
        }
        if (callback_result != CallbackResult::ok) {
            output.actions_compared = impl_->active_replay_actions_compared;
            return impl_->fail(
                Status::callback_failed,
                "active replay input callback failed");
        }
        ++output.replay.records_processed;
        Status step_status = Status::internal_error;
        {
            struct ReplayDispatchGuard {
                std::atomic<bool>& flag;
                explicit ReplayDispatchGuard(std::atomic<bool>& value)
                    : flag(value) {
                    flag.store(true, std::memory_order_release);
                }
                ~ReplayDispatchGuard() {
                    flag.store(false, std::memory_order_release);
                }
            } dispatch_guard(impl_->replay_dispatch);
            step_status = step(input.frame);
        }
        output.actions_compared = impl_->active_replay_actions_compared;
        if (impl_->active_replay_mismatch_status != Status::ok) {
            output.mismatch_sequence =
                impl_->active_replay_mismatch_sequence;
            output.mismatch_status =
                impl_->active_replay_mismatch_status;
            return impl_->fail(
                Status::incompatible_artifact,
                "active replay action mismatch");
        }
        if (step_status != Status::ok) {
            return step_status;
        }
        ++output.replay.frames_replayed;
    }
    if (impl_->active_replay_expected_action !=
            view.metadata.action_record_count) {
        output.mismatch_sequence =
            view.metadata.first_action_sequence +
            impl_->active_replay_expected_action;
        output.mismatch_status = Status::incompatible_artifact;
        return impl_->fail(
            Status::incompatible_artifact,
            "active replay transcript has unconsumed actions");
    }
    output.replay.final_state_hash = impl_->state_hash();
    if (output.replay.final_state_hash != view.metadata.final_state_hash) {
        output.mismatch_sequence = view.metadata.last_action_sequence;
        output.mismatch_status = Status::incompatible_artifact;
        return impl_->fail(
            Status::incompatible_artifact,
            "active replay final registered state does not match");
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::write_live_control_replay_artifact(
    std::span<const std::byte> checkpoint,
    std::span<const std::byte> nested_artifact,
    LiveControlNestedArtifactKind nested_kind,
    std::span<std::byte> output,
    ArtifactWriteResult& result) noexcept {
    result = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state == RuntimeState::configuring ||
        !live_control_closure_enabled() ||
        !impl_->live_control_closure_policy.replay_enabled) {
        return impl_->fail(
            Status::invalid_state,
            "live-control replay artifact construction is not enabled");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire) ||
        impl_->stop_pending || impl_->lane_cleanup_pending) {
        return impl_->fail(
            Status::invalid_state,
            "live-control replay artifact requires quiescent host control");
    }
    CheckpointMetadata checkpoint_metadata;
    const auto checkpoint_status = detail::parse_checkpoint_artifact(
        checkpoint,
        impl_->config.snapshot_max_bytes,
        impl_->checkpoint_state_count(),
        checkpoint_metadata);
    if (checkpoint_status != Status::ok ||
        checkpoint_metadata.replay_id != impl_->replay_id ||
        checkpoint_metadata.graph_id != impl_->graph_id ||
        checkpoint_metadata.state_schema_id != impl_->state_schema_id ||
        checkpoint_metadata.workload_id != impl_->observability.workload_id) {
        return impl_->fail(
            checkpoint_status == Status::ok
                ? Status::incompatible_artifact
                : checkpoint_status,
            "live-control replay checkpoint is malformed or foreign");
    }
    if (nested_kind == LiveControlNestedArtifactKind::input_log) {
        InputLogMetadata nested_metadata;
        if (inspect_input_log_artifact(
                nested_artifact, nested_metadata) != Status::ok ||
            impl_->rate_execution_policy_set ||
            nested_metadata.replay_id != impl_->replay_id ||
            nested_metadata.state_schema_id != impl_->state_schema_id ||
            nested_metadata.workload_id != impl_->observability.workload_id) {
            return impl_->fail(
                Status::incompatible_artifact,
                "nested input log is malformed or incompatible");
        }
    } else if (nested_kind ==
                   LiveControlNestedArtifactKind::active_replay) {
        ActiveReplayMetadata nested_metadata;
        detail::ActiveReplayArtifactView nested_view;
        if (inspect_active_replay_artifact(
                nested_artifact, nested_metadata) != Status::ok ||
            detail::parse_active_replay_artifact(
                nested_artifact,
                impl_->mixed_rate_closure_policy.active_replay_max_bytes,
                nested_view) != Status::ok ||
            nested_view.checkpoint.size() != checkpoint.size() ||
            !std::equal(
                nested_view.checkpoint.begin(),
                nested_view.checkpoint.end(),
                checkpoint.begin()) ||
            !impl_->mixed_rate_closure_policy_set ||
            !impl_->mixed_rate_closure_policy.active_replay_enabled ||
            nested_metadata.replay_id != impl_->replay_id ||
            nested_metadata.graph_id != impl_->graph_id ||
            nested_metadata.state_schema_id != impl_->state_schema_id ||
            nested_metadata.workload_id != impl_->observability.workload_id) {
            return impl_->fail(
                Status::incompatible_artifact,
                "nested active replay artifact is malformed or incompatible");
        }
    } else {
        return impl_->fail(
            Status::invalid_argument,
            "live-control nested artifact kind is unsupported");
    }
    detail::CheckpointRecordCursor cursor;
    detail::CheckpointRecordView record;
    bool found_live_control = false;
    for (std::size_t index = 0;
         index < checkpoint_metadata.state_count;
         ++index) {
        if (!detail::next_checkpoint_record(
                checkpoint,
                checkpoint_metadata,
                cursor,
                record)) {
            return impl_->fail(
                Status::invalid_artifact,
                "live-control checkpoint record traversal failed");
        }
        if (record.name == kLiveControlStateName) {
            found_live_control = true;
            break;
        }
    }
    std::uint64_t first_action_sequence = 0;
    if (!found_live_control ||
        record.schema_version != kLiveControlStateSchema ||
        !impl_->live_control_mailboxes->checkpoint_action_sequence(
            record.payload, first_action_sequence)) {
        return impl_->fail(
            Status::incompatible_artifact,
            "checkpoint lacks compatible live-control correlation state");
    }
    if (!impl_->live_control_mailboxes->claim_all()) {
        return impl_->fail(
            Status::invalid_state,
            "live-control replay artifact claim is busy; retry");
    }
    struct ClaimGuard {
        detail::LiveControlMailboxSet* mailboxes;
        ~ClaimGuard() { mailboxes->release_all(); }
    } claim{impl_->live_control_mailboxes.get()};
    LiveControlReplayMetadata metadata;
    metadata.runtime_id = impl_->live_control_mailboxes->runtime_id();
    metadata.configuration_generation =
        impl_->live_control_mailboxes->configuration_generation();
    metadata.config_id = impl_->observability.config_id;
    metadata.replay_id = impl_->replay_id;
    metadata.graph_id = impl_->graph_id;
    metadata.state_schema_id = impl_->state_schema_id;
    metadata.policy_identity =
        impl_->live_control_closure_policy.policy_identity;
    metadata.schema_version = impl_->live_control_retention_policy.policy_identity != 0
                                  ? live_control_lossless_replay_schema_version
                                  : live_control_replay_schema_version;
    metadata.final_state_hash =
        metadata.schema_version == live_control_lossless_replay_schema_version
            ? impl_->state_hash()
            : impl_->registered_application_state_hash();
    metadata.nested_kind = nested_kind;
    metadata.determinism_tier = impl_->config.determinism_tier;
    metadata.build_id = impl_->observability.build_id;
    metadata.workload_id = impl_->observability.workload_id;
    const auto status = impl_->live_control_mailboxes->write_replay_artifact(
        metadata,
        checkpoint,
        nested_artifact,
        first_action_sequence,
        output,
        result);
    if (status != Status::ok) {
        return impl_->fail(
            status,
            status == Status::capacity_exceeded
                ? "live-control replay output is too small or exceeds its bound"
                : "live-control replay transcript is incomplete or invalid");
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::replay_live_control(
    std::span<const std::byte> artifact,
    ReplayInputCallback input_callback,
    void* input_user_data,
    LiveControlReplayResult* result) noexcept {
    LiveControlReplayResult local_result;
    auto& output = result ? *result : local_result;
    output = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state != RuntimeState::running ||
        !live_control_closure_enabled() ||
        !impl_->live_control_closure_policy.replay_enabled) {
        return impl_->fail(
            Status::invalid_state,
            "live-control replay requires a running replay-enabled runtime");
    }
    if (impl_->stop_pending || impl_->lane_cleanup_pending ||
        impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "live-control replay requires non-reentrant quiescent host control");
    }
    detail::LiveControlReplayArtifactView view;
    const auto parse_status = detail::parse_live_control_replay_artifact(
        artifact,
        impl_->live_control_closure_policy.replay_max_bytes,
        view);
    if (parse_status != Status::ok) {
        return impl_->fail(
            parse_status,
            "live-control replay artifact validation failed before restore");
    }
    if (view.metadata.config_id != impl_->observability.config_id ||
        view.metadata.replay_id != impl_->replay_id ||
        view.metadata.graph_id != impl_->graph_id ||
        view.metadata.state_schema_id != impl_->state_schema_id ||
        view.metadata.policy_identity !=
            impl_->live_control_closure_policy.policy_identity ||
        view.metadata.determinism_tier != impl_->config.determinism_tier ||
        view.metadata.build_id != impl_->observability.build_id ||
        view.metadata.workload_id != impl_->observability.workload_id ||
        view.metadata.action_record_count >
            impl_->live_control_closure_policy.replay_record_capacity ||
        view.metadata.retained_generation_count >
            impl_->live_control_closure_policy.retained_generation_capacity ||
        view.metadata.retained_record_count >
            impl_->live_control_closure_policy.retained_record_capacity ||
        view.metadata.retained_payload_bytes >
            impl_->live_control_closure_policy.retained_payload_bytes) {
        return impl_->fail(
            Status::incompatible_artifact,
            "live-control replay identity or frozen bounds do not match");
    }
    for (const auto& state : impl_->states) {
        if (byte_spans_overlap(artifact, std::as_bytes(state.storage))) {
            return impl_->fail(
                Status::invalid_argument,
                "live-control replay artifact cannot overlap registered state");
        }
    }
    if (!impl_->live_control_mailboxes->claim_all()) {
        return impl_->fail(
            Status::invalid_state,
            "live-control replay claim is busy; retry");
    }
    struct ReplayMailboxGuard {
        detail::LiveControlMailboxSet& mailboxes;
        bool replay_begun = false;
        ~ReplayMailboxGuard() {
            if (replay_begun) {
                mailboxes.end_replay();
            }
            mailboxes.release_all();
        }
    } mailbox_guard{*impl_->live_control_mailboxes};

    if (!impl_->live_control_mailboxes->validate_replay_artifact(view)) {
        return impl_->fail(
            Status::incompatible_artifact,
            "live-control replay topology or checkpoint state is incompatible");
    }
    Status nested_status = Status::ok;
    if (view.metadata.nested_kind ==
        LiveControlNestedArtifactKind::input_log) {
        CheckpointMetadata checkpoint_metadata;
        InputLogMetadata input_metadata;
        if (detail::parse_checkpoint_artifact(
                view.checkpoint,
                impl_->config.snapshot_max_bytes,
                impl_->checkpoint_state_count(),
                checkpoint_metadata) != Status::ok ||
            detail::parse_input_log_artifact(
                view.nested_artifact,
                impl_->config.input_log_max_bytes,
                impl_->config.replay_input_capacity,
                input_metadata) != Status::ok ||
            (input_metadata.record_count != 0 && !input_callback)) {
            return impl_->fail(
                Status::invalid_artifact,
                "nested ordinary replay artifacts are incompatible");
        }
        if (input_metadata.runtime_version_major != version_major ||
            input_metadata.determinism_tier != impl_->config.determinism_tier ||
            input_metadata.replay_id != impl_->replay_id ||
            input_metadata.state_schema_id != impl_->state_schema_id ||
            input_metadata.workload_id != impl_->observability.workload_id ||
            (input_metadata.record_count != 0 &&
             input_metadata.first_frame_index <=
                 checkpoint_metadata.checkpoint_frame_index)) {
            return impl_->fail(
                Status::incompatible_artifact,
                "nested input identity or checkpoint ordering is incompatible");
        }
        const auto restore_status = restore_checkpoint(view.checkpoint, nullptr);
        if (restore_status != Status::ok) {
            return restore_status;
        }
        if (!impl_->live_control_mailboxes->begin_replay(view)) {
            return impl_->fail(
                Status::incompatible_artifact,
                "live-control replay state exceeds finalized storage");
        }
        mailbox_guard.replay_begun = true;
        impl_->live_control_mailboxes->apply_replay_history();
        if (impl_->live_control_mailboxes->replay_mismatch_status() !=
                Status::ok) {
            return impl_->fail(
                Status::incompatible_artifact,
                "validated live-control admission history is incompatible");
        }
        bool expected = false;
        if (!impl_->in_replay.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return impl_->fail(Status::invalid_state, "replay is already active");
        }
        struct InReplayGuard {
            std::atomic<bool>& flag;
            ~InReplayGuard() { flag.store(false, std::memory_order_release); }
        } in_replay_guard{impl_->in_replay};
        output.checkpoint_frame_index =
            checkpoint_metadata.checkpoint_frame_index;
        output.first_frame_index = input_metadata.first_frame_index;
        output.last_frame_index = input_metadata.last_frame_index;
        detail::InputLogRecordCursor cursor;
        for (std::size_t index = 0; index < input_metadata.record_count;
             ++index) {
            detail::InputLogRecordView record;
            if (!detail::next_input_log_record(
                    view.nested_artifact,
                    input_metadata,
                    cursor,
                    record)) {
                nested_status = Status::invalid_artifact;
                break;
            }
            CallbackResult callback_result = CallbackResult::error;
            try {
                callback_result = input_callback(
                    input_user_data,
                    ReplayInputView{
                        record.frame, record.input_type, record.payload});
            } catch (...) {
                callback_result = CallbackResult::error;
            }
            if (callback_result != CallbackResult::ok) {
                nested_status = Status::callback_failed;
                break;
            }
            struct DispatchGuard {
                std::atomic<bool>& flag;
                explicit DispatchGuard(std::atomic<bool>& value) : flag(value) {
                    flag.store(true, std::memory_order_release);
                }
                ~DispatchGuard() { flag.store(false, std::memory_order_release); }
            } dispatch_guard(impl_->replay_dispatch);
            nested_status = step(record.frame);
            if (nested_status != Status::ok) {
                output.mismatch_frame_index = record.frame.frame_index;
                break;
            }
            ++output.frames_replayed;
        }
    } else {
        detail::ActiveReplayArtifactView nested_view;
        if (!input_callback ||
            detail::parse_active_replay_artifact(
                view.nested_artifact,
                impl_->mixed_rate_closure_policy.active_replay_max_bytes,
                nested_view) != Status::ok ||
            nested_view.checkpoint.size() != view.checkpoint.size() ||
            !std::equal(
                nested_view.checkpoint.begin(),
                nested_view.checkpoint.end(),
                view.checkpoint.begin())) {
            return impl_->fail(
                Status::incompatible_artifact,
                "nested active replay checkpoint is not the outer checkpoint");
        }
        if (!impl_->live_control_mailboxes->begin_replay(view)) {
            return impl_->fail(
                Status::incompatible_artifact,
                "live-control replay state exceeds finalized storage");
        }
        mailbox_guard.replay_begun = true;
        ActiveReplayResult active_result;
        nested_status = replay_active(
            view.nested_artifact,
            input_callback,
            input_user_data,
            &active_result);
        output.checkpoint_frame_index =
            active_result.replay.checkpoint_frame_index;
        output.first_frame_index = active_result.replay.first_frame_index;
        output.last_frame_index = active_result.replay.last_frame_index;
        output.frames_replayed = active_result.replay.frames_replayed;
        output.mismatch_action_sequence = active_result.mismatch_sequence;
    }
    if (mailbox_guard.replay_begun) {
        impl_->live_control_mailboxes->end_replay();
        mailbox_guard.replay_begun = false;
    }
    output.actions_compared = view.metadata.action_record_count;
    output.generations_compared =
        impl_->live_control_mailboxes->replay_generations_compared();
    const auto live_mismatch =
        impl_->live_control_mailboxes->replay_mismatch_status();
    const auto live_mismatch_sequence =
        impl_->live_control_mailboxes->replay_mismatch_action_sequence();
    if (live_mismatch_sequence != 0) {
        output.mismatch_action_sequence = live_mismatch_sequence;
        if (live_mismatch_sequence >=
                view.metadata.first_action_sequence &&
            live_mismatch_sequence <=
                view.metadata.last_action_sequence) {
            LiveControlActionRecord action;
            const auto index = static_cast<std::size_t>(
                live_mismatch_sequence -
                view.metadata.first_action_sequence);
            if (detail::live_control_replay_action_at(view, index, action)) {
                output.mismatch_target = action.target;
                output.mismatch_generation_identity =
                    action.generation_identity;
            }
        }
    }
    if (live_mismatch != Status::ok) {
        nested_status = live_mismatch;
    }
    output.final_state_hash =
        view.metadata.schema_version == live_control_lossless_replay_schema_version
            ? impl_->state_hash()
            : impl_->registered_application_state_hash();
    if (live_mismatch == Status::ok &&
        (view.metadata.schema_version == live_control_lossless_replay_schema_version ||
         view.metadata.final_state_hash != 0) &&
        output.final_state_hash != view.metadata.final_state_hash) {
        nested_status = Status::invalid_artifact;
    }
    if (nested_status != Status::ok &&
        output.mismatch_generation_identity == 0 &&
        view.metadata.retained_generation_count != 0 &&
        (output.generations_compared != 0 ||
         live_mismatch != Status::ok)) {
        const auto index = std::min(
            output.generations_compared,
            static_cast<std::size_t>(
                view.metadata.retained_generation_count - 1));
        detail::LiveControlRetainedGenerationView generation;
        if (detail::live_control_replay_generation_at(
                view, index, generation)) {
            output.mismatch_target = generation.target;
            output.mismatch_generation_identity =
                generation.generation_identity;
        }
    }
    if (nested_status != Status::ok &&
        output.mismatch_target.kind ==
            LiveControlTargetKind::host_frame &&
        output.mismatch_target.frame_index !=
            std::numeric_limits<std::uint64_t>::max()) {
        output.mismatch_frame_index =
            output.mismatch_target.frame_index;
    }
    output.mismatch_status = nested_status;
    if (nested_status != Status::ok) {
        return impl_->fail(
            nested_status,
            "live-control replay diverged from the validated transcript");
    }
    impl_->live_control_mailboxes->record_replay_verified(
        view.metadata.artifact_checksum);
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::registered_state_hash(
    std::uint64_t& hash) noexcept {
    hash = 0;
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->provider_callback_active()) {
        return Status::invalid_state;
    }
    if (impl_->state == RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "registered-state hashing requires a finalized runtime");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "registered-state hashing cannot run during execution");
    }
    hash = impl_->state_hash();
    impl_->clear_error();
    return Status::ok;
}

std::size_t Runtime::trace_event_count() const noexcept {
    if (!impl_ || impl_->provider_callback_active() || !impl_->telemetry) {
        return 0;
    }
    return impl_->telemetry->retained_count();
}

bool Runtime::trace_event(
    std::size_t chronological_index,
    RuntimeTraceEvent& event) const noexcept {
    if (!impl_ || impl_->provider_callback_active() || !impl_->telemetry) {
        return false;
    }
    return impl_->telemetry->event_at(
        chronological_index,
        event);
}

} // namespace rt
