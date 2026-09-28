if((WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux") AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(test_host_adapter test_host.cpp)
    target_link_libraries(test_host_adapter PRIVATE rtfw::runtime)
    if(WIN32)
        target_link_libraries(test_host_adapter PRIVATE psapi)
    endif()
    rtfw_enable_project_warnings(test_host_adapter)
    add_test(NAME host_adapter_contract COMMAND test_host_adapter)
    set_tests_properties(host_adapter_contract PROPERTIES TIMEOUT 60)

    # The existing targeted sanitizer build selects all host_adapter CTests.
    # Building its established target must also produce the new test binaries.
    add_dependencies(rtfw_host_adapter_tests test_host_adapter)
    if(TARGET sample_host_adapter)
        add_dependencies(rtfw_host_adapter_tests sample_host_adapter)
    endif()
endif()
