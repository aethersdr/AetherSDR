#pragma once

// Keeps the OS from throttling or idle-sleeping AetherSDR while it is open, so
// audio, DAX and TCI keep flowing when the window is hidden, occluded or
// silent (e.g. WSJT-X in front, AetherSDR minimized and muted, feeding it
// over TCI/DAX). The display may still sleep.
//
//   macOS:   NSProcessInfo NSActivityUserInitiated — no App Nap (timer
//            coalescing, I/O and priority throttling), no idle system sleep,
//            and (implied by that option set) sudden/automatic termination
//            disabled, so logout sends a normal quit.
//   Windows: PowerRequestSystemRequired (no idle system sleep until the
//            first manual sleep, or Modern Standby on battery, ends it; not
//            re-taken after resume), process power throttling off (HighQoS,
//            not EcoQoS), and timer-resolution requests honoured while
//            minimized/occluded.
//   Linux:   no-op; the opt-in "Prevent system sleep while connected"
//            (SleepInhibitor, #1420) remains, Linux only.
//
// Supersedes the #1420 opt-in on macOS/Windows, where that option is no
// longer shown. Call once from the GUI thread after QApplication exists;
// held until exit.
namespace AetherSDR {
#if defined(__APPLE__) || defined(_WIN32)
void keepAppActive();
#else
inline void keepAppActive() {}
#endif
} // namespace AetherSDR
