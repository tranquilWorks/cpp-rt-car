if(NOT DEFINED HOST_EXECUTABLE OR NOT DEFINED HOST_MODE)
    message(FATAL_ERROR "HOST_EXECUTABLE and HOST_MODE are required")
endif()
execute_process(COMMAND "${HOST_EXECUTABLE}" "${HOST_MODE}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
string(REPLACE "\r\n" "\n" output "${output}")
if(NOT "${result}" STREQUAL "0" OR NOT "${error}" STREQUAL "" OR
   NOT "${output}" STREQUAL "host_adapter: mode=${HOST_MODE} frames=16 instances=2 checksums=2176,6528 telemetry=32/32 lost=0 jobs=balanced memory=6/6 stopped=ok\n")
    message(FATAL_ERROR "host adapter oracle mismatch: exit=${result}; stdout=${output}; stderr=${error}")
endif()
