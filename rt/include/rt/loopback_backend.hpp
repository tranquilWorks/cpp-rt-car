#pragma once

#include <rt/runtime.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace rt {

inline constexpr std::size_t sampled_io_loopback_capacity = 16;

struct SampledIoLoopbackConfig {
    std::uint32_t queue_capacity = 8;
    std::uint32_t buffer_capacity = 8;
    std::uint64_t maximum_buffer_bytes = 1u << 20u;
    std::uint64_t memory_domain_identity = 1;
    std::uint64_t timestamp_domain_identity = 1;
};

struct SampledIoLoopbackRoute {
    std::uint32_t opcode = 0;
    std::uint64_t source_channel_identity = 0;
    std::uint64_t destination_channel_identity = 0;
    std::uint64_t destination_timestamp_domain_identity = 0;
    std::uint64_t destination_calibration_identity = 0;
    std::uint64_t destination_trigger_identity = 0;
};

struct SampledIoLoopbackStats {
    std::uint64_t submissions = 0;
    std::uint64_t completions = 0;
    std::uint64_t cancellations = 0;
    std::uint64_t rejected = 0;
    std::uint64_t frames_copied = 0;
    std::uint64_t logical_actions = 0;
};

enum class SampledIoLoopbackFault : std::uint8_t {
    none = 0,
    reject_submission = 1,
    malformed_sequence = 2,
    completion_error = 3,
    completion_timeout = 4,
    completion_lost = 5,
};

struct SampledIoLoopbackLogicalAction {
    std::uint64_t sequence = 0;
    std::uint64_t batch_id = 0;
    std::uint64_t device_timestamp = 0;
    std::uint64_t content_identity = 0;
    std::uint32_t opcode = 0;
    std::int32_t status = 0;
    SampledIoLoopbackFault fault = SampledIoLoopbackFault::none;
    std::uint8_t command_count = 0;
    std::array<std::byte, 22> reserved{};
};

static_assert(sizeof(SampledIoLoopbackLogicalAction) == 64);

// A bounded, deterministic public HAL-v2 backend for sampled-I/O integration,
// examples, and portable tests. Each configured dispatch opcode copies buffer
// reference 0 to reference 1 and rewrites the sampled-frame destination
// identity fields. It performs no allocation after construction.
// Submit, poll and cancel use exclusive completion-slot ownership. Cancellation
// can claim only a published batch; an in-progress submit or competing consumer
// may make a bounded cancel scan return invalid_argument. A published injected
// timeout remains held until cancellation. Repeated cancellation before polling
// is idempotent. request_stop closes admission; callbacks already admitted may
// finish. The caller quiesces callbacks before buffer changes, reset, shutdown,
// reinitialization, move or destruction, as in Runtime's checked-stop lifecycle.
class SampledIoLoopbackBackend final {
public:
    explicit SampledIoLoopbackBackend(
        const SampledIoLoopbackConfig& config = {});
    ~SampledIoLoopbackBackend();

    SampledIoLoopbackBackend(const SampledIoLoopbackBackend&) = delete;
    SampledIoLoopbackBackend& operator=(
        const SampledIoLoopbackBackend&) = delete;
    SampledIoLoopbackBackend(SampledIoLoopbackBackend&&) noexcept;
    SampledIoLoopbackBackend& operator=(
        SampledIoLoopbackBackend&&) noexcept;

    [[nodiscard]] Status add_route(
        const SampledIoLoopbackRoute& route) noexcept;
    // One bounded atomic injection consumed by the next batch submission.
    [[nodiscard]] Status inject_next(
        SampledIoLoopbackFault fault) noexcept;
    [[nodiscard]] HalV2BackendRegistration hal_v2_registration(
        std::string_view name = "sampled_io.loopback") noexcept;
    [[nodiscard]] SampledIoLoopbackStats stats() const noexcept;
    // Logical replay hooks are bounded and instance-local. Reset is accepted
    // only after checked backend shutdown; inspection uses retained order.
    [[nodiscard]] Status reset_logical_actions() noexcept;
    [[nodiscard]] std::size_t logical_action_count() const noexcept;
    [[nodiscard]] bool logical_action_at(
        std::size_t chronological_index,
        SampledIoLoopbackLogicalAction& action) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rt
