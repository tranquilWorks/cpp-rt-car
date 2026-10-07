#include "../../integrations/host_lifecycle/lifecycle.hpp"
#include "../../integrations/host_lifecycle/time.hpp"
#include <algorithm>
#include "../cuda_physics/allocation_guard.hpp"
#include <rt/sdk.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static unsigned checks = 0;
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "lifecycle check failed at %d: %s\n", __LINE__, #expression); \
    std::abort(); } } while (false)

namespace {
struct Data {
    rtfw_status initialize = RTFW_STATUS_OK;
    rtfw_status request = RTFW_STATUS_OK;
    rtfw_status quiesce = RTFW_STATUS_OK;
    rtfw_status shutdown = RTFW_STATUS_OK;
    bool fail_frame = false;
    unsigned entries = 0, frames = 0;
    rt::Status (*reenter)() noexcept = nullptr;
    rt::Status reentry = rt::Status::ok;
};
std::array<Data, 4> data;
rtfw_callback_result callback(void* opaque, const rtfw_callback_context*) {
    auto& value = *static_cast<Data*>(opaque);
    if (value.reenter) value.reentry = value.reenter();
    ++value.frames;
    return value.fail_frame ? RTFW_CALLBACK_ERROR : RTFW_CALLBACK_OK;
}
rtfw_status RTFW_EXTENSION_CALL initialize(void* opaque) { return static_cast<Data*>(opaque)->initialize; }
rtfw_status RTFW_EXTENSION_CALL request(void* opaque) { return static_cast<Data*>(opaque)->request; }
rtfw_status RTFW_EXTENSION_CALL quiesce(void* opaque) { return static_cast<Data*>(opaque)->quiesce; }
rtfw_status RTFW_EXTENSION_CALL shutdown(void* opaque) { return static_cast<Data*>(opaque)->shutdown; }
template<unsigned Index>
rtfw_status RTFW_EXTENSION_CALL entry(const rtfw_extension_host_api_v1* host,
                                     rtfw_extension_descriptor_v1* descriptor) {
    auto& value = data[Index];
    ++value.entries;
    if (value.reenter) value.reentry = value.reenter();
    rtfw_extension_phase_v1 phase{};
    phase.struct_size = static_cast<std::uint32_t>(sizeof(phase)); phase.abi_version = 1;
    std::snprintf(phase.name, sizeof(phase.name), "test.phase%u", Index);
    phase.callback = callback; phase.user_data = &value;
    rtfw_extension_handle_v1 handle{};
    auto status = host->stage_phase(host->context, &phase, &handle);
    if (status != RTFW_STATUS_OK) return status;
    rtfw_extension_service_v1 service{};
    service.struct_size = static_cast<std::uint32_t>(sizeof(service)); service.abi_version = 1;
    std::snprintf(service.name, sizeof(service.name), "test.service%u", Index);
    std::strcpy(service.interface_name, "test.owner"); service.interface_version = 1;
    service.api.struct_size = static_cast<std::uint32_t>(sizeof(service.api)); service.api.abi_version = 1;
    service.api.instance = &value;
    service.api.initialize = initialize; service.api.request_stop = request;
    service.api.quiesce = quiesce; service.api.shutdown = shutdown;
    status = host->stage_service(host->context, &service, &handle);
    if (status != RTFW_STATUS_OK) return status;
    *descriptor = {};
    descriptor->struct_size = static_cast<std::uint32_t>(sizeof(*descriptor));
    descriptor->current_abi_version = 1; descriptor->min_compatible_abi_version = 1;
    std::snprintf(descriptor->name, sizeof(descriptor->name), "test.module%u", Index);
    std::strcpy(descriptor->version, "1.0"); descriptor->phase_count = 1; descriptor->service_count = 1;
    return RTFW_STATUS_OK;
}
rtfw_status RTFW_EXTENSION_CALL rejected_entry(const rtfw_extension_host_api_v1*,
                                              rtfw_extension_descriptor_v1*) {
    return RTFW_STATUS_CALLBACK_FAILED;
}
using Registry = rtfw_host::Registry<2, 2, 2>;
Registry* reentrant = nullptr;
rtfw_host::ModuleHandle reentrant_module;
rt::Status reenter() noexcept { return reentrant->release_module(reentrant_module); }
rt::RuntimeConfig config() {
    rt::RuntimeConfig value; value.worker_count = 1; return value;
}
template<class R>
void running(R& registry, rtfw_host::WorldHandle world, rtfw_host::ModuleHandle module) {
    rt::ExtensionHandle extension;
    CHECK(registry.attach(world, module, extension) == rt::Status::ok);
    CHECK(registry.finalize(world) == rt::Status::ok);
    CHECK(registry.start(world) == rt::Status::ok);
}

void identity_and_capacity() {
    data = {};
    Registry first, second;
    CHECK(first.ready() && second.ready());
    int a = 0, b = 0, c = 0, d = 0;
    rtfw_host::WorldHandle world, other, spare, output{99, 88, 77};
    CHECK(first.create_world(nullptr, config(), output) == rt::Status::invalid_argument);
    auto invalid = config(); invalid.worker_count = 0;
    CHECK(first.create_world(&a, invalid, output) == rt::Status::invalid_config);
    CHECK(output.registry == 99 && output.generation == 88 && output.slot == 77);
    CHECK(first.create_world(&a, config(), world) == rt::Status::ok);
    CHECK(second.create_world(&b, config(), other) == rt::Status::ok);
    CHECK(first.create_world(&a, config(), output) == rt::Status::invalid_state);
    CHECK(first.create_world(&c, config(), spare) == rt::Status::ok);
    CHECK(first.create_world(&d, config(), output) == rt::Status::resource_exhausted);
    CHECK(first.close_world(other) == rt::Status::invalid_handle);
    CHECK(first.start(world) == rt::Status::invalid_state);
    CHECK(first.close_all() == rt::Status::ok && second.close_all() == rt::Status::ok);
    CHECK(first.close_world(world) == rt::Status::invalid_handle);
    CHECK(first.create_world(&a, config(), output) == rt::Status::ok);
    CHECK(output.generation != world.generation);
    CHECK(first.close_all() == rt::Status::ok);
}

void modules_and_registration() {
    data = {};
    Registry registry, foreign;
    int a = 0, b = 0, c = 0;
    rtfw_host::ModuleHandle one, two, other, output{99, 88, 77};
    CHECK(registry.add_module(nullptr, entry<0>, output) == rt::Status::invalid_argument);
    CHECK(registry.add_module(&a, nullptr, output) == rt::Status::invalid_argument);
    CHECK(registry.add_module(&a, entry<0>, one) == rt::Status::ok);
    CHECK(registry.add_module(&a, entry<1>, output) == rt::Status::invalid_state);
    CHECK(registry.add_module(&b, entry<0>, output) == rt::Status::invalid_state);
    CHECK(registry.add_module(&b, rejected_entry, two) == rt::Status::ok);
    CHECK(registry.add_module(&c, entry<1>, output) == rt::Status::resource_exhausted);
    CHECK(output.registry == 99 && output.generation == 88 && output.slot == 77);
    CHECK(foreign.add_module(&c, entry<1>, other) == rt::Status::ok);
    rtfw_host::WorldHandle world;
    CHECK(registry.create_world(&c, config(), world) == rt::Status::ok);
    rt::ExtensionHandle extension; extension.owner = 99;
    CHECK(registry.attach(world, other, extension) == rt::Status::invalid_handle);
    CHECK(registry.attach(world, two, extension) == rt::Status::callback_failed);
    CHECK(extension.owner == 99);
    std::size_t refs = 99;
    CHECK(registry.module_references(two, refs) == rt::Status::ok && refs == 0);
    CHECK(registry.attach(world, one, extension) == rt::Status::ok);
    CHECK(registry.attach(world, one, extension) == rt::Status::invalid_state);
    CHECK(registry.release_module(one) == rt::Status::invalid_state);
    CHECK(registry.close_world(world) == rt::Status::ok); // Discard unstarted configuration.
    CHECK(registry.module_references(one, refs) == rt::Status::ok && refs == 0);
    CHECK(registry.release_module(one) == rt::Status::ok);
    CHECK(registry.module_references(one, refs) == rt::Status::invalid_handle);
    CHECK(registry.release_module(two) == rt::Status::ok && foreign.release_module(other) == rt::Status::ok);
}

void binding_capacity_and_generation_retirement() {
    data = {};
    rtfw_host::Registry<1, 2, 1, 2> registry;
    int a = 0, b = 0;
    rtfw_host::ModuleHandle one, two;
    CHECK(registry.add_module(&a, entry<0>, one) == rt::Status::ok);
    CHECK(registry.add_module(&b, entry<1>, two) == rt::Status::ok);
    rtfw_host::WorldHandle previous;
    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        rtfw_host::WorldHandle world;
        CHECK(registry.create_world(&a, config(), world) == rt::Status::ok);
        if (cycle) CHECK(registry.start(previous) == rt::Status::invalid_handle);
        rt::ExtensionHandle extension;
        CHECK(registry.attach(world, one, extension) == rt::Status::ok);
        const auto entries = data[1].entries;
        CHECK(registry.attach(world, two, extension) == rt::Status::resource_exhausted);
        CHECK(data[1].entries == entries);
        CHECK(registry.close_world(world) == rt::Status::ok);
        previous = world;
    }
    rtfw_host::WorldHandle output{99, 88, 77};
    CHECK(registry.create_world(&a, config(), output) == rt::Status::resource_exhausted);
    CHECK(output.registry == 99);
    CHECK(registry.release_module(one) == rt::Status::ok && registry.release_module(two) == rt::Status::ok);
    CHECK(registry.add_module(&a, entry<0>, one) == rt::Status::ok);
    CHECK(registry.release_module(one) == rt::Status::ok);
    CHECK(registry.add_module(&b, entry<1>, two) == rt::Status::ok);
    CHECK(registry.release_module(two) == rt::Status::ok);
    CHECK(registry.add_module(&a, entry<0>, one) == rt::Status::resource_exhausted);
}

