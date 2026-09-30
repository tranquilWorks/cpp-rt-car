#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <rt/extension_abi.h>
#include <rt/runtime.hpp>

extern "C" rtfw_status RTFW_EXTENSION_CALL rtfw_extension_entry_v1(
    const rtfw_extension_host_api_v1*, rtfw_extension_descriptor_v1*);
extern "C" rtfw_status RTFW_EXTENSION_CALL rtfw_extension_bad_entry_v1(
    const rtfw_extension_host_api_v1*, rtfw_extension_descriptor_v1*);
extern "C" std::uint32_t rtfw_extension_fixture_calls(void);
extern "C" void rtfw_extension_fixture_reset(void);

namespace {

enum Event : std::uint32_t {
    service_one_initialize = 1,
    service_two_initialize,
    backend_initialize_event,
    phase_execute,
    service_one_stop,
    service_two_stop,
    backend_shutdown_event,
    service_two_quiesce,
    service_two_shutdown,
    service_one_quiesce,
    service_one_shutdown,
    service_status,
};

struct FixtureState;

struct ServiceInstance {
    FixtureState* fixture = nullptr;
    std::uint32_t id = 0;
    bool initialized = false;
};

struct FixtureState {
    std::array<std::uint32_t, 32> events{};
    std::size_t event_count = 0;
    std::uint32_t phase_calls = 0;
    bool backend_initialized = false;
    bool malformed_service_status = false;
    std::uint32_t shutdown_failures = 0;
    std::uint32_t service_shutdown_failures = 0;
    std::uint32_t initialize_failures = 0;
    std::uint32_t request_stop_failures = 0;
    ServiceInstance first{this, 1, false};
    ServiceInstance second{this, 2, false};

