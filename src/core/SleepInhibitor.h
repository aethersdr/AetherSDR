#pragma once

#include <QString>

namespace MasterSDR {

// Prevents the operating system from entering idle sleep while an assertion
// is held. Used to keep TCP/UDP/audio streams alive during radio connections.
//
// Platform backends:
//   macOS:   IOPMAssertionCreateWithName (kIOPMAssertionTypeNoIdleSleep)
//   Linux:   org.freedesktop.ScreenSaver.Inhibit via D-Bus
//   Windows: SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)
class SleepInhibitor {
public:
    SleepInhibitor() = default;
    ~SleepInhibitor();

    // Acquire the power assertion. No-op if already held.
    void acquire(const QString& reason = "Connected to radio");

    // Release the power assertion. No-op if not held.
    void release();

    bool isHeld() const { return m_held; }

private:
    bool m_held{false};

#ifdef Q_OS_MAC
    uint32_t m_assertionId{0};
#endif
#if defined(Q_OS_LINUX) && defined(HAVE_DBUS)
    uint32_t m_cookie{0};
#endif
};

} // namespace MasterSDR