void partial_cleanup_and_retry() {
    data = {};
    Registry registry;
    int a = 0, b = 0;
    rtfw_host::ModuleHandle one, two;
    rtfw_host::WorldHandle first, second;
    CHECK(registry.add_module(&a, entry<0>, one) == rt::Status::ok);
    CHECK(registry.add_module(&b, entry<1>, two) == rt::Status::ok);
    CHECK(registry.create_world(&a, config(), first) == rt::Status::ok);
    CHECK(registry.create_world(&b, config(), second) == rt::Status::ok);
    running(registry, first, one); running(registry, second, two);
    data[0].quiesce = RTFW_STATUS_INVALID_STATE;
    CHECK(registry.close_all() == rt::Status::invalid_state);
    CHECK(registry.step(first, {}) == rt::Status::invalid_state);
    CHECK(registry.step(second, {}) == rt::Status::invalid_handle);
    CHECK(registry.release_module(one) == rt::Status::invalid_state);
    CHECK(registry.release_module(two) == rt::Status::ok);
    std::size_t refs = 0;
    CHECK(registry.module_references(one, refs) == rt::Status::ok && refs == 1);
    data[0].quiesce = RTFW_STATUS_OK;
    CHECK(registry.close_all() == rt::Status::ok);
    CHECK(registry.module_references(one, refs) == rt::Status::ok && refs == 0);
    CHECK(registry.release_module(one) == rt::Status::ok);
}

