<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Test evidence

`tests/test_main.c` is deterministic and uses a fixed `xorshift32` hostile-input seed. The normal test covers:

- exact V1 unprotected bytes, PFCS, all 660 protected bits, zero-metric recovery, representative single-bit protected recovery, and all 312 one-bit covered-header PFCS mutations with and without recomputation;
- exact V2 scrambled message fragments with all four normative wire mini-headers `0x40`-`0x43`, explicit `0x44` rejection, out-of-order assembly, duplicate suppression, conflict reset, transmission prefix sizes, and complete 41-byte header repetition with PFCS validation;
- V3 product/reset/parity-disable/rate/decode packets, every two-write split of a parity packet, bytewise corruption, PCM endianness, product identity, FIFO reply expectations, timeout, and generation invalidation;
- VITA exact size/class/timestamps/float byte order, every truncated length from 0 through 1,051, wrong stream/class, NaN, modulo-16 rollover, one gap, duplicate, and reorder;
- arbitrary control-line split points, strict responses and four distinct stream IDs, exact RX/TX metric key sets, duplicate/missing/negative/overflow/NaN rejection, correlated errors, unknown duplicates, invalidation, and timeout;
- five-sample-per-bit output count, normalized explicit Gaussian taps, finite 0.98 limiting, demodulator round trip, preamble-qualified frame-sync acquisition, rejection beyond the two-error preamble bound, and header/voice/data-sync/end callbacks;
- zero-policy compatibility, every Hamming threshold from 0 through 24, all 24 single-bit sync errors, all 276 two-bit combinations, all 2,024 three-bit combinations, accepted/rejected threshold boundaries, and exact/tolerant event separation;
- every early and late sliding offset from 1 through the 24-bit safety cap, tolerant early realignment, next-frame boundary recovery, corrupted-frame suppression, false-candidate rejection, late-window look-ahead preservation, two-miss unlock, and exactly-once miss/loss/end callbacks; and
- 100,000 seeded hostile bytes through each streaming parser plus 1,000 random bounded VITA inputs.

`tests/quarantined_acceptance.c` names unresolved C1-C4 and C6-C17 and returns 77. C5 is resolved by JARL D-STAR Standard 7.0 section 6.3 and is covered by the deterministic mini-header regression. CMake marks 77 as skipped so unresolved hardware evidence is visible without being mistaken for a passing acceptance result. The synchronization tests prove the configurable mechanism and bounds; they do not select AetherSDR's C11 compatibility values.

The same source-only build and deterministic suite passed in a generic Debian trixie-slim container with GCC 14.2.0 and `-Werror`. The implementation team did not have ThumbDV or FLEX-6000 hardware, Windows, an RF analyzer, or an independent D-STAR receiver in this projectless workspace. Those environment-dependent acceptance items remain unclaimed.
