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
