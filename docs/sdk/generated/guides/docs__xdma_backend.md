[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)

> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.

# Xilinx XDMA Backend Candidate and Qualification Contract

RTFW 1.2 contains the M10 XDMA backend candidate for one deliberately narrow
stack:

- x86-64 Linux;
- the official `Xilinx/dma_ip_drivers` XDMA Linux character driver, anchored
  for implementation review at commit
  `8721136e74a66500b02d16cb41922d966139cd46`;
- the PG195 XDMA AXI memory-mapped example design or a host design preserving
  its H2C/C2H address and data-integrity contract;
- channel-zero `/dev/xdma0_h2c_0` and `/dev/xdma0_c2h_0` endpoints by default;
- page-aligned host buffers and page-multiple transfers in the committed
  qualification procedure.

The portable backend state machine and Linux adapter are implemented, but no
hardware tuple is approved. M10 therefore remains Candidate. This is not a
generic FPGA/DMA backend and does not cover QDMA, AXI streaming, Windows,
vendor runtime libraries, arbitrary bitstreams, or kernel-bypass descriptor
ownership.

## Claim boundary

The implementation establishes framework-level properties:

- the scheduler continues to depend only on device ABI version 1;
- `submit()` validates and publishes into a fixed-capacity queue without
  calling the XDMA driver, waiting, allocating, or creating a thread;
- `poll()` inspects fixed slots and never invokes a host callback or blocking
  transfer;
- a fixed worker team created during backend initialization owns all blocking
  `pread()`/`pwrite()` calls;
- registered buffers, transfer slots, workers, and channel counts have
  configured capacities;
- H2C/C2H ranges, access flags, channel indexes, offsets, lengths, overflow,
  and optional alignment are validated before queue publication;
- shutdown is a non-RT host lifecycle operation that joins the fixed I/O team
  before borrowed buffers or channel descriptors may be released.

These properties do not bound Linux scheduling, page pinning, PCIe completion,
interrupt delivery, IOMMU behavior, driver allocation, FPGA response, or the
duration of a blocking character-device call. The official character driver
may pin pages or allocate internal request state per call. Consequently, the
candidate is RT0 functional infrastructure until a named deployment publishes
resource and latency evidence. It makes no RT1, RT2, worst-case, or portable
XDMA claim.

