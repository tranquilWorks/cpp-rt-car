if(NOT DEFINED HELLO_EXECUTABLE)
    message(FATAL_ERROR "HELLO_EXECUTABLE is required")
endif()
execute_process(COMMAND "${HELLO_EXECUTABLE}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
string(REPLACE "\r\n" "\n" output "${output}")
if(NOT "${result}" STREQUAL "0" OR NOT "${error}" STREQUAL "" OR
   NOT "${output}" STREQUAL "hello_runtime: frames=3 produced=3 consumed=3 stopped=ok\n")
    message(FATAL_ERROR "hello Runtime failed: exit=${result}; stdout=${output}; stderr=${error}")
endif()
