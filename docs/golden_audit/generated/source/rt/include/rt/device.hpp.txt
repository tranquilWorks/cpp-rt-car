#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include <rt/device_abi.h>

namespace rt {

inline constexpr std::uint64_t invalid_device_handle =
    std::numeric_limits<std::uint64_t>::max();
inline constexpr std::uint32_t device_buffer_kind_bit =
    std::uint32_t{1} << 31u;
inline constexpr std::uint32_t device_handle_index_mask =
    device_buffer_kind_bit - 1u;

inline constexpr std::uint32_t hal_v2_api_version = 2u;
inline constexpr std::size_t hal_v2_identifier_capacity =
    RTFW_DEVICE_IDENTIFIER_CAPACITY;
inline constexpr std::size_t hal_v2_inline_payload_capacity =
    RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY;
inline constexpr std::size_t hal_v2_buffer_ref_capacity =
    RTFW_DEVICE_BUFFER_REF_CAPACITY;
inline constexpr std::uint32_t hal_v2_memory_topology_extension_version = 1u;
inline constexpr std::size_t hal_v2_memory_domain_capacity = 16u;
inline constexpr std::size_t hal_v2_topology_node_capacity = 32u;
inline constexpr std::size_t hal_v2_topology_link_capacity = 64u;
inline constexpr std::size_t hal_v2_timestamp_domain_capacity = 8u;
inline constexpr std::size_t hal_v2_opaque_handle_capacity = 64u;
inline constexpr std::uint32_t hal_v2_command_timeline_extension_version = 1u;
inline constexpr std::size_t hal_v2_command_capacity = 16u;
inline constexpr std::size_t hal_v2_timeline_wait_capacity = 8u;
inline constexpr std::size_t hal_v2_timeline_signal_capacity = 8u;
inline constexpr std::size_t hal_v2_timeline_capacity = 16u;

enum class HalV2Status : std::int32_t {
    ok = 0,
    invalid_argument = -1,
    invalid_state = -2,
    queue_full = -3,
    timeout = -4,
    error = -5,
    lost = -6,
    canceled = -7,
    unsupported = -8,
    resource_exhausted = -9,
    internal_error = -10,
    reset_required = -11,
};

enum class HalV2HealthState : std::uint32_t {
    shutdown = 0,
    healthy = 1,
    degraded = 2,
    reset_required = 3,
    lost = 4,
};

struct HalV2Capabilities {
    std::uint32_t struct_size = sizeof(HalV2Capabilities);
    std::uint32_t api_version = hal_v2_api_version;
    std::uint64_t max_in_flight = 0;
    std::uint64_t max_registered_buffers = 0;
    std::uint64_t max_buffer_bytes = 0;
    std::uint32_t inline_payload_capacity =
        hal_v2_inline_payload_capacity;
    std::uint32_t buffer_ref_capacity =
        hal_v2_buffer_ref_capacity;
    std::uint8_t supports_cancel = 0;
    std::uint8_t supports_reset = 0;
    std::uint8_t deterministic_mock = 0;
    std::uint8_t reserved0 = 0;
    std::array<char, hal_v2_identifier_capacity> backend_id{};
    std::uint64_t control_storage_bytes = 0;
    std::array<std::uint64_t, 4> reserved{};
};

struct HalV2InitializeConfig {
    std::uint32_t struct_size = sizeof(HalV2InitializeConfig);
    std::uint32_t api_version = hal_v2_api_version;
    std::uint64_t requested_in_flight = 0;
    std::uint64_t requested_registered_buffers = 0;
    std::array<std::uint64_t, 4> reserved{};
};

struct HalV2BufferRegistration {
    std::uint32_t struct_size = sizeof(HalV2BufferRegistration);
    std::uint32_t flags = 0;
    void* data = nullptr;
    std::uint64_t bytes = 0;
    std::array<char, hal_v2_identifier_capacity> name{};
    std::array<std::uint64_t, 4> reserved{};
};

struct HalV2BufferReference {
    std::uint64_t buffer_token = 0;
    std::uint32_t access = 0;
    std::uint32_t reserved0 = 0;
    std::uint64_t offset = 0;
    std::uint64_t bytes = 0;
};

struct HalV2Submission {
    std::uint32_t struct_size = sizeof(HalV2Submission);
    std::uint32_t api_version = hal_v2_api_version;
    std::uint64_t submission_id = 0;
    std::uint64_t frame_index = 0;
    std::uint64_t timeout_ns = 0;
    std::uint32_t opcode = 0;
    std::uint32_t flags = 0;
    std::uint32_t payload_size = 0;
    std::uint32_t buffer_count = 0;
    std::array<std::uint8_t, hal_v2_inline_payload_capacity> payload{};
    std::array<HalV2BufferReference, hal_v2_buffer_ref_capacity> buffers{};
    std::array<std::uint64_t, 4> reserved{};
};

struct HalV2Completion {
    std::uint32_t struct_size = sizeof(HalV2Completion);
    std::int32_t status = static_cast<std::int32_t>(HalV2Status::ok);
    std::uint64_t submission_id = 0;
    std::uint64_t device_timestamp_ns = 0;
    std::uint64_t value = 0;
    std::array<std::uint64_t, 4> reserved{};
};

struct HalV2Health {
    std::uint32_t struct_size = sizeof(HalV2Health);
    std::uint32_t state =
        static_cast<std::uint32_t>(HalV2HealthState::shutdown);
    std::int32_t last_status = static_cast<std::int32_t>(HalV2Status::ok);
    std::uint32_t reserved0 = 0;
    std::uint64_t generation = 0;
    std::uint64_t submissions = 0;
    std::uint64_t completions = 0;
    std::uint64_t queue_rejections = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t errors = 0;
    std::uint64_t losses = 0;
    std::uint64_t cancellations = 0;
    std::uint64_t resets = 0;
    std::uint64_t outstanding = 0;
    std::array<std::uint64_t, 4> reserved{};
};

using HalV2GetCapabilitiesFn = HalV2Status (*)(
    void*, HalV2Capabilities*);
using HalV2InitializeFn = HalV2Status (*)(
    void*, const HalV2InitializeConfig*);
using HalV2RegisterBufferFn = HalV2Status (*)(
    void*, const HalV2BufferRegistration*, std::uint64_t*);
using HalV2UnregisterBufferFn = HalV2Status (*)(void*, std::uint64_t);
using HalV2SubmitFn = HalV2Status (*)(void*, const HalV2Submission*);
using HalV2PollFn = HalV2Status (*)(
    void*, HalV2Completion*, std::uint64_t, std::uint64_t*);
using HalV2CancelFn = HalV2Status (*)(void*, std::uint64_t);
using HalV2GetHealthFn = HalV2Status (*)(void*, HalV2Health*);
using HalV2ResetFn = HalV2Status (*)(void*);
using HalV2ShutdownFn = HalV2Status (*)(void*);

/*
 * The table is copied by Runtime. Its non-null instance remains borrowed
 * until checked stop succeeds. Operations must not throw. Submission and poll
 * are bounded and nonblocking, and poll must not invoke host callbacks.
 */
struct HalV2BackendApi {
    std::uint32_t struct_size = sizeof(HalV2BackendApi);
    std::uint32_t api_version = hal_v2_api_version;
    void* instance = nullptr;
    HalV2GetCapabilitiesFn get_capabilities = nullptr;
    HalV2InitializeFn initialize = nullptr;
    HalV2RegisterBufferFn register_buffer = nullptr;
    HalV2UnregisterBufferFn unregister_buffer = nullptr;
    HalV2SubmitFn submit = nullptr;
    HalV2PollFn poll = nullptr;
    HalV2CancelFn cancel = nullptr;
    HalV2GetHealthFn get_health = nullptr;
    HalV2ResetFn reset = nullptr;
    HalV2ShutdownFn shutdown = nullptr;
    std::array<std::uint64_t, 8> reserved{};
};

enum class HalV2MemoryDomainKind : std::uint32_t {
  host = 1,
  pinned_host = 2,
  cuda_device = 3,
  imported = 4,
  dma_mapped = 5,
  peer = 6,
};

enum class HalV2MemoryOwnership : std::uint32_t {
  borrowed_host = 1,
  borrowed_opaque = 2,
  backend = 3,
};

inline constexpr std::uint32_t hal_v2_memory_ownership_borrowed_host =
    std::uint32_t{1} << 0u;
inline constexpr std::uint32_t hal_v2_memory_ownership_borrowed_opaque =
    std::uint32_t{1} << 1u;
inline constexpr std::uint32_t hal_v2_memory_ownership_backend =
    std::uint32_t{1} << 2u;

enum class HalV2MemoryCoherency : std::uint32_t {
  host_coherent = 1,
  explicit_flush_invalidate = 2,
  staged_copy = 3,
  device_only = 4,
};

inline constexpr std::uint32_t hal_v2_memory_sync_none = 0;
inline constexpr std::uint32_t hal_v2_memory_sync_flush = std::uint32_t{1}
                                                          << 0u;
inline constexpr std::uint32_t hal_v2_memory_sync_invalidate = std::uint32_t{1}
                                                               << 1u;
inline constexpr std::uint32_t hal_v2_memory_sync_copy_to_device =
    std::uint32_t{1} << 2u;
inline constexpr std::uint32_t hal_v2_memory_sync_copy_from_device =
    std::uint32_t{1} << 3u;
inline constexpr std::uint32_t hal_v2_memory_sync_timeline = std::uint32_t{1}
                                                             << 4u;

enum class HalV2TopologyNodeKind : std::uint32_t {
  host = 1,
  numa = 2,
  device = 3,
  dma_endpoint = 4,
  peer_endpoint = 5,
};

enum class HalV2TopologyLinkKind : std::uint32_t {
  local = 1,
  host_access = 2,
  device_access = 3,
  dma = 4,
  peer = 5,
};

enum class HalV2TimestampDomainKind : std::uint32_t {
  runtime_monotonic = 1,
  backend_device = 2,
  external = 3,
};

struct HalV2OpaqueHandle {
  std::uint32_t struct_size = sizeof(HalV2OpaqueHandle);
  std::uint32_t size = 0;
  std::array<std::byte, hal_v2_opaque_handle_capacity> bytes{};
  std::array<std::uint64_t, 2> reserved{};
};

struct HalV2MemoryDomain {
  std::uint32_t struct_size = sizeof(HalV2MemoryDomain);
  std::uint32_t extension_version = hal_v2_memory_topology_extension_version;
  std::uint64_t identity = 0;
  std::uint32_t kind = 0;
  std::uint32_t ownership_modes = 0;
  std::uint64_t maximum_bytes = 0;
  std::uint64_t byte_granularity = 0;
  std::uint64_t alignment = 0;
  std::uint64_t offset_granularity = 0;
  std::uint32_t access = 0;
  std::uint32_t coherency = 0;
  std::uint32_t required_synchronization = 0;
  std::uint32_t reserved0 = 0;
  std::uint64_t topology_node_identity = 0;
  std::uint64_t timestamp_domain_identity = 0;
  std::array<std::uint64_t, 4> reserved{};
};

struct HalV2TopologyNode {
  std::uint32_t struct_size = sizeof(HalV2TopologyNode);
  std::uint32_t extension_version = hal_v2_memory_topology_extension_version;
  std::uint64_t identity = 0;
  std::uint32_t kind = 0;
  std::uint32_t reserved0 = 0;
  std::array<std::uint64_t, 4> reserved{};
};

struct HalV2TopologyLink {
  std::uint32_t struct_size = sizeof(HalV2TopologyLink);
  std::uint32_t extension_version = hal_v2_memory_topology_extension_version;
  std::uint64_t identity = 0;
  std::uint64_t source_node_identity = 0;
  std::uint64_t destination_node_identity = 0;
  std::uint32_t kind = 0;
  std::uint32_t reserved0 = 0;
  std::array<std::uint64_t, 4> reserved{};
};

struct HalV2TimestampDomain {
  std::uint32_t struct_size = sizeof(HalV2TimestampDomain);
  std::uint32_t extension_version = hal_v2_memory_topology_extension_version;
  std::uint64_t identity = 0;
  std::uint32_t kind = 0;
  std::uint32_t reserved0 = 0;
  std::uint64_t tick_numerator_ns = 0;
  std::uint64_t tick_denominator = 0;
  std::uint64_t wrap_ticks = 0;
  std::uint64_t correlation_destination_identity = 0;
  std::uint8_t monotonic = 0;
  std::uint8_t resets_on_backend_reset = 0;
  std::uint8_t supports_correlation = 0;
  std::uint8_t reserved1 = 0;
  std::uint32_t reserved2 = 0;
  std::array<std::uint64_t, 4> reserved{};
};

struct HalV2MemoryTopologySnapshot {
  std::uint32_t struct_size = sizeof(HalV2MemoryTopologySnapshot);
  std::uint32_t extension_version = hal_v2_memory_topology_extension_version;
  std::uint32_t memory_domain_count = 0;
  std::uint32_t topology_node_count = 0;
  std::uint32_t topology_link_count = 0;
  std::uint32_t timestamp_domain_count = 0;
  std::uint64_t completion_timestamp_domain_identity = 0;
  std::array<HalV2MemoryDomain, hal_v2_memory_domain_capacity> memory_domains{};
  std::array<HalV2TopologyNode, hal_v2_topology_node_capacity> topology_nodes{};
  std::array<HalV2TopologyLink, hal_v2_topology_link_capacity> topology_links{};
  std::array<HalV2TimestampDomain, hal_v2_timestamp_domain_capacity>
      timestamp_domains{};
  std::array<std::uint64_t, 8> reserved{};
};

struct HalV2MemoryRegistration {
  std::uint32_t struct_size = sizeof(HalV2MemoryRegistration);
  std::uint32_t extension_version = hal_v2_memory_topology_extension_version;
  std::uint64_t domain_identity = 0;
  std::uint64_t bytes = 0;
  std::uint32_t ownership = 0;
  std::uint32_t access = 0;
  std::uint32_t coherency = 0;
  std::uint32_t synchronization = 0;
  void *host_data = nullptr;
  HalV2OpaqueHandle opaque_handle{};
  std::array<char, hal_v2_identifier_capacity> name{};
  std::array<std::uint64_t, 4> reserved{};
};

struct HalV2MemoryToken {
  std::uint32_t struct_size = sizeof(HalV2MemoryToken);
  std::uint32_t extension_version = hal_v2_memory_topology_extension_version;
  std::uint64_t submission_token = 0;
  HalV2OpaqueHandle native_token{};
  std::array<std::uint64_t, 4> reserved{};
};

struct HalV2TimestampCorrelationQuery {
  std::uint32_t struct_size = sizeof(HalV2TimestampCorrelationQuery);
  std::uint32_t extension_version = hal_v2_memory_topology_extension_version;
  std::uint64_t source_domain_identity = 0;
  std::uint64_t destination_domain_identity = 0;
  std::array<std::uint64_t, 4> reserved{};
};

struct HalV2TimestampCorrelation {
  std::uint32_t struct_size = sizeof(HalV2TimestampCorrelation);
  std::uint32_t extension_version = hal_v2_memory_topology_extension_version;
  std::uint64_t source_domain_identity = 0;
  std::uint64_t destination_domain_identity = 0;
  std::uint64_t generation = 0;
  std::uint64_t source_value = 0;
  std::uint64_t destination_value = 0;
  std::uint64_t uncertainty_ns = 0;
  std::array<std::uint64_t, 4> reserved{};
};

using HalV2DiscoverMemoryTopologyFn =
    HalV2Status (*)(void *, HalV2MemoryTopologySnapshot *);
using HalV2RegisterMemoryFn = HalV2Status (*)(void *,
                                              const HalV2MemoryRegistration *,
                                              HalV2MemoryToken *);
using HalV2UnregisterMemoryFn = HalV2Status (*)(void *,
                                                const HalV2MemoryRegistration *,
                                                const HalV2MemoryToken *);
using HalV2QueryTimestampCorrelationFn =
    HalV2Status (*)(void *, const HalV2TimestampCorrelationQuery *,
                    HalV2TimestampCorrelation *);

struct HalV2MemoryTopologyExtension {
  std::uint32_t struct_size = sizeof(HalV2MemoryTopologyExtension);
  std::uint32_t extension_version = hal_v2_memory_topology_extension_version;
  void *instance = nullptr;
  HalV2DiscoverMemoryTopologyFn discover = nullptr;
  HalV2RegisterMemoryFn register_memory = nullptr;
  HalV2UnregisterMemoryFn unregister_memory = nullptr;
  HalV2QueryTimestampCorrelationFn query_timestamp_correlation = nullptr;
  std::array<std::uint64_t, 8> reserved{};
};

enum class HalV2CommandKind : std::uint32_t {
  invalid = 0,
  dispatch = 1,
  copy = 2,
  memory_synchronization = 3,
};

enum class HalV2MemoryOperation : std::uint32_t {
  invalid = 0,
  copy_to_device = 1,
  copy_from_device = 2,
  flush = 3,
  invalidate = 4,
};

struct DeviceTimelineHandle {
  std::uint64_t value = invalid_device_handle;

