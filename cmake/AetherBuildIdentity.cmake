# Capture the build identity and write the generated header (#5804). Invoked by
# a custom target at BUILD time (see CMakeLists.txt), so it re-evaluates on every
# build.
#
# Inputs: AETHER_SRC_DIR (the checkout), AETHER_IN_FILE (the .h.in template),
# AETHER_OUT_FILE (the header to write). Without git, or outside a checkout (a
# source tarball), every field stays "unknown" / -1 / false.
#
# configure_file() only rewrites when the content differs, so an unchanged HEAD
# costs nothing and does not trigger a rebuild of everything that includes it.

find_package(Git QUIET)

set(AETHER_BUILD_DESCRIBE          "unknown")
set(AETHER_BUILD_SHA               "unknown")
set(AETHER_BUILD_BASELINE          "unknown")
set(AETHER_BUILD_COMMITS_SINCE_TAG -1)
set(AETHER_BUILD_DIRTY             false)

if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${AETHER_SRC_DIR}
        OUTPUT_VARIABLE _d OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _rv)
    if(_rv EQUAL 0 AND NOT "${_d}" STREQUAL "")
        set(AETHER_BUILD_DESCRIBE "${_d}")

        if(_d MATCHES "-dirty$")
            set(AETHER_BUILD_DIRTY true)
            string(REGEX REPLACE "-dirty$" "" _d "${_d}")
        endif()

        # "v26.9.3-68-g7e841682" -> baseline v26.9.3, 68 commits, sha 7e841682.
        # A bare tag has neither the count nor the g-prefixed hash.
        if(_d MATCHES "^(.+)-([0-9]+)-g([0-9a-f]+)$")
            set(AETHER_BUILD_BASELINE          "${CMAKE_MATCH_1}")
            set(AETHER_BUILD_COMMITS_SINCE_TAG "${CMAKE_MATCH_2}")
            set(AETHER_BUILD_SHA               "${CMAKE_MATCH_3}")
        elseif(_d MATCHES "^[0-9a-f]+$")
            # --always fallback: no tag is reachable at all.
            set(AETHER_BUILD_SHA "${_d}")
        else()
            # A bare tag: HEAD is exactly on it. describe carries no hash
            # here, so the rev-parse below supplies it.
            set(AETHER_BUILD_BASELINE          "${_d}")
            set(AETHER_BUILD_COMMITS_SINCE_TAG 0)
        endif()

        if("${AETHER_BUILD_SHA}" STREQUAL "unknown")
            execute_process(
                COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                WORKING_DIRECTORY ${AETHER_SRC_DIR}
                OUTPUT_VARIABLE _s OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE _srv)
            if(_srv EQUAL 0 AND NOT "${_s}" STREQUAL "")
                set(AETHER_BUILD_SHA "${_s}")
            endif()
        endif()
    endif()
endif()

configure_file("${AETHER_IN_FILE}" "${AETHER_OUT_FILE}" @ONLY)
