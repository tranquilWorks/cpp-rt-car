# M27-07 risk review

High-risk native boundary review, preparation scope only. Canonical control603
merged38437dba944fc305c35e5421411893d84473725d activates target baseline
3b9fc08afc1e7c4b5e6230b80f1235477a5acbab (target291). No production/native
backend, frozen kit, previous test, ABI/default or workflow source is changed.

- Ownership: Linux driver and config outlive Io/backend, six aligned borrowed
  host buffers and Runtime. Partial acquisition rolls back; checked failed stop
  preserves memory and driver ownership for retry. Persistent cleanup failure
  uses the inherited fail-stop destructor contract. The fake owner tests cover
  partial initialize and failed shutdown followed by successful retry.
- Native identity: no deterministic capability spoof or simulation timing. Active
  replay and live-control replay are disabled; frozen phase/domain/channel and
  memory-plan accounting assertions remain. Original frozen model/codecs are
  included directly. Native graph assembly duplicates adapter-specific wiring;
  future frozen-graph changes require reviewing both assemblies.
- Configuration: exact bounded 15-key file, explicit tuple/bitstream/driver/layout,
  seven paths, four aligned bounded non-overlapping windows. Missing/regular paths,
  duplicate/unknown/missing keys and overlapping windows refuse before native
  initialization. Named identities are operator assertions, not loaded-image
  attestation; endpoint replacement between stat/open requires bench ownership.
- Staging/format: two fixed lanes, explicit host copies, no peer DMA. Ordered
  input/template/control/event/download protocol retains sampled envelopes. Only
  an application copy normalizes timestamps; Runtime keeps original headers.
  Ordinary protocol tests assert short transfer, missing ACK, corrupt header/
  checksum and stale sequence rejection, with zero submit/poll allocations.
- Installed entry point: default component refusal; actual relocated optional
  CPack host compiles and refuses missing nodes. Final installed kit bytes equal
  reviewed sources. Independent copied fake consumer and optional package-consumer
  CMake/CTest wiring pass. Default targets/header inventory and C ABI remain covered
  by unchanged contract and required complete local/hosted gates.

## Retained findings and limits

The initial full native-frame fixture passed ordinary and ASan execution but
failed under initialized TSan with status-18, zero allocations and `device
submission timed out`. Inspection confirmed the native DeviceManager's real
500-microsecond submission deadline. The executable requested here only prepares
the graph and checks startup/stop. Its mandatory suite therefore verifies those
operations and the actual native command protocol with an injected driver clock;
it does not use instrumented-host timing as a preparation oracle. The original
full-frame/oracle/refusal assertions remain callable with `--frame`; ordinary
final execution passes, and the TSan execution result remains failed/unqualified.
No Runtime deadline, native capability, old assertion or old gate is altered.
Actual golden native execution and performance remain unperformed on hardware.

The first package harness expected CMake's diagnostic to name xdma_linux, while
CMake correctly emitted generic rtfw_FOUND=false. The final harness verifies that
refusal and the missing optional library instead. The first canonical hosted
attempt failed in unrelated temporary Git-directory cleanup; unchanged rerun
passed. All attempts remain in the retained manifest. TSan pre-main mapping
failures are retained separately from initialized results; no race suppression
or successful-run fabrication is used.

Final mandatory preparation/protocol tests pass complete Runtime/native-backend
ASan/UBSan/leaks, initialized TSan and Clang14 static analysis. Full repository
portable157/strict162 and all32 exact-head hosted records remain mandatory before
merge; the closure records their actual results and guarded fetched-tree identity.
No live endpoint, FPGA output, hardware timing, RT1/RT2 or release claim is made.
