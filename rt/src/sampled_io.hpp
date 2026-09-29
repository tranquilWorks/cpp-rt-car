#pragma once

#include "cross_rate_data.hpp"
#include "rate_dispatch.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace rt::detail {

struct SampledIoChannelSpec {
    CrossRateChannelHandle channel{};
    SampledIoDirection direction = SampledIoDirection::input;
    std::uint64_t channel_identity = 0;
    SampledIoEncoding encoding = SampledIoEncoding::signed_int16_le;
    std::uint32_t element_count = 0;
    std::uint32_t samples_per_frame = 0;
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
    std::vector<std::byte> initial_frame;
    std::vector<std::byte> startup_safe_frame;
    std::vector<std::byte> failure_safe_frame;
    std::vector<std::byte> shutdown_safe_frame;
};

struct CompiledSampledIoRecord {
    CompiledSampledIoChannel public_record{};
    std::size_t cross_rate_channel_index = 0;
    std::size_t initial_offset = 0;
    std::size_t startup_safe_offset = 0;
    std::size_t failure_safe_offset = 0;
    std::size_t shutdown_safe_offset = 0;
};

struct CompiledSampledIoSafePhase {
    PhaseHandle phase{};
    DeviceBackendHandle backend{};
    std::size_t first_channel_index = 0;
    std::size_t channel_count = 0;
    std::size_t reference_index = invalid_reference_release_index;
};

struct CompiledSampledIoPlan {
    std::vector<CompiledSampledIoRecord> channels;
    std::vector<std::size_t> channel_index_by_cross_rate;
    std::vector<std::size_t> safe_channel_indices;
    std::vector<CompiledSampledIoSafePhase> safe_phases;
    std::vector<std::byte> frame_storage;
};

struct SampledIoCompileDiagnostic {
    Status status = Status::ok;
    const char* message = nullptr;
    CrossRateChannelHandle channel{};
};

// Resolve only bounded explicit capacities before snapshot allocation. The
// destination is transactional; ordinary channels retain the default count.
[[nodiscard]] Status sampled_io_snapshot_slots(
    std::uint32_t graph_owner,
    std::span<const SampledIoChannelSpec> specifications,
    std::span<const CrossRateChannelSpec> cross_rate_specs,
    std::span<std::size_t> slot_counts,
    SampledIoCompileDiagnostic& diagnostic) noexcept;

[[nodiscard]] Status compile_sampled_io(
    std::uint32_t graph_owner,
    std::span<const SampledIoChannelSpec> specifications,
    const CompiledRatePlan& rate_plan,
    const CompiledDeviceRatePlan& device_rate_plan,
    std::span<const CrossRateChannelSpec> cross_rate_specs,
    const CompiledCrossRatePlan& cross_rate_plan,
    CompiledSampledIoPlan& output,
    SampledIoCompileDiagnostic& diagnostic) noexcept;

[[nodiscard]] bool sampled_io_frame_valid(
    std::span<const std::byte> frame,
    const CompiledSampledIoChannel& channel,
    SampledIoFrameStatus expected_status,
    std::uint64_t expected_sequence,
    std::uint64_t expected_release_generation,
    bool require_exact_sequence) noexcept;

[[nodiscard]] bool sampled_io_read_header(
    std::span<const std::byte> frame,
    SampledIoFrameHeader& header) noexcept;

} // namespace rt::detail
