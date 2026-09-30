add_executable(sample_cuda_pipeline cuda_physics/pipeline/main.cpp)
target_link_libraries(sample_cuda_pipeline PRIVATE rtfw::runtime rtfw::cuda_backend)
rtfw_enable_project_warnings(sample_cuda_pipeline)
set(RTFW_SAMPLE_TEST_TARGETS sample_cuda_pipeline)

add_executable(sample_cuda_physics cuda_physics/main.cpp)
target_link_libraries(sample_cuda_physics PRIVATE rtfw::runtime rtfw::cuda_backend)
rtfw_enable_project_warnings(sample_cuda_physics)
list(APPEND RTFW_SAMPLE_TEST_TARGETS sample_cuda_physics)

if (RTFW_BUILD_EXPERIMENTAL)
    add_executable(sample_particles particles.cpp)
    target_link_libraries(sample_particles PRIVATE simcore_platform)

    add_executable(sample_tyre_belt tyre_belt_kernel.cpp)
    target_link_libraries(sample_tyre_belt PRIVATE simcore_platform)

    add_executable(sample_async_gpu async_gpu_stub.cpp)
    target_link_libraries(sample_async_gpu PRIVATE simcore_platform)

    list(APPEND RTFW_SAMPLE_TEST_TARGETS
        sample_particles
        sample_tyre_belt
        sample_async_gpu)
endif()

add_executable(sample_embed_c embed_c/mini_app.c)
target_link_libraries(sample_embed_c PRIVATE rtfw)
if (WIN32)
    add_custom_command(
        TARGET sample_embed_c
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rtfw_shared>
            $<TARGET_FILE_DIR:sample_embed_c>
        COMMENT "Staging the RTFW DLL beside the shared C embedding sample"
        VERBATIM)
endif()

add_executable(sample_embed_c_static embed_c/mini_app.c)
target_link_libraries(sample_embed_c_static PRIVATE rtfw_static)

add_executable(sample_embed_cpp embed_cpp/mini_app.cpp)
target_link_libraries(sample_embed_cpp PRIVATE rtfw_runtime)

add_executable(sample_device_mock device_mock.cpp)
target_link_libraries(sample_device_mock PRIVATE rtfw_runtime)

add_executable(sample_cpu_gpu_fpga_cpu cpu_gpu_fpga_cpu.cpp)
target_link_libraries(
    sample_cpu_gpu_fpga_cpu
    PRIVATE
        rtfw_runtime
        rtfw_cuda_backend
        rtfw_xdma_backend)
rtfw_enable_project_warnings(sample_cpu_gpu_fpga_cpu)

add_executable(sample_live_control_typed live_control_typed.cpp)
target_link_libraries(sample_live_control_typed PRIVATE rtfw_runtime)
rtfw_enable_project_warnings(sample_live_control_typed)

list(APPEND RTFW_SAMPLE_TEST_TARGETS
    sample_embed_c
    sample_embed_c_static
    sample_embed_cpp
    sample_device_mock
    sample_live_control_typed
    sample_cpu_gpu_fpga_cpu)

if (RTFW_ENABLE_CUDA)
    include(cuda_physics/real.cmake)
    rtfw_add_physics_real(sample_cuda_pipeline_real "${CMAKE_CURRENT_SOURCE_DIR}/cuda_physics"
        "${CMAKE_CURRENT_SOURCE_DIR}/cuda_physics/pipeline/real.cpp")
    rtfw_enable_project_warnings(sample_cuda_pipeline_real)
    rtfw_add_physics_real(sample_cuda_physics_real "${CMAKE_CURRENT_SOURCE_DIR}/cuda_physics")
    rtfw_enable_project_warnings(sample_cuda_physics_real)
    add_executable(sample_cuda_qualification cuda_qualification.cpp)
    target_link_libraries(
        sample_cuda_qualification
        PRIVATE rtfw_cuda_driver)
endif()

if (RTFW_ENABLE_XDMA)
    add_executable(sample_xdma_qualification xdma_qualification.cpp)
    target_link_libraries(
        sample_xdma_qualification
        PRIVATE rtfw_xdma_linux)
endif()

if (RTFW_BUILD_TESTS)
    foreach(sample_target IN LISTS RTFW_SAMPLE_TEST_TARGETS)
        add_test(NAME ${sample_target} COMMAND ${sample_target})
    endforeach()
    set_tests_properties(${RTFW_SAMPLE_TEST_TARGETS} PROPERTIES TIMEOUT 30)
endif()

add_executable(sample_cuda_lifetime cuda_physics/lifetime/main.cpp)
target_link_libraries(sample_cuda_lifetime PRIVATE rtfw::runtime rtfw::cuda_backend)
rtfw_enable_project_warnings(sample_cuda_lifetime)
if(RTFW_BUILD_TESTS)
    add_test(NAME sample_cuda_lifetime COMMAND sample_cuda_lifetime)
    set_tests_properties(sample_cuda_lifetime PROPERTIES TIMEOUT 120)
endif()

