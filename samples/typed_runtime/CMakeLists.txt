cmake_minimum_required(VERSION 3.20)
project(typed_runtime LANGUAGES CXX)

# Set this only to embed a source checkout instead of using an installed SDK.
set(TYPED_RTFW_SOURCE "" CACHE PATH "RTFW source checkout for add_subdirectory")
if(TYPED_RTFW_SOURCE)
    if(NOT EXISTS "${TYPED_RTFW_SOURCE}/CMakeLists.txt")
        message(FATAL_ERROR "TYPED_RTFW_SOURCE must name an RTFW source checkout")
    endif()
    add_subdirectory("${TYPED_RTFW_SOURCE}" rtfw-build)
elseif(NOT TARGET rtfw::runtime)
    find_package(rtfw 1.2 CONFIG REQUIRED COMPONENTS runtime)
endif()

add_executable(typed_runtime main.cpp)
target_link_libraries(typed_runtime PRIVATE rtfw::runtime)
# C++20 and platform link requirements come from the public target.
enable_testing()
add_test(NAME typed_runtime_output
    COMMAND "${CMAKE_COMMAND}" "-DTYPED_EXECUTABLE=$<TARGET_FILE:typed_runtime>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_output.cmake")
set_tests_properties(typed_runtime_output PROPERTIES TIMEOUT 30)
