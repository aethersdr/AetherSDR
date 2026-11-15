# TX Audio Sync Fix Report (DAX + SSB)

## Executive Summary

This report documents how the TX audio timing issue was fixed in AetherSDR, including:

- The original DAX edge-sync problem (unstable start/stop timing).
- The SSB regression introduced during iterative tuning (voice delay after PTT).
- The final architecture that keeps DAX behavior correct while restoring immediate SSB response.

Relevant commits:

- `5fc2ecd` - `Fix DAX TX edge sync and bridge backlog latency`
- `be2ff04` - `Fix SSB TX edge sync and keep DAX path isolated`

## Symptoms Seen During Debugging

1. DAX TX (VARA/WSJT-X style traffic) showed non-deterministic timing at key/unkey edges.
2. After DAX timing changes, SSB developed a delay:
   - Press PTT: voice started late.
   - Release PTT: TX/audio could continue briefly.
3. At one point, a previously fixed DAX behavior regressed while trying to tune SSB.

## Root Causes

### 1) TX audio gating for SSB was tied to interlock confirmation

In the intermediate implementation, SSB TX-audio start could wait for `interlock state=TRANSMITTING` (via `txAudioGateChanged`) instead of following the local MOX/PTT edge immediately.

That made SSB audio intentionally start later than the user key event.

### 2) DAX TX audio callback could still run outside DAX TX mode

The DAX bridge callback (`DaxBridge::txAudioReady`) originally forwarded audio to `AudioEngine::feedDaxTxAudio()` without a strict `isDaxTxMode()` guard.

This allowed DAX-origin audio to interfere with non-DAX operation and made timing behavior harder to stabilize.

### 3) Bridge backlog pressure during DAX TX path

The shared-memory TX bridge could accumulate stale data under load. Without controlled draining and backlog clamping, edge timing drifted and could produce delayed behavior.

## What Was Implemented

### Phase A: DAX edge and backlog stabilization (`5fc2ecd`)

### `src/models/RadioModel.cpp`

- Added local TX intent tracking (`m_txRequested`) so TX gating follows user intent through interlock transitions.
- When unkey is requested, TX-off is forced even if a late stale `TRANSMITTING` status arrives.
- Added `txAudioGateChanged` signaling behavior for controlled pipeline gating.

### `src/core/VirtualAudioBridge.cpp`

- Reduced TX poll interval to 2 ms.
- Drained multiple fixed-size chunks per timer tick.
- Added backlog clamp logic to skip stale samples and keep only a small recent window.

This reduced queue growth and removed old-audio tail effects.

### `src/core/AudioEngine.cpp`

- On TX-off, clears TX accumulators to prevent carry-over into the next burst.
- Kept DAX and mic routes separated.
- Preserved deterministic behavior for low-latency DAX packetization.

### `src/gui/MainWindow.cpp`

- Added mode-aware TX gating flow and DAX route control options.
- Later refined in Phase B to remove SSB delay side effects.

### Phase B: SSB delay removal + DAX isolation hardening (`be2ff04`)

### `src/gui/MainWindow.cpp` (critical final fix)

1. In the MainWindow handler connected to `TransmitModel::moxChanged`:
   - TX audio now follows MOX edge immediately for all modes:
     - `m_audio.setTransmitting(tx);`
   - DAX bridge TX state is updated immediately too (where applicable).

2. In the MainWindow handler connected to `RadioModel::txAudioGateChanged`:
   - It is now treated as TX-off fallback only.
   - We consume only `tx == false` to force-safe shutdown if needed.
   - We do not use it to delay TX-on for SSB anymore.

3. In DAX bridge TX callback:
   - Added strict mode guard:
     - `if (!m_audio.isDaxTxMode()) return;`
   - This prevents DAX TX callback traffic from entering SSB path.

## Final TX Behavior After Fix

1. PTT/MOX ON:
   - TX audio pipeline opens immediately on local edge (no intentional SSB wait).
2. PTT/MOX OFF:
   - TX audio pipeline closes immediately.
   - Interlock fallback can still force-safe TX-off if needed.
3. DAX callback path:
   - Active only in DAX TX mode.
   - Cannot leak into SSB path.

## Why This Solves the Problem

- The user expectation was edge-synchronous behavior (no perceivable lag between software keying and RF/audio action).
- SSB delay was introduced by waiting for interlock on TX-on.
- Moving TX-on gating back to local MOX/PTT edge restores immediate response.
- Keeping interlock as TX-off fallback preserves safety.
- Hard mode isolation on DAX callback prevents cross-path interference and regressions.

## Validation

- Build validation completed successfully after final fix:
  - `cmake --build build-new -j8`
- User-side functional confirmation sequence during debugging:
  - DAX sync issue resolved.
  - SSB delay addressed by final MainWindow gating update.

## Maintenance Notes

1. Keep only one active TX audio source per mode.
2. Do not reintroduce TX-on wait-for-interlock for SSB unless explicitly required for a hardware reason.
3. Preserve the DAX callback mode guard to avoid SSB contamination.
4. If future latency appears, inspect:
   - MOX edge timing
   - interlock state transitions
   - bridge backlog (`readPos/writePos` growth)
   - first/last packet emission timing in TX path
