#pragma once

// Request microphone permission on macOS at app startup.
// Shows the system permission dialog on first launch.
// No-op on non-macOS platforms.
#ifdef Q_OS_MACOS
void requestMicrophonePermission();
#else
inline void requestMicrophonePermission() {}
#endif
