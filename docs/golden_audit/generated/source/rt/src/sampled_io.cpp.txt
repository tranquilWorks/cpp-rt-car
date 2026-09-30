#include "sampled_io.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace rt {

std::size_t sampled_io_encoding_bytes(SampledIoEncoding encoding) noexcept {
    switch (encoding) {
    case SampledIoEncoding::signed_int16_le:
    case SampledIoEncoding::unsigned_int16_le:
        return 2;
    case SampledIoEncoding::signed_int32_le:
    case SampledIoEncoding::unsigned_int32_le:
        return 4;
    }
    return 0;
}

std::uint64_t sampled_io_payload_checksum(
    std::span<const std::byte> payload) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto value : payload) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace rt

namespace rt::detail {
namespace {

bool checked_add_size(
    std::size_t left,
    std::size_t right,
    std::size_t& output) noexcept {
    if (left > std::numeric_limits<std::size_t>::max() - right) {
        return false;
    }
    output = left + right;
    return true;
}

bool checked_multiply_size(
    std::size_t left,
    std::size_t right,
    std::size_t& output) noexcept {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    output = left * right;
    return true;
}

bool zero_reserved(const SampledIoFrameHeader& header) noexcept {
    return header.reserved0 == 0 &&
        std::all_of(
            header.reserved.begin(), header.reserved.end(),
            [](std::uint64_t value) { return value == 0; });
}

bool enum_valid(SampledIoDirection value) noexcept {
    return value == SampledIoDirection::input ||
        value == SampledIoDirection::output;
}

bool enum_valid(SampledIoTriggerMode value) noexcept {
    return value == SampledIoTriggerMode::periodic ||
        value == SampledIoTriggerMode::software ||
        value == SampledIoTriggerMode::external;
}

bool enum_valid(SampledIoStalePolicy value) noexcept {
    return value == SampledIoStalePolicy::fail_release ||
        value == SampledIoStalePolicy::hold_last ||
        value == SampledIoStalePolicy::substitute_initial;
}

bool enum_valid(SampledIoOverrunPolicy value) noexcept {
    return value == SampledIoOverrunPolicy::fail_release ||
        value == SampledIoOverrunPolicy::reject_newest;
}

bool enum_valid(SampledIoUnderrunPolicy value) noexcept {
    return value == SampledIoUnderrunPolicy::fail_release ||
        value == SampledIoUnderrunPolicy::substitute_safe;
}

} // namespace

bool sampled_io_read_header(
    std::span<const std::byte> frame,
    SampledIoFrameHeader& header) noexcept {
    if (frame.size() < sizeof(SampledIoFrameHeader)) {
        return false;
    }
    std::memcpy(&header, frame.data(), sizeof(header));
    return true;
}

bool sampled_io_frame_valid(
    std::span<const std::byte> frame,
    const CompiledSampledIoChannel& channel,
    SampledIoFrameStatus expected_status,
    std::uint64_t expected_sequence,
    std::uint64_t expected_release_generation,
    bool require_exact_sequence) noexcept {
    SampledIoFrameHeader header{};
    if (!sampled_io_read_header(frame, header) ||
        frame.size() != channel.frame_bytes ||
        header.struct_size != sizeof(SampledIoFrameHeader) ||
        header.version != sampled_io_frame_version ||
        header.channel_identity != channel.channel_identity ||
        header.sample_count != channel.samples_per_frame ||
        header.encoding != static_cast<std::uint32_t>(channel.encoding) ||
        header.timestamp_domain_identity !=
            channel.timestamp_domain_identity ||
        header.sample_interval_ns != channel.sample_period_ns ||
        header.trigger_identity != channel.trigger_identity ||
        (require_exact_sequence &&
         header.trigger_sequence != expected_sequence) ||
        header.calibration_identity != channel.calibration_identity ||
        header.status != static_cast<std::uint32_t>(expected_status) ||
        !zero_reserved(header) || header.sequence == 0 ||
        (require_exact_sequence &&
         (header.sequence != expected_sequence ||
          header.release_generation != expected_release_generation)) ||
        (!require_exact_sequence && header.release_generation != 0)) {
        return false;
    }
    const auto payload = frame.subspan(sizeof(SampledIoFrameHeader));
    return header.payload_checksum == sampled_io_payload_checksum(payload);
}

Status sampled_io_snapshot_slots(
    std::uint32_t graph_owner,
    std::span<const SampledIoChannelSpec> specifications,
    std::span<const CrossRateChannelSpec> cross_rate_specs,
    std::span<std::size_t> slot_counts,
    SampledIoCompileDiagnostic& diagnostic) noexcept {
    diagnostic = {};
    if (cross_rate_specs.size() > cross_rate_channel_capacity ||
        specifications.size() > sampled_io_channel_capacity ||
        slot_counts.size() != cross_rate_specs.size()) {
        diagnostic = {Status::capacity_exceeded,
                      "sampled-I/O storage capacity exceeded", {}};
        return diagnostic.status;
    }
    std::array<std::size_t, cross_rate_channel_capacity> candidate{};
    candidate.fill(cross_rate_snapshot_slot_count);
    std::array<bool, cross_rate_channel_capacity> seen{};
    for (std::size_t index = 0; index < specifications.size(); ++index) {
        const auto& spec = specifications[index];
        if (!spec.channel.valid() || spec.channel.owner() != graph_owner ||
            spec.channel.index() >= cross_rate_specs.size() ||
            spec.channel_identity == 0 ||
            (spec.ring_capacity != 2 && spec.ring_capacity != 4) ||
            seen[spec.channel.index()]) {
            diagnostic = {Status::invalid_argument,
                          "sampled-I/O storage request is invalid", spec.channel};
            return diagnostic.status;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (specifications[prior].channel_identity == spec.channel_identity) {
                diagnostic = {Status::invalid_argument,
                              "sampled-I/O storage identity is duplicated", spec.channel};
                return diagnostic.status;
            }
        }
        // Four-slot geometry must be proven before allocating the larger store.
        // Existing two-slot validation and its diagnostic ordering stay intact.
        if (spec.ring_capacity == 4) {
            const auto width = sampled_io_encoding_bytes(spec.encoding);
            std::size_t values = 0, payload = 0, bytes = 0;
            if (!width || !spec.element_count || !spec.samples_per_frame ||
                !enum_valid(spec.direction)) {
                diagnostic = {Status::invalid_argument,
                              "sampled-I/O storage geometry is invalid", spec.channel};
                return diagnostic.status;
            }
            if (!checked_multiply_size(spec.element_count, spec.samples_per_frame, values) ||
                !checked_multiply_size(values, width, payload) ||
                !checked_add_size(sizeof(SampledIoFrameHeader), payload, bytes) ||
                bytes > cross_rate_payload_capacity) {
                diagnostic = {Status::capacity_exceeded,
                              "sampled-I/O storage geometry overflows", spec.channel};
                return diagnostic.status;
            }
            const auto& cross = cross_rate_specs[spec.channel.index()];
            if (bytes != cross.payload_size || spec.initial_frame != cross.initial_sample) {
                diagnostic = {Status::invalid_argument,
                              "sampled-I/O storage shape disagrees with channel", spec.channel};
                return diagnostic.status;
            }
        }
        seen[spec.channel.index()] = true;
        candidate[spec.channel.index()] = spec.ring_capacity;
    }
    std::copy_n(candidate.begin(), slot_counts.size(), slot_counts.begin());
    return Status::ok;
}

Status compile_sampled_io(
    std::uint32_t graph_owner,
    std::span<const SampledIoChannelSpec> specifications,
    const CompiledRatePlan& rate_plan,
    const CompiledDeviceRatePlan& device_rate_plan,
    std::span<const CrossRateChannelSpec> cross_rate_specs,
    const CompiledCrossRatePlan& cross_rate_plan,
    CompiledSampledIoPlan& output,
    SampledIoCompileDiagnostic& diagnostic) noexcept {
    diagnostic = {};
    if (specifications.size() > sampled_io_channel_capacity) {
        diagnostic = {Status::capacity_exceeded,
                      "sampled-I/O channel capacity exceeded", {}};
        return diagnostic.status;
    }
    if (specifications.empty()) {
        output = {};
        return Status::ok;
    }
    try {
        CompiledSampledIoPlan candidate;
        candidate.channel_index_by_cross_rate.assign(
            cross_rate_plan.channels.size(),
            std::numeric_limits<std::size_t>::max());
        candidate.channels.reserve(specifications.size());
        candidate.safe_channel_indices.reserve(specifications.size());
        candidate.safe_phases.reserve(specifications.size());

        for (std::size_t index = 0; index < specifications.size(); ++index) {
            const auto& spec = specifications[index];
            diagnostic.channel = spec.channel;
            if (!spec.channel.valid() || spec.channel.owner() != graph_owner ||
                spec.channel.index() >= cross_rate_plan.channels.size() ||
                spec.channel.index() >= cross_rate_specs.size() ||
                !enum_valid(spec.direction) ||
                sampled_io_encoding_bytes(spec.encoding) == 0 ||
                spec.channel_identity == 0 || spec.element_count == 0 ||
                spec.samples_per_frame == 0 || spec.scale_denominator == 0 ||
                spec.offset_denominator == 0 || spec.units_identity == 0 ||
                spec.calibration_identity == 0 || spec.sample_period_ns == 0 ||
                spec.timestamp_domain_identity == 0 ||
                spec.clock_domain_identity == 0 ||
                !enum_valid(spec.trigger_mode) || spec.trigger_identity == 0 ||
                (spec.ring_capacity != 2 && spec.ring_capacity != 4) ||
                spec.initial_sequence == 0 ||
                !enum_valid(spec.stale_policy) ||
                !enum_valid(spec.overrun_policy) ||
                !enum_valid(spec.underrun_policy) ||
                candidate.channel_index_by_cross_rate[spec.channel.index()] !=
                    std::numeric_limits<std::size_t>::max()) {
                diagnostic = {Status::invalid_argument,
                              "sampled-I/O descriptor fields or identity are invalid",
                              spec.channel};
                return diagnostic.status;
            }
            std::size_t sample_values = 0;
            std::size_t payload_bytes = 0;
            std::size_t frame_bytes = 0;
            if (!checked_multiply_size(
                    spec.element_count, spec.samples_per_frame,
                    sample_values) ||
                !checked_multiply_size(
                    sample_values, sampled_io_encoding_bytes(spec.encoding),
                    payload_bytes) ||
                !checked_add_size(
                    sizeof(SampledIoFrameHeader), payload_bytes,
                    frame_bytes) || frame_bytes > cross_rate_payload_capacity) {
                diagnostic = {Status::capacity_exceeded,
                              "sampled-I/O frame geometry overflows",
                              spec.channel};
                return diagnostic.status;
            }
            const auto channel_index =
                static_cast<std::size_t>(spec.channel.index());
            const auto& cross = cross_rate_plan.channels[channel_index];
            const auto endpoint_index =
                channel_index <
                        cross_rate_plan.device_endpoint_index_by_channel.size()
                ? cross_rate_plan.device_endpoint_index_by_channel[channel_index]
                : std::numeric_limits<std::size_t>::max();
            if (cross.channel != spec.channel || cross.payload_size != frame_bytes ||
                cross_rate_specs[channel_index].initial_sample !=
                    spec.initial_frame ||
                cross.maximum_age_ns != spec.maximum_age_ns ||
                cross.snapshot_slot_count != spec.ring_capacity ||
                endpoint_index >= cross_rate_plan.device_endpoints.size()) {
                diagnostic = {Status::invalid_argument,
                              "sampled-I/O descriptor disagrees with its cross-rate channel",
                              spec.channel};
                return diagnostic.status;
            }
            const auto& endpoint =
                cross_rate_plan.device_endpoints[endpoint_index];
            const bool direction_matches =
                spec.direction == SampledIoDirection::input
                ? endpoint.producer && cross.producer_device.valid() &&
                      !cross.consumer_device.valid()
                : !endpoint.producer && cross.consumer_device.valid() &&
                      !cross.producer_device.valid();
            if (!direction_matches || endpoint.host_storage.empty() ||
                endpoint.slot_count == 0 ||
                endpoint.slot_count > spec.ring_capacity ||
                endpoint.timestamp_domain_identity !=
                    (spec.direction == SampledIoDirection::input
                         ? spec.timestamp_domain_identity
                         : cross_rate_runtime_logical_timestamp_domain_identity) ||
                (spec.direction == SampledIoDirection::output &&
                 spec.timestamp_domain_identity !=
                    cross_rate_runtime_logical_timestamp_domain_identity)) {
                diagnostic = {Status::invalid_argument,
                              "sampled-I/O direction or timestamp domain disagrees with its device endpoint",
                              spec.channel};
                return diagnostic.status;
            }

            CompiledSampledIoRecord record;
            record.public_record.channel = spec.channel;
            record.public_record.registration_index = index;
            record.public_record.direction = spec.direction;
            record.public_record.channel_identity = spec.channel_identity;
            record.public_record.encoding = spec.encoding;
            record.public_record.element_count = spec.element_count;
            record.public_record.samples_per_frame = spec.samples_per_frame;
            record.public_record.frame_bytes = frame_bytes;
            record.public_record.scale_numerator = spec.scale_numerator;
            record.public_record.scale_denominator = spec.scale_denominator;
            record.public_record.offset_numerator = spec.offset_numerator;
            record.public_record.offset_denominator = spec.offset_denominator;
            record.public_record.units_identity = spec.units_identity;
            record.public_record.calibration_identity =
                spec.calibration_identity;
            record.public_record.sample_period_ns = spec.sample_period_ns;
            record.public_record.timestamp_domain_identity =
                spec.timestamp_domain_identity;
            record.public_record.clock_domain_identity =
                spec.clock_domain_identity;
            record.public_record.trigger_mode = spec.trigger_mode;
            record.public_record.trigger_identity = spec.trigger_identity;
            record.public_record.ring_capacity = spec.ring_capacity;
            record.public_record.initial_sequence = spec.initial_sequence;
            record.public_record.maximum_age_ns = spec.maximum_age_ns;
            record.public_record.stale_policy = spec.stale_policy;
            record.public_record.overrun_policy = spec.overrun_policy;
            record.public_record.underrun_policy = spec.underrun_policy;
            record.public_record.safe_transition_timeout_ns =
                spec.safe_transition_timeout_ns;
            record.public_record.device_phase = endpoint.phase;
            record.public_record.backend = endpoint.backend;
            const auto phase = std::find_if(
                device_rate_plan.phases.begin(),
                device_rate_plan.phases.end(),
                [&](const CompiledDeviceRatePhase& value) {
                    return value.phase == endpoint.phase;
                });
            if (phase == device_rate_plan.phases.end()) {
                diagnostic = {Status::invalid_argument,
                              "sampled-I/O endpoint has no admitted device-rate phase",
                              spec.channel};
                return diagnostic.status;
            }
            record.public_record.payload_reference_ordinal =
                spec.direction == SampledIoDirection::input
                ? cross.producer_device.payload_reference_ordinal
                : cross.consumer_device.payload_reference_ordinal;
            record.cross_rate_channel_index = channel_index;

            const auto append_frame = [&](std::span<const std::byte> bytes,
                                          SampledIoFrameStatus status,
                                          std::size_t& offset) {
                offset = candidate.frame_storage.size();
                if (!sampled_io_frame_valid(
                        bytes, record.public_record, status,
                        spec.initial_sequence, 0, false)) {
                    return false;
                }
                candidate.frame_storage.insert(
                    candidate.frame_storage.end(), bytes.begin(), bytes.end());
                return true;
            };
            if (!append_frame(
                    spec.initial_frame, SampledIoFrameStatus::initial,
                    record.initial_offset)) {
                diagnostic = {Status::invalid_argument,
                              "sampled-I/O initial frame is invalid",
                              spec.channel};
                return diagnostic.status;
            }
            if (spec.direction == SampledIoDirection::input) {
                if (!spec.startup_safe_frame.empty() ||
                    !spec.failure_safe_frame.empty() ||
                    !spec.shutdown_safe_frame.empty() ||
                    spec.safe_transition_timeout_ns != 0 ||
                    spec.underrun_policy ==
                        SampledIoUnderrunPolicy::substitute_safe) {
                    diagnostic = {Status::invalid_argument,
                                  "sampled input cannot declare output safe frames",
                                  spec.channel};
                    return diagnostic.status;
                }
            } else {
                if (spec.safe_transition_timeout_ns == 0 ||
                    phase->wait_count != 0 || phase->signal_count == 0 ||
                    !append_frame(
                        spec.startup_safe_frame, SampledIoFrameStatus::safe,
                        record.startup_safe_offset) ||
                    !append_frame(
                        spec.failure_safe_frame, SampledIoFrameStatus::safe,
                        record.failure_safe_offset) ||
                    !append_frame(
                        spec.shutdown_safe_frame, SampledIoFrameStatus::safe,
                        record.shutdown_safe_offset)) {
                    diagnostic = {Status::invalid_argument,
                                  "sampled output safe frames or phase timeline shape are invalid",
                                  spec.channel};
                    return diagnostic.status;
                }
                candidate.safe_channel_indices.push_back(index);
            }
            candidate.channel_index_by_cross_rate[channel_index] = index;
            candidate.channels.push_back(std::move(record));
        }

        std::sort(
            candidate.safe_channel_indices.begin(),
            candidate.safe_channel_indices.end(),
            [&](std::size_t left, std::size_t right) {
                const auto& a = candidate.channels[left].public_record;
                const auto& b = candidate.channels[right].public_record;
                return a.device_phase.index() != b.device_phase.index()
                    ? a.device_phase.index() < b.device_phase.index()
                    : a.channel_identity < b.channel_identity;
            });
        for (std::size_t cursor = 0;
             cursor < candidate.safe_channel_indices.size();) {
            const auto first = cursor;
            const auto& first_record = candidate.channels[
                candidate.safe_channel_indices[cursor]].public_record;
            while (cursor < candidate.safe_channel_indices.size() &&
                   candidate.channels[candidate.safe_channel_indices[cursor]]
                           .public_record.device_phase ==
                       first_record.device_phase) {
                ++cursor;
            }
            const auto reference = std::find_if(
                rate_plan.releases.begin(), rate_plan.releases.end(),
                [&](const ReferenceRelease& release) {
                    return release.phase == first_record.device_phase;
                });
            if (reference == rate_plan.releases.end()) {
                diagnostic = {Status::invalid_argument,
                              "sampled output phase has no reference release",
                              first_record.channel};
                return diagnostic.status;
            }
            candidate.safe_phases.push_back({
                first_record.device_phase,
                first_record.backend,
                first,
                cursor - first,
                static_cast<std::size_t>(
                    reference - rate_plan.releases.begin()),
            });
        }
        output = std::move(candidate);
        diagnostic = {};
        return Status::ok;
    } catch (const std::bad_alloc&) {
        diagnostic = {Status::resource_exhausted,
                      "sampled-I/O compiler allocation failed", {}};
        return diagnostic.status;
    } catch (...) {
        diagnostic = {Status::internal_error,
                      "sampled-I/O compiler failed", {}};
        return diagnostic.status;
    }
}

} // namespace rt::detail
