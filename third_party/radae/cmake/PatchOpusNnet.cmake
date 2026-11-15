# PatchOpusNnet.cmake — Prepare a vendored Opus tree for RADE.
# This applies the RADE_EXPORT patch to Opus dnn/nnet.h and can also extract
# the Opus neural model payload when refreshing `third_party/opus-rade/`.
# Normal AetherSDR builds do not invoke this script; they consume the prepared
# vendored archive directly via BuildOpus.cmake.

set(NNET_H "${SOURCE_DIR}/dnn/nnet.h")
file(READ "${NNET_H}" content)

# 1. Add RADE_EXPORT fallback definition after #define NNET_H_
# MSVC: __attribute__ not supported; for static lib, RADE_EXPORT is empty
string(REPLACE
    "#define NNET_H_\n"
    "#define NNET_H_\n\n#ifndef RADE_EXPORT\n#ifdef _MSC_VER\n#define RADE_EXPORT\n#else\n#define RADE_EXPORT __attribute__((visibility(\"default\")))\n#endif\n#endif /* RADE_EXPORT */\n"
    content "${content}")

# 2. Add RADE_EXPORT to function declarations
string(REPLACE
    "void compute_generic_dense("
    "void RADE_EXPORT compute_generic_dense("
    content "${content}")
string(REPLACE
    "void compute_generic_gru("
    "void RADE_EXPORT compute_generic_gru("
    content "${content}")
string(REPLACE
    "void compute_generic_conv1d("
    "void RADE_EXPORT compute_generic_conv1d("
    content "${content}")
string(REPLACE
    "void compute_generic_conv1d_dilation("
    "void RADE_EXPORT compute_generic_conv1d_dilation("
    content "${content}")
string(REPLACE
    "void compute_glu("
    "void RADE_EXPORT compute_glu("
    content "${content}")
string(REPLACE
    "void compute_gated_activation("
    "void RADE_EXPORT compute_gated_activation("
    content "${content}")
string(REPLACE
    "int parse_weights("
    "int RADE_EXPORT parse_weights("
    content "${content}")
string(REPLACE
    "int linear_init("
    "int RADE_EXPORT linear_init("
    content "${content}")
string(REPLACE
    "int conv2d_init("
    "int RADE_EXPORT conv2d_init("
    content "${content}")

file(WRITE "${NNET_H}" "${content}")
message(STATUS "Patched ${NNET_H} with RADE_EXPORT attributes")

# ── Download and extract neural network model data ───────────────────────
# The autotools build does this via autogen.sh; the CMake build needs it
# done explicitly.  The tarball contains generated *_data.h / *_data.c files
# (fargan_data.h, plc_data.h, lace_data.h, etc.) that carry the trained
# neural network weights.
set(MODEL_HASH "4ed9445b96698bad25d852e912b41495ddfa30c8dbc8a55f9cde5826ed793453")
set(MODEL_TAR  "opus_data-${MODEL_HASH}.tar.gz")
set(MODEL_PATH "${SOURCE_DIR}/${MODEL_TAR}")

if(NOT EXISTS "${SOURCE_DIR}/dnn/fargan_data.h")
    if(NOT EXISTS "${MODEL_PATH}")
        message(STATUS "Downloading Opus neural network model data...")
        file(DOWNLOAD
            "https://media.xiph.org/opus/models/${MODEL_TAR}"
            "${MODEL_PATH}"
            SHOW_PROGRESS
            STATUS download_status)
        list(GET download_status 0 download_code)
        if(NOT download_code EQUAL 0)
            message(FATAL_ERROR
                "Failed to download Opus neural net model from media.xiph.org.\n"
                "You can download it manually:\n"
                "  curl -o ${MODEL_PATH} https://media.xiph.org/opus/models/${MODEL_TAR}\n"
                "Then re-run cmake --build.")
        endif()
    endif()
    message(STATUS "Extracting Opus model data...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf "${MODEL_PATH}"
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE extract_result)
    if(NOT extract_result EQUAL 0)
        message(FATAL_ERROR "Failed to extract Opus model data")
    endif()
    message(STATUS "Opus model data extracted")
endif()
