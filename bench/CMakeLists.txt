# M23 is an optional host-control library, never a Runtime dependency.
add_library(rtfw_benchmark STATIC src/benchmark.cpp)
add_library(rtfw::benchmark ALIAS rtfw_benchmark)
set_target_properties(rtfw_benchmark PROPERTIES EXPORT_NAME benchmark)
target_compile_features(rtfw_benchmark PUBLIC cxx_std_20)
target_include_directories(rtfw_benchmark PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
rtfw_enable_project_warnings(rtfw_benchmark)

# Hash build flags rather than disclosing paths, command lines, or environment.
set(_bench_commit unknown)
set(_bench_tree unknown)
set(_bench_dirty unknown)
find_package(Git QUIET)
if (GIT_FOUND AND EXISTS "${RTFW_PROJECT_SOURCE_DIR}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${RTFW_PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE _bench_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _bench_git_result ERROR_QUIET)
    if (_bench_git_result EQUAL 0)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse "HEAD^{tree}"
            WORKING_DIRECTORY "${RTFW_PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE _bench_tree
            OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _bench_tree_result ERROR_QUIET)
        if (NOT _bench_tree_result EQUAL 0)
            set(_bench_tree unknown)
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=normal
            WORKING_DIRECTORY "${RTFW_PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE _bench_status RESULT_VARIABLE _bench_status_result ERROR_QUIET)
        if (NOT _bench_status_result EQUAL 0)
            set(_bench_dirty unknown)
        elseif (_bench_status STREQUAL "")
            set(_bench_dirty false)
        else()
            set(_bench_dirty true)
        endif()
    else()
        set(_bench_commit unknown)
    endif()
endif()
get_directory_property(_bench_directory_options COMPILE_OPTIONS)
get_directory_property(_bench_directory_definitions COMPILE_DEFINITIONS)
get_directory_property(_bench_directory_links LINK_OPTIONS)
get_target_property(_bench_target_options rtfw_benchmark COMPILE_OPTIONS)
get_target_property(_bench_target_definitions rtfw_benchmark COMPILE_DEFINITIONS)
get_target_property(_bench_target_links rtfw_benchmark LINK_OPTIONS)
string(SHA256 _bench_flags "${CMAKE_CXX_FLAGS}|${CMAKE_CXX_FLAGS_DEBUG}|${CMAKE_CXX_FLAGS_RELEASE}|${CMAKE_CXX_FLAGS_RELWITHDEBINFO}|${CMAKE_CXX_FLAGS_MINSIZEREL}|${CMAKE_EXE_LINKER_FLAGS}|${CMAKE_STATIC_LINKER_FLAGS}|${_bench_directory_options}|${_bench_directory_definitions}|${_bench_directory_links}|${_bench_target_options}|${_bench_target_definitions}|${_bench_target_links}|${CMAKE_POSITION_INDEPENDENT_CODE}|cxx_std_20")
# A parent may alter target options after add_subdirectory returns. Do not claim
# that a configure-time digest captures those later parent-owned mutations.
if (NOT RTFW_TOP_LEVEL)
    set(_bench_flags unknown)
endif()
target_compile_definitions(rtfw_benchmark PRIVATE
    RTFW_BENCH_SOURCE_COMMIT="${_bench_commit}"
    RTFW_BENCH_SOURCE_TREE="${_bench_tree}"
    RTFW_BENCH_SOURCE_DIRTY="${_bench_dirty}"
    RTFW_BENCH_COMPILER="${CMAKE_CXX_COMPILER_ID}"
    RTFW_BENCH_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
    RTFW_BENCH_BUILD_CONFIG="$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,unknown>"
    RTFW_BENCH_BUILD_FLAGS="${_bench_flags}")
add_executable(rtfw-bench src/benchmark_cli.cpp)
add_library(rtfw_benchmark_cpu STATIC providers/cpu_provider.cpp)
target_link_libraries(rtfw_benchmark_cpu PUBLIC rtfw_benchmark rtfw_runtime)
target_include_directories(rtfw_benchmark_cpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/providers)
rtfw_enable_project_warnings(rtfw_benchmark_cpu)
add_library(rtfw_benchmark_runtime STATIC
    providers/runtime_provider.cpp
    providers/runtime_cases/rates.cpp
    providers/runtime_cases/controls.cpp
    providers/runtime_cases/checkpoint.cpp
    providers/runtime_cases/replay.cpp
    providers/runtime_cases/observability.cpp
    providers/runtime_cases/capacity.cpp
    providers/runtime_cases/device.cpp)
target_link_libraries(rtfw_benchmark_runtime PUBLIC rtfw_benchmark rtfw_runtime)
target_include_directories(rtfw_benchmark_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/providers)
rtfw_enable_project_warnings(rtfw_benchmark_runtime)
target_link_libraries(rtfw-bench PRIVATE rtfw_benchmark_cpu rtfw_benchmark_runtime)
rtfw_enable_project_warnings(rtfw-bench)
install(TARGETS rtfw_benchmark EXPORT rtfwBenchmarkTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS rtfw-bench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(EXPORT rtfwBenchmarkTargets NAMESPACE rtfw::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rtfw)
install(FILES include/rtfw/benchmark.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rtfw)
install(FILES schemas/descriptor.schema.json schemas/result.schema.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/bench/schemas)
install(FILES "${RTFW_PROJECT_SOURCE_DIR}/tools/check_benchmark_artifact.py"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/tools)
install(FILES "${RTFW_PROJECT_SOURCE_DIR}/docs/benchmarking.md"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/docs)
install(FILES "${RTFW_PROJECT_SOURCE_DIR}/tests/package_consumer/benchmark_consumer.cpp"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/bench/examples)
install(FILES providers/cpu_provider.hpp providers/cpu_provider.cpp fixtures/cpu_cases.json
    "${RTFW_PROJECT_SOURCE_DIR}/tests/package_consumer/benchmark_cpu_consumer.cpp"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/bench/examples)
install(FILES providers/runtime_provider.hpp providers/runtime_provider.cpp fixtures/runtime_cases.json
    "${RTFW_PROJECT_SOURCE_DIR}/tests/package_consumer/benchmark_runtime_consumer.cpp"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/bench/examples)
install(DIRECTORY providers/runtime_cases
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/bench/examples)

if (RTFW_BUILD_TESTS)
    add_executable(rtfw_benchmark_runtime_consumer "${RTFW_PROJECT_SOURCE_DIR}/tests/package_consumer/benchmark_runtime_consumer.cpp")
    target_link_libraries(rtfw_benchmark_runtime_consumer PRIVATE rtfw_benchmark_runtime)
    rtfw_enable_project_warnings(rtfw_benchmark_runtime_consumer)
    add_test(NAME benchmark_runtime_package_consumer COMMAND rtfw_benchmark_runtime_consumer)
    add_executable(rtfw_benchmark_runtime_noalloc "${RTFW_PROJECT_SOURCE_DIR}/tests/benchmark_fixtures/runtime_consumer/noalloc.cpp")
    target_link_libraries(rtfw_benchmark_runtime_noalloc PRIVATE rtfw_benchmark_runtime)
    rtfw_enable_project_warnings(rtfw_benchmark_runtime_noalloc)
    add_test(NAME m23_benchmark_runtime_noalloc COMMAND rtfw_benchmark_runtime_noalloc)
    add_executable(rtfw_benchmark_cpu_consumer "${RTFW_PROJECT_SOURCE_DIR}/tests/package_consumer/benchmark_cpu_consumer.cpp")
    target_link_libraries(rtfw_benchmark_cpu_consumer PRIVATE rtfw_benchmark_cpu)
    rtfw_enable_project_warnings(rtfw_benchmark_cpu_consumer)
    add_test(NAME benchmark_cpu_package_consumer COMMAND rtfw_benchmark_cpu_consumer)
    add_executable(rtfw_benchmark_cpu_noalloc "${RTFW_PROJECT_SOURCE_DIR}/tests/benchmark_fixtures/cpu_consumer/noalloc.cpp")
    target_link_libraries(rtfw_benchmark_cpu_noalloc PRIVATE rtfw_benchmark_cpu)
    rtfw_enable_project_warnings(rtfw_benchmark_cpu_noalloc)
    add_test(NAME m23_benchmark_cpu_noalloc COMMAND rtfw_benchmark_cpu_noalloc)
    add_executable(rtfw_benchmark_consumer "${RTFW_PROJECT_SOURCE_DIR}/tests/package_consumer/benchmark_consumer.cpp")
    target_link_libraries(rtfw_benchmark_consumer PRIVATE rtfw_benchmark)
    rtfw_enable_project_warnings(rtfw_benchmark_consumer)
    add_test(NAME benchmark_package_consumer COMMAND rtfw_benchmark_consumer)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME m23_benchmark_runtime_inventory
        COMMAND "${Python3_EXECUTABLE}" "${RTFW_PROJECT_SOURCE_DIR}/tests/benchmark_fixtures/test_runtime_cases.py"
            --cli $<TARGET_FILE:rtfw-bench> --inventory "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/runtime_cases.json")
    set_tests_properties(m23_benchmark_runtime_inventory m23_benchmark_runtime_noalloc PROPERTIES TIMEOUT 600)
    add_test(NAME m23_benchmark_cpu_inventory
        COMMAND "${Python3_EXECUTABLE}" "${RTFW_PROJECT_SOURCE_DIR}/tests/benchmark_fixtures/test_cpu_cases.py"
            --cli $<TARGET_FILE:rtfw-bench> --inventory "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cpu_cases.json")
    set_tests_properties(m23_benchmark_cpu_inventory m23_benchmark_cpu_noalloc PROPERTIES TIMEOUT 120)
    add_test(NAME m23_benchmark_artifact_validator
        COMMAND "${Python3_EXECUTABLE}" "${RTFW_PROJECT_SOURCE_DIR}/tests/benchmark_fixtures/test_artifacts.py"
            --cli $<TARGET_FILE:rtfw-bench> --consumer $<TARGET_FILE:rtfw_benchmark_consumer>)
    set_tests_properties(m23_benchmark_artifact_validator PROPERTIES TIMEOUT 120)
endif()

# Keep the legacy microbenchmark experimental and unchanged.
if (RTFW_BUILD_EXPERIMENTAL)
    add_executable(bench_aosoa_vs_soa bench_aosoa_vs_soa.cpp)
    target_link_libraries(bench_aosoa_vs_soa PRIVATE simcore_platform)
endif()
