# Extension registration ABI v1

M19-01 adds a supported C11 registration boundary for trusted in-process
extensions. It is independently versioned from stable C ABI v8 and device ABI
v1. The Runtime receives an already-resolved `rtfw_extension_entry_v1`
function pointer; discovery, provenance checks, module ownership, and eventual
operating-system unload belong to the host. Runtime does not accept a path or
module handle and does not call a platform loader.

## Version and layout contract

The host range is current 1, minimum compatible 1. An extension descriptor
declares its current and minimum compatible versions. Registration selects the
highest common version, which is version 1 for this implementation. A valid
larger record is copied only through its known version-1 prefix; suffix bytes
are ignored. A short prefix, zero or reversed range, or nonoverlapping range
fails before publication.

Every input record must set `struct_size`, its version field, all required
callbacks and instances, a NUL-terminated `[A-Za-z0-9._:/@-]+` identifier with
a zero tail, valid fixed-width enum/boolean fields, and zero known reserved
fields. Counts must exactly match successfully staged records. The following
LP64 and LLP64 layout is normative:

| Record | Size | Align | Significant offsets |
| --- | ---: | ---: | --- |
| `rtfw_extension_handle_v1` | 16 | 4 | owner 0, kind 4, slot 8, generation 12 |
| `rtfw_extension_phase_v1` | 120 | 8 | name 8, callback 72, user data 80, reserved 88 |
| `rtfw_extension_backend_v1` | 264 | 8 | name 8, device-v1 API 72, reserved 232 |
| `rtfw_extension_service_status_v1` | 64 | 8 | healthy 8, status 16, value 24, reserved 32 |
| `rtfw_extension_service_api_v1` | 88 | 8 | instance 8, callbacks 16–48, reserved 56 |
| `rtfw_extension_service_v1` | 264 | 8 | name 8, interface 72, version 136, API 144, reserved 232 |
| `rtfw_extension_resource_v1` | 104 | 8 | name 8, reserved 72 |
| `rtfw_extension_relationship_v1` | 80 | 8 | kind 8, access 12, handles 16/32, reserved 48 |
| `rtfw_extension_host_api_v1` | 136 | 8 | context 16, capacities 24–56, callbacks 64–96, reserved 104 |
| `rtfw_extension_descriptor_v1` | 216 | 8 | name 16, version 80, counts 144–176, reserved 184 |

Extension entry, staging, and service callbacks use `__cdecl` with MSVC and
the platform C calling convention elsewhere. Fixed-width fields and the table
above are the ABI contract; no C++ object, STL layout, compiler exception,
borrowed string, or persisted process pointer crosses or enters identity.

## Transaction and handles

`Runtime::register_extension(entry, handle)` is configuring-only. One call may
stage at most 64 CPU phases, 16 device-v1 backends, 16 services, 64 logical
resources, and 256 local relationships. A Runtime retains at most 16 extension
records. Host-reported phase and backend capacity is additionally clamped by
the Runtime configuration and already registered objects.

Staging returns provisional owner/kind/slot/generation handles. Relationships
may refer only to handles from the same current transaction and with the
required kind. Every attempt consumes a generation, including failure, so a
provisional handle retained from a rejected attempt cannot refer to a later
attempt. The complete graph, resource hazards, names, capacities, device
capabilities, counts, versions, arithmetic, and records are validated in
temporary storage. Publication swaps the fully prepared storage without a
throwing operation. Failure leaves no extension, phase, backend, resource,
relationship, identity contribution, or changed control-storage capacity.

Committed extension handles contain the Runtime owner, extension kind, slot,
and generation. Zero, foreign-owner, wrong-kind, never-issued, failed-attempt,
retired-generation, and detached handles return `invalid_handle` without
calling extension code or changing caller output.

## Copied and borrowed ownership

| Data | Runtime action | Lifetime |
| --- | --- | --- |
| Names, versions, interface IDs, counts, table prefixes, capabilities, relationships | copied | Runtime record |
| Entry function | invoked, not retained | registration call only |
| Phase callback and user data | borrowed | through checked detach |
| Device-v1 table and instance | copied table, borrowed callable/instance | through checked detach |
| Service table and instance | copied table, borrowed callable/instance | through checked detach |
| Module handle and path | never accepted | host-owned |

CPU phases use the exact stable C-ABI-v8 callback context, result, scratch, and
nested-task semantics and enter the ordinary graph. Device records contain
only the complete device-ABI-v1 table and traverse
`DeviceV1CompatibilityAdapter`; they expose no native HAL-v2 memory/topology,
command/timeline, CUDA Graph, XDMA control/event, peer, or combined-path
capability. This cannot repair or bypass the M17-05 discovery blocker.

## Services and lifecycle

A service has a copied name, interface name/version, required nonthrowing
initialize/request-stop/quiesce/shutdown hooks, and an optional status hook.
All run synchronously on the host's single serialized control path. No service
is an executor phase, creates a Runtime thread, or may reenter Runtime control.

The extension state sequence is:

`configuring -> registered -> running -> stop-requested -> quiescent -> detached`

Failures report `failed` or `cleanup-pending`. Startup initializes services in
registration order before watchdog, executor, device service, submission
lanes, backend initialization, or phase admission. A failed startup closes
admission, requests stop, cleans independent owners, and remains retryable.

Checked stop performs this order:

1. close phase, backend, and status admission and request each service stop;
2. reject or wait for a later caller retry if frame/replay execution is active;
3. stop device backends and submission/completion service, then executor and
   watchdog lanes;
4. retain any service related to an unresolved backend;
5. quiesce and shut down services in reverse registration order;
6. roll back resident memory and enter Runtime `stopped` only after every
   owner is released.

Independent cleanup continues after an error, while the first error is
returned. Successful request-stop and cleanup steps are idempotent and are not
repeated; uncertain owners remain exact and retryable. Destruction performs
the same best-effort order and terminates rather than relabel uncertain
ownership as released.

`detach_extension` succeeds only after checked stop and complete quiescence.
It clears every borrowed callable and instance pointer, increments the record
generation, marks the inspector row detached, and then reports unload
readiness. It performs no unload and supports no hot reload. M19-03 owns Unreal
world/module orchestration and long-run host unload coverage.

## Allocation, identity, and trust

Extension descriptors, owners, maps, and lifecycle bookkeeping use fixed
record capacities. Their heap storage is created only during configuring and
is counted exactly once in the existing `runtime_control_bytes`; canonical
device-manager storage remains in `device_control_bytes`. No MemoryPlan row or
provider region is added. Start, phase execution, status, stop/retry,
inspection, detach, and stale-handle rejection allocate no ordinary heap
memory and create no extension lane.

Extension name/version, negotiated ABI, copied phase/backend/resource names
and capabilities, service names/interfaces/versions, and local relationships
participate in configuration, graph, and replay identity. Function and data
pointers, module handles, provisional handles, owner/generation values,
lifecycle state, and status counters do not. Each Runtime owns independent
records and handle owners.

Extensions are trusted native code in the Runtime process. Version and bounds
checks reduce integration mistakes; they do not sandbox, authenticate, or
establish provenance. A fixture, shared-library build, successful readiness
check, mock backend, hosted CI, or preflight result is not Unreal, arbitrary
hot-reload, physical hardware, HIL, field, controlled-latency, RT1/RT2,
signing, release, deployment, or production evidence.

ABI v1 evolves only by appending a size-guarded suffix and preserving this
prefix. An incompatible change requires a new ABI version and entry symbol.
Rollback removes the header and Runtime surface as one unit only after every
host integration has stopped and detached its extensions; version 1 must never
be silently reused for an incompatible layout.
