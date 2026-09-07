if (NOT DEFINED SIM_GTEST_AVAILABLE)
    set(SIM_GTEST_AVAILABLE OFF)
endif()

if (SIM_GTEST_AVAILABLE)
    set(TEST_SOURCES
        test_simcore_basic.cpp
        test_simcore_determinism.cpp
        test_logging.cpp
        test_async_logger.cpp
        test_logger_backpressure.cpp
        test_profiler.cpp
        test_adaptive_param.cpp
        test_soa.cpp
        test_sparse_set.cpp
        test_soa_view.cpp
        test_jobqueue.cpp
        test_queue_metrics.cpp
        test_metrics_json.cpp
        test_jobqueue_perf.cpp
        test_workerpool_priority.cpp
        test_phase_graph.cpp
        test_static_phase_graph.cpp
        test_chunk_autotune.cpp
        test_deterministic_reduce.cpp
        test_differential_output.cpp
        test_robust_fp.cpp
        test_budget_monitor.cpp
        test_fault_injection.cpp
        test_simd.cpp
        test_aosoa_autotune.cpp
        test_rt_pool.cpp
        test_rt_arena.cpp
        test_arena_binding_assert.cpp
        test_rt_overflow_death.cpp
        test_fiber_pool.cpp
        test_fiber_pool_race.cpp
        test_bintrace.cpp
        test_budget_degrade.cpp
        test_predictive_adaptive.cpp
        test_small_array_balance.cpp
        test_phase_telemetry.cpp
        test_aligned_allocator.cpp
        test_highres_clock.cpp
        test_highres_clock_integration.cpp
        test_rate_governor.cpp
        test_hazard_ptr.cpp
        test_lockfree_queue.cpp
        test_seqlock_config.cpp
        test_backpressure.cpp
        test_physics_integrators.cpp
        test_constraint_solver.cpp
        test_narrowphase.cpp
        test_hal.cpp
        test_gpu_stub.cpp
        test_gpu_overlap.cpp
        test_frame_graph.cpp
        test_watchdog.cpp
        test_rt_watchdog.cpp
        test_crashdump.cpp
        test_numerics.cpp
        test_prng_determinism.cpp
        test_snapshot.cpp
        test_trace_export.cpp
        test_trace_noalloc.cpp
        test_units.cpp
        test_huge_page.cpp
        test_thermal_limp.cpp
        test_limp_mode.cpp
        test_config_hot_reload.cpp
        test_plugin_manager.cpp
        test_sandbox.cpp
        test_minidump_symbols.cpp
        test_perf_artifacts.cpp
        test_scheduler.cpp
        test_host_runtime.cpp
        test_periodic_runtime.cpp
        test_platform_preflight.cpp
        test_compiled_graph.cpp
        test_executor.cpp
        test_memory_plan.cpp
        test_observability.cpp
        test_determinism_replay.cpp
        test_device_runtime.cpp
        test_cuda_backend.cpp
        test_vendor_hal_v2.cpp
        test_rt_pipeline.cpp
        test_platform_gate.cpp
        integration/test_determinism.cpp
    )

    if (NOT RTFW_BUILD_EXPERIMENTAL)
        set(TEST_SOURCES
            test_host_runtime.cpp
            test_periodic_runtime.cpp
            test_platform_preflight.cpp
            test_compiled_graph.cpp
            test_executor.cpp
            test_memory_plan.cpp
            test_observability.cpp
            test_determinism_replay.cpp
            test_device_runtime.cpp
            test_cuda_backend.cpp
            test_vendor_hal_v2.cpp
        )
    endif()

    # M15 CPU/memory policy is part of the supported runtime and must remain in both the
    # default and experimental-enabled GoogleTest configurations.
    list(APPEND TEST_SOURCES
        test_extension_registration.cpp
        test_command_batch.cpp
        test_heterogeneous_memory.cpp
        test_hal_v2.cpp
        test_cross_rate_data.cpp
        test_rate_dispatch.cpp
        test_rate_telemetry.cpp
        test_rate_timeline.cpp
        test_sampled_io.cpp
        test_loopback_backend.cpp
        test_live_control_mailbox.cpp
        test_live_control_actions.cpp
        test_live_control_sdk.cpp
        test_live_control_stress.cpp
        test_mixed_rate_actions.cpp
        test_mixed_rate_replay.cpp
        test_mixed_rate_conformance.cpp
        test_cpu_memory_policy.cpp
        test_memory_policy.cpp
        test_thread_policy.cpp)

    if (RTFW_BUILD_EXPERIMENTAL AND
        ENABLE_RAPIDCHECK AND TARGET rapidcheck_gtest)
        list(APPEND TEST_SOURCES test_jobqueue_property.cpp)
    endif()

    if (RTFW_BUILD_EXPERIMENTAL AND UNIX AND NOT APPLE)   # linux
        list(APPEND TEST_SOURCES
            test_affinity.cpp
            test_numa_pool.cpp
            test_numa_integration.cpp
        )
    endif()

    if (RTFW_BUILD_BENCHMARKS)
        list(APPEND TEST_SOURCES test_benchmark_contract.cpp test_benchmark_runner.cpp test_benchmark_artifacts.cpp test_benchmark_cpu.cpp)
    endif()
    add_executable(simcore_tests ${TEST_SOURCES})
    if (RTFW_BUILD_BENCHMARKS)
        target_link_libraries(simcore_tests PRIVATE rtfw_benchmark rtfw_benchmark_cpu)
        add_test(NAME m23_benchmark COMMAND simcore_tests
            --gtest_filter=BenchmarkContract.*:BenchmarkRunner.*:BenchmarkArtifacts.*:BenchmarkCpu.*)
        set_tests_properties(m23_benchmark PROPERTIES TIMEOUT 120)
    endif()
    add_library(test_extension_fixture SHARED extension_fixture.c)
    target_include_directories(
        test_extension_fixture
        PRIVATE
            ${RTFW_PROJECT_SOURCE_DIR}/include
            ${RTFW_PROJECT_SOURCE_DIR}/rt/include)
    set_target_properties(
        test_extension_fixture PROPERTIES OUTPUT_NAME extension_fixture)
    add_library(test_extension_fixture_bad SHARED extension_fixture_bad.c)
    target_include_directories(
        test_extension_fixture_bad
        PRIVATE
            ${RTFW_PROJECT_SOURCE_DIR}/include
            ${RTFW_PROJECT_SOURCE_DIR}/rt/include)
    set_target_properties(
        test_extension_fixture_bad PROPERTIES OUTPUT_NAME extension_fixture_bad)
    target_link_libraries(
        simcore_tests
        PRIVATE
            rtfw_runtime
            rtfw_cuda_backend
            rtfw_xdma_backend
            test_extension_fixture
            test_extension_fixture_bad
            ${SIM_GTEST_LIBRARIES})
    if (RTFW_BUILD_EXPERIMENTAL)
        target_link_libraries(
            simcore_tests
            PRIVATE
                simcore_platform
                simcore_rt
                rtfw_experimental)
    endif()
    if (RTFW_BUILD_EXPERIMENTAL AND
        ENABLE_RAPIDCHECK AND TARGET rapidcheck_gtest)
        target_link_libraries(simcore_tests PRIVATE rapidcheck rapidcheck_gtest)
    endif()
    target_include_directories(simcore_tests PRIVATE
        ${RTFW_PROJECT_SOURCE_DIR}/include
        ${RTFW_PROJECT_SOURCE_DIR}/tests
        ${RTFW_PROJECT_SOURCE_DIR}
    )
    target_compile_definitions(
        simcore_tests
        PRIVATE PROJECT_SOURCE_DIR="${RTFW_PROJECT_SOURCE_DIR}")

    if (RTFW_BUILD_EXPERIMENTAL)
        target_compile_definitions(
            simcore_tests
            PRIVATE RTFW_DEMO_PATH="$<TARGET_FILE:simcore_app>")
        add_dependencies(simcore_tests simcore_app)

        add_library(test_sample_plugin SHARED sample_plugin.c)
        target_include_directories(
            test_sample_plugin
            PRIVATE ${RTFW_PROJECT_SOURCE_DIR}/rt/include)
        set_target_properties(
            test_sample_plugin PROPERTIES OUTPUT_NAME sample_plugin)

        add_library(test_incompatible_plugin SHARED sample_plugin_bad.c)
        target_include_directories(
            test_incompatible_plugin
            PRIVATE ${RTFW_PROJECT_SOURCE_DIR}/rt/include)
        set_target_properties(
            test_incompatible_plugin PROPERTIES OUTPUT_NAME bad_plugin)

        add_dependencies(
            simcore_tests test_sample_plugin test_incompatible_plugin)
        target_compile_definitions(simcore_tests PRIVATE
            PLUGIN_PATH="$<TARGET_FILE:test_sample_plugin>"
            BAD_PLUGIN_PATH="$<TARGET_FILE:test_incompatible_plugin>")
    endif()
    rtfw_enable_project_warnings(simcore_tests)

    add_test(NAME simcore_all COMMAND simcore_tests)
    # This aggregate executes the complete suite, including the bounded
    # timing/concurrency cases that are also exposed as focused CTest targets.
    # Keep a finite guard while allowing slower Debug and contended CI hosts to
    # finish the assertions instead of terminating the process at 180 seconds.
    set_tests_properties(simcore_all PROPERTIES TIMEOUT 300)
    add_test(
        NAME m22_live_control_mailbox
        COMMAND simcore_tests
            --gtest_filter=LiveControlSdk.*:LiveControlStress.*:LiveControlMailbox.*:LiveControlActions.*:HostRuntime.*LiveControl*:MemoryPlan.*LiveControl*:Determinism.*LiveControl*:DeterminismReplay.*LiveControl*:MultipleInstances.*LiveControl*:TraceNoAlloc.*LiveControl*:PeriodicRuntime.*LiveControl*)
    set_tests_properties(m22_live_control_mailbox PROPERTIES TIMEOUT 60)
    add_test(
        NAME m22_live_control_sdk
        COMMAND simcore_tests
            --gtest_filter=LiveControlSdk.*)
    set_tests_properties(m22_live_control_sdk PROPERTIES TIMEOUT 30)
    add_test(
        NAME m22_live_control_stress
        COMMAND simcore_tests
            --gtest_filter=LiveControlStress.*)
    set_tests_properties(m22_live_control_stress PROPERTIES TIMEOUT 120)
    add_test(
        NAME m19_extension_registration
        COMMAND simcore_tests
            --gtest_filter=ExtensionAbi.*:ExtensionRegistration.*:ExtensionLifecycle.*:MemoryPlan.*Extension*:DeterminismReplay.*Extension*:TraceNoAlloc.*Extension*)
    set_tests_properties(m19_extension_registration PROPERTIES TIMEOUT 60)
    add_test(
        NAME m17_command_batch_timeline
        COMMAND simcore_tests
            --gtest_filter=CommandBatch.*:HalV2.*Batch*:DeviceRuntime.*Batch*:HeterogeneousMemory.*Batch*:ThreadPolicy.*DeviceSubmission*:CpuMemoryPolicy.*DeviceSubmission*:MemoryPlan.*Batch*:DeterminismReplay.*Batch*:TraceNoAlloc.*Batch*)
    set_tests_properties(m17_command_batch_timeline PROPERTIES TIMEOUT 60)
    add_test(
        NAME m17_hal_v2_compatibility
        COMMAND simcore_tests
            --gtest_filter=HalV2.*:DeviceRuntime.*:DeviceMock.*:CudaBackend.RuntimeGraphAcceptsCandidateBackendAtD0:MemoryPlan.*Hal*:DeterminismReplay.*Hal*:TraceNoAlloc.*Hal*)
    set_tests_properties(m17_hal_v2_compatibility PROPERTIES TIMEOUT 60)
    add_test(
        NAME m17_heterogeneous_memory_topology
        COMMAND simcore_tests
            --gtest_filter=HeterogeneousMemory.*:HalV2.*Memory*:DeviceRuntime.*Memory*:MemoryPlan.*Heterogeneous*:DeterminismReplay.*Heterogeneous*:TraceNoAlloc.*Heterogeneous*)
    set_tests_properties(
        m17_heterogeneous_memory_topology PROPERTIES TIMEOUT 60)
    add_test(
        NAME m17_cuda_graph
        COMMAND simcore_tests
            --gtest_filter=VendorHalV2.*:CudaBackend.*Graph*:CudaBackend.*HalV2*:HeterogeneousMemory.*Cuda*:MemoryPlan.*Vendor*:DeterminismReplay.*Vendor*:TraceNoAlloc.*Vendor*)
    set_tests_properties(m17_cuda_graph PROPERTIES TIMEOUT 60)
    add_test(
        NAME m17_xdma_control
        COMMAND simcore_tests
            --gtest_filter=VendorHalV2.*:MemoryPlan.*Vendor*:DeterminismReplay.*Vendor*:TraceNoAlloc.*Vendor*)
    set_tests_properties(m17_xdma_control PROPERTIES TIMEOUT 60)
    add_test(
        NAME m16_rate_timeline
        COMMAND simcore_tests
            --gtest_filter=RateTimeline.*:CompiledGraph.*Rate*:HostRuntime.*Rate*:PeriodicRuntime.*Rate*:MemoryPlan.*Rate*:DeterminismReplay.*Rate*:TraceNoAlloc.*Rate*)
    set_tests_properties(m16_rate_timeline PROPERTIES TIMEOUT 60)

    add_test(
        NAME m21_device_rate_model
        COMMAND simcore_tests
            --gtest_filter=RateDispatch.DeviceRate*:CommandBatch.DeviceRate*:RateTimeline.*Device*:CompiledGraph.*DeviceRate*:HostRuntime.*DeviceRate*:DeviceRuntime.*Rate*:VendorHalV2.*Rate*:MemoryPlan.*DeviceRate*:DeterminismReplay.*DeviceRate*:PeriodicRuntime.*DeviceRate*:TraceNoAlloc.*DeviceRate*)
    set_tests_properties(m21_device_rate_model PROPERTIES TIMEOUT 60)

    add_test(
        NAME m21_cross_rate_payload
        COMMAND simcore_tests
            --gtest_filter=CommandBatch.*CrossRatePayload*:CrossRateData.*Device*:RateDispatch.*DevicePayload*:HostRuntime.*DevicePayload*:HeterogeneousMemory.*CrossRate*:MemoryPlan.*DevicePayload*:PeriodicRuntime.*DevicePayload*:TraceNoAlloc.*DevicePayload*:MultipleInstances.*DevicePayload*:DeterminismReplay.*DevicePayload*)
    set_tests_properties(m21_cross_rate_payload PROPERTIES TIMEOUT 60)

    add_test(
        NAME m16_rate_dispatch
        COMMAND simcore_tests
            --gtest_filter=RateDispatch.*:RateTimeline.*Admission*:CrossRateData.*Active*:HostRuntime.*RateDispatch*:PeriodicRuntime.*RateDispatch*:UnifiedExecutor.*RateDispatch*:MemoryPlan.*RateDispatch*:DeterminismReplay.*RateDispatch*:TraceNoAlloc.*RateDispatch*)
    set_tests_properties(m16_rate_dispatch PROPERTIES TIMEOUT 60)
    add_test(
        NAME m16_shedding_telemetry
        COMMAND simcore_tests
            --gtest_filter=RateShedding.*:RateTelemetry.*:MemoryPlan.*RateTelemetry*:DeterminismReplay.*RateTelemetry*:Observability.*RateTelemetry*:TraceNoAlloc.*RateTelemetry*)
    set_tests_properties(m16_shedding_telemetry PROPERTIES TIMEOUT 60)

    add_test(
        NAME m16_cross_rate_data
        COMMAND simcore_tests
            --gtest_filter=CrossRateData.*:RateTimeline.*CrossRate*:CompiledGraph.*CrossRate*:HostRuntime.*CrossRate*:MemoryPlan.*CrossRate*:DeterminismReplay.*CrossRate*:TraceNoAlloc.*CrossRate*)
    set_tests_properties(m16_cross_rate_data PROPERTIES TIMEOUT 60)
    add_test(
        NAME m15_accounting_closure
        COMMAND simcore_tests
            --gtest_filter=CpuMemoryPolicy.*:MemoryPlan.ExactControlExtentLedgerRejectsMalformedInventories:MemoryPolicy.StrictClosureDoesNotDoubleCountProviderStorage)
    add_test(
        NAME m15_stack_lifecycle
        COMMAND simcore_tests
            --gtest_filter=ThreadPolicy.LiveRuntimeStackAccountingIsExactBoundedAndIsolated:ThreadPolicy.StrictRuntimeStackMismatchRollsBackAndRecovers:ThreadPolicy.FailedStartCleanupRetryClearsRetainedStackError:ThreadPolicy.BestEffortRuntimeStackFailureIsReportedWithoutCommitBlock:ThreadPolicy.StrictStackLockingCannotClaimIndependentReadback)
    add_test(
        NAME m15_cross_category_rollback
        COMMAND simcore_tests
            --gtest_filter=DeviceRuntime.DeviceStackCleanupFailureDefersRollbackAndRetriesOnOwner:MemoryPolicy.ExecutorStackCleanupRetriesWithWatchdogStillAccounted:MemoryPolicy.PartialRollbackRetriesOnlyTheUnresolvedRegion:MemoryPolicy.StartupRetainsFirstErrorWhenRollbackAlsoFails)
    set_tests_properties(
        m15_accounting_closure
        m15_stack_lifecycle
        m15_cross_category_rollback
        PROPERTIES TIMEOUT 60)

    add_executable(
        rtfw_thread_policy_rollback_test test_thread_policy.cpp)
    target_link_libraries(
        rtfw_thread_policy_rollback_test
        PRIVATE rtfw_runtime ${SIM_GTEST_LIBRARIES})
    target_include_directories(
        rtfw_thread_policy_rollback_test
        PRIVATE
            ${RTFW_PROJECT_SOURCE_DIR}/include
            ${RTFW_PROJECT_SOURCE_DIR}/tests
            ${RTFW_PROJECT_SOURCE_DIR})
    target_compile_definitions(
        rtfw_thread_policy_rollback_test
        PRIVATE RTFW_STANDALONE_ROLLBACK_TEST=1)
    rtfw_enable_project_warnings(rtfw_thread_policy_rollback_test)
    add_test(
        NAME thread_policy_rollback
        COMMAND rtfw_thread_policy_rollback_test
            --gtest_filter=ThreadPolicy.StrictFailureJoinsCreatedLanesInReverseBeforeReturning)
    add_test(
        NAME thread_policy_recovery
        COMMAND rtfw_thread_policy_rollback_test
            --gtest_filter=ThreadPolicy.StrictFailuresRollbackWithoutCallbacksAndCanRecover)
    set_tests_properties(
        thread_policy_rollback thread_policy_recovery
        PROPERTIES TIMEOUT 30)

    if (SIM_ENABLE_AVX2 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
      add_compile_options(-mavx2 -mfma)
    endif()

    if (RTFW_BUILD_EXPERIMENTAL AND UNIX AND NOT APPLE)
        find_library(LIBNUMA numa)
        find_path(NUMA_INCLUDE_DIR numa.h)
        if (LIBNUMA AND NUMA_INCLUDE_DIR)
            target_compile_definitions(simcore_tests PRIVATE SIM_USE_NUMA)
            target_link_libraries(simcore_tests PRIVATE ${LIBNUMA})
            target_include_directories(simcore_tests PRIVATE ${NUMA_INCLUDE_DIR})
        endif()
    endif()
endif()

find_program(RTFW_PYTHON_EXECUTABLE NAMES python3 python REQUIRED)
add_test(
    NAME sbom_dependencies
    COMMAND
        ${RTFW_PYTHON_EXECUTABLE}
        ${RTFW_PROJECT_SOURCE_DIR}/tools/sbom.py
        verify-dependencies
        --policy ${RTFW_PROJECT_SOURCE_DIR}/tools/sbom_expected.json)
set_tests_properties(sbom_dependencies PROPERTIES TIMEOUT 30)

# Portable M17-06 combined-protocol tests use the actual native candidate
# classes with injected driver tables and do not depend on GoogleTest,
# vendor toolkits, device nodes, or physical hardware.
add_executable(
    rtfw_cpu_gpu_fpga_cpu_sample_tests
    test_cpu_gpu_fpga_cpu_sample.cpp)
target_link_libraries(
    rtfw_cpu_gpu_fpga_cpu_sample_tests
    PRIVATE
        rtfw_runtime
        rtfw_cuda_backend
        rtfw_xdma_backend)
rtfw_enable_project_warnings(rtfw_cpu_gpu_fpga_cpu_sample_tests)
add_test(
    NAME m17_cpu_gpu_fpga_cpu_sample
    COMMAND rtfw_cpu_gpu_fpga_cpu_sample_tests)
set_tests_properties(m17_cpu_gpu_fpga_cpu_sample PROPERTIES TIMEOUT 30)

add_executable(rtfw_determinism_artifact determinism_artifact.cpp)
target_link_libraries(rtfw_determinism_artifact PRIVATE rtfw_runtime)

add_executable(test_cabi_dlopen test_cabi_dlopen.c)
target_include_directories(test_cabi_dlopen PRIVATE
    ${RTFW_PROJECT_SOURCE_DIR}
    ${RTFW_PROJECT_SOURCE_DIR}/include
    ${RTFW_PROJECT_SOURCE_DIR}/rt/include)
target_compile_definitions(test_cabi_dlopen PRIVATE RTFW_LIB_PATH="$<TARGET_FILE:rtfw>")
add_dependencies(test_cabi_dlopen rtfw_shared)
if (UNIX)
    target_link_libraries(test_cabi_dlopen PRIVATE dl)
endif()
add_test(NAME cabi_dlopen COMMAND test_cabi_dlopen)
set_tests_properties(cabi_dlopen PROPERTIES TIMEOUT 30)

add_executable(rtfw_host_adapter_tests host_adapter_tests.cpp)
target_link_libraries(
    rtfw_host_adapter_tests PRIVATE rtfw_static rtfw_runtime)
add_test(NAME host_adapter COMMAND rtfw_host_adapter_tests)
set_tests_properties(host_adapter PROPERTIES TIMEOUT 60)

add_executable(rtfw_runtime_profile_tests runtime_profile_tests.cpp)
target_link_libraries(
    rtfw_runtime_profile_tests PRIVATE rtfw_runtime)
add_test(NAME runtime_profile COMMAND rtfw_runtime_profile_tests)
set_tests_properties(runtime_profile PROPERTIES TIMEOUT 30)

if (RTFW_BUILD_EXPERIMENTAL)
    add_executable(
        simcore_thread_violation EXCLUDE_FROM_ALL thread_violation.cpp)
    target_link_libraries(
        simcore_thread_violation PRIVATE simcore_platform)
    target_compile_definitions(
        simcore_thread_violation PRIVATE RTFW_ENFORCE_NO_RAW_THREADS=1)
    add_test(
        NAME simcore_thread_violation_build
        COMMAND
            ${CMAKE_COMMAND} --build ${RTFW_PROJECT_BINARY_DIR}
            --target simcore_thread_violation --config $<CONFIG>)
    set_tests_properties(simcore_thread_violation_build PROPERTIES
        TIMEOUT 120
        WILL_FAIL TRUE)
endif()

if (SIM_BUILD_FUZZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if (RTFW_BUILD_EXPERIMENTAL)
        add_executable(jobqueue_fuzz jobqueue_fuzz.cpp)
        target_link_libraries(jobqueue_fuzz PRIVATE simcore_platform)
        target_compile_options(
            jobqueue_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
        target_link_options(
            jobqueue_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()

    # Compile the codec into the fuzz binary so ASan/UBSan instrument the
    # parser itself rather than only the libFuzzer entry point.
    add_executable(snapshot_fuzz
        snapshot_fuzz.cpp
        ${RTFW_PROJECT_SOURCE_DIR}/rt/src/snapshot_codec.cpp)
    target_include_directories(
        snapshot_fuzz
        PRIVATE
            ${RTFW_PROJECT_SOURCE_DIR}/rt/include
            ${RTFW_PROJECT_SOURCE_DIR}/core/include
            ${RTFW_PROJECT_SOURCE_DIR}/include)
    target_compile_options(snapshot_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(snapshot_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(runtime_profile_fuzz runtime_profile_fuzz.cpp)
    target_link_libraries(runtime_profile_fuzz PRIVATE rtfw_runtime)
    target_compile_options(
        runtime_profile_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(
        runtime_profile_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(
        live_control_replay_fuzz fuzz_live_control_replay.cpp)
    target_link_libraries(live_control_replay_fuzz PRIVATE rtfw_runtime)
    target_compile_options(
        live_control_replay_fuzz
        PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(
        live_control_replay_fuzz
        PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

# Portable M10 XDMA state-machine tests do not require GoogleTest, vendor
# headers, a kernel driver, or hardware.
add_executable(rtfw_xdma_backend_tests xdma_backend_tests.cpp)
target_link_libraries(
    rtfw_xdma_backend_tests PRIVATE rtfw_xdma_backend)
if (TARGET rtfw_xdma_linux)
    target_link_libraries(
        rtfw_xdma_backend_tests PRIVATE rtfw_xdma_linux)
endif()
add_test(NAME xdma_backend COMMAND rtfw_xdma_backend_tests)
set_tests_properties(xdma_backend PROPERTIES TIMEOUT 60)
