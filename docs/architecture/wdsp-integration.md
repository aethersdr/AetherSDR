# WDSP 2.00 integration boundary

**Status:** Foundation implemented; Hermes-Lite 2 backend integration remains a
separate phase. This boundary is engine-only and introduces no radio behavior.

## Decision

AetherSDR uses WDSP as a **whole-channel DSP engine** for raw-IQ radio backends.
It does not select individual WDSP stages and interleave them with
`AudioEngine`. For Hermes-Lite 2, one RX `WdspChannel` will own the complete
IQ-to-audio receive chain; a separate TX `WdspChannel` will later own the
complete audio-to-IQ transmit chain. A spectrum tap may observe the raw IQ, but
must not participate in or mutate the audio DSP chain.

This makes the ownership line testable:

```text
Metis UDP RX -> bounded IQ queue -> HL2 DSP worker -> WDSP RX channel -> PCM outlet
                                      |
                                      +-> read-only FFT/spectrum tap

PCM TX source -> bounded audio queue -> HL2 DSP worker -> WDSP TX channel -> Metis EP2
```

No GUI target, `AudioEngine`, or backend-neutral model may include a WDSP
header. `src/core/dsp/WdspChannel` is the only C++ entry point, and the C API in
`third_party/wdsp/include/aether_wdsp.h` is private to `aethercore`.

## Reproducible source and build

The snapshot is pinned to TAPR/OpenHPSDR-wdsp commit
`584e8aca5ba1c4c6bc66fc0cc164ce567c8ba1e3`, whose subject is
`Release Version 2.00`. `third_party/wdsp/COMMIT` is the machine-readable pin.
The upstream PDF and binary calculus lookup table are not vendored. The latter
is optional in WDSP and its source fallback remains active.

`third_party/wdsp/CMakeLists.txt` produces the position-independent static
target `aether::wdsp`. Only `aethercore` links it, privately. WDSP's FFTW and
thread dependencies, source include directory, warning policy, and platform
definitions do not become public AetherSDR usage requirements.

The source snapshot has three documented teardown corrections. See
`third_party/wdsp/AETHERSDR-PATCHES.md`; refreshes must either find the fixes
upstream or reapply and retest them explicitly.

## Portability layer

All platform adaptation stays under `third_party/wdsp/port/`:

- Win32-style critical sections, semaphores, events, waits, and worker starts
  map to pthreads on macOS/Linux.
- aligned allocation is routed through one shim on every platform; counters
  make callback allocation and teardown leaks test-visible.
- symbol export annotations and the small CRT compatibility surface are
  neutralized at the boundary rather than scattered through WDSP.
- flush-to-zero setup uses AArch64 FPCR or SSE MXCSR as appropriate.
- priority requests are deliberately best-effort; the owning backend chooses
  its worker scheduling policy rather than allowing WDSP to own it globally.

The port implements only calls consumed by the pinned snapshot. Adding another
Win32 emulation API requires a concrete WDSP call site and a unit test.

## Lifetime and real-time contract

`WdspChannel` owns one numeric slot in WDSP's process-global channel table.
Construction acquires a unique slot; destruction closes the channel and returns
the slot. Copy and move are disabled so a slot has exactly one C++ owner.

WDSP setup is serialized because its FFTW planner and channel tables are global.
`OpenChannel`, `CloseChannel`, rate changes, mode changes, and filter changes are
control-thread operations. `processIq` is the only real-time operation:

- input and output buffers have fixed, validated sizes;
- no FFT plan or WDSP object is created or destroyed;
- allocation sequence changes are reported as `AllocationViolation`;
- nonblocking starvation is reported as `Underrun`;
- control/callback overlap is rejected as `Busy`.

The current `reconfigure` API requires the caller to stop the feed. For a live
HL2, do not close and rebuild the active channel on the DSP worker. Prepare a
second `WdspChannel` completely on a control thread, then swap owners at a block
boundary and retire the old instance off the real-time path. This also makes
48/96/192/384 kHz changes bounded and rollback-safe.

## HL2 staging and safety

Phase 1 is one receiver and no transmission:

1. packet parsing and loss accounting feed a bounded SPSC IQ queue;
2. a fixed-block DSP worker owns one preplanned RX `WdspChannel`;
3. queue starvation inserts a timestamped zero-IQ gap and increments an
   underrun counter rather than blocking the UDP thread;
4. demodulated PCM and spectrum frames leave through backend-owned outlets;
5. `canTransmit` remains false and no MOX-bearing packet is generated.

Phase 2 adds a distinct TX channel only after live RX is stable. TX construction
does not imply authorization to key: the operator-intent/arbiter gate must pass
before the backend sets MOX or emits a nonzero TX IQ sample. Teardown clears MOX
first, stops EP2 production second, then destroys the TX channel.

## Verification and future performance matrix

`wdsp_channel_test` currently covers:

- reproducible RX complex-tone to audio and TX audio-tone to IQ vectors;
- allocation-free processing;
- explicit nonblocking underruns;
- rate/block reconfiguration outside the callback;
- unique concurrent channel IDs and leak-free repeated teardown.

Captured Metis frames and packet-gap fixtures belong in the future HL2 backend
test, not in this generic WDSP test.

Performance work starts only after the live RX path is correct. Record p50/p95/
p99 block time, CPU, peak resident memory, underruns, and queue high-water mark
for 1, 2, 4, and the supported maximum receiver count at 48/96/192/384 kHz on
macOS ARM, Windows x64, and Linux x64. A run passes only when p99 remains below
the block deadline with zero steady-state allocations and zero underruns after
warm-up.
