function(rtfw_assert_target target)
    if (NOT TARGET "${target}")
        message(FATAL_ERROR "Installed package is missing target ${target}")
    endif()
endfunction()

foreach(target
        rtfw::c_shared
        rtfw::c_static
        rtfw::runtime
        rtfw::rtfw
        rtfw::rtfw_static
        rtfw::simcore_rt
        rtfw::cuda_backend
        rtfw::xdma_backend)
    rtfw_assert_target("${target}")
endforeach()

foreach(target
        rtfw::experimental
        rtfw::simcore
        rtfw::simcore_core
        rtfw::simcore_platform)
    if (TARGET "${target}")
        message(FATAL_ERROR
            "Default package unexpectedly exports ${target}")
    endif()
endforeach()

if (RTFW_WITH_EXPERIMENTAL)
    message(FATAL_ERROR "Default package unexpectedly installs experiments")
endif()

get_target_property(
    rtfw_runtime_features
    rtfw::runtime
    INTERFACE_COMPILE_FEATURES)
if (NOT "cxx_std_20" IN_LIST rtfw_runtime_features)
    message(FATAL_ERROR "rtfw::runtime does not propagate C++20")
endif()

set(rtfw_supported_targets
    rtfw::c_shared
    rtfw::c_static
    rtfw::runtime
    rtfw::rtfw
    rtfw::rtfw_static
    rtfw::simcore_rt
    rtfw::cuda_backend
    rtfw::xdma_backend)
if (RTFW_TEST_CUDA_DRIVER)
    list(APPEND rtfw_supported_targets rtfw::cuda_driver)
endif()
if (RTFW_TEST_XDMA_LINUX)
    list(APPEND rtfw_supported_targets rtfw::xdma_linux)
endif()
if (RTFW_TEST_BENCHMARK)
    rtfw_assert_target(rtfw::benchmark)
    list(APPEND rtfw_supported_targets rtfw::benchmark)
    get_target_property(benchmark_links rtfw::benchmark INTERFACE_LINK_LIBRARIES)
    if (benchmark_links)
        message(FATAL_ERROR "Standalone benchmark target has unexpected dependencies: ${benchmark_links}")
    endif()
    if (TARGET rtfw::benchmark_cpu OR TARGET rtfw_benchmark_cpu OR TARGET rtfw::benchmark_runtime OR TARGET rtfw_benchmark_runtime)
        message(FATAL_ERROR "CPU example leaked as an installed target")
    endif()
    set(expected_benchmark_examples
        benchmark_consumer.cpp benchmark_cpu_consumer.cpp
        cpu_provider.hpp cpu_provider.cpp cpu_cases.json
        benchmark_runtime_consumer.cpp
        runtime_provider.hpp
        runtime_provider.cpp
        runtime_cases.json
        runtime_cases/capacity.cpp
        runtime_cases/catalog.inc
        runtime_cases/checkpoint.cpp
        runtime_cases/control_support.hpp
        runtime_cases/controls.cpp
        runtime_cases/device.cpp
        runtime_cases/observability.cpp
        runtime_cases/rates.cpp
        runtime_cases/replay.cpp
        runtime_cases/support.hpp)
    file(GLOB_RECURSE actual_benchmark_examples LIST_DIRECTORIES FALSE RELATIVE "${RTFW_DATA_DIR}/bench/examples"
        "${RTFW_DATA_DIR}/bench/examples/*")
    list(SORT expected_benchmark_examples)
    list(SORT actual_benchmark_examples)
    if (NOT actual_benchmark_examples STREQUAL expected_benchmark_examples)
        message(FATAL_ERROR "Optional benchmark source-example inventory differs from contract")
    endif()
elseif (TARGET rtfw::benchmark)
    message(FATAL_ERROR "Unrequested benchmark component leaked into default imports")
endif()
set(rtfw_forbidden_usage
    "-Wall"
    "-Wextra"
    "-Wpedantic"
    "-Wconversion"
    "-Wshadow"
    "-Wnon-virtual-dtor"
    "-Werror"
    "/W4"
    "/WX"
    "LOG_ENABLED"
    "LOG_DEFAULT_LEVEL"
    "PROF_ENABLED"
    "SIM_WERROR"
    "SIM_ENABLE")
foreach(target IN LISTS rtfw_supported_targets)
    get_target_property(options "${target}" INTERFACE_COMPILE_OPTIONS)
    get_target_property(definitions "${target}" INTERFACE_COMPILE_DEFINITIONS)
    set(usage "${options};${definitions}")
    foreach(forbidden IN LISTS rtfw_forbidden_usage)
        string(FIND "${usage}" "${forbidden}" position)
        if (NOT position EQUAL -1)
            message(FATAL_ERROR
                "${target} leaks forbidden consumer policy ${forbidden}: "
                "${usage}")
        endif()
    endforeach()
endforeach()

foreach(target rtfw::runtime rtfw::rtfw_static rtfw::simcore_rt)
    get_target_property(links "${target}" INTERFACE_LINK_LIBRARIES)
    foreach(forbidden "simcore_platform" "rtfw::experimental" "rtfw::simcore" "dl")
        string(FIND "${links}" "${forbidden}" position)
        if (NOT position EQUAL -1)
            message(FATAL_ERROR
                "${target} leaks forbidden dependency ${forbidden}: ${links}")
        endif()
    endforeach()
endforeach()

