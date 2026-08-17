# ── AetherSDR test registration ──────────────────────────────────────────────
#
# Every test target in the project is declared here. Split out of the root
# CMakeLists.txt, which this cut roughly in half.
#
# Two things in here are NOT tests: ax25_replay and ax25_session_analyze are
# EXCLUDE_FROM_ALL analysis tools. They moved with their neighbourhood rather
# than being singled out — they sit in AETHER_SETTINGS_CONSUMERS alongside the
# test targets, and separating them would have made the move non-verbatim for
# no gain. If a third such tool appears, that is the point to reconsider
# whether they want a file of their own.
#
# Pulled in by `include(tests/tests.cmake)` from the root file, NOT by
# add_subdirectory(). That is deliberate and load-bearing:
#
#   include() executes in the CALLER's directory scope, so CMAKE_CURRENT_SOURCE_DIR
#   is still the repository root here. Every relative path below — tests/foo.cpp,
#   src/gui/Bar.cpp, target_include_directories(... PRIVATE src) — resolves exactly
#   as it did when these lines lived in the root file, and the ten
#   ${CMAKE_CURRENT_SOURCE_DIR} references in this file (tools/*.py,
#   docs/automation/*.csv, AETHER_SOURCE_DIR) still point at the repo root.
#
#   Under add_subdirectory(tests) all of those would silently re-root to
#   <repo>/tests. The ~800 source paths would fail loudly, which is survivable;
#   the ${CMAKE_CURRENT_SOURCE_DIR} ones would fail QUIETLY — an env var handed to
#   a passing test pointing one directory too deep. That is the failure mode this
#   choice avoids, and the reason not to "tidy" it into add_subdirectory() later.
#
# So: write paths here relative to the REPOSITORY ROOT, exactly as you would have
# in the root CMakeLists.txt — `tests/my_new_test.cpp`, not `my_new_test.cpp`.
#
# Adding a test: drop <feature>_test.cpp into tests/ and declare it here. There is
# no glob; every test is declared explicitly. Copy a neighbouring target's block.
# See tests/README.md.

# ── Guard: no test registration in the root CMakeLists.txt ───────────────────
# Fails the configure step if a test target is declared in the root file, so that
# anyone reaching for the old location finds out immediately — with a message that
# names the right one — rather than at review time, or not at all.
#
# This scans the root listfile as text rather than overriding add_test(). An
# override cannot work here: include() keeps this file in the root's directory
# scope, so a command override could not tell the two files apart. Scanning from
# THIS file (rather than from the root file scanning itself) is also what keeps
# the patterns below from matching their own source text.
# CMAKE_CURRENT_LIST_DIR, not CMAKE_SOURCE_DIR: this resolves relative to THIS
# file, so it keeps pointing at the right listfile if the project is ever
# consumed from a superproject, where CMAKE_SOURCE_DIR is the parent's.
file(READ "${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt" _aether_root_listfile)
string(REGEX REPLACE "#[^\n]*" "" _aether_root_code "${_aether_root_listfile}")
string(REGEX MATCHALL "add_executable[ \t]*\\([A-Za-z0-9_-]+_test"
       _aether_stray_targets "${_aether_root_code}")
string(REGEX MATCHALL "add_test[ \t]*\\(" _aether_stray_registrations "${_aether_root_code}")
if(_aether_stray_targets OR _aether_stray_registrations)
    set(_aether_strays ${_aether_stray_targets} ${_aether_stray_registrations})
    list(JOIN _aether_strays ", " _aether_stray_list)
    message(FATAL_ERROR
        "Test registration found in the root CMakeLists.txt: ${_aether_stray_list}\n"
        "Every test target is declared in tests/tests.cmake. Move the block there "
        "verbatim — paths in that file are still relative to the repository root, "
        "so nothing needs rewriting. See tests/README.md.")
endif()
unset(_aether_root_listfile)
unset(_aether_root_code)
unset(_aether_stray_targets)
unset(_aether_stray_registrations)


