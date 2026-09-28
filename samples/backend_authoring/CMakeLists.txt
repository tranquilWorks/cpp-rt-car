cmake_minimum_required(VERSION 3.20)
project(backend_authoring LANGUAGES CXX)

# Set this only to embed a source checkout instead of using an installed SDK.
set(BACKEND_RTFW_SOURCE "" CACHE PATH "RTFW source checkout for add_subdirectory")
if(BACKEND_RTFW_SOURCE)
    if(NOT EXISTS "${BACKEND_RTFW_SOURCE}/CMakeLists.txt")
        message(FATAL_ERROR "BACKEND_RTFW_SOURCE must name an RTFW source checkout")
    endif()
    add_subdirectory("${BACKEND_RTFW_SOURCE}" rtfw-build)
elseif(NOT TARGET rtfw::runtime)
    find_package(rtfw 1.2 CONFIG REQUIRED COMPONENTS runtime)
endif()

add_executable(backend_authoring main.cpp)
target_link_libraries(backend_authoring PRIVATE rtfw::runtime)
# C++20 and platform link requirements come from the public target.
enable_testing()
add_test(NAME backend_authoring_output
    COMMAND "${CMAKE_COMMAND}" "-DBACKEND_EXECUTABLE=$<TARGET_FILE:backend_authoring>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_output.cmake")
set_tests_properties(backend_authoring_output PROPERTIES TIMEOUT 30)
