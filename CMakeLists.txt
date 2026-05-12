cmake_minimum_required(VERSION 3.20)
project(AetherSDR VERSION 26.5.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Verify What's New data includes the current version.
# Run scripts/gen_whatsnew.py after updating CHANGELOG.md for a new release.
file(READ "${CMAKE_SOURCE_DIR}/src/generated/WhatsNewData.cpp" _whatsnew_content)
if(NOT _whatsnew_content MATCHES "\"${PROJECT_VERSION}\"")
    message(FATAL_ERROR
        "WhatsNewData.cpp has no entry for version ${PROJECT_VERSION}. "
        "Update CHANGELOG.md and run: python3 scripts/gen_whatsnew.py CHANGELOG.md src/generated/WhatsNewData.cpp")
endif()

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
# Qt 6.2 minimum: matches Ubuntu 22.04 LTS (our oldest supported distro)
# and is the floor where QWindow::startSystemMove() — used by the
# frameless title-bar drag in TitleBar / ContainerWidget / PanadapterApplet
# — became available.  Binary distributions bundle Qt 6.7+ for QRhiWidget.
find_package(Qt6 6.2 REQUIRED COMPONENTS
    Core
    Widgets
    Network
    Multimedia
    Test
)
option(REQUIRE_SERIALPORT "Fail if Qt6 SerialPort is not found (use for release builds)" OFF)
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
find_package(Qt6Keychain QUIET)
if(Qt6Keychain_FOUND)
    message(STATUS "Qt6Keychain found — SmartLink credential persistence enabled")
else()
    message(STATUS "Qt6Keychain not found — SmartLink credential persistence disabled")
endif()

# PortAudio (optional fallback for audio)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PORTAUDIO portaudio-2.0)
endif()

# FFTW3 (required for NR2 spectral noise reduction)
# Windows: run setup-fftw.ps1 first to download prebuilt DLLs
# Linux:   apt install libfftw3-dev
# macOS:   brew install fftw
if(WIN32)
    set(FFTW3_ROOT "${CMAKE_SOURCE_DIR}/third_party/fftw3")
    if(EXISTS "${FFTW3_ROOT}/include/fftw3.h")
        set(FFTW3_FOUND TRUE)
        set(FFTW3_INCLUDE_DIRS "${FFTW3_ROOT}/include")
        set(FFTW3_LIBRARIES "${FFTW3_ROOT}/lib/fftw3.lib")
        set(FFTW3_DLL "${FFTW3_ROOT}/bin/libfftw3-3.dll")
    else()
        message(WARNING "FFTW3 not found. Run setup-fftw.ps1 to download it. NR2 will use fallback FFT.")
    endif()
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

# Bundled RADE (BSD-2) — FreeDV Radio Autoencoder digital voice codec
# Opus (with FARGAN/LPCNet) is built from a vendored local snapshot via ExternalProject
set(RADE_DIR ${CMAKE_SOURCE_DIR}/third_party/radae)
option(ENABLE_RADE "Build with RADE digital voice support (uses vendored Opus snapshot)" ON)
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
# Windows: run setup-opus.ps1 first to download prebuilt DLLs
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
        message(WARNING "Opus not found. Run setup-opus.ps1 to download it. "
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
# Defaults to ON but auto-disables on Qt < 6.7 (e.g. Ubuntu 24.04 ships Qt 6.4).
option(AETHER_GPU_SPECTRUM "Enable QRhi GPU spectrum rendering" ON)
if(AETHER_GPU_SPECTRUM)
    if(Qt6_VERSION VERSION_LESS "6.7")
        set(AETHER_GPU_SPECTRUM OFF)
        message(STATUS "GPU spectrum rendering disabled (requires Qt 6.7+, found ${Qt6_VERSION} — pass -DCMAKE_PREFIX_PATH=/path/to/Qt/6.7.x/gcc_64 to use a newer Qt installation)")
    else()
        find_package(Qt6 REQUIRED COMPONENTS ShaderTools)
        # Qt6GuiPrivate provides QRhi headers needed for GPU rendering.
        find_package(Qt6GuiPrivate QUIET)

        # Detect Debian paths early
        if(NOT Qt6GuiPrivate_FOUND)
            message(STATUS "Qt6GuiPrivate not found, checking Debian multi-arch paths...")
            find_path(DEBIAN_PRIVATE_INC
                NAMES "QtGui/${Qt6_VERSION}/QtGui/private/qhighdpiscaling_p.h"
                PATHS "/usr/include/x86_64-linux-gnu/qt6" "/usr/include/qt6"
                NO_DEFAULT_PATH
            )
            if(DEBIAN_PRIVATE_INC)
                set(Qt6GuiPrivate_FOUND TRUE)
                set(DEBIAN_GPU_FIX_REQUIRED TRUE) # Mark for Step 2
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

# NVIDIA NIM BNR (Background Noise Removal) — GPU-accelerated neural denoising
# Requires: grpc++, protobuf, NVIDIA RTX 4000+ GPU, Docker container
option(ENABLE_BNR "Enable NVIDIA NIM BNR noise removal (requires grpc++)" OFF)
if(ENABLE_BNR)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GRPC REQUIRED grpc++)
    pkg_check_modules(PROTOBUF REQUIRED protobuf)
    find_program(PROTOC protoc REQUIRED)
    find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

    # Generate C++ stubs from bnr.proto
    set(BNR_PROTO "${CMAKE_SOURCE_DIR}/src/core/proto/bnr.proto")
    set(BNR_GEN_DIR "${CMAKE_BINARY_DIR}/bnr_gen")
    file(MAKE_DIRECTORY ${BNR_GEN_DIR})

    set(BNR_PB_CC "${BNR_GEN_DIR}/bnr.pb.cc")
    set(BNR_PB_H  "${BNR_GEN_DIR}/bnr.pb.h")
    set(BNR_GRPC_CC "${BNR_GEN_DIR}/bnr.grpc.pb.cc")
    set(BNR_GRPC_H  "${BNR_GEN_DIR}/bnr.grpc.pb.h")

    add_custom_command(
        OUTPUT ${BNR_PB_CC} ${BNR_PB_H} ${BNR_GRPC_CC} ${BNR_GRPC_H}
        COMMAND ${PROTOC}
            --proto_path=${CMAKE_SOURCE_DIR}/src/core/proto
            --cpp_out=${BNR_GEN_DIR}
            --grpc_out=${BNR_GEN_DIR}
            --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
            ${BNR_PROTO}
        DEPENDS ${BNR_PROTO}
        COMMENT "Generating BNR gRPC stubs from bnr.proto"
    )

    set(BNR_SOURCES ${BNR_PB_CC} ${BNR_GRPC_CC})
    message(STATUS "NVIDIA BNR enabled (gRPC ${GRPC_VERSION})")
else()
    set(BNR_SOURCES "")
    message(STATUS "NVIDIA BNR disabled (use -DENABLE_BNR=ON to enable)")
endif()

# libspecbleach NR4 — bundled spectral noise reduction (LGPL-2.1)
# MSVC: libspecbleach uses C99 VLAs and __attribute__ which MSVC doesn't support.
# When clang-cl is available, build as a separate static lib using it.
if(MSVC)
    find_program(CLANG_CL clang-cl HINTS "C:/Program Files/LLVM/bin")
    if(CLANG_CL)
        option(ENABLE_SPECBLEACH "Enable NR4 spectral bleach noise reduction" ON)
    else()
        option(ENABLE_SPECBLEACH "Enable NR4 spectral bleach noise reduction" OFF)
    endif()
else()
    option(ENABLE_SPECBLEACH "Enable NR4 spectral bleach noise reduction" ON)
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
# Pre-built static library from Rust crate (libdf), model weights embedded.
option(ENABLE_DFNR "Enable DFNR DeepFilterNet3 noise reduction" ON)
if(ENABLE_DFNR)
    set(DEEPFILTER_DIR ${CMAKE_SOURCE_DIR}/third_party/deepfilter)
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
                       "(run ./setup-deepfilter.sh before cmake to enable)")
    endif()
