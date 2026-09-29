#include "cross_rate_data.hpp"
#include "rate_dispatch.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <tuple>

namespace {

constexpr std::uint64_t kStateMask = 3;
constexpr std::uint64_t kFree = 0;
constexpr std::uint64_t kWriting = 1;
constexpr std::uint64_t kPublished = 2;
constexpr std::uint64_t kReading = 3;

std::uint64_t encode_control(
    std::uint64_t generation,
    std::uint64_t state) noexcept {
    return (generation << 2u) | state;
}

std::uint64_t control_generation(std::uint64_t control) noexcept {
    return control >> 2u;
}

std::uint64_t control_state(std::uint64_t control) noexcept {
    return control & kStateMask;
}

bool checked_add(
    std::size_t left,
    std::size_t right,
    std::size_t& output) noexcept {
    if (left > std::numeric_limits<std::size_t>::max() - right) {
        return false;
    }
    output = left + right;
    return true;
}

bool checked_multiply(
    std::size_t left,
    std::size_t right,
    std::size_t& output) noexcept {
    if (left != 0 &&
        right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    output = left * right;
    return true;
}

bool checked_add_u64(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& output) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return false;
    }
    output = left + right;
    return true;
}

bool checked_multiply_u64(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& output) noexcept {
    if (left != 0 &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    output = left * right;
    return true;
}

bool checked_age(
    std::uint64_t supercycle,
    std::uint64_t producer_time,
    std::uint64_t consumer_time,
    bool prior_cycle,
    std::uint64_t& age) noexcept {
    if (!prior_cycle) {
        if (producer_time > consumer_time) {
            return false;
        }
        age = consumer_time - producer_time;
        return true;
    }
    const auto prior_tail = supercycle - producer_time;
    if (consumer_time >
        std::numeric_limits<std::uint64_t>::max() - prior_tail) {
        return false;
    }
    age = prior_tail + consumer_time;
    return true;
}

bool valid_name(std::string_view name) noexcept {
    if (name.empty() || name.size() >= rt::cross_rate_channel_name_capacity) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char character) {
        return (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '.' || character == '_' || character == ':' ||
            character == '/' || character == '@' || character == '-';
    });
}

struct SourceKey {
    rt::CrossRateSampleProvenance provenance =
        rt::CrossRateSampleProvenance::initial_sample;
    std::size_t reference_index = rt::invalid_reference_release_index;
    std::int32_t cycle = 0;

    bool operator==(const SourceKey&) const noexcept = default;
};

SourceKey source_key(
    const rt::CompiledCrossRateSelection& selection,
    std::int32_t target_cycle = 0) noexcept {
    return {
        selection.provenance,
        selection.producer_reference_index,
        target_cycle + selection.source_cycle_offset,
    };
}

} // namespace

