# M26-03R reproducible commands

Run from the repair worktree. Raw outputs are in `M26-03R-logs/`.
The ordinary cumulative profile runs contract, quick and full sequentially.

```sh
./scripts/agent-verify.sh contract
./scripts/agent-verify.sh full
build/agent-quick/tests/simcore_tests --gtest_filter='CommandBatch.*:RateDispatch.*:MixedRateReplay.*'
build/agent-quick/tests/rtfw_simulation_timing_noalloc
python3 tools/generate_sdk_docs.py --check
python3 tools/check_release_contract.py
python3 tools/check_c_abi.py
python3 tests/golden_system/verify_package.py --work-directory build/m26-03r-package
./scripts/verify-portable-assurance.sh static --build-dir build/m26-03r-assurance-fresh --source-commit 439b32e7d942f34253b976e31c54b1988c153ccd
```

The full profile uses the exact pinned RapidCheck source6e8dadfdafa3a74eabb52ead87f8787f72eccd0b
from the preserved local M26-02 build, supplied via CMake's
`FETCHCONTENT_SOURCE_DIR_RAPIDCHECK`; no dependency revision is changed.

```sh
cmake -S . -B build/m26-03r-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTS=ON -DRTFW_BUILD_EXPERIMENTAL=OFF -DSIM_WERROR=ON '-DSIM_SANITIZERS=address;undefined' -DCMAKE_CXX_FLAGS=-fno-pie -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build build/m26-03r-asan --target simcore_tests rtfw_simulation_timing_noalloc -j2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 build/m26-03r-asan/tests/simcore_tests --gtest_filter='CommandBatch.*:RateDispatch.*:MixedRateReplay.*'
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 build/m26-03r-asan/tests/rtfw_simulation_timing_noalloc
cmake -S . -B build/m26-03r-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTS=ON -DRTFW_BUILD_EXPERIMENTAL=OFF -DSIM_WERROR=ON -DSIM_SANITIZERS=thread -DCMAKE_CXX_FLAGS=-fno-pie -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build build/m26-03r-tsan --target simcore_tests rtfw_simulation_timing_noalloc -j2
setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=1 build/m26-03r-tsan/tests/simcore_tests --gtest_filter='CommandBatch.*:RateDispatch.*:MixedRateReplay.*'
setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=1 build/m26-03r-tsan/tests/rtfw_simulation_timing_noalloc
```

Default artifact and positive probes:

```sh
c++ -std=c++20 -Irt/include -Icore/include -Iinclude tests/device_rate_simulation/default_identity.cpp build/agent-quick/librtfw_runtime.a -pthread -o /tmp/cpp-m26-03r-default-verified
/tmp/cpp-m26-03r-default-verified /tmp/cpp-m26-03r-default-verified.bin
cmp /tmp/cpp-m26-03r-default-final-before.bin /tmp/cpp-m26-03r-default-verified.bin
c++ -std=c++20 -Irt/include -Icore/include -Iinclude tests/device_rate_simulation/old_path_probe.cpp build/agent-quick/librtfw_runtime.a -pthread -o /tmp/cpp-m26-03r-positive-verified
/tmp/cpp-m26-03r-positive-verified
```

The baseline default probe uses the same `default_identity.cpp` and fixture but
all three include directories and `librtfw_runtime.a` from preserved M26-02.
The negative policy probe uses repaired headers and the preserved old library;
its required result is exit3/device_timeout/zero publication with timeout1000000.
It demonstrates the old implementation's behavior, not supported cross-version
C++ binary compatibility. The baseline and repaired536-byte artifacts are retained.

For the metadata regression negative control, run the new test before the
opt-in metadata condition in `DeviceManager::finish_batch_slot` is applied:
`--gtest_filter=MixedRateReplay.SimulationFailedCompletionRetainsBackendTimestamp`.
The retained pre-fix log shows the missing201 domain and900000000000 timestamp.

For the final analysis, the already configured compile database was reused with
an unused output path (the checker intentionally refuses to overwrite reports):

```sh
python3 tools/check_static_analysis.py --repo-root . --compile-commands build/m26-03r-assurance/static/compile_commands.json --source-manifest tools/static_analysis_sources.txt --clang-tidy clang-tidy-14 --output build/m26-03r-assurance/static-report-final.json
```

After the queued-pointer retention correction (the only production delta from
439b32e), rerun the focused/full, both sanitizers, allocation, CPack and default
comparison commands above. Complete36-unit static analysis is combined with a
fresh analysis of the only changed production unit:

```sh
clang-tidy-14 -p build/m26-03r-assurance/static --config-file .clang-tidy --warnings-as-errors='*' rt/src/device_manager.cpp
```

See `M26-03R-logs/static-composition.json` for the exact final source digest.