set(expected_headers
    core/units.hpp
    rt/c_api.h
    rt/canonical_bytes.hpp
    rt/config.hpp
    rt/cuda_backend.hpp
    rt/device.hpp
    rt/device_abi.h
    rt/extension_abi.h
    rt/graph.hpp
    rt/loopback_backend.hpp
    rt/live_control.hpp
    rt/mock_device.hpp
    rt/observability_export.hpp
    rt/profile.hpp
    rt/runtime.hpp
    rt/status.hpp
    rt/version.hpp
    rt/xdma_backend.hpp
    rtfw/version.h)
if (RTFW_WITH_CUDA_DRIVER)
    list(APPEND expected_headers rt/cuda_driver.hpp)
endif()
if (RTFW_WITH_XDMA_LINUX)
    list(APPEND expected_headers rt/xdma_linux.hpp)
endif()
if (RTFW_WITH_BENCHMARK)
    list(APPEND expected_headers rtfw/benchmark.hpp)
endif()
list(SORT expected_headers)

file(
    GLOB_RECURSE installed_headers
    RELATIVE "${RTFW_INCLUDE_DIR}"
    "${RTFW_INCLUDE_DIR}/*.h"
    "${RTFW_INCLUDE_DIR}/*.hh"
    "${RTFW_INCLUDE_DIR}/*.hpp"
    "${RTFW_INCLUDE_DIR}/*.hxx"
    "${RTFW_INCLUDE_DIR}/*.inl"
    "${RTFW_INCLUDE_DIR}/*.ipp"
    "${RTFW_INCLUDE_DIR}/*.tpp")
list(SORT installed_headers)
if (NOT installed_headers STREQUAL expected_headers)
    message(FATAL_ERROR
        "Default installed header inventory differs from the SDK contract.\n"
        "Expected: ${expected_headers}\n"
        "Actual:   ${installed_headers}")
endif()

set(rtfw_license "${RTFW_DATA_DIR}/LICENSE")
if (NOT EXISTS "${rtfw_license}")
    message(FATAL_ERROR "Installed Apache-2.0 license is missing")
endif()
file(SHA256 "${rtfw_license}" installed_license_sha256)
if (NOT installed_license_sha256 STREQUAL
    "c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4")
    message(FATAL_ERROR "Installed Apache-2.0 license digest changed")
endif()

function(rtfw_check_c_header header id target)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header_${id}.c")
    file(WRITE "${source}"
        "#include <${header}>\nint rtfw_header_${id}(void) { return 0; }\n")
    add_library("rtfw_header_${id}" OBJECT "${source}")
    target_link_libraries("rtfw_header_${id}" PRIVATE "${target}")
endfunction()

function(rtfw_check_cpp_header header id target)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header_${id}.cpp")
    file(WRITE "${source}"
        "#include <${header}>\nint rtfw_header_${id}() { return 0; }\n")
    add_library("rtfw_header_${id}" OBJECT "${source}")
    target_link_libraries("rtfw_header_${id}" PRIVATE "${target}")
endfunction()

rtfw_check_c_header("rt/c_api.h" c_api rtfw::c_shared)
rtfw_check_c_header("rt/device_abi.h" device_abi rtfw::c_shared)
rtfw_check_c_header("rt/extension_abi.h" extension_abi_c rtfw::runtime)
rtfw_check_c_header("rtfw/version.h" version_c rtfw::c_shared)

rtfw_check_cpp_header("core/units.hpp" units rtfw::runtime)
rtfw_check_cpp_header("rt/canonical_bytes.hpp" canonical_bytes rtfw::runtime)
rtfw_check_cpp_header("rt/config.hpp" config rtfw::runtime)
rtfw_check_cpp_header("rt/device.hpp" device rtfw::runtime)
rtfw_check_cpp_header("rt/extension_abi.h" extension_abi_cpp rtfw::runtime)
rtfw_check_cpp_header("rt/graph.hpp" graph rtfw::runtime)
rtfw_check_cpp_header(
    "rt/loopback_backend.hpp" loopback_backend rtfw::runtime)
rtfw_check_cpp_header("rt/live_control.hpp" live_control rtfw::runtime)
rtfw_check_cpp_header("rt/mock_device.hpp" mock_device rtfw::runtime)
rtfw_check_cpp_header(
    "rt/observability_export.hpp" observability_export rtfw::runtime)
rtfw_check_cpp_header("rt/profile.hpp" profile rtfw::runtime)
rtfw_check_cpp_header("rt/runtime.hpp" runtime rtfw::runtime)
rtfw_check_cpp_header("rt/status.hpp" status rtfw::runtime)
rtfw_check_cpp_header("rt/version.hpp" version_cpp rtfw::runtime)
rtfw_check_cpp_header(
    "rt/cuda_backend.hpp" cuda_backend rtfw::cuda_backend)
rtfw_check_cpp_header(
    "rt/xdma_backend.hpp" xdma_backend rtfw::xdma_backend)
if (RTFW_TEST_CUDA_DRIVER)
    rtfw_assert_target(rtfw::cuda_driver)
    rtfw_check_cpp_header(
        "rt/cuda_driver.hpp" cuda_driver rtfw::cuda_driver)
endif()
if (RTFW_TEST_XDMA_LINUX)
    rtfw_assert_target(rtfw::xdma_linux)
    rtfw_check_cpp_header(
        "rt/xdma_linux.hpp" xdma_linux rtfw::xdma_linux)
endif()

if (RTFW_TEST_BENCHMARK)
    rtfw_check_cpp_header("rtfw/benchmark.hpp" benchmark rtfw::benchmark)
endif()
