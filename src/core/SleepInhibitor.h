#pragma once

#include <QString>

#include <initializer_list>

namespace AetherSDR {

// Prevents the operating system from entering idle sleep while an assertion
// is held. Used to keep TCP/UDP/audio streams alive during radio connections.
//
// Blocks system sleep only: the display may still blank and the screen may
// still lock on every OS.
//
// Platform backends:
//   macOS:   IOPMAssertionCreateWithName (kIOPMAssertionTypeNoIdleSleep)
//   Linux:   first available of org.gnome.SessionManager.Inhibit (flag
//            Suspend, GNOME), org.freedesktop.PowerManagement.Inhibit (KDE),
//            then systemd-logind Inhibit("sleep", "block") (Hyprland and
//            other desktops). Never org.freedesktop.ScreenSaver: that would
//            also stop the screen locking.
//   Windows: SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)

enum class LinuxSleepBackend { None, GnomeSession, KdePowerManagement, Logind };

// Tries the Linux backends in order (GNOME, KDE, logind) until `tryBackend`
// reports that one took the inhibitor, and returns it (None if none did).
// The desktop's own inhibitor comes first: it blocks idle suspend and leaves
// a manual Sleep working, where logind's block also refuses a manual Sleep.
template <typename TryBackend>
LinuxSleepBackend acquireFirstLinuxSleepBackend(TryBackend&& tryBackend)
{
    for (LinuxSleepBackend backend : {LinuxSleepBackend::GnomeSession,
                                      LinuxSleepBackend::KdePowerManagement,
                                      LinuxSleepBackend::Logind}) {
        if (tryBackend(backend))
            return backend;
    }
    return LinuxSleepBackend::None;
}

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

#ifdef Q_OS_MACOS
    uint32_t m_assertionId{0};
#endif
#if defined(Q_OS_LINUX) && defined(HAVE_DBUS)
    LinuxSleepBackend m_backend{LinuxSleepBackend::None};
    uint32_t m_cookie{0};   // GNOME / KDE inhibit cookie
    int m_logindFd{-1};     // logind inhibitor: held while this fd is open
#endif
};

} // namespace AetherSDR
