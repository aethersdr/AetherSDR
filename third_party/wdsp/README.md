# WDSP 2.00 vendor snapshot

This directory contains the WDSP sources used by AetherSDR's engine-side radio
DSP path. It is deliberately isolated behind `aether::wdsp`; no GUI target or
public AetherSDR header includes WDSP headers.

## Provenance

- Upstream: <https://github.com/TAPR/OpenHPSDR-wdsp>
- Upstream revision: `584e8aca5ba1c4c6bc66fc0cc164ce567c8ba1e3`
- Upstream commit subject: `Release Version 2.00`
- Imported path: `wdsp 2.00/Source/*.[ch]`
- License: GPL-2.0-or-later; see `LICENSE`

The revision is also recorded in `COMMIT`. The upstream PDF guide and the
runtime `Source/calculus` binary table are intentionally omitted: AetherSDR's
source-only contribution gate rejects binary additions, and neither artifact is
required for the initial RX chain. The source that optionally reads `calculus`
therefore retains its upstream fallback behavior.

## Local boundary

`upstream/` matches that source snapshot except for three teardown fixes recorded
in `AETHERSDR-PATCHES.md`. All portability changes live outside it:

- `port/` implements the narrow Windows compatibility surface WDSP uses on
  Unix: threads, mutexes, semaphores/events, atomic operations, aligned
  allocation, exports, diagnostics, and flush-to-zero SIMD state.
- `include/aether_wdsp.h` is the only C API AetherSDR code may include.
- `CMakeLists.txt` builds a position-independent static archive and suppresses
  warnings only for vendored code.

Do not patch `upstream/` casually. Every unavoidable source change must be
recorded in `AETHERSDR-PATCHES.md` with the upstream revision, rationale, and
refresh instructions.

## Refresh procedure

1. Resolve the intended upstream revision to a full 40-character commit SHA.
2. Replace only `upstream/*.[ch]` from the matching release directory.
3. Update `COMMIT`, this file, and `THIRD_PARTY_LICENSES`.
4. Build on macOS, Windows, and Linux and run `wdsp_channel_test`.
5. Confirm the audio callback allocation assertion and lifecycle leak checks.
