[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)

> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.

# External controller shared-memory reference

This optional source kit connects two actual processes: a public RTFW Runtime
plant and a controller. It supports little-endian x86-64 Linux with GCC/Clang
and Windows with MSVC. It requires C++20, CMake 3.20+, Python 3 and the RTFW SDK.
It installs source only; no extra SDK header, exported target, Runtime dependency,
or compiled ABI is introduced. Peers must be trusted local processes in the same
user/session security boundary. This is a software protocol example, with no
network, CAN, physical HIL, safety certification or hard-real-time claim.

## Build and run

After installing RTFW, use its data directory (normally `share/rtfw`; custom
`CMAKE_INSTALL_DATADIR` is supported):

```sh
cmake -S "$PREFIX/share/rtfw/examples/cil_shared_memory" -B cil-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX"
cmake --build cil-build --config Release --parallel 2
python3 "$PREFIX/share/rtfw/examples/cil_shared_memory/runner.py" --plant "$PWD/cil-build/cil_plant" --controller "$PWD/cil-build/cil_controller" --normal-only
ctest --test-dir cil-build -C Release --output-on-failure
```

For Visual Studio, use `cil-build/Release/cil_plant.exe` and
`cil-build/Release/cil_controller.exe` in the Python command. In a source checkout,
replace the first command with:

```sh
cmake -S samples/cil_shared_memory -B cil-build -DCMAKE_BUILD_TYPE=Release -DCIL_RTFW_SOURCE="$PWD"
```

The kit supports both `find_package(rtfw CONFIG REQUIRED COMPONENTS runtime)`
and explicit source embedding. Root examples remain opt-in when embedding.
The normal supervisor checks the complete transcript before printing:

```text
ready plant
plant status=ok generation=11 applied=8 position=255 ack=1 history=128,192,224,240,248,252,254,255
ready controller
controller status=ok sent=8 efforts=128,64,32,16,8,4,2,1
```

For manual two-terminal operation, launch `cil_plant NAME GENERATION TIMEOUT_MS`
then `cil_controller NAME GENERATION TIMEOUT_MS`. NAME is 1–48 ASCII letters,
digits or underscores. GENERATION is a nonzero uint64. TIMEOUT_MS is 100–10000.
Use a fresh unpredictable name and generation on every session/reconnect; the
caller owns generation uniqueness. The reference does not maintain a persistent
registry. The supervisor chooses UUID names. Controller fixture modes are for
testing only; omit them for ordinary use.

## Plant and failure policy

Three typed Runtime phases form an explicit dependency graph: validate input,
integrate integer position, publish state. Initial position is zero. The controller
computes `(256 - position) / 2`; eight accepted commands reach 255. The host records
all positions in fixed storage. An independent process oracle checks every state,
command and final shutdown, rather than accepting successful exit codes alone.

Only a valid, fresh command for the exact last published plant record can apply.
Malformed, duplicate, skipped, stale-generation, future, expired or miscorrelated
input selects **hold/no apply** and ends the session. No previous effort is replayed.
A full output slot rejects the step before consuming input or changing position.
A missing/partially published command times out on the control thread and holds.
This illustrative hold policy is not a qualified safe state for a physical plant.

Host timestamps use one monotonic nanosecond clock domain. The controller echoes
that domain's issue/expiry values and never compares its own clock epoch with them.
The host checks freshness at the frame-entry snapshot: `issued <= now < expiry`.
This bounds admission age, not wall-clock callback completion time. Startup allows
five seconds for attachment before issuing the first plant record. TIMEOUT_MS
separately sets the command lease, control-thread reply wait and stop-ack wait.
The controller has a 30-second overall demonstration bound. OS launch, polling
and 1-ms sleeps happen outside Runtime callbacks. Callbacks perform fixed work
without allocation, locks, OS calls, I/O or retry loops; the host still depends on
ordinary OS scheduling. Overflow/exhaustion fails closed without sequence wrap.

## Wire v1

Every message is exactly 96 bytes with explicit little-endian loads/stores; no
C++ object or `std::atomic` representation is serialized. Offsets are bytes.

| Offset | Width | Meaning |
| --- | --- | --- |
| 0 | 4 | Magic `CIL1` (`0x314c4943`) |
| 4 | 4 | Version 1 |
| 8 | 4 | Length 96 |
| 12 | 4 | Kind: plant 1, command 2 |
| 16 | 8 | Nonzero generation |
| 24 | 8 | Sequence, starting at 1 independently in each direction |
| 32 | 8 | Nonzero host issue time, ns |
| 40 | 8 | Exclusive expiry, greater than issue, at most 10 seconds later |
| 48 | 8 | Correlation: zero for plant, exact plant sequence for command |
| 56 | 8 | Frame: initial plant 0, then accepted integration count |
| 64 | 8 | Signed two's-complement value: position or effort |
| 72 | 4 | Flags, must be zero |
| 76 | 4 | Clock domain 1 (host monotonic ns) |
| 80 | 16 | Reserved, must be zero |

