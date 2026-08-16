cmake_minimum_required(VERSION 3.25)

# CMake 3.29+ can safely de-duplicate libraries for linkers that support it.
# This avoids duplicate-library warnings from Apple's linker while preserving
# compatibility with the project's CMake 3.25 minimum.
if(POLICY CMP0156)
    cmake_policy(SET CMP0156 NEW)
endif()

project(AetherSDR VERSION 26.8.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Libraries options — opt-in system-package replacements for vendored
# dependencies.  For dependencies enabled in a build, these stay off by
# default so CI, releases, and contributor laptops use the bundled,
# version-pinned third_party/ snapshots.  Distro packagers turn these ON to
# match their dynamic-linking policy.

option(USE_SYSTEM_ZLIB         "Use system zlib"         OFF)
option(USE_SYSTEM_MSPACK       "Use system libmspack"    OFF)
option(USE_SYSTEM_LIBMOSQUITTO "Use system libmosquitto" OFF)
option(USE_SYSTEM_RTMIDI       "Use system RtMidi"       OFF)
option(USE_SYSTEM_LIBWHISPER   "Use system libwhisper"   OFF)

# Build features options

option(REQUIRE_SERIALPORT "Fail if Qt6 SerialPort is not found (use for release builds)" OFF)
option(REQUIRE_KEYCHAIN "Fail if Qt6Keychain is not found (use for release builds)" OFF)
# ASR release guards: these features auto-detect and silently compile out if the
# toolchain/lib is missing, so a release could ship them off unnoticed. Setting
# these makes a missing dependency a hard configure error (use in release CI).
option(REQUIRE_ASR_ONNX "Fail if ONNX Runtime is not found (Silero VAD + speaker labeling + signal classifier; use for release builds)" OFF)
option(REQUIRE_ASR_GPU "Fail if no ASR GPU backend (Vulkan/Metal) is enabled (use for release builds)" OFF)
option(REQUIRE_ASR_SHERPA "Fail if sherpa-onnx is not found (non-whisper ASR backend; use for release builds)" OFF)
option(ENABLE_RADE "Build with RADE digital voice support (uses vendored Opus snapshot)" ON)
option(ENABLE_DSTAR "Build the local D-STAR waveform helper (vendored smartsdr-dsp/ThumbDV)" ON)
option(AETHER_GPU_SPECTRUM "Enable QRhi GPU spectrum rendering" ON)
option(ENABLE_SPECBLEACH "Enable NR4 spectral bleach noise reduction" ON)
option(ENABLE_DFNR "Enable DFNR DeepFilterNet3 noise reduction" ON)
# ON by default (like the other NR engines) so the pre-compiled release images
# all ship BNR — the runtime is download-on-demand, so this only compiles a small
# dlopen wrapper (no NVIDIA SDK at build time). Compiled on Linux/Windows only
# (see the platform gate below; macOS has no AFX runtime and uses DFNR). Pass
# -DENABLE_NVIDIA_AFX=OFF to opt out.
option(ENABLE_NVIDIA_AFX "Enable the NVIDIA Maxine AFX GPU denoiser (BNR; runtime-loaded, download-on-demand; NVIDIA RTX/GeForce, Linux/Windows)" ON)
# ON by default so release images ship ASR. Weights are NOT bundled — they are
# downloaded on first enable (RFC #4333), so this only compiles the vendored
# whisper.cpp/ggml CPU engine. GPU/Metal backends are trimmed for now (Phase 1
# is CPU-only); see third_party/whisper.cpp/AETHER_VENDORING.md. Opt out with
# -DENABLE_ASR=OFF.
option(ENABLE_ASR "Enable on-device speech-to-text (ASR) via vendored whisper.cpp" ON)
# Windows-only: consume prebuilt whisper/ggml/ggml-vulkan static libs from the
# whisper-gpu-<ver> release asset instead of compiling from source. Bypasses
# the 4h step-timeout on stock GitHub windows-latest runners (4 core / 16 GB)
# where the ggml-vulkan Release compile alone exceeds the budget. Local dev
# builds should leave this OFF and compile from source.
option(ASR_USE_PREBUILT_WHISPER_GPU
    "Windows CI: consume prebuilt whisper-gpu asset instead of building from source" OFF)
option(ENABLE_MQTT "Enable MQTT client support" ON)
option(MQTT_TLS "Enable MQTT TLS via OpenSSL (disable for AppImage)" ON)
option(LOWER_CASE_BINARY_NAME "Make the output binary lower case. Only affects Linux" OFF)
if(WIN32)
    set(AETHER_EMBED_DFNR_MODEL_DEFAULT ON)
else()
    set(AETHER_EMBED_DFNR_MODEL_DEFAULT OFF)
endif()
option(AETHER_EMBED_DFNR_MODEL
       "Embed the DFNR model payload into application resources instead of deploying a loose archive"
       ${AETHER_EMBED_DFNR_MODEL_DEFAULT})

# macOS: ensure Homebrew lib/include paths are in the search path
# (universal builds with CMAKE_OSX_ARCHITECTURES may not search /opt/homebrew by default)
if(APPLE)
    execute_process(COMMAND brew --prefix OUTPUT_VARIABLE HOMEBREW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(HOMEBREW_PREFIX)
        link_directories("${HOMEBREW_PREFIX}/lib")
        include_directories("${HOMEBREW_PREFIX}/include")
    endif()
endif()

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Qt6 components
# Qt 6.8 minimum, matching the Qt every shipped artifact is built against:
# the AppImage (both arches, .github/workflows/appimage.yml), the Windows
# installer (check-windows pins 6.8.3 in ci.yml), and the Linux CI image
# (.github/docker/Dockerfile installs 6.8.3 via aqt). One number, so a Qt API
# that compiles in CI also compiles for a release build.
#
# Raised from 6.2 deliberately. The old floor was nominal — nothing tested it
# after Ubuntu 22.04 left the matrix, and the only leg anywhere near it was the
# CI image on Ubuntu 24.04's Qt 6.4.2, a release EOL upstream since the 6.4
# series ended at 6.4.3. That gap rejected valid code (PR #4646, QList::assign)
# while testing a Qt no artifact shipped.
#
# Consequence for source builds: Ubuntu 24.04's distro Qt (6.4.2) no longer
# satisfies this — build against 6.8+ from aqt/Qt online installer, or use a
# distro carrying it (Debian Trixie, Ubuntu 25.10+, Fedora 41+, Arch).
# 6.8 also clears QRhiWidget (6.7+), so the GPU spectrum path is no longer
# conditionally compiled out on Linux CI.
#
# Probe QUIET first so a below-floor Qt gets an actionable error instead of
# CMake's bare "the version found is not compatible", which names neither the
# floor as a project decision nor the way out. Ubuntu 24.04 LTS (Qt 6.4.2) is
# the common case, it has the largest install base of any supported distro, and
# it was a documented apt line here until this floor moved — so the people most
# likely to hit this have the least context for it, and the README they need is
# a document they have already scrolled past.
#
# Falls through untouched when Qt is absent entirely (the REQUIRED call below
# emits the normal not-found error) or already satisfies the floor.
find_package(Qt6 QUIET COMPONENTS Core)
if(Qt6_FOUND AND Qt6_VERSION VERSION_LESS 6.8)
    message(FATAL_ERROR
        "Qt ${Qt6_VERSION} found, but AetherSDR requires Qt 6.8 or newer.\n"
        "This is the Qt every AetherSDR release binary is built against.\n"
        "Ubuntu 24.04 LTS ships 6.4.2; Debian Trixie, Ubuntu 25.10+, "
        "Fedora 41+ and Arch all ship 6.8 or newer.\n"
        "To build on a distro below the floor, install Qt 6.8+ with aqtinstall "
        "or the Qt online installer and re-run with "
        "-DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/gcc_64 — see the README's "
        "\"Building from Source\" dependency section.")
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS
    Core
    Concurrent
    Widgets
    Network
    Multimedia
    Test
)
# zlib is bundled under third_party/zlib (1.3.1) for parity with the
# libmosquitto bundling pattern (#699) and per the constitution's
# Technology Constraint preferring bundled libraries over package
# managers.  The vcpkg dependency on Windows + system-zlib on Linux/macOS
# is gone — single source-of-truth across all platforms. (#2651)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "Disable zlib examples" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "Don't install zlib" FORCE)

if (USE_SYSTEM_ZLIB)
    find_package(PkgConfig REQUIRED)
    if(PkgConfig_FOUND)
        pkg_check_modules(zlib REQUIRED IMPORTED_TARGET zlib)
    endif()
else()
    add_subdirectory(third_party/zlib EXCLUDE_FROM_ALL)
endif()

# Note: on Windows the SerialPortController uses raw Win32 WaitCommEvent for
# DSR/CTS edge detection (FTDI VCP drivers don't refresh
# GetCommModemStatus outside a WaitCommEvent completion).  Qt6::SerialPort
# is still useful on Linux/macOS for pin polling, so the find_package call
# stays platform-agnostic.
if(REQUIRE_SERIALPORT)
    find_package(Qt6 REQUIRED COMPONENTS SerialPort)
else()
    find_package(Qt6 QUIET COMPONENTS SerialPort)
endif()
if(Qt6SerialPort_FOUND)
    message(STATUS "Qt6::SerialPort found — serial PTT/CW support enabled")
else()
    message(STATUS "Qt6::SerialPort not found — serial PTT/CW support disabled")
endif()
find_package(Qt6 QUIET COMPONENTS WebSockets)
if(Qt6WebSockets_FOUND)
    message(STATUS "Qt6::WebSockets found — FreeDV Reporter spot source enabled")
else()
    message(STATUS "Qt6::WebSockets not found — FreeDV Reporter spot source disabled")
endif()
if(UNIX AND NOT APPLE)
    find_package(Qt6 QUIET COMPONENTS DBus)
    if(Qt6DBus_FOUND)
        message(STATUS "Qt6::DBus found — sleep inhibition via D-Bus enabled")
    else()
        message(STATUS "Qt6::DBus not found — sleep inhibition disabled on Linux")
    endif()
endif()
# qtkeychain may be staged into third_party/ by scripts/setup/setup-qtkeychain.*
# (Windows + Linux AppImage release builds build it from source there). Prepend
# so find_package resolves it before any system install. This EXISTS-guarded
# prepend supersedes the earlier WIN32-only one (#3634): Windows stages into the
# same dir, so the general form covers both platforms.
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/qtkeychain")
    list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/third_party/qtkeychain")
endif()
# REQUIRE_KEYCHAIN makes a missing Qt6Keychain a hard build failure rather than
# a silent compile-out of SmartLink credential persistence — release/packaged
# builds set it so an artifact can never ship without working persistence (#3639).
if(REQUIRE_KEYCHAIN)
    find_package(Qt6Keychain REQUIRED)
else()
    find_package(Qt6Keychain QUIET)
endif()
if(Qt6Keychain_FOUND)
    message(STATUS "Qt6Keychain found — credential persistence enabled")
else()
    message(STATUS "Qt6Keychain not found — credential persistence disabled")
endif()

# PortAudio (optional fallback for audio)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PORTAUDIO portaudio-2.0)
endif()

# FFTW3 (required by vendored WDSP; also used by NR2 spectral noise reduction)
# Windows: run scripts/setup/setup-fftw.ps1 first to download prebuilt DLLs
# Linux:   apt install libfftw3-dev
# macOS:   brew install fftw
if(WIN32)
    set(FFTW3_ROOT "${CMAKE_SOURCE_DIR}/third_party/fftw3")
    if(EXISTS "${FFTW3_ROOT}/include/fftw3.h")
        set(FFTW3_FOUND TRUE)
        set(FFTW3_INCLUDE_DIRS "${FFTW3_ROOT}/include")
        set(FFTW3_LIBRARIES "${FFTW3_ROOT}/lib/fftw3.lib")
        set(FFTW3_DLL "${FFTW3_ROOT}/bin/libfftw3-3.dll")
    endif()   # not-found is handled by the unified FATAL_ERROR below
else()
    if(PkgConfig_FOUND)
        pkg_check_modules(FFTW3 fftw3)
    endif()
    if(NOT FFTW3_FOUND)
        find_library(FFTW3_LIB fftw3)
        find_path(FFTW3_INC fftw3.h)
        if(FFTW3_LIB AND FFTW3_INC)
            set(FFTW3_FOUND TRUE)
            set(FFTW3_LIBRARIES ${FFTW3_LIB})
            set(FFTW3_INCLUDE_DIRS ${FFTW3_INC})
        endif()
    endif()
endif()

# The vendored WDSP DSP library (third_party/wdsp) hard-requires FFTW3 — unlike
# NR2, which had a fallback FFT. Fail at configure with a clear message instead
# of letting the build proceed to a cryptic "undefined FFTW symbol" link error.
if(NOT FFTW3_FOUND)
    message(FATAL_ERROR
        "FFTW3 is required by the vendored WDSP DSP library (third_party/wdsp) but "
        "was not found. Install it and reconfigure — Linux: apt install libfftw3-dev; "
        "macOS: brew install fftw; Windows: run scripts/setup/setup-fftw.ps1.")
endif()

# Bundled RADE (BSD-2) — FreeDV Radio Autoencoder digital voice codec
# Opus (with FARGAN/LPCNet) is built from a vendored local snapshot via ExternalProject
set(RADE_DIR ${CMAKE_SOURCE_DIR}/third_party/radae)
option(RADE_WAV_TAP "Write diagnostic WAV files at RADE TX tap points (Taps A/B/D/E/F) for offline demod analysis" OFF)
if(ENABLE_RADE AND EXISTS "${RADE_DIR}/src/rade_api.h")
    # macOS universal binary: build Opus for both architectures
    if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64.*arm64|arm64.*x86_64")
        set(BUILD_OSX_UNIVERSAL ON)
    endif()
    include(${RADE_DIR}/cmake/BuildOpus.cmake)
    set(RADE_SOURCES
        ${RADE_DIR}/src/rade_api_nopy.c
        ${RADE_DIR}/src/rade_dsp.c
        ${RADE_DIR}/src/rade_ofdm.c
        ${RADE_DIR}/src/rade_bpf.c
        ${RADE_DIR}/src/rade_acq.c
        ${RADE_DIR}/src/rade_tx.c
        ${RADE_DIR}/src/rade_rx.c
        ${RADE_DIR}/src/rade_enc.c
        ${RADE_DIR}/src/rade_dec.c
        ${RADE_DIR}/src/rade_enc_data.c
        ${RADE_DIR}/src/rade_dec_data.c
        ${RADE_DIR}/src/kiss_fft.c
        ${RADE_DIR}/src/kiss_fftr.c
        # codec2-derived LDPC + GP-interleaver for EOO callsign (rade_text)
        ${RADE_DIR}/src/mpdecode_core.c
        ${RADE_DIR}/src/gp_interleaver.c
        ${RADE_DIR}/src/HRA_56_56.c
        ${RADE_DIR}/src/ldpc_codes.c
        ${RADE_DIR}/src/rade_text.c
    )
    if(MSVC)
        set(RADE_WARN_FLAG "/w")
    else()
        set(RADE_WARN_FLAG "-w")
    endif()
    set_source_files_properties(${RADE_SOURCES} PROPERTIES
        COMPILE_FLAGS "${RADE_WARN_FLAG} -DIS_BUILDING_RADE_API=1 -DRADE_PYTHON_FREE=1"
        INCLUDE_DIRECTORIES "${RADE_DIR}/src"
    )
    set(RADE_FOUND TRUE)
    message(STATUS "RADE enabled (bundled vendored Opus snapshot)")

    # Standalone offline diagnostic tool: demodulate a WAV file and decode the EOO callsign.
    # Guarded by source-file existence so builds against vendored RADE snapshots that
    # predate rade_demod_wav.c don't hard-fail. Independent of RADE_WAV_TAP.
    # Excluded on WIN32: rade_demod_wav.c uses POSIX getopt.h, unavailable in MSVC's SDK.
    if(EXISTS "${RADE_DIR}/src/rade_demod_wav.c" AND NOT WIN32)
        add_executable(rade_demod_wav
            ${RADE_SOURCES}
            ${RADE_DIR}/src/rade_demod_wav.c
        )
        set_source_files_properties(${RADE_DIR}/src/rade_demod_wav.c PROPERTIES
            COMPILE_FLAGS "${RADE_WARN_FLAG} -DIS_BUILDING_RADE_API=1 -DRADE_PYTHON_FREE=1"
            INCLUDE_DIRECTORIES "${RADE_DIR}/src"
        )
        target_include_directories(rade_demod_wav PRIVATE ${RADE_DIR}/src)
        target_link_libraries(rade_demod_wav PRIVATE opus
            $<$<NOT:$<PLATFORM_ID:Windows>>:m>)
        add_dependencies(rade_demod_wav build_opus)
    endif()

else()
    set(RADE_FOUND FALSE)
    set(RADE_SOURCES "")
    if(ENABLE_RADE)
        message(STATUS "RADE source not found at ${RADE_DIR}. Digital voice disabled.")
    else()
        message(STATUS "RADE disabled by ENABLE_RADE=OFF.")
    endif()
endif()

# Opus codec — required for SmartLink compressed audio, independent of RADE.
# When RADE is enabled, Opus comes from AetherSDR's vendored RADE snapshot
# (with FARGAN/OSCE).
# When RADE is disabled, find system libopus.
# Windows: run scripts/setup/setup-opus.ps1 first to download prebuilt DLLs
# Linux:   apt install libopus-dev
# macOS:   brew install opus
if(RADE_FOUND)
    set(OPUS_FOUND TRUE)
    message(STATUS "Opus: using RADE's bundled build")
elseif(WIN32)
    set(OPUS_ROOT "${CMAKE_SOURCE_DIR}/third_party/opus")
    if(EXISTS "${OPUS_ROOT}/include/opus/opus.h")
        set(OPUS_FOUND TRUE)
        set(OPUS_INCLUDE_DIRS "${OPUS_ROOT}/include/opus")
        set(OPUS_LIBRARIES "${OPUS_ROOT}/lib/opus.lib")
        message(STATUS "Opus: using prebuilt Windows library (static)")
    else()
        set(OPUS_FOUND FALSE)
        message(WARNING "Opus not found. Run scripts/setup/setup-opus.ps1 to download it. "
                        "SmartLink compressed audio will be unavailable.")
    endif()
else()
    if(PkgConfig_FOUND)
        pkg_check_modules(OPUS opus)
    endif()
    if(NOT OPUS_FOUND)
        find_library(OPUS_LIB opus)
        find_path(OPUS_INC opus/opus.h)
        if(OPUS_LIB AND OPUS_INC)
            set(OPUS_FOUND TRUE)
            set(OPUS_LIBRARIES ${OPUS_LIB})
            set(OPUS_INCLUDE_DIRS "${OPUS_INC}/opus")
        endif()
    endif()
    if(OPUS_FOUND)
        message(STATUS "Opus: using system libopus")
    else()
        message(WARNING "Opus not found — SmartLink compressed audio will be unavailable. "
                        "Install libopus-dev (Debian/Ubuntu), opus (Arch/vcpkg), or brew install opus (macOS).")
    endif()
endif()

# GPU-accelerated spectrum/waterfall rendering via QRhi (#391)
# Requires: Qt 6.7+ (QRhiWidget), Qt6::ShaderTools (build-time shader compilation)
# The version guard below is unreachable now that find_package demands 6.8 — it
# is kept as a cheap backstop so lowering the floor again can't silently produce
# a build that references QRhiWidget on a Qt that lacks it.
if(AETHER_GPU_SPECTRUM)
    if(Qt6_VERSION VERSION_LESS "6.7")
        set(AETHER_GPU_SPECTRUM OFF)
        # 6.8.3, not 6.7.x, in the suggested path: this guard is unreachable
        # while the floor is 6.8, so if it ever fires the floor has been
        # lowered — and pointing that reader at a 6.7 install would send them
        # to a Qt find_package still rejects.
        message(STATUS "GPU spectrum rendering disabled (QRhiWidget requires Qt 6.7+, found ${Qt6_VERSION} — pass -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/gcc_64 to use a newer Qt installation)")
    else()
        find_package(Qt6 REQUIRED COMPONENTS ShaderTools)
        # Qt6GuiPrivate provides QRhi headers needed for GPU rendering.
        find_package(Qt6GuiPrivate QUIET)

        # Some Qt distributions ship the private QtGui headers on disk but omit the
        # Qt6GuiPrivate CMake package (so no Qt6::GuiPrivate target): Debian multi-arch,
        # and Windows/macOS installs deployed via aqtinstall. Locate the private
        # include root directly and wire it up manually (see DEBIAN_GPU_FIX_REQUIRED).
        if(NOT Qt6GuiPrivate_FOUND)
            message(STATUS "Qt6GuiPrivate CMake package not found, searching for private headers directly...")
            set(DEB_HOST_MULTIARCH "")
            find_program(DPKG_ARCH dpkg-architecture)
            if(DPKG_ARCH)
                execute_process(COMMAND ${DPKG_ARCH} -qDEB_HOST_MULTIARCH
                    OUTPUT_VARIABLE DEB_HOST_MULTIARCH OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
            endif()
            # Derive the Qt include root from the Gui target — covers Windows/macOS SDK
            # layouts (<prefix>/include) where dpkg multi-arch paths don't apply.
            set(_qt_gui_inc_root "")
            if(TARGET Qt6::Gui)
                get_target_property(_qt_gui_incs Qt6::Gui INTERFACE_INCLUDE_DIRECTORIES)
                foreach(_inc IN LISTS _qt_gui_incs)
                    if(EXISTS "${_inc}/QtGui/${Qt6_VERSION}/QtGui/private/qhighdpiscaling_p.h")
                        set(_qt_gui_inc_root "${_inc}")
                        break()
                    endif()
                endforeach()
            endif()
            find_path(DEBIAN_PRIVATE_INC
                NAMES "QtGui/${Qt6_VERSION}/QtGui/private/qhighdpiscaling_p.h"
                PATHS
                    "${_qt_gui_inc_root}"
                    "/usr/include/${DEB_HOST_MULTIARCH}/qt6" "/usr/include/qt6"
                NO_DEFAULT_PATH
            )
            if(DEBIAN_PRIVATE_INC)
                set(Qt6GuiPrivate_FOUND TRUE)
                set(DEBIAN_GPU_FIX_REQUIRED TRUE) # Mark for Step 2 (manual include dirs)
            endif()
            # macOS framework layout. Qt's own macOS builds — the official
            # installer and aqtinstall alike — ship QtGui as a framework, so the
            # private headers live at QtGui.framework/Headers/<ver>/QtGui, one
            # path component short of the SDK layout probed above (which expects
            # a further QtGui/ beneath the include root). They also ship no
            # Qt6GuiPrivate CMake package at all, so nothing above finds them.
            #
            # This is not a hypothetical layout: pinning the Apple Silicon DMG
            # to aqt Qt 6.8.3 (#4688 §1) moved that leg off Homebrew's Qt, which
            # *does* provide Qt6GuiPrivate — and AETHER_GPU_SPECTRUM promptly
            # turned itself OFF, with the CPU QPainter fallback compiled in and
            # the workflow green. Only the #4690 assertion caught it. Probe the
            # framework layout here so the gate agrees with the wiring in the
            # AETHER_GPU_SPECTRUM block below, which has always had a branch for
            # this shape and could never be reached.
            if(NOT Qt6GuiPrivate_FOUND AND APPLE)
                foreach(_inc IN LISTS _qt_gui_incs)
                    if(EXISTS "${_inc}/${Qt6_VERSION}/QtGui/private/qhighdpiscaling_p.h")
                        set(QT_FRAMEWORK_PRIVATE_INC "${_inc}/${Qt6_VERSION}")
                        set(Qt6GuiPrivate_FOUND TRUE)
                        break()
                    endif()
                endforeach()
            endif()
        endif()

        if(Qt6GuiPrivate_FOUND)
            message(STATUS "GPU spectrum rendering enabled (QRhi, Qt ${Qt6_VERSION})")
        else()
            set(AETHER_GPU_SPECTRUM OFF)
            message(STATUS "GPU spectrum rendering disabled — Qt6GuiPrivate not found "
                           "(install qt6-base-private-dev / qt6-qtbase-private-devel)")
        endif()
    endif()
else()
    message(STATUS "GPU spectrum rendering disabled (use -DAETHER_GPU_SPECTRUM=ON to enable)")
endif()

# libspecbleach NR4 — bundled spectral noise reduction (LGPL-2.1)
# MSVC: libspecbleach uses C99 VLAs and __attribute__ which MSVC doesn't support.
# When clang-cl is available, build as a separate static lib using it.
if(MSVC)
    find_program(CLANG_CL clang-cl HINTS "C:/Program Files/LLVM/bin")
    if(NOT CLANG_CL)
        set(ENABLE_SPECBLEACH OFF CACHE BOOL "Enable NR4 spectral bleach noise reduction" FORCE)
    endif()
endif()
if(ENABLE_SPECBLEACH)
    file(GLOB_RECURSE SPECBLEACH_SOURCES third_party/libspecbleach/src/*.c)
    if(MSVC AND CLANG_CL)
        # Pre-build specbleach.lib with clang-cl (supports VLAs and MSVC ABI)
        set(SPECBLEACH_BUILD_DIR "${CMAKE_BINARY_DIR}/specbleach")
        file(MAKE_DIRECTORY ${SPECBLEACH_BUILD_DIR})
        set(SPECBLEACH_STATIC_LIB "${SPECBLEACH_BUILD_DIR}/specbleach.lib")
        # When -T ClangCL is used CMAKE_C_COMPILER is the VS-bundled clang-cl which
        # auto-detects the Windows SDK and MSVC include paths. Prefer it over the
        # standalone LLVM found by find_program (which may not locate stdint.h in a
        # MSBuild custom-command environment).
        if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
            set(_specbleach_cc "${CMAKE_C_COMPILER}")
        else()
            set(_specbleach_cc "${CLANG_CL}")
        endif()
        add_custom_command(
            OUTPUT ${SPECBLEACH_STATIC_LIB}
            COMMAND ${_specbleach_cc} -w --target=x86_64-pc-windows-msvc
                -DFFTW_DLL
                -I${CMAKE_SOURCE_DIR}/third_party/libspecbleach/include
                -I${CMAKE_SOURCE_DIR}/third_party/libspecbleach/src
                -I${CMAKE_SOURCE_DIR}/third_party/fftw3/include
                -c ${SPECBLEACH_SOURCES}
            COMMAND lib /nologo /out:specbleach.lib *.obj
            WORKING_DIRECTORY ${SPECBLEACH_BUILD_DIR}
            DEPENDS ${SPECBLEACH_SOURCES}
            COMMENT "Building libspecbleach with clang-cl"
        )
        add_custom_target(specbleach_build DEPENDS ${SPECBLEACH_STATIC_LIB})
        set(SPECBLEACH_SOURCES "")
        message(STATUS "NR4 (libspecbleach) enabled — building with clang-cl")
    else()
        message(STATUS "NR4 (libspecbleach) enabled — ${CMAKE_SOURCE_DIR}/third_party/libspecbleach")
    endif()
else()
    set(SPECBLEACH_SOURCES "")
    if(MSVC AND NOT CLANG_CL)
        message(STATUS "NR4 (libspecbleach) disabled — install LLVM for clang-cl VLA support")
    else()
        message(STATUS "NR4 (libspecbleach) disabled")
    endif()
endif()

# DeepFilterNet3 DFNR — bundled neural noise reduction (MIT/Apache-2.0)
# Pre-built libdf library from the Rust crate; model payload is handled below.
if(ENABLE_DFNR)
    set(DEEPFILTER_DIR ${CMAKE_SOURCE_DIR}/third_party/deepfilter)
    set(DFNR_MODEL "${DEEPFILTER_DIR}/models/DeepFilterNet3_onnx.tar.gz")
    set(DFNR_EMBEDDED_MODEL_NAME "DeepFilterNet3_onnx.dfmodel")
    if(WIN32)
        set(DFNR_LIB_DIR "${DEEPFILTER_DIR}/lib/windows-x86_64")
        if(MINGW)
            set(DFNR_LIB "${DFNR_LIB_DIR}/libdeepfilter.dll.a")
            set(DFNR_DLL "${DFNR_LIB_DIR}/deepfilter.dll")
        else()
            # MSVC: prefer .dll.lib import library, fall back to .lib
            if(EXISTS "${DFNR_LIB_DIR}/deepfilter.dll.lib")
                set(DFNR_LIB "${DFNR_LIB_DIR}/deepfilter.dll.lib")
            else()
                set(DFNR_LIB "${DFNR_LIB_DIR}/deepfilter.lib")
            endif()
            set(DFNR_DLL "${DFNR_LIB_DIR}/deepfilter.dll")
        endif()
    elseif(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
            set(DFNR_LIB_DIR "${DEEPFILTER_DIR}/lib/darwin-arm64")
        else()
            set(DFNR_LIB_DIR "${DEEPFILTER_DIR}/lib/darwin-x86_64")
        endif()
        set(DFNR_LIB "${DFNR_LIB_DIR}/libdeepfilter.a")
    else()
        if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
            set(DFNR_LIB_DIR "${DEEPFILTER_DIR}/lib/linux-aarch64")
        else()
            set(DFNR_LIB_DIR "${DEEPFILTER_DIR}/lib/linux-x86_64")
        endif()
        set(DFNR_LIB "${DFNR_LIB_DIR}/libdeepfilter.a")
    endif()
    if(EXISTS ${DFNR_LIB})
        message(STATUS "DFNR (DeepFilterNet3) enabled — ${DFNR_LIB}")
    else()
        set(ENABLE_DFNR OFF)
        message(STATUS "DFNR (DeepFilterNet3) disabled — library not found at ${DFNR_LIB} "
                       "(run ./scripts/setup/setup-deepfilter.sh before cmake to enable)")
    endif()
endif()

# MQTT client support — bundled libmosquitto (#699)
if(ENABLE_MQTT)
    if (USE_SYSTEM_LIBMOSQUITTO)
        find_package(PkgConfig REQUIRED)
        if(PkgConfig_FOUND)
            pkg_check_modules(libmosquitto REQUIRED IMPORTED_TARGET libmosquitto)
        endif()
    else()
        if(MQTT_TLS)
            find_package(OpenSSL)
        endif()
        set(MOSQUITTO_DIR ${CMAKE_SOURCE_DIR}/third_party/mosquitto)
        file(GLOB MOSQUITTO_SOURCES ${MOSQUITTO_DIR}/src/*.c)
        # Remove broker-only and optional files we don't need
        list(FILTER MOSQUITTO_SOURCES EXCLUDE REGEX "socks_mosq\\.c$")
        list(FILTER MOSQUITTO_SOURCES EXCLUDE REGEX "http_client\\.c$")
        list(FILTER MOSQUITTO_SOURCES EXCLUDE REGEX "picohttpparser\\.c$")
        list(FILTER MOSQUITTO_SOURCES EXCLUDE REGEX "extended_auth\\.c$")
        list(FILTER MOSQUITTO_SOURCES EXCLUDE REGEX "cjson_common\\.c$")
        list(FILTER MOSQUITTO_SOURCES EXCLUDE REGEX "password_common\\.c$")
        list(FILTER MOSQUITTO_SOURCES EXCLUDE REGEX "base64_common\\.c$")
        list(FILTER MOSQUITTO_SOURCES EXCLUDE REGEX "json_help\\.c$")
        list(FILTER MOSQUITTO_SOURCES EXCLUDE REGEX "srv_mosq\\.c$")
        if(OpenSSL_FOUND)
            message(STATUS "MQTT client support enabled (bundled libmosquitto, TLS via OpenSSL ${OPENSSL_VERSION})")
        else()
            # tls_mosq.c and file_common.c (cert file access) require OpenSSL — exclude when not available
            list(FILTER MOSQUITTO_SOURCES EXCLUDE REGEX "tls_mosq\\.c$")
            list(FILTER MOSQUITTO_SOURCES EXCLUDE REGEX "file_common\\.c$")
            message(STATUS "MQTT client support enabled (bundled libmosquitto, TLS disabled — OpenSSL not found)")
        endif()
    endif()
endif()

# Sources
# The settings store (RFC #4603): AppSettings' implementation spans the SQLite
# wrapper, path helpers, pre-QApplication bootstrap reader, and the sanitizer.
# Every target that compiles AppSettings.cpp needs the whole group — and the
# vendored engine, linked by the AETHER_SETTINGS_CONSUMERS loop near the end
# of this file.
set(AETHER_SETTINGS_SOURCES
    src/core/AppSettings.cpp
    src/core/SettingsDatabase.cpp
    src/core/SettingsPaths.cpp
    src/core/SettingsBootstrap.cpp
    src/core/SettingsSanitizer.cpp
)

set(CORE_SOURCES
    src/core/backends/MemoryWireCodec.cpp    # memory kv-set decode, shared by Flex + local bank
    src/core/backends/flex/FlexBackend.cpp   # aetherd RFC step 2.2 (§5.5)
    src/core/backends/sim/SimBackend.cpp     # demo mode — 2nd IRadioBackend (RFC #4288)
    src/core/backends/sim/SimSignalSource.cpp  # demo RX engine, worker thread (#4878)
    src/core/backends/sim/SpectrumPatternGenerator.cpp  # demo mode — signal engine (RFC #4288 Phase 2)
    src/core/backends/sim/NoiseMixer.cpp                 # demo mode — audio engine (RFC #4288 Phase 2b)
    src/core/backends/hl2/MetisProtocol.cpp  # aetherd HL2 Phase 1a (HPSDR Protocol 1)
    src/core/backends/hl2/MetisClient.cpp    # aetherd HL2 Phase 1a (UDP wire + RX ingest)
    src/core/backends/hl2/Hl2EmergencyStop.cpp # release the radio from a signal handler
    src/core/backends/hl2/Hl2Receivers.cpp   # per-receiver index-space map (docs/HERMES.md §12.5)
    src/core/backends/hl2/Hl2Spectrum.cpp    # aetherd HL2 Phase 1a (FFT panadapter, FFTW)
    src/core/backends/hl2/Hl2RxDsp.cpp       # aetherd HL2 Phase 1a (IQ -> WdspChannel demod + spectrum)
    src/core/backends/hl2/Hl2TxDsp.cpp       # SSB transmit chain (TX audio -> baseband IQ)
    src/core/backends/hl2/Hl2Backend.cpp     # aetherd HL2 Phase 1b (IRadioBackend impl)
    src/core/backends/hl2/Hl2Discovery.cpp   # aetherd HL2 Phase 1c (HPSDR discovery -> picker)
    src/core/backends/hl2/Hl2Settings.cpp    # owned config object, "Hl2" root key (Principle V)
    src/core/backends/hl2/Hl2FreqCal.cpp     # manual frequency calibration (no such register exists)
    src/core/backends/icom/IcomProtocol.cpp  # IcomCIV Phase 0 (RS-BA1 UDP transport)
    src/core/backends/icom/CivCodec.cpp      # IcomCIV Phase 1 (CI-V command plane, transport-free)
    src/core/backends/icom/IcomStream.cpp    # IcomCIV Phase 0 (one UDP stream + ARQ)
    src/core/backends/icom/IcomSession.cpp   # IcomCIV Phase 0 (handshake orchestration)
    src/core/backends/icom/IcomScope.cpp     # IcomCIV Phase 2 (0x27 waveform -> panadapter)
    src/core/backends/icom/IcomAudio.cpp     # IcomCIV Phase 3 (codecs + the 1364/556 split)
    src/core/backends/icom/IcomMeters.cpp    # IcomCIV Phase 4 (calibration + poll scheduler)
    src/core/backends/icom/IcomCivScheduler.cpp # RFC #4983 shared CI-V pacing + causality
    src/core/backends/icom/IcomModels.cpp    # IcomCIV Phase 5 (per-model capability table)
    src/core/backends/icom/IcomControls.cpp  # the CI-V control registry (controls.map / controls.scrub)
    src/core/backends/icom/IcomCivBackend.cpp # IcomCIV (IRadioBackend impl)
    src/core/backends/icom/IcomSettings.cpp  # owned config, "Icom" root key (Principle V)
    src/core/backends/icom/IcomCredentials.cpp # password -> keychain, NEVER settings
    src/core/dsp/WdspChannel.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/RadioStateMemory.cpp   # RFC #4603 client-side radio memory
    src/core/GpuSelector.cpp
    src/core/AgcTCalibrator.cpp
    src/core/AdaptiveFilterEngine.cpp
    src/core/OccupiedRegion.cpp
    src/core/SettingsHelpers.cpp
    src/core/ThemeManager.cpp
    src/core/ThemeSeedGenerated.cpp
    src/core/BandStackSettings.cpp
    src/core/RadioDiscovery.cpp
    src/core/RadioConnection.cpp
    src/core/NetworkPathResolver.cpp
    src/core/TgxlConnection.cpp
    src/core/CommandParser.cpp
    src/core/AudioSummaryLogger.cpp
    src/core/AudioFormatNegotiator.cpp
    src/core/AudioDeviceNegotiator.cpp
    src/core/AudioOutputRouter.cpp
    src/core/AetherDspModePolicy.cpp
    src/core/AudioEngine.cpp
    src/core/TxCaptureBuffer.cpp
    src/core/TxMicChannelNormalizer.cpp
    src/core/TxVoiceProcessor.cpp
    src/core/ChannelStripPresets.cpp
    src/core/Biquad.cpp
    src/core/StereoBiquad.cpp
    src/core/ClientEq.cpp
    src/core/ClientComp.cpp
    src/core/ClientGate.cpp
    src/core/ClientDeEss.cpp
    src/core/ClientTube.cpp
    src/core/ClientPudu.cpp
    src/core/ClientPuduMonitor.cpp
    src/core/ClientReverb.cpp
    src/core/ClientPhaseRotator.cpp
    src/core/ClientFinalLimiter.cpp
    src/core/ClientTxTestTone.cpp
    src/core/WsprBeacon.cpp
    src/core/ClientQuindarTone.cpp
    src/core/QuindarLocalSink.cpp
    src/core/CwSidetoneGenerator.cpp
    src/core/CwSidetoneQAudioSink.cpp
    src/core/CwxLocalKeyer.cpp
    src/core/IambicKeyer.cpp
    src/core/SpectralNR.cpp
    src/core/MonoDspStereoAdapter.cpp
    src/core/PanadapterStream.cpp
    src/core/TimeFrameVoter.cpp
    src/core/WwvDecoder.cpp
    src/core/WwvbDecoder.cpp
    src/core/AetherClockEngine.cpp
    src/core/AetherClockSettings.cpp
    src/core/MiniPanSettings.cpp
    src/core/PacketLossConcealment.cpp
    src/core/PerfTelemetry.cpp
    src/core/MemoryTelemetry.cpp
    src/core/RigctlProtocol.cpp
    src/core/SmartCatProtocol.cpp
    src/core/SmartCatSession.cpp
    src/core/CatPort.cpp
    src/core/SmartLinkClient.cpp
    src/core/KiwiSdrProtocol.cpp
    src/core/KiwiSdrRedirectPolicy.cpp
    src/core/KiwiSdrCredentialStore.cpp
    src/core/KiwiSdrClient.cpp
    src/core/KiwiSdrManager.cpp
    src/core/ReceivePresentationSync.cpp
    src/core/KiwiPublicDirectory.cpp
    src/core/WanConnection.cpp
    src/core/DxClusterClient.cpp
    src/core/WsjtxClient.cpp
    src/core/SpotCollectorClient.cpp
    src/core/PotaClient.cpp
    src/core/EibiClient.cpp
    src/core/EibiCodeMaps.cpp
    src/core/N1MMSpotClient.cpp
    src/core/N1MMSpotParser.cpp
    src/core/PropForecastClient.cpp
    src/core/PskReporterClient.cpp
    src/core/LocationAddressResolver.cpp
    src/core/MqttAntennaAlias.cpp
    src/core/MqttSettings.cpp
    src/core/AutomationBridgeSettings.cpp
    src/core/SpotCommandPolicy.cpp
    src/core/SpotModeResolver.cpp
    src/core/TciServer.cpp
    src/core/TciProtocol.cpp
    src/core/TciRoutingState.cpp
    src/core/RadioCertification.cpp
    src/core/TciTrxMap.cpp
    src/core/AutomationServer.cpp
    src/core/MqttClient.cpp
    src/core/PgxlConnection.cpp
    src/core/AcomProtocol.cpp
    src/core/AcomConnection.cpp
    src/core/SpeProtocol.cpp
    src/core/SpeConnection.cpp
    src/core/VkampProtocol.cpp
    src/core/VkampConnection.cpp
    src/core/FirmwareUploader.cpp
    src/core/WaveformInstaller.cpp
    src/core/WaveformUploadState.cpp
    src/core/LegacyWaveformPackage.cpp
    src/core/DigitalVoiceModeRegistry.cpp
    src/core/DigitalVoiceWaveformTelemetry.cpp
    src/core/DigitalVoiceWaveformProcess.cpp
    src/core/DvkWavTransfer.cpp
    src/core/ZipArchive.cpp
    src/core/ProfileTransfer.cpp
    src/core/QsoRecorder.cpp
    src/core/FirmwareStager.cpp
    src/core/UpdateChecker.cpp
    src/core/OleCompoundFile.cpp
    src/core/CabExtractor.cpp
    src/core/RNNoiseFilter.cpp
    src/core/OpusTxPacer.cpp
    src/core/SpecbleachFilter.cpp
    src/core/CwDecoder.cpp
    src/core/CwCallsignSpotter.cpp
    src/core/CallsignInfo.cpp
    src/core/QrzClient.cpp
    src/core/CallsignLookupService.cpp
    src/core/RttyDecoder.cpp
    src/core/VoiceSignalDetector.cpp
    src/core/SpectrogramBuffer.cpp
    src/core/SignalClassifier.cpp
    src/core/Resampler.cpp
    src/core/NvidiaAfxFilter.cpp
    src/core/NvidiaAfxPack.cpp
    src/core/DeepFilterFilter.cpp
    src/core/RADEEngine.cpp
    src/core/AsyncLogWriter.cpp
    src/core/LogManager.cpp
    src/core/ShortcutManager.cpp
    src/core/SupportBundle.cpp
    src/core/IssueReport.cpp
    src/core/DeviceDiagnostics.cpp
    src/core/SerialPortController.cpp
    src/core/FlexControlManager.cpp
    src/core/OpusCodec.cpp
    src/core/CtyDatParser.cpp
    src/core/AdifParser.cpp
    src/core/DxccWorkedStatus.cpp
    src/core/DxccColorProvider.cpp
    src/core/SleepInhibitor.cpp
    src/core/LocalMemoryBank.cpp
    src/core/LocalMemoryStore.cpp
    src/core/MemoryCsvCompat.cpp
    src/core/MemoryFieldValues.cpp
    src/core/MemoryRecallPolicy.cpp
    src/core/NetRecurrence.cpp
    src/core/NetSchedulePlanner.cpp
    src/core/NetScheduleStore.cpp
    src/core/NetScheduler.cpp
    src/core/WfmDemodulator.cpp
    src/core/WfmDsp.cpp
    src/core/WaveOutWriter.cpp
    src/core/tnc/AetherAx25LibmodemShim.cpp
    src/core/tnc/HdlcCodec.cpp
    src/core/tnc/Ax25FrameFormatter.cpp
    src/core/tnc/Ax25.cpp
    src/core/tnc/Ax25Connection.cpp
    src/core/tnc/HeardList.cpp
    src/core/tnc/KissFraming.cpp
    src/core/tnc/KissTncServer.cpp
    src/core/tnc/TncTerminal.cpp
    src/core/pms/PmsMailbox.cpp
    src/core/aprs/AprsPacket.cpp
    src/core/aprs/AprsStationList.cpp
    src/core/aprs/AprsMessenger.cpp
    src/core/aprs/AprsBeacon.cpp
    src/core/aprs/AprsSettings.cpp
)

if(APPLE)
    list(APPEND CORE_SOURCES src/core/VirtualAudioBridge.cpp src/core/MacMicPermission.mm)
elseif(UNIX)
    # Linux DAX uses PulseAudio pipe modules via pactl (works with PipeWire too).
    # When libpipewire-0.3 dev headers are present we additionally compile a
    # native pw_stream-based RX source for sub-100ms DAX RX latency.
    list(APPEND CORE_SOURCES src/core/PipeWireAudioBridge.cpp)
    set(HAVE_PIPEWIRE TRUE)
    pkg_check_modules(PIPEWIRE_NATIVE libpipewire-0.3)
    if(PIPEWIRE_NATIVE_FOUND)
        list(APPEND CORE_SOURCES
            src/core/PipeWireNativeContext.cpp
            src/core/PipeWireNativeRxSource.cpp
        )
        set(HAVE_PIPEWIRE_NATIVE TRUE)
    endif()
endif()

set(MODEL_SOURCES
    src/models/Nr2SettingsModel.cpp
    src/models/Rn2SettingsModel.cpp
    src/models/RadioModel.cpp
    src/models/DeclaredBands.cpp
    src/models/RadioSession.cpp
    src/models/ModelCapabilities.cpp
    src/models/AntennaAliasStore.cpp
    src/models/SliceModel.cpp
    src/models/PanadapterModel.cpp
    src/models/MeterModel.cpp
    src/models/TunerModel.cpp
    src/models/AmpModel.cpp
    src/models/TransmitModel.cpp
    src/models/EqualizerModel.cpp
    src/models/TnfModel.cpp
    src/models/UsbCableModel.cpp
    src/models/DaxIqModel.cpp
    src/models/SpotModel.cpp
    src/models/CwxModel.cpp
    src/models/DvkModel.cpp
    src/models/NavtexModel.cpp
    src/models/FlexWaveformModel.cpp
    src/models/DStarModel.cpp
    src/models/DigitalVoiceWaveformHistory.cpp
    src/models/BandSettings.cpp
    src/models/BandPlanManager.cpp
    src/models/XvtrPolicy.cpp
    src/models/AntennaGeniusModel.cpp
    src/models/AetherClockModel.cpp
)

# Vendored QGeoView (LGPL-3.0) — slippy-map widget for the common
# mapping engine (PSK Reporter map, future APRS map). Patched per
# third_party/QGeoView/AETHERSDR-PATCHES.md.
add_subdirectory(third_party/QGeoView/lib EXCLUDE_FROM_ALL)

set(GUI_SOURCES
    src/gui/Contribute.cpp
    src/gui/MainWindow.cpp
    src/gui/MainWindowHelpers.cpp
    src/gui/WindowGeometryRestore.cpp
    src/gui/MainWindow_Controllers.cpp
    src/gui/MainWindow_DspApplets.cpp
    src/gui/MainWindow_Menus.cpp
    src/gui/MainWindow_Session.cpp
    src/gui/MainWindow_Shortcuts.cpp
    src/gui/MainWindow_DigitalModes.cpp
    src/gui/MainWindow_Spots.cpp
    src/gui/MainWindow_SwrSweep.cpp
    src/gui/MainWindow_AetherClock.cpp
    src/gui/MainWindow_Wiring.cpp
    src/gui/MainWindow_Workspace.cpp
    src/gui/MainWindow_Nets.cpp
    src/gui/MainWindow_Callsign.cpp
    src/gui/MainWindow_ReceiveSync.cpp
    src/gui/MainWindow_KiwiSdr.cpp
    src/gui/map/MapView.cpp
    src/gui/map/MapMarkerItem.cpp
    src/gui/map/MapPathItem.cpp
    src/gui/PskReporterMapDialog.cpp
    src/gui/GpsLocationDialog.cpp
    src/gui/AgcCalibrationDialog.cpp
    src/gui/AudioDeviceChangeDialog.cpp
    src/gui/WfmDeviceDialog.cpp
    src/gui/ConnectionPanel.cpp
    src/gui/ClientDisconnectDialog.cpp
    src/gui/ConnectedStationsDialog.cpp
    src/gui/PanadapterMessageOverlay.cpp
    src/gui/PanadapterMessageOverlay.h
    src/gui/PanadapterRenderScheduler.cpp
    src/gui/SpectrumWidget.cpp
    src/gui/WaterfallHistoryBuffer.cpp
    src/gui/SpectrumOverlayMenu.cpp
    src/gui/SpectrumOverlayWheelGuard.cpp
    src/gui/DssRenderer.cpp
    src/gui/FrequencyEntryParser.cpp
    src/gui/SliceColorManager.cpp
    src/gui/SliceLabel.cpp
    src/gui/VfoWidget.cpp
    src/gui/SmartMtrWidget.cpp
    src/gui/SmartMtrConfig.cpp
    src/gui/MeterViewController.cpp
    src/gui/RadioSetupDialog.cpp
    src/gui/NetworkDiagnosticsDialog.cpp
    src/gui/Ax25HfPacketDecodeDialog.cpp
    src/gui/DStarAccessibility.cpp
    src/gui/DStarModemPage.cpp
    src/gui/AprsMessagesDialog.cpp
    src/gui/AprsSymbolIcons.cpp
    src/gui/FlexControlDialog.cpp
    src/gui/PropDashboardDialog.cpp
    src/gui/MemoryCommands.cpp
    src/gui/MemoryBrowsePanel.cpp
    src/gui/MemoryDialog.cpp
    src/gui/NetSchedulerDialog.cpp
    src/gui/NetReminderBanner.cpp
    src/gui/SpotSettingsDialog.cpp
    src/gui/AetherDspDialog.cpp
    src/gui/AetherDspWidget.cpp
    src/gui/WaveformsDialog.cpp
    src/gui/ClientRxDspApplet.cpp
    src/gui/DragValuePopup.cpp
    src/gui/DspParamPopup.cpp
    src/gui/DxClusterDialog.cpp
    src/gui/DxClusterStartupCommandsDialog.cpp
    src/gui/CallsignCard.cpp
    src/gui/CallsignLookupDialog.cpp
    src/gui/CwxPanel.cpp
    src/gui/BandStackPanel.cpp
    src/gui/FramelessWindowTitleBar.cpp
    src/gui/FramelessResizer.cpp
    src/gui/PanFloatingWindow.cpp
    src/gui/DvkPanel.cpp
    src/gui/AmpApplet.cpp
    src/gui/AcomApplet.cpp
    src/gui/SpeApplet.cpp
    src/gui/VkampApplet.cpp
    src/gui/MeterApplet.cpp
    src/gui/ProfileSwitcherApplet.cpp
    src/gui/RadeApplet.cpp
    src/gui/HealthApplet.cpp
    src/gui/PersistentDialog.cpp
    src/gui/FramelessMessageBox.cpp
    src/gui/ProfileManagerDialog.cpp
    src/gui/SettingsBrowserDialog.cpp
    src/gui/ProfileImportExportDialog.cpp
    src/gui/TxBandDialog.cpp
    src/gui/PanadapterApplet.cpp
    src/gui/MiniPanScope.cpp
    src/gui/MiniPanApplet.cpp
    src/gui/RangeSlider.cpp
    src/gui/PanadapterStack.cpp
    src/gui/PanLayoutDialog.cpp
    src/gui/AppletPanel.cpp
    src/gui/KiwiSdrApplet.cpp
    src/gui/KiwiPublicReceiverPicker.cpp
    src/gui/FavoritesPickerDialog.cpp
    src/gui/RxApplet.cpp
    src/gui/AdaptiveFilterControls.cpp
    src/gui/FilterPassbandWidget.cpp
    src/gui/AnalogMeterFaceTheme.cpp
    src/gui/RadioSwrValidityFilter.cpp
    src/gui/SMeterGeometry.cpp
    src/gui/SMeterWidget.cpp
    src/gui/CrossNeedleMeterGeometry.cpp
    src/gui/CrossNeedleMeterWidget.cpp
    src/gui/CrossNeedleMeterApplet.cpp
    src/gui/CrossNeedleMeterSettings.cpp
    src/gui/VuMeterSettings.cpp
    src/gui/TunerApplet.cpp
    src/gui/TxApplet.cpp
    src/gui/AtuPreTuneDialog.cpp
    src/gui/SwrSweepLicenseDialog.cpp
    src/gui/PhoneCwApplet.cpp
    src/gui/PhoneApplet.cpp
    src/gui/EqApplet.cpp
    src/gui/WaveApplet.cpp
    src/gui/WaveformScopeModel.cpp
    src/gui/WaveformWidget.cpp
    src/gui/AetherClockApplet.cpp
    src/gui/ClockAlignmentWidget.cpp
    src/gui/ClientEqApplet.cpp
    src/gui/ClientEqCurveWidget.cpp
    src/gui/ClientEqEditor.cpp
    src/gui/ClientEqEditorCanvas.cpp
    src/gui/StripEqPanel.cpp
    src/gui/ClientEqFftAnalyzer.cpp
    src/gui/ClientEqIconRow.cpp
    src/gui/ClientEqOutputFader.cpp
    src/gui/ClientLevelMeter.cpp
    src/gui/ClientEqParamRow.cpp
    src/gui/ClientChainApplet.cpp
    src/gui/ClientChainWidget.cpp
    src/gui/StripChainWidget.cpp
    src/gui/StripRxChainWidget.cpp
    src/gui/StripRxOutputPanel.cpp
    src/gui/ClientRxChainWidget.cpp
    src/gui/EditorFramelessTitleBar.cpp
    src/gui/containers/ContainerManager.cpp
    src/gui/containers/ContainerTitleBar.cpp
    src/gui/containers/ContainerWidget.cpp
    src/gui/containers/FloatingContainerWindow.cpp
    src/gui/workspace/CanvasInteraction.cpp
    src/gui/workspace/CanvasItemFrame.cpp
    src/gui/workspace/CanvasLayout.cpp
    src/gui/workspace/ClassicLayout.cpp
    src/gui/workspace/WorkspaceCanvas.cpp
    src/gui/workspace/WorkspaceController.cpp
    src/gui/workspace/WorkspaceDocument.cpp
    src/gui/workspace/WorkspaceGeometry.cpp
    src/gui/workspace/WorkspaceMigration.cpp
    src/gui/workspace/WorkspaceStore.cpp
    src/gui/workspace/WorkspaceWindow.cpp
    src/gui/ClientCompApplet.cpp
    src/gui/ClientCompCurveWidget.cpp
    src/gui/ClientCompKnob.cpp
    src/gui/ClientGateApplet.cpp
    src/gui/DemoApplet.cpp                # demo mode — noise-scene control tile (RFC #4288)
    src/gui/ClientGateCurveWidget.cpp
    src/gui/ClientGateEditor.cpp
    src/gui/StripGatePanel.cpp
    src/gui/ClientGateLevelView.cpp
    src/gui/ClientDeEssApplet.cpp
    src/gui/ClientDeEssCurveWidget.cpp
    src/gui/StripDeEssPanel.cpp
    src/gui/ClientTubeApplet.cpp
    src/gui/ClientTubeCurveWidget.cpp
    src/gui/ClientTubeEditor.cpp
    src/gui/StripTubePanel.cpp
    src/gui/ClientPuduApplet.cpp
    src/gui/ClientPuduEditor.cpp
    src/gui/StripPuduPanel.cpp
    src/gui/ClientReverbApplet.cpp
    src/gui/StripReverbPanel.cpp
    src/gui/StripWaveformPanel.cpp
    src/gui/StripFinalOutputPanel.cpp
    src/gui/AetherialAudioStrip.cpp
    src/gui/PooDooLogo.cpp
    src/gui/ClientCompLimiterButton.cpp
    src/gui/ClientCompMeter.cpp
    src/gui/ClientCompThresholdFader.cpp
    src/gui/ClientCompEditor.cpp
    src/gui/ClientCompEditorCanvas.cpp
    src/gui/StripCompPanel.cpp
    src/gui/CatControlApplet.cpp
    src/gui/DaxApplet.cpp
    src/gui/TciApplet.cpp
    src/gui/DaxIqApplet.cpp
    src/gui/MqttApplet.cpp
    src/gui/MqttSettingsDialog.cpp
    src/gui/MeterSlider.cpp
    src/gui/PhaseKnob.cpp
    src/gui/AntennaGeniusApplet.cpp
    src/gui/ShackSwitchApplet.cpp
    src/gui/TitleBar.cpp
    src/gui/SupportDialog.cpp
    src/gui/SliceTroubleshootingDialog.cpp
    src/gui/RadioHealthDialog.cpp
    src/gui/KeyboardMapWidget.cpp
    src/gui/ShortcutDialog.cpp
    src/gui/MultiFlexDialog.cpp
    src/gui/HelpDialog.cpp
    src/gui/WhatsNewDialog.cpp
    src/gui/ThemeEditorDialog.cpp
    src/gui/ThemeInspector.cpp
    src/gui/GradientEditorDialog.cpp
    src/gui/TokenEditorWidget.cpp
    src/gui/CompactColorPicker.cpp
    src/gui/ImageFileDialog.cpp
)

# Bundled RNNoise (Mozilla/Xiph BSD-3) — client-side neural noise suppression
set(RNNOISE_DIR ${CMAKE_SOURCE_DIR}/third_party/rnnoise)
set(RNNOISE_SOURCES
    ${RNNOISE_DIR}/src/denoise.c
    ${RNNOISE_DIR}/src/celt_lpc.c
    ${RNNOISE_DIR}/src/kiss_fft.c
    ${RNNOISE_DIR}/src/pitch.c
    ${RNNOISE_DIR}/src/rnn.c
    ${RNNOISE_DIR}/src/nnet.c
    ${RNNOISE_DIR}/src/nnet_default.c
    ${RNNOISE_DIR}/src/rnnoise_data.c
    ${RNNOISE_DIR}/src/rnnoise_tables.c
    ${RNNOISE_DIR}/src/parse_lpcnet_weights.c
)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86")
    list(APPEND RNNOISE_SOURCES
        ${RNNOISE_DIR}/src/x86/x86cpu.c
        ${RNNOISE_DIR}/src/x86/x86_dnn_map.c
        ${RNNOISE_DIR}/src/x86/nnet_sse4_1.c
        ${RNNOISE_DIR}/src/x86/nnet_avx2.c
    )
endif()

# Suppress warnings and set include paths for bundled C code
set_source_files_properties(${RNNOISE_SOURCES} PROPERTIES
    INCLUDE_DIRECTORIES "${RNNOISE_DIR}/include;${RNNOISE_DIR}/src;${RNNOISE_DIR}/src/x86"
)
# x86 RTCD (runtime CPU detection) + per-file SIMD flags
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86")
    if(MSVC)
        # MSVC: uses _MSC_VER path in x86cpu.c (intrin.h), no CPU_INFO_BY_C needed
        # MSVC doesn't need -msse4.1/-mavx flags — intrinsics are always available
        set_source_files_properties(${RNNOISE_SOURCES} PROPERTIES
            COMPILE_FLAGS "/w /DRNN_ENABLE_X86_RTCD=1"
        )
        set_source_files_properties(${RNNOISE_DIR}/src/x86/nnet_sse4_1.c PROPERTIES
            COMPILE_FLAGS "/w /DRNN_ENABLE_X86_RTCD=1 /D__SSE4_1__"
        )
        set_source_files_properties(${RNNOISE_DIR}/src/x86/nnet_avx2.c PROPERTIES
            COMPILE_FLAGS "/w /DRNN_ENABLE_X86_RTCD=1 /arch:AVX2 /D__AVX2__"
        )
    else()
        # GCC/Clang: use cpuid.h intrinsic and per-file ISA flags
        set_source_files_properties(${RNNOISE_SOURCES} PROPERTIES
            COMPILE_FLAGS "-w -DRNN_ENABLE_X86_RTCD=1 -DCPU_INFO_BY_C=1"
        )
        set_source_files_properties(${RNNOISE_DIR}/src/x86/nnet_sse4_1.c PROPERTIES
            COMPILE_FLAGS "-w -DRNN_ENABLE_X86_RTCD=1 -DCPU_INFO_BY_C=1 -msse4.1"
        )
        set_source_files_properties(${RNNOISE_DIR}/src/x86/nnet_avx2.c PROPERTIES
            COMPILE_FLAGS "-w -DRNN_ENABLE_X86_RTCD=1 -DCPU_INFO_BY_C=1 -mavx -mfma -mavx2"
        )
    endif()
else()
    set_source_files_properties(${RNNOISE_SOURCES} PROPERTIES
        COMPILE_FLAGS "-w"
    )
endif()

# Bundled ggmorse (MIT) — CW Morse code decoder
set(GGMORSE_DIR ${CMAKE_SOURCE_DIR}/third_party/ggmorse)
set(GGMORSE_SOURCES
    ${GGMORSE_DIR}/src/ggmorse.cpp
    ${GGMORSE_DIR}/src/resampler.cpp
)
set_source_files_properties(${GGMORSE_SOURCES} PROPERTIES
    INCLUDE_DIRECTORIES "${GGMORSE_DIR}/include;${GGMORSE_DIR}/src"
)
if(MSVC)
    set_source_files_properties(${GGMORSE_SOURCES} PROPERTIES COMPILE_FLAGS "/w")
else()
    set_source_files_properties(${GGMORSE_SOURCES} PROPERTIES COMPILE_FLAGS "-w")
endif()

add_library(aether_libmodem_core STATIC
    third_party/libmodem_core/bitstream.cpp
    third_party/libmodem_core/demodulator.cpp
)
target_include_directories(aether_libmodem_core PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/libmodem_core
)
target_compile_features(aether_libmodem_core PUBLIC cxx_std_20)
target_compile_definitions(aether_libmodem_core PUBLIC
    LIBMODEM_NAMESPACE=aether_libmodem_core
    "LIBMODEM_NAMESPACE_REFERENCE=aether_libmodem_core::"
)
if(MSVC)
    target_compile_options(aether_libmodem_core PRIVATE /w)
else()
    target_compile_options(aether_libmodem_core PRIVATE -w)
endif()

# AetherAFSKDemod — Direwolf profile-A AFSK demodulator.
# Vendored Dire Wolf source is GPL-2.0-or-later; the combined AetherSDR work is
# GPL-3.0-or-later (see THIRD_PARTY_LICENSES).
# Profile A: IQ-mix + RRC + AGC/multi-slicer (best for amplitude-imbalanced signals).
# VHF 1200 baud only; HF 300 baud stays on aether_libmodem_core.
# AX.25 framing (bitstream, HDLC, FCS) continues to use aether_libmodem_core.
add_library(aether_afskdemod STATIC
    third_party/direwolf_afsk/AetherAFSKDemod.cpp
)
target_include_directories(aether_afskdemod PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/direwolf_afsk
)
target_compile_features(aether_afskdemod PUBLIC cxx_std_20)
message(STATUS "AetherAFSKDemod: Direwolf profile-A (VHF 1200 baud), GPL-2.0-or-later")

# Vendored SQLite amalgamation (public domain) — the client settings store
# (RFC #4603). Deliberately NOT Qt6::Sql: the amalgamation needs no runtime
# driver plugin in any deployment artifact and is usable before QApplication
# exists (the pre-QApplication bootstrap reads in main.cpp / GpuSelector).
# The ONLY permitted consumer is src/core/SettingsDatabase.cpp — see
# third_party/sqlite/README.md.
add_library(aether_sqlite3 STATIC
    third_party/sqlite/sqlite3.c
)
target_include_directories(aether_sqlite3 PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/sqlite
)
target_compile_definitions(aether_sqlite3 PRIVATE
    SQLITE_THREADSAFE=1              # serialized mode; AppSettings adds its own cache lock
    SQLITE_DQS=0                     # no double-quoted string literals
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 # NORMAL is durability-sufficient under WAL
    SQLITE_DEFAULT_FILE_PERMISSIONS=0600  # settings dir is user-private (unix)
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_MAX_EXPR_DEPTH=0
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_SHARED_CACHE
)
if(MSVC)
    target_compile_options(aether_sqlite3 PRIVATE /w)
else()
    target_compile_options(aether_sqlite3 PRIVATE -w)
endif()
message(STATUS "Vendored SQLite ${CMAKE_SOURCE_DIR}/third_party/sqlite (settings store, RFC #4603)")

set(DFNR_RESOURCES "")
if(ENABLE_DFNR AND AETHER_EMBED_DFNR_MODEL)
    if(EXISTS "${DFNR_MODEL}")
        file(TO_CMAKE_PATH "${DFNR_MODEL}" DFNR_MODEL_QRC_PATH)
        set(DFNR_MODEL_QRC "${CMAKE_CURRENT_BINARY_DIR}/dfnr_model.qrc")
        file(WRITE "${DFNR_MODEL_QRC}"
"<RCC>
    <qresource prefix=\"/models\">
        <file alias=\"${DFNR_EMBEDDED_MODEL_NAME}\">${DFNR_MODEL_QRC_PATH}</file>
    </qresource>
</RCC>
")
        qt_add_resources(DFNR_RESOURCES "${DFNR_MODEL_QRC}")
        message(STATUS "DFNR model will be embedded in Qt resources")
    else()
        message(WARNING "AETHER_EMBED_DFNR_MODEL is ON, but DFNR model not found at ${DFNR_MODEL}")
    endif()
endif()

qt_add_resources(RESOURCES resources/resources.qrc)

# WDSP 2.00 is an engine-only dependency for raw-IQ radio backends. Its C API,
# platform shims, and FFTW dependency remain private to aethercore.
add_subdirectory(third_party/wdsp EXCLUDE_FROM_ALL)

# aetherd RFC step 1: engine static library — src/core + src/models
# plus the vendored C/C++ sources those engine files compile against
# (rnnoise, ggmorse, RADE, specbleach; mosquitto/rtmidi are appended below in
# their feature blocks). Qt resources (resources.qrc, DFNR model qrc) stay on
# the executable: rcc registration runs from the exe's static initializers and
# is process-global, so core code reading ":/..." keeps working.
add_library(aethercore STATIC
    ${CORE_SOURCES}
    ${MODEL_SOURCES}
    ${RNNOISE_SOURCES}
    ${GGMORSE_SOURCES}
    ${RADE_SOURCES}
    ${SPECBLEACH_SOURCES}
)
target_link_libraries(aethercore PRIVATE aether::wdsp)

add_executable(AetherSDR
    src/main.cpp
    ${GUI_SOURCES}
    ${RESOURCES}
    ${DFNR_RESOURCES}
)
target_link_libraries(AetherSDR PRIVATE aethercore)

# --- ASR engine: vendored whisper.cpp (RFC #4333, Phase 1) -------------------
# whisper.cpp is a self-contained CMake project exposing a `whisper` target
# (which pulls in `ggml`). We build it static and CPU-only, then link it into
# libaethercore — engine code only (no gui/ include), so EB1/EB2 stay clean.
# The GPU/accelerator ggml backends are trimmed from the vendored tree for now;
# force every backend option OFF so the missing directories are never referenced
# and builds are deterministic across platforms (incl. no -march=native, which
# would break portable/Pi/CI binaries). GPU backends land in a later phase.
#
# USE_SYSTEM_LIBWHISPER=ON swaps that vendored tree for a distro libwhisper found
# through pkg-config — the packagers' path, off by default so CI, releases, and
# contributor laptops all build the pinned snapshot.
if (ENABLE_ASR)
    # GPU acceleration for ASR: the native GPU backend per platform — Metal on
    # Apple (macOS has no native Vulkan), Vulkan elsewhere (cross-platform
    # NVIDIA/AMD/Intel on Linux/Windows). Both are the only GPU backends vendored,
    # and both are gated so machines/CI without the toolchain build CPU-only,
    # exactly as before. At runtime ggml falls back to CPU when no GPU device is
    # found, so a GPU-enabled binary still runs on GPU-less hosts.
    #
    # Declared ahead of the engine branch below so they always reach the cache: an
    # option() that exists on only one branch turns a user's -DENABLE_ASR_VULKAN=OFF
    # into a bare "unused variable" warning on the other.
    set(_asr_vulkan OFF)
    set(_asr_metal  OFF)
    # Only the vendored-whisper branch can precompile the Metal kernels, so this
    # stays OFF when USE_SYSTEM_LIBWHISPER=ON — where the distro's libwhisper
    # decides how its own shaders are built.
    set(_asr_metal_precompile OFF)
    if (APPLE)
        # Metal uses the Metal framework + the `metal` shader compiler from full
        # Xcode (present on macOS CI); no external SDK to install.
        option(ENABLE_ASR_METAL "Enable Metal GPU acceleration for ASR (macOS)" ON)
    else()
        option(ENABLE_ASR_VULKAN "Enable Vulkan GPU acceleration for ASR (auto-detected)" ON)
    endif()

    if(USE_SYSTEM_LIBWHISPER)
        # Distro-packaged libwhisper, for packagers who link everything dynamically.
        # Which ggml backends it carries is the packager's call and is not visible
        # from here, so this path never claims a GPU backend.
        #
        # Release guard: for that same reason it can never satisfy REQUIRE_ASR_GPU.
        # Fail here rather than let a release image quietly lose GPU inference —
        # the whole point of the guard.
        if (REQUIRE_ASR_GPU)
            message(FATAL_ERROR "REQUIRE_ASR_GPU is set but USE_SYSTEM_LIBWHISPER=ON. The GPU "
                "backends compiled into a distro libwhisper are the packager's choice and cannot "
                "be verified at configure time; build release images against the vendored engine "
                "(-DUSE_SYSTEM_LIBWHISPER=OFF).")
        endif()
        find_package(PkgConfig REQUIRED)
        if(PkgConfig_FOUND)
            # 1.8.0 is the real floor: WhisperAsrBackend.cpp enumerates devices with
            # GGML_BACKEND_DEVICE_TYPE_IGPU, which whisper.cpp only gained in 1.8.0.
            # Against 1.7.x this configures cleanly and then fails to compile.
            pkg_check_modules(whisper REQUIRED IMPORTED_TARGET whisper>=1.8.0)
        endif()
        # That same code calls ggml directly (<ggml-backend.h>, ggml_backend_dev_*),
        # so take ggml as a dependency of our own instead of leaning on whisper.pc's
        # transitive "-lggml -lggml-base" and flat "-I${includedir}". Upstream ggml
        # ships a CMake package config and Debian additionally ships ggml.pc; use
        # whichever exists, and fall back to whisper's own flags where neither does
        # (that fallback is what carries the build on Debian today).
        pkg_check_modules(ggml QUIET IMPORTED_TARGET ggml)
        if (TARGET PkgConfig::ggml)
            set(_asr_ggml_target PkgConfig::ggml)
        else()
            find_package(ggml QUIET CONFIG)
            if (TARGET ggml::ggml)
                set(_asr_ggml_target ggml::ggml)
            endif()
        endif()
        message(STATUS "ASR: system libwhisper ${whisper_VERSION} "
            "(ggml: ${_asr_ggml_target}; backends per distro, GPU unverifiable)")
    else()
        set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
        set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
        set(GGML_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
        set(GGML_BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
        set(GGML_NATIVE            OFF CACHE BOOL "" FORCE)  # portable binaries (Pi/CI/release)
        set(GGML_CPU               ON  CACHE BOOL "" FORCE)

        if (APPLE)
            if (ENABLE_ASR_METAL)
                set(_asr_metal ON)
                message(STATUS "ASR: Metal GPU backend enabled (Apple)")
            endif()
        else()
            if (ENABLE_ASR_VULKAN)
                # Prebuilt Windows path: skips the glslc + SPIRV-Headers requirement
                # because the shaders are already compiled INTO the shipped
                # ggml-vulkan.lib. Only the runtime Vulkan loader import lib is
                # needed to link the final exe.
                if (WIN32 AND MSVC AND ASR_USE_PREBUILT_WHISPER_GPU)
                    find_package(Vulkan QUIET)
                    if (Vulkan_FOUND)
                        set(_asr_vulkan ON)
                        set(_asr_use_prebuilt ON)
                        message(STATUS "ASR: Vulkan GPU backend enabled via prebuilt whisper-gpu pack")
                    else()
                        message(STATUS "ASR: ASR_USE_PREBUILT_WHISPER_GPU requested but Vulkan loader not found — CPU-only")
                    endif()
                else()
                    find_package(Vulkan QUIET COMPONENTS glslc)
                    find_package(SPIRV-Headers QUIET CONFIG)
                    if (Vulkan_FOUND AND Vulkan_glslc_FOUND AND SPIRV-Headers_FOUND)
                        set(_asr_vulkan ON)
                        message(STATUS "ASR: Vulkan GPU backend enabled (glslc: ${Vulkan_GLSLC_EXECUTABLE})")
                    else()
                        message(STATUS "ASR: Vulkan toolchain not found — CPU-only "
                            "(Vulkan=${Vulkan_FOUND} glslc=${Vulkan_glslc_FOUND} SPIRV-Headers=${SPIRV-Headers_FOUND})")
                    endif()
                endif()
            endif()
        endif()
        # Release guard: fail loudly rather than silently shipping a CPU-only build.
        if (REQUIRE_ASR_GPU AND NOT _asr_vulkan AND NOT _asr_metal)
            message(FATAL_ERROR "REQUIRE_ASR_GPU is set but no ASR GPU backend was enabled. "
                "Install the Vulkan SDK (glslc + Vulkan headers + SPIRV-Headers) on Linux/Windows, "
                "or full Xcode on macOS, then reconfigure.")
        endif()
        set(GGML_VULKAN ${_asr_vulkan} CACHE BOOL "" FORCE)
        set(GGML_METAL  ${_asr_metal}  CACHE BOOL "" FORCE)
        # Embed the compiled Metal shader source into the binary instead of relying on
        # a runtime file lookup (bundle Resources / cwd) for ggml-metal.metal — the
        # unembedded path fails whenever the app isn't launched from inside its own
        # bundle, silently disabling Metal and then crashing later via a confusing
        # ggml_abort() deep in the backend scheduler rather than a clear GPU-init
        # error. Fix diagnosed + verified by K5PTB on large-v3-turbo + Metal. Only
        # needs the assembler + sed/echo (already in play). (RFC #4333)
        set(GGML_METAL_EMBED_LIBRARY ${_asr_metal} CACHE BOOL "" FORCE)
        # Embed the library as a COMPILED .metallib (built by the offline `xcrun
        # metal` toolchain), not as .metal source. Embedded source forces a runtime
        # shader compile (newLibraryWithSource) at the first ggml touch of every
        # cold-cache launch — seconds on Apple Silicon, and on Intel-GPU Macs
        # Apple's runtime compiler can live-lock indefinitely, freezing the GUI
        # (#4535). The compiled embed keeps #4333's no-file-lookup property while
        # never invoking the runtime compiler. Needs the offline Metal toolchain,
        # which newer Xcode ships as a separate downloadable component — so keep it
        # opt-out: a host without the component can still build with
        # -DENABLE_ASR_METAL_PRECOMPILE=OFF (source embed + runtime compile).
        #
        # The whole decision lives here rather than in the vendored ggml CMakeLists:
        # which toolchain is required, what happens when it is missing, and which OS
        # the kernels are built for are all AetherSDR release policy. The vendored
        # file only consumes the result.
        option(ENABLE_ASR_METAL_PRECOMPILE
            "Compile the ASR Metal kernels at build time (needs the offline Metal toolchain)" ON)
        if (_asr_metal AND ENABLE_ASR_METAL_PRECOMPILE)
            # Probe the offline compiler at configure time so a missing toolchain is
            # caught here rather than as a mid-build pipe error. Fatal only under
            # REQUIRE_ASR_GPU (the fail-rather-than-ship-degraded flag); otherwise
            # warn — never silently — and fall back to the source embed.
            execute_process(COMMAND xcrun -sdk macosx metal --version
                            RESULT_VARIABLE _asr_metal_cc_rc
                            OUTPUT_QUIET ERROR_QUIET)
            if (_asr_metal_cc_rc EQUAL 0)
                set(_asr_metal_precompile ON)
            elseif (REQUIRE_ASR_GPU)
                message(FATAL_ERROR "ENABLE_ASR_METAL_PRECOMPILE is ON but the offline Metal "
                    "compiler is not runnable ('xcrun -sdk macosx metal --version' failed). "
                    "Install it with: xcodebuild -downloadComponent MetalToolchain, or "
                    "configure with -DENABLE_ASR_METAL_PRECOMPILE=OFF to embed shader source "
                    "and compile at runtime instead.")
            else()
                message(WARNING "The offline Metal compiler is not runnable "
                    "('xcrun -sdk macosx metal --version' failed) — embedding shader source "
                    "and compiling it at runtime instead of embedding a compiled .metallib. "
                    "This build carries the #4535 runtime-compile hazard, so ASR stays "
                    "CPU-only on Intel-GPU Macs (see asrMetalUsableHost() in "
                    "src/asr/WhisperAsrBackend.cpp). Install the toolchain with: "
                    "xcodebuild -downloadComponent MetalToolchain")
            endif()
        endif()
        set(GGML_METAL_EMBED_LIBRARY_COMPILED ${_asr_metal_precompile} CACHE BOOL "" FORCE)
        if (_asr_metal_precompile)
            # The kernels are frozen at build time now, so both halves of "which OS
            # can load this library" have to be pinned here. The runtime compile used
            # to answer both by construction, because it ran on the target machine.
            #
            # 1. Deployment target. Without -mmacosx-version-min the offline compiler
            #    targets the build host's SDK default, which would make the shipped
            #    binary's Metal floor a property of the CI runner image rather than of
            #    the release (macos-dmg.yml builds at 14.0 / 13.0). The vendored
            #    CMakeLists appends the flag when this is set. FORCEd both ways, so a
            #    reconfigure with a different deployment target cannot inherit the
            #    previous one out of the cache.
            if (CMAKE_OSX_DEPLOYMENT_TARGET)
                set(GGML_METAL_MACOSX_VERSION_MIN "${CMAKE_OSX_DEPLOYMENT_TARGET}" CACHE STRING "" FORCE)
            else()
                set(GGML_METAL_MACOSX_VERSION_MIN "" CACHE STRING "" FORCE)
            endif()
            # 2. Shader language. bf16 kernels need >= metal3.1 (== macOS 14) — below
            #    that ggml-metal.metal drops them itself — while props.has_bfloat stays
            #    a runtime device query, so a library built below 3.1 would be missing
            #    kernels the device then asks for. Pin 3.1 wherever the deployment
            #    target allows it; where it does not (the Intel DMG's 13.0) build 3.0
            #    and have the runtime clamp has_bfloat to match, the same way it
            #    already clamps has_tensor. This is not a preference: asking for 3.1
            #    against a 13.0 deployment target is a hard compile error.
            #
            #    The mapping is one shader-language version per macOS release —
            #    metal2.4 = macOS 12, metal3.0 = 13, metal3.1 = 14 — so the tier has
            #    to follow the deployment target exactly, and the same hard error
            #    applies at every step. The Intel DMG dropped to 12.0 once its Qt
            #    stopped being hand-built at 13.0 (its floor was the deployment
            #    target of that hand-built Qt, not a product decision), which is why
            #    a third tier exists here at all.
            #
            #    ggml's own shader carries exactly one version guard,
            #    `__METAL_VERSION__ < 310`, i.e. the bf16 kernels already handled by
            #    NO_BF16 — nothing in it distinguishes 3.0 from 2.4. That makes the
            #    2.4 build plausible but NOT proven: ggml may still use unguarded
            #    Metal-3 intrinsics. REQUIRE_ASR_GPU=ON on the DMG job means a wrong
            #    answer is a loud build failure rather than a silently CPU-only
            #    artifact, so this is safe to discover in CI.
            #
            #    Note the 2.x tier is spelled `macos-metal2.4`, NOT `metal2.4`.
            #    Metal 3 unified the platforms and dropped the prefix, so 3.0/3.1
            #    below are correct unqualified while everything at 2.x needs it.
            #    The unqualified 2.4 spelling is not a legacy alias — the compiler
            #    rejects it outright ("invalid value 'metal2.4' in '-std=metal2.4'")
            #    and lists ios-metal2.4 / macos-metal2.4 / metal3.0 / metal3.1 /
            #    metal3.2 as the accepted set. That killed the first Intel DMG
            #    build to reach this tier, at 32%, in run 30764239938.
            if (CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 13.0)
                set(GGML_METAL_STD "macos-metal2.4" CACHE STRING "" FORCE)
                set(GGML_METAL_EMBED_LIBRARY_NO_BF16 ON CACHE BOOL "" FORCE)
                message(STATUS "ASR: Metal kernels precompiled for macOS ${CMAKE_OSX_DEPLOYMENT_TARGET} "
                    "(macos-metal2.4, no bf16 — props.has_bfloat clamped to match)")
            elseif (CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 14.0)
                set(GGML_METAL_STD "metal3.0" CACHE STRING "" FORCE)
                set(GGML_METAL_EMBED_LIBRARY_NO_BF16 ON CACHE BOOL "" FORCE)
                message(STATUS "ASR: Metal kernels precompiled for macOS ${CMAKE_OSX_DEPLOYMENT_TARGET} "
                    "(metal3.0, no bf16 — props.has_bfloat clamped to match)")
            else()
                set(GGML_METAL_STD "metal3.1" CACHE STRING "" FORCE)
                set(GGML_METAL_EMBED_LIBRARY_NO_BF16 OFF CACHE BOOL "" FORCE)
                message(STATUS "ASR: Metal kernels precompiled at build time (metal3.1, bf16)")
            endif()
        endif()

        # Every other non-CPU backend OFF (their source dirs are not vendored):
        foreach(_ggml_backend
                ACCELERATE BLAS CUDA HIP MUSA WEBGPU SYCL OPENCL
                OPENVINO RPC CANN ZDNN ZENDNN HEXAGON)
            set(GGML_${_ggml_backend} OFF CACHE BOOL "" FORCE)
        endforeach()
        if (_asr_use_prebuilt)
            # Consume the prebuilt static libs from the whisper-gpu release asset.
            # See scripts/build/build-whisper-vulkan-windows.ps1 for the producer.
            # Public headers still come from the vendored source tree (in git); only
            # the compiled .libs come off GitHub. Everything else — dep graph,
            # target names — matches what add_subdirectory would have produced, so
            # the rest of this file is unchanged.
            include(FetchContent)
            set(_whisper_gpu_ver "1.9.1")
            set(_whisper_gpu_sha "7671384731bc286e8eacaab36d2d69e5ffad5a6458fb47fff8e8dec361f68ca7")
            FetchContent_Declare(whisper_gpu_prebuilt
                URL      "https://github.com/aethersdr/AetherSDR/releases/download/whisper-gpu-${_whisper_gpu_ver}/whisper-gpu-${_whisper_gpu_ver}-windows-x86_64.zip"
                URL_HASH SHA256=${_whisper_gpu_sha}
            )
            FetchContent_MakeAvailable(whisper_gpu_prebuilt)
            set(_wg_libs "${whisper_gpu_prebuilt_SOURCE_DIR}/libs")
            set(_wc_root "${CMAKE_SOURCE_DIR}/third_party/whisper.cpp")

            find_package(Threads REQUIRED)
            foreach(_tgt IN ITEMS ggml-base ggml ggml-cpu ggml-vulkan whisper)
                add_library(${_tgt} STATIC IMPORTED)
                set_target_properties(${_tgt} PROPERTIES
                    IMPORTED_LOCATION "${_wg_libs}/${_tgt}.lib"
                )
            endforeach()
            # Header search paths — mirror what the source targets propagate.
            set_target_properties(ggml-base   PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_wc_root}/ggml/include")
            set_target_properties(ggml        PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_wc_root}/ggml/include")
            set_target_properties(ggml-cpu    PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_wc_root}/ggml/include")
            set_target_properties(ggml-vulkan PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_wc_root}/ggml/include")
            set_target_properties(whisper     PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_wc_root}/include;${_wc_root}/ggml/include")
            # Link graph — matches whisper.cpp/ggml's own target_link_libraries so
            # the final exe pulls in every transitive .lib and the Vulkan loader.
            set_target_properties(ggml-base   PROPERTIES INTERFACE_LINK_LIBRARIES "Threads::Threads")
            set_target_properties(ggml-cpu    PROPERTIES INTERFACE_LINK_LIBRARIES "ggml-base")
            set_target_properties(ggml-vulkan PROPERTIES INTERFACE_LINK_LIBRARIES "ggml-base;ggml-cpu;Vulkan::Vulkan")
            set_target_properties(ggml        PROPERTIES INTERFACE_LINK_LIBRARIES "ggml-base;ggml-cpu;ggml-vulkan")
            set_target_properties(whisper     PROPERTIES INTERFACE_LINK_LIBRARIES "ggml;Threads::Threads")
            message(STATUS "ASR: prebuilt whisper-gpu ${_whisper_gpu_ver} pack consumed (skipped ~1h+ MSVC compile)")
        else()
            # Build whisper/ggml as static objects, not .so's.
            set(_aether_saved_shared_libs ${BUILD_SHARED_LIBS})
            set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
            add_subdirectory(third_party/whisper.cpp EXCLUDE_FROM_ALL)
            set(BUILD_SHARED_LIBS ${_aether_saved_shared_libs} CACHE BOOL "" FORCE)

            if (MSVC)
                # MSVC's optimizer is pathologically slow on ggml's SIMD/quant kernels
                # (ggml-cpu/ops.cpp, ggml.c, ggml-quants.c, repack.cpp) — a cold Release
                # build of the vendored engine ran Windows CI from ~28 min to 3h+. These
                # kernels are explicitly vectorized (intrinsics) and memory-bandwidth-
                # bound, so /O1 keeps runtime fast enough while cutting MSVC compile time
                # dramatically. Scoped to the vendored ASR targets only; the rest of
                # AetherSDR keeps its normal optimization. (/O1 intentionally overrides
                # the config's /O2 — the D9025 override notice is expected.)
                foreach(_asr_msvc_target ggml-base ggml ggml-cpu ggml-vulkan whisper)
                    if (TARGET ${_asr_msvc_target})
                        target_compile_options(${_asr_msvc_target} PRIVATE /O1)
                    endif()
                endforeach()
            endif()
        endif()
    endif()

    # One name for "the whisper we link", resolved once above, so aetherasr and the
    # three whisper-linked tests below can't drift apart on the system/vendored
    # split. (asr_whisper_smoke_test, asr_whisper_backend_test, asr_gpu_probe_test.)
    if (USE_SYSTEM_LIBWHISPER)
        set(_asr_whisper_link PkgConfig::whisper ${_asr_ggml_target})
    else()
        set(_asr_whisper_link whisper)
    endif()

    # ASR ships as its OWN static library, deliberately NOT part of libaethercore.
    # Per the aetherd direction (RFC #3849/#4333), speech processing belongs on
    # the engine/headless side and streams only the resulting text to a thin UI;
    # the base engine and any thin client must not carry whisper/ggml. aetherasr
    # depends on nothing but Qt Core/Network + whisper, so it can be linked by the
    # processing host today (the monolith app) and by aetherd tomorrow — while
    # aethercore stays whisper-free. AsrEngine::finalText is the stream seam the
    # UI subscribes to (a direct signal now; over the wire later).
    add_library(aetherasr STATIC
        src/asr/AsrModelCatalog.cpp
        src/asr/AsrModelManager.cpp
        src/asr/AsrSegmenter.cpp
        src/asr/AsrEngine.cpp
        src/asr/SileroVad.cpp    # ONNX Silero VAD (inert stub unless HAVE_ONNX)
        src/asr/Fbank.cpp        # kaldi-style fbank for the speaker embedder
        src/asr/SpeakerEmbedder.cpp # ONNX speaker embedding (stub unless HAVE_ONNX)
        src/asr/SpeakerClusterer.cpp # online A/B/C speaker clustering
        src/asr/WhisperAsrBackend.cpp
        src/asr/RemoteAsrBackend.cpp
        src/asr/SherpaOnnxBackend.cpp # sherpa-onnx offline models (stub unless HAVE_SHERPA)
        src/core/Resampler.cpp   # standalone r8brain wrapper (24k->16k on the ASR worker)
    )
    target_include_directories(aetherasr PUBLIC src PRIVATE ${CMAKE_SOURCE_DIR}/third_party/r8brain)
    target_link_libraries(aetherasr PUBLIC Qt6::Core Qt6::Network PRIVATE Qt6::Concurrent ${_asr_whisper_link})
    target_compile_definitions(aetherasr PUBLIC AETHER_ASR_ENABLED=1)
    if (_asr_vulkan OR _asr_metal)
        target_compile_definitions(aetherasr PRIVATE AETHER_ASR_GPU=1)
    endif()
    if (_asr_metal_precompile)
        target_compile_definitions(aetherasr PRIVATE AETHER_ASR_METAL_PRECOMPILED=1)
    endif()
    set_target_properties(aetherasr PROPERTIES AUTOMOC ON)

    # The desktop app links ASR today; the compile define propagates PUBLICly so
    # the GUI can conditionally compile the audio tap + Copy Assist wiring.
    target_link_libraries(AetherSDR PRIVATE aetherasr)
    # App-layer glue (Phase 4 audio tap): connects AudioEngine's post-NR RX audio
    # to AsrEngine. Plus the Phase 5 Copy Assist UI (panel + controller + window).
    # Gated here so they only build when ASR is enabled.
    target_sources(AetherSDR PRIVATE
        src/gui/AsrAudioTap.cpp
        src/gui/CopyAssistPanel.cpp
        src/gui/CopyAssistSettings.cpp
        src/gui/CopyAssistSettingsDialog.cpp
        src/gui/CopyAssistController.cpp
    )

    message(STATUS "ASR: whisper.cpp enabled (CPU backend), separate aetherasr lib; weights download-on-demand")
else()
    message(STATUS "ASR: disabled (-DENABLE_ASR=OFF)")
endif()
# ----------------------------------------------------------------------------

if (USE_SYSTEM_ZLIB)
    target_link_libraries(aethercore PRIVATE PkgConfig::zlib)      # core: ZipArchive
else()
    target_link_libraries(aethercore PRIVATE zlibstatic) # bundled third_party/zlib 1.3.1
endif()

if (USE_SYSTEM_MSPACK)
    target_link_libraries(aethercore PRIVATE PkgConfig::libmspack) # core: CabExtractor
else()
    target_link_libraries(aethercore PRIVATE mspack_static)
endif()

# Link the system libmosquitto target only when MQTT is enabled — the
# pkg_check_modules() call that defines PkgConfig::libmosquitto lives
# inside the ENABLE_MQTT block above, so without this guard the combo
# (ENABLE_MQTT=OFF + USE_SYSTEM_LIBMOSQUITTO=ON) would configure cleanly
# then fail at link time with a missing target.
if (ENABLE_MQTT AND USE_SYSTEM_LIBMOSQUITTO)
    target_link_libraries(aethercore PRIVATE PkgConfig::libmosquitto) # core: MqttClient
endif()

# Optionally change binary name to lower case on Linux
if (LINUX AND LOWER_CASE_BINARY_NAME)
    set_target_properties(AetherSDR PROPERTIES OUTPUT_NAME aethersdr)

    set(AETHERSDR_OUTPUT_BINARY_NAME aethersdr)
else()
    set(AETHERSDR_OUTPUT_BINARY_NAME AetherSDR)
endif()
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/AetherSDR.desktop.in
    ${CMAKE_CURRENT_BINARY_DIR}/packaging/linux/AetherSDR.desktop
    @ONLY
)

# Capture the git short SHA at configure time and surface it in the About
# dialog so dev/test builds are identifiable.  Falls back to "unknown" for
# source-tarball builds where .git is absent.
find_package(Git QUIET)
set(AETHER_GIT_SHA "unknown")
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE _aether_git_sha_out
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _aether_git_sha_rv
    )
    if(_aether_git_sha_rv EQUAL 0 AND NOT "${_aether_git_sha_out}" STREQUAL "")
        set(AETHER_GIT_SHA "${_aether_git_sha_out}")
    endif()
endif()
target_compile_definitions(AetherSDR PRIVATE AETHER_GIT_SHA="${AETHER_GIT_SHA}")
if(Git_FOUND AND _aether_git_sha_rv EQUAL 0 AND NOT "${_aether_git_sha_out}" STREQUAL "")
    message(STATUS "Build SHA: ${AETHER_GIT_SHA}")
else()
    message(STATUS "Build SHA: ${AETHER_GIT_SHA} (git unavailable or not a git checkout)")
endif()

# Reproducible-builds.org SOURCE_DATE_EPOCH support.  When a distro
# packager (Debian, Arch, NixOS, openSUSE, Fedora, etc.) sets
#   export SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)
# before invoking cmake, GCC 7.2+ and Clang automatically substitute
# __DATE__ and __TIME__ with that fixed timestamp.  Many archive tools
# (ar, tar, zip, objcopy) honour the same variable, so the built binary
# becomes byte-reproducible across hosts.
#
# No source-tree changes needed — this block just surfaces a configure-
# time status line so packagers can confirm at a glance that their
# env-var actually reached the build.  Originally proposed as
# https://github.com/aethersdr/AetherSDR/pull/3139 (closed in favour
# of this ecosystem-standard approach).
if(DEFINED ENV{SOURCE_DATE_EPOCH})
    message(STATUS "Reproducible build: honoring SOURCE_DATE_EPOCH=$ENV{SOURCE_DATE_EPOCH}")
endif()

if((UNIX OR WIN32) AND ENABLE_DSTAR)
    target_compile_definitions(AetherSDR PRIVATE AETHER_ENABLE_DIGITAL_VOICE_HELPER)

    set(DIGITAL_VOICE_WAVEFORM_DIR "${CMAKE_SOURCE_DIR}/third_party/smartsdr-dsp")
    set(CRDV_DIR "${CMAKE_SOURCE_DIR}/third_party/crdv")
    add_library(aether_crdv STATIC
        ${CRDV_DIR}/src/air.c
        ${CRDV_DIR}/src/config.c
        ${CRDV_DIR}/src/control.c
        ${CRDV_DIR}/src/dv3000.c
        ${CRDV_DIR}/src/modem.c
        ${CRDV_DIR}/src/vita.c)
    add_library(crdv::crdv ALIAS aether_crdv)
    target_include_directories(aether_crdv PUBLIC ${CRDV_DIR}/include)
    target_compile_features(aether_crdv PUBLIC c_std_17)
    set_target_properties(aether_crdv PROPERTIES
        C_EXTENSIONS OFF
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF)
    if(MSVC)
        target_compile_options(aether_crdv PRIVATE /W4 /permissive-)
    else()
        target_compile_options(aether_crdv PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        target_link_libraries(aether_crdv PUBLIC m)
    endif()
    set(DIGITAL_VOICE_WAVEFORM_SOURCES
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/aether_dstar_protocol.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/cmd_basics.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/cmd_engine.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/aether_buffer_queue.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/aether_ipv4_source_filter.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/aether_smartsdr_command.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/aether_tcp_frame_buffer.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/aether_vita_packet_validator.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/discovery_client.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/digital_voice_mode_registry.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/digital_voice_slice_ownership.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/digital_voice_tx_gate.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/dstar_transmit_state.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/dstar_tx_output.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/dstar_tx_stream.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/dstar_waveform_metrics.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/hal_buffer.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/hal_listener.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/hal_vita.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/io_utils.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/sched_waveform.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/smartsdr_dsp_api.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/status_processor.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/traffic_cop.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/utils.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/vita_packet_sequence.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/vita_output.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/ThumbDV/bit_pattern_matcher.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/ThumbDV/gmsk_modem.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/ThumbDV/thumbDV.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/aether_vocoder_backend.cpp
        ${DIGITAL_VOICE_WAVEFORM_DIR}/aether_sem_compat.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/aether_serial_compat.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/circular_buffer.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/main.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/resampler.c
    )

    # ThumbDV is vendored C code and is also compiled directly by its queue
    # test, so suppress platform deprecation warnings at the source boundary.
    if(MSVC)
        set_source_files_properties(
            ${DIGITAL_VOICE_WAVEFORM_DIR}/ThumbDV/thumbDV.c
            PROPERTIES COMPILE_FLAGS "/w")
    else()
        set_source_files_properties(
            ${DIGITAL_VOICE_WAVEFORM_DIR}/ThumbDV/thumbDV.c
            PROPERTIES COMPILE_FLAGS "-w")
    endif()

    add_executable(aether-dv-waveform ${DIGITAL_VOICE_WAVEFORM_SOURCES})
    set_target_properties(aether-dv-waveform PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )
    target_compile_definitions(aether-dv-waveform PRIVATE _DEFAULT_SOURCE)
    if(WIN32)
        target_compile_definitions(aether-dv-waveform PRIVATE
            _CRT_SECURE_NO_WARNINGS
            _WINSOCK_DEPRECATED_NO_WARNINGS)
    endif()
    target_include_directories(aether-dv-waveform BEFORE PRIVATE
        $<$<BOOL:${WIN32}>:${DIGITAL_VOICE_WAVEFORM_DIR}/compat/windows>
        ${DIGITAL_VOICE_WAVEFORM_DIR}/compat
        ${DIGITAL_VOICE_WAVEFORM_DIR}/include
        ${DIGITAL_VOICE_WAVEFORM_DIR}
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface
        ${DIGITAL_VOICE_WAVEFORM_DIR}/ThumbDV
        ${CMAKE_SOURCE_DIR}/third_party/crdv/include
    )
    find_package(Threads REQUIRED)
    target_link_libraries(aether-dv-waveform PRIVATE Threads::Threads crdv::crdv)
    if(NOT WIN32)
        target_link_libraries(aether-dv-waveform PRIVATE m)
    endif()
    if(WIN32)
        target_link_libraries(aether-dv-waveform PRIVATE ws2_32)
    endif()
    if(APPLE)
        target_link_libraries(aether-dv-waveform PRIVATE "-framework IOKit")
        set(DIGITAL_VOICE_WAVEFORM_RUNTIME_DIR "${CMAKE_BINARY_DIR}/AetherSDR.app/Contents/MacOS")
        # AetherDV.cfg is a metadata manifest, not a Mach-O, so it must live in
        # Contents/Resources/ — codesign rejects a non-executable in MacOS/
        # ("code object is not signed at all"). The helper is env-var driven and
        # never reads the .cfg, so this is purely its packaging location.
        set(DIGITAL_VOICE_WAVEFORM_CONFIG_DIR "${CMAKE_BINARY_DIR}/AetherSDR.app/Contents/Resources")
    else()
        set(DIGITAL_VOICE_WAVEFORM_RUNTIME_DIR "${CMAKE_BINARY_DIR}")
        set(DIGITAL_VOICE_WAVEFORM_CONFIG_DIR "${CMAKE_BINARY_DIR}")
    endif()
    set_target_properties(aether-dv-waveform PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${DIGITAL_VOICE_WAVEFORM_RUNTIME_DIR}"
    )
    if(NOT MSVC)
        target_compile_options(aether-dv-waveform PRIVATE -w)
    endif()
    add_custom_command(TARGET aether-dv-waveform POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DIGITAL_VOICE_WAVEFORM_CONFIG_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DIGITAL_VOICE_WAVEFORM_DIR}/config/AetherDV.cfg"
            "${DIGITAL_VOICE_WAVEFORM_CONFIG_DIR}/AetherDV.cfg"
        COMMAND ${CMAKE_COMMAND} -E rm -f
            "$<TARGET_FILE_DIR:aether-dv-waveform>/aether-dstar-waveform"
            "$<TARGET_FILE_DIR:aether-dv-waveform>/aether-dstar-waveform.exe"
            "$<TARGET_FILE_DIR:aether-dv-waveform>/ThumbDV.cfg"
        COMMENT "Copying AetherDV waveform manifest"
    )
    if(APPLE)
        # Clear a stale AetherDV.cfg left in the OLD Contents/MacOS/ location by
        # an incremental build tree from before the move to Contents/Resources/.
        # Apple-only: elsewhere RUNTIME_DIR == CONFIG_DIR == ${CMAKE_BINARY_DIR},
        # so removing it there would delete the manifest just staged above.
        add_custom_command(TARGET aether-dv-waveform POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -f
                "${DIGITAL_VOICE_WAVEFORM_RUNTIME_DIR}/AetherDV.cfg"
        )
    endif()

    add_dependencies(AetherSDR aether-dv-waveform)
endif()

# Windows: GUI app (no console window) + icon resource
if(WIN32)
    set_target_properties(AetherSDR PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# macOS app bundle
if(APPLE)
    target_sources(AetherSDR PRIVATE src/MacStartupAbortGuard.cpp)
    set_target_properties(AetherSDR PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.aethersdr.AetherSDR"
        MACOSX_BUNDLE_BUNDLE_NAME "AetherSDR"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "AetherSDR.icns"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in"
    )
    target_link_libraries(aethercore PRIVATE "-framework AVFoundation" "-framework Accelerate") # core: MacNRFilter/VirtualAudioBridge
    target_sources(aethercore PRIVATE src/core/MacNRFilter.cpp)

    # Generate AetherSDR.icns at build time from docs/assets/logo-circle.png so local
    # builds get an app icon without any extra setup (sips and iconutil are
    # part of macOS).
    set(ICON_SOURCE "${CMAKE_SOURCE_DIR}/docs/assets/logo-circle.png")
    set(ICNS_PATH "${CMAKE_BINARY_DIR}/AetherSDR.icns")
    set(ICONSET_PATH "${CMAKE_BINARY_DIR}/AetherSDR.iconset")
    add_custom_command(
        OUTPUT "${ICNS_PATH}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ICONSET_PATH}"
        COMMAND sips -z 16  16  "${ICON_SOURCE}" --out "${ICONSET_PATH}/icon_16x16.png"
        COMMAND sips -z 32  32  "${ICON_SOURCE}" --out "${ICONSET_PATH}/icon_16x16@2x.png"
        COMMAND sips -z 32  32  "${ICON_SOURCE}" --out "${ICONSET_PATH}/icon_32x32.png"
        COMMAND sips -z 64  64  "${ICON_SOURCE}" --out "${ICONSET_PATH}/icon_32x32@2x.png"
        COMMAND sips -z 128 128 "${ICON_SOURCE}" --out "${ICONSET_PATH}/icon_128x128.png"
        COMMAND sips -z 256 256 "${ICON_SOURCE}" --out "${ICONSET_PATH}/icon_128x128@2x.png"
        COMMAND sips -z 256 256 "${ICON_SOURCE}" --out "${ICONSET_PATH}/icon_256x256.png"
        COMMAND sips -z 512 512 "${ICON_SOURCE}" --out "${ICONSET_PATH}/icon_256x256@2x.png"
        COMMAND sips -z 512 512 "${ICON_SOURCE}" --out "${ICONSET_PATH}/icon_512x512.png"
        COMMAND sips -z 1024 1024 "${ICON_SOURCE}" --out "${ICONSET_PATH}/icon_512x512@2x.png"
        COMMAND iconutil -c icns "${ICONSET_PATH}" -o "${ICNS_PATH}"
        DEPENDS "${ICON_SOURCE}"
        COMMENT "Generating AetherSDR.icns"
    )
    target_sources(AetherSDR PRIVATE "${ICNS_PATH}")
    set_source_files_properties("${ICNS_PATH}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
elseif(WIN32)
    # Windows application icon (taskbar, Start Menu, Alt-Tab)
    set(WIN_RC "${CMAKE_SOURCE_DIR}/packaging/windows/AetherSDR.rc")
    if(EXISTS "${WIN_RC}")
        target_sources(AetherSDR PRIVATE "${WIN_RC}")
    endif()
    if(MSVC)
        # Embed the application manifest the CMake-native way: list it as a
        # source so CMake hands it to its own vs_link_exe/mt.exe embed step.
        # Passing /MANIFEST:EMBED + /MANIFESTINPUT directly fights that
        # machinery — link.exe embeds inline and emits no side-car manifest,
        # so CMake's mt.exe step runs with an empty --manifests list and dies
        # with c10100a7 (cmake 4.x) / LNK1220 the other way (#3237).
        target_sources(AetherSDR PRIVATE
            "${CMAKE_SOURCE_DIR}/packaging/windows/AetherSDR.exe.manifest")
    endif()
endif()

# PUBLIC: src/ is the project include root for both layers ("core/..." and
# "gui/..." includes); the vendored include dirs leak into headers used by
# gui translation units today (e.g. hidapi via MainWindow_Controllers.cpp).
target_include_directories(aethercore PUBLIC
    src/
    ${RNNOISE_DIR}/include
    ${RNNOISE_DIR}/src
    ${GGMORSE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/r8brain
)

# Engine dependency surface. Qt6::Widgets is still linked into the engine
# because five core files use QtWidgets (TxKeyingMarker.h, ThemeManager.cpp,
# AutomationServer.cpp, ShortcutManager.cpp, SettingsHelpers.cpp). These are
# tracked as EB2 legacy in tools/check_engine_boundary.py (per-file baseline
# counts that may only shrink) and split out in step 3 — AutomationServer's
# widget-facing dumpTree/grab half is the bulk of it.
target_link_libraries(aethercore PUBLIC
    aether_sqlite3
    Qt6::Core
    Qt6::Concurrent
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Multimedia
    aether_libmodem_core
    aether_afskdemod
    ${CMAKE_DL_LIBS}   # NvidiaAfxFilter dlopen
)

target_link_libraries(AetherSDR PRIVATE
    qgeoview           # gui/map only
    ${CMAKE_DL_LIBS}   # dlopen/dlsym for tolerant X11 error handler (#1839)
)

if(Qt6SerialPort_FOUND)
    # PUBLIC: gui (MainWindow.cpp, RadioSetupDialog.cpp) uses QSerialPort and
    # #ifdef HAVE_SERIALPORT directly.
    target_compile_definitions(aethercore PUBLIC HAVE_SERIALPORT)
    target_link_libraries(aethercore PUBLIC Qt6::SerialPort)
endif()

if(Qt6WebSockets_FOUND)
    # PUBLIC: gui (MainWindow_Spots.cpp) uses QWebSocket directly.
    target_compile_definitions(aethercore PUBLIC HAVE_WEBSOCKETS)
    target_sources(aethercore PRIVATE
        src/core/FreeDvClient.cpp
    )
    target_sources(AetherSDR PRIVATE
        src/gui/FreeDvReporterDialog.cpp
        src/gui/FreeDvReporterModel.cpp
    )
    target_link_libraries(aethercore PUBLIC Qt6::WebSockets)
endif()

if(Qt6Keychain_FOUND)
    # PUBLIC (both): gui setup tabs test HAVE_KEYCHAIN *and* MqttApplet.cpp /
    # MqttSettingsDialog.cpp include <qt6keychain/keychain.h> directly, so the
    # exe's gui TUs need the imported target's INTERFACE_INCLUDE_DIRECTORIES.
    # A PRIVATE link on a static lib propagates only $<LINK_ONLY:...> — the
    # include dir would be lost and the gui TU fails to find the header on any
    # platform where qtkeychain is not on a default include path (e.g. the
    # bundled third_party/qtkeychain on Windows). Matches Qt6::SerialPort /
    # Qt6::WebSockets, which are PUBLIC for the same reason.
    target_compile_definitions(aethercore PUBLIC HAVE_KEYCHAIN)
    target_link_libraries(aethercore PUBLIC Qt6Keychain::Qt6Keychain)
    # Local Windows builds run straight from the build dir with no packaging
    # step — windeployqt, which stages qt6keychain.dll for the installer, isn't
    # involved — so the DLL needs to land next to the exe or the app fails to
    # start with "qt6keychain.dll was not found".
    if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/qtkeychain/bin/qt6keychain.dll")
        add_custom_command(TARGET AetherSDR POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/third_party/qtkeychain/bin/qt6keychain.dll"
                "$<TARGET_FILE_DIR:AetherSDR>"
        )
    endif()
    # Same problem on macOS: a staged third_party/qtkeychain dylib is found
    # via an absolute rpath into THIS checkout, so a bundle copied to another
    # Mac (SFTP test deploys) fails to load it.  Ship the dylib inside the
    # bundle and add the standard Frameworks rpath at link time (linker-added
    # rpath keeps the ad-hoc signature valid — no install_name_tool).
    # macdeployqt still handles release DMGs; copy_if_different is a no-op
    # for it.
    if(APPLE AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/qtkeychain/lib/libqt6keychain.1.dylib")
        set_property(TARGET AetherSDR APPEND PROPERTY
            BUILD_RPATH "@executable_path/../Frameworks")
        add_custom_command(TARGET AetherSDR POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_CONTENT_DIR:AetherSDR>/Frameworks"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/third_party/qtkeychain/lib/libqt6keychain.1.dylib"
                "$<TARGET_BUNDLE_CONTENT_DIR:AetherSDR>/Frameworks"
        )
    endif()
endif()

if(Qt6DBus_FOUND)
    target_compile_definitions(aethercore PUBLIC HAVE_DBUS)
    target_link_libraries(aethercore PRIVATE Qt6::DBus)
endif()

if(AETHER_GPU_SPECTRUM)
    # PUBLIC on the engine: src/core/AutomationServer.cpp compiles QRhiWidget
    # grab paths under #ifdef AETHER_GPU_SPECTRUM (QRhiWidget is public
    # QtWidgets API on Qt 6.7+, so no GuiPrivate needed engine-side).
    target_compile_definitions(aethercore PUBLIC AETHER_GPU_SPECTRUM)

    # Apply the manual private-include paths now that AetherSDR exists. Covers
    # Debian multi-arch and Windows/macOS aqt installs that lack the Qt6::GuiPrivate
    # target (headers present on disk — located during detection above).
    if(DEBIAN_GPU_FIX_REQUIRED)
        message(STATUS "Applying private QtGui include paths to AetherSDR (${DEBIAN_PRIVATE_INC})")
        target_include_directories(AetherSDR PRIVATE
            "${DEBIAN_PRIVATE_INC}/QtGui/${Qt6_VERSION}"
            "${DEBIAN_PRIVATE_INC}/QtGui/${Qt6_VERSION}/QtGui"
        )
    elseif(QT_FRAMEWORK_PRIVATE_INC)
        # macOS framework layout located during detection. Both dirs, matching
        # what Qt's own Qt6::Gui generator expression adds when the private
        # module is available: <ver>/ resolves QtGui/private/... spellings,
        # <ver>/QtGui/ resolves the <rhi/qrhi.h> the spectrum widgets include.
        message(STATUS "Applying framework private QtGui include paths to AetherSDR (${QT_FRAMEWORK_PRIVATE_INC})")
        target_include_directories(AetherSDR PRIVATE
            "${QT_FRAMEWORK_PRIVATE_INC}"
            "${QT_FRAMEWORK_PRIVATE_INC}/QtGui"
        )
    endif()

    if(TARGET Qt6::GuiPrivate)
        target_link_libraries(AetherSDR PRIVATE Qt6::GuiPrivate)
    elseif(NOT DEBIAN_GPU_FIX_REQUIRED AND NOT QT_FRAMEWORK_PRIVATE_INC)
        # No GuiPrivate target and neither manual path applied — last resort:
        # locate the rhi/ headers within the Qt6::Gui include dirs. Qt's macOS
        # framework layout used to land here and no longer does (the branch
        # above handles it), so this now covers only distributions that expose
        # rhi/ directly on the Qt6::Gui include path.
        get_target_property(_qt_gui_inc Qt6::Gui INTERFACE_INCLUDE_DIRECTORIES)
        foreach(_dir IN LISTS _qt_gui_inc)
            if(EXISTS "${_dir}/rhi/qrhi.h")
                break() # already on the include path via Qt6::Gui
            endif()
            file(GLOB _priv_dirs "${_dir}/${Qt6_VERSION}/QtGui")
            foreach(_pd IN LISTS _priv_dirs)
                if(EXISTS "${_pd}/rhi/qrhi.h")
                    target_include_directories(AetherSDR PRIVATE "${_pd}")
                    message(STATUS "QRhi headers found at ${_pd}/rhi/")
                    break()
                endif()
            endforeach()
        endforeach()
    endif()

    # Shaders in this list get qsb's default GLSL set (100es/120/150). Anything
    # using GLSL 130+ syntax (e.g. the `flat` qualifier) must NOT go here — the
    # 120 slice would bake illegal code that only the driver rejects, on-device,
    # where CI can't see it. Put such shaders in the GLSL-130+ block below;
    # tools/check_shader_dialects.py fails the build-gate if they land here.
    qt_add_shaders(AetherSDR "aether_shaders"
        PREFIX "/shaders"
        FILES
            resources/shaders/texturedquad.vert
            resources/shaders/texturedquad.frag
            resources/shaders/texturedquad_rowframes.frag
            resources/shaders/overlay.vert
            resources/shaders/overlay.frag
            resources/shaders/overlay_frequency_preview.frag
            resources/shaders/spectrum.vert
            resources/shaders/spectrum.frag
            resources/shaders/panscope.frag
            resources/shaders/wavescope.frag
    )

    # Shaders needing GLSL 130+. dss_mesh (3D-FFT stacked-trace) is the only one
    # today: it uses the `flat` interpolation qualifier — added for ridge
    # stability in #4539 — which is legal only in GLSL 130 / ES 300 and later.
    # SPIRV-Cross cannot lower an interpolation qualifier the way it lowers
    # explicit locations and UBO blocks, so `flat` reaches every baked slice
    # verbatim; the qsb default (100es, 120, 150) therefore bakes a 120 slice
    # containing `flat varying float …`, which is a syntax error, and a 100es
    # slice where `flat` is a reserved word. Both still BAKE — qsb exits 0 — so
    # the only symptom is a driver rejecting the shader at runtime.
    #
    # Bake instead the flat-capable slices our GL backends actually report:
    # 130 (OpenGL 3.0, e.g. Mesa compat-profile caps), 140 (desktop OpenGL 3.1 —
    # the Raspberry Pi's v3d driver, which is what #4746 hit: no 140 slice baked,
    # so QRhi fell back to 120 and the 3D FFT was silently disabled), 150
    # (OpenGL 3.2+, what desktop GPUs already selected — byte-identical to
    # before), and 300es (OpenGL ES 3, which previously matched only the illegal
    # 100es slice). HLSL 50 / MSL 12 / SPIR-V are untouched: only GLSL is
    # overridden, so the D3D11, Metal and Vulkan paths bake exactly as before.
    #
    # On an older GL<3.0 / GLES2 context no slice matches. QShader stays valid,
    # so this is NOT caught by the vs.isValid() guard — QRhi finds no GLSL code
    # and the m_dssMeshFillPipeline->create() call in
    # SpectrumWidget::initDssMeshPipeline fails, m_dssMeshReady stays false, and
    # the CPU image fallback still renders the 3D FFT. That is no worse than
    # before: those contexts previously selected 120/100es, which never
    # compiled. No shader-source change — #4539's `flat` is preserved.
    # Complementary to #4730 (OpenGL outline rendering).
    qt_add_shaders(AetherSDR "aether_shaders_glsl130"
        PREFIX "/shaders"
        GLSL "130,140,150,300es"
        FILES
            resources/shaders/dss_mesh.vert
            resources/shaders/dss_mesh.frag
    )
endif()

# Bundled libmspack (LGPL-2.1) — CAB+LZX decompression for the v4.2+ MSI
# firmware-installer extraction path. See third_party/libmspack/README.md.
if (USE_SYSTEM_MSPACK)
    find_package(PkgConfig REQUIRED)
    if(PkgConfig_FOUND)
        pkg_check_modules(libmspack REQUIRED IMPORTED_TARGET libmspack)
    endif()
else()
    add_subdirectory(third_party/libmspack)
endif()

# Bundled RtMidi (MIT license) — MIDI controller support on all platforms
message(STATUS "MIDI controller support enabled (RtMidi)")
target_compile_definitions(aethercore PUBLIC HAVE_MIDI) # gui MidiMappingDialog + MainWindow check it too
target_sources(aethercore PRIVATE
    src/core/MidiControlManager.cpp
    src/core/MidiSettings.cpp)
target_sources(AetherSDR PRIVATE
    src/gui/MidiMappingDialog.cpp)
if (USE_SYSTEM_RTMIDI)
    find_package(PkgConfig REQUIRED)
    if(PkgConfig_FOUND)
        pkg_check_modules(rtmidi REQUIRED IMPORTED_TARGET rtmidi)
    endif()
    target_link_libraries(aethercore PUBLIC PkgConfig::rtmidi)
else()
    target_sources(aethercore PRIVATE third_party/rtmidi/RtMidi.cpp)
    if(MSVC)
        set_source_files_properties(third_party/rtmidi/RtMidi.cpp PROPERTIES COMPILE_FLAGS "/w")
    else()
        set_source_files_properties(third_party/rtmidi/RtMidi.cpp PROPERTIES COMPILE_FLAGS "-w")
    endif()
    # PUBLIC: MidiControlManager.h includes RtMidi.h
    target_include_directories(aethercore PUBLIC third_party/rtmidi)
endif()

if(APPLE)
    # RtMidi backend selectors — PUBLIC because MidiControlManager.h includes
    # RtMidi.h (its class layout is backend-conditional in places).
    target_compile_definitions(aethercore PUBLIC __MACOSX_CORE__)
    target_link_libraries(aethercore PRIVATE "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation" "-framework IOKit")
elseif(WIN32)
    target_compile_definitions(aethercore PUBLIC __WINDOWS_MM__)
    target_link_libraries(aethercore PRIVATE winmm)
    target_link_libraries(aethercore PRIVATE dxgi)  # GpuSelector adapter enumeration
else()
    target_compile_definitions(aethercore PUBLIC __LINUX_ALSA__)
    find_package(PkgConfig REQUIRED)
    if(PkgConfig_FOUND)
        pkg_check_modules(alsa REQUIRED IMPORTED_TARGET alsa)
    endif()
    target_link_libraries(aethercore PRIVATE PkgConfig::alsa)
endif()

# hidapi — USB HID encoder support (Stream Deck, Icom RC-28, Griffin PowerMate, Contour Shuttle)
# Windows: run scripts/setup/setup-hidapi.ps1 first to download and build hidapi
# Linux:   apt install libhidapi-dev
# macOS:   brew install hidapi
if(WIN32)
    set(HIDAPI_ROOT "${CMAKE_SOURCE_DIR}/third_party/hidapi")
    if(EXISTS "${HIDAPI_ROOT}/include/hidapi/hidapi.h")
        set(HIDAPI_FOUND TRUE)
        set(HIDAPI_INCLUDE_DIRS "${HIDAPI_ROOT}/include")
        set(HIDAPI_LIBRARIES "${HIDAPI_ROOT}/lib/hidapi.lib")
        set(HIDAPI_DLL "${HIDAPI_ROOT}/bin/hidapi.dll")
    else()
        message(WARNING "hidapi not found. Run scripts/setup/setup-hidapi.ps1 to download it. "
                        "USB HID device support (Stream Deck, etc.) will be disabled.")
    endif()
else()
    if(PkgConfig_FOUND)
        pkg_check_modules(HIDAPI hidapi-hidraw)
        if(NOT HIDAPI_FOUND)
            pkg_check_modules(HIDAPI hidapi-libusb)
        endif()
        if(NOT HIDAPI_FOUND)
            pkg_check_modules(HIDAPI hidapi)
        endif()
    endif()
endif()
# Ulanzi Dial backend — one per platform.  All three implementations
# expose the same Qt signal contract (see UlanziDialBackend.h); the
# mapper dialog and MainWindow dispatcher are unchanged across
# platforms.  See #3232 for the design.
target_sources(aethercore PRIVATE
    src/core/UlanziDialBackend.h    # header-only Q_OBJECT — listed so AUTOMOC mocs it in the engine
    src/core/UlanziChordDecoder.cpp # chord decode shared by all three backends (ulanzi_chord_decoder_test)
    src/core/UlanziChordDecoder.h
    src/core/UlanziDialMappings.cpp # pill->action document owner (ulanzi_mapping_migration_test)
    src/core/UlanziDialMappings.h
    src/core/backends/IRadioBackend.h)  # aetherd RFC step 2.1: radio-facing seam (§5.5); header-only Q_OBJECT
target_sources(AetherSDR PRIVATE
    src/gui/UlanziDialMapperDialog.cpp
    src/gui/UlanziDialMapperDialog.h)
if(UNIX AND NOT APPLE)
    target_sources(aethercore PRIVATE
        src/core/EvdevEncoderManager.cpp
        src/core/EvdevEncoderManager.h)
endif()
if(WIN32)
    target_sources(aethercore PRIVATE
        src/core/UlanziDialWindowsManager.cpp
        src/core/UlanziDialWindowsManager.h)
endif()
if(APPLE)
    target_sources(aethercore PRIVATE
        src/core/UlanziDialMacOSManager.cpp
        src/core/UlanziDialMacOSManager.h)
    target_link_libraries(aethercore PRIVATE
        "-framework IOKit"
        "-framework CoreFoundation")
endif()

if(HIDAPI_FOUND)
    message(STATUS "hidapi found — USB HID encoder support enabled")
    # PUBLIC: gui (MainWindow_Controllers.cpp) includes hidapi.h and checks
    # HAVE_HIDAPI directly.
    target_compile_definitions(aethercore PUBLIC HAVE_HIDAPI)
    target_sources(aethercore PRIVATE
        src/core/HidEncoderManager.cpp
        src/core/HidEncoderManager.h
        src/core/HidDeviceParser.cpp
        src/core/HidDeviceParser.h)
    target_sources(AetherSDR PRIVATE
        src/gui/RC28MappingDialog.cpp
        src/gui/RC28MappingDialog.h)
    set(HIDAPI_NORMALIZED_INCLUDE_DIRS ${HIDAPI_INCLUDE_DIRS})
    foreach(hidapi_dir IN LISTS HIDAPI_INCLUDE_DIRS)
        if(EXISTS "${hidapi_dir}/hidapi.h")
            get_filename_component(hidapi_leaf "${hidapi_dir}" NAME)
            if(hidapi_leaf STREQUAL "hidapi")
                get_filename_component(hidapi_parent "${hidapi_dir}" DIRECTORY)
                list(APPEND HIDAPI_NORMALIZED_INCLUDE_DIRS "${hidapi_parent}")
            endif()
        endif()
    endforeach()
    list(REMOVE_DUPLICATES HIDAPI_NORMALIZED_INCLUDE_DIRS)
    target_include_directories(aethercore PUBLIC ${HIDAPI_NORMALIZED_INCLUDE_DIRS})
    # pkg_check_modules sets HIDAPI_LIBRARIES to a bare name ("hidapi"), so the
    # link needs the -L that HIDAPI_LIBRARY_DIRS carries — exactly as portaudio
    # and fftw3 do below. hidapi was the one dependency missing it, and it went
    # unnoticed because the only prefix it ever came from on macOS was
    # Homebrew's, already covered by the global link_directories() near the top
    # of this file. #4706 moved hidapi to a from-source prefix so it could be
    # built at the deployment target, which is off every default search path:
    # CMake still reported "Found hidapi, version 0.15.0" from the .pc file and
    # the DMG build then died at 100% with "ld: library 'hidapi' not found".
    # Empty on Windows, where HIDAPI_LIBRARIES is a full path — a no-op there.
    #
    # PUBLIC, not PRIVATE: aethercore is a STATIC library, so the link that has
    # to resolve -lhidapi is the AetherSDR executable's. PRIVATE link libraries
    # still reach it (CMake records them behind $<LINK_ONLY:>), but PRIVATE link
    # DIRECTORIES do not — which produces exactly one symptom, -lhidapi on the
    # exe link line with no -L to find it by. PRIVATE here was tried first and
    # failed the DMG build the same way.
    target_link_directories(aethercore PUBLIC ${HIDAPI_LIBRARY_DIRS})
    target_link_libraries(aethercore PRIVATE ${HIDAPI_LIBRARIES})
    # Copy DLL to build dir on Windows
    if(WIN32 AND HIDAPI_DLL)
        add_custom_command(TARGET AetherSDR POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${HIDAPI_DLL}" "$<TARGET_FILE_DIR:AetherSDR>"
            COMMENT "Copying hidapi.dll to build directory"
        )
    endif()
else()
    message(STATUS "hidapi not found — USB HID encoder support disabled")
endif()

# ONNX Runtime — powers the CNN signal classifier AND the ASR ONNX features
# (Silero VAD + speaker labeling). Release builds get a prebuilt drop under
# third_party/onnxruntime via the setup scripts (checked first, all platforms);
# dev boxes can also use a system install.
#   Linux/macOS: scripts/setup/setup-onnxruntime.sh   (or: brew install onnxruntime / apt)
#   Windows:     scripts/setup/setup-onnxruntime.ps1
set(ORT_ROOT "${CMAKE_SOURCE_DIR}/third_party/onnxruntime")
if(EXISTS "${ORT_ROOT}/include/onnxruntime_cxx_api.h")
    # Prebuilt drop from the setup scripts — used by the release builds.
    set(ORT_FOUND TRUE)
    set(ORT_INCLUDE_DIRS "${ORT_ROOT}/include")
    if(WIN32)
        set(ORT_LIBRARIES "${ORT_ROOT}/lib/onnxruntime.lib")
        file(GLOB ORT_DLLS "${ORT_ROOT}/bin/*.dll" "${ORT_ROOT}/lib/*.dll")
    else()
        # Link the unversioned symlink; keep every versioned object for packaging.
        file(GLOB ORT_RUNTIME_LIBS
            "${ORT_ROOT}/lib/libonnxruntime.so*" "${ORT_ROOT}/lib/libonnxruntime*.dylib")
        if(APPLE)
            set(ORT_LIBRARIES "${ORT_ROOT}/lib/libonnxruntime.dylib")
        else()
            set(ORT_LIBRARIES "${ORT_ROOT}/lib/libonnxruntime.so")
        endif()
    endif()
elseif(NOT WIN32)
    if(PkgConfig_FOUND)
        pkg_check_modules(ORT libonnxruntime)
    endif()
    if(NOT ORT_FOUND)
        find_library(ORT_LIB onnxruntime)
        find_path(ORT_INC onnxruntime_cxx_api.h)
        if(ORT_LIB AND ORT_INC)
            set(ORT_FOUND TRUE)
            set(ORT_LIBRARIES ${ORT_LIB})
            set(ORT_INCLUDE_DIRS ${ORT_INC})
        endif()
    endif()
endif()
if(ORT_FOUND)
    message(STATUS "ONNX Runtime found — ASR ONNX (Silero VAD, speaker labeling) + signal classifier enabled")
elseif(REQUIRE_ASR_ONNX)
    message(FATAL_ERROR "REQUIRE_ASR_ONNX is set but ONNX Runtime was not found. "
        "Run scripts/setup/setup-onnxruntime.sh (Linux/macOS) or setup-onnxruntime.ps1 (Windows) "
        "to stage a prebuilt runtime under third_party/onnxruntime, then reconfigure.")
else()
    message(STATUS "ONNX Runtime not found — ASR ONNX features + signal classifier disabled "
        "(set REQUIRE_ASR_ONNX=ON to make this a hard error in release builds)")
endif()

# sherpa-onnx (detected further below) bundles the SAME ONNX Runtime version we
# stage (1.27 — the setup scripts are pinned to match). If it's staged, link its
# libonnxruntime for our ORT code too, so the app ships/loads a single runtime
# instead of two identical copies, and there's no rpath ambiguity.
#
# macOS is the exception, and it goes the other way: same version, but NOT the
# same binary. k2-fsa build their copy on whatever macOS their runner happens to
# be, which stamps LC_BUILD_VERSION minos 15.5 into both slices of the
# universal2 archive; Microsoft's own 1.27.0 build is 14.0. dyld enforces that
# stamp on every dylib macdeployqt stages, so preferring sherpa's would set the
# DMG's real minimum macOS to 15.5 no matter what CMAKE_OSX_DEPLOYMENT_TARGET
# says — the app would not launch at all below it, which is #4532.
# setup-sherpa-onnx.sh deletes sherpa's copy on macOS for the same reason; the
# install name and C API are identical, so sherpa's own dylib resolves against
# the upstream runtime unchanged. Upstream publishes no x86_64 macOS build, so
# Intel Macs get no ORT and no sherpa backend at all — see macos-dmg.yml.
if(ORT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx/lib")
    if(WIN32)
        # sherpa's Windows bundle carries onnxruntime.lib + onnxruntime.dll; link
        # its import lib and drop our ORT DLL copy (sherpa's onnxruntime.dll ships
        # via SHERPA_DLLS) so only one onnxruntime.dll lands next to the exe.
        if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx/lib/onnxruntime.lib")
            set(ORT_LIBRARIES "${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx/lib/onnxruntime.lib")
            set(ORT_DLLS "")
            message(STATUS "ONNX Runtime: sharing sherpa-onnx's bundled 1.27 runtime (one copy for the app)")
        endif()
    elseif(APPLE)
        message(STATUS "ONNX Runtime: keeping the upstream 1.27 runtime, not sherpa's "
            "(sherpa's macOS build is stamped minos 15.5 and would become the bundle's floor)")
    else()
        file(GLOB _sherpa_ort
            "${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx/lib/libonnxruntime.so")
        if(_sherpa_ort)
            list(GET _sherpa_ort 0 ORT_LIBRARIES)
            message(STATUS "ONNX Runtime: sharing sherpa-onnx's bundled 1.27 runtime (one copy for the app)")
        endif()
    endif()
endif()

if(PORTAUDIO_FOUND)
    target_sources(aethercore PRIVATE src/core/CwSidetonePortAudioSink.cpp)
    target_compile_definitions(aethercore PUBLIC HAVE_PORTAUDIO)
    target_include_directories(aethercore PRIVATE ${PORTAUDIO_INCLUDE_DIRS})
    # PUBLIC for the reason spelled out at the hidapi block above: aethercore is
    # static, so a PRIVATE link directory never reaches the executable that does
    # the linking. portaudio has not failed yet only because its library has so
    # far always sat on a default search path; that stopped being guaranteed when
    # #4706 moved it into third_party/macos-deps.
    target_link_directories(aethercore PUBLIC ${PORTAUDIO_LIBRARY_DIRS})
    target_link_libraries(aethercore PRIVATE ${PORTAUDIO_LIBRARIES})
endif()

if(FFTW3_FOUND)
    target_compile_definitions(aethercore PUBLIC HAVE_FFTW3
        # On Windows lld-link (used by ClangCL) requires explicit dllimport
        # declarations; define FFTW_DLL so fftw3.h emits __declspec(dllimport)
        # and the linker can resolve __imp_fftwf_* symbols from the import lib.
        $<$<BOOL:${WIN32}>:FFTW_DLL>)
    # PUBLIC: core/SpectralNR.h includes <fftw3.h> under HAVE_FFTW3 (also
    # PUBLIC), and SpectralNR.h is pulled in by AudioEngine.h — a header many
    # gui TUs include. A PRIVATE include dir is lost to the exe, so those gui
    # TUs fail to find fftw3.h wherever it is off the default path (Windows
    # bundled third_party/fftw3). The library link stays PRIVATE (symbols reach
    # the exe transitively through the static lib).
    target_include_directories(aethercore PUBLIC ${FFTW3_INCLUDE_DIRS})
    # PUBLIC — same static-library propagation rule as hidapi and portaudio.
    target_link_directories(aethercore PUBLIC ${FFTW3_LIBRARY_DIRS})
    target_link_libraries(aethercore PRIVATE ${FFTW3_LIBRARIES})
    # Copy DLL to build dir on Windows
    if(WIN32 AND FFTW3_DLL)
        add_custom_command(TARGET AetherSDR POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFTW3_DLL}" "$<TARGET_FILE_DIR:AetherSDR>"
            COMMENT "Copying libfftw3-3.dll to build directory"
        )
    endif()
endif()

if(ORT_FOUND)
    target_compile_definitions(aethercore PUBLIC HAVE_ONNX)
    # PUBLIC include dir: core/SignalClassifier.h includes onnxruntime under
    # HAVE_ONNX (PUBLIC) and is pulled in by gui/MainWindow.h. Same transitive
    # leak as FFTW3 above. Library link stays PRIVATE (transitive via the lib).
    target_include_directories(aethercore PUBLIC ${ORT_INCLUDE_DIRS})
    target_link_libraries(aethercore PRIVATE ${ORT_LIBRARIES})
    if(WIN32 AND ORT_DLLS)
        foreach(_ort_dll IN LISTS ORT_DLLS)
            add_custom_command(TARGET AetherSDR POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_ort_dll}" "$<TARGET_FILE_DIR:AetherSDR>"
                COMMENT "Copying ONNX Runtime DLL to build directory")
        endforeach()
    endif()
    # Give aetherasr the same ONNX Runtime so SileroVad can run the learned VAD
    # (the segmenter falls back to energy VAD when this isn't built).
    if(TARGET aetherasr)
        target_compile_definitions(aetherasr PRIVATE HAVE_ONNX)
        target_include_directories(aetherasr PRIVATE ${ORT_INCLUDE_DIRS})
        target_link_libraries(aetherasr PRIVATE ${ORT_LIBRARIES})
    endif()
endif()

# sherpa-onnx — optional non-whisper ASR backend (offline models via its C API:
# transducer / Moonshine / Paraformer / CTC …). Staged under third_party/sherpa-onnx
# by scripts/setup/setup-sherpa-onnx.sh (Linux/macOS). Ships its own ONNX Runtime.
set(SHERPA_ROOT "${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx")
if(EXISTS "${SHERPA_ROOT}/include/sherpa-onnx/c-api/c-api.h")
    set(SHERPA_FOUND TRUE)
    set(SHERPA_INCLUDE_DIRS "${SHERPA_ROOT}/include")
    if(WIN32)
        set(SHERPA_LIBRARIES "${SHERPA_ROOT}/lib/sherpa-onnx-c-api.lib")
        file(GLOB SHERPA_DLLS "${SHERPA_ROOT}/bin/*.dll" "${SHERPA_ROOT}/lib/*.dll")
    elseif(APPLE)
        set(SHERPA_LIBRARIES "${SHERPA_ROOT}/lib/libsherpa-onnx-c-api.dylib")
        file(GLOB SHERPA_RUNTIME_LIBS "${SHERPA_ROOT}/lib/*.dylib")
    else()
        set(SHERPA_LIBRARIES "${SHERPA_ROOT}/lib/libsherpa-onnx-c-api.so")
        file(GLOB SHERPA_RUNTIME_LIBS "${SHERPA_ROOT}/lib/*.so*")
    endif()
endif()
if(SHERPA_FOUND AND TARGET aetherasr)
    message(STATUS "sherpa-onnx found — non-whisper ASR backend enabled")
    target_compile_definitions(aetherasr PRIVATE HAVE_SHERPA)
    target_include_directories(aetherasr PRIVATE ${SHERPA_INCLUDE_DIRS})
    target_link_libraries(aetherasr PRIVATE ${SHERPA_LIBRARIES})
    if(WIN32 AND SHERPA_DLLS)
        foreach(_sh_dll IN LISTS SHERPA_DLLS)
            add_custom_command(TARGET AetherSDR POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_sh_dll}" "$<TARGET_FILE_DIR:AetherSDR>"
                COMMENT "Copying sherpa-onnx DLL to build directory")
        endforeach()
    endif()
elseif(REQUIRE_ASR_SHERPA)
    message(FATAL_ERROR "REQUIRE_ASR_SHERPA is set but sherpa-onnx was not found. "
        "Run scripts/setup/setup-sherpa-onnx.sh to stage it under third_party/sherpa-onnx.")
else()
    message(STATUS "sherpa-onnx not found — non-whisper ASR backend disabled "
        "(run scripts/setup/setup-sherpa-onnx.sh to enable)")
endif()

if(RADE_FOUND)
    # PUBLIC defines: gui (RadeApplet etc.) checks HAVE_RADE/HAVE_OPUS.
    target_compile_definitions(aethercore PUBLIC HAVE_RADE HAVE_OPUS IS_BUILDING_RADE_API=1)
    if(RADE_WAV_TAP)
        set(RADE_TAP_DIR "${CMAKE_BINARY_DIR}/rade_taps"
            CACHE PATH "Output directory for RADE WAV tap files")
        # PUBLIC: RADEEngine.h guards two QByteArray members with
        # #ifdef RADE_WAV_TAP mid-class, so the flag changes sizeof(RADEEngine)
        # and every later member offset. A gui TU allocates it
        # (MainWindow_DigitalModes.cpp `new RADEEngine`); a PRIVATE define would
        # give the exe a smaller layout than the constructor compiled here →
        # ODR violation / heap corruption under -DRADE_WAV_TAP=ON. Matches the
        # sibling HAVE_RADE/HAVE_OPUS defines.
        target_compile_definitions(aethercore PUBLIC RADE_WAV_TAP
            RADE_TAP_DIR="${RADE_TAP_DIR}")
    endif()
    target_include_directories(aethercore PRIVATE ${RADE_DIR}/src)
    if(WIN32)
        target_link_libraries(aethercore PRIVATE opus)
    else()
        target_link_libraries(aethercore PRIVATE opus m)
    endif()
elseif(OPUS_FOUND)
    target_compile_definitions(aethercore PUBLIC HAVE_OPUS)
    target_include_directories(aethercore PRIVATE ${OPUS_INCLUDE_DIRS})
    target_link_libraries(aethercore PRIVATE ${OPUS_LIBRARIES})
endif()

if(ENABLE_SPECBLEACH)
    target_compile_definitions(aethercore PUBLIC HAVE_SPECBLEACH)
    target_include_directories(aethercore PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/libspecbleach/include
        ${CMAKE_SOURCE_DIR}/third_party/libspecbleach/src)
    if(MSVC AND SPECBLEACH_STATIC_LIB)
        # Link pre-built clang-cl static lib
        add_dependencies(aethercore specbleach_build)
        target_link_libraries(aethercore PRIVATE ${SPECBLEACH_STATIC_LIB})
        # fftw3f (float precision) for Windows
        set(FFTW3F_LIB "${CMAKE_SOURCE_DIR}/third_party/fftw3/lib/fftw3f.lib")
        if(EXISTS ${FFTW3F_LIB})
            target_link_libraries(aethercore PRIVATE ${FFTW3F_LIB})
            set(FFTW3F_DLL "${CMAKE_SOURCE_DIR}/third_party/fftw3/bin/libfftw3f-3.dll")
            add_custom_command(TARGET AetherSDR POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${FFTW3F_DLL}" "$<TARGET_FILE_DIR:AetherSDR>"
                COMMENT "Copying libfftw3f-3.dll to build directory")
        else()
            message(WARNING "fftw3f.lib not found — run scripts/setup/setup-fftw.ps1 and gen-fftw3f-lib.bat")
        endif()
    else()
        # libspecbleach C sources include <fftw3.h> directly — ensure it's
        # findable. PUBLIC for the same transitive reason as the main FFTW3
        # block above (AudioEngine.h -> SpectralNR.h -> <fftw3.h> reaches gui).
        if(FFTW3_INCLUDE_DIRS)
            target_include_directories(aethercore PUBLIC ${FFTW3_INCLUDE_DIRS})
        else()
            find_path(FFTW3_H_DIR fftw3.h HINTS ${CMAKE_SOURCE_DIR}/third_party/fftw3/include)
            if(FFTW3_H_DIR)
                target_include_directories(aethercore PUBLIC ${FFTW3_H_DIR})
            endif()
        endif()
        # libspecbleach uses fftwf (float precision FFTW3)
        find_library(FFTW3F_LIB fftw3f HINTS /opt/homebrew/lib /usr/local/lib ${CMAKE_SOURCE_DIR}/third_party/fftw3/lib)
        if(FFTW3F_LIB)
            target_link_libraries(aethercore PRIVATE ${FFTW3F_LIB})
        else()
            message(WARNING "fftw3f not found — NR4 will fail to link")
        endif()
        # Suppress warnings from third-party C code
        set_source_files_properties(${SPECBLEACH_SOURCES} PROPERTIES COMPILE_FLAGS "-w")
    endif()
endif()

if(ENABLE_DFNR)
    target_compile_definitions(aethercore PUBLIC HAVE_DFNR)
    target_include_directories(aethercore PRIVATE ${DEEPFILTER_DIR}/include)
    target_link_libraries(aethercore PRIVATE ${DFNR_LIB})
    if(WIN32)
        target_link_libraries(aethercore PRIVATE ws2_32 bcrypt userenv ntdll)
    elseif(APPLE)
        # Rust runtime deps on macOS
        target_link_libraries(aethercore PRIVATE "-framework Security" "-framework CoreFoundation")
    else()
        # Rust runtime deps on Linux
        target_link_libraries(aethercore PRIVATE pthread dl m)
    endif()
    # Copy DLL and model to build directory
    if(DFNR_DLL AND EXISTS ${DFNR_DLL})
        add_custom_command(TARGET AetherSDR POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DFNR_DLL}" "$<TARGET_FILE_DIR:AetherSDR>"
            COMMENT "Copying deepfilter.dll to build directory")
    endif()
    if(EXISTS "${DFNR_MODEL}" AND NOT AETHER_EMBED_DFNR_MODEL)
        if(APPLE)
            # macOS: place in Resources/ so notarization doesn't reject a
            # non-Mach-O file inside Contents/MacOS/
            add_custom_command(TARGET AetherSDR POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_BUNDLE_DIR:AetherSDR>/Contents/Resources"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DFNR_MODEL}" "$<TARGET_BUNDLE_DIR:AetherSDR>/Contents/Resources/"
                COMMENT "Copying DeepFilterNet3 model to app bundle Resources")
        else()
            add_custom_command(TARGET AetherSDR POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DFNR_MODEL}" "$<TARGET_FILE_DIR:AetherSDR>"
                COMMENT "Copying DeepFilterNet3 model to build directory")
        endif()
    endif()
    # Install the model alongside the binary for cmake --install
    if(NOT APPLE AND NOT AETHER_EMBED_DFNR_MODEL)
        install(FILES "${DFNR_MODEL}"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/AetherSDR"
            OPTIONAL)
    endif()
endif()

# Optional NVIDIA Maxine AFX GPU denoiser. The AFX runtime (libnv_audiofx +
# the ~2 GB CUDA/TensorRT libs) is NOT linked here — it is dlopen'd at runtime
# from a downloaded/cached pack, so the shipped binary carries none of it.
# We only compile the wrapper (it uses its own minimal NvAFX_* declarations,
# so no NVIDIA headers are vendored) and link the dynamic loader.
if(ENABLE_NVIDIA_AFX)
    # x86_64 ONLY: NVIDIA Maxine AFX ships no ARM runtime, so aarch64 (Raspberry
    # Pi etc. — which also have no NVIDIA GPU) and macOS get the DFNR fallback,
    # not BNR. The arch guard means BNR is simply absent there (no dead button).
    if(((UNIX AND NOT APPLE) OR WIN32) AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        # PUBLIC: gui surfaces the BNR button under #ifdef HAVE_NVIDIA_AFX.
        target_compile_definitions(aethercore PUBLIC HAVE_NVIDIA_AFX)
        # Linux loads the AFX runtime via libdl; Windows uses the Win32
        # LoadLibrary family from kernel32 (linked by default).
        if(UNIX)
            target_link_libraries(aethercore PRIVATE ${CMAKE_DL_LIBS})
        endif()
        message(STATUS "NVIDIA AFX GPU denoiser enabled (runtime-loaded)")
    else()
        message(STATUS "NVIDIA AFX GPU denoiser not built (x86_64 Linux/Windows only; macOS/aarch64 use DFNR)")
    endif()
endif()

if(ENABLE_MQTT)
    # HAVE_MQTT gates the project's MQTT client code (MqttApplet,
    # RadioModel plumbing) — must be set regardless of whether
    # libmosquitto is the bundled or the system copy.
    target_compile_definitions(aethercore PUBLIC HAVE_MQTT) # gui MqttApplet checks it
    if (USE_SYSTEM_LIBMOSQUITTO)
        # System libmosquitto is conventionally built with TLS support on
        # distro packages — assume HAVE_MQTT_TLS unless the packager
        # explicitly opted out via -DMQTT_TLS=OFF.
        if(MQTT_TLS)
            target_compile_definitions(aethercore PUBLIC HAVE_MQTT_TLS)
        endif()
    else()
        target_include_directories(aethercore SYSTEM PRIVATE
            ${MOSQUITTO_DIR}/include
            ${MOSQUITTO_DIR}/src)
        target_sources(aethercore PRIVATE ${MOSQUITTO_SOURCES})
        if(OpenSSL_FOUND)
            set(MQTT_TLS_FLAG " -DWITH_TLS")
            target_compile_definitions(aethercore PUBLIC HAVE_MQTT_TLS)
            target_link_libraries(aethercore PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        else()
            set(MQTT_TLS_FLAG "")
        endif()
        if(WIN32)
            set_source_files_properties(${MOSQUITTO_SOURCES} PROPERTIES
                COMPILE_FLAGS "-w -DLIBMOSQUITTO_STATIC -DLIBMOSQCOMMON_STATIC${MQTT_TLS_FLAG}")
            target_compile_definitions(aethercore PRIVATE LIBMOSQUITTO_STATIC LIBMOSQCOMMON_STATIC)
        else()
            set_source_files_properties(${MOSQUITTO_SOURCES} PROPERTIES
                COMPILE_FLAGS "-w -DWITH_THREADING${MQTT_TLS_FLAG}")
        endif()
    endif()
    if(NOT WIN32)
        target_link_libraries(aethercore PRIVATE pthread)
    endif()
endif()

if(HAVE_PIPEWIRE)
    target_compile_definitions(aethercore PUBLIC HAVE_PIPEWIRE) # gui checks HAVE_PIPEWIRE too
    message(STATUS "Linux DAX bridge enabled (PulseAudio pipe modules)")
endif()

if(HAVE_PIPEWIRE_NATIVE)
    target_compile_definitions(aethercore PUBLIC HAVE_PIPEWIRE_NATIVE)
    target_include_directories(aethercore PRIVATE ${PIPEWIRE_NATIVE_INCLUDE_DIRS})
    target_link_libraries(aethercore PRIVATE ${PIPEWIRE_NATIVE_LIBRARIES})
    target_compile_options(aethercore PRIVATE ${PIPEWIRE_NATIVE_CFLAGS_OTHER})
    message(STATUS "Linux DAX RX uses native pw_stream (libpipewire-0.3 ${PIPEWIRE_NATIVE_VERSION})")
endif()

# Pass project version to code — PUBLIC so main.cpp and gui (About dialog,
# Ax25HfPacketDecodeDialog) see it as before.
target_compile_definitions(aethercore PUBLIC
    AETHERSDR_VERSION="${PROJECT_VERSION}"
)

# Compiler warnings — same flags for both first-party targets. Vendored
# sources compiled into aethercore keep their per-source -w COMPILE_FLAGS,
# which land after these target options and win (unchanged from monolith).
foreach(_aether_tgt aethercore AetherSDR)
    if(MSVC)
        target_compile_options(${_aether_tgt} PRIVATE /W3 /Zc:__cplusplus /permissive- /utf-8 /bigobj)
    else()
        target_compile_options(${_aether_tgt} PRIVATE
            -Wall -Wextra -Wpedantic
            $<$<CONFIG:Debug>:-g3 -fsanitize=address>
            $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
        )
        target_link_options(${_aether_tgt} PRIVATE
            $<$<CONFIG:Debug>:-fsanitize=address>
        )
    endif()
endforeach()


# ── Unit test harnesses ──────────────────────────────────────────────────────
# MOVED: every test target is now declared in tests/tests.cmake, which cut this
# file roughly in half. Do NOT declare a test target here — tests/tests.cmake
# fails the configure step if you do, and tools/check_test_registration.py fails
# the PR in CI.
#
# It is include()d rather than add_subdirectory()'d so that relative paths inside
# it keep resolving against the repository root. The header of that file explains
# why that matters and why it should not be "tidied up" into a subdirectory.
enable_testing()
include(tests/tests.cmake)


# ── Install rules ────────────────────────────────────────────────────────────
include(GNUInstallDirs)

if(APPLE)
    install(TARGETS AetherSDR
        BUNDLE DESTINATION .
    )
    # BUNDLE installation copies the existing app directory. Remove the
    # superseded helper if an incremental build tree still contains it.
    install(CODE [[
        file(REMOVE
            "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/AetherSDR.app/Contents/MacOS/aether-dstar-waveform"
            "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/AetherSDR.app/Contents/MacOS/aether-dstar-waveform.exe"
            "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/AetherSDR.app/Contents/MacOS/ThumbDV.cfg"
            "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/AetherSDR.app/Contents/MacOS/AetherDV.cfg")
    ]])
    if(TARGET aether-dv-waveform)
        install(PROGRAMS "$<TARGET_FILE:aether-dv-waveform>"
            DESTINATION AetherSDR.app/Contents/MacOS
        )
        # The manifest is not a Mach-O; keep it out of MacOS/ (codesign) — see
        # the POST_BUILD note above.
        install(FILES third_party/smartsdr-dsp/config/AetherDV.cfg
            DESTINATION AetherSDR.app/Contents/Resources
        )
    endif()
else()
    install(TARGETS AetherSDR
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    if(TARGET aether-dv-waveform)
        install(TARGETS aether-dv-waveform
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
        install(FILES third_party/smartsdr-dsp/config/AetherDV.cfg
            DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
        if(LINUX)
            install(FILES packaging/linux/70-aethersdr-thumbdv.rules
                DESTINATION lib/udev/rules.d
            )
        endif()
    endif()
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/packaging/linux/AetherSDR.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
    )
    install(FILES docs/assets/logo-circle-256.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps
        RENAME aethersdr.png
    )
    install(FILES packaging/linux/io.github.aethersdr.aethersdr.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo
    )
    # Install the man page uncompressed; distro packaging compresses man pages
    # per its own policy (Debian/Arch re-gzip, so shipping a .gz would clash).
    install(FILES packaging/linux/aethersdr.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
    )
endif()

