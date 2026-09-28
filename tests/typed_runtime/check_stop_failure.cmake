execute_process(COMMAND "${EXECUTABLE}" --stop-failure
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(NOT "${result}" STREQUAL "73" OR NOT "${output}" STREQUAL "" OR NOT "${error}" STREQUAL "")
    message(FATAL_ERROR "Expected fail-closed destructor exit73 after one stop attempt: ${result}; ${output}; ${error}")
endif()
