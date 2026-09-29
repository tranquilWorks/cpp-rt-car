if((WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux") AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(test_golden_system test_system.cpp)
    target_link_libraries(test_golden_system PRIVATE rtfw::runtime)
    if(WIN32)
        target_link_libraries(test_golden_system PRIVATE psapi)
    endif()
    rtfw_enable_project_warnings(test_golden_system)
    add_test(NAME host_adapter_golden_system_contract COMMAND test_golden_system)
    set_tests_properties(host_adapter_golden_system_contract PROPERTIES TIMEOUT 120)
endif()
if(TARGET sample_golden_system)
    foreach(mode native host external)
        add_test(NAME host_adapter_golden_system_${mode} COMMAND ${RTFW_PYTHON_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/samples/golden_system/run.py
            --plant $<TARGET_FILE:sample_golden_system>
            --controller $<TARGET_FILE:sample_golden_controller> --mode ${mode}
            --provenance ${PROJECT_BINARY_DIR}/golden-provenance.json
            --output ${CMAKE_CURRENT_BINARY_DIR}/${mode}-evidence)
        set_tests_properties(host_adapter_golden_system_${mode} PROPERTIES TIMEOUT 60)
    endforeach()
endif()
if(TARGET sample_golden_system)
    add_test(NAME host_adapter_golden_system_process COMMAND ${RTFW_PYTHON_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/process_suite.py --plant $<TARGET_FILE:sample_golden_system>
        --controller $<TARGET_FILE:sample_golden_controller>
        --provenance ${PROJECT_BINARY_DIR}/golden-provenance.json)
    set_tests_properties(host_adapter_golden_system_process PROPERTIES TIMEOUT 120)
endif()

# Existing targeted TSan jobs build this established target and select the
# host_adapter filter. Preserve the workflow's protected historical binding.
if(TARGET rtfw_host_adapter_tests)
    add_dependencies(rtfw_host_adapter_tests test_golden_system)
    if(TARGET sample_golden_system)
        add_dependencies(rtfw_host_adapter_tests sample_golden_system sample_golden_controller)
    endif()
endif()
# Existing compiler/FMA determinism jobs explicitly build this target. Exercise
# the golden oracle/replay matrix on each compiler without changing job names.
if(TARGET rtfw_determinism_artifact AND TARGET sample_golden_system)
    add_custom_target(golden_determinism_check
        COMMAND $<TARGET_FILE:test_golden_system>
        COMMAND ${RTFW_PYTHON_EXECUTABLE} ${PROJECT_SOURCE_DIR}/samples/golden_system/run.py
            --plant $<TARGET_FILE:sample_golden_system> --mode native
            --provenance ${PROJECT_BINARY_DIR}/golden-provenance.json
            --output ${CMAKE_CURRENT_BINARY_DIR}/determinism-native
        COMMAND ${RTFW_PYTHON_EXECUTABLE} ${PROJECT_SOURCE_DIR}/samples/golden_system/run.py
            --plant $<TARGET_FILE:sample_golden_system> --mode host
            --provenance ${PROJECT_BINARY_DIR}/golden-provenance.json
            --output ${CMAKE_CURRENT_BINARY_DIR}/determinism-host
        DEPENDS test_golden_system sample_golden_system
        VERBATIM)
    add_dependencies(rtfw_determinism_artifact golden_determinism_check)
endif()