# ── Digital-voice / D-STAR tests ─────────────────────────────────────────────
# Guarded by the same condition as the aether-dv-waveform target they exercise.
# DIGITAL_VOICE_WAVEFORM_DIR, CRDV_DIR and crdv::crdv are all defined by the time
# the root file reaches its include() of this one.
if((UNIX OR WIN32) AND ENABLE_DSTAR)
    add_test(NAME aether_dv_waveform_no_args
        COMMAND ${CMAKE_COMMAND}
            -DHELPER=$<TARGET_FILE:aether-dv-waveform>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_digital_voice_waveform_no_args.cmake
    )

    # These targets opt IN to ASan+UBSan locally, so a developer running them
    # by hand gets the checks without configuring anything.
    #
    # STAND DOWN when the build is already being compiled under a sanitizer of
    # its own. ASan and TSan are mutually exclusive — `cc1plus: error:
    # '-fsanitize=thread' is incompatible with '-fsanitize=address'` — so
    # adding ASan unconditionally here breaks the TSan CI job at COMPILE time,
    # and it breaks it for the whole repo, not just these 13 targets: the job
    # never gets far enough to run anything. That is what took thread-sanitizer
    # coverage to zero for ten consecutive weekly runs (issue #4360).
    #
    # Keyed off the flags actually arriving from the environment, but matched
    # against a DELIBERATELY NARROW list of sanitizer names — see the last
    # paragraph for why it cannot be "any sanitizer". The list is thread,
    # memory and hwaddress: the three that cannot coexist with address. A new
    # sanitizer that also conflicts has to be added here; the alternation is
    # the whole contract, so keep it and the message below in step.
    #
    # BOTH language flags are checked. Most of these targets are C, but Qt's
    # AUTOMOC generates a C++ TU (mocs_compilation.cpp) for each of them —
    # CMAKE_AUTOMOC is ON globally — and that generated file is the one CI
    # actually dies on, compiled by c++ with CMAKE_CXX_FLAGS. Today's workflow
    # sets only CXXFLAGS, so CXX alone would be enough; checking CFLAGS too
    # means a job that sets only CFLAGS does not quietly reintroduce this.
    #
    # ONLY A CONFLICTING SANITIZER DISARMS THIS, not any sanitizer at all.
    # The distinction is load-bearing and the naive test gets it backwards:
    # sanitizers.yml sets CXXFLAGS but never CFLAGS, so on the ASAN job a
    # blanket "an external sanitizer is present" test would stand this opt-in
    # down and nothing would replace it for the C sources — the .c files would
    # silently lose the ASan coverage this helper exists to give them, in the
    # one job that currently reports real results. ASan+UBSan arriving from the
    # environment is what we add anyway, so there is nothing to stand down for;
    # only a sanitizer that cannot coexist with address (thread, memory,
    # hwaddress) forces the retreat.
    #
    # `[a-z,]*` matches neither `-` nor `=` nor space, which is what keeps the
    # alternation from over-reaching: `-fno-sanitize=thread` has no
    # `-fsanitize=` substring, `-fsanitize=kernel-address` stops at the hyphen,
    # and `-fsanitize=address` does not contain `hwaddress`. Reordered lists
    # (`-fsanitize=undefined,thread`) still match.
    set(_aether_dv_external_sanitizer OFF)
    foreach(_aether_dv_flags "${CMAKE_CXX_FLAGS}" "${CMAKE_C_FLAGS}")
        if(_aether_dv_flags MATCHES "-fsanitize=[a-z,]*(thread|memory|hwaddress)")
            set(_aether_dv_external_sanitizer ON)
        endif()
    endforeach()
    if(_aether_dv_external_sanitizer)
        message(STATUS
            "Digital-voice tests: a conflicting sanitizer "
            "(thread/memory/hwaddress) is in the C/CXX flags — not adding "
            "ASan+UBSan, they cannot coexist")
    endif()
    # Cached so the function does not depend on its caller's scope: today every
    # call site is in this file, but a function reading a plain variable set
    # elsewhere would silently re-arm if it were ever called from another
    # directory scope.
    set(AETHER_DV_EXTERNAL_SANITIZER "${_aether_dv_external_sanitizer}"
        CACHE INTERNAL "A conflicting sanitizer arrived in CMAKE_{C,CXX}_FLAGS")

    function(aether_enable_digital_voice_test_sanitizers target)
        if(AETHER_DV_EXTERNAL_SANITIZER)
            return()
        endif()
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
endif()


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

