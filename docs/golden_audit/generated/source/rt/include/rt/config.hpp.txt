#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <rt/status.hpp>

namespace rt {

inline constexpr std::uint32_t runtime_config_schema_version = 7;
inline constexpr std::size_t observability_identifier_capacity = 64;
inline constexpr std::size_t cpu_set_capacity = 256;
inline constexpr std::size_t thread_name_capacity = 32;
inline constexpr std::size_t thread_policy_request_capacity = 16;
inline constexpr std::size_t memory_policy_request_capacity = 16;
inline constexpr std::size_t resource_accounting_declaration_capacity = 32;
inline constexpr std::uint32_t memory_provider_api_version = 1;
inline constexpr std::size_t rate_domain_capacity = 64;
inline constexpr std::size_t rate_domain_name_capacity = 64;
inline constexpr std::uint32_t rate_domain_substep_capacity = 64;
inline constexpr std::size_t reference_release_capacity = 65'536;
inline constexpr std::size_t cross_rate_channel_capacity = 256;
inline constexpr std::size_t cross_rate_channel_name_capacity = 64;
inline constexpr std::size_t cross_rate_payload_capacity = 64 * 1024;
inline constexpr std::size_t cross_rate_initial_bytes_capacity = 1024 * 1024;
inline constexpr std::size_t cross_rate_selection_capacity = 262'144;
inline constexpr std::size_t cross_rate_snapshot_slot_count = 2;
inline constexpr std::size_t cross_rate_snapshot_bytes_capacity =
    2 * 1024 * 1024;

enum class RateCriticality : std::uint8_t {
    background,
    normal,
    critical,
};

enum class NumericalMode : std::uint8_t {
    precise,
    fused_multiply_add,
};

enum class ExecutorPolicy : std::uint8_t {
    static_deterministic,
    bounded_throughput,
    host_adapter,
};

enum class OverloadPolicy : std::uint8_t {
    reject_submission,
    fail_frame,
};

enum class PlatformPreflightMode : std::uint8_t {
    disabled,
    strict,
};

enum class DeterminismTier : std::uint8_t {
    unspecified = 0,
    schedule_independent = 1,
    reproducible_build = 2,
    portable_deterministic = 3,
};

// Stable, source-level role identifiers. Values at or above custom_first are
// reserved for additive accelerator/service roles. A custom role remains
// externally owned and verify-only until a later runtime version recognizes
// and owns it.
struct ThreadRoleId {
    std::uint32_t value = 0;

    [[nodiscard]] constexpr bool operator==(
        const ThreadRoleId&) const noexcept = default;
};

inline constexpr ThreadRoleId thread_role_frame{1};
inline constexpr ThreadRoleId thread_role_executor_worker{2};
inline constexpr ThreadRoleId thread_role_watchdog{3};
inline constexpr ThreadRoleId thread_role_device_service{4};
inline constexpr ThreadRoleId thread_role_xdma_io{5};
inline constexpr ThreadRoleId thread_role_device_submission{6};
inline constexpr std::uint32_t thread_role_custom_first = 0x0001'0000u;

[[nodiscard]] constexpr std::uint64_t thread_resource_accounting_key(
    ThreadRoleId role) noexcept {
    return (std::uint64_t{1} << 32u) |
        static_cast<std::uint64_t>(role.value);
}

// Stable memory accounting identities. Each resolved report contains exactly
// one row for every identifier below, including zero-cardinality categories.
struct MemoryRegionId {
    std::uint32_t value = 0;

