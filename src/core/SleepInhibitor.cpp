#include "SleepInhibitor.h"
#include <QDebug>

#ifdef Q_OS_MAC
#include <IOKit/pwr_mgt/IOPMLib.h>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#if defined(Q_OS_LINUX) && defined(HAVE_DBUS)
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace AetherSDR {

#if defined(Q_OS_LINUX) && defined(HAVE_DBUS)
namespace {
// GNOME and KDE hand back a cookie; logind hands back a file descriptor.
struct CookieService {
    LinuxSleepBackend backend;
    const char* service;
    const char* path;
    const char* interface;
    const char* uninhibit;   // GNOME spells it Uninhibit, KDE UnInhibit
};
constexpr CookieService kCookieServices[] = {
    {LinuxSleepBackend::GnomeSession, "org.gnome.SessionManager",
     "/org/gnome/SessionManager", "org.gnome.SessionManager", "Uninhibit"},
    {LinuxSleepBackend::KdePowerManagement, "org.freedesktop.PowerManagement",
     "/org/freedesktop/PowerManagement/Inhibit", "org.freedesktop.PowerManagement.Inhibit",
     "UnInhibit"},
};
constexpr uint kGnomeInhibitSuspend = 4;   // suspend only, not idle/screen lock

const CookieService* cookieService(LinuxSleepBackend backend)
{
    for (const CookieService& s : kCookieServices) {
        if (s.backend == backend)
            return &s;
    }
    return nullptr;
}

bool hasOwner(const QDBusConnection& bus, const QString& service)
{
    return bus.isConnected() && bus.interface()
        && bus.interface()->isServiceRegistered(service).value();
}
} // namespace
#endif

SleepInhibitor::~SleepInhibitor()
{
    release();
}

void SleepInhibitor::acquire(const QString& reason)
{
    if (m_held)
        return;

#ifdef Q_OS_MAC
    CFStringRef cfReason = reason.toCFString();
    IOReturn ret = IOPMAssertionCreateWithName(
        kIOPMAssertionTypeNoIdleSleep,
        kIOPMAssertionLevelOn,
        cfReason,
        &m_assertionId);
    CFRelease(cfReason);
    if (ret == kIOReturnSuccess) {
        m_held = true;
        qDebug() << "SleepInhibitor: acquired (macOS IOPMAssertion)";
    } else {
        qWarning() << "SleepInhibitor: IOPMAssertionCreate failed:" << ret;
    }
#endif

#ifdef Q_OS_WIN
    EXECUTION_STATE prev = SetThreadExecutionState(
        ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    if (prev != 0 || true) { // SetThreadExecutionState returns previous state, 0 only on error
        m_held = true;
        qDebug() << "SleepInhibitor: acquired (Windows SetThreadExecutionState)";
    }
#endif

#if defined(Q_OS_LINUX) && defined(HAVE_DBUS)
    // Each backend is probed only when the ones before it did not take the
    // inhibitor, so a GNOME session never touches the system bus.
    m_backend = acquireFirstLinuxSleepBackend([&](LinuxSleepBackend backend) {
        if (const CookieService* svc = cookieService(backend)) {
            QDBusConnection session = QDBusConnection::sessionBus();
            if (!hasOwner(session, QString::fromLatin1(svc->service)))
                return false;
            QDBusInterface iface(QString::fromLatin1(svc->service), QString::fromLatin1(svc->path),
                                 QString::fromLatin1(svc->interface), session);
            const QList<QVariant> args = backend == LinuxSleepBackend::GnomeSession
                ? QList<QVariant>{QStringLiteral("AetherSDR"), 0u, reason, kGnomeInhibitSuspend}
                : QList<QVariant>{QStringLiteral("AetherSDR"), reason};
            QDBusReply<uint> reply = iface.callWithArgumentList(QDBus::Block,
                                                                QStringLiteral("Inhibit"), args);
            if (!reply.isValid()) {
                qWarning() << "SleepInhibitor:" << svc->service << "Inhibit failed:"
                           << reply.error().message();
                return false;
            }
            m_cookie = reply.value();
            qDebug() << "SleepInhibitor: acquired (" << svc->service << ", suspend only)";
            return true;
        }

        QDBusConnection system = QDBusConnection::systemBus();
        const QString logind = QStringLiteral("org.freedesktop.login1");
        if (!hasOwner(system, logind))
            return false;
        QDBusInterface iface(logind, QStringLiteral("/org/freedesktop/login1"),
                             QStringLiteral("org.freedesktop.login1.Manager"), system);
        QDBusReply<QDBusUnixFileDescriptor> reply = iface.call(
            QStringLiteral("Inhibit"), QStringLiteral("sleep"), QStringLiteral("AetherSDR"),
            reason, QStringLiteral("block"));
        // The reply's descriptor closes with it; the inhibitor lives as long
        // as our copy stays open. Close-on-exec, so a child process never
        // inherits the block and holds it after we release or exit.
        const int fd = reply.isValid() && reply.value().isValid()
            ? ::fcntl(reply.value().fileDescriptor(), F_DUPFD_CLOEXEC, 0) : -1;
        if (fd < 0) {
            qWarning() << "SleepInhibitor: logind Inhibit failed:"
                       << (reply.isValid() ? QStringLiteral("no descriptor")
                                           : reply.error().message());
            return false;
        }
        m_logindFd = fd;
        qDebug() << "SleepInhibitor: acquired (logind sleep block)";
        return true;
    });
    if (m_backend != LinuxSleepBackend::None)
        m_held = true;
    else
        qWarning() << "SleepInhibitor: no GNOME, KDE or logind inhibitor; idle sleep not blocked";
#endif

    Q_UNUSED(reason);
}

void SleepInhibitor::release()
{
    if (!m_held)
        return;

#ifdef Q_OS_MAC
    IOPMAssertionRelease(m_assertionId);
    m_assertionId = 0;
    qDebug() << "SleepInhibitor: released (macOS)";
#endif

#ifdef Q_OS_WIN
    SetThreadExecutionState(ES_CONTINUOUS);
    qDebug() << "SleepInhibitor: released (Windows)";
#endif

#if defined(Q_OS_LINUX) && defined(HAVE_DBUS)
    if (const CookieService* svc = cookieService(m_backend)) {
        QDBusInterface iface(QString::fromLatin1(svc->service), QString::fromLatin1(svc->path),
                             QString::fromLatin1(svc->interface), QDBusConnection::sessionBus());
        iface.call(QString::fromLatin1(svc->uninhibit), m_cookie);
    } else if (m_backend == LinuxSleepBackend::Logind && m_logindFd >= 0) {
        ::close(m_logindFd);
    }
    m_backend = LinuxSleepBackend::None;
    m_cookie = 0;
    m_logindFd = -1;
    qDebug() << "SleepInhibitor: released (Linux)";
#endif

    m_held = false;
}

} // namespace AetherSDR