    void push(std::uint32_t event) noexcept {
        if (event_count < events.size()) {
            events[event_count++] = event;
        }
    }
};

thread_local FixtureState* entry_state = nullptr;
thread_local std::uint32_t malformed_mode = 0;
thread_local rtfw_extension_handle_v1 retired_provisional{};

void set_name(char* output, const char* value) {
    const auto size = std::min<std::size_t>(
        std::strlen(value), RTFW_EXTENSION_IDENTIFIER_CAPACITY - 1);
    std::memset(output, 0, RTFW_EXTENSION_IDENTIFIER_CAPACITY);
    std::memcpy(output, value, size);
}

rtfw_callback_result phase_callback(
    void* opaque,
    const rtfw_callback_context* context) {
    auto& state = *static_cast<FixtureState*>(opaque);
    if (!context || context->struct_size != sizeof(*context) ||
        context->scratch == nullptr || context->tasks == nullptr) {
        return RTFW_CALLBACK_ERROR;
    }
    ++state.phase_calls;
    state.push(phase_execute);
    return RTFW_CALLBACK_OK;
}

rtfw_status RTFW_EXTENSION_CALL service_initialize(void* opaque) {
    auto& service = *static_cast<ServiceInstance*>(opaque);
    if (service.id == 1 && service.fixture->initialize_failures != 0) {
        --service.fixture->initialize_failures;
        return RTFW_STATUS_CALLBACK_FAILED;
    }
    service.initialized = true;
    service.fixture->push(
        service.id == 1 ? service_one_initialize : service_two_initialize);
    return RTFW_STATUS_OK;
}

rtfw_status RTFW_EXTENSION_CALL service_request_stop(void* opaque) {
    auto& service = *static_cast<ServiceInstance*>(opaque);
    service.fixture->push(
        service.id == 1 ? service_one_stop : service_two_stop);
    if (service.id == 1 && service.fixture->request_stop_failures != 0) {
        --service.fixture->request_stop_failures;
        return RTFW_STATUS_CALLBACK_FAILED;
    }
    return RTFW_STATUS_OK;
}

rtfw_status RTFW_EXTENSION_CALL service_quiesce(void* opaque) {
    auto& service = *static_cast<ServiceInstance*>(opaque);
    service.fixture->push(
        service.id == 1 ? service_one_quiesce : service_two_quiesce);
    return RTFW_STATUS_OK;
}

rtfw_status RTFW_EXTENSION_CALL service_shutdown(void* opaque) {
    auto& service = *static_cast<ServiceInstance*>(opaque);
    service.fixture->push(
        service.id == 1 ? service_one_shutdown : service_two_shutdown);
    if (service.id == 2 && service.fixture->service_shutdown_failures != 0) {
        --service.fixture->service_shutdown_failures;
        return RTFW_STATUS_INVALID_STATE;
    }
    service.initialized = false;
    return RTFW_STATUS_OK;
}

rtfw_status RTFW_EXTENSION_CALL service_get_status(
    void* opaque,
    rtfw_extension_service_status_v1* output) {
    auto& service = *static_cast<ServiceInstance*>(opaque);
    service.fixture->push(service_status);
    const auto requested_size = output->struct_size;
    *output = {};
    output->struct_size = requested_size;
    output->abi_version = RTFW_EXTENSION_ABI_VERSION;
    output->healthy = service.initialized ? 1u : 0u;
    output->status = RTFW_STATUS_OK;
    output->observed_value = service.id;
    if (service.fixture->malformed_service_status) {
        output->reserved[0] = 1;
    }
    return RTFW_STATUS_OK;
}

rtfw_device_status backend_capabilities(
    void* opaque,
    rtfw_device_capabilities* output) {
    if (!opaque || !output || output->struct_size < sizeof(*output)) {
        return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    }
    *output = {};
    output->struct_size = sizeof(*output);
    output->abi_version = RTFW_DEVICE_ABI_VERSION;
    output->max_in_flight = 4;
    output->max_registered_buffers = 4;
    output->max_buffer_bytes = 4096;
    output->inline_payload_capacity = RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY;
    output->buffer_ref_capacity = RTFW_DEVICE_BUFFER_REF_CAPACITY;
    output->supports_reset = 1;
    output->deterministic_mock = 1;
    set_name(output->backend_id, "extension.device.v1");
    return RTFW_DEVICE_STATUS_OK;
}

rtfw_device_status backend_initialize(
    void* opaque,
    const rtfw_device_init_config*) {
    auto& state = *static_cast<FixtureState*>(opaque);
    state.backend_initialized = true;
    state.push(backend_initialize_event);
    return RTFW_DEVICE_STATUS_OK;
}

rtfw_device_status backend_register_buffer(
    void*, const rtfw_device_buffer_registration*, std::uint64_t*) {
    return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
}

rtfw_device_status backend_unregister_buffer(void*, std::uint64_t) {
    return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
}

rtfw_device_status backend_submit(void*, const rtfw_device_submission*) {
    return RTFW_DEVICE_STATUS_OK;
}

rtfw_device_status backend_poll(
    void*, rtfw_device_completion*, std::uint64_t, std::uint64_t* count) {
    if (!count) {
        return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    }
    *count = 0;
    return RTFW_DEVICE_STATUS_OK;
}

rtfw_device_status backend_cancel(void*, std::uint64_t) {
    return RTFW_DEVICE_STATUS_CANCELED;
}

rtfw_device_status backend_health(void*, rtfw_device_health* output) {
    if (!output || output->struct_size < sizeof(*output)) {
        return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    }
    const auto size = output->struct_size;
    *output = {};
    output->struct_size = size;
    output->state = RTFW_DEVICE_HEALTH_HEALTHY;
    output->last_status = RTFW_DEVICE_STATUS_OK;
    return RTFW_DEVICE_STATUS_OK;
}

rtfw_device_status backend_reset(void*) {
    return RTFW_DEVICE_STATUS_OK;
}

rtfw_device_status backend_shutdown(void* opaque) {
    auto& state = *static_cast<FixtureState*>(opaque);
    state.push(backend_shutdown_event);
    if (state.shutdown_failures != 0) {
        --state.shutdown_failures;
        return RTFW_DEVICE_STATUS_RESET_REQUIRED;
    }
    state.backend_initialized = false;
    return RTFW_DEVICE_STATUS_OK;
}

rtfw_extension_service_v1 make_service(
    const char* name,
    ServiceInstance& instance) {
    rtfw_extension_service_v1 service{};
    service.struct_size = RTFW_EXTENSION_SERVICE_V1_REQUIRED_SIZE;
    service.abi_version = RTFW_EXTENSION_ABI_VERSION;
    set_name(service.name, name);
    set_name(service.interface_name, "test.control");
    service.interface_version = 3;
    service.api.struct_size = RTFW_EXTENSION_SERVICE_API_V1_REQUIRED_SIZE;
    service.api.abi_version = RTFW_EXTENSION_ABI_VERSION;
    service.api.instance = &instance;
    service.api.initialize = &service_initialize;
    service.api.request_stop = &service_request_stop;
    service.api.quiesce = &service_quiesce;
    service.api.shutdown = &service_shutdown;
    service.api.status = &service_get_status;
    return service;
}

rtfw_device_backend_api make_backend(FixtureState& state) {
    rtfw_device_backend_api api{};
    api.struct_size = sizeof(api);
    api.abi_version = RTFW_DEVICE_ABI_VERSION;
    api.instance = &state;
    api.get_capabilities = &backend_capabilities;
    api.initialize = &backend_initialize;
    api.register_buffer = &backend_register_buffer;
    api.unregister_buffer = &backend_unregister_buffer;
    api.submit = &backend_submit;
    api.poll = &backend_poll;
    api.cancel = &backend_cancel;
    api.get_health = &backend_health;
    api.reset = &backend_reset;
    api.shutdown = &backend_shutdown;
    return api;
}

extern "C" rtfw_status RTFW_EXTENSION_CALL full_entry(
    const rtfw_extension_host_api_v1* host,
    rtfw_extension_descriptor_v1* descriptor) {
    auto& state = *entry_state;
    rtfw_extension_phase_v1 phase{};
    phase.struct_size = RTFW_EXTENSION_PHASE_V1_REQUIRED_SIZE + 64;
    phase.abi_version = RTFW_EXTENSION_ABI_VERSION;
    set_name(phase.name, "extension.phase");
    phase.callback = &phase_callback;
    phase.user_data = &state;
    rtfw_extension_backend_v1 backend{};
    backend.struct_size = RTFW_EXTENSION_BACKEND_V1_REQUIRED_SIZE;
    backend.abi_version = RTFW_EXTENSION_ABI_VERSION;
    set_name(backend.name, "extension.backend");
    backend.api = make_backend(state);
    auto first = make_service("extension.service.one", state.first);
    auto second = make_service("extension.service.two", state.second);
    rtfw_extension_resource_v1 resource{};
    resource.struct_size = RTFW_EXTENSION_RESOURCE_V1_REQUIRED_SIZE;
    resource.abi_version = RTFW_EXTENSION_ABI_VERSION;
    set_name(resource.name, "extension.resource");
    rtfw_extension_handle_v1 phase_handle{};
    rtfw_extension_handle_v1 backend_handle{};
    rtfw_extension_handle_v1 first_handle{};
    rtfw_extension_handle_v1 second_handle{};
    rtfw_extension_handle_v1 resource_handle{};
    if (host->stage_phase(host->context, &phase, &phase_handle) ||
        host->stage_backend(host->context, &backend, &backend_handle) ||
        host->stage_service(host->context, &first, &first_handle) ||
        host->stage_service(host->context, &second, &second_handle) ||
        host->stage_resource(host->context, &resource, &resource_handle)) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    rtfw_extension_relationship_v1 relation{};
    relation.struct_size = RTFW_EXTENSION_RELATIONSHIP_V1_REQUIRED_SIZE;
    relation.abi_version = RTFW_EXTENSION_ABI_VERSION;
    relation.kind = RTFW_EXTENSION_RELATIONSHIP_PHASE_RESOURCE;
    relation.access = RTFW_EXTENSION_RESOURCE_ACCESS_WRITE;
    relation.first = phase_handle;
    relation.second = resource_handle;
    if (host->stage_relationship(host->context, &relation)) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    relation = {};
    relation.struct_size = RTFW_EXTENSION_RELATIONSHIP_V1_REQUIRED_SIZE;
    relation.abi_version = RTFW_EXTENSION_ABI_VERSION;
    relation.kind = RTFW_EXTENSION_RELATIONSHIP_SERVICE_BACKEND;
    relation.first = first_handle;
    relation.second = backend_handle;
    if (host->stage_relationship(host->context, &relation)) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    *descriptor = {};
    descriptor->struct_size = RTFW_EXTENSION_DESCRIPTOR_V1_REQUIRED_SIZE + 32;
    descriptor->current_abi_version = 2;
    descriptor->min_compatible_abi_version = 1;
    set_name(descriptor->name, "extension.full");
    set_name(descriptor->version, "2.4.0");
    descriptor->phase_count = 1;
    descriptor->backend_count = 1;
    descriptor->service_count = 2;
    descriptor->resource_count = 1;
    descriptor->relationship_count = 2;
    return RTFW_STATUS_OK;
}

rtfw_status RTFW_EXTENSION_CALL throwing_entry(
    const rtfw_extension_host_api_v1*, rtfw_extension_descriptor_v1*) {
    throw std::runtime_error("extension exception");
}

extern "C" rtfw_status RTFW_EXTENSION_CALL malformed_entry(
    const rtfw_extension_host_api_v1* host,
    rtfw_extension_descriptor_v1* descriptor) {
    *descriptor = {};
    descriptor->struct_size = RTFW_EXTENSION_DESCRIPTOR_V1_REQUIRED_SIZE;
    descriptor->current_abi_version = 1;
    descriptor->min_compatible_abi_version = 1;
    set_name(descriptor->name, "malformed.extension");
    set_name(descriptor->version, "1");
    if (malformed_mode == 1) {
        descriptor->current_abi_version = 2;
        descriptor->min_compatible_abi_version = 2;
    } else if (malformed_mode == 2) {
        descriptor->reserved[0] = 1;
    } else if (malformed_mode == 3) {
        descriptor->name[1] = '\0';
        descriptor->name[2] = 'x';
    } else if (malformed_mode == 4) {
        descriptor->phase_count = UINT64_MAX;
    } else if (malformed_mode == 5) {
        rtfw_extension_phase_v1 phase{};
        rtfw_extension_handle_v1 handle{};
        phase.struct_size = RTFW_EXTENSION_PHASE_V1_REQUIRED_SIZE;
        phase.abi_version = 1;
        set_name(phase.name, "null.callback");
        (void)host->stage_phase(host->context, &phase, &handle);
    } else if (malformed_mode == 6) {
        descriptor->struct_size = UINT32_MAX;
        descriptor->current_abi_version = 0;
    } else if (malformed_mode == 7) {
        return static_cast<rtfw_status>(-999);
    }
    return RTFW_STATUS_OK;
}

extern "C" rtfw_status RTFW_EXTENSION_CALL retire_provisional_entry(
    const rtfw_extension_host_api_v1* host,
    rtfw_extension_descriptor_v1* descriptor) {
    rtfw_extension_phase_v1 phase{};
    phase.struct_size = RTFW_EXTENSION_PHASE_V1_REQUIRED_SIZE;
    phase.abi_version = 1;
    set_name(phase.name, "retired.phase");
    phase.callback = &phase_callback;
    phase.user_data = entry_state;
    if (host->stage_phase(
            host->context, &phase, &retired_provisional) != RTFW_STATUS_OK) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    descriptor->struct_size = 1;
    return RTFW_STATUS_OK;
}

extern "C" rtfw_status RTFW_EXTENSION_CALL reuse_provisional_entry(
    const rtfw_extension_host_api_v1* host,
    rtfw_extension_descriptor_v1* descriptor) {
    rtfw_extension_resource_v1 resource{};
    resource.struct_size = RTFW_EXTENSION_RESOURCE_V1_REQUIRED_SIZE;
    resource.abi_version = 1;
    set_name(resource.name, "fresh.resource");
    rtfw_extension_handle_v1 resource_handle{};
    if (host->stage_resource(
            host->context, &resource, &resource_handle) != RTFW_STATUS_OK) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    rtfw_extension_relationship_v1 relation{};
    relation.struct_size = RTFW_EXTENSION_RELATIONSHIP_V1_REQUIRED_SIZE;
    relation.abi_version = 1;
    relation.kind = RTFW_EXTENSION_RELATIONSHIP_PHASE_RESOURCE;
    relation.access = RTFW_EXTENSION_RESOURCE_ACCESS_READ;
    relation.first = retired_provisional;
    relation.second = resource_handle;
    (void)host->stage_relationship(host->context, &relation);
    *descriptor = {};
    descriptor->struct_size = RTFW_EXTENSION_DESCRIPTOR_V1_REQUIRED_SIZE;
    descriptor->current_abi_version = 1;
    descriptor->min_compatible_abi_version = 1;
    set_name(descriptor->name, "reuse.provisional");
    set_name(descriptor->version, "1");
    descriptor->resource_count = 1;
    return RTFW_STATUS_OK;
}

rt::RuntimeConfig test_config() {
    rt::RuntimeConfig config;
    config.callback_capacity = 4;
    config.worker_count = 1;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    config.scratch_bytes = 128;
    config.trace_capacity = 32;
    config.device_backend_capacity = 2;
    config.device_outstanding_capacity = 4;
    config.device_completion_batch = 2;
    return config;
}

rt::CallbackResult cpp_phase(void*, const rt::CallbackContext&) {
    return rt::CallbackResult::ok;
}

struct IdentityAndMemory {
    std::uint64_t config_id = 0;
    std::uint64_t graph_id = 0;
    std::uint64_t replay_id = 0;
    rt::MemoryPlan memory{};
};

IdentityAndMemory capture_identity(
    rtfw_extension_entry_fn_v1 entry,
    FixtureState* state = nullptr) {
    if (state) {
        entry_state = state;
    }
    rt::Runtime runtime;
    EXPECT_EQ(runtime.configure(test_config()), rt::Status::ok);
    if (entry) {
        rt::ExtensionHandle extension;
        EXPECT_EQ(runtime.register_extension(entry, extension), rt::Status::ok);
    }
    EXPECT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();
    IdentityAndMemory result;
    EXPECT_TRUE(runtime.memory_plan(result.memory));
    rt::ObservabilityMetadata observability;
    EXPECT_EQ(
        runtime.observability_metadata(observability),
        rt::Status::ok);
    result.config_id = observability.config_id;
    std::size_t bytes = 0;
    EXPECT_EQ(runtime.checkpoint_size(bytes), rt::Status::ok);
    std::vector<std::byte> checkpoint(bytes);
    rt::ArtifactWriteResult write;
    EXPECT_EQ(runtime.write_checkpoint(0, checkpoint, write), rt::Status::ok);
    rt::CheckpointMetadata metadata;
    EXPECT_EQ(
        rt::inspect_checkpoint_artifact(checkpoint, metadata),
        rt::Status::ok);
    result.graph_id = metadata.graph_id;
    result.replay_id = metadata.replay_id;
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    return result;
}

} // namespace

