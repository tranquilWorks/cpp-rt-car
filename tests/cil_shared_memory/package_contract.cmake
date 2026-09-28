# Separate additive reference: leave the historical SDK/CUDA contract untouched.
if((WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux") AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(cil_kit "${RTFW_DATA_DIR}/examples/cil_shared_memory")
    file(GLOB cil_sources LIST_DIRECTORIES FALSE RELATIVE "${cil_kit}" "${cil_kit}/*")
    list(SORT cil_sources)
    if(NOT "${cil_sources}" STREQUAL "CMakeLists.txt;channel.hpp;common.hpp;controller.cpp;mapping.hpp;plant.cpp;plant_runtime.hpp;runner.py;wire.hpp")
        message(FATAL_ERROR "CIL source inventory differs from contract")
    endif()
    if(NOT EXISTS "${RTFW_DATA_DIR}/cil_shared_memory.md")
        message(FATAL_ERROR "CIL guide is missing")
    endif()
    add_subdirectory("${cil_kit}" cil-shared-memory)
endif()
