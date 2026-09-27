function(rtfw_add_physics_real target source)
  find_package(CUDAToolkit REQUIRED)
  if(NOT CUDAToolkit_NVCC_EXECUTABLE)
    message(FATAL_ERROR "The optional physics kernel requires the CUDA toolkit nvcc compiler")
  endif()
  set(ptx "${CMAKE_CURRENT_BINARY_DIR}/${target}.ptx")
  add_custom_command(OUTPUT "${ptx}"
    COMMAND "${CUDAToolkit_NVCC_EXECUTABLE}" --ptx -arch=compute_75
            "${source}/particle.cu" -o "${ptx}"
    DEPENDS "${source}/particle.cu" VERBATIM)
  add_custom_target(${target}_kernel DEPENDS "${ptx}")
  add_executable(${target} "${source}/real.cpp")
  add_dependencies(${target} ${target}_kernel)
  target_compile_definitions(${target} PRIVATE RTFW_PHYSICS_PTX_PATH="${ptx}")
  target_link_libraries(${target} PRIVATE rtfw::runtime rtfw::cuda_driver)
endfunction()