  constexpr DeviceTimelineHandle() noexcept = default;
  explicit constexpr DeviceTimelineHandle(std::uint64_t encoded) noexcept
      : value(encoded) {}
  constexpr DeviceTimelineHandle(std::uint32_t owner,
                                 std::uint32_t index) noexcept
      : value((static_cast<std::uint64_t>(owner) << 32u) |
              static_cast<std::uint64_t>(index)) {}

  [[nodiscard]] constexpr bool valid() const noexcept {
    return value != invalid_device_handle;
  }
  [[nodiscard]] constexpr std::uint32_t owner() const noexcept {
    return static_cast<std::uint32_t>(value >> 32u);
  }
  [[nodiscard]] constexpr std::uint32_t index() const noexcept {
    return static_cast<std::uint32_t>(value);
  }
  friend constexpr bool operator==(DeviceTimelineHandle,
                                   DeviceTimelineHandle) noexcept = default;
};

struct HalV2TimelinePoint {
  std::uint32_t struct_size = sizeof(HalV2TimelinePoint);
  std::uint32_t extension_version =
      hal_v2_command_timeline_extension_version;
  std::uint64_t timeline_handle = invalid_device_handle;
  std::uint64_t value = 0;
  std::array<std::uint64_t, 3> reserved{};
};

struct DeviceCommand {
  std::uint32_t struct_size = sizeof(DeviceCommand);
  std::uint32_t extension_version =
      hal_v2_command_timeline_extension_version;
  std::uint32_t kind = static_cast<std::uint32_t>(HalV2CommandKind::invalid);
  std::uint32_t operation =
      static_cast<std::uint32_t>(HalV2MemoryOperation::invalid);
  std::uint32_t opcode = 0;
  std::uint32_t flags = 0;
  std::uint32_t payload_size = 0;
  std::uint32_t buffer_count = 0;
  std::array<std::uint8_t, hal_v2_inline_payload_capacity> payload{};
  std::array<HalV2BufferReference, hal_v2_buffer_ref_capacity> buffers{};
  HalV2BufferReference source{};
  HalV2BufferReference destination{};
  HalV2BufferReference target{};
  std::array<std::uint64_t, 4> reserved{};
};

struct DeviceCommandBatch {
  std::uint32_t struct_size = sizeof(DeviceCommandBatch);
  std::uint32_t extension_version =
      hal_v2_command_timeline_extension_version;
  std::uint64_t batch_id = 0;
  std::uint64_t frame_index = 0;
  std::uint64_t timeout_ns = 0;
  std::uint32_t command_count = 0;
  std::uint32_t wait_count = 0;
  std::uint32_t signal_count = 0;
  std::uint32_t reserved0 = 0;
  std::array<DeviceCommand, hal_v2_command_capacity> commands{};
  std::array<HalV2TimelinePoint, hal_v2_timeline_wait_capacity> waits{};
  std::array<HalV2TimelinePoint, hal_v2_timeline_signal_capacity> signals{};
  std::array<std::uint64_t, 8> reserved{};
};

struct HalV2BatchCompletion {
  std::uint32_t struct_size = sizeof(HalV2BatchCompletion);
  std::uint32_t extension_version =
      hal_v2_command_timeline_extension_version;
  std::int32_t status = static_cast<std::int32_t>(HalV2Status::ok);
  std::uint32_t signal_count = 0;
  std::uint64_t batch_id = 0;
  std::uint64_t device_timestamp = 0;
  std::uint64_t timestamp_domain_identity = 0;
  std::array<HalV2TimelinePoint, hal_v2_timeline_signal_capacity> signals{};
  std::array<std::uint64_t, 8> reserved{};
};

struct HalV2CommandTimelineCapabilities {
  std::uint32_t struct_size = sizeof(HalV2CommandTimelineCapabilities);
  std::uint32_t extension_version =
      hal_v2_command_timeline_extension_version;
  std::uint32_t max_in_flight_batches = 0;
  std::uint32_t max_commands_per_batch = 0;
  std::uint32_t max_wait_points = 0;
  std::uint32_t max_signal_points = 0;
  std::uint32_t max_timelines = 0;
  std::uint32_t completion_batch_capacity = 0;
  std::uint64_t backend_control_storage_bytes = 0;
  std::array<std::uint64_t, 8> reserved{};
};

using HalV2GetCommandTimelineCapabilitiesFn = HalV2Status (*)(
    void *, HalV2CommandTimelineCapabilities *);
using HalV2SubmitCommandBatchFn = HalV2Status (*)(
    void *, const DeviceCommandBatch *);
using HalV2PollBatchCompletionsFn = HalV2Status (*)(
    void *, HalV2BatchCompletion *, std::uint64_t, std::uint64_t *);
using HalV2CancelCommandBatchFn = HalV2Status (*)(void *, std::uint64_t);
using HalV2RequestCommandStopFn = HalV2Status (*)(void *);

struct HalV2CommandTimelineExtension {
  std::uint32_t struct_size = sizeof(HalV2CommandTimelineExtension);
  std::uint32_t extension_version =
      hal_v2_command_timeline_extension_version;
  void *instance = nullptr;
  HalV2GetCommandTimelineCapabilitiesFn get_capabilities = nullptr;
  HalV2SubmitCommandBatchFn submit = nullptr;
  HalV2PollBatchCompletionsFn poll = nullptr;
  HalV2CancelCommandBatchFn cancel = nullptr;
  HalV2RequestCommandStopFn request_stop = nullptr;
  std::array<std::uint64_t, 8> reserved{};
};

struct HalV2BackendRegistration {
    std::string_view name;
    HalV2BackendApi api{};
    // Optional and borrowed only for this call. Runtime copies the table and
    // its complete validated discovery snapshot before publishing a handle.
    const HalV2MemoryTopologyExtension *memory_topology = nullptr;
    // Optional and borrowed only for this call. Runtime copies the complete
    // validated table and capabilities before publishing a backend handle.
    const HalV2CommandTimelineExtension *command_timeline = nullptr;
};

struct DeviceBackendHandle {
    std::uint64_t value = invalid_device_handle;

