# Embedded HD FM dependencies. Upstream build files are retained as provenance
# only: they include download fallbacks, CLI/shared targets and install hooks.
include_guard(GLOBAL)

option(ENABLE_HD_FM "Build experimental embedded HD FM (Linux GNU C only)" OFF)
if(NOT ENABLE_HD_FM)
    return()
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR
        "ENABLE_HD_FM currently supports qualification builds on Linux with GNU C only. "
        "macOS, MSVC, MinGW and Clang have not been qualified. Use -DENABLE_HD_FM=OFF.")
endif()
if(NOT AETHER_BACKEND_RTL)
    message(FATAL_ERROR "ENABLE_HD_FM requires the enabled RTL backend and its existing dependencies.")
endif()
foreach(dependency_variable RTLSDR_TARGET RTL_FFTW3F_TARGET)
    if(NOT DEFINED ${dependency_variable} OR "${${dependency_variable}}" STREQUAL "")
        message(FATAL_ERROR "ENABLE_HD_FM requires an existing ${dependency_variable}; no download fallback is provided.")
    endif()
    if(NOT TARGET "${${dependency_variable}}")
        message(FATAL_ERROR "ENABLE_HD_FM dependency target ${${dependency_variable}} does not exist.")
    endif()
endforeach()

# Scope feature checks and upstream config names to this function. Do not alter
# the parent project's C dialect, optimization flags or generic BUILD_CLI cache.
function(aether_add_hd_fm_dependencies)
    include(CheckSymbolExists)
    include(CheckCSourceCompiles)
    find_package(Threads REQUIRED)
    find_library(AETHER_HD_MATH_LIBRARY NAMES m REQUIRED)
    get_filename_component(vendor_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party" ABSOLUTE)
    set(nrsc5_root "${vendor_root}/nrsc5/upstream")
    set(faad_root "${vendor_root}/faad_hdc/upstream")
    set(config_root "${CMAKE_CURRENT_BINARY_DIR}/aether-hd-fm")

    set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
    set(CMAKE_REQUIRED_LIBRARIES "${AETHER_HD_MATH_LIBRARY}")
    string(APPEND CMAKE_REQUIRED_FLAGS " -std=gnu11")
    check_c_source_compiles("#include <complex.h>\nint main(void) { float complex z = 1.0f + 2.0f * I; return crealf(z) != 1.0f; }"
        AETHER_NRSC5_HAVE_C_COMPLEX)
    if(NOT AETHER_NRSC5_HAVE_C_COMPLEX)
        message(FATAL_ERROR "ENABLE_HD_FM requires working GNU C11 complex arithmetic.")
    endif()
    check_symbol_exists(strndup "string.h" AETHER_NRSC5_HAVE_STRNDUP)
    check_symbol_exists(CMPLXF "complex.h" AETHER_NRSC5_HAVE_CMPLXF)
    check_symbol_exists(_Imaginary_I "complex.h" AETHER_NRSC5_HAVE_IMAGINARY_I)
    check_symbol_exists(_Complex_I "complex.h" AETHER_NRSC5_HAVE_COMPLEX_I)
    check_symbol_exists(lrintf "math.h" AETHER_FAAD_HAVE_LRINTF)

    set(USE_FAAD2 ON)
    set(HAVE_STRNDUP "${AETHER_NRSC5_HAVE_STRNDUP}")
    set(HAVE_CMPLXF "${AETHER_NRSC5_HAVE_CMPLXF}")
    set(HAVE_IMAGINARY_I "${AETHER_NRSC5_HAVE_IMAGINARY_I}")
    set(HAVE_COMPLEX_I "${AETHER_NRSC5_HAVE_COMPLEX_I}")
    # Upstream level 5 disables its synchronous stderr logging. Reception and
    # processing health are surfaced by the typed wrapper, not unbounded logs.
    set(LIBRARY_DEBUG_LEVEL 5)
    configure_file("${nrsc5_root}/src/config.h.in" "${config_root}/config.h" @ONLY)

    # Exactly the patched floating-point HDC decoder. No ordinary/DRM/fixed
    # variants, frontend, shared library, downloader or install target is added.
    set(faad_sources
        bits.c cfft.c common.c decoder.c drc.c drm_dec.c error.c filtbank.c
        hcr.c huffman.c ic_predict.c is.c lt_predict.c mdct.c mp4.c ms.c
        output.c pns.c ps_dec.c ps_syntax.c pulse.c rvlc.c sbr_dct.c sbr_dec.c
        sbr_e_nf.c sbr_fbt.c sbr_hfadj.c sbr_hfgen.c sbr_huff.c sbr_qmf.c
        sbr_syntax.c sbr_tf_grid.c specrec.c ssr.c ssr_fb.c ssr_ipqf.c syntax.c tns.c)
    list(TRANSFORM faad_sources PREPEND "${faad_root}/libfaad/")
    add_library(aether_faad_hdc STATIC ${faad_sources})
    target_include_directories(aether_faad_hdc SYSTEM PUBLIC "${faad_root}/include")
    target_include_directories(aether_faad_hdc PRIVATE "${faad_root}/libfaad")
    target_compile_definitions(aether_faad_hdc PRIVATE
        HDC_SUPPORT APPLY_DRC HAVE_INTTYPES_H=1 HAVE_MEMCPY=1 HAVE_STRING_H=1
        HAVE_STRINGS_H=1 HAVE_SYS_STAT_H=1 HAVE_SYS_TYPES_H=1 PACKAGE_VERSION="2.11.2")
    if(AETHER_FAAD_HAVE_LRINTF)
        target_compile_definitions(aether_faad_hdc PRIVATE HAVE_LRINTF=1)
    endif()
    # Match the pinned FAAD GNU compiler setting, without changing other targets.
    target_compile_options(aether_faad_hdc PRIVATE -ffloat-store)
    target_link_libraries(aether_faad_hdc PRIVATE "${AETHER_HD_MATH_LIBRARY}")

    # BUILD_CLI=OFF by construction: neither upstream CMakeLists is evaluated.
    # rtltcp/device symbols remain upstream-identical but Aether's wrapper uses
    # only open_pipe/pipe_samples_cf32/close; it is the sole USB capture owner.
    set(nrsc5_sources
        acquire.c decode.c frame.c here_images.c input.c nrsc5.c output.c
        pids.c rtltcp.c sync.c firdecim_cf32.c conv_dec.c rs_init.c rs_decode.c
        unicode.c strndup.c)
    list(TRANSFORM nrsc5_sources PREPEND "${nrsc5_root}/src/")
    add_library(aether_nrsc5 STATIC ${nrsc5_sources})
    target_include_directories(aether_nrsc5 SYSTEM PUBLIC "${nrsc5_root}/include")
    target_include_directories(aether_nrsc5 PRIVATE "${config_root}")
    target_compile_definitions(aether_nrsc5 PRIVATE
        _GNU_SOURCE GIT_COMMIT_HASH="0225922b6f68109df39d07391f4d855464598ab8")
    target_compile_definitions(aether_nrsc5 INTERFACE AETHER_ENABLE_NRSC5=1)
    target_link_libraries(aether_nrsc5 PRIVATE aether_faad_hdc
        "${RTL_FFTW3F_TARGET}" "${RTLSDR_TARGET}" Threads::Threads "${AETHER_HD_MATH_LIBRARY}")

    set_target_properties(aether_faad_hdc aether_nrsc5 PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON
        POSITION_INDEPENDENT_CODE ON AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    message(STATUS "HD FM ... experimental pinned nrsc5 + HDC enabled; embedded pipe API, CLI OFF")
endfunction()

aether_add_hd_fm_dependencies()
