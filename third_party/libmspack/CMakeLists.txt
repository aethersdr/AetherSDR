# Vendored subset of libmspack 0.11alpha (LGPL-2.1).
# See README.md in this directory for what's included and why.

add_library(mspack_static STATIC
    mspack/system.c
    mspack/cabd.c
    mspack/mszipd.c
    mspack/lzxd.c
    mspack/qtmd.c
    mspack/crc32.c
)

# libmspack source files use #include <name.h> for internal headers,
# so the mspack/ directory needs to be on the include path. Keep that
# private — consumers in AetherSDR only need the public API in mspack.h.
target_include_directories(mspack_static
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/mspack>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/mspack
)

# We don't supply a config.h; libmspack's source guards every reference
# behind HAVE_CONFIG_H so undefined is the right state. We deliberately
# do NOT define MSPACK_NO_DEFAULT_SYSTEM — that macro is checked with
# #ifndef in system.h, so the value is irrelevant; just defining it at
# all (even to 0) makes libmspack provide its own memcmp/memset/strlen,
# which collides with MSVC's compiler intrinsics for those functions
# (error C2169: "intrinsic function, cannot be defined").

# Upstream code emits a fair amount of -Wall noise (sign comparisons,
# implicit narrowing, missing-field-init). We don't want those polluting
# our own build output, and we don't want to modify upstream sources to
# silence them — that would break the "vendored verbatim" property.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(mspack_static PRIVATE -w)
elseif(MSVC)
    target_compile_options(mspack_static PRIVATE /w)
endif()

# Position-independent code so we can link into AetherSDR (which is built
# with PIE on most platforms). Also disable Qt's AUTOMOC/AUTOUIC/AUTORCC —
# this is plain C with no Qt usage, so they're pure overhead.
set_target_properties(mspack_static PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
