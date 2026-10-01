#pragma once

#include <QString>

namespace AetherSDR {

// Keeps Qt 6.12's QtMultimedia off its PipeWire audio backend when that backend
// would crash.
//
// Qt 6.12 picks its PipeWire backend whenever libpipewire can be loaded, and if
// it then cannot create a PipeWire client context it segfaults in
// QPlatformAudioDevices::create() — the first time anything touches
// QMediaDevices — instead of falling back to PulseAudio. The trigger seen so far
// is a system with libpipewire installed but no PipeWire client configuration
// (/usr/share/pipewire/client.conf ships in a separate package), which is
// exactly where pw_context_new() returns NULL.
//
// applyAtStartup() runs in main() before QApplication: unless the user already
// chose a backend via QT_AUDIO_BACKEND, it loads libpipewire the way Qt does
// (at runtime, no build-time dependency), tries to create a context, and on
// failure sets QT_AUDIO_BACKEND=pulseaudio so Qt never takes the crashing path.
// Linux only; a no-op everywhere else and whenever libpipewire is absent (Qt
// cannot use PipeWire then either).
class QtAudioBackendGuard {
public:
    static void applyAtStartup();

    // One line for the log, written once logging exists (main() calls this
    // after the log file is open, as it does for GpuSelector).
    static QString appliedSummary();
};

} // namespace AetherSDR