endif()

# MQTT client support — bundled libmosquitto (#699)
option(ENABLE_MQTT "Enable MQTT client support" ON)
option(MQTT_TLS "Enable MQTT TLS via OpenSSL (disable for AppImage)" ON)
if(ENABLE_MQTT)
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

# Sources
set(CORE_SOURCES
    src/core/AppSettings.cpp
    src/core/BandStackSettings.cpp
    src/core/RadioDiscovery.cpp
    src/core/RadioConnection.cpp
    src/core/NetworkPathResolver.cpp
    src/core/TgxlConnection.cpp
    src/core/CommandParser.cpp
    src/core/AudioEngine.cpp
    src/core/TxMicChannelNormalizer.cpp
    src/core/ChannelStripPresets.cpp
    src/core/ClientEq.cpp
    src/core/ClientComp.cpp
    src/core/ClientGate.cpp
    src/core/ClientDeEss.cpp
    src/core/ClientTube.cpp
    src/core/ClientPudu.cpp
    src/core/ClientPuduMonitor.cpp
    src/core/ClientReverb.cpp
    src/core/ClientFinalLimiter.cpp
    src/core/ClientTxTestTone.cpp
    src/core/ClientQuindarTone.cpp
    src/core/QuindarLocalSink.cpp
    src/core/CwSidetoneGenerator.cpp
    src/core/CwSidetoneQAudioSink.cpp
    src/core/CwxLocalKeyer.cpp
    src/core/IambicKeyer.cpp
    src/core/SpectralNR.cpp
    src/core/PanadapterStream.cpp
    src/core/PerfTelemetry.cpp
    src/core/RigctlProtocol.cpp
    src/core/RigctlPty.cpp
    src/core/SmartLinkClient.cpp
    src/core/WanConnection.cpp
    src/core/DxClusterClient.cpp
    src/core/WsjtxClient.cpp
    src/core/SpotCollectorClient.cpp
    src/core/PotaClient.cpp
    src/core/PropForecastClient.cpp
    src/core/SpotCommandPolicy.cpp
    src/core/SpotModeResolver.cpp
    src/core/RigctlServer.cpp
    src/core/TciServer.cpp
    src/core/TciProtocol.cpp
    src/core/MqttClient.cpp
    src/core/PgxlConnection.cpp
    src/core/FirmwareUploader.cpp
    src/core/DvkWavTransfer.cpp
    src/core/QsoRecorder.cpp
    src/core/FirmwareStager.cpp
    src/core/OleCompoundFile.cpp
    src/core/CabExtractor.cpp
    src/core/RNNoiseFilter.cpp
    src/core/SpecbleachFilter.cpp
    src/core/CwDecoder.cpp
    src/core/VoiceSignalDetector.cpp
    src/core/SpectrogramBuffer.cpp
    src/core/SignalClassifier.cpp
    src/core/Resampler.cpp
    src/core/NvidiaBnrFilter.cpp
    src/core/DeepFilterFilter.cpp
    src/core/RADEEngine.cpp
    src/core/AsyncLogWriter.cpp
    src/core/LogManager.cpp
    src/core/ShortcutManager.cpp
    src/core/SupportBundle.cpp
    src/core/DeviceDiagnostics.cpp
    src/core/SerialPortController.cpp
    src/core/FlexControlManager.cpp
    src/core/OpusCodec.cpp
    src/core/CtyDatParser.cpp
    src/core/AdifParser.cpp
    src/core/DxccWorkedStatus.cpp
    src/core/DxccColorProvider.cpp
    src/core/SleepInhibitor.cpp
    src/core/MemoryCsvCompat.cpp
    src/core/MemoryRecallPolicy.cpp
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
    src/models/RadioModel.cpp
    src/models/SliceModel.cpp
    src/models/PanadapterModel.cpp
    src/models/MeterModel.cpp
    src/models/TunerModel.cpp
    src/models/TransmitModel.cpp
    src/models/EqualizerModel.cpp
    src/models/TnfModel.cpp
    src/models/UsbCableModel.cpp
    src/models/DaxIqModel.cpp
    src/models/SpotModel.cpp
    src/models/CwxModel.cpp
    src/models/DvkModel.cpp
    src/models/NavtexModel.cpp
    src/models/BandSettings.cpp
    src/models/BandPlanManager.cpp
    src/models/XvtrPolicy.cpp
    src/models/AntennaGeniusModel.cpp
)