    constexpr DeviceBackendHandle() noexcept = default;
    explicit constexpr DeviceBackendHandle(
        std::uint64_t encoded) noexcept
        : value(encoded) {}
    constexpr DeviceBackendHandle(
        std::uint32_t owner,
        std::uint32_t index) noexcept
        : value(
              (static_cast<std::uint64_t>(owner) << 32u) |
              static_cast<std::uint64_t>(index)) {}

    [[nodiscard]] constexpr bool valid() const noexcept {
        return value != invalid_device_handle &&
               (static_cast<std::uint32_t>(value) &
                device_buffer_kind_bit) == 0;
    }
    [[nodiscard]] constexpr std::uint32_t owner() const noexcept {
        return static_cast<std::uint32_t>(value >> 32u);
    }
    [[nodiscard]] constexpr std::uint32_t index() const noexcept {
        return static_cast<std::uint32_t>(value) &
               device_handle_index_mask;
    }

    friend constexpr bool operator==(
        DeviceBackendHandle,
        DeviceBackendHandle) noexcept = default;
};

struct DeviceBufferHandle {
    std::uint64_t value = invalid_device_handle;

    constexpr DeviceBufferHandle() noexcept = default;
    explicit constexpr DeviceBufferHandle(
        std::uint64_t encoded) noexcept
        : value(encoded) {}
    constexpr DeviceBufferHandle(
        std::uint32_t owner,
        std::uint32_t index) noexcept
        : value(
              (static_cast<std::uint64_t>(owner) << 32u) |
              device_buffer_kind_bit |
              static_cast<std::uint64_t>(index)) {}

