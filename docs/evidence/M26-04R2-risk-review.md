# M26-04R2 capacity policy review

Candidate review; integration and complete gates remain pending.

The opt-in is selected before any successful backend registration. Unknown enum
values and adapted-v1/core-only/missing-extension backends reject without backend
initialization. Registration copies validated capabilities; finalize resolves
min(global, core, command) slots and min(global, command) poll capacity. The
unchanged admission compiler receives these stricter copied bounds and retains
its global/backend/phase demand checks. Global3 does not inflate CUDA1/XDMA2.

Existing BatchBackendState slot_offset/slot_count drive allocation, initialization,
submission, retirement, cancellation, stop and replay reconstruction. The actual
sum drives MemoryPlan and the DeviceManager estimator. Shared completion scratch
stays global-sized; opted-in polling and count validation use the native bound.
Uniform polling remains exactly global-sized, including configure-after-register
behavior. Policy booleans occupy existing alignment padding; Linux baseline
checkpoint and active replay bytes match exactly after the change. Other hosted
platform checks remain pending. No existing public structure was resized.

Conditional graph/config markers bind the opt-in and resolved counts. Default
hash inputs remain unchanged. Tests cover same-topology policy mismatch,
origin-owner replay, foreign-owner rejection, compatible paired recovery and
corrupt artifacts before effects. No artifact or historical evidence is rewritten.

Actual one/two-slot initialized counts, 80-frame work, three-reservation admission,
aggregate and backend rejection, real Runtime slot saturation, narrow polling,
over-count rejection, partial initialization, failed stop/unregister/shutdown
retention and retry, terminal errors/cancellation and concurrent owners are tested.
Positive-controlled ordinary heap monitoring includes native workers and replay.
The retained feature prototype also executes unchanged native CUDA1/XDMA2 in
kernel/Graph and native/host modes with complete canonical CPU parity at9 entities
and24 ticks. That probe is prerequisite proof, not completion of the feature matrix.

Native backends, prior fixtures, config/C ABI/device ABI/SONAME, rate admission
algorithm, deadlines, sampled safe acknowledgement and old CUDA event pools are
unchanged. Remaining risk is concurrency/platform behavior until sanitizer,
static, full/package and all32 exact-head gates finish. No physical/HIL/RT/human/
Unreal/performance/release claim. M26-05/M26-06 remain inactive.