set(GUI_SOURCES
    src/gui/MainWindow.cpp
    src/gui/AudioDeviceChangeDialog.cpp
    src/gui/ConnectionPanel.cpp
    src/gui/ClientDisconnectDialog.cpp
    src/gui/ConnectedStationsDialog.cpp
    src/gui/SpectrumWidget.cpp
    src/gui/SpectrumOverlayMenu.cpp
    src/gui/SliceColorManager.cpp
    src/gui/VfoWidget.cpp
    src/gui/RadioSetupDialog.cpp
    src/gui/NetworkDiagnosticsDialog.cpp
    src/gui/PropDashboardDialog.cpp
    src/gui/MemoryCommands.cpp
    src/gui/MemoryBrowsePanel.cpp
    src/gui/MemoryDialog.cpp
    src/gui/SpotSettingsDialog.cpp
    src/gui/AetherDspDialog.cpp
    src/gui/AetherDspWidget.cpp
    src/gui/ClientRxDspApplet.cpp
    src/gui/DspParamPopup.cpp
    src/gui/DxClusterDialog.cpp
    src/gui/CwxPanel.cpp
    src/gui/BandStackPanel.cpp
    src/gui/FramelessWindowTitleBar.cpp
    src/gui/FramelessResizer.cpp
    src/gui/PanFloatingWindow.cpp
    src/gui/DvkPanel.cpp
    src/gui/AmpApplet.cpp
    src/gui/MeterApplet.cpp
    src/gui/ProfileManagerDialog.cpp
    src/gui/PanadapterApplet.cpp
    src/gui/PanadapterStack.cpp
    src/gui/PanLayoutDialog.cpp
    src/gui/AppletPanel.cpp
    src/gui/RxApplet.cpp
    src/gui/FilterPassbandWidget.cpp
    src/gui/SMeterWidget.cpp
    src/gui/TunerApplet.cpp
    src/gui/TxApplet.cpp
    src/gui/PhoneCwApplet.cpp
    src/gui/PhoneApplet.cpp
    src/gui/EqApplet.cpp
    src/gui/WaveApplet.cpp
    src/gui/WaveformWidget.cpp
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
    src/gui/StripWaveform.cpp
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
    src/gui/MeterSlider.cpp
    src/gui/PhaseKnob.cpp
    src/gui/AntennaGeniusApplet.cpp
    src/gui/ShackSwitchApplet.cpp
    src/gui/TitleBar.cpp
    src/gui/SupportDialog.cpp
    src/gui/SliceTroubleshootingDialog.cpp
    src/gui/KeyboardMapWidget.cpp
    src/gui/ShortcutDialog.cpp
    src/gui/MultiFlexDialog.cpp
    src/gui/HelpDialog.cpp
    src/gui/WhatsNewDialog.cpp
)

