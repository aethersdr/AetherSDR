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
# The install is a generation directory named by <version>-<revision>.current
# (see setup-qt.sh); this resolves the pointer rather than assuming a path.
#
# Sets AETHER_QT_PIN_VERSION, AETHER_QT_PIN_REVISION and AETHER_QT_PIN_PREFIX
# (the live generation's Qt prefix, or empty when nothing is installed) for the
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
macro(_aether_read_pin_prefix)
    set(AETHER_QT_PIN_PREFIX "")
    set(_aether_ptr "${_aether_qt_cache}/${AETHER_QT_PIN_VERSION}-${AETHER_QT_PIN_REVISION}.current")
    if(EXISTS "${_aether_ptr}")
        file(STRINGS "${_aether_ptr}" _aether_gen LIMIT_COUNT 1)
        string(STRIP "${_aether_gen}" _aether_gen)
        if(_aether_gen)
            set(AETHER_QT_PIN_PREFIX
                "${_aether_qt_cache}/gen/${_aether_gen}/${AETHER_QT_PIN_VERSION}/${_aether_kit}")
        endif()
    endif()
endmacro()
_aether_read_pin_prefix()

# A build directory remembers the Qt it was configured against in Qt6_DIR,
# Qt6Core_DIR, ... When that is one of OUR generations but not the live one —
# superseded by a reinstall, pruned, or left behind by a pin bump (6.12.0 ->
# 6.12.1 leaves the old version's generation on disk) — the cached entry would read as "the
# builder chose a Qt": the cache below would be skipped and the build would
# silently stay on the superseded kit, or fall through to whatever else is
# installed. Drop exactly those entries — ours, and not live — so the build
# re-resolves to the live generation. A Qt the builder named outside our cache
# is never touched.
set(_aether_live_gen "")
if(AETHER_QT_PIN_PREFIX)
    set(_aether_live_gen "${_aether_qt_cache}/gen/${_aether_gen}/")
endif()
get_cmake_property(_aether_cache_vars CACHE_VARIABLES)
foreach(_v IN LISTS _aether_cache_vars)
    if(_v MATCHES "^Qt6.*_DIR$")
        string(FIND "${${_v}}" "${_aether_qt_cache}/gen/" _at)
        if(_at EQUAL 0)
            set(_live -1)
            if(_aether_live_gen)
                string(FIND "${${_v}}" "${_aether_live_gen}" _live)
            endif()
            if(NOT EXISTS "${${_v}}" OR NOT _live EQUAL 0)
                unset(${_v} CACHE)
            endif()
        endif()
    endif()
endforeach()

# The same trap, from outside our cache: a build directory that once
# auto-found a distro Qt below the floor (say Arch's 6.11) keeps it in
# Qt6_DIR. That kit can never configure this tree, and leaving it cached would
# make the error below repeat even after setup-qt.sh has installed the release
# Qt. So a cached kit below QT_SOURCE_FLOOR is dropped, with every Qt6*_DIR
# entry it brought, and the build re-resolves. Read from the kit's own
# Qt6ConfigVersion*.cmake rather than include()d, which would leak its
# variables into this scope.
if(Qt6_DIR AND _aether_pin_QT_SOURCE_FLOOR)
    file(GLOB _aether_cfgver "${Qt6_DIR}/Qt6ConfigVersion*.cmake")
    set(_aether_cached_qt "")
    foreach(_f IN LISTS _aether_cfgver)
        file(STRINGS "${_f}" _aether_line REGEX "^set\\(PACKAGE_VERSION \"[0-9.]+\"\\)")
        if(_aether_line MATCHES "\"([0-9.]+)\"")
            set(_aether_cached_qt "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(_aether_cached_qt AND _aether_cached_qt VERSION_LESS _aether_pin_QT_SOURCE_FLOOR)
        message(STATUS "Cached Qt ${_aether_cached_qt} (${Qt6_DIR}) is below the "
                       "${_aether_pin_QT_SOURCE_FLOOR} floor; dropping it from this build directory")
        get_cmake_property(_aether_cache_vars CACHE_VARIABLES)
        foreach(_v IN LISTS _aether_cache_vars)
            if(_v MATCHES "^Qt6.*_DIR$")
                unset(${_v} CACHE)
            endif()
        endforeach()
    endif()
endif()

# Has the builder already pointed CMake at a Qt? Qt6_DIR / Qt6_ROOT name one
# outright. A CMAKE_PREFIX_PATH entry names one when find_package() would find
# a Qt6 config under it — so probe with find_package's own per-prefix layouts
# (cmake-packages(7), "Config Mode Search Procedure"), not just lib/cmake/Qt6:
# Debian and Ubuntu install to lib/<multiarch-triplet>/cmake/Qt6, and an
# explicit -DCMAKE_PREFIX_PATH=/usr there must still win over the cache.
# Globbing rather than calling find_package(): loading Qt6Config here would
# pin Qt6_DIR before CMakeLists.txt's own probe gets to run.
function(_aether_prefix_has_qt6 prefix out)
    set(${out} FALSE PARENT_SCOPE)
    if(NOT prefix)
        return()
    endif()
    set(_dirs
        "${prefix}" "${prefix}/cmake" "${prefix}/CMake"
        "${prefix}/Qt6*" "${prefix}/Qt6*/cmake" "${prefix}/Qt6*/CMake")
    foreach(_lib IN ITEMS "lib/*" "lib*" "share")
        list(APPEND _dirs
            "${prefix}/${_lib}/cmake/Qt6*"
            "${prefix}/${_lib}/Qt6*"
            "${prefix}/${_lib}/Qt6*/cmake" "${prefix}/${_lib}/Qt6*/CMake")
    endforeach()
    foreach(_d IN LISTS _dirs)
        file(GLOB _hits LIST_DIRECTORIES false
            "${_d}/Qt6Config.cmake" "${_d}/qt6-config.cmake")
        if(_hits)
            set(${out} TRUE PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

set(_aether_qt_chosen FALSE)
if(Qt6_DIR OR DEFINED ENV{Qt6_DIR} OR Qt6_ROOT OR DEFINED ENV{Qt6_ROOT})
    set(_aether_qt_chosen TRUE)
endif()
set(_aether_env_prefixes "$ENV{CMAKE_PREFIX_PATH}")
if(NOT WIN32)
    string(REPLACE ":" ";" _aether_env_prefixes "${_aether_env_prefixes}")
endif()
foreach(_p IN LISTS CMAKE_PREFIX_PATH _aether_env_prefixes)
    _aether_prefix_has_qt6("${_p}" _hit)
    if(_hit)
        set(_aether_qt_chosen TRUE)
    endif()
endforeach()

if(AETHER_USE_PINNED_QT AND NOT _aether_qt_chosen)
    if(NOT (AETHER_QT_PIN_PREFIX AND EXISTS "${AETHER_QT_PIN_PREFIX}/lib/cmake/Qt6/Qt6Config.cmake")
       AND AETHER_FETCH_QT)
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
        _aether_read_pin_prefix()
    endif()
    if(AETHER_QT_PIN_PREFIX AND EXISTS "${AETHER_QT_PIN_PREFIX}/lib/cmake/Qt6/Qt6Config.cmake")
        list(PREPEND CMAKE_PREFIX_PATH "${AETHER_QT_PIN_PREFIX}")
        message(STATUS "Using pinned Qt ${AETHER_QT_PIN_VERSION} from setup-qt: ${AETHER_QT_PIN_PREFIX}"
                       " (-DAETHER_USE_PINNED_QT=OFF to use another Qt)")
    endif()
endif()
