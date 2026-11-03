if((WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux") AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(rtfw_unreal_worlds_contract test_worlds.cpp)
    target_compile_features(rtfw_unreal_worlds_contract PRIVATE cxx_std_20)
    target_include_directories(rtfw_unreal_worlds_contract PRIVATE
        "${RTFW_PROJECT_SOURCE_DIR}/integrations/unreal/portable"
        "${RTFW_PROJECT_SOURCE_DIR}/integrations/host_lifecycle")
    target_link_libraries(rtfw_unreal_worlds_contract PRIVATE rtfw::runtime ${CMAKE_DL_LIBS})
    if(WIN32)
        target_link_libraries(rtfw_unreal_worlds_contract PRIVATE Psapi)
    endif()
    rtfw_enable_project_warnings(rtfw_unreal_worlds_contract)
    add_dependencies(rtfw_unreal_worlds_contract rtfw_host_lifecycle_plugin)
    add_test(NAME host_adapter_unreal_worlds_contract COMMAND rtfw_unreal_worlds_contract
        $<TARGET_FILE:rtfw_host_lifecycle_plugin>)
    set_tests_properties(host_adapter_unreal_worlds_contract PROPERTIES TIMEOUT 60)
    add_test(NAME unreal_source_prepare_contract COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/test_prepare.py")
    set_tests_properties(unreal_source_prepare_contract PROPERTIES TIMEOUT 30)
    set(unreal_source_sanitizers "")
    if(SIM_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        string(REPLACE ";" "," unreal_source_sanitizers "${SIM_SANITIZERS}")
    endif()
    add_test(NAME unreal_source_package COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/verify_package.py" --build "${RTFW_PROJECT_BINARY_DIR}"
        --config "$<CONFIG>" --c-compiler "${CMAKE_C_COMPILER}"
        --cxx-compiler "${CMAKE_CXX_COMPILER}" "--sanitizers=${unreal_source_sanitizers}")
    set_tests_properties(unreal_source_package PROPERTIES TIMEOUT 180)
    if(TARGET rtfw_host_adapter_tests)
        add_dependencies(rtfw_host_adapter_tests rtfw_unreal_worlds_contract)
    endif()
endif()
