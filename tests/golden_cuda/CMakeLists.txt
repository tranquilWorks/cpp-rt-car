if((WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux") AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(test_golden_cuda test_cuda.cpp allocation.cpp)
    target_link_libraries(test_golden_cuda PRIVATE rtfw::runtime rtfw::cuda_backend)
    if(WIN32)
        target_link_libraries(test_golden_cuda PRIVATE psapi)
    endif()
    rtfw_enable_project_warnings(test_golden_cuda)
    # Keep full sanitizer instrumentation and the Debug Runtime libraries.
    # The new inline-heavy 1024-tick fixture uses the validated -O1 sanitizer
    # profile; no Runtime, prior target, diagnostic or timeout is changed.
    if(SIM_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(test_golden_cuda PRIVATE -O1)
    endif()
    add_test(NAME host_adapter_golden_cuda_contract COMMAND test_golden_cuda --case 0)
    set_tests_properties(host_adapter_golden_cuda_contract PROPERTIES TIMEOUT 120)
    foreach(case RANGE 1 18)
        if(case EQUAL 9 OR case EQUAL 18)
            # Each 1024-tick owner retains its complete originating-owner replay.
            # Actual checked CPU bytes are a CTest fixture for both device paths.
            add_test(NAME host_adapter_golden_cuda_parity_${case} COMMAND test_golden_cuda --case ${case}
                --dispatch cpu --reference ${CMAKE_CURRENT_BINARY_DIR}/reference-${case})
            set_tests_properties(host_adapter_golden_cuda_parity_${case} PROPERTIES
                TIMEOUT 120 FIXTURES_SETUP golden_cuda_reference_${case})
            foreach(dispatch kernel graph)
                add_test(NAME host_adapter_golden_cuda_parity_${case}_${dispatch} COMMAND test_golden_cuda --case ${case}
                    --dispatch ${dispatch} --reference ${CMAKE_CURRENT_BINARY_DIR}/reference-${case})
                set_tests_properties(host_adapter_golden_cuda_parity_${case}_${dispatch} PROPERTIES
                    TIMEOUT 120 FIXTURES_REQUIRED golden_cuda_reference_${case})
            endforeach()
        else()
            add_test(NAME host_adapter_golden_cuda_parity_${case} COMMAND test_golden_cuda --case ${case})
            set_tests_properties(host_adapter_golden_cuda_parity_${case} PROPERTIES TIMEOUT 120)
        endif()
    endforeach()
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
        --provenance ${PROJECT_BINARY_DIR}/golden-cuda-provenance.json --group external)
    set_tests_properties(host_adapter_golden_cuda_process PROPERTIES TIMEOUT 120)
    foreach(mode native host)
        add_test(NAME host_adapter_golden_cuda_process_${mode} COMMAND ${RTFW_PYTHON_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/process_suite.py --plant $<TARGET_FILE:sample_golden_cuda>
            --controller $<TARGET_FILE:sample_golden_controller>
            --provenance ${PROJECT_BINARY_DIR}/golden-cuda-provenance.json --group ${mode})
        set_tests_properties(host_adapter_golden_cuda_process_${mode} PROPERTIES TIMEOUT 120)
    endforeach()
endif()