TEST(ExtensionAbi, VersionOneLayoutsAndConstantsAreFixed) {
    EXPECT_EQ(RTFW_EXTENSION_ABI_VERSION, 1u);
    EXPECT_EQ(RTFW_EXTENSION_ABI_MIN_COMPATIBLE_VERSION, 1u);
    EXPECT_STREQ(RTFW_EXTENSION_ENTRY_SYMBOL_V1, "rtfw_extension_entry_v1");
    EXPECT_EQ(sizeof(rtfw_extension_handle_v1), 16u);
    EXPECT_EQ(sizeof(rtfw_extension_phase_v1), 120u);
    EXPECT_EQ(sizeof(rtfw_extension_backend_v1), 264u);
    EXPECT_EQ(sizeof(rtfw_extension_service_status_v1), 64u);
    EXPECT_EQ(sizeof(rtfw_extension_service_api_v1), 88u);
    EXPECT_EQ(sizeof(rtfw_extension_service_v1), 264u);
    EXPECT_EQ(sizeof(rtfw_extension_resource_v1), 104u);
    EXPECT_EQ(sizeof(rtfw_extension_relationship_v1), 80u);
    EXPECT_EQ(sizeof(rtfw_extension_host_api_v1), 136u);
    EXPECT_EQ(sizeof(rtfw_extension_descriptor_v1), 216u);
    EXPECT_EQ(alignof(rtfw_extension_handle_v1), 4u);
    EXPECT_EQ(alignof(rtfw_extension_phase_v1), 8u);
    EXPECT_EQ(alignof(rtfw_extension_backend_v1), 8u);
    EXPECT_EQ(alignof(rtfw_extension_service_status_v1), 8u);
    EXPECT_EQ(alignof(rtfw_extension_service_api_v1), 8u);
    EXPECT_EQ(alignof(rtfw_extension_service_v1), 8u);
    EXPECT_EQ(alignof(rtfw_extension_resource_v1), 8u);
    EXPECT_EQ(alignof(rtfw_extension_relationship_v1), 8u);
    EXPECT_EQ(alignof(rtfw_extension_host_api_v1), 8u);
    EXPECT_EQ(alignof(rtfw_extension_descriptor_v1), 8u);
    EXPECT_EQ(offsetof(rtfw_extension_handle_v1, generation), 12u);
    EXPECT_EQ(offsetof(rtfw_extension_phase_v1, callback), 72u);
    EXPECT_EQ(offsetof(rtfw_extension_backend_v1, api), 72u);
    EXPECT_EQ(offsetof(rtfw_extension_service_status_v1, observed_value), 24u);
    EXPECT_EQ(offsetof(rtfw_extension_service_api_v1, initialize), 16u);
    EXPECT_EQ(offsetof(rtfw_extension_service_v1, api), 144u);
    EXPECT_EQ(offsetof(rtfw_extension_resource_v1, reserved), 72u);
    EXPECT_EQ(offsetof(rtfw_extension_relationship_v1, second), 32u);
    EXPECT_EQ(offsetof(rtfw_extension_host_api_v1, stage_phase), 64u);
    EXPECT_EQ(offsetof(rtfw_extension_descriptor_v1, phase_count), 144u);
}

