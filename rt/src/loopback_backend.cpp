#include <rt/loopback_backend.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <limits>
#include <utility>

namespace rt {

namespace {

constexpr std::uint32_t kFree = 0;
constexpr std::uint32_t kOwned = 1;
constexpr std::uint32_t kReady = 2;
// A timeout completion is fully published but only cancellation may release it.
// kOwned always grants exclusive access: submit, poll and cancel never inspect
// another callback's completion storage, including stale data during reuse.
constexpr std::uint32_t kHeld = 3;
constexpr std::uint32_t kKnownAccess =
    RTFW_DEVICE_BUFFER_HOST_READ | RTFW_DEVICE_BUFFER_HOST_WRITE |
    RTFW_DEVICE_BUFFER_DEVICE_READ | RTFW_DEVICE_BUFFER_DEVICE_WRITE;

bool add_overflows(std::uint64_t left, std::uint64_t right) noexcept {
    return left > std::numeric_limits<std::uint64_t>::max() - right;
}

} // namespace

struct SampledIoLoopbackBackend::Impl {
    struct Buffer {
        void* data = nullptr;
        std::uint64_t bytes = 0;
        bool occupied = false;
    };

    struct Slot {
        std::atomic<std::uint32_t> state{kFree};
        HalV2BatchCompletion completion{};
    };

#if defined(_MSC_VER)
// The cache-line alignment is intentional. MSVC reports the resulting
// isolation padding as C4324 under /W4.
#    pragma warning(push)
#    pragma warning(disable : 4324)
#endif
    struct alignas(64) LogicalActionSlot {
        std::atomic_flag writer = ATOMIC_FLAG_INIT;
        std::atomic<std::uint64_t> committed{
            std::numeric_limits<std::uint64_t>::max()};
        std::array<std::atomic<std::uint64_t>, 8> words{};
    };
#if defined(_MSC_VER)
#    pragma warning(pop)
#endif

