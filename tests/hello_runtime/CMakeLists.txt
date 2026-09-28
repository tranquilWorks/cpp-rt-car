# Private copies exercise the actual example's error handling, including under sanitizers.
file(READ "${RTFW_PROJECT_SOURCE_DIR}/samples/hello_runtime/main.cpp" hello_source)
string(REPLACE "config.callback_capacity = 2;" "config.callback_capacity = 1;"
    capacity_source "${hello_source}")
string(REPLACE "++static_cast<State*>(user)->produced;\n    return rt::CallbackResult::ok;"
    "(void)user;\n    return rt::CallbackResult::error;"
    step_source "${hello_source}")
string(REPLACE "if (!ok || !stopped) return 1;"
    "if (stopped) std::cerr << \"checked stop reached\\n\";\n    if (!ok || !stopped) return 1;"
    step_source "${step_source}")
foreach(failure capacity step)
    if("${${failure}_source}" STREQUAL "${hello_source}")
        message(FATAL_ERROR "hello ${failure} negative control was not applied")
    endif()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${failure}.cpp" "${${failure}_source}")
    add_executable(hello_${failure}_failure "${CMAKE_CURRENT_BINARY_DIR}/${failure}.cpp")
    target_link_libraries(hello_${failure}_failure PRIVATE rtfw::runtime)
    rtfw_enable_project_warnings(hello_${failure}_failure)
    add_test(NAME hello_${failure}_failure
        COMMAND "${CMAKE_COMMAND}" "-DHELLO_EXECUTABLE=$<TARGET_FILE:hello_${failure}_failure>"
            "-DHELLO_FAILURE=${failure}" -P "${CMAKE_CURRENT_SOURCE_DIR}/check_failure.cmake")
    set_tests_properties(hello_${failure}_failure PROPERTIES TIMEOUT 30)
endforeach()