Plant position is bounded to ±1,000,000; command effort to ±256. UINT64_MAX
sequence/frame is rejected. Command correlation, frame, issue and expiry must
exactly match the latest plant publication. Sequence advances only on successful
publication or valid reception. A rejected record is consumed but does not advance
valid sequence or alter the decoded destination. Reconnection is required after
protocol failure. No authentication or protection against a malicious mapper exists.

## Region, publication and ownership

The platform region is exactly 4096 bytes, aligned to 64. Shared aligned 32-bit
words at offsets 0/4/8/12 are ready/controller-claim/stop/ack. A 48-byte immutable
header at 16 contains magic, version, region length, wire length, generation,
clock domain and zero reserved bytes. Plant and command slots begin at 64 and
256. Each 192-byte slot has a state word, 60 padding bytes, 96-byte payload and
32 padding bytes. The final 3648 bytes are reserved. Compile-time size/offset
assertions bind this platform layout independently of the wire layout.

Each direction is a single-producer, single-consumer one-record channel. State 0
means empty; 1 means published. Producer acquire-loads empty, copies all bytes,
then release-publishes 1. Consumer acquire-loads 1, copies the payload, then
release-publishes 0. Full returns backpressure, never overwrite. Unpublished
partial payload is never read. Each operation has a fixed number of accesses.
Linux uses aligned lock-free compiler atomics; Windows uses process-shared
Interlocked operations. This is a declared platform contract, not a claim that
portable C++ atomics universally work in mapped memory. See
[GCC atomic builtins](https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html)
and [Microsoft Interlocked access](https://learn.microsoft.com/en-us/windows/win32/sync/interlocked-variable-access).

The creator exclusively owns initialization and its mapping name. Newly created
OS pages are zeroed; initialization writes only the immutable header before
release-publishing ready. It never resets a shared word after the name becomes
visible. Attachment first validates readiness, size/schema/generation and stop,
then claims the sole controller using one compare/exchange. The claim is never
cleared or reused. Linux verifies exact object extent with `fstat`; Windows maps
a 4096-byte view and validates declared extent/header, without claiming inspection
of a larger section's full native extent. Names use Linux `/rtfw_cil_` with mode
0600 or Windows `Local\rtfw_cil_` and the creator's default security descriptor.
See [Linux shm_open](https://man7.org/linux/man-pages/man3/shm_open.3.html),
[Windows named mappings](https://learn.microsoft.com/en-us/windows/win32/memory/creating-named-shared-memory)
and [MapViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile).

On shutdown the host checks Runtime stop, detaches its endpoint, publishes stop,
and waits a bounded time for the controller to drain final output and acknowledge.
Missing acknowledgement is an explicit failure, even if all eight commands applied.
Only then does the host close its own mapping. Stop failure retains ownership and
terminates; it never unmaps beneath live callbacks. The controller detaches before
unmapping. Failed unlink/unmap/Windows handle close retains the corresponding
ownership marker for retry. Tests inject a pre-close failure and check retention.
Linux close errors invalidate the local descriptor marker because retrying the
numeric descriptor can close a reused resource; this is Linux-specific
[close behavior](https://man7.org/linux/man-pages/man2/close.2.html).
An unresolved cleanup retry terminates rather than reporting successful cleanup.

Reconnect always creates a **fresh name, mapping and generation**. Never reset a
live region in place or hand its writer claim to a replacement peer. Closing one
process's view does not revoke another process's view. Linux unlink removes the
name while old mappings survive; Windows destroys the object after its remaining
views/handles close. A paused old peer can resume writing only its old region.
An abruptly killed Linux creator can leave a name behind: manual recovery must
identify that exact abandoned session after its processes stop, never reuse/reset
it or unlink another live session. This reference provides no crash-recovery daemon.

## Verification and limits

CTest runs real separate processes for normal feedback, malformed records, expiry,
replay, missing/dead/partial peers, duplicate creator/writer, missing stop ack,
fresh reconnect while an old mapped writer remains alive, and two simultaneous
Runtime instances. Supervision uses finite waits and terminates only owned children.
Unit tests independently bind golden wire bytes, negative decode/acceptance,
fake-clock Runtime admission, full-slot hold, allocation counts, concurrent payload
conservation and cleanup retry. ASan/UBSan/leak checks exercise process and unit
paths; TSan checks same-process publication and Runtime execution. TSan does not
establish cross-process race freedom. Linux and Windows hosted gates remain
required. Independent novice first-use observation is unperformed.

The installed inventory is `CMakeLists.txt`, `wire.hpp`, `channel.hpp`, `mapping.hpp`,
`common.hpp`, `plant_runtime.hpp`, `plant.cpp`, `controller.cpp`, `runner.py`, plus
this guide outside the kit directory. Package verification builds from a relocated,
tests-disabled SDK after removing its original prefix, with custom data/include
paths and spaces; it checks exact bytes, full SDK compatibility, source embedding
and negative package/source/header/output controls. The mandatory Runtime library,
SDK target graph, previous examples and qualification claims remain unchanged.