void first_error_and_shutdown_failure() {
    data = {};
    Registry registry;
    int a = 0, b = 0;
    rtfw_host::ModuleHandle one, two;
    rtfw_host::WorldHandle first, second;
    CHECK(registry.add_module(&a, entry<0>, one) == rt::Status::ok);
    CHECK(registry.add_module(&b, entry<1>, two) == rt::Status::ok);
    CHECK(registry.create_world(&a, config(), first) == rt::Status::ok);
    CHECK(registry.create_world(&b, config(), second) == rt::Status::ok);
    running(registry, first, one); running(registry, second, two);
    data[0].request = RTFW_STATUS_CALLBACK_FAILED;
    data[1].shutdown = RTFW_STATUS_INVALID_STATE;
    CHECK(registry.close_all() == rt::Status::callback_failed);
    CHECK(registry.release_module(one) == rt::Status::invalid_state);
    CHECK(registry.release_module(two) == rt::Status::invalid_state);
    data[0].request = RTFW_STATUS_OK; data[1].shutdown = RTFW_STATUS_OK;
    CHECK(registry.close_all() == rt::Status::ok);
    CHECK(registry.release_module(one) == rt::Status::ok && registry.release_module(two) == rt::Status::ok);
}

struct Graph {
    rt::CallbackResult phase(const rt::CallbackContext&) noexcept { return rt::CallbackResult::ok; }
    static rt::Status cycle(void* opaque, rt::Runtime& runtime) noexcept {
        auto& self = *static_cast<Graph*>(opaque);
        rt::sdk::GraphBuilder builder(runtime);
        rt::PhaseHandle a, b;
        auto status = builder.phase<&Graph::phase>("cycle.a", self, a);
        if (status == rt::Status::ok) status = builder.phase<&Graph::phase>("cycle.b", self, b);
        if (status == rt::Status::ok) status = builder.depends_on(a, b);
        if (status == rt::Status::ok) status = builder.depends_on(b, a);
        return status;
    }
};
void configuration_and_run_failure() {
    data = {};
    Registry registry;
    int owner = 0;
    rtfw_host::ModuleHandle module;
    CHECK(registry.add_module(&owner, entry<0>, module) == rt::Status::ok);
    rtfw_host::WorldHandle world;
    CHECK(registry.create_world(&owner, config(), world) == rt::Status::ok);
    CHECK(registry.configure_world(world, nullptr, nullptr) == rt::Status::invalid_argument);
    rt::ExtensionHandle extension;
    CHECK(registry.attach(world, module, extension) == rt::Status::ok);
    Graph graph;
    CHECK(registry.configure_world(world, Graph::cycle, &graph) == rt::Status::ok);
    CHECK(registry.finalize(world) == rt::Status::graph_cycle);
    CHECK(registry.close_world(world) == rt::Status::ok);
    CHECK(registry.create_world(&owner, config(), world) == rt::Status::ok);
    CHECK(registry.attach(world, module, extension) == rt::Status::ok);
    CHECK(registry.finalize(world) == rt::Status::ok);
    data[0].initialize = RTFW_STATUS_CALLBACK_FAILED;
    CHECK(registry.start(world) == rt::Status::callback_failed);
    CHECK(registry.close_world(world) == rt::Status::ok);
    data[0].initialize = RTFW_STATUS_OK;
    CHECK(registry.create_world(&owner, config(), world) == rt::Status::ok);
    running(registry, world, module);
    data[0].fail_frame = true;
    CHECK(registry.step(world, {0, std::chrono::milliseconds(1)}) == rt::Status::callback_failed);
    CHECK(registry.close_world(world) == rt::Status::ok);
    CHECK(registry.release_module(module) == rt::Status::ok);
}

