# No local package selection in ordinary builds. Development pins are compiled
# into the application; adjacent runtime manifests and environment are ignored.
set(AETHERPD_DEVELOPMENT_SELECTION "" CACHE FILEPATH
    "Reviewed AetherPD local development selection JSON (never for shipping builds)")
set(_pd_selection_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/aetherpd")
set(_pd_selection_header "${_pd_selection_dir}/AetherPDDevelopmentSelection.h")
file(MAKE_DIRECTORY "${_pd_selection_dir}")
if(AETHERPD_DEVELOPMENT_SELECTION)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    get_filename_component(_pd_selection "${AETHERPD_DEVELOPMENT_SELECTION}" ABSOLUTE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_pd_selection}" "${CMAKE_SOURCE_DIR}/tools/pd-waveform/development_selection.py")
    execute_process(COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/pd-waveform/development_selection.py"
        "${_pd_selection}" "${_pd_selection_header}"
        RESULT_VARIABLE _pd_selection_result)
    if(NOT _pd_selection_result EQUAL 0)
        message(FATAL_ERROR "Invalid AetherPD development selection")
    endif()
    message(WARNING "AetherPD local development package enabled: this build must not ship")
else()
    configure_file("${CMAKE_SOURCE_DIR}/cmake/AetherPDDevelopmentSelection.h.in"
        "${_pd_selection_header}" COPYONLY)
endif()
target_include_directories(aethercore PRIVATE "${_pd_selection_dir}")
