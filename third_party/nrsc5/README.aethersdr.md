# Embedded nrsc5 in AetherSDR

This is the bounded source subset of [nrsc5](https://github.com/theori-io/nrsc5)
at commit `0225922b6f68109df39d07391f4d855464598ab8`. `upstream/` is unchanged.
`SOURCE-MANIFEST.json` lists every included path, upstream and vendored SHA-256,
and every omitted upstream path. The sample IQ recording, Python/CLI support
tools, CI files and generated documentation are excluded. The retained C CLI
source and upstream CMake files are reference/provenance only and are not built.

`upstream/LICENSE` declares GPL-3.0-or-later. The complete GPLv3 text is provided
in `COPYING.GPL-3.0`, with its source/hash in `LICENSE-SOURCE.json`. Preserve
per-file notices, including Ettus Research's GPLv3 convolutional decoder,
Phil Karn's GPL Reed-Solomon code, and rxi's MIT notice in `src/log.c` (not built).
The HDC patch has its own author/commit header and is preserved byte-for-byte.
These source licenses are not a patent clearance or a product certification.

## Build and target use

The parent includes `cmake/AetherHdFm.cmake` after resolving its RTL dependencies:

```cmake
include(cmake/AetherHdFm.cmake)
if(TARGET aether_nrsc5)
    target_link_libraries(aethercore PRIVATE aether_nrsc5)
endif()
```

`ENABLE_HD_FM` defaults OFF. Opting in currently requires Linux, GNU C, an
enabled RTL backend, and its existing `RTLSDR_TARGET` / `RTL_FFTW3F_TARGET`.
The module creates static `aether_nrsc5` and `aether_faad_hdc` only. Linking
`aether_nrsc5` supplies `nrsc5.h`, `AETHER_ENABLE_NRSC5=1`, and the transitive
link dependencies. Test targets that compile the wrapper directly must link
this target themselves; no global compile flag enables an unlinked decoder.

The upstream build systems are not executed. CLI is OFF by construction;
there is no CLI runtime, downloader, libao dependency, shared decoder library,
configure-time patch, install hook, or architecture-specific SIMD flag.
Configuration writes the private generated header to the build directory.
Library stderr logging remains at upstream level 5 (disabled). Normal Aether
builds on other platforms remain available with the default OFF setting.
Linux/GNU is the initial qualification scope, not evidence that a build or
reception test has passed. macOS, MSVC, MinGW and Clang remain unqualified.

## Required owner and lifetime contract

Aether alone owns the USB device. Its core wrapper uses `nrsc5_open_pipe`,
`nrsc5_set_mode(NRSC5_MODE_FM)`, `nrsc5_set_callback`,
`nrsc5_pipe_samples_cf32`, and `nrsc5_close`; device/file/TCP entrypoints must
never be called. Their unchanged upstream symbols remain compiled, not exposed
as an Aether feature. Pipe mode creates no nrsc5 worker; input processing and
callbacks are synchronous on the caller, with internal allocations and HDC work.
They belong on the bounded decoder owner, never the capture or GUI thread.

Hold Aether's process-global `fftwfPlannerLock()` across open and close,
including all FFTW allocations/plans/destruction/frees. nrsc5's own pthread
mutex does not serialize with Aether's other single-precision FFTW users.
Do not hold the planner lock during sample processing (`fftwf_execute`).
One owner serializes all calls and callbacks for each decoder instance.

The cf32 pipe requires exactly **744187.5 complex samples/second**, passed as
interleaved float I/Q. Its length is a count of scalar floats and must be even;
do not round the rate to 744188. Audio callbacks contain interleaved signed
16-bit stereo at 44100 Hz; their count is scalar samples, not stereo frames.
Callback data is borrowed: validate its bounds/program/flags and copy it during
the callback. Preserve all session/revision/receiver/service fences in Aether.

This dependency import does not harden upstream allocation failure paths:
`nrsc5_open_pipe` and nested initialization assume successful allocations.
Do not claim that a null result or a wrapper catch makes out-of-memory recovery
safe. Runtime boundedness, malformed-input handling, callback copying, RF
coverage and lifecycle validation belong to the integration and its tests.

## Reproducing the import

Use the exact source revision above, then copy only the paths listed in
`SOURCE-MANIFEST.json`. All upstream and vendored hashes must match. No
nrsc5 source patch is applied. The included
`upstream/support/faad2-hdc-support.patch` is applied to the separately pinned
FAAD subset as documented in `../faad_hdc/README.aethersdr.md`.
