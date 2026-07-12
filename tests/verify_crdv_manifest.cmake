if(NOT DEFINED CRDV_DIR)
    message(FATAL_ERROR "CRDV_DIR is required")
endif()

set(manifest "${CRDV_DIR}/CONTENT_SHA256.txt")
file(STRINGS "${manifest}" lines)

set(verified 0)
foreach(line IN LISTS lines)
    if(line STREQUAL "" OR line MATCHES "^#")
        continue()
    endif()
    string(LENGTH "${line}" line_length)
    if(line_length LESS 67)
        message(FATAL_ERROR "Malformed CRDV manifest line: ${line}")
    endif()
    string(SUBSTRING "${line}" 0 64 expected)
    string(SUBSTRING "${line}" 64 2 separator)
    string(SUBSTRING "${line}" 66 -1 relative_path)
    string(LENGTH "${expected}" expected_length)
    if(NOT expected_length EQUAL 64
        OR NOT expected MATCHES "^[0-9a-f]+$"
        OR NOT separator STREQUAL "  "
        OR relative_path STREQUAL "")
        message(FATAL_ERROR "Malformed CRDV manifest line: ${line}")
    endif()
    set(path "${CRDV_DIR}/${relative_path}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "CRDV manifest entry is missing: ${relative_path}")
    endif()
    file(SHA256 "${path}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR
            "CRDV manifest mismatch for ${relative_path}: expected ${expected}, got ${actual}")
    endif()
    math(EXPR verified "${verified} + 1")
endforeach()

if(verified EQUAL 0)
    message(FATAL_ERROR "CRDV manifest contains no file entries")
endif()

message(STATUS "Verified ${verified} CRDV manifest entries")
