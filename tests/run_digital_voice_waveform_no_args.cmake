if(NOT DEFINED HELPER OR HELPER STREQUAL "")
    message(FATAL_ERROR "HELPER is required")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "SSDR_RADIO_ADDRESS="
        "AETHER_DV_THUMBDV_SERIAL="
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

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "AETHER_DSTAR_MYCALL="
        "${HELPER}"
        --host 127.0.0.1
        --vocoder thumbdv
        --serial invalid-test-device
        --mode DSTR
        --underlying-mode DFM
        --waveform-name AetherDStar
        --mycall CALLSIGN
    RESULT_VARIABLE invalid_config_result
    OUTPUT_VARIABLE invalid_config_output
    ERROR_VARIABLE invalid_config_error
)

set(invalid_config_combined "${invalid_config_output}${invalid_config_error}")
if(invalid_config_result EQUAL 0)
    message(FATAL_ERROR "Expected helper to reject invalid D-STAR MYCALL")
endif()
if(NOT invalid_config_combined MATCHES "Invalid D-STAR MYCALL")
    message(FATAL_ERROR
        "Expected invalid-MYCALL diagnostic, got: ${invalid_config_combined}")
endif()

message(STATUS "helper rejected missing host and invalid D-STAR configuration as expected")
