# Reproduction and claim boundary

Run from the target repository. Earlier failures are retained, including native
cleanup aborts, test construction corrections, loader-only TSan mapping failures
and experimental SimCore timeout. No functional/target gate is waived.

    ./scripts/agent-verify.sh full
    build/agent-quick/tests/simcore_tests --gtest_filter='CommandBatch.*:RateDispatch.*:MixedRateReplay.*:SampledIo.*:SampledIoStorage.*:SampledIoRecovery.*:DeviceCapacity.*:CrossRateData.*:DeviceRateSimulation.*:LiveControl.*:ControlReplay.*'
    build/agent-quick/tests/rtfw_sampled_simulation_noalloc
    python3 tests/golden_cuda/verify_package.py --work-directory /tmp/cpp-m26-04r2-package

The package gate creates a fresh CPack SDK with custom include/data paths,
spaces and removed original prefix; all56 SDK consumers, source embedding and
negative consumers must pass. It reuses only generated build cache.

Sanitizer commands, flags and exits are retained in drain-*-compile.gz and
attempt logs. Runtime/XDMA Debug caches use ASan+UBSan or TSan; new tests are
compiled O1 with matching sanitizer/no-pie flags. All13 new CommandBatch tests
join the existing CI filters. Each ordinary/ASan/TSan noalloc executable records
startup's existing allocation separately and requires steady/stop0, positive1.
No test failure retries; only diagnosed loader mapping startup retries.

Baseline mainf5402b9 Runtime archive is losslessly retained by its SHA256.
Compile the unchanged prior sampled_io_recovery_consumer.cpp against baseline
and repaired archives, run --defaults OUTPUT.checkpoint, and compare stdout,
checkpoint and checkpoint.active exactly. Compile the new noalloc consumer
against both: startup1 and steady/stop0 must match. Current corrected12-case
public consumer passes; pre-drain held-completion cases10/11 deterministically
retain unknown/live resources across eight stops and fail closed at destruction.
These are intentional baseline negatives, not successful recovery evidence.

    python3 tools/check_static_analysis.py --compile-commands build/m26-04r-static/compile_commands.json --source-manifest tools/static_analysis_sources.txt --clang-tidy clang-tidy-14 --output /tmp/static-report.json
    clang++-14 --analyze -std=c++20 -Xanalyzer -analyzer-output=text -Iinclude -Irt/include -Icore/include tests/package_consumer/sampled_io_simulation_consumer.cpp

The retained36-TU static run contains final host_runtime.cpp; subsequent
DeviceManager change was reanalyzed using the same checked policy and exact
compile-database entry (1TU delta). All other production sources are unchanged.
Both reports have zero diagnostics. Public fixture Clang analysis passes.

Only private host_runtime.cpp/device_manager.cpp production behavior changes.
Reviewed source hashes, default byte hashes and all failed diagnostics are
retained. No physical/HIL/RT/human/Unreal/performance/release claim.
