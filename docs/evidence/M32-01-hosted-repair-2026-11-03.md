# M32 hosted portability repair and source integration boundary

The owner requested merging the completed pre-environment implementation and
awaiting real testing, or continuing any remaining source work. This authorizes
source integration after the unchanged standard gates pass; it does not establish
Unreal or physical acceptance.

## Observed failures

Initial PR306 head `338d9bd81f926c04bf35efe642f064bd03b116f5`, workflow
[41677149889](https://github.com/tranquilWorks/cpp-rt-car/actions/runs/41677149889),
exposed two defects in the new M32 test harness:

- Clang embedding job126060730353: `rtfw_unreal/worlds.hpp` not found when
  `add_subdirectory` is invoked from an external consumer. The new test used
  `CMAKE_SOURCE_DIR`, which belongs to the parent project.
- Windows RelWithDebInfo job126060730125: 208 of 209 tests passed; the new
  `unreal_source_package` failed at CMake configuration. Its untyped DATA cache
  argument retained Windows backslashes, making `\a` an invalid CMake escape
  when the path was expanded into the installed module source.

Complete initial failing logs are retained under `M32-01-checkpoint/` as
`hosted-initial-126060730353.log.gz` and
`hosted-initial-126060730125.log.gz`. Cancelled sibling matrix jobs do not count
as passes. No failure is waived by the earlier local success.

## Repair

The new test now uses the existing `RTFW_PROJECT_SOURCE_DIR` and
`RTFW_PROJECT_BINARY_DIR` so includes and package installation refer to the
RTFW subproject. The relocated consumer passes typed PATH cache arguments with
forward slashes, and its checkout-leak assertion normalizes separators as well.
The exact two source hashes are refreshed in the release manifest.

Runtime, engine adapters, shared lifecycle logic, existing tests/assertions/
timeouts, workflows, ABI/default SDK and frozen acceptance ledgers are unchanged.
The existing embedding and Windows package gates are direct regressions for
these failures; no extra duplicate test or changed threshold is introduced.

## Verification and integration

The repaired new contract compiles and passes in the real existing
`tests/add_subdirectory_consumer` parent project, with tests explicitly enabled:

```sh
cmake -S tests/add_subdirectory_consumer -B build/m32-embedded \
  -DCMAKE_BUILD_TYPE=Release -DRTFW_SOURCE_DIR="$PWD" \
  -DRTFW_BUILD_TESTS=ON -DRTFW_BUILD_EXAMPLES=ON \
  -DRTFW_EXPECT_TESTS=ON -DSIM_DOWNLOAD_GTEST=OFF
cmake --build build/m32-embedded --target rtfw_unreal_worlds_contract --parallel 2
ctest --test-dir build/m32-embedded/rtfw-build \
  -R 'host_adapter_unreal_worlds_contract|unreal_source_prepare_contract' \
  --output-on-failure
```

Both selected tests passed. A clean tests-disabled RelWithDebInfo SDK build and
`python3 tests/unreal_source/verify_package.py --build build/m32-sdk --config RelWithDebInfo`
also passed, including removal of the original install prefix, source staging,
standalone compilation and execution of the relocated world/module consumer.
Focused logs and command receipts are retained in the checkpoint directory.
Final complete local and hosted results must be recorded on PR306 before merge.
The first local repair runner
stopped at the release hash check before the two modified source hashes were
refreshed; the refreshed runner is a separate run, not a suppression.

Rollback only these new test-harness changes and current documentation/hash
bindings. The ordinary installed SDK can test the portable orchestration; it
does not compile Unreal-dependent translation units. Native Windows Unreal is
not part of this first Linux engine tuple. Engine builds, real PIE, packaged
execution, hardware/RT/performance/endurance and independent human acceptance
remain unperformed. Excluded cyber criteria, signing, release and deployment
remain open. Source integration must not mark global completion true.
