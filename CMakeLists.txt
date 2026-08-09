cmake_minimum_required(VERSION 3.25)

# CMake 3.29+ can safely de-duplicate libraries for linkers that support it.
# This avoids duplicate-library warnings from Apple's linker while preserving
# compatibility with the project's CMake 3.25 minimum.
if(POLICY CMP0156)
    cmake_policy(SET CMP0156 NEW)
endif()

project(AetherSDR VERSION 26.8.2 LANGUAGES C CXX)

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
option(USE_SYSTEM_LIQUID_DSP   "Use system liquid-dsp"   OFF)
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
option(ENABLE_LIQUID_DSP "Build with liquid-dsp toolkit support" OFF)
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
    src/core/backends/sim/SpectrumPatternGenerator.cpp  # demo mode — signal engine (RFC #4288 Phase 2)
    src/core/backends/sim/NoiseMixer.cpp                 # demo mode — audio engine (RFC #4288 Phase 2b)
    src/core/backends/hl2/MetisProtocol.cpp  # aetherd HL2 Phase 1a (HPSDR Protocol 1)
    src/core/backends/hl2/MetisClient.cpp    # aetherd HL2 Phase 1a (UDP wire + RX ingest)
    src/core/backends/hl2/Hl2EmergencyStop.cpp # release the radio from a signal handler
    src/core/backends/hl2/Hl2Receivers.cpp   # per-receiver index-space map (HERMES.md §12.5)
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
    src/core/TxMicChannelNormalizer.cpp
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

# Bundled liquid-dsp (MIT) — comprehensive DSP toolkit covering modems
# (PSK/QAM/FSK/GMSK/OFDM), FEC (Hamming/Golay/RS/convolutional), adaptive
# filters (LMS/RLS), AGC, NCO, polyphase resampling, and equalizers.
# Vendored proactively under #3043 as foundation infrastructure for future
# digital-mode work (e.g. native FT8/FT4 — #85 Phase 4 — or PSK31 / RTTY).
#
# Linux + macOS only — liquid-dsp upstream targets GCC/Clang/MinGW; its
# include/liquid.h:473 uses C99 `float _Complex` typedefs which the MSVC C
# compiler doesn't accept (syntax error: identifier 'liquid_float_complex').
# Upstream has no first-class MSVC support and patching the header for
# `_Fcomplex`/`_Dcomplex` would be invasive against future upstream syncs.
# Tracked as a follow-up; today the Windows build skips this vendor and
# any future consumer that needs liquid-dsp on Windows needs to land MSVC
# support first.
#
# Built via the upstream CMakeLists.txt with examples / tests / benchmarks /
# sandbox / docs disabled to keep the build slim. FFTW lookup is disabled
# (FIND_FFTW=OFF) so liquid-dsp uses its own internal FFT — wiring it to our
# bundled third_party/fftw3 can be a follow-up if a future consumer wants
# FFTW-accelerated transforms.
#
# Disabled by default on every platform because no AetherSDR module currently
# consumes liquid-dsp symbols. Opt-in non-MSVC builds link liquid-static; the
# static linker then extracts only symbols used by a future consumer.
#
# USE_SYSTEM_LIQUID_DSP only takes effect once liquid-dsp itself is enabled, so
# warn if a packager set it without ENABLE_LIQUID_DSP rather than silently
# ignoring it (#4146 review).
if(USE_SYSTEM_LIQUID_DSP AND NOT ENABLE_LIQUID_DSP)
    message(WARNING
        "USE_SYSTEM_LIQUID_DSP=ON has no effect without ENABLE_LIQUID_DSP=ON — "
        "liquid-dsp is off by default. Pass -DENABLE_LIQUID_DSP=ON to build "
        "against it.")
endif()

if(ENABLE_LIQUID_DSP AND MSVC)
    message(FATAL_ERROR
        "ENABLE_LIQUID_DSP is not supported with MSVC because liquid-dsp's "
        "public headers use C99 _Complex types")
endif()

