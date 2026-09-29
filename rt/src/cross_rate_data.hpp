#pragma once

#include "rate_timeline.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace rt::detail {

struct CrossRateChannelSpec {
    std::string name;
    PhaseHandle producer{};
    PhaseHandle consumer{};
    std::size_t payload_size = 0;
    std::vector<std::byte> initial_sample;
    CrossRateMode mode = CrossRateMode::sample_and_hold;
    std::uint64_t maximum_age_ns =
        std::numeric_limits<std::uint64_t>::max();
    CrossRateDeviceEndpointSelector producer_device{};
    CrossRateDeviceEndpointSelector consumer_device{};
};

struct CompiledDeviceRatePlan;

struct CrossRateDeviceBufferSource {
    DeviceBufferHandle buffer{};
    DeviceBackendHandle backend{};
    std::span<std::byte> storage{};
    std::uint64_t bytes = 0;
    std::uint32_t access = 0;
    std::uint32_t coherency = 0;
    std::uint32_t synchronization = 0;
    std::uint64_t byte_granularity = 1;
    std::uint64_t offset_granularity = 1;
};

struct CompiledCrossRateDeviceEndpoint {
    std::size_t channel_index = 0;
    bool producer = false;
    PhaseHandle phase{};
    DeviceBackendHandle backend{};
    DeviceBufferHandle buffer{};
    DeviceRateReferenceKind reference_kind =
        DeviceRateReferenceKind::dispatch_buffer;
    std::uint32_t command_index = 0;
    std::uint32_t command_reference_index = 0;
    std::uint64_t envelope_offset = 0;
    std::uint64_t envelope_bytes = 0;
    std::uint64_t slot_stride_bytes = 0;
    std::uint32_t slot_count = 0;
    std::uint64_t timestamp_domain_identity = 0;
    std::span<std::byte> host_storage{};
};

enum class SnapshotStoreResult : std::uint8_t {
    ok,
    capacity_exceeded,
    not_ready,
    stale_generation,
    invalid_argument,
};

enum class SnapshotRetention : std::uint8_t {
    retain,
    retire,
};

struct SnapshotSlotControl {
    std::atomic<std::uint64_t> value{0};
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

// A fixed-capacity exact-generation SPSC store. One packed atomic word owns
// each slot through free/writing/published/reading states, so producer payload
// writes and consumer copies never overlap. Calls make one bounded claim and
// never spin, block, allocate, search, or substitute another generation.
class SnapshotStore {
public:
    SnapshotStore() = default;
    ~SnapshotStore() = default;

    SnapshotStore(SnapshotStore&&) noexcept = default;
    SnapshotStore& operator=(SnapshotStore&&) noexcept = default;
    SnapshotStore(const SnapshotStore&) = delete;
    SnapshotStore& operator=(const SnapshotStore&) = delete;

    [[nodiscard]] static Status create(
        std::size_t payload_size,
        std::size_t slot_count,
        SnapshotStore& output) noexcept;

    [[nodiscard]] SnapshotStoreResult publish(
        std::uint64_t generation,
        std::span<const std::byte> payload) noexcept;
    [[nodiscard]] SnapshotStoreResult copy(
        std::uint64_t generation,
        std::span<std::byte> output,
        SnapshotRetention retention) noexcept;
    [[nodiscard]] SnapshotStoreResult retire(
        std::uint64_t generation) noexcept;
    [[nodiscard]] bool can_publish(std::uint64_t generation) const noexcept;
    // Control-path rebuild used only after complete checkpoint validation and
    // outside execution. It performs no allocation and publishes the supplied
    // complete payload before returning.
    [[nodiscard]] SnapshotStoreResult restore_committed(
        std::uint64_t generation,
        std::uint64_t next_generation,
        std::span<const std::byte> payload) noexcept;

    [[nodiscard]] std::size_t payload_size() const noexcept {
        return payload_size_;
    }
    [[nodiscard]] std::size_t slot_count() const noexcept {
        return slot_count_;
    }
    [[nodiscard]] std::size_t payload_storage_bytes() const noexcept {
        return payload_storage_bytes_;
    }
    [[nodiscard]] const void* control_data() const noexcept {
        return controls_.get();
    }
    [[nodiscard]] std::size_t control_storage_bytes() const noexcept {
        return slot_count_ * sizeof(SnapshotSlotControl);
    }
    [[nodiscard]] const void* payload_data() const noexcept {
        return payload_.get();
    }
    [[nodiscard]] static constexpr std::uint64_t maximum_generation() noexcept {
        return std::numeric_limits<std::uint64_t>::max() >> 2u;
    }
    [[nodiscard]] std::uint64_t next_generation() const noexcept {
        return next_generation_;
    }

private:
    std::size_t payload_size_ = 0;
    std::size_t slot_count_ = 0;
    std::size_t payload_storage_bytes_ = 0;
    std::uint64_t next_generation_ = 1;
    std::unique_ptr<SnapshotSlotControl[]> controls_;
    std::unique_ptr<std::byte[]> payload_;
};

struct CompiledCrossRatePlan {
    std::vector<CompiledCrossRateChannel> channels;
    std::vector<CompiledCrossRateSelection> selections;
    std::vector<SnapshotStore> stores;
    std::vector<CompiledCrossRateDeviceEndpoint> device_endpoints;
    std::vector<std::size_t> device_endpoint_index_by_channel;
};

struct CrossRateCompileDiagnostic {
    Status status = Status::ok;
    const char* message = nullptr;
};

// Compiles immutable first-cycle and repeated-cycle selection records and
// constructs one preallocated store per channel. Output changes only after
// complete validation and construction succeeds.
[[nodiscard]] Status compile_cross_rate_data(
    std::uint32_t graph_owner,
    std::size_t phase_count,
    const CompiledRatePlan& rate_plan,
    std::span<const CrossRateChannelSpec> channels,
    const CompiledDeviceRatePlan* device_rate_plan,
    std::span<const CrossRateDeviceBufferSource> device_buffers,
    CompiledCrossRatePlan& output,
    CrossRateCompileDiagnostic& diagnostic,
    std::span<const std::size_t> snapshot_slot_counts = {}) noexcept;

} // namespace rt::detail
