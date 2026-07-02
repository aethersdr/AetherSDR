if(NOT DEFINED HELPER OR HELPER STREQUAL "")
    message(FATAL_ERROR "HELPER is required")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "SSDR_RADIO_ADDRESS="
        "AETHER_DSTAR_THUMBDV_SERIAL="
        "${HELPER}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)

set(combined "${output}${error}")
if(result EQUAL 0)
    message(FATAL_ERROR "Expected helper to fail without --host")
endif()

if(NOT combined MATCHES "Missing --host/SSDR_RADIO_ADDRESS")
    message(FATAL_ERROR "Expected missing-host diagnostic, got: ${combined}")
endif()

message(STATUS "helper failed without --host as expected")
