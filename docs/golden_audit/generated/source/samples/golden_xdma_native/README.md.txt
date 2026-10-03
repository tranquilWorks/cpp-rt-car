# Native golden XDMA host preparation

This optional 64-bit Linux source kit assembles the frozen golden graph using
`rtfw::xdma_linux` and explicit borrowed host staging. Its native backend keeps
`deterministic_mock=0`; active and live-control replay are disabled. The CPU model,
integer codecs, phase/domain/channel identities, 500-microsecond completion budget
and 8-millisecond safe transition deadline remain unchanged. It does not use CUDA
or peer DMA. The existing simulated golden kits are preserved.

The executable performs graph construction, startup and checked stop only. It
never reports a completed golden run. A successful preparation means the native
protocol acknowledged the startup/stop requests; physical output safety, FPGA
correctness, timing and qualification still require independent bench evidence.
The scenario graph is available in the kit's Session for subsequent bench work;
full native execution, external CIL, replay and a hardware artifact validator are
not delivered by this preparation batch.

## Build the actual installed host

Build/install RTFW with `-DRTFW_ENABLE_XDMA=ON` on Linux, then build the kit against
that installation. Keep the sibling golden_system and golden_xdma source kits.

```sh
cmake -S /sdk/share/rtfw/examples/golden_xdma_native -B /tmp/native-host \
  -DCMAKE_PREFIX_PATH=/sdk
cmake --build /tmp/native-host
/tmp/native-host/golden_xdma_native --prepare /approved-bench/tuple.cfg
```

The default SDK cannot satisfy the optional xdma_linux component and must refuse
this configure. No native target or feature macro is added to `rtfw::runtime`.

## Named configuration and FPGA contract

All 15 keys below are required, unique and exact. Offsets are decimal unsigned
integers, 64-byte aligned, within signed Linux off_t range, and four windows must
not overlap even if both channel nodes share one AXI address space. Sensor windows
are 6264 bytes; actuator windows are 3192 bytes. The control aperture is eight
bytes: 32-bit little-endian calibration words at 0 and 4; events 0 and 1 acknowledge
the corresponding lanes. Each lane uploads input, uploads the output envelope,
writes calibration, waits for its event, then downloads output. The FPGA must
preserve sampled header identity/sequence/generation/nominal timestamp, apply the
frozen sensor calibration or actuator echo, zero safe output payloads, and recompute
the sampled payload checksum. A generic XDMA memory image is insufficient.

This is a template only: replace every tuple, hash, path and offset with the named
approved bench values. No endpoint has a fallback or guessed default.

```text
tuple=APPROVED_BENCH_RECORD
bitstream_sha256=REPLACE_WITH_64_LOWERCASE_HEX_DIGITS
driver_revision=8721136e74a66500b02d16cb41922d966139cd46
layout=golden-xdma-native-v1
h2c0=/approved/h2c0
h2c1=/approved/h2c1
c2h0=/approved/c2h0
c2h1=/approved/c2h1
user=/approved/user
event0=/approved/event0
event1=/approved/event1
input0=0
output0=8192
input1=32768
output1=40960
```

The tuple must resolve to the operator's retained CPU/NUMA, PCI BDF/link, IOMMU,
OS/kernel, module parameters, FPGA/IP/clock/memory map, compiler/runtime build,
host-memory and worker policy record described in `xdma_support_matrix.json`.
The file records operator assertions; it does not attest a loaded driver/bitstream
or qualify a tuple. Paths are checked before constructing the adapter or Runtime.
Missing paths return exit3 and `NOT_RUN`; malformed configuration or non-character
nodes return exit2. Startup/stop failure returns exit1. Preparation success returns
exit0 and explicitly leaves scenario execution unverified. Filesystem inspection
cannot prevent endpoint replacement between inspection and native open; an approved
bench must retain exclusive endpoint/configuration control.

## Ownership and evidence boundary

Driver, backend, six aligned host buffers and Runtime memory outlive checked stop.
Failed startup still goes through cleanup. A failed stop retains ownership for a
bounded retry; persistent failure follows the existing fail-stop Session destructor
contract rather than releasing borrowed storage while work may remain. This is an
off-lane preparation command, not a real-time launcher. Native frame execution uses
the caller's monotonic RuntimeClock; injected logical clocks in tests are simulation
only, never hardware timing evidence.

Ordinary tests exercise the native capability path with an injected driver,
startup/stop, zero protocol submit/poll allocations, format/protocol
refusals, partial initialization and missing endpoints in the actual executable.
The retained optional `test_golden_xdma_native --frame` probe checks one full
native frame and the frozen oracle. Ordinary and ASan runs passed, but TSan
execution exceeded the unchanged 500-microsecond Runtime submission deadline.
The mandatory preparation/protocol suite uses the injected driver clock and does
not assert host timing. No deadline, original test or native capability is changed.
No live node is opened by validation. FPGA/driver functionality, physical safe
outputs, device-loss/thermal/saturation/endurance, actual golden native execution,
controlled latency, HIL and RT1/RT2 remain unperformed. Existing support claims,
ABI v8/device ABI1, frozen workload and original tests remain unchanged.
