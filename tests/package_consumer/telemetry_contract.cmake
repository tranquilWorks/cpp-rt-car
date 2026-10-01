# M27-04: optional source kit does not expand default SDK headers/targets.
set(expected_telemetry_sources CMakeLists.txt README.md etw.cpp etw.hpp export.py
    main.cpp requirements.txt telemetry.cpp telemetry.hpp)
file(GLOB actual_telemetry_sources LIST_DIRECTORIES FALSE
    RELATIVE "${RTFW_DATA_DIR}/integrations/telemetry"
    "${RTFW_DATA_DIR}/integrations/telemetry/*")
list(SORT expected_telemetry_sources)
list(SORT actual_telemetry_sources)
if(NOT actual_telemetry_sources STREQUAL expected_telemetry_sources)
    message(FATAL_ERROR "Optional telemetry source-kit inventory differs from contract")
endif()
if(TARGET rtfw::telemetry OR TARGET rtfw_telemetry_kit)
    message(FATAL_ERROR "Optional telemetry kit leaked into default SDK targets")
endif()
