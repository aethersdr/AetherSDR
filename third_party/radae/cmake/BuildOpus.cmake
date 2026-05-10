message(STATUS "Will build vendored opus with FARGAN")

set(OPUS_VENDOR_DIR "${CMAKE_SOURCE_DIR}/third_party/opus-rade")
if (NOT DEFINED OPUS_PREPARED_ARCHIVE)
set(OPUS_PREPARED_ARCHIVE "${OPUS_VENDOR_DIR}/opus-rade-prepared.tar.gz")
endif (NOT DEFINED OPUS_PREPARED_ARCHIVE)
if (NOT DEFINED OPUS_PREPARED_ARCHIVE_HASH)
set(OPUS_PREPARED_ARCHIVE_HASH "SHA256=118539b194c82b3aedb843b4ef8499f223bd12c433871b80c4cbb853bcd463a7")
endif (NOT DEFINED OPUS_PREPARED_ARCHIVE_HASH)

if (NOT EXISTS "${OPUS_PREPARED_ARCHIVE}")
message(FATAL_ERROR "Vendored RADE Opus archive not found: ${OPUS_PREPARED_ARCHIVE}")
endif ()

include(ExternalProject)

set(OPUS_CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_SHARED_LIBS=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DOPUS_BUILD_TESTING=OFF
    -DOPUS_BUILD_PROGRAMS=OFF
    -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF
    -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF
    -DOPUS_DRED=ON
    -DOPUS_OSCE=ON
)

if (CMAKE_C_COMPILER)
list(APPEND OPUS_CMAKE_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
endif ()
if (CMAKE_MAKE_PROGRAM)
list(APPEND OPUS_CMAKE_ARGS -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM})
endif ()
if (DEFINED CMAKE_TOOLCHAIN_FILE AND NOT "${CMAKE_TOOLCHAIN_FILE}" STREQUAL "")
list(APPEND OPUS_CMAKE_ARGS -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
endif ()
if (APPLE AND DEFINED CMAKE_OSX_SYSROOT AND NOT "${CMAKE_OSX_SYSROOT}" STREQUAL "")
list(APPEND OPUS_CMAKE_ARGS -DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT})
endif ()
if (APPLE AND DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND NOT "${CMAKE_OSX_DEPLOYMENT_TARGET}" STREQUAL "")
list(APPEND OPUS_CMAKE_ARGS -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET})
endif ()

# ── Windows: build Opus via CMake ─────────────────────────────────────────
if(WIN32)
    # VS (multi-config) places opus.lib in <BINARY_DIR>/<Config>/; Ninja and
    # other single-config generators place it flat at <BINARY_DIR>/.  CI uses
    # Ninja, most local Windows ClangCL setups also use Ninja, while -T ClangCL
    # implies the VS generator — so both layouts are real and we must detect.
    get_property(_opus_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_opus_is_multi_config)
        set(_opus_byproducts
            <BINARY_DIR>/Debug/opus${CMAKE_STATIC_LIBRARY_SUFFIX}
            <BINARY_DIR>/Release/opus${CMAKE_STATIC_LIBRARY_SUFFIX}
            <BINARY_DIR>/RelWithDebInfo/opus${CMAKE_STATIC_LIBRARY_SUFFIX}
            <BINARY_DIR>/MinSizeRel/opus${CMAKE_STATIC_LIBRARY_SUFFIX})
    else()
        # Single-config: MSVC emits opus.lib, MinGW emits libopus.a — list both
        # so the generator picks whichever the toolchain produced.
        set(_opus_byproducts
            <BINARY_DIR>/opus${CMAKE_STATIC_LIBRARY_SUFFIX}
            <BINARY_DIR>/libopus${CMAKE_STATIC_LIBRARY_SUFFIX})
    endif()

    ExternalProject_Add(build_opus
        DOWNLOAD_EXTRACT_TIMESTAMP NO
        URL "${OPUS_PREPARED_ARCHIVE}"
        URL_HASH ${OPUS_PREPARED_ARCHIVE_HASH}
        CMAKE_ARGS
            ${OPUS_CMAKE_ARGS}
        BUILD_BYPRODUCTS ${_opus_byproducts}
        INSTALL_COMMAND ""
    )

    ExternalProject_Get_Property(build_opus BINARY_DIR)
    ExternalProject_Get_Property(build_opus SOURCE_DIR)
    add_library(opus STATIC IMPORTED)
    add_dependencies(opus build_opus)

    if(MSVC)
        if(_opus_is_multi_config)
            # $<CONFIG> is not evaluated in IMPORTED_LOCATION for imported targets;
            # set per-config properties for each standard VS build configuration.
            foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
                set(_lib "${BINARY_DIR}/${_cfg}/opus${CMAKE_STATIC_LIBRARY_SUFFIX}")
                set_target_properties(opus PROPERTIES
                    "IMPORTED_LOCATION_${_cfg}" "${_lib}"
                    "IMPORTED_IMPLIB_${_cfg}"   "${_lib}"
                )
            endforeach()
            # Fallback for any unlisted config — use RelWithDebInfo
            set_target_properties(opus PROPERTIES
                IMPORTED_LOCATION "${BINARY_DIR}/RelWithDebInfo/opus${CMAKE_STATIC_LIBRARY_SUFFIX}"
            )
        else()
            # Ninja / single-config MSVC: flat layout
            set(_opus_lib "${BINARY_DIR}/opus${CMAKE_STATIC_LIBRARY_SUFFIX}")
            set_target_properties(opus PROPERTIES
                IMPORTED_LOCATION "${_opus_lib}"
                IMPORTED_IMPLIB   "${_opus_lib}"
            )
        endif()
    else()
        set(_opus_lib "${BINARY_DIR}/libopus${CMAKE_STATIC_LIBRARY_SUFFIX}")
        set_target_properties(opus PROPERTIES
            IMPORTED_LOCATION "${_opus_lib}"
            IMPORTED_IMPLIB   "${_opus_lib}"
        )
    endif()

    include_directories(${SOURCE_DIR}/dnn ${SOURCE_DIR}/celt ${SOURCE_DIR}/include ${SOURCE_DIR})

