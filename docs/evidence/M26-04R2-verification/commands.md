# Reproduction and exact command families

Repository: `/home/kbianco/.local/share/portfolio-control/worktrees/targets/cpp-rt-car-m24`.
Baseline e8caf4396cd5987d9aa4a4bf3dad9a178cf2ded7; canonical control564.
All commands ran from this target checkout unless stated otherwise.

```
./scripts/agent-verify.sh contract
./scripts/agent-verify.sh full
cmake --build build/agent-quick --target simcore_tests rtfw_device_capacity_noalloc -j 3
build/agent-quick/tests/simcore_tests --gtest_filter='CommandBatch.*:RateDispatch.*:MixedRateReplay.*:SampledIo.*:CrossRateData.*:MemoryPlan.*'
build/agent-quick/tests/rtfw_device_capacity_noalloc
python3 tools/check_static_analysis.py --compile-commands build/m26-04r-static/compile_commands.json --source-manifest tools/static_analysis_sources.txt --clang-tidy clang-tidy-14 --output /tmp/cpp-m26-04r2-static-report.json
python3 tests/golden_cuda/verify_package.py --work-directory /tmp/cpp-m26-04r2-package
python3 tools/generate_sdk_docs.py
python3 tools/check_release_contract.py --write-hashes
```

The existing sanitizer build trees were recompiled for the changed Runtime:
`cmake --build build/m26-04r-{asan,tsan} --target rtfw_runtime -j 3`.
They retain Debug, full Runtime instrumentation and SIM_SANITIZERS
`address;undefined` / `thread`, respectively. Tests compile with
`g++ -std=c++20 -O1 -g -pthread -fno-omit-frame-pointer -fno-pie -no-pie`, the
matching `-fsanitize=address,undefined` or `-fsanitize=thread`, include flags
`-I. -Irt/include -Icore/include -Iinclude -Iexternal/googletest/googletest/include`,
the matching instrumented Runtime archive, and existing matching instrumented
GTest main/core archives under `../cpp-rt-car-m26-03r/build/m26-03r-{asan,tsan}/lib`.

The51-test source list is test_device_capacity.cpp, test_sampled_io.cpp,
test_cross_rate_data.cpp, test_mixed_rate_replay.cpp and
test_device_rate_simulation.cpp under tests/. The final fixture-accounting
correction reruns test_device_capacity.cpp alone (11tests); prior sources and
Runtime are unchanged. Allocation executables compile tests/device_capacity/noalloc.cpp
with the same sanitizer flags/Runtime, without GTest. ASan executes with
`ASAN_OPTIONS=detect_leaks=1`. TSan retry source and every attempt are retained;
only explicit startup mapping failures are automatically retried. A bare SIGSEGV
stopped that retry and was investigated separately:

```
gdb -q -batch -ex 'set disable-randomization off' -ex run -ex bt --args /tmp/cpp-m26-04r2-tsan-final-tests
gdb -q -batch -ex 'set disable-randomization off' -ex run -ex bt --args /tmp/cpp-m26-04r2-tsan-final-tests --gtest_list_tests
```

The list-only diagnostic reproduced allocator mmap failure during `_dl_init`,
before main; both the complete debugger run and a separate direct11-test run
passed. This is retained as startup evidence, not a waived test/race failure.

Default checks compile unchanged tests/package_consumer/sampled_io_storage_consumer.cpp
with `g++ -std=c++20 -O2 -pthread -Irt/include -Icore/include -Iinclude` against
both baseline and repaired Runtime archives, run `--two <checkpoint-path>`, and
compare checkpoint plus `.active` bytes. The retained plan-accounting.cpp prints
named MemoryPlan fields against those same archives. The old-order negative uses
the new device_capacity_consumer.cpp with `-DRTFW_CAPACITY_BASELINE` to omit only
the unavailable setter; its unchanged successful-start assertion fails on old
registration, with no backend initialization.

Native prerequisite probe compiles retained native-probe/tests/golden_xdma/test_xdma.cpp
and unchanged tests/golden_cuda/allocation.cpp with the same O2/include flags,
linked to build/agent-quick/librtfw_xdma_backend.a, librtfw_cuda_backend.a and
librtfw_runtime.a. Restore its sibling headers from the exact hashes in
native-probe/source-binding.json. All six actual simulated native backend modes
pass at9 entities/24ticks; this is not the complete M26-04 acceptance campaign.
