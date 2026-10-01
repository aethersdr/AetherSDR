# AetherQtPin.cmake — find the pinned Qt that scripts/setup/setup-qt.{sh,ps1}
# installs.
#
# Included by CMakeLists.txt BEFORE its first find_package(Qt6). It only ever
# adds a search path; it never overrides a Qt the builder chose:
#
#   - AETHER_USE_PINNED_QT=OFF               → does nothing
#   - Qt6_DIR set (cache or environment)     → does nothing
#   - a CMAKE_PREFIX_PATH entry (cache or
#     environment) already holds a Qt6 kit  → does nothing
#   - otherwise, if setup-qt.sh's install exists, prepend it, so it outranks
#     a distro Qt in the system prefix
#
# AETHER_FETCH_QT=ON runs setup-qt.sh (setup-qt.ps1 on Windows) at configure time when the install is
# missing. It is OFF by default on purpose: a 2 GB download is not something a
# plain `cmake -B build` should start on its own, and an offline or packaging
# build must never try.
#
# Sets AETHER_QT_PIN_VERSION, AETHER_QT_PIN_REVISION, AETHER_QT_PIN_PREFIX (the
# path setup-qt.sh installs to on this host, whether or not it exists) for the
# diagnostics in CMakeLists.txt.

option(AETHER_USE_PINNED_QT
    "Prefer the pinned Qt installed by scripts/setup/setup-qt.sh when present" ON)
option(AETHER_FETCH_QT
    "Run scripts/setup/setup-qt.{sh,ps1} at configure time if the pinned Qt is missing" OFF)

set(_aether_pin_file "${CMAKE_CURRENT_LIST_DIR}/qt-pin.env")
file(STRINGS "${_aether_pin_file}" _aether_pin_lines REGEX "^[A-Z0-9_]+=")
foreach(_line IN LISTS _aether_pin_lines)
    string(REGEX MATCH "^([A-Z0-9_]+)=\"?([^\"]*)\"?$" _ "${_line}")
    set(_aether_pin_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
endforeach()
set(AETHER_QT_PIN_VERSION "${_aether_pin_QT_VERSION}")
set(AETHER_QT_PIN_REVISION "${_aether_pin_QT_PACKAGE_REVISION}")
if(NOT AETHER_QT_PIN_VERSION OR NOT AETHER_QT_PIN_REVISION)
    message(FATAL_ERROR "${_aether_pin_file} is missing QT_VERSION or QT_PACKAGE_REVISION.")
endif()

# Mirror setup-qt.{sh,ps1}'s install path exactly — they must agree or this
# finds nothing. AETHER_QT_CACHE overrides all three.
if(DEFINED ENV{AETHER_QT_CACHE} AND NOT "$ENV{AETHER_QT_CACHE}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{AETHER_QT_CACHE}" _aether_qt_cache)
elseif(WIN32)
    file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/aethersdr/qt" _aether_qt_cache)
elseif(APPLE)
    set(_aether_qt_cache "$ENV{HOME}/Library/Caches/aethersdr/qt")
elseif(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
    set(_aether_qt_cache "$ENV{XDG_CACHE_HOME}/aethersdr/qt")
else()
    set(_aether_qt_cache "$ENV{HOME}/.cache/aethersdr/qt")
endif()
if(WIN32)
    set(_aether_kit msvc2022_64)
elseif(APPLE)
    set(_aether_kit macos)
elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(_aether_kit gcc_arm64)
else()
    set(_aether_kit gcc_64)
endif()
set(AETHER_QT_PIN_PREFIX
    "${_aether_qt_cache}/${AETHER_QT_PIN_VERSION}-${AETHER_QT_PIN_REVISION}/${AETHER_QT_PIN_VERSION}/${_aether_kit}")

# Has the builder already pointed CMake at a Qt?
set(_aether_qt_chosen FALSE)
if(Qt6_DIR OR DEFINED ENV{Qt6_DIR})
    set(_aether_qt_chosen TRUE)
endif()
set(_aether_env_prefixes "$ENV{CMAKE_PREFIX_PATH}")
if(NOT WIN32)
    string(REPLACE ":" ";" _aether_env_prefixes "${_aether_env_prefixes}")
endif()
foreach(_p IN LISTS CMAKE_PREFIX_PATH _aether_env_prefixes)
    if(_p AND EXISTS "${_p}/lib/cmake/Qt6/Qt6Config.cmake")
        set(_aether_qt_chosen TRUE)
    endif()
endforeach()

if(AETHER_USE_PINNED_QT AND NOT _aether_qt_chosen)
    if(NOT EXISTS "${AETHER_QT_PIN_PREFIX}/lib/cmake/Qt6/Qt6Config.cmake" AND AETHER_FETCH_QT)
        if(WIN32)
            find_program(_aether_ps NAMES pwsh powershell REQUIRED)
            set(_aether_fetch_cmd "${_aether_ps}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/setup/setup-qt.ps1")
        else()
            set(_aether_fetch_cmd bash "${CMAKE_SOURCE_DIR}/scripts/setup/setup-qt.sh")
        endif()
        message(STATUS "AETHER_FETCH_QT: installing Qt ${AETHER_QT_PIN_VERSION} via scripts/setup/setup-qt")
        execute_process(
            COMMAND ${_aether_fetch_cmd}
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE _aether_fetch_rc)
        if(NOT _aether_fetch_rc EQUAL 0)
            message(FATAL_ERROR "scripts/setup/setup-qt failed (exit ${_aether_fetch_rc}); see its output above.")
        endif()
    endif()
    if(EXISTS "${AETHER_QT_PIN_PREFIX}/lib/cmake/Qt6/Qt6Config.cmake")
        list(PREPEND CMAKE_PREFIX_PATH "${AETHER_QT_PIN_PREFIX}")
        message(STATUS "Using pinned Qt ${AETHER_QT_PIN_VERSION} from setup-qt: ${AETHER_QT_PIN_PREFIX}"
                       " (-DAETHER_USE_PINNED_QT=OFF to use another Qt)")
    endif()
endif()
