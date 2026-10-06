#include <rt/extension_abi.h>
#include <string.h>

#if defined(_WIN32)
#define DEMO_EXPORT __declspec(dllexport)
#else
#define DEMO_EXPORT __attribute__((visibility("default")))
#endif

/* Trusted demonstration module. Two independent worlds; no device or engine.
 * Entry registration is serialized by the host. No module-owned worker runs.
 * The first service deliberately retains ownership until the host releases it.
 */
struct demo_world { int claimed, initialized, hold; uint64_t frames; };
static struct demo_world worlds[2];

static rtfw_callback_result frame(void* data, const rtfw_callback_context* context) {
    struct demo_world* world = (struct demo_world*)data;
    if (!world->initialized || !context) return RTFW_CALLBACK_ERROR;
    ++world->frames;
    return RTFW_CALLBACK_OK;
}
static rtfw_status RTFW_EXTENSION_CALL initialize(void* data) {
    struct demo_world* world = (struct demo_world*)data;
    world->initialized = 1;
    return RTFW_STATUS_OK;
}
static rtfw_status RTFW_EXTENSION_CALL request_stop(void* data) {
    (void)data;
    return RTFW_STATUS_OK;
}
static rtfw_status RTFW_EXTENSION_CALL quiesce(void* data) {
    return ((struct demo_world*)data)->hold ? RTFW_STATUS_INVALID_STATE : RTFW_STATUS_OK;
}
static rtfw_status RTFW_EXTENSION_CALL shutdown(void* data) {
    struct demo_world* world = (struct demo_world*)data;
    world->initialized = 0;
    world->claimed = 0;
    return RTFW_STATUS_OK;
}

DEMO_EXPORT rtfw_status RTFW_EXTENSION_CALL rtfw_extension_entry_v1(
    const rtfw_extension_host_api_v1* host, rtfw_extension_descriptor_v1* descriptor) {
    unsigned index;
    rtfw_extension_phase_v1 phase;
    rtfw_extension_service_v1 service;
    rtfw_extension_handle_v1 handle;
    rtfw_status status;
    if (!host || !descriptor) return RTFW_STATUS_INVALID_ARGUMENT;
    for (index = 0; index < 2; ++index) if (!worlds[index].claimed) break;
    if (index == 2) return RTFW_STATUS_RESOURCE_EXHAUSTED;
    memset(&phase, 0, sizeof(phase));
    phase.struct_size = (uint32_t)sizeof(phase);
    phase.abi_version = RTFW_EXTENSION_ABI_VERSION;
    strcpy(phase.name, "demo.integrate");
    phase.callback = &frame;
    phase.user_data = &worlds[index];
    status = host->stage_phase(host->context, &phase, &handle);
    if (status != RTFW_STATUS_OK) return status;
    memset(&service, 0, sizeof(service));
    service.struct_size = (uint32_t)sizeof(service);
    service.abi_version = RTFW_EXTENSION_ABI_VERSION;
    strcpy(service.name, "demo.owner");
    strcpy(service.interface_name, "demo.world");
    service.interface_version = 1;
    service.api.struct_size = (uint32_t)sizeof(service.api);
    service.api.abi_version = RTFW_EXTENSION_ABI_VERSION;
    service.api.instance = &worlds[index];
    service.api.initialize = &initialize;
    service.api.request_stop = &request_stop;
    service.api.quiesce = &quiesce;
    service.api.shutdown = &shutdown;
    status = host->stage_service(host->context, &service, &handle);
    if (status != RTFW_STATUS_OK) return status;
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->struct_size = (uint32_t)sizeof(*descriptor);
    descriptor->current_abi_version = RTFW_EXTENSION_ABI_VERSION;
    descriptor->min_compatible_abi_version = RTFW_EXTENSION_ABI_VERSION;
    strcpy(descriptor->name, "demo.module");
    strcpy(descriptor->version, "1.0");
    descriptor->phase_count = 1;
    descriptor->service_count = 1;
    worlds[index].claimed = 1;
    worlds[index].frames = 0;
    worlds[index].hold = index == 0;
    return RTFW_STATUS_OK;
}

DEMO_EXPORT void RTFW_EXTENSION_CALL rtfw_host_demo_release(void) { worlds[0].hold = 0; }
DEMO_EXPORT uint64_t RTFW_EXTENSION_CALL rtfw_host_demo_frames(unsigned index) {
    return index < 2 ? worlds[index].frames : 0;
}
