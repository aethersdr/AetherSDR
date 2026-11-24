// DATA2G_SIMD_CLONES on a hot loop's function: on x86-64 Linux, GCC also
// builds it for AVX2 and AVX-512 (4 or 8 doubles a vector, not SSE2's 2),
// picked at load time. The clones round identically: the core builds
// without FP contraction (native/CMakeLists.txt), so the FMA these ISAs
// bring is never used for a * b + c, and nothing else changes a result.
// Not under TSan: the ifunc resolver runs before its runtime is up (a crash
// at load).
#pragma once

// -DDATA2G_NO_SIMD_CLONES: the baseline code only (SSE2 here, 2 doubles a vector like NEON), for profiling what
// a machine without the wider ISAs runs.
#if defined(__x86_64__) && defined(__linux__) && defined(__GNUC__) && !defined(__SANITIZE_THREAD__) && !defined(DATA2G_NO_SIMD_CLONES)
#define DATA2G_SIMD_CLONES __attribute__((target_clones("avx512f", "avx2", "default")))
#define DATA2G_HAVE_SIMD_CLONES 1
#else
#define DATA2G_SIMD_CLONES
#endif
