#pragma once

#include "compiled_graph.hpp"
#include "cross_rate_data.hpp"
#include "rate_timeline.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rt::detail {

struct RateAdmissionRecord {
    std::size_t reference_index = 0;
    std::uint64_t declared_start_ns = 0;
    std::uint64_t declared_finish_ns = 0;
    std::uint64_t deadline_ns = 0;
};

struct RateReleaseGroup {
    std::size_t first_reference_index = 0;
    std::size_t reference_count = 0;
    std::size_t domain_registration_index = 0;
    std::uint64_t domain_release_sequence = 0;
    std::uint64_t release_time_ns = 0;
};

struct CompiledRateDispatchPlan {
    RateExecutionPolicy policy{};
    std::vector<RateAdmissionRecord> admission;
    std::vector<RateReleaseGroup> groups;
    // Registration-order domain slices. Each slice contains group indexes in
    // ascending within-supercycle domain release sequence.
    std::array<std::size_t, rate_domain_capacity + 1>
        domain_group_offsets{};
    std::vector<std::size_t> domain_group_indices;
    std::array<std::size_t, rate_domain_capacity>
        records_per_domain_release{};
    // Lower criticality first, then later registration. Recovery traverses
    // this immutable list in reverse.
    std::vector<std::size_t> optional_shed_order;
    // Producer-channel slices by phase index avoid a channel-registry scan for
    // every active reference release.
    std::vector<std::size_t> producer_channel_offsets;
    std::vector<std::size_t> producer_channel_indices;
    // Consumer-channel slices are required for pre-provider CPU-to-device
    // materialization without a registry scan.
    std::vector<std::size_t> consumer_channel_offsets;
    std::vector<std::size_t> consumer_channel_indices;
    // One entry per reference release. Non-device records contain the invalid
    // sentinel. Device dependency slices contain only admitted device
    // prerequisites from the same release group, so runtime dispatch never
    // scans the graph or resource registry.
    std::vector<std::size_t> device_phase_by_reference;
    std::vector<std::size_t> device_dependency_offsets;
    std::vector<std::size_t> device_dependencies;
    std::size_t maximum_device_records_per_group = 0;
};

struct RateDispatchDiagnostic {
    Status status = Status::ok;
    const char* message = nullptr;
    std::size_t reference_index = invalid_reference_release_index;
};

struct RateDueCounts {
    std::uint64_t domain_releases = 0;
    std::uint64_t reference_records = 0;
};

struct DeviceRateBackendSource {
    DeviceBackendHandle backend{};
    HalV2CommandTimelineCapabilities capabilities{};
    std::uint64_t completion_timestamp_domain_identity = 0;
    bool completion_timestamp_domain_valid = false;
    bool deterministic_mock = false;
};

struct DeviceRateBufferSource {
    DeviceBufferHandle buffer{};
    DeviceBackendHandle backend{};
    std::uint64_t bytes = 0;
    std::uint32_t access = 0;
    std::uint64_t byte_granularity = 1;
    std::uint64_t offset_granularity = 1;
};

struct DeviceRateTimelineSource {
    DeviceTimelineHandle timeline{};
    DeviceBackendHandle backend{};
    std::uint64_t initial_value = 0;
};

struct DeviceRatePhaseSource {
    PhaseHandle phase{};
    DeviceBackendHandle backend{};
    const DeviceCommandBatch* declaration = nullptr;
};

struct CompiledDeviceRatePlan {
    std::vector<CompiledDeviceRatePhase> phases;
    std::vector<CompiledDeviceRateCommand> commands;
    std::vector<CompiledDeviceRatePayloadReference> payload_references;
    std::vector<CompiledDeviceRateTimelineReference> timeline_references;
    DeviceRateAdmissionReport report{};
    std::vector<DeviceRateAdmissionBackend> admission_backends;
    std::vector<DeviceRateAdmissionPhase> admission_phases;
    std::vector<DeviceRateAdmissionInterval> admission_intervals;
};

struct DeviceRateDiagnostic {
    Status status = Status::ok;
    const char* message = nullptr;
    std::size_t reference_index = invalid_reference_release_index;
    PhaseHandle phase{};
};

// Compiles copied-declaration inspection and conservative cyclic admission.
// Input pointers are used only during this call; output owns no caller span or
// provider/vendor pointer and is published only on complete success.
[[nodiscard]] Status compile_device_rate_plan(
    std::uint32_t graph_owner,
    std::uint32_t runtime_outstanding_capacity,
    std::uint32_t runtime_completion_batch_capacity,
    const CompiledRatePlan& rate_plan,
    std::span<const DeviceRateBindingSpec> bindings,
    std::span<const DeviceRateBackendSource> backends,
    std::span<const DeviceRateBufferSource> buffers,
    std::span<const DeviceRateTimelineSource> timelines,
    std::span<const DeviceRatePhaseSource> phases,
    CompiledDeviceRatePlan& output,
    DeviceRateDiagnostic& diagnostic) noexcept;

// Active-only validation and conservative single-lane declared-budget
// admission. Output is published only after the complete simulation succeeds.
[[nodiscard]] Status compile_rate_dispatch(
    std::uint32_t graph_owner,
    DeterminismTier determinism_tier,
    const RateExecutionPolicy& policy,
    std::span<const GraphDependency> dependencies,
    const CompiledRatePlan& rate_plan,
    std::span<const CrossRateChannelSpec> channels,
    const CompiledDeviceRatePlan* device_rate_plan,
    CompiledRateDispatchPlan& output,
    RateDispatchDiagnostic& diagnostic) noexcept;

// Counts the half-open logical interval without iterating through repeated
// supercycles. All arithmetic is checked and the output is transactional.
[[nodiscard]] Status count_due_rate_work(
    const CompiledRatePlan& rate_plan,
    const CompiledRateDispatchPlan& dispatch_plan,
    std::uint64_t cursor_ns,
    std::uint64_t end_ns,
    RateDueCounts& output) noexcept;

[[nodiscard]] Status count_due_domain_releases(
    const CompiledRatePlan& rate_plan,
    const CompiledRateDispatchPlan& dispatch_plan,
    std::size_t domain_index,
    std::uint64_t cursor_ns,
    std::uint64_t end_ns,
    std::uint64_t& domain_releases,
    std::uint64_t& reference_records) noexcept;

} // namespace rt::detail
