#pragma once

#include <QString>

namespace AetherSDR {

// Qt 6.12 picks its PipeWire audio backend whenever libpipewire loads, then
// segfaults in QPlatformAudioDevices::create() if pw_context_new() fails (e.g.
// libpipewire present but no /usr/share/pipewire/client.conf). applyAtStartup()
// runs before QApplication: unless QT_AUDIO_BACKEND is set, it dlopens
// libpipewire, tries a context, and on failure sets QT_AUDIO_BACKEND=pulseaudio.
// Linux only; no-op elsewhere or when libpipewire is absent.
class QtAudioBackendGuard {
public:
    static void applyAtStartup();

    // One line for the log, written once logging exists (main() calls this
    // after the log file is open, as it does for GpuSelector).
    static QString appliedSummary();
};

} // namespace AetherSDR