TEST(ExtensionRegistration, CFixtureUsesDirectEntryAndOrdinaryGraph) {
    rtfw_extension_fixture_reset();
    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
    rt::ExtensionHandle handle;
    ASSERT_EQ(
        runtime.register_extension(&rtfw_extension_entry_v1, handle),
        rt::Status::ok);
    EXPECT_TRUE(handle.valid());
    EXPECT_EQ(runtime.extension_count(), 1u);
    EXPECT_EQ(runtime.callback_count(), 1u);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(
        runtime.step(rt::HostFrameContext{
            9, std::chrono::milliseconds(1), std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(rtfw_extension_fixture_calls(), 1u);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
    bool ready = false;
    ASSERT_EQ(runtime.detach_extension(handle, ready), rt::Status::ok);
    EXPECT_TRUE(ready);
}

TEST(ExtensionRegistration, MalformedLateOutputAndExceptionAreAtomic) {
    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
    rt::ExtensionHandle output{7, RTFW_EXTENSION_HANDLE_EXTENSION, 8, 9};
    EXPECT_EQ(
        runtime.register_extension(&rtfw_extension_bad_entry_v1, output),
        rt::Status::invalid_argument);
    EXPECT_FALSE(output.valid());
    EXPECT_EQ(runtime.extension_count(), 0u);
    EXPECT_EQ(runtime.callback_count(), 0u);
    EXPECT_EQ(
        runtime.register_extension(&throwing_entry, output),
        rt::Status::callback_failed);
    EXPECT_EQ(runtime.extension_count(), 0u);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
    EXPECT_EQ(
        runtime.register_extension(&rtfw_extension_entry_v1, output),
        rt::Status::ok);
}

TEST(ExtensionRegistration, NegotiationAndMalformedRecordsFailClosed) {
    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
    rt::ExtensionHandle output;
    EXPECT_EQ(
        runtime.register_extension(nullptr, output),
        rt::Status::invalid_argument);
    for (malformed_mode = 1; malformed_mode <= 7; ++malformed_mode) {
        const auto expected = malformed_mode == 1
            ? rt::Status::incompatible_abi
            : malformed_mode == 7
                ? rt::Status::internal_error
                : rt::Status::invalid_argument;
        EXPECT_EQ(runtime.register_extension(&malformed_entry, output), expected)
            << malformed_mode;
        EXPECT_FALSE(output.valid());
        EXPECT_EQ(runtime.extension_count(), 0u);
        EXPECT_EQ(runtime.callback_count(), 0u);
    }
}

TEST(ExtensionRegistration, FailedProvisionalGenerationCannotBeReused) {
    FixtureState state;
    entry_state = &state;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
    rt::ExtensionHandle output;
    EXPECT_EQ(
        runtime.register_extension(&retire_provisional_entry, output),
        rt::Status::invalid_argument);
    ASSERT_NE(retired_provisional.generation, 0u);
    EXPECT_EQ(
        runtime.register_extension(&reuse_provisional_entry, output),
        rt::Status::invalid_argument);
    EXPECT_EQ(runtime.extension_count(), 0u);
    EXPECT_EQ(runtime.callback_count(), 0u);
    EXPECT_EQ(
        runtime.register_extension(&rtfw_extension_entry_v1, output),
        rt::Status::ok);
    EXPECT_GT(output.generation, retired_provisional.generation);
}

TEST(ExtensionRegistration, ExistingNameAndCapacityConflictsStayAtomic) {
    {
        rt::Runtime runtime;
        auto config = test_config();
        config.callback_capacity = 1;
        ASSERT_EQ(runtime.configure(config), rt::Status::ok);
        ASSERT_EQ(
            runtime.register_callback({"existing", &cpp_phase, nullptr}),
            rt::Status::ok);
        rt::ExtensionHandle output;
        EXPECT_EQ(
            runtime.register_extension(&rtfw_extension_entry_v1, output),
            rt::Status::capacity_exceeded);
        EXPECT_EQ(runtime.callback_count(), 1u);
        EXPECT_EQ(runtime.extension_count(), 0u);
    }
    {
        rt::Runtime runtime;
        ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
        ASSERT_EQ(
            runtime.register_callback({"c.phase", &cpp_phase, nullptr}),
            rt::Status::ok);
        rt::ExtensionHandle output;
        EXPECT_EQ(
            runtime.register_extension(&rtfw_extension_entry_v1, output),
            rt::Status::invalid_argument);
        EXPECT_EQ(runtime.callback_count(), 1u);
        EXPECT_EQ(runtime.extension_count(), 0u);
    }
}

TEST(ExtensionLifecycle, ServicesBackendsRetryDetachAndStaleHandles) {
    FixtureState state;
    state.shutdown_failures = 1;
    entry_state = &state;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
    rt::ExtensionHandle handle;
    ASSERT_EQ(runtime.register_extension(&full_entry, handle), rt::Status::ok);
    rt::ExtensionInfo info;
    ASSERT_EQ(runtime.extension_info(handle, info), rt::Status::ok);
    EXPECT_EQ(info.negotiated_abi_version, 1u);
    EXPECT_EQ(info.phase_count, 1u);
    EXPECT_EQ(info.backend_count, 1u);
    EXPECT_EQ(info.service_count, 2u);
    EXPECT_EQ(info.relationship_count, 2u);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_GE(state.event_count, 3u);
    EXPECT_EQ(state.events[0], service_one_initialize);
    EXPECT_EQ(state.events[1], service_two_initialize);
    EXPECT_EQ(state.events[2], backend_initialize_event);
    rtfw_extension_service_status_v1 service_status{};
    ASSERT_EQ(
        runtime.extension_service_status(handle, 0, service_status),
        rt::Status::ok);
    EXPECT_EQ(service_status.healthy, 1u);
    EXPECT_EQ(service_status.observed_value, 1u);
    state.malformed_service_status = true;
    service_status.observed_value = 77;
    EXPECT_EQ(
        runtime.extension_service_status(handle, 0, service_status),
        rt::Status::invalid_argument);
    EXPECT_EQ(service_status.observed_value, 77u);
    state.malformed_service_status = false;
    ASSERT_EQ(
        runtime.step(rt::HostFrameContext{
            1, std::chrono::milliseconds(1), std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(state.phase_calls, 1u);

    EXPECT_EQ(runtime.stop(), rt::Status::device_reset_required);
    ASSERT_EQ(runtime.extension_info(handle, info), rt::Status::ok);
    EXPECT_EQ(info.state, rt::ExtensionLifecycleState::cleanup_pending);
    EXPECT_FALSE(info.unload_ready);
    bool ready = true;
    EXPECT_EQ(
        runtime.detach_extension(handle, ready),
        rt::Status::invalid_state);
    EXPECT_TRUE(ready);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
    ASSERT_EQ(runtime.detach_extension(handle, ready), rt::Status::ok);
    EXPECT_TRUE(ready);
    const auto retired = info.generation + 1;
    ASSERT_TRUE(runtime.extension_at(0, info));
    EXPECT_EQ(info.state, rt::ExtensionLifecycleState::detached);
    EXPECT_EQ(info.generation, retired);
    EXPECT_TRUE(info.unload_ready);
    rt::ExtensionInfo unchanged = info;
    EXPECT_EQ(
        runtime.extension_info(handle, unchanged),
        rt::Status::invalid_handle);
    EXPECT_EQ(unchanged.generation, info.generation);
    ready = false;
    EXPECT_EQ(
        runtime.detach_extension(handle, ready),
        rt::Status::invalid_handle);
    EXPECT_FALSE(ready);

    ASSERT_GE(state.event_count, 11u);
    EXPECT_LT(
        std::find(
            state.events.begin(), state.events.end(), backend_shutdown_event),
        std::find(
            state.events.begin(), state.events.end(), service_one_quiesce));
    EXPECT_LT(
        std::find(
            state.events.begin(), state.events.end(), service_two_shutdown),
        std::find(
            state.events.begin(), state.events.end(), service_one_quiesce));
}

TEST(ExtensionLifecycle, OwnersAndGenerationsAreRuntimeLocal) {
    FixtureState first_state;
    FixtureState second_state;
    rt::Runtime first;
    rt::Runtime second;
    ASSERT_EQ(first.configure(test_config()), rt::Status::ok);
    ASSERT_EQ(second.configure(test_config()), rt::Status::ok);
    rt::ExtensionHandle first_handle;
    rt::ExtensionHandle second_handle;
    entry_state = &first_state;
    ASSERT_EQ(first.register_extension(&full_entry, first_handle), rt::Status::ok);
    entry_state = &second_state;
    ASSERT_EQ(
        second.register_extension(&full_entry, second_handle), rt::Status::ok);
    EXPECT_NE(first_handle.owner, second_handle.owner);
    rt::ExtensionInfo sentinel;
    sentinel.generation = 99;
    EXPECT_EQ(
        first.extension_info(second_handle, sentinel),
        rt::Status::invalid_handle);
    EXPECT_EQ(sentinel.generation, 99u);
    auto wrong_kind = first_handle;
    wrong_kind.kind = RTFW_EXTENSION_HANDLE_SERVICE;
    EXPECT_EQ(
        first.extension_info(wrong_kind, sentinel),
        rt::Status::invalid_handle);
    EXPECT_EQ(sentinel.generation, 99u);
    ASSERT_EQ(first.finalize(), rt::Status::ok) << first.last_error();
    ASSERT_EQ(second.finalize(), rt::Status::ok) << second.last_error();
    ASSERT_EQ(first.stop(), rt::Status::ok);
    ASSERT_EQ(second.stop(), rt::Status::ok);
}

TEST(ExtensionLifecycle, ServiceStartupFailureRollsBackAndRecovers) {
    FixtureState state;
    state.initialize_failures = 1;
    entry_state = &state;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
    rt::ExtensionHandle handle;
    ASSERT_EQ(runtime.register_extension(&full_entry, handle), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    EXPECT_EQ(runtime.start(), rt::Status::callback_failed);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    ASSERT_EQ(runtime.start(), rt::Status::ok) << runtime.last_error();
    EXPECT_TRUE(state.first.initialized);
    EXPECT_TRUE(state.second.initialized);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(ExtensionLifecycle, RequestStopFailureRetainsFirstErrorAndRetriesOwner) {
    FixtureState state;
    state.request_stop_failures = 1;
    entry_state = &state;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
    rt::ExtensionHandle handle;
    ASSERT_EQ(runtime.register_extension(&full_entry, handle), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    EXPECT_EQ(runtime.stop(), rt::Status::callback_failed);
    rt::ExtensionInfo info;
    ASSERT_EQ(runtime.extension_info(handle, info), rt::Status::ok);
    EXPECT_EQ(info.state, rt::ExtensionLifecycleState::cleanup_pending);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(
        std::count(
            state.events.begin(), state.events.end(), service_two_stop),
        1);
    EXPECT_EQ(
        std::count(
            state.events.begin(), state.events.end(), service_one_stop),
        2);
}

TEST(ExtensionLifecycle, ServiceShutdownFailureRetriesOnlyUnresolvedOwner) {
    FixtureState state;
    state.service_shutdown_failures = 1;
    entry_state = &state;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(test_config()), rt::Status::ok);
    rt::ExtensionHandle handle;
    ASSERT_EQ(runtime.register_extension(&full_entry, handle), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    EXPECT_EQ(runtime.stop(), rt::Status::invalid_state);
    rt::ExtensionInfo info;
    ASSERT_EQ(runtime.extension_info(handle, info), rt::Status::ok);
    EXPECT_EQ(info.state, rt::ExtensionLifecycleState::cleanup_pending);
    EXPECT_FALSE(info.unload_ready);
    EXPECT_EQ(
        std::count(
            state.events.begin(), state.events.end(), service_two_quiesce),
        1);
    EXPECT_EQ(
        std::count(
            state.events.begin(), state.events.end(), service_two_shutdown),
        1);
    EXPECT_EQ(
        std::count(
            state.events.begin(), state.events.end(), service_one_shutdown),
        1);

    ASSERT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(
        std::count(
            state.events.begin(), state.events.end(), service_two_quiesce),
        1);
    EXPECT_EQ(
        std::count(
            state.events.begin(), state.events.end(), service_two_shutdown),
        2);
    EXPECT_EQ(
        std::count(
            state.events.begin(), state.events.end(), service_one_shutdown),
        1);
}

TEST(MemoryPlan, ExtensionStorageIsAccountedInExistingControlRows) {
    FixtureState state;
    const auto base = capture_identity(nullptr);
    const auto phase = capture_identity(&rtfw_extension_entry_v1);
    const auto full = capture_identity(&full_entry, &state);
    EXPECT_GT(phase.memory.runtime_control_bytes,
              base.memory.runtime_control_bytes);
    EXPECT_EQ(phase.memory.device_control_bytes,
              base.memory.device_control_bytes);
    EXPECT_GT(full.memory.runtime_control_bytes,
              phase.memory.runtime_control_bytes);
    EXPECT_GT(full.memory.device_control_bytes,
              base.memory.device_control_bytes);
    EXPECT_EQ(
        full.memory.planned_bytes,
        full.memory.runtime_control_bytes +
            full.memory.executor_control_bytes +
            full.memory.device_control_bytes +
            full.memory.phase_scratch_total_bytes +
            full.memory.task_scratch_total_bytes +
            full.memory.trace_storage_bytes);
}

TEST(DeterminismReplay, ExtensionSemanticsParticipateWithoutPointers) {
    FixtureState first_state;
    FixtureState second_state;
    const auto base = capture_identity(nullptr);
    const auto first = capture_identity(&full_entry, &first_state);
    const auto second = capture_identity(&full_entry, &second_state);
    EXPECT_NE(first.config_id, base.config_id);
    EXPECT_NE(first.graph_id, base.graph_id);
    EXPECT_NE(first.replay_id, base.replay_id);
    EXPECT_EQ(first.config_id, second.config_id);
    EXPECT_EQ(first.graph_id, second.graph_id);
    EXPECT_EQ(first.replay_id, second.replay_id);
}