    [[nodiscard]] constexpr bool valid() const noexcept {
        return value != invalid_device_handle &&
               (static_cast<std::uint32_t>(value) &
                device_buffer_kind_bit) != 0;
    }
    [[nodiscard]] constexpr std::uint32_t owner() const noexcept {
        return static_cast<std::uint32_t>(value >> 32u);
    }
    [[nodiscard]] constexpr std::uint32_t index() const noexcept {
        return static_cast<std::uint32_t>(value) &
               device_handle_index_mask;
    }

    friend constexpr bool operator==(
        DeviceBufferHandle,
        DeviceBufferHandle) noexcept = default;
};

struct DeviceBackendRegistration {
    std::string_view name;
    rtfw_device_backend_api api{};
};

struct DeviceBufferRegistration {
    std::string_view name;
    DeviceBackendHandle backend{};
    std::span<std::byte> storage{};
    rtfw_device_buffer_flags flags =
        RTFW_DEVICE_BUFFER_HOST_READ |
        RTFW_DEVICE_BUFFER_HOST_WRITE |
        RTFW_DEVICE_BUFFER_DEVICE_READ |
        RTFW_DEVICE_BUFFER_DEVICE_WRITE;
};

struct DeviceMemoryDomainHandle {
  DeviceBackendHandle backend{};
  std::uint64_t identity = 0;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return backend.valid() && identity != 0;
  }
  friend constexpr bool operator==(DeviceMemoryDomainHandle,
                                   DeviceMemoryDomainHandle) noexcept = default;
};

