cmake_minimum_required(VERSION 3.20)
project(rtfw_package_consumer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(rtfw_requested_components
    c_shared
    c_static
    runtime
    cpp_runtime
    cuda_backend
    xdma_backend)
if (RTFW_TEST_CUDA_DRIVER)
    list(APPEND rtfw_requested_components cuda_driver)
endif()
if (RTFW_TEST_XDMA_LINUX)
    list(APPEND rtfw_requested_components xdma_linux)
endif()

if (RTFW_TEST_BENCHMARK)
    list(APPEND rtfw_requested_components benchmark)
endif()
find_package(
    rtfw 1.2 CONFIG REQUIRED
    COMPONENTS ${rtfw_requested_components})

include("${CMAKE_CURRENT_LIST_DIR}/package_contract.cmake")

if (NOT RTFW_C_ABI_VERSION EQUAL 8)
    message(FATAL_ERROR "Expected installed RTFW C ABI version 8")
endif()

add_executable(rtfw_consumer_shared c_consumer.c)
target_link_libraries(rtfw_consumer_shared PRIVATE rtfw::c_shared)

add_executable(rtfw_consumer_static c_consumer.c)
target_link_libraries(rtfw_consumer_static PRIVATE rtfw::c_static)

add_executable(rtfw_consumer_shared_compat c_consumer.c)
target_link_libraries(rtfw_consumer_shared_compat PRIVATE rtfw::rtfw)

add_executable(rtfw_consumer_static_compat c_consumer.c)
target_link_libraries(
    rtfw_consumer_static_compat PRIVATE rtfw::rtfw_static)

add_executable(rtfw_consumer_cpp cpp_consumer.cpp)
target_link_libraries(
    rtfw_consumer_cpp
    PRIVATE
        rtfw::runtime)

add_executable(rtfw_consumer_loopback loopback_consumer.cpp)
target_link_libraries(rtfw_consumer_loopback PRIVATE rtfw::runtime)

add_executable(rtfw_consumer_mixed_rate mixed_rate_consumer.cpp)
target_link_libraries(rtfw_consumer_mixed_rate PRIVATE rtfw::runtime)

add_executable(rtfw_consumer_live_control live_control_consumer.cpp)
target_link_libraries(rtfw_consumer_live_control PRIVATE rtfw::runtime)

add_executable(
    rtfw_consumer_live_control_sdk
    live_control_sdk_consumer.cpp)
target_link_libraries(
    rtfw_consumer_live_control_sdk PRIVATE rtfw::runtime)

add_executable(rtfw_consumer_extension_c extension_consumer.c)
target_link_libraries(
    rtfw_consumer_extension_c PRIVATE rtfw::runtime)

add_executable(
    rtfw_consumer_extension_cpp extension_cpp_consumer.cpp)
target_link_libraries(
    rtfw_consumer_extension_cpp PRIVATE rtfw::runtime)

add_executable(rtfw_consumer_cpp_compat compat_consumer.cpp)
target_link_libraries(
    rtfw_consumer_cpp_compat
    PRIVATE
        rtfw::simcore_rt)

add_executable(rtfw_consumer_profile profile_consumer.cpp)
target_link_libraries(
    rtfw_consumer_profile
    PRIVATE
        rtfw::runtime)

add_executable(rtfw_consumer_cuda cuda_consumer.cpp)
target_link_libraries(
    rtfw_consumer_cuda PRIVATE rtfw::cuda_backend)

add_executable(rtfw_consumer_xdma xdma_consumer.cpp)
target_link_libraries(
    rtfw_consumer_xdma PRIVATE rtfw::xdma_backend)

if (RTFW_TEST_CUDA_DRIVER)
    add_executable(
        rtfw_consumer_cuda_driver
        cuda_driver_consumer.cpp)
    target_link_libraries(
        rtfw_consumer_cuda_driver
        PRIVATE rtfw::cuda_driver)
endif()

if (RTFW_TEST_XDMA_LINUX)
    add_executable(
        rtfw_consumer_xdma_linux
        xdma_linux_consumer.cpp)
    target_link_libraries(
        rtfw_consumer_xdma_linux
        PRIVATE rtfw::xdma_linux)
endif()

add_executable(rtfw_consumer_policy warning_consumer.cpp)
target_link_libraries(
    rtfw_consumer_policy
    PRIVATE
        rtfw::runtime
        rtfw::c_static)

if (WIN32)
    add_custom_command(
        TARGET rtfw_consumer_shared
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rtfw::rtfw>
            $<TARGET_FILE_DIR:rtfw_consumer_shared>
        VERBATIM)
    add_custom_command(
        TARGET rtfw_consumer_shared_compat
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rtfw::rtfw>
            $<TARGET_FILE_DIR:rtfw_consumer_shared_compat>
        VERBATIM)
endif()

enable_testing()
add_test(NAME installed_c_shared COMMAND rtfw_consumer_shared)
add_test(NAME installed_c_static COMMAND rtfw_consumer_static)
add_test(NAME installed_c_shared_compat COMMAND rtfw_consumer_shared_compat)
add_test(NAME installed_c_static_compat COMMAND rtfw_consumer_static_compat)
add_test(NAME installed_cpp_runtime COMMAND rtfw_consumer_cpp)
add_test(NAME installed_loopback COMMAND rtfw_consumer_loopback)
add_test(NAME installed_mixed_rate COMMAND rtfw_consumer_mixed_rate)
add_test(NAME installed_live_control COMMAND rtfw_consumer_live_control)
add_test(
    NAME installed_live_control_sdk
    COMMAND rtfw_consumer_live_control_sdk)
add_test(NAME installed_extension_c COMMAND rtfw_consumer_extension_c)
add_test(NAME installed_extension_cpp COMMAND rtfw_consumer_extension_cpp)
add_test(NAME installed_cpp_compat COMMAND rtfw_consumer_cpp_compat)
add_test(NAME installed_cpp_profile COMMAND rtfw_consumer_profile)
add_test(NAME installed_cuda_backend COMMAND rtfw_consumer_cuda)
add_test(NAME installed_xdma_backend COMMAND rtfw_consumer_xdma)
add_test(NAME installed_policy_isolation COMMAND rtfw_consumer_policy)
if (RTFW_TEST_CUDA_DRIVER)
    add_test(
        NAME installed_cuda_driver
        COMMAND rtfw_consumer_cuda_driver)
endif()
if (RTFW_TEST_XDMA_LINUX)
    add_test(
        NAME installed_xdma_linux
        COMMAND rtfw_consumer_xdma_linux)
endif()

if (UNIX)
    add_test(
        NAME installed_pure_c_project
        COMMAND
            ${CMAKE_COMMAND}
            "-DRTFW_PURE_C_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/pure_c"
            "-DRTFW_PURE_C_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/pure_c"
            "-DRTFW_PURE_C_PREFIX=${RTFW_PREFIX}"
            "-DRTFW_PURE_C_CONFIG=$<CONFIG>"
            -P "${CMAKE_CURRENT_LIST_DIR}/run_pure_c.cmake")
    set_tests_properties(installed_pure_c_project PROPERTIES TIMEOUT 120)
endif()

if (RTFW_TEST_BENCHMARK)
    add_executable(rtfw_consumer_benchmark benchmark_consumer.cpp)
    target_link_libraries(rtfw_consumer_benchmark PRIVATE rtfw::benchmark)
    add_test(NAME benchmark_package_consumer COMMAND rtfw_consumer_benchmark)
    add_executable(rtfw_consumer_benchmark_cpu benchmark_cpu_consumer.cpp
        "${RTFW_DATA_DIR}/bench/examples/cpu_provider.cpp")
    target_include_directories(rtfw_consumer_benchmark_cpu PRIVATE "${RTFW_DATA_DIR}/bench/examples")
    target_link_libraries(rtfw_consumer_benchmark_cpu PRIVATE rtfw::runtime rtfw::benchmark)
    add_test(NAME benchmark_cpu_package_consumer COMMAND rtfw_consumer_benchmark_cpu)
    file(GLOB runtime_case_sources "${RTFW_DATA_DIR}/bench/examples/runtime_cases/*.cpp")
    add_executable(rtfw_consumer_benchmark_runtime benchmark_runtime_consumer.cpp
        "${RTFW_DATA_DIR}/bench/examples/runtime_provider.cpp" ${runtime_case_sources})
    target_include_directories(rtfw_consumer_benchmark_runtime PRIVATE "${RTFW_DATA_DIR}/bench/examples")
    target_link_libraries(rtfw_consumer_benchmark_runtime PRIVATE rtfw::runtime rtfw::benchmark)
    add_test(NAME benchmark_runtime_package_consumer COMMAND rtfw_consumer_benchmark_runtime)
endif()
