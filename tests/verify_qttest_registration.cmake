# Compare QtTest's executable method inventory with the split CTest cases.
if(NOT DEFINED TEST_EXECUTABLE OR NOT DEFINED EXPECTED_METHODS)
    message(FATAL_ERROR "TEST_EXECUTABLE and EXPECTED_METHODS are required")
endif()
execute_process(
    COMMAND "${TEST_EXECUTABLE}" -functions
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _error
    TIMEOUT 10)
if(NOT "${_result}" STREQUAL "0")
    message(FATAL_ERROR "QtTest method discovery failed: ${_result}\n${_error}")
endif()
string(REPLACE "\r\n" "\n" _output "${_output}")
string(REPLACE "\n" ";" _lines "${_output}")
set(_actual)
foreach(_line IN LISTS _lines)
    string(STRIP "${_line}" _line)
    if(_line STREQUAL "")
        continue()
    endif()
    if(NOT _line MATCHES "^([A-Za-z_][A-Za-z0-9_]*)\\(\\)$")
        message(FATAL_ERROR "Unexpected QtTest discovery output: ${_line}")
    endif()
    list(APPEND _actual "${CMAKE_MATCH_1}")
endforeach()
if(NOT _actual)
    message(FATAL_ERROR "QtTest discovery returned no methods")
endif()
set(_expected "${EXPECTED_METHODS}")
list(SORT _actual)
list(SORT _expected)
if(NOT "${_actual}" STREQUAL "${_expected}")
    set(_missing ${_actual})
    list(REMOVE_ITEM _missing ${_expected})
    set(_unknown ${_expected})
    list(REMOVE_ITEM _unknown ${_actual})
    message(FATAL_ERROR
        "QtTest registration mismatch\nUnregistered methods: ${_missing}\n"
        "Unknown or duplicate registrations: ${_unknown}\n"
        "Expected: ${_expected}\nDiscovered: ${_actual}")
endif()
