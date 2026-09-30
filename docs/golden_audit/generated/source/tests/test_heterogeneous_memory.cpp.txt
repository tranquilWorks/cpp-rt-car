#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include <rt/runtime.hpp>

namespace {

template <typename Range> bool all_zero(const Range &values) {
  return std::all_of(values.begin(), values.end(),
                     [](const auto value) { return value == 0; });
}

template <typename Range>
void set_identifier(Range &output, std::string_view value) {
  output.fill(0);
  std::copy(value.begin(), value.end(), output.begin());
}

rt::RuntimeConfig memory_config(std::size_t backend_capacity = 1,
                                std::size_t buffer_capacity = 8) {
  rt::RuntimeConfig config;
  config.callback_capacity = 1;
  config.worker_count = 1;
  config.executor_queue_capacity = 4;
  config.task_scratch_slots = 4;
  config.trace_capacity = 32;
  config.device_backend_capacity = backend_capacity;
  config.device_buffer_capacity = buffer_capacity;
  config.device_outstanding_capacity = 4;
  config.device_completion_batch = 4;
  return config;
}

rt::HalV2MemoryTopologySnapshot six_domain_snapshot() {
  rt::HalV2MemoryTopologySnapshot snapshot;
  snapshot.memory_domain_count = 6;
  snapshot.topology_node_count = 6;
  snapshot.topology_link_count = 2;
  snapshot.timestamp_domain_count = 2;
  snapshot.completion_timestamp_domain_identity = 202;

  constexpr std::array<rt::HalV2TopologyNodeKind, 6> node_kinds{
      rt::HalV2TopologyNodeKind::host,
      rt::HalV2TopologyNodeKind::numa,
      rt::HalV2TopologyNodeKind::device,
      rt::HalV2TopologyNodeKind::dma_endpoint,
      rt::HalV2TopologyNodeKind::peer_endpoint,
      rt::HalV2TopologyNodeKind::peer_endpoint,
  };
  for (std::size_t index = 0; index < node_kinds.size(); ++index) {
    snapshot.topology_nodes[index].identity = 101 + index;
    snapshot.topology_nodes[index].kind =
        static_cast<std::uint32_t>(node_kinds[index]);
  }
  snapshot.topology_links[0].identity = 301;
  snapshot.topology_links[0].source_node_identity = 101;
  snapshot.topology_links[0].destination_node_identity = 101;
  snapshot.topology_links[0].kind =
      static_cast<std::uint32_t>(rt::HalV2TopologyLinkKind::local);
  snapshot.topology_links[1].identity = 302;
  snapshot.topology_links[1].source_node_identity = 105;
  snapshot.topology_links[1].destination_node_identity = 106;
  snapshot.topology_links[1].kind =
      static_cast<std::uint32_t>(rt::HalV2TopologyLinkKind::peer);

  auto &host_time = snapshot.timestamp_domains[0];
  host_time.identity = 201;
  host_time.kind = static_cast<std::uint32_t>(
      rt::HalV2TimestampDomainKind::runtime_monotonic);
  host_time.tick_numerator_ns = 1;
  host_time.tick_denominator = 1;
  host_time.monotonic = 1;

  auto &device_time = snapshot.timestamp_domains[1];
  device_time.identity = 202;
  device_time.kind =
      static_cast<std::uint32_t>(rt::HalV2TimestampDomainKind::backend_device);
  device_time.tick_numerator_ns = 1;
  device_time.tick_denominator = 1;
  device_time.wrap_ticks = std::uint64_t{1} << 48u;
  device_time.correlation_destination_identity = 201;
  device_time.monotonic = 1;
  device_time.resets_on_backend_reset = 1;
  device_time.supports_correlation = 1;

  constexpr std::array<rt::HalV2MemoryDomainKind, 6> domain_kinds{
      rt::HalV2MemoryDomainKind::host,
      rt::HalV2MemoryDomainKind::pinned_host,
      rt::HalV2MemoryDomainKind::cuda_device,
      rt::HalV2MemoryDomainKind::imported,
      rt::HalV2MemoryDomainKind::dma_mapped,
      rt::HalV2MemoryDomainKind::peer,
  };
  for (std::size_t index = 0; index < domain_kinds.size(); ++index) {
    auto &domain = snapshot.memory_domains[index];
    domain.identity = index + 1;
    domain.kind = static_cast<std::uint32_t>(domain_kinds[index]);
    domain.maximum_bytes = 4096;
    domain.byte_granularity = 8;
    domain.alignment = 8;
    domain.offset_granularity = 8;
    domain.topology_node_identity = 101 + index;
    domain.timestamp_domain_identity = index < 2 ? 201 : 202;
    if (index < 2) {
      domain.ownership_modes = rt::hal_v2_memory_ownership_borrowed_host;
      domain.access =
          RTFW_DEVICE_BUFFER_HOST_READ | RTFW_DEVICE_BUFFER_HOST_WRITE |
          RTFW_DEVICE_BUFFER_DEVICE_READ | RTFW_DEVICE_BUFFER_DEVICE_WRITE;
      domain.coherency =
          static_cast<std::uint32_t>(rt::HalV2MemoryCoherency::host_coherent);
    } else {
      domain.ownership_modes =
          index == 2 ? rt::hal_v2_memory_ownership_backend
                     : rt::hal_v2_memory_ownership_borrowed_opaque;
      domain.access =
          RTFW_DEVICE_BUFFER_DEVICE_READ | RTFW_DEVICE_BUFFER_DEVICE_WRITE;
      domain.coherency =
          static_cast<std::uint32_t>(rt::HalV2MemoryCoherency::device_only);
    }
  }
  return snapshot;
}

struct MemoryBackend {
  MemoryBackend() {
    set_identifier(capabilities.backend_id, "test.heterogeneous.v2");
  }

  rt::HalV2BackendApi api() {
    rt::HalV2BackendApi table;
    table.instance = this;
    table.get_capabilities = &get_capabilities;
    table.initialize = &initialize;
    table.register_buffer = &register_buffer;
    table.unregister_buffer = &unregister_buffer;
    table.submit = &submit;
    table.poll = &poll;
    table.cancel = &cancel;
    table.get_health = &get_health;
    table.reset = &reset;
    table.shutdown = &shutdown;
    return table;
  }

  rt::HalV2MemoryTopologyExtension extension() {
    rt::HalV2MemoryTopologyExtension table;
    table.instance = this;
    table.discover = &discover;
    table.register_memory = &register_memory;
    table.unregister_memory = &unregister_memory;
    table.query_timestamp_correlation = &query_correlation;
    return table;
  }

  rt::HalV2Capabilities capabilities = [] {
    rt::HalV2Capabilities value;
    value.max_in_flight = 8;
    value.max_registered_buffers = 16;
    value.max_buffer_bytes = 4096;
    value.supports_cancel = 1;
    value.supports_reset = 1;
    value.deterministic_mock = 1;
    return value;
  }();
  rt::HalV2MemoryTopologySnapshot snapshot = six_domain_snapshot();
  bool leave_discovery_untouched = false;
  bool partially_write_discovery = false;
  bool throw_discovery = false;
  bool throw_register = false;
  bool throw_correlation = false;
  std::size_t fail_registration_call = 0;
  bool failed_registration_has_empty_token = false;
  rt::HalV2Status unregister_status = rt::HalV2Status::ok;
  rt::HalV2Status correlation_status = rt::HalV2Status::ok;
  bool malformed_correlation = false;
  std::size_t discovery_calls = 0;
  std::size_t memory_register_calls = 0;
  std::size_t memory_unregister_calls = 0;
  std::size_t legacy_register_calls = 0;
  std::size_t legacy_unregister_calls = 0;
  std::size_t correlation_calls = 0;
  std::atomic<std::size_t> submit_calls{0};
  std::atomic<std::uint64_t> pending_submission{0};
  rt::HalV2Submission last_submission{};
  std::vector<char> order;
  std::vector<rt::HalV2MemoryRegistration> registrations;
  std::vector<rt::HalV2MemoryRegistration> unregistrations;
  std::vector<rt::HalV2MemoryToken> unregistration_tokens;

  static MemoryBackend *self(void *instance) {
    return static_cast<MemoryBackend *>(instance);
  }