    void record_logical_action(
        const DeviceCommandBatch& batch,
        SampledIoLoopbackFault fault,
        HalV2Status status,
        std::uint64_t device_timestamp) noexcept {
        auto sequence = logical_action_sequence.load(std::memory_order_relaxed);
        while (sequence != std::numeric_limits<std::uint64_t>::max() &&
               !logical_action_sequence.compare_exchange_weak(
                   sequence,
                   sequence + 1,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
        if (sequence == std::numeric_limits<std::uint64_t>::max()) {
            return;
        }
        auto& slot = logical_actions[sequence % logical_actions.size()];
        if (slot.writer.test_and_set(std::memory_order_acquire)) {
            return;
        }
        SampledIoLoopbackLogicalAction action;
        action.sequence = sequence;
        action.batch_id = batch.batch_id;
        action.device_timestamp = device_timestamp;
        action.opcode = batch.command_count == 0
            ? 0
            : batch.commands[0].opcode;
        action.status = static_cast<std::int32_t>(status);
        action.fault = fault;
        action.command_count = static_cast<std::uint8_t>(batch.command_count);
        std::uint64_t identity = 14695981039346656037ull;
        const auto mix = [&identity](std::uint64_t value) noexcept {
            for (std::size_t index = 0; index < 8; ++index) {
                identity ^= static_cast<std::uint8_t>(value >> (index * 8));
                identity *= 1099511628211ull;
            }
        };
        mix(action.batch_id);
        mix(action.device_timestamp);
        mix(action.opcode);
        mix(static_cast<std::uint32_t>(action.status));
        mix(static_cast<std::uint8_t>(fault));
        action.content_identity = identity;
        slot.committed.store(
            std::numeric_limits<std::uint64_t>::max(),
            std::memory_order_release);
        const auto words = std::bit_cast<std::array<std::uint64_t, 8>>(action);
        for (std::size_t index = 0; index < words.size(); ++index) {
            slot.words[index].store(words[index], std::memory_order_relaxed);
        }
        slot.committed.store(sequence, std::memory_order_release);
        slot.writer.clear(std::memory_order_release);
    }

    [[nodiscard]] bool read_logical_action(
        std::uint64_t sequence,
        SampledIoLoopbackLogicalAction& action) const noexcept {
        action = {};
        const auto& slot = logical_actions[sequence % logical_actions.size()];
        if (slot.committed.load(std::memory_order_acquire) != sequence) {
            return false;
        }
        std::array<std::uint64_t, 8> words{};
        for (std::size_t index = 0; index < words.size(); ++index) {
            words[index] = slot.words[index].load(std::memory_order_relaxed);
        }
        const auto candidate =
            std::bit_cast<SampledIoLoopbackLogicalAction>(words);
        if (slot.committed.load(std::memory_order_acquire) != sequence ||
            candidate.sequence != sequence) {
            return false;
        }
        action = candidate;
        return true;
    }

    explicit Impl(const SampledIoLoopbackConfig& requested) noexcept
        : config(requested) {
        valid_config = config.queue_capacity != 0 &&
            config.queue_capacity <= sampled_io_loopback_capacity &&
            config.buffer_capacity != 0 &&
            config.buffer_capacity <= sampled_io_loopback_capacity &&
            config.maximum_buffer_bytes != 0 &&
            config.memory_domain_identity != 0 &&
            config.timestamp_domain_identity != 0;
        api.instance = this;
        api.get_capabilities = &get_capabilities;
        api.initialize = &initialize;
        api.register_buffer = &register_buffer;
        api.unregister_buffer = &unregister_buffer;
        api.submit = &submit;
        api.poll = &poll;
        api.cancel = &cancel;
        api.get_health = &get_health;
        api.reset = &reset;
        api.shutdown = &shutdown;
        memory.instance = this;
        memory.discover = &discover_memory;
        memory.register_memory = &register_memory;
        memory.unregister_memory = &unregister_memory;
        memory.query_timestamp_correlation = &query_correlation;
        commands.instance = this;
        commands.get_capabilities = &get_command_capabilities;
        commands.submit = &submit_batch;
        commands.poll = &poll_batches;
        commands.cancel = &cancel_batch;
        commands.request_stop = &request_stop;
    }

    static Impl* self(void* instance) noexcept {
        return static_cast<Impl*>(instance);
    }

    static HalV2Status get_capabilities(
        void* instance, HalV2Capabilities* output) noexcept {
        auto* backend = self(instance);
        if (!backend || !output || !backend->valid_config) {
            return HalV2Status::invalid_argument;
        }
        *output = {};
        output->max_in_flight = backend->config.queue_capacity;
        output->max_registered_buffers = backend->config.buffer_capacity;
        output->max_buffer_bytes = backend->config.maximum_buffer_bytes;
        output->supports_cancel = 1;
        output->supports_reset = 1;
        output->deterministic_mock = 1;
        constexpr char prefix[] = "sampled_io.loopback.";
        std::copy_n(prefix, sizeof(prefix) - 1, output->backend_id.begin());
        constexpr char digits[] = "0123456789abcdef";
        const auto route_identity = backend->route_identity();
        for (std::size_t index = 0; index < 16; ++index) {
            const auto shift = static_cast<unsigned>((15 - index) * 4);
            output->backend_id[sizeof(prefix) - 1 + index] =
                digits[(route_identity >> shift) & 0xfu];
        }
        output->control_storage_bytes = sizeof(Impl);
        return HalV2Status::ok;
    }

    static HalV2Status initialize(
        void* instance, const HalV2InitializeConfig* request) noexcept {
        auto* backend = self(instance);
        if (!backend || !request || !backend->valid_config ||
            request->requested_in_flight > backend->config.queue_capacity ||
            request->requested_registered_buffers >
                backend->config.buffer_capacity) {
            return HalV2Status::invalid_argument;
        }
        backend->accepting.store(true, std::memory_order_release);
        backend->initialized.store(true, std::memory_order_release);
        return HalV2Status::ok;
    }

    static HalV2Status register_buffer(
        void* instance,
        const HalV2BufferRegistration* registration,
        std::uint64_t* token) noexcept {
        auto* backend = self(instance);
        if (token) {
            *token = 0;
        }
        if (!backend || !registration || !token || !registration->data ||
            registration->bytes == 0 ||
            registration->bytes > backend->config.maximum_buffer_bytes ||
            (registration->flags & ~kKnownAccess) != 0) {
            return HalV2Status::invalid_argument;
        }
        for (std::size_t index = 0;
             index < backend->config.buffer_capacity; ++index) {
            auto& slot = backend->buffers[index];
            if (!slot.occupied) {
                slot.data = registration->data;
                slot.bytes = registration->bytes;
                slot.occupied = true;
                *token = index + 1;
                return HalV2Status::ok;
            }
        }
        return HalV2Status::resource_exhausted;
    }

    static HalV2Status unregister_buffer(
        void* instance, std::uint64_t token) noexcept {
        auto* backend = self(instance);
        if (!backend || token == 0 ||
            token > backend->config.buffer_capacity ||
            !backend->buffers[token - 1].occupied) {
            return HalV2Status::invalid_argument;
        }
        backend->buffers[token - 1] = {};
        return HalV2Status::ok;
    }

    static HalV2Status submit(
        void*, const HalV2Submission*) noexcept {
        return HalV2Status::unsupported;
    }

    static HalV2Status poll(
        void*, HalV2Completion*, std::uint64_t,
        std::uint64_t* count) noexcept {
        if (!count) {
            return HalV2Status::invalid_argument;
        }
        *count = 0;
        return HalV2Status::ok;
    }

    static HalV2Status cancel(void*, std::uint64_t) noexcept {
        return HalV2Status::unsupported;
    }

    static HalV2Status get_health(
        void* instance, HalV2Health* output) noexcept {
        auto* backend = self(instance);
        if (!backend || !output) {
            return HalV2Status::invalid_argument;
        }
        *output = {};
        output->state = static_cast<std::uint32_t>(
            backend->initialized.load(std::memory_order_acquire)
                ? HalV2HealthState::healthy
                : HalV2HealthState::shutdown);
        output->submissions = backend->submissions.load();
        output->completions = backend->completions.load();
        output->queue_rejections = backend->rejected.load();
        output->cancellations = backend->cancellations.load();
        output->outstanding = output->submissions - output->completions;
        return HalV2Status::ok;
    }

    static HalV2Status reset(void* instance) noexcept {
        auto* backend = self(instance);
        if (!backend) {
            return HalV2Status::invalid_argument;
        }
        for (auto& slot : backend->slots) {
            slot.state.store(kFree, std::memory_order_release);
        }
        backend->accepting.store(true, std::memory_order_release);
        return HalV2Status::ok;
    }

    static HalV2Status shutdown(void* instance) noexcept {
        auto* backend = self(instance);
        if (!backend) {
            return HalV2Status::invalid_argument;
        }
        backend->accepting.store(false, std::memory_order_release);
        backend->initialized.store(false, std::memory_order_release);
        return HalV2Status::ok;
    }

    static HalV2Status discover_memory(
        void* instance, HalV2MemoryTopologySnapshot* output) noexcept {
        auto* backend = self(instance);
        if (!backend || !output || output->struct_size < sizeof(*output)) {
            return HalV2Status::invalid_argument;
        }
        *output = {};
        output->memory_domain_count = 1;
        output->topology_node_count = 1;
        output->timestamp_domain_count = 1;
        output->completion_timestamp_domain_identity =
            backend->config.timestamp_domain_identity;
        auto& domain = output->memory_domains[0];
        domain.identity = backend->config.memory_domain_identity;
        domain.kind = static_cast<std::uint32_t>(HalV2MemoryDomainKind::host);
        domain.ownership_modes = hal_v2_memory_ownership_borrowed_host;
        domain.maximum_bytes = backend->config.maximum_buffer_bytes;
        domain.byte_granularity = 1;
        domain.alignment = 1;
        domain.offset_granularity = 1;
        domain.access = kKnownAccess;
        domain.coherency = static_cast<std::uint32_t>(
            HalV2MemoryCoherency::host_coherent);
        domain.topology_node_identity = 1;
        domain.timestamp_domain_identity =
            backend->config.timestamp_domain_identity;
        output->topology_nodes[0].identity = 1;
        output->topology_nodes[0].kind = static_cast<std::uint32_t>(
            HalV2TopologyNodeKind::host);
        auto& timestamp = output->timestamp_domains[0];
        timestamp.identity = backend->config.timestamp_domain_identity;
        timestamp.kind = static_cast<std::uint32_t>(
            HalV2TimestampDomainKind::runtime_monotonic);
        timestamp.tick_numerator_ns = 1;
        timestamp.tick_denominator = 1;
        timestamp.monotonic = 1;
        return HalV2Status::ok;
    }

    static HalV2Status register_memory(
        void* instance,
        const HalV2MemoryRegistration* registration,
        HalV2MemoryToken* token) noexcept {
        auto* backend = self(instance);
        if (token) {
            // The caller owns the extension/version/native metadata. This
            // backend produces only the submission token; avoid a whole-
            // record assignment across a shared-library ABI boundary.
            token->submission_token = 0;
        }
        if (!backend || !registration || !token ||
            registration->domain_identity !=
                backend->config.memory_domain_identity ||
            registration->ownership != static_cast<std::uint32_t>(
                HalV2MemoryOwnership::borrowed_host) ||
            registration->coherency != static_cast<std::uint32_t>(
                HalV2MemoryCoherency::host_coherent) ||
            registration->synchronization != hal_v2_memory_sync_none) {
            return HalV2Status::invalid_argument;
        }
        HalV2BufferRegistration legacy{};
        legacy.data = registration->host_data;
        legacy.bytes = registration->bytes;
        legacy.flags = registration->access;
        legacy.name = registration->name;
        return register_buffer(instance, &legacy, &token->submission_token);
    }

    static HalV2Status unregister_memory(
        void* instance,
        const HalV2MemoryRegistration*,
        const HalV2MemoryToken* token) noexcept {
        return token
            ? unregister_buffer(instance, token->submission_token)
            : HalV2Status::invalid_argument;
    }

    static HalV2Status query_correlation(
        void*, const HalV2TimestampCorrelationQuery*,
        HalV2TimestampCorrelation*) noexcept {
        return HalV2Status::unsupported;
    }

    static HalV2Status get_command_capabilities(
        void* instance,
        HalV2CommandTimelineCapabilities* output) noexcept {
        auto* backend = self(instance);
        if (!backend || !output) {
            return HalV2Status::invalid_argument;
        }
        *output = {};
        output->max_in_flight_batches = backend->config.queue_capacity;
        output->max_commands_per_batch = hal_v2_command_capacity;
        output->max_wait_points = hal_v2_timeline_wait_capacity;
        output->max_signal_points = hal_v2_timeline_signal_capacity;
        output->max_timelines = hal_v2_timeline_capacity;
        output->completion_batch_capacity = backend->config.queue_capacity;
        output->backend_control_storage_bytes = sizeof(backend->slots);
        return HalV2Status::ok;
    }

    const SampledIoLoopbackRoute* route(std::uint32_t opcode) const noexcept {
        for (std::size_t index = 0; index < route_count; ++index) {
            if (routes[index].opcode == opcode) {
                return &routes[index];
            }
        }
        return nullptr;
    }

    std::uint64_t route_identity() const noexcept {
        std::uint64_t hash = 14695981039346656037ull;
        const auto mix = [&](std::uint64_t value) {
            for (std::size_t index = 0; index < sizeof(value); ++index) {
                hash ^= (value >> (index * 8u)) & 0xffu;
                hash *= 1099511628211ull;
            }
        };
        mix(route_count);
        std::uint32_t previous_opcode = 0;
        for (std::size_t ordinal = 0; ordinal < route_count; ++ordinal) {
            const SampledIoLoopbackRoute* selected = nullptr;
            for (std::size_t index = 0; index < route_count; ++index) {
                if (routes[index].opcode > previous_opcode &&
                    (!selected || routes[index].opcode < selected->opcode)) {
                    selected = &routes[index];
                }
            }
            if (!selected) {
                break;
            }
            previous_opcode = selected->opcode;
            mix(selected->opcode);
            mix(selected->source_channel_identity);
            mix(selected->destination_channel_identity);
            mix(selected->destination_timestamp_domain_identity);
            mix(selected->destination_calibration_identity);
            mix(selected->destination_trigger_identity);
        }
        return hash;
    }

    HalV2Status resolve(
        const HalV2BufferReference& reference,
        std::span<std::byte>& bytes) noexcept {
        if (reference.buffer_token == 0 ||
            reference.buffer_token > config.buffer_capacity) {
            return HalV2Status::invalid_argument;
        }
        const auto& buffer = buffers[reference.buffer_token - 1];
        if (!buffer.occupied) {
            return HalV2Status::invalid_state;
        }
        if (add_overflows(reference.offset, reference.bytes) ||
            reference.offset + reference.bytes > buffer.bytes) {
            return HalV2Status::resource_exhausted;
        }
        bytes = std::span<std::byte>(
            static_cast<std::byte*>(buffer.data) + reference.offset,
            static_cast<std::size_t>(reference.bytes));
        return HalV2Status::ok;
    }

    HalV2Status execute(
        const DeviceCommandBatch& batch,
        std::uint64_t completion_timestamp,
        SampledIoLoopbackFault fault) noexcept {
        for (std::size_t index = 0; index < batch.command_count; ++index) {
            const auto& command = batch.commands[index];
            const auto* selected = route(command.opcode);
            if (!selected) {
                return HalV2Status::unsupported;
            }
            if (command.kind != static_cast<std::uint32_t>(
                    HalV2CommandKind::dispatch) || command.buffer_count != 2) {
                return HalV2Status::invalid_argument;
            }
            std::span<std::byte> source;
            std::span<std::byte> destination;
            const auto source_status = resolve(command.buffers[0], source);
            if (source_status != HalV2Status::ok) {
                return source_status;
            }
            const auto destination_status = resolve(
                command.buffers[1], destination);
            if (destination_status != HalV2Status::ok) {
                return destination_status;
            }
            if (source.size() != destination.size() ||
                source.size() < sizeof(SampledIoFrameHeader)) {
                return HalV2Status::resource_exhausted;
            }
            SampledIoFrameHeader header{};
            std::memcpy(&header, source.data(), sizeof(header));
            const auto payload = std::span<const std::byte>(source).subspan(
                sizeof(SampledIoFrameHeader));
            if (header.struct_size != sizeof(header) ||
                header.version != sampled_io_frame_version) {
                return HalV2Status::reset_required;
            }
            if (header.channel_identity !=
                selected->source_channel_identity) {
                return HalV2Status::error;
            }
            if (header.payload_checksum !=
                sampled_io_payload_checksum(payload)) {
                return HalV2Status::error;
            }
            SampledIoFrameHeader destination_template{};
            std::memcpy(
                &destination_template,
                destination.data(),
                sizeof(destination_template));
            // A Runtime-dispatched device output carries its exact expected
            // release correlation in the destination slot. Standalone use
            // without that template preserves the source correlation.
            const bool has_runtime_correlation =
                destination_template.struct_size ==
                    sizeof(destination_template) &&
                destination_template.version == sampled_io_frame_version &&
                destination_template.channel_identity ==
                    selected->destination_channel_identity &&
                destination_template.sample_count == header.sample_count &&
                destination_template.encoding == header.encoding &&
                destination_template.timestamp_domain_identity ==
                    selected->destination_timestamp_domain_identity &&
                destination_template.sample_interval_ns ==
                    header.sample_interval_ns &&
                destination_template.trigger_identity ==
                    selected->destination_trigger_identity &&
                destination_template.calibration_identity ==
                    selected->destination_calibration_identity &&
                destination_template.status == static_cast<std::uint32_t>(
                    SampledIoFrameStatus::produced) &&
                destination_template.sequence != 0 &&
                destination_template.release_generation != 0 &&
                destination_template.trigger_sequence ==
                    destination_template.sequence;
            std::copy(source.begin(), source.end(), destination.begin());
            header.channel_identity = selected->destination_channel_identity;
            header.timestamp_domain_identity =
                selected->destination_timestamp_domain_identity;
            header.first_sample_timestamp = completion_timestamp;
            header.calibration_identity =
                selected->destination_calibration_identity;
            header.trigger_identity = selected->destination_trigger_identity;
            header.status = static_cast<std::uint32_t>(
                SampledIoFrameStatus::produced);
            if (has_runtime_correlation) {
                header.sequence = destination_template.sequence;
                header.release_generation =
                    destination_template.release_generation;
                header.trigger_sequence =
                    destination_template.trigger_sequence;
            }
            if (fault == SampledIoLoopbackFault::malformed_sequence) {
                ++header.sequence;
            }
            std::memcpy(destination.data(), &header, sizeof(header));
            frames_copied.fetch_add(1, std::memory_order_relaxed);
        }
        return HalV2Status::ok;
    }

    static HalV2Status submit_batch(
        void* instance, const DeviceCommandBatch* batch) noexcept {
        auto* backend = self(instance);
        if (!backend || !batch || !backend->initialized.load(
                std::memory_order_acquire) ||
            !backend->accepting.load(std::memory_order_acquire)) {
            return HalV2Status::invalid_state;
        }
        const auto fault = backend->next_fault.exchange(
            SampledIoLoopbackFault::none, std::memory_order_acq_rel);
        if (fault == SampledIoLoopbackFault::reject_submission) {
            backend->rejected.fetch_add(1, std::memory_order_relaxed);
            backend->record_logical_action(
                *batch, fault, HalV2Status::queue_full, 0);
            return HalV2Status::queue_full;
        }
        Slot* slot = nullptr;
        for (std::size_t index = 0;
             index < backend->config.queue_capacity; ++index) {
            auto expected = kFree;
            if (backend->slots[index].state.compare_exchange_strong(
                    expected, kOwned, std::memory_order_acq_rel)) {
                slot = &backend->slots[index];
                break;
            }
        }
        if (!slot) {
            backend->rejected.fetch_add(1, std::memory_order_relaxed);
            backend->record_logical_action(
                *batch, fault, HalV2Status::queue_full, 0);
            return HalV2Status::queue_full;
        }
        HalV2BatchCompletion completion{};
        completion.batch_id = batch->batch_id;
        completion.signal_count = batch->signal_count;
        completion.timestamp_domain_identity =
            backend->config.timestamp_domain_identity;
        completion.device_timestamp = backend->timestamp.fetch_add(
            1, std::memory_order_relaxed) + 1;
        std::copy_n(
            batch->signals.begin(), batch->signal_count,
            completion.signals.begin());
        const auto execution_status = backend->execute(
            *batch, completion.device_timestamp, fault);
        const auto completion_status =
            fault == SampledIoLoopbackFault::completion_error
            ? HalV2Status::error
            : fault == SampledIoLoopbackFault::completion_lost
                ? HalV2Status::lost
                : execution_status;
        completion.status = static_cast<std::int32_t>(completion_status);
        backend->record_logical_action(
            *batch,
            fault,
            completion_status,
            completion.device_timestamp);
        slot->completion = completion;
        backend->submissions.fetch_add(1, std::memory_order_relaxed);
        slot->state.store(
            fault == SampledIoLoopbackFault::completion_timeout ? kHeld : kReady,
            std::memory_order_release);
        return HalV2Status::ok;
    }

    static HalV2Status poll_batches(
        void* instance, HalV2BatchCompletion* output,
        std::uint64_t capacity, std::uint64_t* count) noexcept {
        auto* backend = self(instance);
        if (!backend || !count || (capacity != 0 && !output)) {
            return HalV2Status::invalid_argument;
        }
        *count = 0;
        for (std::size_t index = 0;
             index < backend->config.queue_capacity && *count < capacity;
             ++index) {
            auto expected = kReady;
            if (backend->slots[index].state.compare_exchange_strong(
                    expected, kOwned, std::memory_order_acq_rel)) {
                output[*count] = backend->slots[index].completion;
                ++*count;
                backend->completions.fetch_add(1, std::memory_order_relaxed);
                backend->slots[index].state.store(
                    kFree, std::memory_order_release);
            }
        }
        return HalV2Status::ok;
    }

    static HalV2Status cancel_batch(
        void* instance, std::uint64_t batch_id) noexcept {
        auto* backend = self(instance);
        if (!backend || batch_id == 0) {
            return HalV2Status::invalid_argument;
        }
        for (std::size_t index = 0;
             index < backend->config.queue_capacity; ++index) {
            auto& slot = backend->slots[index];
            auto state = slot.state.load(std::memory_order_acquire);
            if (state != kReady && state != kHeld) {
                continue;
            }
            const auto published_state = state;
            if (!slot.state.compare_exchange_strong(
                    state, kOwned, std::memory_order_acq_rel)) {
                continue;
            }
            // Check identity only after ownership: the slot may have been
            // retired and reused since our initial state observation.
            if (slot.completion.batch_id != batch_id) {
                slot.state.store(published_state, std::memory_order_release);
                continue;
            }
            const auto canceled = static_cast<std::int32_t>(HalV2Status::canceled);
            if (slot.completion.status != canceled) {
                slot.completion.status = canceled;
                backend->cancellations.fetch_add(1, std::memory_order_relaxed);
            }
            slot.state.store(kReady, std::memory_order_release);
            return HalV2Status::ok;
        }
        return HalV2Status::invalid_argument;
    }

    static HalV2Status request_stop(void* instance) noexcept {
        auto* backend = self(instance);
        if (!backend) {
            return HalV2Status::invalid_argument;
        }
        backend->accepting.store(false, std::memory_order_release);
        return HalV2Status::ok;
    }

    SampledIoLoopbackConfig config{};
    bool valid_config = false;
    std::array<Buffer, sampled_io_loopback_capacity> buffers{};
    std::array<Slot, sampled_io_loopback_capacity> slots{};
    std::array<SampledIoLoopbackRoute, sampled_io_loopback_capacity> routes{};
    std::size_t route_count = 0;
    bool registration_published = false;
    std::atomic<bool> initialized{false};
    std::atomic<bool> accepting{false};
    std::atomic<std::uint64_t> timestamp{0};
    std::atomic<std::uint64_t> submissions{0};
    std::atomic<std::uint64_t> completions{0};
    std::atomic<std::uint64_t> cancellations{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> frames_copied{0};
    std::array<LogicalActionSlot, sampled_io_loopback_capacity>
        logical_actions{};
    std::atomic<std::uint64_t> logical_action_sequence{0};
    std::atomic<SampledIoLoopbackFault> next_fault{
        SampledIoLoopbackFault::none};
    HalV2BackendApi api{};
    HalV2MemoryTopologyExtension memory{};
    HalV2CommandTimelineExtension commands{};
};

SampledIoLoopbackBackend::SampledIoLoopbackBackend(
    const SampledIoLoopbackConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

SampledIoLoopbackBackend::~SampledIoLoopbackBackend() = default;
SampledIoLoopbackBackend::SampledIoLoopbackBackend(
    SampledIoLoopbackBackend&&) noexcept = default;
SampledIoLoopbackBackend& SampledIoLoopbackBackend::operator=(
    SampledIoLoopbackBackend&&) noexcept = default;

Status SampledIoLoopbackBackend::add_route(
    const SampledIoLoopbackRoute& route) noexcept {
    if (!impl_ || !impl_->valid_config || impl_->registration_published ||
        impl_->initialized.load(std::memory_order_acquire)) {
        return Status::invalid_state;
    }
    if (route.opcode == 0 || route.source_channel_identity == 0 ||
        route.destination_channel_identity == 0 ||
        route.destination_timestamp_domain_identity == 0 ||
        route.destination_calibration_identity == 0 ||
        route.destination_trigger_identity == 0) {
        return Status::invalid_argument;
    }
    if (impl_->route_count >= impl_->routes.size()) {
        return Status::capacity_exceeded;
    }
    for (std::size_t index = 0; index < impl_->route_count; ++index) {
        if (impl_->routes[index].opcode == route.opcode) {
            return Status::invalid_argument;
        }
    }
    impl_->routes[impl_->route_count++] = route;
    return Status::ok;
}

Status SampledIoLoopbackBackend::inject_next(
    SampledIoLoopbackFault fault) noexcept {
    if (!impl_ || fault == SampledIoLoopbackFault::none ||
        fault > SampledIoLoopbackFault::completion_lost) {
        return Status::invalid_argument;
    }
    auto expected = SampledIoLoopbackFault::none;
    return impl_->next_fault.compare_exchange_strong(
        expected, fault, std::memory_order_acq_rel)
        ? Status::ok
        : Status::invalid_state;
}

HalV2BackendRegistration SampledIoLoopbackBackend::hal_v2_registration(
    std::string_view name) noexcept {
    HalV2BackendRegistration registration{};
    registration.name = name;
    if (impl_ && impl_->valid_config) {
        impl_->registration_published = true;
        registration.api = impl_->api;
        registration.memory_topology = &impl_->memory;
        registration.command_timeline = &impl_->commands;
    }
    return registration;
}

SampledIoLoopbackStats SampledIoLoopbackBackend::stats() const noexcept {
    if (!impl_) {
        return {};
    }
    return {
        impl_->submissions.load(std::memory_order_relaxed),
        impl_->completions.load(std::memory_order_relaxed),
        impl_->cancellations.load(std::memory_order_relaxed),
        impl_->rejected.load(std::memory_order_relaxed),
        impl_->frames_copied.load(std::memory_order_relaxed),
        impl_->logical_action_sequence.load(std::memory_order_relaxed),
    };
}

Status SampledIoLoopbackBackend::reset_logical_actions() noexcept {
    if (!impl_ || impl_->initialized.load(std::memory_order_acquire)) {
        return Status::invalid_state;
    }
    for (auto& slot : impl_->logical_actions) {
        slot.committed.store(
            std::numeric_limits<std::uint64_t>::max(),
            std::memory_order_release);
        slot.writer.clear(std::memory_order_release);
    }
    impl_->logical_action_sequence.store(0, std::memory_order_release);
    impl_->timestamp.store(0, std::memory_order_release);
    impl_->next_fault.store(
        SampledIoLoopbackFault::none,
        std::memory_order_release);
    return Status::ok;
}

std::size_t SampledIoLoopbackBackend::logical_action_count() const noexcept {
    if (!impl_) {
        return 0;
    }
    const auto count = impl_->logical_action_sequence.load(
        std::memory_order_acquire);
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        count, sampled_io_loopback_capacity));
}

bool SampledIoLoopbackBackend::logical_action_at(
    std::size_t chronological_index,
    SampledIoLoopbackLogicalAction& action) const noexcept {
    action = {};
    if (!impl_) {
        return false;
    }
    const auto end = impl_->logical_action_sequence.load(
        std::memory_order_acquire);
    const auto retained = std::min<std::uint64_t>(
        end, sampled_io_loopback_capacity);
    if (chronological_index >= retained) {
        return false;
    }
    const auto first = end - retained;
    return impl_->read_logical_action(
        first + chronological_index, action);
}

} // namespace rt