void reentry_and_allocation() {
    data = {};
    Registry registry;
    reentrant = &registry;
    int owner = 0;
    CHECK(registry.add_module(&owner, entry<0>, reentrant_module) == rt::Status::ok);
    rtfw_host::WorldHandle world;
    CHECK(registry.create_world(&owner, config(), world) == rt::Status::ok);
    data[0].reenter = reenter;
    running(registry, world, reentrant_module);
    CHECK(data[0].reentry == rt::Status::invalid_state);
    using namespace rtfw_physics_allocation;
    begin(); auto* control = ::operator new(17); ::operator delete(control);
    tracking = false;
    CHECK(count.load() != 0);
    begin();
    const auto step = registry.step(world, {0, std::chrono::milliseconds(1)});
    const auto close = registry.close_world(world);
    const auto release = registry.release_module(reentrant_module);
    tracking = false;
    CHECK(step == rt::Status::ok && close == rt::Status::ok && release == rt::Status::ok);
    CHECK(count.load() == 0 && data[0].reentry == rt::Status::invalid_state);
    data[0].reenter = nullptr; reentrant = nullptr;
}

void time_boundaries() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t value = 77;
    CHECK(rtfw_host::ticks_to_ns(1, 0, value) == rt::Status::invalid_argument && value == 77);
    CHECK(rtfw_host::ticks_to_ns(1, maximum / 1000000000 + 1, value)
          == rt::Status::invalid_argument && value == 77);
    CHECK(rtfw_host::ticks_to_ns(maximum, 1, value) == rt::Status::invalid_argument && value == 77);
    CHECK(rtfw_host::ticks_to_ns(2, 3, value) == rt::Status::ok && value == 666666666);
    CHECK(rtfw_host::ticks_to_ns(3, 3000000000, value) == rt::Status::ok && value == 1);
    CHECK(rtfw_host::ticks_to_ns(maximum, 1000000000, value) == rt::Status::ok && value == maximum);
    rt::HostFrameContext frame{77, std::chrono::nanoseconds(88), 99};
    CHECK(rtfw_host::frame_from_ticks(1, maximum, 1, std::nullopt, frame) == rt::Status::invalid_argument);
    CHECK(frame.frame_index == 77 && frame.delta.count() == 88 && frame.deadline_ns == 99);
    CHECK(rtfw_host::frame_from_ticks(1, 1, 1, maximum, frame) == rt::Status::invalid_argument);
    CHECK(frame.frame_index == 77 && frame.delta.count() == 88 && frame.deadline_ns == 99);
    const auto signed_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    CHECK(rtfw_host::frame_from_ticks(1, signed_max + 1, 1000000000, std::nullopt, frame)
          == rt::Status::invalid_argument);
    CHECK(rtfw_host::frame_from_ticks(1, signed_max, 1000000000, std::nullopt, frame) == rt::Status::ok);
    CHECK(frame.delta.count() == std::numeric_limits<std::int64_t>::max() && !frame.deadline_ns);
    CHECK(rtfw_host::frame_from_ticks(2, 1, 1000, 1500, frame) == rt::Status::ok);
    CHECK(frame.frame_index == 2 && frame.delta.count() == 1000000 && frame.deadline_ns == 1500000000);
    CHECK(rtfw_host::frame_from_ticks(3, 1, 1, std::nullopt, frame, maximum) == rt::Status::invalid_argument);
    CHECK(frame.frame_index == 2 && !frame.nominal_release_ns);
    CHECK(rtfw_host::frame_from_ticks(3, 1, 1000, 1500, frame, 1200) == rt::Status::ok);
    CHECK(frame.nominal_release_ns == 1200000000 && frame.deadline_ns == 1500000000);
}
}

int main() {
    identity_and_capacity(); modules_and_registration(); binding_capacity_and_generation_retirement();
    partial_cleanup_and_retry(); first_error_and_shutdown_failure(); configuration_and_run_failure();
    reentry_and_allocation(); time_boundaries();
    std::printf("host_lifecycle: contract_groups=8 checks=%u allocation_positive_control=pass\n", checks);
}
