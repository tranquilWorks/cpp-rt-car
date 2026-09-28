add_executable(test_typed_sdk test_sdk.cpp)
target_link_libraries(test_typed_sdk PRIVATE rtfw::runtime)
rtfw_enable_project_warnings(test_typed_sdk)
add_test(NAME typed_sdk COMMAND test_typed_sdk)
add_test(NAME typed_sdk_stop_failure
    COMMAND "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:test_typed_sdk>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_stop_failure.cmake")
set_tests_properties(typed_sdk typed_sdk_stop_failure PROPERTIES TIMEOUT 60)