    [[nodiscard]] constexpr bool operator==(
        const MemoryRegionId&) const noexcept = default;
};

inline constexpr MemoryRegionId memory_region_runtime_control{1};
inline constexpr MemoryRegionId memory_region_executor_control{2};
inline constexpr MemoryRegionId memory_region_device_control{3};
inline constexpr MemoryRegionId memory_region_phase_scratch{4};
inline constexpr MemoryRegionId memory_region_task_scratch{5};
inline constexpr MemoryRegionId memory_region_trace_storage{6};
inline constexpr MemoryRegionId memory_region_registered_state{7};
inline constexpr MemoryRegionId memory_region_backend_control{8};
inline constexpr MemoryRegionId memory_region_registered_device_buffer{9};
inline constexpr MemoryRegionId memory_region_runtime_thread_stack{10};
inline constexpr MemoryRegionId memory_region_external_thread_stack{11};
inline constexpr MemoryRegionId memory_region_host_provider{12};

[[nodiscard]] constexpr std::uint64_t memory_resource_accounting_key(
    MemoryRegionId region) noexcept {
    return (std::uint64_t{2} << 32u) |
        static_cast<std::uint64_t>(region.value);
}

enum class PolicyRequirement : std::uint8_t {
    best_effort,
    strict,
};

enum class SchedulingClass : std::uint8_t {
    inherit,
    normal,
    fifo,
    round_robin,
};

enum class WaitStrategy : std::uint8_t {
    inherit,
    spin,
    yield,
    park,
};

struct CpuSetRequest {
    std::size_t count = 0;
    std::array<std::uint32_t, cpu_set_capacity> cpu_ids{};
};

struct ThreadPolicy {
    PolicyRequirement requirement = PolicyRequirement::best_effort;
    CpuSetRequest cpu_set{};
    SchedulingClass scheduling_class = SchedulingClass::inherit;
    std::int32_t scheduling_priority = 0;
    // -1 inherits the current placement. Nonnegative values name a NUMA node.
    std::int32_t numa_node = -1;
    WaitStrategy wait_strategy = WaitStrategy::inherit;
    // Zero retains the implementation/host default.
    std::size_t stack_bytes = 0;
    std::size_t guard_bytes = 0;
    // Empty means unnamed/inherit. Nonempty values are NUL-terminated and use
    // [A-Za-z0-9._-].
    std::array<char, thread_name_capacity> name{};
};

struct ThreadPolicyRequest {
    ThreadRoleId role{};
    ThreadPolicy policy{};
};

enum class PolicyToggle : std::uint8_t {
    inherit,
    disabled,
    enabled,
};

enum class MemoryProviderOwnership : std::uint8_t {
    inherit,
    runtime,
    host,
    backend,
    borrowed,
};

enum class PageRounding : std::uint8_t {
    inherit,
    none,
    base_page,
};

enum class HugePagePreference : std::uint8_t {
    inherit,
    disabled,
    prefer,
};

enum class FirstTouchPolicy : std::uint8_t {
    inherit,
    none,
    caller,
    owner_thread,
};

enum class RollbackIntent : std::uint8_t {
    inherit,
    none,
    release,
};

struct MemoryPolicy {
    PolicyRequirement requirement = PolicyRequirement::best_effort;
    MemoryProviderOwnership provider = MemoryProviderOwnership::inherit;
    // Zero retains the category's current alignment.
    std::size_t alignment = 0;
    PageRounding page_rounding = PageRounding::inherit;
    std::size_t guard_bytes_before = 0;
    std::size_t guard_bytes_after = 0;
    PolicyToggle prefault = PolicyToggle::inherit;
    PolicyToggle locking = PolicyToggle::inherit;
    PolicyToggle pinning = PolicyToggle::inherit;
    HugePagePreference huge_pages = HugePagePreference::inherit;
    PolicyToggle huge_page_fallback = PolicyToggle::inherit;
    // -1 inherits the current placement. Nonnegative values name a NUMA node.
    std::int32_t numa_node = -1;
    FirstTouchPolicy first_touch = FirstTouchPolicy::inherit;
    PolicyToggle residency_verification = PolicyToggle::inherit;
    RollbackIntent rollback = RollbackIntent::inherit;
};

struct MemoryPolicyRequest {
    MemoryRegionId region{};
    MemoryPolicy policy{};
};

// Trusted configuring-time accounting metadata for storage the runtime does
// not own and cannot inspect. The key must identify one of the stable thread
// or memory rows above. A declaration reports logical bytes only: it never
// authorizes mutation or establishes residency, locking, pinning, or hardware
// qualification.
struct ResourceAccountingDeclaration {
    std::uint64_t accounting_key = 0;
    std::size_t logical_region_count = 0;
    std::size_t accounted_bytes = 0;
};

enum class MemoryProviderCapability : std::uint64_t {
    none = 0,
    guard_pages = std::uint64_t{1} << 0,
    explicit_huge_pages = std::uint64_t{1} << 1,
    policy_operations = std::uint64_t{1} << 2,
    independent_observation = std::uint64_t{1} << 3,
    pinning = std::uint64_t{1} << 4,
    numa_binding = std::uint64_t{1} << 5,
};

[[nodiscard]] constexpr std::uint64_t memory_provider_capability_bit(
    MemoryProviderCapability capability) noexcept {
    return static_cast<std::uint64_t>(capability);
}

struct MemoryProviderAcquireRequest {
    MemoryRegionId region{};
    std::size_t logical_bytes = 0;
    std::size_t required_alignment = 0;
    PageRounding page_rounding = PageRounding::none;
    std::size_t guard_bytes_before = 0;
    std::size_t guard_bytes_after = 0;
    HugePagePreference huge_pages = HugePagePreference::disabled;
    PolicyToggle huge_page_fallback = PolicyToggle::disabled;
    std::int32_t numa_node = -1;
    RollbackIntent rollback = RollbackIntent::release;
};

struct MemoryProviderAllocation {
    void* token = nullptr;
    std::byte* allocation_base = nullptr;
    std::size_t allocation_bytes = 0;
    std::byte* usable_data = nullptr;
    std::size_t usable_bytes = 0;
    std::size_t committed_bytes = 0;
    std::size_t alignment = 0;
    std::size_t actual_page_bytes = 0;
    std::size_t guard_bytes_before = 0;
    std::size_t guard_bytes_after = 0;
    bool explicit_huge_pages = false;
    bool used_huge_page_fallback = false;
    std::int32_t provider_error = 0;
};

struct MemoryProviderObservation {
    std::size_t resident_bytes = 0;
    std::size_t locked_bytes = 0;
    std::size_t pinned_bytes = 0;
    std::int32_t numa_node = -1;
    bool prefaulted = false;
    bool caller_first_touched = false;
    bool independently_observed = false;
    std::int32_t system_error = 0;
};

using MemoryProviderAcquire = Status (*)(
    void* user_data,
    const MemoryProviderAcquireRequest& request,
    MemoryProviderAllocation& allocation) noexcept;
using MemoryProviderApply = Status (*)(
    void* user_data,
    void* token,
    const MemoryPolicy& resolved,
    MemoryProviderObservation& applied) noexcept;
using MemoryProviderObserve = Status (*)(
    void* user_data,
    void* token,
    const MemoryPolicy& resolved,
    MemoryProviderObservation& observed) noexcept;
// Once apply is invoked for a token, rollback is invoked once for that attempt
// before retry or release, including when apply reports failure. Implementations
// must therefore make rollback safe for a partially applied operation and
// retryable when rollback itself reports failure.
using MemoryProviderRollback = Status (*)(
    void* user_data,
    void* token,
    const MemoryPolicy& resolved,
    const MemoryProviderObservation& applied) noexcept;
using MemoryProviderRelease = void (*)(
    void* user_data,
    void* token,
    RollbackIntent intent) noexcept;

// The runtime copies this bounded callback table while configuring and borrows
// user_data until checked stop (or best-effort destruction) has released every
// acquired token. Provider callbacks must not reenter the Runtime.
struct MemoryProvider {
    std::uint32_t struct_size = sizeof(MemoryProvider);
    std::uint32_t api_version = memory_provider_api_version;
    std::uint64_t capabilities = 0;
    void* user_data = nullptr;
    MemoryProviderAcquire acquire = nullptr;
    MemoryProviderApply apply = nullptr;
    MemoryProviderObserve observe = nullptr;
    MemoryProviderRollback rollback = nullptr;
    MemoryProviderRelease release = nullptr;
    std::array<std::uint64_t, 4> reserved{};
};

// This policy is intentionally separate from RuntimeConfig schema 7. It is a
// C++ source API and is not a JSON/profile or stable-C-ABI field.
struct CpuMemoryPolicy {
    std::size_t thread_policy_count = 0;
    std::array<ThreadPolicyRequest, thread_policy_request_capacity>
        thread_policies{};
    std::size_t memory_policy_count = 0;
    std::array<MemoryPolicyRequest, memory_policy_request_capacity>
        memory_policies{};
    // Appended M15-04 closure fields preserve the pre-M15-04 aggregate prefix.
    PolicyRequirement accounting_requirement =
        PolicyRequirement::best_effort;
    std::size_t accounting_declaration_count = 0;
    std::array<
        ResourceAccountingDeclaration,
        resource_accounting_declaration_capacity>
        accounting_declarations{};
};

struct RuntimeConfig {
    std::size_t callback_capacity = 64;
    std::size_t scratch_bytes = 64 * 1024;
    std::size_t trace_capacity = 1024;
    NumericalMode numerical_mode = NumericalMode::precise;
    ExecutorPolicy executor_policy = ExecutorPolicy::static_deterministic;
    std::size_t worker_count = 1;
    std::size_t executor_queue_capacity = 1024;
    std::size_t scratch_alignment = 64;
    std::size_t task_scratch_bytes = 4 * 1024;
    std::size_t task_scratch_slots = 1024;
    std::size_t memory_budget_bytes = 256 * 1024 * 1024;
    OverloadPolicy overload_policy = OverloadPolicy::reject_submission;
    std::uint64_t watchdog_timeout_ns = 0;
    std::uint32_t watchdog_max_degradation_level = 0;
    PlatformPreflightMode platform_preflight_mode =
        PlatformPreflightMode::disabled;
    // D1 is an explicit application contract: only registered canonical state
    // is compared, and callbacks must obey the restrictions documented in
    // docs/determinism_replay.md. D2 and D3 are reserved and rejected.
    DeterminismTier determinism_tier = DeterminismTier::unspecified;
    std::size_t state_capacity = 64;
    std::size_t snapshot_max_bytes = 1024 * 1024;
    std::size_t replay_input_capacity = 4096;
    std::size_t input_log_max_bytes = 1024 * 1024;
    std::size_t device_backend_capacity = 1;
    std::size_t device_buffer_capacity = 64;
    std::size_t device_outstanding_capacity = 64;
    std::size_t device_completion_batch = 16;
    // Stable caller-supplied provenance copied into every observability
    // snapshot. IDs use [A-Za-z0-9._:/@-] and must be NUL terminated.
    std::array<char, observability_identifier_capacity> workload_id{
        'u', 'n', 's', 'p', 'e', 'c', 'i', 'f', 'i', 'e', 'd', '\0'};
};

// Applies one strict schema key to a typed configuration. The supported keys
// are callback_capacity, scratch_bytes, trace_capacity, numerical_mode,
// executor_policy, worker_count, executor_queue_capacity, scratch_alignment,
// task_scratch_bytes, task_scratch_slots, memory_budget_bytes,
// overload_policy, watchdog_timeout_ns, watchdog_max_degradation_level,
// platform_preflight_mode, determinism_tier, state_capacity,
// snapshot_max_bytes, replay_input_capacity, input_log_max_bytes,
// device_backend_capacity, device_buffer_capacity,
// device_outstanding_capacity, device_completion_batch, and workload_id.
// Unknown keys and partially parsed values are rejected.
[[nodiscard]] Status set_runtime_config_value(
    RuntimeConfig& config,
    std::string_view key,
    std::string_view value) noexcept;

} // namespace rt
