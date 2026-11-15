#pragma once

#include <QThread>

// Holds, for the life of the process, the OS opt-outs that keep audio, DAX
// and TCI flowing while AetherSDR is hidden or muted: no App Nap (macOS), no
// power throttling (Windows); no-op on Linux. Never blocks system sleep; that
// is SleepInhibitor's job, and only while connected.
// Per-OS policy and caveats: docs/architecture/pipelines.md ("Thread QoS").
// Call once from the GUI thread after QApplication exists.
namespace AetherSDR {
#if defined(__APPLE__) || defined(_WIN32)
void keepAppActive();
#else
inline void keepAppActive() {}
#endif

// Starts a thread that carries audio or data streams at High QoS: the GUI
// thread's class on macOS (performance cores), no power throttling on
// Windows, a no-op on Linux. Never add setPriority() or start(priority) to
// such a thread: on macOS an explicit priority silently cancels the QoS class.
inline void startStreamThread(QThread* thread)
{
    thread->setServiceLevel(QThread::QualityOfService::High);
    thread->start();
}
} // namespace AetherSDR