# Canonical first-use graph; the same source is shipped as a standalone consumer.
add_executable(sample_hello_runtime hello_runtime/main.cpp)
target_link_libraries(sample_hello_runtime PRIVATE rtfw::runtime)
rtfw_enable_project_warnings(sample_hello_runtime)
if(RTFW_BUILD_TESTS)
    add_test(NAME hello_runtime_output
        COMMAND "${CMAKE_COMMAND}" "-DHELLO_EXECUTABLE=$<TARGET_FILE:sample_hello_runtime>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/hello_runtime/check_output.cmake")
    set_tests_properties(hello_runtime_output PROPERTIES TIMEOUT 30)
endif()

add_executable(sample_typed_runtime typed_runtime/main.cpp)
target_link_libraries(sample_typed_runtime PRIVATE rtfw::runtime)
rtfw_enable_project_warnings(sample_typed_runtime)
if(RTFW_BUILD_TESTS)
    add_test(NAME typed_runtime_output
        COMMAND "${CMAKE_COMMAND}" "-DTYPED_EXECUTABLE=$<TARGET_FILE:sample_typed_runtime>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/typed_runtime/check_output.cmake")
    set_tests_properties(typed_runtime_output PROPERTIES TIMEOUT 30)
endif()

add_executable(sample_backend_authoring backend_authoring/main.cpp)
target_link_libraries(sample_backend_authoring PRIVATE rtfw::runtime)
rtfw_enable_project_warnings(sample_backend_authoring)
if(RTFW_BUILD_TESTS)
    add_test(NAME backend_authoring_output
        COMMAND "${CMAKE_COMMAND}" "-DBACKEND_EXECUTABLE=$<TARGET_FILE:sample_backend_authoring>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/backend_authoring/check_output.cmake")
    set_tests_properties(backend_authoring_output PROPERTIES TIMEOUT 30)
endif()

# Optional source reference: OS mapping dependencies never reach rtfw::runtime.
if((WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux") AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    foreach(role plant controller)
        add_executable(sample_cil_${role} cil_shared_memory/${role}.cpp)
        target_link_libraries(sample_cil_${role} PRIVATE rtfw::runtime)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            target_link_libraries(sample_cil_${role} PRIVATE rt)
        endif()
        rtfw_enable_project_warnings(sample_cil_${role})
    endforeach()
    if(RTFW_BUILD_TESTS)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME cil_shared_memory_processes
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cil_shared_memory/runner.py"
                --plant "$<TARGET_FILE:sample_cil_plant>" --controller "$<TARGET_FILE:sample_cil_controller>")
        set_tests_properties(cil_shared_memory_processes PROPERTIES TIMEOUT 90)
    endif()
endif()

# Source-only independent host; platform observation stays off the SDK graph.
if((WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux") AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(sample_host_adapter host_adapter/main.cpp)
    target_link_libraries(sample_host_adapter PRIVATE rtfw::runtime)
    if(WIN32)
        target_link_libraries(sample_host_adapter PRIVATE psapi)
    endif()
    rtfw_enable_project_warnings(sample_host_adapter)
    if(RTFW_BUILD_TESTS)
        foreach(mode native host)
            add_test(NAME host_adapter_${mode} COMMAND ${CMAKE_COMMAND}
                -DHOST_EXECUTABLE=$<TARGET_FILE:sample_host_adapter> -DHOST_MODE=${mode}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/host_adapter/check_output.cmake")
            set_tests_properties(host_adapter_${mode} PROPERTIES TIMEOUT 40)
        endforeach()
    endif()
endif()

# Installed recipes reuse the unchanged public-package conformance sources.
add_subdirectory(recipes)

add_executable(sample_golden_system golden_system/main.cpp)
target_link_libraries(sample_golden_system PRIVATE rtfw::runtime)
if(WIN32)
    target_link_libraries(sample_golden_system PRIVATE psapi)
endif()
rtfw_enable_project_warnings(sample_golden_system)
add_executable(sample_golden_controller golden_system/controller.cpp)
target_link_libraries(sample_golden_controller PRIVATE rtfw::runtime)
if(UNIX AND NOT APPLE)
    target_link_libraries(sample_golden_controller PRIVATE rt)
endif()
rtfw_enable_project_warnings(sample_golden_controller)

add_executable(sample_golden_cuda golden_cuda/main.cpp)
target_link_libraries(sample_golden_cuda PRIVATE rtfw::runtime rtfw::cuda_backend)
if(WIN32)
    target_link_libraries(sample_golden_cuda PRIVATE psapi)
elseif(UNIX)
    target_link_libraries(sample_golden_cuda PRIVATE rt)
endif()
rtfw_enable_project_warnings(sample_golden_cuda)

add_executable(sample_golden_xdma golden_xdma/main.cpp)
target_link_libraries(sample_golden_xdma PRIVATE rtfw::runtime rtfw::cuda_backend rtfw::xdma_backend)
if(WIN32)
    target_link_libraries(sample_golden_xdma PRIVATE psapi)
elseif(UNIX)
    target_link_libraries(sample_golden_xdma PRIVATE rt)
endif()
rtfw_enable_project_warnings(sample_golden_xdma)
