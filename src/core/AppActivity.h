#pragma once

// Holds, for the life of the process, the OS opt-outs that keep audio, DAX
// and TCI flowing while AetherSDR is hidden or muted: no App Nap or idle
// sleep (macOS), no power throttling or idle sleep (Windows); no-op on Linux.
// Per-OS policy and caveats: docs/architecture/pipelines.md ("Thread QoS").
// Call once from the GUI thread after QApplication exists.
namespace AetherSDR {
#if defined(__APPLE__) || defined(_WIN32)
void keepAppActive();
#else
inline void keepAppActive() {}
#endif
} // namespace AetherSDR
