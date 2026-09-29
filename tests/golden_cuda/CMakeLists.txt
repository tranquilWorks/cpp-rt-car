if((WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux") AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(test_golden_cuda test_cuda.cpp)
    target_link_libraries(test_golden_cuda PRIVATE rtfw::runtime rtfw::cuda_backend)
    if(WIN32)
        target_link_libraries(test_golden_cuda PRIVATE psapi)
    endif()
    rtfw_enable_project_warnings(test_golden_cuda)
    add_test(NAME host_adapter_golden_cuda_contract COMMAND test_golden_cuda)
    set_tests_properties(host_adapter_golden_cuda_contract PROPERTIES TIMEOUT 120)
    if(TARGET rtfw_host_adapter_tests)
        add_dependencies(rtfw_host_adapter_tests test_golden_cuda)
    endif()
endif()
if(TARGET sample_golden_cuda)
    foreach(mode native host external)
        foreach(dispatch cpu kernel graph)
            add_test(NAME host_adapter_golden_cuda_${mode}_${dispatch} COMMAND ${RTFW_PYTHON_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/samples/golden_cuda/run.py
                --plant $<TARGET_FILE:sample_golden_cuda> --controller $<TARGET_FILE:sample_golden_controller>
                --mode ${mode} --dispatch ${dispatch} --provenance ${PROJECT_BINARY_DIR}/golden-cuda-provenance.json
                --output ${CMAKE_CURRENT_BINARY_DIR}/${mode}-${dispatch}-evidence)
            set_tests_properties(host_adapter_golden_cuda_${mode}_${dispatch} PROPERTIES TIMEOUT 60)
        endforeach()
    endforeach()
    if(TARGET rtfw_host_adapter_tests)
        add_dependencies(rtfw_host_adapter_tests sample_golden_cuda sample_golden_controller)
    endif()
    if(TARGET rtfw_determinism_artifact)
        add_custom_target(golden_cuda_determinism_check COMMAND $<TARGET_FILE:test_golden_cuda>
            DEPENDS test_golden_cuda VERBATIM)
        add_dependencies(rtfw_determinism_artifact golden_cuda_determinism_check)
    endif()
endif()

if(TARGET sample_golden_cuda)
    add_test(NAME host_adapter_golden_cuda_process COMMAND ${RTFW_PYTHON_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/process_suite.py --plant $<TARGET_FILE:sample_golden_cuda>
        --controller $<TARGET_FILE:sample_golden_controller>
        --provenance ${PROJECT_BINARY_DIR}/golden-cuda-provenance.json)
    set_tests_properties(host_adapter_golden_cuda_process PROPERTIES TIMEOUT 120)
endif()