add_executable(icom_civ_scheduler_test
    tests/icom_civ_scheduler_test.cpp
    src/core/backends/icom/IcomCivScheduler.cpp
    src/core/backends/icom/CivCodec.cpp)
target_include_directories(icom_civ_scheduler_test PRIVATE src)
add_test(NAME icom_civ_scheduler_test COMMAND icom_civ_scheduler_test)

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

# IcomCIV live CI-V ADDRESS probe — also REQUIRES A REAL RADIO, also EXCLUDE_FROM_ALL.
# Drives IcomCivBackend (not IcomSession) because the address resolution under
# test lives there, and reports ALIVE/DEAD on whether a frequency ever arrived —
# a wrong CI-V address fails silently, so "it connected" proves nothing.
#   cmake --build build --target icom_live_civ_probe
#   ICOM_USER=.. ICOM_PW=.. ./build/icom_live_civ_probe 172.17.0.96          # auto
#   ICOM_USER=.. ICOM_PW=.. ./build/icom_live_civ_probe 172.17.0.96 A4 pin   # typed
# Credentials come from the environment, never argv: argv is world-readable
# through /proc for the life of the process.
add_executable(icom_live_civ_probe EXCLUDE_FROM_ALL tests/icom_live_civ_probe.cpp)
target_include_directories(icom_live_civ_probe PRIVATE src)
target_link_libraries(icom_live_civ_probe PRIVATE aethercore Qt6::Core Qt6::Network)

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

# The host-side impulse noise blanker (WDSP ANB) ahead of the demodulator. The
# HL2 runs no firmware DSP, so this stage is the only noise blanker the radio
# has and there is no wire traffic to assert against — the test measures the
# audio instead.
add_executable(hl2_noise_blanker_test tests/hl2_noise_blanker_test.cpp)
target_include_directories(hl2_noise_blanker_test PRIVATE src)
target_link_libraries(hl2_noise_blanker_test PRIVATE aethercore Qt6::Core Qt6::Test)
add_test(NAME hl2_noise_blanker_test COMMAND hl2_noise_blanker_test)

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

# CW BFO geometry: the marker sits on the signal, the pitch comes from the
# detector's zero. Companion to hl2_shift_test — same synthetic-tone harness,
# one more offset in the chain.
add_executable(hl2_cw_bfo_test tests/hl2_cw_bfo_test.cpp)
target_include_directories(hl2_cw_bfo_test PRIVATE src)
target_link_libraries(hl2_cw_bfo_test PRIVATE aethercore Qt6::Core Qt6::Test)
add_test(NAME hl2_cw_bfo_test COMMAND hl2_cw_bfo_test)

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

