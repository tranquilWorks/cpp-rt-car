if(RTFW_PYTHON_EXECUTABLE)
    add_test(NAME host_adapter_golden_audit_contract
        COMMAND ${RTFW_PYTHON_EXECUTABLE} -m unittest discover
                -s ${CMAKE_CURRENT_SOURCE_DIR} -p test_*.py)
    set_tests_properties(host_adapter_golden_audit_contract PROPERTIES TIMEOUT 120)
endif()
