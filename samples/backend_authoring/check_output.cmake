if(NOT DEFINED BACKEND_EXECUTABLE)
    message(FATAL_ERROR "BACKEND_EXECUTABLE is required")
endif()
execute_process(COMMAND "${BACKEND_EXECUTABLE}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
string(REPLACE "\r\n" "\n" output "${output}")
if(NOT "${result}" STREQUAL "0" OR NOT "${error}" STREQUAL "" OR
   NOT "${output}" STREQUAL "backend_authoring: conformance=ok native=3 v1=3 verified=3 ownership=0\n")
    message(FATAL_ERROR "backend authoring failed: exit=${result}; stdout=${output}; stderr=${error}")
endif()
