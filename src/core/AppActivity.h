#pragma once

// Keeps the OS from throttling or idle-sleeping AetherSDR while it is open, so
// audio, DAX and TCI keep flowing when the window is hidden, occluded or
// silent (e.g. WSJT-X in front, AetherSDR minimized and muted, feeding it
// over TCI/DAX). The display may still sleep.
//
//   macOS:   NSProcessInfo user-initiated activity — no App Nap (timer
//            coalescing, I/O and priority throttling), no idle system sleep.
//   Windows: PowerRequestSystemRequired (no idle system sleep), process power
//            throttling off (HighQoS, not EcoQoS), and timer-resolution
//            requests honoured while minimized/occluded.
//   Linux:   no-op; SleepInhibitor still covers the connected session.
//
// Independent of SleepInhibitor, so its release on disconnect does not drop
// this. Call once from the GUI thread after QApplication exists; held until
// exit.
namespace AetherSDR {
#if defined(__APPLE__) || defined(_WIN32)
void keepAppActive();
#else
inline void keepAppActive() {}
#endif
} // namespace AetherSDR