# ── macOS universal: build twice and lipo ────────────────────────────────
elseif(APPLE AND BUILD_OSX_UNIVERSAL)
    ExternalProject_Add(build_opus_x86
        DOWNLOAD_EXTRACT_TIMESTAMP NO
        URL "${OPUS_PREPARED_ARCHIVE}"
        URL_HASH ${OPUS_PREPARED_ARCHIVE_HASH}
        CMAKE_ARGS
            ${OPUS_CMAKE_ARGS}
            -DCMAKE_OSX_ARCHITECTURES=x86_64
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS <BINARY_DIR>/libopus${CMAKE_STATIC_LIBRARY_SUFFIX}
    )
    ExternalProject_Add(build_opus_arm
        DOWNLOAD_EXTRACT_TIMESTAMP NO
        URL "${OPUS_PREPARED_ARCHIVE}"
        URL_HASH ${OPUS_PREPARED_ARCHIVE_HASH}
        CMAKE_ARGS
            ${OPUS_CMAKE_ARGS}
            -DCMAKE_OSX_ARCHITECTURES=arm64
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS <BINARY_DIR>/libopus${CMAKE_STATIC_LIBRARY_SUFFIX}
    )

    ExternalProject_Get_Property(build_opus_arm BINARY_DIR)
    ExternalProject_Get_Property(build_opus_arm SOURCE_DIR)
    set(OPUS_ARM_BINARY_DIR ${BINARY_DIR})
    ExternalProject_Get_Property(build_opus_x86 BINARY_DIR)
    set(OPUS_X86_BINARY_DIR ${BINARY_DIR})

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/libopus${CMAKE_STATIC_LIBRARY_SUFFIX}
        COMMAND lipo ${OPUS_ARM_BINARY_DIR}/libopus${CMAKE_STATIC_LIBRARY_SUFFIX} ${OPUS_X86_BINARY_DIR}/libopus${CMAKE_STATIC_LIBRARY_SUFFIX} -output ${CMAKE_CURRENT_BINARY_DIR}/libopus${CMAKE_STATIC_LIBRARY_SUFFIX} -create
        DEPENDS build_opus_arm build_opus_x86)

    add_custom_target(
        build_opus
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/libopus${CMAKE_STATIC_LIBRARY_SUFFIX})

    include_directories(${SOURCE_DIR}/dnn ${SOURCE_DIR}/celt ${SOURCE_DIR}/include ${SOURCE_DIR})

    add_library(opus STATIC IMPORTED)
    add_dependencies(opus build_opus)
    set_target_properties(opus PROPERTIES
        IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/libopus${CMAKE_STATIC_LIBRARY_SUFFIX}"
    )

# ── Unix/Linux/macOS single-arch: CMake build ────────────────────────────
else()
    ExternalProject_Add(build_opus
        DOWNLOAD_EXTRACT_TIMESTAMP NO
        URL "${OPUS_PREPARED_ARCHIVE}"
        URL_HASH ${OPUS_PREPARED_ARCHIVE_HASH}
        CMAKE_ARGS
            ${OPUS_CMAKE_ARGS}
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS <BINARY_DIR>/libopus${CMAKE_STATIC_LIBRARY_SUFFIX}
    )

    ExternalProject_Get_Property(build_opus BINARY_DIR)
    ExternalProject_Get_Property(build_opus SOURCE_DIR)
    add_library(opus STATIC IMPORTED)
    add_dependencies(opus build_opus)

    set_target_properties(opus PROPERTIES
        IMPORTED_LOCATION "${BINARY_DIR}/libopus${CMAKE_STATIC_LIBRARY_SUFFIX}"
        IMPORTED_IMPLIB   "${BINARY_DIR}/libopus${CMAKE_STATIC_LIBRARY_SUFFIX}"
    )

    include_directories(${SOURCE_DIR}/dnn ${SOURCE_DIR}/celt ${SOURCE_DIR}/include ${SOURCE_DIR})
endif()
