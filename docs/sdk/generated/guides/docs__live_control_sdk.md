[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)

> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.

# Fixed typed live-control SDK

`<rt/live_control.hpp>` is an optional header-only C++20 source API above the
raw `<rt/runtime.hpp>` live-control surface. It adds no compiled symbol or C++
binary ABI. Applications that do not include it retain the exact M22-01 through
M22-03 API, installation targets, Runtime behavior, and artifact formats.

## Canonical payload format

Every typed payload has one 32-byte little-endian envelope followed by one
positive fixed-size application body:

| Offset | Bytes | Field | Rule |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | `0x434c5452`, bytes `52 54 4c 43` (`RTLC`) |
| 4 | 4 | envelope version | `1` |
| 8 | 4 | total encoded bytes | exact envelope plus body extent |
| 12 | 4 | application type identity | positive and application-owned |
| 16 | 4 | application schema version | positive and application-owned |
| 20 | 4 | update kind | one closed `LiveControlUpdateKind` value |
| 24 | 4 | reserved | zero |
| 28 | 4 | reserved | zero |
| 32 | fixed | application body | canonical field encoding from the codec |

The complete payload cannot exceed the retained 65,536-byte per-record limit.
This envelope is application content carried by the existing raw record. It
does not version or extend checkpoints, input logs, live-control actions,
live-control replay, mixed-rate actions, active replay, observability, Runtime
profiles, C ABI v8, device ABI v1, extension ABI v1, or HAL tables.

Never serialize `sizeof(T)` bytes, a native object representation, padding,
addresses, pointer values, `typeid`, host-endian integers, or implementation-
defined enum storage. Encode every field explicitly with canonical helpers such
as `store_u32_le` and `store_u64_le`; decode with their matching loads.

## Codec contract

Define an application type, then specialize `rt::LiveControlTypeTraits<T>`:

```cpp
struct Gains {
    std::uint32_t proportional_milli;
    std::uint32_t integral_milli;
};

template <>
struct rt::LiveControlTypeTraits<Gains> {
    static constexpr std::uint32_t application_type_identity = 0x4741494e;
    static constexpr std::uint32_t application_schema_version = 1;
    static constexpr rt::LiveControlUpdateKind update_kind =
        rt::LiveControlUpdateKind::controller_parameters;
    static constexpr std::size_t encoded_extent = 8;

    static bool validate(const Gains& value) noexcept {
        return value.proportional_milli <= 100'000 &&
            value.integral_milli <= 100'000;
    }
    static bool encode(
        const Gains& value,
        std::span<std::byte, encoded_extent> output) noexcept {
        return rt::store_u32_le(output, 0, value.proportional_milli) &&
            rt::store_u32_le(output, 4, value.integral_milli);
    }
    static bool decode(
        std::span<const std::byte, encoded_extent> input,
        Gains& output) noexcept {
        Gains candidate;
        if (!rt::load_u32_le(input, 0, candidate.proportional_milli) ||
            !rt::load_u32_le(input, 4, candidate.integral_milli)) {
            return false;
        }
        output = candidate;
        return true;
    }
};
```

`T` must be standard-layout, trivially copyable, nothrow default-constructible,
and nothrow copy-assignable. The identities, schema, extent, and kind are
compile-time constants; the extent and identities are positive; and all three
codec operations are `noexcept` fixed-span functions. These structural checks
do not recursively inspect application members. The application remains
responsible for excluding raw pointers, borrowed views, variable-size
containers, dynamic strings, paths, callbacks, vendor handles, implicit clocks,
and any other unstable or unbounded representation.

Validation must be finite and allocation-free. If a source format requires
text parsing, authentication, file/network access, schema negotiation, dynamic
allocation, or logging, perform it on an application control thread and
materialize a validated fixed type before calling the SDK.

## Construction and staging

`LiveControlTypedPayload<T>` is caller-owned fixed storage. Use
`make_live_control_host_update` or `make_live_control_rate_update` to encode one
value and construct one existing `LiveControlUpdateRecord`. A successful
builder copies the exact Runtime/configuration/mailbox/producer identities from
the handle, preserves the caller's positive producer sequence, writes the exact
target, kind, extent, alignment, canonical flag, and digest, and leaves mailbox
sequence zero for Runtime assignment.

Builders do not call Runtime. They do not reserve a slot or advance a producer
sequence. They never retry `busy` or `full`, choose a later target, change an
admission result, retain caller storage, or perform a destructor action. The
explicit raw call remains mandatory:

```cpp
rt::LiveControlTypedPayload<Gains> payload{};
rt::LiveControlUpdateRecord record;
auto typed = rt::make_live_control_host_update(
    handle, producer_sequence, frame_index, gains, payload, record);
if (typed != rt::LiveControlTypedStatus::ok) {
    // Correct the application value/target; nothing was published.
}

rt::LiveControlAdmissionResult admission;
rt::Status status = runtime.stage_live_control_update(
    handle, record, payload, admission);
// Handle accepted/invalid/full/busy/stale/stopped/exhausted/missed exactly.
```

A rate builder accepts a complete `LiveControlBoundaryTarget` and copies its
exact compiled reference-release, domain-registration, phase, release-sequence,
and substep identities. It does not discover or synthesize a target.

## Callback decode

`decode_live_control_typed_payload` first validates the raw record schema,
record size, update kind, exact extent, canonical flag, and payload digest. It
then validates every envelope field and calls the fixed body decoder into a
temporary value. Caller output changes only after decode and semantic
validation succeed.

This operation is fixed-work for `T`, allocation-free, and `noexcept`; it is
suitable for bounded callback copy when the application codec has the same
properties. It does not authorize variable-length parsing, authentication,
filesystem/network access, logging, vendor operations, backend transfers, or
mutation before complete validation. A callback error can roll back the
Runtime-owned immutable generation under M22-03; it cannot reverse arbitrary
application state already mutated by the callback or any backend, process, or
physical side effect. This rollback changes only the Runtime generation.

## Clear fault and raw compatibility

The original empty raw `clear_fault` record remains supported. M22-04 also
allows a nonempty fixed canonical typed clear-fault payload for applications
that require explicit fault identity or authorization fields. Both forms use
the unchanged numeric kind. Existing raw host/rate builders, manual records,
admission results, action inspection, checkpoints, and replay remain directly
usable beside the typed helper.

## Evidence boundary

The shipped sample defines fixed scenario parameters, controller gains, sensor
calibration, and fault configuration. It demonstrates exact host and compiled
rate targets, raw clear-fault compatibility, whole-value callback observation,
replacement, commit, callback rollback, action inspection, and checked stop.
The stress suite covers absolute capacities, maximum payload, concurrency,
loss, lifecycle, watchdog interaction, and multiple Runtime isolation with
finite operation counts.

This is portable RT0 data/configuration evidence. It is not executable or
shared-library hot reload, Unreal integration, arbitrary side-effect rollback,
physical control, vendor I/O, HIL, controlled latency/electrical/calibration
evidence, RT1, RT2, signing, publication, release, deployment, or production
readiness.
