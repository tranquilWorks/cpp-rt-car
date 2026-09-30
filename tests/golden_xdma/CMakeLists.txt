if((WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux") AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(test_golden_xdma test_xdma.cpp ../golden_cuda/allocation.cpp)
    target_link_libraries(test_golden_xdma PRIVATE rtfw::runtime rtfw::cuda_backend rtfw::xdma_backend)
    if(WIN32)
        target_link_libraries(test_golden_xdma PRIVATE psapi)
    endif()
    rtfw_enable_project_warnings(test_golden_xdma)
    # Keep full sanitizer instrumentation and the Debug Runtime libraries.
    # The new inline-heavy 1024-tick fixture uses the validated -O1 sanitizer
    # profile; no Runtime, prior target, diagnostic or timeout is changed.
    if(SIM_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(test_golden_xdma PRIVATE -O1)
    endif()
    add_test(NAME host_adapter_golden_xdma_contract COMMAND test_golden_xdma --case 0)
    set_tests_properties(host_adapter_golden_xdma_contract PROPERTIES TIMEOUT 120)
    # Instrumented full-horizon execution includes 1024 recorded frames and
    # complete originating-owner replay. Retained TSan evidence takes 121 s;
    # allow sanitizer overhead without changing logical/watchdog deadlines,
    # the workload, assertions, or any earlier test's timeout.
    set(golden_xdma_long_test_timeout 120)
    if(SIM_SANITIZERS)
        set(golden_xdma_long_test_timeout 300)
    endif()
    foreach(case RANGE 1 18)
        if(case EQUAL 9 OR case EQUAL 18)
            # Each 1024-tick owner retains its complete originating-owner replay.
            # Actual checked CPU bytes are a CTest fixture for both device paths.
            add_test(NAME host_adapter_golden_xdma_parity_${case} COMMAND test_golden_xdma --case ${case}
                --dispatch reference --reference ${CMAKE_CURRENT_BINARY_DIR}/reference-${case})
            set_tests_properties(host_adapter_golden_xdma_parity_${case} PROPERTIES
                TIMEOUT ${golden_xdma_long_test_timeout} FIXTURES_SETUP golden_xdma_reference_${case})
            foreach(dispatch cpu kernel graph)
                add_test(NAME host_adapter_golden_xdma_parity_${case}_${dispatch} COMMAND test_golden_xdma --case ${case}
                    --dispatch ${dispatch} --reference ${CMAKE_CURRENT_BINARY_DIR}/reference-${case})
                set_tests_properties(host_adapter_golden_xdma_parity_${case}_${dispatch} PROPERTIES
                    TIMEOUT ${golden_xdma_long_test_timeout} FIXTURES_REQUIRED golden_xdma_reference_${case})
            endforeach()
        else()
            add_test(NAME host_adapter_golden_xdma_parity_${case} COMMAND test_golden_xdma --case ${case})
            set_tests_properties(host_adapter_golden_xdma_parity_${case} PROPERTIES TIMEOUT 120)
        endif()
    endforeach()
    if(TARGET rtfw_host_adapter_tests)
        add_dependencies(rtfw_host_adapter_tests test_golden_xdma)
    endif()
endif()

if(TARGET sample_golden_xdma)
    foreach(mode native host external)
        foreach(dispatch cpu kernel graph)
            add_test(NAME host_adapter_golden_xdma_${mode}_${dispatch} COMMAND ${RTFW_PYTHON_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/samples/golden_xdma/run.py
                --plant $<TARGET_FILE:sample_golden_xdma> --controller $<TARGET_FILE:sample_golden_controller>
                --mode ${mode} --dispatch ${dispatch} --provenance ${PROJECT_BINARY_DIR}/golden-xdma-provenance.json
                --output ${CMAKE_CURRENT_BINARY_DIR}/${mode}-${dispatch}-evidence)
            set_tests_properties(host_adapter_golden_xdma_${mode}_${dispatch} PROPERTIES TIMEOUT 60)
        endforeach()
    endforeach()
    if(TARGET rtfw_host_adapter_tests)
        add_dependencies(rtfw_host_adapter_tests sample_golden_xdma sample_golden_controller)
    endif()
    if(TARGET rtfw_determinism_artifact)
        add_custom_target(golden_xdma_determinism_check COMMAND $<TARGET_FILE:test_golden_xdma>
            DEPENDS test_golden_xdma VERBATIM)
        # Keep complete prior checks, but avoid running their busy native
        # workers concurrently with this kit's unchanged 8-ms safe ACK bound.
        # Ordering is local to the new target; workloads/timeouts stay intact.
        if(TARGET golden_determinism_check)
            add_dependencies(golden_xdma_determinism_check golden_determinism_check)
        endif()
        if(TARGET golden_cuda_determinism_check)
            add_dependencies(golden_xdma_determinism_check golden_cuda_determinism_check)
        endif()
        add_dependencies(rtfw_determinism_artifact golden_xdma_determinism_check)
    endif()
endif()

if(TARGET sample_golden_xdma)
    add_test(NAME host_adapter_golden_xdma_process COMMAND ${RTFW_PYTHON_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/process_suite.py --plant $<TARGET_FILE:sample_golden_xdma>
        --controller $<TARGET_FILE:sample_golden_controller>
        --provenance ${PROJECT_BINARY_DIR}/golden-xdma-provenance.json --group external)
    set_tests_properties(host_adapter_golden_xdma_process PROPERTIES TIMEOUT 120)
    foreach(mode native host)
        add_test(NAME host_adapter_golden_xdma_process_${mode} COMMAND ${RTFW_PYTHON_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/process_suite.py --plant $<TARGET_FILE:sample_golden_xdma>
            --controller $<TARGET_FILE:sample_golden_controller>
            --provenance ${PROJECT_BINARY_DIR}/golden-xdma-provenance.json --group ${mode})
        # This aggregate includes 40 short cases plus three complete 1024-tick
        # CLI runs. Under sanitizers retain each child's existing 300-s bound
        # plus the original 120-s budget for the short cases. Ordinary runs,
        # all other tests, and Runtime logical/watchdog deadlines are unchanged.
        set(golden_xdma_process_timeout 120)
        if(SIM_SANITIZERS AND mode STREQUAL "native")
            math(EXPR golden_xdma_process_timeout "120 + 3 * 300")
        endif()
        set_tests_properties(host_adapter_golden_xdma_process_${mode} PROPERTIES
            TIMEOUT ${golden_xdma_process_timeout})
    endforeach()
endif()
