# WDSP 2.10 vendor snapshot

This directory contains the WDSP sources used by AetherSDR's engine-side radio
DSP path. It is deliberately isolated behind `aether::wdsp`; no GUI target or
public AetherSDR header includes WDSP headers.

## Provenance

- Upstream: <https://github.com/TAPR/OpenHPSDR-wdsp>
- Upstream revision: `b02d5bac675dd2f33ec2bab2b339f79a597c47dd`
- Upstream commit subject: `Release Version 2.10`
- Imported path: `wdsp 2.10/Source/*.[ch]`
- License: GPL-2.0-or-later; see `LICENSE`

The revision is also recorded in `COMMIT`. The upstream PDF guide and the
runtime `Source/calculus` binary table are intentionally omitted: AetherSDR's
source-only contribution gate rejects binary additions, and neither artifact is
required for the initial RX chain. The source that optionally reads `calculus`
therefore retains its upstream fallback behavior.

## Local boundary

`upstream/` matches that source snapshot except for the four fixes recorded in
`AETHERSDR-PATCHES.md` — three teardown corrections and one use-after-free that
2.10 introduced on the channel-open path. All portability changes live outside it:

- `port/` implements the narrow Windows compatibility surface WDSP uses on
  Unix: threads, mutexes, semaphores/events, atomic operations, aligned
  allocation, exports, diagnostics, and flush-to-zero SIMD state.
- `include/aether_wdsp.h` is the only C API AetherSDR code may include.
- `CMakeLists.txt` builds a position-independent static archive, suppresses
  warnings only for vendored code, and force-includes `port/include/wdsp_port.h`
  on non-Windows targets. That last part is load-bearing: 2.10's
  `extrapolate.c`, `nurbs_fit.c` and `nurbs_spline.c` call `_aligned_malloc()`
  and `_aligned_free()` without including `comm.h`, which resolves to the CRT
  on MSVC and to nothing at all anywhere else.

## Neural Noise Reduction model data

`upstream/nnr_model_0.c` and `upstream/nnr_model_1.c` are 34.7 MB of generated C
holding the two trained NNR networks: `const unsigned char` blobs of 2,098,944
and 4,682,240 bytes, which `nnio_parse()` expands into `double` tensors at load.
They are vendored exactly as upstream ships them, which keeps the snapshot
verifiable and means no model file has to be packaged alongside the executable.

Every NNR construction looks for `wdsp_nnr_0.bin` and `wdsp_nnr_1.bin` **in the
process's working directory** and prefers either over the built-in copy for that
slot. The working directory, not the executable's: `nnet.c` holds those names as
bare relative paths and `nnio_open()` hands them straight to `fopen()`, and
upstream's own fallback message says "in the working directory" even though the
Guide describes it as the directory containing the executable. That is how an
experimental model reaches a tester without a rebuild. It also means a
well-formed file of either name in the launch directory silently replaces a
shipped DSP model with nothing in the UI to say so; `SetNNRModelPathSlot(slot,
"")` skips the lookup for that slot and pins it to the built-in.

Do not patch `upstream/` casually. Every unavoidable source change must be
recorded in `AETHERSDR-PATCHES.md` with the upstream revision, rationale, and
refresh instructions.

## Refresh procedure

1. Resolve the intended upstream revision to a full 40-character commit SHA.
2. Replace only `upstream/*.[ch]` from the matching release directory.
3. Update `COMMIT`, this file, and `THIRD_PARTY_LICENSES`.
4. Build on macOS, Windows, and Linux and run `wdsp_channel_test`.
5. Confirm the audio callback allocation assertion and lifecycle leak checks.