The machine-readable boundary is
[the XDMA support matrix](https://github.com/tranquilWorks/cpp-rt-car/blob/98ecca5a6572dd83d59e26063716f6d76b7a5d4f/docs/xdma_support_matrix.json).

## Targets and package surface

`rtfw::xdma_backend` is always built and installed. It contains the portable
fixed-capacity state machine plus injectable `rt::XdmaDriverApi`; ordinary
Linux and Windows builders need no vendor headers or device.

`RTFW_ENABLE_XDMA=ON` is supported only on Linux and additionally builds and
exports:

- `rtfw::xdma_linux`, the production character-device adapter;
- `sample_xdma_qualification`, the opt-in functional and raw-evidence tool.

The adapter uses only the documented character-device behavior exercised by
the upstream `dma_to_device` and `dma_from_device` tools. It does not copy or
link upstream driver code.

## Host-owned lifecycle

The host must:

1. load a compatible `xdma` kernel module and program the declared bitstream;
2. verify that the expected H2C/C2H nodes correspond to the intended PCI
   function and channel;
3. construct `rt::LinuxXdmaDriver` with immutable endpoint paths;
4. construct `rt::XdmaDeviceBackend` with matching H2C/C2H channel counts,
   transfer limit, buffer limit, worker count, and alignment;
5. register borrowed host buffers through the runtime;
6. keep the driver object, backend object, device nodes, bitstream, PCI
   function, and borrowed buffers stable through shutdown;
7. unload/rebind the driver or reprogram hardware only after shutdown returns.

Initialization opens every configured endpoint with `O_RDWR | O_CLOEXEC` and
creates the fixed I/O workers. Buffer registration does not allocate a bounce
buffer or pin memory itself. The host is responsible for any explicit
`mlock()` policy and must include that policy in qualification evidence.

## Submission contract

Both opcodes use one `rt::XdmaTransfer` payload and one registered-buffer
reference:

| Opcode | Required access | Operation |
| --- | --- | --- |
| `xdma_device_opcode_host_to_card` | Host-readable, device-writable reference | `pwrite()` to the selected H2C node at `device_offset` |
| `xdma_device_opcode_card_to_host` | Host-writable, device-readable reference | `pread()` from the selected C2H node at `device_offset` |

`XdmaTransfer::channel` selects the direction-specific channel. The backend
checks the buffer slice, device-address addition, configured maximum transfer,
and configured power-of-two alignment. The Linux adapter retries `EINTR` and
continues correct short transfers; a zero-length or terminal short transfer is
an error. A successful completion reports the physical byte count in
`rtfw_device_completion::value`.

The named qualification profile uses one 4 KiB-aligned buffer, 4 KiB-multiple
transfers, channel zero, and the AXI-MM address supplied to the workflow. Other
values are configurable functionality, not qualified tuples.

## Concurrency and boundedness

Slots move through free, owned, queued, running, complete, and reaping states.
Submitters claim slots with a bounded compare/exchange scan. Workers claim
queued slots independently and publish results with release ordering. Polling
reaps only physically completed slots. There is no spill queue, inline
fallback, per-submission thread, callback, condition-variable wait, or heap
allocation in steady-state `submit()`/`poll()`.

The backend may use more than one fixed worker and channel, but that does not
prove parallel DMA engines, throughput scaling, ordering, or absence of host
buffer races. Graph dependencies and application resource declarations remain
the ordering contract.

## Timeout, reset, and shutdown

The character-device transfer cannot be safely canceled by this ABI.
`cancel()` returns `UNSUPPORTED`.

If a queued request expires before a worker claims it, the worker publishes a
timeout without touching hardware. If a running request expires, the slot is
marked timed out but remains quarantined until `pread()`/`pwrite()` physically
returns. Only then does polling publish `TIMEOUT` and release the dependency.
This prevents buffer reuse while the kernel could still access the span. A
permanently hung driver can therefore prevent `step()` or shutdown from
returning; falsely completing the graph would violate memory lifetime.

Backend `reset()` is allowed only with no outstanding request. The Linux
adapter closes and reopens the configured endpoints. This is a soft file-handle
recovery attempt, not a PCIe function-level reset, driver reload, FPGA reset,
or bitstream reinitialization. Device loss cannot be repaired by this soft
reset; the host must stop, repair/rebind the deployment, and construct a new
backend.

Shutdown stops acceptance, drains queued/running work, joins all fixed workers,
and closes endpoints. A failed close leaves backend shutdown retryable. The
host must retain borrowed storage and deployment ownership until shutdown
succeeds or an external device/driver teardown becomes the final reclamation
boundary.

If driver initialization reports failure after acquiring partial ownership,
the backend immediately attempts driver shutdown. `success` and
`invalid_value` establish that no ownership remains; any other cleanup result
sets the backend's retryable shutdown-incomplete state. This lets the runtime's
failed-start rollback preserve the backend instance until a later checked
`stop()` completes cleanup.

## Portable evidence

Automated CPU-only evidence covers:

- malformed configuration, payload, channel, access, range, and alignment
  rejection;
- H2C/C2H functional round trips through an injectable driver;
- bounded saturation with no spill or inline transfer;
- timeout quarantine until physical worker return;
- error, reset-required, health, soft reset, and shutdown behavior;
- concurrent submitters, fixed workers, and polling under ThreadSanitizer;
- zero allocation in steady-state submit/worker/poll;
- strict GCC, Clang, MSVC, AddressSanitizer, and UndefinedBehaviorSanitizer
  builds.

The opt-in qualification tool writes and reads deterministic patterns through
the real nodes, separates warm-up from measurement, checks every byte, and
emits raw per-direction:

- submit host-call duration;
- completion wait duration;
- accumulated poll host-call duration;
- poll count and completed byte count.

The workflow also records kernel, module, PCI, compiler, runtime, driver, and
bitstream identity. It validates the JSON as `evidence_only`, binds every raw
file to the complete source commit with byte lengths and SHA-256 values,
verifies that manifest, and uploads the bundle without modifying the support
matrix.

## Qualification exit gates

M10 can move from Candidate to Complete only after a declared tuple records:

- PCI vendor/device ID, BDF, link width/speed, IOMMU policy, NUMA placement,
  CPU, OS, kernel, PREEMPT_RT state, compiler, and runtime build;
- exact `dma_ip_drivers` revision/module build and module parameters;
- FPGA part, XDMA IP configuration, bitstream hash, clocks, channel mode, and
  memory-map contract;
- host alignment, page-lock policy, transfer sizes, queue depth, worker
  affinity/priority, warm-up, sample count, and thresholds selected before
  measurement;
- repeated H2C/C2H integrity and steady-state resource evidence;
- saturation, timeout, driver/device loss, reset, shutdown, and reboot/rebind
  recovery evidence;
- raw latency decomposition and an explicit reviewed pass/fail record.

No tuple has completed those gates in the 1.2.1 support matrix.

## M17-04 native HAL-v2 control and event operations

`XdmaDeviceBackend::api()` remains the exact device-ABI-v1 transfer path.
`hal_v2_registration(name)` adds the unchanged core-v2, memory/topology-v1,
and command/timeline-v1 tables over the same borrowed object. One backend
object can use only one path until checked shutdown succeeds.

`XdmaDriverApi` retains its complete version-1 aggregate prefix and version-1
default. Exact version 2 adds `control_read32`, `control_write32`,
`wait_user_event`, and idempotent nonblocking `request_stop`. Version 1 exposes
only H2C/C2H behavior. A native capability is published only when its complete
callback set and configured bounds are present. The Linux adapter supplies the
control callbacks only when a user-BAR path exists and supplies the event/stop
callbacks only when at least one event path exists, so mismatched backend and
endpoint configurations fail before initialization.

The native snapshot describes borrowed host staging, the configured AXI-MM
endpoint, and host-monotonic completion timestamps. It declares no pinned or
DMA-mapped pages, generic FPGA memory, peer path, interrupt latency, safe
register map, or driver cancellation. Existing H2C/C2H opcodes remain stable.

The vendor dispatch encodings are exact: read is
`0x58480000 | (offset / 4)`, write is `0x58490000 | (offset / 4)`, and event
wait is `0x584A0000 | event_index`.

When control is enabled, the aperture is nonzero, four-byte aligned, and at
most 262144 bytes, so offsets through 262140 are representable. At most 16
events are configured. A read or event wait uses one exactly sized writable
borrowed-host reference; a write uses one fixed four-byte little-endian
payload. Foreign/high bits, bad alignment/range/index/access/size/payload,
overflow, reserved data, and unavailable callbacks fail before driver entry.

Linux configuration copies an optional user-BAR path and up to 16 event-device
paths. Initialization opens only configured endpoints with close-on-exec and
the nonblocking behavior required for event wakeup. MMIO and event operations
execute only on the existing fixed backend I/O team. Event wait consumes the
checked finite batch timeout and is interruptible by the stop wakeup.
Canceling a queued batch produces its bounded canceled completion without a
driver stop request and therefore cannot interrupt an unrelated running event.
A running event cancellation alone issues the driver stop wakeup; consuming
that wakeup rearms later event waits. Noncancelable H2C/C2H calls retain their
existing quarantine.

Shutdown never detaches a worker or closes a descriptor that an in-flight call
may reference. Partial open, short I/O, `EINTR`, poll wake, close/reopen failure,
and stop retain first-error and unresolved-only retry semantics. The fixed
control/event paths, descriptors, queue slots, wakeup owners, and workers are
backend-private storage reported through existing capability bytes.

Portable injected MMIO/event tests and node-free Linux compile/package checks
do not execute a physical register or establish safe design-specific effects,
DMA, interrupt latency, bounded driver calls, hardware, HIL, field, RT1, or RT2
evidence. Any physical control-write plan requires separate maintainer review.

## M17-06 portable composition use

The combined sample uses the actual `XdmaDeviceBackend` native registration
with a complete injected version-2 driver and its existing fixed two-worker
team. Each successful frame performs exact H2C, aligned little-endian control
write, finite user-event wait, and C2H order against fixed simulated card,
control, and event state. Runtime timeout cancellation wakes a blocked event,
publishes no failing-frame timeline completion, suppresses C2H/validation, and
is followed by bounded stale-completion reaping and corrected execution.

No node, BAR, PCI function, kernel driver, firmware, bitstream, DMA engine, or
interrupt is touched. These are simulated protocol and cleanup semantics only,
not a safe hardware register plan or XDMA qualification.
