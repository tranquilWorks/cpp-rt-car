if(NOT DEFINED TYPED_EXECUTABLE)
    message(FATAL_ERROR "TYPED_EXECUTABLE is required")
endif()
execute_process(COMMAND "${TYPED_EXECUTABLE}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
string(REPLACE "\r\n" "\n" output "${output}")
if(NOT "${result}" STREQUAL "0" OR NOT "${error}" STREQUAL "" OR
   NOT "${output}" STREQUAL "typed_runtime: frames=3 produced=3 consumed=3 stopped=ok\n")
    message(FATAL_ERROR "typed Runtime failed: exit=${result}; stdout=${output}; stderr=${error}")
endif()