if(ENABLE_LIQUID_DSP AND NOT USE_SYSTEM_LIQUID_DSP)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_AUTOTESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(BUILD_SANDBOX OFF CACHE BOOL "" FORCE)
    set(BUILD_DOC OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
    set(FIND_FFTW OFF CACHE BOOL "" FORCE)
    set(ENABLE_LOGGING OFF CACHE BOOL "" FORCE)
# liquid-dsp's cmake_minimum_required(VERSION 3.10) puts its option() calls
# under CMP0077 OLD behavior — option() with that policy does NOT honor a
# same-name *normal* variable. We must use CACHE BOOL FORCE so the override
# takes effect. Without this, BUILD_SHARED_LIBS stays at the option's ON
# default. Linux silently ships both libliquid.so and libliquid.a (different
# extensions); the OUTPUT_NAME `liquid` at upstream CMakeLists.txt:444
# renames the STATIC target's archive to drop the -static suffix.
#
# Cache pollution is undone with unset(CACHE) after add_subdirectory so
# this doesn't leak into the rest of the project's BUILD_SHARED_LIBS
# semantics.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/liquid-dsp EXCLUDE_FROM_ALL)
    unset(BUILD_SHARED_LIBS CACHE)
# Suppress warnings from vendored code (same pattern as other third_party
# integrations — we don't want -Werror surfaces on code we don't own).
    if(TARGET liquid-static)
        target_compile_options(liquid-static PRIVATE -w)
    endif()
endif()

if(ENABLE_LIQUID_DSP AND USE_SYSTEM_LIQUID_DSP)
    find_package(PkgConfig REQUIRED)
    if(PkgConfig_FOUND)
        pkg_check_modules(liquid-dsp REQUIRED IMPORTED_TARGET liquid-dsp)
    endif()
endif()

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
    add_test(NAME aether_dv_waveform_no_args
        COMMAND ${CMAKE_COMMAND}
            -DHELPER=$<TARGET_FILE:aether-dv-waveform>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_digital_voice_waveform_no_args.cmake
    )

    function(aether_enable_digital_voice_test_sanitizers target)
        if(NOT WIN32 AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${target} PRIVATE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endfunction()

    add_executable(crdv_cleanroom_test
        third_party/crdv/tests/test_main.c)
    target_link_libraries(crdv_cleanroom_test PRIVATE crdv::crdv)
    aether_enable_digital_voice_test_sanitizers(crdv_cleanroom_test)
    add_test(NAME crdv_cleanroom_test COMMAND crdv_cleanroom_test)
    add_test(NAME crdv_manifest_test
        COMMAND ${CMAKE_COMMAND}
            -DCRDV_DIR=${CRDV_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_crdv_manifest.cmake)

    add_executable(crdv_quarantined_test
        third_party/crdv/tests/quarantined_acceptance.c)
    target_link_libraries(crdv_quarantined_test PRIVATE crdv::crdv)
    add_test(NAME crdv_quarantined_test COMMAND crdv_quarantined_test)
    set_tests_properties(crdv_quarantined_test PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(digital_voice_protocol_test
        tests/digital_voice_protocol_test.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/aether_ipv4_source_filter.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/aether_smartsdr_command.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/aether_tcp_frame_buffer.c)
    target_include_directories(digital_voice_protocol_test BEFORE PRIVATE
        $<$<BOOL:${WIN32}>:${DIGITAL_VOICE_WAVEFORM_DIR}/compat/windows>
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface)
    if(WIN32)
        target_link_libraries(digital_voice_protocol_test PRIVATE ws2_32)
    endif()
    aether_enable_digital_voice_test_sanitizers(digital_voice_protocol_test)
    add_test(NAME digital_voice_protocol_test COMMAND digital_voice_protocol_test)

    add_executable(digital_voice_buffer_queue_test
        tests/digital_voice_buffer_queue_test.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/aether_buffer_queue.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/hal_buffer.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/utils.c)
    target_compile_definitions(digital_voice_buffer_queue_test PRIVATE _DEFAULT_SOURCE)
    target_include_directories(digital_voice_buffer_queue_test BEFORE PRIVATE
        $<$<BOOL:${WIN32}>:${DIGITAL_VOICE_WAVEFORM_DIR}/compat/windows>
        ${DIGITAL_VOICE_WAVEFORM_DIR}/compat
        ${DIGITAL_VOICE_WAVEFORM_DIR}/include
        ${DIGITAL_VOICE_WAVEFORM_DIR}
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface)
    target_link_libraries(digital_voice_buffer_queue_test PRIVATE Threads::Threads)
    if(NOT WIN32)
        target_link_libraries(digital_voice_buffer_queue_test PRIVATE m)
    endif()
    aether_enable_digital_voice_test_sanitizers(digital_voice_buffer_queue_test)
    add_test(NAME digital_voice_buffer_queue_test COMMAND digital_voice_buffer_queue_test)

    add_executable(digital_voice_slice_ownership_test
        tests/digital_voice_slice_ownership_test.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/digital_voice_slice_ownership.c)
    target_include_directories(digital_voice_slice_ownership_test PRIVATE
        ${DIGITAL_VOICE_WAVEFORM_DIR}/include
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface)
    aether_enable_digital_voice_test_sanitizers(digital_voice_slice_ownership_test)
    add_test(NAME digital_voice_slice_ownership_test
        COMMAND digital_voice_slice_ownership_test)

    add_executable(dstar_modem_test
        tests/dstar_modem_test.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/aether_dstar_protocol.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/aether_smartsdr_command.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/utils.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/ThumbDV/bit_pattern_matcher.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/ThumbDV/gmsk_modem.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/circular_buffer.c
    )
    target_compile_definitions(dstar_modem_test PRIVATE _DEFAULT_SOURCE)
    target_include_directories(dstar_modem_test BEFORE PRIVATE
        $<$<BOOL:${WIN32}>:${DIGITAL_VOICE_WAVEFORM_DIR}/compat/windows>
        ${DIGITAL_VOICE_WAVEFORM_DIR}/compat
        ${DIGITAL_VOICE_WAVEFORM_DIR}/include
        ${DIGITAL_VOICE_WAVEFORM_DIR}
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface
        ${DIGITAL_VOICE_WAVEFORM_DIR}/ThumbDV
        ${CMAKE_SOURCE_DIR}/third_party/crdv/include)
    target_link_libraries(dstar_modem_test PRIVATE Threads::Threads crdv::crdv)
    if(NOT WIN32)
        target_link_libraries(dstar_modem_test PRIVATE m)
    endif()
    aether_enable_digital_voice_test_sanitizers(dstar_modem_test)
    add_test(NAME dstar_modem_test COMMAND dstar_modem_test)

    add_executable(dstar_transmit_state_test
        tests/dstar_transmit_state_test.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/dstar_transmit_state.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/dstar_tx_output.c)
    target_include_directories(dstar_transmit_state_test BEFORE PRIVATE
        $<$<BOOL:${WIN32}>:${DIGITAL_VOICE_WAVEFORM_DIR}/compat/windows>
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface)
    target_link_libraries(dstar_transmit_state_test PRIVATE Threads::Threads)
    if(NOT WIN32)
        target_link_libraries(dstar_transmit_state_test PRIVATE m)
    endif()
    aether_enable_digital_voice_test_sanitizers(dstar_transmit_state_test)
    add_test(NAME dstar_transmit_state_test COMMAND dstar_transmit_state_test)

    add_executable(digital_voice_tx_gate_test
        tests/digital_voice_tx_gate_test.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/digital_voice_tx_gate.c)
    target_include_directories(digital_voice_tx_gate_test PRIVATE
        ${DIGITAL_VOICE_WAVEFORM_DIR}/include
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface)
    aether_enable_digital_voice_test_sanitizers(digital_voice_tx_gate_test)
    add_test(NAME digital_voice_tx_gate_test COMMAND digital_voice_tx_gate_test)

    add_executable(vita_packet_admission_test
        tests/vita_packet_admission_test.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/aether_vita_packet_validator.c)
    target_include_directories(vita_packet_admission_test PRIVATE
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface
        ${CMAKE_SOURCE_DIR}/third_party/crdv/include)
    target_link_libraries(vita_packet_admission_test PRIVATE crdv::crdv)
    aether_enable_digital_voice_test_sanitizers(vita_packet_admission_test)
    add_test(NAME vita_packet_admission_test COMMAND vita_packet_admission_test)

    add_executable(dstar_tx_path_test
        tests/dstar_tx_path_test.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/aether_smartsdr_command.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/aether_dstar_protocol.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/digital_voice_tx_gate.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/dstar_transmit_state.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/dstar_tx_stream.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/utils.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/vita_output.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/ThumbDV/bit_pattern_matcher.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/ThumbDV/gmsk_modem.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/circular_buffer.c)
    target_compile_definitions(dstar_tx_path_test PRIVATE _DEFAULT_SOURCE)
    target_include_directories(dstar_tx_path_test BEFORE PRIVATE
        $<$<BOOL:${WIN32}>:${DIGITAL_VOICE_WAVEFORM_DIR}/compat/windows>
        ${DIGITAL_VOICE_WAVEFORM_DIR}/compat
        ${DIGITAL_VOICE_WAVEFORM_DIR}/include
        ${DIGITAL_VOICE_WAVEFORM_DIR}
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface
        ${DIGITAL_VOICE_WAVEFORM_DIR}/ThumbDV
        ${CMAKE_SOURCE_DIR}/third_party/crdv/include)
    target_link_libraries(dstar_tx_path_test PRIVATE Threads::Threads crdv::crdv)
    if(NOT WIN32)
        target_link_libraries(dstar_tx_path_test PRIVATE m)
    endif()
    if(WIN32)
        target_link_libraries(dstar_tx_path_test PRIVATE ws2_32)
    endif()
    aether_enable_digital_voice_test_sanitizers(dstar_tx_path_test)
    add_test(NAME dstar_tx_path_test COMMAND dstar_tx_path_test)

    add_executable(vita_packet_count_test
        tests/vita_packet_count_test.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/vita_packet_sequence.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/vita_output.c)
    target_compile_definitions(vita_packet_count_test PRIVATE _DEFAULT_SOURCE)
    target_include_directories(vita_packet_count_test BEFORE PRIVATE
        $<$<BOOL:${WIN32}>:${DIGITAL_VOICE_WAVEFORM_DIR}/compat/windows>
        ${DIGITAL_VOICE_WAVEFORM_DIR}/compat
        ${DIGITAL_VOICE_WAVEFORM_DIR}/include
        ${DIGITAL_VOICE_WAVEFORM_DIR}
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface)
    aether_enable_digital_voice_test_sanitizers(vita_packet_count_test)
    if(WIN32)
        target_link_libraries(vita_packet_count_test PRIVATE ws2_32)
    endif()
    add_test(NAME vita_packet_count_test COMMAND vita_packet_count_test)

    add_executable(dstar_waveform_metrics_test
        tests/dstar_waveform_metrics_test.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/dstar_waveform_metrics.c)
    target_include_directories(dstar_waveform_metrics_test PRIVATE
        ${DIGITAL_VOICE_WAVEFORM_DIR}/include
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface)
    if(NOT WIN32)
        target_link_libraries(dstar_waveform_metrics_test PRIVATE m)
    endif()
    aether_enable_digital_voice_test_sanitizers(dstar_waveform_metrics_test)
    add_test(NAME dstar_waveform_metrics_test COMMAND dstar_waveform_metrics_test)

    add_executable(digital_voice_mode_registry_test
        tests/digital_voice_mode_registry_test.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/aether_smartsdr_command.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/digital_voice_mode_registry.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/utils.c)
    target_compile_definitions(digital_voice_mode_registry_test PRIVATE _DEFAULT_SOURCE)
    target_include_directories(digital_voice_mode_registry_test BEFORE PRIVATE
        $<$<BOOL:${WIN32}>:${DIGITAL_VOICE_WAVEFORM_DIR}/compat/windows>
        ${DIGITAL_VOICE_WAVEFORM_DIR}/compat
        ${DIGITAL_VOICE_WAVEFORM_DIR}/include
        ${DIGITAL_VOICE_WAVEFORM_DIR}
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface
        ${CMAKE_SOURCE_DIR}/third_party/crdv/include)
    target_link_libraries(digital_voice_mode_registry_test PRIVATE
        Threads::Threads crdv::crdv)
    if(WIN32)
        target_link_libraries(digital_voice_mode_registry_test PRIVATE ws2_32)
    endif()
    aether_enable_digital_voice_test_sanitizers(digital_voice_mode_registry_test)
    add_test(NAME digital_voice_mode_registry_test COMMAND digital_voice_mode_registry_test)

    add_executable(thumbdv_queue_test
        tests/thumbdv_queue_test.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/hal_buffer.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface/utils.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/ThumbDV/thumbDV.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/aether_sem_compat.c
        ${DIGITAL_VOICE_WAVEFORM_DIR}/aether_serial_compat.c)
    target_compile_definitions(thumbdv_queue_test PRIVATE
        _DEFAULT_SOURCE
        AETHER_DSTAR_TESTING)
    target_include_directories(thumbdv_queue_test BEFORE PRIVATE
        $<$<BOOL:${WIN32}>:${DIGITAL_VOICE_WAVEFORM_DIR}/compat/windows>
        ${DIGITAL_VOICE_WAVEFORM_DIR}/compat
        ${DIGITAL_VOICE_WAVEFORM_DIR}/include
        ${DIGITAL_VOICE_WAVEFORM_DIR}
        ${DIGITAL_VOICE_WAVEFORM_DIR}/SmartSDR_Interface
        ${DIGITAL_VOICE_WAVEFORM_DIR}/ThumbDV)
    target_link_libraries(thumbdv_queue_test PRIVATE Threads::Threads)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(thumbdv_queue_test PRIVATE util)
    endif()
    if(APPLE)
        target_link_libraries(thumbdv_queue_test PRIVATE "-framework IOKit")
    endif()
    if(WIN32)
        target_link_libraries(thumbdv_queue_test PRIVATE ws2_32)
    endif()
    aether_enable_digital_voice_test_sanitizers(thumbdv_queue_test)
    add_test(NAME thumbdv_queue_test COMMAND thumbdv_queue_test)

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

if(ENABLE_LIQUID_DSP)
    if(USE_SYSTEM_LIQUID_DSP)
        target_link_libraries(aethercore PRIVATE PkgConfig::liquid-dsp)
    else()
        target_link_libraries(aethercore PRIVATE liquid-static)
    endif()
endif()

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
# Standalone DSP smoke tests. Built alongside the main target so they share
# the same toolchain and warning flags. Run manually with ./build/<target>.

add_executable(wdsp_channel_test tests/wdsp_channel_test.cpp)
target_link_libraries(wdsp_channel_test PRIVATE aethercore)
add_test(NAME wdsp_channel_test COMMAND wdsp_channel_test)

# HL2 Metis protocol — pure wire encode/decode, standalone (no Qt / aethercore).
add_executable(hl2_metis_protocol_test
    tests/hl2_metis_protocol_test.cpp
    src/core/backends/hl2/MetisProtocol.cpp)
target_include_directories(hl2_metis_protocol_test PRIVATE src)
add_test(NAME hl2_metis_protocol_test COMMAND hl2_metis_protocol_test)

# IcomCIV wire layers — pure encode/decode, standalone (no Qt / aethercore).
# An Icom networked radio is two protocols stacked: CI-V is the command plane
# and RS-BA1 is the UDP transport it travels inside. Both halves unit-test
# without a socket, and CivCodec is deliberately transport-free so a future
# USB / local-serial mode reuses it unchanged.
add_executable(icom_protocol_test
    tests/icom_protocol_test.cpp
    src/core/backends/icom/IcomProtocol.cpp)
target_include_directories(icom_protocol_test PRIVATE src)
add_test(NAME icom_protocol_test COMMAND icom_protocol_test)

add_executable(icom_civ_test
    tests/icom_civ_test.cpp
    src/core/backends/icom/CivCodec.cpp)
target_include_directories(icom_civ_test PRIVATE src)
add_test(NAME icom_civ_test COMMAND icom_civ_test)

add_executable(icom_scope_test
    tests/icom_scope_test.cpp
    src/core/backends/icom/IcomScope.cpp
    src/core/backends/icom/CivCodec.cpp)
target_include_directories(icom_scope_test PRIVATE src)
add_test(NAME icom_scope_test COMMAND icom_scope_test)

add_executable(icom_audio_test
    tests/icom_audio_test.cpp
    src/core/backends/icom/IcomAudio.cpp
    src/core/backends/icom/IcomProtocol.cpp)
target_include_directories(icom_audio_test PRIVATE src)
add_test(NAME icom_audio_test COMMAND icom_audio_test)

# IcomCIV phases 4-5 — meter calibration curves, the poll scheduler (driven by
# a synthetic clock), and the per-model capability table.
add_executable(icom_meters_test
    tests/icom_meters_test.cpp
    src/core/backends/icom/IcomMeters.cpp
    src/core/backends/icom/IcomModels.cpp
    src/core/backends/icom/CivCodec.cpp)
target_include_directories(icom_meters_test PRIVATE src)
add_test(NAME icom_meters_test COMMAND icom_meters_test)

# IcomCIV phases 0-3 end to end — the session against a fake IC-705 on
# localhost. This is what proves the ORDER of the RS-BA1 handshake, which is the
# part of the protocol Icom documents nowhere.
add_executable(icom_session_test
    tests/icom_session_test.cpp
    src/core/backends/icom/IcomSession.cpp
    src/core/backends/icom/IcomStream.cpp
    src/core/backends/icom/IcomProtocol.cpp
    src/core/backends/icom/CivCodec.cpp
    src/core/backends/icom/IcomScope.cpp
    src/core/backends/icom/IcomAudio.cpp)
target_include_directories(icom_session_test PRIVATE src tests)
target_link_libraries(icom_session_test PRIVATE Qt6::Core Qt6::Network)
add_test(NAME icom_session_test COMMAND icom_session_test)

# IcomCIV backend seam test — the IRadioBackend implementor against the fake
# IC-705, with the TCI/WSJT-X audio contract as the load-bearing assertion.
add_executable(icom_backend_test tests/icom_backend_test.cpp)
target_include_directories(icom_backend_test PRIVATE src tests)
target_link_libraries(icom_backend_test PRIVATE aethercore Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME icom_backend_test COMMAND icom_backend_test)

# IcomCIV live probe — REQUIRES A REAL RADIO, so deliberately NOT registered
# with add_test(). Receive-only: it never sends a PTT command.
#   cmake --build build --target icom_live_probe
#   ./build/icom_live_probe ic-705.local <user> <password>
add_executable(icom_live_probe EXCLUDE_FROM_ALL
    tests/icom_live_probe.cpp
    src/core/backends/icom/IcomSession.cpp
    src/core/backends/icom/IcomStream.cpp
    src/core/backends/icom/IcomProtocol.cpp
    src/core/backends/icom/CivCodec.cpp
    src/core/backends/icom/IcomScope.cpp
    src/core/backends/icom/IcomAudio.cpp)
target_include_directories(icom_live_probe PRIVATE src)
target_link_libraries(icom_live_probe PRIVATE Qt6::Core Qt6::Network)

# HL2 live band-filter probe — REQUIRES REAL HARDWARE, so deliberately NOT
# registered with add_test(). Build it and run it by hand against a radio:
#   cmake --build build --target hl2_live_band_filter_probe
#   ./build/hl2_live_band_filter_probe 192.168.1.21
add_executable(hl2_live_band_filter_probe EXCLUDE_FROM_ALL
    tests/hl2_live_band_filter_probe.cpp)
target_include_directories(hl2_live_band_filter_probe PRIVATE src)
target_link_libraries(hl2_live_band_filter_probe PRIVATE aethercore Qt6::Core Qt6::Network)

# A killed AetherSDR must still release the radio. A child process runs a real
# MetisClient against a fake radio; the Python driver kills it and requires a
# metis-stop datagram to arrive. End-to-end on purpose — see the .py header.
add_executable(hl2_signal_stop_child tests/hl2_signal_stop_child.cpp)
target_include_directories(hl2_signal_stop_child PRIVATE src)
target_link_libraries(hl2_signal_stop_child PRIVATE aethercore Qt6::Core Qt6::Network)
find_package(Python3 COMPONENTS Interpreter)
if(NOT WIN32 AND Python3_Interpreter_FOUND)
    add_test(NAME hl2_signal_stop_test
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/hl2_signal_stop_test.py
                     $<TARGET_FILE:hl2_signal_stop_child>)
endif()

# HL2 MetisClient loopback test — drives the UDP wire against a fake HL2.
add_executable(hl2_metis_client_test tests/hl2_metis_client_test.cpp)
target_include_directories(hl2_metis_client_test PRIVATE src)
target_link_libraries(hl2_metis_client_test PRIVATE aethercore Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME hl2_metis_client_test COMMAND hl2_metis_client_test)

# HL2 receiver-count restart — a fake radio that "loses" a metis-start, proving
# the restart retries it and that the recovery is not mistaken for a reconnect.
add_executable(hl2_receiver_count_restart_test tests/hl2_receiver_count_restart_test.cpp)
target_include_directories(hl2_receiver_count_restart_test PRIVATE src)
target_link_libraries(hl2_receiver_count_restart_test
    PRIVATE aethercore Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME hl2_receiver_count_restart_test COMMAND hl2_receiver_count_restart_test)

# HL2 per-receiver index-space map — standalone, needs only QtCore for QString.
add_executable(hl2_receivers_test
    tests/hl2_receivers_test.cpp
    src/core/backends/hl2/Hl2Receivers.cpp)
target_include_directories(hl2_receivers_test PRIVATE src)
target_link_libraries(hl2_receivers_test PRIVATE Qt6::Core)
add_test(NAME hl2_receivers_test COMMAND hl2_receivers_test)

# HL2 spectrum (FFT panadapter) — standalone, links FFTW3 directly.
add_executable(hl2_spectrum_test
    tests/hl2_spectrum_test.cpp
    src/core/backends/hl2/Hl2Spectrum.cpp)
target_include_directories(hl2_spectrum_test PRIVATE src ${FFTW3_INCLUDE_DIRS})
target_link_libraries(hl2_spectrum_test PRIVATE ${FFTW3_LIBRARIES})
add_test(NAME hl2_spectrum_test COMMAND hl2_spectrum_test)

# HL2 RX DSP — IQ -> WdspChannel demod + Hl2Spectrum. Links aethercore (WDSP+FFTW).
add_executable(hl2_rxdsp_test tests/hl2_rxdsp_test.cpp)
target_include_directories(hl2_rxdsp_test PRIVATE src)
target_link_libraries(hl2_rxdsp_test PRIVATE aethercore Qt6::Core Qt6::Test)
add_test(NAME hl2_rxdsp_test COMMAND hl2_rxdsp_test)

# AM/SAM come back from WDSP's envelope detector with the carrier as a DC
# pedestal; the blocker on the audio output must strip it without touching the
# modes that were already zero-mean, and without its corner creeping up into
# the audio band.
add_executable(hl2_am_dcblock_test tests/hl2_am_dcblock_test.cpp)
target_include_directories(hl2_am_dcblock_test PRIVATE src)
target_link_libraries(hl2_am_dcblock_test PRIVATE aethercore Qt6::Core Qt6::Test)
add_test(NAME hl2_am_dcblock_test COMMAND hl2_am_dcblock_test)

# The RX DSP must demodulate at every IQ rate the operator can select by zooming.
add_executable(hl2_rxdsp_rate_test tests/hl2_rxdsp_rate_test.cpp)
target_include_directories(hl2_rxdsp_rate_test PRIVATE src)
target_link_libraries(hl2_rxdsp_rate_test PRIVATE aethercore Qt6::Core Qt6::Test)
add_test(NAME hl2_rxdsp_rate_test COMMAND hl2_rxdsp_rate_test)

# The panadapter frame rate must follow the operator's slider, not the span
# (#4470). Wall-clock paced, so it lives in its own target.
add_executable(hl2_spectrum_rate_test tests/hl2_spectrum_rate_test.cpp)
target_include_directories(hl2_spectrum_rate_test PRIVATE src)
target_link_libraries(hl2_spectrum_rate_test PRIVATE aethercore Qt6::Core Qt6::Test)
add_test(NAME hl2_spectrum_rate_test COMMAND hl2_spectrum_rate_test)

add_executable(hl2_shift_test tests/hl2_shift_test.cpp)
target_include_directories(hl2_shift_test PRIVATE src)
target_link_libraries(hl2_shift_test PRIVATE aethercore Qt6::Core Qt6::Test)
add_test(NAME hl2_shift_test COMMAND hl2_shift_test)

add_executable(hl2_notch_seed_test tests/hl2_notch_seed_test.cpp)
target_include_directories(hl2_notch_seed_test PRIVATE src)
target_link_libraries(hl2_notch_seed_test PRIVATE aethercore Qt6::Core Qt6::Test)
add_test(NAME hl2_notch_seed_test COMMAND hl2_notch_seed_test)

# HL2 manual frequency calibration — simulates the gateware's own NCO arithmetic
# (radio.v freqcomp/M2/M3) on a deliberately wrong master clock, so the
# correction is checked against the hardware's behaviour rather than against its
# own algebra. Also pins the sign convention and the wire command banks.
add_executable(hl2_freq_cal_test tests/hl2_freq_cal_test.cpp)
target_include_directories(hl2_freq_cal_test PRIVATE src)
target_link_libraries(hl2_freq_cal_test PRIVATE aethercore Qt6::Core)
add_test(NAME hl2_freq_cal_test COMMAND hl2_freq_cal_test)

# Calibration page Trim buttons — applied live while held, persisted exactly
# once on release. Pins the Qt auto-repeat property the split rests on (every
# repeat tick emits released()/clicked() with the button still DOWN), so a
# regression shows up as one failing assertion rather than as a settings file
# written eight times a second. Widgets only — no project sources.
add_executable(hl2_trim_autorepeat_test tests/hl2_trim_autorepeat_test.cpp)
target_link_libraries(hl2_trim_autorepeat_test PRIVATE Qt6::Core Qt6::Widgets Qt6::Test)
add_test(NAME hl2_trim_autorepeat_test COMMAND hl2_trim_autorepeat_test)
set_tests_properties(hl2_trim_autorepeat_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# HL2 backend — IRadioBackend seam contract against a capped fake HL2.
add_executable(hl2_backend_test tests/hl2_backend_test.cpp)
target_include_directories(hl2_backend_test PRIVATE src)
target_link_libraries(hl2_backend_test PRIVATE aethercore Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME hl2_backend_test COMMAND hl2_backend_test)

# HL2 transport telemetry — the IRadioBackend::linkStats seam that feeds the
# heartbeat indicator, the status-bar Network field and the diagnostics pane on
# a family that owns no RadioConnection and no PanadapterStream.
add_executable(hl2_link_stats_test tests/hl2_link_stats_test.cpp)
target_include_directories(hl2_link_stats_test PRIVATE src tests)
target_link_libraries(hl2_link_stats_test PRIVATE aethercore Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME hl2_link_stats_test COMMAND hl2_link_stats_test)

# The CONSUMER half of the same seam. The backend can publish a perfect snapshot
# while every readout downstream still lies, so this drives a real RadioModel
# against a fake HL2: the readouts leaving their structural zero, the
# absent-vs-zero predicates on BOTH sides of the disconnect edge, and the
# per-session reset the two scoring-session entry points share.
add_executable(hl2_link_stats_model_test tests/hl2_link_stats_model_test.cpp)
target_include_directories(hl2_link_stats_model_test PRIVATE src tests)
target_link_libraries(hl2_link_stats_model_test PRIVATE aethercore Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME hl2_link_stats_model_test COMMAND hl2_link_stats_model_test)

# HL2 receiver churn — add/close receivers against a LIVE EP6 stream. The only
# test that puts the m_rx reshape and the I/O-thread fan-out in contention, which
# is what lets the weekly TSan job (.github/workflows/sanitizers.yml) exercise the
# ordering that keeps them apart: Hl2Backend::publishIoDsps() hands the sample
# path its own copy (m_ioDsps) and blocks until the I/O thread has taken it, so
# the reshape never mutates a container a fan-out is walking. (There is no
# fenceIo() — an earlier draft named one and this comment outlived it.)
add_executable(hl2_receiver_churn_test tests/hl2_receiver_churn_test.cpp)
target_include_directories(hl2_receiver_churn_test PRIVATE src tests)
target_link_libraries(hl2_receiver_churn_test PRIVATE aethercore Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME hl2_receiver_churn_test COMMAND hl2_receiver_churn_test)

# HL2 connect re-entrancy — a connect or a disconnect landing while the WDSP
# chains are still opening. Needs no radio; see the file header.
add_executable(hl2_connect_reentrancy_test tests/hl2_connect_reentrancy_test.cpp)
target_include_directories(hl2_connect_reentrancy_test PRIVATE src tests)
target_link_libraries(hl2_connect_reentrancy_test PRIVATE aethercore Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME hl2_connect_reentrancy_test COMMAND hl2_connect_reentrancy_test)

add_executable(client_eq_test
    tests/client_eq_test.cpp
    src/core/ClientEq.cpp
)
target_include_directories(client_eq_test PRIVATE src)

# Fractional-octave smoothing — exercises the static helper on
# ClientEqCurveWidget with no live widget required.
add_executable(client_eq_smoothing_test
    tests/client_eq_smoothing_test.cpp
    src/gui/ClientEqCurveWidget.cpp
    src/core/ClientEq.cpp
)
target_include_directories(client_eq_smoothing_test PRIVATE src)
target_link_libraries(client_eq_smoothing_test PRIVATE Qt6::Widgets)
set_target_properties(client_eq_smoothing_test PROPERTIES AUTOMOC ON)
add_test(NAME client_eq_smoothing_test COMMAND client_eq_smoothing_test)
set_tests_properties(client_eq_smoothing_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2")

add_executable(client_comp_test
    tests/client_comp_test.cpp
    src/core/ClientComp.cpp
    src/core/ClientPhaseRotator.cpp
)
target_include_directories(client_comp_test PRIVATE src)

add_executable(slice_label_test
    tests/slice_label_test.cpp
    src/gui/SliceLabel.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(slice_label_test PRIVATE src)
target_link_libraries(slice_label_test PRIVATE Qt6::Gui)
add_test(NAME slice_label_test COMMAND slice_label_test)

add_executable(vfo_flag_placement_test
    tests/vfo_flag_placement_test.cpp
)
target_include_directories(vfo_flag_placement_test PRIVATE src)
target_link_libraries(vfo_flag_placement_test PRIVATE Qt6::Widgets)
add_test(NAME vfo_flag_placement_test COMMAND vfo_flag_placement_test)

add_executable(mac_cursor_compat_test
    tests/mac_cursor_compat_test.cpp
)
target_include_directories(mac_cursor_compat_test PRIVATE src)
target_link_libraries(mac_cursor_compat_test PRIVATE Qt6::Core)
add_test(NAME mac_cursor_compat_test COMMAND mac_cursor_compat_test)

add_executable(slice_model_letter_test
    tests/slice_model_letter_test.cpp
    src/models/SliceModel.cpp
    src/core/DigitalVoiceModeRegistry.cpp
)
target_include_directories(slice_model_letter_test PRIVATE src)
target_link_libraries(slice_model_letter_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME slice_model_letter_test COMMAND slice_model_letter_test)

# Per-slice manual squelch memory (#3326 follow-up, #4592) — guards against
# the cross-slice leak reopening via a caller that forgets to keep the live
# level and the manual memory in sync, or an Auto-mode echo overwriting it.
add_executable(slice_model_squelch_memory_test
    tests/slice_model_squelch_memory_test.cpp
    src/models/SliceModel.cpp
    src/core/DigitalVoiceModeRegistry.cpp
)
target_include_directories(slice_model_squelch_memory_test PRIVATE src)
target_link_libraries(slice_model_squelch_memory_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME slice_model_squelch_memory_test COMMAND slice_model_squelch_memory_test)

# ThemeManager — RFC #3076 Phase 1.  Verifies the built-in default-dark
# theme loads from Qt resources, scalar tokens resolve, missing tokens
# don't crash, and the stylesheet template resolver substitutes correctly.
qt_add_resources(THEME_TEST_RESOURCES resources/resources.qrc)
add_executable(theme_manager_test
    tests/theme_manager_test.cpp
    src/core/ThemeManager.cpp
    src/core/ThemeSeedGenerated.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    ${THEME_TEST_RESOURCES}
)
target_include_directories(theme_manager_test PRIVATE src)
target_link_libraries(theme_manager_test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
add_test(NAME theme_manager_test COMMAND theme_manager_test)
set_tests_properties(theme_manager_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Compiled-in theme seed (#3184).  DELIBERATELY has no ${THEME_TEST_RESOURCES}:
# with the theme resource linked, the ThemeManager constructor loads Default
# Dark straight after seeding and the JSON hides whatever the seed actually
# contains — which is how nine seed tokens drifted unnoticed for months.
# Without it, the seed IS the rendered palette and every token is assertable
# through the public API.  Adding the resource here would silently turn this
# into a second, weaker copy of theme_manager_test; the test asserts
# availableThemes() is empty so that mistake fails loudly instead.
add_executable(theme_seed_test
    tests/theme_seed_test.cpp
    src/core/ThemeManager.cpp
    src/core/ThemeSeedGenerated.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
)
target_include_directories(theme_seed_test PRIVATE src)
target_link_libraries(theme_seed_test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
add_test(NAME theme_seed_test COMMAND theme_seed_test)
set_tests_properties(theme_seed_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Panadapter overlay collapse semantics for owner-managed status cards (#4387):
# a collapse must shrink the card, survive owner re-assertion, and never make
# the indicator disappear while its condition still holds.
qt_add_resources(PAN_OVERLAY_TEST_RESOURCES resources/resources.qrc)
add_executable(panadapter_message_overlay_test
    tests/panadapter_message_overlay_test.cpp
    src/gui/PanadapterMessageOverlay.cpp
    src/core/ThemeManager.cpp
    src/core/ThemeSeedGenerated.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    ${PAN_OVERLAY_TEST_RESOURCES}
)
target_include_directories(panadapter_message_overlay_test PRIVATE src)
target_link_libraries(panadapter_message_overlay_test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
add_test(NAME panadapter_message_overlay_test COMMAND panadapter_message_overlay_test)
set_tests_properties(panadapter_message_overlay_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# AppSettings persistence safety. Each scenario is a separate process because
# AppSettings is a process-wide singleton and load state must not leak between
# cases.
add_executable(settings_browser_dialog_test
    tests/settings_browser_dialog_test.cpp
    src/gui/SettingsBrowserDialog.cpp
    src/gui/PersistentDialog.cpp
    src/gui/FramelessResizer.cpp
    src/gui/FramelessWindowTitleBar.cpp
    src/gui/FramelessMessageBox.cpp
)
target_include_directories(settings_browser_dialog_test PRIVATE src tests)
target_link_libraries(settings_browser_dialog_test PRIVATE
    aethercore Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Test
)
set_target_properties(settings_browser_dialog_test PROPERTIES AUTOMOC ON)
add_test(NAME settings_browser_dialog COMMAND settings_browser_dialog_test)
set_tests_properties(settings_browser_dialog PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(app_settings_safety_test
    tests/app_settings_safety_test.cpp
    ${AETHER_SETTINGS_SOURCES}
    # format instead of a copy of it (Qt6::Network/AUTOMOC are for its QObject
    # + QUdpSocket; the test only calls its static helpers).
    src/core/backends/hl2/Hl2Discovery.cpp
    src/core/backends/hl2/MetisProtocol.cpp
)
set_target_properties(app_settings_safety_test PROPERTIES AUTOMOC ON)
target_include_directories(app_settings_safety_test PRIVATE src)
target_link_libraries(app_settings_safety_test PRIVATE Qt6::Core Qt6::Network)
foreach(APP_SETTINGS_SCENARIO
        save-before-load
        xml-import-parity
        first-run
        xml-import-tmp-promotion
        xml-import-bak-fallback
        xml-artifacts-unusable
        no-reimport
        xml-changed-notice
        credential-exodus
        corrupt-db-restore-backup
        corrupt-db-reimport-xml
        locked-db-fails-closed
        newer-schema-readonly
        dirty-row-save
        display-slice-depth-default
        nickname-key-roundtrip
        browser-api)
    add_test(
        NAME app_settings_safety_${APP_SETTINGS_SCENARIO}
        COMMAND app_settings_safety_test ${APP_SETTINGS_SCENARIO})
endforeach()

add_executable(nr2_settings_model_test
    tests/nr2_settings_model_test.cpp
    src/models/Nr2SettingsModel.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(nr2_settings_model_test PRIVATE src tests)
target_link_libraries(nr2_settings_model_test PRIVATE Qt6::Core Qt6::Test)
set_target_properties(nr2_settings_model_test PROPERTIES AUTOMOC ON)
add_test(NAME nr2_settings_model_test COMMAND nr2_settings_model_test)

add_executable(rn2_settings_model_test
    tests/rn2_settings_model_test.cpp
    src/models/Rn2SettingsModel.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(rn2_settings_model_test PRIVATE src tests)
target_link_libraries(rn2_settings_model_test PRIVATE Qt6::Core Qt6::Test)
set_target_properties(rn2_settings_model_test PROPERTIES AUTOMOC ON)
add_test(NAME rn2_settings_model_test COMMAND rn2_settings_model_test)

add_executable(panadapter_model_rx_antenna_test
    tests/panadapter_model_rx_antenna_test.cpp
    src/models/PanadapterModel.cpp
    src/core/PerfTelemetry.cpp
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(panadapter_model_rx_antenna_test PRIVATE src)
target_link_libraries(panadapter_model_rx_antenna_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME panadapter_model_rx_antenna_test COMMAND panadapter_model_rx_antenna_test)

add_executable(packet_loss_concealment_test
    tests/packet_loss_concealment_test.cpp
    src/core/PacketLossConcealment.cpp
)
target_include_directories(packet_loss_concealment_test PRIVATE src)
target_link_libraries(packet_loss_concealment_test PRIVATE Qt6::Core)
add_test(NAME packet_loss_concealment_test COMMAND packet_loss_concealment_test)

add_executable(model_capabilities_test
    tests/model_capabilities_test.cpp
    src/models/ModelCapabilities.cpp
)
target_include_directories(model_capabilities_test PRIVATE src)
target_link_libraries(model_capabilities_test PRIVATE Qt6::Core)
add_test(NAME model_capabilities_test COMMAND model_capabilities_test)

# Waterfall Black Level mode arithmetic (#4606) — header-only pure logic.
add_executable(auto_black_mode_test tests/auto_black_mode_test.cpp)
target_include_directories(auto_black_mode_test PRIVATE src)
add_test(NAME auto_black_mode_test COMMAND auto_black_mode_test)

# Waterfall rate <-> row cadence mapping (#4606) — header-only pure logic.
add_executable(waterfall_rate_test tests/waterfall_rate_test.cpp)
target_include_directories(waterfall_rate_test PRIVATE src)
add_test(NAME waterfall_rate_test COMMAND waterfall_rate_test)

# Adaptive-throttle display-status echo gate (#4261) — header-only pure logic.
add_executable(display_status_gate_test tests/display_status_gate_test.cpp)
target_include_directories(display_status_gate_test PRIVATE src)
add_test(NAME display_status_gate_test COMMAND display_status_gate_test)

# Frameless-window geometry restore (#4328) — blob parse + the caption-free
# re-clamp.  Windows-only in effect, but the logic is pure, so it is pinned on
# every platform; case 4 drives a real QWidget so a future Qt changing the
# saveGeometry() layout or dropping the clamp fails here instead of silently
# misplacing the main window.
add_executable(window_geometry_restore_test
    tests/window_geometry_restore_test.cpp
    src/gui/WindowGeometryRestore.cpp
)
target_include_directories(window_geometry_restore_test PRIVATE src)
target_link_libraries(window_geometry_restore_test PRIVATE Qt6::Widgets)
add_test(NAME window_geometry_restore_test COMMAND window_geometry_restore_test)
set_tests_properties(window_geometry_restore_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# KiwiSDR band-recall re-bind policy (#4158) — header-only, pure logic.
add_executable(kiwi_rebind_tracker_test tests/kiwi_rebind_tracker_test.cpp)
target_include_directories(kiwi_rebind_tracker_test PRIVATE src)
target_link_libraries(kiwi_rebind_tracker_test PRIVATE Qt6::Core)
add_test(NAME kiwi_rebind_tracker_test COMMAND kiwi_rebind_tracker_test)

# Center Lock band-recall re-bind policy — header-only, pure logic.
add_executable(center_lock_rebind_tracker_test tests/center_lock_rebind_tracker_test.cpp)
target_include_directories(center_lock_rebind_tracker_test PRIVATE src)
target_link_libraries(center_lock_rebind_tracker_test PRIVATE Qt6::Core)
add_test(NAME center_lock_rebind_tracker_test COMMAND center_lock_rebind_tracker_test)

# Last-session DAX restore window + quit-time key prune (#4558) — header-only.
add_executable(dax_restore_policy_test tests/dax_restore_policy_test.cpp)
target_include_directories(dax_restore_policy_test PRIVATE src)
target_link_libraries(dax_restore_policy_test PRIVATE Qt6::Core)
add_test(NAME dax_restore_policy_test COMMAND dax_restore_policy_test)

# Active-slice policy during FLEX band-stack teardown/rebuild — header-only.
add_executable(band_recall_slice_selection_policy_test
    tests/band_recall_slice_selection_policy_test.cpp
)
target_include_directories(band_recall_slice_selection_policy_test PRIVATE src)
add_test(NAME band_recall_slice_selection_policy_test
    COMMAND band_recall_slice_selection_policy_test)

# When that policy applies — the window opened by an actually-dispatched
# `display pan set <pan> band=` write. Header-only.
add_executable(band_recall_selection_guard_test
    tests/band_recall_selection_guard_test.cpp
)
target_include_directories(band_recall_selection_guard_test PRIVATE src)
target_link_libraries(band_recall_selection_guard_test PRIVATE Qt6::Core)
add_test(NAME band_recall_selection_guard_test
    COMMAND band_recall_selection_guard_test)

add_executable(declared_bands_test
    tests/declared_bands_test.cpp
    src/models/DeclaredBands.cpp
)
target_include_directories(declared_bands_test PRIVATE src)
target_link_libraries(declared_bands_test PRIVATE Qt6::Core)
add_test(NAME declared_bands_test COMMAND declared_bands_test)

# Pins the BandDefs.h band edges through BandSettings::bandForFrequency() —
# the lookup every frequency->band consumer shares, including the TX-filter
# PTT preflight. #4723 shipped a 60m upper edge 1.3 kHz below the top of a
# legal channel and nothing in the tree noticed.
add_executable(band_edges_test
    tests/band_edges_test.cpp
    src/models/BandSettings.cpp
)
target_include_directories(band_edges_test PRIVATE src)
target_link_libraries(band_edges_test PRIVATE Qt6::Core)
add_test(NAME band_edges_test COMMAND band_edges_test)

# Band-plan segment labels feed isVoiceSegmentLabel(), which gates S-History /
# QRM voice detection — a label carrying no recognised emission token silently
# switches voice markers off for that spectrum. Reads the shipped resource, so
# the assertion is about the file that actually ships (#4723).
qt_add_resources(BANDPLAN_VOICE_LABELS_TEST_RESOURCES resources/resources.qrc)
add_executable(bandplan_voice_labels_test
    tests/bandplan_voice_labels_test.cpp
    src/core/VoiceSignalDetector.cpp
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    ${AETHER_SETTINGS_SOURCES}
    ${BANDPLAN_VOICE_LABELS_TEST_RESOURCES}
)
target_include_directories(bandplan_voice_labels_test PRIVATE src)
target_link_libraries(bandplan_voice_labels_test PRIVATE Qt6::Core Qt6::Gui)
add_test(NAME bandplan_voice_labels_test COMMAND bandplan_voice_labels_test)

add_executable(radio_discovery_test
    tests/radio_discovery_test.cpp
    src/core/RadioDiscovery.cpp
)
target_include_directories(radio_discovery_test PRIVATE src)
target_compile_definitions(radio_discovery_test PRIVATE AETHERSDR_TESTING)
target_link_libraries(radio_discovery_test PRIVATE Qt6::Core Qt6::Network)
add_test(NAME radio_discovery_test COMMAND radio_discovery_test)

# Agent automation bridge phaseful-gesture lifecycle (#4353). Uses two real
# QLocalSocket clients so the regression proves an independent request can run
# while a QSlider remains genuinely down, plus auth/read-only/TX cleanup rails.
add_executable(automation_server_gesture_test
    tests/automation_server_gesture_test.cpp
)
target_include_directories(automation_server_gesture_test PRIVATE src tests)
target_link_libraries(automation_server_gesture_test PRIVATE
    aethercore Qt6::Core Qt6::Network Qt6::Widgets
)
set_target_properties(automation_server_gesture_test PROPERTIES AUTOMOC ON)
add_test(NAME automation_server_gesture_test COMMAND automation_server_gesture_test)
set_tests_properties(automation_server_gesture_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(client_quindar_test
    tests/client_quindar_test.cpp
    src/core/ClientQuindarTone.cpp
)
target_include_directories(client_quindar_test PRIVATE src)
target_link_libraries(client_quindar_test PRIVATE Qt6::Core)

add_executable(tx_mic_channel_normalizer_test
    tests/tx_mic_channel_normalizer_test.cpp
    src/core/TxMicChannelNormalizer.cpp
    src/core/Resampler.cpp
)
target_include_directories(tx_mic_channel_normalizer_test PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/third_party/r8brain
)
target_link_libraries(tx_mic_channel_normalizer_test PRIVATE Qt6::Core)
add_test(NAME tx_mic_channel_normalizer_test COMMAND tx_mic_channel_normalizer_test)

# Pins the SkyRoof-parity WFM DSP chain: NCO offset correction removes the
# discriminator DC term (fixed pan + Doppler-stepped slice), twin linear-phase
# resamplers deliver exactly 48 kHz from any native DAX IQ rate, and streaming
# state is continuous across block boundaries.
add_executable(wfm_dsp_test
    tests/wfm_dsp_test.cpp
    src/core/WfmDsp.cpp
    src/core/Resampler.cpp
)
target_include_directories(wfm_dsp_test PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/third_party/r8brain
)
target_link_libraries(wfm_dsp_test PRIVATE Qt6::Core)
add_test(NAME wfm_dsp_test COMMAND wfm_dsp_test)

# Hardware-gated functional test for the optional NVIDIA AFX GPU denoiser.
# Built only when the feature is enabled; SKIPs at runtime without a pack/GPU.
if(ENABLE_NVIDIA_AFX AND ((UNIX AND NOT APPLE) OR WIN32) AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    add_executable(nvidia_afx_filter_test
        tests/nvidia_afx_filter_test.cpp
        src/core/NvidiaAfxFilter.cpp
        src/core/MonoDspStereoAdapter.cpp
        src/core/Resampler.cpp
    )
    target_compile_definitions(nvidia_afx_filter_test PRIVATE HAVE_NVIDIA_AFX)
    target_include_directories(nvidia_afx_filter_test PRIVATE
        src
        ${CMAKE_SOURCE_DIR}/third_party/r8brain
    )
    target_link_libraries(nvidia_afx_filter_test PRIVATE Qt6::Core)
    if(UNIX)
        target_link_libraries(nvidia_afx_filter_test PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME nvidia_afx_filter_test COMMAND nvidia_afx_filter_test)
endif()

# Pure, headless, hardware-free golden matrix for the consolidated audio
# format/rate negotiation policy (#3306). TargetOs is data, so this one binary
# exercises the Windows/macOS/Linux ladders regardless of the CI host.
add_executable(audio_format_negotiation_test
    tests/audio_format_negotiation_test.cpp
    src/core/AudioFormatNegotiator.cpp
)
target_include_directories(audio_format_negotiation_test PRIVATE src)
target_link_libraries(audio_format_negotiation_test PRIVATE Qt6::Core)
add_test(NAME audio_format_negotiation_test COMMAND audio_format_negotiation_test)

# Smoke test for the live Qt-Multimedia wrapper (AudioDeviceNegotiator): probes
# the real default devices and round-trips to an openable QAudioFormat. Tolerant
# of headless runners with no audio hardware.
add_executable(audio_device_negotiator_test
    tests/audio_device_negotiator_test.cpp
    src/core/AudioDeviceNegotiator.cpp
    src/core/AudioFormatNegotiator.cpp
)
target_include_directories(audio_device_negotiator_test PRIVATE src)
target_link_libraries(audio_device_negotiator_test PRIVATE Qt6::Core Qt6::Multimedia)
add_test(NAME audio_device_negotiator_test COMMAND audio_device_negotiator_test)

# Unit test for the AudioOutputRouter sink registry (#3306) — seeding, fan-out,
# and the QPointer guard. Hardware-independent. AudioOutputRouter is a QObject,
# so this relies on the project-global AUTOMOC.
add_executable(audio_output_router_test
    tests/audio_output_router_test.cpp
    src/core/AudioOutputRouter.cpp
)
target_include_directories(audio_output_router_test PRIVATE src)
target_link_libraries(audio_output_router_test PRIVATE Qt6::Core Qt6::Multimedia)
add_test(NAME audio_output_router_test COMMAND audio_output_router_test)

# Pure mode-policy regression for global AetherDSP selection (#4415).
add_executable(aether_dsp_mode_policy_test
    tests/aether_dsp_mode_policy_test.cpp
    src/core/AetherDspModePolicy.cpp
)
target_include_directories(aether_dsp_mode_policy_test PRIVATE src)
target_link_libraries(aether_dsp_mode_policy_test PRIVATE Qt6::Core)
add_test(NAME aether_dsp_mode_policy_test COMMAND aether_dsp_mode_policy_test)

# Hardware-free state-machine coverage for the opt-in TX capture health
# summary. Reproduces the Qt pull-mode Active -> Idle/full-buffer signature and
# verifies anomaly rate limiting without requiring PipeWire or an audio device.
add_executable(tx_capture_health_test
    tests/tx_capture_health_test.cpp
)
target_include_directories(tx_capture_health_test PRIVATE src)
target_link_libraries(tx_capture_health_test PRIVATE Qt6::Core)
add_test(NAME tx_capture_health_test COMMAND tx_capture_health_test)

# Regression test for #4003 — QsoRecorder must not dereference a SliceModel that
# was freed (reconnect prune) before recording starts. QPointer auto-nulls the
# reference; the test deletes the slice and asserts the metadata is cleared.
add_executable(qso_recorder_slice_lifetime_test
    tests/qso_recorder_slice_lifetime_test.cpp
    src/core/QsoRecorder.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/AudioDeviceNegotiator.cpp
    src/core/AudioFormatNegotiator.cpp
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    src/core/Resampler.cpp
    src/models/SliceModel.cpp
    src/core/DigitalVoiceModeRegistry.cpp
)
target_include_directories(qso_recorder_slice_lifetime_test PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/third_party/r8brain
)
target_link_libraries(qso_recorder_slice_lifetime_test PRIVATE Qt6::Core Qt6::Multimedia)
add_test(NAME qso_recorder_slice_lifetime_test COMMAND qso_recorder_slice_lifetime_test)

# #4629 — the start policy alone. Pure/constexpr, no Qt at all: the radio-side
# case (which must NEVER be blocked) is also asserted at compile time.
add_executable(qso_record_start_policy_test
    tests/qso_record_start_policy_test.cpp
)
target_include_directories(qso_record_start_policy_test PRIVATE src)
add_test(NAME qso_record_start_policy_test COMMAND qso_record_start_policy_test)

# #4629 — the recorder honoring that policy: Client-Side + PC Audio off must
# create NO FILE (not merely fail to record), radio-side must still start, and a
# zero-capture recording must raise an error instead of passing for success.
add_executable(qso_recorder_pc_audio_guard_test
    tests/qso_recorder_pc_audio_guard_test.cpp
    src/core/QsoRecorder.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/AudioDeviceNegotiator.cpp
    src/core/AudioFormatNegotiator.cpp
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    src/core/Resampler.cpp
    src/models/SliceModel.cpp
    src/core/DigitalVoiceModeRegistry.cpp
)
target_include_directories(qso_recorder_pc_audio_guard_test PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/third_party/r8brain
)
target_link_libraries(qso_recorder_pc_audio_guard_test PRIVATE Qt6::Core Qt6::Multimedia)
add_test(NAME qso_recorder_pc_audio_guard_test COMMAND qso_recorder_pc_audio_guard_test)

add_executable(profile_transfer_test
    tests/profile_transfer_test.cpp
)
target_include_directories(profile_transfer_test PRIVATE src)
target_link_libraries(profile_transfer_test PRIVATE Qt6::Core)
add_test(NAME profile_transfer_test COMMAND profile_transfer_test)

add_executable(waveform_upload_state_test
    tests/waveform_upload_state_test.cpp
    src/core/WaveformUploadState.cpp
)
target_include_directories(waveform_upload_state_test PRIVATE src)
target_link_libraries(waveform_upload_state_test PRIVATE Qt6::Core)
add_test(NAME waveform_upload_state_test COMMAND waveform_upload_state_test)

add_executable(zip_archive_test
    tests/zip_archive_test.cpp
    src/core/ZipArchive.cpp
)
target_include_directories(zip_archive_test PRIVATE src)
target_link_libraries(zip_archive_test PRIVATE Qt6::Core)
if (USE_SYSTEM_ZLIB)
    target_link_libraries(zip_archive_test PRIVATE PkgConfig::zlib)
else()
    target_link_libraries(zip_archive_test PRIVATE zlibstatic)
endif()
add_test(NAME zip_archive_test COMMAND zip_archive_test)

add_executable(legacy_waveform_package_test
    tests/legacy_waveform_package_test.cpp
    src/core/LegacyWaveformPackage.cpp
    src/core/ZipArchive.cpp
)
target_include_directories(legacy_waveform_package_test PRIVATE src)
target_link_libraries(legacy_waveform_package_test PRIVATE Qt6::Core)
if (USE_SYSTEM_ZLIB)
    target_link_libraries(legacy_waveform_package_test PRIVATE PkgConfig::zlib)
else()
    target_link_libraries(legacy_waveform_package_test PRIVATE zlibstatic)
endif()
add_test(NAME legacy_waveform_package_test COMMAND legacy_waveform_package_test)

# Pins the filter-before-merge invariant on the license-class-aware overload
# of BandPlanManager::contiguousRegionsForBand (PR #3050, closing #2649). (#3060)
add_executable(band_plan_license_filter_test
    tests/band_plan_license_filter_test.cpp
    src/models/BandPlanManager.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(band_plan_license_filter_test PRIVATE src)
target_link_libraries(band_plan_license_filter_test PRIVATE Qt6::Core Qt6::Gui)
add_test(NAME band_plan_license_filter_test COMMAND band_plan_license_filter_test)

qt_add_resources(KIWISDR_DX_SPOTS_TEST_RESOURCES resources/resources.qrc)
add_executable(kiwisdr_dx_spots_test
    tests/kiwisdr_dx_spots_test.cpp
    src/models/BandPlanManager.cpp
    ${AETHER_SETTINGS_SOURCES}
    ${KIWISDR_DX_SPOTS_TEST_RESOURCES}
)
target_include_directories(kiwisdr_dx_spots_test PRIVATE src tests)
target_link_libraries(kiwisdr_dx_spots_test PRIVATE Qt6::Core Qt6::Gui)
add_test(NAME kiwisdr_dx_spots_test COMMAND kiwisdr_dx_spots_test)

add_executable(biquad_test
    tests/biquad_test.cpp
    src/core/Biquad.cpp
    src/core/StereoBiquad.cpp
)
target_include_directories(biquad_test PRIVATE src)
add_test(NAME biquad_test COMMAND biquad_test)

add_executable(spectral_nr_test
    tests/spectral_nr_test.cpp
    src/core/SpectralNR.cpp
)
target_include_directories(spectral_nr_test PRIVATE src)
target_link_libraries(spectral_nr_test PRIVATE Qt6::Core)
add_test(NAME spectral_nr_test COMMAND spectral_nr_test)

add_executable(mono_dsp_stereo_adapter_test
    tests/mono_dsp_stereo_adapter_test.cpp
    src/core/MonoDspStereoAdapter.cpp
)
target_include_directories(mono_dsp_stereo_adapter_test PRIVATE src)
target_link_libraries(mono_dsp_stereo_adapter_test PRIVATE Qt6::Core)
add_test(NAME mono_dsp_stereo_adapter_test COMMAND mono_dsp_stereo_adapter_test)

# tests/TestEventLoop.h is test infrastructure that makes correctness claims, so
# it carries its own proof — including a negative case that pins the #4693
# iteration-count idiom as genuinely broken, so the trap cannot quietly stop
# being a trap. No aethercore link: the header depends only on Qt, and keeping
# the target minimal means it still builds when the app does not. Every case
# drives a worker thread, which is the delivery path the helpers exist to
# observe.
add_executable(test_event_loop_test tests/test_event_loop_test.cpp)
target_include_directories(test_event_loop_test PRIVATE tests)
target_link_libraries(test_event_loop_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME test_event_loop_test COMMAND test_event_loop_test)

# Just the voice fixture — linking the full resources.qrc pulled 5.8 MB of
# application assets into a unit test to reach one 458 KB WAV (PR #4689 review).
qt_add_resources(RNNOISE_FILTER_TEST_RESOURCES tests/rnnoise_filter_test.qrc)
add_executable(rnnoise_filter_test
    tests/rnnoise_filter_test.cpp
    ${RNNOISE_FILTER_TEST_RESOURCES}
)
target_link_libraries(rnnoise_filter_test PRIVATE aethercore Qt6::Core)
add_test(NAME rnnoise_filter_test COMMAND rnnoise_filter_test)

add_executable(opus_tx_pacer_test
    tests/opus_tx_pacer_test.cpp
    src/core/OpusTxPacer.cpp
)
target_include_directories(opus_tx_pacer_test PRIVATE src)
target_link_libraries(opus_tx_pacer_test PRIVATE Qt6::Core)
add_test(NAME opus_tx_pacer_test COMMAND opus_tx_pacer_test)

add_executable(adaptive_filter_test
    tests/adaptive_filter_test.cpp
    src/core/OccupiedRegion.cpp
)
target_include_directories(adaptive_filter_test PRIVATE src)
target_link_libraries(adaptive_filter_test PRIVATE Qt6::Core)
add_test(NAME adaptive_filter_test COMMAND adaptive_filter_test)

add_executable(waveform_scope_model_test
    tests/waveform_scope_model_test.cpp
    src/gui/WaveformScopeModel.cpp
)
target_include_directories(waveform_scope_model_test PRIVATE src)
target_link_libraries(waveform_scope_model_test PRIVATE Qt6::Core)
add_test(NAME waveform_scope_model_test COMMAND waveform_scope_model_test)

# Engine-level test: drives AdaptiveFilterEngine::processFrame through a real
# SliceModel (signals only, no radio) with monotonic timestamps — covers the
# wall-clock pacing / send-throttle / QSO-handoff logic the measurement test
# can't reach. Both AdaptiveFilterEngine and SliceModel are Q_OBJECT (AUTOMOC).
add_executable(adaptive_engine_test
    tests/adaptive_engine_test.cpp
    src/core/AdaptiveFilterEngine.cpp
    src/core/OccupiedRegion.cpp
    src/models/SliceModel.cpp
    src/core/DigitalVoiceModeRegistry.cpp
    src/core/KiwiSdrProtocol.cpp
)
target_include_directories(adaptive_engine_test PRIVATE src)
target_link_libraries(adaptive_engine_test PRIVATE Qt6::Core)
set_target_properties(adaptive_engine_test PROPERTIES AUTOMOC ON)
add_test(NAME adaptive_engine_test COMMAND adaptive_engine_test)

add_executable(kiwi_sdr_protocol_test
    tests/kiwi_sdr_protocol_test.cpp
    src/core/KiwiSdrProtocol.cpp
)
target_include_directories(kiwi_sdr_protocol_test PRIVATE src)
target_link_libraries(kiwi_sdr_protocol_test PRIVATE Qt6::Core)
add_test(NAME kiwi_sdr_protocol_test COMMAND kiwi_sdr_protocol_test)

add_executable(kiwi_sdr_manager_password_test
    tests/kiwi_sdr_manager_password_test.cpp
)
target_include_directories(kiwi_sdr_manager_password_test PRIVATE src)
target_link_libraries(kiwi_sdr_manager_password_test PRIVATE
    aethercore Qt6::Core Qt6::Test)
set_target_properties(kiwi_sdr_manager_password_test PROPERTIES AUTOMOC ON)
add_test(NAME kiwi_sdr_manager_password_test
         COMMAND kiwi_sdr_manager_password_test)

add_executable(kiwi_sdr_manager_csv_test
    tests/kiwi_sdr_manager_csv_test.cpp
)
target_include_directories(kiwi_sdr_manager_csv_test PRIVATE src)
target_link_libraries(kiwi_sdr_manager_csv_test PRIVATE
    aethercore Qt6::Core Qt6::Test)
set_target_properties(kiwi_sdr_manager_csv_test PROPERTIES AUTOMOC ON)
add_test(NAME kiwi_sdr_manager_csv_test COMMAND kiwi_sdr_manager_csv_test)

add_executable(kiwi_sdr_trace_math_test
    tests/kiwi_sdr_trace_math_test.cpp
)
target_include_directories(kiwi_sdr_trace_math_test PRIVATE src)
target_link_libraries(kiwi_sdr_trace_math_test PRIVATE Qt6::Core)
add_test(NAME kiwi_sdr_trace_math_test COMMAND kiwi_sdr_trace_math_test)

add_executable(dss_renderer_test
    tests/dss_renderer_test.cpp
    src/gui/DssRenderer.cpp
)
target_include_directories(dss_renderer_test PRIVATE src)
target_link_libraries(dss_renderer_test PRIVATE Qt6::Core Qt6::Gui)
add_test(NAME dss_renderer_test COMMAND dss_renderer_test)

add_executable(spectrum_preview_logic_test
    tests/spectrum_preview_logic_test.cpp
)
target_include_directories(spectrum_preview_logic_test PRIVATE src)
target_link_libraries(spectrum_preview_logic_test PRIVATE Qt6::Core)
add_test(NAME spectrum_preview_logic_test COMMAND spectrum_preview_logic_test)

add_executable(software_opengl_request_test
    tests/software_opengl_request_test.cpp
)
target_include_directories(software_opengl_request_test PRIVATE src)
target_link_libraries(software_opengl_request_test PRIVATE Qt6::Core)
add_test(NAME software_opengl_request_test COMMAND software_opengl_request_test)

# Kiwi-display recenter write policy — pins that tune-driven recenters on a
# kiwi-display pan stay widget-local (never pairing a new center with the
# frozen PanadapterModel bandwidth, which snapped the zoom back to the
# kiwi-assignment span).
add_executable(pan_recenter_policy_test
    tests/pan_recenter_policy_test.cpp
)
target_include_directories(pan_recenter_policy_test PRIVATE src)
add_test(NAME pan_recenter_policy_test COMMAND pan_recenter_policy_test)

add_executable(waterfall_history_buffer_test
    tests/waterfall_history_buffer_test.cpp
    src/gui/WaterfallHistoryBuffer.cpp
)
target_include_directories(waterfall_history_buffer_test PRIVATE src)
target_link_libraries(waterfall_history_buffer_test PRIVATE Qt6::Core)
add_test(NAME waterfall_history_buffer_test COMMAND waterfall_history_buffer_test)

add_executable(kiwi_sdr_redirect_policy_test
    tests/kiwi_sdr_redirect_policy_test.cpp
    src/core/KiwiSdrRedirectPolicy.cpp
)
target_include_directories(kiwi_sdr_redirect_policy_test PRIVATE src)
target_link_libraries(kiwi_sdr_redirect_policy_test PRIVATE Qt6::Core)
add_test(NAME kiwi_sdr_redirect_policy_test COMMAND kiwi_sdr_redirect_policy_test)

add_executable(receive_presentation_sync_test
    tests/receive_presentation_sync_test.cpp
    src/core/ReceivePresentationSync.cpp
)
target_include_directories(receive_presentation_sync_test PRIVATE src)
target_link_libraries(receive_presentation_sync_test PRIVATE Qt6::Core)
add_test(NAME receive_presentation_sync_test COMMAND receive_presentation_sync_test)

# Public-directory parser + external-API (ext_api) policy honoring.
add_executable(kiwi_public_directory_test
    tests/kiwi_public_directory_test.cpp
    src/core/KiwiPublicDirectory.cpp
)
target_include_directories(kiwi_public_directory_test PRIVATE src)
target_link_libraries(kiwi_public_directory_test PRIVATE Qt6::Core Qt6::Network)
target_compile_definitions(kiwi_public_directory_test PRIVATE AETHERSDR_VERSION="${PROJECT_VERSION}")
set_target_properties(kiwi_public_directory_test PROPERTIES AUTOMOC ON)
add_test(NAME kiwi_public_directory_test COMMAND kiwi_public_directory_test)

# Demonstration tool: honest, API-policy-aware read of kiwisdr.com/public
# (proof-of-concept shown to operators — see docs/kiwisdr-public-directory.md).
add_executable(kiwi_directory_poc
    tools/kiwi_directory_poc.cpp
    src/core/KiwiPublicDirectory.cpp
)
target_include_directories(kiwi_directory_poc PRIVATE src)
target_link_libraries(kiwi_directory_poc PRIVATE Qt6::Core Qt6::Network)
target_compile_definitions(kiwi_directory_poc PRIVATE AETHERSDR_VERSION="${PROJECT_VERSION}")
set_target_properties(kiwi_directory_poc PROPERTIES AUTOMOC ON)

add_executable(client_gate_test
    tests/client_gate_test.cpp
    src/core/ClientGate.cpp
)
target_include_directories(client_gate_test PRIVATE src)

add_executable(client_deess_test
    tests/client_deess_test.cpp
    src/core/ClientDeEss.cpp
)
target_include_directories(client_deess_test PRIVATE src)

add_executable(client_tube_test
    tests/client_tube_test.cpp
    src/core/ClientTube.cpp
)
target_include_directories(client_tube_test PRIVATE src)

add_executable(client_pudu_test
    tests/client_pudu_test.cpp
    src/core/ClientPudu.cpp
)
target_include_directories(client_pudu_test PRIVATE src)

add_executable(client_reverb_test
    tests/client_reverb_test.cpp
    src/core/ClientReverb.cpp
)
target_include_directories(client_reverb_test PRIVATE src)

add_executable(iambic_keyer_test
    tests/iambic_keyer_test.cpp
    src/core/IambicKeyer.cpp
)
target_include_directories(iambic_keyer_test PRIVATE src)
if(UNIX)
    target_link_libraries(iambic_keyer_test PRIVATE pthread)
endif()
add_test(NAME iambic_keyer_test COMMAND iambic_keyer_test)

add_executable(passive_spots_policy_test
    tests/passive_spots_policy_test.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/SpotCommandPolicy.cpp
)
target_include_directories(passive_spots_policy_test PRIVATE src)
target_link_libraries(passive_spots_policy_test PRIVATE Qt6::Core)

add_executable(spot_mode_resolver_test
    tests/spot_mode_resolver_test.cpp
    src/core/SpotModeResolver.cpp
)
target_include_directories(spot_mode_resolver_test PRIVATE src)
target_link_libraries(spot_mode_resolver_test PRIVATE Qt6::Core)
add_test(NAME spot_mode_resolver_test COMMAND spot_mode_resolver_test)

add_executable(n1mm_spot_client_test
    tests/n1mm_spot_client_test.cpp
    src/core/N1MMSpotParser.cpp
    src/models/BandSettings.cpp
)
target_include_directories(n1mm_spot_client_test PRIVATE src)
target_link_libraries(n1mm_spot_client_test PRIVATE Qt6::Core)
add_test(NAME n1mm_spot_client_test COMMAND n1mm_spot_client_test)

add_executable(eibi_client_test
    tests/eibi_client_test.cpp
)
target_include_directories(eibi_client_test PRIVATE src)
target_link_libraries(eibi_client_test PRIVATE Qt6::Core Qt6::Network Qt6::Test aethercore)
add_test(NAME eibi_client_test COMMAND eibi_client_test)

add_executable(navtex_model_test
    tests/navtex_model_test.cpp
    src/models/NavtexModel.cpp
)
target_include_directories(navtex_model_test PRIVATE src)
target_link_libraries(navtex_model_test PRIVATE Qt6::Core Qt6::Test)

add_executable(flex_waveform_model_test
    tests/flex_waveform_model_test.cpp
    src/models/FlexWaveformModel.cpp
)
target_include_directories(flex_waveform_model_test PRIVATE src)
target_link_libraries(flex_waveform_model_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME flex_waveform_model_test COMMAND flex_waveform_model_test)

# Docker-waveform install gate policy (#4210) — header-only, pure logic.
add_executable(waveform_install_gate_test
    tests/waveform_install_gate_test.cpp
)
target_include_directories(waveform_install_gate_test PRIVATE src)
target_link_libraries(waveform_install_gate_test PRIVATE Qt6::Core)
add_test(NAME waveform_install_gate_test COMMAND waveform_install_gate_test)

# DVK indicator availability — TX-slice mode + the radio's DVK entitlement.
# Header-only, pure logic.
add_executable(dvk_availability_gate_test
    tests/dvk_availability_gate_test.cpp
)
target_include_directories(dvk_availability_gate_test PRIVATE src)
target_link_libraries(dvk_availability_gate_test PRIVATE Qt6::Core)
add_test(NAME dvk_availability_gate_test COMMAND dvk_availability_gate_test)

add_executable(digital_voice_waveform_process_test
    tests/digital_voice_waveform_process_test.cpp
    src/core/DigitalVoiceWaveformTelemetry.cpp
    src/core/DigitalVoiceWaveformProcess.cpp
    src/core/DigitalVoiceModeRegistry.cpp
    src/models/DigitalVoiceWaveformHistory.cpp
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(digital_voice_waveform_process_test PRIVATE src)
target_link_libraries(digital_voice_waveform_process_test PRIVATE Qt6::Core Qt6::Network)
add_test(NAME digital_voice_waveform_process_test COMMAND digital_voice_waveform_process_test)

add_executable(digital_voice_slice_lifecycle_test
    tests/digital_voice_slice_lifecycle_test.cpp
    src/models/SliceModel.cpp
    src/core/DigitalVoiceModeRegistry.cpp
)
target_include_directories(digital_voice_slice_lifecycle_test PRIVATE src)
target_link_libraries(digital_voice_slice_lifecycle_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME digital_voice_slice_lifecycle_test COMMAND digital_voice_slice_lifecycle_test)

add_executable(dstar_model_test
    tests/dstar_model_test.cpp
    src/models/DStarModel.cpp
    src/core/DigitalVoiceWaveformTelemetry.cpp
    src/core/DigitalVoiceWaveformProcess.cpp
    src/core/DigitalVoiceModeRegistry.cpp
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(dstar_model_test PRIVATE src)
target_link_libraries(dstar_model_test PRIVATE Qt6::Core Qt6::Network Qt6::Test)
if(Qt6SerialPort_FOUND)
    target_compile_definitions(dstar_model_test PRIVATE HAVE_SERIALPORT)
    target_link_libraries(dstar_model_test PRIVATE Qt6::SerialPort)
endif()
add_test(NAME dstar_model_test COMMAND dstar_model_test)

add_executable(dstar_accessibility_test
    tests/dstar_accessibility_test.cpp
    src/gui/DStarAccessibility.cpp
)
target_include_directories(dstar_accessibility_test PRIVATE src)
target_link_libraries(dstar_accessibility_test PRIVATE Qt6::Widgets)
add_test(NAME dstar_accessibility_test COMMAND dstar_accessibility_test)
set_tests_properties(dstar_accessibility_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(acom_protocol_test
    tests/acom_protocol_test.cpp
    src/core/AcomProtocol.cpp
)
target_include_directories(acom_protocol_test PRIVATE src)
target_link_libraries(acom_protocol_test PRIVATE Qt6::Core)
add_test(NAME acom_protocol_test COMMAND acom_protocol_test)

add_executable(spe_protocol_test
    tests/spe_protocol_test.cpp
    src/core/SpeProtocol.cpp
)
target_include_directories(spe_protocol_test PRIVATE src)
target_link_libraries(spe_protocol_test PRIVATE Qt6::Core)
add_test(NAME spe_protocol_test COMMAND spe_protocol_test)

add_executable(ole_compound_file_test
    tests/ole_compound_file_test.cpp
    src/core/OleCompoundFile.cpp
    src/core/CabExtractor.cpp
    src/core/AsyncLogWriter.cpp
    src/core/LogManager.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(ole_compound_file_test PRIVATE src)
if (USE_SYSTEM_MSPACK)
    target_link_libraries(ole_compound_file_test PRIVATE Qt6::Core PkgConfig::libmspack)
else()
    target_link_libraries(ole_compound_file_test PRIVATE Qt6::Core mspack_static)
endif()
if(UNIX)
    target_link_libraries(ole_compound_file_test PRIVATE pthread)
endif()

add_executable(xvtr_policy_test
    tests/xvtr_policy_test.cpp
    src/models/XvtrPolicy.cpp
)
target_include_directories(xvtr_policy_test PRIVATE src)
target_link_libraries(xvtr_policy_test PRIVATE Qt6::Core)

# #3449 — VITA-49 waterfall tile frequency decode (no 1 GHz ceiling)
add_executable(vita_tile_frequency_test
    tests/vita_tile_frequency_test.cpp
)
target_include_directories(vita_tile_frequency_test PRIVATE src)
target_link_libraries(vita_tile_frequency_test PRIVATE Qt6::Core)
add_test(NAME vita_tile_frequency_test COMMAND vita_tile_frequency_test)

add_executable(frequency_entry_parser_test
    tests/frequency_entry_parser_test.cpp
    src/gui/FrequencyEntryParser.cpp
)
target_include_directories(frequency_entry_parser_test PRIVATE src)
target_link_libraries(frequency_entry_parser_test PRIVATE Qt6::Core)
add_test(NAME frequency_entry_parser_test COMMAND frequency_entry_parser_test)

add_executable(radio_status_ownership_test
    tests/radio_status_ownership_test.cpp
    src/core/CommandParser.cpp
)
target_include_directories(radio_status_ownership_test PRIVATE src)
target_link_libraries(radio_status_ownership_test PRIVATE Qt6::Core)
enable_testing()

if(APPLE)
    add_executable(mac_nr_filter_test
        tests/mac_nr_filter_test.cpp
        src/core/MacNRFilter.cpp
    )
    target_include_directories(mac_nr_filter_test PRIVATE src)
    target_link_libraries(mac_nr_filter_test PRIVATE Qt6::Core "-framework Accelerate")
    add_test(NAME mac_nr_filter_test COMMAND mac_nr_filter_test)

    add_executable(mac_startup_abort_guard_test
        tests/mac_startup_abort_guard_test.cpp
        src/MacStartupAbortGuard.cpp
    )
    target_include_directories(mac_startup_abort_guard_test PRIVATE src)
    add_test(NAME mac_startup_abort_guard_test COMMAND mac_startup_abort_guard_test)
endif()

add_test(NAME radio_status_ownership_test COMMAND radio_status_ownership_test)

# ASR (RFC #4333, Phase 1): prove the vendored whisper.cpp/ggml CPU engine
# compiles and links via two model-free entry points. No model, no audio.
if (ENABLE_ASR)
    add_executable(asr_whisper_smoke_test tests/asr_whisper_smoke_test.cpp)
    target_link_libraries(asr_whisper_smoke_test PRIVATE ${_asr_whisper_link})
    add_test(NAME asr_whisper_smoke_test COMMAND asr_whisper_smoke_test)

    # Model manager: offline download/verify/failover test (file:// sources).
    add_executable(asr_model_manager_test
        tests/asr_model_manager_test.cpp
        src/asr/AsrModelCatalog.cpp
        src/asr/AsrModelManager.cpp
    )
    target_include_directories(asr_model_manager_test PRIVATE src)
    target_link_libraries(asr_model_manager_test PRIVATE Qt6::Core Qt6::Network Qt6::Concurrent)
    set_target_properties(asr_model_manager_test PROPERTIES AUTOMOC ON)
    add_test(NAME asr_model_manager_test COMMAND asr_model_manager_test)

    # VAD segmenter: pure C++, synthetic audio, no Qt/model.
    add_executable(asr_segmenter_test
        tests/asr_segmenter_test.cpp
        src/asr/AsrSegmenter.cpp
    )
    target_include_directories(asr_segmenter_test PRIVATE src)
    add_test(NAME asr_segmenter_test COMMAND asr_segmenter_test)

    # Engine orchestration: fake backend, worker thread, no whisper/model.
    add_executable(asr_engine_test
        tests/asr_engine_test.cpp
        src/asr/AsrEngine.cpp
        src/asr/AsrSegmenter.cpp
        src/asr/SileroVad.cpp    # AsrEngine references it (stub without HAVE_ONNX)
        src/asr/Fbank.cpp
        src/asr/SpeakerEmbedder.cpp
        src/asr/SpeakerClusterer.cpp
        src/core/Resampler.cpp
    )
    target_include_directories(asr_engine_test PRIVATE src ${CMAKE_SOURCE_DIR}/third_party/r8brain)
    target_link_libraries(asr_engine_test PRIVATE Qt6::Core Qt6::Test)
    set_target_properties(asr_engine_test PROPERTIES AUTOMOC ON)
    add_test(NAME asr_engine_test COMMAND asr_engine_test)

    # Silero VAD (ONNX) smoke test — only when ONNX Runtime is available; env-gated
    # on a model + WAV at run time (see the test's header), so it SKIPs otherwise.
    if(ORT_FOUND)
        add_executable(asr_silero_vad_test
            tests/asr_silero_vad_test.cpp
            src/asr/SileroVad.cpp
            src/asr/AsrSegmenter.cpp
        )
        target_include_directories(asr_silero_vad_test PRIVATE src ${ORT_INCLUDE_DIRS})
        target_compile_definitions(asr_silero_vad_test PRIVATE HAVE_ONNX)
        target_link_libraries(asr_silero_vad_test PRIVATE ${ORT_LIBRARIES})
        add_test(NAME asr_silero_vad_test COMMAND asr_silero_vad_test)
    endif()

    # Speaker clustering: pure-C++ unit test (no ONNX), always runs.
    add_executable(asr_speaker_clusterer_test
        tests/asr_speaker_clusterer_test.cpp
        src/asr/SpeakerClusterer.cpp
    )
    target_include_directories(asr_speaker_clusterer_test PRIVATE src)
    add_test(NAME asr_speaker_clusterer_test COMMAND asr_speaker_clusterer_test)

    # Speaker embedder (Fbank + ONNX) end-to-end — only with ONNX Runtime; env-
    # gated on a model + two speaker WAVs, so it SKIPs otherwise.
    if(ORT_FOUND)
        add_executable(asr_speaker_embedder_test
            tests/asr_speaker_embedder_test.cpp
            src/asr/SpeakerEmbedder.cpp
            src/asr/Fbank.cpp
            src/asr/SpeakerClusterer.cpp
        )
        target_include_directories(asr_speaker_embedder_test PRIVATE src ${ORT_INCLUDE_DIRS})
        target_compile_definitions(asr_speaker_embedder_test PRIVATE HAVE_ONNX)
        target_link_libraries(asr_speaker_embedder_test PRIVATE ${ORT_LIBRARIES})
        add_test(NAME asr_speaker_embedder_test COMMAND asr_speaker_embedder_test)
    endif()

    # sherpa-onnx backend end-to-end — only when sherpa-onnx is available; env-
    # gated on a model dir + WAV, so it SKIPs otherwise.
    if(SHERPA_FOUND)
        add_executable(asr_sherpa_backend_test
            tests/asr_sherpa_backend_test.cpp
            src/asr/SherpaOnnxBackend.cpp
        )
        target_include_directories(asr_sherpa_backend_test PRIVATE src ${SHERPA_INCLUDE_DIRS})
        target_compile_definitions(asr_sherpa_backend_test PRIVATE HAVE_SHERPA)
        target_link_libraries(asr_sherpa_backend_test PRIVATE Qt6::Core ${SHERPA_LIBRARIES})
        add_test(NAME asr_sherpa_backend_test COMMAND asr_sherpa_backend_test)
    endif()

    # Real whisper inference on a model+clip. Skips (exit 0) unless
    # AETHER_ASR_TEST_MODEL and AETHER_ASR_TEST_PCM are set, so CI stays offline.
    add_executable(asr_whisper_backend_test
        tests/asr_whisper_backend_test.cpp
        src/asr/WhisperAsrBackend.cpp
    )
    target_include_directories(asr_whisper_backend_test PRIVATE src)
    target_link_libraries(asr_whisper_backend_test PRIVATE Qt6::Core ${_asr_whisper_link})
    add_test(NAME asr_whisper_backend_test COMMAND asr_whisper_backend_test)
    if (_asr_metal_precompile)
        target_compile_definitions(asr_whisper_backend_test PRIVATE AETHER_ASR_METAL_PRECOMPILED=1)
    endif()

    # GPU probe must return promptly, and must keep Metal off Intel Macs on the
    # source-embed fallback build (#4535: the probe used to run Apple's runtime
    # shader compiler on the calling thread, which can live-lock there). The
    # define has to match aetherasr's or the test asserts the wrong branch.
    add_executable(asr_gpu_probe_test
        tests/asr_gpu_probe_test.cpp
        src/asr/WhisperAsrBackend.cpp
    )
    target_include_directories(asr_gpu_probe_test PRIVATE src)
    target_link_libraries(asr_gpu_probe_test PRIVATE Qt6::Core ${_asr_whisper_link})
    add_test(NAME asr_gpu_probe_test COMMAND asr_gpu_probe_test)
    set_tests_properties(asr_gpu_probe_test PROPERTIES TIMEOUT 180)
    if (_asr_metal_precompile)
        target_compile_definitions(asr_gpu_probe_test PRIVATE AETHER_ASR_METAL_PRECOMPILED=1)
    endif()

    # Remote backend: offline round-trip against a local mock HTTP endpoint.
    add_executable(asr_remote_backend_test
        tests/asr_remote_backend_test.cpp
        src/asr/RemoteAsrBackend.cpp
    )
    target_include_directories(asr_remote_backend_test PRIVATE src)
    target_link_libraries(asr_remote_backend_test PRIVATE Qt6::Core Qt6::Network)
    add_test(NAME asr_remote_backend_test COMMAND asr_remote_backend_test)

    # Copy Assist audio tap: which RX source it follows, and the stereo→mono
    # collapse including the non-finite guard. The policy is header-only, so
    # this needs no AudioEngine — which cannot be built headless anyway (#4486).
    add_executable(asr_tap_policy_test tests/asr_tap_policy_test.cpp)
    target_include_directories(asr_tap_policy_test PRIVATE src)
    target_link_libraries(asr_tap_policy_test PRIVATE Qt6::Core)
    add_test(NAME asr_tap_policy_test COMMAND asr_tap_policy_test)

    # Copy Assist panel: offscreen UI test of confidence color-coding + controls.
    add_executable(copy_assist_panel_test
        tests/copy_assist_panel_test.cpp
        src/gui/CopyAssistPanel.cpp
    )
    target_include_directories(copy_assist_panel_test PRIVATE src)
    target_link_libraries(copy_assist_panel_test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
    set_target_properties(copy_assist_panel_test PROPERTIES AUTOMOC ON)
    add_test(NAME copy_assist_panel_test COMMAND copy_assist_panel_test)
    set_tests_properties(copy_assist_panel_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Copy Assist settings dialog: offscreen UI test of the model + GPU pickers.
    add_executable(copy_assist_settings_dialog_test
        tests/copy_assist_settings_dialog_test.cpp
        src/gui/CopyAssistSettingsDialog.cpp
        src/gui/CopyAssistSettings.cpp
        src/gui/PersistentDialog.cpp
        src/gui/FramelessResizer.cpp
        src/gui/FramelessWindowTitleBar.cpp
        src/core/ThemeManager.cpp
        src/core/ThemeSeedGenerated.cpp
        ${AETHER_SETTINGS_SOURCES}
        src/core/LogManager.cpp
        src/core/AsyncLogWriter.cpp
    )
    target_include_directories(copy_assist_settings_dialog_test PRIVATE src)
    target_link_libraries(copy_assist_settings_dialog_test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
    set_target_properties(copy_assist_settings_dialog_test PROPERTIES AUTOMOC ON)
    add_test(NAME copy_assist_settings_dialog_test COMMAND copy_assist_settings_dialog_test)
    set_tests_properties(copy_assist_settings_dialog_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# Approved V12 cross-needle meter construction: versioned face resource,
# two-pivot mechanics, SWR math/guide registration, RX parking and rendering.
qt_add_resources(CROSS_NEEDLE_METER_TEST_RESOURCES resources/resources.qrc)
add_executable(cross_needle_meter_test
    tests/cross_needle_meter_test.cpp
    src/gui/AnalogMeterFaceTheme.cpp
    src/gui/CrossNeedleMeterGeometry.cpp
    src/gui/CrossNeedleMeterWidget.cpp
    ${CROSS_NEEDLE_METER_TEST_RESOURCES}
)
target_include_directories(cross_needle_meter_test PRIVATE src)
target_link_libraries(cross_needle_meter_test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
)
set_target_properties(cross_needle_meter_test PROPERTIES AUTOMOC ON)
add_test(NAME cross_needle_meter_test COMMAND cross_needle_meter_test)
set_tests_properties(cross_needle_meter_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Versioned standard S-meter face: responsive geometry, calibrated mappings,
# resource validation/fallback, and offscreen rendering of every meter mode.
add_executable(s_meter_geometry_test
    tests/s_meter_geometry_test.cpp
    src/gui/AnalogMeterFaceTheme.cpp
    src/gui/RadioSwrValidityFilter.cpp
    src/gui/SMeterGeometry.cpp
    src/gui/SMeterWidget.cpp
    src/core/ThemeManager.cpp
    src/core/ThemeSeedGenerated.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    ${CROSS_NEEDLE_METER_TEST_RESOURCES}
)
target_include_directories(s_meter_geometry_test PRIVATE src)
target_link_libraries(s_meter_geometry_test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
)
set_target_properties(s_meter_geometry_test PROPERTIES AUTOMOC ON)
add_test(NAME s_meter_geometry_test COMMAND s_meter_geometry_test)
set_tests_properties(s_meter_geometry_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(hgauge_range_test
    tests/hgauge_range_test.cpp
    src/gui/DragValuePopup.cpp
)
target_include_directories(hgauge_range_test PRIVATE src src/gui)
target_link_libraries(hgauge_range_test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
set_target_properties(hgauge_range_test PROPERTIES AUTOMOC ON)
add_test(NAME hgauge_range_test COMMAND hgauge_range_test)
# AETHER_AUTOMATION is NOT set here on purpose — the test qputenv()s it itself,
# before the first HGauge caches the gate, so running the binary directly
# exercises the same assertions ctest does.
set_tests_properties(hgauge_range_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# HGauge hover readout (#3936 follow-up): exactly one badge on screen across a
# meter-to-meter traverse, and it releases itself even if the leaveEvent is
# dropped and the meter keeps updating.
add_executable(hgauge_hover_popup_test
    tests/hgauge_hover_popup_test.cpp
    src/gui/DragValuePopup.cpp
)
target_include_directories(hgauge_hover_popup_test PRIVATE src src/gui)
target_link_libraries(hgauge_hover_popup_test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets)
set_target_properties(hgauge_hover_popup_test PROPERTIES AUTOMOC ON)
add_test(NAME hgauge_hover_popup_test COMMAND hgauge_hover_popup_test)
set_tests_properties(hgauge_hover_popup_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# RangeSlider accessibility announcements (#4565): arrow-key bursts debounce to
# one settled announcement, while discrete handle-focus moves announce
# immediately instead of being swallowed by that debounce.
add_executable(range_slider_a11y_test
    tests/range_slider_a11y_test.cpp
    src/gui/RangeSlider.cpp
)
target_include_directories(range_slider_a11y_test PRIVATE src)
target_link_libraries(range_slider_a11y_test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
)
set_target_properties(range_slider_a11y_test PROPERTIES AUTOMOC ON)
add_test(NAME range_slider_a11y_test COMMAND range_slider_a11y_test)
set_tests_properties(range_slider_a11y_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# RelayBar accessibility announcements (#4565): an ATU sweep debounces to one
# settled position, and the last-published value is forgotten on focus loss so
# a position that moved while unfocused is still announced when it returns.
# HGauge.h is listed so AUTOMOC picks up RelayBar's Q_OBJECT (header-only class).
add_executable(relay_bar_a11y_test
    tests/relay_bar_a11y_test.cpp
    src/gui/DragValuePopup.cpp
    src/gui/HGauge.h
)
target_include_directories(relay_bar_a11y_test PRIVATE src)
target_link_libraries(relay_bar_a11y_test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
)
set_target_properties(relay_bar_a11y_test PROPERTIES AUTOMOC ON)
add_test(NAME relay_bar_a11y_test COMMAND relay_bar_a11y_test)
set_tests_properties(relay_bar_a11y_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# `get rhi` native-widget topology contract (#4339): the native QRhi leaf,
# ancestor-isolation attribute, and native-ancestor count reported to agents.
add_executable(native_widget_topology_test
    tests/native_widget_topology_test.cpp
)
target_include_directories(native_widget_topology_test PRIVATE src)
target_link_libraries(native_widget_topology_test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
)
add_test(NAME native_widget_topology_test COMMAND native_widget_topology_test)
set_tests_properties(native_widget_topology_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# MCP server field-mapping / protocol regression test (#4177). Pure Python,
# no app or Qt needed — guards the schema ↔ bridge verb field mapping.
find_program(PYTHON3_EXECUTABLE NAMES python3 python)
if(PYTHON3_EXECUTABLE)
    add_test(NAME aether_mcp_field_mapping
             COMMAND ${PYTHON3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_aether_mcp.py)
    add_test(NAME automation_probe_field_mapping
             COMMAND ${PYTHON3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_automation_probe.py)
    # Bridge docs must stay in sync with the verb registry (#4174 Phase 3):
    # fail CI if the generated verb table drifts or a detail heading is dup'd.
    add_test(NAME bridge_docs_check
             COMMAND ${PYTHON3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_bridge_docs.py --check)
endif()

# JSON boundary regression for shared bridge `id` normalization. Exercises the
# real QLocalSocket request path and tune dispatcher without touching a radio.
add_executable(automation_json_id_test
    tests/automation_json_id_test.cpp
)
target_include_directories(automation_json_id_test PRIVATE src tests)
target_link_libraries(automation_json_id_test PRIVATE
    aethercore Qt6::Core Qt6::Network
)
add_test(NAME automation_json_id_test COMMAND automation_json_id_test)

add_executable(gui_client_identity_policy_test
    tests/gui_client_identity_policy_test.cpp
)
target_include_directories(gui_client_identity_policy_test PRIVATE src)
target_link_libraries(gui_client_identity_policy_test PRIVATE Qt6::Core)
add_test(NAME gui_client_identity_policy_test COMMAND gui_client_identity_policy_test)

add_executable(gui_client_registration_state_test
    tests/gui_client_registration_state_test.cpp
)
target_include_directories(gui_client_registration_state_test PRIVATE src)
target_link_libraries(gui_client_registration_state_test PRIVATE Qt6::Core)
add_test(NAME gui_client_registration_state_test COMMAND gui_client_registration_state_test)

add_executable(gui_client_registration_recovery_test
    tests/gui_client_registration_recovery_test.cpp
)
target_include_directories(gui_client_registration_recovery_test PRIVATE src tests)
target_link_libraries(gui_client_registration_recovery_test PRIVATE
    aethercore Qt6::Core Qt6::Network Qt6::Test
)
add_test(NAME gui_client_registration_recovery_test
         COMMAND gui_client_registration_recovery_test)

add_executable(profile_load_command_test
    tests/profile_load_command_test.cpp
)
target_include_directories(profile_load_command_test PRIVATE src)
target_link_libraries(profile_load_command_test PRIVATE Qt6::Core)
add_test(NAME profile_load_command_test COMMAND profile_load_command_test)

# #3212 — slice recreate policy (reuse restored pan vs create new; slice freq)
add_executable(slice_recreate_policy_test
    tests/slice_recreate_policy_test.cpp
)
target_include_directories(slice_recreate_policy_test PRIVATE src)
target_link_libraries(slice_recreate_policy_test PRIVATE Qt6::Core)
add_test(NAME slice_recreate_policy_test COMMAND slice_recreate_policy_test)

# #3856 — radio-side display inventory policy (Layer B leak classification)
add_executable(display_inventory_policy_test
    tests/display_inventory_policy_test.cpp
)
target_include_directories(display_inventory_policy_test PRIVATE src)
target_link_libraries(display_inventory_policy_test PRIVATE Qt6::Core)
add_test(NAME display_inventory_policy_test COMMAND display_inventory_policy_test)

# QRZ callsign lookup — spotter stream detection, callsign regex,
# CallsignInfo JSON round-trip, 7-day cache TTL rule, and the QRZ XML
# response parser (QrzClient.cpp needs Qt6::Network for QNetworkReply).
add_executable(qrz_callsign_test
    tests/qrz_callsign_test.cpp
    src/core/CwCallsignSpotter.cpp
    src/core/CallsignInfo.cpp
    src/core/CtyDatParser.cpp
    src/core/QrzClient.cpp
    # LogManager provides lcQrz (spotter's qCDebug category); it drags
    # AsyncLogWriter + AppSettings for its writer/ctor chain at link time.
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(qrz_callsign_test PRIVATE src)
target_link_libraries(qrz_callsign_test PRIVATE Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME qrz_callsign_test COMMAND qrz_callsign_test)

add_executable(shortcut_manager_test
    tests/shortcut_manager_test.cpp
    src/core/ShortcutManager.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(shortcut_manager_test PRIVATE src)
target_link_libraries(shortcut_manager_test PRIVATE Qt6::Core Qt6::Widgets)
add_test(NAME shortcut_manager_test COMMAND shortcut_manager_test)

add_executable(antenna_alias_test
    tests/antenna_alias_test.cpp
    src/models/AntennaAliasStore.cpp
    src/models/SliceModel.cpp
    src/core/DigitalVoiceModeRegistry.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(antenna_alias_test PRIVATE src)
target_link_libraries(antenna_alias_test PRIVATE Qt6::Core)
set_target_properties(antenna_alias_test PROPERTIES AUTOMOC ON)
add_test(NAME antenna_alias_test COMMAND antenna_alias_test)

add_executable(mqtt_antenna_alias_test
    tests/mqtt_antenna_alias_test.cpp
    src/core/MqttAntennaAlias.cpp
)
target_include_directories(mqtt_antenna_alias_test PRIVATE src)
target_link_libraries(mqtt_antenna_alias_test PRIVATE Qt6::Core)
add_test(NAME mqtt_antenna_alias_test COMMAND mqtt_antenna_alias_test)

add_executable(mqtt_settings_test
    tests/mqtt_settings_test.cpp
    src/core/MqttSettings.cpp
    src/core/MqttAntennaAlias.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(mqtt_settings_test PRIVATE src)
target_link_libraries(mqtt_settings_test PRIVATE Qt6::Core)
add_test(NAME mqtt_settings_test COMMAND mqtt_settings_test)

# Pure-math test for the ATU pre-tune center-frequency calculator.
# Locks in the IARU R1 reference table from issue #2624 so future edits
# to computeCenters() can't silently regress the per-band point counts.
add_executable(atu_pretune_centers_test
    tests/atu_pretune_centers_test.cpp
)
target_include_directories(atu_pretune_centers_test PRIVATE src)
target_link_libraries(atu_pretune_centers_test PRIVATE Qt6::Core)
add_test(NAME atu_pretune_centers_test COMMAND atu_pretune_centers_test)

add_executable(cw_sidetone_test
    tests/cw_sidetone_test.cpp
    src/core/CwSidetoneGenerator.cpp
)
target_include_directories(cw_sidetone_test PRIVATE src)
target_link_libraries(cw_sidetone_test PRIVATE Qt6::Core)
add_test(NAME cw_sidetone_test COMMAND cw_sidetone_test)

add_executable(cwx_local_keyer_drift_test
    tests/cwx_local_keyer_drift_test.cpp
    src/core/CwxLocalKeyer.cpp
    src/core/CwxLocalKeyer.h
)
target_include_directories(cwx_local_keyer_drift_test PRIVATE src)
target_link_libraries(cwx_local_keyer_drift_test PRIVATE Qt6::Core)
if(UNIX)
    target_link_libraries(cwx_local_keyer_drift_test PRIVATE pthread)
endif()
add_test(NAME cwx_local_keyer_drift_test COMMAND cwx_local_keyer_drift_test)

add_executable(ax25_frame_formatter_test
    tests/ax25_frame_formatter_test.cpp
    src/core/tnc/Ax25FrameFormatter.cpp
)
target_include_directories(ax25_frame_formatter_test PRIVATE src)
target_link_libraries(ax25_frame_formatter_test PRIVATE Qt6::Core)
add_test(NAME ax25_frame_formatter_test COMMAND ax25_frame_formatter_test)

add_executable(ax25_libmodem_shim_test
    tests/ax25_libmodem_shim_test.cpp
    src/core/tnc/AetherAx25LibmodemShim.cpp
    src/core/tnc/HdlcCodec.cpp
    src/core/tnc/Ax25FrameFormatter.cpp
    src/core/tnc/Ax25.cpp
    src/core/tnc/KissFraming.cpp
    # LogManager.cpp provides lcAx25 (the shim's qCDebug category, #2763);
    # LogManager.cpp depends on AsyncLogWriter via the m_writer member, so
    # AppSettings + AsyncLogWriter come along to satisfy the constructor /
    # destructor chain at link time.
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(ax25_libmodem_shim_test PRIVATE src)
target_link_libraries(ax25_libmodem_shim_test PRIVATE Qt6::Core aether_libmodem_core
    aether_afskdemod)
add_test(NAME ax25_libmodem_shim_test COMMAND ax25_libmodem_shim_test)

add_executable(hdlc_codec_test
    tests/hdlc_codec_test.cpp
    src/core/tnc/HdlcCodec.cpp
)
target_include_directories(hdlc_codec_test PRIVATE src)
target_link_libraries(hdlc_codec_test PRIVATE aether_libmodem_core)
add_test(NAME hdlc_codec_test COMMAND hdlc_codec_test)

# Offline AX.25 decode diagnostic: replays a captured WAV through the decoder.
# Not a ctest (needs an input file); built on demand for troubleshooting.
add_executable(ax25_replay EXCLUDE_FROM_ALL
    tools/ax25_replay.cpp
    src/core/tnc/AetherAx25LibmodemShim.cpp
    src/core/tnc/Ax25FrameFormatter.cpp
    src/core/tnc/KissFraming.cpp
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(ax25_replay PRIVATE src)
target_link_libraries(ax25_replay PRIVATE Qt6::Core aether_libmodem_core aether_afskdemod)

add_executable(ax25_session_analyze EXCLUDE_FROM_ALL
    tools/ax25_session_analyze.cpp
    src/core/tnc/AetherAx25LibmodemShim.cpp
    src/core/tnc/Ax25FrameFormatter.cpp
    src/core/tnc/Ax25.cpp
    src/core/tnc/Ax25Connection.cpp
    src/core/tnc/KissFraming.cpp
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(ax25_session_analyze PRIVATE src)
target_link_libraries(ax25_session_analyze PRIVATE Qt6::Core aether_libmodem_core aether_afskdemod)

# AX.25 airtime model + link-liveness behaviour (T3 idle poll, bounded REJ
# recovery, SABME refusal, Karn-safe RTT sampling). See docs/HFMODEM.md.
add_executable(ax25_link_timing_test
    tests/ax25_link_timing_test.cpp
    src/core/tnc/Ax25.cpp
    src/core/tnc/Ax25Connection.cpp
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(ax25_link_timing_test PRIVATE src tests)
target_link_libraries(ax25_link_timing_test PRIVATE Qt6::Core)
add_test(NAME ax25_link_timing_test COMMAND ax25_link_timing_test)

add_executable(pms_mailbox_test
    tests/pms_mailbox_test.cpp
    src/core/tnc/Ax25.cpp
    src/core/tnc/Ax25Connection.cpp
    src/core/pms/PmsMailbox.cpp
    # Ax25Connection logs link timing / RTT via lcAx25Link; LogManager brings
    # AsyncLogWriter (member) along, same pattern as tnc_terminal_test.
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(pms_mailbox_test PRIVATE src)
target_link_libraries(pms_mailbox_test PRIVATE Qt6::Core)
add_test(NAME pms_mailbox_test COMMAND pms_mailbox_test)

# APRS info-field codec (pure parsing/encoding, no DSP).
add_executable(aprs_packet_test
    tests/aprs_packet_test.cpp
    src/core/aprs/AprsPacket.cpp
    src/core/tnc/Ax25.cpp
)
target_include_directories(aprs_packet_test PRIVATE src)
target_link_libraries(aprs_packet_test PRIVATE Qt6::Core)
add_test(NAME aprs_packet_test COMMAND aprs_packet_test)

# APRS messaging engine + station roster. LogManager.cpp provides lcAx25
# (the qCWarning category used by the persistence paths); it drags in
# AsyncLogWriter + AppSettings, same as ax25_libmodem_shim_test.
add_executable(aprs_messenger_test
    tests/aprs_messenger_test.cpp
    src/core/aprs/AprsPacket.cpp
    src/core/aprs/AprsMessenger.cpp
    src/core/aprs/AprsStationList.cpp
    src/core/tnc/Ax25.cpp
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(aprs_messenger_test PRIVATE src)
target_link_libraries(aprs_messenger_test PRIVATE Qt6::Core)
add_test(NAME aprs_messenger_test COMMAND aprs_messenger_test)

add_executable(tnc_terminal_test
    tests/tnc_terminal_test.cpp
    src/core/tnc/Ax25.cpp
    src/core/tnc/Ax25Connection.cpp
    src/core/tnc/HeardList.cpp
    src/core/tnc/TncTerminal.cpp
    # HeardList now emits qCWarning(lcAx25) on persistence failure; pull in
    # LogManager + its deps so the category symbol resolves.
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(tnc_terminal_test PRIVATE src)
target_link_libraries(tnc_terminal_test PRIVATE Qt6::Core)
add_test(NAME tnc_terminal_test COMMAND tnc_terminal_test)

add_executable(cwx_speed_modifier_test
    tests/cwx_speed_modifier_test.cpp
    src/models/CwxModel.cpp
    src/models/CwxModel.h
    # CwxModel now logs via lcCw (qCWarning) — pull in the logging category.
    # LogManager depends on AsyncLogWriter (member) + AppSettings (retention
    # config), so both come along to satisfy the link. (#3949)
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(cwx_speed_modifier_test PRIVATE src)
target_link_libraries(cwx_speed_modifier_test PRIVATE Qt6::Core)
add_test(NAME cwx_speed_modifier_test COMMAND cwx_speed_modifier_test)

# Queue-drain watch state machine: epoch guard, live-char arming, and the
# CwxModel-side invariant the RadioModel flicker-immune release latch relies on. (#3949)
add_executable(cwx_drain_watch_test
    tests/cwx_drain_watch_test.cpp
    src/models/CwxModel.cpp
    src/models/CwxModel.h
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(cwx_drain_watch_test PRIVATE src)
target_link_libraries(cwx_drain_watch_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME cwx_drain_watch_test COMMAND cwx_drain_watch_test)

add_executable(cwx_panel_test
    tests/cwx_panel_test.cpp
    src/gui/CwxPanel.cpp
    src/gui/CwxPanel.h
    src/models/CwxModel.cpp
    src/models/CwxModel.h
    # CwxPanel.cpp calls ThemeManager::resolve() post-Phase-2 migration;
    # pull in the manager + its logging deps so the test links.
    src/core/ThemeManager.cpp
    src/core/ThemeSeedGenerated.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
)
target_include_directories(cwx_panel_test PRIVATE src)
target_link_libraries(cwx_panel_test PRIVATE
    Qt6::Core Qt6::Widgets
)

add_executable(meter_model_test
    tests/meter_model_test.cpp
    src/models/MeterModel.cpp
    src/core/AsyncLogWriter.cpp
    src/core/LogManager.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(meter_model_test PRIVATE src)
target_link_libraries(meter_model_test PRIVATE Qt6::Core)
if(UNIX)
    target_link_libraries(meter_model_test PRIVATE pthread)
endif()
set_target_properties(meter_model_test PROPERTIES AUTOMOC ON)
add_test(NAME meter_model_test COMMAND meter_model_test)

add_executable(health_applet_test
    tests/health_applet_test.cpp
    src/gui/HealthApplet.cpp
    src/models/MeterModel.cpp
    src/core/ThemeManager.cpp
    src/core/ThemeSeedGenerated.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
)
target_include_directories(health_applet_test PRIVATE src)
target_link_libraries(health_applet_test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test
)
if(UNIX)
    target_link_libraries(health_applet_test PRIVATE pthread)
endif()
set_target_properties(health_applet_test PROPERTIES AUTOMOC ON)
add_test(NAME health_applet_test COMMAND health_applet_test)
set_tests_properties(health_applet_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Demo-mode SimBackend lifecycle test (RFC #4288, Phase 1). SimBackend was once
# wire-free, but Path B (RFC #4288) had it own a RadioConnection + PanadapterStream,
# so it no longer links against a hand-picked subset of sources: those pull in
# RadioDiscovery.h (<QUdpSocket>) at compile time and RadioConnection/PanadapterStream
# symbols at link time. Link aethercore (which already contains SimBackend, NoiseMixer,
# RadioConnection and PanadapterStream) exactly like flex_backend_lifecycle_test does,
# so the dependency closure is resolved by the library rather than re-listed here.
add_executable(sim_backend_test tests/sim_backend_test.cpp)
target_include_directories(sim_backend_test PRIVATE src)
target_link_libraries(sim_backend_test PRIVATE aethercore Qt6::Core Qt6::Test)
if(UNIX)
    target_link_libraries(sim_backend_test PRIVATE pthread)
endif()
set_target_properties(sim_backend_test PROPERTIES AUTOMOC ON)
add_test(NAME sim_backend_test COMMAND sim_backend_test)

# Demo-mode signal engine — pure pattern generator (RFC #4288, Phase 2a).
add_executable(spectrum_pattern_test
    tests/spectrum_pattern_test.cpp
    src/core/backends/sim/SpectrumPatternGenerator.cpp
)
target_include_directories(spectrum_pattern_test PRIVATE src)
target_link_libraries(spectrum_pattern_test PRIVATE Qt6::Core)
if(UNIX)
    target_link_libraries(spectrum_pattern_test PRIVATE pthread)
endif()
add_test(NAME spectrum_pattern_test COMMAND spectrum_pattern_test)

add_executable(noise_mixer_test
    tests/noise_mixer_test.cpp
    src/core/backends/sim/NoiseMixer.cpp
    resources/resources.qrc          # for the bundled demo voice clip (:/demo_voice.wav)
)
target_include_directories(noise_mixer_test PRIVATE src)
target_link_libraries(noise_mixer_test PRIVATE Qt6::Core)
if(UNIX)
    target_link_libraries(noise_mixer_test PRIVATE pthread)
endif()
add_test(NAME noise_mixer_test COMMAND noise_mixer_test)

add_executable(vu_meter_settings_test
    tests/vu_meter_settings_test.cpp
    src/gui/CrossNeedleMeterSettings.cpp
    src/gui/VuMeterSettings.cpp
)
target_include_directories(vu_meter_settings_test PRIVATE src)
target_link_libraries(vu_meter_settings_test PRIVATE Qt6::Core)
add_test(NAME vu_meter_settings_test COMMAND vu_meter_settings_test)

# aetherd RFC step 2.2b regression guard: FlexBackend ctor/dtor thread ownership
# + #502 teardown ordering. FlexBackend pulls RadioConnection/PanadapterStream
# and their deep deps, so link the engine library rather than list sources.
add_executable(flex_backend_lifecycle_test tests/flex_backend_lifecycle_test.cpp)
target_include_directories(flex_backend_lifecycle_test PRIVATE src)
target_link_libraries(flex_backend_lifecycle_test PRIVATE aethercore Qt6::Core)
set_target_properties(flex_backend_lifecycle_test PROPERTIES AUTOMOC ON)
add_test(NAME flex_backend_lifecycle_test COMMAND flex_backend_lifecycle_test)

# A pending client dBm request must retain a mismatching radio range for timeout
# reconciliation and yield before a radio-authoritative band-stack restore.
add_executable(panadapter_dbm_range_test tests/panadapter_dbm_range_test.cpp)
target_include_directories(panadapter_dbm_range_test PRIVATE src)
target_link_libraries(panadapter_dbm_range_test PRIVATE aethercore Qt6::Core)
set_target_properties(panadapter_dbm_range_test PROPERTIES AUTOMOC ON)
add_test(NAME panadapter_dbm_range_test COMMAND panadapter_dbm_range_test)

# The bridge preserves dragAt, target-tune, memory-recall, and authenticated
# positional requests across its bare and JSON protocol forms.
add_executable(automation_drag_at_test tests/automation_drag_at_test.cpp)
target_include_directories(automation_drag_at_test PRIVATE src)
target_link_libraries(automation_drag_at_test PRIVATE
    aethercore Qt6::Core Qt6::Network Qt6::Widgets
)
set_target_properties(automation_drag_at_test PROPERTIES AUTOMOC ON)
add_test(NAME automation_drag_at_test COMMAND automation_drag_at_test)
set_tests_properties(automation_drag_at_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# aetherd RFC 2.3 template — pan decode/normalize chain (incl. wnb_level guard).
add_executable(aetherd_pan_decode_test tests/aetherd_pan_decode_test.cpp)
target_include_directories(aetherd_pan_decode_test PRIVATE src)
target_link_libraries(aetherd_pan_decode_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(aetherd_pan_decode_test PROPERTIES AUTOMOC ON)
add_test(NAME aetherd_pan_decode_test COMMAND aetherd_pan_decode_test)

if(Qt6WebSockets_FOUND)
    add_executable(tci_protocol_test tests/tci_protocol_test.cpp)
    target_include_directories(tci_protocol_test PRIVATE src)
    target_link_libraries(tci_protocol_test PRIVATE aethercore Qt6::Core)
    add_test(NAME tci_protocol_test COMMAND tci_protocol_test)

    add_executable(tci_trxmap_test tests/tci_trxmap_test.cpp)
    target_include_directories(tci_trxmap_test PRIVATE src)
    target_link_libraries(tci_trxmap_test PRIVATE aethercore Qt6::Core)
    add_test(NAME tci_trxmap_test COMMAND tci_trxmap_test)

    add_executable(tci_automation_test tests/tci_automation_test.cpp)
    target_include_directories(tci_automation_test PRIVATE src tests)
    target_link_libraries(tci_automation_test PRIVATE
        aethercore Qt6::Core Qt6::Network Qt6::WebSockets
    )
    add_test(NAME tci_automation_test COMMAND tci_automation_test)

    add_executable(tci_server_review_test tests/tci_server_review_test.cpp)
    target_include_directories(tci_server_review_test PRIVATE src tests)
    target_link_libraries(tci_server_review_test PRIVATE
        aethercore Qt6::Core Qt6::Network Qt6::WebSockets
    )
    add_test(NAME tci_server_review_test COMMAND tci_server_review_test)
endif()

# aetherd RFC 2.3 — MeterModel touchpoint: meter-status wire decode.
add_executable(hl2_txdsp_test tests/hl2_txdsp_test.cpp)
target_include_directories(hl2_txdsp_test PRIVATE src)
target_link_libraries(hl2_txdsp_test PRIVATE aethercore Qt6::Core)
add_test(NAME hl2_txdsp_test COMMAND hl2_txdsp_test)

# radiocert's measurement primitives. Header-only by design so this needs no
# Qt and no link against aethercore — see the test's header comment for why it
# exists at all (both shipped bugs in the diagnostic were in this arithmetic).
add_executable(radio_certification_math_test tests/radio_certification_math_test.cpp)
target_include_directories(radio_certification_math_test PRIVATE src)
add_test(NAME radio_certification_math_test COMMAND radio_certification_math_test)

add_executable(hl2_tx_loopback_test tests/hl2_tx_loopback_test.cpp)
target_include_directories(hl2_tx_loopback_test PRIVATE src)
target_link_libraries(hl2_tx_loopback_test PRIVATE aethercore Qt6::Core Qt6::Network)
add_test(NAME hl2_tx_loopback_test COMMAND hl2_tx_loopback_test)
# This test is a no-op without an hpsdrsim answering discovery, which is the
# normal state of every machine that has not deliberately started one. Without
# this property that no-op reports as Passed, which is indistinguishable from a
# run that actually keyed and measured — the exact silent-green this test was
# fixed to stop. Same convention as crdv_quarantined_test above.
set_tests_properties(hl2_tx_loopback_test PROPERTIES SKIP_RETURN_CODE 77)

add_executable(hl2_tx_gate_test tests/hl2_tx_gate_test.cpp)
target_include_directories(hl2_tx_gate_test PRIVATE src)
target_link_libraries(hl2_tx_gate_test PRIVATE aethercore Qt6::Core Qt6::Network)
add_test(NAME hl2_tx_gate_test COMMAND hl2_tx_gate_test)

add_executable(hl2_dbref_test tests/hl2_dbref_test.cpp)
target_include_directories(hl2_dbref_test PRIVATE src)
add_test(NAME hl2_dbref_test COMMAND hl2_dbref_test)

add_executable(radiomodel_dax_null_test tests/radiomodel_dax_null_test.cpp)
target_include_directories(radiomodel_dax_null_test PRIVATE src)
target_link_libraries(radiomodel_dax_null_test PRIVATE aethercore Qt6::Core Qt6::Test)
add_test(NAME radiomodel_dax_null_test COMMAND radiomodel_dax_null_test)

add_executable(dbm_range_plausibility_test tests/dbm_range_plausibility_test.cpp)
target_include_directories(dbm_range_plausibility_test PRIVATE src)
target_link_libraries(dbm_range_plausibility_test PRIVATE Qt6::Core)
add_test(NAME dbm_range_plausibility_test COMMAND dbm_range_plausibility_test)

add_executable(radiomodel_pan_range_null_test tests/radiomodel_pan_range_null_test.cpp)
target_include_directories(radiomodel_pan_range_null_test PRIVATE src)
target_link_libraries(radiomodel_pan_range_null_test PRIVATE aethercore Qt6::Core Qt6::Test)
add_test(NAME radiomodel_pan_range_null_test COMMAND radiomodel_pan_range_null_test)

add_executable(radiomodel_pan_id_mapping_test tests/radiomodel_pan_id_mapping_test.cpp)
target_include_directories(radiomodel_pan_id_mapping_test PRIVATE src)
target_link_libraries(radiomodel_pan_id_mapping_test PRIVATE aethercore Qt6::Core Qt6::Test)
add_test(NAME radiomodel_pan_id_mapping_test COMMAND radiomodel_pan_id_mapping_test)

# RadioModel audio-mute model-state contract (#4771).
add_executable(radiomodel_audio_mute_test tests/radiomodel_audio_mute_test.cpp)
target_include_directories(radiomodel_audio_mute_test PRIVATE src)
target_link_libraries(radiomodel_audio_mute_test PRIVATE aethercore Qt6::Core Qt6::Test)
add_test(NAME radiomodel_audio_mute_test COMMAND radiomodel_audio_mute_test)

add_executable(demo_backend_swap_test tests/demo_backend_swap_test.cpp)
target_include_directories(demo_backend_swap_test PRIVATE src)
target_link_libraries(demo_backend_swap_test PRIVATE aethercore Qt6::Core Qt6::Test)
add_test(NAME demo_backend_swap_test COMMAND demo_backend_swap_test)

add_executable(demo_applet_tooltip_test
    tests/demo_applet_tooltip_test.cpp
    src/gui/DemoApplet.cpp
)
target_include_directories(demo_applet_tooltip_test PRIVATE src)
target_link_libraries(demo_applet_tooltip_test PRIVATE aethercore Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
add_test(NAME demo_applet_tooltip_test COMMAND demo_applet_tooltip_test)
set_tests_properties(demo_applet_tooltip_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# IcomCIV backend selection — proves family="icom" reaches IcomCivBackend
# through RadioModel's real swap path. Without this, every layer below can be
# green while nothing in the application can construct it.
# IcomCIV settings + credentials. The load-bearing check is the SECURITY one:
# the Icom network password must never reach the settings database. Own process
# because AppSettings is a process-wide singleton.
add_executable(icom_settings_test tests/icom_settings_test.cpp)
target_include_directories(icom_settings_test PRIVATE src tests)
target_link_libraries(icom_settings_test PRIVATE aethercore Qt6::Core Qt6::Test)
add_test(NAME icom_settings_test COMMAND icom_settings_test)

add_executable(icom_family_test tests/icom_family_test.cpp)
target_include_directories(icom_family_test PRIVATE src)
target_link_libraries(icom_family_test PRIVATE aethercore Qt6::Core Qt6::Test)
add_test(NAME icom_family_test COMMAND icom_family_test)

add_executable(hl2_family_transition_test tests/hl2_family_transition_test.cpp)
target_include_directories(hl2_family_transition_test PRIVATE src)
target_link_libraries(hl2_family_transition_test PRIVATE aethercore Qt6::Core Qt6::Test)
add_test(NAME hl2_family_transition_test COMMAND hl2_family_transition_test)

# Capability-gated UI surfaces (hasProfiles / hasDaxStreams / hasExtendedDsp):
# every backend declares each flag explicitly, the RadioModel relay fires on
# both connection edges, and the `!connected || caps.hasX` rule the GUI applies
# restores the permissive value on disconnect.
add_executable(radio_capability_gating_test tests/radio_capability_gating_test.cpp)
target_include_directories(radio_capability_gating_test PRIVATE src tests)
target_link_libraries(radio_capability_gating_test PRIVATE aethercore Qt6::Core Qt6::Test)
add_test(NAME radio_capability_gating_test COMMAND radio_capability_gating_test)

# RadioStateMemory + the radio-scoped feature-document store (RFC #4603 PR 2):
# capability-shaped engagement (empty domains ⇒ inert), per-domain gating on
# load AND store, per-radio isolation, family-wide fallback, schema tolerance.
add_executable(radio_state_memory_test tests/radio_state_memory_test.cpp)
target_include_directories(radio_state_memory_test PRIVATE src tests)
target_link_libraries(radio_state_memory_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(radio_state_memory_test PROPERTIES AUTOMOC ON)
add_test(NAME radio_state_memory_test COMMAND radio_state_memory_test)

# HL2 state restore/capture (RFC #4603 PR 3): band-key table, the
# applyRestoredState validation boundary, restored-rate/LNA connect seeding,
# param precedence, and the capture snapshot round-trip. Hardware-path
# validation happens on nigelfenton's bench.
add_executable(hl2_state_restore_test tests/hl2_state_restore_test.cpp)
target_include_directories(hl2_state_restore_test PRIVATE src tests)
target_link_libraries(hl2_state_restore_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(hl2_state_restore_test PROPERTIES AUTOMOC ON)
add_test(NAME hl2_state_restore_test COMMAND hl2_state_restore_test)

# BandStack fold-in (RFC #4603 PR 4): per-radio feature documents,
# write-through mutations, lazy per-radio legacy import with side-file
# retirement, panel prefs in AppSettings.
add_executable(bandstack_scoped_test tests/bandstack_scoped_test.cpp)
target_include_directories(bandstack_scoped_test PRIVATE src tests)
target_link_libraries(bandstack_scoped_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(bandstack_scoped_test PROPERTIES AUTOMOC ON)
add_test(NAME bandstack_scoped_test COMMAND bandstack_scoped_test)

# TCI signaling on a backend with neither a Flex command plane nor a DAX data
# plane (HL2) — the seam WSJT-X rides. Links WebSockets so the TciServer half of
# the file compiles wherever HAVE_WEBSOCKETS is set for aethercore.
add_executable(hl2_tci_signaling_test tests/hl2_tci_signaling_test.cpp)
target_include_directories(hl2_tci_signaling_test PRIVATE src tests)
target_link_libraries(hl2_tci_signaling_test PRIVATE
    aethercore Qt6::Core Qt6::Network Qt6::Test
)
if(Qt6WebSockets_FOUND)
    target_link_libraries(hl2_tci_signaling_test PRIVATE Qt6::WebSockets)
endif()
set_target_properties(hl2_tci_signaling_test PROPERTIES AUTOMOC ON)
add_test(NAME hl2_tci_signaling_test COMMAND hl2_tci_signaling_test)

add_executable(aetherd_meter_decode_test tests/aetherd_meter_decode_test.cpp)
target_include_directories(aetherd_meter_decode_test PRIVATE src)
target_link_libraries(aetherd_meter_decode_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(aetherd_meter_decode_test PROPERTIES AUTOMOC ON)
add_test(NAME aetherd_meter_decode_test COMMAND aetherd_meter_decode_test)

# aetherd RFC 2.3 — SliceModel touchpoint: slice-status wire → canonical decode.
add_executable(aetherd_slice_decode_test tests/aetherd_slice_decode_test.cpp)
target_include_directories(aetherd_slice_decode_test PRIVATE src)
target_link_libraries(aetherd_slice_decode_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(aetherd_slice_decode_test PROPERTIES AUTOMOC ON)
add_test(NAME aetherd_slice_decode_test COMMAND aetherd_slice_decode_test)

# aetherd RFC 2.3 — TransmitModel touchpoint: transmit-family wire → typed delta.
add_executable(aetherd_transmit_decode_test tests/aetherd_transmit_decode_test.cpp)
target_include_directories(aetherd_transmit_decode_test PRIVATE src)
target_link_libraries(aetherd_transmit_decode_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(aetherd_transmit_decode_test PROPERTIES AUTOMOC ON)
add_test(NAME aetherd_transmit_decode_test COMMAND aetherd_transmit_decode_test)

# aetherd RFC 2.3 (RadioModel residual): radio-global wire → typed delta.
add_executable(aetherd_radio_decode_test tests/aetherd_radio_decode_test.cpp)
target_include_directories(aetherd_radio_decode_test PRIVATE src)
target_link_libraries(aetherd_radio_decode_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(aetherd_radio_decode_test PROPERTIES AUTOMOC ON)
add_test(NAME aetherd_radio_decode_test COMMAND aetherd_radio_decode_test)

add_executable(aetherd_residual_decode_test tests/aetherd_residual_decode_test.cpp)
target_include_directories(aetherd_residual_decode_test PRIVATE src)
target_link_libraries(aetherd_residual_decode_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(aetherd_residual_decode_test PROPERTIES AUTOMOC ON)
add_test(NAME aetherd_residual_decode_test COMMAND aetherd_residual_decode_test)

add_executable(location_address_resolver_test
    tests/location_address_resolver_test.cpp
)
target_include_directories(location_address_resolver_test PRIVATE src)
target_link_libraries(location_address_resolver_test PRIVATE
    aethercore Qt6::Core Qt6::Network
)
set_target_properties(location_address_resolver_test PROPERTIES AUTOMOC ON)
add_test(NAME location_address_resolver_test COMMAND location_address_resolver_test)

add_executable(display_presence_test tests/display_presence_test.cpp)
target_include_directories(display_presence_test PRIVATE src)
target_link_libraries(display_presence_test PRIVATE Qt6::Core)
add_test(NAME display_presence_test COMMAND display_presence_test)

add_executable(amp_model_test tests/amp_model_test.cpp)
target_include_directories(amp_model_test PRIVATE src)
target_link_libraries(amp_model_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(amp_model_test PROPERTIES AUTOMOC ON)
add_test(NAME amp_model_test COMMAND amp_model_test)

add_executable(wwv_decoder_test tests/wwv_decoder_test.cpp)
target_include_directories(wwv_decoder_test PRIVATE src)
target_link_libraries(wwv_decoder_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(wwv_decoder_test PROPERTIES AUTOMOC ON)
add_test(NAME wwv_decoder_test COMMAND wwv_decoder_test)

add_executable(wwvb_decoder_test tests/wwvb_decoder_test.cpp)
target_include_directories(wwvb_decoder_test PRIVATE src)
target_link_libraries(wwvb_decoder_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(wwvb_decoder_test PROPERTIES AUTOMOC ON)
add_test(NAME wwvb_decoder_test COMMAND wwvb_decoder_test)

add_executable(aetherclock_engine_test tests/aetherclock_engine_test.cpp)
target_include_directories(aetherclock_engine_test PRIVATE src)
target_link_libraries(aetherclock_engine_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(aetherclock_engine_test PROPERTIES AUTOMOC ON)
add_test(NAME aetherclock_engine_test COMMAND aetherclock_engine_test)

add_executable(wspr_beacon_test tests/wspr_beacon_test.cpp)
target_include_directories(wspr_beacon_test PRIVATE src)
target_link_libraries(wspr_beacon_test PRIVATE aethercore Qt6::Core)
add_test(NAME wspr_beacon_test COMMAND wspr_beacon_test)

add_executable(automation_tx_watchdog_test
    tests/automation_tx_watchdog_test.cpp
)
target_include_directories(automation_tx_watchdog_test PRIVATE src)
target_link_libraries(automation_tx_watchdog_test PRIVATE
    aethercore Qt6::Core Qt6::Network Qt6::Widgets
)
add_test(NAME automation_tx_watchdog_test COMMAND automation_tx_watchdog_test)

add_executable(aetherclock_model_test tests/aetherclock_model_test.cpp)
target_include_directories(aetherclock_model_test PRIVATE src)
target_link_libraries(aetherclock_model_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(aetherclock_model_test PROPERTIES AUTOMOC ON)
add_test(NAME aetherclock_model_test COMMAND aetherclock_model_test)

add_executable(aetherd_amp_decode_test tests/aetherd_amp_decode_test.cpp)
target_include_directories(aetherd_amp_decode_test PRIVATE src)
target_link_libraries(aetherd_amp_decode_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(aetherd_amp_decode_test PROPERTIES AUTOMOC ON)
add_test(NAME aetherd_amp_decode_test COMMAND aetherd_amp_decode_test)

add_executable(tuner_model_test tests/tuner_model_test.cpp)
target_include_directories(tuner_model_test PRIVATE src)
target_link_libraries(tuner_model_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(tuner_model_test PROPERTIES AUTOMOC ON)
add_test(NAME tuner_model_test COMMAND tuner_model_test)

add_executable(aetherd_tuner_decode_test tests/aetherd_tuner_decode_test.cpp)
target_include_directories(aetherd_tuner_decode_test PRIVATE src)
target_link_libraries(aetherd_tuner_decode_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(aetherd_tuner_decode_test PROPERTIES AUTOMOC ON)
add_test(NAME aetherd_tuner_decode_test COMMAND aetherd_tuner_decode_test)

add_executable(aetherd_amp_tuner_encode_test tests/aetherd_amp_tuner_encode_test.cpp)
target_include_directories(aetherd_amp_tuner_encode_test PRIVATE src)
target_link_libraries(aetherd_amp_tuner_encode_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(aetherd_amp_tuner_encode_test PROPERTIES AUTOMOC ON)
add_test(NAME aetherd_amp_tuner_encode_test COMMAND aetherd_amp_tuner_encode_test)

add_executable(usb_cable_model_test
    tests/usb_cable_model_test.cpp
    src/models/UsbCableModel.cpp
)
target_include_directories(usb_cable_model_test PRIVATE src)
target_link_libraries(usb_cable_model_test PRIVATE Qt6::Core Qt6::Test)
set_target_properties(usb_cable_model_test PROPERTIES AUTOMOC ON)
add_test(NAME usb_cable_model_test COMMAND usb_cable_model_test)

add_executable(async_log_writer_test
    tests/async_log_writer_test.cpp
    src/core/AsyncLogWriter.cpp
)
target_include_directories(async_log_writer_test PRIVATE src)
target_link_libraries(async_log_writer_test PRIVATE Qt6::Core)
if(UNIX)
    target_link_libraries(async_log_writer_test PRIVATE pthread)
endif()
set_target_properties(async_log_writer_test PROPERTIES AUTOMOC ON)

# Support & Diagnostics category toggle must enable Info alongside Debug
# (#4419): most categories declare a QtWarningMsg threshold, and the filter
# rules previously re-enabled .debug only, leaving every qCInfo on those
# categories unreachable.
add_executable(log_manager_filter_rules_test
    tests/log_manager_filter_rules_test.cpp
    src/core/LogManager.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/AsyncLogWriter.cpp
)
target_include_directories(log_manager_filter_rules_test PRIVATE src)
target_link_libraries(log_manager_filter_rules_test PRIVATE Qt6::Core)
if(UNIX)
    target_link_libraries(log_manager_filter_rules_test PRIVATE pthread)
endif()
set_target_properties(log_manager_filter_rules_test PROPERTIES AUTOMOC ON)
add_test(NAME log_manager_filter_rules_test COMMAND log_manager_filter_rules_test)

# Pre-filled GitHub issue body + redaction-at-render guarantee (#3705).
# IssueReport.cpp depends only on redactPii (AsyncLogWriter.cpp) — no
# RadioModel — so the redaction contract is unit-testable in isolation.
add_executable(issue_report_test
    tests/issue_report_test.cpp
    src/core/IssueReport.cpp
    src/core/AsyncLogWriter.cpp
)
target_include_directories(issue_report_test PRIVATE src src/core)
target_link_libraries(issue_report_test PRIVATE Qt6::Core)
if(UNIX)
    target_link_libraries(issue_report_test PRIVATE pthread)
endif()
set_target_properties(issue_report_test PROPERTIES AUTOMOC ON)

add_executable(perf_telemetry_test
    tests/perf_telemetry_test.cpp
    src/core/PerfTelemetry.cpp
    # LogManager.cpp owns the lcPerf Q_LOGGING_CATEGORY definition (per
    # the Q_LOGGING_CATEGORY consolidation in #2770); LogManager has AsyncLogWriter
    # as a member which transitively needs AppSettings, so all three .cpp
    # files come along to satisfy the ctor/dtor chain at link time.
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(perf_telemetry_test PRIVATE src)
target_link_libraries(perf_telemetry_test PRIVATE Qt6::Core)
if(UNIX)
    target_link_libraries(perf_telemetry_test PRIVATE pthread)
endif()
set_target_properties(perf_telemetry_test PROPERTIES AUTOMOC ON)
add_test(NAME perf_telemetry_test COMMAND perf_telemetry_test)

add_executable(memory_telemetry_test
    tests/memory_telemetry_test.cpp
    src/core/MemoryTelemetry.cpp
)
target_include_directories(memory_telemetry_test PRIVATE src)
target_link_libraries(memory_telemetry_test PRIVATE Qt6::Core)
add_test(NAME memory_telemetry_test COMMAND memory_telemetry_test)

add_executable(memory_recall_policy_test
    tests/memory_recall_policy_test.cpp
    src/core/MemoryRecallPolicy.cpp
)
target_include_directories(memory_recall_policy_test PRIVATE src)
target_link_libraries(memory_recall_policy_test PRIVATE Qt6::Core)
add_test(NAME memory_recall_policy_test COMMAND memory_recall_policy_test)

add_executable(net_recurrence_test
    tests/net_recurrence_test.cpp
    src/core/NetRecurrence.cpp
)
target_include_directories(net_recurrence_test PRIVATE src)
target_link_libraries(net_recurrence_test PRIVATE Qt6::Core)
add_test(NAME net_recurrence_test COMMAND net_recurrence_test)

add_executable(net_schedule_store_test
    tests/net_schedule_store_test.cpp
    src/core/NetScheduleStore.cpp
)
target_include_directories(net_schedule_store_test PRIVATE src)
target_link_libraries(net_schedule_store_test PRIVATE Qt6::Core)
add_test(NAME net_schedule_store_test COMMAND net_schedule_store_test)

add_executable(net_schedule_planner_test
    tests/net_schedule_planner_test.cpp
    src/core/NetSchedulePlanner.cpp
    src/core/NetRecurrence.cpp
)
target_include_directories(net_schedule_planner_test PRIVATE src)
target_link_libraries(net_schedule_planner_test PRIVATE Qt6::Core)
add_test(NAME net_schedule_planner_test COMMAND net_schedule_planner_test)

add_executable(memory_field_values_test
    tests/memory_field_values_test.cpp
    src/core/MemoryFieldValues.cpp
)
target_include_directories(memory_field_values_test PRIVATE src)
target_link_libraries(memory_field_values_test PRIVATE Qt6::Core)
add_test(NAME memory_field_values_test COMMAND memory_field_values_test)

add_executable(local_memory_store_test
    tests/local_memory_store_test.cpp
    src/core/LocalMemoryStore.cpp
)
target_include_directories(local_memory_store_test PRIVATE src)
target_link_libraries(local_memory_store_test PRIVATE Qt6::Core)
add_test(NAME local_memory_store_test COMMAND local_memory_store_test)

add_executable(local_memory_bank_test
    tests/local_memory_bank_test.cpp
    src/core/LocalMemoryBank.cpp
    src/core/LocalMemoryStore.cpp
    src/core/backends/MemoryWireCodec.cpp
    src/core/AsyncLogWriter.cpp
    src/core/LogManager.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(local_memory_bank_test PRIVATE src)
target_link_libraries(local_memory_bank_test PRIVATE Qt6::Core)
add_test(NAME local_memory_bank_test COMMAND local_memory_bank_test)

add_executable(memory_csv_compat_test
    tests/memory_csv_compat_test.cpp
    src/core/MemoryCsvCompat.cpp
    src/core/MemoryFieldValues.cpp
)
target_include_directories(memory_csv_compat_test PRIVATE src)
target_compile_definitions(memory_csv_compat_test PRIVATE
    CHIRP_SAMPLE_CSV="${CMAKE_CURRENT_SOURCE_DIR}/docs/automation/sample-chirp-memories.csv")
target_link_libraries(memory_csv_compat_test PRIVATE Qt6::Core)
add_test(NAME memory_csv_compat_test COMMAND memory_csv_compat_test)

add_executable(transmit_model_apd_test
    tests/transmit_model_apd_test.cpp
    src/models/TransmitModel.cpp
    src/core/ClientQuindarTone.cpp
    src/core/AsyncLogWriter.cpp
    src/core/LogManager.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(transmit_model_apd_test PRIVATE src)
target_link_libraries(transmit_model_apd_test PRIVATE Qt6::Core Qt6::Test)
if(UNIX)
    target_link_libraries(transmit_model_apd_test PRIVATE pthread)
endif()
set_target_properties(transmit_model_apd_test PROPERTIES AUTOMOC ON)
add_test(NAME transmit_model_apd_test COMMAND transmit_model_apd_test)

# Help guide search tests - needs QApplication + Widgets.
add_executable(help_dialog_test
    tests/help_dialog_test.cpp
    src/gui/HelpDialog.cpp
    src/gui/PersistentDialog.cpp
    src/gui/FramelessResizer.cpp
    src/gui/FramelessWindowTitleBar.cpp
    # HelpDialog.cpp calls ThemeManager::resolve() post-Phase-2 migration;
    # pull in the manager + its logging deps so the test links.
    src/core/ThemeManager.cpp
    src/core/ThemeSeedGenerated.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
)
target_include_directories(help_dialog_test PRIVATE src)
target_link_libraries(help_dialog_test PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Test
)
set_target_properties(help_dialog_test PROPERTIES AUTOMOC ON)

# FreeDV Reporter status-message row (#4231). Guarded like the dialog
# itself — FreeDvReporterDialog only exists when WebSockets are available.
# Dialog-side only: the test never reaches qso.freedv.org.
if(Qt6WebSockets_FOUND)
    # src/gui/ sources belong to the AetherSDR executable rather than
    # aethercore, so the dialog's GUI dependencies are listed explicitly;
    # SliceModel/ThemeManager/settings come in via aethercore.
    add_executable(freedv_reporter_message_test
        tests/freedv_reporter_message_test.cpp
        src/gui/FreeDvReporterDialog.cpp
        src/gui/FreeDvReporterModel.cpp
        src/gui/PersistentDialog.cpp
        src/gui/FramelessResizer.cpp
        src/gui/FramelessWindowTitleBar.cpp
    )
    target_include_directories(freedv_reporter_message_test PRIVATE src)
    target_link_libraries(freedv_reporter_message_test PRIVATE
        aethercore Qt6::Core Qt6::Widgets Qt6::Test
    )
    set_target_properties(freedv_reporter_message_test PROPERTIES AUTOMOC ON)
    add_test(NAME freedv_reporter_message_test
             COMMAND freedv_reporter_message_test)
    set_tests_properties(freedv_reporter_message_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# Regression guard for #3662: the AetherControl window must never demand a
# minimum height taller than the screen (auto-engages compact when it would).
add_executable(flex_control_dialog_size_test
    tests/flex_control_dialog_size_test.cpp
    src/gui/FlexControlDialog.cpp
    src/gui/PersistentDialog.cpp
    src/gui/FramelessResizer.cpp
    src/gui/FramelessWindowTitleBar.cpp
    src/gui/SliceLabel.cpp
    src/gui/DragValuePopup.cpp
    src/models/SliceModel.cpp
    src/core/DigitalVoiceModeRegistry.cpp
    src/core/KiwiSdrProtocol.cpp
    src/core/ThemeManager.cpp
    src/core/ThemeSeedGenerated.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
)
target_include_directories(flex_control_dialog_size_test PRIVATE src)
target_link_libraries(flex_control_dialog_size_test PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Test
)
set_target_properties(flex_control_dialog_size_test PROPERTIES AUTOMOC ON)
add_test(NAME flex_control_dialog_size_test COMMAND flex_control_dialog_size_test)
set_tests_properties(flex_control_dialog_size_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(connection_panel_size_test
    tests/connection_panel_size_test.cpp
    src/gui/ConnectionPanel.cpp
    src/gui/FramelessResizer.cpp
    src/gui/FramelessWindowTitleBar.cpp
)
target_include_directories(connection_panel_size_test PRIVATE src tests)
target_link_libraries(connection_panel_size_test PRIVATE
    aethercore Qt6::Core Qt6::Network Qt6::Widgets Qt6::Test
)
set_target_properties(connection_panel_size_test PROPERTIES AUTOMOC ON)
add_test(NAME connection_panel_size_test COMMAND connection_panel_size_test)
set_tests_properties(connection_panel_size_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# FlexControl port-loss recovery (#4574). Guarded on SERIALPORT for the same
# reason FlexControlManager itself is: the whole class is inside
# #ifdef HAVE_SERIALPORT, so without Qt SerialPort there is nothing to test.
if(TARGET Qt6::SerialPort)
    add_executable(flex_control_recovery_test
        tests/flex_control_recovery_test.cpp
    )
    target_include_directories(flex_control_recovery_test PRIVATE src)
    # aethercore already carries FlexControlManager and the logging category it
    # uses; compiling the .cpp standalone would drag in LogManager -> AppSettings
    # -> AsyncLogWriter and need most of the core library linked by hand anyway.
    target_link_libraries(flex_control_recovery_test PRIVATE
        aethercore Qt6::Core Qt6::SerialPort Qt6::Test
    )
    set_target_properties(flex_control_recovery_test PROPERTIES AUTOMOC ON)
    add_test(NAME flex_control_recovery_test COMMAND flex_control_recovery_test)
endif()

add_executable(pan_layout_dialog_size_test
    tests/pan_layout_dialog_size_test.cpp
    src/gui/PanLayoutDialog.cpp
    src/gui/PersistentDialog.cpp
    src/gui/FramelessResizer.cpp
    src/gui/FramelessWindowTitleBar.cpp
    src/core/ThemeManager.cpp
    src/core/ThemeSeedGenerated.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
)
target_include_directories(pan_layout_dialog_size_test PRIVATE src)
target_link_libraries(pan_layout_dialog_size_test PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Test
)
set_target_properties(pan_layout_dialog_size_test PROPERTIES AUTOMOC ON)
add_test(NAME pan_layout_dialog_size_test COMMAND pan_layout_dialog_size_test)
set_tests_properties(pan_layout_dialog_size_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(spectrum_overlay_wheel_guard_test
    tests/spectrum_overlay_wheel_guard_test.cpp
    src/gui/SpectrumOverlayWheelGuard.cpp
    src/gui/DragValuePopup.cpp   # GuardedSlider.h (ControlsLock coverage)
)
target_include_directories(spectrum_overlay_wheel_guard_test PRIVATE src)
target_link_libraries(spectrum_overlay_wheel_guard_test PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Test
)
set_target_properties(spectrum_overlay_wheel_guard_test PROPERTIES AUTOMOC ON)
add_test(NAME spectrum_overlay_wheel_guard_test
         COMMAND spectrum_overlay_wheel_guard_test)
set_tests_properties(spectrum_overlay_wheel_guard_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(device_diagnostics_test
    tests/device_diagnostics_test.cpp
)
target_include_directories(device_diagnostics_test PRIVATE src)
target_link_libraries(device_diagnostics_test PRIVATE Qt6::Core)

add_executable(midi_settings_test
    tests/midi_settings_test.cpp
    src/core/MidiSettings.cpp
)
target_compile_definitions(midi_settings_test PRIVATE HAVE_MIDI)

if (USE_SYSTEM_RTMIDI)
    target_link_libraries(midi_settings_test PRIVATE PkgConfig::rtmidi)
else()
    target_include_directories(midi_settings_test PRIVATE third_party/rtmidi)
endif()
target_include_directories(midi_settings_test PRIVATE src)
target_link_libraries(midi_settings_test PRIVATE Qt6::Core)
add_test(NAME midi_settings_test COMMAND midi_settings_test)

add_executable(midi_relative_cc_decoder_test
    tests/midi_relative_cc_decoder_test.cpp
)
target_include_directories(midi_relative_cc_decoder_test PRIVATE src)
add_test(NAME midi_relative_cc_decoder_test COMMAND midi_relative_cc_decoder_test)

add_executable(ulanzi_chord_decoder_test
    tests/ulanzi_chord_decoder_test.cpp
    src/core/UlanziChordDecoder.cpp
)
target_include_directories(ulanzi_chord_decoder_test PRIVATE src)
target_link_libraries(ulanzi_chord_decoder_test PRIVATE Qt6::Core)
add_test(NAME ulanzi_chord_decoder_test COMMAND ulanzi_chord_decoder_test)

add_executable(ulanzi_mapping_migration_test
    tests/ulanzi_mapping_migration_test.cpp
    src/core/UlanziDialMappings.cpp
    src/core/AsyncLogWriter.cpp
    src/core/LogManager.cpp
    ${AETHER_SETTINGS_SOURCES}
)
target_include_directories(ulanzi_mapping_migration_test PRIVATE src tests)
# The drift assertion parses the wheel-action else-if chain out of
# MainWindow_Controllers.cpp at run time, so it needs to find the source tree
# from wherever ctest runs it.
target_compile_definitions(ulanzi_mapping_migration_test PRIVATE
    AETHER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ulanzi_mapping_migration_test PRIVATE Qt6::Core)
add_test(NAME ulanzi_mapping_migration_test COMMAND ulanzi_mapping_migration_test)

add_executable(transmit_model_test
    tests/transmit_model_test.cpp
    src/models/TransmitModel.cpp
    src/core/ClientQuindarTone.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/AsyncLogWriter.cpp
    src/core/LogManager.cpp
)
target_include_directories(transmit_model_test PRIVATE src)
target_link_libraries(transmit_model_test PRIVATE Qt6::Core)
if(UNIX)
    target_link_libraries(transmit_model_test PRIVATE pthread)
endif()
add_test(NAME transmit_model_test COMMAND transmit_model_test)

add_executable(transmit_inhibit_policy_test
    tests/transmit_inhibit_policy_test.cpp
    src/core/CommandParser.cpp
)
target_include_directories(transmit_inhibit_policy_test PRIVATE src)
target_link_libraries(transmit_inhibit_policy_test PRIVATE Qt6::Core)
add_test(NAME transmit_inhibit_policy_test COMMAND transmit_inhibit_policy_test)

add_executable(kiwi_sdr_tx_mute_policy_test
    tests/kiwi_sdr_tx_mute_policy_test.cpp
    # The resume hold's presentation-holdback input is routed by
    # receivePresentationExternalKiwiDelayMs(); pinned against the real one.
    src/core/ReceivePresentationSync.cpp
)
target_include_directories(kiwi_sdr_tx_mute_policy_test PRIVATE src)
target_link_libraries(kiwi_sdr_tx_mute_policy_test PRIVATE Qt6::Core)
add_test(NAME kiwi_sdr_tx_mute_policy_test COMMAND kiwi_sdr_tx_mute_policy_test)
add_executable(host_voice_chain_policy_test
    tests/host_voice_chain_policy_test.cpp
)
target_include_directories(host_voice_chain_policy_test PRIVATE src)
add_test(NAME host_voice_chain_policy_test COMMAND host_voice_chain_policy_test)
add_executable(hl2_tx_level_policy_test
    tests/hl2_tx_level_policy_test.cpp
)
target_include_directories(hl2_tx_level_policy_test PRIVATE src)
add_test(NAME hl2_tx_level_policy_test COMMAND hl2_tx_level_policy_test)
add_executable(slice_link_policy_test
    tests/slice_link_policy_test.cpp
)
target_include_directories(slice_link_policy_test PRIVATE src)
target_link_libraries(slice_link_policy_test PRIVATE Qt6::Core)
add_test(NAME slice_link_policy_test COMMAND slice_link_policy_test)

# Container system Phase 1 tests — needs QApplication + Widgets.
add_executable(container_widget_test
    tests/container_widget_test.cpp
    src/gui/FramelessResizer.cpp
    src/gui/containers/ContainerTitleBar.cpp
    src/gui/containers/ContainerWidget.cpp
    src/gui/containers/FloatingContainerWindow.cpp
    ${AETHER_SETTINGS_SOURCES}
    # through ThemeManager::applyStyleSheet — pull in the manager + its
    # logging deps so the test links.  ThemeManager's compiled-in
    # seedBuiltinDefaults() means we don't need the theme resource here.
    src/core/ThemeManager.cpp
    src/core/ThemeSeedGenerated.cpp
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
)
target_include_directories(container_widget_test PRIVATE src)
target_link_libraries(container_widget_test PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Test
)
set_target_properties(container_widget_test PROPERTIES AUTOMOC ON)
add_test(NAME container_widget_test COMMAND container_widget_test)
set_tests_properties(container_widget_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Mini-pan applet: scope render API, feed lifecycle driven by show/hide, and
# the Principle V single-object span persistence.  Offscreen.
add_executable(mini_pan_widget_test
    tests/mini_pan_widget_test.cpp
    src/gui/MiniPanScope.cpp
    src/gui/MiniPanApplet.cpp
    src/gui/FramelessResizer.cpp
    src/gui/FramelessWindowTitleBar.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/MiniPanSettings.cpp
    src/core/ThemeManager.cpp
    # ThemeManager.cpp calls seedGeneratedDefaults(), which lives ONLY in the
    # generated seed TU — every other ThemeManager consumer in this file pairs
    # the two, and omitting it is a link error, not a compile one.
    src/core/ThemeSeedGenerated.cpp
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
)
target_include_directories(mini_pan_widget_test PRIVATE src)
target_link_libraries(mini_pan_widget_test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test
)
set_target_properties(mini_pan_widget_test PROPERTIES AUTOMOC ON)
add_test(NAME mini_pan_widget_test COMMAND mini_pan_widget_test)
set_tests_properties(mini_pan_widget_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# PC-audio lock contract for host-modulating backends (HL2) — needs
# QApplication + Widgets.
add_executable(hl2_pc_audio_lock_test
    tests/hl2_pc_audio_lock_test.cpp
    src/gui/TitleBar.cpp
    # TitleBar's dialogs (PC-audio tooltip help, message boxes) are frameless,
    # so the resizer + frameless title bar come along; ThemeManager pulls its
    # logging deps, same as container_widget_test.
    src/gui/FramelessMessageBox.cpp
    src/gui/PersistentDialog.cpp
    src/gui/FramelessResizer.cpp
    src/gui/FramelessWindowTitleBar.cpp
    src/gui/DragValuePopup.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/ThemeManager.cpp
    src/core/ThemeSeedGenerated.cpp
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
)
target_include_directories(hl2_pc_audio_lock_test PRIVATE src)
target_link_libraries(hl2_pc_audio_lock_test PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Network Qt6::Test
)
set_target_properties(hl2_pc_audio_lock_test PROPERTIES AUTOMOC ON)
add_test(NAME hl2_pc_audio_lock_test COMMAND hl2_pc_audio_lock_test)
set_tests_properties(hl2_pc_audio_lock_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Title-bar headphone mute reconcile contract (#4722) — needs QApplication +
# Widgets, same dependency set as hl2_pc_audio_lock_test above.
add_executable(titlebar_headphone_mute_test
    tests/titlebar_headphone_mute_test.cpp
    src/gui/TitleBar.cpp
    # TitleBar's dialogs are frameless, so the resizer + frameless title bar
    # come along; ThemeManager pulls its logging deps.
    src/gui/FramelessMessageBox.cpp
    src/gui/PersistentDialog.cpp
    src/gui/FramelessResizer.cpp
    src/gui/FramelessWindowTitleBar.cpp
    src/gui/DragValuePopup.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/ThemeManager.cpp
    src/core/ThemeSeedGenerated.cpp
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
)
target_include_directories(titlebar_headphone_mute_test PRIVATE src tests)
target_link_libraries(titlebar_headphone_mute_test PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Network Qt6::Test
)
set_target_properties(titlebar_headphone_mute_test PROPERTIES AUTOMOC ON)
add_test(NAME titlebar_headphone_mute_test COMMAND titlebar_headphone_mute_test)
set_tests_properties(titlebar_headphone_mute_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Pure index arithmetic lifted out of RxApplet — no GUI, no radio.
add_executable(icom_replay_test tests/icom_replay_test.cpp)
target_include_directories(icom_replay_test PRIVATE src)
target_link_libraries(icom_replay_test PRIVATE aethercore Qt6::Core Qt6::Network)
add_test(NAME icom_replay_test COMMAND icom_replay_test)

add_executable(rx_filter_step_test tests/rx_filter_step_test.cpp)
target_include_directories(rx_filter_step_test PRIVATE src)
target_link_libraries(rx_filter_step_test PRIVATE Qt6::Core)
add_test(NAME rx_filter_step_test COMMAND rx_filter_step_test)

add_executable(amp_applet_test
    tests/amp_applet_test.cpp
    src/gui/AmpApplet.cpp
    src/gui/DragValuePopup.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/ThemeManager.cpp
    src/core/ThemeSeedGenerated.cpp
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
)
target_include_directories(amp_applet_test PRIVATE src)
target_link_libraries(amp_applet_test PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Test
)
set_target_properties(amp_applet_test PROPERTIES AUTOMOC ON)
add_test(NAME amp_applet_test COMMAND amp_applet_test)

add_executable(tx_applet_power_reconciliation_test
    tests/tx_applet_power_reconciliation_test.cpp
    src/gui/TxApplet.cpp
    src/gui/AtuPreTuneDialog.cpp
    src/gui/DragValuePopup.cpp
    src/gui/FramelessResizer.cpp
    src/gui/FramelessWindowTitleBar.cpp
)
target_include_directories(tx_applet_power_reconciliation_test PRIVATE src)
target_link_libraries(tx_applet_power_reconciliation_test PRIVATE
    aethercore Qt6::Core Qt6::Widgets Qt6::Test
)
set_target_properties(tx_applet_power_reconciliation_test PROPERTIES AUTOMOC ON)
add_test(NAME tx_applet_power_reconciliation_test
         COMMAND tx_applet_power_reconciliation_test)
set_tests_properties(tx_applet_power_reconciliation_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(container_manager_test
    tests/container_manager_test.cpp
    src/gui/FramelessResizer.cpp
    src/gui/containers/ContainerManager.cpp
    src/gui/containers/ContainerTitleBar.cpp
    src/gui/containers/ContainerWidget.cpp
    src/gui/containers/FloatingContainerWindow.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/ThemeManager.cpp
    src/core/ThemeSeedGenerated.cpp
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
)
target_include_directories(container_manager_test PRIVATE src)
target_link_libraries(container_manager_test PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Test
)
set_target_properties(container_manager_test PROPERTIES AUTOMOC ON)

add_executable(container_nesting_test
    tests/container_nesting_test.cpp
    src/gui/FramelessResizer.cpp
    src/gui/containers/ContainerManager.cpp
    src/gui/containers/ContainerTitleBar.cpp
    src/gui/containers/ContainerWidget.cpp
    src/gui/containers/FloatingContainerWindow.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/ThemeManager.cpp
    src/core/ThemeSeedGenerated.cpp
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
)
target_include_directories(container_nesting_test PRIVATE src)
target_link_libraries(container_nesting_test PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Test
)
set_target_properties(container_nesting_test PROPERTIES AUTOMOC ON)

# Integration test — requires a running AetherSDR instance.
# Not added to ctest; run manually:
#   ./build/rigctld_test [--host HOST] [--port PORT] [--ptt] [--cw]
add_executable(rigctld_test
    tests/rigctld_test.cpp
)
target_include_directories(rigctld_test PRIVATE src)
target_link_libraries(rigctld_test PRIVATE Qt6::Core Qt6::Network)

# Integration tests — require a running AetherSDR instance with CAT ports enabled.
# Not added to ctest; run manually:
#   ./build/CAT_TS-2000_test  [--host HOST] [--port PORT] [--ptt] [--cw] [--pty PATH]
#   ./build/CAT_Flex_test     [--host HOST] [--port PORT] [--ptt] [--cw] [--pty PATH]
add_executable(CAT_TS-2000_test
    tests/CAT_TS-2000_test.cpp
)
target_include_directories(CAT_TS-2000_test PRIVATE src)
target_link_libraries(CAT_TS-2000_test PRIVATE Qt6::Core Qt6::Network)

add_executable(CAT_Flex_test
    tests/CAT_Flex_test.cpp
)
target_include_directories(CAT_Flex_test PRIVATE src)
target_link_libraries(CAT_Flex_test PRIVATE Qt6::Core Qt6::Network)


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

# ── Settings store (RFC #4603) ───────────────────────────────────────────────
# Every standalone test/tool target that compiles ${AETHER_SETTINGS_SOURCES}
# directly (rather than linking aethercore) needs the vendored SQLite engine.
# Conditional targets are guarded with if(TARGET ...).
set(AETHER_SETTINGS_CONSUMERS
    slice_label_test
    ulanzi_mapping_migration_test
    theme_manager_test
    theme_seed_test
    panadapter_message_overlay_test
    app_settings_safety_test
    nr2_settings_model_test
    rn2_settings_model_test
    panadapter_model_rx_antenna_test
    qso_recorder_slice_lifetime_test
    qso_recorder_pc_audio_guard_test
    band_plan_license_filter_test
    kiwisdr_dx_spots_test
    passive_spots_policy_test
    digital_voice_waveform_process_test
    dstar_model_test
    ole_compound_file_test
    copy_assist_settings_dialog_test
    s_meter_geometry_test
    qrz_callsign_test
    shortcut_manager_test
    antenna_alias_test
    mqtt_settings_test
    ax25_libmodem_shim_test
    ax25_replay
    ax25_session_analyze
    ax25_link_timing_test
    pms_mailbox_test
    aprs_messenger_test
    tnc_terminal_test
    cwx_speed_modifier_test
    cwx_drain_watch_test
    cwx_panel_test
    meter_model_test
    health_applet_test
    perf_telemetry_test
    local_memory_bank_test
    transmit_model_apd_test
    help_dialog_test
    flex_control_dialog_size_test
    pan_layout_dialog_size_test
    transmit_model_test
    container_widget_test
    hl2_pc_audio_lock_test
    titlebar_headphone_mute_test
    amp_applet_test
    container_manager_test
    container_nesting_test
    mini_pan_widget_test
    log_manager_filter_rules_test
    bandplan_voice_labels_test
)
foreach(_settings_consumer IN LISTS AETHER_SETTINGS_CONSUMERS)
    if(TARGET ${_settings_consumer})
        target_link_libraries(${_settings_consumer} PRIVATE aether_sqlite3)
    endif()
endforeach()