struct DeviceTopologyNodeHandle {
  DeviceBackendHandle backend{};
  std::uint64_t identity = 0;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return backend.valid() && identity != 0;
  }
  friend constexpr bool operator==(DeviceTopologyNodeHandle,
                                   DeviceTopologyNodeHandle) noexcept = default;
};

struct DeviceTopologyLinkHandle {
  DeviceBackendHandle backend{};
  std::uint64_t identity = 0;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return backend.valid() && identity != 0;
  }
  friend constexpr bool operator==(DeviceTopologyLinkHandle,
                                   DeviceTopologyLinkHandle) noexcept = default;
};

struct DeviceTimestampDomainHandle {
  DeviceBackendHandle backend{};
  std::uint64_t identity = 0;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return backend.valid() && identity != 0;
  }
  friend constexpr bool
  operator==(DeviceTimestampDomainHandle,
             DeviceTimestampDomainHandle) noexcept = default;
};

struct HeterogeneousDeviceBufferRegistration {
  std::string_view name;
  DeviceBackendHandle backend{};
  DeviceMemoryDomainHandle domain{};
  std::span<std::byte> host_storage{};
  HalV2OpaqueHandle opaque_handle{};
  std::uint64_t bytes = 0;
  HalV2MemoryOwnership ownership = HalV2MemoryOwnership::borrowed_host;
  std::uint32_t access = 0;
  HalV2MemoryCoherency coherency = HalV2MemoryCoherency::host_coherent;
  std::uint32_t synchronization = hal_v2_memory_sync_none;
};

struct DeviceMemoryObjectInfo {
  DeviceBufferHandle buffer{};
  DeviceBackendHandle backend{};
  DeviceMemoryDomainHandle domain{};
  std::uint64_t bytes = 0;
  std::uint32_t ownership = 0;
  std::uint32_t access = 0;
  std::uint32_t coherency = 0;
  std::uint32_t synchronization = 0;
  std::uint8_t heterogeneous = 0;
  std::uint8_t host_addressable = 0;
  std::uint16_t reserved0 = 0;
  std::uint32_t opaque_handle_size = 0;
  std::array<std::uint64_t, 4> reserved{};
};

using DeviceSubmission = rtfw_device_submission;
using DeviceCapabilities = rtfw_device_capabilities;
using DeviceHealth = rtfw_device_health;

[[nodiscard]] inline DeviceSubmission make_device_submission() noexcept {
    DeviceSubmission submission{};
    submission.struct_size = sizeof(submission);
    submission.abi_version = RTFW_DEVICE_ABI_VERSION;
    return submission;
}

[[nodiscard]] inline DeviceHealth make_device_health() noexcept {
    DeviceHealth health{};
    health.struct_size = sizeof(health);
    return health;
}

} // namespace rt
