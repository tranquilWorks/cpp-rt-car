# M26-04 resumed checkpoint risk review

Blocking finding: the combined graph cannot satisfy both current global admission
and each native backend's actual capacity. Registration requires global3 on CUDA1;
smaller global1/2 configurations reject the conservative simultaneous3-release
admission demand. Enlarging the native XDMA queue would violate the frozen two
slots. The proposed explicit per-backend capacity policy is a new Runtime/C++
contract decision, not part of the completed ring-storage repair. Do not merge
feature279 or silently change Runtime, old tests, timeouts or advertised limits.

The original ring blocker is resolved: actual four-slot XDMA-only graph executes
24ticks with native driver operations and startup safe ACKs. New count9/ticks24
native/host tests compare every canonical non-owner byte with an actual CPU owner;
only the original owner-bound generation/checksum differ between owners, and
originating-owner trusted replay verifies complete original canonical bytes.
Steady/replay allocations are0 with an independently detected positive control.
The new owner retains memory/jobs/drivers through checked Runtime close. Replay
uses public envelope ownership checks and synchronizes the injected card clock.

Prototype review limits: the new tests are not yet in CMake/mandatory CI, no
installed CLI/kit exists, combined execution has not run, and the full numerical,
fault/recovery/lifetime/isolation/sanitizer/static/package matrix remains
unperformed. The old source kit and every Runtime/native/repair source/test are
unchanged. Scoped MSVC alignment annotations apply only to new sample classes.
No complete safety, feature, CAP-M26 or physical/RT/release claim is justified.

Retain failed combined preparation, all diagnostic variants including the failed
all-three startup, and the first XDMA-only log's overbroad static output label.
The corrected selected-mode label and successful rerun are separately retained.
Proposal M26-04-capacity-repair-proposal.md is concrete but not implemented.