namespace rt::detail {

Status SnapshotStore::create(
    std::size_t payload_size,
    std::size_t slot_count,
    SnapshotStore& output) noexcept {
    std::size_t payload_bytes = 0;
    if (payload_size == 0 || slot_count == 0 ||
        !checked_multiply(payload_size, slot_count, payload_bytes)) {
        return Status::invalid_config;
    }
    try {
        SnapshotStore candidate;
        candidate.payload_size_ = payload_size;
        candidate.slot_count_ = slot_count;
        candidate.payload_storage_bytes_ = payload_bytes;
        candidate.controls_ =
            std::make_unique<SnapshotSlotControl[]>(slot_count);
        candidate.payload_ = std::make_unique<std::byte[]>(payload_bytes);
        output = std::move(candidate);
        return Status::ok;
    } catch (const std::bad_alloc&) {
        return Status::resource_exhausted;
    } catch (...) {
        return Status::internal_error;
    }
}

SnapshotStoreResult SnapshotStore::publish(
    std::uint64_t generation,
    std::span<const std::byte> payload) noexcept {
    if (!controls_ || !payload_ || payload.size() != payload_size_ ||
        generation == 0 || generation > maximum_generation() ||
        generation != next_generation_) {
        return SnapshotStoreResult::invalid_argument;
    }
    const auto slot_index = static_cast<std::size_t>(
        (generation - 1) % slot_count_);
    auto& control = controls_[slot_index].value;
    auto observed = control.load(std::memory_order_acquire);
    if (control_state(observed) != kFree) {
        return SnapshotStoreResult::capacity_exceeded;
    }
    const auto writing = encode_control(generation, kWriting);
    if (!control.compare_exchange_strong(
            observed,
            writing,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return SnapshotStoreResult::capacity_exceeded;
    }
    std::memcpy(
        payload_.get() + slot_index * payload_size_,
        payload.data(),
        payload_size_);
    control.store(
        encode_control(generation, kPublished),
        std::memory_order_release);
    next_generation_ = generation == maximum_generation()
        ? 0
        : generation + 1;
    return SnapshotStoreResult::ok;
}

SnapshotStoreResult SnapshotStore::copy(
    std::uint64_t generation,
    std::span<std::byte> output,
    SnapshotRetention retention) noexcept {
    if (!controls_ || !payload_ || output.size() != payload_size_ ||
        generation == 0 || generation > maximum_generation() ||
        (retention != SnapshotRetention::retain &&
         retention != SnapshotRetention::retire)) {
        return SnapshotStoreResult::invalid_argument;
    }
    const auto slot_index = static_cast<std::size_t>(
        (generation - 1) % slot_count_);
    auto& control = controls_[slot_index].value;
    auto observed = control.load(std::memory_order_acquire);
    const auto observed_generation = control_generation(observed);
    const auto observed_state = control_state(observed);
    if (observed_generation < generation ||
        (observed_generation == generation && observed_state == kWriting)) {
        return SnapshotStoreResult::not_ready;
    }
    if (observed_generation > generation ||
        (observed_generation == generation && observed_state == kFree)) {
        return SnapshotStoreResult::stale_generation;
    }
    if (observed_state != kPublished) {
        return SnapshotStoreResult::not_ready;
    }
    const auto reading = encode_control(generation, kReading);
    if (!control.compare_exchange_strong(
            observed,
            reading,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return SnapshotStoreResult::not_ready;
    }
    std::memcpy(
        output.data(),
        payload_.get() + slot_index * payload_size_,
        payload_size_);
    control.store(
        encode_control(
            generation,
            retention == SnapshotRetention::retain ? kPublished : kFree),
        std::memory_order_release);
    return SnapshotStoreResult::ok;
}

SnapshotStoreResult SnapshotStore::retire(
    std::uint64_t generation) noexcept {
    if (!controls_ || generation == 0 ||
        generation > maximum_generation()) {
        return SnapshotStoreResult::invalid_argument;
    }
    const auto slot_index = static_cast<std::size_t>(
        (generation - 1) % slot_count_);
    auto& control = controls_[slot_index].value;
    auto observed = control.load(std::memory_order_acquire);
    if (control_generation(observed) != generation) {
        return control_generation(observed) < generation
            ? SnapshotStoreResult::not_ready
            : SnapshotStoreResult::stale_generation;
    }
    if (control_state(observed) != kPublished) {
        return SnapshotStoreResult::not_ready;
    }
    if (!control.compare_exchange_strong(
            observed,
            encode_control(generation, kFree),
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return SnapshotStoreResult::not_ready;
    }
    return SnapshotStoreResult::ok;
}

bool SnapshotStore::can_publish(std::uint64_t generation) const noexcept {
    if (!controls_ || generation == 0 ||
        generation > maximum_generation() ||
        generation != next_generation_) {
        return false;
    }
    const auto slot_index = static_cast<std::size_t>(
        (generation - 1) % slot_count_);
    return control_state(
        controls_[slot_index].value.load(std::memory_order_acquire)) == kFree;
}

SnapshotStoreResult SnapshotStore::restore_committed(
    std::uint64_t generation,
    std::uint64_t next_generation,
    std::span<const std::byte> payload) noexcept {
    if (!controls_ || !payload_ || payload.size() != payload_size_ ||
        generation == 0 || generation > maximum_generation() ||
        (next_generation != 0 &&
         (next_generation > maximum_generation() ||
          next_generation != generation + 1))) {
        return SnapshotStoreResult::invalid_argument;
    }
    for (std::size_t index = 0; index < slot_count_; ++index) {
        controls_[index].value.store(0, std::memory_order_relaxed);
    }
    const auto slot_index = static_cast<std::size_t>(
        (generation - 1) % slot_count_);
    std::memcpy(
        payload_.get() + slot_index * payload_size_,
        payload.data(),
        payload_size_);
    controls_[slot_index].value.store(
        encode_control(generation, kPublished),
        std::memory_order_release);
    next_generation_ = next_generation;
    return SnapshotStoreResult::ok;
}

Status compile_cross_rate_data(
    std::uint32_t graph_owner,
    std::size_t phase_count,
    const CompiledRatePlan& rate_plan,
    std::span<const CrossRateChannelSpec> channels,
    const CompiledDeviceRatePlan* device_rate_plan,
    std::span<const CrossRateDeviceBufferSource> device_buffers,
    CompiledCrossRatePlan& output,
    CrossRateCompileDiagnostic& diagnostic,
    std::span<const std::size_t> snapshot_slot_counts) noexcept {
    diagnostic = {};
    // The public sampled-I/O descriptor is the only Runtime opt-in. Ordinary
    // callers omit this private override and retain the exact two-slot plan.
    if ((!snapshot_slot_counts.empty() &&
         snapshot_slot_counts.size() != channels.size()) ||
        std::any_of(snapshot_slot_counts.begin(), snapshot_slot_counts.end(),
                    [](std::size_t count) { return count != 2 && count != 4; })) {
        diagnostic = {Status::invalid_argument,
                      "cross-rate snapshot slot counts are invalid"};
        return diagnostic.status;
    }
    const auto snapshot_slots = [&](std::size_t index) {
        return snapshot_slot_counts.empty() ? cross_rate_snapshot_slot_count
                                           : snapshot_slot_counts[index];
    };
    if (channels.empty()) {
        output = {};
        return Status::ok;
    }
    if (rate_plan.domains.empty()) {
        diagnostic = {
            Status::invalid_config,
            "cross-rate channels require an explicit rate model"};
        return diagnostic.status;
    }
    if (channels.size() > cross_rate_channel_capacity ||
        rate_plan.bindings.size() != phase_count) {
        diagnostic = {
            Status::capacity_exceeded,
            "cross-rate channel or binding capacity exceeded"};
        return diagnostic.status;
    }

    try {
        const auto compile_device_endpoint = [&](const CrossRateChannelSpec& channel,
                                                 bool producer,
                                                 CompiledCrossRateDeviceEndpoint& endpoint)
            -> Status {
            const auto phase = producer ? channel.producer : channel.consumer;
            const auto& selector = producer
                ? channel.producer_device
                : channel.consumer_device;
            if (!device_rate_plan || !selector.valid()) {
                return Status::invalid_config;
            }
            const auto phase_record = std::find_if(
                device_rate_plan->phases.begin(),
                device_rate_plan->phases.end(),
                [&](const CompiledDeviceRatePhase& candidate) {
                    return candidate.phase == phase;
                });
            if (phase_record == device_rate_plan->phases.end() ||
                selector.payload_reference_ordinal >=
                    phase_record->payload_reference_count ||
                phase_record->maximum_in_flight == 0) {
                return Status::invalid_config;
            }
            const auto reference_index =
                phase_record->first_payload_reference_index +
                selector.payload_reference_ordinal;
            if (reference_index >= device_rate_plan->payload_references.size()) {
                return Status::invalid_config;
            }
            const auto& reference =
                device_rate_plan->payload_references[reference_index];
            const auto role_ok = producer
                ? reference.role == DeviceRatePayloadRole::output ||
                      reference.role == DeviceRatePayloadRole::input_output
                : reference.role == DeviceRatePayloadRole::input ||
                      reference.role == DeviceRatePayloadRole::input_output;
            const auto access_ok = producer
                ? reference.access == RTFW_DEVICE_ACCESS_WRITE ||
                      reference.access == RTFW_DEVICE_ACCESS_READ_WRITE
                : reference.access == RTFW_DEVICE_ACCESS_READ ||
                      reference.access == RTFW_DEVICE_ACCESS_READ_WRITE;
            const auto buffer = std::find_if(
                device_buffers.begin(),
                device_buffers.end(),
                [&](const CrossRateDeviceBufferSource& candidate) {
                    return candidate.buffer == reference.buffer &&
                           candidate.backend == reference.backend;
                });
            const auto host_access = producer
                ? RTFW_DEVICE_BUFFER_HOST_READ
                : RTFW_DEVICE_BUFFER_HOST_WRITE;
            if (!role_ok || !access_ok || buffer == device_buffers.end() ||
                buffer->storage.empty() ||
                (buffer->access & host_access) == 0 ||
                buffer->coherency != static_cast<std::uint32_t>(
                    HalV2MemoryCoherency::host_coherent) ||
                buffer->byte_granularity == 0 ||
                buffer->offset_granularity == 0 ||
                channel.payload_size % buffer->byte_granularity != 0 ||
                selector.slot_stride_bytes < channel.payload_size ||
                selector.slot_stride_bytes % buffer->offset_granularity != 0 ||
                reference.offset % buffer->offset_granularity != 0) {
                return Status::invalid_config;
            }
            std::uint64_t slot_delta = 0;
            std::uint64_t final_offset = 0;
            std::uint64_t final_end = 0;
            std::uint64_t envelope_end = 0;
            if (!checked_multiply_u64(
                    phase_record->maximum_in_flight - 1u,
                    selector.slot_stride_bytes,
                    slot_delta) ||
                !checked_add_u64(reference.offset, slot_delta, final_offset) ||
                !checked_add_u64(
                    final_offset, channel.payload_size, final_end) ||
                !checked_add_u64(
                    reference.offset, reference.bytes, envelope_end) ||
                final_end > envelope_end || final_end > buffer->bytes ||
                final_end > buffer->storage.size()) {
                return Status::invalid_config;
            }
            endpoint.producer = producer;
            endpoint.phase = phase;
            endpoint.backend = reference.backend;
            endpoint.buffer = reference.buffer;
            endpoint.reference_kind = reference.kind;
            endpoint.command_index = reference.command_index;
            endpoint.command_reference_index =
                reference.command_reference_index;
            endpoint.envelope_offset = reference.offset;
            endpoint.envelope_bytes = reference.bytes;
            endpoint.slot_stride_bytes = selector.slot_stride_bytes;
            endpoint.slot_count = phase_record->maximum_in_flight;
            endpoint.timestamp_domain_identity = producer
                ? phase_record->completion_timestamp_domain_identity
                : cross_rate_runtime_logical_timestamp_domain_identity;
            endpoint.host_storage = buffer->storage;
            return Status::ok;
        };

        std::vector<std::size_t> binding_by_phase(
            phase_count,
            std::numeric_limits<std::size_t>::max());
        for (std::size_t index = 0;
             index < rate_plan.bindings.size();
             ++index) {
            const auto& binding = rate_plan.bindings[index];
            if (!binding.phase.valid() ||
                binding.phase.owner() != graph_owner ||
                binding.phase.index() >= phase_count ||
                !binding.domain.valid() ||
                binding.domain.owner() != graph_owner ||
                binding.domain.index() >= rate_plan.domains.size() ||
                binding.compiled_phase_index >= phase_count ||
                binding_by_phase[binding.phase.index()] !=
                    std::numeric_limits<std::size_t>::max()) {
                diagnostic = {
                    Status::invalid_handle,
                    "cross-rate endpoint binding is invalid or duplicated"};
                return diagnostic.status;
            }
            binding_by_phase[binding.phase.index()] = index;
        }

        std::size_t initial_bytes = 0;
        std::size_t snapshot_bytes = 0;
        std::size_t selection_count = 0;
        for (std::size_t index = 0; index < channels.size(); ++index) {
            const auto& channel = channels[index];
            if (!valid_name(channel.name) ||
                channel.mode != CrossRateMode::sample_and_hold ||
                channel.payload_size == 0 ||
                channel.payload_size > cross_rate_payload_capacity ||
                channel.initial_sample.size() != channel.payload_size) {
                diagnostic = {
                    Status::invalid_config,
                    "cross-rate channel name, mode, payload, or initial sample is invalid"};
                return diagnostic.status;
            }
            if (!channel.producer.valid() || !channel.consumer.valid() ||
                channel.producer.owner() != graph_owner ||
                channel.consumer.owner() != graph_owner ||
                channel.producer.index() >= phase_count ||
                channel.consumer.index() >= phase_count) {
                diagnostic = {
                    Status::invalid_handle,
                    "cross-rate channel contains an invalid or foreign endpoint"};
                return diagnostic.status;
            }
            if (channel.producer == channel.consumer) {
                diagnostic = {
                    Status::invalid_config,
                    "cross-rate channel endpoints must be distinct"};
                return diagnostic.status;
            }
            const auto producer_binding_index =
                binding_by_phase[channel.producer.index()];
            const auto consumer_binding_index =
                binding_by_phase[channel.consumer.index()];
            if (producer_binding_index ==
                    std::numeric_limits<std::size_t>::max() ||
                consumer_binding_index ==
                    std::numeric_limits<std::size_t>::max()) {
                diagnostic = {
                    Status::invalid_config,
                    "cross-rate endpoint is not bound exactly once"};
                return diagnostic.status;
            }
            const auto& producer_binding =
                rate_plan.bindings[producer_binding_index];
            const auto& consumer_binding =
                rate_plan.bindings[consumer_binding_index];
            const auto producer_device =
                producer_binding.phase_kind == RatePhaseKind::device;
            const auto consumer_device =
                consumer_binding.phase_kind == RatePhaseKind::device;
            const auto producer_selector_partial =
                channel.producer_device.payload_reference_ordinal !=
                    invalid_device_rate_payload_reference_ordinal ||
                channel.producer_device.slot_stride_bytes != 0;
            const auto consumer_selector_partial =
                channel.consumer_device.payload_reference_ordinal !=
                    invalid_device_rate_payload_reference_ordinal ||
                channel.consumer_device.slot_stride_bytes != 0;
            if ((producer_device && consumer_device) ||
                (producer_device != channel.producer_device.valid()) ||
                (consumer_device != channel.consumer_device.valid()) ||
                (producer_selector_partial != producer_device) ||
                (consumer_selector_partial != consumer_device)) {
                diagnostic = {
                    Status::invalid_config,
                    "cross-rate device endpoint selector is missing, malformed, or device-to-device"};
                return diagnostic.status;
            }
            if (producer_device || consumer_device) {
                CompiledCrossRateDeviceEndpoint endpoint;
                const auto endpoint_status = compile_device_endpoint(
                    channel, producer_device, endpoint);
                if (endpoint_status != Status::ok) {
                    diagnostic = {
                        endpoint_status,
                        "cross-rate device endpoint is inaccessible or conflicts with its admitted payload envelope"};
                    return diagnostic.status;
                }
            }
            if (producer_binding.domain == consumer_binding.domain) {
                diagnostic = {
                    Status::invalid_config,
                    "cross-rate endpoints must use different rate domains"};
                return diagnostic.status;
            }
            for (std::size_t earlier = 0; earlier < index; ++earlier) {
                if (channels[earlier].name == channel.name) {
                    diagnostic = {
                        Status::invalid_config,
                        "cross-rate channel names must be unique"};
                    return diagnostic.status;
                }
                if (channels[earlier].producer == channel.producer &&
                    channels[earlier].consumer == channel.consumer) {
                    diagnostic = {
                        Status::invalid_config,
                        "duplicate cross-rate semantic edge"};
                    return diagnostic.status;
                }
            }
            std::size_t channel_snapshot_bytes = 0;
            if (!checked_add(
                    initial_bytes,
                    channel.initial_sample.size(),
                    initial_bytes) ||
                initial_bytes > cross_rate_initial_bytes_capacity ||
                !checked_multiply(
                    channel.payload_size,
                    snapshot_slots(index),
                    channel_snapshot_bytes) ||
                !checked_add(
                    snapshot_bytes,
                    channel_snapshot_bytes,
                    snapshot_bytes) ||
                snapshot_bytes > cross_rate_snapshot_bytes_capacity) {
                diagnostic = {
                    Status::capacity_exceeded,
                    "cross-rate aggregate payload storage exceeded"};
                return diagnostic.status;
            }
            std::size_t consumer_releases = 0;
            for (const auto& release : rate_plan.releases) {
                consumer_releases += release.phase == channel.consumer ? 1u : 0u;
            }
            std::size_t channel_selections = 0;
            if (consumer_releases == 0 ||
                !checked_multiply(consumer_releases, 2, channel_selections) ||
                !checked_add(
                    selection_count,
                    channel_selections,
                    selection_count) ||
                selection_count > cross_rate_selection_capacity) {
                diagnostic = {
                    Status::capacity_exceeded,
                    "cross-rate selection capacity exceeded"};
                return diagnostic.status;
            }
        }

        CompiledCrossRatePlan candidate;
        candidate.channels.reserve(channels.size());
        candidate.selections.reserve(selection_count);
        candidate.stores.reserve(channels.size());
        candidate.device_endpoints.reserve(channels.size());
        if (std::any_of(
                channels.begin(), channels.end(),
                [](const CrossRateChannelSpec& channel) {
                    return channel.producer_device.valid() ||
                           channel.consumer_device.valid();
                })) {
            candidate.device_endpoint_index_by_channel.assign(
                channels.size(), invalid_reference_release_index);
        }

        for (std::size_t channel_index = 0;
             channel_index < channels.size();
             ++channel_index) {
            const auto& channel = channels[channel_index];
            const auto& producer_binding = rate_plan.bindings[
                binding_by_phase[channel.producer.index()]];
            const auto& consumer_binding = rate_plan.bindings[
                binding_by_phase[channel.consumer.index()]];

            CompiledCrossRateDeviceEndpoint device_endpoint;
            const bool has_device_endpoint =
                producer_binding.phase_kind == RatePhaseKind::device ||
                consumer_binding.phase_kind == RatePhaseKind::device;
            if (has_device_endpoint) {
                const auto endpoint_status = compile_device_endpoint(
                    channel,
                    producer_binding.phase_kind == RatePhaseKind::device,
                    device_endpoint);
                if (endpoint_status != Status::ok) {
                    diagnostic = {
                        endpoint_status,
                        "cross-rate device endpoint compilation failed"};
                    return diagnostic.status;
                }
                device_endpoint.channel_index = channel_index;
                std::uint64_t endpoint_end = 0;
                if (!checked_add_u64(
                        device_endpoint.envelope_offset,
                        device_endpoint.envelope_bytes,
                        endpoint_end)) {
                    diagnostic = {
                        Status::capacity_exceeded,
                        "cross-rate device endpoint envelope overflowed"};
                    return diagnostic.status;
                }
                for (const auto& prior : candidate.device_endpoints) {
                    if (prior.buffer != device_endpoint.buffer) {
                        continue;
                    }
                    std::uint64_t prior_end = 0;
                    if (!checked_add_u64(
                            prior.envelope_offset,
                            prior.envelope_bytes,
                            prior_end) ||
                        (device_endpoint.envelope_offset < prior_end &&
                         prior.envelope_offset < endpoint_end)) {
                        diagnostic = {
                            Status::invalid_config,
                            "cross-rate device endpoint envelopes overlap"};
                        return diagnostic.status;
                    }
                }
            }

            std::vector<std::size_t> producer_releases;
            std::vector<std::size_t> consumer_releases;
            for (std::size_t reference_index = 0;
                 reference_index < rate_plan.releases.size();
                 ++reference_index) {
                const auto phase = rate_plan.releases[reference_index].phase;
                if (phase == channel.producer) {
                    producer_releases.push_back(reference_index);
                }
                if (phase == channel.consumer) {
                    consumer_releases.push_back(reference_index);
                }
            }
            if (producer_releases.empty() || consumer_releases.empty()) {
                diagnostic = {
                    Status::invalid_config,
                    "cross-rate endpoint has no reference release"};
                return diagnostic.status;
            }

            std::vector<CompiledCrossRateSelection> first;
            std::vector<CompiledCrossRateSelection> repeating;
            first.reserve(consumer_releases.size());
            repeating.reserve(consumer_releases.size());
            for (const auto consumer_reference_index : consumer_releases) {
                const auto& consumer_release =
                    rate_plan.releases[consumer_reference_index];
                const auto producer_position = std::lower_bound(
                    producer_releases.begin(),
                    producer_releases.end(),
                    consumer_reference_index);
                const bool has_in_cycle =
                    producer_position != producer_releases.begin();
                const auto in_cycle_reference = has_in_cycle
                    ? *(producer_position - 1)
                    : invalid_reference_release_index;
                const auto wrap_reference = producer_releases.back();

                const auto make_selection = [&](bool steady) {
                    CompiledCrossRateSelection selection;
                    selection.channel = CrossRateChannelHandle{
                        graph_owner,
                        static_cast<std::uint32_t>(channel_index)};
                    selection.channel_registration_index = channel_index;
                    selection.producer = channel.producer;
                    selection.consumer = channel.consumer;
                    selection.producer_domain = producer_binding.domain;
                    selection.consumer_domain = consumer_binding.domain;
                    selection.horizon = steady
                        ? CrossRateSelectionHorizon::repeating_supercycle
                        : CrossRateSelectionHorizon::first_supercycle;
                    selection.consumer_reference_index =
                        consumer_reference_index;
                    selection.consumer_release_sequence =
                        consumer_release.domain_release_sequence;
                    selection.consumer_substep_ordinal =
                        consumer_release.substep_ordinal;
                    const bool use_initial = !steady && !has_in_cycle;
                    if (use_initial) {
                        selection.provenance =
                            CrossRateSampleProvenance::initial_sample;
                        selection.age_ns = consumer_release.release_time_ns;
                    } else {
                        const auto source_reference = has_in_cycle
                            ? in_cycle_reference
                            : wrap_reference;
                        const auto& source =
                            rate_plan.releases[source_reference];
                        selection.provenance =
                            CrossRateSampleProvenance::produced;
                        selection.producer_reference_index = source_reference;
                        selection.producer_release_sequence =
                            source.domain_release_sequence;
                        selection.producer_substep_ordinal =
                            source.substep_ordinal;
                        selection.source_cycle_offset = has_in_cycle ? 0 : -1;
                        if (!checked_age(
                                rate_plan.supercycle_ns,
                                source.release_time_ns,
                                consumer_release.release_time_ns,
                                !has_in_cycle,
                                selection.age_ns)) {
                            selection.producer_reference_index =
                                invalid_reference_release_index;
                        }
                    }
                    selection.freshness =
                        channel.maximum_age_ns ==
                                std::numeric_limits<std::uint64_t>::max() ||
                            selection.age_ns <= channel.maximum_age_ns
                        ? CrossRateFreshness::fresh
                        : CrossRateFreshness::stale;
                    return selection;
                };
                auto first_selection = make_selection(false);
                auto repeating_selection = make_selection(true);
                if ((!has_in_cycle &&
                     repeating_selection.producer_reference_index ==
                         invalid_reference_release_index) ||
                    (has_in_cycle &&
                     first_selection.producer_reference_index ==
                         invalid_reference_release_index)) {
                    diagnostic = {
                        Status::capacity_exceeded,
                        "cross-rate freshness age is not representable"};
                    return diagnostic.status;
                }
                first.push_back(first_selection);
                repeating.push_back(repeating_selection);
            }
            for (std::size_t index = 1; index < first.size(); ++index) {
                first[index].held =
                    source_key(first[index]) == source_key(first[index - 1]);
            }
            for (std::size_t index = 0; index < repeating.size(); ++index) {
                const auto previous = index == 0
                    ? source_key(repeating.back(), -1)
                    : source_key(repeating[index - 1]);
                repeating[index].held =
                    source_key(repeating[index]) == previous;
            }

            CompiledCrossRateChannel descriptor;
            descriptor.channel = CrossRateChannelHandle{
                graph_owner,
                static_cast<std::uint32_t>(channel_index)};
            std::copy(
                channel.name.begin(),
                channel.name.end(),
                descriptor.name.begin());
            descriptor.registration_index = channel_index;
            descriptor.producer = channel.producer;
            descriptor.consumer = channel.consumer;
            descriptor.producer_domain = producer_binding.domain;
            descriptor.consumer_domain = consumer_binding.domain;
            descriptor.producer_compiled_phase_index =
                producer_binding.compiled_phase_index;
            descriptor.consumer_compiled_phase_index =
                consumer_binding.compiled_phase_index;
            descriptor.payload_size = channel.payload_size;
            descriptor.mode = channel.mode;
            descriptor.maximum_age_ns = channel.maximum_age_ns;
            descriptor.first_selection_index = candidate.selections.size();
            descriptor.selection_count = first.size() * 2;
            descriptor.snapshot_slot_count = snapshot_slots(channel_index);
            descriptor.producer_device = channel.producer_device;
            descriptor.consumer_device = channel.consumer_device;
            if (has_device_endpoint) {
                if (device_endpoint.producer) {
                    descriptor.producer_device_backend =
                        device_endpoint.backend;
                    descriptor.producer_device_buffer = device_endpoint.buffer;
                    descriptor.producer_device_base_offset =
                        device_endpoint.envelope_offset;
                    descriptor.producer_device_slot_count =
                        device_endpoint.slot_count;
                    descriptor.producer_timestamp_domain_identity =
                        device_endpoint.timestamp_domain_identity;
                } else {
                    descriptor.consumer_device_backend =
                        device_endpoint.backend;
                    descriptor.consumer_device_buffer = device_endpoint.buffer;
                    descriptor.consumer_device_base_offset =
                        device_endpoint.envelope_offset;
                    descriptor.consumer_device_slot_count =
                        device_endpoint.slot_count;
                }
            }
            if (!checked_multiply(
                    channel.payload_size,
                    descriptor.snapshot_slot_count,
                    descriptor.snapshot_bytes)) {
                diagnostic = {
                    Status::capacity_exceeded,
                    "cross-rate snapshot size is not representable"};
                return diagnostic.status;
            }
            candidate.channels.push_back(descriptor);
            for (std::size_t index = 0; index < first.size(); ++index) {
                candidate.selections.push_back(first[index]);
                candidate.selections.push_back(repeating[index]);
            }
            SnapshotStore store;
            const auto store_status = SnapshotStore::create(
                channel.payload_size,
                descriptor.snapshot_slot_count,
                store);
            if (store_status != Status::ok) {
                diagnostic = {
                    store_status,
                    "cross-rate snapshot store construction failed"};
                return diagnostic.status;
            }
            candidate.stores.push_back(std::move(store));
            if (has_device_endpoint) {
                candidate.device_endpoint_index_by_channel[channel_index] =
                    candidate.device_endpoints.size();
                candidate.device_endpoints.push_back(device_endpoint);
            }
        }

        output = std::move(candidate);
        return Status::ok;
    } catch (const std::bad_alloc&) {
        diagnostic = {Status::resource_exhausted, nullptr};
        return diagnostic.status;
    } catch (...) {
        diagnostic = {Status::internal_error, nullptr};
        return diagnostic.status;
    }
}

} // namespace rt::detail
