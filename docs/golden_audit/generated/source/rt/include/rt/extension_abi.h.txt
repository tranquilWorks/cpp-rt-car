#pragma once

#include <stddef.h>
#include <stdint.h>

#include <rt/c_api.h>
#include <rt/device_abi.h>

#if defined(_MSC_VER)
#define RTFW_EXTENSION_CALL __cdecl
#else
#define RTFW_EXTENSION_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RTFW_EXTENSION_ABI_VERSION 1u
#define RTFW_EXTENSION_ABI_MIN_COMPATIBLE_VERSION 1u
#define RTFW_EXTENSION_IDENTIFIER_CAPACITY 64u
#define RTFW_EXTENSION_ENTRY_SYMBOL_V1 "rtfw_extension_entry_v1"

#define RTFW_EXTENSION_PHASE_CAPACITY 64u
#define RTFW_EXTENSION_BACKEND_CAPACITY 16u
#define RTFW_EXTENSION_SERVICE_CAPACITY 16u
#define RTFW_EXTENSION_RESOURCE_CAPACITY 64u
#define RTFW_EXTENSION_RELATIONSHIP_CAPACITY 256u
#define RTFW_RUNTIME_EXTENSION_CAPACITY 16u

typedef uint32_t rtfw_extension_handle_kind;
#define RTFW_EXTENSION_HANDLE_EXTENSION ((rtfw_extension_handle_kind)1u)
#define RTFW_EXTENSION_HANDLE_PHASE ((rtfw_extension_handle_kind)2u)
#define RTFW_EXTENSION_HANDLE_BACKEND ((rtfw_extension_handle_kind)3u)
#define RTFW_EXTENSION_HANDLE_SERVICE ((rtfw_extension_handle_kind)4u)
#define RTFW_EXTENSION_HANDLE_RESOURCE ((rtfw_extension_handle_kind)5u)

typedef struct rtfw_extension_handle_v1 {
    uint32_t owner;
    uint32_t kind;
    uint32_t slot;
    uint32_t generation;
} rtfw_extension_handle_v1;

typedef uint32_t rtfw_extension_relationship_kind;
#define RTFW_EXTENSION_RELATIONSHIP_PHASE_DEPENDENCY \
    ((rtfw_extension_relationship_kind)1u)
#define RTFW_EXTENSION_RELATIONSHIP_PHASE_RESOURCE \
    ((rtfw_extension_relationship_kind)2u)
#define RTFW_EXTENSION_RELATIONSHIP_SERVICE_BACKEND \
    ((rtfw_extension_relationship_kind)3u)

typedef uint32_t rtfw_extension_resource_access;
#define RTFW_EXTENSION_RESOURCE_ACCESS_NONE \
    ((rtfw_extension_resource_access)0u)
#define RTFW_EXTENSION_RESOURCE_ACCESS_READ \
    ((rtfw_extension_resource_access)1u)
#define RTFW_EXTENSION_RESOURCE_ACCESS_WRITE \
    ((rtfw_extension_resource_access)2u)

typedef struct rtfw_extension_phase_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    char name[RTFW_EXTENSION_IDENTIFIER_CAPACITY];
    rtfw_frame_callback callback;
    void* user_data;
    uint64_t reserved[4];
} rtfw_extension_phase_v1;

typedef struct rtfw_extension_backend_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    char name[RTFW_EXTENSION_IDENTIFIER_CAPACITY];
    rtfw_device_backend_api api;
    uint64_t reserved[4];
} rtfw_extension_backend_v1;

typedef rtfw_status (RTFW_EXTENSION_CALL *rtfw_extension_service_initialize_fn)(
    void* instance);
typedef rtfw_status (RTFW_EXTENSION_CALL *rtfw_extension_service_request_stop_fn)(
    void* instance);
typedef rtfw_status (RTFW_EXTENSION_CALL *rtfw_extension_service_quiesce_fn)(
    void* instance);
typedef rtfw_status (RTFW_EXTENSION_CALL *rtfw_extension_service_shutdown_fn)(
    void* instance);

typedef struct rtfw_extension_service_status_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t healthy;
    uint32_t reserved0;
    int32_t status;
    uint32_t reserved1;
    uint64_t observed_value;
    uint64_t reserved[4];
} rtfw_extension_service_status_v1;

typedef rtfw_status (RTFW_EXTENSION_CALL *rtfw_extension_service_status_fn)(
    void* instance,
    rtfw_extension_service_status_v1* status);

typedef struct rtfw_extension_service_api_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    void* instance;
    rtfw_extension_service_initialize_fn initialize;
    rtfw_extension_service_request_stop_fn request_stop;
    rtfw_extension_service_quiesce_fn quiesce;
    rtfw_extension_service_shutdown_fn shutdown;
    rtfw_extension_service_status_fn status;
    uint64_t reserved[4];
} rtfw_extension_service_api_v1;

