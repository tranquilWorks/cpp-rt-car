# M28-03 scoped risk review

Production remains byte-identical to target295. Startup rollback must report failed
cleanup rather than conceal borrowed native ownership behind the original timeout.
The channel independently retains timeout/unknown safety. Existing 8ms assertions
can still fail under scheduling variation; unchanged retry success is not a fix.
The Windows 2024ms observation has a plausible two-second driver-bound control,
but the original cause remains unproven.

The new fixture uses the original public graph, real native backend and injected
owned driver. Test callback holds are finite; helpers modify only atomic clocks or
release flags while the owner thread uses Runtime. Borrowed driver/command wrappers
outlive every callback through successful checked stop. Failed shutdown preserves
live ownership for retry. No RT callback implementation changes. No cross-owner
state is shared other than the pre-existing allocation observer, which is disabled
for concurrent-owner cases. The successful-owner-during-pending-owner case supplies
explicit coexistence evidence. Native readback and application publication are
separate assertions, including no timeline success after logical expiry.

Normal/sanitizer/static checks and old sampled tests pass. Full/package/all32 exact
head checks and source/scope/ABI/clean/review/tree integration guards remain required.
Agent review does not substitute for independent human acceptance. No live endpoint,
licensed engine, controlled performance, production signing or release is performed.
Keep broader backend/SDK/native timing findings and final owner gates unresolved.