# Slippy-map cylindrical-world math plus QGraphicsView camera constraints.
# Drives a real QGVMap, so it needs the offscreen platform; never touches the
# tile network.
add_executable(map_wrap_test tests/map_wrap_test.cpp)
target_link_libraries(map_wrap_test PRIVATE
    qgeoview
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
)
add_test(NAME map_wrap_test COMMAND map_wrap_test)
set_tests_properties(map_wrap_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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

# Workspace canvas (RFC #4887) phase 1 — normalized geometry.  Pure logic, no
# widgets: the edge-rounding rule that keeps tiled items seam-free, and the
# resolution independence the whole RFC rests on, are pinned on every platform.
add_executable(workspace_geometry_test
    tests/workspace_geometry_test.cpp
    src/gui/workspace/WorkspaceGeometry.cpp
)
target_include_directories(workspace_geometry_test PRIVATE src)
target_link_libraries(workspace_geometry_test PRIVATE Qt6::Core)
add_test(NAME workspace_geometry_test COMMAND workspace_geometry_test)

# Workspace canvas (RFC #4887) phase 1 — the canvas model: membership,
# placement clamping, hit testing, and the dense-contiguous z invariant that
# keeps raise/lower working after arbitrarily many operations.
add_executable(workspace_layout_test
    tests/workspace_layout_test.cpp
    src/gui/workspace/CanvasLayout.cpp
    src/gui/workspace/WorkspaceGeometry.cpp
)
target_include_directories(workspace_layout_test PRIVATE src)
target_link_libraries(workspace_layout_test PRIVATE Qt6::Core)
add_test(NAME workspace_layout_test COMMAND workspace_layout_test)

# Workspace canvas (RFC #4887) phase 1 — the widget half, offscreen: model
# answers applied to real geometry and real Qt stacking, plus the take-vs-remove
# ownership contract phase 3 depends on.
add_executable(workspace_canvas_widget_test
    tests/workspace_canvas_widget_test.cpp
    src/gui/workspace/CanvasInteraction.cpp
    src/gui/workspace/CanvasItemFrame.cpp
    src/gui/workspace/CanvasLayout.cpp
    src/gui/workspace/WorkspaceCanvas.cpp
    src/gui/workspace/WorkspaceGeometry.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/ThemeManager.cpp
    src/core/ThemeSeedGenerated.cpp
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
)
target_include_directories(workspace_canvas_widget_test PRIVATE src)
target_link_libraries(workspace_canvas_widget_test PRIVATE Qt6::Widgets Qt6::Test)
add_test(NAME workspace_canvas_widget_test COMMAND workspace_canvas_widget_test)
set_tests_properties(workspace_canvas_widget_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Workspace canvas (RFC #4887) phase 2 — the document schema.  Pure logic: the
# newer-schema guard that refuses to re-write what a later build wrote, and the
# boundary validation that repairs what it can and reports every repair.
add_executable(workspace_document_test
    tests/workspace_document_test.cpp
    src/gui/workspace/WorkspaceDocument.cpp
    src/gui/workspace/WorkspaceGeometry.cpp
)
target_include_directories(workspace_document_test PRIVATE src)
target_link_libraries(workspace_document_test PRIVATE Qt6::Core)
add_test(NAME workspace_document_test COMMAND workspace_document_test)

# Workspace canvas (RFC #4887) phase 2 — Classic geometry and the one-way
# migration off the legacy layout keys, against a real AppSettings in a
# temporary home.  Pins that every pan layout id tiles the surface exactly, and
# that migration leaves floating pans and applets alone (RFC decision 1).
add_executable(workspace_migration_test
    tests/workspace_migration_test.cpp
    src/gui/workspace/ClassicLayout.cpp
    src/gui/workspace/WorkspaceDocument.cpp
    src/gui/workspace/WorkspaceGeometry.cpp
    src/gui/workspace/WorkspaceMigration.cpp
)
target_include_directories(workspace_migration_test PRIVATE src tests)
target_link_libraries(workspace_migration_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(workspace_migration_test PROPERTIES AUTOMOC ON)
add_test(NAME workspace_migration_test COMMAND workspace_migration_test)

# Workspace canvas (RFC #4887) phase 2 — persistence and the auto-commit
# contract (decision 7): gestures coalesce into one whole-document write, the
# write is verified against the FILE rather than the settings cache, and a
# restore replay never writes back what it is reading (#4427).
add_executable(workspace_store_test
    tests/workspace_store_test.cpp
    src/gui/workspace/ClassicLayout.cpp
    src/gui/workspace/WorkspaceDocument.cpp
    src/gui/workspace/WorkspaceGeometry.cpp
    src/gui/workspace/WorkspaceMigration.cpp
    src/gui/workspace/WorkspaceStore.cpp
)
target_include_directories(workspace_store_test PRIVATE src tests)
target_link_libraries(workspace_store_test PRIVATE aethercore Qt6::Core Qt6::Test)
set_target_properties(workspace_store_test PROPERTIES AUTOMOC ON)
add_test(NAME workspace_store_test COMMAND workspace_store_test)

# Workspace canvas (RFC #4887) phase 5 — the pure interaction core: hit
# zones, anchored resize (the anchored edge must never move), and the snap
# solver (snaps only the gripped edges, never below the minimum).
add_executable(workspace_interaction_test
    tests/workspace_interaction_test.cpp
    src/gui/workspace/CanvasInteraction.cpp
    src/gui/workspace/WorkspaceGeometry.cpp
)
target_include_directories(workspace_interaction_test PRIVATE src)
target_link_libraries(workspace_interaction_test PRIVATE Qt6::Core Qt6::Gui)
add_test(NAME workspace_interaction_test COMMAND workspace_interaction_test)

# Workspace canvas (RFC #4887) phase 3 — DockMode::Canvas at the manager
# level: slot-preserving detach/return, float-docks-first, the evictor
# routing, width-cap lift (#3451 on canvas), and a stored "canvas" mode
# restoring panel-docked.
add_executable(workspace_container_mode_test
    tests/workspace_container_mode_test.cpp
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
target_include_directories(workspace_container_mode_test PRIVATE src tests)
target_link_libraries(workspace_container_mode_test PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Test
)
set_target_properties(workspace_container_mode_test PROPERTIES AUTOMOC ON)
add_test(NAME workspace_container_mode_test COMMAND workspace_container_mode_test)
set_tests_properties(workspace_container_mode_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Workspace canvas (RFC #4887) phase 3 — the controller end to end: first
# enable migrates and places, explicit returns forget the canvas home while
# closes keep it, drops move/place through the canvas's real event path, and
# an unusable store fails the enable without moving a widget.
add_executable(workspace_controller_test
    tests/workspace_controller_test.cpp
    src/gui/FramelessResizer.cpp
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
    ${AETHER_SETTINGS_SOURCES}
    src/core/ThemeManager.cpp
    src/core/ThemeSeedGenerated.cpp
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
)
target_include_directories(workspace_controller_test PRIVATE src tests)
target_link_libraries(workspace_controller_test PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Test
)
set_target_properties(workspace_controller_test PROPERTIES AUTOMOC ON)
add_test(NAME workspace_controller_test COMMAND workspace_controller_test)
set_tests_properties(workspace_controller_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Demo RX engine on a worker thread (#4878) — the generator's thread affinity,
# 24 kHz long-run pacing on a coarse timer, keyed-mute, stallscope, and queued
# controls. Pure QObject + event loop; no widgets, no radio.
add_executable(sim_signal_source_test
    tests/sim_signal_source_test.cpp
    src/core/backends/sim/SimSignalSource.cpp
    src/core/backends/sim/NoiseMixer.cpp
)
target_include_directories(sim_signal_source_test PRIVATE src)
target_link_libraries(sim_signal_source_test PRIVATE Qt6::Core Qt6::Test)
set_target_properties(sim_signal_source_test PROPERTIES AUTOMOC ON)
add_test(NAME sim_signal_source_test COMMAND sim_signal_source_test)

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
    src/core/TxCaptureBuffer.cpp
    src/core/TxMicChannelNormalizer.cpp
    src/core/Resampler.cpp
)
target_include_directories(tx_mic_channel_normalizer_test PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/third_party/r8brain
)
target_link_libraries(tx_mic_channel_normalizer_test PRIVATE Qt6::Core)
add_test(NAME tx_mic_channel_normalizer_test COMMAND tx_mic_channel_normalizer_test)

add_executable(tx_voice_processor_test
    tests/tx_voice_processor_test.cpp
)
target_include_directories(tx_voice_processor_test PRIVATE src)
target_link_libraries(tx_voice_processor_test PRIVATE aethercore Qt6::Core)
add_test(NAME tx_voice_processor_test COMMAND tx_voice_processor_test)

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

# Floating-panadapter crash-loop guard (#4617) — pins that a session which died
# inside floatPanadapter() comes up docked instead of replaying the crash.
add_executable(floating_restore_policy_test
    tests/floating_restore_policy_test.cpp
)
target_include_directories(floating_restore_policy_test PRIVATE src)
add_test(NAME floating_restore_policy_test COMMAND floating_restore_policy_test)

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

# SpotHub's newest-spot-follows-the-viewport decision (#4889), header-only
# and dependency-free by design so it's testable without DxClusterDialog's
# full client/model dependency graph.
add_executable(spot_auto_scroll_test
    tests/spot_auto_scroll_test.cpp
)
target_include_directories(spot_auto_scroll_test PRIVATE src)
target_link_libraries(spot_auto_scroll_test PRIVATE Qt6::Core)
add_test(NAME spot_auto_scroll_test COMMAND spot_auto_scroll_test)

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

add_executable(vkamp_protocol_test
    tests/vkamp_protocol_test.cpp
    src/core/VkampProtocol.cpp
)
target_include_directories(vkamp_protocol_test PRIVATE src)
target_link_libraries(vkamp_protocol_test PRIVATE Qt6::Core)
add_test(NAME vkamp_protocol_test COMMAND vkamp_protocol_test)

# VkampConnection against a stub amp on loopback -- the transport behaviour
# the pure-codec test above can't reach: the bypass/voltage safety interlock,
# the reset hold's exclusive claim on the wire, the command rate limiter,
# hostname resolution for the UDP telemetry port, and TX-gated telemetry
# expiry. Isolated settings dir per the CMake contract: LogManager pulls in
# AppSettings.
add_executable(vkamp_connection_test
    tests/vkamp_connection_test.cpp
    src/core/VkampConnection.cpp
    src/core/VkampProtocol.cpp
    ${AETHER_SETTINGS_SOURCES}
    src/core/LogManager.cpp
    src/core/AsyncLogWriter.cpp
)
target_include_directories(vkamp_connection_test PRIVATE src tests)
target_link_libraries(vkamp_connection_test PRIVATE Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME vkamp_connection_test COMMAND vkamp_connection_test)
set_tests_properties(vkamp_connection_test PROPERTIES TIMEOUT 120)

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
    add_test(NAME tx_meter_safety
             COMMAND ${PYTHON3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_tx_meter_test.py)
    # Argument parsing for the logwatch helper (#4912) — blind rest[0]/rest[1]
    # indexing turned a typo into an IndexError traceback.
    add_test(NAME automation_logwatch_arguments
             COMMAND ${PYTHON3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_automation_logwatch.py)
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

# `connect ip` family resolution + the `family` field on `connect list` (#4912).
# Pure verb-level test against a fake IConnectionAutomation — no socket, no radio.
add_executable(automation_connect_family_test
    tests/automation_connect_family_test.cpp
)
target_include_directories(automation_connect_family_test PRIVATE src tests)
target_link_libraries(automation_connect_family_test PRIVATE
    aethercore Qt6::Core Qt6::Network
)
add_test(NAME automation_connect_family_test COMMAND automation_connect_family_test)

# `connect wait` phase reporting + deferred connect-failure delivery (#4912).
add_executable(automation_connect_wait_phase_test
    tests/automation_connect_wait_phase_test.cpp
)
target_include_directories(automation_connect_wait_phase_test PRIVATE src tests)
target_link_libraries(automation_connect_wait_phase_test PRIVATE
    aethercore Qt6::Core Qt6::Network
)
add_test(NAME automation_connect_wait_phase_test
         COMMAND automation_connect_wait_phase_test)

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

# wirePanadapter() can see the placeholder SpectrumWidget more than once. The
# helper keeps one timer/path per widget while retaining separate per-pan timers.
add_executable(owned_single_shot_timer_test tests/owned_single_shot_timer_test.cpp)
target_include_directories(owned_single_shot_timer_test PRIVATE src)
target_link_libraries(owned_single_shot_timer_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME owned_single_shot_timer_test COMMAND owned_single_shot_timer_test)

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

# CAT/rigctld retune policy (#4497). The pan recenter is radio-side and the only
# lever is the autopan=0 flag on "slice tune", which the CAT integration suites
# cannot observe — reverting the recenter arm leaves all three of them green. So
# the in-span predicate, the seam's rejections, and the fact that an out-of-span
# target really reaches tuneAndRecenter are pinned here instead.
add_executable(cat_tune_policy_test tests/cat_tune_policy_test.cpp)
target_include_directories(cat_tune_policy_test PRIVATE src)
target_link_libraries(cat_tune_policy_test PRIVATE aethercore Qt6::Core Qt6::Test)
add_test(NAME cat_tune_policy_test COMMAND cat_tune_policy_test)

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

# FramelessResizer's clampManualResize()/windowOwnsChain() pure-logic
# helpers (#4827/#4829 review). offscreen never exercises the real manual
# resize path end-to-end (see FramelessResizer.h), so this is coverage for
# the arithmetic and parent-chain matching in isolation, not a replacement
# for the PR's own real-X11-input proof.
#
# Compiled and linked by the Linux build job, but not currently in any of
# ci.yml's named ctest -R filters for the per-PR gate — CI building it
# without running it (per-PR) is the only thing exercising it today; the
# weekly sanitizers job is the sole scheduled `ctest` run that includes it
# by not filtering, and per ci.yml that job has failed every run since
# 2026-06-08. Pure arithmetic and four bare QWindows, no widgets/sockets/
# wall clock, milliseconds to run — a reasonable candidate for one of the
# per-PR filters, but that's a maintainer call on the gate's scope, not
# this PR's to make unilaterally.
add_executable(frameless_resizer_test
    tests/frameless_resizer_test.cpp
    src/gui/FramelessResizer.cpp
)
target_include_directories(frameless_resizer_test PRIVATE src tests)
target_link_libraries(frameless_resizer_test PRIVATE
    aethercore Qt6::Core Qt6::Network Qt6::Widgets Qt6::Test
)
set_target_properties(frameless_resizer_test PROPERTIES AUTOMOC ON)
add_test(NAME frameless_resizer_test COMMAND frameless_resizer_test)
set_tests_properties(frameless_resizer_test PROPERTIES
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
add_test(NAME device_diagnostics_test COMMAND device_diagnostics_test)

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

add_executable(phone_cw_mic_gain_authority_test
    tests/phone_cw_mic_gain_authority_test.cpp
    src/gui/PhoneCwApplet.cpp
    src/gui/DragValuePopup.cpp
)
target_include_directories(phone_cw_mic_gain_authority_test PRIVATE src)
target_link_libraries(phone_cw_mic_gain_authority_test PRIVATE
    aethercore Qt6::Core Qt6::Widgets Qt6::Test
)
set_target_properties(phone_cw_mic_gain_authority_test PROPERTIES AUTOMOC ON)
add_test(NAME phone_cw_mic_gain_authority_test
         COMMAND phone_cw_mic_gain_authority_test)
set_tests_properties(phone_cw_mic_gain_authority_test PROPERTIES
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
    workspace_canvas_widget_test
    workspace_container_mode_test
    workspace_controller_test
    mini_pan_widget_test
    log_manager_filter_rules_test
    bandplan_voice_labels_test
    vkamp_connection_test
)
foreach(_settings_consumer IN LISTS AETHER_SETTINGS_CONSUMERS)
    if(TARGET ${_settings_consumer})
        target_link_libraries(${_settings_consumer} PRIVATE aether_sqlite3)
    endif()
endforeach()
