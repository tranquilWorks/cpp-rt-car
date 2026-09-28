cmake_minimum_required(VERSION 3.20)
project(hello_runtime LANGUAGES CXX)

# Set this only to embed a source checkout instead of using an installed SDK.
set(HELLO_RTFW_SOURCE "" CACHE PATH "RTFW source checkout for add_subdirectory")
if(HELLO_RTFW_SOURCE)
    if(NOT EXISTS "${HELLO_RTFW_SOURCE}/CMakeLists.txt")
        message(FATAL_ERROR "HELLO_RTFW_SOURCE must name an RTFW source checkout")
    endif()
    add_subdirectory("${HELLO_RTFW_SOURCE}" rtfw-build)
elseif(NOT TARGET rtfw::runtime)
    find_package(rtfw 1.2 CONFIG REQUIRED COMPONENTS runtime)
endif()

add_executable(hello_runtime main.cpp)
target_link_libraries(hello_runtime PRIVATE rtfw::runtime)
# C++20 and platform link requirements come from the public target.
enable_testing()
add_test(NAME hello_runtime_output
    COMMAND "${CMAKE_COMMAND}" "-DHELLO_EXECUTABLE=$<TARGET_FILE:hello_runtime>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_output.cmake")
set_tests_properties(hello_runtime_output PROPERTIES TIMEOUT 30)
