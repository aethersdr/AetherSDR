# WDSP 2.11 vendor snapshot

This directory contains the WDSP sources used by AetherSDR's engine-side radio
DSP path. It is deliberately isolated behind `aether::wdsp`; no GUI target or
public AetherSDR header includes WDSP headers.

## Provenance

- Upstream: <https://github.com/TAPR/OpenHPSDR-wdsp>
- Upstream revision: `e69de50d1d702216098019013899ad87534c64e0`
- Upstream commit subject: `Release Version 2.11 (NNR model 1)`
- Imported path: `wdsp 2.11/Source/*.[ch]`, normalized to LF line endings
  (2.11 ships 28 of its files with CRLF; 2.10 shipped none)
- License: GPL-2.0-or-later; see `LICENSE`

The revision is also recorded in `COMMIT`. The upstream PDF guide and the
runtime `Source/calculus` binary table are intentionally omitted: AetherSDR's
source-only contribution gate rejects binary additions, and neither artifact is
required for the initial RX chain. The source that optionally reads `calculus`
therefore retains its upstream fallback behavior.

## Local boundary

`upstream/` matches that source snapshot except for the local changes
recorded in `AETHERSDR-PATCHES.md`. The WFM integration adds patch 14: live
paired deemphasis, lock-free stereo observation, complete decoder reset, and
opt-in phase-difference/DC corrections that preserve stereo separation.
Patch 15 prepares depth-8 exchange rings only for nonblocking opt-in WFM,
retains legacy depth 2, and publishes copied output before its ring credit.
Its fixed memory/latency costs and deterministic tests are in the patch registry.
Existing WDSP overlap-save RF filtering is exposed
through the narrow C facade without changing the vendor filter. All portability
changes live outside it:

- `port/` implements the narrow Windows compatibility surface WDSP uses on
  Unix: threads, mutexes, semaphores/events, atomic operations, aligned
  allocation, exports, diagnostics, and flush-to-zero SIMD state.
  Its private `malloc.h` shim supplies the standard allocation declarations
  and counted aligned allocator for `nnrbypass.c` on Unix, including macOS,
  whose SDK does not provide `<malloc.h>`. Windows uses the native CRT header.
- `include/aether_wdsp.h` is the only C API AetherSDR code may include.
  `port/wdsp_port.c` checks its NNR declarations against upstream's `nnr.h`
  during compilation; the constructor takes its geometry from model metadata.
- `CMakeLists.txt` builds a position-independent static archive, suppresses
  warnings only for vendored code, and force-includes `port/include/wdsp_port.h`
  on non-Windows targets. That last part is load-bearing: `extrapolate.c`,
  `nurbs_fit.c` and `nurbs_spline.c` call `_aligned_malloc()` and
  `_aligned_free()` without including `comm.h`. 2.11 gives them
  `wdsp_align.h` instead, a `posix_memalign` shim of its own; the port header
  claims that header's include guard, so the port's counted allocator stays the
  only one and the real-time allocation checks still see every allocation.

## Neural Noise Reduction model data

`upstream/nnr_model_0.c` through `upstream/nnr_model_3.c` are 57 MB of generated
C holding the four trained NNR networks: Standard and Premium speech models
(slots 0 and 1, 2,098,944 and 4,682,240 bytes) and Standard and Premium CW
models (slots 2 and 3, 2,135,072 and 2,196,704 bytes). They are
`const unsigned char` blobs, which `nnio_parse()` expands into `double` tensors
at load. Every `create_nnr()` builds all four slots, each with its own STFT and
FFTW plans, whether or not the host ever selects it. The CW models carry a
training band; inside RXA, 2.11's `nnrbypass` routes audio outside that band
around NNR, and the standalone block AetherSDR runs has no such bypass.
They are vendored exactly as upstream ships them, which keeps the snapshot
verifiable and means no model file has to be packaged alongside the executable.

Every NNR construction looks for `wdsp_nnr_0.bin` through `wdsp_nnr_3.bin` **in
the process's working directory** and prefers either over the built-in copy for that
slot. The working directory, not the executable's: `nnet.c` holds those names as
bare relative paths and `nnio_open()` hands them straight to `fopen()`, and
upstream's own fallback message says "in the working directory" even though the
Guide describes it as the directory containing the executable. That is how an
experimental model reaches a tester without a rebuild, and AetherSDR keeps that
behaviour deliberately (RFC #5684 §8). Know what it costs, measured against a
standalone NNR on 2.10:

| File present as `wdsp_nnr_0.bin` | Result |
|---|---|
| none | built-in loads; noise attenuation −28.15 dB |
| malformed | rejected, falls back to the built-in; −28.15 dB |
| another valid model | **loads it** — slot 0 ran the Premium network while the selector still read "Standard" |
| valid, different dimensions | **"passing audio through"** — NNR silently does nothing; −5.46 dB, which is only the resampling band limit |

2.11 reads each slot's transform geometry from the model and validates every
slot independently, so the last row may behave differently now; it has not been
re-measured. On 2.10 it was the one that would be mistaken for a bug: a well-formed model from
a different WDSP release, left in a launch directory, disables NNR entirely. The
only trace is WDSP's own `dprintf`, which reaches `stderr` here — so a terminal
launch shows it and a desktop launch discards it. `SetNNRModelPathSlot(slot, "")`
skips the lookup for that slot and pins it to the built-in, if a host ever wants
to close it off.

Do not patch `upstream/` casually. Every unavoidable source change must be
recorded in `AETHERSDR-PATCHES.md` with the upstream revision, rationale, and
refresh instructions.

## Refresh procedure

1. Resolve the intended upstream revision to a full 40-character commit SHA.
2. Replace only `upstream/*.[ch]` from the matching release directory.
3. Normalize line endings to LF, then reapply or retire every change in
   `AETHERSDR-PATCHES.md` with a three-way merge (base: the old release's
   pristine sources; ours: `upstream/`; theirs: the new release).
4. Update `COMMIT`, this file, `THIRD_PARTY_LICENSES`, and the version gate in
   `WdspChannel` (`GetWDSPVersion()`).
5. Build on macOS, Windows, and Linux and run `wdsp_channel_test`.
6. Confirm the audio callback allocation assertion and lifecycle leak checks.