  static rt::HalV2Status get_capabilities(void *instance,
                                          rt::HalV2Capabilities *output) {
    if (!instance || !output)
      return rt::HalV2Status::invalid_argument;
    *output = self(instance)->capabilities;
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status initialize(void *instance,
                                    const rt::HalV2InitializeConfig *) {
    return instance ? rt::HalV2Status::ok : rt::HalV2Status::invalid_argument;
  }

  static rt::HalV2Status register_buffer(void *instance,
                                         const rt::HalV2BufferRegistration *,
                                         std::uint64_t *token) {
    if (!instance || !token)
      return rt::HalV2Status::invalid_argument;
    auto &backend = *self(instance);
    ++backend.legacy_register_calls;
    backend.order.push_back('L');
    *token = 0x1000u + backend.legacy_register_calls;
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status unregister_buffer(void *instance, std::uint64_t) {
    if (!instance)
      return rt::HalV2Status::invalid_argument;
    auto &backend = *self(instance);
    ++backend.legacy_unregister_calls;
    backend.order.push_back('l');
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status submit(void *instance,
                                const rt::HalV2Submission *submission) {
    if (!instance || !submission) {
      return rt::HalV2Status::invalid_argument;
    }
    auto &backend = *self(instance);
    backend.last_submission = *submission;
    backend.submit_calls.fetch_add(1, std::memory_order_relaxed);
    backend.pending_submission.store(submission->submission_id,
                                     std::memory_order_release);
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status poll(void *instance, rt::HalV2Completion *completions,
                              std::uint64_t capacity, std::uint64_t *count) {
    if (!instance || !count || (capacity != 0 && !completions)) {
      return rt::HalV2Status::invalid_argument;
    }
    auto &backend = *self(instance);
    const auto submission =
        capacity == 0
            ? 0
            : backend.pending_submission.exchange(0, std::memory_order_acq_rel);
    if (submission != 0) {
      completions[0] = {};
      completions[0].submission_id = submission;
      *count = 1;
      return rt::HalV2Status::ok;
    }
    *count = 0;
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status cancel(void *, std::uint64_t) {
    return rt::HalV2Status::unsupported;
  }

  static rt::HalV2Status get_health(void *instance, rt::HalV2Health *output) {
    if (!instance || !output)
      return rt::HalV2Status::invalid_argument;
    *output = {};
    output->state = static_cast<std::uint32_t>(rt::HalV2HealthState::healthy);
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status reset(void *instance) {
    return instance ? rt::HalV2Status::ok : rt::HalV2Status::invalid_argument;
  }

  static rt::HalV2Status shutdown(void *instance) {
    return instance ? rt::HalV2Status::ok : rt::HalV2Status::invalid_argument;
  }

  static rt::HalV2Status discover(void *instance,
                                  rt::HalV2MemoryTopologySnapshot *output) {
    if (!instance || !output)
      return rt::HalV2Status::invalid_argument;
    auto &backend = *self(instance);
    ++backend.discovery_calls;
    if (backend.throw_discovery)
      throw std::runtime_error("discover");
    if (backend.partially_write_discovery) {
      *output = {};
      output->memory_domain_count = 1;
      output->topology_node_count = 1;
      output->timestamp_domain_count = 1;
    } else if (!backend.leave_discovery_untouched) {
      *output = backend.snapshot;
    }
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status
  register_memory(void *instance,
                  const rt::HalV2MemoryRegistration *registration,
                  rt::HalV2MemoryToken *token) {
    if (!instance || !registration || !token) {
      return rt::HalV2Status::invalid_argument;
    }
    auto &backend = *self(instance);
    ++backend.memory_register_calls;
    backend.order.push_back('H');
    backend.registrations.push_back(*registration);
    if (backend.throw_register)
      throw std::runtime_error("register");
    const bool fail =
        backend.fail_registration_call == backend.memory_register_calls;
    if (!fail || !backend.failed_registration_has_empty_token) {
      token->submission_token = 0x2000u + backend.memory_register_calls;
      token->native_token.size = 1;
      token->native_token.bytes[0] = std::byte{0x5a};
    }
    return fail ? rt::HalV2Status::error : rt::HalV2Status::ok;
  }

  static rt::HalV2Status
  unregister_memory(void *instance,
                    const rt::HalV2MemoryRegistration *registration,
                    const rt::HalV2MemoryToken *token) {
    if (!instance || !registration || !token) {
      return rt::HalV2Status::invalid_argument;
    }
    auto &backend = *self(instance);
    ++backend.memory_unregister_calls;
    backend.order.push_back('U');
    backend.unregistrations.push_back(*registration);
    backend.unregistration_tokens.push_back(*token);
    return backend.unregister_status;
  }

  static rt::HalV2Status
  query_correlation(void *instance,
                    const rt::HalV2TimestampCorrelationQuery *query,
                    rt::HalV2TimestampCorrelation *output) {
    if (!instance || !query || !output) {
      return rt::HalV2Status::invalid_argument;
    }
    auto &backend = *self(instance);
    ++backend.correlation_calls;
    if (backend.throw_correlation)
      throw std::runtime_error("correlation");
    if (backend.correlation_status != rt::HalV2Status::ok) {
      return backend.correlation_status;
    }
    *output = {};
    output->source_domain_identity = query->source_domain_identity;
    output->destination_domain_identity = query->destination_domain_identity;
    output->generation = backend.malformed_correlation ? 0 : 7;
    output->source_value = 1000;
    output->destination_value = 2000;
    output->uncertainty_ns = 25;
    return rt::HalV2Status::ok;
  }
};

struct V1Backend {
  rtfw_device_backend_api api() {
    rtfw_device_backend_api table{};
    table.struct_size = sizeof(table);
    table.abi_version = RTFW_DEVICE_ABI_VERSION;
    table.instance = this;
    table.get_capabilities = &get_capabilities;
    table.initialize = &initialize;
    table.register_buffer = &register_buffer;
    table.unregister_buffer = &unregister_buffer;
    table.submit = &submit;
    table.poll = &poll;
    table.cancel = &cancel;
    table.get_health = &get_health;
    table.reset = &reset;
    table.shutdown = &shutdown;
    return table;
  }

  static rtfw_device_status get_capabilities(void *instance,
                                             rtfw_device_capabilities *output) {
    if (!instance || !output)
      return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    *output = {};
    output->struct_size = sizeof(*output);
    output->abi_version = RTFW_DEVICE_ABI_VERSION;
    output->max_in_flight = 8;
    output->max_registered_buffers = 8;
    output->max_buffer_bytes = 4096;
    output->inline_payload_capacity = RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY;
    output->buffer_ref_capacity = RTFW_DEVICE_BUFFER_REF_CAPACITY;
    output->supports_cancel = 1;
    output->supports_reset = 1;
    output->deterministic_mock = 1;
    constexpr std::string_view id = "test.heterogeneous.v1";
    std::copy(id.begin(), id.end(), std::begin(output->backend_id));
    return RTFW_DEVICE_STATUS_OK;
  }

  static rtfw_device_status initialize(void *instance,
                                       const rtfw_device_init_config *) {
    return instance ? RTFW_DEVICE_STATUS_OK
                    : RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
  }

  static rtfw_device_status
  register_buffer(void *instance, const rtfw_device_buffer_registration *,
                  std::uint64_t *token) {
    if (!instance || !token)
      return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    *token = 0x1234;
    return RTFW_DEVICE_STATUS_OK;
  }

  static rtfw_device_status unregister_buffer(void *instance, std::uint64_t) {
    return instance ? RTFW_DEVICE_STATUS_OK
                    : RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
  }

  static rtfw_device_status submit(void *instance,
                                   const rtfw_device_submission *) {
    return instance ? RTFW_DEVICE_STATUS_OK
                    : RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
  }

  static rtfw_device_status poll(void *instance, rtfw_device_completion *,
                                 std::uint64_t, std::uint64_t *count) {
    if (!instance || !count)
      return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    *count = 0;
    return RTFW_DEVICE_STATUS_OK;
  }

  static rtfw_device_status cancel(void *, std::uint64_t) {
    return RTFW_DEVICE_STATUS_UNSUPPORTED;
  }

  static rtfw_device_status get_health(void *instance,
                                       rtfw_device_health *output) {
    if (!instance || !output)
      return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    *output = {};
    output->struct_size = sizeof(*output);
    output->state = RTFW_DEVICE_HEALTH_HEALTHY;
    return RTFW_DEVICE_STATUS_OK;
  }

  static rtfw_device_status reset(void *instance) {
    return instance ? RTFW_DEVICE_STATUS_OK
                    : RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
  }

  static rtfw_device_status shutdown(void *instance) {
    return instance ? RTFW_DEVICE_STATUS_OK
                    : RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
  }
};

rt::Status register_memory_backend(rt::Runtime &runtime, MemoryBackend &backend,
                                   std::string_view name,
                                   rt::DeviceBackendHandle &handle) {
  auto extension = backend.extension();
  return runtime.register_device_backend({name, backend.api(), &extension},
                                         handle);
}

rt::HalV2OpaqueHandle opaque_handle(std::byte value = std::byte{0x44}) {
  rt::HalV2OpaqueHandle handle;
  handle.size = 4;
  std::fill_n(handle.bytes.begin(), 4, value);
  return handle;
}

rt::DeviceMemoryDomainHandle domain_at(rt::Runtime &runtime,
                                       rt::DeviceBackendHandle backend,
                                       std::size_t index) {
  rt::DeviceMemoryDomainHandle handle;
  rt::HalV2MemoryDomain ignored;
  EXPECT_TRUE(runtime.device_memory_domain_at(backend, index, handle, ignored));
  return handle;
}

std::uint64_t graph_id(rt::Runtime &runtime) {
  std::array<std::byte, 1024> bytes{};
  rt::ArtifactWriteResult result;
  EXPECT_EQ(runtime.write_checkpoint(0, bytes, result), rt::Status::ok);
  rt::CheckpointMetadata metadata;
  EXPECT_EQ(rt::inspect_checkpoint_artifact(
                std::span<const std::byte>(bytes.data(), result.bytes_written),
                metadata),
            rt::Status::ok);
  return metadata.graph_id;
}

struct BufferCommand {
  rt::DeviceBufferHandle buffer{};
  std::uint32_t access = RTFW_DEVICE_ACCESS_READ;
  std::uint64_t offset = 0;
  std::uint64_t bytes = 0;
};

struct ActiveFrameGate {
  std::latch entered{1};
  std::latch release{1};
};

rt::CallbackResult hold_active_frame(void *user_data,
                                     const rt::CallbackContext &) {
  auto &gate = *static_cast<ActiveFrameGate *>(user_data);
  gate.entered.count_down();
  gate.release.wait();
  return rt::CallbackResult::ok;
}

rt::CallbackResult submit_buffer(void *user_data,
                                 const rt::DeviceCallbackContext &,
                                 rt::DeviceSubmission &submission) {
  const auto &command = *static_cast<BufferCommand *>(user_data);
  submission.timeout_ns = 10'000'000;
  submission.opcode = 7;
  submission.buffer_count = 1;
  submission.buffers[0].buffer_token = command.buffer.value;
  submission.buffers[0].access = command.access;
  submission.buffers[0].offset = command.offset;
  submission.buffers[0].bytes = command.bytes;
  return rt::CallbackResult::ok;
}

const rt::MemoryPolicyReport *
find_memory_row(const rt::CpuMemoryPolicyReport &report,
                rt::MemoryRegionId region) {
  const auto found = std::find_if(
      report.memory.begin(),
      report.memory.begin() + static_cast<std::ptrdiff_t>(report.memory_count),
      [region](const auto &row) { return row.region == region; });
  return found == report.memory.begin() +
                      static_cast<std::ptrdiff_t>(report.memory_count)
             ? nullptr
             : &*found;
}

} // namespace

TEST(HeterogeneousMemory,
     ConstantsEnumsDefaultsLayoutsAndReservedTailsAreExact) {
  EXPECT_EQ(rt::hal_v2_memory_topology_extension_version, 1u);
  EXPECT_EQ(rt::hal_v2_memory_domain_capacity, 16u);
  EXPECT_EQ(rt::hal_v2_topology_node_capacity, 32u);
  EXPECT_EQ(rt::hal_v2_topology_link_capacity, 64u);
  EXPECT_EQ(rt::hal_v2_timestamp_domain_capacity, 8u);
  EXPECT_EQ(rt::hal_v2_opaque_handle_capacity, 64u);
  EXPECT_EQ(static_cast<std::uint32_t>(rt::HalV2MemoryDomainKind::host), 1u);
  EXPECT_EQ(
      static_cast<std::uint32_t>(rt::HalV2MemoryDomainKind::pinned_host), 2u);
  EXPECT_EQ(static_cast<std::uint32_t>(rt::HalV2MemoryDomainKind::cuda_device),
            3u);
  EXPECT_EQ(static_cast<std::uint32_t>(rt::HalV2MemoryDomainKind::imported),
            4u);
  EXPECT_EQ(static_cast<std::uint32_t>(rt::HalV2MemoryDomainKind::dma_mapped),
            5u);
  EXPECT_EQ(static_cast<std::uint32_t>(rt::HalV2MemoryDomainKind::peer), 6u);
  EXPECT_EQ(static_cast<std::uint32_t>(rt::HalV2MemoryOwnership::borrowed_host),
            1u);
  EXPECT_EQ(
      static_cast<std::uint32_t>(rt::HalV2MemoryOwnership::borrowed_opaque),
      2u);
  EXPECT_EQ(static_cast<std::uint32_t>(rt::HalV2MemoryOwnership::backend), 3u);
  EXPECT_EQ(rt::hal_v2_memory_ownership_borrowed_host, 1u);
  EXPECT_EQ(rt::hal_v2_memory_ownership_borrowed_opaque, 2u);
  EXPECT_EQ(rt::hal_v2_memory_ownership_backend, 4u);
  EXPECT_EQ(static_cast<std::uint32_t>(rt::HalV2MemoryCoherency::host_coherent),
            1u);
  EXPECT_EQ(static_cast<std::uint32_t>(
                rt::HalV2MemoryCoherency::explicit_flush_invalidate),
            2u);
  EXPECT_EQ(static_cast<std::uint32_t>(rt::HalV2MemoryCoherency::staged_copy),
            3u);
  EXPECT_EQ(static_cast<std::uint32_t>(rt::HalV2MemoryCoherency::device_only),
            4u);
  EXPECT_EQ(rt::hal_v2_memory_sync_none, 0u);
  EXPECT_EQ(rt::hal_v2_memory_sync_flush, 1u);
  EXPECT_EQ(rt::hal_v2_memory_sync_invalidate, 2u);
  EXPECT_EQ(rt::hal_v2_memory_sync_copy_to_device, 4u);
  EXPECT_EQ(rt::hal_v2_memory_sync_copy_from_device, 8u);
  EXPECT_EQ(rt::hal_v2_memory_sync_timeline, 16u);
  EXPECT_EQ(static_cast<std::uint32_t>(rt::HalV2TopologyNodeKind::host), 1u);
  EXPECT_EQ(static_cast<std::uint32_t>(rt::HalV2TopologyNodeKind::numa), 2u);
  EXPECT_EQ(static_cast<std::uint32_t>(rt::HalV2TopologyNodeKind::device), 3u);
  EXPECT_EQ(
      static_cast<std::uint32_t>(rt::HalV2TopologyNodeKind::dma_endpoint), 4u);
  EXPECT_EQ(
      static_cast<std::uint32_t>(rt::HalV2TopologyNodeKind::peer_endpoint), 5u);
  EXPECT_EQ(static_cast<std::uint32_t>(rt::HalV2TopologyLinkKind::local), 1u);
  EXPECT_EQ(
      static_cast<std::uint32_t>(rt::HalV2TopologyLinkKind::host_access), 2u);
  EXPECT_EQ(
      static_cast<std::uint32_t>(rt::HalV2TopologyLinkKind::device_access),
      3u);
  EXPECT_EQ(static_cast<std::uint32_t>(rt::HalV2TopologyLinkKind::dma), 4u);
  EXPECT_EQ(static_cast<std::uint32_t>(rt::HalV2TopologyLinkKind::peer), 5u);
  EXPECT_EQ(static_cast<std::uint32_t>(
                rt::HalV2TimestampDomainKind::runtime_monotonic),
            1u);
  EXPECT_EQ(static_cast<std::uint32_t>(
                rt::HalV2TimestampDomainKind::backend_device),
            2u);
  EXPECT_EQ(static_cast<std::uint32_t>(rt::HalV2TimestampDomainKind::external),
            3u);

  EXPECT_EQ(sizeof(rt::HalV2OpaqueHandle), 88u);
  EXPECT_EQ(sizeof(rt::HalV2MemoryDomain), 120u);
  EXPECT_EQ(sizeof(rt::HalV2TopologyNode), 56u);
  EXPECT_EQ(sizeof(rt::HalV2TopologyLink), 72u);
  EXPECT_EQ(sizeof(rt::HalV2TimestampDomain), 96u);
  EXPECT_EQ(sizeof(rt::HalV2MemoryTopologySnapshot), 9184u);
  EXPECT_EQ(sizeof(rt::HalV2MemoryRegistration), 232u);
  EXPECT_EQ(sizeof(rt::HalV2MemoryToken), 136u);
  EXPECT_EQ(sizeof(rt::HalV2TimestampCorrelationQuery), 56u);
  EXPECT_EQ(sizeof(rt::HalV2TimestampCorrelation), 88u);
  EXPECT_EQ(sizeof(rt::HalV2MemoryTopologyExtension), 112u);
  EXPECT_EQ(sizeof(rt::HalV2BackendRegistration), 192u);
  EXPECT_EQ(sizeof(rt::DeviceMemoryDomainHandle), 16u);
  EXPECT_EQ(sizeof(rt::DeviceTopologyNodeHandle), 16u);
  EXPECT_EQ(sizeof(rt::DeviceTopologyLinkHandle), 16u);
  EXPECT_EQ(sizeof(rt::DeviceTimestampDomainHandle), 16u);
  EXPECT_EQ(sizeof(rt::HeterogeneousDeviceBufferRegistration), 168u);
  EXPECT_EQ(sizeof(rt::DeviceMemoryObjectInfo), 96u);
  static_assert(std::is_standard_layout_v<rt::HalV2MemoryDomain>);
  static_assert(std::is_standard_layout_v<rt::HalV2MemoryTopologySnapshot>);
  static_assert(std::is_standard_layout_v<rt::HalV2MemoryRegistration>);

  const rt::HalV2OpaqueHandle opaque;
  const rt::HalV2MemoryDomain domain;
  const rt::HalV2TopologyNode node;
  const rt::HalV2TopologyLink link;
  const rt::HalV2TimestampDomain timestamp;
  const rt::HalV2MemoryTopologySnapshot snapshot;
  const rt::HalV2MemoryRegistration registration;
  const rt::HalV2MemoryToken token;
  const rt::HalV2TimestampCorrelationQuery query;
  const rt::HalV2TimestampCorrelation correlation;
  const rt::HalV2MemoryTopologyExtension extension;
  const rt::HeterogeneousDeviceBufferRegistration heterogeneous;
  const rt::DeviceMemoryObjectInfo object;
  EXPECT_EQ(opaque.struct_size, sizeof(opaque));
  EXPECT_EQ(domain.struct_size, sizeof(domain));
  EXPECT_EQ(node.struct_size, sizeof(node));
  EXPECT_EQ(link.struct_size, sizeof(link));
  EXPECT_EQ(timestamp.struct_size, sizeof(timestamp));
  EXPECT_EQ(snapshot.struct_size, sizeof(snapshot));
  EXPECT_EQ(registration.struct_size, sizeof(registration));
  EXPECT_EQ(token.struct_size, sizeof(token));
  EXPECT_EQ(query.struct_size, sizeof(query));
  EXPECT_EQ(correlation.struct_size, sizeof(correlation));
  EXPECT_EQ(extension.struct_size, sizeof(extension));
  EXPECT_EQ(opaque.size, 0u);
  EXPECT_EQ(domain.identity, 0u);
  EXPECT_EQ(domain.kind, 0u);
  EXPECT_EQ(domain.ownership_modes, 0u);
  EXPECT_EQ(domain.maximum_bytes, 0u);
  EXPECT_EQ(node.identity, 0u);
  EXPECT_EQ(node.kind, 0u);
  EXPECT_EQ(link.identity, 0u);
  EXPECT_EQ(link.source_node_identity, 0u);
  EXPECT_EQ(link.destination_node_identity, 0u);
  EXPECT_EQ(timestamp.identity, 0u);
  EXPECT_EQ(timestamp.tick_numerator_ns, 0u);
  EXPECT_EQ(snapshot.memory_domain_count, 0u);
  EXPECT_EQ(snapshot.topology_node_count, 0u);
  EXPECT_EQ(snapshot.topology_link_count, 0u);
  EXPECT_EQ(snapshot.timestamp_domain_count, 0u);
  EXPECT_EQ(registration.domain_identity, 0u);
  EXPECT_EQ(registration.bytes, 0u);
  EXPECT_EQ(registration.host_data, nullptr);
  EXPECT_EQ(token.submission_token, 0u);
  EXPECT_EQ(query.source_domain_identity, 0u);
  EXPECT_EQ(query.destination_domain_identity, 0u);
  EXPECT_EQ(correlation.generation, 0u);
  EXPECT_EQ(extension.instance, nullptr);
  EXPECT_EQ(extension.discover, nullptr);
  EXPECT_EQ(extension.register_memory, nullptr);
  EXPECT_EQ(extension.unregister_memory, nullptr);
  EXPECT_EQ(extension.query_timestamp_correlation, nullptr);
  EXPECT_FALSE(heterogeneous.backend.valid());
  EXPECT_FALSE(heterogeneous.domain.valid());
  EXPECT_TRUE(heterogeneous.host_storage.empty());
  EXPECT_EQ(heterogeneous.bytes, 0u);
  EXPECT_EQ(heterogeneous.ownership,
            rt::HalV2MemoryOwnership::borrowed_host);
  EXPECT_EQ(heterogeneous.access, 0u);
  EXPECT_EQ(heterogeneous.coherency,
            rt::HalV2MemoryCoherency::host_coherent);
  EXPECT_EQ(heterogeneous.synchronization, 0u);
  EXPECT_FALSE(object.buffer.valid());
  EXPECT_FALSE(object.backend.valid());
  EXPECT_FALSE(object.domain.valid());
  EXPECT_TRUE(all_zero(opaque.reserved));
  EXPECT_TRUE(all_zero(domain.reserved));
  EXPECT_TRUE(all_zero(node.reserved));
  EXPECT_TRUE(all_zero(link.reserved));
  EXPECT_TRUE(all_zero(timestamp.reserved));
  EXPECT_TRUE(all_zero(snapshot.reserved));
  EXPECT_TRUE(all_zero(registration.reserved));
  EXPECT_TRUE(all_zero(token.reserved));
  EXPECT_TRUE(all_zero(query.reserved));
  EXPECT_TRUE(all_zero(correlation.reserved));
  EXPECT_TRUE(all_zero(extension.reserved));

  const rt::HalV2BackendRegistration old_prefix{"old", {}};
  const rt::DeviceBufferRegistration legacy_prefix{"old.buffer", {}, {}};
  EXPECT_EQ(old_prefix.memory_topology, nullptr);
  EXPECT_EQ(old_prefix.command_timeline, nullptr);
  EXPECT_EQ(legacy_prefix.flags, RTFW_DEVICE_BUFFER_HOST_READ |
                                     RTFW_DEVICE_BUFFER_HOST_WRITE |
                                     RTFW_DEVICE_BUFFER_DEVICE_READ |
                                     RTFW_DEVICE_BUFFER_DEVICE_WRITE);
}

TEST(HeterogeneousMemory,
     DiscoversSixKindsAndFreezesInspectorsAcrossLifecycle) {
  MemoryBackend probe;
  rt::Runtime runtime;
  ASSERT_EQ(runtime.configure(memory_config()), rt::Status::ok);
  rt::DeviceBackendHandle backend;
  ASSERT_EQ(register_memory_backend(runtime, probe, "memory", backend),
            rt::Status::ok);
  ASSERT_EQ(probe.discovery_calls, 1u);
  ASSERT_EQ(runtime.device_memory_domain_count(backend), 6u);
  ASSERT_EQ(runtime.device_topology_node_count(backend), 6u);
  ASSERT_EQ(runtime.device_topology_link_count(backend), 2u);
  ASSERT_EQ(runtime.device_timestamp_domain_count(backend), 2u);
  for (std::size_t index = 0; index < 6; ++index) {
    rt::DeviceMemoryDomainHandle handle;
    rt::HalV2MemoryDomain domain;
    ASSERT_TRUE(
        runtime.device_memory_domain_at(backend, index, handle, domain));
    EXPECT_EQ(handle.backend, backend);
    EXPECT_EQ(handle.identity, index + 1);
    EXPECT_EQ(domain.kind, index + 1);
  }
  rt::DeviceTopologyLinkHandle link_handle;
  rt::HalV2TopologyLink link;
  ASSERT_TRUE(runtime.device_topology_link_at(backend, 1, link_handle, link));
  EXPECT_EQ(link_handle.backend, backend);
  EXPECT_EQ(link_handle.identity, 302u);
  EXPECT_EQ(link.kind,
            static_cast<std::uint32_t>(rt::HalV2TopologyLinkKind::peer));
  rt::DeviceTimestampDomainHandle completion_domain;
  ASSERT_TRUE(
      runtime.device_completion_timestamp_domain(backend, completion_domain));
  EXPECT_EQ(completion_domain.identity, 202u);

  probe.snapshot.memory_domains[0].identity = 999;
  ASSERT_EQ(runtime.finalize(), rt::Status::ok);
  EXPECT_EQ(domain_at(runtime, backend, 0).identity, 1u);
  ASSERT_EQ(runtime.start(), rt::Status::ok);
  EXPECT_EQ(runtime.device_memory_domain_count(backend), 6u);
  ASSERT_EQ(runtime.stop(), rt::Status::ok);
  EXPECT_EQ(runtime.device_memory_domain_count(backend), 6u);
  EXPECT_EQ(domain_at(runtime, backend, 5).identity, 6u);
}

TEST(HeterogeneousMemory, MalformedDiscoveryFailsWithoutPublication) {
  const auto expect_rejected = [](auto mutate) {
    MemoryBackend probe;
    mutate(probe);
    rt::Runtime runtime;
    EXPECT_EQ(runtime.configure(memory_config()), rt::Status::ok);
    rt::DeviceBackendHandle backend;
    EXPECT_EQ(register_memory_backend(runtime, probe, "bad", backend),
              rt::Status::invalid_argument);
    EXPECT_FALSE(backend.valid());
    EXPECT_EQ(runtime.device_backend_count(), 0u);
  };

  expect_rejected([](auto &p) {
    p.snapshot.memory_domain_count = rt::hal_v2_memory_domain_capacity + 1;
  });
  expect_rejected(
      [](auto &p) { p.snapshot.struct_size = sizeof(p.snapshot) - 1; });
  expect_rejected([](auto &p) { p.snapshot.extension_version = 2; });
  expect_rejected([](auto &p) {
    p.snapshot.topology_node_count = rt::hal_v2_topology_node_capacity + 1;
  });
  expect_rejected([](auto &p) {
    p.snapshot.timestamp_domain_count =
        rt::hal_v2_timestamp_domain_capacity + 1;
  });
  expect_rejected([](auto &p) {
    p.snapshot.memory_domains[1].identity =
        p.snapshot.memory_domains[0].identity;
  });
  expect_rejected([](auto &p) {
    p.snapshot.memory_domains[0].struct_size =
        sizeof(rt::HalV2MemoryDomain) - 1;
  });
  expect_rejected(
      [](auto &p) { p.snapshot.memory_domains[0].extension_version = 2; });
  expect_rejected([](auto &p) {
    p.snapshot.memory_domains[0].topology_node_identity = 999;
  });
  expect_rejected(
      [](auto &p) { p.snapshot.memory_domains[0].byte_granularity = 0; });
  expect_rejected([](auto &p) { p.snapshot.memory_domains[0].alignment = 3; });
  expect_rejected(
      [](auto &p) { p.snapshot.memory_domains[0].offset_granularity = 3; });
  expect_rejected([](auto &p) { p.snapshot.memory_domains[0].kind = 99; });
  expect_rejected([](auto &p) {
    p.snapshot.memory_domains[2].ownership_modes =
        rt::hal_v2_memory_ownership_borrowed_host;
  });
  expect_rejected([](auto &p) {
    p.snapshot.memory_domains[0].access = RTFW_DEVICE_BUFFER_DEVICE_READ;
  });
  expect_rejected([](auto &p) {
    p.snapshot.memory_domains[0].required_synchronization =
        rt::hal_v2_memory_sync_flush;
  });
  expect_rejected(
      [](auto &p) { p.snapshot.memory_domains[0].reserved[0] = 1; });
  expect_rejected([](auto &p) { p.snapshot.topology_nodes[0].reserved0 = 1; });
  expect_rejected([](auto &p) {
    p.snapshot.topology_links[1].destination_node_identity = 999;
  });
  expect_rejected([](auto &p) {
    p.snapshot.topology_links[1].destination_node_identity = 105;
  });
  expect_rejected([](auto &p) {
    p.snapshot.topology_links[0] = p.snapshot.topology_links[1];
    p.snapshot.topology_links[0].identity = 303;
  });
  expect_rejected(
      [](auto &p) { p.snapshot.topology_links[1].source_node_identity = 104; });
  expect_rejected([](auto &p) {
    p.snapshot.memory_domains[5].topology_node_identity = 104;
  });
  expect_rejected(
      [](auto &p) { p.snapshot.topology_links[1].reserved[0] = 1; });
  expect_rejected([](auto &p) { p.snapshot.topology_link_count = 1; });
  expect_rejected([](auto &p) {
    p.snapshot.timestamp_domains[1].supports_correlation = 2;
  });
  expect_rejected([](auto &p) {
    p.snapshot.timestamp_domains[1].correlation_destination_identity = 999;
  });
  expect_rejected(
      [](auto &p) { p.snapshot.completion_timestamp_domain_identity = 999; });
  expect_rejected(
      [](auto &p) { p.snapshot.timestamp_domains[0].tick_denominator = 0; });
  expect_rejected([](auto &p) {
    p.snapshot.timestamp_domains[1].wrap_ticks =
        std::numeric_limits<std::uint64_t>::max();
    p.snapshot.timestamp_domains[1].tick_numerator_ns = 2;
  });
  expect_rejected(
      [](auto &p) { p.snapshot.timestamp_domains[0].reserved2 = 1; });
  expect_rejected([](auto &p) { p.leave_discovery_untouched = true; });
  expect_rejected([](auto &p) { p.partially_write_discovery = true; });
}

TEST(HeterogeneousMemory, MalformedExtensionTableFailsWithoutDiscovery) {
  const auto expect_rejected = [](auto mutate) {
    MemoryBackend probe;
    auto extension = probe.extension();
    mutate(extension);
    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(memory_config()), rt::Status::ok);
    rt::DeviceBackendHandle backend;
    EXPECT_EQ(runtime.register_device_backend(
                  {"bad-extension", probe.api(), &extension}, backend),
              rt::Status::invalid_argument);
    EXPECT_FALSE(backend.valid());
    EXPECT_EQ(runtime.device_backend_count(), 0u);
    EXPECT_EQ(probe.discovery_calls, 0u);
  };
  expect_rejected(
      [](auto &extension) { extension.struct_size = sizeof(extension) - 1; });
  expect_rejected([](auto &extension) { extension.extension_version = 2; });
  expect_rejected([](auto &extension) { extension.instance = nullptr; });
  expect_rejected([](auto &extension) { extension.discover = nullptr; });
  expect_rejected([](auto &extension) { extension.register_memory = nullptr; });
  expect_rejected(
      [](auto &extension) { extension.unregister_memory = nullptr; });
  expect_rejected(
      [](auto &extension) { extension.query_timestamp_correlation = nullptr; });
  expect_rejected([](auto &extension) { extension.reserved[0] = 1; });
}

TEST(HeterogeneousMemory, DiscoveryAndCorrelationExceptionsAreContained) {
  MemoryBackend discovery_probe;
  discovery_probe.throw_discovery = true;
  rt::Runtime rejected;
  ASSERT_EQ(rejected.configure(memory_config()), rt::Status::ok);
  rt::DeviceBackendHandle ignored;
  EXPECT_EQ(
      register_memory_backend(rejected, discovery_probe, "throw", ignored),
      rt::Status::internal_error);
  EXPECT_FALSE(ignored.valid());

  MemoryBackend correlation_probe;
  rt::Runtime runtime;
  ASSERT_EQ(runtime.configure(memory_config()), rt::Status::ok);
  rt::DeviceBackendHandle backend;
  ASSERT_EQ(
      register_memory_backend(runtime, correlation_probe, "corr", backend),
      rt::Status::ok);
  ASSERT_EQ(runtime.finalize(), rt::Status::ok);
  ASSERT_EQ(runtime.start(), rt::Status::ok);
  rt::DeviceTimestampDomainHandle destination;
  rt::DeviceTimestampDomainHandle source;
  rt::HalV2TimestampDomain descriptor;
  ASSERT_TRUE(
      runtime.device_timestamp_domain_at(backend, 0, destination, descriptor));
  ASSERT_TRUE(
      runtime.device_timestamp_domain_at(backend, 1, source, descriptor));
  correlation_probe.throw_correlation = true;
  rt::HalV2TimestampCorrelation output;
  EXPECT_EQ(runtime.query_device_timestamp_correlation(backend, source,
                                                       destination, output),
            rt::Status::device_error);
  EXPECT_EQ(output.generation, 0u);
  EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(HeterogeneousMemory, CoreOnlyNativeAndV1ExposeOnlyImplicitHost) {
  MemoryBackend native;
  rt::Runtime runtime;
  ASSERT_EQ(runtime.configure(memory_config()), rt::Status::ok);
  rt::DeviceBackendHandle native_handle;
  ASSERT_EQ(runtime.register_device_backend({"core-only", native.api()},
                                            native_handle),
            rt::Status::ok);
  ASSERT_EQ(runtime.device_memory_domain_count(native_handle), 1u);
  rt::DeviceMemoryDomainHandle domain;
  rt::HalV2MemoryDomain descriptor;
  ASSERT_TRUE(
      runtime.device_memory_domain_at(native_handle, 0, domain, descriptor));
  EXPECT_EQ(descriptor.kind,
            static_cast<std::uint32_t>(rt::HalV2MemoryDomainKind::host));
  EXPECT_EQ(descriptor.ownership_modes,
            rt::hal_v2_memory_ownership_borrowed_host);
  EXPECT_EQ(descriptor.required_synchronization, 0u);
  EXPECT_EQ(runtime.device_topology_node_count(native_handle), 0u);
  EXPECT_EQ(runtime.device_timestamp_domain_count(native_handle), 0u);

  // A native core-only backend cannot turn its implicit compatibility domain
  // into an explicit heterogeneous registration.
  alignas(8) std::array<std::byte, 8> storage{};
  rt::DeviceBufferHandle buffer;
  EXPECT_EQ(
      runtime.register_device_buffer({"heterogeneous",
                                      native_handle,
                                      domain,
                                      storage,
                                      {},
                                      8,
                                      rt::HalV2MemoryOwnership::borrowed_host,
                                      RTFW_DEVICE_BUFFER_HOST_READ,
                                      rt::HalV2MemoryCoherency::host_coherent,
                                      0},
                                     buffer),
      rt::Status::invalid_argument);

  V1Backend v1;
  rt::Runtime adapted;
  ASSERT_EQ(adapted.configure(memory_config()), rt::Status::ok);
  rt::DeviceBackendHandle adapted_handle;
  ASSERT_EQ(
      adapted.register_device_backend({"adapted", v1.api()}, adapted_handle),
      rt::Status::ok);
  ASSERT_EQ(adapted.device_memory_domain_count(adapted_handle), 1u);
  ASSERT_TRUE(
      adapted.device_memory_domain_at(adapted_handle, 0, domain, descriptor));
  EXPECT_EQ(descriptor.kind,
            static_cast<std::uint32_t>(rt::HalV2MemoryDomainKind::host));
  EXPECT_EQ(descriptor.ownership_modes,
            rt::hal_v2_memory_ownership_borrowed_host);
  EXPECT_EQ(adapted.device_topology_node_count(adapted_handle), 0u);
  EXPECT_EQ(adapted.device_topology_link_count(adapted_handle), 0u);
  EXPECT_EQ(adapted.device_timestamp_domain_count(adapted_handle), 0u);
}

TEST(HeterogeneousMemory,
     HeterogeneousHostOpaqueAndRejectionPathsAreTransactional) {
  MemoryBackend probe;
  rt::Runtime runtime;
  ASSERT_EQ(runtime.configure(memory_config(1, 8)), rt::Status::ok);
  rt::DeviceBackendHandle backend;
  ASSERT_EQ(register_memory_backend(runtime, probe, "memory", backend),
            rt::Status::ok);
  const auto host_domain = domain_at(runtime, backend, 0);
  const auto imported_domain = domain_at(runtime, backend, 3);
  alignas(64) std::array<std::byte, 128> storage{};
  rt::DeviceBufferHandle host_buffer;
  ASSERT_EQ(runtime.register_device_buffer(
                {"host",
                 backend,
                 host_domain,
                 std::span<std::byte>(storage.data(), 64),
                 {},
                 64,
                 rt::HalV2MemoryOwnership::borrowed_host,
                 RTFW_DEVICE_BUFFER_HOST_READ | RTFW_DEVICE_BUFFER_DEVICE_READ,
                 rt::HalV2MemoryCoherency::host_coherent,
                 0},
                host_buffer),
            rt::Status::ok);
  rt::DeviceBufferHandle opaque_buffer;
  ASSERT_EQ(
      runtime.register_device_buffer({"opaque",
                                      backend,
                                      imported_domain,
                                      {},
                                      opaque_handle(),
                                      64,
                                      rt::HalV2MemoryOwnership::borrowed_opaque,
                                      RTFW_DEVICE_BUFFER_DEVICE_READ,
                                      rt::HalV2MemoryCoherency::device_only,
                                      0},
                                     opaque_buffer),
      rt::Status::ok);
  rt::DeviceMemoryObjectInfo object;
  ASSERT_TRUE(runtime.device_memory_object_at(0, object));
  EXPECT_EQ(object.buffer, host_buffer);
  EXPECT_EQ(object.heterogeneous, 1u);
  EXPECT_EQ(object.host_addressable, 1u);
  ASSERT_TRUE(runtime.device_memory_object_at(1, object));
  EXPECT_EQ(object.buffer, opaque_buffer);
  EXPECT_EQ(object.host_addressable, 0u);
  EXPECT_EQ(object.opaque_handle_size, 4u);

  const auto before = runtime.device_buffer_count();
  const auto reject = [&](auto registration, rt::Status expected) {
    rt::DeviceBufferHandle result;
    EXPECT_EQ(runtime.register_device_buffer(registration, result), expected);
    EXPECT_FALSE(result.valid());
    EXPECT_EQ(runtime.device_buffer_count(), before);
  };
  reject(
      rt::HeterogeneousDeviceBufferRegistration{
          "empty",
          backend,
          host_domain,
          {},
          {},
          64,
          rt::HalV2MemoryOwnership::borrowed_host,
          RTFW_DEVICE_BUFFER_HOST_READ,
          rt::HalV2MemoryCoherency::host_coherent,
          0},
      rt::Status::invalid_argument);
  reject(
      rt::HeterogeneousDeviceBufferRegistration{
          "range",
          backend,
          host_domain,
          std::span<std::byte>(storage.data() + 64, 64),
          {},
          32,
          rt::HalV2MemoryOwnership::borrowed_host,
          RTFW_DEVICE_BUFFER_HOST_READ,
          rt::HalV2MemoryCoherency::host_coherent,
          0},
      rt::Status::invalid_argument);
  reject(
      rt::HeterogeneousDeviceBufferRegistration{
          "unaligned",
          backend,
          host_domain,
          std::span<std::byte>(storage.data() + 1, 8),
          {},
          8,
          rt::HalV2MemoryOwnership::borrowed_host,
          RTFW_DEVICE_BUFFER_HOST_READ,
          rt::HalV2MemoryCoherency::host_coherent,
          0},
      rt::Status::invalid_argument);
  reject(
      rt::HeterogeneousDeviceBufferRegistration{
          "overlap",
          backend,
          host_domain,
          std::span<std::byte>(storage.data() + 8, 8),
          {},
          8,
          rt::HalV2MemoryOwnership::borrowed_host,
          RTFW_DEVICE_BUFFER_HOST_READ,
          rt::HalV2MemoryCoherency::host_coherent,
          0},
      rt::Status::invalid_argument);
  reject(
      rt::HeterogeneousDeviceBufferRegistration{
          "access",
          backend,
          imported_domain,
          {},
          opaque_handle(),
          64,
          rt::HalV2MemoryOwnership::borrowed_opaque,
          RTFW_DEVICE_BUFFER_HOST_READ,
          rt::HalV2MemoryCoherency::device_only,
          0},
      rt::Status::invalid_argument);
  reject(
      rt::HeterogeneousDeviceBufferRegistration{
          "sync",
          backend,
          host_domain,
          std::span<std::byte>(storage.data() + 64, 64),
          {},
          64,
          rt::HalV2MemoryOwnership::borrowed_host,
          RTFW_DEVICE_BUFFER_HOST_READ,
          rt::HalV2MemoryCoherency::host_coherent,
          rt::hal_v2_memory_sync_flush},
      rt::Status::invalid_argument);
  auto malformed = opaque_handle();
  malformed.bytes[4] = std::byte{1};
  reject(
      rt::HeterogeneousDeviceBufferRegistration{
          "opaque-tail",
          backend,
          imported_domain,
          {},
          malformed,
          64,
          rt::HalV2MemoryOwnership::borrowed_opaque,
          RTFW_DEVICE_BUFFER_DEVICE_READ,
          rt::HalV2MemoryCoherency::device_only,
          0},
      rt::Status::invalid_argument);

  ASSERT_EQ(runtime.finalize(), rt::Status::ok);
  ASSERT_EQ(runtime.start(), rt::Status::ok);
  ASSERT_EQ(probe.memory_register_calls, 2u);
  ASSERT_EQ(probe.registrations.size(), 2u);
  EXPECT_EQ(probe.registrations[0].domain_identity, host_domain.identity);
  EXPECT_EQ(probe.registrations[0].host_data, storage.data());
  EXPECT_EQ(probe.registrations[0].opaque_handle.size, 0u);
  EXPECT_EQ(probe.registrations[1].domain_identity, imported_domain.identity);
  EXPECT_EQ(probe.registrations[1].host_data, nullptr);
  EXPECT_EQ(probe.registrations[1].opaque_handle.size, 4u);
  ASSERT_EQ(runtime.stop(), rt::Status::ok);
  ASSERT_EQ(probe.memory_unregister_calls, 2u);
  ASSERT_EQ(probe.unregistrations.size(), 2u);
  EXPECT_EQ(probe.unregistrations[0].domain_identity, imported_domain.identity);
  EXPECT_EQ(probe.unregistrations[1].domain_identity, host_domain.identity);
}

TEST(HeterogeneousMemory,
     ForeignHandlesAndBackendCapabilitiesRejectBeforePublication) {
  MemoryBackend first;
  MemoryBackend second;
  second.capabilities.max_buffer_bytes = 32;
  rt::Runtime one;
  rt::Runtime two;
  ASSERT_EQ(one.configure(memory_config()), rt::Status::ok);
  ASSERT_EQ(two.configure(memory_config()), rt::Status::ok);
  rt::DeviceBackendHandle one_backend;
  rt::DeviceBackendHandle two_backend;
  ASSERT_EQ(register_memory_backend(one, first, "one", one_backend),
            rt::Status::ok);
  ASSERT_EQ(register_memory_backend(two, second, "two", two_backend),
            rt::Status::ok);
  const auto foreign_domain = domain_at(one, one_backend, 0);
  const auto two_domain = domain_at(two, two_backend, 0);
  alignas(64) std::array<std::byte, 64> storage{};
  rt::DeviceBufferHandle output;
  EXPECT_EQ(two.register_device_buffer({"foreign",
                                        two_backend,
                                        foreign_domain,
                                        storage,
                                        {},
                                        64,
                                        rt::HalV2MemoryOwnership::borrowed_host,
                                        RTFW_DEVICE_BUFFER_HOST_READ,
                                        rt::HalV2MemoryCoherency::host_coherent,
                                        0},
                                       output),
            rt::Status::invalid_handle);
  EXPECT_EQ(two.register_device_buffer({"too-large",
                                        two_backend,
                                        two_domain,
                                        storage,
                                        {},
                                        64,
                                        rt::HalV2MemoryOwnership::borrowed_host,
                                        RTFW_DEVICE_BUFFER_HOST_READ,
                                        rt::HalV2MemoryCoherency::host_coherent,
                                        0},
                                       output),
            rt::Status::capacity_exceeded);
  EXPECT_EQ(two.device_buffer_count(), 0u);
}

TEST(HeterogeneousMemory, MixedRegistrationRollbackIsReverseAndRetryable) {
  MemoryBackend probe;
  probe.fail_registration_call = 1;
  rt::Runtime runtime;
  ASSERT_EQ(runtime.configure(memory_config()), rt::Status::ok);
  rt::DeviceBackendHandle backend;
  ASSERT_EQ(register_memory_backend(runtime, probe, "mixed", backend),
            rt::Status::ok);
  alignas(64) std::array<std::byte, 64> legacy_storage{};
  rt::DeviceBufferHandle legacy;
  ASSERT_EQ(runtime.register_device_buffer({"legacy", backend, legacy_storage},
                                           legacy),
            rt::Status::ok);
  const auto imported = domain_at(runtime, backend, 3);
  rt::DeviceBufferHandle heterogeneous;
  ASSERT_EQ(
      runtime.register_device_buffer({"heterogeneous",
                                      backend,
                                      imported,
                                      {},
                                      opaque_handle(),
                                      64,
                                      rt::HalV2MemoryOwnership::borrowed_opaque,
                                      RTFW_DEVICE_BUFFER_DEVICE_READ,
                                      rt::HalV2MemoryCoherency::device_only,
                                      0},
                                     heterogeneous),
      rt::Status::ok);
  ASSERT_EQ(runtime.finalize(), rt::Status::ok);
  EXPECT_EQ(runtime.start(), rt::Status::device_error);
  EXPECT_EQ(probe.order, (std::vector<char>{'L', 'H', 'U', 'l'}));
  probe.fail_registration_call = 0;
  probe.order.clear();
  ASSERT_EQ(runtime.start(), rt::Status::ok);
  EXPECT_EQ(probe.order, (std::vector<char>{'L', 'H'}));
  ASSERT_EQ(runtime.stop(), rt::Status::ok);
  EXPECT_EQ(probe.order, (std::vector<char>{'L', 'H', 'U', 'l'}));
}

TEST(HeterogeneousMemory,
     UncertainEmptyTokenBlocksShutdownAndRetriesOnlyUnresolved) {
  MemoryBackend probe;
  probe.fail_registration_call = 1;
  probe.failed_registration_has_empty_token = true;
  probe.unregister_status = rt::HalV2Status::error;
  rt::Runtime runtime;
  ASSERT_EQ(runtime.configure(memory_config()), rt::Status::ok);
  rt::DeviceBackendHandle backend;
  ASSERT_EQ(register_memory_backend(runtime, probe, "uncertain", backend),
            rt::Status::ok);
  const auto imported = domain_at(runtime, backend, 3);
  rt::DeviceBufferHandle buffer;
  ASSERT_EQ(
      runtime.register_device_buffer({"opaque",
                                      backend,
                                      imported,
                                      {},
                                      opaque_handle(),
                                      64,
                                      rt::HalV2MemoryOwnership::borrowed_opaque,
                                      RTFW_DEVICE_BUFFER_DEVICE_READ,
                                      rt::HalV2MemoryCoherency::device_only,
                                      0},
                                     buffer),
      rt::Status::ok);
  ASSERT_EQ(runtime.finalize(), rt::Status::ok);
  EXPECT_EQ(runtime.start(), rt::Status::device_error);
  ASSERT_EQ(probe.memory_unregister_calls, 1u);
  ASSERT_EQ(probe.unregistration_tokens.size(), 1u);
  EXPECT_EQ(probe.unregistration_tokens[0].submission_token, 0u);
  EXPECT_EQ(probe.unregistration_tokens[0].native_token.size, 0u);
  probe.unregister_status = rt::HalV2Status::ok;
  EXPECT_EQ(runtime.stop(), rt::Status::ok);
  EXPECT_EQ(probe.memory_unregister_calls, 2u);
  EXPECT_EQ(runtime.stop(), rt::Status::ok);
  EXPECT_EQ(probe.memory_unregister_calls, 2u);
}

TEST(HeterogeneousMemory,
     CorrelationSuccessUnsupportedAndMalformedAreObservable) {
  MemoryBackend probe;
  rt::Runtime runtime;
  ASSERT_EQ(runtime.configure(memory_config()), rt::Status::ok);
  rt::DeviceBackendHandle backend;
  ASSERT_EQ(register_memory_backend(runtime, probe, "correlation", backend),
            rt::Status::ok);
  rt::DeviceTimestampDomainHandle destination;
  rt::DeviceTimestampDomainHandle source;
  rt::HalV2TimestampDomain descriptor;
  ASSERT_TRUE(
      runtime.device_timestamp_domain_at(backend, 0, destination, descriptor));
  ASSERT_TRUE(
      runtime.device_timestamp_domain_at(backend, 1, source, descriptor));
  rt::HalV2TimestampCorrelation output;
  EXPECT_EQ(runtime.query_device_timestamp_correlation(backend, source,
                                                       destination, output),
            rt::Status::invalid_state);
  ASSERT_EQ(runtime.finalize(), rt::Status::ok);
  ASSERT_EQ(runtime.start(), rt::Status::ok);
  ASSERT_EQ(runtime.query_device_timestamp_correlation(backend, source,
                                                       destination, output),
            rt::Status::ok);
  EXPECT_EQ(output.generation, 7u);
  EXPECT_EQ(output.source_value, 1000u);
  EXPECT_EQ(output.destination_value, 2000u);
  EXPECT_EQ(output.uncertainty_ns, 25u);
  const auto successful_calls = probe.correlation_calls;
  EXPECT_EQ(runtime.query_device_timestamp_correlation(backend, destination,
                                                       source, output),
            rt::Status::device_error);
  EXPECT_EQ(probe.correlation_calls, successful_calls);
  probe.malformed_correlation = true;
  EXPECT_EQ(runtime.query_device_timestamp_correlation(backend, source,
                                                       destination, output),
            rt::Status::device_error);
  EXPECT_EQ(output.generation, 0u);
  probe.malformed_correlation = false;
  probe.correlation_status = rt::HalV2Status::unsupported;
  EXPECT_EQ(runtime.query_device_timestamp_correlation(backend, source,
                                                       destination, output),
            rt::Status::device_error);
  EXPECT_EQ(runtime.stop(), rt::Status::ok);
  EXPECT_EQ(runtime.query_device_timestamp_correlation(backend, source,
                                                       destination, output),
            rt::Status::invalid_state);
}

TEST(HeterogeneousMemory,
     CorrelationRejectsAnActiveFrameWithoutEnteringTheBackend) {
  using namespace std::chrono_literals;

  MemoryBackend probe;
  ActiveFrameGate gate;
  rt::Runtime runtime;
  ASSERT_EQ(runtime.configure(memory_config()), rt::Status::ok);
  rt::DeviceBackendHandle backend;
  ASSERT_EQ(register_memory_backend(runtime, probe, "active-correlation",
                                    backend),
            rt::Status::ok);
  rt::DeviceTimestampDomainHandle destination;
  rt::DeviceTimestampDomainHandle source;
  rt::HalV2TimestampDomain descriptor;
  ASSERT_TRUE(
      runtime.device_timestamp_domain_at(backend, 0, destination, descriptor));
  ASSERT_TRUE(
      runtime.device_timestamp_domain_at(backend, 1, source, descriptor));
  rt::PhaseHandle phase;
  ASSERT_EQ(runtime.register_callback({"hold", &hold_active_frame, &gate},
                                      phase),
            rt::Status::ok);
  ASSERT_EQ(runtime.finalize(), rt::Status::ok);
  ASSERT_EQ(runtime.start(), rt::Status::ok);

  rt::Status step_status = rt::Status::internal_error;
  std::thread step_thread([&] {
    step_status = runtime.step({1, 1ms, std::nullopt});
  });
  gate.entered.wait();

  rt::HalV2TimestampCorrelation output;
  output.generation = 99;
  EXPECT_EQ(runtime.query_device_timestamp_correlation(backend, source,
                                                       destination, output),
            rt::Status::invalid_state);
  EXPECT_EQ(output.generation, 0u);
  EXPECT_EQ(probe.correlation_calls, 0u);

  gate.release.count_down();
  step_thread.join();
  EXPECT_EQ(step_status, rt::Status::ok);
  EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(HeterogeneousMemory, RuntimeHandlesSnapshotsAndTokensAreIsolated) {
  MemoryBackend first;
  MemoryBackend second;
  second.snapshot.memory_domains[0].maximum_bytes = 2048;
  rt::Runtime one;
  rt::Runtime two;
  ASSERT_EQ(one.configure(memory_config()), rt::Status::ok);
  ASSERT_EQ(two.configure(memory_config()), rt::Status::ok);
  rt::DeviceBackendHandle one_backend;
  rt::DeviceBackendHandle two_backend;
  ASSERT_EQ(register_memory_backend(one, first, "same", one_backend),
            rt::Status::ok);
  ASSERT_EQ(register_memory_backend(two, second, "same", two_backend),
            rt::Status::ok);
  EXPECT_NE(one_backend.owner(), two_backend.owner());
  rt::DeviceMemoryDomainHandle handle;
  rt::HalV2MemoryDomain descriptor;
  ASSERT_TRUE(one.device_memory_domain_at(one_backend, 0, handle, descriptor));
  EXPECT_EQ(descriptor.maximum_bytes, 4096u);
  ASSERT_TRUE(two.device_memory_domain_at(two_backend, 0, handle, descriptor));
  EXPECT_EQ(descriptor.maximum_bytes, 2048u);
  EXPECT_FALSE(two.device_memory_domain_at(one_backend, 0, handle, descriptor));
}

TEST(HeterogeneousMemory, SemanticSnapshotFieldsParticipateInGraphIdentity) {
  MemoryBackend first;
  MemoryBackend second;
  second.snapshot.memory_domains[0].alignment = 16;
  rt::Runtime one;
  rt::Runtime two;
  ASSERT_EQ(one.configure(memory_config()), rt::Status::ok);
  ASSERT_EQ(two.configure(memory_config()), rt::Status::ok);
  rt::DeviceBackendHandle backend;
  ASSERT_EQ(register_memory_backend(one, first, "same", backend),
            rt::Status::ok);
  ASSERT_EQ(register_memory_backend(two, second, "same", backend),
            rt::Status::ok);
  ASSERT_EQ(one.finalize(), rt::Status::ok);
  ASSERT_EQ(two.finalize(), rt::Status::ok);
  EXPECT_NE(graph_id(one), graph_id(two));
}

TEST(HeterogeneousMemory, CoherentSubmissionTranslatesAndSyncRequiredRejects) {
  using namespace std::chrono_literals;
  {
    MemoryBackend probe;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(memory_config()), rt::Status::ok);
    rt::DeviceBackendHandle backend;
    ASSERT_EQ(register_memory_backend(runtime, probe, "coherent", backend),
              rt::Status::ok);
    const auto imported = domain_at(runtime, backend, 3);
    rt::DeviceBufferHandle buffer;
    ASSERT_EQ(runtime.register_device_buffer(
                  {"opaque",
                   backend,
                   imported,
                   {},
                   opaque_handle(),
                   64,
                   rt::HalV2MemoryOwnership::borrowed_opaque,
                   RTFW_DEVICE_BUFFER_DEVICE_READ,
                   rt::HalV2MemoryCoherency::device_only,
                   0},
                  buffer),
              rt::Status::ok);
    BufferCommand command{buffer, RTFW_DEVICE_ACCESS_READ, 8, 16};
    rt::PhaseHandle phase;
    ASSERT_EQ(runtime.register_device_phase(
                  {"read", backend, &submit_buffer, &command}, phase),
              rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(runtime.step({1, 1ms, std::nullopt}), rt::Status::ok);
    EXPECT_EQ(probe.submit_calls.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(probe.last_submission.buffer_count, 1u);
    EXPECT_EQ(probe.last_submission.buffers[0].buffer_token, 0x2001u);
    EXPECT_EQ(probe.last_submission.buffers[0].offset, 8u);
    EXPECT_EQ(probe.last_submission.buffers[0].bytes, 16u);
    EXPECT_EQ(probe.last_submission.buffers[0].access, RTFW_DEVICE_ACCESS_READ);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
  }

  {
    MemoryBackend probe;
    auto &imported = probe.snapshot.memory_domains[3];
    imported.coherency = static_cast<std::uint32_t>(
        rt::HalV2MemoryCoherency::explicit_flush_invalidate);
    imported.required_synchronization = rt::hal_v2_memory_sync_flush;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(memory_config()), rt::Status::ok);
    rt::DeviceBackendHandle backend;
    ASSERT_EQ(register_memory_backend(runtime, probe, "synchronized", backend),
              rt::Status::ok);
    const auto domain = domain_at(runtime, backend, 3);
    rt::DeviceBufferHandle buffer;
    ASSERT_EQ(runtime.register_device_buffer(
                  {"opaque",
                   backend,
                   domain,
                   {},
                   opaque_handle(),
                   64,
                   rt::HalV2MemoryOwnership::borrowed_opaque,
                   RTFW_DEVICE_BUFFER_DEVICE_READ,
                   rt::HalV2MemoryCoherency::explicit_flush_invalidate,
                   rt::hal_v2_memory_sync_flush},
                  buffer),
              rt::Status::ok);
    BufferCommand command{buffer, RTFW_DEVICE_ACCESS_READ, 0, 16};
    rt::PhaseHandle phase;
    ASSERT_EQ(runtime.register_device_phase(
                  {"read", backend, &submit_buffer, &command}, phase),
              rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    EXPECT_EQ(runtime.step({1, 1ms, std::nullopt}), rt::Status::device_error);
    EXPECT_EQ(probe.submit_calls.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
  }
}

TEST(MemoryPlan, HeterogeneousPayloadBytesRemainExternalAndControlIsExact) {
  const auto build = [](std::uint64_t opaque_bytes) {
    struct Result {
      rt::MemoryPlan plan{};
      rt::CpuMemoryPolicyReport report{};
    } result;
    MemoryBackend probe;
    rt::Runtime runtime;
    EXPECT_EQ(runtime.configure(memory_config()), rt::Status::ok);
    rt::DeviceBackendHandle backend;
    EXPECT_EQ(register_memory_backend(runtime, probe, "accounting", backend),
              rt::Status::ok);
    alignas(64) std::array<std::byte, 64> host_storage{};
    rt::DeviceBufferHandle buffer;
    EXPECT_EQ(
        runtime.register_device_buffer(
            {"host",
             backend,
             domain_at(runtime, backend, 0),
             host_storage,
             {},
             host_storage.size(),
             rt::HalV2MemoryOwnership::borrowed_host,
             RTFW_DEVICE_BUFFER_HOST_READ | RTFW_DEVICE_BUFFER_DEVICE_READ,
             rt::HalV2MemoryCoherency::host_coherent,
             0},
            buffer),
        rt::Status::ok);
    EXPECT_EQ(runtime.register_device_buffer(
                  {"opaque",
                   backend,
                   domain_at(runtime, backend, 3),
                   {},
                   opaque_handle(),
                   opaque_bytes,
                   rt::HalV2MemoryOwnership::borrowed_opaque,
                   RTFW_DEVICE_BUFFER_DEVICE_READ,
                   rt::HalV2MemoryCoherency::device_only,
                   0},
                  buffer),
              rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::ok);
    EXPECT_TRUE(runtime.memory_plan(result.plan));
    EXPECT_TRUE(runtime.cpu_memory_policy_report(result.report));
    return result;
  };

  const auto small = build(64);
  const auto large = build(128);
  EXPECT_EQ(small.plan.device_backend_count, 1u);
  EXPECT_EQ(small.plan.device_buffer_count, 2u);
  EXPECT_GT(small.plan.device_control_bytes, 0u);
  EXPECT_EQ(small.plan.device_control_bytes, large.plan.device_control_bytes);
  EXPECT_EQ(small.plan.planned_bytes, large.plan.planned_bytes);
  EXPECT_EQ(small.plan.planned_bytes, small.plan.runtime_control_bytes +
                                          small.plan.executor_control_bytes +
                                          small.plan.device_control_bytes +
                                          small.plan.phase_scratch_total_bytes +
                                          small.plan.task_scratch_total_bytes +
                                          small.plan.trace_storage_bytes);
  EXPECT_EQ(small.report.memory_count, 12u);
  const auto *device_control =
      find_memory_row(small.report, rt::memory_region_device_control);
  const auto *small_payload =
      find_memory_row(small.report, rt::memory_region_registered_device_buffer);
  const auto *large_payload =
      find_memory_row(large.report, rt::memory_region_registered_device_buffer);
  ASSERT_NE(device_control, nullptr);
  ASSERT_NE(small_payload, nullptr);
  ASSERT_NE(large_payload, nullptr);
  EXPECT_EQ(device_control->accounted_bytes, small.plan.device_control_bytes);
  EXPECT_EQ(device_control->accounting_exactness,
            rt::ResourceAccountingExactness::exact);
  EXPECT_EQ(small_payload->logical_region_count, 2u);
  EXPECT_EQ(small_payload->accounted_bytes, 128u);
  EXPECT_EQ(large_payload->accounted_bytes, 192u);
  EXPECT_EQ(small_payload->accounting_scope,
            rt::MemoryAccountingScope::informational_external);
  EXPECT_EQ(small_payload->accounting_exactness,
            rt::ResourceAccountingExactness::declared_only);
}

TEST(DeterminismReplay, HeterogeneousRegistrationIdentityIsSemanticOnly) {
  const auto opaque_id = [](std::size_t domain_index, std::uint64_t bytes,
                            rt::HalV2MemoryOwnership ownership,
                            std::uint32_t access, std::byte handle_value) {
    MemoryBackend probe;
    probe.snapshot.memory_domains[3].ownership_modes |=
        rt::hal_v2_memory_ownership_backend;
    rt::Runtime runtime;
    EXPECT_EQ(runtime.configure(memory_config()), rt::Status::ok);
    rt::DeviceBackendHandle backend;
    EXPECT_EQ(register_memory_backend(runtime, probe, "identity", backend),
              rt::Status::ok);
    rt::DeviceBufferHandle buffer;
    EXPECT_EQ(runtime.register_device_buffer(
                  {"memory",
                   backend,
                   domain_at(runtime, backend, domain_index),
                   {},
                   opaque_handle(handle_value),
                   bytes,
                   ownership,
                   access,
                   rt::HalV2MemoryCoherency::device_only,
                   0},
                  buffer),
              rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::ok);
    return graph_id(runtime);
  };
  const auto baseline =
      opaque_id(3, 64, rt::HalV2MemoryOwnership::borrowed_opaque,
                RTFW_DEVICE_BUFFER_DEVICE_READ, std::byte{0x44});
  EXPECT_NE(baseline,
            opaque_id(3, 128, rt::HalV2MemoryOwnership::borrowed_opaque,
                      RTFW_DEVICE_BUFFER_DEVICE_READ, std::byte{0x44}));
  EXPECT_NE(baseline,
            opaque_id(3, 64, rt::HalV2MemoryOwnership::backend,
                      RTFW_DEVICE_BUFFER_DEVICE_READ, std::byte{0x44}));
  EXPECT_NE(baseline,
            opaque_id(3, 64, rt::HalV2MemoryOwnership::borrowed_opaque,
                      RTFW_DEVICE_BUFFER_DEVICE_WRITE, std::byte{0x44}));
  EXPECT_NE(baseline,
            opaque_id(4, 64, rt::HalV2MemoryOwnership::borrowed_opaque,
                      RTFW_DEVICE_BUFFER_DEVICE_READ, std::byte{0x44}));
  EXPECT_NE(baseline,
            opaque_id(3, 64, rt::HalV2MemoryOwnership::borrowed_opaque,
                      RTFW_DEVICE_BUFFER_DEVICE_READ, std::byte{0x45}));

  const auto host_id = [](std::span<std::byte> storage) {
    MemoryBackend probe;
    rt::Runtime runtime;
    EXPECT_EQ(runtime.configure(memory_config()), rt::Status::ok);
    rt::DeviceBackendHandle backend;
    EXPECT_EQ(register_memory_backend(runtime, probe, "identity", backend),
              rt::Status::ok);
    rt::DeviceBufferHandle buffer;
    EXPECT_EQ(
        runtime.register_device_buffer(
            {"memory",
             backend,
             domain_at(runtime, backend, 0),
             storage,
             {},
             storage.size(),
             rt::HalV2MemoryOwnership::borrowed_host,
             RTFW_DEVICE_BUFFER_HOST_READ | RTFW_DEVICE_BUFFER_DEVICE_READ,
             rt::HalV2MemoryCoherency::host_coherent,
             0},
            buffer),
        rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::ok);
    return graph_id(runtime);
  };
  alignas(64) std::array<std::byte, 64> first{};
  alignas(64) std::array<std::byte, 64> second{};
  EXPECT_EQ(host_id(first), host_id(second));
  EXPECT_EQ(rt::checkpoint_schema_version, 1u);
  EXPECT_EQ(rt::input_log_schema_version, 1u);
}
