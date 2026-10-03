# Sampled I/O and portable loopback

M21-04 adds a portable RT0 sampled-I/O contract on top of the admitted M21-03
CPU/device cross-rate payload path. M21-05 records its publication, selection,
freshness, substitution, and acknowledged-safety outcomes in a separate closed
action schema and can replay them only through deterministic mock/loopback
backends. These are additive C++ APIs; stable ABIs and prior schemas do not
change.

## Channel contract

`Runtime::register_sampled_io_channel()` copies a fixed descriptor while the
Runtime is configuring. The descriptor names one existing cross-rate channel
with exactly one device endpoint and freezes its direction, fixed-width
little-endian encoding, element and frame geometry, rational scale and offset,
units and calibration identities, sample interval, timestamp and clock
domains, trigger mode and identity, ring capacity, initial sequence, maximum
age, stale/overrun/underrun policies, and exact copied initial/safe frames.

Finalization rejects foreign or duplicate identities, checked-arithmetic
overflow, malformed frames or checksums, mismatched cross-rate payloads,
incoherent/inaccessible device envelopes, wrong endpoint direction or
timestamp domain, insufficient execution/ring capacity, and invalid output
safety declarations. Failure publishes no partial plan and may be corrected
with the existing replacement operations.

Each frame begins with `SampledIoFrameHeader`. The header is copied with
`memcpy`; consumers never depend on alignment or type-punning. Produced frames
must match the exact nonzero sequence, release generation, trigger sequence,
sample count, interval, timestamp domain, channel/calibration/trigger
identities, status, extent, and payload checksum before publication.

## Runtime behavior

- CPU output publication validates the complete produced frame before it can
  enter the existing fixed snapshot store.
- Device input publication occurs only after the exact M21-02 ticket reaches
  correlated terminal success. The frame timestamp must match completion.
- Before provider submission, Runtime pre-materializes the exact expected
  sequence, release generation, and trigger sequence in each selected sampled
  device-output slot. This is correlation metadata only: the backend must
  still produce a complete frame that passes terminal validation.
- Stale input follows only `fail_release`, bounded `hold_last`, or copied
  `substitute_initial`. Output underrun either fails or copies the declared
  failure-safe frame. Duplicate publication records an overrun and fails.
- Fixed-size inspectors expose last sequence, accepted/stale/overrun/
  underrun/substitution counts, last status, and output safety state without
  changing versioned observability or rate-action schemas.

Startup copies and submits every declared startup-safe output through the
existing backend submission lane and requires terminal acknowledgement before
`start()` succeeds. Checked stop performs the failure-safe or shutdown-safe
transition before backend teardown. Submission, timeout, loss, cancellation,
malformed acknowledgement, or cleanup failure leaves the safety state
`unknown` and returns non-success; enqueue alone is never called safe.

## Public loopback backend

`SampledIoLoopbackBackend` is an installed, fixed-capacity HAL-v2 backend for
portable integration tests and examples. It exposes one host-coherent memory
domain, one monotonic timestamp domain, command timelines, bounded buffer and
completion slots, deterministic opcode routes, cancellation/reset/stop, and a
single atomic next-submission fault injection. A route copies a complete frame
from dispatch buffer reference 0 to reference 1 and rewrites only its frozen
destination channel, timestamp, calibration, trigger, status, and logical
timestamp fields. When Runtime supplied a valid destination correlation
template, loopback preserves its sequence, release generation, and trigger
sequence rather than retaining a differently rated source channel's values.
It owns no thread and allocates nothing after construction.

M21-05 adds a fixed instance-local logical-action log to the loopback. Accepted
and rejected submissions and terminal outcomes retain deterministic identities;
inspection is bounded, and reset is allowed only after shutdown. No mutable
global state, hidden thread, process observation, or sleep timing participates.

This is fake software loopback evidence. It does not establish DAC, DAQ, CAN,
IIO, XDMA, CUDA, electrical scaling, physical trigger/sample-clock accuracy,
HIL, controlled latency, RT1, or RT2 behavior.

## Completion ownership and direct-caller lifecycle

Submission privately owns a free slot until its completion and accounting are
published. A normal completion becomes ready; an injected timeout becomes held
and remains invisible to polling until cancellation. Poll and cancel each claim
exclusive ownership before reading or writing completion storage. Cancellation
checks the batch ID after claiming, preventing stale-ID access during slot reuse;
a nonmatching slot is restored to its original published state. Repeated cancel
before polling is idempotent and increments cancellation accounting once.

All scans stay bounded by configured capacity and use no blocking lock or heap
allocation. A cancel scan may return `invalid_argument` while the matching submit
or competing consumer has exclusive ownership. `request_stop` prevents later
admission, while an already admitted callback may finish. Direct users must
quiesce callbacks before buffer mutation, reset, shutdown, initialization, move
or destruction, matching Runtime's checked-stop ordering. This is not a new
concurrent-reset guarantee or a change to Runtime's safety deadlines.