set(ALL_SOURCES
    src/main.cpp
    ${CORE_SOURCES}
    ${MODEL_SOURCES}
    ${GUI_SOURCES}
    src/generated/WhatsNewData.cpp
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

qt_add_resources(RESOURCES resources.qrc)

add_executable(AetherSDR ${ALL_SOURCES} ${RNNOISE_SOURCES} ${GGMORSE_SOURCES} ${RADE_SOURCES} ${BNR_SOURCES} ${SPECBLEACH_SOURCES} ${RESOURCES})

# Windows: GUI app (no console window) + icon resource
if(WIN32)
    set_target_properties(AetherSDR PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# macOS app bundle
if(APPLE)
    set_target_properties(AetherSDR PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.aethersdr.AetherSDR"
        MACOSX_BUNDLE_BUNDLE_NAME "AetherSDR"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "AetherSDR.icns"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in"
    )
    target_link_libraries(AetherSDR PRIVATE "-framework AVFoundation" "-framework Accelerate")
    target_sources(AetherSDR PRIVATE src/core/MacNRFilter.cpp)

    # Generate AetherSDR.icns at build time from docs/logo-circle.png so local
    # builds get an app icon without any extra setup (sips and iconutil are
    # part of macOS).
    set(ICON_SOURCE "${CMAKE_SOURCE_DIR}/docs/logo-circle.png")
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
    set(WIN_RC "${CMAKE_SOURCE_DIR}/docs/AetherSDR.rc")
    if(EXISTS "${WIN_RC}")
        target_sources(AetherSDR PRIVATE "${WIN_RC}")
    endif()
endif()

target_include_directories(AetherSDR PRIVATE
    src/
    ${RNNOISE_DIR}/include
    ${RNNOISE_DIR}/src
    ${GGMORSE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/r8brain
)

target_link_libraries(AetherSDR PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Multimedia
    ${CMAKE_DL_LIBS}   # dlopen/dlsym for tolerant X11 error handler (#1839)
)

if(Qt6SerialPort_FOUND)
    target_compile_definitions(AetherSDR PRIVATE HAVE_SERIALPORT)
    target_link_libraries(AetherSDR PRIVATE Qt6::SerialPort)
endif()

if(Qt6WebSockets_FOUND)
    target_compile_definitions(AetherSDR PRIVATE HAVE_WEBSOCKETS)
    target_sources(AetherSDR PRIVATE src/core/FreeDvClient.cpp)
    target_link_libraries(AetherSDR PRIVATE Qt6::WebSockets)
endif()

if(Qt6Keychain_FOUND)
    target_compile_definitions(AetherSDR PRIVATE HAVE_KEYCHAIN)
    target_link_libraries(AetherSDR PRIVATE Qt6Keychain::Qt6Keychain)
endif()

if(Qt6DBus_FOUND)
    target_compile_definitions(AetherSDR PRIVATE HAVE_DBUS)
    target_link_libraries(AetherSDR PRIVATE Qt6::DBus)
endif()

if(AETHER_GPU_SPECTRUM)
    target_compile_definitions(AetherSDR PRIVATE AETHER_GPU_SPECTRUM)

    # Apply the Debian include paths now that AetherSDR exists
    if(DEBIAN_GPU_FIX_REQUIRED)
        message(STATUS "Applying Debian GPU include paths to AetherSDR")
        target_include_directories(AetherSDR PRIVATE
            "${DEBIAN_PRIVATE_INC}/QtGui/${Qt6_VERSION}"
            "${DEBIAN_PRIVATE_INC}/QtGui/${Qt6_VERSION}/QtGui"
        )
    endif()

    if(TARGET Qt6::GuiPrivate)
        target_link_libraries(AetherSDR PRIVATE Qt6::GuiPrivate)
    endif()

    qt_add_shaders(AetherSDR "aether_shaders"
        PREFIX "/shaders"
        FILES
            resources/shaders/texturedquad.vert
            resources/shaders/texturedquad.frag
            resources/shaders/overlay.vert
            resources/shaders/overlay.frag
            resources/shaders/spectrum.vert
            resources/shaders/spectrum.frag
    )
endif()

# Bundled libmspack (LGPL-2.1) — CAB+LZX decompression for the v4.2+ MSI
# firmware-installer extraction path. See third_party/libmspack/README.md.
add_subdirectory(third_party/libmspack)
target_link_libraries(AetherSDR PRIVATE mspack_static)

# Bundled RtMidi (MIT license) — MIDI controller support on all platforms
message(STATUS "MIDI controller support enabled (bundled RtMidi)")
target_compile_definitions(AetherSDR PRIVATE HAVE_MIDI)
target_sources(AetherSDR PRIVATE
    third_party/rtmidi/RtMidi.cpp
    src/core/MidiControlManager.cpp
    src/core/MidiSettings.cpp
    src/gui/MidiMappingDialog.cpp)
target_include_directories(AetherSDR PRIVATE third_party/rtmidi)
if(APPLE)
    target_compile_definitions(AetherSDR PRIVATE __MACOSX_CORE__)
    target_link_libraries(AetherSDR PRIVATE "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation" "-framework IOKit")
elseif(WIN32)
    target_compile_definitions(AetherSDR PRIVATE __WINDOWS_MM__)
    target_link_libraries(AetherSDR PRIVATE winmm)
else()
    target_compile_definitions(AetherSDR PRIVATE __LINUX_ALSA__)
    target_link_libraries(AetherSDR PRIVATE asound)
endif()

# hidapi — USB HID encoder support (Stream Deck, Icom RC-28, Griffin PowerMate, Contour Shuttle)
# Windows: run setup-hidapi.ps1 first to download and build hidapi
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
        message(WARNING "hidapi not found. Run setup-hidapi.ps1 to download it. "
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
if(HIDAPI_FOUND)
    message(STATUS "hidapi found — USB HID encoder support enabled")
    target_compile_definitions(AetherSDR PRIVATE HAVE_HIDAPI)
    target_sources(AetherSDR PRIVATE
        src/core/HidEncoderManager.cpp
        src/core/HidEncoderManager.h
        src/core/HidDeviceParser.cpp
        src/core/HidDeviceParser.h)
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
    target_include_directories(AetherSDR PRIVATE ${HIDAPI_NORMALIZED_INCLUDE_DIRS})
    target_link_libraries(AetherSDR PRIVATE ${HIDAPI_LIBRARIES})
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

# ONNX Runtime — optional CNN signal classifier for S-History v2
# Windows: run setup-onnxruntime.ps1 first to download prebuilt DLLs
# Linux:   apt install libonnxruntime-dev  (or build from source)
# macOS:   brew install onnxruntime
if(WIN32)
    set(ORT_ROOT "${CMAKE_SOURCE_DIR}/third_party/onnxruntime")
    if(EXISTS "${ORT_ROOT}/include/onnxruntime_cxx_api.h")
        set(ORT_FOUND TRUE)
        set(ORT_INCLUDE_DIRS "${ORT_ROOT}/include")
        set(ORT_LIBRARIES "${ORT_ROOT}/lib/onnxruntime.lib")
        file(GLOB ORT_DLLS "${ORT_ROOT}/bin/*.dll")
    else()
        message(STATUS "ONNX Runtime not found — CNN signal classifier disabled. "
                       "Run setup-onnxruntime.ps1 to enable.")
    endif()
else()
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
    message(STATUS "ONNX Runtime found — CNN signal classifier enabled")
else()
    message(STATUS "ONNX Runtime not found — CNN signal classifier disabled")
endif()

if(PORTAUDIO_FOUND)
    target_sources(AetherSDR PRIVATE src/core/CwSidetonePortAudioSink.cpp)
    target_compile_definitions(AetherSDR PRIVATE HAVE_PORTAUDIO)
    target_include_directories(AetherSDR PRIVATE ${PORTAUDIO_INCLUDE_DIRS})
    target_link_directories(AetherSDR PRIVATE ${PORTAUDIO_LIBRARY_DIRS})
    target_link_libraries(AetherSDR PRIVATE ${PORTAUDIO_LIBRARIES})
endif()

if(FFTW3_FOUND)
    target_compile_definitions(AetherSDR PRIVATE HAVE_FFTW3
        # On Windows lld-link (used by ClangCL) requires explicit dllimport
        # declarations; define FFTW_DLL so fftw3.h emits __declspec(dllimport)
        # and the linker can resolve __imp_fftwf_* symbols from the import lib.
        $<$<BOOL:${WIN32}>:FFTW_DLL>)
    target_include_directories(AetherSDR PRIVATE ${FFTW3_INCLUDE_DIRS})
    target_link_directories(AetherSDR PRIVATE ${FFTW3_LIBRARY_DIRS})
    target_link_libraries(AetherSDR PRIVATE ${FFTW3_LIBRARIES})
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
    target_compile_definitions(AetherSDR PRIVATE HAVE_ONNX)
    target_include_directories(AetherSDR PRIVATE ${ORT_INCLUDE_DIRS})
    target_link_libraries(AetherSDR PRIVATE ${ORT_LIBRARIES})
    if(WIN32 AND ORT_DLLS)
        foreach(_ort_dll IN LISTS ORT_DLLS)
            add_custom_command(TARGET AetherSDR POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_ort_dll}" "$<TARGET_FILE_DIR:AetherSDR>"
                COMMENT "Copying ONNX Runtime DLL to build directory")
        endforeach()
    endif()
endif()

if(RADE_FOUND)
    target_compile_definitions(AetherSDR PRIVATE HAVE_RADE HAVE_OPUS IS_BUILDING_RADE_API=1)
    target_include_directories(AetherSDR PRIVATE ${RADE_DIR}/src)
    if(WIN32)
        target_link_libraries(AetherSDR PRIVATE opus)
    else()
        target_link_libraries(AetherSDR PRIVATE opus m)
    endif()
elseif(OPUS_FOUND)
    target_compile_definitions(AetherSDR PRIVATE HAVE_OPUS)
    target_include_directories(AetherSDR PRIVATE ${OPUS_INCLUDE_DIRS})
    target_link_libraries(AetherSDR PRIVATE ${OPUS_LIBRARIES})
endif()

if(ENABLE_BNR)
    target_compile_definitions(AetherSDR PRIVATE HAVE_BNR)
    target_include_directories(AetherSDR PRIVATE
        ${BNR_GEN_DIR}
        ${GRPC_INCLUDE_DIRS}
        ${PROTOBUF_INCLUDE_DIRS})
    target_link_libraries(AetherSDR PRIVATE ${GRPC_LIBRARIES} ${PROTOBUF_LIBRARIES})
endif()

if(ENABLE_SPECBLEACH)
    target_compile_definitions(AetherSDR PRIVATE HAVE_SPECBLEACH)
    target_include_directories(AetherSDR PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/libspecbleach/include
        ${CMAKE_SOURCE_DIR}/third_party/libspecbleach/src)
    if(MSVC AND SPECBLEACH_STATIC_LIB)
        # Link pre-built clang-cl static lib
        add_dependencies(AetherSDR specbleach_build)
        target_link_libraries(AetherSDR PRIVATE ${SPECBLEACH_STATIC_LIB})
        # fftw3f (float precision) for Windows
        set(FFTW3F_LIB "${CMAKE_SOURCE_DIR}/third_party/fftw3/lib/fftw3f.lib")
        if(EXISTS ${FFTW3F_LIB})
            target_link_libraries(AetherSDR PRIVATE ${FFTW3F_LIB})
            set(FFTW3F_DLL "${CMAKE_SOURCE_DIR}/third_party/fftw3/bin/libfftw3f-3.dll")
            add_custom_command(TARGET AetherSDR POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${FFTW3F_DLL}" "$<TARGET_FILE_DIR:AetherSDR>"
                COMMENT "Copying libfftw3f-3.dll to build directory")
        else()
            message(WARNING "fftw3f.lib not found — run setup-fftw.ps1 and gen-fftw3f-lib.bat")
        endif()
    else()
        # libspecbleach C sources include <fftw3.h> directly — ensure it's findable
        if(FFTW3_INCLUDE_DIRS)
            target_include_directories(AetherSDR PRIVATE ${FFTW3_INCLUDE_DIRS})
        else()
            find_path(FFTW3_H_DIR fftw3.h HINTS ${CMAKE_SOURCE_DIR}/third_party/fftw3/include)
            if(FFTW3_H_DIR)
                target_include_directories(AetherSDR PRIVATE ${FFTW3_H_DIR})
            endif()
        endif()
        # libspecbleach uses fftwf (float precision FFTW3)
        find_library(FFTW3F_LIB fftw3f HINTS /opt/homebrew/lib /usr/local/lib ${CMAKE_SOURCE_DIR}/third_party/fftw3/lib)
        if(FFTW3F_LIB)
            target_link_libraries(AetherSDR PRIVATE ${FFTW3F_LIB})
        else()
            message(WARNING "fftw3f not found — NR4 will fail to link")
        endif()
        # Suppress warnings from third-party C code
        set_source_files_properties(${SPECBLEACH_SOURCES} PROPERTIES COMPILE_FLAGS "-w")
    endif()
endif()

if(ENABLE_DFNR)
    target_compile_definitions(AetherSDR PRIVATE HAVE_DFNR)
    target_include_directories(AetherSDR PRIVATE ${DEEPFILTER_DIR}/include)
    target_link_libraries(AetherSDR PRIVATE ${DFNR_LIB})
    if(WIN32)
        target_link_libraries(AetherSDR PRIVATE ws2_32 bcrypt userenv ntdll)
    elseif(APPLE)
        # Rust runtime deps on macOS
        target_link_libraries(AetherSDR PRIVATE "-framework Security" "-framework CoreFoundation")
    else()
        # Rust runtime deps on Linux
        target_link_libraries(AetherSDR PRIVATE pthread dl m)
    endif()
    # Copy DLL and model to build directory
    if(DFNR_DLL AND EXISTS ${DFNR_DLL})
        add_custom_command(TARGET AetherSDR POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DFNR_DLL}" "$<TARGET_FILE_DIR:AetherSDR>"
            COMMENT "Copying deepfilter.dll to build directory")
    endif()
    set(DFNR_MODEL "${DEEPFILTER_DIR}/models/DeepFilterNet3_onnx.tar.gz")
    if(EXISTS ${DFNR_MODEL})
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
    if(NOT APPLE)
        install(FILES "${DFNR_MODEL}"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/AetherSDR"
            OPTIONAL)
    endif()
endif()

if(ENABLE_MQTT)
    target_compile_definitions(AetherSDR PRIVATE HAVE_MQTT)
    target_include_directories(AetherSDR PRIVATE
        ${MOSQUITTO_DIR}/include
        ${MOSQUITTO_DIR}/src)
    target_sources(AetherSDR PRIVATE ${MOSQUITTO_SOURCES})
    if(OpenSSL_FOUND)
        set(MQTT_TLS_FLAG " -DWITH_TLS")
        target_compile_definitions(AetherSDR PRIVATE HAVE_MQTT_TLS)
        target_link_libraries(AetherSDR PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    else()
        set(MQTT_TLS_FLAG "")
    endif()
    if(WIN32)
        set_source_files_properties(${MOSQUITTO_SOURCES} PROPERTIES
            COMPILE_FLAGS "-w -DLIBMOSQUITTO_STATIC -DLIBMOSQCOMMON_STATIC${MQTT_TLS_FLAG}")
        target_compile_definitions(AetherSDR PRIVATE LIBMOSQUITTO_STATIC LIBMOSQCOMMON_STATIC)
    else()
        set_source_files_properties(${MOSQUITTO_SOURCES} PROPERTIES
            COMPILE_FLAGS "-w -DWITH_THREADING${MQTT_TLS_FLAG}")
    endif()
    if(NOT WIN32)
        target_link_libraries(AetherSDR PRIVATE pthread)
    endif()
endif()

if(AETHER_GPU_SPECTRUM)
    target_compile_definitions(AetherSDR PRIVATE AETHER_GPU_SPECTRUM)
    if(TARGET Qt6::GuiPrivate)
        target_link_libraries(AetherSDR PRIVATE Qt6::GuiPrivate)
    else()
        # Qt6::GuiPrivate not available (macOS/skipped) — find rhi/ headers manually
        get_target_property(_qt_gui_inc Qt6::Gui INTERFACE_INCLUDE_DIRECTORIES)
        foreach(_dir IN LISTS _qt_gui_inc)
            if(EXISTS "${_dir}/rhi/qrhi.h")
                # Already in the include path via Qt6::Gui
                break()
            endif()
            # Check versioned private header directory (e.g. QtGui/6.7.3/QtGui/rhi/)
            file(GLOB _priv_dirs "${_dir}/${Qt6_VERSION}/QtGui")
            foreach(_pd IN LISTS _priv_dirs)
                if(EXISTS "${_pd}/rhi/qrhi.h")
                    target_include_directories(AetherSDR PRIVATE "${_pd}/..")
                    message(STATUS "QRhi headers found at ${_pd}/rhi/")
                    break()
                endif()
            endforeach()
        endforeach()
    endif()
    qt_add_shaders(AetherSDR "aether_shaders"
        PREFIX "/shaders"
        FILES
            resources/shaders/texturedquad.vert
            resources/shaders/texturedquad.frag
            resources/shaders/overlay.vert
            resources/shaders/overlay.frag
            resources/shaders/spectrum.vert
            resources/shaders/spectrum.frag
    )
endif()

if(HAVE_PIPEWIRE)
    target_compile_definitions(AetherSDR PRIVATE HAVE_PIPEWIRE)
    message(STATUS "Linux DAX bridge enabled (PulseAudio pipe modules)")
endif()

if(HAVE_PIPEWIRE_NATIVE)
    target_compile_definitions(AetherSDR PRIVATE HAVE_PIPEWIRE_NATIVE)
    target_include_directories(AetherSDR PRIVATE ${PIPEWIRE_NATIVE_INCLUDE_DIRS})
    target_link_libraries(AetherSDR PRIVATE ${PIPEWIRE_NATIVE_LIBRARIES})
    target_compile_options(AetherSDR PRIVATE ${PIPEWIRE_NATIVE_CFLAGS_OTHER})
    message(STATUS "Linux DAX RX uses native pw_stream (libpipewire-0.3 ${PIPEWIRE_NATIVE_VERSION})")
endif()

# Pass project version to code
target_compile_definitions(AetherSDR PRIVATE
    AETHERSDR_VERSION="${PROJECT_VERSION}"
)

# Compiler warnings
if(MSVC)
    target_compile_options(AetherSDR PRIVATE /W3 /Zc:__cplusplus /permissive- /utf-8 /bigobj)
else()
    target_compile_options(AetherSDR PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<CONFIG:Debug>:-g3 -fsanitize=address>
        $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
    )
    target_link_options(AetherSDR PRIVATE
        $<$<CONFIG:Debug>:-fsanitize=address>
    )
endif()


# ── Unit test harnesses ──────────────────────────────────────────────────────
# Standalone DSP smoke tests. Built alongside the main target so they share
# the same toolchain and warning flags. Run manually with ./build/<target>.

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

add_executable(client_comp_test
    tests/client_comp_test.cpp
    src/core/ClientComp.cpp
)
target_include_directories(client_comp_test PRIVATE src)

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

add_executable(passive_spots_policy_test
    tests/passive_spots_policy_test.cpp
    src/core/AppSettings.cpp
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

add_executable(navtex_model_test
    tests/navtex_model_test.cpp
    src/models/NavtexModel.cpp
)
target_include_directories(navtex_model_test PRIVATE src)
target_link_libraries(navtex_model_test PRIVATE Qt6::Core Qt6::Test)

add_executable(ole_compound_file_test
    tests/ole_compound_file_test.cpp
    src/core/OleCompoundFile.cpp
    src/core/CabExtractor.cpp
    src/core/AsyncLogWriter.cpp
    src/core/LogManager.cpp
    src/core/AppSettings.cpp
)
target_include_directories(ole_compound_file_test PRIVATE src)
target_link_libraries(ole_compound_file_test PRIVATE Qt6::Core mspack_static)
if(UNIX)
    target_link_libraries(ole_compound_file_test PRIVATE pthread)
endif()

add_executable(xvtr_policy_test
    tests/xvtr_policy_test.cpp
    src/models/XvtrPolicy.cpp
)
target_include_directories(xvtr_policy_test PRIVATE src)
target_link_libraries(xvtr_policy_test PRIVATE Qt6::Core)

add_executable(radio_status_ownership_test
    tests/radio_status_ownership_test.cpp
    src/core/CommandParser.cpp
)
target_include_directories(radio_status_ownership_test PRIVATE src)
target_link_libraries(radio_status_ownership_test PRIVATE Qt6::Core)
enable_testing()
add_test(NAME radio_status_ownership_test COMMAND radio_status_ownership_test)

add_executable(shortcut_manager_test
    tests/shortcut_manager_test.cpp
    src/core/ShortcutManager.cpp
    src/core/AppSettings.cpp
)
target_include_directories(shortcut_manager_test PRIVATE src)
target_link_libraries(shortcut_manager_test PRIVATE Qt6::Core Qt6::Widgets)
add_test(NAME shortcut_manager_test COMMAND shortcut_manager_test)

add_executable(cw_sidetone_test
    tests/cw_sidetone_test.cpp
    src/core/CwSidetoneGenerator.cpp
)
target_include_directories(cw_sidetone_test PRIVATE src)
target_link_libraries(cw_sidetone_test PRIVATE Qt6::Core)

add_executable(cwx_panel_test
    tests/cwx_panel_test.cpp
    src/gui/CwxPanel.cpp
    src/gui/CwxPanel.h
    src/models/CwxModel.cpp
    src/models/CwxModel.h
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
    src/core/AppSettings.cpp
)
target_include_directories(meter_model_test PRIVATE src)
target_link_libraries(meter_model_test PRIVATE Qt6::Core)
if(UNIX)
    target_link_libraries(meter_model_test PRIVATE pthread)
endif()
set_target_properties(meter_model_test PROPERTIES AUTOMOC ON)

add_executable(memory_recall_policy_test
    tests/memory_recall_policy_test.cpp
    src/core/MemoryRecallPolicy.cpp
)
target_include_directories(memory_recall_policy_test PRIVATE src)
target_link_libraries(memory_recall_policy_test PRIVATE Qt6::Core)

add_executable(transmit_model_apd_test
    tests/transmit_model_apd_test.cpp
    src/models/TransmitModel.cpp
    src/core/ClientQuindarTone.cpp
    src/core/AsyncLogWriter.cpp
    src/core/LogManager.cpp
    src/core/AppSettings.cpp
)
target_include_directories(transmit_model_apd_test PRIVATE src)
target_link_libraries(transmit_model_apd_test PRIVATE Qt6::Core Qt6::Test)
if(UNIX)
    target_link_libraries(transmit_model_apd_test PRIVATE pthread)
endif()
set_target_properties(transmit_model_apd_test PROPERTIES AUTOMOC ON)

# Help guide search tests - needs QApplication + Widgets.
add_executable(help_dialog_test
    tests/help_dialog_test.cpp
    src/gui/HelpDialog.cpp
)
target_include_directories(help_dialog_test PRIVATE src)
target_link_libraries(help_dialog_test PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Test
)
set_target_properties(help_dialog_test PROPERTIES AUTOMOC ON)

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
target_include_directories(midi_settings_test PRIVATE src third_party/rtmidi)
target_link_libraries(midi_settings_test PRIVATE Qt6::Core)
add_test(NAME midi_settings_test COMMAND midi_settings_test)

add_executable(transmit_model_test
    tests/transmit_model_test.cpp
    src/models/TransmitModel.cpp
    src/core/ClientQuindarTone.cpp
    src/core/AppSettings.cpp
    src/core/AsyncLogWriter.cpp
    src/core/LogManager.cpp
)
target_include_directories(transmit_model_test PRIVATE src)
target_link_libraries(transmit_model_test PRIVATE Qt6::Core)
if(UNIX)
    target_link_libraries(transmit_model_test PRIVATE pthread)
endif()

# Container system Phase 1 tests — needs QApplication + Widgets.
add_executable(container_widget_test
    tests/container_widget_test.cpp
    src/gui/FramelessResizer.cpp
    src/gui/containers/ContainerTitleBar.cpp
    src/gui/containers/ContainerWidget.cpp
    src/gui/containers/FloatingContainerWindow.cpp
    src/core/AppSettings.cpp
)
target_include_directories(container_widget_test PRIVATE src)
target_link_libraries(container_widget_test PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Test
)
set_target_properties(container_widget_test PROPERTIES AUTOMOC ON)

add_executable(container_manager_test
    tests/container_manager_test.cpp
    src/gui/FramelessResizer.cpp
    src/gui/containers/ContainerManager.cpp
    src/gui/containers/ContainerTitleBar.cpp
    src/gui/containers/ContainerWidget.cpp
    src/gui/containers/FloatingContainerWindow.cpp
    src/core/AppSettings.cpp
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
    src/core/AppSettings.cpp
)
target_include_directories(container_nesting_test PRIVATE src)
target_link_libraries(container_nesting_test PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Test
)
set_target_properties(container_nesting_test PROPERTIES AUTOMOC ON)


# ── Install rules ────────────────────────────────────────────────────────────
include(GNUInstallDirs)

if(APPLE)
    install(TARGETS AetherSDR
        BUNDLE DESTINATION .
    )
else()
    install(TARGETS AetherSDR
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(FILES AetherSDR.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
    )
    install(FILES docs/logo-circle.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps
        RENAME aethersdr.png
    )
endif()