typedef struct rtfw_extension_service_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    char name[RTFW_EXTENSION_IDENTIFIER_CAPACITY];
    char interface_name[RTFW_EXTENSION_IDENTIFIER_CAPACITY];
    uint32_t interface_version;
    uint32_t reserved0;
    rtfw_extension_service_api_v1 api;
    uint64_t reserved[4];
} rtfw_extension_service_v1;

typedef struct rtfw_extension_resource_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    char name[RTFW_EXTENSION_IDENTIFIER_CAPACITY];
    uint64_t reserved[4];
} rtfw_extension_resource_v1;

typedef struct rtfw_extension_relationship_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t kind;
    uint32_t access;
    rtfw_extension_handle_v1 first;
    rtfw_extension_handle_v1 second;
    uint64_t reserved[4];
} rtfw_extension_relationship_v1;

struct rtfw_extension_host_api_v1;

typedef rtfw_status (RTFW_EXTENSION_CALL *rtfw_extension_stage_phase_fn)(
    void* context,
    const rtfw_extension_phase_v1* phase,
    rtfw_extension_handle_v1* out_phase);
typedef rtfw_status (RTFW_EXTENSION_CALL *rtfw_extension_stage_backend_fn)(
    void* context,
    const rtfw_extension_backend_v1* backend,
    rtfw_extension_handle_v1* out_backend);
typedef rtfw_status (RTFW_EXTENSION_CALL *rtfw_extension_stage_service_fn)(
    void* context,
    const rtfw_extension_service_v1* service,
    rtfw_extension_handle_v1* out_service);
typedef rtfw_status (RTFW_EXTENSION_CALL *rtfw_extension_stage_resource_fn)(
    void* context,
    const rtfw_extension_resource_v1* resource,
    rtfw_extension_handle_v1* out_resource);
typedef rtfw_status (RTFW_EXTENSION_CALL *rtfw_extension_stage_relationship_fn)(
    void* context,
    const rtfw_extension_relationship_v1* relationship);

typedef struct rtfw_extension_host_api_v1 {
    uint32_t struct_size;
    uint32_t current_abi_version;
    uint32_t min_compatible_abi_version;
    uint32_t reserved0;
    void* context;
    uint64_t phase_capacity;
    uint64_t backend_capacity;
    uint64_t service_capacity;
    uint64_t resource_capacity;
    uint64_t relationship_capacity;
    rtfw_extension_stage_phase_fn stage_phase;
    rtfw_extension_stage_backend_fn stage_backend;
    rtfw_extension_stage_service_fn stage_service;
    rtfw_extension_stage_resource_fn stage_resource;
    rtfw_extension_stage_relationship_fn stage_relationship;
    uint64_t reserved[4];
} rtfw_extension_host_api_v1;

typedef struct rtfw_extension_descriptor_v1 {
    uint32_t struct_size;
    uint32_t current_abi_version;
    uint32_t min_compatible_abi_version;
    uint32_t reserved0;
    char name[RTFW_EXTENSION_IDENTIFIER_CAPACITY];
    char version[RTFW_EXTENSION_IDENTIFIER_CAPACITY];
    uint64_t phase_count;
    uint64_t backend_count;
    uint64_t service_count;
    uint64_t resource_count;
    uint64_t relationship_count;
    uint64_t reserved[4];
} rtfw_extension_descriptor_v1;

typedef rtfw_status (RTFW_EXTENSION_CALL *rtfw_extension_entry_fn_v1)(
    const rtfw_extension_host_api_v1* host,
    rtfw_extension_descriptor_v1* descriptor);

#define RTFW_EXTENSION_PHASE_V1_REQUIRED_SIZE \
    ((uint32_t)sizeof(rtfw_extension_phase_v1))
#define RTFW_EXTENSION_BACKEND_V1_REQUIRED_SIZE \
    ((uint32_t)sizeof(rtfw_extension_backend_v1))
#define RTFW_EXTENSION_SERVICE_API_V1_REQUIRED_SIZE \
    ((uint32_t)sizeof(rtfw_extension_service_api_v1))
#define RTFW_EXTENSION_SERVICE_V1_REQUIRED_SIZE \
    ((uint32_t)sizeof(rtfw_extension_service_v1))
#define RTFW_EXTENSION_SERVICE_STATUS_V1_REQUIRED_SIZE \
    ((uint32_t)sizeof(rtfw_extension_service_status_v1))
#define RTFW_EXTENSION_RESOURCE_V1_REQUIRED_SIZE \
    ((uint32_t)sizeof(rtfw_extension_resource_v1))
#define RTFW_EXTENSION_RELATIONSHIP_V1_REQUIRED_SIZE \
    ((uint32_t)sizeof(rtfw_extension_relationship_v1))
#define RTFW_EXTENSION_HOST_API_V1_REQUIRED_SIZE \
    ((uint32_t)sizeof(rtfw_extension_host_api_v1))
#define RTFW_EXTENSION_DESCRIPTOR_V1_REQUIRED_SIZE \
    ((uint32_t)sizeof(rtfw_extension_descriptor_v1))

#ifdef __cplusplus
}
#endif
